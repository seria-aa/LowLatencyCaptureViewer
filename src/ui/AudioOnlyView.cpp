#include "ui/AudioOnlyView.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>

namespace llcv::audio_only_view {
namespace {
// Painting and hit-testing share the same full-card interaction bounds.
constexpr Rect kMaster{12, 38, 368, 100};
constexpr Rect kLeft{12, 107, 186, 195};
constexpr Rect kRight{194, 107, 368, 195};

Rect ScaleRect(const Rect& content, const Rect& logical) noexcept {
    const int width = content.right - content.left;
    const int height = content.bottom - content.top;
    return {content.left + MulDiv(logical.left, width, kBaseClientWidth),
            content.top + MulDiv(logical.top, height, kBaseClientHeight),
            content.left + MulDiv(logical.right, width, kBaseClientWidth),
            content.top + MulDiv(logical.bottom, height, kBaseClientHeight)};
}

bool Contains(const Rect& rect, int x, int y) noexcept {
    return x >= rect.left && x < rect.right &&
           y >= rect.top && y < rect.bottom;
}
}  // namespace

Size MinimumClientSize(UINT dpi) noexcept {
    const int scale = static_cast<int>(std::clamp(dpi, 96u, 768u));
    return {MulDiv(kBaseClientWidth, scale, 96),
            MulDiv(kBaseClientHeight, scale, 96)};
}

TextSizes TextSizesForContent(int contentHeight) noexcept {
    // The DPI-aware minimum window keeps text legible. Scale with the cards
    // even below that minimum (e.g. a work-area clamp) to avoid overlapping.
    const auto size = [&](int normal) {
        return (std::max)(1, MulDiv(normal, (std::max)(1, contentHeight),
                                   kBaseClientHeight));
    };
    return {size(32), size(25), size(14), size(12)};
}

Rect ContentRect(int clientWidth, int clientHeight) noexcept {
    if (clientWidth <= 0 || clientHeight <= 0) return {};
    const bool widthLimited =
        static_cast<int64_t>(clientWidth) * kBaseClientHeight <=
        static_cast<int64_t>(clientHeight) * kBaseClientWidth;
    const int scaleNumerator = widthLimited ? clientWidth : clientHeight;
    const int scaleDenominator = widthLimited ? kBaseClientWidth
                                              : kBaseClientHeight;
    const int width = static_cast<int>(
        (static_cast<int64_t>(kBaseClientWidth) * scaleNumerator +
         scaleDenominator / 2) / scaleDenominator);
    const int height = static_cast<int>(
        (static_cast<int64_t>(kBaseClientHeight) * scaleNumerator +
         scaleDenominator / 2) / scaleDenominator);
    const int left = (clientWidth - width) / 2;
    const int top = (clientHeight - height) / 2;
    return {left, top, left + width, top + height};
}

audio_osd::HitTarget HitTest(int clientWidth, int clientHeight,
                             int clientX, int clientY) noexcept {
    const Rect rect = ContentRect(clientWidth, clientHeight);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || clientX < rect.left ||
        clientX >= rect.right || clientY < rect.top || clientY >= rect.bottom) {
        return audio_osd::HitTarget::Outside;
    }
    if (Contains(ScaleRect(rect, kLeft), clientX, clientY))
        return audio_osd::HitTarget::Left;
    if (Contains(ScaleRect(rect, kRight), clientX, clientY))
        return audio_osd::HitTarget::Right;
    if (Contains(ScaleRect(rect, kMaster), clientX, clientY))
        return audio_osd::HitTarget::Master;
    return audio_osd::HitTarget::Panel;
}

Size FitClientSize(int desiredWidth, int maximumWidth,
                   int maximumHeight) noexcept {
    if (desiredWidth <= 0 || maximumWidth <= 0 || maximumHeight <= 0)
        return {};
    int width = (std::min)(desiredWidth, maximumWidth);
    int height = static_cast<int>(
        (static_cast<int64_t>(width) * kBaseClientHeight +
         kBaseClientWidth / 2) / kBaseClientWidth);
    if (height > maximumHeight) {
        width = static_cast<int>(
            (static_cast<int64_t>(maximumHeight) * kBaseClientWidth) /
            kBaseClientHeight);
        width = (std::max)(1, width);
        height = static_cast<int>(
            (static_cast<int64_t>(width) * kBaseClientHeight +
             kBaseClientWidth / 2) / kBaseClientWidth);
    }
    return {width, height};
}

void Paint(HDC dc, const Rect& content, const State& state) {
    if (!dc) return;
    const int width = content.right - content.left;
    const int height = content.bottom - content.top;
    if (width <= 0 || height <= 0) return;
    const int saved = SaveDC(dc);
    if (saved == 0) return;
    const auto x = [&](int value) {
        return content.left + MulDiv(value, width, kBaseClientWidth);
    };
    const auto y = [&](int value) {
        return content.top + MulDiv(value, height, kBaseClientHeight);
    };
    const auto rect = [&](int left, int top, int right, int bottom) {
        return RECT{x(left), y(top), x(right), y(bottom)};
    };

    constexpr COLORREF backgroundColor = RGB(12, 15, 19);
    constexpr COLORREF cardColor = RGB(23, 28, 34);
    constexpr COLORREF masterColor = RGB(30, 36, 44);
    constexpr COLORREF hoverColor = RGB(39, 49, 59);
    constexpr COLORREF textColor = RGB(237, 242, 245);
    constexpr COLORREF secondaryColor = RGB(162, 178, 188);
    constexpr COLORREF accentColor = RGB(129, 206, 186);
    constexpr COLORREF trackColor = RGB(58, 72, 79);
    constexpr COLORREF clipColor = RGB(237, 98, 84);
    constexpr COLORREF boostColor = RGB(226, 176, 117);

    HBRUSH background = CreateSolidBrush(backgroundColor);
    HBRUSH cardBrush = CreateSolidBrush(cardColor);
    HBRUSH hover = CreateSolidBrush(hoverColor);
    HBRUSH accent = CreateSolidBrush(accentColor);
    HBRUSH track = CreateSolidBrush(trackColor);
    HBRUSH master = CreateSolidBrush(masterColor);
    HBRUSH clipping = CreateSolidBrush(state.clipping ? clipColor : accentColor);
    HBRUSH outerEdge = CreateSolidBrush(RGB(59, 70, 80));
    HPEN cardEdge = CreatePen(PS_SOLID, 1, RGB(48, 58, 67));
    HPEN hoverEdge = CreatePen(PS_SOLID, 1, RGB(109, 155, 145));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, GetStockObject(NULL_PEN));

    const RECT backgroundRect{content.left, content.top,
                              content.right, content.bottom};
    FillRect(dc, &backgroundRect, background);
    FrameRect(dc, &backgroundRect, outerEdge);

    const auto font = [&](int pixelHeight, int weight) {
        LOGFONTW descriptor{};
        descriptor.lfHeight = -(std::max)(1, pixelHeight);
        descriptor.lfWeight = weight;
        wcscpy_s(descriptor.lfFaceName, L"Segoe UI");
        return CreateFontIndirectW(&descriptor);
    };
    const auto textSizes = TextSizesForContent(height);
    HFONT masterFont = font(textSizes.master, FW_MEDIUM);
    HFONT numberFont = font(textSizes.channel, FW_MEDIUM);
    HFONT bodyFont = font(textSizes.body, FW_NORMAL);
    HFONT smallFont = font(textSizes.secondary, FW_NORMAL);
    HGDIOBJ oldFont = SelectObject(dc, bodyFont);
    const auto label = [&](const wchar_t* value, int left, int top,
                           int right, int bottom, COLORREF color,
                           UINT alignment = DT_LEFT) {
        SetTextColor(dc, color);
        RECT bounds = rect(left, top, right, bottom);
        DrawTextW(dc, value, -1, &bounds,
                  alignment | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
    };
    const auto rounded = [&](HBRUSH brush, int left, int top,
                             int right, int bottom, int radius) {
        SelectObject(dc, brush);
        RoundRect(dc, x(left), y(top), x(right), y(bottom),
                  (std::max)(1, x(radius) - x(0)),
                  (std::max)(1, y(radius) - y(0)));
    };

    SelectObject(dc, bodyFont);
    label(state.english ? L"Audio" : L"오디오", 14, 10, 150, 32, textColor);
    SelectObject(dc, smallFont);
    label(state.outputLabel ? state.outputLabel : L"",
          152, 12, 366, 30, secondaryColor, DT_RIGHT);

    const auto card = [&](const Rect& bounds, int target, HBRUSH normal) {
        SelectObject(dc, state.hoveredTarget == target ? hoverEdge : cardEdge);
        rounded(state.hoveredTarget == target ? hover : normal,
                bounds.left, bounds.top, bounds.right, bounds.bottom, 16);
        SelectObject(dc, GetStockObject(NULL_PEN));
    };
    const auto percentage = [&](int percent, int left, int top,
                                 int right, int bottom, bool isMaster) {
        wchar_t value[16]{};
        swprintf_s(value, L"%d", std::clamp(percent, 0, 200));
        SelectObject(dc, isMaster ? masterFont : numberFont);
        const COLORREF valueColor = isMaster && state.allowBoost && percent > 100
            ? boostColor : textColor;
        label(value, left, top - 2, right - 15, bottom + 2, valueColor, DT_RIGHT);
        SelectObject(dc, smallFont);
        label(L"%", right - 12, bottom - 20, right, bottom - 2,
              secondaryColor, DT_RIGHT);
    };
    card(kMaster, 3, master);
    SelectObject(dc, bodyFont);
    if (state.allowBoost) {
        label(state.english ? L"Master volume" : L"마스터 음량",
              26, 46, 216, 69, textColor);
        SelectObject(dc, smallFont);
        label(state.english ? L"Up to 200%" : L"최대 200%",
              26, 70, 216, 89, secondaryColor);
    } else {
        label(state.english ? L"Master volume" : L"마스터 음량",
              26, 48, 216, 90, textColor);
    }
    percentage(state.masterPercent, 218, 48, 354, 90, true);

    const auto channel = [&](int target, const wchar_t* name, int percent,
                             double peakDb, const Rect& bounds) {
        const int left = bounds.left;
        const int right = bounds.right;
        card(bounds, target, cardBrush);
        SelectObject(dc, bodyFont);
        label(name, left + 12, 115, left + 40, 151, textColor);
        percentage(percent, left + 44, 115, right - 12, 151, false);
        const double validPeak = std::isfinite(peakDb)
            ? std::clamp(peakDb, -96.0, 0.0) : -96.0;
        wchar_t peak[48]{};
        swprintf_s(peak, L"%.1f dBFS", validPeak);
        SelectObject(dc, smallFont);
        label(state.english ? L"Output level" : L"출력 레벨",
              left + 12, 159, left + 82, 177, secondaryColor);
        label(peak, left + 82, 159, right - 12, 177, textColor, DT_RIGHT);
        rounded(track, left + 12, 181, right - 12, 187, 4);
        const double level = std::clamp((validPeak + 60.0) / 60.0, 0.0, 1.0);
        const int fillRight = left + 12 + static_cast<int>(
            std::lround((right - left - 24) * level));
        if (fillRight > left + 12)
            rounded(accent, left + 12, 181, fillRight, 187, 4);
    };
    channel(1, L"L", state.leftPercent, state.leftPeakDb, kLeft);
    channel(2, L"R", state.rightPercent, state.rightPeakDb, kRight);

    SelectObject(dc, clipping);
    Ellipse(dc, x(14), y(209), x(18), y(213));
    SelectObject(dc, smallFont);
    label(state.clipping ? (state.english ? L"Clipping" : L"클리핑")
                         : (state.english ? L"No clipping" : L"클리핑 없음"),
          23, 203, 117, 220,
          state.clipping ? clipColor : secondaryColor);
    label(state.english ? L"Wheel: volume · Double-click: reset"
                        : L"휠 조절 · 두 번 클릭 초기화",
          119, 203, 366, 220, secondaryColor, DT_RIGHT);

    SelectObject(dc, oldFont);
    RestoreDC(dc, saved);
    DeleteObject(smallFont);
    DeleteObject(hoverEdge);
    DeleteObject(cardEdge);
    DeleteObject(outerEdge);
    DeleteObject(bodyFont);
    DeleteObject(numberFont);
    DeleteObject(masterFont);
    DeleteObject(clipping);
    DeleteObject(master);
    DeleteObject(track);
    DeleteObject(accent);
    DeleteObject(cardBrush);
    DeleteObject(hover);
    DeleteObject(background);
}

}  // namespace llcv::audio_only_view
