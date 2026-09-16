#include "SettingsView.h"
#include "PresentationModeUi.h"
#include "settings/AppSettings.h"

#include <commctrl.h>
#include <algorithm>
#include <cwchar>

namespace llcv::settings_ui {
using settings::VideoPixelFormat;

int SettingsPixels(int dips, UINT dpi) {
    return MulDiv(dips, dpi ? dpi : USER_DEFAULT_SCREEN_DPI,
                  USER_DEFAULT_SCREEN_DPI);
}

static constexpr int kSettingsTabbedClientHeightDip = 630;

int SettingsClientHeightDip(const SettingsControls* state) {
    (void)state;
    return kSettingsTabbedClientHeightDip;
}

SIZE SettingsDialogOuterSize(HWND hwnd, UINT dpi,
                                    const SettingsControls* state) {
    RECT rect{0, 0, SettingsPixels(kSettingsClientWidthDip, dpi),
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

void PlaceSettingsControl(HWND control, int x, int y, int width,
                                 int height, UINT dpi) {
    if (!control) return;
    SetWindowPos(control, nullptr, SettingsPixels(x, dpi),
                 SettingsPixels(y, dpi), SettingsPixels(width, dpi),
                 SettingsPixels(height, dpi),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static BOOL CALLBACK SetSettingsChildFont(HWND child, LPARAM fontValue) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(fontValue), FALSE);
    return TRUE;
}

void ApplySettingsFont(SettingsControls* state, HWND hwnd,
                              UINT dpi) {
    if (!state || !hwnd) return;
    HFONT font = CreateFontW(
        -MulDiv(9, dpi ? dpi : USER_DEFAULT_SCREEN_DPI, 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!font) return;
    state->uiFonts.push_back(font);
    EnumChildWindows(hwnd, SetSettingsChildFont,
                     reinterpret_cast<LPARAM>(font));

    // Section labels are deliberately subtle, but bold enough to make the
    // vertically grouped audio controls scannable at a glance.
    HFONT sectionFont = CreateFontW(
        -MulDiv(9, dpi ? dpi : USER_DEFAULT_SCREEN_DPI, 72),
        0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!sectionFont) return;
    state->uiFonts.push_back(sectionFont);
    for (HWND control : {state->audioOutputSection,
                         state->audioPlaybackSection,
                         state->audioStabilitySection,
                         state->videoCaptureSection,
                         state->videoDisplaySection,
                         state->videoWindowSection,
                         state->guideShortcutsTitle,
                         state->guideDiagnosticsTitle}) {
        if (control) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(sectionFont), FALSE);
        }
    }

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
    return std::min(18 + textWidthDip, 430);
}

void LayoutSettingsControls(SettingsControls* state, UINT dpi) {
    if (!state) return;
    // The dialog is deliberately tabbed rather than expanded vertically. This
    // keeps the startup view small while leaving every setting reachable.
    PlaceSettingsControl(state->tabControl, 24, 16, 901, 31, dpi);

    // Global preferences remain fixed below every tab, especially direct-start.
    // Leave a clear visual break after the PCM-buffer group. Language and
    // quick-start are application preferences, not audio-tuning controls.
    PlaceSettingsControl(state->languageLabel, 24, 500, 160, 24, dpi);
    PlaceSettingsControl(state->languageCombo, 195, 496, 280, 120, dpi);
    PlaceSettingsControl(state->skipStartupCheck, 24, 540, 451, 28, dpi);
    PlaceSettingsControl(state->skipStartupHint, 44, 568, 500, 22, dpi);
    PlaceSettingsControl(state->versionWatermark, 24, 602, 260, 20, dpi);
    PlaceSettingsControl(state->startButton, 745, 568, 80, 30, dpi);
    PlaceSettingsControl(state->cancelButton, 835, 568, 80, 30, dpi);

    // Audio tab: output choice first, then everyday playback controls, then
    // the latency/stability controls that usually only need adjustment after
    // diagnostics report a problem.
    PlaceSettingsControl(state->audioOutputSection, 34, 58, 200, 20, dpi);
    PlaceSettingsControl(state->audioLabel, 34, 80, 160, 24, dpi);
    PlaceSettingsControl(state->audioCombo, 205, 76, 360, 120, dpi);
    // Exclusive endpoint verification configures the selected output mode,
    // so keep its explicit recheck action beside that mode instead of making
    // it look like a generic status-row operation.
    // Match the visible combobox field (rather than its dropdown height) so
    // the recheck action reads as part of the output-mode row.
    PlaceSettingsControl(state->exclusiveTestButton, 575, 76, 185, 22, dpi);
    PlaceSettingsControl(state->audioOutputLabel, 34, 116, 160, 24, dpi);
    PlaceSettingsControl(state->audioOutputCombo, 205, 112, 680, 220, dpi);
    PlaceSettingsControl(state->bufferLabel, 34, 152, 160, 24, dpi);
    PlaceSettingsControl(state->bufferCombo, 205, 148, 280, 180, dpi);
    PlaceSettingsControl(state->audioStatus, 34, 188, 580, 24, dpi);
    PlaceSettingsControl(state->audioPlaybackSection, 34, 222, 250, 20, dpi);
    PlaceSettingsControl(state->volumeHudLabel, 34, 246, 160, 24, dpi);
    PlaceSettingsControl(state->volumeHudCombo, 205, 242, 280, 160, dpi);
    const int volumeBoostWidth = SettingsCheckboxWidthDip(
        state->volumeBoostCheck, dpi);
    PlaceSettingsControl(state->volumeBoostCheck, 34, 282, volumeBoostWidth,
                         28, dpi);
    PlaceSettingsControl(state->volumeBoostHelp,
                         34 + volumeBoostWidth + 10,
                         284, 24, 24, dpi);
    PlaceSettingsControl(state->muteBackgroundCheck, 34, 318, 500, 28, dpi);
    PlaceSettingsControl(state->audioOnlyCheck, 34, 354, 500, 28, dpi);
    PlaceSettingsControl(state->audioStabilitySection, 34, 392, 250, 20, dpi);
    PlaceSettingsControl(state->driftLabel, 34, 416, 160, 24, dpi);
    PlaceSettingsControl(state->driftHelp, 170, 412, 24, 24, dpi);
    PlaceSettingsControl(state->driftCombo, 205, 412, 360, 120, dpi);
    PlaceSettingsControl(state->pcmQueueLabel, 34, 452, 160, 24, dpi);
    PlaceSettingsControl(state->pcmQueueHelp, 170, 448, 24, 24, dpi);
    PlaceSettingsControl(state->pcmQueueCombo, 205, 448, 280, 140, dpi);

    // Video & window tab: capture format on the left; how it is shown and
    // how the viewer window behaves on the right. HDR stays last because it
    // is an experimental override rather than a normal display choice.
    // Leave a real breathing gap below each section heading.  The previous
    // first-row placement was inherited from the no-heading layout and made
    // headings read like part of the option label.
    PlaceSettingsControl(state->captureDeviceLabel, 34, 84, 140, 24, dpi);
    PlaceSettingsControl(state->videoCaptureSection, 34, 58, 140, 20, dpi);
    PlaceSettingsControl(state->captureDeviceCombo, 190, 80, 270, 220, dpi);
    PlaceSettingsControl(state->captureAudioDeviceLabel, 34, 124, 140, 24, dpi);
    PlaceSettingsControl(state->captureAudioDeviceCombo, 190, 120, 270, 220, dpi);
    PlaceSettingsControl(state->captureAudioStatus, 190, 124, 300, 24, dpi);
    PlaceSettingsControl(state->videoLabel, 34, 164, 140, 24, dpi);
    PlaceSettingsControl(state->videoCombo, 190, 160, 270, 120, dpi);
    PlaceSettingsControl(state->pixelFormatLabel, 34, 204, 140, 24, dpi);
    PlaceSettingsControl(state->pixelFormatCombo, 190, 200, 270, 160, dpi);
    PlaceSettingsControl(state->frameRateLabel, 34, 244, 140, 24, dpi);
    PlaceSettingsControl(state->frameRateCombo, 190, 240, 270, 200, dpi);
    PlaceSettingsControl(state->videoCapabilityStatus, 34, 278, 430, 90, dpi);
    PlaceSettingsControl(state->presentationLabel, 505, 84, 95, 24, dpi);
    PlaceSettingsControl(state->videoDisplaySection, 505, 58, 140, 20, dpi);
    PlaceSettingsControl(state->presentationHelp, 604, 80, 24, 24, dpi);
    PlaceSettingsControl(state->presentationCombo, 630, 80, 255, 120, dpi);
    PlaceSettingsControl(state->displayMonitorLabel, 505, 124, 120, 24, dpi);
    PlaceSettingsControl(state->displayMonitorCombo, 630, 120, 255, 180, dpi);
    if (state->displayMonitorCombo)
        SendMessageW(state->displayMonitorCombo, CB_SETDROPPEDWIDTH, SettingsPixels(560, dpi), 0);
    PlaceSettingsControl(state->pixelCheck, 505, 160, 380, 28, dpi);
    PlaceSettingsControl(state->scalingLabel, 505, 200, 120, 24, dpi);
    PlaceSettingsControl(state->scalingCombo, 630, 196, 255, 120, dpi);
    // The controls from here onward affect the viewer window itself rather
    // than captured video format or rendering policy.  When Pixel-perfect is
    // enabled the scaling row is hidden, so pull this section up by one grid
    // row instead of leaving an arbitrary empty gap.
    const bool pixelPerfect = state->pixelCheck &&
        SendMessageW(state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const int windowSectionY = pixelPerfect ? 208 : 244;
    const int windowOptionY = windowSectionY + 24;
    PlaceSettingsControl(state->videoWindowSection, 505, windowSectionY,
                         140, 20, dpi);
    PlaceSettingsControl(state->relativeSizeCheck, 505, windowOptionY,
                         400, 28, dpi);
    PlaceSettingsControl(state->relativeSizeWarning, 525, windowOptionY + 28,
                         370, 28, dpi);
    PlaceSettingsControl(state->borderlessCheck, 505, windowOptionY + 68,
                         400, 28, dpi);
    PlaceSettingsControl(state->windowSnapCheck, 505, windowOptionY + 104,
                         400, 28, dpi);
    PlaceSettingsControl(state->fullscreenCursorLabel, 505,
                         windowOptionY + 144, 120, 24, dpi);
    PlaceSettingsControl(state->fullscreenCursorCombo, 630,
                         windowOptionY + 140, 255, 120, dpi);
    PlaceSettingsControl(state->fullscreenCursorHint, 630,
                         windowOptionY + 172, 255, 24, dpi);
    // The combo's configured height includes its drop-down list rectangle.
    // Keep the adjacent hint above that sibling after every relayout so a
    // Pixel-perfect redraw cannot paint over it while the list is closed.
    if (state->fullscreenCursorHint) {
        SetWindowPos(state->fullscreenCursorHint, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // P010 and MJPEG use the same final capture-format row. Only the control
    // relevant to the selected input format is made visible.
    PlaceSettingsControl(state->forceHdr10Check, 34, 374, 360, 28, dpi);
    PlaceSettingsControl(state->forceHdr10Help, 402, 370, 24, 24, dpi);
    PlaceSettingsControl(state->hdrChromaLabel, 34, 414, 140, 24, dpi);
    PlaceSettingsControl(state->hdrChromaCombo, 190, 410, 240, 150, dpi);
    PlaceSettingsControl(state->hdrChromaHelp, 438, 410, 24, 24, dpi);
    PlaceSettingsControl(state->mjpegColorLabel, 34, 374, 140, 24, dpi);
    PlaceSettingsControl(state->mjpegColorCombo, 190, 370, 240, 150, dpi);
    PlaceSettingsControl(state->mjpegColorHelp, 438, 370, 24, 24, dpi);

    // Guide and update tabs.
    PlaceSettingsControl(state->guideShortcutsTitle, 34, 58, 280, 20, dpi);
    PlaceSettingsControl(state->guideText, 34, 84, 400, 220, dpi);
    PlaceSettingsControl(state->guideDiagnosticsTitle, 505, 58, 320, 20, dpi);
    PlaceSettingsControl(state->guideDiagnosticsText, 505, 84, 360, 70, dpi);
    PlaceSettingsControl(state->saveLogCheck, 505, 170, 360, 28, dpi);
    PlaceSettingsControl(state->showConsoleCheck, 505, 206, 360, 28, dpi);
    PlaceSettingsControl(state->guideLogFolderButton, 505, 248, 165, 26, dpi);
    PlaceSettingsControl(state->updateTitle, 34, 76, 400, 24, dpi);
    PlaceSettingsControl(state->updateText, 34, 110, 760, 64, dpi);
    PlaceSettingsControl(state->checkForUpdatesCheck, 34, 190, 500, 28, dpi);
    PlaceSettingsControl(state->updateNowButton, 34, 230, 185, 30, dpi);
    PlaceSettingsControl(state->updateStatus, 235, 234, 650, 24, dpi);
}

void SetSettingsControlVisible(HWND control, bool visible) {
    if (!control) return;
    ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    EnableWindow(control, visible ? TRUE : FALSE);
}

void UpdateScalingControlVisibility(SettingsControls* state) {
    if (!state) return;
    const bool pixelPerfect = state->pixelCheck &&
        SendMessageW(state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool visible = state->activeTab == SettingsTab::VideoWindow &&
                         !pixelPerfect;
    SetSettingsControlVisible(state->scalingLabel, visible);
    SetSettingsControlVisible(state->scalingCombo, visible);
}

void UpdateWindowBehaviorVisibility(SettingsControls* state) {
    if (!state) return;
    const bool pixelPerfect = state->pixelCheck &&
        SendMessageW(state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool relativeSize = state->relativeSizeCheck &&
        SendMessageW(state->relativeSizeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

    // These are everyday window-behavior preferences, not advanced tuning.
    // Only show the caveat when the currently selected combination needs it.
    const bool visible = state->activeTab == SettingsTab::VideoWindow;
    SetSettingsControlVisible(state->relativeSizeCheck, visible);
    SetSettingsControlVisible(state->borderlessCheck, visible);
    SetSettingsControlVisible(state->fullscreenCursorLabel, visible);
    SetSettingsControlVisible(state->fullscreenCursorCombo, visible);
    SetSettingsControlVisible(state->fullscreenCursorHint, visible);
    SetSettingsControlVisible(state->relativeSizeWarning,
                              visible && pixelPerfect && relativeSize);
}

void UpdateAdvancedControlVisibility(SettingsControls* state, bool exclusive,
                                            settings::VideoPixelFormat selectedFormat) {
    if (!state) return;
    const bool audio = state->activeTab == SettingsTab::Audio;
    const bool video = state->activeTab == SettingsTab::VideoWindow;
    const bool guide = state->activeTab == SettingsTab::GuideDiagnostics;
    const bool updates = state->activeTab == SettingsTab::Updates;
    for (HWND control : {state->tabControl, state->languageLabel,
                         state->languageCombo, state->skipStartupCheck,
                         state->skipStartupHint, state->versionWatermark,
                         state->startButton, state->cancelButton}) {
        SetSettingsControlVisible(control, true);
    }
    for (HWND control : {state->audioOutputSection,
                         state->audioPlaybackSection,
                         state->audioStabilitySection,
                         state->audioLabel, state->audioCombo,
                         state->audioOutputLabel, state->audioOutputCombo,
                         state->bufferLabel, state->bufferCombo,
                         state->audioStatus,
                         state->volumeHudLabel, state->volumeHudCombo,
                         state->volumeBoostCheck, state->volumeBoostHelp,
                         state->muteBackgroundCheck, state->audioOnlyCheck,
                         state->driftLabel, state->driftHelp, state->driftCombo,
                         state->pcmQueueLabel, state->pcmQueueHelp,
                         state->pcmQueueCombo}) {
        SetSettingsControlVisible(control, audio);
    }
    // The endpoint recheck belongs only to WASAPI Exclusive.  In Shared and
    // ASIO modes it is both irrelevant and misleading, even on the Audio tab.
    SetSettingsControlVisible(state->exclusiveTestButton,
                              audio && exclusive);
    for (HWND control : {state->videoCaptureSection,
                         state->videoDisplaySection,
                         state->videoWindowSection,
                         state->presentationLabel, state->presentationHelp,
                         state->presentationCombo, state->displayMonitorLabel,
                         state->displayMonitorCombo, state->captureDeviceLabel,
                         state->captureDeviceCombo,
                         state->captureAudioDeviceLabel, state->videoLabel,
                         state->videoCombo, state->pixelFormatLabel,
                         state->pixelFormatCombo, state->frameRateLabel,
                         state->frameRateCombo, state->videoCapabilityStatus,
                         state->pixelCheck, state->windowSnapCheck,
                         state->fullscreenCursorLabel,
                         state->fullscreenCursorCombo,
                         state->fullscreenCursorHint}) {
        SetSettingsControlVisible(control, video);
    }
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
    SetSettingsControlVisible(state->forceHdr10Check, video && p010Selected);
    SetSettingsControlVisible(state->forceHdr10Help, video && p010Selected);
    SetSettingsControlVisible(state->hdrChromaLabel, video && p010Selected);
    SetSettingsControlVisible(state->hdrChromaCombo, video && p010Selected);
    SetSettingsControlVisible(state->hdrChromaHelp, video && p010Selected);
    SetSettingsControlVisible(state->mjpegColorLabel, video && mjpegSelected);
    SetSettingsControlVisible(state->mjpegColorCombo, video && mjpegSelected);
    SetSettingsControlVisible(state->mjpegColorHelp, video && mjpegSelected);
    SetSettingsControlVisible(state->guideShortcutsTitle, guide);
    SetSettingsControlVisible(state->guideText, guide);
    SetSettingsControlVisible(state->guideDiagnosticsTitle, guide);
    SetSettingsControlVisible(state->guideDiagnosticsText, guide);
    SetSettingsControlVisible(state->guideLogFolderButton, guide);
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
                      target == state->mjpegColorHelp);
}

const wchar_t* SettingsHelpText(SettingsHelpTopic topic, bool english) {
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
                   L"Use this only when the source is confirmed to be HDR and the capture driver does not expose "
                   L"color metadata. It treats P010 as BT.2020/PQ and enables the HDR10 swap chain.\n\n"
                   L"If the source is SDR, or the monitor is not handling HDR correctly, colors can look strongly "
                   L"oversaturated or otherwise wrong. Turn it off in that case. This does not add a frame queue; "
                   L"it only changes the output color interpretation.";
        case SettingsHelpTopic::HdrChroma:
            return L"HDR chroma placement\n\n"
                   L"Auto follows the device metadata and rejects unsupported placements. Use Top-left or Left "
                   L"only for a P010 HDR device with missing or incorrect chroma metadata. Compare fine colored "
                   L"edges and text against a reference.\n\n"
                   L"This overrides the declared chroma placement; it does not repair genuinely staggered Cb/Cr "
                   L"planes or guarantee that placement 6 is supported. It does not change brightness, saturation, "
                   L"PQ interpretation or range validation, and adds no frame queue or processing pass. "
                   L"For missing HDR metadata, Force HDR10 is a separate setting.";
        case SettingsHelpTopic::MjpegColor:
            return L"MJPEG color interpretation\n\n"
                   L"Auto uses decoder metadata first, then DirectShow metadata. If neither identifies "
                   L"the format, it uses JPEG Full range with BT.709 for HD or BT.601 for SD.\n\n"
                   L"Use a manual combination only when MJPEG colors still differ from another capture "
                   L"application. Full/Limited changes black and white levels; BT.709/BT.601 changes the "
                   L"YUV color matrix. This does not add a frame queue or increase latency.";
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
               L"캡처 드라이버가 색공간 메타데이터를 제공하지 않지만 입력이 HDR임을 확인한 경우에만 사용하세요. "
               L"P010을 BT.2020/PQ로 처리하고 HDR10 출력으로 표시합니다.\n\n"
               L"입력이 SDR이거나 모니터의 HDR 처리가 맞지 않으면 색상이 과포화되거나 부정확해질 수 있습니다. "
               L"그 경우 이 옵션을 끄세요. 프레임 큐를 추가하지 않으므로 표시 지연은 늘지 않고 출력 색상 해석만 바뀝니다.";
    case SettingsHelpTopic::HdrChroma:
        return L"HDR 색차 배치 안내\n\n"
               L"자동은 장치 메타데이터를 따르며 지원하지 않는 배치는 차단합니다. P010 HDR 장치의 "
               L"색차 배치 정보가 없거나 잘못된 경우에만 Top-left 또는 Left를 선택하고, 가는 색 경계와 "
               L"글자를 기준 화면과 비교하세요.\n\n"
               L"이 옵션은 배치 정보의 해석을 바꿉니다. 실제 Cb/Cr가 서로 어긋난 데이터를 복원하거나 "
               L"배치 값 6의 지원을 보장하지 않습니다. 밝기·채도·PQ 해석·색 범위 검증은 바꾸지 않으며 "
               L"프레임 큐나 처리 단계를 추가하지 않습니다. HDR 메타데이터가 없다면 HDR10 강제는 별도로 설정하세요.";
    case SettingsHelpTopic::MjpegColor:
        return L"MJPEG 색상 해석 안내\n\n"
               L"자동은 디코더 메타데이터를 먼저 사용하고, 없으면 DirectShow 정보를 확인합니다. 양쪽 모두 "
               L"알려주지 않으면 JPEG Full range와 HD BT.709 또는 SD BT.601을 사용합니다.\n\n"
               L"자동 색상이 다른 캡처 프로그램과 계속 다를 때만 수동 조합을 선택하세요. Full/Limited는 "
               L"명암 범위를, BT.709/BT.601은 YUV 색상 행렬을 바꿉니다. 프레임 큐를 추가하지 않아 "
               L"표시 지연은 늘지 않습니다.";
    }
    return L"";
}

} // namespace llcv::settings_ui
