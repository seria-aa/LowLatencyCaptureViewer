#pragma once

#include "settings/AppSettings.h"
#include <dxgi1_5.h>

namespace llcv::presentation {

using Mode = settings::PresentationMode;

constexpr bool UsesVSync(Mode mode) { return mode != Mode::AllowTearing; }
constexpr bool IsCompatibility(Mode mode) { return mode == Mode::Compatibility; }
constexpr const wchar_t* PathName(Mode mode) {
    return IsCompatibility(mode) ? L"Blt-discard" : L"Flip-discard";
}
constexpr const wchar_t* ModeName(Mode mode) {
    return IsCompatibility(mode) ? L"Blt + VSync"
         : UsesVSync(mode) ? L"VSync" : L"Immediate";
}

// Keep the original flip path unchanged. The opt-in SDR comparison path has
// neither tearing nor flip-only waitable-object flags; it is never exclusive.
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
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (tearingSupported) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    return desc;
}

}  // namespace llcv::presentation
