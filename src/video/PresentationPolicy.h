#pragma once

#include "settings/AppSettings.h"
#include <cstdint>
#include <dxgi1_5.h>

namespace llcv::presentation {

using Mode = settings::PresentationMode;

constexpr bool IsExclusive(Mode mode) { return mode == Mode::ExclusiveFullscreen; }
constexpr bool UsesVSync(Mode mode) { return mode == Mode::VSync || mode == Mode::Compatibility; }
constexpr bool IsCompatibility(Mode mode) { return mode == Mode::Compatibility; }
constexpr bool UsesPresentationReadiness(Mode mode) { return mode == Mode::VSync; }
enum class VfgBlock { None, Compatibility, RefreshRate, SyncUnavailable, SyncRecovering };

constexpr VfgBlock VfgPresentationBlock(Mode mode, int captureFps,
                                        unsigned refreshHz, bool hasReadiness) {
    if (IsCompatibility(mode)) return VfgBlock::Compatibility;
    if (mode != Mode::VSync) return VfgBlock::None;
    if (!hasReadiness) return VfgBlock::SyncUnavailable;
    // Windows reports an integer (e.g. 59/119 for 59.94/119.88).
    return captureFps > 0 && refreshHz > 1 && refreshHz + 1 >= 2u * unsigned(captureFps)
        ? VfgBlock::None : VfgBlock::RefreshRate;
}

constexpr bool UseVfgOriginalTimer(Mode mode, int captureFps, unsigned refreshHz) {
    // Near 2x, the next vblank already provides the half-frame separation.
    // Sleeping another half frame here can miss that slot entirely.
    return mode != Mode::VSync || (captureFps > 0 && refreshHz >= 3u * unsigned(captureFps));
}

constexpr DWORD VfgOriginalWaitMs(unsigned refreshHz) {
    if (refreshHz < 48) return 50;
    return (2000 + refreshHz - 1) / refreshHz + 4;
}

// A missed second slot is not evidence that the waitable object is broken.
// Retry after a run of successfully presented originals, not merely elapsed
// time while hidden. A new request also allows a deliberate fresh attempt.
class VfgSecondSlotRecovery {
  public:
    void ObserveRequest(bool active, std::uint64_t revision) {
        if (!active || revision_ != revision) remaining_ = 0;
        revision_ = revision;
    }
    void TimedOut(int captureFps) {
        remaining_ = captureFps < 24 ? 24 : captureFps > 60 ? 60 : captureFps;
    }
    bool OriginalOnly() const { return remaining_ != 0; }
    void OriginalPresented() {
        if (remaining_) --remaining_;
    }

  private:
    std::uint64_t revision_ = 0;
    int remaining_ = 0;
};

constexpr const wchar_t* VfgBlockText(VfgBlock reason, bool en) {
    switch (reason) {
    case VfgBlock::Compatibility:
        return en ? L"Not available in Compatibility mode" : L"호환 모드에서는 사용할 수 없음";
    case VfgBlock::RefreshRate:
        return en ? L"VSync needs double the capture rate" : L"VSync: 캡처 FPS의 두 배 주사율 필요";
    case VfgBlock::SyncUnavailable:
        return en ? L"VSync timing signal unavailable" : L"VSync 표시 준비 신호 없음";
    case VfgBlock::SyncRecovering:
        return en ? L"Retrying VSync after a missed display slot" : L"VSync 표시 기한 초과 · 재시도 대기 중";
    default: return L"";
    }
}
// P010 is HDR-only even when Force HDR10 is off. Audio-only has no video output.
constexpr bool SupportsCapture(Mode mode, settings::VideoPixelFormat format,
                               bool audioOnly = false) {
    return audioOnly || !IsCompatibility(mode) || format != settings::VideoPixelFormat::P010;
}
constexpr const wchar_t* PathName(Mode mode) {
    return IsCompatibility(mode) ? L"Blt-discard" : L"Flip-discard";
}
constexpr const wchar_t* ModeName(Mode mode) {
    return IsExclusive(mode) ? L"Exclusive fullscreen"
         : IsCompatibility(mode) ? L"Blt + VSync"
         : UsesVSync(mode) ? L"VSync" : L"Immediate";
}

constexpr UINT PresentFlags(Mode mode, bool actualExclusive, bool tearingSupported) {
    // ALLOW_TEARING is valid only for an interval-zero windowed Present.
    return !actualExclusive && !UsesVSync(mode) && tearingSupported
        ? DXGI_PRESENT_ALLOW_TEARING : 0u;
}

// Existing modes retain their original descriptors. The opt-in exclusive
// chain omits the D3D11 windowed-only waitable-object flag.
inline DXGI_SWAP_CHAIN_DESC1 Description(Mode mode, UINT width, UINT height,
                                         bool hdr, bool tearingSupported) {
    const bool compatibility = IsCompatibility(mode);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = hdr ? DXGI_FORMAT_R10G10B10A2_UNORM
                      : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = compatibility ? DXGI_SWAP_EFFECT_DISCARD
                                   : DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (!compatibility) {
        // D3D11 exclusive chains cannot carry the waitable-object flag.
        desc.Flags = IsExclusive(mode) ? 0 : DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (tearingSupported) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    return desc;
}

}  // namespace llcv::presentation
