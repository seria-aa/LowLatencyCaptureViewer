#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace llcv::overlay_ui {

constexpr LONG Width(const RECT& rect) noexcept { return rect.right - rect.left; }
constexpr LONG Height(const RECT& rect) noexcept { return rect.bottom - rect.top; }
constexpr bool HasArea(const RECT& rect) noexcept { return Width(rect) > 0 && Height(rect) > 0; }
constexpr bool Overlaps(const RECT& a, const RECT& b) noexcept {
    return HasArea(a) && HasArea(b) && a.left < b.right && a.right > b.left &&
           a.top < b.bottom && a.bottom > b.top;
}
constexpr bool Fits(const RECT& rect, SIZE viewport) noexcept {
    return HasArea(rect) && rect.left >= 0 && rect.top >= 0 &&
           rect.right <= viewport.cx && rect.bottom <= viewport.cy;
}
constexpr RECT At(const RECT& rect, LONG x, LONG y) noexcept {
    return {x, y, x + Width(rect), y + Height(rect)};
}

// Keep the unscaled layout when it fits; on small clients shrink uniformly.
// Shrink the requested margin as well on exceptionally small clients.
constexpr RECT FitPanel(SIZE viewport, SIZE panel, LONG margin = 16,
                        bool right = false, bool bottom = false) noexcept {
    if (viewport.cx <= 0 || viewport.cy <= 0 || panel.cx <= 0 || panel.cy <= 0)
        return {};
    const LONG inset = (std::min)({margin, viewport.cx / 12, viewport.cy / 12});
    const LONG availableWidth = viewport.cx - 2 * inset;
    const LONG availableHeight = viewport.cy - 2 * inset;
    LONG width = (std::min)(panel.cx, availableWidth);
    LONG height = (std::min)(panel.cy, availableHeight);
    if (int64_t(width) * panel.cy <= int64_t(height) * panel.cx)
        height = (std::max)(1L, static_cast<LONG>(int64_t(width) * panel.cy / panel.cx));
    else
        width = (std::max)(1L, static_cast<LONG>(int64_t(height) * panel.cx / panel.cy));
    const LONG x = right ? viewport.cx - width - inset : inset;
    const LONG y = bottom ? viewport.cy - height - inset : inset;
    return {x, y, x + width, y + height};
}

// Recording controls stay in place: moving only the transient volume panel
// means the recording hit targets never move while the cursor is over them.
constexpr RECT ClearRecordingCard(const RECT& panel, const RECT& card, SIZE viewport,
                                  bool right, bool bottom) noexcept {
    if (!Overlaps(panel, card)) return panel;
    constexpr LONG gap = 8;
    const RECT above = At(panel, panel.left, card.top - gap - Height(panel));
    const RECT beside = At(panel, card.left - gap - Width(panel), panel.top);
    const RECT otherSide = At(panel, card.right + gap, panel.top);
    const RECT below = At(panel, panel.left, card.bottom + gap);
    for (const auto& candidate : {above, beside, otherSide, below})
        if (Fits(candidate, viewport)) return candidate;

    // If a full-width failure card covers a large part of a small viewport,
    // fit a smaller volume panel in whichever remaining space gives it the
    // most room. Never hide the recording status or cover its actions.
    const RECT free[]{{0, 0, viewport.cx, card.top - gap},
                      {0, 0, card.left - gap, viewport.cy},
                      {card.right + gap, 0, viewport.cx, viewport.cy},
                      {0, card.bottom + gap, viewport.cx, viewport.cy}};
    RECT best{};
    for (const auto& area : free) {
        if (!HasArea(area)) continue;
        const auto fitted = FitPanel({Width(area), Height(area)},
            {Width(panel), Height(panel)}, 0, right, bottom);
        if (int64_t(Width(fitted)) * Height(fitted) > int64_t(Width(best)) * Height(best))
            best = At(fitted, fitted.left + area.left, fitted.top + area.top);
    }
    return best;
}

} // namespace llcv::overlay_ui
