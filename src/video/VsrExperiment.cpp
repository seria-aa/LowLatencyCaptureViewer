#include "VsrExperiment.h"
#include <algorithm>
#include <cmath>
#include <dxgi.h>

namespace llcv::vsr {
AdapterProbe ProbeDefaultAdapter() {
    AdapterProbe probe;
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    probe.result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);
    if (FAILED(probe.result)) return probe;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(probe.result = device.As(&dxgi)) ||
        FAILED(probe.result = dxgi->GetAdapter(&adapter)) ||
        FAILED(probe.result = adapter->GetDesc(&desc))) return probe;
    probe.vendor = desc.VendorId;
    probe.luid = desc.AdapterLuid;
    probe.rtx30=IsGeForceRtx30(desc.VendorId,desc.Description);
    probe.prefersVfg=NvidiaAdapter(desc.VendorId) &&
        (wcsstr(desc.Description,L"GeForce RTX 40") || wcsstr(desc.Description,L"GeForce RTX 50"));
    return probe;
}
SupportProbe ProbeSupport() {
    SupportProbe probe;
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    probe.result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(probe.result)) return probe;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(probe.result = device.As(&dxgiDevice)) ||
        FAILED(probe.result = dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(probe.result = adapter->GetDesc(&desc))) return probe;
    probe.adapter = desc.Description;
    if (desc.VendorId != 0x10de) {
        probe.status = SupportProbe::Status::NonNvidia;
        probe.result = DXGI_ERROR_UNSUPPORTED;
        return probe;
    }
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    if (FAILED(probe.result = device.As(&videoDevice)) ||
        FAILED(probe.result = context.As(&videoContext))) return probe;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = 1280; content.InputHeight = 720;
    content.OutputWidth = 2560; content.OutputHeight = 1440;
    content.InputFrameRate = content.OutputFrameRate = {60, 1};
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(probe.result = videoDevice->CreateVideoProcessorEnumerator(&content, &enumerator)))
        return probe;
    UINT inputFlags = 0, outputFlags = 0;
    if (FAILED(probe.result = enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &inputFlags)) ||
        FAILED(probe.result = enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &outputFlags)))
        return probe;
    if (!(inputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        !(outputFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        probe.result = DXGI_ERROR_UNSUPPORTED;
        return probe;
    }
    if (FAILED(probe.result = videoDevice->CreateVideoProcessor(enumerator.Get(), 0, &processor)))
        return probe;
    probe.result = SetRequest(videoContext.Get(), processor.Get(), true);
    const HRESULT off = SetRequest(videoContext.Get(), processor.Get(), false);
    if (probe.result == S_OK && off == S_OK)
        probe.status = SupportProbe::Status::RequestSupported;
    else if (probe.result == S_OK) probe.result = off;
    return probe;
}
HRESULT SetRequest(ID3D11VideoContext* context, ID3D11VideoProcessor* processor, bool enable) {
    if (!context || !processor) return E_POINTER;
    constexpr GUID extension{0xd43ce1b3, 0x1f4b, 0x48ac,
        {0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7}};
    struct Request { UINT version, method, enable; } request{1, 2, enable ? 1u : 0u};
    static_assert(sizeof(Request) == 12);
    return context->VideoProcessorSetStreamExtension(processor, 0, &extension,
                                                    sizeof(request), &request);
}
const wchar_t* StateName(State state) {
    switch (state) {
    case State::Untouched: return L"untouched";
    case State::Bypassed: return L"bypassed (not eligible)";
    case State::Off: return L"OFF requested";
    case State::Requested: return L"ON requested (activation unverified)";
    case State::Rejected: return L"ON rejected; OFF requested";
    case State::Unknown: return L"unknown (switch failed)";
    }
    return L"unknown";
}
void Distribution::Add(double ms) {
    if (!std::isfinite(ms) || ms < 0) return;
    const auto index = static_cast<size_t>((std::min)(1000.0, std::floor(ms * 10.0)));
    ++bins[index]; ++count; total += ms; maximum = (std::max)(maximum, ms);
}
double Distribution::Percentile(double fraction) const {
    if (!count) return 0;
    const auto target = (std::max)(uint64_t{1}, static_cast<uint64_t>(
        std::ceil(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(count))));
    uint64_t accumulated = 0;
    for (size_t i = 0; i < bins.size(); ++i) {
        accumulated += bins[i];
        if (accumulated >= target) return i == 1000 ? maximum : (i + 1) / 10.0;
    }
    return maximum;
}
HRESULT Timing::Initialize(ID3D11Device* device, unsigned warmupFrames) {
    Reset();
    if (!device) return E_POINTER;
    // GPU timestamps around VideoProcessorBlt substantially under-reported
    // observed completion time on RTX 3080. Do not expose them as VSR latency.
    // The separate synthetic test checks completion using a forced readback;
    // the viewer has neither readbacks nor timing queries in its hot path.
    warmup_ = warmupFrames;
    ready_ = true;
    return S_OK;
}
void Timing::Reset() {
    results_ = {}; warmup_ = 0; ready_ = false; sample_ = false;
    previousPresent_ = {};
}
void Timing::Begin(ID3D11DeviceContext* context) {
    sample_ = false;
    if (!ready_ || !context) return;
    if (warmup_) { --warmup_; return; }
    sample_ = true;
    cpuStart_ = Clock::now();
}
void Timing::End(ID3D11DeviceContext* context, bool success) {
    if (!sample_ || !context) return;
    if (success) results_.cpuBlt.Add(std::chrono::duration<double, std::milli>(
        Clock::now() - cpuStart_).count());
    if (!success) sample_ = false;
}
void Timing::AddCaptureToPresent(double ms) {
    if (sample_) results_.captureToPresent.Add(ms);
}
void Timing::AddUpload(double ms) {
    if (ready_ && !warmup_) results_.upload.Add(ms);
}
void Timing::AddPresent(double ms) {
    if (!sample_) return;
    results_.present.Add(ms);
    const auto now = Clock::now();
    if (previousPresent_ != Clock::time_point{})
        results_.intervals.Add(std::chrono::duration<double,std::milli>(now-previousPresent_).count());
    previousPresent_ = now;
}
Timing::Results Timing::Snapshot() const {
    return results_;
}
} // namespace llcv::vsr
