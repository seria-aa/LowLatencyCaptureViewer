#include "video/HdrPolicy.h"
#include "video/HdrDisplay.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <initializer_list>
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
    // Reproduce the reported device tuples without opening a capture device.
    video::CaptureColorMetadata ezcap{};
    ezcap.present = true;
    ezcap.controlFlags = 0x008CA681;
    ezcap.primaries = 2;
    ezcap.transferMatrix = 1;
    ezcap.nominalRange = 2;
    ezcap.chromaSubsampling = 6;
    Require(hdr::ResolveInput(ezcap, true).kind == hdr::InputKind::Unsupported,
        "reported chroma 6 must not silently become top-left under Force HDR");
    for (auto mode : {hdr::ChromaLocation::TopLeft, hdr::ChromaLocation::Left}) {
        const auto explicitChoice = hdr::ResolveInput(ezcap, true, mode);
        Require(explicitChoice.kind == hdr::InputKind::Hdr10 && explicitChoice.assumed &&
                explicitChoice.chromaOverridden && explicitChoice.colorSpace ==
                (mode == hdr::ChromaLocation::Left ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 :
                                                   DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020),
            "reported chroma 6 only bypassed by explicit recorded interpretation");
        Require(hdr::ResolveInput(ezcap, false, mode).kind != hdr::InputKind::Hdr10,
            "chroma override alone cannot turn SDR/unknown transfer into HDR");
        Require(hdr::ResolveInput(conflict, false, mode).kind == hdr::InputKind::Unsupported &&
                hdr::ResolveInput(hlg, false, mode).kind == hdr::InputKind::Unsupported,
            "placement override cannot bypass gamut or HLG rejection");
        for (UINT range = 0; range < 8; ++range) {
            auto tuple = ezcap; tuple.nominalRange = range;
            Require((hdr::ResolveInput(tuple, true, mode).kind == hdr::InputKind::Hdr10) ==
                    (range == 0 || range == 2), "placement override preserves range guard");
        }
        for (UINT chroma = 0; chroma < 16; ++chroma) {
            auto tuple = m; tuple.chromaSubsampling = chroma;
            const auto selected = hdr::ResolveInput(tuple, false, mode);
            Require(selected.kind == hdr::InputKind::Hdr10 && selected.chromaOverridden &&
                    tuple.chromaSubsampling == chroma, "manual interpretation preserves original metadata");
        }
    }
    Require(hdr::ResolveInput(m, true, static_cast<hdr::ChromaLocation>(99)).kind == hdr::InputKind::Unsupported,
        "invalid programmatic placement rejected");
    Require(!hdr::ResolveInput(m, false).chromaOverridden,
        "Auto does not claim a manual override");
    // Cross-field regression: a placement override must ONLY relax chroma.
    // Unknown/SDR/HLG, contradictory gamut, and unsupported ranges retain the
    // same independent HDR gate; no state may leak between consecutive calls.
    unsigned combinations = 0;
    for (UINT transfer : {0u, 5u, 15u, 16u, 99u})
    for (UINT primaries : {0u, 2u, 9u, 99u})
    for (UINT matrix : {0u, 1u, 4u, 5u, 99u})
    for (UINT range = 0; range < 8; ++range)
    for (UINT chroma = 0; chroma < 16; ++chroma)
    for (bool force : {false, true})
    for (auto mode : {hdr::ChromaLocation::Auto, hdr::ChromaLocation::TopLeft,
                     hdr::ChromaLocation::Left}) {
        video::CaptureColorMetadata tuple{};
        tuple.present = true; tuple.transferFunction = transfer;
        tuple.primaries = primaries; tuple.transferMatrix = matrix;
        tuple.nominalRange = range; tuple.chromaSubsampling = chroma;
        const bool manual = mode != hdr::ChromaLocation::Auto;
        const bool supported = (force || transfer == 15) &&
            (force || ((primaries == 0 || primaries == 9) &&
                       (matrix == 0 || matrix == 4 || matrix == 5))) &&
            (range == 0 || range == 2) &&
            (manual || chroma == 0 || chroma == 5 || chroma == 7 || chroma == 13 || chroma == 15);
        const auto resolved = hdr::ResolveInput(tuple, force, mode);
        Require((resolved.kind == hdr::InputKind::Hdr10) == supported,
            "cross-field placement must not bypass independent HDR gates");
        Require(resolved.chromaOverridden == (supported && manual),
            "override flag only describes an accepted manual HDR interpretation");
        ++combinations;
    }
    std::printf("HDR interpretation cross-field cases: %u\n", combinations);
    auto explicitCosited = ezcap;
    explicitCosited.chromaSubsampling = 7;
    Require(hdr::ResolveInput(explicitCosited, true).kind == hdr::InputKind::Hdr10,
        "explicit cosited tuple remains available with user-forced PQ/2020");
    for (unsigned repeat = 0; repeat < 1000; ++repeat) {
        const video::CaptureColorMetadata absent{};
        Require(hdr::ResolveInput(absent, true).kind == hdr::InputKind::Hdr10,
            "GC573 absent metadata plus force is an explicit assumption");
        Require(hdr::ResolveInput(absent, false).kind == hdr::InputKind::Unknown,
            "turning force off must not retain prior HDR interpretation");
        const auto toSdr = hdr::ConnectedMetadata(m, sdr);
        Require(toSdr.primaries == 0 && toSdr.transferMatrix == 0 &&
                hdr::ResolveInput(toSdr, false).kind == hdr::InputKind::Sdr,
            "HDR-to-SDR negotiation drops old gamut and matrix");
        const auto toPq = hdr::ConnectedMetadata(toSdr, partial);
        Require(hdr::ResolveInput(toPq, false).kind == hdr::InputKind::Hdr10,
            "SDR-to-PQ negotiation resolves independently on repeated restarts");
    }
    std::puts("HDR policy cases passed");
}
