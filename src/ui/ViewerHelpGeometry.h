#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace llcv::viewer_help::detail {
inline constexpr int kMinimumClientWidth=760, kMinimumClientHeight=640;

// Adapt only this guide, never Windows DPI or viewer/capture settings. Floor
// the fitted DPI so rounding cannot make its minimum layout exceed the client.
inline UINT ContentDpi(UINT windowDpi,int width,int height) {
    const auto fitted=[](int pixels,int dips) {
        return UINT(std::max<int64_t>(1,int64_t(std::max(1,pixels))*96/dips));
    };
    return std::min({windowDpi ? windowDpi : 96u,
        fitted(width,kMinimumClientWidth),fitted(height,kMinimumClientHeight)});
}
inline RECT FitToWorkArea(RECT rect,const RECT& work) {
    const LONG width=std::clamp(rect.right-rect.left,1L,std::max(1L,work.right-work.left));
    const LONG height=std::clamp(rect.bottom-rect.top,1L,std::max(1L,work.bottom-work.top));
    const LONG left=std::clamp(rect.left,work.left,std::max(work.left,work.right-width));
    const LONG top=std::clamp(rect.top,work.top,std::max(work.top,work.bottom-height));
    return {left,top,left+width,top+height};
}
}
