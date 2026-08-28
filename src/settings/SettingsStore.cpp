#include "settings/SettingsStore.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwchar>

namespace llcv::settings {
namespace {

constexpr int kMaximumVolumePercent = 200;
constexpr size_t kMaximumExclusiveEndpointCacheEntries = 32;
constexpr int kRelativeScaleUnit = 1'000'000;
constexpr std::array<int, 6> kExclusiveBufferOptionsMs{5, 10, 15, 20, 30, 40};
constexpr std::array<int, 5> kPcmQueueOptionsMs{10, 15, 20, 25, 30};

std::wstring ReadString(
    const std::wstring& path, const wchar_t* section, const wchar_t* key,
    const wchar_t* defaultValue = L"") {
    std::array<wchar_t, 1024> buffer{};
    GetPrivateProfileStringW(
        section, key, defaultValue, buffer.data(),
        static_cast<DWORD>(buffer.size()), path.c_str());
    return buffer.data();
}

int ReadInt(
    const std::wstring& path, const wchar_t* section, const wchar_t* key,
    int defaultValue = 0) {
    wchar_t defaultText[32]{};
    swprintf_s(defaultText, L"%d", defaultValue);
    const std::wstring value = ReadString(path, section, key, defaultText);
    return static_cast<int>(wcstol(value.c_str(), nullptr, 10));
}

bool ReadBool(
    const std::wstring& path, const wchar_t* section, const wchar_t* key,
    bool defaultValue = false) {
    return ReadInt(path, section, key, defaultValue ? 1 : 0) != 0;
}

void WriteString(
    const std::wstring& path, const wchar_t* section, const wchar_t* key,
    const wchar_t* value) {
    WritePrivateProfileStringW(section, key, value, path.c_str());
}

void WriteInt(
    const std::wstring& path, const wchar_t* section, const wchar_t* key,
    int value) {
    wchar_t text[32]{};
    swprintf_s(text, L"%d", value);
    WriteString(path, section, key, text);
}

bool IsExclusiveLowLatencyBuffer(int bufferMs) {
    return std::find(
               kExclusiveBufferOptionsMs.begin(),
               kExclusiveBufferOptionsMs.end(), bufferMs) !=
           kExclusiveBufferOptionsMs.end();
}

video_color::Override ParseMjpegColorOverride(const std::wstring& value) {
    if (_wcsicmp(value.c_str(), L"Bt709Full") == 0) {
        return video_color::Override::Bt709Full;
    }
    if (_wcsicmp(value.c_str(), L"Bt709Limited") == 0) {
        return video_color::Override::Bt709Limited;
    }
    if (_wcsicmp(value.c_str(), L"Bt601Full") == 0) {
        return video_color::Override::Bt601Full;
    }
    if (_wcsicmp(value.c_str(), L"Bt601Limited") == 0) {
        return video_color::Override::Bt601Limited;
    }
    return video_color::Override::Auto;
}

const wchar_t* MjpegColorOverrideSettingName(video_color::Override mode) {
    switch (mode) {
    case video_color::Override::Bt709Full: return L"Bt709Full";
    case video_color::Override::Bt709Limited: return L"Bt709Limited";
    case video_color::Override::Bt601Full: return L"Bt601Full";
    case video_color::Override::Bt601Limited: return L"Bt601Limited";
    default: return L"Auto";
    }
}

const wchar_t* PixelFormatSettingName(VideoPixelFormat format) {
    switch (format) {
    case VideoPixelFormat::Nv12: return L"NV12";
    case VideoPixelFormat::Yuy2: return L"YUY2";
    case VideoPixelFormat::Mjpeg: return L"MJPEG";
    case VideoPixelFormat::P010: return L"P010";
    default: return L"Auto";
    }
}

void VideoDimensions(VideoPreset preset, int& width, int& height) {
    switch (preset) {
    case VideoPreset::R2560x1440:
        width = 2560;
        height = 1440;
        break;
    case VideoPreset::R3840x2160:
        width = 3840;
        height = 2160;
        break;
    default:
        width = 1920;
        height = 1080;
        break;
    }
}

}  // namespace

LoadResult LoadFromIni(const std::wstring& path) {
    LoadResult result{};
    AppSettings& settings = result.settings;

    const std::wstring language =
        ReadString(path, L"General", L"Language", L"Auto");
    if (_wcsicmp(language.c_str(), L"English") == 0) {
        settings.uiLanguage = UiLanguage::English;
    } else if (_wcsicmp(language.c_str(), L"Korean") == 0) {
        settings.uiLanguage = UiLanguage::Korean;
    }
    settings.skipStartupSettings =
        ReadBool(path, L"General", L"SkipStartupSettings");
    settings.checkForUpdates =
        ReadBool(path, L"General", L"CheckForUpdates", true);
    settings.audioOnly = ReadBool(path, L"General", L"AudioOnly");

    const std::wstring audioMode =
        ReadString(path, L"Audio", L"Mode", L"Shared");
    if (_wcsicmp(audioMode.c_str(), L"Exclusive") == 0) {
        settings.audioMode = AudioMode::WasapiExclusive;
    } else if (_wcsicmp(audioMode.c_str(), L"ASIO") == 0) {
        settings.audioMode = AudioMode::Asio;
    }
    settings.asioDriverName =
        ReadString(path, L"Audio", L"AsioDriver");
    settings.audioOutputDeviceId =
        ReadString(path, L"Audio", L"OutputDeviceId");
    settings.exclusiveVerifiedEndpointId =
        ReadString(path, L"Audio", L"ExclusiveVerifiedEndpointId");
    settings.exclusiveVerifiedBufferMs =
        ReadInt(path, L"Audio", L"ExclusiveVerifiedBufferMs");

    const int requestedBufferMs =
        ReadInt(path, L"Audio", L"BufferMs", 20);
    if (IsExclusiveLowLatencyBuffer(requestedBufferMs)) {
        settings.wasapiBufferMs = requestedBufferMs;
    }
    settings.wasapiSharedPeriodFrames = static_cast<uint32_t>(
        std::max(0, ReadInt(path, L"Audio", L"SharedPeriodFrames")));
    const std::wstring drift =
        ReadString(path, L"Audio", L"DriftCorrection", L"Auto");
    if (_wcsicmp(drift.c_str(), L"Resample") == 0) {
        settings.driftCorrection = DriftCorrectionMode::Resample;
    } else if (_wcsicmp(drift.c_str(), L"Auto") != 0) {
        settings.driftCorrection = DriftCorrectionMode::Off;
    }
    const int requestedQueueMs =
        ReadInt(path, L"Audio", L"PcmQueueTargetMs", 20);
    if (std::find(kPcmQueueOptionsMs.begin(), kPcmQueueOptionsMs.end(),
                  requestedQueueMs) != kPcmQueueOptionsMs.end()) {
        settings.pcmQueueTargetMs = requestedQueueMs;
    }

    settings.allowVolumeBoost =
        ReadBool(path, L"Audio", L"AllowVolumeBoost");
    const int maximumVolume =
        settings.allowVolumeBoost ? kMaximumVolumePercent : 100;
    const int loadedVolume = std::clamp(
        ReadInt(path, L"Audio", L"Volume", 100), 0, maximumVolume);
    settings.volumePercent = std::clamp(
        ((loadedVolume + 2) / 5) * 5, 0, maximumVolume);
    settings.leftVolumePercent = std::clamp(
        ((ReadInt(path, L"Audio", L"LeftVolume", 100) + 2) / 5) * 5,
        0, 100);
    settings.rightVolumePercent = std::clamp(
        ((ReadInt(path, L"Audio", L"RightVolume", 100) + 2) / 5) * 5,
        0, 100);
    const std::wstring hudPosition =
        ReadString(path, L"Audio", L"VolumeHudPosition", L"TopLeft");
    if (_wcsicmp(hudPosition.c_str(), L"TopRight") == 0) {
        settings.volumeHudPosition = VolumeHudPosition::TopRight;
    } else if (_wcsicmp(hudPosition.c_str(), L"BottomLeft") == 0) {
        settings.volumeHudPosition = VolumeHudPosition::BottomLeft;
    } else if (_wcsicmp(hudPosition.c_str(), L"BottomRight") == 0) {
        settings.volumeHudPosition = VolumeHudPosition::BottomRight;
    }
    settings.muteWhenBackground =
        ReadBool(path, L"Audio", L"MuteWhenBackground");

    const UINT cacheCount = (std::min)(
        GetPrivateProfileIntW(
            L"ExclusiveEndpointCache", L"Count", 0, path.c_str()),
        static_cast<UINT>(kMaximumExclusiveEndpointCacheEntries));
    for (UINT index = 0; index < cacheCount; ++index) {
        wchar_t idKey[32]{};
        wchar_t stateKey[32]{};
        wchar_t bufferKey[32]{};
        swprintf_s(idKey, L"Endpoint%uId", index);
        swprintf_s(stateKey, L"Endpoint%uState", index);
        swprintf_s(bufferKey, L"Endpoint%uBufferMs", index);
        const std::wstring endpointId =
            ReadString(path, L"ExclusiveEndpointCache", idKey);
        const std::wstring state =
            ReadString(path, L"ExclusiveEndpointCache", stateKey);
        const int bufferMs =
            ReadInt(path, L"ExclusiveEndpointCache", bufferKey);
        const bool supported = _wcsicmp(state.c_str(), L"Supported") == 0;
        const bool unsupported =
            _wcsicmp(state.c_str(), L"Unsupported") == 0;
        if (!endpointId.empty() &&
            (unsupported ||
             (supported && IsExclusiveLowLatencyBuffer(bufferMs)))) {
            settings.exclusiveEndpointCache.push_back(
                {endpointId, supported, supported ? bufferMs : 0});
        }
    }
    if (settings.exclusiveEndpointCache.empty() &&
        !settings.exclusiveVerifiedEndpointId.empty() &&
        IsExclusiveLowLatencyBuffer(settings.exclusiveVerifiedBufferMs)) {
        settings.exclusiveEndpointCache.push_back({
            settings.exclusiveVerifiedEndpointId, true,
            settings.exclusiveVerifiedBufferMs});
    }

    settings.forceHdr10 = ReadBool(path, L"Video", L"ForceHdr10");
    settings.mjpegColorOverride = ParseMjpegColorOverride(
        ReadString(path, L"Video", L"MjpegColor", L"Auto"));
    settings.captureDeviceId =
        ReadString(path, L"Video", L"CaptureDeviceId");
    settings.captureAudioDeviceId =
        ReadString(path, L"Video", L"CaptureAudioDeviceId");
    const std::wstring resolution =
        ReadString(path, L"Video", L"Resolution", L"1920x1080");
    if (_wcsicmp(resolution.c_str(), L"1920x1080") == 0) {
        settings.videoPreset = VideoPreset::R1920x1080;
    } else if (_wcsicmp(resolution.c_str(), L"3840x2160") == 0) {
        settings.videoPreset = VideoPreset::R3840x2160;
    } else {
        settings.videoPreset = VideoPreset::R2560x1440;
    }
    const std::wstring pixelFormat =
        ReadString(path, L"Video", L"PixelFormat", L"Auto");
    if (_wcsicmp(pixelFormat.c_str(), L"NV12") == 0) {
        settings.pixelFormat = VideoPixelFormat::Nv12;
    } else if (_wcsicmp(pixelFormat.c_str(), L"YUY2") == 0) {
        settings.pixelFormat = VideoPixelFormat::Yuy2;
    } else if (_wcsicmp(pixelFormat.c_str(), L"MJPEG") == 0) {
        settings.pixelFormat = VideoPixelFormat::Mjpeg;
    } else if (_wcsicmp(pixelFormat.c_str(), L"P010") == 0) {
        settings.pixelFormat = VideoPixelFormat::P010;
    }
    settings.videoFrameRate =
        std::max(0, ReadInt(path, L"Video", L"FrameRate"));
    settings.presentationMode =
        _wcsicmp(ReadString(path, L"Video", L"Presentation",
                            L"AllowTearing").c_str(),
                 L"VSync") == 0
            ? PresentationMode::VSync
            : PresentationMode::AllowTearing;
    settings.scalingMode =
        _wcsicmp(ReadString(path, L"Video", L"Scaling", L"Smooth").c_str(),
                 L"Sharp") == 0
            ? ScalingMode::Sharp
            : ScalingMode::Smooth;
    settings.pixelPerfect = ReadBool(path, L"Video", L"PixelPerfect", true);
    settings.relativeWindowSize =
        ReadBool(path, L"Video", L"RelativeWindowSize");
    settings.relativeWindowScalePpm = std::clamp(
        ReadInt(path, L"Video", L"RelativeWindowScalePpm"),
        0, kRelativeScaleUnit);
    result.relativeWindowScaleVersion =
        ReadInt(path, L"Video", L"RelativeWindowScaleVersion");
    settings.borderlessWindow =
        ReadBool(path, L"Video", L"BorderlessWindow");
    settings.fullscreenCursorMode =
        _wcsicmp(ReadString(path, L"Video", L"FullscreenCursor",
                            L"AutoHide").c_str(),
                 L"AlwaysVisible") == 0
            ? FullscreenCursorMode::AlwaysVisible
            : FullscreenCursorMode::AutoHide;

    settings.windowSnap = ReadBool(path, L"Window", L"Snap", true);
    const std::wstring windowX = ReadString(path, L"Window", L"X");
    const std::wstring windowY = ReadString(path, L"Window", L"Y");
    if (!windowX.empty() && !windowY.empty()) {
        settings.hasWindowPosition = true;
        settings.windowX = static_cast<int>(wcstol(windowX.c_str(), nullptr, 10));
        settings.windowY = static_cast<int>(wcstol(windowY.c_str(), nullptr, 10));
        settings.monitorDevice = ReadString(path, L"Window", L"Monitor");
    }
    settings.saveLog = ReadBool(path, L"Diagnostics", L"SaveLog");
    settings.showDiagnosticConsole =
        ReadBool(path, L"Diagnostics", L"ShowConsole");
    return result;
}

void SaveToIni(const std::wstring& path, const AppSettings& settings) {
    const wchar_t* language = L"Auto";
    if (settings.uiLanguage == UiLanguage::Korean) language = L"Korean";
    if (settings.uiLanguage == UiLanguage::English) language = L"English";
    WriteString(path, L"General", L"Language", language);
    WriteInt(path, L"General", L"SkipStartupSettings",
             settings.skipStartupSettings ? 1 : 0);
    WriteInt(path, L"General", L"CheckForUpdates",
             settings.checkForUpdates ? 1 : 0);
    WriteInt(path, L"General", L"AudioOnly", settings.audioOnly ? 1 : 0);

    const wchar_t* audioMode = L"Shared";
    if (settings.audioMode == AudioMode::WasapiExclusive) {
        audioMode = L"Exclusive";
    } else if (settings.audioMode == AudioMode::Asio) {
        audioMode = L"ASIO";
    }
    WriteString(path, L"Audio", L"Mode", audioMode);
    WriteString(path, L"Audio", L"AsioDriver", settings.asioDriverName.c_str());
    WriteInt(path, L"Audio", L"BufferMs", settings.wasapiBufferMs);
    WriteInt(path, L"Audio", L"SharedPeriodFrames",
             static_cast<int>(settings.wasapiSharedPeriodFrames));
    const wchar_t* drift = L"Off";
    if (settings.driftCorrection == DriftCorrectionMode::Auto) {
        drift = L"Auto";
    } else if (settings.driftCorrection == DriftCorrectionMode::Resample) {
        drift = L"Resample";
    }
    WriteString(path, L"Audio", L"DriftCorrection", drift);
    WriteInt(path, L"Audio", L"PcmQueueTargetMs", settings.pcmQueueTargetMs);
    WriteInt(path, L"Audio", L"Volume", settings.volumePercent);
    WriteInt(path, L"Audio", L"LeftVolume", settings.leftVolumePercent);
    WriteInt(path, L"Audio", L"RightVolume", settings.rightVolumePercent);
    WriteInt(path, L"Audio", L"AllowVolumeBoost",
             settings.allowVolumeBoost ? 1 : 0);
    const wchar_t* hudPosition = L"TopLeft";
    switch (settings.volumeHudPosition) {
    case VolumeHudPosition::TopRight: hudPosition = L"TopRight"; break;
    case VolumeHudPosition::BottomLeft: hudPosition = L"BottomLeft"; break;
    case VolumeHudPosition::BottomRight: hudPosition = L"BottomRight"; break;
    default: break;
    }
    WriteString(path, L"Audio", L"VolumeHudPosition", hudPosition);
    WriteInt(path, L"Audio", L"MuteWhenBackground",
             settings.muteWhenBackground ? 1 : 0);
    WriteString(path, L"Audio", L"OutputDeviceId",
                settings.audioOutputDeviceId.c_str());
    WriteString(path, L"Audio", L"ExclusiveVerifiedEndpointId",
                settings.exclusiveVerifiedEndpointId.c_str());
    WriteInt(path, L"Audio", L"ExclusiveVerifiedBufferMs",
             settings.exclusiveVerifiedBufferMs);

    WritePrivateProfileStringW(
        L"ExclusiveEndpointCache", nullptr, nullptr, path.c_str());
    const size_t cacheCount = (std::min)(
        settings.exclusiveEndpointCache.size(),
        kMaximumExclusiveEndpointCacheEntries);
    WriteInt(path, L"ExclusiveEndpointCache", L"Count",
             static_cast<int>(cacheCount));
    for (size_t index = 0; index < cacheCount; ++index) {
        const auto& entry = settings.exclusiveEndpointCache[index];
        wchar_t idKey[32]{};
        wchar_t stateKey[32]{};
        wchar_t bufferKey[32]{};
        swprintf_s(idKey, L"Endpoint%zuId", index);
        swprintf_s(stateKey, L"Endpoint%zuState", index);
        swprintf_s(bufferKey, L"Endpoint%zuBufferMs", index);
        WriteString(path, L"ExclusiveEndpointCache", idKey,
                    entry.endpointId.c_str());
        WriteString(path, L"ExclusiveEndpointCache", stateKey,
                    entry.supported ? L"Supported" : L"Unsupported");
        WriteInt(path, L"ExclusiveEndpointCache", bufferKey,
                 entry.recommendedBufferMs);
    }

    WriteInt(path, L"Video", L"ForceHdr10", settings.forceHdr10 ? 1 : 0);
    WriteString(path, L"Video", L"MjpegColor",
                MjpegColorOverrideSettingName(settings.mjpegColorOverride));
    int width = 0;
    int height = 0;
    VideoDimensions(settings.videoPreset, width, height);
    wchar_t resolution[32]{};
    swprintf_s(resolution, L"%dx%d", width, height);
    WriteString(path, L"Video", L"Resolution", resolution);
    WriteString(path, L"Video", L"CaptureDeviceId",
                settings.captureDeviceId.c_str());
    WriteString(path, L"Video", L"CaptureAudioDeviceId",
                settings.captureAudioDeviceId.c_str());
    WriteString(path, L"Video", L"PixelFormat",
                PixelFormatSettingName(settings.pixelFormat));
    WriteInt(path, L"Video", L"FrameRate", settings.videoFrameRate);
    WriteString(path, L"Video", L"Presentation",
                settings.presentationMode == PresentationMode::VSync
                    ? L"VSync" : L"AllowTearing");
    WriteString(path, L"Video", L"Scaling",
                settings.scalingMode == ScalingMode::Sharp
                    ? L"Sharp" : L"Smooth");
    WriteInt(path, L"Video", L"PixelPerfect",
             settings.pixelPerfect ? 1 : 0);
    WriteInt(path, L"Video", L"RelativeWindowSize",
             settings.relativeWindowSize ? 1 : 0);
    WriteInt(path, L"Video", L"RelativeWindowScalePpm",
             settings.relativeWindowScalePpm);
    WriteInt(path, L"Video", L"RelativeWindowScaleVersion",
             kRelativeWindowScaleVersion);
    WriteInt(path, L"Video", L"BorderlessWindow",
             settings.borderlessWindow ? 1 : 0);
    WriteString(path, L"Video", L"FullscreenCursor",
                settings.fullscreenCursorMode ==
                        FullscreenCursorMode::AlwaysVisible
                    ? L"AlwaysVisible" : L"AutoHide");

    WriteInt(path, L"Window", L"Snap", settings.windowSnap ? 1 : 0);
    WriteInt(path, L"Diagnostics", L"SaveLog", settings.saveLog ? 1 : 0);
    WriteInt(path, L"Diagnostics", L"ShowConsole",
             settings.showDiagnosticConsole ? 1 : 0);
}

}  // namespace llcv::settings
