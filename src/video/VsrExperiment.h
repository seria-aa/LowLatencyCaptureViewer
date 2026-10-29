#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>

// Opt-in driver enhancement. No SDK DLLs, CPU readbacks, extra video surfaces,
// future frames, or application frame queue are used by this path.
namespace llcv::vsr {
enum class Mode { Disabled, Off, On };
enum class State { Untouched, Bypassed, Off, Requested, Rejected, Unknown };
constexpr bool NvidiaAdapter(UINT vendor) { return vendor == 0x10de; }
struct AdapterProbe {
    HRESULT result = E_FAIL;
    UINT vendor = 0;
};
// Startup only, using the same default-device policy as the renderer. This
// identifies the selected vendor, not RTX capability or effect activation.
AdapterProbe ProbeDefaultAdapter();
constexpr bool Eligible(UINT vendor, DXGI_FORMAT format, bool hdr,
                        UINT width, UINT height, UINT outputWidth, UINT outputHeight) {
    // Native-resolution de-artifacting uses the same processor request as
    // upscaling. Never request it while either axis is being downscaled.
    // HDR stays P010 -> RGB10 BT.2020/PQ on the existing HDR10 processor.
    // This is VSR for native HDR, NOT RTX Video HDR (SDR-to-HDR conversion).
    const bool supportedRoute = hdr ? format == DXGI_FORMAT_P010
                                    : (format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_YUY2 ||
                                       format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8X8_UNORM);
    return vendor == 0x10de && supportedRoute &&
        width > 0 && height > 0 && outputWidth >= width && outputHeight >= height;
}

// Some drivers accept the extension but reject Blt for a particular format.
// Retry the SAME frame once with VSR OFF; keep HDR/color/rectangles untouched.
// Rejected is latched until an explicit mode change or processor rebuild, so
// unsupported drivers do not pay for a failed VSR request on every frame.
template<class Blit, class Disable>
HRESULT ProcessWithFallback(State& state, Blit&& blit, Disable&& disable) {
    const HRESULT hr = blit();
    if (SUCCEEDED(hr) || state != State::Requested ||
        hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_HUNG) return hr;
    const HRESULT offHr = disable();
    if (offHr != S_OK) {
        state = State::Unknown;
        return FAILED(offHr) ? offHr : E_FAIL;
    }
    state = State::Rejected;
    return blit();
}

// S_OK only means the driver accepted the request, NOT confirmed VSR activation.
// ABI also used by Chromium ui/gl/swap_chain_presenter.cc ToggleNvidiaVpSuperResolution.
HRESULT SetRequest(ID3D11VideoContext*, ID3D11VideoProcessor*, bool enable);
const wchar_t* StateName(State);
struct SupportProbe {
    enum class Status { Unavailable, NonNvidia, RequestSupported };
    Status status = Status::Unavailable;
    HRESULT result = E_FAIL;
    std::wstring adapter;
};
// Explicit settings action only. Isolated processor; never changes driver profiles,
// opens a capture device, or certifies the NVIDIA global setting/actual activation.
SupportProbe ProbeSupport();

struct Distribution {
    // Bounded histogram: no allocations or sorting on the render thread.
    // Bucket 1000 is overflow (>=100 ms); max/mean remain exact.
    std::array<uint64_t, 1001> bins{};
    uint64_t count = 0;
    double total = 0, maximum = 0;
    void Add(double ms);
    double Percentile(double fraction) const;
    double Mean() const { return count ? total / static_cast<double>(count) : 0; }
};

class Timing {
public:
    using Clock = std::chrono::steady_clock;
    struct Results {
        Distribution cpuBlt, captureToPresent, upload, present, intervals;
    };
    HRESULT Initialize(ID3D11Device*, unsigned warmupFrames);
    void Reset();
    void Begin(ID3D11DeviceContext*);
    void End(ID3D11DeviceContext*, bool success);
    void AddCaptureToPresent(double ms);
    void AddUpload(double ms);
    void AddPresent(double ms);
    Results Snapshot() const;
private:
    Results results_{};
    Clock::time_point cpuStart_{};
    Clock::time_point previousPresent_{};
    unsigned warmup_ = 0;
    bool ready_ = false, sample_ = false;
};
} // namespace llcv::vsr
