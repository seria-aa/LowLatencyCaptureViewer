#pragma once
#include <windows.h>

namespace llcv::hdr {
// DISPLAYCONFIG_SDR_WHITE_LEVEL is an 80-nit multiplier in thousandths.
// Do not impose an undocumented 80..1000 nit UI range on a successful query.
// Only reject zero or values outside PQ's absolute representable luminance.
inline bool DecodeSdrWhiteLevel(ULONG level, float& nits) {
    const float value = level * 0.08f;
    if (level == 0 || value > 10000.0f) return false;
    nits = value;
    return true;
}

struct DisplayState {
    int hdr = -1; // -1 unknown, 0 SDR, 1 HDR (actual DXGI output state)
    UINT bits = 0;
    float uiWhiteNits = 203.0f; // reference white if Windows query unavailable
    bool systemWhite = false;
    wchar_t name[32]{};
};
// Initialization / UI timer only. Never called from the steady-state frame loop.
DisplayState QueryDisplay(HWND window);
}
