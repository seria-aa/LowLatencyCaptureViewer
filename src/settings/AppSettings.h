#pragma once

#include "video/VideoColor.h"
#include "video/HdrChroma.h"
#include "input/Shortcuts.h"

#include <cstdint>
#include <string>
#include <vector>

namespace llcv::settings {

enum class AudioMode {
    WasapiShared,
    WasapiExclusive,
    Asio,
};

enum class DriftCorrectionMode {
    Off,
    Auto,
    Resample,
};

enum class VolumeHudPosition {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

enum class VideoPreset {
    R1920x1080,
    R2560x1440,
    R3840x2160,
    R1280x720,
};

struct VideoPresetInfo {
    VideoPreset preset;
    int width;
    int height;
    int framerate;
    const wchar_t* label;
};

enum class PresentationMode {
    AllowTearing = 0,
    VSync = 1,
    Compatibility = 2,
    ExclusiveFullscreen = 3,
};

enum class ScalingMode {
    Smooth,
    Sharp,
};

enum class FullscreenCursorMode {
    AutoHide,
    AlwaysVisible,
};

enum class FullscreenVideoPosition {
    Center, TopLeft, Top, TopRight, Left, Right, BottomLeft, Bottom, BottomRight,
};
inline constexpr int kFullscreenVideoPositionCount = 9;
constexpr FullscreenVideoPosition FullscreenPositionFromIndex(int index) {
    return index >= 0 && index < kFullscreenVideoPositionCount
        ? static_cast<FullscreenVideoPosition>(index) : FullscreenVideoPosition::Center;
}

enum class VideoPixelFormat {
    Auto,
    Nv12,
    Yuy2,
    Mjpeg,
    P010,
};

enum class UiLanguage {
    Auto,
    Korean,
    English,
};

// A preflight verdict belongs to one stable Core Audio endpoint ID. Both
// passes and failures are retained so settings need not retest every launch.
struct ExclusiveEndpointCacheEntry {
    std::wstring endpointId;
    bool supported = false;
    int recommendedBufferMs = 0;
    bool inconclusive = false; // A completed attempt awaiting explicit retry.
};

struct AppSettings {
    shortcuts::Bindings shortcuts = shortcuts::Defaults;
    UiLanguage uiLanguage = UiLanguage::Auto;
    bool settingsLightTheme = false; // App chrome; persisted key retained for compatibility.
    AudioMode audioMode = AudioMode::WasapiShared;
    bool consoleSurround51 = false;
    int wasapiBufferMs = 20;
    uint32_t wasapiSharedPeriodFrames = 0;
    DriftCorrectionMode driftCorrection = DriftCorrectionMode::Auto;
    int pcmQueueTargetMs = 25;
    int volumePercent = 100;
    int leftVolumePercent = 100;
    int rightVolumePercent = 100;
    bool allowVolumeBoost = false;
    VolumeHudPosition volumeHudPosition = VolumeHudPosition::TopLeft;
    bool muteWhenBackground = false;
    PresentationMode presentationMode = PresentationMode::AllowTearing;
    ScalingMode scalingMode = ScalingMode::Smooth;
    FullscreenCursorMode fullscreenCursorMode =
        FullscreenCursorMode::AutoHide;
    VideoPreset videoPreset = VideoPreset::R1920x1080; // Display size, independent of VSR.
    VideoPreset capturePreset = VideoPreset::R1920x1080;
    // New profiles follow capture; retain a fixed selection for manual switching.
    bool displayMatchesCapture = true;
    FullscreenVideoPosition fullscreenVideoPosition = FullscreenVideoPosition::Center;
    VideoPixelFormat pixelFormat = VideoPixelFormat::Auto;
    int videoFrameRate = 0;
    // Explicit driver request; leave the normal frame-rate preference intact.
    bool capture40Fps = false;
    bool allowUnlistedFps = false;
    std::wstring captureDeviceId;
    // Empty means: use an audio pin on the video filter when available, then
    // try a clearly matching DirectShow audio-capture filter.
    std::wstring captureAudioDeviceId;
    std::wstring audioOutputDeviceId;
    // A successful low-latency Exclusive preflight is tied to the resolved
    // endpoint, even when the user follows the Windows default.
    std::wstring exclusiveVerifiedEndpointId;
    int exclusiveVerifiedBufferMs = 0;
    std::vector<ExclusiveEndpointCacheEntry> exclusiveEndpointCache;
    std::wstring asioDriverName;
    bool saveLog = false;
    bool screenshotClipboard = false;
    bool vsrEnabled = false;
    bool cmaaNisEnabled = false;
    int nisSharpnessPercent = 15; // 0..100; default 0.15. Display processing only.
    bool showDiagnosticConsole = false;
    bool skipStartupSettings = false;
    bool checkForUpdates = true;
    bool preventSleep = true;
    bool audioOnly = false;
    // Client pixels, independent from video sizing preferences.
    int audioOnlyWidth = 380;
    int audioOnlyHeight = 230;
    bool forceHdr10 = false;
    hdr::ChromaLocation hdrChromaLocation = hdr::ChromaLocation::Auto;
    video_color::MatrixOverride mjpegMatrixOverride = video_color::MatrixOverride::Auto;
    video_color::RangeOverride sdrRangeOverride = video_color::RangeOverride::Auto;
    // Older MJPEG profiles stored matrix and range together. Preserve their
    // correction without changing raw SDR until the common range is edited.
    video_color::RangeOverride legacyMjpegRangeOverride = video_color::RangeOverride::Auto;
    bool pixelPerfect = true;
    bool relativeWindowSize = false;
    int relativeWindowScalePpm = 0;
    bool borderlessWindow = false;
    bool roundedCorners = true;
    bool windowSnap = true;
    bool hasWindowPosition = false;
    int windowX = 0;
    int windowY = 0;
    std::wstring monitorDevice;
    // Explicit startup display; empty preserves the last-window-position policy.
    std::wstring preferredDisplayMonitor;
};

inline VideoPreset EffectiveDisplayPreset(const AppSettings& settings) {
    return settings.displayMatchesCapture ? settings.capturePreset : settings.videoPreset;
}

inline video_color::RangeOverride EffectiveSdrRange(const AppSettings& settings, bool mjpeg) {
    return settings.sdrRangeOverride != video_color::RangeOverride::Auto || !mjpeg
        ? settings.sdrRangeOverride : settings.legacyMjpegRangeOverride;
}

}  // namespace llcv::settings
