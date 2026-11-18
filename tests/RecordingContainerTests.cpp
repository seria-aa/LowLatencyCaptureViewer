#include "recording/RecordingContainer.h"
#include "recording/RecordingStorage.h"
#include "recording/EncoderSupport.h"
#include "recording/EncoderProbe.h"
#include "recording/RecordingTimeline.h"
#include "RecordingFileTestSupport.h"
#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
using Microsoft::WRL::ComPtr;
using namespace llcv::recording;
namespace {
void Check(bool ok, const char* text) { if (!ok) { printf("FAIL %s\n", text); std::exit(1); } }
void Hr(HRESULT hr, const char* text) { if (FAILED(hr)) { printf("FAIL %s %08lX\n", text, static_cast<unsigned long>(hr)); std::exit(1); } }
constexpr UINT32 width = 320, height = 192, fps = 30, frames = 360;
LONGLONG Time(UINT32 frame) { return frame * 10000000LL / fps; }
ComPtr<IMFMediaType> Type() { ComPtr<IMFMediaType> result; Hr(MFCreateMediaType(&result), "media type"); return result; }
void Audio(IMFMediaType* type, bool encoded) {
    Hr(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "audio major");
    Hr(type->SetGUID(MF_MT_SUBTYPE, encoded ? MFAudioFormat_AAC : MFAudioFormat_PCM), "audio subtype");
    Hr(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2), "channels");
    Hr(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000), "sample rate");
    Hr(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "sample size");
    Hr(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, encoded ? 1 : 4), "alignment");
    Hr(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, encoded ? 24000 : 192000), "audio rate");
    if (encoded) Hr(type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0), "AAC payload");
}
ComPtr<IMFSample> Sample(DWORD size, LONGLONG time, LONGLONG duration, unsigned seed) {
    ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> buffer;
    Hr(MFCreateSample(&sample), "sample"); Hr(MFCreateMemoryBuffer(size, &buffer), "buffer");
    BYTE* data = nullptr; Hr(buffer->Lock(&data, nullptr, nullptr), "lock");
    for (DWORD i = 0; i < size; ++i) data[i] = static_cast<BYTE>((i / 16 + seed) % 220 + 16);
    Hr(buffer->Unlock(), "unlock"); Hr(buffer->SetCurrentLength(size), "length");
    Hr(sample->AddBuffer(buffer.Get()), "attach"); Hr(sample->SetSampleTime(time), "PTS");
    Hr(sample->SetSampleDuration(duration), "duration"); return sample;
}
struct Boxes { QWORD length = 0, lastComplete = 0, firstMediaEnd = 0; unsigned movies = 0, fragments = 0, media = 0; };
Boxes Scan(IMFByteStream* bytes) {
    Boxes result; Hr(bytes->GetLength(&result.length), "file length");
    QWORD position = 0;
    while (position + 8 <= result.length) {
        BYTE header[16]{}; ULONG read = 0; Hr(bytes->SetCurrentPosition(position), "box seek");
        Hr(bytes->Read(header, 8, &read), "box read"); Check(read == 8, "box header available");
        QWORD size = 0; for (int i = 0; i < 4; ++i) size = (size << 8) | header[i];
        unsigned headerSize = 8;
        if (size == 1) { Hr(bytes->Read(header + 8, 8, &read), "extended box"); Check(read == 8, "extended header available"); size = 0; for (int i = 8; i < 16; ++i) size = (size << 8) | header[i]; headerSize = 16; }
        if (size < headerSize || size > result.length - position) break;
        if (!memcmp(header + 4, "moov", 4)) ++result.movies;
        if (!memcmp(header + 4, "moof", 4)) ++result.fragments;
        if (!memcmp(header + 4, "mdat", 4)) { ++result.media; if (!result.firstMediaEnd) result.firstMediaEnd = position + size; }
        position += size; result.lastComplete = position;
    }
    return result;
}
void Encode(const wchar_t* path, const Options& options, bool terminate) {
    ComPtr<IMFAttributes> attributes; Hr(MFCreateAttributes(&attributes, 2), "attributes");
    Hr(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, options.encoder != L"cpu"), "encoder policy");
    if (!terminate) Hr(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE), "probe only unthrottled");
    ComPtr<IMFByteStream> bytes; ComPtr<IMFSinkWriter> writer;
    Hr(CreateRecordingFile(path, attributes.Get(), &bytes, &writer, options.codec), "recording writer");
    auto output = Type(); Hr(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "video major");
    Hr(output->SetGUID(MF_MT_SUBTYPE, CodecSubtype(options.codec)), "video subtype");
    Hr(output->SetUINT32(MF_MT_AVG_BITRATE, 2000000), "video bitrate");
    Hr(output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "progressive");
    Hr(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height), "dimensions");
    Hr(MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, fps, 1), "frame rate");
    Hr(MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "pixel aspect");
    DWORD video = 0, audio = 0; Hr(writer->AddStream(output.Get(), &video), "add video");
    auto input = Type(); Hr(output->CopyAllItems(input.Get()), "copy video");
    Hr(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "NV12");
    Hr(ConfigureVideoInput(writer.Get(), video, input.Get(), options), "configure encoder");
    auto audioOutput = Type(); Audio(audioOutput.Get(), true); Hr(writer->AddStream(audioOutput.Get(), &audio), "add audio");
    auto audioInput = Type(); Audio(audioInput.Get(), false); Hr(writer->SetInputMediaType(audio, audioInput.Get(), nullptr), "configure audio");
    Hr(writer->BeginWriting(), "begin");
    const auto start = std::chrono::steady_clock::now();
    for (UINT32 frame = 0; frame < frames; ++frame) {
        if (terminate) std::this_thread::sleep_until(start + std::chrono::nanoseconds(Time(frame) * 100));
        auto v = Sample(width * height * 3 / 2, Time(frame), Time(frame + 1) - Time(frame), frame);
        auto a = Sample(1600 * 4, Time(frame), Time(frame + 1) - Time(frame), 0);
        Hr(writer->WriteSample(video, v.Get()), "write video"); Hr(writer->WriteSample(audio, a.Get()), "write audio");
        if (terminate && frame % fps == 0) Hr(RequestRecordingFlush(bytes.Get()), "periodic storage flush");
    }
    if (terminate) {
        // No destructor / Finalize / normal app-exit path is allowed here.
        TerminateProcess(GetCurrentProcess(), 73);
    }
    Hr(writer->Finalize(), "finalize"); Hr(bytes->Flush(), "final flush");
    const auto boxes = Scan(bytes.Get());
    printf("finalized codec=%d bytes=%llu movies=%u fragments=%u media=%u\n", static_cast<int>(options.codec), boxes.length, boxes.movies, boxes.fragments, boxes.media);
#ifdef LLCV_RECORDING_MODERN_MP4
    Check(boxes.movies == 1 && boxes.fragments == 0 && boxes.media == 1,
        "successful finalization exposes a conventional MP4 index");
#else
    Check(boxes.movies == 1 && boxes.fragments > 1 && boxes.media > 1,
        "playback-only build's native H.264 test fallback remains fragmented");
#endif
    bytes->Close();
}
unsigned Read(const wchar_t* path, bool video, bool exact, LONGLONG* lastTime = nullptr) {
    ComPtr<IMFSourceReader> reader; Hr(MFCreateSourceReaderFromURL(path, nullptr, &reader), "open recorded file");
    Hr(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "unselect");
    const DWORD stream = video ? static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM) : static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    Hr(reader->SetStreamSelection(stream, TRUE), "select");
    std::vector<LONGLONG> times; bool eof = false;
    for (unsigned reads = 0; reads < frames * 4; ++reads) {
        ComPtr<IMFSample> sample; LONGLONG time = 0; DWORD flags = 0;
        const HRESULT hr = reader->ReadSample(stream, 0, nullptr, &flags, &time, &sample);
        if (FAILED(hr)) { printf("read ended hr=%08lX\n", static_cast<unsigned long>(hr)); Check(!exact && !times.empty(), "partial file retains earlier packets"); break; }
        if (sample) { DWORD length = 0; Hr(sample->GetTotalLength(&length), "packet size"); Check(length > 0, "nonempty packet"); times.push_back(time); }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { eof = true; break; }
    }
    std::sort(times.begin(), times.end());
    Check(!times.empty(), "recorded stream not empty");
    if (video) for (size_t i = 1; i < times.size(); ++i) Check(std::abs((times[i] - times[i - 1]) - 10000000LL / fps) < 10000, "no video gaps across fragments");
    else for (size_t i = 1; i < times.size(); ++i) Check(std::abs((times[i] - times[i - 1]) - 1024LL * 10000000 / 48000) < 10000, "no audio gaps across fragments");
    if (exact) { Check(eof, "finite file"); if (video) Check(times.size() == frames, "all frames present after finalization"); }
    if (lastTime) *lastTime = times.back();
    printf("%s %s packets=%zu end=%.3f\n", exact ? "complete" : "recovered", video ? "video" : "audio", times.size(), times.back() / 1e7);
    return static_cast<unsigned>(times.size());
}
void Timing(const std::filesystem::path& directory, LONGLONG videoOffset, LONGLONG audioOffset) {
    const auto path = directory / (L"timing-" + std::to_wstring(videoOffset) + L"-" + std::to_wstring(audioOffset) + L".mp4");
    auto videoTime = [=](unsigned frame) {
        return videoOffset + frame * 200000LL + (frame ? (static_cast<int>(frame % 5) - 2) * 15000 : 0);
    };
    ComPtr<IMFAttributes> attrs; Hr(MFCreateAttributes(&attrs, 2), "timing attributes");
    Hr(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE), "CPU timing encoder");
    Hr(attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE), "timing probe unthrottled");
    ComPtr<IMFByteStream> bytes; ComPtr<IMFSinkWriter> writer;
    Hr(CreateRecordingFile(path.c_str(), attrs.Get(), &bytes, &writer), "timing file");
    auto output = Type(); output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video); output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, 2000000); output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height); MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, 50, 1);
    MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD video = 0, audio = 0; Hr(writer->AddStream(output.Get(), &video), "timing video");
    auto input = Type(); Hr(output->CopyAllItems(input.Get()), "timing input"); input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    Options options; options.encoder = L"cpu"; options.rateControl = RateControl::Bitrate; options.videoMbps = 2;
    Hr(ConfigureVideoInput(writer.Get(), video, input.Get(), options), "timing encoder");
    auto audioOut = Type(); Audio(audioOut.Get(), true); Hr(writer->AddStream(audioOut.Get(), &audio), "timing AAC");
    auto audioIn = Type(); Audio(audioIn.Get(), false); Hr(writer->SetInputMediaType(audio, audioIn.Get(), nullptr), "timing PCM");
    Hr(writer->BeginWriting(), "timing begin"); RecordingTimeline timeline(true, video, audio);
    {
        RecordingTimeline invalid(true, video, audio);
        auto tooLate = Sample(4, 5 * 10000000LL + 1, 200000, 0);
        Check(invalid.Push(writer.Get(), true, tooLate.Get()) == MF_E_INVALID_TIMESTAMP, "video prefix is bounded before writing");
        Check(invalid.Push(writer.Get(), false, tooLate.Get()) == MF_E_INVALID_TIMESTAMP, "audio prefix is bounded before allocation");
        auto negative = Sample(4, -1, 200000, 0);
        Check(invalid.Push(writer.Get(), true, negative.Get()) == MF_E_INVALID_TIMESTAMP, "negative timeline rejected");
        Check(invalid.Push(nullptr, true, negative.Get()) == E_POINTER, "missing timeline writer rejected");
    }
    for (unsigned frame = 0; frame < 150; ++frame) {
        auto v = Sample(width * height * 3 / 2, videoTime(frame), 200000, frame);
        auto a = Sample(960 * 4, audioOffset + frame * 200000LL, 200000, 0);
        ComPtr<IMFMediaBuffer> buffer; Hr(a->GetBufferByIndex(0, &buffer), "tone buffer");
        BYTE* data = nullptr; Hr(buffer->Lock(&data, nullptr, nullptr), "tone lock");
        auto pcm = reinterpret_cast<short*>(data);
        for (unsigned sample = 0; sample < 960; ++sample)
            pcm[sample * 2] = pcm[sample * 2 + 1] = frame >= 25 && frame < 30
                ? static_cast<short>(12000 * sin(sample * 6.28318530718 * 1000 / 48000)) : 0;
        Hr(buffer->Unlock(), "tone unlock");
        // Deliver the earlier stream first, matching the recorder worker merge.
        if (videoTime(frame) <= audioOffset + frame * 200000LL) {
            Hr(timeline.Push(writer.Get(), true, v.Get()), "timing video push");
            Hr(timeline.Push(writer.Get(), false, a.Get()), "timing audio push");
        } else {
            Hr(timeline.Push(writer.Get(), false, a.Get()), "timing audio push");
            Hr(timeline.Push(writer.Get(), true, v.Get()), "timing video push");
        }
    }
    Hr(timeline.Finish(writer.Get()), "last pending frame");
    Hr(timeline.Finish(writer.Get()), "pending frame cannot be written twice"); Hr(writer->Finalize(), "timing finalize");
    writer.Reset(); Hr(bytes->Close(), "timing close"); bytes.Reset();
    {
        ComPtr<IMFSourceReader> reader; Hr(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "timing video reader");
        constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE); reader->SetStreamSelection(stream, TRUE);
        unsigned count = 0; LONGLONG maximumError = 0;
        for (unsigned read = 0; read < 200; ++read) {
            ComPtr<IMFSample> sample; DWORD flags = 0; LONGLONG time = 0;
            Hr(reader->ReadSample(stream, 0, nullptr, &flags, &time, &sample), "timing video packet");
            if (sample) {
                const LONGLONG expected = videoOffset && !count ? 0 : videoTime(count - (videoOffset ? 1 : 0));
                maximumError = (std::max)(maximumError, std::abs(time - expected)); ++count;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        Check(count == 150u + (videoOffset ? 1u : 0u), "one bounded prefix holds the first image without losing any frames");
        Check(maximumError < 10000, "fragment boundary preserves variable video timestamps within 1 ms");
    }
    {
        ComPtr<IMFSourceReader> reader; Hr(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader), "timing audio reader");
        constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE); reader->SetStreamSelection(stream, TRUE);
        auto pcmType = Type(); Audio(pcmType.Get(), false); Hr(reader->SetCurrentMediaType(stream, nullptr, pcmType.Get()), "decode timing audio");
        double pulseTime = -1;
        for (unsigned read = 0; read < 200 && pulseTime < 0; ++read) {
            ComPtr<IMFSample> sample; DWORD flags = 0; LONGLONG time = 0;
            Hr(reader->ReadSample(stream, 0, nullptr, &flags, &time, &sample), "timing audio packet");
            if (sample) {
                ComPtr<IMFMediaBuffer> buffer; Hr(sample->ConvertToContiguousBuffer(&buffer), "decoded PCM buffer");
                BYTE* data = nullptr; DWORD length = 0; Hr(buffer->Lock(&data, nullptr, &length), "decoded PCM lock");
                const auto pcm = reinterpret_cast<const short*>(data);
                for (DWORD i = 0; i < length / 4; ++i) if (std::abs(static_cast<int>(pcm[2 * i])) > 5000) {
                    pulseTime = time / 1e7 + i / 48000.0; break;
                }
                Hr(buffer->Unlock(), "decoded PCM unlock");
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        const double expected = 0.5 + audioOffset / 1e7;
        Check(pulseTime >= 0 && std::abs(pulseTime - expected) < .002, "initial audio offset is preserved within 2 ms");
        printf("timing videoOffset=%.3fms audioOffset=%.3fms toneResidual=%.3fms\n", videoOffset / 10000.0, audioOffset / 10000.0, (pulseTime - expected) * 1000);
    }
}
}
int wmain(int argc, wchar_t** argv) {
    Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM"); Hr(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MF");
    Options options; options.encoder = L"cpu"; options.rateControl = RateControl::Bitrate; options.videoMbps = 2;
    if (argc == 3 && (std::wstring(argv[1]) == L"--crash-child" || std::wstring(argv[1]) == L"--crash-child-hardware")) {
        if (std::wstring(argv[1]) == L"--crash-child-hardware") options.encoder.clear();
        Encode(argv[2], options, true); return 1;
    }
    if (argc == 3 && (std::wstring(argv[1]) == L"--hevc" || std::wstring(argv[1]) == L"--hardware-h264")) {
        options.encoder.clear(); options.codec = std::wstring(argv[1]) == L"--hardware-h264" ? Codec::H264 : Codec::Hevc;
        Encode(argv[2], options, false);
#ifdef LLCV_RECORDING_MODERN_MP4
        test::CheckShellDuration(argv[2], Time(frames));
#endif
        Read(argv[2], true, true); Read(argv[2], false, true);
        Hr(ValidateCompletedRecording(argv[2]), "hardware output structural check"); return 0;
    }
    const bool hardware = argc == 2 && std::wstring(argv[1]) == L"--hardware";
    Check(UsesFragmentedContainer(Codec::H264), "H.264 selects fragmented MP4");
    Check(UsesFragmentedContainer(Codec::Hevc) && UsesFragmentedContainer(Codec::Av1), "all recording codecs require crash-tolerant storage");
    Check(RequiresZeroOriginTimeline(Codec::H264) && RequiresZeroOriginTimeline(Codec::Hevc) &&
        RequiresZeroOriginTimeline(Codec::Av1), "all codecs preserve initial track offsets for Windows fragmented readers");
#ifndef LLCV_RECORDING_MODERN_MP4
    {
        ComPtr<IMFByteStream> bytes; ComPtr<IMFSinkWriter> writer;
        Hr(MFCreateTempFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, &bytes), "no-SDK test stream");
        for (const auto codec : {Codec::Hevc, Codec::Av1})
            Check(CreateRecordingWriter(bytes.Get(), nullptr, &writer, codec) == MF_E_TOPO_CODEC_NOT_FOUND && !writer,
                "missing private mux dependency never falls back to unprotected MP4");
    }
#endif
    if (hardware) options.encoder.clear();
    const auto directory = std::filesystem::temp_directory_path() / (L"LLCV-container-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(directory);
    const auto good = directory / L"complete.mp4", crash = directory / L"terminated.mp4";
    wprintf(L"fixtures: %ls\n", directory.c_str());
    if (!hardware) { Timing(directory, 0, 100000); Timing(directory, 150000, 0); Timing(directory, 0, 0); }
    Encode(good.c_str(), options, false); Read(good.c_str(), true, true); Read(good.c_str(), false, true);
#ifdef LLCV_RECORDING_MODERN_MP4
    test::CheckShellDuration(good.c_str(), Time(frames));
#endif
    Hr(ValidateCompletedRecording(good.c_str()), "finished file structural check");
    const auto truncated = directory / L"truncated-last-fragment.mp4";
#ifdef LLCV_RECORDING_MODERN_MP4
    test::FragmentedSnapshot(good.c_str(), truncated.c_str());
#else
    Check(CopyFileW(good.c_str(), truncated.c_str(), TRUE) != FALSE, "copy native fragmented fixture");
#endif
    {
        ComPtr<IMFByteStream> bytes;
        Hr(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE, truncated.c_str(), &bytes), "open truncation fixture");
        const auto boxes = Scan(bytes.Get()); Check(boxes.length > 16384, "truncation fixture size");
        Hr(bytes->SetLength(boxes.length - 16384), "truncate final fragment"); Hr(bytes->Close(), "close truncated fixture");
    }
    Check(Read(truncated.c_str(), true, false) > frames * 3 / 4, "earlier video fragments survive torn tail");
    Check(Read(truncated.c_str(), false, false) > frames, "earlier audio fragments survive torn tail");
    {
        ComPtr<IMFByteStream> bytes; ComPtr<IMFSinkWriter> writer;
        Check(FAILED(CreateRecordingFile(good.c_str(), nullptr, &bytes, &writer)), "existing file never overwritten");
        Check(FAILED(CreateRecordingFile(nullptr, nullptr, &bytes, &writer)), "missing path rejected");
        Check(FAILED(CreateRecordingWriter(nullptr, nullptr, &writer)), "missing stream rejected");
        Check(FAILED(ValidateCompletedRecording(nullptr)), "missing validation path rejected");
        const auto empty = directory / L"empty.mp4";
        Hr(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_EXIST, MF_FILEFLAGS_NONE, empty.c_str(), &bytes), "empty fixture");
        Hr(bytes->Close(), "close empty fixture"); bytes.Reset();
        Check(FAILED(ValidateCompletedRecording(empty.c_str())), "empty file never called saved");
        const auto invalid = directory / L"invalid-codec.mp4";
        Check(FAILED(CreateRecordingFile(invalid.c_str(), nullptr, &bytes, &writer, static_cast<Codec>(99))), "invalid codec rejected");
        Check(!std::filesystem::exists(invalid), "invalid codec does not create a file");
        const auto silent = directory / L"video-only.mp4";
        Hr(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_EXIST, MF_FILEFLAGS_NONE, silent.c_str(), &bytes), "video-only fixture");
        Hr(detail::WriteEncoderProbe(bytes.Get(), options), "video-only encode");
        Hr(bytes->Close(), "close video-only fixture"); bytes.Reset();
        Check(FAILED(ValidateCompletedRecording(silent.c_str())), "missing recorded audio never called saved");
    }
    wchar_t executable[32768]{}; Check(GetModuleFileNameW(nullptr, executable, 32768) != 0, "self path");
    std::wstring command = L"\"" + std::wstring(executable) + (hardware ? L"\" --crash-child-hardware \"" : L"\" --crash-child \"") + crash.wstring() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "spawn crash child");
    const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
    if (waited != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 99); WaitForSingleObject(process.hProcess, 5000); }
    DWORD code = 0; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    Check(waited == WAIT_OBJECT_0 && code == 73, "forced termination before finalization");
    LONGLONG videoEnd = 0, audioEnd = 0;
    const auto recoveredFrames = Read(crash.c_str(), true, false, &videoEnd);
    Check(recoveredFrames < frames && recoveredFrames > frames * 3 / 4, "recent completed video fragments survive interruption");
    Check(Read(crash.c_str(), false, false, &audioEnd) > frames, "audio is not held until Finalize");
    Check(std::abs(videoEnd - audioEnd) < 1000000, "surviving A/V tails remain within 100 ms");
    MFShutdown(); CoUninitialize(); puts("Fragmented recording container tests passed.");
}
