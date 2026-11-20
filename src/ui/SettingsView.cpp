#include "SettingsView.h"
#include "SettingsDialogControls.h"
#ifdef LLCV_RECORDING
#include "recording/RecordingSettingsView.h"
#endif
#include "SettingsFonts.h"
#include "PresentationModeUi.h"
#include "settings/AppSettings.h"
#ifdef LLCV_RIFE_TEST
#include "video/RifeHalfTest.h"
#endif
#ifdef LLCV_INTERPOLATION_RELEASE
#include "video/VfgRuntimeInstall.h"
#endif
#ifdef LLCV_RIFE_VULKAN_BUNDLED
#include "video/InterpolationMethodPolicy.h"
#include "video/PresentationPolicy.h"
#include "ui/PresentationModeUi.h"
#endif

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
#ifdef LLCV_INTERPOLATION_RELEASE
void RefreshVfgDownloadControl(SettingsControls* state) {
    if (!state || !state->vfgDownloadButton) return;
#ifdef LLCV_RIFE_VULKAN_BUNDLED
    if (state->interpolationMethod == settings::InterpolationMethod::RifeVulkan) {
        SetSettingsControlVisible(state->vfgDownloadButton,false);
        return;
    }
#endif
    if (state->ShowVfgGpuSwitch()) {
        SetSettingsText(state->vfgDownloadButton, state->english
            ? L"Windows graphics settings" : L"Windows 그래픽 설정");
        SetSettingsControlVisible(state->vfgDownloadButton, state->activeTab == SettingsTab::Interpolation);
        return;
    }
    const auto status = llcv::vfg_runtime::GetStatus();
    using llcv::vfg_runtime::Stage;
    const auto tr = [state](const wchar_t* ko, const wchar_t* en) { return state->english ? en : ko; };
    const bool canStart = status.stage == Stage::Idle || status.stage == Stage::Failed;
    std::wstring text;
    switch (status.stage) {
    case Stage::Idle: text = tr(L"VFG 구성품 받기", L"Download VFG components"); break;
    case Stage::Downloading:
        text = tr(L"다운로드 중 · ", L"Downloading · ") + std::to_wstring(status.percent) + L"%"; break;
    case Stage::Verifying: text = tr(L"구성품 확인 중", L"Verifying components"); break;
    case Stage::Installing: text = tr(L"설치 중", L"Installing"); break;
    case Stage::Ready: text = tr(L"VFG 구성품 설치됨", L"VFG components installed"); break;
    case Stage::Failed: text = tr(L"설치 실패 · 다시 받기", L"Install failed · Retry"); break;
    }
    SetSettingsText(state->vfgDownloadButton, text.c_str());
    SetSettingsControlVisible(state->vfgDownloadButton, state->activeTab == SettingsTab::Interpolation
        && state->InterpolationGpuAvailable(), canStart);
}
#endif

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
    // Five capture formats plus the heading can need a sixth line. Preserve
    // the existing anchor for shorter summaries, borrowing only footer padding
    // when needed; never overlap the HDR fields ending at 470 DIPs or shrink
    // the type. Even the longest supported summary retains an 8-DIP footer gap.
    constexpr int earliestTop=472;
    const int latestBottom=state->footerTopDip-8;
    height=std::clamp(height,16,std::max(16,latestBottom-earliestTop-extra));
    const int bottom=std::min(latestBottom,std::max(state->footerTopDip-24,earliestTop+height+extra));
    PlaceSettingsControl(state->videoCapabilityStatus,184,bottom-extra-height,376,height,dpi);
    PlaceSettingsControl(state->unlistedFpsStatus,184,bottom-16,376,16,dpi);
}

void RefreshShortcutCaptions(SettingsControls* state) {
    for (size_t i=0;i<state->guideKeys.size();++i)
        SetSettingsText(state->guideKeys[i],viewer_help::KeyLabel(i,state->shortcutBindings,state->english).c_str());
    SetSettingsText(state->guideVideoHint,state->english
        ? L"Click a key to change\n* Video mode only"
        : L"키 버튼을 눌러 변경\n* 영상 모드 전용");
    for (const auto& [control,source]:state->shortcutCaptionTemplates)
        SetSettingsText(control,shortcuts::Expand(source.c_str(),state->shortcutBindings).c_str());
    SetSettingsText(state->vsrStatus,shortcuts::Expand(L"SDR / HDR10 · VSR: F6",state->shortcutBindings).c_str());
    if (!state->uiFonts.empty()) LayoutSettingsControls(state,state->layoutDpi);
}
ShortcutLayout MeasureShortcutLayout(const SettingsControls* state,UINT dpi,int rowWidth) {
    ShortcutLayout result;
    dpi=dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    HDC dc=GetDC(nullptr);
    if (!dc) return result;
    const auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(state->guideKeys[0],WM_GETFONT,0,0)));
    for (size_t i=0;i<state->shortcutBindings.size();++i) {
        const auto label=viewer_help::KeyLabel(i,state->shortcutBindings,state->english);
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
    SetSettingsText(state->guideKeys[index],state->english ? L"Press key" : L"키 입력");
#ifdef LLCV_RIFE_OPTIONAL
    if (index==static_cast<size_t>(shortcuts::Action::HudCorrection)) {
        SetSettingsText(state->guideVideoHint,state->english
            ? L"Press a key to assign. Backspace clears it.\nClick the key button again to cancel."
            : L"새 키를 입력합니다. Backspace로 배정을 해제합니다.\n키 버튼을 다시 누르면 취소합니다.");
        return;
    }
#endif
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
#ifdef LLCV_RIFE_OPTIONAL
    if (index==static_cast<size_t>(shortcuts::Action::HudCorrection) && key==VK_BACK && !modifiers) {
        state->shortcutBindings[index]=0;
        state->shortcutKeyToRelease=key;
        CancelShortcutEdit(state);
        return true;
    }
#endif
    const unsigned binding=key|modifiers;
    if (!shortcuts::Valid(binding)) {
        SetSettingsText(state->guideVideoHint,state->english
            ? L"Reserved or unsupported key. Try another."
            : L"예약되었거나 지원하지 않는 키입니다.\n다른 키를 입력하면 변경됩니다.");
        return true;
    }
    for (size_t i=0;i<shortcuts::Count;++i) if (i!=index && state->shortcutBindings[i]==binding) {
        const auto& other=viewer_help::kShortcuts[i];
        const std::wstring reason=state->english
            ? std::wstring(L"Already assigned: ")+other.english+L"\nPress a different key."
            : std::wstring(L"이미 사용 중인 키: ")+other.korean+L"\n다른 키를 입력하면 변경됩니다.";
        SetSettingsText(state->guideVideoHint,reason.c_str());
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
#ifdef LLCV_RIFE_AUTO_UI_TEST
    SettingsTab::Interpolation,
#endif
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
#ifdef LLCV_RIFE_AUTO_UI_TEST
    case SettingsTab::Interpolation:
        title = state->english ? L"Frame interpolation" : L"프레임 보간";
        subtitle = state->english ? L"Private RIFE comparison · Session-only settings." : L"RIFE 비교판 · 설정은 이번 실행에만 적용됩니다.";
#ifdef LLCV_RIFE_OPTIONAL
#ifdef LLCV_INTERPOLATION_RELEASE
        subtitle=state->english?L"Choose whether to use NVIDIA VFG during playback.":L"재생 중 NVIDIA VFG를 사용할지 선택합니다.";
#else
        subtitle=state->english?L"Enable components for this session, then toggle interpolation with F7.":L"이번 실행에서 구성품을 사용하려면 체크하세요. 이후 F7로 보간을 전환합니다.";
#endif
#endif
#ifdef LLCV_FLYHIGH_TEST
        subtitle=state->english?L"FlyHighFG / RIFE comparison · Session-only settings.":L"FlyHighFG / RIFE 비교판 · 설정은 이번 실행에만 적용됩니다.";
#endif
        break;
#endif
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
#ifdef LLCV_RIFE_VULKAN_BUNDLED
                         state->rifeCaptureHint,
#endif
                         state->audioStatus,
#ifndef LLCV_DOLBY_CAPTURE
                         state->surround51Hint,
#endif
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
int SettingsCaptionWidthDip(HWND checkbox, UINT dpi) {
    if (!checkbox) return 0;
    wchar_t text[512]{};
    GetWindowTextW(checkbox, text, ARRAYSIZE(text));
    HDC hdc = GetDC(checkbox);
    if (!hdc) return 0;
    const HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(checkbox, WM_GETFONT, 0, 0));
    const HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
    SIZE size{};
    GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &size);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(checkbox, hdc);
    dpi=dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    return static_cast<int>((size.cx*96+dpi-1)/dpi);
}
static int SettingsCheckboxWidthDip(HWND checkbox, UINT dpi) {
    // Checkbox glyph plus caption.  Keeping the HWND no wider than this is
    // important: a wide checkbox would overlap a nearby help button and
    // steal its clicks even when the button looks visually separate.
    return std::min(29 + SettingsCaptionWidthDip(checkbox,dpi), 430);
}

static void LayoutVideoHeadingControls(SettingsControls* state,UINT dpi) {
    if(SettingsCompactLayout(state))return;
    const auto heading=[dpi](HWND label,HWND help,int y,int helpY) {
        PlaceSettingsControl(label,600,y,340,20,dpi);
        PlaceSettingsControl(help,952,helpY,24,24,dpi);
    };
    heading(state->presentationLabel,state->presentationHelp,136,134);
    // Center the 24-DIP button between the preceding field (ends at 386)
    // and its own field (starts at 420), leaving five DIPs on either side.
    heading(state->upscalerLabel,state->vsrGuideButton,392,391);
    PlaceSettingsControl(state->vsrStatus,600,460,376,30,dpi);
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
#ifdef LLCV_DOLBY_CAPTURE
    place(state->surround51Hint,600,336,340,20);
    place(state->surroundHelp,952,334,24,24);
    place(state->surround51Check,600,364,376,160);
    SendMessageW(state->surround51Check,CB_SETMINVISIBLE,3,0);
#else
    place(state->surround51Check, 600, 336, 376, 32);
    place(state->surround51Hint, 600, 376, 376, 88);
#endif
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
#ifdef LLCV_RIFE_VULKAN_BUNDLED
    place(state->rifeCaptureResolution,184,292,180,120);
    place(state->rifeCaptureFps,380,292,180,200);
    SendMessageW(state->rifeCaptureFps, CB_SETDROPPEDWIDTH, SettingsPixels(250, dpi), 0);
    place(state->rifeCaptureHint,184,state->footerTopDip-68,376,44);
#endif
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
    place(state->presentationCombo, 600, 164, 376, 120);
#ifdef LLCV_INTERPOLATION_RELEASE
    place(state->displayMonitorLabel, 600, 200, 180, 20);
    place(state->displayMonitorCombo, 600, 228, 180, 180);
    place(state->playbackGpuLabel, 796, 200, 180, 20);
    place(state->playbackGpuCombo, 796, 228, 180, 180);
    SendMessageW(state->playbackGpuCombo, CB_SETDROPPEDWIDTH, SettingsPixels(360, dpi), 0);
#else
    place(state->displayMonitorLabel, 600, 200, 376, 20);
    place(state->displayMonitorCombo, 600, 228, 376, 180);
#endif
    SendMessageW(state->displayMonitorCombo, CB_SETDROPPEDWIDTH, SettingsPixels(560, dpi), 0);
    place(state->pixelCheck, 800, 326, 176, 24);
    place(state->scalingLabel, 600, 264, 376, 20);
    place(state->scalingCombo, 600, 292, 376, 120);
    // Keep the existing field rhythm; tuning details have their own row.
    place(state->displayResolutionLabel, 600, 328, 188, 20);
    place(state->displayResolutionCombo, 600, 356, 376, 150);
    place(state->upscalerCombo, 600, 420, 376, 150);
    place(state->cmaaQualityLabel, 600, 456, 180, 20);
    place(state->cmaaQualityCombo, 600, 484, 180, 140);
    place(state->nisSharpnessLabel, 796, 456, 180, 20);
    place(state->nisSharpnessCombo, 796, 484, 180, 230);
#ifdef LLCV_RIFE_TEST
#ifdef LLCV_RIFE_AUTO_UI_TEST
    place(state->rifeHalfCheck,184,120,376,32);
#if defined(LLCV_INTERPOLATION_RELEASE)
    place(state->rifeScaleLabel,184,176,340,20);
#else
    place(state->rifeScaleLabel,184,176,376,20);
#endif
    place(state->rifeScaleCombo,184,204,376,160);
#if defined(LLCV_INTERPOLATION_RELEASE) && !defined(LLCV_RIFE_VULKAN_BUNDLED)
    place(state->interpolationQualityHelp,536,174,24,24);
#elif defined(LLCV_FLYHIGH_TEST) || defined(LLCV_RIFE_OPTIONAL)
    place(state->interpolationProviderLabel,184,176,376,20);
    place(state->interpolationProviderCombo,184,204,376,160);
#ifdef LLCV_RIFE_OPTIONAL
    place(state->rifeScaleLabel,600,176,340,20);
    place(state->interpolationQualityHelp,952,174,24,24);
#else
    place(state->rifeScaleLabel,600,176,376,20);
#endif
    place(state->rifeScaleCombo,600,204,376,160);
#endif
#ifdef LLCV_INTERPOLATION_RELEASE
    place(state->vfgDownloadButton,600,120,280,32);
#ifdef LLCV_RIFE_VULKAN_BUNDLED
    place(state->rifeAutoUiLabel,600,176,376,20);
    place(state->rifeAutoUiCombo,600,204,376,160);
#endif
    place(state->rifeQualityCheck,184,256,640,32);
#ifdef LLCV_RIFE_VULKAN_BUNDLED
    place(state->rifeAutoUiHints[0],184,304,792,48);
    place(state->rifeAutoUiHints[2],184,370,792,90);
#else
    place(state->rifeAutoUiHints[0],184,304,792,28);
    place(state->rifeAutoUiHints[2],184,352,792,90);
#endif
#else
    place(state->rifeQualityCheck,184,256,640,32);
    place(state->rifeAutoUiLabel,184,320,376,20);
    place(state->rifeAutoUiCombo,184,348,376,160);
#ifdef LLCV_RIFE_OPTIONAL
    place(state->rifeAutoUiHints[0],184,396,792,24);
    place(state->rifeAutoUiHints[1],184,424,792,24);
    place(state->rifeAutoUiHints[2],184,460,792,90);
#else
    for(size_t i=0;i<state->rifeAutoUiHints.size();++i)place(state->rifeAutoUiHints[i],184,396+int(i)*32,740,28);
#endif
#endif
#else
    place(state->rifeHalfCheck,600,526,148,24);
    place(state->rifeScaleLabel,756,528,64,20);
    place(state->rifeScaleCombo,828,524,148,160);
    SendMessageW(state->rifeScaleCombo,CB_SETDROPPEDWIDTH,SettingsPixels(180,dpi),0);
    place(state->rifeQualityCheck,600,556,376,18);
#endif
#endif
    LayoutVideoHeadingControls(state,dpi);

    // Window behavior has its own page, avoiding a packed video/options wall.
    LayoutWindowBehaviorControls(state);

#ifdef LLCV_RIFE_OPTIONAL
    place(state->guideShortcutsTitle,204,116,166,22);
    place(state->shortcutListToggle,state->showAllShortcuts?204:374,110,166,28);
    place(state->shortcutResetButton,432,110,108,26);
    const auto shortcutLayout=MeasureShortcutLayout(state,dpi,350);
    place(state->guideVideoHint,204,142,350,shortcutLayout.hintHeight);
    int shortcutY=142+shortcutLayout.hintHeight+6;
    for (const auto i : viewer_help::kDisplayOrder) {
        if (!state->ShowShortcut(i)) continue;
        const int height=std::max(state->showAllShortcuts?27:36,shortcutLayout.descriptionHeights[i]);
        place(state->guideKeys[i],204,shortcutY+(height-26)/2,shortcutLayout.keyWidth,26);
        place(state->guideDescriptions[i],216+shortcutLayout.keyWidth,shortcutY,338-shortcutLayout.keyWidth,height);
        shortcutY+=height+3;
    }
#else
    place(state->guideShortcutsTitle, 204, 120, 216, 20);
    place(state->shortcutResetButton, 432, 118, 108, 24);
    const auto shortcutLayout=MeasureShortcutLayout(state,dpi);
    const int shortcutStep=state->guideKeys.size()>10 ? 32 : state->guideKeys.size()>9 ? 35 : 36;
    constexpr int shortcutKeyHeight=29;
    constexpr int shortcutTop=156;
    int shortcutY=shortcutTop;
    for (const auto i : viewer_help::kDisplayOrder) {
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
#endif
    place(state->guideDiagnosticsTitle, 600, 120, 376, 20);
    place(state->guideDiagnosticsText, 600, 148, 376, 32);
    place(state->saveLogCheck, 600, 196, 376, 32);
    place(state->guideReportButton, 600, 240, 185, 32);
    place(state->guideLogFolderButton, 801, 240, 175, 32);
    place(state->showConsoleCheck, 600, 300, 376, 32);
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
    place(state->updateTitle, 184, 328, 376, 24);
    place(state->updateText, 184, 360, 376, 44);
    place(state->checkForUpdatesCheck, 184, 416, 376, 32);
    place(state->updateNowButton, 184, 460, 180, 32);
    place(state->releaseNotesButton, 380, 460, 180, 32);
    place(state->updateStatus, 184, 504, 792, 48);
    // Screenshot preferences belong to the app page, not the display pipeline.
    place(state->screenshotTitle, 600, 328, 376, 24);
    place(state->screenshotClipboardCheck, 600, 362, 182, 26);
    place(state->screenshotFolderButton, 794, 360, 182, 30);
    place(state->screenshotHelp, 600, 408, 376, 18);
    for (HWND combo : {state->audioCombo, state->audioOutputCombo,
#ifdef LLCV_DOLBY_CAPTURE
                        state->surround51Check,
#endif
                        state->bufferCombo, state->volumeHudCombo,
                        state->driftCombo, state->pcmQueueCombo,
                        state->captureDeviceCombo, state->captureAudioDeviceCombo,
                        state->videoCombo, state->displayResolutionCombo, state->frameRateCombo,
#ifdef LLCV_RIFE_VULKAN_BUNDLED
                        state->rifeCaptureResolution,state->rifeCaptureFps,
#endif
                        state->upscalerCombo, state->cmaaQualityCombo, state->nisSharpnessCombo,
#ifdef LLCV_RIFE_AUTO_UI_TEST
#ifdef LLCV_INTERPOLATION_RELEASE
                        state->playbackGpuCombo,
#endif
                        state->rifeScaleCombo, state->rifeAutoUiCombo,
#if defined(LLCV_FLYHIGH_TEST) || defined(LLCV_RIFE_OPTIONAL)
                        state->interpolationProviderCombo,
#endif
#endif
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

#ifdef LLCV_RIFE_VULKAN_BUNDLED
bool SettingsRifeCaptureOverride(const SettingsControls& state, settings::VideoPixelFormat format) {
    const auto mode=presentation_ui::ModeFromSelection(
        static_cast<int>(SendMessageW(state.presentationCombo,CB_GETCURSEL,0,0)));
    const bool audioOnly=SendMessageW(state.audioOnlyCheck,BM_GETCHECK,0,0)==BST_CHECKED;
    const auto provider=interpolation::ChooseProvider(state.interpolationMethod,state.InterpolationGpuAvailable(),true);
    return !presentation::UsesVSync(mode) &&
        interpolation::UseRifeCaptureOverride(provider,state.interpolationEnabled,audioOnly,format);
}

bool SettingsRifeCaptureOverride(const SettingsControls& state) {
    const auto row=SendMessageW(state.pixelFormatCombo,CB_GETCURSEL,0,0);
    const auto format=row==CB_ERR?CB_ERR:SendMessageW(state.pixelFormatCombo,CB_GETITEMDATA,row,0);
    return SettingsRifeCaptureOverride(state,format==CB_ERR?settings::VideoPixelFormat::Auto:
        static_cast<settings::VideoPixelFormat>(format));
}
#endif

static bool HasSplitResolution(const SettingsControls& state) {
    const bool matchesCapture=DisplayResolutionMatchesCapture(state.displayResolutionCombo);
    const LRESULT capture = SendMessageW(state.videoCombo, CB_GETCURSEL, 0, 0);
    const LRESULT display = SendMessageW(state.displayResolutionCombo, CB_GETCURSEL, 0, 0);
#ifdef LLCV_RIFE_VULKAN_BUNDLED
    if(SettingsRifeCaptureOverride(state)){
        const LRESULT fhd=SendMessageW(state.videoCombo,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(L"1920 x 1080"));
        const LRESULT effectiveDisplay=matchesCapture?capture:display;
        if(fhd!=CB_ERR&&effectiveDisplay>=0)return fhd!=effectiveDisplay;
    }
#endif
    return !matchesCapture && capture >= 0 && display >= 0 && capture != display;
}

#ifdef LLCV_RIFE_OPTIONAL
void UpdateInterpolationAvailability(SettingsControls* state) {
    const bool page=state->activeTab==SettingsTab::Interpolation;
#ifdef LLCV_RIFE_VULKAN_BUNDLED
    const auto resolved=interpolation::ChooseProvider(state->interpolationMethod,state->InterpolationGpuAvailable(),true);
    const bool showVfg=resolved==interpolation::Provider::NvidiaVfg;
    const bool showRife=resolved==interpolation::Provider::RifeVulkan;
    const auto tr=[state](const wchar_t* ko,const wchar_t* en){return state->english?en:ko;};
    SetSettingsText(state->rifeHalfCheck,tr(L"프레임 보간 사용",L"Enable frame interpolation"));
    SetSettingsText(state->interpolationProviderLabel,tr(L"보간 방식",L"Interpolation method"));
    SetSettingsText(state->rifeAutoUiLabel,tr(L"HUD 보호",L"HUD protection"));
    SendMessageW(state->interpolationProviderCombo,CB_SETCURSEL,
        state->interpolationMethod==settings::InterpolationMethod::NvidiaVfg?1:0,0);
    SendMessageW(state->rifeHalfCheck,BM_SETCHECK,state->interpolationEnabled?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(state->rifeAutoUiCombo,CB_SETCURSEL,state->rifeHudMode,0);
    for(HWND child:{state->rifeHalfCheck,state->interpolationProviderLabel,state->interpolationProviderCombo})
        SetSettingsControlVisible(child,page);
    for(HWND child:{state->rifeScaleLabel,state->rifeScaleCombo,state->rifeQualityCheck,state->interpolationQualityHelp})
        SetSettingsControlVisible(child,page&&showVfg);
    for(HWND child:{state->rifeAutoUiLabel,state->rifeAutoUiCombo})SetSettingsControlVisible(child,page&&showRife);
    SetSettingsText(state->rifeScaleLabel,tr(L"품질",L"Quality"));
    SetSettingsText(state->rifeQualityCheck,tr(L"생성 프레임 완료 대기 (지연 증가)",L"Wait for generated frames (increases latency)"));
    if(SendMessageW(state->rifeScaleCombo,CB_GETCOUNT,0,0)!=2){
        SendMessageW(state->rifeScaleCombo,CB_RESETCONTENT,0,0);
        for(auto scale:{rife_test::Scale::VfgLow,rife_test::Scale::VfgMedium}){
            const auto label=rife_test::PresetLabel(scale,state->english);
            const auto idx=SendMessageW(state->rifeScaleCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(wcschr(label,L'·')+2));
            SendMessageW(state->rifeScaleCombo,CB_SETITEMDATA,idx,static_cast<LPARAM>(scale));
        }
    }
    SendMessageW(state->rifeScaleCombo,CB_SETCURSEL,state->interpolationVfgLow?0:1,0);
    SendMessageW(state->rifeQualityCheck,BM_SETCHECK,state->interpolationVfgWait?BST_CHECKED:BST_UNCHECKED,0);
    const wchar_t* hint=showRife
        ? tr(L"RIFE Vulkan은 저지연 출력에서 FHD SDR로 보간합니다. 픽셀 포맷은 유지하고, P010 HDR은 보간하지 않습니다.\r\n"
             L"다른 방식으로 바꾸거나 보간 사용을 끄면 다음 시작 때 저장한 입력으로 돌아갑니다.",
             L"RIFE Vulkan uses FHD SDR in low-latency output. The selected pixel format is kept. P010 HDR is not interpolated.\r\n"
             L"Switch methods or disable interpolation to restore your saved input on the next start.")
        : showVfg ? tr(L"NVIDIA VFG · FHD / QHD / 4K UHD SDR",L"NVIDIA VFG · FHD / QHD / 4K UHD SDR")
        : tr(L"현재 재생 GPU에서는 VFG를 사용할 수 없습니다.",L"VFG is not available on the current playback GPU.");
    SetSettingsText(state->rifeAutoUiHints[0],hint);
    RefreshVfgDownloadControl(state);
#else
    const bool enabled=state->InterpolationGpuAvailable()&&llcv::rife_test::componentsEnabled.load();
#ifdef LLCV_INTERPOLATION_RELEASE
    const bool hud=false;
#else
    const bool hud=enabled&&!llcv::rife_test::IsVfg(llcv::rife_test::selectedScale.load());
#endif
    for(HWND child:{state->interpolationProviderLabel,state->interpolationProviderCombo,
                    state->rifeScaleLabel,state->rifeScaleCombo,state->rifeQualityCheck})
        SetSettingsControlVisible(child,page,enabled);
    for(HWND child:{state->rifeAutoUiLabel,state->rifeAutoUiCombo})
#ifdef LLCV_INTERPOLATION_RELEASE
        SetSettingsControlVisible(child,page&&hud,hud);
#else
        SetSettingsControlVisible(child,page,hud);
#endif
#endif
}
#endif
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
#ifdef LLCV_RIFE_VULKAN_BUNDLED
    const bool rifeInput=SettingsRifeCaptureOverride(*state);
#else
    constexpr bool rifeInput=false;
#endif
    if (!video || SettingsCompactLayout(state) || rifeInput) state->frameOptionsOpen = false;
    const bool flyout = video && state->frameOptionsOpen;
    const bool frameOptions = video && !rifeInput && (flyout || SettingsCompactLayout(state));
    const bool guide = state->activeTab == SettingsTab::GuideDiagnostics;
    const bool updates = state->activeTab == SettingsTab::Updates;
#if defined(LLCV_RIFE_OPTIONAL) && !defined(LLCV_INTERPOLATION_RELEASE)
    SetSettingsText(state->rifeAutoUiHints[1],llcv::rife_test::IsVfg(llcv::rife_test::selectedScale.load())
        ? (state->english ? L"HUD correction is only available with RIFE." : L"HUD 보정은 RIFE에서만 사용합니다.") : L"");
#endif
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
#ifdef LLCV_DOLBY_CAPTURE
    SetSettingsControlVisible(state->surroundHelp,audio);
#endif
    SetSettingsControlVisible(state->surround51Check, audio, shared);
    SetSettingsControlVisible(state->surround51Hint, audio, shared);
#ifdef LLCV_INTERPOLATION_RELEASE
    SetSettingsControlVisible(state->playbackGpuLabel, video);
    SetSettingsControlVisible(state->playbackGpuCombo, video);
#endif
    for (HWND control : {state->videoCaptureSection,
                         state->videoRefreshButton,
                         state->videoDisplaySection,
                         state->presentationLabel, state->presentationHelp,
                         state->presentationCombo, state->displayMonitorLabel,
                         state->displayMonitorCombo, state->captureDeviceLabel,
                         state->captureAudioDeviceLabel, state->videoLabel,
                         state->pixelFormatLabel, state->frameRateLabel,
                         state->pixelCheck}) {
        const bool obscured = control == state->pixelFormatLabel || control == state->videoCapabilityStatus;
        SetSettingsControlVisible(control, video && !(flyout && obscured));
    }
    SetSettingsControlVisible(state->videoCombo,video&&!rifeInput);
    SetSettingsControlVisible(state->videoCapabilityStatus,video&&!flyout&&!rifeInput);
    SetSettingsControlVisible(state->captureDeviceCombo, video, availability.captureDevice);
    SetSettingsControlVisible(state->pixelFormatCombo, video && !flyout, availability.formats);
    const bool capture40 = SendMessageW(state->capture40Check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    SetSettingsControlVisible(state->frameRateCombo, video&&!rifeInput, availability.formats && !capture40);
    SetSettingsControlVisible(state->frameOptionsButton, video && !rifeInput && !SettingsCompactLayout(state));
#ifdef LLCV_RIFE_VULKAN_BUNDLED
    SetSettingsControlVisible(state->rifeCaptureResolution,video&&rifeInput,false);
    SetSettingsControlVisible(state->rifeCaptureFps,video&&rifeInput,availability.formats);
    SetSettingsControlVisible(state->rifeCaptureHint,video&&rifeInput);
#endif
    SetSettingsText(state->frameOptionsButton, state->english
        ? (flyout ? L"More ▴" : L"More ▾") : (flyout ? L"추가 설정 ▴" : L"추가 설정 ▾"));
    for (HWND control : {state->frameOptionsTitle, state->frameOptionsClose, state->frameOptionsHint})
        SetSettingsControlVisible(control, flyout);
    SetSettingsControlVisible(state->capture40Check, frameOptions, availability.formats);
    SetSettingsControlVisible(state->unlistedFpsCheck, frameOptions, availability.formats && !capture40);
    SetSettingsControlVisible(state->unlistedFpsStatus, video && !flyout && !rifeInput &&
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
    const bool editingShortcuts=!state->shortcutListToggle || state->showAllShortcuts;
    if (state->shortcutListToggle) {
        SetSettingsText(state->shortcutListToggle,state->showAllShortcuts
            ? (state->english?L"Back to quick guide":L"간단히 보기")
            : (state->english?L"Edit all shortcuts":L"전체 단축키 편집"));
        SetSettingsControlVisible(state->shortcutListToggle,guide);
    }
    SetSettingsControlVisible(state->guideShortcutsTitle,guide && !state->showAllShortcuts);
    SetSettingsControlVisible(state->shortcutResetButton,guide && editingShortcuts);
    SetSettingsControlVisible(state->guideVideoHint, guide);
    for (size_t i=0;i<state->guideKeys.size();++i) {
        const bool visible=guide && state->ShowShortcut(i);
        SetSettingsControlVisible(state->guideKeys[i],visible);
        SetSettingsControlVisible(state->guideDescriptions[i],visible);
    }
    SetSettingsControlVisible(state->guideDiagnosticsTitle, guide);
    SetSettingsControlVisible(state->guideDiagnosticsText, guide);
    SetSettingsControlVisible(state->guideLogFolderButton, guide);
    SetSettingsControlVisible(state->guideReportButton, guide, !state->reportPending);
    SetSettingsControlVisible(state->screenshotTitle, updates);
    if (state->vsrGpuUnavailable && SendMessageW(state->upscalerCombo,CB_GETCURSEL,0,0)==2)
        SendMessageW(state->upscalerCombo,CB_SETCURSEL,0,0);
    const bool cmaaNis=SendMessageW(state->upscalerCombo,CB_GETCURSEL,0,0)==1;
    // Keep option indices and the saved SDR preference stable across formats.
    // Do not report CMAA2 as active when HDR runs NIS without AA.
    wchar_t method[128]{};
    SendMessageW(state->upscalerCombo,CB_GETLBTEXT,1,reinterpret_cast<LPARAM>(method));
    const wchar_t* methodLabel=p010Selected?L"NIS (HDR10)":L"CMAA2 → NIS";
    if(std::wcscmp(method,methodLabel)!=0) {
        const auto selection=SendMessageW(state->upscalerCombo,CB_GETCURSEL,0,0);
        SendMessageW(state->upscalerCombo,WM_SETREDRAW,FALSE,0);
        SendMessageW(state->upscalerCombo,CB_DELETESTRING,1,0);
        SendMessageW(state->upscalerCombo,CB_INSERTSTRING,1,reinterpret_cast<LPARAM>(methodLabel));
        SendMessageW(state->upscalerCombo,CB_SETCURSEL,selection,0);
        SendMessageW(state->upscalerCombo,WM_SETREDRAW,TRUE,0);
        InvalidateRect(state->upscalerCombo,nullptr,FALSE);
    }
    SetSettingsText(state->cmaaQualityLabel,p010Selected
        ? (state->english?L"CMAA2 (SDR)":L"CMAA2 보정 (SDR)")
        : (state->english?L"CMAA2 correction":L"CMAA2 보정"));
    LayoutVideoHeadingControls(state,state->layoutDpi);
    SetSettingsControlVisible(state->upscalerLabel, video);
    SetSettingsControlVisible(state->upscalerCombo, video);
#ifdef LLCV_RIFE_TEST
#ifdef LLCV_RIFE_AUTO_UI_TEST
    const bool rifePage=state->activeTab==SettingsTab::Interpolation;
    SetSettingsControlVisible(state->rifeAutoUiLabel,rifePage);
    SetSettingsControlVisible(state->rifeAutoUiCombo,rifePage);
    for(auto hint:state->rifeAutoUiHints)SetSettingsControlVisible(hint,rifePage);
    SetSettingsText(state->rifeAutoUiLabel,state->english?L"Automatic HUD protection (F4)":L"자동 HUD 보호 (F4)");
    SendMessageW(state->rifeAutoUiCombo,CB_SETCURSEL,llcv::rife_test::SelectedHudWidth()==16?1:llcv::rife_test::SelectedHudWidth()==32?2:0,0);
#ifndef LLCV_RIFE_OPTIONAL
#ifdef LLCV_RIFE_TRAJECTORY
    SetSettingsText(state->rifeAutoUiHints[0],state->english?L"Tracks background motion around stationary HUD edges.":L"고정된 HUD 주변에서 배경의 이동 경로를 추적합니다.");
    SetSettingsText(state->rifeAutoUiHints[1],state->english?L"Falls back to ordinary RIFE when uncertain.":L"추적이 불확실하면 기본 RIFE를 유지합니다.");
#ifdef LLCV_RIFE_UHD_PROBE
    SetSettingsText(state->rifeAutoUiHints[2],state->english?L"F4: HUD protection · F7: interpolation · 4K SDR":L"F4: HUD 보정 · F7: 보간 · 4K SDR");
#else
    SetSettingsText(state->rifeAutoUiHints[2],state->english?L"F4: trajectory · F8: Fast / Half / High · FHD SDR":L"F4: 이동 추적 · F8: Fast / Half / High · FHD SDR");
#endif
#else
    SetSettingsText(state->rifeAutoUiHints[0],state->english
        ?L"Learns stationary edges while the background moves (about 1 second)."
        :L"배경이 움직일 때 약 1초 동안 고정된 윤곽을 찾습니다.");
    SetSettingsText(state->rifeAutoUiHints[1],state->english
        ?L"16 px: narrow transition. 32 px: stronger protection, more background ghosting."
        :L"16px: 좁은 전환 · 32px: 강한 보호, 배경 이중상이 늘 수 있음");
    SetSettingsText(state->rifeAutoUiHints[2],state->english
        ?L"Uncertain detection keeps ordinary RIFE. SDR NV12, FHD/QHD only."
        :L"감지가 불확실하면 기존 RIFE를 유지합니다. SDR NV12 · FHD/QHD 전용");
#endif
#endif
#else
    const bool rifePage=video;
#endif
    SetSettingsControlVisible(state->rifeHalfCheck,rifePage);
    SetSettingsControlVisible(state->rifeScaleLabel,rifePage);
    SetSettingsControlVisible(state->rifeScaleCombo,rifePage);
    SetSettingsControlVisible(state->rifeQualityCheck,rifePage);
    SendMessageW(state->rifeHalfCheck,BM_SETCHECK,llcv::rife_test::requested.load()?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(state->rifeScaleCombo,CB_SETCURSEL,llcv::rife_test::ScaleIndex(llcv::rife_test::selectedScale.load()),0);
    SetSettingsText(state->rifeHalfCheck,state->english?L"RIFE (F7)":L"RIFE 보간 (F7)");
    SetSettingsText(state->rifeScaleLabel,state->english?L"Scale":L"추정 크기");
    SendMessageW(state->rifeQualityCheck,BM_SETCHECK,llcv::rife_test::qualityComparison.load()?BST_CHECKED:BST_UNCHECKED,0);
    SetSettingsText(state->rifeQualityCheck,state->english?L"Quality compare (wait; more latency; F10)":L"화질 비교 (완료 대기·지연 증가·F10)");
#ifdef LLCV_RIFE_OPTIONAL
#ifdef LLCV_RIFE_VULKAN_BUNDLED
    SetSettingsControlVisible(state->rifeAutoUiHints[1],false);
    UpdateInterpolationAvailability(state);
#else
    const bool allow=state->InterpolationGpuAvailable()&&llcv::rife_test::componentsEnabled.load();
#ifdef LLCV_INTERPOLATION_RELEASE
    constexpr bool vfg=true;
    SetSettingsControlVisible(state->rifeHalfCheck,rifePage,state->InterpolationGpuAvailable());
    SetSettingsText(state->rifeHalfCheck,state->english?L"Enable NVIDIA VFG":L"NVIDIA VFG 사용");
    RefreshVfgDownloadControl(state);
    SetSettingsControlVisible(state->rifeAutoUiHints[1],false);
#else
    const bool vfg=llcv::rife_test::IsVfg(llcv::rife_test::selectedScale.load());
    SetSettingsControlVisible(state->rifeHalfCheck,rifePage,!state->vsrGpuUnavailable);
    SetSettingsText(state->rifeHalfCheck,state->english?L"Enable NVIDIA frame interpolation":L"NVIDIA 프레임 보간 사용");
    SetSettingsControlVisible(state->interpolationProviderLabel,rifePage,allow);
    SetSettingsControlVisible(state->interpolationProviderCombo,rifePage,allow);
    SetSettingsText(state->interpolationProviderLabel,state->english?L"Interpolation method":L"보간 방식");
    SendMessageW(state->interpolationProviderCombo,CB_SETCURSEL,vfg?1:0,0);
#endif
    SetSettingsText(state->rifeScaleLabel,state->english?L"Quality":L"품질");
    SetSettingsControlVisible(state->interpolationQualityHelp,rifePage);
    const auto first=llcv::rife_test::PresetLabel(vfg?llcv::rife_test::Scale::VfgLow:llcv::rife_test::Scale::Fast,state->english);
    if(SendMessageW(state->rifeScaleCombo,CB_GETCOUNT,0,0)!=(vfg?2:3)||SendMessageW(state->rifeScaleCombo,CB_FINDSTRINGEXACT,-1,reinterpret_cast<LPARAM>(wcschr(first,L'·')+2))==CB_ERR){
        SendMessageW(state->rifeScaleCombo,CB_RESETCONTENT,0,0);
        for(auto preset:llcv::rife_test::Scales)if(llcv::rife_test::IsVfg(preset)==vfg){
            auto label=llcv::rife_test::PresetLabel(preset,state->english);auto separator=wcschr(label,L'·');
            auto index=SendMessageW(state->rifeScaleCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(separator?separator+2:label));
            SendMessageW(state->rifeScaleCombo,CB_SETITEMDATA,index,static_cast<LPARAM>(preset));
        }
    }
    const auto scale=llcv::rife_test::selectedScale.load();
    const size_t qualityIndex=vfg&&!llcv::rife_test::IsVfg(scale)
        ? size_t{1} : llcv::rife_test::ScaleIndex(scale)-(vfg?size_t{3}:size_t{0});
    SendMessageW(state->rifeScaleCombo,CB_SETCURSEL,qualityIndex,0);
    SetSettingsText(state->rifeQualityCheck,state->english?L"Wait for generated frames (increases latency)":L"생성 프레임 완료 대기 (지연 증가)");
    SetSettingsText(state->rifeAutoUiLabel,state->shortcutBindings[static_cast<size_t>(shortcuts::Action::HudCorrection)]
        ? shortcuts::Expand(state->english?L"HUD correction (F4)":L"HUD 보정 (F4)",state->shortcutBindings).c_str()
        : (state->english?L"HUD correction":L"HUD 보정"));
    const wchar_t* inputHint=
#ifdef LLCV_INTERPOLATION_RELEASE
        state->ShowVfgGpuSwitch() ? (state->english
            ? L"Another VFG-capable GPU was found. Select it under Video → Playback GPU."
            : L"다른 호환 GPU가 감지되었습니다. 영상 → 재생 그래픽카드에서 NVIDIA GPU를 선택합니다.") :
        state->vfgGpuCheckFailed && !state->InterpolationGpuAvailable() ? (state->english
            ? L"Could not check the playback GPU. Check the graphics driver and restart LLCV."
            : L"재생 GPU를 확인하지 못했습니다. 그래픽 드라이버를 확인한 뒤 LLCV를 다시 실행합니다.") :
        !state->InterpolationGpuAvailable()?(state->english?L"VFG is not available on this GPU.":L"이 GPU에서는 VFG를 사용할 수 없습니다."):
#ifdef LLCV_VFG_AMPERE_COMPAT
        state->vfgRtx30?(state->english?L"RTX 30: experimental compatibility · F7: toggle VFG":L"RTX 30: 실험적 호환 · F7: VFG 켜기/끄기"):
#endif
#else
        state->vsrGpuUnavailable?(state->english?L"An NVIDIA playback GPU is required.":L"재생에 사용하는 GPU가 NVIDIA여야 합니다."):
#endif
        vfg?(state->english?L"F7: toggle interpolation · FHD / QHD / 4K UHD SDR":L"F7: 보간 켜기/끄기 · FHD / QHD / 4K UHD SDR"):
            (state->english?L"F7: toggle interpolation · FHD SDR":L"F7: 보간 켜기/끄기 · FHD SDR");
    SetSettingsText(state->rifeAutoUiHints[0],shortcuts::Expand(inputHint,state->shortcutBindings).c_str());
    SendMessageW(state->rifeHalfCheck,BM_SETCHECK,allow?BST_CHECKED:BST_UNCHECKED,0);
    UpdateInterpolationAvailability(state);
#endif
#endif
#ifdef LLCV_FLYHIGH_TEST
    const bool fly=llcv::rife_test::IsFlyHigh();
    SetSettingsControlVisible(state->interpolationProviderLabel,rifePage);
    SetSettingsControlVisible(state->interpolationProviderCombo,rifePage);
    SetSettingsText(state->interpolationProviderLabel,state->english?L"Method (F8)":L"보간 방식 (F8)");
    SendMessageW(state->interpolationProviderCombo,CB_SETCURSEL,fly?1:0,0);
    SetSettingsText(state->rifeHalfCheck,state->english?L"Frame interpolation (F7)":L"프레임 보간 (F7)");
    SetSettingsText(state->rifeScaleLabel,state->english?L"RIFE estimation size":L"RIFE 추정 크기");
    SetSettingsControlVisible(state->rifeScaleCombo,rifePage,!fly);
    SetSettingsControlVisible(state->rifeScaleLabel,rifePage,!fly);
    const int choices=fly?2:3;
    if(SendMessageW(state->rifeAutoUiCombo,CB_GETCOUNT,0,0)!=choices){
        SendMessageW(state->rifeAutoUiCombo,CB_RESETCONTENT,0,0);
        for(const wchar_t* choice:{state->english?L"Off":L"끄기",fly?(state->english?L"On (FlyHigh native)":L"켜기 (FlyHigh 기본)"):L"16 px"})SendMessageW(state->rifeAutoUiCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(choice));
        if(!fly)SendMessageW(state->rifeAutoUiCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"32 px"));
    }
    SendMessageW(state->rifeAutoUiCombo,CB_SETCURSEL,fly?(llcv::rife_test::flyHighHud.load()?1:0):llcv::rife_test::SelectedHudWidth()==16?1:llcv::rife_test::SelectedHudWidth()==32?2:0,0);
    if(fly){
        SetSettingsText(state->rifeAutoUiHints[0],state->english?L"FlyHigh native size · Original-resolution synthesis · No video lookahead.":L"FlyHigh 기본 추정 크기 · 원본 해상도 합성 · 영상용 선행 대기 없음");
        SetSettingsText(state->rifeAutoUiHints[1],state->english?L"Native HUD protection looks for unchanged pixels; translucent HUD may still distort.":L"기본 HUD 보호는 변하지 않는 픽셀을 찾습니다. 반투명 HUD는 왜곡될 수 있습니다.");
        SetSettingsText(state->rifeAutoUiHints[2],state->english?L"F7: ON/OFF · F8: RIFE/FlyHigh · SDR NV12 FHD/QHD · Non-commercial test.":L"F7: 켜기/끄기 · F8: RIFE/FlyHigh · SDR NV12 FHD/QHD · 비상업 테스트");
    }
#endif
#endif
    SetSettingsControlVisible(state->cmaaQualityLabel, video && cmaaNis);
    SetSettingsControlVisible(state->cmaaQualityCombo, video && cmaaNis, !p010Selected);
    SetSettingsControlVisible(state->nisSharpnessLabel, video && cmaaNis);
    SetSettingsControlVisible(state->nisSharpnessCombo, video && cmaaNis);
    SetSettingsControlVisible(state->vsrGuideButton, video);
    SetSettingsControlVisible(state->vsrStatus, video &&
        !cmaaNis && (SendMessageW(state->upscalerCombo,CB_GETCURSEL,0,0)==2 || state->vsrGpuUnavailable));
    SetSettingsControlVisible(state->displayResolutionLabel, video);
    SetSettingsControlVisible(state->displayResolutionCombo, video);
    SetSettingsText(state->videoLabel,
        state->english ? L"Capture resolution" : L"캡처 해상도");
    SetSettingsText(state->pixelCheck, HasSplitResolution(*state)
        ? (state->english ? L"Lock display size" : L"표시 크기 고정")
        : L"Pixel-perfect (1:1)");
    SetSettingsText(state->vsrStatus, shortcuts::Expand(state->vsrGpuUnavailable
        ? (state->english ? L"VSR requires an NVIDIA GPU" : L"VSR은 NVIDIA GPU가 필요합니다")
        : L"SDR / HDR10 · VSR: F6", state->shortcutBindings).c_str());
    SetSettingsControlVisible(state->screenshotClipboardCheck, updates);
    SetSettingsControlVisible(state->screenshotHelp, updates);
    SetSettingsControlVisible(state->screenshotFolderButton, updates);
    SetSettingsControlVisible(state->saveLogCheck, guide);
    SetSettingsControlVisible(state->showConsoleCheck, guide);
    SetSettingsControlVisible(state->updateTitle, updates);
    SetSettingsControlVisible(state->updateText, updates);
    SetSettingsControlVisible(state->checkForUpdatesCheck, updates);
    SetSettingsControlVisible(state->updateNowButton, updates);
    SetSettingsControlVisible(state->releaseNotesButton, updates);
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
    switch (topic) {
    case SettingsHelpTopic::Capture40:
        return english
            ? L"Leave this off for normal playback. Turn it on to request 40 fps instead of your selected frame rate. If the device does not support 40 fps, capture will not start.\n\n"
              L"This does not lock the game's frame rate or replace VRR. For a game running at a steady 40 fps, one option is 120 Hz console output → 40 fps capture → 3× in Lossless Scaling → 120 Hz display. If the game varies between 30 and 40 fps, interpolation can make stutter more noticeable and add latency."
            : L"이 옵션을 켜면 선택한 프레임 속도 대신 40fps로 캡처합니다.\n"
              L"장치가 40fps를 지원하지 않으면 재생을 시작할 수 없습니다. 게임의 프레임 속도나 VRR은 바뀌지 않습니다.\n\n"
              L"게임이 40fps를 유지할 때의 설정 예시는 다음과 같습니다.\n"
              L"콘솔 120Hz → LLCV 캡처 40fps → Lossless Scaling 3배 → 모니터 120Hz\n\n"
              L"게임이 30~40fps 사이를 오가면 보간 후 끊김이 더 눈에 띄거나 지연이 늘 수 있습니다.";
    case SettingsHelpTopic::Drift:
        return english
            ? L"If audio plays cleanly with correction off, leave it off. If Tab repeatedly reports a clock imbalance during playback, choose Auto.\n\n"
              L"Auto adjusts the audio rate only when the capture and playback devices drift apart. Once active, correction stays on until playback ends. If Tab reports a low PCM buffer instead, raise the PCM buffer target."
            : L"보정을 꺼도 소리가 깨끗하면 설정을 바꿀 필요가 없습니다.\n"
              L"재생 중 Tab 진단에 ‘반복 불균형’이 계속 표시되면 ‘자동’을 선택합니다.\n\n"
              L"‘자동’은 캡처 장치와 출력 장치의 속도가 어긋날 때만 보정을 시작합니다.\n"
              L"한 번 시작한 보정은 재생이 끝날 때까지 유지됩니다.\n\n"
              L"‘PCM 버퍼 부족’이 표시되면 보정 대신 PCM 버퍼 목표를 높입니다.";
    case SettingsHelpTopic::PcmQueue:
        return english
            ? L"Start with 25 ms. If the sound cuts out, raise the target. If it plays cleanly but you want less audio delay, lower it one step at a time.\n\nThis controls how much captured audio the app holds before playback. The WASAPI output buffer is a separate setting."
            : L"재생 전에 앱에 모아 두는 소리의 양입니다. 기본값은 25ms입니다.\n\n"
              L"소리가 끊기면 값을 높입니다.\n"
              L"끊김 없이 재생되고 소리 지연을 더 줄이고 싶다면 한 단계씩 낮춥니다.\n\n"
              L"WASAPI 출력 버퍼는 이 값과 별도로 설정합니다.";
    case SettingsHelpTopic::Presentation:
        return llcv::presentation_ui::HelpText(english);
    case SettingsHelpTopic::VolumeBoost:
        return english
            ? L"Turn this on if the capture audio is still too quiet at 100%. You can then use the mouse wheel to raise the app volume to 200%.\n\nIf loud sounds start to distort, lower the volume. At 100%, the app applies no boost."
            : L"캡처 소리가 작을 때 마우스 휠로 앱 음량을 최대 200%까지 높일 수 있습니다.\n"
              L"100%에서는 소리를 증폭하지 않습니다.\n\n"
              L"큰 소리가 찢어져 들리면 음량을 낮춥니다.";
    case SettingsHelpTopic::ForceHdr10:
        return english
            ? L"Turn this on only if the source is HDR10 but the capture device reports the wrong color information. If the information is missing, the app already treats P010 as HDR10. Enable Windows HDR on the playback monitor.\n\nThis does not convert SDR to HDR. For an SDR source, choose NV12/YUY2 instead of P010."
            : L"실제 입력은 HDR10인데 캡처 장치가 색 정보를 잘못 알릴 때만 사용합니다.\n"
              L"색 정보가 없으면 P010을 HDR10으로 인식하므로 강제할 필요가 없습니다.\n\n"
              L"HDR로 재생하려면 재생할 모니터에서 Windows HDR을 켭니다.\n"
              L"SDR 영상을 HDR로 바꾸는 기능은 아닙니다. SDR 입력에는 P010 대신 NV12/YUY2를 선택합니다.";
    case SettingsHelpTopic::HdrChroma:
        return english
            ? L"Leave this on Auto. If color bleeds to one side of fine text or lines in P010 HDR video, compare Top-left and Left against the source.\n\nThis changes where the app reads color samples. It cannot repair misaligned color channels in the captured image."
            : L"일반적으로 ‘자동’을 사용합니다.\n"
              L"P010 HDR 영상에서 글씨나 가는 선 한쪽으로 색이 번지면 ‘Top-left’와 ‘Left’를 원본과 비교합니다.\n\n"
              L"이 설정은 색을 읽는 위치만 바꿉니다.\n"
              L"캡처된 영상 자체에서 색이 어긋난 경우에는 복구할 수 없습니다.";
    case SettingsHelpTopic::MjpegColor:
        return english
            ? L"Leave this on Auto unless MJPEG colors look wrong. If the device supplies no color information, Auto uses BT.709 for HD and BT.601 for SD.\n\nIf the colors look right but blacks look gray or shadow detail is lost, check SDR color range instead."
            : L"MJPEG 영상의 색이 자연스러우면 ‘자동’을 유지합니다.\n"
              L"색 정보가 없을 때는 HD에 BT.709, SD에 BT.601을 사용합니다.\n\n"
              L"색상은 맞는데 검정이 회색처럼 보이거나 어두운 부분이 뭉개지면 ‘SDR 색 범위’를 확인합니다.";
    case SettingsHelpTopic::SdrRange:
        return english
            ? L"Leave this on Auto unless blacks look gray or shadow detail is lost. If that happens, match Limited or Full to the source output.\n\nWithout range information, Auto uses Limited for NV12/YUY2 and Full for MJPEG and RGB. This affects playback, screenshots and recordings, not the console output or P010/HDR."
            : L"일반적으로 ‘자동’을 사용합니다. 검정이 회색처럼 뜨거나 어두운 부분이 뭉개지면\n"
              L"입력 기기의 출력 범위에 맞춰 Limited(제한) 또는 Full(전체)을 선택합니다.\n\n"
              L"범위 정보가 없으면 NV12/YUY2는 Limited, MJPEG와 RGB는 Full로 읽습니다.\n"
              L"재생·스크린샷·녹화에 적용되며 콘솔 출력이나 P010/HDR에는 영향을 주지 않습니다.";
    }
    return L"";
}

} // namespace llcv::settings_ui
