#include "ui/SettingsView.h"
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

using namespace llcv::settings_ui;
using llcv::settings::VideoPixelFormat;

static void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAILED: %s\n", message); std::abort(); }
}
struct Member { HWND SettingsControls::* handle; const char* name; };
static constexpr Member kMembers[] = {
    {&SettingsControls::tabControl, "tabControl"},
    {&SettingsControls::guideText, "guideText"},
    {&SettingsControls::guideShortcutsTitle, "guideShortcutsTitle"},
    {&SettingsControls::guideDiagnosticsTitle, "guideDiagnosticsTitle"},
    {&SettingsControls::guideDiagnosticsText, "guideDiagnosticsText"},
    {&SettingsControls::guideLogFolderButton, "guideLogFolderButton"},
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
    {&SettingsControls::scalingLabel, "scalingLabel"},
    {&SettingsControls::videoLabel, "videoLabel"},
    {&SettingsControls::captureDeviceLabel, "captureDeviceLabel"},
    {&SettingsControls::captureAudioDeviceLabel, "captureAudioDeviceLabel"},
    {&SettingsControls::captureAudioStatus, "captureAudioStatus"},
    {&SettingsControls::pixelFormatLabel, "pixelFormatLabel"},
    {&SettingsControls::frameRateLabel, "frameRateLabel"},
    {&SettingsControls::videoCapabilityStatus, "videoCapabilityStatus"},
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
    {&SettingsControls::windowSnapCheck, "windowSnapCheck"},
    {&SettingsControls::saveLogCheck, "saveLogCheck"},
    {&SettingsControls::showConsoleCheck, "showConsoleCheck"},
    {&SettingsControls::skipStartupCheck, "skipStartupCheck"},
    {&SettingsControls::skipStartupHint, "skipStartupHint"},
    {&SettingsControls::checkForUpdatesCheck, "checkForUpdatesCheck"},
    {&SettingsControls::versionWatermark, "versionWatermark"},
    {&SettingsControls::startButton, "startButton"},
    {&SettingsControls::cancelButton, "cancelButton"},
};
struct Geometry {
    HWND SettingsControls::* handle;
    const char* name;
    int x, normalY, pixelY, width, height;
};
// Golden DIP coordinates captured from main.cpp before extraction. Intentionally
// do not derive expected rectangles from the new implementation under test.
static constexpr Geometry kGeometry[] = {
    {&SettingsControls::tabControl, "tabControl", 24, 16, 16, 901, 31},
    {&SettingsControls::languageLabel, "languageLabel", 24, 500, 500, 160, 24},
    {&SettingsControls::languageCombo, "languageCombo", 195, 496, 496, 280, 120},
    {&SettingsControls::skipStartupCheck, "skipStartupCheck", 24, 540, 540, 451, 28},
    {&SettingsControls::skipStartupHint, "skipStartupHint", 44, 568, 568, 500, 22},
    {&SettingsControls::versionWatermark, "versionWatermark", 24, 602, 602, 260, 20},
    {&SettingsControls::startButton, "startButton", 745, 568, 568, 80, 30},
    {&SettingsControls::cancelButton, "cancelButton", 835, 568, 568, 80, 30},
    {&SettingsControls::audioOutputSection, "audioOutputSection", 34, 58, 58, 200, 20},
    {&SettingsControls::audioLabel, "audioLabel", 34, 80, 80, 160, 24},
    {&SettingsControls::audioCombo, "audioCombo", 205, 76, 76, 360, 120},
    {&SettingsControls::exclusiveTestButton, "exclusiveTestButton", 575, 76, 76, 185, 22},
    {&SettingsControls::audioOutputLabel, "audioOutputLabel", 34, 116, 116, 160, 24},
    {&SettingsControls::audioOutputCombo, "audioOutputCombo", 205, 112, 112, 680, 220},
    {&SettingsControls::bufferLabel, "bufferLabel", 34, 152, 152, 160, 24},
    {&SettingsControls::bufferCombo, "bufferCombo", 205, 148, 148, 280, 180},
    {&SettingsControls::audioStatus, "audioStatus", 34, 188, 188, 580, 24},
    {&SettingsControls::audioPlaybackSection, "audioPlaybackSection", 34, 222, 222, 250, 20},
    {&SettingsControls::volumeHudLabel, "volumeHudLabel", 34, 246, 246, 160, 24},
    {&SettingsControls::volumeHudCombo, "volumeHudCombo", 205, 242, 242, 280, 160},
    {&SettingsControls::muteBackgroundCheck, "muteBackgroundCheck", 34, 318, 318, 500, 28},
    {&SettingsControls::audioOnlyCheck, "audioOnlyCheck", 34, 354, 354, 500, 28},
    {&SettingsControls::audioStabilitySection, "audioStabilitySection", 34, 392, 392, 250, 20},
    {&SettingsControls::driftLabel, "driftLabel", 34, 416, 416, 160, 24},
    {&SettingsControls::driftHelp, "driftHelp", 170, 412, 412, 24, 24},
    {&SettingsControls::driftCombo, "driftCombo", 205, 412, 412, 360, 120},
    {&SettingsControls::pcmQueueLabel, "pcmQueueLabel", 34, 452, 452, 160, 24},
    {&SettingsControls::pcmQueueHelp, "pcmQueueHelp", 170, 448, 448, 24, 24},
    {&SettingsControls::pcmQueueCombo, "pcmQueueCombo", 205, 448, 448, 280, 140},
    {&SettingsControls::captureDeviceLabel, "captureDeviceLabel", 34, 84, 84, 140, 24},
    {&SettingsControls::videoCaptureSection, "videoCaptureSection", 34, 58, 58, 140, 20},
    {&SettingsControls::captureDeviceCombo, "captureDeviceCombo", 190, 80, 80, 270, 220},
    {&SettingsControls::captureAudioDeviceLabel, "captureAudioDeviceLabel", 34, 124, 124, 140, 24},
    {&SettingsControls::captureAudioDeviceCombo, "captureAudioDeviceCombo", 190, 120, 120, 270, 220},
    {&SettingsControls::captureAudioStatus, "captureAudioStatus", 190, 124, 124, 300, 24},
    {&SettingsControls::videoLabel, "videoLabel", 34, 164, 164, 140, 24},
    {&SettingsControls::videoCombo, "videoCombo", 190, 160, 160, 270, 120},
    {&SettingsControls::pixelFormatLabel, "pixelFormatLabel", 34, 204, 204, 140, 24},
    {&SettingsControls::pixelFormatCombo, "pixelFormatCombo", 190, 200, 200, 270, 160},
    {&SettingsControls::frameRateLabel, "frameRateLabel", 34, 244, 244, 140, 24},
    {&SettingsControls::frameRateCombo, "frameRateCombo", 190, 240, 240, 270, 200},
    {&SettingsControls::videoCapabilityStatus, "videoCapabilityStatus", 34, 278, 278, 430, 90},
    {&SettingsControls::presentationLabel, "presentationLabel", 505, 84, 84, 95, 24},
    {&SettingsControls::videoDisplaySection, "videoDisplaySection", 505, 58, 58, 140, 20},
    {&SettingsControls::presentationHelp, "presentationHelp", 604, 80, 80, 24, 24},
    {&SettingsControls::presentationCombo, "presentationCombo", 630, 80, 80, 255, 120},
    {&SettingsControls::displayMonitorLabel, "displayMonitorLabel", 505, 124, 124, 120, 24},
    {&SettingsControls::displayMonitorCombo, "displayMonitorCombo", 630, 120, 120, 255, 180},
    {&SettingsControls::pixelCheck, "pixelCheck", 505, 160, 160, 380, 28},
    {&SettingsControls::scalingLabel, "scalingLabel", 505, 200, 200, 120, 24},
    {&SettingsControls::scalingCombo, "scalingCombo", 630, 196, 196, 255, 120},
    {&SettingsControls::videoWindowSection, "videoWindowSection", 505, 244, 208, 140, 20},
    {&SettingsControls::relativeSizeCheck, "relativeSizeCheck", 505, 268, 232, 400, 28},
    {&SettingsControls::relativeSizeWarning, "relativeSizeWarning", 525, 296, 260, 370, 28},
    {&SettingsControls::borderlessCheck, "borderlessCheck", 505, 336, 300, 400, 28},
    {&SettingsControls::windowSnapCheck, "windowSnapCheck", 505, 372, 336, 400, 28},
    {&SettingsControls::fullscreenCursorLabel, "fullscreenCursorLabel", 505, 412, 376, 120, 24},
    {&SettingsControls::fullscreenCursorCombo, "fullscreenCursorCombo", 630, 408, 372, 255, 120},
    {&SettingsControls::fullscreenCursorHint, "fullscreenCursorHint", 630, 440, 404, 255, 24},
    {&SettingsControls::forceHdr10Check, "forceHdr10Check", 34, 374, 374, 360, 28},
    {&SettingsControls::forceHdr10Help, "forceHdr10Help", 402, 370, 370, 24, 24},
    {&SettingsControls::hdrChromaLabel, "hdrChromaLabel", 34, 414, 414, 140, 24},
    {&SettingsControls::hdrChromaCombo, "hdrChromaCombo", 190, 410, 410, 240, 150},
    {&SettingsControls::hdrChromaHelp, "hdrChromaHelp", 438, 410, 410, 24, 24},
    {&SettingsControls::mjpegColorLabel, "mjpegColorLabel", 34, 374, 374, 140, 24},
    {&SettingsControls::mjpegColorCombo, "mjpegColorCombo", 190, 370, 370, 240, 150},
    {&SettingsControls::mjpegColorHelp, "mjpegColorHelp", 438, 370, 370, 24, 24},
    {&SettingsControls::guideShortcutsTitle, "guideShortcutsTitle", 34, 58, 58, 280, 20},
    {&SettingsControls::guideText, "guideText", 34, 84, 84, 400, 220},
    {&SettingsControls::guideDiagnosticsTitle, "guideDiagnosticsTitle", 505, 58, 58, 320, 20},
    {&SettingsControls::guideDiagnosticsText, "guideDiagnosticsText", 505, 84, 84, 360, 70},
    {&SettingsControls::saveLogCheck, "saveLogCheck", 505, 170, 170, 360, 28},
    {&SettingsControls::showConsoleCheck, "showConsoleCheck", 505, 206, 206, 360, 28},
    {&SettingsControls::guideLogFolderButton, "guideLogFolderButton", 505, 248, 248, 165, 26},
    {&SettingsControls::updateTitle, "updateTitle", 34, 76, 76, 400, 24},
    {&SettingsControls::updateText, "updateText", 34, 110, 110, 760, 64},
    {&SettingsControls::checkForUpdatesCheck, "checkForUpdatesCheck", 34, 190, 190, 500, 28},
    {&SettingsControls::updateNowButton, "updateNowButton", 34, 230, 230, 185, 30},
    {&SettingsControls::updateStatus, "updateStatus", 235, 234, 234, 650, 24},
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
static void TestHelpText() {
    Check(std::wcsstr(SettingsHelpText(SettingsHelpTopic::HdrChroma, true), L"staggered") != nullptr &&
          std::wcsstr(SettingsHelpText(SettingsHelpTopic::HdrChroma, false), L"보장하지") != nullptr,
          "chroma help discloses interpretation override limitations in both languages");
    const struct { SettingsHelpTopic topic; uint32_t english, korean; } golden[] = {
    {SettingsHelpTopic::Drift, 754620631u, 3340162937u},
    {SettingsHelpTopic::PcmQueue, 2979267523u, 1200270022u},
    {SettingsHelpTopic::VolumeBoost, 2473238580u, 2846859296u},
    {SettingsHelpTopic::ForceHdr10, 1978381312u, 295927312u},
    {SettingsHelpTopic::MjpegColor, 1599057429u, 3941322600u},
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

static void TestActualControlCreation() {
    using namespace llcv::settings;
    const VideoPresetInfo presets[] = {
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
    {&SettingsControls::skipStartupCheck, 2028},
    {&SettingsControls::checkForUpdatesCheck, 2035},
    {&SettingsControls::guideLogFolderButton, 2039},
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
    {&SettingsControls::forceHdr10Check, 2033},
    {&SettingsControls::forceHdr10Help, 2034},
    {&SettingsControls::hdrChromaCombo, 2044},
    {&SettingsControls::hdrChromaHelp, 2045},
    {&SettingsControls::surround51Check, 2046},
    {&SettingsControls::mjpegColorCombo, 2041},
    {&SettingsControls::mjpegColorHelp, 2042},
    {&SettingsControls::pixelCheck, 2003},
    {&SettingsControls::relativeSizeCheck, 2013},
    {&SettingsControls::borderlessCheck, 2007},
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
        settings.presentationMode = static_cast<PresentationMode>(profile % 3);
        settings.volumeHudPosition = static_cast<VolumeHudPosition>(profile % 4);
        settings.scalingMode = profile % 2 ? ScalingMode::Sharp : ScalingMode::Smooth;
        settings.fullscreenCursorMode = profile % 2 ? FullscreenCursorMode::AlwaysVisible : FullscreenCursorMode::AutoHide;
        settings.uiLanguage = static_cast<UiLanguage>(profile % 3);
        settings.mjpegColorOverride = static_cast<llcv::video_color::Override>(profile % 5);
        settings.hdrChromaLocation = static_cast<llcv::hdr::ChromaLocation>(profile % 3);
        settings.allowVolumeBoost = (profile & 1) != 0;
        settings.pixelPerfect = (profile & 2) != 0;
        settings.relativeWindowSize = (profile & 4) != 0;
        settings.borderlessWindow = (profile & 8) != 0;
        settings.windowSnap = (profile & 16) != 0;
        settings.forceHdr10 = !settings.allowVolumeBoost;
        settings.muteWhenBackground = !settings.pixelPerfect;
        settings.audioOnly = !settings.relativeWindowSize;
        settings.consoleSurround51 = profile % 2 != 0;
        settings.saveLog = !settings.borderlessWindow;
        settings.showDiagnosticConsole = !settings.windowSnap;
        settings.skipStartupSettings = settings.allowVolumeBoost;
        settings.checkForUpdates = settings.pixelPerfect;
        settings.captureDeviceId = devices[profile % 2].id;
        settings.captureAudioDeviceId = devices[1 - profile % 2].id;
        const bool available = (profile & 1) != 0;
        const bool noDevices = profile % 7 == 0;
        const auto inputs = noDevices ? std::span<const llcv::capture::DeviceInfo>{}
                                     : std::span<const llcv::capture::DeviceInfo>{devices};
        state.activeTab = static_cast<SettingsTab>(profile % 4);
        const llcv::display::MonitorChoice monitors[] = {
            {nullptr,L"monitor-a",L"DISPLAY1 · Test A"},
            {nullptr,L"monitor-b",L"DISPLAY2 · Test B"}};
        settings.preferredDisplayMonitor = profile % 3 == 0 ? L"" : profile % 3 == 1 ? L"monitor-b" : L"missing";
        SettingsControlInitialValues initial{
            settings, english, available, L"vTEST", presets[profile % 3].preset,
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
        Check(TabCtrl_GetItemCount(state.tabControl) == 4 &&
              TabCtrl_GetCurSel(state.tabControl) == static_cast<int>(state.activeTab), "tab count and initial selection");
        Check(SendMessageW(state.audioCombo, CB_GETCOUNT, 0, 0) == (available ? 3 : 2), "ASIO option visibility");
        const int audio = settings.audioMode == AudioMode::Asio && available ? 2 :
                          settings.audioMode == AudioMode::WasapiExclusive ? 1 : 0;
        Check(selection(state.audioCombo) == audio, "initial audio mode with unavailable-ASIO fallback");
        const int drift = settings.driftCorrection == DriftCorrectionMode::Resample ? 2 :
                          settings.driftCorrection == DriftCorrectionMode::Auto ? 1 : 0;
        Check(selection(state.driftCombo) == drift, "initial clock correction");
        Check(selection(state.pcmQueueCombo) == profile % 5 &&
              SendMessageW(state.pcmQueueCombo, CB_GETITEMDATA, profile % 5, 0) == settings.pcmQueueTargetMs, "PCM values and selection");
        Check(selection(state.presentationCombo) == profile % 3, "presentation mode mapping");
        Check(selection(state.displayMonitorCombo) == (profile % 3 == 0 ? 0 : profile % 3 == 1 ? 2 : 3), "display selection / disconnected preference");
        Check(selection(state.videoCombo) == profile % 3, "initial video preset");
        Check(selection(state.volumeHudCombo) == profile % 4, "volume HUD selection");
        Check(selection(state.languageCombo) == profile % 3, "language preference selection");
        Check(selection(state.scalingCombo) == profile % 2, "scaling selection");
        Check(selection(state.fullscreenCursorCombo) == profile % 2, "cursor selection");
        Check(selection(state.mjpegColorCombo) == profile % 5, "MJPEG interpretation selection");
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
            {state.windowSnapCheck, settings.windowSnap},
            {state.forceHdr10Check, settings.forceHdr10},
            {state.muteBackgroundCheck, settings.muteWhenBackground},
            {state.audioOnlyCheck, settings.audioOnly},
            {state.surround51Check, settings.consoleSurround51},
            {state.saveLogCheck, settings.saveLog},
            {state.showConsoleCheck, settings.showDiagnosticConsole},
            {state.skipStartupCheck, settings.skipStartupSettings},
            {state.checkForUpdatesCheck, settings.checkForUpdates},
        };
        for (const auto& item : checks)
            Check((SendMessageW(item.handle, BM_GETCHECK, 0, 0) == BST_CHECKED) == item.expected, "initial checkbox value");
        wchar_t caption[512]{};
        GetWindowTextW(state.startButton, caption, 512);
        Check(std::wcscmp(caption, english ? L"Start" : L"시작") == 0, "factory language is explicit");
        GetWindowTextW(state.updateTitle, caption, 512);
        Check(std::wcsstr(caption, L"vTEST") != nullptr, "explicit version label");
        GetWindowTextW(state.fullscreenCursorHint, caption, 512);
        Check(std::wcscmp(caption, english ? L"F11  Toggle borderless fullscreen" :
                         L"F11  보더리스 전체화면 켜기/끄기") == 0, "F11 caption unchanged");
        Check((GetWindowLongPtrW(state.fullscreenCursorHint, GWL_STYLE) & SS_TYPEMASK) == SS_RIGHT,
              "F11 right alignment unchanged");
        Check(SendMessageW(state.tooltipWindow, TTM_GETTOOLCOUNT, 0, 0) == 8, "seven help buttons and display monitor tooltip registered");
        ApplySettingsFont(&state, parent, 96);
        LayoutSettingsControls(&state, 96);
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
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX init{sizeof(init), ICC_WIN95_CLASSES};
    Check(InitCommonControlsEx(&init) != FALSE, "initialize tooltip controls");
    TestTranslationBoundary();
    TestActualControlCreation();
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
            L"", WS_CHILD | (combo ? CBS_DROPDOWNLIST : check ? BS_AUTOCHECKBOX : 0),
            0, 0, 100, 100, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(state.*entry.handle != nullptr, entry.name);
    }
    unsigned cases = 0;
    for (UINT dpi : {96u, 144u, 192u}) for (bool english : {false, true}) {
        SetWindowTextW(state.volumeBoostCheck, english
            ? L"Allow volume boost above 100% (up to 200%)"
            : L"100% 이상 볼륨 증폭 허용 (최대 200%)");
        ApplySettingsFont(&state, parent, dpi);
        LOGFONTW regular{}, section{};
        GetObjectW(reinterpret_cast<HFONT>(SendMessageW(state.audioLabel, WM_GETFONT, 0, 0)), sizeof(regular), &regular);
        GetObjectW(reinterpret_cast<HFONT>(SendMessageW(state.audioOutputSection, WM_GETFONT, 0, 0)), sizeof(section), &section);
        Check(regular.lfWeight == FW_NORMAL && section.lfWeight == FW_SEMIBOLD, "font hierarchy unchanged");
        Check(regular.lfHeight == -MulDiv(9, dpi, 72), "font size unchanged");
        for (bool pixel : {false, true}) for (bool relative : {false, true})
        for (auto tab : {SettingsTab::Audio, SettingsTab::VideoWindow, SettingsTab::GuideDiagnostics, SettingsTab::Updates})
        for (auto format : {VideoPixelFormat::Auto, VideoPixelFormat::Nv12, VideoPixelFormat::Yuy2,
                            VideoPixelFormat::Mjpeg, VideoPixelFormat::P010})
        for (bool exclusive : {false, true}) {
            state.activeTab = tab;
            SendMessageW(state.pixelCheck, BM_SETCHECK, pixel ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(state.relativeSizeCheck, BM_SETCHECK, relative ? BST_CHECKED : BST_UNCHECKED, 0);
            LayoutSettingsControls(&state, dpi);
            UpdateAdvancedControlVisibility(&state, exclusive, format);
            const bool video = tab == SettingsTab::VideoWindow;
            const bool audio = tab == SettingsTab::Audio;
            for (const auto& item : kGeometry) {
                const RECT rect = ClientRectOf(parent, state.*item.handle);
                const bool combo = std::strstr(item.name, "Combo") != nullptr;
                if (rect.left != MulDiv(item.x, dpi, 96) ||
                    rect.top != MulDiv(pixel ? item.pixelY : item.normalY, dpi, 96) ||
                    rect.right - rect.left != MulDiv(item.width, dpi, 96) ||
                    (!combo && rect.bottom - rect.top != MulDiv(item.height, dpi, 96))) {
                    std::fprintf(stderr, "Geometry mismatch: %s dpi=%u pixel=%d\n", item.name, dpi, pixel);
                    return EXIT_FAILURE;
                }
            }
            const RECT boost = ClientRectOf(parent, state.volumeBoostCheck);
            const RECT button = ClientRectOf(parent, state.volumeBoostHelp);
            Check(button.left - boost.right >= MulDiv(9, dpi, 96) &&
                  button.left - boost.right <= MulDiv(11, dpi, 96),
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
            ExpectVisible(state.scalingCombo, video && !pixel);
            ExpectVisible(state.relativeSizeWarning, video && pixel && relative);
            ExpectVisible(state.fullscreenCursorHint, video);
            ExpectVisible(state.guideLogFolderButton, tab == SettingsTab::GuideDiagnostics);
            ExpectVisible(state.updateNowButton, tab == SettingsTab::Updates);
            for (HWND control : {state.languageCombo, state.skipStartupCheck, state.versionWatermark,
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
                                 state.volumeBoostHelp, state.forceHdr10Help, state.mjpegColorHelp, state.hdrChromaHelp};
    for (HWND help : helpControls) {
        AddSettingsTooltip(&state, parent, help, L"Persistent test tooltip");
        Check(IsSettingsHelpControl(&state, help), "help handle is recognized");
    }
    Check(state.tooltipWindow != nullptr &&
          SendMessageW(state.tooltipWindow, TTM_GETTOOLCOUNT, 0, 0) == 7, "all tooltip tools register with v1 structure");
    TestHelpText();
    DestroyWindow(state.tooltipWindow);
    DestroyWindow(parent);
    for (HFONT font : state.uiFonts) Check(DeleteObject(font) != FALSE, "release dialog fonts");
    std::printf("Settings view: %u combinations, %zu golden rectangles, Korean/English help, tooltips passed.\n",
                cases, sizeof(kGeometry) / sizeof(kGeometry[0]));
    return EXIT_SUCCESS;
}
