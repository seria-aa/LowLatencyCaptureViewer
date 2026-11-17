#include "recording/RecordingContainer.h"
#include "recording/FragmentedMp4Sink.h"
#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace llcv::recording;
namespace {
void Check(bool value, const char* text) { if (!value) { printf("FAIL %s\n", text); std::exit(1); } }
void Hr(HRESULT hr, const char* text) { if (FAILED(hr)) { printf("FAIL %s %08lX\n", text, static_cast<unsigned long>(hr)); std::exit(1); } }
enum class Fault { None, DiskFull, WriteError, ZeroWrite, SeekError, FlushError, FinalIndexError };
constexpr HRESULT kDiskFull = HRESULT_FROM_WIN32(ERROR_DISK_FULL);
constexpr HRESULT kWriteError = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
constexpr HRESULT kSeekError = HRESULT_FROM_WIN32(ERROR_SEEK);
class Bytes final : public IMFByteStream {
    std::atomic<ULONG> references_{1};
    std::vector<BYTE> data_;
    QWORD position_ = 0;
    QWORD FinalIndexOffset(const BYTE* bytes, ULONG count) const {
        // Parse top-level boxes in the prospective write, not byte patterns
        // inside compressed media. The second moov is the hybrid commit index.
        const auto length = (std::max)(QWORD(data_.size()), position_ + count);
        const auto byte = [&](QWORD at) -> BYTE {
            return at >= position_ && at - position_ < count ? bytes[at - position_] :
                (at < data_.size() ? data_[at] : 0);
        };
        unsigned movies = 0;
        for (QWORD at = 0; at + 8 <= length;) {
            QWORD size = 0; for (unsigned i = 0; i < 4; ++i) size = (size << 8) | byte(at + i);
            if (byte(at + 4) == 'm' && byte(at + 5) == 'o' && byte(at + 6) == 'o' && byte(at + 7) == 'v')
                if (++movies == 2) return at;
            unsigned header = 8;
            if (size == 1) {
                if (at + 16 > length) break;
                size = 0; for (unsigned i = 8; i < 16; ++i) size = (size << 8) | byte(at + i);
                header = 16;
            }
            if (size < header || size > length - at) break;
            at += size;
        }
        return UINT64_MAX;
    }
public:
    Fault fault = Fault::None;
    unsigned failAfterWrites = 0, writes = 0, seeks = 0, flushes = 0;
    bool closed = false;
    bool finalIndexRejected = false;
    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER; *object = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IMFByteStream)) return E_NOINTERFACE;
        *object = static_cast<IMFByteStream*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override { const auto left = --references_; if (!left) delete this; return left; }
    STDMETHODIMP GetCapabilities(DWORD* capabilities) override {
        if (!capabilities) return E_POINTER;
        *capabilities = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_WRITABLE | MFBYTESTREAM_IS_SEEKABLE; return S_OK;
    }
    STDMETHODIMP GetLength(QWORD* length) override { if (!length) return E_POINTER; *length = data_.size(); return S_OK; }
    STDMETHODIMP SetLength(QWORD length) override {
        if (length > 16 * 1024 * 1024) return kDiskFull;
        data_.resize(static_cast<size_t>(length)); return S_OK;
    }
    STDMETHODIMP GetCurrentPosition(QWORD* position) override {
        if (!position) return E_POINTER;
        *position = position_; return S_OK;
    }
    STDMETHODIMP SetCurrentPosition(QWORD position) override {
        ++seeks; if (fault == Fault::SeekError) return kSeekError;
        if (position > 16 * 1024 * 1024) return E_INVALIDARG;
        position_ = position; return S_OK;
    }
    STDMETHODIMP IsEndOfStream(BOOL* end) override { if (!end) return E_POINTER; *end = position_ >= data_.size(); return S_OK; }
    STDMETHODIMP Read(BYTE* bytes, ULONG count, ULONG* read) override {
        if (!read || (!bytes && count)) return E_POINTER; *read = 0;
        if (position_ >= data_.size()) return S_OK;
        *read = static_cast<ULONG>((std::min)(static_cast<QWORD>(count), data_.size() - position_));
        memcpy(bytes, data_.data() + position_, *read); position_ += *read; return S_OK;
    }
    STDMETHODIMP BeginRead(BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override { return E_NOTIMPL; }
    STDMETHODIMP EndRead(IMFAsyncResult*, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP Write(const BYTE* bytes, ULONG count, ULONG* written) override {
        if (!written || (!bytes && count)) return E_POINTER; *written = 0;
        if (closed) return MF_E_SHUTDOWN;
        const bool failing = writes++ >= failAfterWrites;
        if (failing && fault == Fault::DiskFull) return kDiskFull;
        if (failing && fault == Fault::WriteError) return kWriteError;
        if (failing && fault == Fault::ZeroWrite) return S_OK;
        if (fault == Fault::FinalIndexError) {
            const auto offset = FinalIndexOffset(bytes, count);
            if (offset >= position_ && offset - position_ < count) {
                // A short failing write exactly at the final-index boundary:
                // preceding fragments reach storage, the new index does not.
                count = static_cast<ULONG>(offset - position_); finalIndexRejected = true;
                printf("reject final index at=%llu write-position=%llu prefix=%lu\n", offset, position_, count);
            }
        }
        if (position_ > 16 * 1024 * 1024 || count > 16 * 1024 * 1024 - position_) return kDiskFull;
        if (position_ + count > data_.size()) data_.resize(static_cast<size_t>(position_ + count));
        memcpy(data_.data() + position_, bytes, count); position_ += count; *written = count;
        return finalIndexRejected ? kWriteError : S_OK;
    }
    STDMETHODIMP BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override { return E_NOTIMPL; }
    STDMETHODIMP EndWrite(IMFAsyncResult*, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD, QWORD* position) override {
        const LONGLONG base = origin == msoBegin ? 0 : static_cast<LONGLONG>(position_);
        if (offset < -base || offset > 16 * 1024 * 1024 - base) return E_INVALIDARG;
        const HRESULT hr = SetCurrentPosition(static_cast<QWORD>(base + offset));
        if (position) *position = position_; return hr;
    }
    STDMETHODIMP Flush() override { ++flushes; return fault == Fault::FlushError ? kWriteError : S_OK; }
    STDMETHODIMP Close() override { closed = true; return S_OK; }
};
struct Packet { ComPtr<IMFSample> sample; LONGLONG time = 0; bool video = false; };
struct Fixture { ComPtr<IMFMediaType> video, audio; std::vector<Packet> packets; };
Fixture Load(const wchar_t* path) {
    Fixture result;
    for (const bool video : {true, false}) {
        ComPtr<IMFSourceReader> reader; Hr(MFCreateSourceReaderFromURL(path, nullptr, &reader), "fixture reader");
        const DWORD stream = static_cast<DWORD>(video ? MF_SOURCE_READER_FIRST_VIDEO_STREAM : MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        auto& type = video ? result.video : result.audio;
        Hr(reader->GetNativeMediaType(stream, 0, &type), "fixture native type");
        Hr(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "unselect fixture");
        Hr(reader->SetStreamSelection(stream, TRUE), "select fixture");
        Hr(reader->SetCurrentMediaType(stream, nullptr, type.Get()), "fixture encoded passthrough");
        bool ended = false;
        for (unsigned reads = 0; reads < 10000; ++reads) {
            Packet packet; packet.video = video; DWORD flags = 0;
            Hr(reader->ReadSample(stream, 0, nullptr, &flags, &packet.time, &packet.sample), "fixture packet");
            if (packet.sample) result.packets.push_back(std::move(packet));
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { ended = true; break; }
        }
        Check(ended, "fixture reading bounded");
    }
    std::stable_sort(result.packets.begin(), result.packets.end(), [](const Packet& a, const Packet& b) { return a.time < b.time; });
    return result;
}
HRESULT Exercise(const Fixture& fixture, Bytes* bytes, bool invalidTime = false) {
    ComPtr<IMFAttributes> attributes; Hr(MFCreateAttributes(&attributes, 2), "fault attributes");
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    ComPtr<IMFSinkWriter> writer;
    HRESULT hr = CreateRecordingWriter(bytes, attributes.Get(), &writer, Codec::Hevc);
    if (FAILED(hr)) return hr;
    DWORD video = 0, audio = 0;
    if (FAILED(hr = writer->AddStream(fixture.video.Get(), &video)) ||
        FAILED(hr = writer->SetInputMediaType(video, fixture.video.Get(), nullptr)) ||
        FAILED(hr = writer->AddStream(fixture.audio.Get(), &audio)) ||
        FAILED(hr = writer->SetInputMediaType(audio, fixture.audio.Get(), nullptr)) ||
        FAILED(hr = writer->BeginWriting())) return hr;
    for (const auto& packet : fixture.packets) {
        if (invalidTime && packet.video && packet.time > 1000000) {
            // A repeated decode timestamp is invalid even for reordered video.
            // Clone metadata instead of mutating a sample still owned by MF.
            ComPtr<IMFSample> invalid; Hr(MFCreateSample(&invalid), "invalid timestamp sample");
            Hr(packet.sample->CopyAllItems(invalid.Get()), "invalid timestamp attributes");
            DWORD count = 0; Hr(packet.sample->GetBufferCount(&count), "invalid sample buffers");
            for (DWORD i = 0; i < count; ++i) {
                ComPtr<IMFMediaBuffer> buffer; Hr(packet.sample->GetBufferByIndex(i, &buffer), "shared fixture buffer");
                Hr(invalid->AddBuffer(buffer.Get()), "attach shared fixture buffer");
            }
            LONGLONG duration = 0; Hr(packet.sample->GetSampleDuration(&duration), "invalid sample duration");
            Hr(invalid->SetSampleDuration(duration), "copy invalid sample duration");
            Hr(invalid->SetSampleTime(0), "backwards PTS");
            Hr(invalid->SetUINT64(MFSampleExtension_DecodeTimestamp, 0), "backwards DTS");
            hr = writer->WriteSample(video, invalid.Get());
            if (FAILED(hr)) break;
        } else hr = writer->WriteSample(packet.video ? video : audio, packet.sample.Get());
        if (FAILED(hr)) break;
    }
    const HRESULT finalized = writer->Finalize();
    if (FAILED(hr)) return hr;
    if (FAILED(finalized)) return finalized;
    return bytes->Flush(); // Recorder owns the final file flush, not the sink.
}
void Released(Bytes* bytes) {
    ULONG references = 0;
    for (unsigned wait = 0; wait < 100; ++wait) {
        references = bytes->AddRef(); bytes->Release();
        if (references == 2) break;
        Sleep(10); // Allow already queued Media Foundation callbacks to retire.
    }
    printf("byte stream references including temporary probe=%lu\n", references);
    Check(references == 2, "all writer/sink/clock references released; only caller retains file");
}
void TestFault(const Fixture& fixture, Fault fault, HRESULT expected, unsigned after) {
    ComPtr<Bytes> bytes; bytes.Attach(new Bytes()); bytes->fault = fault; bytes->failAfterWrites = after;
    const auto start = std::chrono::steady_clock::now();
    const HRESULT result = Exercise(fixture, bytes.Get());
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    printf("fault=%d after=%u result=%08lX writes=%u seeks=%u flushes=%u duration=%.3fs\n",
        static_cast<int>(fault), after, static_cast<unsigned long>(result), bytes->writes, bytes->seeks, bytes->flushes, elapsed);
    Check(elapsed < 5, "storage failure/finalization/destruction remains bounded");
    Released(bytes.Get());
    if (fault == Fault::SeekError && !bytes->seeks) {
        Check(SUCCEEDED(result), "sequential fragmented mux needs no seek"); return;
    }
    if (fault == Fault::ZeroWrite) Check(FAILED(result), "zero-byte successful write cannot silently truncate a recording");
    else Check(result == expected, "original storage HRESULT retained");
    Check(!bytes->closed, "writer does not close caller-owned byte stream");
}
void FailedIndexKeepsFragments(const Fixture& fixture) {
    ComPtr<Bytes> bytes; bytes.Attach(new Bytes()); bytes->fault = Fault::FinalIndexError;
    Check(Exercise(fixture, bytes.Get()) == kWriteError && bytes->finalIndexRejected,
        "final sample-index write failure is reported, never called saved");
    Released(bytes.Get());
    QWORD length = 0; Hr(bytes->GetLength(&length), "failed index file length");
    printf("failed index length=%llu writes=%u seeks=%u\n", length, bytes->writes, bytes->seeks);
    Hr(bytes->SetCurrentPosition(0), "seek failed-index prefix");
    // The fault stream intentionally has no async Read implementation. Give
    // MF's asynchronous source a real test-owned temporary byte stream.
    ComPtr<IMFByteStream> view;
    Hr(MFCreateTempFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, &view), "recovery reader file");
    std::array<BYTE, 65536> buffer{};
    for (QWORD left = length; left;) {
        ULONG read = 0, written = 0;
        Hr(bytes->Read(buffer.data(), static_cast<ULONG>((std::min)(left, QWORD(buffer.size()))), &read), "copy recovered bytes");
        Hr(view->Write(buffer.data(), read, &written), "write recovered bytes");
        Check(read && written == read, "copy failed-index prefix completely"); left -= read;
    }
    Hr(view->Flush(), "flush recovered fixture"); Hr(view->SetCurrentPosition(0), "seek recovered fixture");
    ComPtr<IMFSourceReader> reader;
    Hr(MFCreateSourceReaderFromByteStream(view.Get(), nullptr, &reader), "original fragmented prefix remains readable");
    constexpr DWORD video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFMediaType> type; Hr(reader->GetNativeMediaType(video, 0, &type), "recover encoded type");
    Hr(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "unselect recovery streams");
    Hr(reader->SetStreamSelection(video, TRUE), "select recovery video");
    Hr(reader->SetCurrentMediaType(video, nullptr, type.Get()), "read recovery packets without decoder");
    unsigned recovered = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        ComPtr<IMFSample> sample; DWORD flags = 0;
        const auto hr = reader->ReadSample(video, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) break;
        if (sample) ++recovered;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    unsigned expected = 0; for (const auto& packet : fixture.packets) if (packet.video) ++expected;
    printf("index failure recovery video packets=%u/%u\n", recovered, expected);
    Check(recovered > expected * 3 / 4 && recovered <= expected,
        "failed final index preserves the earlier video fragments");
}
void DirectLifecycle(const Fixture& fixture) {
    ComPtr<Bytes> bytes; bytes.Attach(new Bytes());
    ComPtr<IMFMediaSink> sink; Hr(CreateFragmentedMp4Sink(bytes.Get(), &sink), "direct lifecycle sink");
    ComPtr<IMFStreamSink> child; Hr(sink->AddStreamSink(0, fixture.video.Get(), &child), "retained child stream");
    ComPtr<IMFPresentationClock> clock; Hr(MFCreatePresentationClock(&clock), "lifecycle presentation clock");
    Hr(sink->SetPresentationClock(clock.Get()), "attach presentation clock");
    sink.Reset(); // No explicit Shutdown: clock must not form a strong COM cycle.
    ComPtr<IMFMediaSink> orphan;
    Check(child->GetMediaSink(&orphan) == MF_E_SHUTDOWN && !orphan,
        "retained child cannot resurrect released parent through presentation clock");
    Released(bytes.Get());
    ULONG written = 0; const BYTE marker = 42;
    Hr(bytes->Write(&marker, 1, &written), "caller stream still usable after sink destruction");
    Check(written == 1 && !bytes->closed, "caller stream remains writable");
}
void Bounds(const Fixture& fixture) {
    for (unsigned problem = 0; problem < 3; ++problem) {
        ComPtr<Bytes> bytes; bytes.Attach(new Bytes());
        ComPtr<IMFMediaSink> sink; Hr(CreateFragmentedMp4Sink(bytes.Get(), &sink), "invalid type sink");
        ComPtr<IMFMediaType> invalid; Hr(MFCreateMediaType(&invalid), "invalid media type");
        Hr(fixture.video->CopyAllItems(invalid.Get()), "invalid type copy");
        if (problem == 0) Hr(MFSetAttributeSize(invalid.Get(), MF_MT_FRAME_SIZE, 16385, 192), "oversized dimensions");
        else if (problem == 1) Hr(MFSetAttributeRatio(invalid.Get(), MF_MT_FRAME_RATE, UINT32_MAX, 1), "overflowed rate");
        else Hr(invalid->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "unencoded archive type");
        ComPtr<IMFStreamSink> stream;
        Check(FAILED(sink->AddStreamSink(0, invalid.Get(), &stream)), "invalid/unbounded media type rejected");
        sink.Reset(); Released(bytes.Get());
    }
    for (const bool oversizedHeader : {true, false}) {
        ComPtr<Bytes> bytes; bytes.Attach(new Bytes());
        ComPtr<IMFMediaSink> sink; Hr(CreateFragmentedMp4Sink(bytes.Get(), &sink), "payload bound sink");
        ComPtr<IMFMediaType> type; Hr(MFCreateMediaType(&type), "bounded type");
        Hr(fixture.video->CopyAllItems(type.Get()), "bounded type copy");
        if (oversizedHeader) {
            const std::vector<BYTE> tooLarge(1024 * 1024 + 1, 0);
            Hr(type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, tooLarge.data(), static_cast<UINT32>(tooLarge.size())), "oversized sequence header");
        }
        ComPtr<IMFStreamSink> video; Hr(sink->AddStreamSink(0, type.Get(), &video), "bounded video stream");
        ComPtr<IMFSample> sample; Hr(MFCreateSample(&sample), "oversized packet sample");
        ComPtr<IMFMediaBuffer> buffer;
        const DWORD size = oversizedHeader ? 16 : 16 * 1024 * 1024 + 1;
        Hr(MFCreateMemoryBuffer(size, &buffer), "oversized packet allocation");
        Hr(buffer->SetCurrentLength(size), "oversized packet length"); Hr(sample->AddBuffer(buffer.Get()), "oversized packet buffer");
        Hr(sample->SetSampleTime(0), "oversized packet PTS"); Hr(sample->SetSampleDuration(333333), "oversized packet duration");
        Check(FAILED(video->ProcessSample(sample.Get())), oversizedHeader ? "codec header cap enforced" : "encoded packet cap enforced");
        sink.Reset(); Released(bytes.Get());
    }
    {
        ComPtr<Bytes> bytes; bytes.Attach(new Bytes());
        ComPtr<IMFMediaSink> sink; Hr(CreateFragmentedMp4Sink(bytes.Get(), &sink), "preheader bound sink");
        ComPtr<IMFStreamSink> video, audio;
        Hr(sink->AddStreamSink(0, fixture.video.Get(), &video), "unseen video track");
        Hr(sink->AddStreamSink(1, fixture.audio.Get(), &audio), "active audio track");
        HRESULT result = S_OK;
        for (unsigned i = 0; i < 600 && SUCCEEDED(result); ++i) {
            ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> buffer;
            Hr(MFCreateSample(&sample), "bounded pending sample");
            Hr(MFCreateMemoryBuffer(16, &buffer), "bounded pending buffer");
            BYTE* data = nullptr; Hr(buffer->Lock(&data, nullptr, nullptr), "pending buffer lock");
            memset(data, 0, 16); Hr(buffer->Unlock(), "pending buffer unlock");
            Hr(buffer->SetCurrentLength(16), "pending packet size"); Hr(sample->AddBuffer(buffer.Get()), "pending packet buffer");
            Hr(sample->SetSampleTime(static_cast<LONGLONG>(i) * 213333), "pending audio time");
            Hr(sample->SetSampleDuration(213333), "pending audio duration");
            result = audio->ProcessSample(sample.Get());
        }
        Check(FAILED(result), "missing video cannot accumulate an unlimited preheader audio queue");
        sink.Reset(); Released(bytes.Get());
    }
}
void ColorNegotiation() {
    const GUID keys[]{MF_MT_YUV_MATRIX, MF_MT_VIDEO_NOMINAL_RANGE,
                      MF_MT_VIDEO_PRIMARIES, MF_MT_TRANSFER_FUNCTION};
    const UINT32 requested[]{MFVideoTransferMatrix_BT601, MFNominalRange_0_255,
                             MFVideoPrimaries_SMPTE170M, MFVideoTransFunc_709};
    const UINT32 changed[]{MFVideoTransferMatrix_BT709, MFNominalRange_16_235,
                           MFVideoPrimaries_BT709, MFVideoTransFunc_sRGB};
    const BYTE initialHeader[]{0, 0, 0, 1, 0x67, 0x42};
    const BYTE negotiatedHeader[]{0, 0, 0, 1, 0x67, 0x64, 0x12, 0x34};
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        ComPtr<Bytes> bytes; bytes.Attach(new Bytes());
        ComPtr<IMFAttributes> capturedColor; Hr(MFCreateAttributes(&capturedColor, ARRAYSIZE(keys)), "captured color attributes");
        for (unsigned i = 0; i < ARRAYSIZE(keys); ++i)
            Hr(capturedColor->SetUINT32(keys[i], requested[i]), "captured color attribute");
        ComPtr<IMFAttributes> colorSnapshot; Hr(MFCreateAttributes(&colorSnapshot, ARRAYSIZE(keys)), "captured color snapshot");
        Hr(capturedColor->CopyAllItems(colorSnapshot.Get()), "copy captured color snapshot");
        ComPtr<IMFMediaSink> sink;
        Hr(CreateFragmentedMp4Sink(bytes.Get(), &sink, nullptr, nullptr, capturedColor.Get()), "color negotiation sink");
        BOOL colorUnchanged = FALSE;
        Hr(capturedColor->Compare(colorSnapshot.Get(), MF_ATTRIBUTES_MATCH_ALL_ITEMS, &colorUnchanged), "compare captured color after creation");
        UINT32 colorCount = 0; Hr(capturedColor->GetCount(&colorCount), "captured color attribute count");
        Check(colorUnchanged && colorCount == ARRAYSIZE(keys), "sink creation never mutates caller-owned captured color attributes");
        // Changing the caller's attributes after creation must not change the
        // recorder's captured interpretation used by later negotiations.
        for (unsigned i = 0; i < ARRAYSIZE(keys); ++i)
            Hr(capturedColor->SetUINT32(keys[i], changed[i]), "caller reuses captured color attribute store");
        ComPtr<IMFMediaType> original; Hr(MFCreateMediaType(&original), "requested color type");
        Hr(original->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "requested major type");
        Hr(original->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "requested H.264 type");
        Hr(MFSetAttributeSize(original.Get(), MF_MT_FRAME_SIZE, 640, 480), "requested dimensions");
        Hr(MFSetAttributeRatio(original.Get(), MF_MT_FRAME_RATE, 30, 1), "requested frame rate");
        Hr(original->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, initialHeader, sizeof(initialHeader)), "requested codec header");
        Hr(original->SetUINT32(MF_MT_AVG_BITRATE, 4000000), "requested bitrate");
        ComPtr<IMFMediaType> originalSnapshot; Hr(MFCreateMediaType(&originalSnapshot), "requested type snapshot");
        Hr(original->CopyAllItems(originalSnapshot.Get()), "copy requested type snapshot");
        ComPtr<IMFStreamSink> stream; Hr(sink->AddStreamSink(0, original.Get(), &stream), "requested video stream");
        ComPtr<IMFMediaTypeHandler> handler; Hr(stream->GetMediaTypeHandler(&handler), "color type handler");
        ComPtr<IMFMediaType> initial; Hr(handler->GetCurrentMediaType(&initial), "initial stream type color fallback");
        for (unsigned i = 0; i < ARRAYSIZE(keys); ++i) {
            UINT32 value = 0; Hr(initial->GetUINT32(keys[i], &value), "initial captured color attribute");
            Check(value == requested[i], "first stripped stream type uses independently captured original color");
        }
        ComPtr<IMFMediaType> negotiated; Hr(MFCreateMediaType(&negotiated), "negotiated color type");
        Hr(original->CopyAllItems(negotiated.Get()), "copy negotiated base type");
        for (unsigned i = 0; i < ARRAYSIZE(keys); ++i) {
            if (scenario == 0 || (scenario == 3 && i == 0))
                Hr(negotiated->DeleteItem(keys[i]), "encoder omitted color attribute");
            else Hr(negotiated->SetUINT32(keys[i], scenario == 1 || (scenario == 3 && i == 1) ? 0 : changed[i]),
                    "encoder color attribute");
        }
        Hr(negotiated->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, negotiatedHeader, sizeof(negotiatedHeader)), "new negotiated codec header");
        Hr(negotiated->SetUINT32(MF_MT_AVG_BITRATE, 8000000), "new negotiated bitrate");
        Hr(negotiated->SetUINT32(MF_MT_MPEG2_PROFILE, 100), "new negotiated profile");
        ComPtr<IMFMediaType> negotiatedSnapshot; Hr(MFCreateMediaType(&negotiatedSnapshot), "negotiated type snapshot");
        Hr(negotiated->CopyAllItems(negotiatedSnapshot.Get()), "copy negotiated type snapshot");
        Hr(handler->SetCurrentMediaType(negotiated.Get()), "accept negotiated type without losing requested color");
        ComPtr<IMFMediaType> current; Hr(handler->GetCurrentMediaType(&current), "stored negotiated type");
        for (unsigned i = 0; i < ARRAYSIZE(keys); ++i) {
            UINT32 value = 0; Hr(current->GetUINT32(keys[i], &value), "stored explicit color attribute");
            const auto expected = scenario < 2 || (scenario == 3 && i < 2) ? requested[i] : changed[i];
            Check(value == expected, "missing/Unknown color restored and explicit negotiated color respected");
        }
        UINT32 bitrate = 0, profile = 0;
        Hr(current->GetUINT32(MF_MT_AVG_BITRATE, &bitrate), "stored negotiated bitrate");
        Hr(current->GetUINT32(MF_MT_MPEG2_PROFILE, &profile), "stored negotiated profile");
        Check(bitrate == 8000000 && profile == 100, "unrelated negotiated attributes remain authoritative");
        BYTE header[sizeof(negotiatedHeader)]{}; UINT32 headerBytes = 0;
        Hr(current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header, sizeof(header), &headerBytes), "stored negotiated codec header");
        Check(headerBytes == sizeof(negotiatedHeader) && !std::memcmp(header, negotiatedHeader, sizeof(header)),
            "negotiated codec header is preserved instead of reverting to requested bytes");
        for (const auto& pair : {std::pair{original.Get(), originalSnapshot.Get()},
                                 std::pair{negotiated.Get(), negotiatedSnapshot.Get()}}) {
            BOOL equal = FALSE; UINT32 count = 0, snapshotCount = 0;
            Hr(pair.first->Compare(pair.second, MF_ATTRIBUTES_MATCH_ALL_ITEMS, &equal), "compare caller-owned media type");
            Hr(pair.first->GetCount(&count), "caller attribute count");
            Hr(pair.second->GetCount(&snapshotCount), "caller snapshot attribute count");
            Check(equal && count == snapshotCount, "color merge never mutates requested or negotiated caller-owned type");
        }
        handler.Reset(); stream.Reset(); sink.Reset(); Released(bytes.Get());
    }
    puts("Recording mux missing/Unknown/explicit color negotiation passed without encoded fixtures.");
}
void ColorNegotiationAfterFirstPacket(const Fixture& fixture) {
    ComPtr<Bytes> bytes; bytes.Attach(new Bytes());
    ComPtr<IMFAttributes> captured; Hr(MFCreateAttributes(&captured, 2), "active mux captured color");
    Hr(captured->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601), "active mux matrix");
    Hr(captured->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255), "active mux range");
    ComPtr<IMFMediaSink> sink;
    Hr(CreateFragmentedMp4Sink(bytes.Get(), &sink, nullptr, nullptr, captured.Get()), "active mux color sink");
    ComPtr<IMFMediaType> stripped; Hr(MFCreateMediaType(&stripped), "stripped fixture type");
    Hr(fixture.video->CopyAllItems(stripped.Get()), "copy stripped fixture type");
    for (const auto& key : {MF_MT_YUV_MATRIX, MF_MT_VIDEO_NOMINAL_RANGE,
                            MF_MT_VIDEO_PRIMARIES, MF_MT_TRANSFER_FUNCTION})
        Hr(stripped->DeleteItem(key), "strip negotiated fixture color");
    ComPtr<IMFStreamSink> stream; Hr(sink->AddStreamSink(0, stripped.Get(), &stream), "active mux video stream");
    ComPtr<IMFMediaTypeHandler> handler; Hr(stream->GetMediaTypeHandler(&handler), "active mux type handler");
    ComPtr<IMFMediaType> initial; Hr(handler->GetCurrentMediaType(&initial), "normalized initial mux type");
    const auto packet = std::find_if(fixture.packets.begin(), fixture.packets.end(),
        [](const Packet& item) { return item.video; });
    Check(packet != fixture.packets.end(), "fixture supplies a real first video packet");
    Hr(stream->ProcessSample(packet->sample.Get()), "first packet creates the mux format");
    Hr(handler->SetCurrentMediaType(stripped.Get()), "identical stripped type remains accepted after first packet");
    ComPtr<IMFMediaType> unknown; Hr(MFCreateMediaType(&unknown), "active mux Unknown color type");
    Hr(stripped->CopyAllItems(unknown.Get()), "copy active mux Unknown color type");
    Hr(unknown->SetUINT32(MF_MT_YUV_MATRIX, 0), "active mux Unknown matrix");
    Hr(unknown->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, 0), "active mux Unknown range");
    Hr(handler->SetCurrentMediaType(unknown.Get()), "equivalent Unknown color type remains accepted after first packet");
    for (const auto& key : {MF_MT_AVG_BITRATE, MF_MT_MPEG2_PROFILE}) {
        ComPtr<IMFMediaType> changed; Hr(MFCreateMediaType(&changed), "changed active mux type");
        Hr(stripped->CopyAllItems(changed.Get()), "copy changed active mux type");
        UINT32 old = 0; stripped->GetUINT32(key, &old);
        Hr(changed->SetUINT32(key, old == UINT32_MAX ? old - 1 : old + 1), "change active mux encoding attribute");
        Check(FAILED(handler->SetCurrentMediaType(changed.Get())),
            "actual bitrate/profile change remains rejected after mux initialization");
    }
    ComPtr<IMFMediaType> changedCodec; Hr(MFCreateMediaType(&changedCodec), "changed active mux codec");
    Hr(stripped->CopyAllItems(changedCodec.Get()), "copy changed active mux codec");
    GUID originalCodec{}; Hr(stripped->GetGUID(MF_MT_SUBTYPE, &originalCodec), "original active mux codec subtype");
    Hr(changedCodec->SetGUID(MF_MT_SUBTYPE, originalCodec == MFVideoFormat_H264 ? MFVideoFormat_HEVC : MFVideoFormat_H264),
        "change active mux codec subtype");
    Check(FAILED(handler->SetCurrentMediaType(changedCodec.Get())), "actual codec change remains rejected after first packet");
    ComPtr<IMFMediaType> current; Hr(handler->GetCurrentMediaType(&current), "active mux final type");
    BOOL equal = FALSE; UINT32 initialCount = 0, currentCount = 0;
    Hr(initial->Compare(current.Get(), MF_ATTRIBUTES_MATCH_ALL_ITEMS, &equal), "compare active mux type after rejected changes");
    Hr(initial->GetCount(&initialCount), "initial mux attribute count");
    Hr(current->GetCount(&currentCount), "current mux attribute count");
    Check(equal && initialCount == currentCount, "reapplying equivalent types and rejecting changes leaves the active mux type intact");
    handler.Reset(); stream.Reset(); sink.Reset(); Released(bytes.Get());
    puts("Recording mux color type normalization stays idempotent after the first encoded packet.");
}
}
int wmain(int argc, wchar_t** argv) {
    Check(argc == 2, "supply HEVC encoded fixture path or --color-negotiation");
    Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM"); Hr(MFStartup(MF_VERSION), "MF");
    ColorNegotiation();
    if (std::wcscmp(argv[1], L"--color-negotiation") == 0) {
        MFShutdown(); CoUninitialize(); return 0;
    }
    const auto fixture = Load(argv[1]);
    ColorNegotiationAfterFirstPacket(fixture);
    DirectLifecycle(fixture);
    FailedIndexKeepsFragments(fixture);
    Bounds(fixture);
    {
        ComPtr<Bytes> bytes; bytes.Attach(new Bytes()); Hr(Exercise(fixture, bytes.Get()), "control passthrough");
        Check(bytes->writes > 2 && !bytes->closed, "stream ownership/control file");
        Released(bytes.Get());
    }
    {
        ComPtr<Bytes> bytes; bytes.Attach(new Bytes());
        ComPtr<IMFSinkWriter> writer;
        Hr(CreateRecordingWriter(bytes.Get(), nullptr, &writer, Codec::Hevc), "early cancel writer");
        DWORD stream = 0; Hr(writer->AddStream(fixture.video.Get(), &stream), "early cancel video");
        Hr(writer->SetInputMediaType(stream, fixture.video.Get(), nullptr), "early cancel passthrough");
        Hr(writer->BeginWriting(), "early cancel begin");
        writer.Reset(); Released(bytes.Get());
        Check(!bytes->closed, "early cancel leaves caller-owned stream open");
    }
    for (const unsigned after : {0u, 2u}) {
        TestFault(fixture, Fault::DiskFull, kDiskFull, after);
        TestFault(fixture, Fault::WriteError, kWriteError, after);
        TestFault(fixture, Fault::ZeroWrite, E_FAIL, after);
    }
    TestFault(fixture, Fault::SeekError, kSeekError, 0);
    TestFault(fixture, Fault::FlushError, kWriteError, 0);
    {
        ComPtr<Bytes> bytes; bytes.Attach(new Bytes());
        Check(FAILED(Exercise(fixture, bytes.Get(), true)), "backwards timestamp cannot silently produce a malformed recording");
        Released(bytes.Get());
    }
    MFShutdown(); CoUninitialize(); puts("Recording mux storage fault tests passed."); return 0;
}
