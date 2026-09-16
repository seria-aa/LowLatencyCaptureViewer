#pragma once

#include <windows.h>
#include <vector>

namespace llcv::settings { enum class VideoPixelFormat; }

namespace llcv::settings_ui {

enum class SettingsTab : int {
    Audio = 0,
    VideoWindow = 1,
    GuideDiagnostics = 2,
    Updates = 3,
};

// UI-thread-only, non-owning control handles. The dialog owns the HWNDs and
// deletes uiFonts after its children have been destroyed. Hardware discovery,
// asynchronous workers and persistent settings deliberately do not belong here.
struct SettingsControls {
    HWND tabControl = nullptr;
    HWND guideText = nullptr;
    HWND guideShortcutsTitle = nullptr;
    HWND guideDiagnosticsTitle = nullptr;
    HWND guideDiagnosticsText = nullptr;
    HWND guideLogFolderButton = nullptr;
    HWND updateTitle = nullptr;
    HWND updateText = nullptr;
    HWND updateNowButton = nullptr;
    HWND updateStatus = nullptr;
    HWND audioOutputSection = nullptr;
    HWND audioPlaybackSection = nullptr;
    HWND audioStabilitySection = nullptr;
    HWND videoCaptureSection = nullptr;
    HWND videoDisplaySection = nullptr;
    HWND videoWindowSection = nullptr;
    HWND languageLabel = nullptr;
    HWND languageCombo = nullptr;
    HWND audioLabel = nullptr;
    HWND bufferLabel = nullptr;
    HWND audioOutputLabel = nullptr;
    HWND volumeHudLabel = nullptr;
    HWND volumeBoostCheck = nullptr;
    HWND volumeBoostHelp = nullptr;
    HWND driftLabel = nullptr;
    HWND driftHelp = nullptr;
    HWND pcmQueueLabel = nullptr;
    HWND pcmQueueHelp = nullptr;
    HWND presentationLabel = nullptr;
    HWND presentationHelp = nullptr;
    HWND fullscreenCursorLabel = nullptr;
    HWND fullscreenCursorHint = nullptr;
    HWND scalingLabel = nullptr;
    HWND videoLabel = nullptr;
    HWND captureDeviceLabel = nullptr;
    HWND captureAudioDeviceLabel = nullptr;
    HWND captureAudioStatus = nullptr;
    HWND pixelFormatLabel = nullptr;
    HWND frameRateLabel = nullptr;
    HWND videoCapabilityStatus = nullptr;
    HWND audioCombo = nullptr;
    HWND bufferCombo = nullptr;
    HWND audioOutputCombo = nullptr;
    HWND volumeHudCombo = nullptr;
    HWND muteBackgroundCheck = nullptr;
    HWND audioOnlyCheck = nullptr;
    HWND forceHdr10Check = nullptr;
    HWND forceHdr10Help = nullptr;
    HWND hdrChromaLabel = nullptr;
    HWND hdrChromaCombo = nullptr;
    HWND hdrChromaHelp = nullptr;
    HWND mjpegColorLabel = nullptr;
    HWND mjpegColorCombo = nullptr;
    HWND mjpegColorHelp = nullptr;
    HWND driftCombo = nullptr;
    HWND pcmQueueCombo = nullptr;
    HWND audioStatus = nullptr;
    HWND exclusiveTestButton = nullptr;
    HWND presentationCombo = nullptr;
    HWND displayMonitorLabel = nullptr;
    HWND displayMonitorCombo = nullptr;
    HWND fullscreenCursorCombo = nullptr;
    HWND scalingCombo = nullptr;
    HWND videoCombo = nullptr;
    HWND captureDeviceCombo = nullptr;
    HWND captureAudioDeviceCombo = nullptr;
    HWND pixelFormatCombo = nullptr;
    HWND frameRateCombo = nullptr;
    HWND pixelCheck = nullptr;
    HWND relativeSizeCheck = nullptr;
    HWND relativeSizeWarning = nullptr;
    HWND borderlessCheck = nullptr;
    HWND windowSnapCheck = nullptr;
    HWND saveLogCheck = nullptr;
    HWND showConsoleCheck = nullptr;
    HWND skipStartupCheck = nullptr;
    HWND skipStartupHint = nullptr;
    HWND checkForUpdatesCheck = nullptr;
    HWND versionWatermark = nullptr;
    HWND startButton = nullptr;
    HWND cancelButton = nullptr;
    HWND tooltipWindow = nullptr;
    HWND activeTooltipTarget = nullptr;
    std::vector<HFONT> uiFonts;
    SettingsTab activeTab = SettingsTab::Audio;
};

enum class SettingsHelpTopic {
    Drift,
    PcmQueue,
    Presentation,
    VolumeBoost,
    ForceHdr10,
    HdrChroma,
    MjpegColor,
};

int SettingsPixels(int dips, UINT dpi);
inline constexpr int kSettingsClientWidthDip = 950;
int SettingsClientHeightDip(const SettingsControls* state);
SIZE SettingsDialogOuterSize(HWND hwnd, UINT dpi, const SettingsControls* state);
void PlaceSettingsControl(HWND control, int x, int y, int width, int height, UINT dpi);
void ApplySettingsFont(SettingsControls* state, HWND hwnd, UINT dpi);
void LayoutSettingsControls(SettingsControls* state, UINT dpi);
void SetSettingsControlVisible(HWND control, bool visible);
void UpdateScalingControlVisibility(SettingsControls* state);
void UpdateWindowBehaviorVisibility(SettingsControls* state);
void UpdateAdvancedControlVisibility(SettingsControls* state, bool exclusive,
                                     settings::VideoPixelFormat selectedFormat);
void TrackSettingsTooltip(HWND target, HWND tooltip, bool active);
void AddSettingsTooltip(SettingsControls* state, HWND owner, HWND target, const wchar_t* text);
bool IsSettingsHelpControl(const SettingsControls* state, HWND target);
const wchar_t* SettingsHelpText(SettingsHelpTopic topic, bool english);

} // namespace llcv::settings_ui
