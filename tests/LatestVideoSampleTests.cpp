#include "capture/LatestVideoSample.h"
#include "capture/DirectShowGraphResources.h"
#include "capture/VideoOnlyStart.h"
#include "video/PresentationReadiness.h"
#include "video/PresentationPolicy.h"

#include <cstdio>
#include <thread>
#include <dvdmedia.h>
#include <mfapi.h>
#include <chrono>
#include <algorithm>
#include <array>

static std::atomic<unsigned> liveSamples{0};
static unsigned failures = 0;
static void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", message); ++failures; }
}

#include "CaptureGraphEventsTests.inl"

// A COM sample with explicit ownership accounting; no capture device is opened.
class Sample final : public IMediaSample {
public:
    explicit Sample(long bytes, unsigned sequence = 0)
        : bytes_(bytes), capacity_(bytes), sequence_(sequence) { ++liveSamples; }
    ULONG References() const { return refs_.load(); }
    unsigned Sequence() const { return sequence_; }
    const AM_MEDIA_TYPE* dynamicType = nullptr;
    HRESULT typeResult = S_FALSE;
    unsigned typeQueries = 0;
    HANDLE sizeEntered = nullptr, sizeResume = nullptr;
    void GateSurfaceProbe(HANDLE entered, HANDLE resume) {
        probeEntered_ = entered; probeResume_ = resume;
    }
    void ObserveDestruction(void (*observer)(void*), void* context) {
        destructionObserver_ = observer;
        destructionContext_ = context;
    }
    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IMediaSample2Config) && probeEntered_) {
            SetEvent(probeEntered_);
            if (WaitForSingleObject(probeResume_, 10000) != WAIT_OBJECT_0)
                return E_FAIL;
        }
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
    STDMETHODIMP_(long) GetSize() override {
        if (sizeEntered) {
            SetEvent(sizeEntered);
            WaitForSingleObject(sizeResume, 5000);
        }
        return capacity_;
    }
    STDMETHODIMP GetTime(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    STDMETHODIMP SetTime(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    STDMETHODIMP IsSyncPoint() override { return S_OK; }
    STDMETHODIMP SetSyncPoint(BOOL) override { return S_OK; }
    STDMETHODIMP IsPreroll() override { return S_FALSE; }
    STDMETHODIMP SetPreroll(BOOL) override { return S_OK; }
    STDMETHODIMP_(long) GetActualDataLength() override { return bytes_; }
    STDMETHODIMP SetActualDataLength(long bytes) override { bytes_ = bytes; return S_OK; }
    STDMETHODIMP GetMediaType(AM_MEDIA_TYPE** type) override {
        ++typeQueries;
        if (!type) return E_POINTER;
        *type = nullptr;
        if (dynamicType) {
            auto* copy = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
            if (!copy) return E_OUTOFMEMORY;
            *copy = *dynamicType;
            copy->pbFormat = nullptr;
            if (dynamicType->pbFormat && dynamicType->cbFormat) {
                copy->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(dynamicType->cbFormat));
                if (!copy->pbFormat) { CoTaskMemFree(copy); return E_OUTOFMEMORY; }
                memcpy(copy->pbFormat, dynamicType->pbFormat, dynamicType->cbFormat);
            }
            if (copy->pUnk) copy->pUnk->AddRef();
            *type = copy;
        }
        return typeResult;
    }
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
    const long capacity_;
    const unsigned sequence_;
    HANDLE probeEntered_ = nullptr, probeResume_ = nullptr;
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

static void TestDelayedFirstCallback() {
    using namespace llcv::capture;
    // A first frame delayed by the producer is still accepted. The callback
    // no longer queries optional interfaces or logs before publishing it.
    for (int iteration = 0; iteration < 100; ++iteration) {
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE entered = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE resume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(ready && entered && resume, "create callback replay gates");
        if (!ready || !entered || !resume) {
            if (ready) CloseHandle(ready);
            if (entered) CloseHandle(entered);
            if (resume) CloseHandle(resume);
            return;
        }
        {
            LatestVideoSample slot(100, ready);
            auto* callback = new VideoSampleGrabberCallback(&slot, Log);
            auto* sample = new Sample(100);
            const unsigned logsBefore = logged;
            std::thread producer([&] {
                SetEvent(entered);
                WaitForSingleObject(resume, 10000);
                callback->SampleCB(0, sample); sample->Release();
            });
            Check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0,
                  "producer is deliberately paused before first sample delivery");
            // One wall-clock run exceeds the app's current 3-second deadline;
            // other runs use gates, without real sleeps or timing assumptions.
            Check(WaitForSingleObject(ready, iteration == 0 ? 3100 : 0) == WAIT_TIMEOUT,
                  "consumer waits until delayed producer delivers input");
            int64_t arrival = 0;
            Check(slot.TakeLatest(arrival) == nullptr,
                  "no frame is published before producer delivery");
            SetEvent(resume);
            producer.join();
            Check(WaitForSingleObject(ready, 1000) == WAIT_OBJECT_0,
                  "releasing producer pause publishes the frame");
            auto* frame = slot.TakeLatest(arrival);
            Check(frame != nullptr && logged == logsBefore,
                  "callback publishes input without synchronous log I/O");
            if (frame) frame->Release();
            for (long bytes : {0L, 99L, 100L}) {
                auto* next = new Sample(bytes);
                callback->SampleCB(0, next); next->Release();
                const auto wait = WaitForSingleObject(ready, 0);
                frame = slot.TakeLatest(arrival);
                Check((bytes == 100 ? wait == WAIT_OBJECT_0 && frame != nullptr
                                    : wait == WAIT_TIMEOUT && frame == nullptr),
                      "empty/short callbacks do not wake consumer; valid recovery does");
                if (frame) frame->Release();
            }
            Check(logged == logsBefore && slot.RejectedSamples() == 2,
                  "rejected inputs counted without logging on the callback thread");
            ResetEvent(entered);
            auto* guarded = new Sample(100);
            guarded->GateSurfaceProbe(entered, resume);
            callback->SampleCB(0, guarded); guarded->Release();
            Check(WaitForSingleObject(entered, 0) == WAIT_TIMEOUT,
                  "optional surface interface is never queried on input callback");
            frame = slot.TakeLatest(arrival);
            Check(frame != nullptr, "frame publishes without optional driver-interface probe");
            if (frame) frame->Release();
            callback->Release();
        }
        CloseHandle(ready); CloseHandle(entered); CloseHandle(resume);
        Check(liveSamples == 0, "delayed callback replay leaves no retained samples");
    }
    std::puts("Callback replay: 100 delayed-input recoveries; no optional queries or callback log I/O.");
}

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

#include "VideoMailboxStress.inl"
#include "VideoSampleFormatTests.inl"

static bool HandleIsClosed(HANDLE handle) {
    DWORD flags = 0;
    __try {
        return !GetHandleInformation(handle, &flags) && GetLastError() == ERROR_INVALID_HANDLE;
    } __except (GetExceptionCode() == 0xC0000008UL /* STATUS_INVALID_HANDLE */
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        // Checked-handle environments can raise instead of returning FALSE.
        // Accept only the expected closed-handle result, not an access violation.
        return true;
    }
}
static void TestPresentationAdmission() {
    using Reason = llcv::capture::CaptureGraphEvents::WaitReason;
    using Mode = llcv::settings::PresentationMode;
    Check(llcv::presentation::UsesPresentationReadiness(Mode::VSync) &&
              !llcv::presentation::UsesPresentationReadiness(Mode::AllowTearing) &&
              !llcv::presentation::UsesPresentationReadiness(Mode::Compatibility) &&
              !llcv::presentation::UsesPresentationReadiness(Mode::ExclusiveFullscreen),
          "pacing changes only Flip VSync");
    HANDLE frame = CreateEventW(nullptr, FALSE, FALSE, nullptr),
           wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE slot = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto* source = new TestGraphEvents;
    llcv::capture::CaptureGraphEvents events;
    events.Initialize(static_cast<IMediaEvent*>(source));
    llcv::presentation::Readiness admission;
    admission.Reset(slot);
    {
        llcv::capture::LatestVideoSample latest(128, frame);
        auto* first = new Sample(128, 1);
        latest.Push(first);
        first->Release();
        Check(events.Wait(frame, 0, wake, admission.WaitHandle()) == S_OK &&
                  events.LastWaitReason() == Reason::Frame,
              "capture notification alone is not presentation admission");
        Check(!admission.Ready(), "input is left in mailbox while DXGI is busy");
        for (unsigned id = 2; id <= 32; ++id) {
            auto* sample = new Sample(128, id);
            latest.Push(sample);
            sample->Release();
        }
        Check(events.Wait(nullptr, 0, wake, admission.WaitHandle()) == S_FALSE,
              "repeated capture notifications cannot spin or bypass presentation wait");
        SetEvent(wake);
        SetEvent(slot);
        Check(events.Wait(nullptr, 0, wake, slot) == S_OK &&
                  events.LastWaitReason() == Reason::Wake,
              "lifecycle wake has priority and leaves presentation token untouched");
        ResetEvent(wake);
        source->Push(EC_ERRORABORT, E_ACCESSDENIED);
        Check(events.Wait(nullptr, 0, wake, slot) == E_ACCESSDENIED,
              "graph failure wins while waiting for presentation");
        Check(events.Wait(nullptr, 0, wake, slot) == S_OK &&
                  events.LastWaitReason() == Reason::PresentationReady,
              "ready token wakes the graph-aware wait");
        admission.Signaled();
        int64_t arrived = 0;
        auto* selected = latest.TakeLatest(arrived);
        Check(selected && static_cast<Sample*>(selected)->Sequence() == 32,
              "sample selected AFTER readiness is newest sequence 32, not stale sequence 1");
        if (selected)
            selected->Release();
        Check(admission.Ready() && !admission.WaitHandle() && admission.Poll() == S_OK,
              "unused credit survives decoder/no-picture path");
        admission.Presented();
        Check(!admission.Ready() && admission.Poll() == S_FALSE,
              "one real Present consumes one credit");
        SetEvent(slot);
        Check(admission.Poll() == S_OK, "readmission consumes a fresh ready token");
        admission.Bypass();
        DWORD flags = 0;
        Check(
            !admission.Enabled() && admission.Ready() && !admission.WaitHandle() &&
                GetHandleInformation(slot, &flags),
            "broken readiness signal can fall back without closing a handle still owned by the chain");
    }
    events.Reset();
    source->Release();
    admission.Reset();
    Check(HandleIsClosed(slot), "reset closes owned DXGI readiness handle");
    Check(admission.Ready() && !admission.WaitHandle(), "disabled modes have no wait or handle");
    Check(liveSamples == 0, "admission has no leaked sample ownership");
    CloseHandle(frame);
    CloseHandle(wake);
}
// A callback that only shows who still holds it.
class CountingAudioCallback final : public ISampleGrabberCB {
public:
    ULONG References() const { return refs_.load(); }
    STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(ISampleGrabberCB)) return E_NOINTERFACE;
        *object = this; AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }
    STDMETHODIMP SampleCB(double, IMediaSample*) override { return S_OK; }
    STDMETHODIMP BufferCB(double, BYTE*, long) override { return S_OK; }
private:
    std::atomic<ULONG> refs_{1};
};

// Only RemoveFilter matters to RemoveAudioBranch; everything else is unused.
class RemovalGraph final : public IGraphBuilder {
public:
    HRESULT removeResult = S_OK;
    unsigned removeCalls = 0;
    STDMETHODIMP QueryInterface(REFIID, void** object) override { if (object) *object = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { return --refs_; }
    STDMETHODIMP AddFilter(IBaseFilter*, LPCWSTR) override { return E_NOTIMPL; }
    STDMETHODIMP RemoveFilter(IBaseFilter*) override { ++removeCalls; return removeResult; }
    STDMETHODIMP EnumFilters(IEnumFilters**) override { return E_NOTIMPL; }
    STDMETHODIMP FindFilterByName(LPCWSTR, IBaseFilter**) override { return E_NOTIMPL; }
    STDMETHODIMP ConnectDirect(IPin*, IPin*, const AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP Reconnect(IPin*) override { return E_NOTIMPL; }
    STDMETHODIMP Disconnect(IPin*) override { return E_NOTIMPL; }
    STDMETHODIMP SetDefaultSyncSource() override { return E_NOTIMPL; }
    STDMETHODIMP Connect(IPin*, IPin*) override { return E_NOTIMPL; }
    STDMETHODIMP Render(IPin*) override { return E_NOTIMPL; }
    STDMETHODIMP RenderFile(LPCWSTR, LPCWSTR) override { return E_NOTIMPL; }
    STDMETHODIMP AddSourceFilter(LPCWSTR, LPCWSTR, IBaseFilter**) override { return E_NOTIMPL; }
    STDMETHODIMP SetLogFile(DWORD_PTR) override { return E_NOTIMPL; }
    STDMETHODIMP Abort() override { return E_NOTIMPL; }
    STDMETHODIMP ShouldOperationContinue() override { return E_NOTIMPL; }
    ULONG References() const { return refs_.load(); }
private:
    std::atomic<ULONG> refs_{1};
};

class RemovalFilter final : public IBaseFilter {
public:
    RemovalFilter(IFilterGraph* graph, HRESULT queryResult, unsigned& released)
        : graph_(graph), queryResult_(queryResult), released_(released) {}
    STDMETHODIMP QueryInterface(REFIID, void** object) override { if (object) *object = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --refs_;
        if (!left) { ++released_; delete this; }
        return left;
    }
    STDMETHODIMP GetClassID(CLSID*) override { return E_NOTIMPL; }
    STDMETHODIMP Stop() override { return E_NOTIMPL; }
    STDMETHODIMP Pause() override { return E_NOTIMPL; }
    STDMETHODIMP Run(REFERENCE_TIME) override { return E_NOTIMPL; }
    STDMETHODIMP GetState(DWORD, FILTER_STATE*) override { return E_NOTIMPL; }
    STDMETHODIMP SetSyncSource(IReferenceClock*) override { return E_NOTIMPL; }
    STDMETHODIMP GetSyncSource(IReferenceClock**) override { return E_NOTIMPL; }
    STDMETHODIMP EnumPins(IEnumPins**) override { return E_NOTIMPL; }
    STDMETHODIMP FindPin(LPCWSTR, IPin**) override { return E_NOTIMPL; }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override {
        if (FAILED(queryResult_)) return queryResult_;
        *info = {};
        info->pGraph = graph_;
        if (graph_) graph_->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph*, LPCWSTR) override { return E_NOTIMPL; }
    STDMETHODIMP QueryVendorInfo(LPWSTR*) override { return E_NOTIMPL; }
private:
    IFilterGraph* graph_;
    HRESULT queryResult_;
    unsigned& released_;
    std::atomic<ULONG> refs_{1};
};

static bool PinConnected(IPin* pin) {
    IPin* peer = nullptr;
    const bool connected = pin && SUCCEEDED(pin->ConnectedTo(&peer)) && peer;
    if (peer) peer->Release();
    return connected;
}

// Real graph, stock filters only, with the audio branch actually connected
// (WAV file -> WAVE parser -> sample grabber -> null renderer): a failed
// audio branch must leave exactly the video branch behind, whether the audio
// pin was on the capture filter itself or on a separate audio filter.
static void TestRemoveConnectedAudioBranch() {
    using namespace llcv::capture;
    static constexpr CLSID kWaveParser =
        {0xD51BD5A1, 0x7548, 0x11CF, {0xA5, 0x20, 0x00, 0x80, 0xC7, 0x7E, 0xF5, 0x8A}};
    wchar_t folder[MAX_PATH]{}, file[MAX_PATH]{};
    GetTempPathW(MAX_PATH, folder);
    GetTempFileNameW(folder, L"llc", 0, file);
    {
        // 48 kHz 16-bit stereo PCM, 0.1 s of silence.
        constexpr DWORD dataBytes = 19200;
        std::vector<BYTE> wave(44 + dataBytes);
        const auto put = [&](size_t at, DWORD value, int bytes) {
            for (int i = 0; i < bytes; ++i) wave[at + i] = static_cast<BYTE>(value >> (8 * i));
        };
        std::memcpy(wave.data(), "RIFF", 4); put(4, 36 + dataBytes, 4);
        std::memcpy(wave.data() + 8, "WAVEfmt ", 8); put(16, 16, 4);
        put(20, 1, 2); put(22, 2, 2); put(24, 48000, 4); put(28, 192000, 4); put(32, 4, 2); put(34, 16, 2);
        std::memcpy(wave.data() + 36, "data", 4); put(40, dataBytes, 4);
        const HANDLE out = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        if (out != INVALID_HANDLE_VALUE) {
            WriteFile(out, wave.data(), static_cast<DWORD>(wave.size()), &written, nullptr);
            CloseHandle(out);
        }
    }
    const auto filterCount = [](IGraphBuilder* graph) {
        IEnumFilters* filters = nullptr;
        if (FAILED(graph->EnumFilters(&filters))) return -1;
        int count = 0;
        IBaseFilter* filter = nullptr;
        while (filters->Next(1, &filter, nullptr) == S_OK) { ++count; filter->Release(); }
        filters->Release();
        return count;
    };
    const auto create = [](const CLSID& id) {
        IBaseFilter* filter = nullptr;
        CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&filter));
        return filter;
    };
    const auto firstPin = [](IBaseFilter* filter, PIN_DIRECTION wanted) {
        IEnumPins* pins = nullptr;
        IPin* found = nullptr;
        if (!filter || FAILED(filter->EnumPins(&pins))) return found;
        IPin* pin = nullptr;
        while (!found && pins->Next(1, &pin, nullptr) == S_OK) {
            PIN_DIRECTION direction{};
            if (SUCCEEDED(pin->QueryDirection(&direction)) && direction == wanted) found = pin;
            else pin->Release();
        }
        pins->Release();
        return found;
    };
    for (bool separateAudioFilter : {false, true}) {
        DirectShowGraphResources resources;
        Check(SUCCEEDED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&resources.graph))), "create audio-branch test graph");
        if (!resources.graph) continue;
        // The file reader only feeds the parser and stays in the graph. The
        // parser carries the audio pin: it is either the capture filter
        // itself or a separate audio capture filter.
        IBaseFilter* reader = create(CLSID_AsyncReader);
        IBaseFilter* source = create(kWaveParser);
        IFileSourceFilter* loader = nullptr;
        const bool loaded = reader && source && SUCCEEDED(reader->QueryInterface(IID_PPV_ARGS(&loader))) &&
            SUCCEEDED(loader->Load(file, nullptr));
        if (loader) loader->Release();
        Check(loaded, "file source for the audio branch loads");
        if (!loaded) { if (reader) reader->Release(); if (source) source->Release(); continue; }
        if (separateAudioFilter) {
            resources.capture = create(CLSID_SmartTee);
            resources.audioCapture = source;
        } else {
            resources.capture = source;
        }
        resources.videoNullRenderer = create(kNullRendererClassId);
        resources.audioGrabberFilter = create(kSampleGrabberClassId);
        resources.audioNullRenderer = create(kNullRendererClassId);
        if (!resources.capture || !resources.videoNullRenderer || !resources.audioGrabberFilter ||
            !resources.audioNullRenderer) { reader->Release(); Check(false, "stock test filters are available"); continue; }
        resources.graph->AddFilter(reader, L"file reader");
        resources.graph->AddFilter(resources.capture, L"capture");
        resources.graph->AddFilter(resources.videoNullRenderer, L"video sink");
        if (separateAudioFilter) resources.graph->AddFilter(resources.audioCapture, L"audio capture");
        {
            IPin* readerOut = firstPin(reader, PINDIR_OUTPUT);
            IPin* parserIn = firstPin(source, PINDIR_INPUT);
            Check(readerOut && parserIn && SUCCEEDED(resources.graph->ConnectDirect(readerOut, parserIn, nullptr)),
                  "WAVE parser reads the test file");
            if (readerOut) readerOut->Release();
            if (parserIn) parserIn->Release();
        }
        reader->Release();
        resources.graph->AddFilter(resources.audioGrabberFilter, L"audio grabber");
        resources.graph->AddFilter(resources.audioNullRenderer, L"audio sink");
        resources.audioGrabberFilter->QueryInterface(__uuidof(ISampleGrabber),
            reinterpret_cast<void**>(&resources.audioGrabber));
        auto* callback = new CountingAudioCallback();
        resources.audioCallback = callback;
        callback->AddRef();  // the test's own reference, to observe the others
        Check(resources.audioGrabber && SUCCEEDED(resources.audioGrabber->SetCallback(callback, 0)) &&
                  callback->References() == 3, "grabber and resources both hold the audio callback");
        resources.audioPin = firstPin(source, PINDIR_OUTPUT);
        resources.audioGrabberInput = firstPin(resources.audioGrabberFilter, PINDIR_INPUT);
        resources.audioGrabberOutput = firstPin(resources.audioGrabberFilter, PINDIR_OUTPUT);
        resources.audioNullInput = firstPin(resources.audioNullRenderer, PINDIR_INPUT);
        const bool connected = resources.audioPin && resources.audioGrabberInput &&
            resources.audioGrabberOutput && resources.audioNullInput &&
            SUCCEEDED(resources.graph->ConnectDirect(resources.audioPin, resources.audioGrabberInput, nullptr)) &&
            SUCCEEDED(resources.graph->ConnectDirect(resources.audioGrabberOutput, resources.audioNullInput, nullptr));
        Check(connected && PinConnected(resources.audioPin) && PinConnected(resources.audioNullInput),
              "audio branch is connected end to end");
        IPin* sourcePin = resources.audioPin;
        if (sourcePin) sourcePin->AddRef();
        Check(filterCount(resources.graph) == (separateAudioFilter ? 6 : 5), "audio branch test graph is built");
        Check(SUCCEEDED(resources.RemoveAudioBranch()), "connected audio branch removal succeeds");
        Check(filterCount(resources.graph) == 3 && resources.capture && resources.videoNullRenderer,
              "only the reader, video capture filter and video sink stay in the graph");
        Check(!PinConnected(sourcePin), "the audio source pin is disconnected");
        Check(callback->References() == 1, "grabber and resources released the audio callback");
        Check(!resources.audioPin && !resources.audioCapture && !resources.audioGrabberFilter &&
                  !resources.audioNullRenderer && !resources.audioGrabber && !resources.audioCallback &&
                  !resources.audioGrabberInput && !resources.audioGrabberOutput && !resources.audioNullInput &&
                  !resources.selectedAudioType,
              "every audio object is released after removal");
        Check(SUCCEEDED(resources.RemoveAudioBranch()) && filterCount(resources.graph) == 3,
              "removing an absent audio branch changes nothing");
        if (sourcePin) sourcePin->Release();
        callback->Release();
    }
    DeleteFileW(file);
}

// A filter that cannot be removed, or cannot even say whether it is in the
// graph, must fail the removal; the rest is still detached and released.
static void TestRemoveAudioBranchFailures() {
    using namespace llcv::capture;
    for (int scenario = 0; scenario < 3; ++scenario) {
        RemovalGraph graph;
        unsigned released = 0;
        {
            DirectShowGraphResources resources;
            resources.graph = &graph; graph.AddRef();
            // 0: RemoveFilter fails. 1: QueryFilterInfo fails. 2: a filter
            // that never joined the graph is released without RemoveFilter.
            graph.removeResult = scenario == 0 ? E_ACCESSDENIED : S_OK;
            resources.audioNullRenderer = new RemovalFilter(scenario == 2 ? nullptr : &graph, S_OK, released);
            resources.audioGrabberFilter = new RemovalFilter(&graph, scenario == 1 ? E_UNEXPECTED : S_OK, released);
            resources.audioCapture = new RemovalFilter(&graph, S_OK, released);
            const HRESULT result = resources.RemoveAudioBranch();
            if (scenario == 0)
                Check(result == E_ACCESSDENIED && graph.removeCalls == 3,
                      "a RemoveFilter failure is reported after every filter was still attempted");
            else if (scenario == 1)
                Check(result == E_UNEXPECTED && graph.removeCalls == 2,
                      "a filter that cannot report its graph is a removal failure, not a success");
            else
                Check(result == S_OK && graph.removeCalls == 2,
                      "a filter outside the graph is released without a RemoveFilter call");
            Check(released == 3 && !resources.audioNullRenderer && !resources.audioGrabberFilter &&
                      !resources.audioCapture, "failed removal still releases every audio filter");
        }
        Check(graph.References() == 1, "graph references are balanced after removal");
    }
}

static void TestRemoveAudioBranch() {
    using namespace llcv::capture;
    static_assert(OnCaptureAudioFailure(false, false) == AudioFailureAction::AskVideoOnly);
    static_assert(OnCaptureAudioFailure(true, false) == AudioFailureAction::ReportFailure);
    static_assert(OnCaptureAudioFailure(false, true) == AudioFailureAction::ReportFailure);
    constexpr auto yes = AfterVideoOnlyPrompt(true), no = AfterVideoOnlyPrompt(false);
    static_assert(yes.startVideoOnly && yes.closeAudioOutput && !yes.returnToSettingsQuietly);
    static_assert(!no.startVideoOnly && !no.closeAudioOutput && no.returnToSettingsQuietly);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    TestRemoveConnectedAudioBranch();
    TestRemoveAudioBranchFailures();
    if (SUCCEEDED(com)) CoUninitialize();
}

int main(int argc, char** argv) {
    TestPresentationAdmission();
    if (argc == 2 && std::string(argv[1]) == "--stress") return RunMailboxStress();
    if (argc == 2 && std::string(argv[1]) == "--format") { TestVideoSampleChanges(); return failures ? 1 : 0; }
    if (argc == 2 && std::string(argv[1]) == "--benchmark") return BenchmarkSampleGuard();
    TestVideoSampleChanges();
    TestRgb32CallbackOrientationAndShortBuffer();
    TestRgb24CallbackOrientationAndShortBuffer();
    TestRemoveAudioBranch();
    using namespace llcv::capture;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(ready != nullptr, "create test frame event");
    if (!ready) return 1;
    std::atomic<uint64_t> start{0}, captured{0}, replaced{0};
    {
        LatestVideoSample slot(100, ready, {&start, &captured, &replaced});
        int64_t arrival = -1;
        for (long invalidLength : {0L, -1L, 101L, LONG_MAX}) {
            auto* malformed = new Sample(100);
            malformed->SetActualDataLength(invalidLength);
            slot.Push(malformed);
            Check(malformed->References() == 1 && slot.TakeLatest(arrival) == nullptr &&
                  captured == 0, "invalid raw frame length is not retained or counted");
            malformed->Release();
        }
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
        Check(logged == 0, "capture callback performs no diagnostic log I/O");
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
    TestDelayedFirstCallback();
    TestCaptureGraphEvents();
    return failures ? 1 : 0;
}
