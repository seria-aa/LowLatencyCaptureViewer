#include "ui/WindowGeometry.h"

#include <cstdio>
#include <cstdlib>

namespace {
bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}
}

int main() {
    using namespace llcv::window_geometry;
    bool ok = true;
    const AspectConstraint aspect{16, 39, 16, 9, 320, 180};

    RECT rightEdge{100, 100, 756, 700};
    ConstrainToAspect(rightEdge, WMSZ_RIGHT, aspect);
    ok &= Check(rightEdge.left == 100 && rightEdge.top == 100 &&
                    rightEdge.right == 756 && rightEdge.bottom == 499,
                "right-edge resize must preserve the 16:9 client ratio");

    RECT topEdge{100, 100, 900, 499};
    ConstrainToAspect(topEdge, WMSZ_TOP, aspect);
    ok &= Check(topEdge.bottom == 499 && topEdge.top == 100 &&
                    topEdge.right == 756,
                "top-edge resize must preserve the opposite edge");

    RECT tooSmall{20, 20, 100, 80};
    ConstrainToAspect(tooSmall, WMSZ_BOTTOMRIGHT, aspect);
    ok &= Check(tooSmall.right - tooSmall.left >= 336 &&
                    tooSmall.bottom - tooSmall.top >= 219,
                "aspect constraint must enforce the minimum client size");

    // Exercise every drag handle, including monitors left/above the primary
    // display, with both decorated and borderless window frames.
    const UINT edges[] = {WMSZ_LEFT, WMSZ_RIGHT, WMSZ_TOP, WMSZ_BOTTOM,
                          WMSZ_TOPLEFT, WMSZ_TOPRIGHT,
                          WMSZ_BOTTOMLEFT, WMSZ_BOTTOMRIGHT};
    const AspectConstraint constraints[] = {
        {16, 39, 1920, 1080, 320, 180},
        {24, 59, 2560, 1440, 320, 180},
        {0, 0, 3840, 2160, 320, 180}};
    const RECT proposals[] = {
        {-1800, -900, -1040, -460},
        {-1800, -900, -1790, -890},
        {-1800, -900, 1745, 900}};
    for (const auto& constraint : constraints) {
        for (const RECT& proposal : proposals) {
            for (const UINT edge : edges) {
                RECT result = proposal;
                ConstrainToAspect(result, edge, constraint);
                const long width = result.right - result.left -
                    constraint.frameWidth;
                const long height = result.bottom - result.top -
                    constraint.frameHeight;
                ok &= Check(width >= constraint.minimumClientWidth &&
                                height >= constraint.minimumClientHeight,
                            "every resize edge must enforce client minimums");
                ok &= Check(std::abs(width * 9 - height * 16) <= 8,
                            "every resize edge must preserve 16:9 within rounding");
                const bool movesLeft = edge == WMSZ_LEFT ||
                    edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT;
                const bool movesTop = edge == WMSZ_TOP ||
                    edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
                ok &= Check(movesLeft ? result.right == proposal.right
                                       : result.left == proposal.left,
                            "resize must preserve the opposite horizontal anchor");
                ok &= Check(movesTop ? result.bottom == proposal.bottom
                                      : result.top == proposal.top,
                            "resize must preserve the opposite vertical anchor");
            }
        }
    }

    const RECT window{100, 100, 900, 600};
    ok &= Check(BorderlessHitTest(window, POINT{102, 102}, 8, true) ==
                    HTTOPLEFT,
                "borderless corner must expose a resize hit target");
    ok &= Check(BorderlessHitTest(window, POINT{500, 300}, 8, true) ==
                    HTCAPTION,
                "borderless center must remain draggable");
    ok &= Check(BorderlessHitTest(window, POINT{102, 102}, 8, false) ==
                    HTCAPTION,
                "fixed pixel-perfect windows must not expose resize edges");
    const struct {
        POINT point;
        LRESULT expected;
    } hitTargets[] = {
        {{102, 300}, HTLEFT}, {{897, 300}, HTRIGHT},
        {{500, 102}, HTTOP}, {{500, 597}, HTBOTTOM},
        {{102, 102}, HTTOPLEFT}, {{897, 102}, HTTOPRIGHT},
        {{102, 597}, HTBOTTOMLEFT}, {{897, 597}, HTBOTTOMRIGHT},
        {{108, 108}, HTCAPTION}};
    for (const auto& target : hitTargets) {
        ok &= Check(BorderlessHitTest(window, target.point, 8, true) ==
                        target.expected,
                    "each borderless drag handle must identify the parent edge");
        ok &= Check(BorderlessHitTest(window, target.point, 8, false) ==
                        HTCAPTION,
                    "fixed windows must keep every borderless edge draggable");
    }
    return ok ? 0 : 1;
}
