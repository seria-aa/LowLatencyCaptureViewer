#include "ui/WindowGeometry.h"

#include <cstdio>

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
    return ok ? 0 : 1;
}
