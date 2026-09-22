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
    Check(!loaded.settings.consoleSurround51, "5.1 is opt-in for old and fresh profiles");
    Check(loaded.settings.audioOnlyWidth == 380 && loaded.settings.audioOnlyHeight == 230,
          "audio-only default size preserves old profiles");
    Check(loaded.settings.preferredDisplayMonitor.empty(), "default display is automatic");
    Check(loaded.settings.audioMode ==
              llcv::settings::AudioMode::WasapiShared,
          "default audio mode");
    Check(loaded.settings.wasapiBufferMs == 20,
          "default WASAPI buffer");
    Check(loaded.settings.pcmQueueTargetMs == 25,
          "default PCM queue");
    Check(loaded.settings.videoPreset ==
              llcv::settings::VideoPreset::R1920x1080,
          "default resolution");
    Check(loaded.settings.checkForUpdates,
          "automatic update check defaults on");
    Check(loaded.settings.hdrChromaLocation == llcv::hdr::ChromaLocation::Auto,
          "old and fresh profiles keep automatic HDR chroma");
}

void TestRoundTrip(const std::wstring& path) {
    using namespace llcv::settings;
    AppSettings saved{};
    saved.consoleSurround51 = true;
    saved.preferredDisplayMonitor = L"interface:monitor-test-id";
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
    saved.hdrChromaLocation = llcv::hdr::ChromaLocation::TopLeft;
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
    saved.audioOnlyWidth = 640;
    saved.audioOnlyHeight = 360;

    SaveToIni(path, saved);
    const LoadResult result = LoadFromIni(path);
    Check(result.settings.consoleSurround51, "surround preference round trip");
    const AppSettings& loaded = result.settings;
    Check(loaded.audioOnlyWidth == 640 && loaded.audioOnlyHeight == 360,
          "audio-only size round trip independent from video scale");
    Check(loaded.hdrChromaLocation == saved.hdrChromaLocation, "HDR chroma round trip");
    Check(loaded.preferredDisplayMonitor == saved.preferredDisplayMonitor, "display monitor round trip");
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

void TestPresentationModes(const std::wstring& path) {
    using namespace llcv::settings;
    for (const auto mode : {PresentationMode::AllowTearing,
                            PresentationMode::VSync,
                            PresentationMode::Compatibility}) {
        AppSettings settings{};
        settings.presentationMode = mode;
        SaveToIni(path, settings);
        Check(LoadFromIni(path).settings.presentationMode == mode,
              "all presentation modes round trip");
    }
    WritePrivateProfileStringW(L"Video", L"Presentation", L"unknown", path.c_str());
    Check(LoadFromIni(path).settings.presentationMode == PresentationMode::AllowTearing,
          "unknown presentation mode retains original default");
    WritePrivateProfileStringW(L"Video", L"Presentation", nullptr, path.c_str());
    Check(LoadFromIni(path).settings.presentationMode == PresentationMode::AllowTearing,
          "missing presentation mode retains original default");
}

void TestPcmDefaultAndPreservation(const std::wstring& path) {
    using namespace llcv::settings;
    Check(AppSettings{}.pcmQueueTargetMs == 25, "fresh/reset settings use 25ms");
    for (int value : {10, 15, 20, 25, 30}) {
        AppSettings saved{};
        saved.pcmQueueTargetMs = value;
        SaveToIni(path, saved);
        Check(LoadFromIni(path).settings.pcmQueueTargetMs == value,
              "existing saved PCM values must not migrate to the new default");
    }
    Check(WritePrivateProfileStringW(L"Audio", L"PcmQueueTargetMs", nullptr, path.c_str()) != 0,
          "remove PCM setting from temporary test INI");
    Check(LoadFromIni(path).settings.pcmQueueTargetMs == 25,
          "missing PCM key uses the same default as fresh settings");
    Check(WritePrivateProfileStringW(L"Audio", L"PcmQueueTargetMs", L"999", path.c_str()) != 0,
          "write invalid PCM setting to temporary test INI");
    Check(LoadFromIni(path).settings.pcmQueueTargetMs == 25,
          "invalid PCM value falls back to the current default");
}

void TestLegacyPcmMigration(const std::wstring& path) {
    using namespace llcv::settings;
    for (int value : {10, 15, 20, 25, 30}) {
        AppSettings saved{};
        saved.pcmQueueTargetMs = value;
        saved.skipStartupSettings = true;
        saved.volumePercent = 75;
        SaveToIni(path, saved);
        Check(WritePrivateProfileStringW(L"Audio", L"PcmQueueDefaultsVersion", nullptr, path.c_str()) != 0,
              "create legacy unversioned PCM settings");
        Check(WritePrivateProfileStringW(L"Custom", L"Untouched", L"73", path.c_str()) != 0,
              "create unrelated test setting");
        const int expected = value == 20 ? 25 : value;
        Check(LoadFromIni(path).settings.pcmQueueTargetMs == expected,
              "read-only loading applies migration in memory too");
        Check(GetPrivateProfileIntW(L"Audio", L"PcmQueueTargetMs", 0, path.c_str()) == static_cast<UINT>(value),
              "LoadFromIni remains read-only");
        Check(MigrateLegacyPcmQueueTarget(path), "legacy migration persists");
        Check(GetPrivateProfileIntW(L"Audio", L"PcmQueueTargetMs", 0, path.c_str()) == static_cast<UINT>(expected),
              "only legacy 20ms is upgraded on disk");
        Check(MigrateLegacyPcmQueueTarget(path), "migration is repeatable without changing the result");
        const auto loaded = LoadFromIni(path).settings;
        Check(loaded.pcmQueueTargetMs == expected && loaded.skipStartupSettings && loaded.volumePercent == 75 &&
              GetPrivateProfileIntW(L"Custom", L"Untouched", 0, path.c_str()) == 73,
              "direct-start and unrelated settings are preserved");
    }
    AppSettings chosen{};
    chosen.pcmQueueTargetMs = 20;
    SaveToIni(path, chosen);
    Check(MigrateLegacyPcmQueueTarget(path) && LoadFromIni(path).settings.pcmQueueTargetMs == 20,
          "user may select 20ms again after migration");
    Check(WritePrivateProfileStringW(L"Audio", L"PcmQueueDefaultsVersion", L"2", path.c_str()) != 0,
          "write future migration marker");
    Check(MigrateLegacyPcmQueueTarget(path) && LoadFromIni(path).settings.pcmQueueTargetMs == 20,
          "future marker must not trigger old migration");
    DeleteFileW(path.c_str());
    Check(MigrateLegacyPcmQueueTarget(path) && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES,
          "migration must not create a settings file for a fresh install");
}

void TestHdrChroma(const std::wstring& path) {
    using namespace llcv;
    for (auto mode : {hdr::ChromaLocation::Auto, hdr::ChromaLocation::TopLeft, hdr::ChromaLocation::Left}) {
        settings::AppSettings saved{};
        saved.pixelFormat = settings::VideoPixelFormat::P010;
        saved.hdrChromaLocation = mode;
        settings::SaveToIni(path, saved);
        Check(settings::LoadFromIni(path).settings.hdrChromaLocation == mode,
              "each HDR placement survives save/load");
    }
    for (const wchar_t* value : {L"garbage", L"6", L"-1", L"999", L""}) {
        WritePrivateProfileStringW(L"Video", L"HdrChromaLocation", value, path.c_str());
        Check(settings::LoadFromIni(path).settings.hdrChromaLocation == hdr::ChromaLocation::Auto,
              "invalid HDR placement never enables an override");
    }
    WritePrivateProfileStringW(L"Video", L"HdrChromaLocation", L"topleft", path.c_str());
    Check(settings::LoadFromIni(path).settings.hdrChromaLocation == hdr::ChromaLocation::TopLeft,
          "placement string is case insensitive");
    WritePrivateProfileStringW(L"Video", L"HdrChromaLocation", nullptr, path.c_str());
    Check(settings::LoadFromIni(path).settings.hdrChromaLocation == hdr::ChromaLocation::Auto,
          "missing placement from legacy profile stays Auto");
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
    WritePrivateProfileStringW(L"Window", L"AudioOnlyWidth", L"-1", path.c_str());
    WritePrivateProfileStringW(L"Window", L"AudioOnlyHeight", L"999999", path.c_str());
    const auto bounded = llcv::settings::LoadFromIni(path).settings;
    Check(bounded.audioOnlyWidth == 380 && bounded.audioOnlyHeight == 16384,
          "invalid audio-only dimensions are bounded");
    TestHdrChroma(path);
    TestPresentationModes(path);
    TestPcmDefaultAndPreservation(path);
    TestLegacyPcmMigration(path);
    DeleteFileW(path.c_str());
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
    std::puts("Settings store tests passed.");
    return 0;
}
