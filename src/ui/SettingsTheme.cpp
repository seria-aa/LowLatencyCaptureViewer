#include "SettingsTheme.h"
#include "SettingsView.h"
#include <algorithm>
#include <cwchar>
#include <dwmapi.h>
#include <initializer_list>
#include <string>
#include <uxtheme.h>

namespace llcv::settings_ui {
namespace {
constexpr UINT_PTR kSubclassId = 0x4c4c4354;
// Shared application chrome; video composition is deliberately independent.
constexpr int kCornerDiameterDip = 14;
int NavigationItemHeight(const SettingsControls& controls, UINT dpi) {
    const int normalHeight = SettingsPixels(44, dpi);
    if (!SettingsCompactLayout(&controls)) return normalHeight;
    RECT client{};
    GetClientRect(controls.tabControl, &client);
    return std::max(1, std::min(normalHeight, static_cast<int>(client.bottom) / kSettingsNavigationCount));
}
void Solid(HDC dc, const RECT& rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
void Rounded(HDC dc, RECT rect, COLORREF fill, COLORREF edge, int radius) {
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, edge);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}
void Line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color) {
    HGDIOBJ old = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, color);
    MoveToEx(dc, x1, y1, nullptr); LineTo(dc, x2, y2);
    SelectObject(dc, old);
}
template<class Paint>
void BufferedWindowPaint(HWND hwnd, bool available, Paint paint) {
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(hwnd, &ps);
    RECT client{}; GetClientRect(hwnd, &client);
    HDC bufferDc = nullptr;
    HPAINTBUFFER buffer = available && target
        ? BeginBufferedPaint(target, &client, BPBF_COMPATIBLEBITMAP, nullptr, &bufferDc)
        : nullptr;
    if (target) paint(buffer && bufferDc ? bufferDc : target);
    if (buffer) EndBufferedPaint(buffer, TRUE);
    EndPaint(hwnd, &ps);
}
}

SettingsTheme::~SettingsTheme() { Detach(); }
int SettingsTheme::Pixels(int dip) const { return MulDiv(dip, dpi_, 96); }
bool SettingsTheme::Attach(HWND owner, const SettingsControls* controls) {
    Detach();
    if (!IsWindow(owner) || !controls) return false;
    owner_ = owner;
    controls_ = controls;
    bufferedPaintInitialized_ = SUCCEEDED(BufferedPaintInit());
    UpdateSystemTheme();
    if (!background_ || !surface_ || !sidebar_ || !card_) { Detach(); return false; }
    RefreshControls();
    return true;
}
void SettingsTheme::Detach() {
    for (const auto& child : children_) {
        DWORD_PTR context = 0;
        if (IsWindow(child.hwnd) && GetWindowSubclass(child.hwnd, ChildProc, kSubclassId, &context) &&
            context == reinterpret_cast<DWORD_PTR>(this)) {
            RemoveWindowSubclass(child.hwnd, ChildProc, kSubclassId);
            InvalidateRect(child.hwnd, nullptr, TRUE);
        }
    }
    children_.clear();
    for (HBRUSH brush : { background_, surface_, sidebar_, card_ }) if (brush) DeleteObject(brush);
    background_ = surface_ = sidebar_ = card_ = nullptr;
    owner_ = nullptr;
    controls_ = nullptr;
    if (bufferedPaintInitialized_) BufferedPaintUnInit();
    bufferedPaintInitialized_ = false;
}
void SettingsTheme::UpdateSystemTheme() {
    const Palette previousPalette = palette_;
    const bool previousHighContrast = highContrast_;
    const bool light = controls_ && controls_->themeCombo &&
        SendMessageW(controls_->themeCombo, CB_GETCURSEL, 0, 0) == 1;
    palette_ = ui::PaletteForTheme(light);
    HIGHCONTRASTW hc{ sizeof(hc) };
    highContrast_ = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
        (hc.dwFlags & HCF_HIGHCONTRASTON);
    // Replace brushes only after all new allocations succeed, never during paint.
    HBRUSH bg = CreateSolidBrush(highContrast_ ? GetSysColor(COLOR_WINDOW) : palette_.kBackground);
    HBRUSH surface = CreateSolidBrush(highContrast_ ? GetSysColor(COLOR_WINDOW) : palette_.kControl);
    HBRUSH sidebar = CreateSolidBrush(highContrast_ ? GetSysColor(COLOR_WINDOW) : palette_.kSidebar);
    HBRUSH card = CreateSolidBrush(highContrast_ ? GetSysColor(COLOR_WINDOW) : palette_.kCard);
    if (bg && surface && sidebar && card) {
        for (HBRUSH brush : { background_, surface_, sidebar_, card_ }) if (brush) DeleteObject(brush);
        background_ = bg; surface_ = surface; sidebar_ = sidebar; card_ = card;
    } else {
        for (HBRUSH brush : { bg, surface, sidebar, card }) if (brush) DeleteObject(brush);
        palette_ = previousPalette;
        highContrast_ = previousHighContrast;
        return;
    }
    const BOOL dark = !highContrast_ && !light;
    DwmSetWindowAttribute(owner_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // Windows versions lacking these documented attributes simply ignore them.
    const COLORREF caption = highContrast_ ? DWMWA_COLOR_DEFAULT : palette_.kBackground;
    const COLORREF text = highContrast_ ? DWMWA_COLOR_DEFAULT : palette_.kText;
    DwmSetWindowAttribute(owner_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(owner_, DWMWA_TEXT_COLOR, &text, sizeof(text));
}
BOOL CALLBACK SettingsTheme::AttachChild(HWND child, LPARAM parameter) {
    auto& theme = *reinterpret_cast<SettingsTheme*>(parameter);
    // EnumChildWindows includes nested combo children; decorate only controls
    // owned directly by this dialog, leaving popup list/edit behavior native.
    if (GetParent(child) != theme.owner_) return TRUE;
    wchar_t name[32]{}; GetClassNameW(child, name, 32);
    const bool combo = _wcsicmp(name, L"COMBOBOX") == 0;
    const bool button = _wcsicmp(name, L"BUTTON") == 0;
    // Disabled native STATIC text is embossed for both labels and wrapping
    // descriptions. Cover every text layout used by settings, never image,
    // frame or owner-drawn statics. Enabled controls still use native painting.
    const DWORD staticType = static_cast<DWORD>(GetWindowLongPtrW(child, GWL_STYLE)) & SS_TYPEMASK;
    const bool label = _wcsicmp(name, L"STATIC") == 0 &&
        (staticType == SS_LEFT || staticType == SS_CENTER || staticType == SS_RIGHT ||
         staticType == SS_LEFTNOWORDWRAP);
    if (!combo && !button && !label) return TRUE;
    if (combo && (GetWindowLongPtrW(child, GWL_STYLE) & 3) != CBS_DROPDOWNLIST) return TRUE;
    if (button && (GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW) return TRUE;
    const auto existing = std::find_if(theme.children_.begin(), theme.children_.end(),
        [child](const Child& item) { return item.hwnd == child; });
    if (existing == theme.children_.end() &&
        SetWindowSubclass(child, ChildProc, kSubclassId, reinterpret_cast<DWORD_PTR>(&theme))) {
        theme.children_.push_back({child, combo, false, label});
    }
    return TRUE;
}
void SettingsTheme::RefreshControls(UINT layoutDpi) {
    if (!IsWindow(owner_)) return;
    dpi_ = layoutDpi ? layoutDpi : GetDpiForWindow(owner_);
    if (!dpi_) dpi_ = 96;
    EnumChildWindows(owner_, AttachChild, reinterpret_cast<LPARAM>(this));
    if (controls_->tabControl)
        SendMessageW(controls_->tabControl, LB_SETITEMHEIGHT, 0, NavigationItemHeight(*controls_, dpi_));
    if (controls_->tooltipWindow) {
        SendMessageW(controls_->tooltipWindow, TTM_SETTIPBKCOLOR,
            highContrast_ ? GetSysColor(COLOR_INFOBK) : palette_.kControl, 0);
        SendMessageW(controls_->tooltipWindow, TTM_SETTIPTEXTCOLOR,
            highContrast_ ? GetSysColor(COLOR_INFOTEXT) : palette_.kText, 0);
    }
    RedrawWindow(owner_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}
bool SettingsTheme::IsHeading(HWND hwnd) const {
    const auto& c = *controls_;
#ifdef LLCV_RECORDING
    if(hwnd==c.recVideoTitle||hwnd==c.recSaveTitle)return true;
#endif
    return hwnd == c.brandLabel || hwnd == c.pageTitle || hwnd == c.audioOutputSection ||
        hwnd == c.audioPlaybackSection || hwnd == c.audioStabilitySection ||
        hwnd == c.videoCaptureSection || hwnd == c.videoDisplaySection ||
        hwnd == c.videoWindowSection || hwnd == c.screenshotTitle ||
        hwnd == c.guideShortcutsTitle || hwnd == c.guideDiagnosticsTitle ||
        hwnd == c.updateTitle || hwnd == c.appPreferencesSection;
}
bool SettingsTheme::IsSecondary(HWND hwnd) const {
    const auto& c = *controls_;
#ifdef LLCV_RECORDING
    if(hwnd==c.recStatus||hwnd==c.recSpace||hwnd==c.recNote||hwnd==c.recSplitHint)return true;
#endif
    return hwnd == c.pageSubtitle || hwnd == c.videoCapabilityStatus || hwnd == c.captureAudioStatus ||
        hwnd == c.vsrStatus || hwnd == c.screenshotHelp || hwnd == c.skipStartupHint || hwnd == c.frameOptionsHint ||
        hwnd == c.preventSleepHint ||
        hwnd == c.versionWatermark || hwnd == c.fullscreenCursorHint || hwnd == c.fullscreenPositionHint || hwnd == c.surround51Hint ||
        hwnd == c.relativeSizeWarning || hwnd == c.guideDiagnosticsText || hwnd == c.guideVideoHint || hwnd == c.updateText ||
        hwnd == c.updateStatus || hwnd == c.audioStatus;
}
bool SettingsTheme::IsCardControl(HWND hwnd) const {
    const auto& c = *controls_;
    // Decoration follows the same functional grouping as the native controls;
    // it is never a child window and therefore cannot cover/capture input.
    return hwnd && ((c.frameOptionsOpen &&
        (hwnd == c.frameOptionsTitle || hwnd == c.frameOptionsClose || hwnd == c.frameOptionsHint ||
         hwnd == c.unlistedFpsCheck || hwnd == c.capture40Check || hwnd == c.capture40Help)) ||
        hwnd == c.vsrCheck || hwnd == c.vsrGuideButton || hwnd == c.vsrStatus ||
        hwnd == c.displayResolutionLabel || hwnd == c.displayResolutionCombo ||
        hwnd == c.screenshotTitle || hwnd == c.screenshotClipboardCheck ||
        hwnd == c.screenshotFolderButton || hwnd == c.screenshotHelp ||
        hwnd == c.guideShortcutsTitle || hwnd == c.shortcutResetButton || hwnd == c.guideVideoHint ||
        std::find(c.guideKeys.begin(), c.guideKeys.end(), hwnd) != c.guideKeys.end() ||
        std::find(c.guideDescriptions.begin(), c.guideDescriptions.end(), hwnd) != c.guideDescriptions.end());
}
void SettingsTheme::PaintCards(HDC dc) {
    const auto paintGroup = [&](const std::vector<HWND>& children, int padding = 8) {
        RECT bounds{};
        bool found = false;
        for (HWND child : children) {
            // WS_VISIBLE works for offscreen QA too (IsWindowVisible also checks
            // the hidden owner). Union actual layout rather than duplicate it.
            if (!child || !(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
            RECT rect{}; GetWindowRect(child, &rect);
            MapWindowPoints(HWND_DESKTOP, owner_, reinterpret_cast<POINT*>(&rect), 2);
            if (found) UnionRect(&bounds, &bounds, &rect);
            else { bounds = rect; found = true; }
        }
        if (found) {
            InflateRect(&bounds, Pixels(padding), Pixels(padding));
            Rounded(dc, bounds, palette_.kCard, palette_.kCardEdge, Pixels(kCornerDiameterDip));
        }
    };
    const auto& c = *controls_;
    if (c.frameOptionsOpen) {
        // Share the actual padded control bounds with outside-click handling.
        const RECT bounds = SettingsFrameOptionsBounds(&c, owner_);
        if (!IsRectEmpty(&bounds))
            Rounded(dc, bounds, palette_.kCard, palette_.kCardEdge, Pixels(kCornerDiameterDip));
    }
    paintGroup({c.vsrCheck, c.vsrGuideButton, c.displayResolutionLabel, c.displayResolutionCombo, c.vsrStatus});
    paintGroup({c.screenshotTitle, c.screenshotClipboardCheck, c.screenshotFolderButton, c.screenshotHelp});
    if (SettingsCompactLayout(&c)) {
        std::vector<HWND> shortcuts{c.guideVideoHint,c.shortcutResetButton};
        shortcuts.insert(shortcuts.end(),c.guideKeys.begin(),c.guideKeys.end());
        shortcuts.insert(shortcuts.end(),c.guideDescriptions.begin(),c.guideDescriptions.end());
        paintGroup(shortcuts);
    } else paintGroup({c.guideShortcutsTitle, c.guideVideoHint}, 20);
}
void SettingsTheme::PaintBackground(HDC dc) {
    RECT client{}; GetClientRect(owner_, &client);
    FillRect(dc, &client, background_);
    if (highContrast_) return;
    RECT sidebar = client; sidebar.right = std::min(client.right, LONG(Pixels(controls_->sidebarWidthDip)));
    FillRect(dc, &sidebar, sidebar_);
    Line(dc, sidebar.right, 0, sidebar.right, client.bottom, palette_.kCardEdge);
    // The footer is constant across tabs; the content itself stays uncluttered.
    const int footer = Pixels(controls_->footerTopDip);
    if (client.bottom > footer) Line(dc, sidebar.right, footer, client.right, footer, palette_.kCardEdge);
    PaintCards(dc);
}
void SettingsTheme::PaintControl(HWND hwnd, HDC dc, bool combo, bool hover) {
    const int saved = SaveDC(dc);
    RECT rect{}; GetClientRect(hwnd, &rect);
    Solid(dc, rect, IsCardControl(hwnd) ? palette_.kCard : palette_.kBackground);
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool focus = GetFocus() == hwnd;
    const bool hideFocus = (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
    const bool hideAccel = (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) != 0;
    const auto font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    if (font) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    COLORREF textColor = enabled ? palette_.kText : palette_.kDisabled;
    const COLORREF edge = focus && enabled ? palette_.kAccent :
        hover && enabled ? palette_.kHoverEdge : palette_.kEdge;
    wchar_t text[2048]{}; GetWindowTextW(hwnd, text, 2048);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    RECT label = rect;
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
    if (hideAccel) flags |= DT_HIDEPREFIX;
    if (combo) {
        Rounded(dc, rect, !enabled ? palette_.kControlDisabled : hover ? palette_.kHover : palette_.kControl,
            edge, Pixels(kCornerDiameterDip));
        label.left += Pixels(11); label.right -= Pixels(31);
        const int x = rect.right - Pixels(16), y = (rect.bottom + rect.top) / 2;
        const COLORREF arrow = enabled ? palette_.kSecondary : palette_.kDisabled;
        Line(dc, x - Pixels(4), y - Pixels(2), x, y + Pixels(2), arrow);
        Line(dc, x, y + Pixels(2), x + Pixels(4) + 1, y - Pixels(2) - 1, arrow);
        flags |= DT_NOPREFIX;
    } else {
        const UINT type = style & BS_TYPEMASK;
        const bool check = type == BS_AUTOCHECKBOX || type == BS_CHECKBOX ||
            type == BS_AUTO3STATE || type == BS_3STATE;
        if (check) {
            const int side = Pixels(17), y = (rect.bottom - side) / 2;
            RECT box{Pixels(1), y, Pixels(1) + side, y + side};
            const LRESULT checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0);
            const COLORREF boxColor = checked && enabled ? palette_.kAccent :
                !enabled ? palette_.kControlDisabled : hover ? palette_.kHover : palette_.kControl;
            Rounded(dc, box, boxColor, checked && enabled && !focus ? palette_.kAccent : edge, Pixels(6));
            if (checked == BST_CHECKED) {
                const COLORREF tick = enabled ? palette_.kBackground : palette_.kSecondary;
                // Parallel strokes give a crisp two-pixel mark without owning pens.
                for (int n = 0; n < std::max(1, Pixels(2)); ++n) {
                    Line(dc, box.left + Pixels(4), y + Pixels(8) + n,
                        box.left + Pixels(7), y + Pixels(11) + n, tick);
                    Line(dc, box.left + Pixels(7), y + Pixels(11) + n,
                        box.left + Pixels(13), y + Pixels(5) + n, tick);
                }
            } else if (checked == BST_INDETERMINATE) {
                RECT dash{box.left + Pixels(4), y + Pixels(7), box.right - Pixels(4), y + Pixels(10)};
                Solid(dc, dash, enabled ? palette_.kBackground : palette_.kSecondary);
            }
            label.left += Pixels(27);
            if (style & BS_MULTILINE) {
                flags &= ~(DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                flags |= DT_WORDBREAK;
                RECT measured = label;
                DrawTextW(dc, text, -1, &measured, flags | DT_CALCRECT);
                label.top += std::max(0L, ((label.bottom - label.top) - (measured.bottom - measured.top)) / 2);
            }
        } else {
            const bool primary = hwnd == controls_->startButton;
            const bool pressed = (SendMessageW(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
            COLORREF fill = !enabled ? palette_.kControlDisabled : primary ? palette_.kAccent : palette_.kControl;
            if (enabled && hover) fill = primary ? palette_.kPrimaryHover : palette_.kHover;
            if (enabled && pressed) fill = primary ? palette_.kPrimaryPressed : palette_.kCard;
            Rounded(dc, rect, fill, primary && enabled ? fill : edge, Pixels(kCornerDiameterDip));
            if (enabled && primary) textColor = palette_.kBackground;
            label.left += Pixels(7); label.right -= Pixels(7);
            if (pressed) OffsetRect(&label, 0, Pixels(1));
            const bool shortcut=std::find(controls_->guideKeys.begin(),controls_->guideKeys.end(),hwnd)!=controls_->guideKeys.end();
            if (shortcut) {
                label.left+=Pixels(3); flags|=DT_LEFT;
            }
            else flags |= DT_CENTER;
        }
    }
    SetTextColor(dc, textColor);
    DrawTextW(dc, text, -1, &label, flags);
    if (focus && enabled && !hideFocus) {
        RECT focusRect = rect; InflateRect(&focusRect, -Pixels(3), -Pixels(3));
        SetTextColor(dc, hwnd == controls_->startButton ? palette_.kBackground : palette_.kAccent);
        SetBkColor(dc, hwnd == controls_->startButton ? palette_.kAccent : palette_.kControl);
        DrawFocusRect(dc, &focusRect);
    }
    RestoreDC(dc, saved);
}
void SettingsTheme::PaintBrand(HWND hwnd, HDC dc) {
    RECT client{}; GetClientRect(hwnd, &client);
    Solid(dc, client, palette_.kSidebar);
    HFONT base = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    LOGFONTW description{};
    HFONT wordmark = nullptr;
    if (base && GetObjectW(base, sizeof(description), &description)) {
        description.lfHeight = -Pixels(18);
        description.lfWeight = FW_SEMIBOLD;
        wordmark = CreateFontIndirectW(&description);
    }
    HGDIOBJ previousFont = SelectObject(dc, wordmark ? wordmark : base);
    SIZE textSize{}; GetTextExtentPoint32W(dc, L"LLCV", 4, &textSize);
    const int groupLeft = static_cast<int>(std::max(0L, (client.right - Pixels(48) - textSize.cx) / 2));
    // Same capture-frame / signal geometry as tools/make-icon.ps1. Render
    // the emblem supersampled so small fractional-DPI strokes stay smooth.
    const int size = Pixels(40), sample = size * 3;
    HDC iconDc = CreateCompatibleDC(dc);
    HBITMAP bitmap = iconDc ? CreateCompatibleBitmap(dc, sample, sample) : nullptr;
    if (bitmap) {
        HGDIOBJ previousBitmap = SelectObject(iconDc, bitmap);
        RECT bounds{0, 0, sample, sample}; Solid(iconDc, bounds, palette_.kSidebar);
        const auto p = [sample](int n) { return MulDiv(n, sample, 256); };
        Rounded(iconDc, RECT{p(12), p(12), p(244), p(244)},
            RGB(32, 32, 33), RGB(32, 32, 33), p(96));
        const auto stroke = [&](const POINT* points, int count, COLORREF color, int width) {
            const LOGBRUSH brush{BS_SOLID, color, 0};
            HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                static_cast<DWORD>(std::max(1, p(width))), &brush, 0, nullptr);
            if (!pen) return;
            HGDIOBJ previous = SelectObject(iconDc, pen);
            Polyline(iconDc, points, count);
            SelectObject(iconDc, previous); DeleteObject(pen);
        };
        const POINT frame[][3] = {
            {{p(76),p(88)}, {p(76),p(72)}, {p(112),p(72)}},
            {{p(180),p(88)}, {p(180),p(72)}, {p(144),p(72)}},
            {{p(76),p(168)}, {p(76),p(184)}, {p(112),p(184)}},
            {{p(180),p(168)}, {p(180),p(184)}, {p(144),p(184)}}};
        for (const auto& corner : frame) stroke(corner, 3, RGB(242,239,232), 15);
        const POINT signal[] = {{p(84),p(128)}, {p(109),p(128)}, {p(124),p(97)},
            {p(144),p(159)}, {p(157),p(128)}, {p(176),p(128)}};
        stroke(signal, 6, RGB(255,111,82), 14);
        const int previousMode = SetStretchBltMode(dc, HALFTONE);
        POINT origin{}; SetBrushOrgEx(dc, 0, 0, &origin);
        StretchBlt(dc, groupLeft, (client.bottom - size) / 2, size, size,
            iconDc, 0, 0, sample, sample, SRCCOPY);
        SetBrushOrgEx(dc, origin.x, origin.y, nullptr);
        SetStretchBltMode(dc, previousMode);
        SelectObject(iconDc, previousBitmap); DeleteObject(bitmap);
    }
    if (iconDc) DeleteDC(iconDc);
    const int previousBk = SetBkMode(dc, TRANSPARENT);
    const COLORREF previousColor = SetTextColor(dc, palette_.kText);
    RECT text{groupLeft + Pixels(48), 0, client.right, client.bottom};
    DrawTextW(dc, L"LLCV", -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, previousColor); SetBkMode(dc, previousBk);
    SelectObject(dc, previousFont);
    if (wordmark) DeleteObject(wordmark);
}

void SettingsTheme::PaintLabel(HWND hwnd, HDC dc) {
    const int saved = SaveDC(dc);
    RECT rect{}; GetClientRect(hwnd, &rect);
    LRESULT brush = 0;
    HandleMessage(WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc),
        reinterpret_cast<LPARAM>(hwnd), brush);
    // Native disabled STATIC text adds system-colored embossed highlights,
    // overriding our flat theme. Fill the actual surface and draw once instead.
    FillRect(dc, &rect, reinterpret_cast<HBRUSH>(brush));
    const auto font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    if (font) SelectObject(dc, font);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD type = style & SS_TYPEMASK;
    UINT flags = DT_LEFT | DT_EXPANDTABS;
    if (type == SS_CENTER) flags |= DT_CENTER;
    else if (type == SS_RIGHT) flags |= DT_RIGHT;
    // Preserve explicit newlines and native wrapping instead of flattening
    // disabled explanations into a single line. Center-image is single-line.
    if (type != SS_LEFTNOWORDWRAP && !(style & SS_CENTERIMAGE)) flags |= DT_WORDBREAK;
    if (style & SS_CENTERIMAGE) flags |= DT_SINGLELINE | DT_VCENTER;
    if (style & SS_EDITCONTROL) flags |= DT_EDITCONTROL;
    const DWORD ellipsis = style & SS_ELLIPSISMASK;
    if (ellipsis) {
        flags = (flags & ~DT_WORDBREAK) | DT_SINGLELINE;
        flags |= ellipsis == SS_PATHELLIPSIS ? DT_PATH_ELLIPSIS :
                 ellipsis == SS_WORDELLIPSIS ? DT_WORD_ELLIPSIS : DT_END_ELLIPSIS;
    }
    if (style & SS_NOPREFIX) flags |= DT_NOPREFIX;
    else if (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) flags |= DT_HIDEPREFIX;
    // Descriptions can be longer than ordinary labels; keep their full text.
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
    if (std::find(controls_->guideDescriptions.begin(),controls_->guideDescriptions.end(),hwnd)!=
            controls_->guideDescriptions.end()) {
        RECT measured=rect;
        DrawTextW(dc,text.c_str(),-1,&measured,flags|DT_CALCRECT);
        rect.top+=std::max(0L,((rect.bottom-rect.top)-(measured.bottom-measured.top))/2);
    }
    DrawTextW(dc, text.c_str(), -1, &rect, flags);
    RestoreDC(dc, saved);
}
void SettingsTheme::PaintComboItem(const DRAWITEMSTRUCT& item) {
    const int saved = SaveDC(item.hDC);
    IntersectClipRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
    const bool field = (item.itemState & ODS_COMBOBOXEDIT) != 0;
    const bool disabled = !IsWindowEnabled(item.hwndItem) || (item.itemState & ODS_DISABLED);
    const bool selected = (item.itemState & ODS_SELECTED) && !disabled && (!field || highContrast_);
    const COLORREF background = selected ? GetSysColor(COLOR_HIGHLIGHT) : highContrast_
        ? GetSysColor(COLOR_WINDOW) : disabled ? palette_.kControlDisabled : palette_.kControl;
    const COLORREF foreground = disabled ? (highContrast_ ? GetSysColor(COLOR_GRAYTEXT) : palette_.kDisabled)
        : selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : highContrast_ ? GetSysColor(COLOR_WINDOWTEXT) : palette_.kText;
    Solid(item.hDC, item.rcItem, background);
    SetBkColor(item.hDC, background); SetTextColor(item.hDC, foreground); SetBkMode(item.hDC, TRANSPARENT);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    if (font) SelectObject(item.hDC, font);
    if (item.itemID != static_cast<UINT>(-1)) {
        const LRESULT length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
        if (length >= 0) {
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            if (SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(text.data())) != CB_ERR) {
                RECT label = item.rcItem;
                label.left = field ? std::max(label.left, LONG(Pixels(11))) : label.left + Pixels(11);
                label.right -= Pixels(8);
                DrawTextW(item.hDC, text.c_str(), -1, &label,
                    DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
        }
    }
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT) && !disabled)
        DrawFocusRect(item.hDC, &item.rcItem);
    RestoreDC(item.hDC, saved);
}
void SettingsTheme::PaintNavigation(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1)) return;
    const int saved = SaveDC(item.hDC);
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    COLORREF background = highContrast_ ? GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW) : palette_.kSidebar;
    Solid(item.hDC, item.rcItem, background);
    RECT box = item.rcItem;
    InflateRect(&box, 0, -Pixels(3));
    if (selected && !highContrast_) Rounded(item.hDC, box, palette_.kSelected, palette_.kSelected, Pixels(kCornerDiameterDip));
    COLORREF text = highContrast_ ? GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT) :
        selected ? palette_.kAccent : palette_.kSecondary;
    SetTextColor(item.hDC, text); SetBkMode(item.hDC, TRANSPARENT);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    if (font) SelectObject(item.hDC, font);
    wchar_t label[256]{};
    const LRESULT length = SendMessageW(item.hwndItem, LB_GETTEXTLEN, item.itemID, 0);
    if (length >= 0 && length < 256) SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(label));
    RECT textRect = box; textRect.left += Pixels(17); textRect.right -= Pixels(8);
    DrawTextW(item.hDC, label, -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        InflateRect(&box, -2, -2); DrawFocusRect(item.hDC, &box);
    }
    RestoreDC(item.hDC, saved);
}
bool SettingsTheme::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    if (!owner_) return false;
    switch (message) {
    case WM_COMMAND:
        if (controls_->themeCombo && reinterpret_cast<HWND>(lParam) == controls_->themeCombo &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            SettingsVisualUpdate update(owner_);
            UpdateSystemTheme();
            RefreshControls(dpi_);
            result = 0; return true;
        }
        break;
    case WM_ERASEBKGND:
        // WM_PAINT covers the background. Avoid a second intermediate erase
        // frame when native fields change visibility or selection.
        result = 1; return true;
    case WM_PAINT: {
        BufferedWindowPaint(owner_, bufferedPaintInitialized_, [this](HDC dc) { PaintBackground(dc); });
        result = 0; return true;
    }
    case WM_PRINTCLIENT:
        PaintBackground(reinterpret_cast<HDC>(wParam)); result = 0; return true;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item && item->CtlType == ODT_COMBOBOX && GetParent(item->hwndItem) == owner_) {
            PaintComboItem(*item); result = TRUE; return true;
        }
        if (item && item->CtlType == ODT_LISTBOX && item->hwndItem == controls_->tabControl) {
            PaintNavigation(*item); result = TRUE; return true;
        }
        break;
    }
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (item && item->CtlType == ODT_COMBOBOX) {
            item->itemHeight = Pixels(24); result = TRUE; return true;
        }
        if (item && item->CtlType == ODT_LISTBOX &&
            static_cast<int>(item->CtlID) == GetDlgCtrlID(controls_->tabControl)) {
            item->itemHeight = Pixels(44); result = TRUE; return true;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const HWND child = reinterpret_cast<HWND>(lParam);
        const HDC dc = reinterpret_cast<HDC>(wParam);
        if (highContrast_) {
            SetTextColor(dc, GetSysColor(IsWindowEnabled(child) ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW)); SetBkMode(dc, TRANSPARENT);
            result = reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW)); return true;
        }
        const bool nav = child == controls_->tabControl || child == controls_->brandLabel || child == controls_->versionWatermark;
        bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        wchar_t name[32]{}; GetClassNameW(child, name, 32);
        if (_wcsicmp(name, L"COMBOBOX") == 0) field = true;
        const bool key = child && std::find(controls_->guideKeys.begin(), controls_->guideKeys.end(), child) != controls_->guideKeys.end();
        if (key) field = true;
        const bool card = IsCardControl(child);
        const COLORREF bg = nav ? palette_.kSidebar : field ? palette_.kControl : card ? palette_.kCard : palette_.kBackground;
        const COLORREF fg = !IsWindowEnabled(child) ? palette_.kDisabled :
            (key || child == controls_->unlistedFpsStatus) ? palette_.kAccent : IsHeading(child) ? palette_.kText :
            IsSecondary(child) ? palette_.kSecondary : palette_.kText;
        SetTextColor(dc, fg); SetBkColor(dc, bg); SetBkMode(dc, TRANSPARENT);
        result = reinterpret_cast<LRESULT>(nav ? sidebar_ : field ? surface_ : card ? card_ : background_);
        return true;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        UpdateSystemTheme(); RefreshControls(); break;
    case WM_DPICHANGED:
        dpi_ = HIWORD(wParam) ? HIWORD(wParam) : 96;
        if (controls_->tabControl)
            SendMessageW(controls_->tabControl, LB_SETITEMHEIGHT, 0, NavigationItemHeight(*controls_, dpi_));
        break;
    case WM_NCDESTROY:
        Detach(); break;
    }
    return false;
}
LRESULT CALLBACK SettingsTheme::ChildProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR, DWORD_PTR reference) {
    auto& theme = *reinterpret_cast<SettingsTheme*>(reference);
    auto found = std::find_if(theme.children_.begin(), theme.children_.end(),
        [hwnd](const Child& item) { return item.hwnd == hwnd; });
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ChildProc, kSubclassId);
        if (found != theme.children_.end()) theme.children_.erase(found);
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }
    if (found == theme.children_.end() || theme.highContrast_) return DefSubclassProc(hwnd, message, wParam, lParam);
    if (hwnd == theme.controls_->brandLabel && IsWindowEnabled(hwnd)) {
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT) {
            BufferedWindowPaint(hwnd, theme.bufferedPaintInitialized_,
                [&](HDC dc) { theme.PaintBrand(hwnd, dc); });
            return 0;
        }
        if (message == WM_PRINTCLIENT) {
            theme.PaintBrand(hwnd, reinterpret_cast<HDC>(wParam)); return 0;
        }
    }
    if (found->label) {
        if (!IsWindowEnabled(hwnd) ||
            std::find(theme.controls_->guideDescriptions.begin(),theme.controls_->guideDescriptions.end(),hwnd)!=
                theme.controls_->guideDescriptions.end()) {
            if (message == WM_ERASEBKGND) return 1;
            if (message == WM_PAINT) {
                BufferedWindowPaint(hwnd, theme.bufferedPaintInitialized_,
                    [&](HDC dc) { theme.PaintLabel(hwnd, dc); });
                return 0;
            }
            if (message == WM_PRINTCLIENT) {
                theme.PaintLabel(hwnd, reinterpret_cast<HDC>(wParam));
                return 0;
            }
        }
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (message == WM_ENABLE || message == WM_SETFONT || message == WM_SETTEXT ||
            message == WM_UPDATEUISTATE)
            if (IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        return result; // labels never acquire hover, focus or click behavior
    }
    const bool combo = found->combo;
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        const bool hover = found->hover;
        BufferedWindowPaint(hwnd, theme.bufferedPaintInitialized_,
            [&](HDC dc) { theme.PaintControl(hwnd, dc, combo, hover); });
        return 0;
    }
    case WM_PRINTCLIENT:
        theme.PaintControl(hwnd, reinterpret_cast<HDC>(wParam), combo, found->hover); return 0;
    case WM_MOUSEMOVE:
        if (!found->hover) {
            found->hover = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&track);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (found->hover) { found->hover = false; InvalidateRect(hwnd, nullptr, FALSE); }
        break;
    case WM_SHOWWINDOW:
        if (!wParam) found->hover = false;
        break;
    case WM_ENABLE:
        if (!wParam) found->hover = false;
        break;
    }
    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    // Coalesce nested native state notifications into one queued paint. Forcing
    // UpdateWindow here painted the same click several times, mid-transition.
    switch (message) {
    case WM_ENABLE: case WM_SETFOCUS: case WM_KILLFOCUS: case WM_UPDATEUISTATE:
    case WM_SETFONT: case WM_SETTEXT: case BM_SETCHECK: case BM_SETSTATE:
    case CB_SETCURSEL: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_KEYDOWN: case WM_KEYUP: case WM_CAPTURECHANGED:
        if (IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        break;
    }
    return result;
}
} // namespace llcv::settings_ui
