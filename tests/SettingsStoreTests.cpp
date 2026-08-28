#include "settings/SettingsStore.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++failures;
}

std::wstring TemporaryIniPath() {
    wchar_t directory[MAX_PATH]{};
    wchar_t path[MAX_PATH]{};
    if (!GetTempPathW(ARRAYSIZE(directory), directory) ||
        !GetTempFileNameW(directory, L"lcv", 0, path)) {
        return {};
    }
    return path;
}

void TestDefaults(const std::wstring& path) {
    DeleteFileW(path.c_str());
    const auto loaded = llcv::settings::LoadFromIni(path);
    Check(loaded.settings.audioMode ==
              llcv::settings::AudioMode::WasapiShared,
          "default audio mode");
    Check(loaded.settings.wasapiBufferMs == 20,
          "default WASAPI buffer");
    Check(loaded.settings.pcmQueueTargetMs == 20,
          "default PCM queue");
    Check(loaded.settings.videoPreset ==
              llcv::settings::VideoPreset::R1920x1080,
          "default resolution");
    Check(loaded.settings.checkForUpdates,
          "automatic update check defaults on");
}

void TestRoundTrip(const std::wstring& path) {
    using namespace llcv::settings;
    AppSettings saved{};
    saved.uiLanguage = UiLanguage::English;
    saved.audioMode = AudioMode::Asio;
    saved.wasapiBufferMs = 30;
    saved.wasapiSharedPeriodFrames = 144;
    saved.driftCorrection = DriftCorrectionMode::Resample;
    saved.pcmQueueTargetMs = 25;
    saved.volumePercent = 175;
    saved.leftVolumePercent = 80;
    saved.rightVolumePercent = 65;
    saved.allowVolumeBoost = true;
    saved.volumeHudPosition = VolumeHudPosition::BottomRight;
    saved.muteWhenBackground = true;
    saved.audioOutputDeviceId = L"output-id";
    saved.exclusiveVerifiedEndpointId = L"exclusive-id";
    saved.exclusiveVerifiedBufferMs = 30;
    saved.exclusiveEndpointCache = {
        {L"supported-id", true, 20},
        {L"unsupported-id", false, 0},
    };
    saved.asioDriverName = L"ASIO Test Driver";
    saved.videoPreset = VideoPreset::R3840x2160;
    saved.pixelFormat = VideoPixelFormat::P010;
    saved.videoFrameRate = 60;
    saved.captureDeviceId = L"capture-id";
    saved.captureAudioDeviceId = L"capture-audio-id";
    saved.presentationMode = PresentationMode::VSync;
    saved.scalingMode = ScalingMode::Sharp;
    saved.fullscreenCursorMode = FullscreenCursorMode::AlwaysVisible;
    saved.forceHdr10 = true;
    saved.mjpegColorOverride = llcv::video_color::Override::Bt709Full;
    saved.pixelPerfect = false;
    saved.relativeWindowSize = true;
    saved.relativeWindowScalePpm = 666'667;
    saved.borderlessWindow = true;
    saved.windowSnap = false;
    saved.saveLog = true;
    saved.showDiagnosticConsole = true;
    saved.skipStartupSettings = true;
    saved.checkForUpdates = false;
    saved.audioOnly = true;

    SaveToIni(path, saved);
    const LoadResult result = LoadFromIni(path);
    const AppSettings& loaded = result.settings;
    Check(loaded.uiLanguage == saved.uiLanguage, "language round trip");
    Check(loaded.audioMode == saved.audioMode, "audio mode round trip");
    Check(loaded.wasapiBufferMs == saved.wasapiBufferMs,
          "audio buffer round trip");
    Check(loaded.wasapiSharedPeriodFrames == saved.wasapiSharedPeriodFrames,
          "shared period round trip");
    Check(loaded.driftCorrection == saved.driftCorrection,
          "drift correction round trip");
    Check(loaded.pcmQueueTargetMs == saved.pcmQueueTargetMs,
          "PCM queue round trip");
    Check(loaded.volumePercent == saved.volumePercent,
          "master volume round trip");
    Check(loaded.leftVolumePercent == saved.leftVolumePercent &&
              loaded.rightVolumePercent == saved.rightVolumePercent,
          "channel volume round trip");
    Check(loaded.volumeHudPosition == saved.volumeHudPosition,
          "HUD position round trip");
    Check(loaded.exclusiveEndpointCache.size() == 2,
          "exclusive cache count round trip");
    Check(loaded.asioDriverName == saved.asioDriverName,
          "ASIO driver round trip");
    Check(loaded.videoPreset == saved.videoPreset,
          "resolution round trip");
    Check(loaded.pixelFormat == saved.pixelFormat,
          "pixel format round trip");
    Check(loaded.presentationMode == saved.presentationMode,
          "presentation mode round trip");
    Check(loaded.scalingMode == saved.scalingMode,
          "scaling mode round trip");
    Check(loaded.fullscreenCursorMode == saved.fullscreenCursorMode,
          "cursor mode round trip");
    Check(loaded.mjpegColorOverride == saved.mjpegColorOverride,
          "MJPEG color override round trip");
    Check(loaded.relativeWindowScalePpm == saved.relativeWindowScalePpm,
          "relative scale round trip");
    Check(result.relativeWindowScaleVersion == kRelativeWindowScaleVersion,
          "relative scale version round trip");
    Check(loaded.saveLog && loaded.showDiagnosticConsole,
          "diagnostics round trip");
    Check(loaded.skipStartupSettings && !loaded.checkForUpdates &&
              loaded.audioOnly,
          "general settings round trip");
}

}  // namespace

int main() {
    const std::wstring path = TemporaryIniPath();
    if (path.empty()) {
        std::fprintf(stderr, "FAILED: could not create temporary INI path\n");
        return 1;
    }
    TestDefaults(path);
    TestRoundTrip(path);
    DeleteFileW(path.c_str());
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
    std::puts("Settings store tests passed.");
    return 0;
}
