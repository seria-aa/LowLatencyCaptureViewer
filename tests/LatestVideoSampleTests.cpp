#include "capture/LatestVideoSample.h"
#include "capture/DirectShowGraphResources.h"

#include <cstdio>
#include <thread>

static std::atomic<unsigned> liveSamples{0};
static unsigned failures = 0;
static void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", message); ++failures; }
}

// A COM sample with explicit ownership accounting; no capture device is opened.
class Sample final : public IMediaSample {
public:
    explicit Sample(long bytes, unsigned sequence = 0)
        : bytes_(bytes), sequence_(sequence) { ++liveSamples; }
    ULONG References() const { return refs_.load(); }
    unsigned Sequence() const { return sequence_; }
    void ObserveDestruction(void (*observer)(void*), void* context) {
        destructionObserver_ = observer;
        destructionContext_ = context;
    }
    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IMediaSample) return E_NOINTERFACE;
        *object = static_cast<IMediaSample*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    STDMETHODIMP GetPointer(BYTE**) override { return E_NOTIMPL; }
    STDMETHODIMP_(long) GetSize() override { return bytes_; }
    STDMETHODIMP GetTime(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    STDMETHODIMP SetTime(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    STDMETHODIMP IsSyncPoint() override { return S_OK; }
    STDMETHODIMP SetSyncPoint(BOOL) override { return S_OK; }
    STDMETHODIMP IsPreroll() override { return S_FALSE; }
    STDMETHODIMP SetPreroll(BOOL) override { return S_OK; }
    STDMETHODIMP_(long) GetActualDataLength() override { return bytes_; }
    STDMETHODIMP SetActualDataLength(long bytes) override { bytes_ = bytes; return S_OK; }
    STDMETHODIMP GetMediaType(AM_MEDIA_TYPE**) override { return E_NOTIMPL; }
    STDMETHODIMP SetMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP IsDiscontinuity() override { return S_FALSE; }
    STDMETHODIMP SetDiscontinuity(BOOL) override { return S_OK; }
    STDMETHODIMP GetMediaTime(LONGLONG*, LONGLONG*) override { return E_NOTIMPL; }
    STDMETHODIMP SetMediaTime(LONGLONG*, LONGLONG*) override { return E_NOTIMPL; }
private:
    ~Sample() {
        --liveSamples;
        if (destructionObserver_) destructionObserver_(destructionContext_);
    }
    std::atomic<ULONG> refs_{1};
    long bytes_;
    const unsigned sequence_;
    void (*destructionObserver_)(void*) = nullptr;
    void* destructionContext_ = nullptr;
};

static unsigned logged = 0;
static void Log(const wchar_t*) { ++logged; }

struct GraphLifetimeTrace {
    llcv::capture::DirectShowGraphResources* resources = nullptr;
    unsigned stopped = 0, detached = 0, finalSampleReleased = 0;
    unsigned controlReleased = 0, grabberReleased = 0;
};

static void ObserveFinalSampleRelease(void* context) {
    auto& trace = *static_cast<GraphLifetimeTrace*>(context);
    ++trace.finalSampleReleased;
    Check(trace.stopped == 1 && trace.detached == 1 &&
              trace.resources->videoCallback == nullptr,
          "retained sample outlives graph stop and callback detachment/release");
    DWORD flags = 0;
    Check(trace.resources->frameEvent &&
              GetHandleInformation(trace.resources->frameEvent, &flags),
          "frame event remains alive until the mailbox releases its sample");
}

// IMediaControl::Run is allowed to fail after some filters have started. This
// fake delivers one sample before failure and one final in-flight sample from
// Stop, without loading DirectShow or opening capture hardware.
class PartialRunControl final : public IMediaControl {
public:
    explicit PartialRunControl(GraphLifetimeTrace& trace) : trace_(trace) {}
    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDispatch && iid != IID_IMediaControl)
            return E_NOINTERFACE;
        *object = static_cast<IMediaControl*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    STDMETHODIMP GetTypeInfoCount(UINT*) override { return E_NOTIMPL; }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*,
                        EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    STDMETHODIMP Run() override {
        auto* sample = new Sample(100);
        trace_.resources->videoCallback->SampleCB(0, sample);
        sample->Release();
        return E_FAIL;
    }
    STDMETHODIMP Pause() override { return E_NOTIMPL; }
    STDMETHODIMP Stop() override {
        Check(trace_.resources->latestVideoSample != nullptr &&
                  trace_.resources->videoCallback != nullptr,
              "mailbox and callback remain alive throughout graph Stop");
        if (trace_.resources->latestVideoSample && trace_.resources->videoCallback) {
            auto* sample = new Sample(100);
            sample->ObserveDestruction(ObserveFinalSampleRelease, &trace_);
            trace_.resources->videoCallback->SampleCB(0, sample);
            sample->Release();
        }
        ++trace_.stopped;
        return S_OK;
    }
    STDMETHODIMP GetState(LONG, OAFilterState*) override { return E_NOTIMPL; }
    STDMETHODIMP RenderFile(BSTR) override { return E_NOTIMPL; }
    STDMETHODIMP AddSourceFilter(BSTR, IDispatch**) override { return E_NOTIMPL; }
    STDMETHODIMP get_FilterCollection(IDispatch**) override { return E_NOTIMPL; }
    STDMETHODIMP get_RegFilterCollection(IDispatch**) override { return E_NOTIMPL; }
    STDMETHODIMP StopWhenReady() override { return E_NOTIMPL; }
private:
    ~PartialRunControl() { ++trace_.controlReleased; }
    std::atomic<ULONG> refs_{1};
    GraphLifetimeTrace& trace_;
};

class Grabber final : public ISampleGrabber {
public:
    explicit Grabber(GraphLifetimeTrace& trace) : trace_(trace) {}
    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(ISampleGrabber)) return E_NOINTERFACE;
        *object = static_cast<ISampleGrabber*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    STDMETHODIMP SetOneShot(BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP SetMediaType(const AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP GetConnectedMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP SetBufferSamples(BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP GetCurrentBuffer(long*, long*) override { return E_NOTIMPL; }
    STDMETHODIMP GetCurrentSample(IMediaSample**) override { return E_NOTIMPL; }
    STDMETHODIMP SetCallback(ISampleGrabberCB* callback, long) override {
        if (!callback) {
            ++trace_.detached;
            Check(trace_.stopped == 1 && trace_.resources->latestVideoSample != nullptr,
                  "callback detaches after Stop and before mailbox destruction");
        }
        if (callback) callback->AddRef();
        if (callback_) callback_->Release();
        callback_ = callback;
        return S_OK;
    }
private:
    ~Grabber() {
        Check(callback_ == nullptr, "graph teardown explicitly unregisters the callback");
        if (callback_) callback_->Release();
        ++trace_.grabberReleased;
    }
    std::atomic<ULONG> refs_{1};
    ISampleGrabberCB* callback_ = nullptr;
    GraphLifetimeTrace& trace_;
};

static void TestPartialRunTeardown(bool explicitReset) {
    using namespace llcv::capture;
    GraphLifetimeTrace trace;
    {
        DirectShowGraphResources resources;
        trace.resources = &resources;
        resources.frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(resources.frameEvent != nullptr, "create partial-Run test event");
        if (!resources.frameEvent) return;
        resources.latestVideoSample = std::make_unique<LatestVideoSample>(100, resources.frameEvent);
        resources.videoCallback = new VideoSampleGrabberCallback(resources.latestVideoSample.get());
        resources.videoGrabber = new Grabber(trace);
        resources.videoGrabber->SetCallback(resources.videoCallback, 0);
        resources.control = new PartialRunControl(trace);
        Check(FAILED(resources.control->Run()) && liveSamples == 1,
              "failed graph Run may already have delivered a retained video sample");
        if (explicitReset) {
            resources.Reset();
            Check(!resources.latestVideoSample && !resources.videoCallback && !resources.frameEvent,
                  "explicit graph Reset clears mailbox, callback and event ownership");
            resources.Reset();
        }
    }
    Check(trace.stopped == 1 && trace.detached == 1 && trace.finalSampleReleased == 1 &&
              trace.controlReleased == 1 && trace.grabberReleased == 1 && liveSamples == 0,
          "explicit Reset and destructor teardown each release all resources exactly once");
}

int main() {
    using namespace llcv::capture;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(ready != nullptr, "create test frame event");
    if (!ready) return 1;
    std::atomic<uint64_t> start{0}, captured{0}, replaced{0};
    {
        LatestVideoSample slot(100, ready, {&start, &captured, &replaced});
        int64_t arrival = -1;
        auto* shortSample = new Sample(99);
        slot.Push(shortSample);
        Check(shortSample->References() == 1, "rejected frame does not acquire a COM reference");
        shortSample->Release();
        Check(slot.TakeLatest(arrival) == nullptr && liveSamples == 0 && captured == 0,
              "reject undersized sample without retaining it");
        auto* first = new Sample(100);
        slot.Push(first);
        Check(first->References() == 2, "accepted frame acquires exactly one mailbox reference");
        first->Release();
        auto* newest = new Sample(100);
        slot.Push(newest);
        Check(newest->References() == 2, "replacement preserves the producer's own reference");
        newest->Release();
        Check(liveSamples == 1 && captured == 2 && replaced == 1,
              "mailbox retains only latest frame and releases replaced ownership");
        auto* taken = slot.TakeLatest(arrival);
        Check(taken == newest && arrival > 0 && slot.TakeLatest(arrival) == nullptr,
              "take transfers latest frame exactly once with arrival timestamp");
        Check(taken && static_cast<Sample*>(taken)->References() == 1,
              "take transfers the mailbox reference without another AddRef or Release");
        if (taken) taken->Release();
        Check(liveSamples == 0, "consumer owns and releases transferred sample");
        start = UINT64_MAX;
        auto* warmup = new Sample(100);
        slot.Push(warmup); warmup->Release();
        Check(captured == 2, "warmup samples are retained but excluded from telemetry");
    }
    Check(liveSamples == 0, "destroy releases an unconsumed final sample");
    start = 0; captured = 0; replaced = 0;
    {
        LatestVideoSample slot(0, ready, {&start, &captured, &replaced});
        auto* callback = new VideoSampleGrabberCallback(&slot, Log);
        Check(callback->QueryInterface(IID_IUnknown, nullptr) == E_POINTER,
              "callback QueryInterface rejects a null output pointer");
        void* queried = nullptr;
        Check(callback->QueryInterface(__uuidof(ISampleGrabberCB), &queried) == S_OK &&
                  queried == static_cast<ISampleGrabberCB*>(callback),
              "callback exposes the sample-grabber callback COM interface");
        if (queried) Check(static_cast<ISampleGrabberCB*>(queried)->Release() == 1,
                           "QueryInterface owns exactly one additional callback reference");
        queried = callback;
        Check(callback->QueryInterface(IID_IMediaSample, &queried) == E_NOINTERFACE && !queried,
              "unsupported callback interfaces clear the output pointer");
        Check(callback->SampleCB(0, nullptr) == E_POINTER, "callback rejects null samples");
        Check(callback->BufferCB(0, nullptr, 0) == E_NOTIMPL,
              "unused byte-buffer callback remains explicitly unsupported");
        for (int i = 0; i < 2; ++i) {
            auto* sample = new Sample(17);
            callback->SampleCB(0, sample); sample->Release();
        }
        Check(logged == 1, "surface capability probe logs only once");
        callback->Release();
    }
    Check(liveSamples == 0, "callback and slot release all retained samples");
    captured = 0; replaced = 0;
    {
        LatestVideoSample slot(100, ready, {&start, &captured, &replaced});
        std::atomic<bool> done{false};
        std::atomic<unsigned> consumedThrough{0};
        std::atomic<bool> checkpointTimedOut{false};
        constexpr unsigned count = 25000;
        std::thread producer([&] {
            for (unsigned i = 0; i < count; ++i) {
                auto* sample = new Sample(100, i + 1);
                slot.Push(sample); sample->Release();
                // Force actual consumer progress before the producer can
                // finish; a scheduler cannot turn this into a serial test.
                if (i + 1 == count / 2) {
                    const uint64_t until = GetTickCount64() + 5000;
                    while (consumedThrough.load(std::memory_order_acquire) < count / 2 &&
                           GetTickCount64() < until) std::this_thread::yield();
                    if (consumedThrough.load(std::memory_order_acquire) < count / 2) {
                        checkpointTimedOut.store(true);
                        break;
                    }
                }
            }
            done.store(true, std::memory_order_release);
            SetEvent(ready);
        });
        unsigned consumed = 0;
        unsigned lastSequence = 0;
        int64_t lastArrival = 0;
        bool consumedWhileProducing = false;
        const auto consume = [&](IMediaSample* sample, int64_t arrival) {
            const auto sequence = static_cast<Sample*>(sample)->Sequence();
            Check(sequence > lastSequence && sequence <= count,
                  "concurrent takes never duplicate or reorder sample identities");
            Check(arrival > 0 && arrival >= lastArrival,
                  "sample arrival timestamp remains valid and ordered with its identity");
            lastSequence = sequence;
            lastArrival = arrival;
            ++consumed;
            consumedWhileProducing |= !done.load(std::memory_order_acquire);
            consumedThrough.store(sequence, std::memory_order_release);
            sample->Release();
        };
        for (;;) {
            int64_t arrival = 0;
            auto* sample = slot.TakeLatest(arrival);
            if (sample) { consume(sample, arrival); continue; }
            if (done.load(std::memory_order_acquire)) break;
            WaitForSingleObject(ready, 100);
        }
        producer.join();
        // The producer may publish its last sample between an empty take and
        // the done check. Drain once after joining before accounting ownership.
        int64_t arrival = 0;
        if (auto* last = slot.TakeLatest(arrival)) consume(last, arrival);
        Check(captured == count && consumed + replaced.load() == count,
              "concurrent producer/consumer accounts for every frame exactly once");
        Check(!checkpointTimedOut && consumedWhileProducing && lastSequence == count,
              "concurrent test consumes during production and ends with the true newest frame");
    }
    Check(liveSamples == 0, "concurrent mailbox leaves no leaked COM samples");
    CloseHandle(ready);
    TestPartialRunTeardown(true);
    TestPartialRunTeardown(false);
    return failures ? 1 : 0;
}
