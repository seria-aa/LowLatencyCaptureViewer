#include "recording/RecordingContainer.h"
#include "recording/RecordingStorage.h"
#include "recording/EncoderSupport.h"
#include "recording/FragmentedMp4Sink.h"
#include "RecordingFileTestSupport.h"
#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace llcv::recording;
namespace {
constexpr UINT32 kWidth = 320, kHeight = 192, kFps = 30, kFrames = 360;
constexpr LONGLONG kSecond = 10000000;
void Check(bool ok, const char* label) {
    if (!ok) { printf("FAIL %s\n", label); std::exit(1); }
}
void Hr(HRESULT result, const char* label) {
    if (FAILED(result)) { printf("FAIL %s %08lX\n", label, static_cast<unsigned long>(result)); std::exit(1); }
}
ComPtr<IMFMediaType> Type() {
    ComPtr<IMFMediaType> type; Hr(MFCreateMediaType(&type), "media type"); return type;
}
struct Packet {
    ComPtr<IMFSample> sample;
    LONGLONG pts = 0, duration = 0;
    DWORD bytes = 0;
};
struct Track {
    ComPtr<IMFMediaType> type;
    std::vector<Packet> packets;
};
Track Read(const wchar_t* path, bool video, bool complete = true) {
    ComPtr<IMFSourceReader> reader;
    Hr(MFCreateSourceReaderFromURL(path, nullptr, &reader), "open native recording");
    const DWORD stream = static_cast<DWORD>(video ? MF_SOURCE_READER_FIRST_VIDEO_STREAM : MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    Track result;
    Hr(reader->GetNativeMediaType(stream, 0, &result.type), "native recording type");
    Hr(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "unselect streams");
    Hr(reader->SetStreamSelection(stream, TRUE), "select stream");
    Hr(reader->SetCurrentMediaType(stream, nullptr, result.type.Get()), "no optional decoder requirement");
    bool ended = false;
    for (unsigned reads = 0; reads < 10000; ++reads) {
        Packet packet; DWORD flags = 0;
        const auto hr = reader->ReadSample(stream, 0, nullptr, &flags, &packet.pts, &packet.sample);
        if (FAILED(hr)) {
            if (!complete && !result.packets.empty()) { ended = true; break; }
            Hr(hr, "read native packet");
        }
        if (packet.sample) {
            Hr(packet.sample->GetSampleDuration(&packet.duration), "packet duration");
            Hr(packet.sample->GetTotalLength(&packet.bytes), "packet length");
            Check(packet.bytes != 0 && packet.duration > 0, "nonempty positive-duration packet");
            result.packets.push_back(std::move(packet));
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
    }
    Check(ended && !result.packets.empty(), "bounded nonempty stream read");
    printf("%s %s packets=%zu first=%.6f last=%.6f\n", complete ? "complete" : "retained",
        video ? "video" : "audio", result.packets.size(), result.packets.front().pts / 1e7,
        result.packets.back().pts / 1e7);
    return result;
}
std::vector<LONGLONG> Times(const Track& track) {
    std::vector<LONGLONG> times;
    for (const auto& packet : track.packets) times.push_back(packet.pts);
    std::sort(times.begin(), times.end()); return times;
}
bool Reordered(const Track& track) {
    for (size_t i = 1; i < track.packets.size(); ++i)
        if (track.packets[i].pts < track.packets[i - 1].pts) return true;
    return false;
}
void SameTimeline(const Track& expected, const Track& actual, bool relativeOnly = false) {
    auto source = Times(expected), target = Times(actual);
    Check(source.size() == target.size(), "all native packets retained");
    LONGLONG maximumError = 0;
    const LONGLONG shift = relativeOnly ? source.front() - target.front() : 0;
    for (size_t i = 0; i < source.size(); ++i) maximumError = (std::max)(maximumError, std::abs(source[i] - target[i] - shift));
    printf("native timeline maximum error %.6f ms\n", maximumError / 10000.0);
    Check(maximumError < 10000, "source presentation timestamps preserved within 1 ms");
    if (relativeOnly) printf("MF native reader normalizes track origin: external demux/decode check required (shift %.6f ms)\n", shift / 10000.0);
}
void Continuous(const Track& track, LONGLONG duration) {
    const auto times = Times(track);
    for (size_t i = 1; i < times.size(); ++i)
        Check(std::abs(times[i] - times[i - 1] - duration) < 10000, "no gap/duplicate across fragment boundary");
}
struct Boxes {
    QWORD length = 0, lastMediaStart = 0, lastMediaSize = 0;
    unsigned movies = 0, fragments = 0, media = 0;
};
Boxes Scan(IMFByteStream* bytes) {
    Boxes result; Hr(bytes->GetLength(&result.length), "file length");
    QWORD position = 0;
    while (position + 8 <= result.length) {
        BYTE header[16]{}; ULONG read = 0; Hr(bytes->SetCurrentPosition(position), "box position");
        Hr(bytes->Read(header, 8, &read), "box header"); Check(read == 8, "complete box header");
        QWORD size = 0; for (unsigned i = 0; i < 4; ++i) size = size * 256 + header[i];
        unsigned headerSize = 8;
        if (size == 1) {
            Hr(bytes->Read(header + 8, 8, &read), "large box size"); Check(read == 8, "complete large header");
            size = 0; for (unsigned i = 8; i < 16; ++i) size = size * 256 + header[i]; headerSize = 16;
        }
        if (size < headerSize || size > result.length - position) break;
        if (!memcmp(header + 4, "moov", 4)) ++result.movies;
        if (!memcmp(header + 4, "moof", 4)) ++result.fragments;
        if (!memcmp(header + 4, "mdat", 4)) {
            ++result.media;
            // Finalization can append an AAC-only tail containing one tiny
            // silence packet. Tear a substantive media fragment, not padding.
            if (size > 32) { result.lastMediaStart = position; result.lastMediaSize = size; }
        }
        position += size;
    }
    return result;
}
void CheckStructure(IMFByteStream* bytes) {
    const auto boxes = Scan(bytes);
    printf("bytes=%llu movies=%u fragments=%u media=%u\n", boxes.length, boxes.movies, boxes.fragments, boxes.media);
    Check(boxes.movies == 1 && boxes.fragments == 0 && boxes.media == 1,
        "completed hybrid recording has a conventional MP4 index");
}
ComPtr<IMFAttributes> Attributes(bool cpu, bool crash) {
    ComPtr<IMFAttributes> attributes;
    Hr(MFCreateAttributes(&attributes, 2), "writer attributes");
    Hr(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, !cpu), "hardware encoder policy");
    if (!crash) Hr(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE), "test only unthrottled");
    return attributes;
}
void Finish(IMFSinkWriter* writer, IMFByteStream* bytes, bool crash) {
    if (crash) {
        // Deliberately skip Finalize and all destructors, like process termination.
        TerminateProcess(GetCurrentProcess(), 73); std::abort();
    }
    Hr(writer->Finalize(), "finalize recording"); Hr(bytes->Flush(), "final flush"); CheckStructure(bytes);
}
void Shift(Track& track, LONGLONG offset) {
    if (!offset) return;
    for (auto& packet : track.packets) {
        packet.pts += offset; Hr(packet.sample->SetSampleTime(packet.pts), "controlled stream PTS offset");
        UINT64 dts = 0;
        if (SUCCEEDED(packet.sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &dts)))
            Hr(packet.sample->SetUINT64(MFSampleExtension_DecodeTimestamp, dts + offset), "controlled stream DTS offset");
    }
}
void Manifest(const wchar_t* outputPath, const Track& video, const Track& audio) {
    const auto path = std::filesystem::path(outputPath).parent_path() / L"input-timestamps.json";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Check(file.good(), "open test-owned native timestamp manifest");
    file << "{\"timeBase\":10000000,\"video\":[";
    auto write = [&](const Track& track) {
        bool first = true;
        for (const auto& packet : track.packets) {
            UINT64 stored = 0;
            const LONGLONG dts = SUCCEEDED(packet.sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &stored))
                ? static_cast<LONGLONG>(stored) : packet.pts;
            if (!first) file << ',';
            first = false;
            file << "{\"pts\":" << packet.pts << ",\"dts\":" << dts << ",\"duration\":" << packet.duration << '}';
        }
    };
    write(video); file << "],\"audio\":["; write(audio); file << "]}\n";
    file.close(); Check(file.good(), "save test-owned native timestamp manifest");
}
void Remux(const wchar_t* inputPath, const wchar_t* outputPath, bool crash, LONGLONG videoOffset, LONGLONG audioOffset, int splitMode=0) {
    auto video = Read(inputPath, true), audio = Read(inputPath, false);
    Shift(video, videoOffset); Shift(audio, audioOffset);
    Manifest(outputPath, video, audio);
    GUID subtype{}; Hr(video.type->GetGUID(MF_MT_SUBTYPE, &subtype), "fixture codec");
    const auto codec = subtype == MFVideoFormat_AV1 ? Codec::Av1 : Codec::Hevc;
    Check(subtype == MFVideoFormat_AV1 || subtype == MFVideoFormat_HEVC, "modern encoded fixture");
    auto attributes = Attributes(true, crash);
    ComPtr<IMFByteStream> bytes; ComPtr<IMFSinkWriter> writer;
    std::shared_ptr<RecordingArchive> archive;
    if(splitMode){
        SplitPolicy split;split.basePath=outputPath;
        // Small deterministic thresholds exercise repeated rotations with tiny fixtures.
        if(splitMode==1)split.duration100ns=2*kSecond;else split.bytes=8192;
        Hr(MFCreateFile(MF_ACCESSMODE_READWRITE,MF_OPENMODE_FAIL_IF_EXIST,MF_FILEFLAGS_NONE,RecordingPartPath(outputPath,1).c_str(),&bytes),"first segment");
        ComPtr<IMFByteStream> buffered;Hr(BufferRecordingStorage(bytes.Get(),nullptr,&buffered),"buffer first segment");bytes=std::move(buffered);
        ComPtr<IMFMediaSink> sink;Hr(CreateFragmentedMp4Sink(bytes.Get(),&sink,&split,&archive),"split sink");
        Hr(MFCreateSinkWriterFromMediaSink(sink.Get(),attributes.Get(),&writer),"split writer");
    }else Hr(CreateRecordingFile(outputPath, attributes.Get(), &bytes, &writer, codec), "modern fixture writer");
    DWORD videoStream = 0, audioStream = 0;
    Hr(writer->AddStream(video.type.Get(), &videoStream), "encoded video stream");
    Hr(writer->SetInputMediaType(videoStream, video.type.Get(), nullptr), "encoded video passthrough");
    Hr(writer->AddStream(audio.type.Get(), &audioStream), "encoded audio stream");
    Hr(writer->SetInputMediaType(audioStream, audio.type.Get(), nullptr), "encoded audio passthrough");
    Hr(writer->BeginWriting(), "begin encoded passthrough");
    size_t vi = 0, ai = 0;
    const auto start = std::chrono::steady_clock::now(); LONGLONG nextFlush = 0;
    while (vi < video.packets.size() || ai < audio.packets.size()) {
        const bool isVideo = vi < video.packets.size() &&
            (ai == audio.packets.size() || video.packets[vi].pts <= audio.packets[ai].pts);
        const auto& packet = isVideo ? video.packets[vi++] : audio.packets[ai++];
        if (crash) std::this_thread::sleep_until(start + std::chrono::nanoseconds((std::max)(0LL, packet.pts) * 100));
        Hr(writer->WriteSample(isVideo ? videoStream : audioStream, packet.sample.Get()), "write encoded packet");
        if (crash && packet.pts >= nextFlush) { Hr(archive?archive->RequestFlush():RequestRecordingFlush(bytes.Get()), "periodic file flush"); nextFlush = packet.pts + kSecond; }
    }
    if(archive){
        if(crash){TerminateProcess(GetCurrentProcess(),73);std::abort();}
        Hr(writer->Finalize(),"finalize segments");Hr(archive->Flush(),"flush last segment");
        writer.Reset();Hr(archive->Close(),"close last segment");bytes.Reset();
        size_t videos=0,audios=0;unsigned part=1;
        for(;;++part){
            const auto path=RecordingPartPath(outputPath,part);if(!std::filesystem::exists(path))break;
            Hr(ValidateCompletedRecording(path.c_str()),"segment has both tracks");
            const auto v=Read(path.c_str(),true),a=Read(path.c_str(),false);
            test::CheckShellDuration(path.c_str(), (std::max)(
                v.packets.back().pts + v.packets.back().duration,
                a.packets.back().pts + a.packets.back().duration));
            videos+=v.packets.size();audios+=a.packets.size();
            Check(MFGetAttributeUINT32(v.packets.front().sample.Get(),MFSampleExtension_CleanPoint,FALSE)!=FALSE,"every segment starts on a keyframe");
        }
        if(!Reordered(video))Check(part>3,"multiple split boundaries exercised");
        Check(videos==video.packets.size()&&audios==audio.packets.size(),"all video and audio packets saved exactly once");
    }else Finish(writer.Get(), bytes.Get(), crash);
}
void AudioType(IMFMediaType* type, bool encoded) {
    Hr(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "audio major type");
    Hr(type->SetGUID(MF_MT_SUBTYPE, encoded ? MFAudioFormat_AAC : MFAudioFormat_PCM), "audio subtype");
    Hr(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2), "audio channels");
    Hr(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000), "audio frequency");
    Hr(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "audio bits");
    Hr(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, encoded ? 1 : 4), "audio alignment");
    Hr(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, encoded ? 24000 : 192000), "audio bitrate");
    if (encoded) Hr(type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0), "AAC raw payload");
}
ComPtr<IMFSample> RawSample(bool video, unsigned frame) {
    ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> buffer;
    Hr(MFCreateSample(&sample), "raw sample");
    const DWORD size = video ? kWidth * kHeight * 3 / 2 : 1600 * 4;
    Hr(MFCreateMemoryBuffer(size, &buffer), "raw buffer"); BYTE* data = nullptr;
    Hr(buffer->Lock(&data, nullptr, nullptr), "raw buffer lock");
    const bool pulse = frame % 60 >= 30 && frame % 60 < 33;
    if (video) { memset(data, pulse ? 220 : 16, kWidth * kHeight); memset(data + kWidth * kHeight, 128, kWidth * kHeight / 2); }
    else {
        auto* pcm = reinterpret_cast<short*>(data);
        for (unsigned i = 0; i < 1600; ++i) pcm[2 * i] = pcm[2 * i + 1] = pulse
            ? static_cast<short>(12000 * sin(i * 6.283185307179586 * 1000 / 48000)) : 0;
    }
    Hr(buffer->Unlock(), "raw buffer unlock"); Hr(buffer->SetCurrentLength(size), "raw size");
    Hr(sample->AddBuffer(buffer.Get()), "raw attach");
    const auto pts = frame * kSecond / kFps;
    Hr(sample->SetSampleTime(pts), "raw PTS"); Hr(sample->SetSampleDuration((frame + 1) * kSecond / kFps - pts), "raw duration");
    return sample;
}
void Encode(const wchar_t* path, const Options& options, bool crash) {
    auto attributes = Attributes(options.encoder == L"cpu", crash);
    ComPtr<IMFByteStream> bytes; ComPtr<IMFSinkWriter> writer;
    Hr(CreateRecordingFile(path, attributes.Get(), &bytes, &writer, options.codec), "HEVC encoding writer");
    auto output = Type();
    Hr(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "video major type");
    Hr(output->SetGUID(MF_MT_SUBTYPE, CodecSubtype(options.codec)), "video codec");
    Hr(output->SetUINT32(MF_MT_AVG_BITRATE, 2000000), "video bitrate");
    Hr(output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "progressive video");
    Hr(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, kWidth, kHeight), "video dimensions");
    Hr(MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, kFps, 1), "video FPS");
    Hr(MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "video pixels");
    DWORD video = 0, audio = 0; Hr(writer->AddStream(output.Get(), &video), "HEVC stream");
    auto input = Type(); Hr(output->CopyAllItems(input.Get()), "NV12 type attributes");
    Hr(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "NV12 type");
    Hr(ConfigureVideoInput(writer.Get(), video, input.Get(), options), "configured requested HEVC encoder/rate control");
    auto audioOut = Type(); AudioType(audioOut.Get(), true); Hr(writer->AddStream(audioOut.Get(), &audio), "AAC stream");
    auto audioIn = Type(); AudioType(audioIn.Get(), false); Hr(writer->SetInputMediaType(audio, audioIn.Get(), nullptr), "PCM input");
    Hr(writer->BeginWriting(), "begin HEVC recording");
    Hr(VerifyRateControl(writer.Get(), video, options), "requested rate control after BeginWriting");
    const auto start = std::chrono::steady_clock::now();
    for (unsigned frame = 0; frame < kFrames; ++frame) {
        if (crash) std::this_thread::sleep_until(start + std::chrono::nanoseconds(frame * kSecond / kFps * 100));
        auto v = RawSample(true, frame), a = RawSample(false, frame);
        Hr(writer->WriteSample(video, v.Get()), "write raw video"); Hr(writer->WriteSample(audio, a.Get()), "write raw audio");
        if (crash && frame % kFps == 0) Hr(RequestRecordingFlush(bytes.Get()), "periodic encoding file flush");
    }
    Hr(VerifyRateControl(writer.Get(), video, options), "requested rate control after encoding");
    Finish(writer.Get(), bytes.Get(), crash);
}
void Prefix(const Track& expected, const Track& recovered) {
    Check(recovered.packets.size() <= expected.packets.size(), "recovery cannot invent packets");
    for (size_t i = 0; i < recovered.packets.size(); ++i)
        Check(std::abs(recovered.packets[i].pts - expected.packets[i].pts) < 10000,
            "every retained encoded packet matches the complete recording prefix");
}
void Tail(const wchar_t* path, const Track& sourceVideo, const Track& sourceAudio, bool torn = false) {
    auto video = Read(path, true, false), audio = Read(path, false, false);
    Check(video.packets.size() > sourceVideo.packets.size() * 3 / 4, "most completed video fragments survive interruption");
    Check(audio.packets.size() > 400, "AAC is not held until Finalize");
    const auto vt = Times(video), at = Times(audio);
    // A retained encoded prefix may end with a P-frame whose B-frames would
    // follow later in decode order. Compare every packet to the complete file,
    // not a sorted-PTS cadence that falsely treats the final B-group as a gap.
    // Full-file cadence was already checked; this rejects ANY interior loss.
    Prefix(sourceVideo, video); Prefix(sourceAudio, audio);
    // An interrupted write can leave video data but not audio from the SAME
    // unfinished mdat. That trailing fragment is not atomic; require the intact
    // prefix and bound only that final fragment's partial loss. A process kill
    // after completed writes must still preserve closely matched stream tails.
    Check((std::min)(vt.back(), at.back()) > 9 * kSecond, "continuous common audio/video prefix survives");
    Check(std::abs(vt.back() - at.back()) < (torn ? 11 * kSecond / 10 : 1500000),
        torn ? "torn fragment loses at most its trailing audio/video portion" : "recovered video/audio tails within 150 ms");
}
std::wstring Quote(const std::wstring& text) { return L"\"" + text + L"\""; }
}

int wmain(int argc, wchar_t** argv) {
    Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM"); Hr(MFStartup(MF_VERSION), "MF");
    Options options; options.codec = Codec::Hevc; options.encoder = L"cpu";
    options.rateControl = RateControl::Bitrate; options.videoMbps = 2;
    std::wstring fixture, childOutput, outputDirectory; bool crashChild = false, noCrash = false, inspect = false;
    LONGLONG videoOffset = 0, audioOffset = 0;int splitMode=0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--fixture" && i + 1 < argc) fixture = argv[++i];
        else if (arg == L"--output-directory" && i + 1 < argc) outputDirectory = argv[++i];
        else if ((arg == L"--video-offset-ms" || arg == L"--audio-offset-ms") && i + 1 < argc) {
            wchar_t* end = nullptr; const auto ms = wcstol(argv[++i], &end, 10);
            Check(end != argv[i] && !*end && ms >= 0 && ms <= 1000, "controlled offset range 0..1000 ms");
            (arg == L"--video-offset-ms" ? videoOffset : audioOffset) = ms * 10000LL;
        }
        else if (arg == L"--crash-child" && i + 1 < argc) { crashChild = true; childOutput = argv[++i]; }
        else if (arg == L"--hardware") options.encoder.clear();
        else if (arg == L"--cpu") options.encoder = L"cpu";
        else if (arg == L"--quality") options.rateControl = RateControl::Quality;
        else if (arg == L"--no-crash") noCrash = true;
        else if (arg == L"--split-time") splitMode=1;
        else if (arg == L"--split-size") splitMode=2;
        else if (arg == L"--inspect-fixture") inspect = true;
        else { fwprintf(stderr, L"Unknown argument: %ls\n", arg.c_str()); return 2; }
    }
    if (inspect) { Check(!fixture.empty(), "inspect requires fixture"); Read(fixture.c_str(), true); Read(fixture.c_str(), false); return 0; }
    if (crashChild) {
        if (fixture.empty()) Encode(childOutput.c_str(), options, true);
        else Remux(fixture.c_str(), childOutput.c_str(), true, videoOffset, audioOffset,splitMode);
        return 1;
    }
    Check(UsesFragmentedContainer(Codec::Hevc) && UsesFragmentedContainer(Codec::Av1), "HEVC and AV1 select interrupted-recording protection");
    const auto root = outputDirectory.empty() ? std::filesystem::temp_directory_path() : std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(root);
    const auto directory = root /
        (L"LLCV-modern-container-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(directory);
    const auto good = directory / L"complete.mp4", truncated = directory / L"torn-tail.mp4", crash = directory / L"terminated.mp4";
    wprintf(L"fixtures: %ls\n", directory.c_str());
    if(splitMode){
        Check(!fixture.empty(),"splitting requires encoded fixture");
        Remux(fixture.c_str(),good.c_str(),false,videoOffset,audioOffset,splitMode);
        MFShutdown();CoUninitialize();puts("Split packet continuity passed.");return 0;
    }
    if (fixture.empty()) Encode(good.c_str(), options, false); else Remux(fixture.c_str(), good.c_str(), false, videoOffset, audioOffset);
    auto video = Read(good.c_str(), true), audio = Read(good.c_str(), false);
    test::CheckShellDuration(good.c_str(), (std::max)(
        video.packets.back().pts + video.packets.back().duration,
        audio.packets.back().pts + audio.packets.back().duration));
    Check(video.packets.size() == kFrames, "all 360 frames saved");
    Continuous(video, kSecond / kFps); Continuous(audio, 1024 * kSecond / 48000);
    if (!fixture.empty()) {
        auto expectedVideo = Read(fixture.c_str(), true), expectedAudio = Read(fixture.c_str(), false);
        Shift(expectedVideo, videoOffset); Shift(expectedAudio, audioOffset);
        // MF's MP4 reader normalizes fragmented track edit-list origins. The
        // external ffprobe/decode regression is the oracle for nonzero origins;
        // native packet reads still verify every frame, order and cadence here.
        SameTimeline(expectedVideo, video, Reordered(expectedVideo) || videoOffset != 0);
        SameTimeline(expectedAudio, audio, audioOffset != 0);
    }
    Hr(ValidateCompletedRecording(good.c_str()), "saved output validates without optional decoder");
    test::FragmentedSnapshot(good.c_str(), truncated.c_str());
    {
        ComPtr<IMFByteStream> bytes;
        Hr(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, truncated.c_str(), &bytes), "open test-owned truncation fixture");
        const auto boxes = Scan(bytes.Get());
        Check(boxes.lastMediaSize > 16, "last media fragment present");
        Hr(bytes->SetLength(boxes.lastMediaStart + boxes.lastMediaSize / 2), "simulate torn final media fragment");
        Hr(bytes->Close(), "close torn file");
    }
    Tail(truncated.c_str(), video, audio, true);
    if (!noCrash) {
        wchar_t executable[32768]{}; Check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "test executable path");
        std::wstring command = Quote(executable) + L" --crash-child " + Quote(crash.wstring());
        if (!fixture.empty()) command += L" --fixture " + Quote(fixture);
        if (videoOffset) command += L" --video-offset-ms " + std::to_wstring(videoOffset / 10000);
        if (audioOffset) command += L" --audio-offset-ms " + std::to_wstring(audioOffset / 10000);
        if (options.encoder.empty()) command += L" --hardware";
        if (options.rateControl == RateControl::Quality) command += L" --quality";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "spawn owned abrupt-stop child");
        const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
        if (waited != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 99); WaitForSingleObject(process.hProcess, 5000); }
        DWORD exitCode = 0; GetExitCodeProcess(process.hProcess, &exitCode); CloseHandle(process.hThread); CloseHandle(process.hProcess);
        Check(waited == WAIT_OBJECT_0 && exitCode == 73, "owned child terminated without Finalize");
        Tail(crash.c_str(), video, audio);
    }
    MFShutdown(); CoUninitialize(); puts("Modern recording container regression passed."); return 0;
}
