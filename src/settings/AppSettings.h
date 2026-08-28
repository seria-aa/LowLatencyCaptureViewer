#pragma once

#include "video/VideoColor.h"

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
};

enum class PresentationMode {
    AllowTearing,
    VSync,
};

enum class ScalingMode {
    Smooth,
    Sharp,
};

enum class FullscreenCursorMode {
    AutoHide,
    AlwaysVisible,
};

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
};

struct AppSettings {
    UiLanguage uiLanguage = UiLanguage::Auto;
    AudioMode audioMode = AudioMode::WasapiShared;
    int wasapiBufferMs = 20;
    uint32_t wasapiSharedPeriodFrames = 0;
    DriftCorrectionMode driftCorrection = DriftCorrectionMode::Auto;
    int pcmQueueTargetMs = 20;
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
    VideoPreset videoPreset = VideoPreset::R1920x1080;
    VideoPixelFormat pixelFormat = VideoPixelFormat::Auto;
    int videoFrameRate = 0;
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
    bool showDiagnosticConsole = false;
    bool skipStartupSettings = false;
    bool checkForUpdates = true;
    bool audioOnly = false;
    bool forceHdr10 = false;
    video_color::Override mjpegColorOverride = video_color::Override::Auto;
    bool pixelPerfect = true;
    bool relativeWindowSize = false;
    int relativeWindowScalePpm = 0;
    bool borderlessWindow = false;
    bool windowSnap = true;
    bool hasWindowPosition = false;
    int windowX = 0;
    int windowY = 0;
    std::wstring monitorDevice;
};

}  // namespace llcv::settings
