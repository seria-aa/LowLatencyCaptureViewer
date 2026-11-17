// Run the production event loop against process-local COM fakes. No endpoint
// or capture device is opened and no audio is played.
#include <mmdeviceapi.h>
static HRESULT WINAPI TestCreate(REFCLSID, IUnknown*, DWORD, REFIID, void**);
#define CoCreateInstance TestCreate
#include "../src/audio/WasapiOutput.cpp"
#undef CoCreateInstance
#include <cstdlib>

static void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", message); std::abort(); }
}
static std::atomic<bool> running{true};
struct Trace {
    bool exclusive = true, started = false;
    HRESULT paddingResult = S_OK;
    UINT32 frames = 480, padding = 0;
    unsigned paddingCalls = 0, renderCalls = 0, releases = 0, fills = 0;
    unsigned initialized[2]{}, destroyed[2]{}, activated = 0;
    UINT32 lastRequested = 0, observedPadding = UINT32_MAX;
    HANDLE event = nullptr;
    unsigned clockServices = 0, clockReads = 0, clockReleased = 0;
};
static Trace* activeTrace = nullptr;

class Renderer final : public IAudioRenderClient {
public:
    explicit Renderer(Trace& trace) : t(trace) {}
    STDMETHODIMP QueryInterface(REFIID, void** p) override { *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override { auto n = --refs; if (!n) delete this; return n; }
    STDMETHODIMP GetBuffer(UINT32 frames, BYTE** p) override {
        const UINT32 expected = t.exclusive || !t.renderCalls ? t.frames : t.frames - t.padding;
        Check(frames == expected && frames <= t.frames, "whole Exclusive packet / available Shared frames");
        ++t.renderCalls; t.lastRequested = frames;
        *p = reinterpret_cast<BYTE*>(data); return S_OK;
    }
    STDMETHODIMP ReleaseBuffer(UINT32 frames, DWORD) override {
        Check(frames == t.lastRequested, "release matches request");
        ++t.releases;
        if (t.started) SetEvent(t.event);
        return S_OK;
    }
private:
    Trace& t; ULONG refs = 1; int16_t data[960]{};
};

class Clock final : public IAudioClock {
public:
    explicit Clock(Trace& trace) : t(trace) {}
    STDMETHODIMP QueryInterface(REFIID, void** p) override { *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto n = --refs; if (!n) { ++t.clockReleased; delete this; } return n;
    }
    STDMETHODIMP GetFrequency(UINT64* value) override { *value = 48000; return S_OK; }
    STDMETHODIMP GetPosition(UINT64* value, UINT64* qpc) override {
        *value = ++t.clockReads * t.frames; if (qpc) *qpc = 0; return S_OK;
    }
    STDMETHODIMP GetCharacteristics(DWORD* value) override { *value = 0; return S_OK; }
private:
    Trace& t; ULONG refs = 1;
};

class Client final : public IAudioClient {
public:
    Client(Trace& trace, unsigned index) : t(trace), id(index) {}
    STDMETHODIMP QueryInterface(REFIID, void** p) override { *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override {
        auto n = --refs; if (!n) { ++t.destroyed[id]; delete this; } return n;
    }
    STDMETHODIMP Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME buffer,
                            REFERENCE_TIME period, const WAVEFORMATEX* format, LPCGUID) override {
        Check(++t.initialized[id] == 1 && format->nSamplesPerSec == 48000,
              "single initialization at original sample rate");
        Check(mode == (t.exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED), "mode unchanged");
        Check((flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0 &&
              period == (t.exclusive ? buffer : 0), "event duration policy unchanged");
        return S_OK;
    }
    STDMETHODIMP GetBufferSize(UINT32* frames) override { *frames = t.frames; return S_OK; }
    STDMETHODIMP GetStreamLatency(REFERENCE_TIME*) override { return E_NOTIMPL; }
    STDMETHODIMP GetCurrentPadding(UINT32* p) override {
        ++t.paddingCalls; *p = t.padding;
        if (t.paddingCalls == 4) running = false;
        // Full Shared buffers have no render submission to trigger the next wake.
        if (t.padding >= t.frames) SetEvent(t.event);
        return t.paddingResult;
    }
    STDMETHODIMP IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX** p) override {
        if (p) *p = nullptr; return S_OK;
    }
    STDMETHODIMP GetMixFormat(WAVEFORMATEX**) override { return E_NOTIMPL; }
    STDMETHODIMP GetDevicePeriod(REFERENCE_TIME* a, REFERENCE_TIME* b) override { *a = *b = 100000; return S_OK; }
    STDMETHODIMP Start() override { t.started = true; SetEvent(t.event); return S_OK; }
    STDMETHODIMP Stop() override { t.started = false; return S_OK; }
    STDMETHODIMP Reset() override { return E_NOTIMPL; }
    STDMETHODIMP SetEventHandle(HANDLE event) override { t.event = event; return S_OK; }
    STDMETHODIMP GetService(REFIID iid, void** p) override {
        *p = nullptr;
        if (iid == __uuidof(IAudioClock)) {
            ++t.clockServices; *p = static_cast<IAudioClock*>(new Clock(t)); return S_OK;
        }
        if (iid != __uuidof(IAudioRenderClient)) return E_NOINTERFACE;
        *p = static_cast<IAudioRenderClient*>(new Renderer(t)); return S_OK;
    }
private:
    Trace& t; unsigned id; ULONG refs = 1;
};

class Device final : public IMMDevice {
public:
    explicit Device(Trace& trace) : t(trace) {}
    STDMETHODIMP QueryInterface(REFIID, void** p) override { *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override { auto n = --refs; if (!n) delete this; return n; }
    STDMETHODIMP Activate(REFIID iid, DWORD, PROPVARIANT*, void** p) override {
        Check(iid == __uuidof(IAudioClient) && t.activated < 2, "only expected audio client activations");
        *p = static_cast<IAudioClient*>(new Client(t, t.activated++)); return S_OK;
    }
    STDMETHODIMP OpenPropertyStore(DWORD, IPropertyStore**) override { return E_NOTIMPL; }
    STDMETHODIMP GetId(LPWSTR*) override { return E_NOTIMPL; }
    STDMETHODIMP GetState(DWORD*) override { return E_NOTIMPL; }
private:
    Trace& t; ULONG refs = 1;
};
class Enumerator final : public IMMDeviceEnumerator {
public:
    STDMETHODIMP QueryInterface(REFIID, void** p) override { *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override { auto n = --refs; if (!n) delete this; return n; }
    STDMETHODIMP EnumAudioEndpoints(EDataFlow, DWORD, IMMDeviceCollection**) override { return E_NOTIMPL; }
    STDMETHODIMP GetDefaultAudioEndpoint(EDataFlow, ERole, IMMDevice**) override { return E_NOTIMPL; }
    STDMETHODIMP GetDevice(LPCWSTR, IMMDevice** p) override { *p = new Device(*activeTrace); return S_OK; }
    STDMETHODIMP RegisterEndpointNotificationCallback(IMMNotificationClient*) override { return E_NOTIMPL; }
    STDMETHODIMP UnregisterEndpointNotificationCallback(IMMNotificationClient*) override { return E_NOTIMPL; }
private:
    ULONG refs = 1;
};
static HRESULT WINAPI TestCreate(REFCLSID cls, IUnknown*, DWORD, REFIID iid, void** p) {
    Check(cls == __uuidof(MMDeviceEnumerator) && iid == __uuidof(IMMDeviceEnumerator), "only fake enumeration allowed");
    *p = static_cast<IMMDeviceEnumerator*>(new Enumerator); return S_OK;
}
static llcv::wasapi::FillResult Fill(void* context, int16_t* data, size_t frames) {
    auto& t = *static_cast<Trace*>(context);
    Check(frames == t.lastRequested, "source fill length matches render packet");
    std::fill_n(data, frames * 2, int16_t{0});
    if (++t.fills == 4) running = false;
    llcv::wasapi::FillResult result; result.writtenFrames = frames; return result;
}
static void Padding(void* context, UINT32 frames) { static_cast<Trace*>(context)->observedPadding = frames; }
static void QuietLog(const wchar_t*) {}

int main() {
    for (bool diagnostics : {true, false}) for (bool exclusive : {true, false}) {
        for (unsigned scenario = 0; scenario < 6; ++scenario) {
            const UINT32 values[] = {0, 1, 240, 480, UINT32_MAX, 0};
            Trace trace; trace.exclusive = exclusive; trace.padding = values[scenario];
            if (scenario == 5) trace.paddingResult = E_FAIL;
            activeTrace = &trace; running = true;
            llcv::wasapi::Configuration config;
            config.mode = exclusive ? llcv::wasapi::Mode::Exclusive : llcv::wasapi::Mode::Shared;
            config.endpointId = L"mock";
            config.detailedDiagnostics = diagnostics;
            llcv::wasapi::Host host; host.context = &trace; host.running = &running;
            host.fill = Fill; host.paddingChanged = Padding; host.log = QuietLog;
            const auto result = llcv::wasapi::Run(config, host);
            Check(trace.clockServices == (exclusive && diagnostics ? 1u : 0u) &&
                  trace.clockReads == (exclusive && diagnostics ? 4u : 0u) &&
                  trace.clockReleased == trace.clockServices,
                  "diagnostics off skips clock COM calls; on retains reads and cleanup");
            if (exclusive) {
                Check(result == llcv::wasapi::RunResult::Stopped && trace.paddingCalls == 0 &&
                      trace.renderCalls == 5 && trace.fills == 4 && trace.observedPadding == 0,
                      "Exclusive ignores padding support/value and writes four complete event packets");
            } else if (scenario == 5) {
                Check(result == llcv::wasapi::RunResult::Retry && trace.paddingCalls == 1 &&
                      trace.renderCalls == 1 && trace.fills == 0, "Shared padding failure still stops runtime safely");
            } else {
                Check(result == llcv::wasapi::RunResult::Stopped && trace.paddingCalls == 4 &&
                      trace.observedPadding == trace.padding, "Shared still queries/reports current padding");
                Check(trace.renderCalls == (trace.padding < trace.frames ? 5u : 1u),
                      "Shared only fills available space, including full/invalid padding guard");
            }
            Check(trace.renderCalls == trace.releases && !trace.started, "all render buffers released and client stopped");
            for (unsigned i = 0; i < trace.activated; ++i)
                Check(trace.destroyed[i] == 1, "every activated client released");
        }
    }
    for (bool exclusive : {true, false}) {
        // A second owner closes the output while the application keeps running.
        Trace trace; trace.exclusive = exclusive; activeTrace = &trace; running = true;
        std::atomic<bool> stop{true};
        llcv::wasapi::Configuration config;
        config.mode = exclusive ? llcv::wasapi::Mode::Exclusive : llcv::wasapi::Mode::Shared;
        config.endpointId = L"mock";
        llcv::wasapi::Host host; host.context = &trace; host.running = &running; host.stop = &stop;
        host.fill = Fill; host.paddingChanged = Padding; host.log = QuietLog;
        const auto result = llcv::wasapi::Run(config, host);
        Check(result == llcv::wasapi::RunResult::Stopped && trace.fills == 0 && running.load(),
              "a stop request ends the output without any fill while the app stays running");
        Check(trace.renderCalls == trace.releases && !trace.started, "stopped output releases buffers and the client");
        for (unsigned i = 0; i < trace.activated; ++i)
            Check(trace.destroyed[i] == 1, "stopped output releases every activated client");
    }
    std::puts("WASAPI: 24 event/padding/diagnostics scenarios passed; no audio device opened.");
}
