#include "ui/WindowGeometry.h"

#include <algorithm>
#include <cstdlib>

namespace llcv::window_geometry {

void ConstrainToAspect(RECT& sizingRect, UINT sizingEdge,
                       const AspectConstraint& constraint) noexcept {
    if (constraint.videoWidth <= 0 || constraint.videoHeight <= 0) return;

    const int minimumOuterWidth =
        constraint.frameWidth + constraint.minimumClientWidth;
    const int minimumOuterHeight =
        constraint.frameHeight + constraint.minimumClientHeight;
    const int proposedWidth = (std::max)(
        minimumOuterWidth,
        static_cast<int>(sizingRect.right - sizingRect.left));
    const int proposedHeight = (std::max)(
        minimumOuterHeight,
        static_cast<int>(sizingRect.bottom - sizingRect.top));

    const auto heightFromWidth = [&](int outerWidth) {
        const int clientWidth = (std::max)(
            constraint.minimumClientWidth,
            outerWidth - constraint.frameWidth);
        return constraint.frameHeight +
            MulDiv(clientWidth, constraint.videoHeight,
                   constraint.videoWidth);
    };
    const auto widthFromHeight = [&](int outerHeight) {
        const int clientHeight = (std::max)(
            constraint.minimumClientHeight,
            outerHeight - constraint.frameHeight);
        return constraint.frameWidth +
            MulDiv(clientHeight, constraint.videoWidth,
                   constraint.videoHeight);
    };

    const bool verticalEdge =
        sizingEdge == WMSZ_TOP || sizingEdge == WMSZ_BOTTOM;
    const bool horizontalEdge =
        sizingEdge == WMSZ_LEFT || sizingEdge == WMSZ_RIGHT;
    int outerWidth = proposedWidth;
    int outerHeight = proposedHeight;
    if (verticalEdge) {
        outerWidth = widthFromHeight(proposedHeight);
    } else if (horizontalEdge) {
        outerHeight = heightFromWidth(proposedWidth);
    } else {
        const int widthDrivenHeight = heightFromWidth(proposedWidth);
        const int heightDrivenWidth = widthFromHeight(proposedHeight);
        if (std::abs(widthDrivenHeight - proposedHeight) <=
            std::abs(heightDrivenWidth - proposedWidth)) {
            outerHeight = widthDrivenHeight;
        } else {
            outerWidth = heightDrivenWidth;
        }
    }

    const LONG left = sizingRect.left;
    const LONG top = sizingRect.top;
    const LONG right = sizingRect.right;
    const LONG bottom = sizingRect.bottom;
    switch (sizingEdge) {
    case WMSZ_LEFT:
        sizingRect.left = right - outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    case WMSZ_RIGHT:
        sizingRect.right = left + outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    case WMSZ_TOP:
        sizingRect.top = bottom - outerHeight;
        sizingRect.right = left + outerWidth;
        break;
    case WMSZ_BOTTOM:
        sizingRect.bottom = top + outerHeight;
        sizingRect.right = left + outerWidth;
        break;
    case WMSZ_TOPLEFT:
        sizingRect.left = right - outerWidth;
        sizingRect.top = bottom - outerHeight;
        break;
    case WMSZ_TOPRIGHT:
        sizingRect.right = left + outerWidth;
        sizingRect.top = bottom - outerHeight;
        break;
    case WMSZ_BOTTOMLEFT:
        sizingRect.left = right - outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    case WMSZ_BOTTOMRIGHT:
        sizingRect.right = left + outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    default:
        break;
    }
}

LRESULT BorderlessHitTest(const RECT& windowRect, POINT cursor,
                          int gripPixels, bool resizable) noexcept {
    if (!resizable) return HTCAPTION;
    const int grip = (std::max)(1, gripPixels);
    const bool left = cursor.x >= windowRect.left &&
                      cursor.x < windowRect.left + grip;
    const bool right = cursor.x < windowRect.right &&
                       cursor.x >= windowRect.right - grip;
    const bool top = cursor.y >= windowRect.top &&
                     cursor.y < windowRect.top + grip;
    const bool bottom = cursor.y < windowRect.bottom &&
                        cursor.y >= windowRect.bottom - grip;
    if (left && top) return HTTOPLEFT;
    if (right && top) return HTTOPRIGHT;
    if (left && bottom) return HTBOTTOMLEFT;
    if (right && bottom) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return HTCAPTION;
}

}  // namespace llcv::window_geometry
