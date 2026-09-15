#include "video/HdrPolicy.h"
#include "video/HdrDisplay.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace llcv;
static void Require(bool value, const char* text) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
int main() {
    float white = 203.0f;
    Require(hdr::DecodeSdrWhiteLevel(1000, white) && white == 80.0f, "Windows 1000 = 80 nit");
    Require(hdr::DecodeSdrWhiteLevel(2000, white) && white == 160.0f, "Windows 2000 = 160 nit");
    Require(hdr::DecodeSdrWhiteLevel(500, white) && white == 40.0f, "do not replace valid low UI white");
    Require(hdr::DecodeSdrWhiteLevel(20000, white) && white == 1600.0f, "do not replace valid high UI white");
    Require(!hdr::DecodeSdrWhiteLevel(0, white) && white == 1600.0f, "zero query leaves fallback intact");
    Require(!hdr::DecodeSdrWhiteLevel((std::numeric_limits<ULONG>::max)(), white), "invalid query cannot exceed PQ range");
    video::CaptureColorMetadata m{};
    Require(hdr::ResolveInput(m, false).kind == hdr::InputKind::Unknown, "absent != HDR");
    Require(hdr::ResolveInput(m, true).kind == hdr::InputKind::Hdr10, "explicit force");
    m.present = true; m.transferFunction = 15; m.primaries = 9;
    m.transferMatrix = 4; m.nominalRange = 2; m.chromaSubsampling = 7;
    auto good = hdr::ResolveInput(m, false);
    Require(good.kind == hdr::InputKind::Hdr10 && !good.assumed, "complete HDR10");
    for (UINT range = 0; range < 5; ++range) {
        auto test = m; test.nominalRange = range;
        Require((hdr::ResolveInput(test, false).kind == hdr::InputKind::Hdr10) ==
                (range == 0 || range == 2), "validate HDR range");
        Require((hdr::ResolveInput(test, true).kind == hdr::InputKind::Hdr10) ==
                (range == 0 || range == 2), "force does not bypass range");
    }
    for (UINT chroma = 0; chroma < 16; ++chroma) {
        auto test = m; test.chromaSubsampling = chroma;
        auto resolved = hdr::ResolveInput(test, false);
        Require((resolved.kind == hdr::InputKind::Hdr10) ==
            (chroma == 0 || chroma == 5 || chroma == 7 || chroma == 13 || chroma == 15), "chroma validation");
        if (chroma == 5 || chroma == 13)
            Require(resolved.colorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020, "left chroma");
    }
    auto partial = m; partial.primaries = partial.transferMatrix = 0;
    Require(hdr::ResolveInput(partial, false).kind == hdr::InputKind::Hdr10 &&
        hdr::ResolveInput(partial, false).assumed, "known PQ missing tuple uses HDR10 assumptions");
    // MF's 10/12-bit BT.2020 matrix enumerators both mean non-constant luminance.
    for (UINT matrix = 0; matrix <= 12; ++matrix) {
        auto test = m; test.transferMatrix = matrix;
        Require((hdr::ResolveInput(test, false).kind == hdr::InputKind::Hdr10) ==
            (matrix == 0 || matrix == 4 || matrix == 5), "do not conflate NCL with ICtCp/other matrices");
    }
    auto conflict = m; conflict.primaries = 2;
    Require(hdr::ResolveInput(conflict, false).kind == hdr::InputKind::Unsupported, "PQ/709 conflict");
    Require(hdr::ResolveInput(conflict, true).kind == hdr::InputKind::Hdr10, "force explicitly overrides tuple");
    auto hlg = m; hlg.transferFunction = 16;
    Require(hdr::ResolveInput(hlg, false).kind == hdr::InputKind::Unsupported, "HLG is not SDR or PQ");
    auto incomplete = m; incomplete.transferFunction = 0;
    Require(hdr::ResolveInput(incomplete, false).kind == hdr::InputKind::Unsupported, "2020 alone not SDR");
    video::CaptureColorMetadata sdr{}; sdr.present = true; sdr.transferFunction = 5;
    Require(hdr::ResolveInput(hdr::ConnectedMetadata(m, sdr), false).kind == hdr::InputKind::Sdr,
        "final SDR transfer replaces stale HDR tuple");
    Require(hdr::ResolveInput(hdr::ConnectedMetadata(sdr, partial), false).kind == hdr::InputKind::Hdr10,
        "final PQ transfer replaces stale SDR tuple");
    Require(hdr::ResolveInput(hdr::ConnectedMetadata(m, {}), false).kind == hdr::InputKind::Hdr10,
        "absent final flags preserve known metadata");
    std::puts("HDR policy cases passed");
}
