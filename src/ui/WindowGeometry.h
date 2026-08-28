#pragma once

#include <windows.h>

namespace llcv::window_geometry {

struct AspectConstraint {
    int frameWidth = 0;
    int frameHeight = 0;
    int videoWidth = 0;
    int videoHeight = 0;
    int minimumClientWidth = 320;
    int minimumClientHeight = 180;
};

void ConstrainToAspect(RECT& sizingRect, UINT sizingEdge,
                       const AspectConstraint& constraint) noexcept;
LRESULT BorderlessHitTest(const RECT& windowRect, POINT cursor,
                          int gripPixels, bool resizable) noexcept;

}  // namespace llcv::window_geometry
