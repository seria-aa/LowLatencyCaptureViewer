#include "SettingsView.h"
#include "SettingsDialogControls.h"
#ifdef LLCV_RECORDING
#include "recording/RecordingSettingsView.h"
#endif
#include "SettingsFonts.h"
#include "PresentationModeUi.h"
#include "settings/AppSettings.h"

#include <commctrl.h>
#include <algorithm>
#include <cwchar>

namespace llcv::settings_ui {
using settings::VideoPixelFormat;

void SetSettingsText(HWND control, const wchar_t* text) {
    if (!control) return;
    wchar_t current[2048]{};
    GetWindowTextW(control, current, ARRAYSIZE(current));
    if (std::wcscmp(current, text) != 0) SetWindowTextW(control, text);
}

// Anchor the actual detected-mode text, not a fixed-height empty rectangle,
// above the footer. The optional direct-request line shares the same block.
static void LayoutVideoStatus(SettingsControls* state, UINT dpi) {
    if (!state || SettingsCompactLayout(state) || !state->videoCapabilityStatus) return;
    const int extra=GetWindowTextLengthW(state->unlistedFpsStatus)>0 ? 20 : 0;
    int height=76;
    HDC dc=GetDC(state->videoCapabilityStatus);
    if(dc) {
        const auto previous=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(state->videoCapabilityStatus,WM_GETFONT,0,0)));
        wchar_t text[4096]{}; GetWindowTextW(state->videoCapabilityStatus,text,ARRAYSIZE(text));
        RECT bounds{0,0,SettingsPixels(376,dpi),0};
        DrawTextW(dc,text,-1,&bounds,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        height=static_cast<int>((bounds.bottom*96+dpi-1)/dpi);
        SelectObject(dc,previous); ReleaseDC(state->videoCapabilityStatus,dc);
    }
    height=std::clamp(height,16,94-extra);
    const int bottom=state->footerTopDip-24;
    PlaceSettingsControl(state->videoCapabilityStatus,184,bottom-extra-height,376,height,dpi);
    PlaceSettingsControl(state->unlistedFpsStatus,184,bottom-16,376,16,dpi);
}

void RefreshShortcutCaptions(SettingsControls* state) {
    for (size_t i=0;i<state->guideKeys.size();++i)
        SetSettingsText(state->guideKeys[i],viewer_help::KeyLabel(i,state->shortcutBindings).c_str());
    SetSettingsText(state->guideVideoHint,state->english
        ? L"Click a key to change\n* Video mode only"
        : L"키 버튼을 눌러 변경\n* 영상 모드 전용");
    for (const auto& [control,source]:state->shortcutCaptionTemplates)
        SetSettingsText(control,shortcuts::Expand(source.c_str(),state->shortcutBindings).c_str());
    SetSettingsText(state->vsrStatus, SendMessageW(state->upscalerCombo,CB_GETCURSEL,0,0)==1
        ? (state->english ? L"SDR only" : L"SDR 전용")
        : shortcuts::Expand(L"SDR / HDR10 · VSR: F6",state->shortcutBindings).c_str());
    if (!state->uiFonts.empty()) LayoutSettingsControls(state,state->layoutDpi);
}
ShortcutLayout MeasureShortcutLayout(const SettingsControls* state,UINT dpi,int rowWidth) {
    ShortcutLayout result;
    dpi=dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    HDC dc=GetDC(nullptr);
    if (!dc) return result;
    const auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(state->guideKeys[0],WM_GETFONT,0,0)));
    for (unsigned binding:state->shortcutBindings) {
        const auto label=shortcuts::Label(binding);
        SIZE size{}; GetTextExtentPoint32W(dc,label.c_str(),static_cast<int>(label.size()),&size);
        result.keyWidth=std::max(result.keyWidth,
            static_cast<int>(((size.cx+SettingsPixels(19,dpi))*96+dpi-1)/dpi));
    }
    SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(state->guideVideoHint,WM_GETFONT,0,0)));
    wchar_t hint[1024]{}; GetWindowTextW(state->guideVideoHint,hint,1024);
    RECT measured{0,0,SettingsPixels(336,dpi),0};
    DrawTextW(dc,hint,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    result.hintHeight=std::max(36,static_cast<int>((measured.bottom*96+dpi-1)/dpi));
    // Measure the actual description font after the key column has grown.
    // Round up at fractional DPI; a fixed row height can clip the second line.
    const int descriptionWidth=SettingsPixels(std::max(1,rowWidth-result.keyWidth-12),dpi);
    for(size_t i=0;i<state->guideDescriptions.size();++i) {
        const HWND control=state->guideDescriptions[i];
        if(!control)continue;
        SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0)));
        wchar_t caption[1024]{};GetWindowTextW(control,caption,ARRAYSIZE(caption));
        RECT bounds{0,0,descriptionWidth,0};
        DrawTextW(dc,caption,-1,&bounds,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        result.descriptionHeights[i]=static_cast<int>((bounds.bottom*96+dpi-1)/dpi);
    }
    SelectObject(dc,old); ReleaseDC(nullptr,dc);
    return result;
}
void CancelShortcutEdit(SettingsControls* state) {
    if (state->editingShortcut<0) return;
    state->editingShortcut=-1;
    RefreshShortcutCaptions(state);
}
void BeginShortcutEdit(SettingsControls* state,size_t index) {
    const bool same=state->editingShortcut==static_cast<int>(index);
    CancelShortcutEdit(state);
    if (same || index>=shortcuts::Count) return;
    state->editingShortcut=static_cast<int>(index);
    SetFocus(state->guideKeys[index]);
    SetSettingsText(state->guideKeys[index],L"…");
    SetSettingsText(state->guideVideoHint,state->english
        ? L"Press a key with Ctrl / Alt / Shift.\nClick the key button again to cancel."
        : L"키 입력 (Ctrl / Alt / Shift 조합 가능)\n취소하려면 키 버튼을 다시 누르세요.");
}
bool ProcessShortcutEdit(SettingsControls* state,const MSG& message,unsigned modifiers) {
    const bool down=shortcuts::KeyDown(message.message);
    const bool up=message.message==WM_KEYUP || message.message==WM_SYSKEYUP;
    if (state->shortcutKeyToRelease && (down || up) && message.wParam==state->shortcutKeyToRelease) {
        if (up) state->shortcutKeyToRelease=0;
        // A new physical press after focus loss must not remain suppressed.
        else if (!(message.lParam & (LPARAM{1}<<30))) state->shortcutKeyToRelease=0;
        else return true;
        if (up) return true;
    }
    if (state->editingShortcut<0) return false;
    const size_t index=static_cast<size_t>(state->editingShortcut);
    if (GetFocus()!=state->guideKeys[index]) { CancelShortcutEdit(state); return false; }
    if (up) return true;
    if (!down) return false;
    const UINT key=static_cast<UINT>(message.wParam);
    if (message.lParam & (LPARAM{1}<<30)) return true;
    if (shortcuts::Modifier(key)) return true;
    const unsigned binding=key|modifiers;
    if (!shortcuts::Valid(binding)) {
        SetSettingsText(state->guideVideoHint,state->english
            ? L"Reserved or unsupported key. Try another."
            : L"예약되었거나 지원하지 않는 키예요.\n다른 키를 입력하세요.");
        return true;
    }
    for (size_t i=0;i<shortcuts::Count;++i) if (i!=index && state->shortcutBindings[i]==binding) {
        SetSettingsText(state->guideVideoHint,state->english
            ? L"Already assigned. Try another key."
            : L"이미 사용 중인 키예요. 다른 키를 입력하세요.");
        return true;
    }
    state->shortcutBindings[index]=binding;
    state->shortcutKeyToRelease=key;
    CancelShortcutEdit(state);
    return true;
}

int SettingsPixels(int dips, UINT dpi) {
    return MulDiv(dips, dpi ? dpi : USER_DEFAULT_SCREEN_DPI,
                  USER_DEFAULT_SCREEN_DPI);
}

static constexpr int kSettingsTabbedClientHeightDip = 650;

static constexpr SettingsTab kNavigationTabs[] = {
    SettingsTab::VideoWindow, SettingsTab::Audio,
#ifdef LLCV_RECORDING
    SettingsTab::Recording,
#endif
    SettingsTab::Window,
    SettingsTab::GuideDiagnostics, SettingsTab::Updates};

SettingsTab SettingsTabFromNavigationIndex(int index) {
    return index >= 0 && index < kSettingsNavigationCount
        ? kNavigationTabs[index] : SettingsTab::VideoWindow;
}

int SettingsNavigationIndex(SettingsTab tab) {
    for (int i = 0; i < kSettingsNavigationCount; ++i)
        if (kNavigationTabs[i] == tab) return i;
    return 0;
}

void RefreshSettingsPageHeader(SettingsControls* state) {
    if (!state) return;
    const wchar_t* title = L"";
    const wchar_t* subtitle = L"";
    switch (state->activeTab) {
#ifdef LLCV_RECORDING
    case SettingsTab::Recording:
        title = state->english ? L"Recording" : L"녹화";
        subtitle = state->english ? L"Codec, quality and storage for SDR recordings." : L"SDR 녹화의 코덱과 화질, 저장 위치를 설정하세요.";
        break;
#endif
    case SettingsTab::VideoWindow:
        title = state->english ? L"Video" : L"영상";
        subtitle = state->english ? L"Capture devices, video formats and display settings." : L"캡처 장치, 영상 형식과 화면 표시를 설정하세요.";
        break;
    case SettingsTab::Audio:
        title = state->english ? L"Audio" : L"오디오";
        subtitle = state->english ? L"Output devices, volume controls and playback stability." : L"출력 장치, 음량 표시와 재생 안정성을 설정하세요.";
        break;
    case SettingsTab::Window:
        title = state->english ? L"Window" : L"창";
        subtitle = state->english ? L"Make the viewer fit your desktop." : L"창 이동과 테두리, 전체화면 동작을 설정하세요.";
        break;
    case SettingsTab::GuideDiagnostics:
        title = state->english ? L"Guide & diagnostics" : L"도움말 · 진단";
        subtitle = state->english ? L"Keyboard shortcuts and diagnostic logs." : L"단축키를 확인하고 진단 로그를 관리하세요.";
        break;
    case SettingsTab::Updates:
        title = state->english ? L"App preferences" : L"앱 설정";
        subtitle = state->english ? L"Language, startup behavior and updates." : L"언어, 시작 방식과 업데이트를 관리하세요.";
        break;
    }
    SetSettingsText(state->pageTitle, title);
    SetSettingsText(state->pageSubtitle, subtitle);
}

int SettingsClientHeightDip(const SettingsControls* state) {
    return state ? state->clientHeightDip : kSettingsTabbedClientHeightDip;
}

SIZE SettingsDialogOuterSize(HWND hwnd, UINT dpi,
                                    const SettingsControls* state) {
    RECT rect{0, 0, SettingsPixels(state ? state->clientWidthDip : kSettingsClientWidthDip, dpi),
              SettingsPixels(SettingsClientHeightDip(state), dpi)};
    const DWORD style =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if (!AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi)) {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

SettingsVisualUpdate::SettingsVisualUpdate(HWND owner) {
    if (owner && IsWindowVisible(owner)) {
        owner_ = owner;
        SendMessageW(owner_, WM_SETREDRAW, FALSE, 0);
    }
}
SettingsVisualUpdate::~SettingsVisualUpdate() {
    if (!owner_ || !IsWindow(owner_)) return;
    SendMessageW(owner_, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(owner_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void PlaceSettingsControl(HWND control, int x, int y, int width,
                                 int height, UINT dpi) {
    if (!control) return;
    RECT current{};
    GetWindowRect(control, &current);
    MapWindowPoints(HWND_DESKTOP, GetParent(control), reinterpret_cast<POINT*>(&current), 2);
    if (current.left == SettingsPixels(x, dpi) && current.top == SettingsPixels(y, dpi) &&
        current.right - current.left == SettingsPixels(width, dpi)) {
        const int actualHeight = current.bottom - current.top;
        if (actualHeight == SettingsPixels(height, dpi)) return;
        wchar_t name[32]{};
        GetClassNameW(control, name, ARRAYSIZE(name));
        if (_wcsicmp(name, L"COMBOBOX") == 0 && actualHeight == SettingsPixels(kSettingsComboHeightDip, dpi))
            return;
    }
    SetWindowPos(control, nullptr, SettingsPixels(x, dpi),
                 SettingsPixels(y, dpi), SettingsPixels(width, dpi),
                 SettingsPixels(height, dpi),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void ApplySettingsComboMetrics(HWND control, UINT dpi) {
    if (!control) return;
    RECT previous{};
    GetWindowRect(control, &previous);
    if (previous.bottom - previous.top == SettingsPixels(kSettingsComboHeightDip, dpi) &&
        SendMessageW(control, CB_GETITEMHEIGHT, 0, 0) == SettingsPixels(24, dpi)) return;
    // Both SetWindowPos and CB_SETDROPPEDWIDTH can restore a combo's
    // font-derived closed height. This must be the LAST layout operation.
    SendMessageW(control, CB_SETITEMHEIGHT, 0, SettingsPixels(24, dpi));
    SendMessageW(control, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
                 SettingsPixels(24, dpi));
    RECT actual{};
    GetWindowRect(control, &actual);
    const int itemHeight = static_cast<int>(SendMessageW(
        control, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0));
    const int adjustment = SettingsPixels(kSettingsComboHeightDip, dpi) -
                           (actual.bottom - actual.top);
    // Native border thickness is not necessarily scaled like our DIPs.
    // Measure it rather than assuming a fixed six-pixel frame at all DPI.
    if (itemHeight != CB_ERR && adjustment != 0)
        SendMessageW(control, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1),
                     std::max(1, itemHeight + adjustment));
}

static BOOL CALLBACK SetSettingsChildFont(HWND child, LPARAM fontValue) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(fontValue), FALSE);
    return TRUE;
}

void ApplySettingsFont(SettingsControls* state, HWND hwnd,
                              UINT dpi) {
    if (!state || !hwnd) return;
    const auto createFont = [dpi, state](int points, int weight) {
        return CreateFontW(-MulDiv(points, dpi ? dpi : USER_DEFAULT_SCREEN_DPI, 72),
            0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, SettingsFontFamily(weight, state->english));
    };
    HFONT font = createFont(kSettingsBodyFontPoints, FW_MEDIUM);
    HFONT secondaryFont = createFont(kSettingsSecondaryFontPoints, FW_NORMAL);
    HFONT sectionFont = createFont(kSettingsBodyFontPoints, FW_SEMIBOLD);
    HFONT titleFont = createFont(kSettingsTitleFontPoints, FW_SEMIBOLD);
    // Allocate the complete replacement before touching the active set. A
    // failed allocation must not leave controls pointing at deleted fonts.
    if (!font || !secondaryFont || !sectionFont || !titleFont) {
        for (HFONT candidate : {font, secondaryFont, sectionFont, titleFont})
            if (candidate) DeleteObject(candidate);
        return;
    }
    std::vector<HFONT> previousFonts;
    previousFonts.swap(state->uiFonts);
    state->uiFonts = {font, secondaryFont, sectionFont, titleFont};
    EnumChildWindows(hwnd, SetSettingsChildFont,
                     reinterpret_cast<LPARAM>(font));
#ifdef LLCV_RECORDING
    for(auto control:{state->recVideoTitle,state->recSaveTitle})
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(sectionFont),FALSE);
    for(auto control:{state->recStatus,state->recSpace,state->recNote,state->recSplitHint})
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(secondaryFont),FALSE);
#endif
    // Smaller explanatory copy recedes without making interactive options
    // harder to read. Labels, fields, navigation and buttons remain 10 pt.
    for (HWND control : {state->pageSubtitle, state->versionWatermark,
                         state->audioStatus, state->surround51Hint,
                         state->captureAudioStatus, state->videoCapabilityStatus, state->unlistedFpsStatus,
                         state->vsrStatus, state->screenshotHelp, state->frameOptionsHint,
                         state->relativeSizeWarning, state->fullscreenCursorHint,
                         state->fullscreenPositionHint,
                         state->guideDiagnosticsText, state->guideVideoHint, state->skipStartupHint,
                         state->preventSleepHint, state->updateText, state->updateStatus}) {
        if (control) SendMessageW(control, WM_SETFONT,
                                  reinterpret_cast<WPARAM>(secondaryFont), FALSE);
    }
    for (HWND control : {state->audioOutputSection,
                         state->audioPlaybackSection,
                         state->audioStabilitySection,
                         state->videoCaptureSection,
                         state->frameOptionsTitle,
                         state->videoDisplaySection,
                         state->videoWindowSection,
                         state->appPreferencesSection,
                         state->brandLabel,
                         state->updateTitle,
                         state->startButton,
                         state->guideShortcutsTitle,
                         state->screenshotTitle,
                         state->guideDiagnosticsTitle}) {
        if (control) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(sectionFont), FALSE);
        }
    }

    for (HWND key : state->guideKeys) {
        if (key) SendMessageW(key, WM_SETFONT, reinterpret_cast<WPARAM>(sectionFont), FALSE);
    }
    if (state->pageTitle) {
        SendMessageW(state->pageTitle, WM_SETFONT,
                     reinterpret_cast<WPARAM>(titleFont), FALSE);
    }
    // Every child now references the replacement set, so monitor transitions
    // release all four prior resources instead of accumulating GDI objects.
    for (HFONT previous : previousFonts) DeleteObject(previous);
}

// Checkbox captions vary substantially between Korean and English.  Measure
// the actual current UI font so a neighbouring help button stays attached to
// its option at every DPI instead of relying on a fragile hard-coded x value.
static int SettingsCheckboxWidthDip(HWND checkbox, UINT dpi) {
    if (!checkbox) return 250;
    wchar_t text[512]{};
    GetWindowTextW(checkbox, text, ARRAYSIZE(text));
    HDC hdc = GetDC(checkbox);
    if (!hdc) return 250;
    const HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(checkbox, WM_GETFONT, 0, 0));
    const HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
    SIZE size{};
    GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &size);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(checkbox, hdc);
    const int textWidthDip = MulDiv(
        size.cx, USER_DEFAULT_SCREEN_DPI,
        dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
    // Checkbox glyph plus caption.  Keeping the HWND no wider than this is
    // important: a wide checkbox would overlap a nearby help button and
    // steal its clicks even when the button looks visually separate.
    return std::min(29 + textWidthDip, 430);
}

static bool WindowBehaviorWarningNeeded(const SettingsControls* state) {
    return state->pixelCheck && state->relativeSizeCheck &&
        SendMessageW(state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED &&
        SendMessageW(state->relativeSizeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void LayoutWindowBehaviorControls(SettingsControls* state) {
    const UINT dpi = state->layoutDpi;
    const auto place = [dpi](HWND control, int x, int y, int width, int height) {
        PlaceSettingsControl(control, x, y, width, height, dpi);
    };
    // A dedicated note row prevents controls from jumping under the pointer
    // when Pixel-perfect/relative sizing changes.
    place(state->videoWindowSection, 184, 108, 792, 20);
    place(state->relativeSizeCheck, 184, 140, 376, 32);
    place(state->relativeSizeWarning, 184, 364, 792, 56);
    place(state->borderlessCheck, 184, 180, 376, 32);
    place(state->roundedCornersCheck, 600, 140, 376, 32);
    place(state->windowSnapCheck, 600, 180, 376, 32);
    place(state->fullscreenCursorLabel, 184, 232, 376, 20);
    place(state->fullscreenCursorCombo, 184, 260, 376, 120);
    place(state->fullscreenCursorHint, 184, 300, 376, 24);
    if (state->fullscreenCursorHint)
        SetWindowPos(state->fullscreenCursorHint, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ApplySettingsComboMetrics(state->fullscreenCursorCombo, dpi);
    place(state->fullscreenPositionLabel, 600, 232, 376, 20);
    place(state->fullscreenPositionCombo, 600, 260, 376, 300);
    place(state->fullscreenPositionHint, 600, 300, 376, 40);
    ApplySettingsComboMetrics(state->fullscreenPositionCombo, dpi);
}

void LayoutSettingsControls(SettingsControls* state, UINT dpi) {
    if (!state) return;
    state->layoutDpi = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    const auto place = [dpi](HWND control, int x, int y, int width, int height) {
        PlaceSettingsControl(control, x, y, width, height, dpi);
    };
    // The navigation is deliberately narrow. Labels sit above fields, allowing
    // full-width device names without making the dialog wider.
    place(state->brandLabel, 10, 28, 132, 40);
    place(state->tabControl, 10, 100, 132, 44*kSettingsNavigationCount);
#ifdef LLCV_RECORDING
    LayoutRecordingSettings(state,dpi);
#endif
    SendMessageW(state->tabControl, LB_SETITEMHEIGHT, 0, SettingsPixels(44, dpi));
    place(state->pageTitle, 184, 24, 792, 40);
    place(state->pageSubtitle, 184, 66, 792, 24);
    place(state->versionWatermark, 10, 608, 132, 20);
    place(state->startButton, 762, 602, 102, 34);
    place(state->cancelButton, 876, 602, 100, 34);

    // Labels share the video page's 64-DIP cadence and eight-DIP field gap. Related
    // checkboxes stay grouped; the lower timing section gets a 24-DIP break.
    place(state->audioOutputSection, 184, 108, 376, 20);
    place(state->audioLabel, 184, 136, 376, 20);
    place(state->audioCombo, 184, 164, 376, 160);
    place(state->audioOutputLabel, 184, 200, 376, 20);
    place(state->audioOutputCombo, 184, 228, 376, 220);
    place(state->bufferLabel, 184, 264, 376, 20);
    place(state->bufferCombo, 184, 292, 376, 180);
    place(state->audioStatus, 184, 332, 376, 48);
    place(state->exclusiveTestButton, 184, 388, 185, 32);
    place(state->audioPlaybackSection, 600, 108, 376, 20);
    place(state->volumeHudLabel, 600, 136, 376, 20);
    place(state->volumeHudCombo, 600, 164, 376, 160);
    const int boostWidth = std::min(SettingsCheckboxWidthDip(state->volumeBoostCheck, dpi), 340);
    place(state->volumeBoostCheck, 600, 204, boostWidth, 32);
    place(state->volumeBoostHelp, 600 + boostWidth + 12, 208, 24, 24);
    place(state->muteBackgroundCheck, 600, 244, 376, 32);
    place(state->audioOnlyCheck, 600, 284, 376, 32);
    place(state->surround51Check, 600, 336, 376, 32);
    place(state->surround51Hint, 600, 376, 376, 88);
    place(state->audioStabilitySection, 184, 488, 792, 20);
    place(state->driftLabel, 184, 516, 340, 20);
    place(state->driftHelp, 536, 514, 24, 24);
    place(state->driftCombo, 184, 544, 376, 120);
    place(state->pcmQueueLabel, 600, 516, 340, 20);
    place(state->pcmQueueHelp, 952, 514, 24, 24);
    place(state->pcmQueueCombo, 600, 544, 376, 140);

    // Video: preserve the detected-format summary instead of hiding capability
    // information behind another interaction.
    place(state->videoCaptureSection, 184, 108, 376, 20);
    // All inline actions share a 24-DIP target centered on the label,
    // leaving six DIPs before the field and twelve beside its caption.
    place(state->videoRefreshButton, 476, 134, 84, 24);
    place(state->captureDeviceLabel, 184, 136, 280, 20);
    place(state->captureDeviceCombo, 184, 164, 376, 220);
    place(state->captureAudioDeviceLabel, 184, 200, 376, 20);
    place(state->captureAudioDeviceCombo, 184, 228, 376, 220);
    place(state->captureAudioStatus, 184, 228, 376, 30);
    place(state->videoLabel, 184, 264, 180, 20);
    place(state->videoCombo, 184, 292, 180, 120);
    place(state->frameRateLabel, 380, 264, 72, 20);
    place(state->frameOptionsButton, 464, 262, 96, 24);
    place(state->frameRateCombo, 380, 292, 180, 200);
    SendMessageW(state->frameRateCombo, CB_SETDROPPEDWIDTH, SettingsPixels(342, dpi), 0);
    SendMessageW(state->captureDeviceCombo, CB_SETDROPPEDWIDTH, SettingsPixels(560, dpi), 0);
    SendMessageW(state->captureAudioDeviceCombo, CB_SETDROPPEDWIDTH, SettingsPixels(560, dpi), 0);
    SendMessageW(state->audioOutputCombo, CB_SETDROPPEDWIDTH, SettingsPixels(560, dpi), 0);
    // Restore the original 64-DIP row rhythm and unchanged field sizes.
    place(state->pixelFormatLabel, 184, 328, 376, 20);
    place(state->pixelFormatCombo, 184, 356, 376, 160);
    LayoutVideoStatus(state,dpi);
    place(state->forceHdr10Check, 184, 392, 344, 40);
    place(state->forceHdr10Help, 536, 400, 24, 24);
    // Keep the full compatibility captions readable in the closed field,
    // including the themed text inset and dropdown arrow.
    place(state->hdrChromaLabel, 184, 444, 112, 20);
    place(state->hdrChromaCombo, 304, 440, 224, 150);
    place(state->hdrChromaHelp, 536, 443, 24, 24);
    place(state->sdrRangeLabel, 184, 392, 180, 20);
    place(state->sdrRangeCombo, 184, 420, 146, 150);
    place(state->sdrRangeHelp, 340, 423, 24, 24);
    place(state->mjpegColorLabel, 380, 392, 180, 20);
    place(state->mjpegColorCombo, 380, 420, 146, 150);
    place(state->mjpegColorHelp, 536, 423, 24, 24);
    // A local flyout, painted with the existing settings card style. Obscured
    // controls are hidden (not reparented) so native selection/IDs stay intact.
    // The outer card spans the full capture column (184..560), with 16-DIP
    // inner margins. Keep aligned rows and room for all three hint lines.
    place(state->frameOptionsTitle, 200, 352, 304, 24);
    place(state->frameOptionsClose, 516, 350, 28, 28);
    place(state->unlistedFpsCheck, 200, 386, 304, 28);
    place(state->capture40Check, 200, 422, 304, 28);
    place(state->capture40Help, 520, 424, 24, 24);
    place(state->frameOptionsHint, 200, 462, 344, 48);

    place(state->videoDisplaySection, 600, 108, 376, 20);
    place(state->presentationLabel, 600, 136, 340, 20);
    place(state->presentationHelp, 952, 134, 24, 24);
    place(state->presentationCombo, 600, 164, 376, 120);
    place(state->displayMonitorLabel, 600, 200, 376, 20);
    place(state->displayMonitorCombo, 600, 228, 376, 180);
    SendMessageW(state->displayMonitorCombo, CB_SETDROPPEDWIDTH, SettingsPixels(560, dpi), 0);
    place(state->pixelCheck, 800, 262, 176, 24);
    place(state->scalingLabel, 600, 264, 188, 20);
    place(state->scalingCombo, 600, 292, 376, 120);
    // Continue the same 64-DIP label/field rhythm as capture and monitor.
    // Keep conditional details in a reserved row so switching filters never
    // moves the screenshot actions or changes the dialog's height.
    place(state->displayResolutionLabel, 600, 328, 376, 20);
    place(state->displayResolutionCombo, 600, 356, 376, 150);
    place(state->upscalerLabel, 600, 392, 340, 20);
    place(state->upscalerCombo, 600, 420, 376, 150);
    place(state->nisSharpnessLabel, 600, 460, 132, 30);
    place(state->nisSharpnessCombo, 740, 460, 88, 180);
    place(state->vsrGuideButton, 952, 390, 24, 24);
    const bool cmaaDetail=SendMessageW(state->upscalerCombo,CB_GETCURSEL,0,0)==1;
    place(state->vsrStatus, cmaaDetail ? 844 : 600, 460, cmaaDetail ? 132 : 376, 30);
    // Raise the intact screenshot group, retaining clearance from the optional
    // sharpness row and a 16-DIP footer margin without shrinking controls.
    place(state->screenshotTitle, 600, 496, 376, 20);
    place(state->screenshotClipboardCheck, 600, 522, 182, 26);
    place(state->screenshotFolderButton, 794, 520, 182, 30);
    place(state->screenshotHelp, 600, 556, 376, 18);

    // Window behavior has its own page, avoiding a packed video/options wall.
    LayoutWindowBehaviorControls(state);

    place(state->guideShortcutsTitle, 204, 120, 216, 20);
    place(state->shortcutResetButton, 432, 118, 108, 24);
    const auto shortcutLayout=MeasureShortcutLayout(state,dpi);
    const int shortcutStep=state->guideKeys.size()>10 ? 32 : state->guideKeys.size()>9 ? 35 : 36;
    constexpr int shortcutKeyHeight=29;
    constexpr int shortcutTop=156;
    int shortcutY=shortcutTop;
    for (size_t i = 0; i < state->guideKeys.size(); ++i) {
        const int height=std::max(shortcutStep-1,shortcutLayout.descriptionHeights[i]);
        place(state->guideKeys[i], 204, shortcutY, shortcutLayout.keyWidth, shortcutKeyHeight);
        place(state->guideDescriptions[i], 216+shortcutLayout.keyWidth, shortcutY-3, 324-shortcutLayout.keyWidth, height);
        shortcutY+=std::max(shortcutStep,height+1);
    }
    // Use the existing gap before the hint for small wrapped-row growth,
    // keeping the card's bottom padding clear of the fixed dialog footer.
    const int normalHintY=shortcutTop+static_cast<int>(state->guideKeys.size()-1)*shortcutStep+shortcutKeyHeight+16;
    const int hintY=std::max(normalHintY,shortcutY-shortcutStep+shortcutKeyHeight+8);
    place(state->guideVideoHint, 204, hintY, 336, shortcutLayout.hintHeight);
    place(state->guideDiagnosticsTitle, 600, 120, 376, 20);
    place(state->guideDiagnosticsText, 600, 148, 376, 32);
    place(state->saveLogCheck, 600, 196, 376, 32);
    place(state->showConsoleCheck, 600, 236, 376, 32);
    place(state->guideLogFolderButton, 600, 284, 185, 32);
    place(state->guideLsButton, 600, 348, 376, 32);

    // App-wide preferences no longer dominate every page.
    place(state->appPreferencesSection, 184, 108, 792, 20);
    place(state->languageLabel, 184, 136, 376, 20);
    place(state->languageCombo, 184, 164, 376, 120);
    place(state->themeLabel, 600, 136, 376, 20);
    place(state->themeCombo, 600, 164, 376, 120);
    place(state->skipStartupCheck, 184, 204, 376, 32);
    place(state->skipStartupHint, 212, 244, 348, 56);
    place(state->preventSleepCheck, 600, 204, 376, 32);
    place(state->preventSleepHint, 628, 244, 348, 56);
    place(state->updateTitle, 184, 328, 792, 24);
    place(state->updateText, 184, 360, 730, 44);
    place(state->checkForUpdatesCheck, 184, 416, 760, 32);
    place(state->updateNowButton, 184, 460, 185, 32);
    place(state->updateStatus, 184, 504, 792, 48);
    for (HWND combo : {state->audioCombo, state->audioOutputCombo,
                        state->bufferCombo, state->volumeHudCombo,
                        state->driftCombo, state->pcmQueueCombo,
                        state->captureDeviceCombo, state->captureAudioDeviceCombo,
                        state->videoCombo, state->displayResolutionCombo, state->frameRateCombo,
                        state->upscalerCombo, state->nisSharpnessCombo,
                        state->pixelFormatCombo, state->presentationCombo,
                        state->displayMonitorCombo, state->scalingCombo,
                        state->hdrChromaCombo, state->sdrRangeCombo, state->mjpegColorCombo,
                        state->fullscreenCursorCombo, state->languageCombo, state->themeCombo,
                        state->sectionNavigation})
        ApplySettingsComboMetrics(combo, dpi);
    LayoutCompactSettingsControls(state, dpi);
}

void SetSettingsControlVisible(HWND control, bool visible, bool enabled) {
    if (!control) return;
    visible = visible && SettingsControlOnCurrentPage(control);
    const bool wasVisible = (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
    if (wasVisible != visible) ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
    const bool shouldEnable = visible && enabled;
    if ((IsWindowEnabled(control) != FALSE) != shouldEnable) EnableWindow(control, shouldEnable);
}

bool DisplayResolutionMatchesCapture(HWND combo) {
    if (!combo) return false;
    const LRESULT row = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    return row >= 0 && SendMessageW(combo, CB_GETITEMDATA, row, 0) == kDisplayMatchCaptureItem;
}

static bool HasSplitResolution(const SettingsControls& state) {
    if (DisplayResolutionMatchesCapture(state.displayResolutionCombo)) return false;
    const LRESULT capture = SendMessageW(state.videoCombo, CB_GETCURSEL, 0, 0);
    const LRESULT display = SendMessageW(state.displayResolutionCombo, CB_GETCURSEL, 0, 0);
    return capture >= 0 && display >= 0 && capture != display;
}

void UpdateScalingControlVisibility(SettingsControls* state) {
    if (!state) return;
    const bool pixelPerfect = state->pixelCheck &&
        SendMessageW(state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool scaling = (!pixelPerfect || HasSplitResolution(*state)) &&
        SendMessageW(state->upscalerCombo,CB_GETCURSEL,0,0)!=1;
    const bool visible = state->activeTab == SettingsTab::VideoWindow;
    SetSettingsControlVisible(state->scalingLabel, visible, scaling);
    SetSettingsControlVisible(state->scalingCombo, visible, scaling);
}

void UpdateWindowBehaviorVisibility(SettingsControls* state) {
    if (!state) return;

    // These are everyday window-behavior preferences, not advanced tuning.
    // Only show the caveat when the currently selected combination needs it.
    const bool visible = state->activeTab == SettingsTab::Window;
    SetSettingsControlVisible(state->videoWindowSection, visible);
    SetSettingsControlVisible(state->windowSnapCheck, visible);
    SetSettingsControlVisible(state->relativeSizeCheck, visible);
    SetSettingsControlVisible(state->borderlessCheck, visible);
    SetSettingsControlVisible(state->roundedCornersCheck, visible);
    SetSettingsControlVisible(state->fullscreenCursorLabel, visible);
    SetSettingsControlVisible(state->fullscreenCursorCombo, visible);
    SetSettingsControlVisible(state->fullscreenCursorHint, visible);
    SetSettingsControlVisible(state->fullscreenPositionLabel, visible);
    SetSettingsControlVisible(state->fullscreenPositionCombo, visible);
    SetSettingsControlVisible(state->fullscreenPositionHint, visible);
    SetSettingsControlVisible(state->relativeSizeWarning,
                              visible && WindowBehaviorWarningNeeded(state));
}

void ApplySdrRangeSelection(SettingsControls* state) {
    if (!state || !state->sdrRangeCombo || !IsWindowEnabled(state->sdrRangeCombo)) return;
    const LRESULT row = SendMessageW(state->sdrRangeCombo, CB_GETCURSEL, 0, 0);
    if (row == CB_ERR) return;
    const LRESULT value = SendMessageW(state->sdrRangeCombo, CB_GETITEMDATA, row, 0);
    if (value < static_cast<LRESULT>(video_color::RangeOverride::Auto) ||
        value > static_cast<LRESULT>(video_color::RangeOverride::Full)) return;
    state->sdrRangeOverride = static_cast<video_color::RangeOverride>(value);
    state->legacyMjpegRangeOverride = video_color::RangeOverride::Auto;
}

void UpdateAdvancedControlVisibility(SettingsControls* state, bool exclusive,
                                            settings::VideoPixelFormat selectedFormat,
                                            SettingsAvailability availability) {
    if (!state) return;
    const bool audio = state->activeTab == SettingsTab::Audio;
    const bool video = state->activeTab == SettingsTab::VideoWindow;
    if (!video || SettingsCompactLayout(state)) state->frameOptionsOpen = false;
    const bool flyout = video && state->frameOptionsOpen;
    const bool frameOptions = video && (flyout || SettingsCompactLayout(state));
    const bool guide = state->activeTab == SettingsTab::GuideDiagnostics;
    const bool updates = state->activeTab == SettingsTab::Updates;
    LayoutCompactSettingsControls(state, state->layoutDpi);
    LayoutVideoStatus(state,state->layoutDpi);
    RefreshSettingsPageHeader(state);
#ifdef LLCV_RECORDING
    ShowRecordingSettings(state);
#endif
    for (HWND control : {state->tabControl, state->brandLabel,
                         state->pageTitle, state->pageSubtitle,
                         state->versionWatermark,
                         state->cancelButton}) {
        SetSettingsControlVisible(control, true);
    }
    SetSettingsControlVisible(state->startButton, true, availability.start);
    SetSettingsControlVisible(state->sectionNavigationLabel, SettingsCompactLayout(state));
    SetSettingsControlVisible(state->sectionNavigation, SettingsCompactLayout(state));
    for (HWND control : {state->languageLabel, state->languageCombo,
                         state->themeLabel, state->themeCombo,
                         state->skipStartupCheck, state->skipStartupHint,
                         state->preventSleepCheck, state->preventSleepHint,
                         state->appPreferencesSection})
        SetSettingsControlVisible(control, updates);
    for (HWND control : {state->audioOutputSection,
                         state->audioPlaybackSection,
                         state->audioStabilitySection,
                         state->audioLabel, state->audioCombo,
                         state->audioOutputLabel, state->audioOutputCombo,
                         state->bufferLabel,
                         state->audioStatus,
                         state->volumeHudLabel, state->volumeHudCombo,
                         state->volumeBoostCheck, state->volumeBoostHelp,
                         state->muteBackgroundCheck, state->audioOnlyCheck,
                         state->driftLabel, state->driftHelp, state->driftCombo,
                         state->pcmQueueLabel, state->pcmQueueHelp,
                         state->pcmQueueCombo}) {
        SetSettingsControlVisible(control, audio);
    }
    SetSettingsControlVisible(state->bufferCombo, audio, availability.audioBuffer);
    // The endpoint recheck belongs only to WASAPI Exclusive.  In Shared and
    // ASIO modes it is both irrelevant and misleading, even on the Audio tab.
    SetSettingsControlVisible(state->exclusiveTestButton,
                              audio && exclusive, availability.exclusiveProbe);
    const bool shared = SendMessageW(state->audioCombo, CB_GETCURSEL, 0, 0) == 0;
    SetSettingsControlVisible(state->surround51Check, audio, shared);
    SetSettingsControlVisible(state->surround51Hint, audio, shared);
    for (HWND control : {state->videoCaptureSection,
                         state->videoRefreshButton,
                         state->videoDisplaySection,
                         state->presentationLabel, state->presentationHelp,
                         state->presentationCombo, state->displayMonitorLabel,
                         state->displayMonitorCombo, state->captureDeviceLabel,
                         state->captureAudioDeviceLabel, state->videoLabel,
                         state->videoCombo, state->pixelFormatLabel,
                         state->frameRateLabel, state->videoCapabilityStatus,
                         state->pixelCheck}) {
        const bool obscured = control == state->pixelFormatLabel || control == state->videoCapabilityStatus;
        SetSettingsControlVisible(control, video && !(flyout && obscured));
    }
    SetSettingsControlVisible(state->captureDeviceCombo, video, availability.captureDevice);
    SetSettingsControlVisible(state->pixelFormatCombo, video && !flyout, availability.formats);
    const bool capture40 = SendMessageW(state->capture40Check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    SetSettingsControlVisible(state->frameRateCombo, video, availability.formats && !capture40);
    SetSettingsControlVisible(state->frameOptionsButton, video && !SettingsCompactLayout(state));
    SetSettingsText(state->frameOptionsButton, state->english
        ? (flyout ? L"More ▴" : L"More ▾") : (flyout ? L"추가 설정 ▴" : L"추가 설정 ▾"));
    for (HWND control : {state->frameOptionsTitle, state->frameOptionsClose, state->frameOptionsHint})
        SetSettingsControlVisible(control, flyout);
    SetSettingsControlVisible(state->capture40Check, frameOptions, availability.formats);
    SetSettingsControlVisible(state->unlistedFpsCheck, frameOptions, availability.formats && !capture40);
    SetSettingsControlVisible(state->unlistedFpsStatus, video && !flyout &&
        GetWindowTextLengthW(state->unlistedFpsStatus) > 0);
    SetSettingsControlVisible(state->capture40Help, frameOptions);
    SetSettingsControlVisible(state->guideLsButton, guide);
    // This row has two mutually exclusive controls: the device picker for a
    // separate capture endpoint, or the short "built-in audio" status. Keep
    // its existing video-tab choice intact; hide both together off-tab.
    if (!video) {
        SetSettingsControlVisible(state->captureAudioDeviceCombo, false);
        SetSettingsControlVisible(state->captureAudioStatus, false);
    }
    const bool p010Selected = selectedFormat ==
        VideoPixelFormat::P010;
    const bool mjpegSelected = selectedFormat ==
        VideoPixelFormat::Mjpeg;
    const auto range = state->sdrRangeOverride != video_color::RangeOverride::Auto || !mjpegSelected
        ? state->sdrRangeOverride : state->legacyMjpegRangeOverride;
    if (SendMessageW(state->sdrRangeCombo, CB_GETCURSEL, 0, 0) != static_cast<LRESULT>(range))
        SendMessageW(state->sdrRangeCombo, CB_SETCURSEL, static_cast<WPARAM>(range), 0);
    SetSettingsControlVisible(state->sdrRangeLabel, video && !flyout && !p010Selected);
    SetSettingsControlVisible(state->sdrRangeCombo, video && !flyout && !p010Selected, availability.formats);
    SetSettingsControlVisible(state->sdrRangeHelp, video && !flyout && !p010Selected);
    SetSettingsControlVisible(state->forceHdr10Check, video && !flyout && p010Selected);
    SetSettingsControlVisible(state->forceHdr10Help, video && !flyout && p010Selected);
    SetSettingsControlVisible(state->hdrChromaLabel, video && !flyout && p010Selected);
    SetSettingsControlVisible(state->hdrChromaCombo, video && !flyout && p010Selected);
    SetSettingsControlVisible(state->hdrChromaHelp, video && !flyout && p010Selected);
    SetSettingsControlVisible(state->mjpegColorLabel, video && !flyout && mjpegSelected);
    SetSettingsControlVisible(state->mjpegColorCombo, video && !flyout && mjpegSelected);
    SetSettingsControlVisible(state->mjpegColorHelp, video && !flyout && mjpegSelected);
    SetSettingsControlVisible(state->guideShortcutsTitle, guide);
    SetSettingsControlVisible(state->shortcutResetButton, guide);
    SetSettingsControlVisible(state->guideVideoHint, guide);
    for (HWND key : state->guideKeys)
        SetSettingsControlVisible(key, guide);
    for (HWND description : state->guideDescriptions) SetSettingsControlVisible(description, guide);
    SetSettingsControlVisible(state->guideDiagnosticsTitle, guide);
    SetSettingsControlVisible(state->guideDiagnosticsText, guide);
    SetSettingsControlVisible(state->guideLogFolderButton, guide);
    SetSettingsControlVisible(state->screenshotTitle, video);
    if (state->vsrGpuUnavailable && SendMessageW(state->upscalerCombo,CB_GETCURSEL,0,0)==2)
        SendMessageW(state->upscalerCombo,CB_SETCURSEL,0,0);
    const bool cmaaNis=SendMessageW(state->upscalerCombo,CB_GETCURSEL,0,0)==1;
    if (!SettingsCompactLayout(state))
        PlaceSettingsControl(state->vsrStatus, cmaaNis ? 844 : 600, 460,
            cmaaNis ? 132 : 376, 30, state->layoutDpi);
    SetSettingsControlVisible(state->upscalerLabel, video);
    SetSettingsControlVisible(state->upscalerCombo, video);
    SetSettingsControlVisible(state->nisSharpnessLabel, video && cmaaNis);
    SetSettingsControlVisible(state->nisSharpnessCombo, video && cmaaNis);
    SetSettingsControlVisible(state->vsrGuideButton, video);
    SetSettingsControlVisible(state->vsrStatus, video &&
        (cmaaNis || SendMessageW(state->upscalerCombo,CB_GETCURSEL,0,0)==2 || state->vsrGpuUnavailable));
    SetSettingsControlVisible(state->displayResolutionLabel, video);
    SetSettingsControlVisible(state->displayResolutionCombo, video);
    SetSettingsText(state->videoLabel,
        state->english ? L"Capture resolution" : L"캡처 해상도");
    SetSettingsText(state->pixelCheck, HasSplitResolution(*state)
        ? (state->english ? L"Lock display size" : L"표시 크기 고정")
        : L"Pixel-perfect (1:1)");
    SetSettingsText(state->vsrStatus, cmaaNis ? (state->english ? L"SDR only" : L"SDR 전용") : shortcuts::Expand(state->vsrGpuUnavailable
        ? (state->english ? L"VSR requires an NVIDIA GPU" : L"VSR은 NVIDIA GPU가 필요합니다")
        : L"SDR / HDR10 · VSR: F6", state->shortcutBindings).c_str());
    SetSettingsControlVisible(state->screenshotClipboardCheck, video);
    SetSettingsControlVisible(state->screenshotHelp, video);
    SetSettingsControlVisible(state->screenshotFolderButton, video);
    SetSettingsControlVisible(state->saveLogCheck, guide);
    SetSettingsControlVisible(state->showConsoleCheck, guide);
    SetSettingsControlVisible(state->updateTitle, updates);
    SetSettingsControlVisible(state->updateText, updates);
    SetSettingsControlVisible(state->checkForUpdatesCheck, updates);
    SetSettingsControlVisible(state->updateNowButton, updates);
    SetSettingsControlVisible(state->updateStatus, updates);
    UpdateScalingControlVisibility(state);
    UpdateWindowBehaviorVisibility(state);
}

RECT SettingsFrameOptionsBounds(const SettingsControls* state, HWND owner) {
    RECT bounds{};
    if (!state || !owner || !state->frameOptionsOpen) return bounds;
    bool found = false;
    for (HWND child : {state->frameOptionsTitle, state->frameOptionsClose,
                      state->frameOptionsHint, state->unlistedFpsCheck,
                      state->capture40Check, state->capture40Help}) {
        if (!child || !(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT rect{};
        GetWindowRect(child, &rect);
        MapWindowPoints(HWND_DESKTOP, owner, reinterpret_cast<POINT*>(&rect), 2);
        if (found) UnionRect(&bounds, &bounds, &rect);
        else { bounds = rect; found = true; }
    }
    if (found) {
        const int padding = SettingsPixels(16, state->layoutDpi);
        InflateRect(&bounds, padding, padding);
    }
    return bounds;
}

bool HandleFrameOptionsMessage(SettingsControls* state, HWND owner, UINT message,
                               WPARAM wParam, LPARAM lParam) {
    if (!state) return false;
    using namespace control_id;
    if (message == WM_COMMAND) {
        const int id = LOWORD(wParam);
        const bool toggle = state->frameOptionsButton && (id == IDC_SETTINGS_FRAME_OPTIONS ||
            (id == IDOK && GetFocus() == state->frameOptionsButton));
        if (toggle && state->activeTab == SettingsTab::VideoWindow && !SettingsCompactLayout(state)) {
            state->frameOptionsOpen = !state->frameOptionsOpen;
            return true;
        }
        if (state->frameOptionsOpen) {
            if (id == IDCANCEL || id == IDOK || id == IDC_SETTINGS_FRAME_OPTIONS_CLOSE) {
                state->frameOptionsOpen = false;
                SetFocus(state->frameOptionsButton);
                return true;
            }
            if (id != IDC_SETTINGS_UNLISTED_FPS && id != IDC_SETTINGS_CAPTURE_40 &&
                id != IDC_SETTINGS_CAPTURE_40_HELP) state->frameOptionsOpen = false;
        }
    } else if (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) {
        state->frameOptionsOpen = false;
    } else if (message == WM_DPICHANGED || message == WM_CLOSE) {
        state->frameOptionsOpen = false;
    } else if (message == WM_LBUTTONDOWN && state->frameOptionsOpen) {
        const RECT bounds = SettingsFrameOptionsBounds(state, owner);
        POINT point{static_cast<short>(LOWORD(lParam)),static_cast<short>(HIWORD(lParam))};
        if (!PtInRect(&bounds, point)) state->frameOptionsOpen = false;
    }
    return false;
}

void TrackSettingsTooltip(HWND target, HWND tooltip, bool active) {
    if (!target || !tooltip) return;
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFO_V1_SIZE;
    tool.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    tool.hwnd = GetParent(target);
    tool.uId = reinterpret_cast<UINT_PTR>(target);
    SendMessageW(tooltip, TTM_TRACKACTIVATE, active ? TRUE : FALSE,
                 reinterpret_cast<LPARAM>(&tool));
}

void AddSettingsTooltip(SettingsControls* state, HWND owner,
                               HWND target, const wchar_t* text) {
    if (!state || !owner || !target || !text) return;
    if (!state->tooltipWindow) {
        state->tooltipWindow = CreateWindowExW(
            WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!state->tooltipWindow) return;
        SetWindowPos(state->tooltipWindow, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SendMessageW(state->tooltipWindow, TTM_SETMAXTIPWIDTH, 0, 430);
        SendMessageW(state->tooltipWindow, TTM_SETDELAYTIME,
                     TTDT_INITIAL, 250);
    }
    TOOLINFOW tool{};
    // The application intentionally has no Common Controls v6 manifest.  The
    // built-in v5 tooltip rejects the newer structure size on some Windows
    // installations, so register the compatible v1 fields explicitly.
    tool.cbSize = TTTOOLINFO_V1_SIZE;
    tool.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    tool.hwnd = owner;
    tool.uId = reinterpret_cast<UINT_PTR>(target);
    tool.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(state->tooltipWindow, TTM_ADDTOOLW, 0,
                 reinterpret_cast<LPARAM>(&tool));
}

bool IsSettingsHelpControl(const SettingsControls* state,
                                  HWND target) {
    return state && (target == state->driftHelp ||
                     target == state->pcmQueueHelp ||
                      target == state->presentationHelp ||
                      target == state->volumeBoostHelp ||
                      target == state->forceHdr10Help ||
                      target == state->hdrChromaHelp ||
                      target == state->mjpegColorHelp || target == state->sdrRangeHelp);
}

const wchar_t* SettingsHelpText(SettingsHelpTopic topic, bool english) {
    if (topic == SettingsHelpTopic::Capture40) {
        // Frame generation is only an external-app example, not an integration.
        return english
            ? L"40 fps capture\n"
              L"• Leave off for normal viewing.\n"
              L"• Enable to request 40 fps; disable to restore your setting.\n"
              L"• Support is checked on Start. No fallback to another FPS.\n"
              L"\n"
              L"Keep in mind\n"
              L"• Does not lock game FPS or replace VRR.\n"
              L"\n"
              L"Example: Lossless Scaling (LS)\n"
              L"Console 120 Hz → LLCV 40 fps → LS 3x → Monitor 120 Hz\n"
              L"• Assumes steady 40 fps. At 30–40 fps, stutter may persist or become more noticeable with LS.\n"
              L"• LS is separate; configure it and refresh rate yourself.\n"
              L"• May add latency or artifacts. If worse, disable and compare."
            : L"40fps 캡처\n"
              L"• 일반적인 시청에는 끈 채 사용하세요.\n"
              L"• 체크하면 40fps 요청, 해제하면 기존 설정으로 돌아갑니다.\n"
              L"• 지원은 시작할 때 확인하며, 다른 FPS로 대체하지 않습니다.\n"
              L"\n"
              L"주의사항\n"
              L"• 게임 FPS 고정·VRR 대체 기능이 아닙니다.\n"
              L"\n"
              L"활용 예시: Lossless Scaling (LS)\n"
              L"콘솔 120Hz → LLCV 40fps → LS 3배 → 모니터 120Hz\n"
              L"• 40fps 유지 시 예시입니다. 30~40fps로 흔들리면 LS를 써도 끊김이 남거나 더 두드러질 수 있습니다.\n"
              L"• LS는 별도 앱입니다. 주사율·LS는 직접 설정하세요.\n"
              L"• 지연·화면 왜곡이 생길 수 있습니다. 더 끊기면 끄고 비교하세요.";
    }
    if (english) {
        switch (topic) {
        case SettingsHelpTopic::Drift:
            return L"Preventing audio tearing · deciding whether correction is needed\n\n"
                    L"The capture and output device clocks can run at slightly different rates. "
                    L"Auto mode watches the application PCM queue first and enables resampling only "
                    L"when a sustained imbalance is detected. It stays enabled for the rest of the "
                    L"session once triggered, avoiding repeated on/off clicks. Check the Tab OSD for "
                    L"10–30 minutes.\n\n"
                   L"'Stable · correction unnecessary' or 'Rare errors · Off can be kept' means "
                   L"you can leave it Off when the audio is clean. If 'Repeated imbalance · "
                    L"correction recommended' continues, choose Auto. Do not judge "
                   L"from errors immediately after startup.\n\n"
                   L"The resampler and PCM safety buffer are independent. 'Resampler correction "
                   L"limit approaching' indicates clock difference; 'Possible PCM buffer shortage' "
                   L"indicates a momentary lack of queued audio; 'Capture packet delay detected' "
                   L"indicates a late input callback. If the resampler is healthy but underruns "
                   L"continue, raise the PCM buffer target first.\n\n"
                   L"The imbalance ppm shown in the OSD is an estimate from accumulated underrun/"
                    L"overrun frames, not a direct hardware-clock measurement. When Auto activates, "
                    L"the resampler adds a small amount of audio buffering and changes PCM samples. "
                    L"Off always preserves the original PCM path; On always uses the resampler.";
        case SettingsHelpTopic::PcmQueue:
            return L"PCM buffer target\n\n"
                   L"The amount of captured audio kept inside the application before playback.\n"
                   L"10 ms is minimum latency, 15 ms is the low-latency target, 20 ms is a stability "
                   L"target, 25 ms is the recommended default, and 30 ms prioritizes stability.\n\n"
                   L"Higher values absorb more scheduling jitter but add the same amount of audio "
                   L"latency. This is independent of the WASAPI output buffer and clock-drift correction.";
        case SettingsHelpTopic::Presentation:
            return llcv::presentation_ui::HelpText(true);
        case SettingsHelpTopic::VolumeBoost:
            return L"Volume boost above 100%\n\n"
                   L"Allows the mouse wheel to raise the app volume up to 200%. "
                   L"100% is the original PCM level; values above it apply digital gain only inside this app.\n\n"
                   L"No audio buffer or frame queue is added, so this option does not add audio latency. "
                   L"At high source volumes, boosting can clip peaks and cause distortion. Keep it off unless "
                   L"the capture audio is genuinely too quiet.";
        case SettingsHelpTopic::ForceHdr10:
            return L"Force HDR10 output\n\n"
                   L"P010 without transfer/gamut metadata already defaults to BT.2020/PQ HDR10; "
                   L"you do not need to enable this option for missing metadata alone. Explicit SDR metadata "
                   L"is rejected when this option is off: P010 is HDR-only. Select NV12/YUY2 for SDR.\n\n"
                   L"Use this override only for confirmed PQ/BT.2020 input with incorrect or incomplete color "
                   L"metadata. Range and chroma validation still apply. It does not convert SDR into HDR. "
                   L"For an SDR source without color metadata, select NV12/YUY2 instead. Enable Windows HDR "
                   L"on the viewing monitor. No frame queue is added.";
        case SettingsHelpTopic::HdrChroma:
            return L"HDR chroma placement\n\n"
                   L"Auto follows the device metadata and rejects unsupported placements. Use Top-left or Left "
                   L"only for a P010 HDR device with missing or incorrect chroma metadata. Compare fine colored "
                   L"edges and text against a reference.\n\n"
                   L"This overrides the declared chroma placement; it does not repair genuinely staggered Cb/Cr "
                   L"planes or guarantee that placement 6 is supported. It does not change brightness, saturation, "
                   L"PQ interpretation or range validation, and adds no frame queue or processing pass. "
                   L"P010 without transfer/gamut metadata already defaults to PQ/BT.2020.";
        case SettingsHelpTopic::MjpegColor:
            return L"MJPEG color matrix\n\n"
                   L"Auto uses decoder metadata first, then DirectShow metadata. Without matrix metadata, "
                   L"it uses BT.709 for HD or BT.601 for SD.\n\n"
                   L"Change this only if MJPEG colors are incorrect. Black and white levels are controlled "
                   L"separately by SDR color range. Playback, screenshots and recordings use the same interpretation.";
        case SettingsHelpTopic::SdrRange:
            return L"SDR color range\n\n"
                   L"Auto keeps the existing behavior: follow color metadata, or use Limited for NV12/YUY2 "
                   L"and Full for MJPEG when range metadata is missing.\n\n"
                   L"Select Limited or Full only to correct misreported input levels. This changes input "
                   L"interpretation, not the HDMI output of your console. Playback, screenshots and recordings "
                   L"use the same range. P010/HDR is unaffected. No frame queue or processing pass is added.\n\n"
                   L"An older MJPEG-only correction is retained until you change this selection; then the "
                   L"selected range is shared by all SDR formats.";
        }
    }
    switch (topic) {
    case SettingsHelpTopic::Drift:
         return L"소리 찢어짐 방지 · 보정 필요 확인\n\n"
                L"자동은 프로그램 내부 PCM 대기량을 관찰하다가 클록 불균형이 일정 시간 지속될 때만 "
                L"리샘플링을 켭니다. 한 번 켜지면 세션 중 반복해서 켰다 끄지 않아 소리 변화와 클릭을 "
                L"줄입니다. Tab OSD를 10~30분 확인하세요.\n\n"
               L"'안정 · 보정 불필요' 또는 '드문 오류 · 끔 유지 가능'이면 소리에 문제가 "
                L"없는 한 끔을 유지해도 됩니다. '반복 불균형 · 보정 권장'이 계속 보이면 자동을 "
                L"선택하세요. 시작 직후 오류만으로 판단하지 마세요.\n\n"
               L"리샘플러와 PCM 안전 대기량은 서로 독립입니다. '리샘플러 보정 한계 접근'은 "
               L"클록 차이, 'PCM 버퍼 부족 가능'은 순간 버퍼 여유 부족, '캡처 패킷 지연 감지'는 "
               L"입력 콜백 지연을 뜻합니다. 리샘플러가 정상인데 underrun이 나면 PCM 버퍼 "
               L"목표를 먼저 높이세요.\n\n"
                L"OSD의 불균형 ppm은 누적 underrun/overrun으로 계산한 참고값이며 실제 하드웨어 "
                L"클록을 직접 측정한 값은 아닙니다. 자동이 작동하면 작은 오디오 대기량을 추가하고 "
                L"PCM 샘플을 변경합니다. 끔은 원본 PCM을 유지하고, 켬은 항상 리샘플러를 사용합니다.";
    case SettingsHelpTopic::PcmQueue:
        return L"PCM 버퍼 목표 안내\n\n"
               L"캡처 오디오를 재생 전에 확보하는 프로그램 내부 대기량입니다.\n"
               L"10ms는 최저 지연, 15ms는 저지연 목표, 20ms는 안정 목표, 25ms는 권장 기본값, "
               L"30ms는 안정성 우선 설정입니다.\n\n"
               L"값을 높이면 순간적인 입력 지연을 흡수할 여유가 커지지만, 그만큼 오디오 지연이 "
               L"늘어납니다. WASAPI 출력 버퍼와 클록 드리프트 보정과는 독립적으로 조정됩니다.";
    case SettingsHelpTopic::Presentation:
        return llcv::presentation_ui::HelpText(false);
    case SettingsHelpTopic::VolumeBoost:
        return L"100% 이상 볼륨 증폭 안내\n\n"
               L"마우스 휠로 앱 음량을 최대 200%까지 올릴 수 있게 합니다. 100%는 원본 PCM 크기이고, "
               L"그 이상은 이 앱 안에서만 디지털 증폭을 적용합니다.\n\n"
               L"추가 오디오 버퍼나 프레임 큐를 만들지 않으므로 오디오 지연은 늘지 않습니다. 다만 원본 "
               L"소리가 이미 큰 경우에는 피크가 잘려 왜곡될 수 있으니, 실제로 음량이 부족할 때만 켜세요.";
    case SettingsHelpTopic::ForceHdr10:
        return L"HDR10 강제 출력 안내\n\n"
               L"전달 함수·색역 정보가 없는 P010은 기본적으로 BT.2020/PQ HDR10으로 해석하므로, "
               L"정보가 없다는 이유만으로 이 옵션을 켤 필요는 없습니다. P010은 HDR 전용이므로, 끈 상태에서 SDR로 보고된 입력은 차단합니다. SDR은 NV12/YUY2를 선택하세요.\n\n"
               L"입력이 실제 PQ/BT.2020인데 색 정보가 잘못되었거나 불완전한 경우에만 강제로 해석하세요. "
               L"색 범위·색차 배치 검증은 유지되며 SDR을 HDR로 변환하지는 않습니다. "
               L"색 정보가 없는 SDR 입력은 NV12/YUY2를 선택하세요. 표시 모니터의 Windows HDR을 켜야 합니다. "
               L"프레임 큐는 추가하지 않습니다.";
    case SettingsHelpTopic::HdrChroma:
        return L"HDR 색차 배치 안내\n\n"
               L"자동은 장치 메타데이터를 따르며 지원하지 않는 배치는 차단합니다. P010 HDR 장치의 "
               L"색차 배치 정보가 없거나 잘못된 경우에만 Top-left 또는 Left를 선택하고, 가는 색 경계와 "
               L"글자를 기준 화면과 비교하세요.\n\n"
               L"이 옵션은 배치 정보의 해석을 바꿉니다. 실제 Cb/Cr가 서로 어긋난 데이터를 복원하거나 "
               L"배치 값 6의 지원을 보장하지 않습니다. 밝기·채도·PQ 해석·색 범위 검증은 바꾸지 않으며 "
               L"프레임 큐나 처리 단계를 추가하지 않습니다. 전달 함수·색역 정보가 없는 P010은 기본적으로 PQ/BT.2020으로 해석합니다.";
    case SettingsHelpTopic::MjpegColor:
        return L"MJPEG 색 공간 안내\n\n"
               L"자동은 디코더 메타데이터를 먼저 사용하고, 없으면 DirectShow 정보를 확인합니다. 색 공간 "
               L"정보가 없으면 HD는 BT.709, SD는 BT.601을 사용합니다.\n\n"
               L"MJPEG 색상이 잘못된 경우에만 변경하세요. 검정·흰색 명암 범위는 별도의 SDR 색 범위에서 "
               L"설정합니다. 재생·스크린샷·녹화에 같은 해석을 적용합니다.";
    case SettingsHelpTopic::SdrRange:
        return L"SDR 색 범위 안내\n\n"
               L"자동은 기존 동작을 유지합니다. 색 범위 메타데이터를 따르고, 정보가 없으면 NV12/YUY2는 "
               L"Limited, MJPEG는 Full로 해석합니다.\n\n"
               L"입력의 색 범위 정보가 잘못된 경우에만 Limited 또는 Full을 선택하세요. 콘솔의 HDMI "
               L"출력을 바꾸는 설정은 아닙니다. 재생·스크린샷·녹화에 같은 범위를 적용하며, P010/HDR에는 "
               L"적용하지 않습니다. 프레임 큐나 처리 단계를 추가하지 않습니다.\n\n"
               L"구버전의 MJPEG 전용 보정은 이 항목을 변경하기 전까지 유지됩니다. 직접 선택하면 모든 "
               L"SDR 형식에 공통으로 적용합니다.";
    }
    return L"";
}

} // namespace llcv::settings_ui
