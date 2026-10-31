#pragma once

#include "settings/AppSettings.h"
#include <dxgi1_5.h>

namespace llcv::presentation {

using Mode = settings::PresentationMode;

constexpr bool IsExclusive(Mode mode) { return mode == Mode::ExclusiveFullscreen; }
constexpr bool UsesVSync(Mode mode) { return mode == Mode::VSync || mode == Mode::Compatibility; }
constexpr bool IsCompatibility(Mode mode) { return mode == Mode::Compatibility; }
constexpr bool UsesPresentationReadiness(Mode mode) { return mode == Mode::VSync; }
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
