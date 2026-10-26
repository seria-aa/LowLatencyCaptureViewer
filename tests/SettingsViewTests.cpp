#include "ui/SettingsView.h"
#include "ui/SettingsFonts.h"
#include "ui/ViewerShortcuts.h"
#include "ui/SettingsDialogControls.h"
#include "ui/UiText.h"
#include "capture/DirectShowDevices.h"
#include "ui/PresentationModeUi.h"
#include "settings/AppSettings.h"

#include <commctrl.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <set>
#include <utility>

using namespace llcv::settings_ui;
using llcv::settings::VideoPixelFormat;

static void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAILED: %s\n", message); std::abort(); }
}
static void CheckCaptionFits(HWND,const char*,UINT,bool,int,bool,bool);
static const wchar_t* FullCapabilityCaption(bool english) {
    return english
        ? L"Detected modes:\r\nNV12 240/144/120/100/90/60/50/30 fps\r\nYUY2 120/100/60/50/30 fps\r\nP010 60/50/30 fps\r\nMJPEG 240/144/120/100/90/60/50/30 fps\r\nUYVY 240/200/180/165/144/120/100/90/60/50/30 fps"
        : L"자동 인식:\r\nNV12 240/144/120/100/90/60/50/30 fps\r\nYUY2 120/100/60/50/30 fps\r\nP010 60/50/30 fps\r\nMJPEG 240/144/120/100/90/60/50/30 fps\r\nUYVY 240/200/180/165/144/120/100/90/60/50/30 fps";
}
static const wchar_t* UnsupportedCapabilityCaption(bool english) {
    return english
        ? L"Only unsupported pixel formats at this resolution:\r\n{11111111-2222-3333-4455-66778899AABB}, {12345678-90AB-CDEF-1122-334455667788}, {AABBCCDD-EEFF-0011-2233-445566778899}, …"
        : L"선택한 해상도에서 미지원 픽셀 포맷만 제공됩니다:\r\n{11111111-2222-3333-4455-66778899AABB}, {12345678-90AB-CDEF-1122-334455667788}, {AABBCCDD-EEFF-0011-2233-445566778899}, …";
}
struct Member { HWND SettingsControls::* handle; const char* name; };
static LRESULT CALLBACK CountEnableTransitions(HWND hwnd, UINT message, WPARAM wParam,
                                               LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    if (message == WM_ENABLE) ++*reinterpret_cast<unsigned*>(data);
    return DefSubclassProc(hwnd, message, wParam, lParam);
}
static constexpr Member kMembers[] = {
    {&SettingsControls::frameOptionsButton, "frameOptionsButton"},
    {&SettingsControls::frameOptionsTitle, "frameOptionsTitle"},
    {&SettingsControls::frameOptionsClose, "frameOptionsClose"},
    {&SettingsControls::frameOptionsHint, "frameOptionsHint"},
    {&SettingsControls::capture40Check, "capture40Check"},
    {&SettingsControls::unlistedFpsCheck, "unlistedFpsCheck"},
    {&SettingsControls::capture40Help, "capture40Help"},
    {&SettingsControls::guideLsButton, "guideLsButton"},
    {&SettingsControls::videoRefreshButton, "videoRefreshButton"},
    {&SettingsControls::brandLabel, "brandLabel"},
    {&SettingsControls::pageTitle, "pageTitle"},
    {&SettingsControls::pageSubtitle, "pageSubtitle"},
    {&SettingsControls::appPreferencesSection, "appPreferencesSection"},
    {&SettingsControls::upscalerCombo, "upscalerCombo"},
    {&SettingsControls::upscalerLabel, "upscalerLabel"},
    {&SettingsControls::nisSharpnessLabel, "nisSharpnessLabel"},
    {&SettingsControls::nisSharpnessCombo, "nisSharpnessCombo"},
    {&SettingsControls::vsrGuideButton, "vsrGuideButton"},
    {&SettingsControls::vsrStatus, "vsrStatus"},
    {&SettingsControls::displayResolutionLabel, "displayResolutionLabel"},
    {&SettingsControls::displayResolutionCombo, "displayResolutionCombo"},
    {&SettingsControls::tabControl, "tabControl"},
    {&SettingsControls::guideVideoHint, "guideVideoHint"},
    {&SettingsControls::guideShortcutsTitle, "guideShortcutsTitle"},
    {&SettingsControls::shortcutResetButton, "shortcutResetButton"},
    {&SettingsControls::guideDiagnosticsTitle, "guideDiagnosticsTitle"},
    {&SettingsControls::guideDiagnosticsText, "guideDiagnosticsText"},
    {&SettingsControls::guideLogFolderButton, "guideLogFolderButton"},
    {&SettingsControls::screenshotClipboardCheck, "screenshotClipboardCheck"},
    {&SettingsControls::screenshotTitle, "screenshotTitle"},
    {&SettingsControls::screenshotHelp, "screenshotHelp"},
    {&SettingsControls::screenshotFolderButton, "screenshotFolderButton"},
    {&SettingsControls::updateTitle, "updateTitle"},
    {&SettingsControls::updateText, "updateText"},
    {&SettingsControls::updateNowButton, "updateNowButton"},
    {&SettingsControls::updateStatus, "updateStatus"},
    {&SettingsControls::audioOutputSection, "audioOutputSection"},
    {&SettingsControls::audioPlaybackSection, "audioPlaybackSection"},
    {&SettingsControls::audioStabilitySection, "audioStabilitySection"},
    {&SettingsControls::videoCaptureSection, "videoCaptureSection"},
    {&SettingsControls::videoDisplaySection, "videoDisplaySection"},
    {&SettingsControls::videoWindowSection, "videoWindowSection"},
    {&SettingsControls::languageLabel, "languageLabel"},
    {&SettingsControls::languageCombo, "languageCombo"},
    {&SettingsControls::themeLabel, "themeLabel"},
    {&SettingsControls::themeCombo, "themeCombo"},
    {&SettingsControls::audioLabel, "audioLabel"},
    {&SettingsControls::bufferLabel, "bufferLabel"},
    {&SettingsControls::audioOutputLabel, "audioOutputLabel"},
    {&SettingsControls::volumeHudLabel, "volumeHudLabel"},
    {&SettingsControls::volumeBoostCheck, "volumeBoostCheck"},
    {&SettingsControls::volumeBoostHelp, "volumeBoostHelp"},
    {&SettingsControls::driftLabel, "driftLabel"},
    {&SettingsControls::driftHelp, "driftHelp"},
    {&SettingsControls::pcmQueueLabel, "pcmQueueLabel"},
    {&SettingsControls::pcmQueueHelp, "pcmQueueHelp"},
    {&SettingsControls::presentationLabel, "presentationLabel"},
    {&SettingsControls::presentationHelp, "presentationHelp"},
    {&SettingsControls::fullscreenCursorLabel, "fullscreenCursorLabel"},
    {&SettingsControls::fullscreenCursorHint, "fullscreenCursorHint"},
    {&SettingsControls::fullscreenPositionLabel, "fullscreenPositionLabel"},
    {&SettingsControls::fullscreenPositionCombo, "fullscreenPositionCombo"},
    {&SettingsControls::fullscreenPositionHint, "fullscreenPositionHint"},
    {&SettingsControls::scalingLabel, "scalingLabel"},
    {&SettingsControls::videoLabel, "videoLabel"},
    {&SettingsControls::captureDeviceLabel, "captureDeviceLabel"},
    {&SettingsControls::captureAudioDeviceLabel, "captureAudioDeviceLabel"},
    {&SettingsControls::captureAudioStatus, "captureAudioStatus"},
    {&SettingsControls::pixelFormatLabel, "pixelFormatLabel"},
    {&SettingsControls::frameRateLabel, "frameRateLabel"},
    {&SettingsControls::videoCapabilityStatus, "videoCapabilityStatus"},
    {&SettingsControls::unlistedFpsStatus, "unlistedFpsStatus"},
    {&SettingsControls::audioCombo, "audioCombo"},
    {&SettingsControls::bufferCombo, "bufferCombo"},
    {&SettingsControls::audioOutputCombo, "audioOutputCombo"},
    {&SettingsControls::volumeHudCombo, "volumeHudCombo"},
    {&SettingsControls::muteBackgroundCheck, "muteBackgroundCheck"},
    {&SettingsControls::audioOnlyCheck, "audioOnlyCheck"},
    {&SettingsControls::surround51Check, "surround51Check"},
    {&SettingsControls::surround51Hint, "surround51Hint"},
    {&SettingsControls::forceHdr10Check, "forceHdr10Check"},
    {&SettingsControls::forceHdr10Help, "forceHdr10Help"},
    {&SettingsControls::hdrChromaLabel, "hdrChromaLabel"},
    {&SettingsControls::hdrChromaCombo, "hdrChromaCombo"},
    {&SettingsControls::hdrChromaHelp, "hdrChromaHelp"},
    {&SettingsControls::mjpegColorLabel, "mjpegColorLabel"},
    {&SettingsControls::mjpegColorCombo, "mjpegColorCombo"},
    {&SettingsControls::mjpegColorHelp, "mjpegColorHelp"},
    {&SettingsControls::sdrRangeLabel, "sdrRangeLabel"},
    {&SettingsControls::sdrRangeCombo, "sdrRangeCombo"},
    {&SettingsControls::sdrRangeHelp, "sdrRangeHelp"},
    {&SettingsControls::driftCombo, "driftCombo"},
    {&SettingsControls::pcmQueueCombo, "pcmQueueCombo"},
    {&SettingsControls::audioStatus, "audioStatus"},
    {&SettingsControls::exclusiveTestButton, "exclusiveTestButton"},
    {&SettingsControls::presentationCombo, "presentationCombo"},
    {&SettingsControls::displayMonitorLabel, "displayMonitorLabel"},
    {&SettingsControls::displayMonitorCombo, "displayMonitorCombo"},
    {&SettingsControls::fullscreenCursorCombo, "fullscreenCursorCombo"},
    {&SettingsControls::scalingCombo, "scalingCombo"},
    {&SettingsControls::videoCombo, "videoCombo"},
    {&SettingsControls::captureDeviceCombo, "captureDeviceCombo"},
    {&SettingsControls::captureAudioDeviceCombo, "captureAudioDeviceCombo"},
    {&SettingsControls::pixelFormatCombo, "pixelFormatCombo"},
    {&SettingsControls::frameRateCombo, "frameRateCombo"},
    {&SettingsControls::pixelCheck, "pixelCheck"},
    {&SettingsControls::relativeSizeCheck, "relativeSizeCheck"},
    {&SettingsControls::relativeSizeWarning, "relativeSizeWarning"},
    {&SettingsControls::borderlessCheck, "borderlessCheck"},
    {&SettingsControls::roundedCornersCheck, "roundedCornersCheck"},
    {&SettingsControls::windowSnapCheck, "windowSnapCheck"},
    {&SettingsControls::saveLogCheck, "saveLogCheck"},
    {&SettingsControls::showConsoleCheck, "showConsoleCheck"},
    {&SettingsControls::skipStartupCheck, "skipStartupCheck"},
    {&SettingsControls::skipStartupHint, "skipStartupHint"},
    {&SettingsControls::preventSleepCheck, "preventSleepCheck"},
    {&SettingsControls::preventSleepHint, "preventSleepHint"},
    {&SettingsControls::checkForUpdatesCheck, "checkForUpdatesCheck"},
    {&SettingsControls::versionWatermark, "versionWatermark"},
    {&SettingsControls::startButton, "startButton"},
    {&SettingsControls::cancelButton, "cancelButton"},
};
struct Geometry {
    HWND SettingsControls::* handle;
    const char* name;
    int x, normalY, warningY, width, height;
};
// Independent layout anchors plus visibility/overlap checks protect the new
// navigation and preserve roomy hit targets across DPI and language variants.
static constexpr Geometry kGeometry[] = {
    {&SettingsControls::brandLabel, "brandLabel", 10, 28, 28, 132, 40},
    {&SettingsControls::tabControl, "tabControl", 10, 100, 100, 132, 220},
    {&SettingsControls::pageTitle, "pageTitle", 184, 24, 24, 792, 40},
    {&SettingsControls::pageSubtitle, "pageSubtitle", 184, 66, 66, 792, 24},
    {&SettingsControls::languageCombo, "languageCombo", 184, 164, 164, 376, 30},
    {&SettingsControls::themeCombo, "themeCombo", 600, 164, 164, 376, 30},
    {&SettingsControls::skipStartupCheck, "skipStartupCheck", 184, 204, 204, 376, 32},
    {&SettingsControls::preventSleepCheck, "preventSleepCheck", 600, 204, 204, 376, 32},
    {&SettingsControls::skipStartupHint, "skipStartupHint", 212, 244, 244, 348, 56},
    {&SettingsControls::preventSleepHint, "preventSleepHint", 628, 244, 244, 348, 56},
    {&SettingsControls::updateTitle, "updateTitle", 184, 328, 328, 792, 24},
    {&SettingsControls::updateText, "updateText", 184, 360, 360, 730, 44},
    {&SettingsControls::checkForUpdatesCheck, "checkForUpdatesCheck", 184, 416, 416, 760, 32},
    {&SettingsControls::updateStatus, "updateStatus", 184, 504, 504, 792, 48},
    {&SettingsControls::versionWatermark, "versionWatermark", 10, 608, 608, 132, 20},
    {&SettingsControls::startButton, "startButton", 762, 602, 602, 102, 34},
    {&SettingsControls::cancelButton, "cancelButton", 876, 602, 602, 100, 34},
    {&SettingsControls::audioOutputSection, "audioOutputSection", 184, 108, 108, 376, 20},
    {&SettingsControls::audioCombo, "audioCombo", 184, 164, 164, 376, 30},
    {&SettingsControls::audioOutputCombo, "audioOutputCombo", 184, 228, 228, 376, 30},
    {&SettingsControls::audioStabilitySection, "audioStabilitySection", 184, 488, 488, 792, 20},
    {&SettingsControls::driftCombo, "driftCombo", 184, 544, 544, 376, 30},
    {&SettingsControls::pcmQueueCombo, "pcmQueueCombo", 600, 544, 544, 376, 30},
    {&SettingsControls::captureDeviceCombo, "captureDeviceCombo", 184, 164, 164, 376, 30},
    {&SettingsControls::captureAudioDeviceCombo, "captureAudioDeviceCombo", 184, 228, 228, 376, 30},
    {&SettingsControls::videoCombo, "videoCombo", 184, 292, 292, 180, 30},
    {&SettingsControls::frameRateCombo, "frameRateCombo", 380, 292, 292, 180, 30},
    {&SettingsControls::pixelFormatCombo, "pixelFormatCombo", 184, 356, 356, 376, 30},
    {&SettingsControls::hdrChromaCombo, "hdrChromaCombo", 304, 440, 440, 224, 30},
    {&SettingsControls::frameOptionsButton, "frameOptionsButton", 464, 262, 262, 96, 24},
    {&SettingsControls::frameOptionsTitle, "frameOptionsTitle", 200, 352, 352, 304, 24},
    {&SettingsControls::frameOptionsClose, "frameOptionsClose", 516, 350, 350, 28, 28},
    {&SettingsControls::frameOptionsHint, "frameOptionsHint", 200, 462, 462, 344, 48},
    {&SettingsControls::capture40Check, "capture40Check", 200, 422, 422, 304, 28},
    {&SettingsControls::unlistedFpsCheck, "unlistedFpsCheck", 200, 386, 386, 304, 28},
    {&SettingsControls::capture40Help, "capture40Help", 520, 424, 424, 24, 24},
    {&SettingsControls::guideLsButton, "guideLsButton", 600, 348, 348, 376, 32},
    {&SettingsControls::upscalerCombo, "upscalerCombo", 600, 420, 420, 376, 30},
    {&SettingsControls::vsrGuideButton, "vsrGuideButton", 952, 390, 390, 24, 24},
    {&SettingsControls::displayResolutionCombo, "displayResolutionCombo", 600, 356, 356, 376, 30},
    {&SettingsControls::vsrStatus, "vsrStatus", 600, 460, 460, 376, 30},
    {&SettingsControls::screenshotTitle, "screenshotTitle", 600, 496, 496, 376, 20},
    {&SettingsControls::screenshotHelp, "screenshotHelp", 600, 556, 556, 376, 18},
    {&SettingsControls::screenshotFolderButton, "screenshotFolderButton", 794, 520, 520, 182, 30},
    {&SettingsControls::relativeSizeCheck, "relativeSizeCheck", 184, 140, 140, 376, 32},
    {&SettingsControls::borderlessCheck, "borderlessCheck", 184, 180, 180, 376, 32},
    {&SettingsControls::roundedCornersCheck, "roundedCornersCheck", 600, 140, 140, 376, 32},
    {&SettingsControls::windowSnapCheck, "windowSnapCheck", 600, 180, 180, 376, 32},
    {&SettingsControls::fullscreenCursorLabel, "fullscreenCursorLabel", 184, 232, 232, 376, 20},
    {&SettingsControls::fullscreenCursorCombo, "fullscreenCursorCombo", 184, 260, 260, 376, 30},
    {&SettingsControls::fullscreenPositionLabel, "fullscreenPositionLabel", 600, 232, 232, 376, 20},
    {&SettingsControls::fullscreenPositionCombo, "fullscreenPositionCombo", 600, 260, 260, 376, 30},
    {&SettingsControls::guideVideoHint, "guideVideoHint", 204, 516, 516, 336, 36},
    {&SettingsControls::guideDiagnosticsText, "guideDiagnosticsText", 600, 148, 148, 376, 32},
    {&SettingsControls::saveLogCheck, "saveLogCheck", 600, 196, 196, 376, 32},
    {&SettingsControls::showConsoleCheck, "showConsoleCheck", 600, 236, 236, 376, 32},
    {&SettingsControls::guideLogFolderButton, "guideLogFolderButton", 600, 284, 284, 185, 32},
    {&SettingsControls::updateNowButton, "updateNowButton", 184, 460, 460, 185, 32},
};
static RECT ClientRectOf(HWND parent, HWND child) {
    RECT r{}; Check(GetWindowRect(child, &r) != FALSE, "read control rectangle");
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&r), 2);
    return r;
}
static bool Visible(HWND hwnd) { return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0; }
static void ExpectVisible(HWND hwnd, bool visible) {
    Check(Visible(hwnd) == visible, "tab-dependent visibility");
    Check((IsWindowEnabled(hwnd) != FALSE) == visible, "hidden controls must not accept input");
}

static void CheckFieldMetrics(SettingsControls& state, HWND parent, UINT dpi) {
    const RECT startup = ClientRectOf(parent, state.skipStartupCheck);
    const RECT power = ClientRectOf(parent, state.preventSleepCheck);
    const RECT hint = ClientRectOf(parent, state.preventSleepHint);
    const RECT updates = ClientRectOf(parent, state.updateTitle);
    const RECT status = ClientRectOf(parent, state.updateStatus);
    Check(startup.top == power.top && startup.right < power.left,
          "startup and sleep options share a row without overlap");
    Check(hint.top >= power.bottom + SettingsPixels(8, dpi) &&
          updates.top >= hint.bottom + SettingsPixels(24, dpi),
          "sleep hint and update section retain spacing");
    Check(status.bottom < SettingsPixels(state.footerTopDip, dpi),
          "update status remains above the footer");
    const struct { HWND label, field; } captureRows[] = {
        {state.captureDeviceLabel, state.captureDeviceCombo},
        {state.captureAudioDeviceLabel, state.captureAudioDeviceCombo},
        {state.videoLabel, state.videoCombo},
        {state.pixelFormatLabel, state.pixelFormatCombo},
        {state.sdrRangeLabel, state.sdrRangeCombo},
    };
    for (size_t row = 1; row < std::size(captureRows); ++row) {
        const RECT previousLabel = ClientRectOf(parent, captureRows[row - 1].label);
        const RECT previousField = ClientRectOf(parent, captureRows[row - 1].field);
        const RECT label = ClientRectOf(parent, captureRows[row].label);
        const RECT field = ClientRectOf(parent, captureRows[row].field);
        Check(std::abs(label.top - previousLabel.top - SettingsPixels(64, dpi)) <= 1 &&
              std::abs(field.top - previousField.top - SettingsPixels(64, dpi)) <= 1,
              "capture rows including SDR range keep the restored sixty-four-DIP rhythm");
        Check(std::abs(label.top - previousField.bottom - SettingsPixels(6, dpi)) <= 1,
              "capture rows leave six DIPs before the next label band");
    }
    const RECT rangeLabel = ClientRectOf(parent, state.sdrRangeLabel);
    const RECT matrixLabel = ClientRectOf(parent, state.mjpegColorLabel);
    const RECT rangeField = ClientRectOf(parent, state.sdrRangeCombo);
    const RECT matrixField = ClientRectOf(parent, state.mjpegColorCombo);
    Check(rangeLabel.top == matrixLabel.top && rangeLabel.bottom == matrixLabel.bottom &&
          rangeField.top == matrixField.top && rangeField.bottom == matrixField.bottom,
          "SDR range and MJPEG color fields share label and field baselines");
    const RECT pixelFormat = ClientRectOf(parent, state.pixelFormatCombo);
    const RECT forceHdr = ClientRectOf(parent, state.forceHdr10Check);
    const RECT hdrChroma = ClientRectOf(parent, state.hdrChromaCombo);
    Check(std::abs(forceHdr.top - pixelFormat.bottom - SettingsPixels(6, dpi)) <= 1 &&
          std::abs(hdrChroma.top - forceHdr.bottom - SettingsPixels(8, dpi)) <= 1,
          "HDR alternatives preserve breathing room above and below the force option");
    const HWND centered[][2] = {
        {state.captureDeviceLabel, state.videoRefreshButton},
        {state.frameRateLabel, state.frameOptionsButton},
        {state.scalingLabel, state.pixelCheck},
        {state.upscalerLabel, state.vsrGuideButton},
        {state.driftLabel, state.driftHelp},
        {state.pcmQueueLabel, state.pcmQueueHelp},
        {state.presentationLabel, state.presentationHelp},
        {state.forceHdr10Check, state.forceHdr10Help},
        {state.hdrChromaCombo, state.hdrChromaHelp},
        {state.mjpegColorCombo, state.mjpegColorHelp},
        {state.sdrRangeCombo, state.sdrRangeHelp},
        {state.volumeBoostCheck, state.volumeBoostHelp},
        {state.guideShortcutsTitle, state.shortcutResetButton},
    };
    for (const auto& pair : centered) {
        const RECT a = ClientRectOf(parent, pair[0]), b = ClientRectOf(parent, pair[1]);
        Check(std::abs((a.top + a.bottom) - (b.top + b.bottom)) <= 1,
              "inline labels/help actions share a vertical center at fractional DPI");
    }
    const HWND inlineActions[][3] = {
        {state.captureDeviceLabel,state.videoRefreshButton,state.captureDeviceCombo},
        {state.frameRateLabel,state.frameOptionsButton,state.frameRateCombo},
        {state.presentationLabel,state.presentationHelp,state.presentationCombo},
        {state.scalingLabel,state.pixelCheck,state.scalingCombo},
        {state.upscalerLabel,state.vsrGuideButton,state.upscalerCombo},
        {state.driftLabel,state.driftHelp,state.driftCombo},
        {state.pcmQueueLabel,state.pcmQueueHelp,state.pcmQueueCombo},
    };
    for(const auto& row:inlineActions) {
        const auto label=ClientRectOf(parent,row[0]), action=ClientRectOf(parent,row[1]), field=ClientRectOf(parent,row[2]);
        Check(action.bottom-action.top==SettingsPixels(24,dpi) &&
            std::abs(field.top-action.bottom-SettingsPixels(6,dpi))<=1 &&
            action.left-label.right==SettingsPixels(12,dpi),
            "inline actions have equal height, caption gutter and field clearance");
        Check(action.right==field.right,"inline actions align with the field's right edge");
    }
    const struct { HWND label, field; } pairs[] = {
        {state.audioLabel, state.audioCombo},
        {state.audioOutputLabel, state.audioOutputCombo},
        {state.bufferLabel, state.bufferCombo},
        {state.volumeHudLabel, state.volumeHudCombo},
        {state.driftLabel, state.driftCombo},
        {state.pcmQueueLabel, state.pcmQueueCombo},
        {state.captureDeviceLabel, state.captureDeviceCombo},
        {state.captureAudioDeviceLabel, state.captureAudioDeviceCombo},
        {state.videoLabel, state.videoCombo},
        {state.frameRateLabel, state.frameRateCombo},
        {state.pixelFormatLabel, state.pixelFormatCombo},
        {state.presentationLabel, state.presentationCombo},
        {state.displayMonitorLabel, state.displayMonitorCombo},
        {state.scalingLabel, state.scalingCombo},
        {state.hdrChromaLabel, state.hdrChromaCombo},
        {state.mjpegColorLabel, state.mjpegColorCombo},
        {state.sdrRangeLabel, state.sdrRangeCombo},
        {state.fullscreenCursorLabel, state.fullscreenCursorCombo},
        {state.fullscreenPositionLabel, state.fullscreenPositionCombo},
        {state.languageLabel, state.languageCombo},
        {state.themeLabel, state.themeCombo},
    };
    for (const auto& pair : pairs) {
        Check((GetWindowLongPtrW(pair.field, GWL_STYLE) & (CBS_OWNERDRAWFIXED | CBS_HASSTRINGS)) ==
              (CBS_OWNERDRAWFIXED | CBS_HASSTRINGS), "all dropdown rows use centered paint with native strings");
        const RECT label = ClientRectOf(parent, pair.label);
        const RECT field = ClientRectOf(parent, pair.field);
        if (pair.field == state.hdrChromaCombo) {
            Check(field.left-label.right == SettingsPixels(8,dpi) &&
                label.top-field.top == SettingsPixels(4,dpi),
                "HDR inline label remains aligned with the field above 40fps");
        } else {
            Check(field.left == label.left && field.top-label.bottom == SettingsPixels(8,dpi),
                "all tabs share the video page's eight-DIP label-to-field gap");
        }
        if (field.bottom - field.top != SettingsPixels(30, dpi)) {
            wchar_t name[128]{};
            GetWindowTextW(pair.label, name, ARRAYSIZE(name));
            std::fwprintf(stderr, L"Combo metric mismatch: %ls id=%d dpi=%u rect=(%ld,%ld,%ld,%ld) selection=%lld row=%lld\n",
                name, GetDlgCtrlID(pair.field), dpi, field.left, field.top, field.right, field.bottom,
                static_cast<long long>(SendMessageW(pair.field, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0)),
                static_cast<long long>(SendMessageW(pair.field, CB_GETITEMHEIGHT, 0, 0)));
        }
        Check(field.bottom - field.top == SettingsPixels(30, dpi),
              "native closed combo really retains a thirty-DIP hit target after layout");
        Check(SendMessageW(pair.field, CB_GETITEMHEIGHT, 0, 0) == SettingsPixels(24, dpi),
              "expanded native combo keeps readable twenty-four-DIP rows");
        HDC dc = GetDC(pair.field);
        const auto previous = SelectObject(dc,
            reinterpret_cast<HFONT>(SendMessageW(pair.field, WM_GETFONT, 0, 0)));
        TEXTMETRICW metrics{};
        Check(GetTextMetricsW(dc, &metrics) != FALSE, "read real combo font metrics");
        SelectObject(dc, previous);
        ReleaseDC(pair.field, dc);
        Check(SendMessageW(pair.field, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0) >= metrics.tmHeight,
              "collapsed native combo can contain the actual font without vertical clipping");
    }
    Check(ClientRectOf(parent, state.audioStabilitySection).top -
          ClientRectOf(parent, state.surround51Hint).bottom == SettingsPixels(24, dpi),
          "major audio sections have a twenty-four-DIP separation");
    Check(std::abs(ClientRectOf(parent, state.screenshotTitle).top -
          ClientRectOf(parent, state.vsrStatus).bottom - SettingsPixels(6, dpi)) <= 1,
          "screenshot actions remain separated from conditional filter details");
    const RECT screenshotHelp = ClientRectOf(parent, state.screenshotHelp);
    const RECT screenshotButton = ClientRectOf(parent, state.screenshotFolderButton);
    const RECT screenshotTitle = ClientRectOf(parent,state.screenshotTitle);
    const RECT screenshotCheck = ClientRectOf(parent,state.screenshotClipboardCheck);
    Check(screenshotHelp.left == screenshotTitle.left && screenshotCheck.left == screenshotTitle.left &&
          screenshotButton.left-screenshotCheck.right == SettingsPixels(12,dpi) &&
          std::abs((screenshotButton.top+screenshotButton.bottom)-(screenshotCheck.top+screenshotCheck.bottom))<=1,
          "screenshot actions retain aligned centers and a consistent gutter");
    Check(std::abs(screenshotHelp.bottom - SettingsPixels(kSettingsFooterTopDip-16,dpi))<=1 &&
          screenshotHelp.top-screenshotButton.bottom >= SettingsPixels(6,dpi)-1,
          "screenshot group retains sixteen DIPs of footer margin without shrinking controls");
    const auto detected=ClientRectOf(parent,state.videoCapabilityStatus);
    const auto requested=ClientRectOf(parent,state.unlistedFpsStatus);
    const bool extra=GetWindowTextLengthW(state.unlistedFpsStatus)>0;
    const auto bottom=extra ? requested.bottom : detected.bottom;
    Check(bottom>=SettingsPixels(state.footerTopDip-24,dpi)-1 &&
          bottom<=SettingsPixels(state.footerTopDip-8,dpi)+1 &&
          detected.top>=ClientRectOf(parent,state.hdrChromaCombo).bottom+SettingsPixels(2,dpi)-1,
          "detected modes retain footer padding without overlapping HDR options");
    CheckCaptionFits(state.videoCapabilityStatus,"complete detected mode list",dpi,state.english,0,true,false);
    if(extra) Check(requested.top>=detected.bottom+SettingsPixels(4,dpi)-1,
        "unlisted FPS note remains separate below detected modes");
    for(HWND last:{state.driftCombo,state.pcmQueueCombo,state.relativeSizeWarning,state.updateStatus})
        Check(ClientRectOf(parent,last).bottom<=SettingsPixels(state.footerTopDip-16,dpi)+1,
            "audio, window and app sections retain at least sixteen DIPs of bottom margin");
    Check(ClientRectOf(parent,state.guideVideoHint).bottom+SettingsPixels(20,dpi)<=SettingsPixels(state.footerTopDip-16,dpi)+1,
        "shortcut card includes its padding inside the shared footer margin");
    Check(ClientRectOf(parent,state.roundedCornersCheck).top==ClientRectOf(parent,state.relativeSizeCheck).top &&
          ClientRectOf(parent,state.windowSnapCheck).top==ClientRectOf(parent,state.borderlessCheck).top,
          "window options form a balanced two-column grid");
    wchar_t savedDetected[4096]{},savedRequested[512]{};
    GetWindowTextW(state.videoCapabilityStatus,savedDetected,ARRAYSIZE(savedDetected));
    GetWindowTextW(state.unlistedFpsStatus,savedRequested,ARRAYSIZE(savedRequested));
    for(const auto caption : {L"Checking modes...", L"Detected:\nNV12 144/120/60 fps\nYUY2 60 fps\nP010 60 fps",
                             FullCapabilityCaption(state.english),UnsupportedCapabilityCaption(state.english)}) {
        for(const bool direct : {false,true}) {
            SetWindowTextW(state.videoCapabilityStatus,caption);
            SetWindowTextW(state.unlistedFpsStatus,direct ? L"Direct request: 30 fps" : L"");
            LayoutSettingsControls(&state,dpi);
            const auto info=ClientRectOf(parent,state.videoCapabilityStatus);
            const auto note=ClientRectOf(parent,state.unlistedFpsStatus);
            const auto blockBottom=direct?note.bottom:info.bottom;
            Check(blockBottom>=SettingsPixels(state.footerTopDip-24,dpi)-1 &&
                  blockBottom<=SettingsPixels(state.footerTopDip-8,dpi)+1,
                "one/six-line summaries retain bottom padding after refresh");
            Check(info.top>=ClientRectOf(parent,state.hdrChromaCombo).bottom+SettingsPixels(2,dpi)-1,
                "six-line capability summary never overlaps HDR options");
            CheckCaptionFits(state.videoCapabilityStatus,"refreshed complete mode list",dpi,state.english,0,true,false);
            if(std::wcscmp(caption,FullCapabilityCaption(state.english))!=0)
                Check(std::abs(blockBottom-SettingsPixels(state.footerTopDip-24,dpi))<=1,
                    "existing short summaries retain their original twenty-four-DIP footer margin");
            if(direct) Check(note.top-info.bottom>=SettingsPixels(4,dpi)-1,
                "adding direct-request status never overlaps detected text");
        }
    }
    SetWindowTextW(state.videoCapabilityStatus,savedDetected);
    SetWindowTextW(state.unlistedFpsStatus,savedRequested);
    LayoutSettingsControls(&state,dpi);
    const HWND displayLabels[]{state.presentationLabel,state.displayMonitorLabel,state.scalingLabel,
        state.displayResolutionLabel,state.upscalerLabel};
    const HWND displayFields[]{state.presentationCombo,state.displayMonitorCombo,state.scalingCombo,
        state.displayResolutionCombo,state.upscalerCombo};
    for(size_t row=0;row<std::size(displayLabels);++row) {
        const auto label=ClientRectOf(parent,displayLabels[row]);
        const auto field=ClientRectOf(parent,displayFields[row]);
        Check(field.left==SettingsPixels(600,dpi) && field.right==SettingsPixels(976,dpi) &&
            field.top-label.top==SettingsPixels(28,dpi),"display fields share width and label gap");
        Check(label.top==ClientRectOf(parent,captureRows[row].label).top,
            "capture and display columns share row baselines");
        if(row) Check(std::abs(label.top-ClientRectOf(parent,displayLabels[row-1]).top-SettingsPixels(64,dpi))<=1,
            "display fields keep the restored sixty-four-DIP rhythm");
    }
}

static void CheckFontHierarchy(const SettingsControls& state, UINT dpi, bool english) {
    Check(state.uiFonts.size() == 4, "DPI changes replace four fonts instead of accumulating");
    const struct { HWND control; int points, weight; } roles[] = {
        {state.audioLabel, 10, FW_MEDIUM},
        {state.audioCombo, 10, FW_MEDIUM},
        {state.tabControl, 10, FW_MEDIUM},
        {state.cancelButton, 10, FW_MEDIUM},
        {state.audioOutputSection, 10, FW_SEMIBOLD},
        {state.startButton, 10, FW_SEMIBOLD},
        {state.pageSubtitle, 9, FW_NORMAL},
        {state.videoCapabilityStatus, 9, FW_NORMAL},
        {state.unlistedFpsStatus, 9, FW_NORMAL},
        {state.vsrStatus, 9, FW_NORMAL},
        {state.pageTitle, 20, FW_SEMIBOLD},
    };
    for (const auto& role : roles) {
        LOGFONTW actual{};
        Check(GetObjectW(reinterpret_cast<HFONT>(SendMessageW(role.control, WM_GETFONT, 0, 0)),
                         sizeof(actual), &actual) == sizeof(actual), "control font remains a live GDI object");
        Check(actual.lfHeight == -MulDiv(role.points, dpi, 72) && actual.lfWeight == role.weight,
              "body, secondary, section and page title font hierarchy is explicit");
        Check(std::wcscmp(actual.lfFaceName, SettingsFontFamily(role.weight, english)) == 0 &&
              std::wcsstr(actual.lfFaceName, L"Pretendard") != nullptr,
              "both languages use the embedded font without installation");
        HDC fontDc = GetDC(role.control);
        HGDIOBJ previousFont = SelectObject(fontDc, reinterpret_cast<HFONT>(
            SendMessageW(role.control, WM_GETFONT, 0, 0)));
        wchar_t resolvedFace[64]{};
        GetTextFaceW(fontDc, ARRAYSIZE(resolvedFace), resolvedFace);
        Check(std::wcscmp(resolvedFace, actual.lfFaceName) == 0, "GDI selects the actual bundled font");
        // The CFF edition resolves to the same family but silently loses GDI
        // ClearType. Checking only GetTextFace/lfQuality missed that regression.
        Check(GetFontData(fontDc, 0x66796c67, 0, nullptr, 0) != GDI_ERROR &&
              GetFontData(fontDc, 0x20464643, 0, nullptr, 0) == GDI_ERROR,
              "bundled settings fonts have TrueType glyf outlines, not CFF");
        Check(actual.lfQuality == CLEARTYPE_QUALITY,
              "settings request ClearType without changing Windows preferences");
        SelectObject(fontDc, previousFont); ReleaseDC(role.control, fontDc);
    }
}

static void CheckWindowBehaviorReflow(SettingsControls& state, HWND parent, UINT dpi) {
    state.activeTab = SettingsTab::Window;
    LayoutSettingsControls(&state, dpi);
    const RECT captureBefore = ClientRectOf(parent, state.captureDeviceCombo);
    const struct { bool pixel, relative; } transitions[] = {
        {false, false}, {true, true}, {true, false},
        {true, true}, {false, true}, {true, true},
    };
    for (const auto& step : transitions) {
        SendMessageW(state.pixelCheck, BM_SETCHECK, step.pixel ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(state.relativeSizeCheck, BM_SETCHECK, step.relative ? BST_CHECKED : BST_UNCHECKED, 0);
        // Exercise checkbox-change handling without a full page relayout.
        UpdateWindowBehaviorVisibility(&state);
        const bool warning = step.pixel && step.relative;
        ExpectVisible(state.relativeSizeWarning, warning);
        const RECT borderless = ClientRectOf(parent, state.borderlessCheck);
        const RECT cursor = ClientRectOf(parent, state.fullscreenCursorCombo);
        Check(borderless.top == SettingsPixels(180, dpi) &&
              cursor.top == SettingsPixels(260, dpi),
              "conditional warning never shifts window options");
        Check(ClientRectOf(parent, state.relativeSizeWarning).top >
              ClientRectOf(parent, state.fullscreenCursorHint).bottom,
              "conditional note has a separate reserved row");
        if (!warning)
            Check(borderless.top - ClientRectOf(parent, state.relativeSizeCheck).bottom == SettingsPixels(8, dpi),
                  "hidden warning leaves only an eight-DIP gap between window options");
        const RECT captureAfter = ClientRectOf(parent, state.captureDeviceCombo);
        Check(EqualRect(&captureBefore, &captureAfter) != FALSE,
              "window warning reflow never moves another page's controls");
        CheckFieldMetrics(state, parent, dpi);
    }
}

static void CheckCaptionFits(HWND control, const char* name, UINT dpi,
                             bool english, int horizontalPadding, bool wrap, bool singleLine = true) {
    wchar_t caption[4096]{};
    GetWindowTextW(control, caption, ARRAYSIZE(caption));
    if (!caption[0]) return;
    RECT client{}; GetClientRect(control, &client);
    const int availableWidth = client.right - SettingsPixels(horizontalPadding, dpi);
    HDC dc = GetDC(control);
    const auto previous = SelectObject(dc,
        reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
    RECT measured{0, 0, availableWidth, 0};
    DrawTextW(dc, caption, -1, &measured,
        DT_CALCRECT | DT_NOPREFIX | DT_EXPANDTABS | (wrap ? DT_WORDBREAK : singleLine ? DT_SINGLELINE : 0));
    SelectObject(dc, previous); ReleaseDC(control, dc);
    if (measured.right > availableWidth || measured.bottom > client.bottom) {
        std::fprintf(stderr,
            "Text clipped: %s lang=%s dpi=%u page-caption size=%ldx%ld available=%dx%ld wrap=%d\n",
            name, english ? "en" : "ko", dpi, measured.right, measured.bottom,
            availableWidth, client.bottom, wrap);
        std::abort();
    }
}

static void CheckShortcutCaptionFits(SettingsControls&,HWND control,UINT dpi) {
    wchar_t caption[256]{}; GetWindowTextW(control,caption,256);
    RECT box{}; GetClientRect(control,&box);
    const int width=box.right-SettingsPixels(17,dpi);
    HDC dc=GetDC(control);
    const auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0)));
    RECT measured{0,0,width,0}; DrawTextW(dc,caption,-1,&measured,DT_CALCRECT|DT_NOPREFIX|DT_SINGLELINE);
    Check(measured.right<=width && measured.bottom<=box.bottom,"full shortcut fits growing keycap on one line");
    SelectObject(dc,old); ReleaseDC(control,dc);
}

static void CheckFrameOptionsLayout(SettingsControls& state, HWND parent, UINT dpi, bool english) {
    const RECT bounds = SettingsFrameOptionsBounds(&state, parent);
    const RECT capture = ClientRectOf(parent, state.videoCombo);
    const RECT frame = ClientRectOf(parent, state.frameRateCombo);
    const RECT title = ClientRectOf(parent, state.frameOptionsTitle);
    const RECT close = ClientRectOf(parent, state.frameOptionsClose);
    const RECT unlisted = ClientRectOf(parent, state.unlistedFpsCheck);
    const RECT capture40 = ClientRectOf(parent, state.capture40Check);
    const RECT help = ClientRectOf(parent, state.capture40Help);
    const RECT hint = ClientRectOf(parent, state.frameOptionsHint);
    const int padding = SettingsPixels(16, dpi);
    Check(bounds.left == capture.left && bounds.right == frame.right &&
          bounds.top - frame.bottom == SettingsPixels(12, dpi),
          "flyout outer edges align with the capture fields and keep a 12-DIP top gap");
    Check(title.left == unlisted.left && title.left == capture40.left && title.left == hint.left &&
          title.left - bounds.left == padding && close.top - bounds.top == padding &&
          bounds.right - close.right == padding && bounds.right - help.right == padding &&
          bounds.right - hint.right == padding && bounds.bottom - hint.bottom == padding,
          "flyout text, checkboxes and actions share 16-DIP card margins");
    Check(std::abs((title.top + title.bottom) - (close.top + close.bottom)) <= 1 &&
          std::abs((capture40.top + capture40.bottom) - (help.top + help.bottom)) <= 1,
          "flyout close and help buttons stay vertically centered on their rows");
    Check(unlisted.top - close.bottom == SettingsPixels(8, dpi) &&
          capture40.top - unlisted.bottom == SettingsPixels(8, dpi) &&
          hint.top - capture40.bottom == SettingsPixels(12, dpi),
          "flyout header, option and hint gaps follow the same spacing rhythm");
    CheckCaptionFits(state.frameOptionsTitle,"frame options title",dpi,english,0,false);
    CheckCaptionFits(state.frameOptionsHint,"frame options three-line hint",dpi,english,0,true,false);
    CheckCaptionFits(state.unlistedFpsCheck,"frame options unlisted checkbox",dpi,english,27,false);
    CheckCaptionFits(state.capture40Check,"frame options 40fps checkbox",dpi,english,27,false);
    const LONG centerX = (bounds.left + bounds.right) / 2;
    const LONG centerY = (bounds.top + bounds.bottom) / 2;
    for (POINT point : {POINT{bounds.left + 1,centerY},POINT{bounds.right - 1,centerY},
                        POINT{centerX,bounds.top + 1},POINT{centerX,bounds.bottom - 1}}) {
        HandleFrameOptionsMessage(&state,parent,WM_LBUTTONDOWN,0,MAKELPARAM(point.x,point.y));
        Check(state.frameOptionsOpen,"clicks inside every painted card margin keep the flyout open");
    }
}

static void CheckHdrChromaChoicesFit(SettingsControls& state, UINT dpi, bool english) {
    for (const auto& field : {std::pair{state.sdrRangeCombo, state.sdrRangeLabel},
                              std::pair{state.mjpegColorCombo, state.mjpegColorLabel}}) {
        RECT bounds{}; GetClientRect(field.first, &bounds);
        HDC colorDc = GetDC(field.first);
        const auto originalFont = SelectObject(colorDc,
            reinterpret_cast<HFONT>(SendMessageW(field.first, WM_GETFONT, 0, 0)));
        Check(SendMessageW(field.first, CB_GETCOUNT, 0, 0) == 3, "SDR controls each expose exactly three choices");
        for (int row = 0; row < 3; ++row) {
            wchar_t label[128]{};
            Check(SendMessageW(field.first, CB_GETLBTEXT, row, reinterpret_cast<LPARAM>(label)) != CB_ERR,
                  "read each SDR range/matrix choice");
            SIZE size{};
            Check(GetTextExtentPoint32W(colorDc, label, static_cast<int>(std::wcslen(label)), &size) != FALSE,
                  "measure actual SDR range/matrix font");
            Check(size.cx <= bounds.right - SettingsPixels(11, dpi) - SettingsPixels(31, dpi) &&
                  size.cy <= bounds.bottom, "all SDR color choices fit the closed field including theme insets");
        }
        SelectObject(colorDc, originalFont); ReleaseDC(field.first, colorDc);
        CheckCaptionFits(field.second, "SDR color field label", dpi, english, 0, false);
    }
    const HWND combo = state.hdrChromaCombo;
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    Check(count == 3, "measure every actual HDR chroma choice");
    RECT client{}; GetClientRect(combo, &client);
    // Match SettingsTheme::PaintControl's separately rounded 11/31-DIP
    // insets, not the expanded list width or the full closed control width.
    const int availableWidth = client.right - SettingsPixels(11, dpi) - SettingsPixels(31, dpi);
    HDC dc = GetDC(combo);
    const auto previous = SelectObject(dc,
        reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0)));
    for (LRESULT index = 0; index < count; ++index) {
        Check(SendMessageW(combo, CB_SETCURSEL, index, 0) != CB_ERR,
              "select every HDR caption in the closed field");
        wchar_t caption[256]{};
        Check(GetWindowTextW(combo, caption, ARRAYSIZE(caption)) > 0,
              "read actual selected HDR caption");
        SIZE measured{};
        Check(GetTextExtentPoint32W(dc, caption, static_cast<int>(std::wcslen(caption)), &measured) != FALSE,
              "measure HDR caption using the actual control font");
        if (measured.cx > availableWidth || measured.cy > client.bottom)
            std::fwprintf(stderr, L"HDR choice clipped: %ls lang=%ls dpi=%u needed=%ldx%ld available=%dx%ld\n",
                caption, english ? L"en" : L"ko", dpi, measured.cx, measured.cy, availableWidth, client.bottom);
        Check(measured.cx <= availableWidth && measured.cy <= client.bottom,
              "every HDR choice fits the closed field with theme padding");
    }
    SelectObject(dc, previous); ReleaseDC(combo, dc);
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
    Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == selected,
          "caption measurements preserve the user's HDR selection");
    CheckCaptionFits(state.hdrChromaLabel, "HDR chroma label", dpi, english, 0, false);
}

static void CheckAllCaptionsFit(SettingsControls& state, UINT dpi, bool english) {
    for (const auto& entry : kMembers) {
        const HWND control = state.*entry.handle;
        wchar_t kind[32]{}; GetClassNameW(control, kind, ARRAYSIZE(kind));
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(control, GWL_STYLE));
        if (_wcsicmp(kind, L"STATIC") == 0) {
            const DWORD type = style & SS_TYPEMASK;
            CheckCaptionFits(control, entry.name, dpi, english, 0,
                             type == SS_LEFT || type == SS_CENTER || type == SS_RIGHT,
                             (style & SS_CENTERIMAGE) != 0);
        } else if (_wcsicmp(kind, L"BUTTON") == 0) {
            const DWORD type = style & BS_TYPEMASK;
            const bool checkbox = type == BS_AUTOCHECKBOX || type == BS_CHECKBOX;
            // Match SettingsTheme's actual content inset rather than measuring
            // against the full HWND and accidentally accepting clipped text.
            CheckCaptionFits(control, entry.name, dpi, english, checkbox ? 27 : 14,
                             checkbox && (style & BS_MULTILINE));
        }
        // User device names are unbounded. Collapsed native combo fields use
        // ellipsis intentionally; expanded device lists retain wider fields.
    }
    wchar_t guide[2048]{}; GetWindowTextW(state.guideVideoHint, guide, ARRAYSIZE(guide));
    Check(std::wcsstr(guide,english ? L"Click" : L"키 버튼") &&
          std::wcsstr(guide,english ? L"* Video mode only" : L"* 영상 모드 전용"),
          "shortcut editor explains editing and video-only keys");
    const HWND parent = GetParent(state.guideVideoHint);
    for (size_t i = 0; i < llcv::viewer_help::kShortcuts.size(); ++i) {
        const auto& shortcut = llcv::viewer_help::kShortcuts[i];
        for (HWND control : {state.guideKeys[i], state.guideDescriptions[i]}) {
            Check(IsWindow(control) != FALSE, "shortcut native text exists");
            Check(bool(GetWindowLongPtrW(control, GWL_STYLE) & WS_TABSTOP)==
                  (control==state.guideKeys[i] && i<llcv::shortcuts::Count),
                  "all keys are keyboard accessible; descriptions are read-only");
            if (control==state.guideKeys[i]) CheckShortcutCaptionFits(state,control,dpi);
            else CheckCaptionFits(control,"shortcut row",dpi,english,0,true,false);
        }
        GetWindowTextW(state.guideKeys[i], guide, ARRAYSIZE(guide));
        Check(std::wcscmp(guide, shortcut.key) == 0, "every key matches F1");
        GetWindowTextW(state.guideDescriptions[i], guide, ARRAYSIZE(guide));
        Check(std::wstring(guide)==std::wstring(english ? shortcut.english : shortcut.korean)+
              (shortcut.videoOnly ? L" *" : L""),
              "every action matches F1 with a compact video-only marker");
        const auto action = ClientRectOf(parent, state.guideDescriptions[i]);
        const auto key = ClientRectOf(parent, state.guideKeys[i]);
        Check(action.left == SettingsPixels(296, dpi) &&
              action.top == SettingsPixels(153 + static_cast<int>(i) *
                  (state.guideKeys.size()>10 ? 32 : state.guideKeys.size()>9 ? 35 : 36), dpi),
              "shortcut rows maintain aligned columns and generous rhythm");
        Check(key.top>=action.top && key.bottom<=action.bottom+1 && key.right<action.left,
              "editable key and wrapping action occupy separate aligned columns");
    }
    Check(ClientRectOf(parent, state.guideShortcutsTitle).top == ClientRectOf(parent, state.guideDiagnosticsTitle).top,
          "guide section titles align");
    Check(ClientRectOf(parent, state.saveLogCheck).top - ClientRectOf(parent, state.guideDiagnosticsText).bottom == SettingsPixels(16, dpi) &&
          ClientRectOf(parent, state.showConsoleCheck).top - ClientRectOf(parent, state.saveLogCheck).bottom == SettingsPixels(8, dpi) &&
          ClientRectOf(parent, state.guideLogFolderButton).top - ClientRectOf(parent, state.showConsoleCheck).bottom == SettingsPixels(16, dpi),
          "diagnostics explanation, options and action use intentional compact spacing");
    HDC dc = GetDC(state.tabControl);
    const auto previous = SelectObject(dc,
        reinterpret_cast<HFONT>(SendMessageW(state.tabControl, WM_GETFONT, 0, 0)));
    for (int i = 0; i < kSettingsNavigationCount; ++i) {
        wchar_t text[256]{};
        SendMessageW(state.tabControl, LB_GETTEXT, i, reinterpret_cast<LPARAM>(text));
        SIZE size{}; GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &size);
        Check(size.cx <= SettingsPixels(107, dpi) && size.cy <= SettingsPixels(44, dpi),
              "localized sidebar captions fit their themed content area");
    }
    SelectObject(dc, previous); ReleaseDC(state.tabControl, dc);
}
static void TestHelpText() {
    using llcv::settings::PresentationMode;
    const PresentationMode modes[] = {
        PresentationMode::AllowTearing, PresentationMode::VSync,
        PresentationMode::Compatibility, PresentationMode::ExclusiveFullscreen};
    for (int i = 0; i < llcv::presentation_ui::ModeCount; ++i) {
        Check(llcv::presentation_ui::ModeFromSelection(i) == modes[i] &&
              llcv::presentation_ui::SelectionFromMode(modes[i]) == i,
              "presentation selection conversion preserves existing order and exclusive option");
    }
    Check(llcv::presentation_ui::ModeFromSelection(-1) == PresentationMode::AllowTearing &&
          llcv::presentation_ui::ModeFromSelection(4) == PresentationMode::AllowTearing &&
          llcv::presentation_ui::SelectionFromMode(static_cast<PresentationMode>(99)) == 0,
          "invalid or missing native selection retains immediate fallback");
    for (bool english : {false, true}) {
        Check(std::wcsstr(llcv::presentation_ui::HelpText(english), L"Alt+Tab") &&
              std::wcsstr(llcv::presentation_ui::HelpText(english),
                  english ? L"fullscreen shortcut" : L"전체화면 단축키") &&
              std::wcsstr(llcv::presentation_ui::ExclusiveFullscreenLabel(english),
                  english ? L"exclusive fullscreen" : L"독점 전체화면"),
              "exclusive output explains focus suspension and customizable fullscreen shortcut");
        Check(std::wcsstr(llcv::presentation_ui::ImmediateLabel(english),
                  english ? L"Low latency" : L"저지연") &&
              std::wcsstr(llcv::presentation_ui::ExclusiveFullscreenLabel(english),
                  english ? L"Low latency" : L"저지연") &&
              !std::wcsstr(llcv::presentation_ui::HelpText(english),
                  english ? L"minimum latency" : L"최소 지연"),
              "both low-latency modes are named consistently without an absolute latency claim");
        const auto* ls = SettingsHelpText(SettingsHelpTopic::Capture40, english);
        const auto* caution = std::wcsstr(ls, english ? L"Keep in mind" : L"주의사항");
        const auto* example = std::wcsstr(ls, english ? L"Example: Lossless Scaling" : L"활용 예시: Lossless Scaling");
        Check(std::wcsstr(ls, english ? L"40 fps capture" : L"40fps 캡처") == ls &&
            std::wcsstr(ls, english ? L"Leave off for normal viewing" : L"일반적인 시청에는 끈 채") &&
            caution && example && caution < example,
            "capture behavior and cautions precede the optional external-app example");
        Check(std::wcsstr(ls,L"Lossless Scaling") && std::wcsstr(ls,L"VRR") &&
            std::wcsstr(ls,english ? L"Start" : L"시작") &&
            std::wcsstr(ls,english ? L"latency" : L"지연") &&
            std::wcsstr(ls,english ? L"LS 3x" : L"LS 3배"),
            "short 40fps help retains startup validation, LS example and caveats");
        Check(std::wcsstr(ls, english ? L"Assumes steady 40 fps" : L"40fps 유지 시") &&
            std::wcsstr(ls, english ? L"At 30–40 fps" : L"30~40fps로 흔들리면") &&
            std::wcsstr(ls, english ? L"stutter may persist or become more noticeable with LS" :
                L"LS를 써도 끊김이 남거나 더 두드러질 수 있습니다"),
            "LS example warns that variable game FPS can retain or emphasize stutter");
        Check(std::wcslen(ls) <= (english ? 550u : 350u) &&
            std::wcsstr(ls,L"\n\n") && std::wcsstr(ls,L"•"),
            "40fps popup stays brief with separated sections and short bullets");
        const wchar_t* guide = llcv::ui_text::VsrSetupGuide(english);
        Check(std::wcsstr(guide,L"NV12/YUY2") && std::wcsstr(guide,L"MJPEG"),
            "VSR guide includes both raw SDR formats and decoded MJPEG");
        Check(guide && *guide && std::wcsstr(guide, L"F6") &&
              std::wcsstr(guide, L"NVIDIA App") && std::wcsstr(guide, L"NV12") &&
              std::wcsstr(guide, english ? L"latency" : L"지연"),
              "bilingual VSR setup guide explains activation, supported input and latency");
        Check(std::wcsstr(guide, L"P010 HDR10") &&
              std::wcsstr(guide, english ? L"native HDR10 is preserved" : L"원래 HDR10을 유지"),
              "VSR guide describes native HDR without SDR-to-HDR conversion");
        Check(std::wcsstr(guide, english ? L"cannot verify activation" : L"확인하지 못합니다") &&
              !std::wcsstr(guide, L"HRESULT") && !std::wcsstr(guide, L"Request supported"),
              "VSR guide does not present diagnostic acceptance as actual activation");
    }
    Check(std::wcsstr(SettingsHelpText(SettingsHelpTopic::HdrChroma, true), L"staggered") != nullptr &&
          std::wcsstr(SettingsHelpText(SettingsHelpTopic::HdrChroma, false), L"보장하지") != nullptr,
          "chroma help discloses interpretation override limitations in both languages");
    const struct { SettingsHelpTopic topic; uint32_t english, korean; } golden[] = {
    {SettingsHelpTopic::Drift, 754620631u, 3340162937u},
    {SettingsHelpTopic::PcmQueue, 2979267523u, 1200270022u},
    {SettingsHelpTopic::VolumeBoost, 2473238580u, 2846859296u},
    };
    for (const auto& entry : golden) for (bool english : {false, true}) {
        uint32_t hash = 2166136261u;
        for (const wchar_t* p = SettingsHelpText(entry.topic, english); *p; ++p)
            hash = (hash ^ static_cast<uint16_t>(*p)) * 16777619u;
        Check(hash == (english ? entry.english : entry.korean), "help text must remain byte-for-byte equivalent");
    }
    for (bool english : {false, true})
        Check(std::wcscmp(SettingsHelpText(SettingsHelpTopic::Presentation, english),
                         llcv::presentation_ui::HelpText(english)) == 0, "presentation help remains centralized");
    for (bool english : {false, true}) {
        const auto* rangeHelp = SettingsHelpText(SettingsHelpTopic::SdrRange, english);
        const auto* matrixHelp = SettingsHelpText(SettingsHelpTopic::MjpegColor, english);
        Check(std::wcsstr(rangeHelp, L"NV12/YUY2") && std::wcsstr(rangeHelp, L"MJPEG") &&
              std::wcsstr(rangeHelp, L"P010/HDR") && std::wcsstr(rangeHelp, L"Limited") &&
              std::wcsstr(rangeHelp, L"Full") && std::wcsstr(rangeHelp, english ? L"screenshots" : L"스크린샷"),
              "SDR range help explains defaults, shared consumers and HDR exclusion");
        Check(std::wcsstr(matrixHelp, L"BT.709") && std::wcsstr(matrixHelp, L"BT.601") &&
              std::wcsstr(matrixHelp, english ? L"SDR color range" : L"SDR 색 범위") &&
              !std::wcsstr(matrixHelp, L"Full/Limited"),
              "MJPEG matrix help does not duplicate the shared range choices");
    }
    Check(std::wcsstr(SettingsHelpText(SettingsHelpTopic::ForceHdr10, true), L"already defaults") != nullptr &&
          std::wcsstr(SettingsHelpText(SettingsHelpTopic::ForceHdr10, false), L"기본적으로") != nullptr &&
          std::wcsstr(SettingsHelpText(SettingsHelpTopic::ForceHdr10, true), L"NV12/YUY2") != nullptr &&
          std::wcsstr(SettingsHelpText(SettingsHelpTopic::ForceHdr10, false), L"NV12/YUY2") != nullptr &&
          std::wcsstr(SettingsHelpText(SettingsHelpTopic::ForceHdr10, true), L"Windows HDR") != nullptr &&
          std::wcsstr(SettingsHelpText(SettingsHelpTopic::ForceHdr10, false), L"Windows HDR") != nullptr,
          "HDR help explains automatic assumption, SDR alternative and Windows HDR requirement");
}

static void TestTranslationBoundary() {
    using llcv::ui_text::Translate;
    Check(Translate(nullptr, true) == nullptr && Translate(nullptr, false) == nullptr, "null translation input");
    const wchar_t* unknown = L"An unregistered caption";
    Check(Translate(unknown, true) == unknown && Translate(unknown, false) == unknown, "unknown text preserves caller pointer");
    const wchar_t* korean = L"오디오 출력 모드";
    Check(Translate(korean, false) == korean, "Korean text preserves caller pointer");
    const wchar_t* translated = Translate(korean, true);
    Check(std::wcscmp(translated, L"Audio output mode") == 0, "English dictionary lookup");
    for (int i = 0; i < 1000; ++i) {
        Check(Translate(korean, true) == translated, "translation storage survives repeated lookups");
        Check(Translate(korean, false) == korean, "language switches do not mutate dictionary");
    }
    // Lookups compare text, not input addresses, and translations must never
    // borrow the caller's mutable storage (including longer, non-SSO captions).
    wchar_t mutableCaption[] = L"Windows 기본 출력 장치 따라가기 (권장)";
    const wchar_t* stable = Translate(mutableCaption, true);
    Check(std::wcscmp(stable, L"Follow Windows default output (recommended)") == 0,
          "long caption translates from caller-owned storage");
    mutableCaption[0] = L'X';
    Check(std::wcscmp(stable, L"Follow Windows default output (recommended)") == 0 &&
          Translate(mutableCaption, true) == mutableCaption,
          "translation survives input mutation; unknown caption preserves pointer");
    Check(std::wcscmp(Translate(L"Shared 저지연 · %.2f~%.2f ms · 검사 %.1f ms", true),
                      L"Shared low latency · %.2f~%.2f ms · probe %.1f ms") == 0,
          "OSD format specifiers survive non-owning lookup");
    Check(std::wcscmp(Translate(L"업데이트 확인", true), L"Update checks") == 0,
          "duplicate cleanup retains the original update caption");
    for (const wchar_t* text : {
            L" · 사용 가능 · %d ms", L" (사용 가능 · %d ms)",
            L" · 검사 중", L" (검사 중)", L" · 사용 불가", L" (사용 불가)",
            L" · 확인 보류", L" (확인 보류)",
            L"장치 검사 중…", L"전체 장치 다시 검사",
            L"Exclusive 출력 장치 검사 중… %zu/%zu 완료",
            L"Exclusive 사용 가능 · 현재 출력 장치 · %d ms 이상",
            L"Exclusive 사용 가능 · %d ms 이상 선택 필요",
            L"Exclusive 사용 불가 · 현재 출력 장치",
            L"Exclusive 검사 필요 · 현재 출력 장치",
            L"Exclusive 확인 보류 · 장치 상태 확인 후 다시 검사해 주세요"}) {
        Check(std::wcscmp(Translate(text, true), text) != 0,
              "every Exclusive status and action translates to English");
        Check(Translate(text, false) == text, "Exclusive Korean remains unchanged");
    }
    wchar_t formatted[160]{};
    swprintf_s(formatted, Translate(L"Exclusive 출력 장치 검사 중… %zu/%zu 완료", true),
               size_t{1}, size_t{2});
    Check(std::wcscmp(formatted, L"Checking Exclusive outputs… 1/2 complete") == 0,
          "Exclusive progress placeholders match argument types");
    swprintf_s(formatted, Translate(L"Exclusive 사용 가능 · 현재 출력 장치 · %d ms 이상", true), 10);
    Check(std::wcscmp(formatted, L"Exclusive available · selected output · 10 ms or more") == 0,
          "Exclusive buffer placeholder is preserved");
}

struct PopulationProbe {
    SettingsControls* controls;
    DWORD thread;
    int stage = 0;
    static void Output(void* context) {
        auto& probe = *static_cast<PopulationProbe*>(context);
        Check(GetCurrentThreadId() == probe.thread && probe.stage++ == 0,
              "output population stays synchronous and first");
        Check(probe.controls->audioOutputCombo && !probe.controls->bufferCombo,
              "output population retains pre-buffer creation timing");
        SendMessageW(probe.controls->audioOutputCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Simulated output"));
        SendMessageW(probe.controls->audioOutputCombo, CB_SETCURSEL, 0, 0);
    }
    static void Buffer(void* context) {
        auto& probe = *static_cast<PopulationProbe*>(context);
        Check(GetCurrentThreadId() == probe.thread && probe.stage++ == 1,
              "buffer population stays synchronous and second");
        Check(probe.controls->bufferCombo && !probe.controls->pixelFormatCombo,
              "buffer population retains pre-video creation timing");
        SendMessageW(probe.controls->bufferCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Simulated buffer"));
        SendMessageW(probe.controls->bufferCombo, CB_SETCURSEL, 0, 0);
    }
    static void Pixel(void* context) {
        auto& probe = *static_cast<PopulationProbe*>(context);
        Check(GetCurrentThreadId() == probe.thread && probe.stage++ == 2,
              "pixel population stays synchronous and third");
        Check(probe.controls->pixelFormatCombo && probe.controls->frameRateCombo &&
              probe.controls->captureAudioStatus && !probe.controls->scalingCombo &&
              !probe.controls->startButton, "video query retains original partial-control boundary");
        SendMessageW(probe.controls->pixelFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"NV12"));
        SendMessageW(probe.controls->pixelFormatCombo, CB_SETCURSEL, 0, 0);
        SendMessageW(probe.controls->frameRateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"60 fps"));
        SendMessageW(probe.controls->frameRateCombo, CB_SETCURSEL, 0, 0);
    }
};

static void TestShortcutEditing() {
    namespace keys=llcv::shortcuts;
    Check(keys::Valid(keys::Defaults),"legacy keys are valid defaults");
    Check(keys::Valid(VK_ESCAPE),"Esc can be assigned like other keys");
    for (unsigned invalid : {0u,keys::Ctrl,keys::Alt|VK_ESCAPE,keys::Ctrl|VK_ESCAPE,keys::Alt|VK_F4,
             keys::Alt|VK_TAB,keys::Alt|VK_RETURN,keys::Ctrl|keys::Alt|VK_DELETE,0xffffffffu})
        Check(!keys::Valid(invalid),"reserved and malformed bindings rejected");
    auto swapped=keys::Defaults;
    std::swap(swapped[0],swapped[6]);
    Check(keys::Expand(L"F1 / F12 / F11 / Tab / F123",swapped)==L"F12 / F1 / F11 / Tab / F123",
          "labels expand once, without cascading swapped keys or partial tokens");
    for (size_t i=0;i<keys::Count;++i) {
        auto map=keys::Defaults;
        map[i]=keys::Ctrl|keys::Shift|'A';
        Check(keys::Match(map,static_cast<keys::Action>(i),'A',keys::Ctrl|keys::Shift) &&
              !keys::Match(map,static_cast<keys::Action>(i),'A',keys::Ctrl) &&
              !keys::Match(map,static_cast<keys::Action>(i),keys::Defaults[i],0),
              "each action matches only the exact configured key and modifiers");
    }
    HWND parent=CreateWindowW(L"STATIC",L"Shortcut input test",WS_POPUP,
        -10000,-10000,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(parent!=nullptr,"shortcut test owner");
    SettingsControls state;
    state.english=true;
    for (auto& key:state.guideKeys)
        key=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            0,0,148,32,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    state.guideVideoHint=CreateWindowW(L"STATIC",L"",WS_CHILD,0,0,336,36,
        parent,nullptr,GetModuleHandleW(nullptr),nullptr);
    const auto press=[&](UINT key,unsigned mods=0,bool repeat=false,UINT message=WM_KEYDOWN) {
        MSG input{}; input.hwnd=state.guideKeys[0]; input.message=message; input.wParam=key;
        input.lParam=repeat ? (LPARAM{1}<<30) : 0;
        return ProcessShortcutEdit(&state,input,mods);
    };
    BeginShortcutEdit(&state,keys::Count);
    Check(state.editingShortcut==-1,"out-of-range action cannot enter edit mode");
    BeginShortcutEdit(&state,0);
    Check(GetFocus()==state.guideKeys[0],"input focus belongs to edited key");
    Check(press(VK_CONTROL) && state.editingShortcut==0,"modifier-only input waits for a key");
    Check(press(VK_F2) && state.shortcutBindings==keys::Defaults && state.editingShortcut==0,
          "duplicate binding rejected without losing edit mode");
    Check(press(VK_F4,keys::Alt) && state.editingShortcut==0,"OS combination rejected");
    Check(press('H',keys::Ctrl,true) && state.editingShortcut==0,"held key cannot complete edit");
    Check(press('H',keys::Ctrl) && state.editingShortcut==-1 &&
          state.shortcutBindings[0]==(keys::Ctrl|'H'),"combination accepted");
    Check(press('H',keys::Ctrl,true) && press('H',keys::Ctrl,false,WM_KEYUP),
          "assignment repeat and key-up cannot trigger action");
    Check(!press('H',keys::Ctrl),"next deliberate press is no longer suppressed");
    const auto edited=state.shortcutBindings;
    BeginShortcutEdit(&state,0);
    Check(press(VK_ESCAPE) && state.shortcutBindings==edited && state.editingShortcut==0,
          "Esc already assigned to exit is rejected as a duplicate");
    BeginShortcutEdit(&state,0);
    Check(state.editingShortcut==-1 && state.shortcutBindings==edited,
          "clicking the same key again cancels without clearing it");
    const size_t exit=static_cast<size_t>(keys::Action::Exit);
    BeginShortcutEdit(&state,exit);
    Check(press(0) && press(VK_CONTROL) && state.shortcutBindings==edited &&
          state.editingShortcut==static_cast<int>(exit),"exit cannot be left blank or modifier-only");
    Check(press('Q',keys::Ctrl) && state.shortcutBindings[exit]==(keys::Ctrl|'Q'),
          "exit can be reassigned from Esc to a combination");
    press('Q',keys::Ctrl,false,WM_KEYUP);
    BeginShortcutEdit(&state,0);
    Check(press(VK_ESCAPE) && state.shortcutBindings[0]==VK_ESCAPE && keys::Valid(state.shortcutBindings),
          "freed Esc can be assigned to another action");
    Check(press(VK_ESCAPE,0,true) && press(VK_ESCAPE,0,false,WM_KEYUP),
          "assigned Esc repeat and release do not close settings");
    state.shortcutBindings=edited;
    BeginShortcutEdit(&state,0);
    SetFocus(state.guideKeys[1]);
    Check(!press('J') && state.editingShortcut==-1 && state.shortcutBindings==edited,
          "focus loss cancels editing without consuming another control's input");
    state.shortcutCaptionTemplates.emplace_back(state.guideVideoHint,L"F1 / F12");
    state.shortcutBindings=swapped;
    RefreshShortcutCaptions(&state);
    wchar_t caption[256]{}; GetWindowTextW(state.guideVideoHint,caption,256);
    Check(!wcscmp(caption,L"F12 / F1"),"settings captions use binding snapshot");
    state.shortcutBindings=keys::Defaults;
    RefreshShortcutCaptions(&state);
    GetWindowTextW(state.guideVideoHint,caption,256);
    Check(!wcscmp(caption,L"F1 / F12"),"reset captions restore from original templates");
    DestroyWindow(parent);
}

static void TestActualControlCreation() {
    using namespace llcv::settings;
    const VideoPresetInfo presets[] = {
        {VideoPreset::R1280x720, 1280, 720, 60, L"1280 x 720"},
        {VideoPreset::R1920x1080, 1920, 1080, 120, L"1920 x 1080"},
        {VideoPreset::R2560x1440, 2560, 1440, 120, L"2560 x 1440"},
        {VideoPreset::R3840x2160, 3840, 2160, 60, L"3840 x 2160"},
    };
    const int pcm[] = {10, 15, 20, 25, 30};
    const llcv::capture::DeviceInfo devices[] = {
        {L"gc573-id", L"AVerMedia GC573"}, {L"other-id", L"Other capture"}};
    // Frozen numeric IDs from the old WM_CREATE: no event-routing changes.
    const struct { HWND SettingsControls::* handle; int id; } controlIds[] = {
    {&SettingsControls::tabControl, 2037},
    {&SettingsControls::upscalerCombo, 2049},
    {&SettingsControls::nisSharpnessCombo, 2083},
    {&SettingsControls::vsrGuideButton, 2050},
    {&SettingsControls::displayResolutionCombo, 2051},
    {&SettingsControls::audioCombo, 2001},
    {&SettingsControls::audioOutputCombo, 2016},
    {&SettingsControls::bufferCombo, 2006},
    {&SettingsControls::audioStatus, 2008},
    {&SettingsControls::exclusiveTestButton, 2036},
    {&SettingsControls::volumeHudCombo, 2010},
    {&SettingsControls::volumeBoostCheck, 2029},
    {&SettingsControls::volumeBoostHelp, 2030},
    {&SettingsControls::driftHelp, 2014},
    {&SettingsControls::driftCombo, 2011},
    {&SettingsControls::pcmQueueHelp, 2023},
    {&SettingsControls::pcmQueueCombo, 2015},
    {&SettingsControls::muteBackgroundCheck, 2021},
    {&SettingsControls::audioOnlyCheck, 2032},
    {&SettingsControls::languageCombo, 2024},
    {&SettingsControls::themeCombo, 2053},
    {&SettingsControls::skipStartupCheck, 2028},
    {&SettingsControls::preventSleepCheck, 2056},
    {&SettingsControls::checkForUpdatesCheck, 2035},
    {&SettingsControls::guideLogFolderButton, 2039},
    {&SettingsControls::screenshotClipboardCheck, 2047},
    {&SettingsControls::capture40Check, 2057},
    {&SettingsControls::unlistedFpsCheck, 2078},
    {&SettingsControls::capture40Help, 2058},
    {&SettingsControls::guideLsButton, 2059},
    {&SettingsControls::screenshotFolderButton, 2048},
    {&SettingsControls::updateNowButton, 2038},
    {&SettingsControls::presentationCombo, 2009},
    {&SettingsControls::displayMonitorCombo, 2043},
    {&SettingsControls::presentationHelp, 2022},
    {&SettingsControls::captureDeviceCombo, 2017},
    {&SettingsControls::captureAudioDeviceCombo, 2026},
    {&SettingsControls::videoCombo, 2002},
    {&SettingsControls::pixelFormatCombo, 2018},
    {&SettingsControls::frameRateCombo, 2020},
    {&SettingsControls::scalingCombo, 2027},
    {&SettingsControls::fullscreenCursorCombo, 2040},
    {&SettingsControls::fullscreenPositionCombo, 2061},
    {&SettingsControls::forceHdr10Check, 2033},
    {&SettingsControls::forceHdr10Help, 2034},
    {&SettingsControls::hdrChromaCombo, 2044},
    {&SettingsControls::hdrChromaHelp, 2045},
    {&SettingsControls::surround51Check, 2046},
    {&SettingsControls::mjpegColorCombo, 2041},
    {&SettingsControls::mjpegColorHelp, 2042},
    {&SettingsControls::sdrRangeCombo, 2079},
    {&SettingsControls::sdrRangeHelp, 2080},
    {&SettingsControls::pixelCheck, 2003},
    {&SettingsControls::relativeSizeCheck, 2013},
    {&SettingsControls::borderlessCheck, 2007},
    {&SettingsControls::roundedCornersCheck, 2054},
    {&SettingsControls::windowSnapCheck, 2012},
    {&SettingsControls::saveLogCheck, 2019},
    {&SettingsControls::showConsoleCheck, 2025},
    {&SettingsControls::startButton, 2004},
    {&SettingsControls::cancelButton, 2005},
    };
    // Cover every option value and both sides of each checkbox/ASIO/device
    // branch without repeatedly creating the same 76-control native window.
    for (unsigned profile : {0u, 1u, 2u, 3u, 4u, 5u, 8u, 15u, 16u, 23u, 24u, 29u})
    for (bool english : {false, true}) {
        std::printf("Factory profile %u (%s)\n", profile, english ? "en" : "ko");
        std::fflush(stdout);
        HWND parent = CreateWindowExW(0, L"STATIC", L"Hidden actual settings controls",
            WS_POPUP, 0, 0, 1900, 1260, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(parent != nullptr, "create factory test parent");
        SettingsControls state;
        AppSettings settings;
        settings.audioMode = static_cast<AudioMode>(profile % 3);
        settings.driftCorrection = static_cast<DriftCorrectionMode>(profile % 3);
        settings.pcmQueueTargetMs = pcm[profile % 5];
        settings.presentationMode = llcv::presentation_ui::ModeFromSelection(profile % 4);
        settings.volumeHudPosition = static_cast<VolumeHudPosition>(profile % 4);
        settings.scalingMode = profile % 2 ? ScalingMode::Sharp : ScalingMode::Smooth;
        settings.fullscreenCursorMode = profile % 2 ? FullscreenCursorMode::AlwaysVisible : FullscreenCursorMode::AutoHide;
        settings.fullscreenVideoPosition = FullscreenPositionFromIndex(profile % 9);
        settings.uiLanguage = static_cast<UiLanguage>(profile % 3);
        settings.settingsLightTheme = profile % 2 != 0;
        settings.mjpegMatrixOverride = static_cast<llcv::video_color::MatrixOverride>(profile % 3);
        settings.sdrRangeOverride = static_cast<llcv::video_color::RangeOverride>(profile % 3);
        settings.hdrChromaLocation = static_cast<llcv::hdr::ChromaLocation>(profile % 3);
        settings.allowVolumeBoost = (profile & 1) != 0;
        settings.pixelPerfect = (profile & 2) != 0;
        settings.relativeWindowSize = (profile & 4) != 0;
        settings.borderlessWindow = (profile & 8) != 0;
        settings.roundedCorners = (profile & 1) != 0;
        settings.windowSnap = (profile & 16) != 0;
        settings.forceHdr10 = !settings.allowVolumeBoost;
        settings.muteWhenBackground = !settings.pixelPerfect;
        settings.audioOnly = !settings.relativeWindowSize;
        settings.consoleSurround51 = profile % 2 != 0;
        settings.saveLog = !settings.borderlessWindow;
        settings.screenshotClipboard = profile % 2 != 0;
        settings.capture40Fps = profile % 2 != 0;
        settings.vsrEnabled = profile % 2 != 0;
        settings.capturePreset = presets[(profile + 1) % 4].preset;
        settings.videoPreset = presets[profile % 4].preset;
        settings.displayMatchesCapture = (profile & 8) != 0;
        settings.showDiagnosticConsole = !settings.windowSnap;
        settings.skipStartupSettings = settings.allowVolumeBoost;
        settings.checkForUpdates = settings.pixelPerfect;
        settings.preventSleep = settings.allowVolumeBoost;
        settings.captureDeviceId = devices[profile % 2].id;
        settings.captureAudioDeviceId = devices[1 - profile % 2].id;
        const bool available = (profile & 1) != 0;
        const bool noDevices = profile % 7 == 0;
        const auto inputs = noDevices ? std::span<const llcv::capture::DeviceInfo>{}
                                     : std::span<const llcv::capture::DeviceInfo>{devices};
        state.activeTab = SettingsTabFromNavigationIndex(profile % kSettingsNavigationCount);
        const llcv::display::MonitorChoice monitors[] = {
            {nullptr,L"monitor-a",L"DISPLAY1 · Test A"},
            {nullptr,L"monitor-b",L"DISPLAY2 · Test B"}};
        settings.preferredDisplayMonitor = profile % 3 == 0 ? L"" : profile % 3 == 1 ? L"monitor-b" : L"missing";
        SettingsControlInitialValues initial{
            settings, english, available, L"vTEST", presets[profile % 4].preset,
            inputs, inputs, presets, pcm, monitors};
        PopulationProbe probe{&state, GetCurrentThreadId()};
        SettingsControlPopulation population{
            &probe, PopulationProbe::Output, PopulationProbe::Buffer, PopulationProbe::Pixel};
        CreateSettingsDialogControls(&state, parent, GetModuleHandleW(nullptr), initial, population);
        Check(probe.stage == 3, "each population callback invoked exactly once");
        for (const auto& member : kMembers) Check(IsWindow(state.*member.handle) != FALSE, member.name);
        for (const auto& item : controlIds)
            Check(GetDlgCtrlID(state.*item.handle) == item.id, "original control/event ID retained");
        const auto selection = [](HWND combo) { return SendMessageW(combo, CB_GETCURSEL, 0, 0); };
        Check(SendMessageW(state.tabControl, LB_GETCOUNT, 0, 0) == kSettingsNavigationCount &&
              SendMessageW(state.tabControl, LB_GETCURSEL, 0, 0) == SettingsNavigationIndex(state.activeTab),
              "sidebar count and initial selection");
        Check(SendMessageW(state.audioCombo, CB_GETCOUNT, 0, 0) == (available ? 3 : 2), "ASIO option visibility");
        const int audio = settings.audioMode == AudioMode::Asio && available ? 2 :
                          settings.audioMode == AudioMode::WasapiExclusive ? 1 : 0;
        Check(selection(state.audioCombo) == audio, "initial audio mode with unavailable-ASIO fallback");
        const int drift = settings.driftCorrection == DriftCorrectionMode::Resample ? 2 :
                          settings.driftCorrection == DriftCorrectionMode::Auto ? 1 : 0;
        Check(selection(state.driftCombo) == drift, "initial clock correction");
        Check(selection(state.pcmQueueCombo) == profile % 5 &&
              SendMessageW(state.pcmQueueCombo, CB_GETITEMDATA, profile % 5, 0) == settings.pcmQueueTargetMs, "PCM values and selection");
        Check(SendMessageW(state.presentationCombo, CB_GETCOUNT, 0, 0) ==
                  llcv::presentation_ui::ModeCount,
              "all four presentation modes are available");
        Check(selection(state.presentationCombo) == profile % 4, "presentation mode mapping");
        wchar_t exclusiveLabel[128]{};
        SendMessageW(state.presentationCombo, CB_GETLBTEXT, 3,
                     reinterpret_cast<LPARAM>(exclusiveLabel));
        Check(std::wcscmp(exclusiveLabel,
                  llcv::presentation_ui::ExclusiveFullscreenLabel(english)) == 0,
              "exclusive choice uses the active settings language");
        Check(selection(state.displayMonitorCombo) == (profile % 3 == 0 ? 0 : profile % 3 == 1 ? 2 : 3), "display selection / disconnected preference");
        Check(SendMessageW(state.videoCombo, CB_GETCOUNT, 0, 0) == 4,
              "720p plus three existing resolutions are listed");
        const auto captureIndex = (profile + 1) % 4;
        Check(selection(state.videoCombo) == captureIndex, "left field always restores capture resolution");
        const LRESULT displayIndex = settings.displayMatchesCapture ? 4 : profile % 4;
        Check(SendMessageW(state.displayResolutionCombo, CB_GETCOUNT, 0, 0) == 5 &&
              selection(state.displayResolutionCombo) == displayIndex &&
              DisplayResolutionMatchesCapture(state.displayResolutionCombo) == settings.displayMatchesCapture,
              "right field restores fixed or same-as-capture display choice without changing capture rows");
        LayoutSettingsControls(&state,96);
        for (int mode : {0, 1, 2, 1, 0}) {
            SendMessageW(state.upscalerCombo, CB_SETCURSEL, mode, 0);
            wchar_t sdrOnlyLabel[128]{};
            SendMessageW(state.upscalerCombo, CB_GETLBTEXT, 1, reinterpret_cast<LPARAM>(sdrOnlyLabel));
            Check(std::wcscmp(sdrOnlyLabel,L"CMAA2 → NIS")==0,
                "upscaler selection uses a concise algorithm name");
            UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::Nv12);
            if(mode==1) {
                GetWindowTextW(state.vsrStatus,sdrOnlyLabel,128);
                Check(std::wcscmp(sdrOnlyLabel,english ? L"SDR only" : L"SDR 전용")==0,
                    "SDR-only support appears in the conditional helper");
                const auto sharpness=ClientRectOf(parent,state.nisSharpnessCombo);
                const auto helper=ClientRectOf(parent,state.vsrStatus);
                Check(helper.left>=sharpness.right+SettingsPixels(16,state.layoutDpi),
                    "conditional helper cannot overlap the sharpness field after switching");
            }
            Check(Visible(state.vsrStatus)==(mode!=0 && state.activeTab==SettingsTab::VideoWindow),
                "supported GPU hides conditional help when upscaling is off");
            wchar_t label[64]{};
            GetWindowTextW(state.videoLabel, label, 64);
            Check(std::wcscmp(label, english ? L"Capture resolution" : L"캡처 해상도") == 0,
                  "VSR toggling never renames the capture field");
            GetWindowTextW(state.displayResolutionLabel, label, 64);
            Check(std::wcscmp(label, english ? L"Display resolution" : L"표시 해상도") == 0,
                  "display label stays fixed");
            Check(selection(state.videoCombo) == captureIndex &&
                  selection(state.displayResolutionCombo) == displayIndex,
                  "upscaler selection preserves capture and display");
            Check(Visible(state.nisSharpnessCombo)==(mode==1 && state.activeTab==SettingsTab::VideoWindow),
                  "sharpness appears only for CMAA/NIS on video page");
            Check(SendMessageW(state.nisSharpnessCombo,CB_GETCURSEL,0,0)==15,"NIS default is 0.15");
            auto applied = settings;
            ApplyResolutionSelections(state, presets, applied);
            Check(applied.fullscreenVideoPosition == settings.fullscreenVideoPosition,
                  "fullscreen position is saved with the resolution selections");
            Check(applied.videoPreset == presets[profile % 4].preset,
                  "display selection is saved independently of VSR");
            Check(applied.capturePreset == presets[captureIndex].preset,
                  "capture selection is saved independently of VSR");
            Check(applied.displayMatchesCapture == settings.displayMatchesCapture &&
                  llcv::settings::EffectiveDisplayPreset(applied) ==
                    presets[settings.displayMatchesCapture ? captureIndex : profile % 4].preset,
                  "same-as-capture follows the current capture selection while retaining the fixed preference");
            Check((IsWindowEnabled(state.displayResolutionCombo) != FALSE) ==
                      (state.activeTab == SettingsTab::VideoWindow),
                  "display resolution remains editable with VSR off or on");
        }
        SendMessageW(state.upscalerCombo, CB_SETCURSEL, settings.vsrEnabled ? 2 : 0, 0);
        Check((SendMessageW(state.upscalerCombo, CB_GETCURSEL, 0, 0) == 2) == settings.vsrEnabled,
              "legacy VSR setting maps to selector");
        Check(selection(state.volumeHudCombo) == profile % 4, "volume HUD selection");
        Check(selection(state.languageCombo) == profile % 3, "language preference selection");
        Check(selection(state.themeCombo) == (settings.settingsLightTheme ? 1 : 0),
              "theme preference restored in the native picker");
        Check(selection(state.scalingCombo) == profile % 2, "scaling selection");
        Check(selection(state.fullscreenCursorCombo) == profile % 2, "cursor selection");
        Check(selection(state.fullscreenPositionCombo) == profile % 9, "fullscreen position selection");
        Check(selection(state.mjpegColorCombo) == profile % 3 &&
              SendMessageW(state.mjpegColorCombo, CB_GETCOUNT, 0, 0) == 3,
              "MJPEG choices contain only Auto, BT.709 and BT.601");
        Check(selection(state.sdrRangeCombo) == profile % 3 &&
              SendMessageW(state.sdrRangeCombo, CB_GETCOUNT, 0, 0) == 3 &&
              SendMessageW(state.sdrRangeCombo, CB_GETITEMDATA, profile % 3, 0) == profile % 3,
              "SDR range choices retain stable values and initial selection");
        Check(selection(state.hdrChromaCombo) == profile % 3 &&
              SendMessageW(state.hdrChromaCombo, CB_GETCOUNT, 0, 0) == 3 &&
              SendMessageW(state.hdrChromaCombo, CB_GETITEMDATA, profile % 3, 0) == profile % 3,
              "HDR chroma choices retain stable values and initial selection");
        Check(selection(state.captureDeviceCombo) == (noDevices ? 0 : 1 + profile % 2), "video device restored");
        Check(selection(state.captureAudioDeviceCombo) == (noDevices ? 0 : 2 - profile % 2), "capture audio device restored");
        const struct { HWND handle; bool expected; } checks[] = {
            {state.volumeBoostCheck, settings.allowVolumeBoost},
            {state.pixelCheck, settings.pixelPerfect},
            {state.relativeSizeCheck, settings.relativeWindowSize},
            {state.borderlessCheck, settings.borderlessWindow},
            {state.roundedCornersCheck, settings.roundedCorners},
            {state.windowSnapCheck, settings.windowSnap},
            {state.forceHdr10Check, settings.forceHdr10},
            {state.muteBackgroundCheck, settings.muteWhenBackground},
            {state.audioOnlyCheck, settings.audioOnly},
            {state.surround51Check, settings.consoleSurround51},
            {state.saveLogCheck, settings.saveLog},
            {state.screenshotClipboardCheck, settings.screenshotClipboard},
            {state.capture40Check, settings.capture40Fps},
            {state.unlistedFpsCheck, settings.allowUnlistedFps},
            {state.showConsoleCheck, settings.showDiagnosticConsole},
            {state.skipStartupCheck, settings.skipStartupSettings},
            {state.checkForUpdatesCheck, settings.checkForUpdates},
            {state.preventSleepCheck, settings.preventSleep},
        };
        for (const auto& item : checks)
            Check((SendMessageW(item.handle, BM_GETCHECK, 0, 0) == BST_CHECKED) == item.expected, "initial checkbox value");
        wchar_t caption[512]{};
        GetWindowTextW(state.startButton, caption, 512);
        Check(std::wcscmp(caption, english ? L"Start" : L"시작") == 0, "factory language is explicit");
        GetWindowTextW(state.guideLsButton, caption, 512);
        Check(std::wcscmp(caption, english ? L"40 fps capture guide" : L"40fps 캡처 안내") == 0,
              "capture guide caption does not imply LS integration");
        GetWindowTextW(state.updateTitle, caption, 512);
        Check(std::wcsstr(caption, L"vTEST") != nullptr, "explicit version label");
        GetWindowTextW(state.fullscreenCursorHint, caption, 512);
        Check(std::wcscmp(caption, english ? L"F11  Toggle fullscreen" :
                         L"F11  전체화면 켜기/끄기") == 0, "fullscreen caption covers borderless and exclusive");
        Check((GetWindowLongPtrW(state.fullscreenCursorHint, GWL_STYLE) & SS_TYPEMASK) == SS_RIGHT,
              "F11 right alignment unchanged");
        Check(SendMessageW(state.tooltipWindow, TTM_GETTOOLCOUNT, 0, 0) == 12,
              "eight help buttons plus monitor, audio mode, frame rate and pixel-perfect tooltips registered");
        // Model a full six-line device capability report, not just the short
        // loading placeholder. Long formats/FPS lists must remain visible.
        SetWindowTextW(state.videoCapabilityStatus,FullCapabilityCaption(english));
        SetWindowTextW(state.unlistedFpsStatus, english
            ? L"Direct request: 30 fps" : L"직접 요청: 30 fps");
        SetWindowTextW(state.audioStatus, english
            ? L"Console 5.1: output period is negotiated on start; actual value in Tab diagnostics."
            : L"콘솔 5.1: 출력 주기는 시작 시 협상 · 실제 값은 Tab 진단에서 확인");
        const SettingsTab initialTab = state.activeTab;
        state.activeTab = SettingsTab::VideoWindow;
        using RangeOverride = llcv::video_color::RangeOverride;
        state.sdrRangeOverride = RangeOverride::Auto;
        state.legacyMjpegRangeOverride = RangeOverride::Full;
        for (auto format : {VideoPixelFormat::Nv12, VideoPixelFormat::Mjpeg, VideoPixelFormat::P010,
                            VideoPixelFormat::Yuy2, VideoPixelFormat::Mjpeg}) {
            UpdateAdvancedControlVisibility(&state, false, format);
            Check(selection(state.sdrRangeCombo) == static_cast<LRESULT>(
                      format == VideoPixelFormat::Mjpeg ? RangeOverride::Full : RangeOverride::Auto),
                  "format switches display effective legacy MJPEG range without changing raw Auto");
            Check(state.sdrRangeOverride == RangeOverride::Auto &&
                  state.legacyMjpegRangeOverride == RangeOverride::Full,
                  "format reflow preserves both pending settings");
        }
        UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::P010);
        SendMessageW(state.sdrRangeCombo, CB_SETCURSEL, 1, 0);
        ApplySdrRangeSelection(&state);
        Check(state.sdrRangeOverride == RangeOverride::Auto &&
              state.legacyMjpegRangeOverride == RangeOverride::Full,
              "hidden HDR range control cannot mutate SDR pending settings");
        for (auto range : {RangeOverride::Auto, RangeOverride::Limited, RangeOverride::Full}) {
            state.sdrRangeOverride = RangeOverride::Auto;
            state.legacyMjpegRangeOverride = RangeOverride::Full;
            UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::Mjpeg);
            SendMessageW(state.sdrRangeCombo, CB_SETCURSEL, static_cast<WPARAM>(range), 0);
            ApplySdrRangeSelection(&state);
            Check(state.sdrRangeOverride == range && state.legacyMjpegRangeOverride == RangeOverride::Auto,
                  "deliberate shared range edit including Auto retires legacy MJPEG-only range");
            UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::Nv12);
            Check(selection(state.sdrRangeCombo) == static_cast<LRESULT>(range),
                  "explicit range follows format changes");
        }
        state.sdrRangeOverride = settings.sdrRangeOverride;
        state.legacyMjpegRangeOverride = settings.legacyMjpegRangeOverride;
        UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::Nv12);
        const LRESULT savedFrameChoice = selection(state.frameRateCombo);
        for (int toggle = 0; toggle < 10; ++toggle) {
            const bool checked = toggle % 2 == 0;
            SendMessageW(state.capture40Check, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
            UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::Nv12);
            Check((IsWindowEnabled(state.frameRateCombo) != FALSE) == !checked &&
                selection(state.frameRateCombo) == savedFrameChoice,
                "40fps toggles normal picker availability without changing its selection");
        }
        SendMessageW(state.capture40Check, BM_SETCHECK, settings.capture40Fps ? BST_CHECKED : BST_UNCHECKED, 0);
        for (UINT textDpi : {96u, 120u, 144u, 192u}) {
            ApplySettingsFont(&state, parent, textDpi);
            LayoutSettingsControls(&state, textDpi);
            state.activeTab = SettingsTab::VideoWindow;
            if (profile == 0) for (auto format : {VideoPixelFormat::Auto, VideoPixelFormat::Nv12,VideoPixelFormat::Uyvy,
                                VideoPixelFormat::Mjpeg, VideoPixelFormat::P010}) {
                UpdateAdvancedControlVisibility(&state, false, format);
                const auto rangeBefore = selection(state.sdrRangeCombo);
                const auto matrixBefore = selection(state.mjpegColorCombo);
                const auto chromaBefore = selection(state.hdrChromaCombo);
                const auto fpsBefore = selection(state.frameRateCombo);
                const auto checkBefore = SendMessageW(state.unlistedFpsCheck,BM_GETCHECK,0,0);
                for (int close : {control_id::IDC_SETTINGS_FRAME_OPTIONS_CLOSE, IDCANCEL,
                                  control_id::IDC_SETTINGS_FRAME_OPTIONS}) {
                    Check(HandleFrameOptionsMessage(&state,parent,WM_COMMAND,
                          control_id::IDC_SETTINGS_FRAME_OPTIONS,0) && state.frameOptionsOpen,
                          "frame options button opens the local flyout");
                    UpdateAdvancedControlVisibility(&state,false,format);
                    ExpectVisible(state.capture40Check,true);
                    Check(Visible(state.unlistedFpsCheck),"unlisted FPS appears in flyout");
                    Check((IsWindowEnabled(state.unlistedFpsCheck)!=FALSE)==
                          (SendMessageW(state.capture40Check,BM_GETCHECK,0,0)!=BST_CHECKED),
                          "40fps retains its existing precedence over unlisted FPS");
                    ExpectVisible(state.pixelFormatCombo,false);
                    ExpectVisible(state.sdrRangeCombo,false);
                    ExpectVisible(state.hdrChromaCombo,false);
                    for (HWND control : {state.frameOptionsButton,state.frameOptionsTitle,
                            state.frameOptionsClose,state.frameOptionsHint,state.unlistedFpsCheck,
                            state.capture40Check,state.capture40Help}) {
                        RECT r=ClientRectOf(parent,control);
                        Check(r.top>=0 && r.bottom<=SettingsPixels(590,textDpi) &&
                              r.left>=SettingsPixels(184,textDpi) && r.right<=SettingsPixels(560,textDpi),
                              "flyout fits existing left column above footer at every DPI");
                    }
                    CheckCaptionFits(state.frameOptionsButton,"frame options",textDpi,english,14,false);
                    CheckFrameOptionsLayout(state,parent,textDpi,english);
                    Check(HandleFrameOptionsMessage(&state,parent,WM_COMMAND,close,0) &&
                          !state.frameOptionsOpen,"close/toggle/Escape dismiss only the flyout");
                    UpdateAdvancedControlVisibility(&state,false,format);
                    Check(selection(state.sdrRangeCombo)==rangeBefore &&
                          selection(state.mjpegColorCombo)==matrixBefore &&
                          selection(state.hdrChromaCombo)==chromaBefore &&
                          selection(state.frameRateCombo)==fpsBefore &&
                          SendMessageW(state.unlistedFpsCheck,BM_GETCHECK,0,0)==checkBefore,
                          "opening and closing never changes capture or color settings");
                }
                for (int edge = 0; edge < 4; ++edge) {
                    HandleFrameOptionsMessage(&state,parent,WM_COMMAND,
                                              control_id::IDC_SETTINGS_FRAME_OPTIONS,0);
                    UpdateAdvancedControlVisibility(&state,false,format);
                    const RECT bounds = SettingsFrameOptionsBounds(&state,parent);
                    const LONG centerX = (bounds.left + bounds.right) / 2;
                    const LONG centerY = (bounds.top + bounds.bottom) / 2;
                    const POINT outside[] = {{bounds.left - 1,centerY},{bounds.right,centerY},
                                             {centerX,bounds.top - 1},{centerX,bounds.bottom}};
                    HandleFrameOptionsMessage(&state,parent,WM_LBUTTONDOWN,0,
                                              MAKELPARAM(outside[edge].x,outside[edge].y));
                    Check(!state.frameOptionsOpen,"outside-click bounds match all four painted card edges");
                    UpdateAdvancedControlVisibility(&state,false,format);
                }
                state.frameOptionsOpen=true;
                state.activeTab=SettingsTab::Audio;
                UpdateAdvancedControlVisibility(&state,false,format);
                Check(!state.frameOptionsOpen,"tab switch clears transient flyout state");
                state.activeTab=SettingsTab::VideoWindow;
            }
            CheckFontHierarchy(state, textDpi, english);
            CheckFieldMetrics(state, parent, textDpi);
            {
                const auto savedDisplayRow = selection(state.displayResolutionCombo);
                SendMessageW(state.displayResolutionCombo, CB_SETCURSEL, 4, 0);
                CheckCaptionFits(state.displayResolutionCombo,"same-as-capture choice",textDpi,english,42,false);
                SendMessageW(state.displayResolutionCombo, CB_SETCURSEL, savedDisplayRow, 0);
            }
            CheckHdrChromaChoicesFit(state, textDpi, english);
            const auto savedVsrCheck = SendMessageW(state.upscalerCombo, CB_GETCURSEL, 0, 0);
            state.activeTab = SettingsTab::VideoWindow;
            for (bool unknown : {false, true}) {
                state.vsrGpuUnavailable = true;
                state.vsrGpuUnknown = unknown;
                SendMessageW(state.upscalerCombo, CB_SETCURSEL, 2, 0);
                UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::Nv12);
                Check(IsWindowEnabled(state.upscalerCombo) &&
                    IsWindowEnabled(state.displayResolutionCombo) &&
                    SendMessageW(state.upscalerCombo, CB_GETCURSEL, 0, 0) == BST_UNCHECKED,
                    "GPU gate disables only VSR, not display resolution");
                Check(IsWindowEnabled(state.vsrGuideButton), "VSR guidance remains accessible");
                CheckCaptionFits(state.vsrStatus, "VSR GPU reason", textDpi, english, 0, false);
                wchar_t reason[256]{};
                GetWindowTextW(state.vsrStatus, reason, 256);
                Check(std::wcsstr(reason, L"NVIDIA") != nullptr,
                    "unknown GPU and non-NVIDIA are explained separately");
                state.activeTab = SettingsTab::Audio;
                UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::Nv12);
                state.activeTab = SettingsTab::VideoWindow;
                UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::Nv12);
                Check(IsWindowEnabled(state.upscalerCombo), "page switching cannot bypass GPU gate");
            }
            state.vsrGpuUnavailable = state.vsrGpuUnknown = false;
            SendMessageW(state.upscalerCombo, CB_SETCURSEL, savedVsrCheck, 0);
            UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::Nv12);
            Check(IsWindowEnabled(state.upscalerCombo), "NVIDIA retains selectable VSR");
            for (int page = 0; page < kSettingsNavigationCount; ++page) {
                state.activeTab = SettingsTabFromNavigationIndex(page);
                RefreshSettingsPageHeader(&state);
                CheckAllCaptionsFit(state, textDpi, english);
            }
            const auto baseKey=ClientRectOf(parent,state.guideKeys[0]);
            Check(baseKey.right-baseKey.left==SettingsPixels(80,textDpi),"default key column stays compact");
            state.shortcutBindings[0]=llcv::shortcuts::Ctrl|VK_F1;
            RefreshShortcutCaptions(&state);
            Check(ClientRectOf(parent,state.guideKeys[0]).right==baseKey.right,
                  "two-key combination fits without expanding the base column");
            CheckShortcutCaptionFits(state,state.guideKeys[0],textDpi);
            state.shortcutBindings=llcv::shortcuts::Defaults;
            for (size_t action=0;action<llcv::shortcuts::Count;++action) {
                state.shortcutBindings[action]=llcv::shortcuts::Ctrl|llcv::shortcuts::Alt|
                    llcv::shortcuts::Shift|VK_SPACE;
                RefreshShortcutCaptions(&state);
                CheckShortcutCaptionFits(state,state.guideKeys[action],textDpi);
                const auto grownKey=ClientRectOf(parent,state.guideKeys[action]);
                Check(grownKey.right>baseKey.right,"long combinations grow the key column");
                for (size_t row=0;row<state.guideKeys.size();++row) {
                    Check(ClientRectOf(parent,state.guideKeys[row]).right==grownKey.right,
                          "all keycaps stay aligned after expansion");
                    CheckCaptionFits(state.guideDescriptions[row],"expanded key action",textDpi,english,0,true,false);
                }
                for (const auto& [control,source]:state.shortcutCaptionTemplates) {
                    wchar_t customCaption[2048]{}; GetWindowTextW(control,customCaption,2048);
                    Check(std::wstring(customCaption)==llcv::shortcuts::Expand(source.c_str(),state.shortcutBindings),
                          "settings labels follow edited bindings");
                    const bool checkbox=false;
                    CheckCaptionFits(control,"custom shortcut caption",textDpi,english,checkbox ? 27 : 0,
                        !checkbox,false);
                }
                state.shortcutBindings=llcv::shortcuts::Defaults;
            }
            RefreshShortcutCaptions(&state);
            for (size_t action=0;action<llcv::shortcuts::Count;++action)
                state.shortcutBindings[action]=llcv::shortcuts::Ctrl|llcv::shortcuts::Alt|
                    llcv::shortcuts::Shift|(VK_F13+static_cast<unsigned>(action));
            RefreshShortcutCaptions(&state);
            CheckCaptionFits(state.guideVideoHint,"long custom key footer",textDpi,english,0,true,false);
            Check(ClientRectOf(parent,state.guideVideoHint).bottom+SettingsPixels(20,textDpi)<
                  SettingsPixels(590,textDpi),"expanded hint stays above footer");
            state.shortcutBindings=llcv::shortcuts::Defaults;
            RefreshShortcutCaptions(&state);
            Check(ClientRectOf(parent,state.guideKeys[0]).right==baseKey.right,"reset restores compact key width");
        }
        state.activeTab = initialTab;
        RefreshSettingsPageHeader(&state);
        ApplySettingsFont(&state, parent, 96);
        LayoutSettingsControls(&state, 96);
        for (HWND control : {state.upscalerCombo, state.vsrGuideButton, state.vsrStatus}) {
            wchar_t label[256]{};
            GetWindowTextW(control, label, ARRAYSIZE(label));
            HDC dc = GetDC(control);
            auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
            SIZE textSize{};
            GetTextExtentPoint32W(dc, label, static_cast<int>(wcslen(label)), &textSize);
            RECT client{}; GetClientRect(control, &client);
            if (control == state.vsrStatus) {
                RECT wrapped{0, 0, client.right, 0};
                DrawTextW(dc, label, -1, &wrapped, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                Check(wrapped.bottom <= client.bottom, "bilingual VSR status wraps within its area");
            } else {
                Check(textSize.cx + (control == state.upscalerCombo ? 42 : 0) <= client.right &&
                      textSize.cy <= client.bottom, "bilingual VSR captions fit");
            }
            SelectObject(dc, oldFont); ReleaseDC(control, dc);
        }
        wchar_t screenshotCaption[256]{};
        GetWindowTextW(state.screenshotTitle,screenshotCaption,256);
        Check(std::wcsstr(screenshotCaption,L"F12")!=nullptr,"video screenshot section advertises F12");
        HDC screenshotDc=GetDC(state.screenshotHelp);
        const auto oldScreenshotFont=SelectObject(screenshotDc,
            reinterpret_cast<HFONT>(SendMessageW(state.screenshotHelp,WM_GETFONT,0,0)));
        GetWindowTextW(state.screenshotHelp,screenshotCaption,256);
        RECT screenshotTextRect{0,0,376,0};
        DrawTextW(screenshotDc,screenshotCaption,-1,&screenshotTextRect,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        Check(screenshotTextRect.bottom<=18,"bilingual screenshot hint fits one line");
        SelectObject(screenshotDc,oldScreenshotFont); ReleaseDC(state.screenshotHelp,screenshotDc);
        UpdateAdvancedControlVisibility(&state, settings.audioMode == AudioMode::WasapiExclusive,
            VideoPixelFormat::Nv12);
        // Missing ASIO drivers intentionally fall back to Shared in the dialog.
        const bool surroundEnabled = state.activeTab == SettingsTab::Audio && audio == 0;
        Check((IsWindowEnabled(state.surround51Check) != FALSE) == surroundEnabled &&
              (IsWindowEnabled(state.surround51Hint) != FALSE) == surroundEnabled,
              "5.1 option cannot be enabled in ASIO/Exclusive or hidden tabs");
        const RECT boost = ClientRectOf(parent, state.volumeBoostCheck);
        const RECT button = ClientRectOf(parent, state.volumeBoostHelp);
        Check(button.left > boost.right, "real checkbox/help hit targets do not overlap");
        // A real pushbutton must not toggle its neighboring checkbox.
        const LRESULT checked = SendMessageW(state.volumeBoostCheck, BM_GETCHECK, 0, 0);
        SendMessageW(state.volumeBoostHelp, BM_CLICK, 0, 0);
        Check(SendMessageW(state.volumeBoostCheck, BM_GETCHECK, 0, 0) == checked,
              "clicking real help control does not toggle volume boost");
        const LRESULT forced = SendMessageW(state.forceHdr10Check, BM_GETCHECK, 0, 0);
        const LRESULT placement = selection(state.hdrChromaCombo);
        SendMessageW(state.hdrChromaHelp, BM_CLICK, 0, 0);
        Check(SendMessageW(state.forceHdr10Check, BM_GETCHECK, 0, 0) == forced &&
              selection(state.hdrChromaCombo) == placement,
              "chroma help does not toggle HDR or change placement");
        DestroyWindow(state.tooltipWindow);
        DestroyWindow(parent);
        for (HFONT font : state.uiFonts) Check(DeleteObject(font) != FALSE, "release real creation fonts");
    }
    std::puts("Actual control factory: 24 profiles, original IDs/settings/population order passed.");
}
static void TestCompactWorkAreas() {
    using namespace llcv::settings;
    const VideoPresetInfo presets[]={{VideoPreset::R1920x1080,1920,1080,60,L"1920 x 1080"}};
    const int pcm[]={10,15,20,25,30};
    const struct { RECT work; UINT dpi; } areas[]={
        {{0,0,1920,1040},96},{{0,0,1366,728},120},{{-1920,0,0,1040},192},
        {{0,0,1280,680},144},{{0,0,800,560},120},{{0,0,1366,728},192},
        // A complete 128-DIP option cannot fit these roughly 96-DIP page
        // bodies. Its explanation and all five navigation entries still must.
        {{0,0,1280,624},192},{{0,0,1920,936},288}};
    for(bool english:{false,true}) for(const auto& area:areas) for(bool unsupported:{false,true}) {
        SettingsControls state;
        constexpr DWORD style=WS_CAPTION|WS_SYSMENU;
        constexpr DWORD exStyle=WS_EX_DLGMODALFRAME;
        RECT fitted=FitSettingsDialogToWorkArea(&state,area.work,area.dpi,style,exStyle);
        Check(fitted.left>=area.work.left && fitted.top>=area.work.top &&
            fitted.right<=area.work.right && fitted.bottom<=area.work.bottom,"settings outer frame fits work area at high DPI");
        HWND parent=CreateWindowExW(exStyle,L"STATIC",L"Hidden compact fixture",style,
            fitted.left,fitted.top,fitted.right-fitted.left,fitted.bottom-fitted.top,
            nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(parent!=nullptr,"create compact fixture");
        AppSettings settings; settings.audioMode=AudioMode::WasapiExclusive;
        SettingsControlInitialValues initial{settings,english,true,L"vTEST",VideoPreset::R1920x1080,
            {},{},presets,pcm,{}};
        PopulationProbe probe{&state,GetCurrentThreadId()};
        SettingsControlPopulation population{&probe,PopulationProbe::Output,PopulationProbe::Buffer,PopulationProbe::Pixel};
        CreateSettingsDialogControls(&state,parent,GetModuleHandleW(nullptr),initial,population);
        ApplySettingsFont(&state,parent,area.dpi);
        const HFONT originalFont=reinterpret_cast<HFONT>(SendMessageW(state.audioLabel,WM_GETFONT,0,0));
        const auto originalFonts=state.uiFonts;
        SetWindowTextW(state.videoCapabilityStatus,unsupported?UnsupportedCapabilityCaption(english):FullCapabilityCaption(english));
        SetWindowTextW(state.unlistedFpsStatus,english?L"Direct request: 30 fps":L"직접 요청: 30 fps");
        SendMessageW(state.pixelCheck,BM_SETCHECK,BST_CHECKED,0);
        SendMessageW(state.relativeSizeCheck,BM_SETCHECK,BST_CHECKED,0);
        std::set<HWND> seen;
        for(int tab=0;tab<kSettingsNavigationCount;++tab) {
            state.activeTab=SettingsTabFromNavigationIndex(tab);
            for(const auto format:{VideoPixelFormat::Nv12,VideoPixelFormat::P010,VideoPixelFormat::Mjpeg,VideoPixelFormat::Uyvy}) {
                SendMessageW(state.pixelFormatCombo,CB_SETITEMDATA,0,static_cast<LPARAM>(format));
                LayoutSettingsControls(&state,area.dpi);
                UpdateAdvancedControlVisibility(&state,true,format);
                const int pages=SettingsCompactLayout(&state)?static_cast<int>(state.compactPageNames.size()):1;
                for(int page=0;page<pages;++page) {
                    state.compactPages[tab]=page;
                    UpdateAdvancedControlVisibility(&state,true,format);
                    // Capture audio visibility is intentionally controller-owned.
                    SetSettingsControlVisible(state.captureAudioDeviceCombo,tab==0);
                    SetSettingsControlVisible(state.captureAudioStatus,false);
                    const int footer=SettingsPixels(state.footerTopDip,area.dpi);
                    const int width=SettingsPixels(state.clientWidthDip,area.dpi);
                    const int height=SettingsPixels(state.clientHeightDip,area.dpi);
                    std::vector<RECT> contentRects;
                    for(HWND child=GetWindow(parent,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
                        if(!Visible(child)) continue;
                        const RECT rect=ClientRectOf(parent,child);
                        Check(rect.left>=0 && rect.top>=0 && rect.right<=width+1 && rect.bottom<=height+1,
                            "every visible control fits the client area without scrolling");
                        if(child!=state.startButton && child!=state.cancelButton && child!=state.versionWatermark &&
                            child!=state.tabControl) Check(rect.bottom<=footer,"content never overlaps footer buttons");
                        if(SettingsCompactLayout(&state)) {
                            for(const auto& previous:contentRects) {
                                RECT intersection{};
                                Check(!IntersectRect(&intersection,&previous,&rect),"compact controls never overlap one another");
                            }
                            contentRects.push_back(rect);
                            wchar_t className[32]{}, caption[2048]{};
                            GetClassNameW(child,className,32); GetWindowTextW(child,caption,2048);
                            if(std::wcscmp(className,L"COMBOBOX")==0)
                                Check(rect.bottom-rect.top==SettingsPixels(30,area.dpi),
                                    "compact dropdown retains exact 30 DIP collapsed height after page changes");
                            if(std::wcscmp(className,L"BUTTON")==0) {
                                const auto buttonStyle=GetWindowLongPtrW(child,GWL_STYLE);
                                const bool checkbox=(buttonStyle&BS_TYPEMASK)==BS_AUTOCHECKBOX;
                                CheckCaptionFits(child,"compact button",area.dpi,english,
                                    checkbox?27:14,checkbox&&(buttonStyle&BS_MULTILINE));
                            }
                            const auto staticType=GetWindowLongPtrW(child,GWL_STYLE)&SS_TYPEMASK;
                            if(std::wcscmp(className,L"STATIC")==0 && caption[0] &&
                                (staticType==SS_LEFT || staticType==SS_RIGHT || staticType==SS_CENTER || staticType==SS_LEFTNOWORDWRAP)) {
                                HDC dc=GetDC(child);
                                HGDIOBJ previous=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0)));
                                RECT text{0,0,rect.right-rect.left,0};
                                DrawTextW(dc,caption,-1,&text,DT_CALCRECT|DT_NOPREFIX|
                                    (staticType==SS_LEFTNOWORDWRAP?DT_SINGLELINE:DT_WORDBREAK));
                                SelectObject(dc,previous); ReleaseDC(child,dc);
                                if(text.bottom>rect.bottom-rect.top+1)
                                    std::fwprintf(stderr,L"Clipped compact text: %s (%ld needed, %ld available)\n",caption,text.bottom,rect.bottom-rect.top);
                                Check(text.bottom<=rect.bottom-rect.top+1,"compact labels and explanations retain all text");
                            }
                        }
                        seen.insert(child);
                    }
                    Check(Visible(state.startButton)&&Visible(state.cancelButton),"action buttons always remain visible");
                    if (Visible(state.hdrChromaCombo))
                        CheckHdrChromaChoicesFit(state, area.dpi, english);
                    if(SettingsCompactLayout(&state)) {
                        // The sidebar itself can fit while its owner-drawn
                        // rows are clipped or internally scrolled off-screen.
                        RECT navClient{}; GetClientRect(state.tabControl,&navClient);
                        Check(SendMessageW(state.tabControl,LB_GETTOPINDEX,0,0)==0,
                            "all navigation options remain visible without scrolling");
                        HDC dc=GetDC(state.tabControl);
                        const auto previous=SelectObject(dc,
                            reinterpret_cast<HFONT>(SendMessageW(state.tabControl,WM_GETFONT,0,0)));
                        for(int row=0;row<kSettingsNavigationCount;++row) {
                            RECT item{};
                            Check(SendMessageW(state.tabControl,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item))!=LB_ERR &&
                                item.top>=0 && item.bottom<=navClient.bottom,
                                "every navigation row fits above the version watermark");
                            wchar_t caption[256]{};
                            SendMessageW(state.tabControl,LB_GETTEXT,row,reinterpret_cast<LPARAM>(caption));
                            SIZE text{}; GetTextExtentPoint32W(dc,caption,static_cast<int>(wcslen(caption)),&text);
                            Check(text.cx<=navClient.right-SettingsPixels(25,area.dpi) &&
                                text.cy<=item.bottom-item.top-SettingsPixels(6,area.dpi),
                                "compact sidebar captions fit with the original font and theme insets");
                        }
                        SelectObject(dc,previous); ReleaseDC(state.tabControl,dc);
                        for(const auto& pair:{std::pair{state.captureDeviceLabel,state.captureDeviceCombo},
                            std::pair{state.hdrChromaLabel,state.hdrChromaCombo},
                            std::pair{state.mjpegColorLabel,state.mjpegColorCombo},
                            std::pair{state.sdrRangeLabel,state.sdrRangeCombo},
                            std::pair{state.driftLabel,state.driftCombo},std::pair{state.pcmQueueLabel,state.pcmQueueCombo}})
                            Check(Visible(pair.first)==Visible(pair.second),"pagination preserves complete labeled fields");
                        if(Visible(state.surround51Hint)&&!Visible(state.surround51Check))
                            Check(state.compactPageNames[page].find(L"5.1")!=std::wstring::npos,
                                "standalone surround explanation retains its option context");
                    }
                }
            }
        }
        for(HWND required:{state.captureDeviceCombo,state.captureAudioDeviceCombo,state.videoCombo,state.frameRateCombo,
            state.pixelFormatCombo,state.forceHdr10Check,state.hdrChromaCombo,state.mjpegColorCombo,state.sdrRangeCombo,
            state.presentationCombo,state.displayMonitorCombo,state.scalingCombo,state.pixelCheck,
            state.upscalerCombo,state.displayResolutionCombo,state.screenshotClipboardCheck,state.screenshotFolderButton,
            state.audioCombo,state.audioOutputCombo,state.bufferCombo,state.exclusiveTestButton,
            state.volumeHudCombo,state.volumeBoostCheck,state.muteBackgroundCheck,state.audioOnlyCheck,state.surround51Check,
            state.driftCombo,state.pcmQueueCombo,state.relativeSizeCheck,state.borderlessCheck,state.roundedCornersCheck,
            state.windowSnapCheck,state.fullscreenCursorCombo,state.saveLogCheck,state.showConsoleCheck,state.guideLogFolderButton,
            state.languageCombo,state.themeCombo,state.skipStartupCheck,state.preventSleepCheck,state.checkForUpdatesCheck,state.updateNowButton})
            Check(seen.contains(required),"all settings remain reachable across compact section pages");
        for(HWND description:{state.videoCapabilityStatus,state.screenshotTitle,state.screenshotHelp,
            state.surround51Hint,state.fullscreenCursorHint,state.relativeSizeWarning,
            state.guideVideoHint,state.guideDiagnosticsText,state.skipStartupHint,state.preventSleepHint,state.updateText,state.updateStatus})
            Check(seen.contains(description),"all descriptions remain reachable across compact section pages");
        Check(state.uiFonts==originalFonts,"pagination preserves the entire original font hierarchy");
        Check(reinterpret_cast<HFONT>(SendMessageW(state.audioLabel,WM_GETFONT,0,0))==originalFont,
            "compact layout does not shrink font for legibility");
        Check(!(GetWindowLongPtrW(parent,GWL_STYLE)&(WS_VSCROLL|WS_HSCROLL)),"no scrollbar required");
        FitSettingsClientArea(&state,{2000,1400},96);
        LayoutSettingsControls(&state,96);
        state.activeTab=SettingsTab::Updates;
        UpdateAdvancedControlVisibility(&state,false,VideoPixelFormat::Nv12);
        Check(!Visible(state.sectionNavigation)&&Visible(state.skipStartupCheck),"returning to large monitor restores normal layout");
        DestroyWindow(parent);
        for(HFONT font:state.uiFonts) DeleteObject(font);
    }
    std::puts("Compact work areas: 8 sizes/scales, Korean/English, every option, fixed font, no scroll passed.");
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX init{sizeof(init), ICC_WIN95_CLASSES};
    Check(InitCommonControlsEx(&init) != FALSE, "initialize tooltip controls");
    if(argc==2 && std::strcmp(argv[1],"--compact-only")==0) {
        TestCompactWorkAreas();
        return EXIT_SUCCESS;
    }
    for (int i = 0; i < kSettingsNavigationCount; ++i)
        Check(SettingsNavigationIndex(SettingsTabFromNavigationIndex(i)) == i,
              "sidebar order maps one-to-one to stable page values");
    Check(SettingsTabFromNavigationIndex(-1) == SettingsTab::VideoWindow &&
          SettingsTabFromNavigationIndex(kSettingsNavigationCount) == SettingsTab::VideoWindow,
          "invalid navigation safely selects video");
    TestTranslationBoundary();
    TestShortcutEditing();
    TestActualControlCreation();
    TestCompactWorkAreas();
    HWND parent = CreateWindowExW(0, L"STATIC", L"Hidden settings view regression",
        WS_POPUP, 0, 0, 2000, 1400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "create hidden parent");
    SettingsControls state;
    for (const auto& entry : kMembers) {
        bool combo = std::strstr(entry.name, "Combo") != nullptr;
        bool check = std::strstr(entry.name, "Check") != nullptr;
        // The original pixelCheck is the only lowercase c among checkbox names.
        check |= std::strcmp(entry.name, "pixelCheck") == 0;
        state.*entry.handle = CreateWindowExW(0, combo ? L"COMBOBOX" : check ? L"BUTTON" : L"STATIC",
            L"", WS_CHILD | (combo ? CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS : check ? BS_AUTOCHECKBOX : 0),
            0, 0, 100, 100, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(state.*entry.handle != nullptr, entry.name);
    }
    for (size_t i=0;i<state.guideKeys.size();++i) {
        state.guideKeys[i]=CreateWindowW(L"BUTTON",llcv::viewer_help::kShortcuts[i].key,
            WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,0,0,80,29,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
        Check(state.guideKeys[i]!=nullptr,"create shortcut visibility fixture");
    }
    unsigned cases = 0;
    for (UINT dpi : {96u, 120u, 144u, 192u}) for (bool english : {false, true}) {
        state.english = english;
        SetWindowTextW(state.volumeBoostCheck, english
            ? L"Allow volume boost above 100% (up to 200%)"
            : L"100% 이상 볼륨 증폭 허용 (최대 200%)");
        const auto oldFonts = state.uiFonts;
        ApplySettingsFont(&state, parent, dpi);
        CheckFontHierarchy(state, dpi, english);
        for (HFONT retired : oldFonts) {
            LOGFONTW oldDescription{};
            Check(GetObjectW(retired, sizeof(oldDescription), &oldDescription) == 0,
                  "monitor and language transitions release the complete previous font set");
        }
        CheckWindowBehaviorReflow(state, parent, dpi);
        for (bool pixel : {false, true}) for (bool relative : {false, true})
        for (auto tab : {SettingsTab::Audio, SettingsTab::VideoWindow, SettingsTab::Window,
                         SettingsTab::GuideDiagnostics, SettingsTab::Updates})
        for (auto format : {VideoPixelFormat::Auto, VideoPixelFormat::Nv12, VideoPixelFormat::Yuy2,
                            VideoPixelFormat::Mjpeg, VideoPixelFormat::P010})
        for (bool exclusive : {false, true}) {
            state.activeTab = tab;
            SendMessageW(state.pixelCheck, BM_SETCHECK, pixel ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(state.relativeSizeCheck, BM_SETCHECK, relative ? BST_CHECKED : BST_UNCHECKED, 0);
            LayoutSettingsControls(&state, dpi);
            CheckFieldMetrics(state, parent, dpi);
            UpdateAdvancedControlVisibility(&state, exclusive, format);
            const bool video = tab == SettingsTab::VideoWindow;
            const bool audio = tab == SettingsTab::Audio;
            for (const auto& item : kGeometry) {
                const RECT rect = ClientRectOf(parent, state.*item.handle);
                if (rect.left != MulDiv(item.x, dpi, 96) ||
                    rect.top != MulDiv(pixel && relative ? item.warningY : item.normalY, dpi, 96) ||
                    rect.right - rect.left != MulDiv(item.width, dpi, 96) ||
                    rect.bottom - rect.top != MulDiv(item.height, dpi, 96)) {
                    std::fprintf(stderr, "Geometry mismatch: %s dpi=%u pixel=%d\n", item.name, dpi, pixel);
                    return EXIT_FAILURE;
                }
            }
            for (size_t i = 0; i < ARRAYSIZE(kMembers); ++i) {
                const HWND first = state.*kMembers[i].handle;
                if (!Visible(first)) continue;
                const RECT a = ClientRectOf(parent, first);
                Check(a.left >= 0 && a.top >= 0 &&
                      a.right <= SettingsPixels(kSettingsClientWidthDip, dpi) &&
                      a.bottom <= SettingsPixels(SettingsClientHeightDip(&state), dpi),
                      "visible controls stay inside settings client area");
                for (size_t j = i + 1; j < ARRAYSIZE(kMembers); ++j) {
                    const HWND second = state.*kMembers[j].handle;
                    if (!Visible(second)) continue;
                    const RECT b = ClientRectOf(parent, second);
                    RECT overlap{};
                    if (IntersectRect(&overlap, &a, &b)) {
                        std::fprintf(stderr, "Visible overlap: %s / %s dpi=%u tab=%d\n",
                                     kMembers[i].name, kMembers[j].name, dpi, static_cast<int>(tab));
                        return EXIT_FAILURE;
                    }
                }
            }
            const RECT boost = ClientRectOf(parent, state.volumeBoostCheck);
            const RECT button = ClientRectOf(parent, state.volumeBoostHelp);
            Check(button.left - boost.right == MulDiv(12, dpi, 96),
                  "volume help remains adjacent and cannot overlap checkbox");
            Check(IsSettingsHelpControl(&state, state.volumeBoostHelp) &&
                  !IsSettingsHelpControl(&state, state.volumeBoostCheck), "help routing distinct from option toggle");
            ExpectVisible(state.exclusiveTestButton, audio && exclusive);
            Check(Visible(state.surround51Check) == audio && Visible(state.surround51Hint) == audio,
                "surround settings only visible on audio tab");
            ExpectVisible(state.forceHdr10Check, video && format == VideoPixelFormat::P010);
            for (HWND control : {state.hdrChromaLabel, state.hdrChromaCombo, state.hdrChromaHelp})
                ExpectVisible(control, video && format == VideoPixelFormat::P010);
            const RECT chromaCombo = ClientRectOf(parent, state.hdrChromaCombo);
            const RECT chromaHelp = ClientRectOf(parent, state.hdrChromaHelp);
            Check(chromaHelp.left > chromaCombo.right, "HDR chroma help cannot overlap combo hit target");
            ExpectVisible(state.mjpegColorCombo, video && format == VideoPixelFormat::Mjpeg);
            for (HWND control : {state.sdrRangeLabel, state.sdrRangeCombo, state.sdrRangeHelp})
                ExpectVisible(control, video && format != VideoPixelFormat::P010);
            Check(Visible(state.scalingCombo) == video, "scaling row never jumps when lock changes");
            Check((IsWindowEnabled(state.scalingCombo) != FALSE) == (video && (!pixel ||
                SendMessageW(state.videoCombo, CB_GETCURSEL, 0, 0) !=
                SendMessageW(state.displayResolutionCombo, CB_GETCURSEL, 0, 0))), "scaling enable policy");
            ExpectVisible(state.relativeSizeWarning, tab == SettingsTab::Window && pixel && relative);
            ExpectVisible(state.fullscreenCursorHint, tab == SettingsTab::Window);
            ExpectVisible(state.fullscreenPositionLabel, tab == SettingsTab::Window);
            ExpectVisible(state.fullscreenPositionCombo, tab == SettingsTab::Window);
            ExpectVisible(state.fullscreenPositionHint, tab == SettingsTab::Window);
            ExpectVisible(state.windowSnapCheck, tab == SettingsTab::Window);
            ExpectVisible(state.roundedCornersCheck, tab == SettingsTab::Window);
            ExpectVisible(state.guideLogFolderButton, tab == SettingsTab::GuideDiagnostics);
            Check((IsWindowEnabled(state.guideKeys.back())!=FALSE)==(tab==SettingsTab::GuideDiagnostics),
                  "exit remains editable on the guide page");
            ExpectVisible(state.screenshotTitle, tab == SettingsTab::VideoWindow);
            ExpectVisible(state.frameOptionsButton, tab == SettingsTab::VideoWindow);
            ExpectVisible(state.capture40Check, false);
            ExpectVisible(state.unlistedFpsCheck, false);
            ExpectVisible(state.capture40Help, false);
            ExpectVisible(state.guideLsButton, tab == SettingsTab::GuideDiagnostics);
            ExpectVisible(state.upscalerCombo, video);
            ExpectVisible(state.vsrGuideButton, video);
            ExpectVisible(state.vsrStatus, video &&
                (SendMessageW(state.upscalerCombo,CB_GETCURSEL,0,0)>0 || state.vsrGpuUnavailable));
            ExpectVisible(state.screenshotClipboardCheck, tab == SettingsTab::VideoWindow);
            ExpectVisible(state.screenshotHelp, tab == SettingsTab::VideoWindow);
            ExpectVisible(state.screenshotFolderButton, tab == SettingsTab::VideoWindow);
            if (video) {
                for (HWND shot : {state.screenshotTitle,state.screenshotClipboardCheck,
                                  state.screenshotHelp,state.screenshotFolderButton}) {
                    const RECT shotRect=ClientRectOf(parent,shot);
                    for (HWND other : {state.fullscreenCursorHint,state.hdrChromaCombo,
                        state.languageLabel,state.languageCombo,state.skipStartupCheck,
                        state.skipStartupHint,state.startButton,state.cancelButton}) {
                        if (!Visible(other)) continue;
                        const RECT otherRect=ClientRectOf(parent,other); RECT overlap{};
                        Check(!IntersectRect(&overlap,&shotRect,&otherRect),"video screenshot section must not overlap footer/controls");
                    }
                }
            }
            ExpectVisible(state.updateNowButton, tab == SettingsTab::Updates);
            for (HWND control : {state.languageCombo, state.themeLabel, state.themeCombo, state.skipStartupCheck,
                                 state.preventSleepCheck, state.preventSleepHint, state.appPreferencesSection})
                ExpectVisible(control, tab == SettingsTab::Updates);
            for (HWND control : {state.brandLabel, state.pageTitle, state.pageSubtitle, state.versionWatermark,
                                 state.startButton, state.cancelButton}) ExpectVisible(control, true);
            if (!video) {
                ExpectVisible(state.captureAudioDeviceCombo, false);
                ExpectVisible(state.captureAudioStatus, false);
            }
            // The F11 hint must retain z-order above the cursor dropdown.
            for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                Check(child != state.fullscreenCursorCombo, "F11 hint stays above dropdown");
                if (child == state.fullscreenCursorHint) break;
            }
            ++cases;
        }
    }
    // Re-applying a selection must not enable disabled controls momentarily:
    // doing so generates repaint/focus churn even when the final state matches.
    unsigned enableTransitions = 0;
    for (const auto& entry : kMembers)
        Check(SetWindowSubclass(state.*entry.handle, CountEnableTransitions, 71,
              reinterpret_cast<DWORD_PTR>(&enableTransitions)) != FALSE, "observe enable transitions");
    SendMessageW(state.pixelCheck, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(state.upscalerCombo, CB_SETCURSEL, 0, 0);
    for (bool flyout : {false,true}) for (const auto tab : {SettingsTab::VideoWindow, SettingsTab::Audio}) {
        state.activeTab = tab;
        state.frameOptionsOpen = flyout;
        UpdateAdvancedControlVisibility(&state, true, VideoPixelFormat::Nv12, {false, false, false});
        enableTransitions = 0;
        for (int repeat = 0; repeat < 50; ++repeat)
            UpdateAdvancedControlVisibility(&state, true, VideoPixelFormat::Nv12, {false, false, false});
        Check(enableTransitions == 0, "unchanged option refresh emits no enable/disable churn");
        Check(!IsWindowEnabled(state.startButton) && !IsWindowEnabled(state.pixelFormatCombo) &&
              !IsWindowEnabled(state.captureDeviceCombo), "controller capability gates remain closed");
    }
    for (const auto& entry : kMembers)
        RemoveWindowSubclass(state.*entry.handle, CountEnableTransitions, 71);

    // Capture-audio row selection belongs to the dialog/controller, not the
    // view: entering/reflowing Video must not undo its internal-audio choice.
    state.activeTab = SettingsTab::VideoWindow;
    SetSettingsControlVisible(state.captureAudioDeviceCombo, true);
    SetSettingsControlVisible(state.captureAudioStatus, false);
    UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::Nv12);
    ExpectVisible(state.captureAudioDeviceCombo, true);
    ExpectVisible(state.captureAudioStatus, false);
    SetSettingsControlVisible(state.captureAudioDeviceCombo, false);
    SetSettingsControlVisible(state.captureAudioStatus, true);
    UpdateAdvancedControlVisibility(&state, false, VideoPixelFormat::Nv12);
    ExpectVisible(state.captureAudioDeviceCombo, false);
    ExpectVisible(state.captureAudioStatus, true);

    const HWND helpControls[] = {state.driftHelp, state.pcmQueueHelp, state.presentationHelp,
                                 state.volumeBoostHelp, state.forceHdr10Help, state.mjpegColorHelp, state.hdrChromaHelp,
                                 state.sdrRangeHelp};
    for (HWND help : helpControls) {
        AddSettingsTooltip(&state, parent, help, L"Persistent test tooltip");
        Check(IsSettingsHelpControl(&state, help), "help handle is recognized");
    }
    Check(state.tooltipWindow != nullptr &&
          SendMessageW(state.tooltipWindow, TTM_GETTOOLCOUNT, 0, 0) == 8, "all tooltip tools register with v1 structure");
    TestHelpText();
    DestroyWindow(state.tooltipWindow);
    DestroyWindow(parent);
    for (HFONT font : state.uiFonts) Check(DeleteObject(font) != FALSE, "release dialog fonts");
    std::printf("Settings view: %u combinations, %zu golden rectangles, Korean/English help, tooltips passed.\n",
                cases, sizeof(kGeometry) / sizeof(kGeometry[0]));
    return EXIT_SUCCESS;
}
