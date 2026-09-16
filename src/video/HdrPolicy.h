#pragma once

#include "CaptureColorMetadata.h"
#include "HdrChroma.h"
#include <dxgicommon.h>

namespace llcv::hdr {

enum class InputKind { Sdr, Unknown, Hdr10, Unsupported };
struct Input {
    InputKind kind = InputKind::Unknown;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;
    bool assumed = false;
    bool chromaOverridden = false;
    const wchar_t* reason = L"P010 without color metadata; SDR assumed (not tone-mapped)";
};

// P010 describes storage, not a transfer function. Only PQ/BT.2020 can use
// our HDR10 path. Unspecified fields in a known PQ signal use HDR10 defaults;
// contradictory fields never silently fall back to BT.709.
// Defaults: BT.2100-3 Tables 8/9 specify top-left cosited/narrow range.
inline Input ResolveInput(const video::CaptureColorMetadata& m, bool force,
                          ChromaLocation location = ChromaLocation::Auto) {
    Input result;
    const UINT transfer = m.present ? m.transferFunction : 0;
    const UINT primaries = m.present ? m.primaries : 0;
    const UINT matrix = m.present ? m.transferMatrix : 0;
    const bool pq = transfer == 15 || force;
    if (!pq) {
        if (transfer == 16 || primaries == 9 || matrix == 4 || matrix == 5 ||
            (transfer != 0 && transfer != 1 && transfer != 2 && transfer != 4 &&
             transfer != 5 && transfer != 6 && transfer != 7 && transfer != 8)) {
            result.kind = InputKind::Unsupported;
            result.reason = L"P010 transfer/gamut unsupported or incomplete; confirmed PQ requires Force HDR10, HLG is not supported";
        } else if (transfer || primaries || matrix) {
            result.kind = InputKind::Sdr;
            result.reason = L"P010 SDR color metadata";
        }
        return result;
    }
    result.kind = InputKind::Unsupported;
    if (!force && ((primaries && primaries != 9) ||
                   (matrix && matrix != 4 && matrix != 5))) {
        result.reason = L"PQ metadata contradicts BT.2020 HDR10";
        return result;
    }
    if (m.present && m.nominalRange != 0 && m.nominalRange != 2) {
        result.reason = L"HDR10 requires Limited P010; Full/other ranges are not supported by this video-processor path";
        return result;
    }
    const UINT chroma = m.present ? m.chromaSubsampling : 0;
    if (location != ChromaLocation::Auto && location != ChromaLocation::TopLeft &&
        location != ChromaLocation::Left) {
        result.reason = L"Invalid HDR chroma placement override; select Auto, Top-left or Left";
        return result;
    }
    const bool overrideChroma = location != ChromaLocation::Auto;
    // DXVA: 5=MPEG2 (left), 7=cosited (top-left); bit 8=progressive.
    if (!overrideChroma && chroma && (chroma & 7) != 5 && (chroma & 7) != 7) {
        result.reason = L"HDR10 chroma placement is not supported; select Top-left or Left in HDR chroma placement only if the device metadata is incorrect";
        return result;
    }
    const bool left = overrideChroma ? location == ChromaLocation::Left
                                     : chroma && (chroma & 7) == 5;
    result.colorSpace = left
        ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020
        : DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;
    result.kind = InputKind::Hdr10;
    result.chromaOverridden = overrideChroma;
    result.assumed = force || overrideChroma || !primaries || !matrix || !m.nominalRange || !chroma;
    result.reason = force ? L"user-forced PQ/BT.2020; range/chroma still validated"
        : result.assumed ? L"PQ/BT.2020; unspecified fields use HDR10 Limited/top-left defaults"
                         : L"explicit PQ/BT.2020 HDR10 metadata";
    if (overrideChroma)
        result.reason = force
            ? L"user-forced PQ/BT.2020 with explicit chroma placement; range still validated"
            : L"PQ/BT.2020 with explicit chroma placement override; range still validated";
    return result;
}

// A changed transfer function invalidates the old tuple. Do not combine an
// old PQ/2020 tuple with a newly negotiated SDR transfer (or the reverse).
inline video::CaptureColorMetadata ConnectedMetadata(
    const video::CaptureColorMetadata& prior,
    const video::CaptureColorMetadata& connected) {
    if (!connected.present) return prior;
    if (connected.transferFunction &&
        connected.transferFunction != prior.transferFunction) return connected;
    auto result = prior;
    result.present = true;
    result.controlFlags = connected.controlFlags;
    if (connected.chromaSubsampling) result.chromaSubsampling = connected.chromaSubsampling;
    if (connected.nominalRange) result.nominalRange = connected.nominalRange;
    if (connected.transferMatrix) result.transferMatrix = connected.transferMatrix;
    if (connected.primaries) result.primaries = connected.primaries;
    if (connected.transferFunction) result.transferFunction = connected.transferFunction;
    return result;
}

} // namespace llcv::hdr
