#pragma once

#include "SettingsView.h"
#include "DisplayMonitorSelection.h"
#include "settings/AppSettings.h"
#include <span>

namespace llcv::capture { struct DeviceInfo; }

namespace llcv::settings_ui {

namespace control_id {
inline constexpr int IDC_SETTINGS_AUDIO = 2001;
inline constexpr int IDC_SETTINGS_VIDEO = 2002;
inline constexpr int IDC_SETTINGS_PIXEL = 2003;
inline constexpr int IDC_SETTINGS_START = 2004;
inline constexpr int IDC_SETTINGS_CANCEL = 2005;
inline constexpr int IDC_SETTINGS_BUFFER = 2006;
inline constexpr int IDC_SETTINGS_BORDERLESS = 2007;
inline constexpr int IDC_SETTINGS_AUDIO_STATUS = 2008;
inline constexpr int IDC_SETTINGS_PRESENTATION = 2009;
inline constexpr int IDC_SETTINGS_VOLUME_HUD = 2010;
inline constexpr int IDC_SETTINGS_DRIFT = 2011;
inline constexpr int IDC_SETTINGS_WINDOW_SNAP = 2012;
inline constexpr int IDC_SETTINGS_RELATIVE_SIZE = 2013;
inline constexpr int IDC_SETTINGS_DRIFT_HELP = 2014;
inline constexpr int IDC_SETTINGS_PCM_QUEUE = 2015;
inline constexpr int IDC_SETTINGS_AUDIO_OUTPUT = 2016;
inline constexpr int IDC_SETTINGS_CAPTURE_DEVICE = 2017;
inline constexpr int IDC_SETTINGS_PIXEL_FORMAT = 2018;
inline constexpr int IDC_SETTINGS_SAVE_LOG = 2019;
inline constexpr int IDC_SETTINGS_FRAME_RATE = 2020;
inline constexpr int IDC_SETTINGS_MUTE_BACKGROUND = 2021;
inline constexpr int IDC_SETTINGS_PRESENTATION_HELP = 2022;
inline constexpr int IDC_SETTINGS_PCM_QUEUE_HELP = 2023;
inline constexpr int IDC_SETTINGS_LANGUAGE = 2024;
inline constexpr int IDC_SETTINGS_SHOW_CONSOLE = 2025;
inline constexpr int IDC_SETTINGS_CAPTURE_AUDIO_DEVICE = 2026;
inline constexpr int IDC_SETTINGS_SCALING = 2027;
inline constexpr int IDC_SETTINGS_SKIP_STARTUP = 2028;
inline constexpr int IDC_SETTINGS_VOLUME_BOOST = 2029;
inline constexpr int IDC_SETTINGS_VOLUME_BOOST_HELP = 2030;
inline constexpr int IDC_SETTINGS_AUDIO_ONLY = 2032;
inline constexpr int IDC_SETTINGS_FORCE_HDR10 = 2033;
inline constexpr int IDC_SETTINGS_FORCE_HDR10_HELP = 2034;
inline constexpr int IDC_SETTINGS_UPDATE_CHECK = 2035;
inline constexpr int IDC_SETTINGS_EXCLUSIVE_TEST = 2036;
inline constexpr int IDC_SETTINGS_TAB = 2037;
inline constexpr int IDC_SETTINGS_UPDATE_NOW = 2038;
inline constexpr int IDC_SETTINGS_OPEN_LOG_FOLDER = 2039;
inline constexpr int IDC_SETTINGS_FULLSCREEN_CURSOR = 2040;
inline constexpr int IDC_SETTINGS_MJPEG_COLOR = 2041;
inline constexpr int IDC_SETTINGS_MJPEG_COLOR_HELP = 2042;
inline constexpr int IDC_SETTINGS_DISPLAY_MONITOR = 2043;
inline constexpr int IDC_SETTINGS_HDR_CHROMA = 2044;
inline constexpr int IDC_SETTINGS_HDR_CHROMA_HELP = 2045;
inline constexpr int IDC_SETTINGS_SURROUND51 = 2046;
inline constexpr int IDC_SETTINGS_SCREENSHOT_CLIPBOARD = 2047;
inline constexpr int IDC_SETTINGS_SCREENSHOT_FOLDER = 2048;
inline constexpr int IDC_SETTINGS_UPSCALER = 2049;
inline constexpr int IDC_SETTINGS_NIS_SHARPNESS = 2083;
inline constexpr int IDC_SETTINGS_VSR_GUIDE = 2050;
inline constexpr int IDC_SETTINGS_DISPLAY_RESOLUTION = 2051;
inline constexpr int IDC_SETTINGS_VIDEO_REFRESH = 2052;
inline constexpr int IDC_SETTINGS_THEME = 2053;
inline constexpr int IDC_SETTINGS_ROUNDED_CORNERS = 2054;
inline constexpr int IDC_SETTINGS_SECTION = 2055;
inline constexpr int IDC_SETTINGS_PREVENT_SLEEP = 2056;
inline constexpr int IDC_SETTINGS_CAPTURE_40 = 2057;
inline constexpr int IDC_SETTINGS_CAPTURE_40_HELP = 2058;
inline constexpr int IDC_SETTINGS_LS_GUIDE = 2059;
inline constexpr int IDC_SETTINGS_SHORTCUT_RESET = 2060;
inline constexpr int IDC_SETTINGS_FULLSCREEN_POSITION = 2061;
inline constexpr int IDC_SETTINGS_UNLISTED_FPS = 2078;
inline constexpr int IDC_SETTINGS_SDR_RANGE = 2079;
inline constexpr int IDC_SETTINGS_SDR_RANGE_HELP = 2080;
inline constexpr int IDC_SETTINGS_FRAME_OPTIONS = 2081;
inline constexpr int IDC_SETTINGS_FRAME_OPTIONS_CLOSE = 2082;
inline constexpr int IDC_SETTINGS_CREATE_REPORT = 2084;
inline constexpr int IDC_SETTINGS_CMAA_QUALITY = 2085;
inline constexpr int IDC_SETTINGS_RELEASE_NOTES = 2086;
inline constexpr int IDC_SETTINGS_SHORTCUT_FIRST = 2100;
} // namespace control_id

// Borrowed for the duration of creation only; no settings/device list copies.
struct SettingsControlInitialValues {
    const settings::AppSettings& settings;
    bool english;
    bool asioAvailable;
    const wchar_t* versionLabel;
    settings::VideoPreset initialVideoPreset;
    std::span<const capture::DeviceInfo> captureDevices;
    std::span<const capture::DeviceInfo> captureAudioDevices;
    std::span<const settings::VideoPresetInfo> videoPresets;
    std::span<const int, 5> pcmQueueOptionsMs;
    std::span<const display::MonitorChoice> displayMonitors{};
};

// Required synchronous population hooks. They run at the same points as the
// old WM_CREATE code: output picker, buffer picker, then video format pickers.
// The controller owns device queries and its working selections. These hooks
// and their context are never retained or invoked from another thread.
struct SettingsControlPopulation {
    void* context;
    void (*audioOutput)(void*);
    void (*buffer)(void*);
    void (*pixelFormat)(void*);
};

void CreateSettingsDialogControls(SettingsControls* state, HWND hwnd,
                                  HINSTANCE instance,
                                  const SettingsControlInitialValues& initial,
                                  const SettingsControlPopulation& population);
void PopulateCaptureDevicePicker(HWND picker, std::span<const capture::DeviceInfo> devices,
                                 const std::wstring& selected, bool audio, bool english);
void PopulateNisSharpnessPicker(HWND picker, int selectedPercent);
int SelectedNisSharpness(HWND picker);
void ApplyResolutionSelections(const SettingsControls& state,
                              std::span<const settings::VideoPresetInfo> presets,
                              settings::AppSettings& settings);

} // namespace llcv::settings_ui
