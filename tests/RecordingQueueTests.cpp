#include "recording/Recorder.h"
#include "recording/RecordingDeliveryPolicy.h"
#include <mfapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

// Observe application-side allocations only while the capture callback runs.
// MF storage and the driver sample are constructed before this scope; no
// worker, encoder, capture device, timer or scheduling delay is involved.
static thread_local bool observeAllocation = false;
static thread_local size_t observedAllocations = 0;
void* operator new(size_t bytes) {
    if (observeAllocation) ++observedAllocations;
    if (void* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }

using Microsoft::WRL::ComPtr;
static unsigned failures = 0;
static void Expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}
static void Check(HRESULT result) { if (FAILED(result)) throw result; }

namespace {
class Sample final : public IMediaSample {
public:
    explicit Sample(long capacity) : bytes(static_cast<size_t>(capacity)), length(capacity) {}
    std::vector<BYTE> bytes;
    long length;
    LONGLONG time = 0, duration = 166666;
    unsigned pointerQueries = 0, addRefs = 0;
    ULONG References() const { return references_; }
    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IMediaSample) return E_NOINTERFACE;
        *object = static_cast<IMediaSample*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { ++addRefs; return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --references_; if (!left) delete this; return left;
    }
    STDMETHODIMP GetPointer(BYTE** output) override {
        ++pointerQueries; if (!output) return E_POINTER;
        *output = bytes.data(); return S_OK;
    }
    STDMETHODIMP_(long) GetSize() override { return static_cast<long>(bytes.size()); }
    STDMETHODIMP GetTime(REFERENCE_TIME* start, REFERENCE_TIME* end) override {
        if (!start || !end) return E_POINTER;
        *start = time; *end = time + duration; return S_OK;
    }
    STDMETHODIMP SetTime(REFERENCE_TIME* start, REFERENCE_TIME* end) override {
        if (!start || !end) return E_POINTER;
        time = *start; duration = *end - *start; return S_OK;
    }
    STDMETHODIMP IsSyncPoint() override { return S_OK; }
    STDMETHODIMP SetSyncPoint(BOOL) override { return S_OK; }
    STDMETHODIMP IsPreroll() override { return S_FALSE; }
    STDMETHODIMP SetPreroll(BOOL) override { return S_OK; }
    STDMETHODIMP_(long) GetActualDataLength() override { return length; }
    STDMETHODIMP SetActualDataLength(long value) override { length = value; return S_OK; }
    STDMETHODIMP GetMediaType(AM_MEDIA_TYPE** type) override {
        if (!type) return E_POINTER; *type = nullptr; return S_FALSE;
    }
    STDMETHODIMP SetMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP IsDiscontinuity() override { return S_FALSE; }
    STDMETHODIMP SetDiscontinuity(BOOL) override { return S_OK; }
    STDMETHODIMP GetMediaTime(LONGLONG*, LONGLONG*) override { return E_NOTIMPL; }
    STDMETHODIMP SetMediaTime(LONGLONG*, LONGLONG*) override { return E_NOTIMPL; }
private:
    ULONG references_ = 1;
};

ComPtr<Sample> MakeSample(long capacity) {
    ComPtr<Sample> sample; sample.Attach(new Sample(capacity)); return sample;
}

BYTE Pattern(size_t index, unsigned sequence) {
    return static_cast<BYTE>((index * 37 + sequence * 71 + (index >> 4)) & 255);
}

struct ExpectedPacket {
    std::vector<BYTE> bytes;
    LONGLONG time = 0, duration = 0;
    uint64_t generation = 0;
    bool video = true;
};
}

namespace llcv::recording {
struct RecorderTestAccess {
    static void Prepare(Recorder& subject, settings::VideoPixelFormat format, bool padded) {
        subject.video_.valid = true;
        subject.video_.format = format;
        subject.video_.width = 32; subject.video_.height = 24; subject.video_.fps = 60;
        subject.video_.bytes = 1234;
        subject.video_.stride = format == settings::VideoPixelFormat::Mjpeg ? 0 :
            static_cast<UINT32>(32 * (format == settings::VideoPixelFormat::Nv12 ? 1 : 2) + (padded ? 16 : 0));
        const bool mjpeg = format == settings::VideoPixelFormat::Mjpeg;
        const auto policy = RecordingDeliveryPolicy(mjpeg);
        Expect(policy.videoBuffers == (mjpeg ? 8u : 16u), "recording pixel-buffer allocation count remains unchanged");
        Expect(policy.videoPackets == policy.videoBuffers && policy.videoPackets > 6,
            "delivery metadata exposes the already-allocated video-buffer capacity");
        Expect(policy.audioPackets == 32 && policy.audioBuffers == 64, "audio storage bounds remain unchanged");
        subject.PrepareQueues();
        Expect(subject.videoQueue_.packets.size() == policy.videoPackets &&
            subject.audioQueue_.packets.size() == policy.audioPackets,
            "regression uses the same queue allocation routine as the recording worker");
        // Start near the end so the first stalled burst also crosses ring wrap.
        subject.videoQueue_.read = subject.videoQueue_.write = policy.videoPackets * 3 - 3;
        subject.audioQueue_.read = subject.audioQueue_.write = policy.audioPackets * 2 - 2;
        subject.videoLimiter_ = {0, subject.video_.fps};
        subject.phase_ = Phase::Recording;
        subject.busy_ = true;
    }

    static ExpectedPacket Fill(Recorder& subject, Sample& sample, unsigned sequence, bool video = true) {
        ExpectedPacket expected;
        expected.video = video;
        expected.generation = subject.pauseGeneration_.load();
        expected.time = video ? static_cast<LONGLONG>(sequence) * 10000000 / 60 :
            static_cast<LONGLONG>(sequence) * 100000;
        expected.duration = video ? 166666 : 100000;
        sample.time = expected.time; sample.duration = expected.duration;
        const bool mjpeg = subject.video_.format == settings::VideoPixelFormat::Mjpeg;
        const bool nv12 = subject.video_.format == settings::VideoPixelFormat::Nv12;
        const size_t row = video ? 32u * (nv12 ? 1u : 2u) : 1920u;
        const size_t rows = video ? 24u * (nv12 ? 3u : 2u) / 2 : 1u;
        expected.bytes.resize(!video ? 1920u : mjpeg ? 1000u + sequence % 100 : row * rows);
        for (size_t i = 0; i < expected.bytes.size(); ++i) expected.bytes[i] = Pattern(i, sequence);
        std::fill(sample.bytes.begin(), sample.bytes.end(), BYTE{0xee});
        if (!video || mjpeg) {
            sample.length = static_cast<long>(expected.bytes.size());
            std::memcpy(sample.bytes.data(), expected.bytes.data(), expected.bytes.size());
        } else {
            sample.length = static_cast<long>(subject.video_.stride * rows);
            for (size_t y = 0; y < rows; ++y)
                std::memcpy(sample.bytes.data() + y * subject.video_.stride, expected.bytes.data() + y * row, row);
        }
        sample.pointerQueries = 0; sample.addRefs = 0;
        return expected;
    }

    static void Push(Recorder& subject, Sample& sample, bool video = true) {
        const auto before = sample.bytes;
        observedAllocations = 0; observeAllocation = true;
        subject.Push(video, &sample);
        observeAllocation = false;
        Expect(observedAllocations == 0, "normal capture delivery performs no C++ heap allocation");
        Expect(sample.pointerQueries == 1, "accepted capture sample is read once");
        Expect(sample.addRefs == 0 && sample.References() == 1, "recording never retains driver sample ownership");
        Expect(before == sample.bytes, "recording does not mutate capture/playback payload or row padding");
        // Reuse the driver memory immediately, before the consumer is allowed
        // to run. A zero-copy alias to the driver would fail payload checks.
        std::fill(sample.bytes.begin(), sample.bytes.end(), BYTE{0xa5});
    }

    static Recorder::Packet Pop(Recorder& subject, bool video = true) {
        auto& queue = video ? subject.videoQueue_ : subject.audioQueue_;
        const size_t read = queue.read.load();
        Expect(read != queue.write.load(), "consumer has a published packet");
        Recorder::Packet packet = std::move(queue.packets[read % queue.packets.size()]);
        queue.read.store(read + 1, std::memory_order_release);
        return packet;
    }

    static void Verify(const Recorder::Packet& packet, const ExpectedPacket& expected) {
        Expect(packet.video == expected.video && packet.time == expected.time &&
            packet.duration == expected.duration && packet.pauseGeneration == expected.generation,
            "stalled/wrapped delivery preserves stream, sequence timestamp, duration and pause epoch");
        Expect(bool(packet.buffer), "published packet owns a recorder buffer");
        if (!packet.buffer) return;
        BYTE* bytes = nullptr; DWORD length = 0;
        Check(packet.buffer->Lock(&bytes, nullptr, &length));
        Expect(length == expected.bytes.size() &&
            std::memcmp(bytes, expected.bytes.data(), (std::min)(size_t(length), expected.bytes.size())) == 0,
            "every packed payload byte survives driver reuse, queue wrap and downstream retention");
        Check(packet.buffer->Unlock());
    }

    static void BurstAndPause(settings::VideoPixelFormat format, bool padded) {
        Recorder subject; Prepare(subject, format, padded);
        auto sample = MakeSample(4096);
        const auto count = subject.videoQueue_.packets.size();
        const auto* storage = subject.videoQueue_.packets.data();
        unsigned sequence = 0;
        for (unsigned cycle = 0; cycle < 4; ++cycle) {
            std::vector<ExpectedPacket> expected;
            expected.reserve(count);
            // Holding the unrelated lifecycle mutex plus withholding every
            // consumer proves admission neither waits for it nor a worker.
            {
                std::lock_guard lock(subject.lifecycle_);
                for (size_t i = 0; i < count; ++i) {
                    expected.push_back(Fill(subject, *sample.Get(), sequence++));
                    Push(subject, *sample.Get());
                    Expect(subject.Recording() && SUCCEEDED(subject.Result()),
                        "burst beyond the old six-slot queue is accepted without a consumer");
                }
            }
            Expect(subject.videoQueue_.packets.data() == storage && subject.videoQueue_.packets.size() == count,
                "capture never grows or relocates the bounded delivery queue");
            for (const auto& packet : expected) Verify(Pop(subject), packet);
            Expect(subject.videoQueue_.read == subject.videoQueue_.write, "draining preserves every accepted frame exactly once");
            Expect(subject.SetPaused(true), "pause accepted after drain");
            Fill(subject, *sample.Get(), sequence++);
            const auto write = subject.videoQueue_.write.load();
            observedAllocations = 0; observeAllocation = true;
            subject.Push(true, sample.Get());
            observeAllocation = false;
            Expect(subject.videoQueue_.write == write && sample->pointerQueries == 0 && observedAllocations == 0,
                "paused input does not copy, allocate or occupy delivery storage");
            Expect(subject.SetPaused(false), "resume accepted after stalled delivery");
        }
        std::vector<ExpectedPacket> audio;
        for (unsigned i = 0; i < 20; ++i) {
            audio.push_back(Fill(subject, *sample.Get(), i, false));
            Push(subject, *sample.Get(), false);
        }
        for (const auto& packet : audio) Verify(Pop(subject, false), packet);
        Expect(SUCCEEDED(subject.Result()), "video backlog handling leaves audio delivery intact");
        subject.Stop();
        Fill(subject, *sample.Get(), sequence);
        const auto write = subject.videoQueue_.write.load();
        subject.Push(true, sample.Get());
        Expect(subject.videoQueue_.write == write && sample->pointerQueries == 0,
            "stopped recording leaves subsequent playback samples alone");
    }

    static void BoundedOverload(settings::VideoPixelFormat format) {
        Recorder subject; Prepare(subject, format, false);
        auto sample = MakeSample(4096);
        std::vector<ExpectedPacket> expected;
        const auto count = subject.videoQueue_.packets.size();
        for (size_t i = 0; i < count; ++i) {
            expected.push_back(Fill(subject, *sample.Get(), static_cast<unsigned>(i)));
            Push(subject, *sample.Get());
        }
        Fill(subject, *sample.Get(), static_cast<unsigned>(count));
        const auto write = subject.videoQueue_.write.load();
        subject.Push(true, sample.Get());
        Expect(subject.Result() == HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW) && !subject.Recording() &&
            subject.failure_ == Failure::EncoderOverload, "hard capacity closes recording admission with an explicit queue failure");
        Expect(subject.videoQueue_.write == write, "overload does not publish a partial frame or silently replace queued frames");
        for (const auto& packet : expected) Verify(Pop(subject), packet);
    }

    static void RetainedLease(settings::VideoPixelFormat format) {
        Recorder subject; Prepare(subject, format, false);
        auto sample = MakeSample(4096);
        const auto count = subject.videoQueue_.packets.size();
        auto first = Fill(subject, *sample.Get(), 0); Push(subject, *sample.Get());
        auto inFlight = Pop(subject); // Worker reclaimed metadata before WriteSample returns.
        std::vector<ExpectedPacket> expected;
        for (size_t i = 1; i < count; ++i) {
            expected.push_back(Fill(subject, *sample.Get(), static_cast<unsigned>(i)));
            Push(subject, *sample.Get());
        }
        Verify(inFlight, first);
        Expect(subject.videoQueue_.write - subject.videoQueue_.read == count - 1,
            "worker-held packet is no longer counted as pending metadata");
        // Releasing the asynchronous encoder-like lease permits exactly one
        // further frame without creating a new pixel buffer.
        inFlight.buffer.Reset();
        expected.push_back(Fill(subject, *sample.Get(), static_cast<unsigned>(count)));
        Push(subject, *sample.Get());
        Expect(SUCCEEDED(subject.Result()), "returned downstream lease is reusable during backlog");
        for (const auto& packet : expected) Verify(Pop(subject), packet);

        Recorder exhausted; Prepare(exhausted, format, false);
        auto retainedExpected = Fill(exhausted, *sample.Get(), 0); Push(exhausted, *sample.Get());
        auto retained = Pop(exhausted);
        for (size_t i = 1; i < count; ++i) {
            Fill(exhausted, *sample.Get(), static_cast<unsigned>(i)); Push(exhausted, *sample.Get());
        }
        Fill(exhausted, *sample.Get(), static_cast<unsigned>(count));
        const auto write = exhausted.videoQueue_.write.load();
        exhausted.Push(true, sample.Get());
        Expect(exhausted.Result() == HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW) &&
            exhausted.failure_ == Failure::EncoderOverload && exhausted.videoQueue_.write == write,
            "retained leases remain a hard bound even with spare delivery metadata");
        Verify(retained, retainedExpected);
    }

    static void Run() {
        using Format = settings::VideoPixelFormat;
        for (const auto format : {Format::Nv12, Format::Yuy2, Format::Mjpeg}) {
            BurstAndPause(format, false);
            if (format != Format::Mjpeg) BurstAndPause(format, true);
            BoundedOverload(format);
            RetainedLease(format);
        }
        std::puts("NV12/YUY2/MJPEG deterministic bursts, wrap, ownership, pause, PCM and hard overload bounds checked");
    }
};
}

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    const HRESULT mf = MFStartup(MF_VERSION);
    if (FAILED(mf)) { CoUninitialize(); return 1; }
    try { llcv::recording::RecorderTestAccess::Run(); }
    catch (HRESULT result) { ++failures; std::printf("HRESULT: %08lX\n", static_cast<unsigned long>(result)); }
    catch (...) { ++failures; std::puts("unexpected exception"); }
    MFShutdown(); CoUninitialize();
    return failures ? 1 : 0;
}
