#include "settings/SettingsStore.h"
#include "settings/TestProfile.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <vector>

namespace llcv::settings {
namespace {

constexpr int kMaximumVolumePercent = 200;
constexpr size_t kMaximumExclusiveEndpointCacheEntries = 32;
constexpr int kExclusiveProbeVersion = 2;
constexpr int kRelativeScaleUnit = 1'000'000;
constexpr std::array<int, 6> kExclusiveBufferOptionsMs{5, 10, 15, 20, 30, 40};
constexpr std::array<int, 5> kPcmQueueOptionsMs{10, 15, 20, 25, 30};
constexpr int kPcmQueueDefaultsVersion = 1;

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
    wchar_t* end = nullptr;
    errno = 0;
    const long parsed = wcstol(value.c_str(), &end, 10);
    if (end == value.c_str() || errno == ERANGE ||
        parsed < (std::numeric_limits<int>::min)() ||
        parsed > (std::numeric_limits<int>::max)()) return defaultValue;
    while (*end && std::iswspace(*end)) ++end;
    // A damaged or manually edited value must not silently become zero (for
    // example muting a channel), nor accept a truncated numeric prefix.
    return *end ? defaultValue : static_cast<int>(parsed);
}

bool ReadBool(
    const std::wstring& path, const wchar_t* section, const wchar_t* key,
    bool defaultValue = false) {
    return ReadInt(path, section, key, defaultValue ? 1 : 0) != 0;
}

struct IniWriter {
    const std::wstring& file;
    DWORD error = ERROR_SUCCESS;
};

void WriteString(
    IniWriter& path, const wchar_t* section, const wchar_t* key,
    const wchar_t* value) {
    if (path.error) return;
    SetLastError(ERROR_SUCCESS);
    if (!WritePrivateProfileStringW(section, key, value, path.file.c_str())) {
        path.error = GetLastError();
        if (!path.error) path.error = ERROR_WRITE_FAULT;
    }
}

void WriteInt(
    IniWriter& path, const wchar_t* section, const wchar_t* key,
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

video_color::MatrixOverride ParseMjpegMatrix(const std::wstring& value) {
    if (_wcsicmp(value.c_str(), L"Bt709") == 0) return video_color::MatrixOverride::Bt709;
    if (_wcsicmp(value.c_str(), L"Bt601") == 0) return video_color::MatrixOverride::Bt601;
    return video_color::MatrixOverride::Auto;
}

const wchar_t* MjpegMatrixSettingName(video_color::MatrixOverride mode) {
    switch (mode) {
    case video_color::MatrixOverride::Bt709: return L"Bt709";
    case video_color::MatrixOverride::Bt601: return L"Bt601";
    default: return L"Auto";
    }
}

video_color::RangeOverride ParseSdrRange(const std::wstring& value) {
    if (_wcsicmp(value.c_str(), L"Limited") == 0) return video_color::RangeOverride::Limited;
    if (_wcsicmp(value.c_str(), L"Full") == 0) return video_color::RangeOverride::Full;
    return video_color::RangeOverride::Auto;
}

const wchar_t* SdrRangeSettingName(video_color::RangeOverride mode) {
    switch (mode) {
    case video_color::RangeOverride::Limited: return L"Limited";
    case video_color::RangeOverride::Full: return L"Full";
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
    case VideoPreset::R1280x720:
        width = 1280;
        height = 720;
        break;
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

bool MigrateLegacyPcmQueueTarget(const std::wstring& path) {
    if(test_profile::Protected(path)){SetLastError(ERROR_ACCESS_DENIED);return false;}
    if (ReadInt(path, L"Audio", L"PcmQueueDefaultsVersion") >= kPcmQueueDefaultsVersion ||
        ReadInt(path, L"Audio", L"PcmQueueTargetMs", 25) != 20) return true;
    // Write the value first: if the marker fails, retrying is harmless.
    // Do not rewrite the entire INI or migrate any other saved setting.
    if (!WritePrivateProfileStringW(L"Audio", L"PcmQueueTargetMs", L"25", path.c_str()))
        return false;
    return WritePrivateProfileStringW(L"Audio", L"PcmQueueDefaultsVersion", L"1", path.c_str()) != 0;
}

LoadResult LoadFromIni(const std::wstring& path) {
    LoadResult result{};
    if(test_profile::Protected(path))return result;
    AppSettings& settings = result.settings;
    for (size_t i=0;i<shortcuts::Count;++i)
        settings.shortcuts[i]=static_cast<unsigned>(ReadInt(path,L"Shortcuts",shortcuts::Names[i],shortcuts::Defaults[i]));
#ifdef LLCV_RECORDING
    if (ReadString(path,L"Shortcuts",L"Recording",L"missing")==L"missing")
        shortcuts::AddRecordingDefault(settings.shortcuts);
#endif
    if (ReadString(path,L"Shortcuts",L"Mute",L"missing")==L"missing")
        shortcuts::AddMuteDefault(settings.shortcuts);
    if (!shortcuts::Valid(settings.shortcuts)) settings.shortcuts=shortcuts::Defaults;
    settings.settingsLightTheme = _wcsicmp(
        ReadString(path, L"General", L"SettingsTheme", L"Dark").c_str(), L"Light") == 0;

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
    settings.preventSleep = ReadBool(path, L"General", L"PreventSleep", true);
    settings.audioOnly = ReadBool(path, L"General", L"AudioOnly");
    settings.audioOnlyWidth = std::clamp(
        ReadInt(path, L"Window", L"AudioOnlyWidth", 380), 380, 16384);
    settings.audioOnlyHeight = std::clamp(
        ReadInt(path, L"Window", L"AudioOnlyHeight", 230), 230, 16384);
    settings.consoleSurround51 = ReadBool(path, L"Audio", L"ConsoleSurround51");

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
        ReadInt(path, L"Audio", L"PcmQueueTargetMs", settings.pcmQueueTargetMs);
    if (std::find(kPcmQueueOptionsMs.begin(), kPcmQueueOptionsMs.end(),
                  requestedQueueMs) != kPcmQueueOptionsMs.end()) {
        settings.pcmQueueTargetMs = requestedQueueMs;
    }
    if (requestedQueueMs == 20 &&
        ReadInt(path, L"Audio", L"PcmQueueDefaultsVersion") < kPcmQueueDefaultsVersion) {
        settings.pcmQueueTargetMs = 25;
    }

    settings.allowVolumeBoost =
        ReadBool(path, L"Audio", L"AllowVolumeBoost");
    const int maximumVolume =
        settings.allowVolumeBoost ? kMaximumVolumePercent : 100;
    const int loadedVolume = std::clamp(
        ReadInt(path, L"Audio", L"Volume", 100), 0, maximumVolume);
    settings.volumePercent = std::clamp(
        ((loadedVolume + 2) / 5) * 5, 0, maximumVolume);
    // Clamp before rounding, as for master volume: even a valid INT_MAX in a
    // profile must not overflow when adding the rounding bias.
    const int loadedLeftVolume = std::clamp(
        ReadInt(path, L"Audio", L"LeftVolume", 100), 0, 100);
    const int loadedRightVolume = std::clamp(
        ReadInt(path, L"Audio", L"RightVolume", 100), 0, 100);
    settings.leftVolumePercent = ((loadedLeftVolume + 2) / 5) * 5;
    settings.rightVolumePercent = ((loadedRightVolume + 2) / 5) * 5;
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

    const bool currentExclusiveProbe = ReadInt(
        path, L"ExclusiveEndpointCache", L"ProbeVersion", 0) == kExclusiveProbeVersion;
    if (!currentExclusiveProbe) {
        settings.exclusiveVerifiedEndpointId.clear();
        settings.exclusiveVerifiedBufferMs = 0;
    }
    const UINT cacheCount = currentExclusiveProbe ? (std::min)(
        GetPrivateProfileIntW(
            L"ExclusiveEndpointCache", L"Count", 0, path.c_str()),
        static_cast<UINT>(kMaximumExclusiveEndpointCacheEntries)) : 0;
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
        const bool inconclusive = _wcsicmp(state.c_str(), L"RetryRequired") == 0;
        if (!endpointId.empty() &&
            (unsupported || inconclusive ||
             (supported && IsExclusiveLowLatencyBuffer(bufferMs)))) {
            settings.exclusiveEndpointCache.push_back(
                {endpointId, supported, supported ? bufferMs : 0, inconclusive});
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
    const auto hdrChroma = ReadString(path, L"Video", L"HdrChromaLocation", L"Auto");
    settings.hdrChromaLocation = _wcsicmp(hdrChroma.c_str(), L"TopLeft") == 0
        ? hdr::ChromaLocation::TopLeft
        : _wcsicmp(hdrChroma.c_str(), L"Left") == 0
            ? hdr::ChromaLocation::Left : hdr::ChromaLocation::Auto;
    const auto mjpegMatrix = ReadString(path, L"Video", L"MjpegMatrix", L"__missing__");
    const auto sdrRange = ReadString(path, L"Video", L"SdrRange", L"__missing__");
    settings.mjpegMatrixOverride = ParseMjpegMatrix(mjpegMatrix);
    settings.sdrRangeOverride = ParseSdrRange(sdrRange);
    settings.legacyMjpegRangeOverride = ParseSdrRange(
        ReadString(path, L"Video", L"LegacyMjpegRange", L"Auto"));
    if (mjpegMatrix == L"__missing__" && sdrRange == L"__missing__") {
        const auto legacy = ReadString(path, L"Video", L"MjpegColor", L"Auto");
        for (const auto* matrix : {L"Bt709", L"Bt601"}) {
            for (const auto* range : {L"Full", L"Limited"}) {
                if (_wcsicmp(legacy.c_str(), (std::wstring(matrix) + range).c_str()) == 0) {
                    settings.mjpegMatrixOverride = ParseMjpegMatrix(matrix);
                    settings.legacyMjpegRangeOverride = ParseSdrRange(range);
                }
            }
        }
    }
    if (settings.sdrRangeOverride != video_color::RangeOverride::Auto)
        settings.legacyMjpegRangeOverride = video_color::RangeOverride::Auto;
    settings.captureDeviceId =
        ReadString(path, L"Video", L"CaptureDeviceId");
    settings.captureAudioDeviceId =
        ReadString(path, L"Video", L"CaptureAudioDeviceId");
    const std::wstring resolution =
        ReadString(path, L"Video", L"Resolution", L"1920x1080");
    if (_wcsicmp(resolution.c_str(), L"1280x720") == 0) {
        settings.videoPreset = VideoPreset::R1280x720;
    } else if (_wcsicmp(resolution.c_str(), L"1920x1080") == 0) {
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
    settings.capture40Fps = ReadBool(path, L"Video", L"Capture40Fps");
    settings.allowUnlistedFps = ReadBool(path, L"Video", L"AllowUnlistedFps");
    const auto presentation = ReadString(path, L"Video", L"Presentation",
                                         L"AllowTearing");
    settings.presentationMode = _wcsicmp(presentation.c_str(), L"ExclusiveFullscreen") == 0
        ? PresentationMode::ExclusiveFullscreen
        : _wcsicmp(presentation.c_str(), L"Compatibility") == 0
        ? PresentationMode::Compatibility
        : _wcsicmp(presentation.c_str(), L"VSync") == 0
            ? PresentationMode::VSync : PresentationMode::AllowTearing;
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

    settings.preferredDisplayMonitor = ReadString(path, L"Video", L"DisplayMonitor");
    settings.fullscreenVideoPosition = FullscreenPositionFromIndex(
        ReadInt(path, L"Video", L"FullscreenVideoPosition"));
    settings.windowSnap = ReadBool(path, L"Window", L"Snap", true);
    settings.roundedCorners = ReadBool(path, L"Window", L"RoundedCorners", true);
    const std::wstring windowX = ReadString(path, L"Window", L"X");
    const std::wstring windowY = ReadString(path, L"Window", L"Y");
    if (!windowX.empty() && !windowY.empty()) {
        settings.hasWindowPosition = true;
        settings.windowX = static_cast<int>(wcstol(windowX.c_str(), nullptr, 10));
        settings.windowY = static_cast<int>(wcstol(windowY.c_str(), nullptr, 10));
        settings.monitorDevice = ReadString(path, L"Window", L"Monitor");
    }
    settings.saveLog = ReadBool(path, L"Diagnostics", L"SaveLog");
    settings.screenshotClipboard = ReadBool(path, L"Screenshot", L"CopyToClipboard");
    settings.vsrEnabled = ReadBool(path, L"Video", L"VsrEnabled");
    settings.cmaaNisEnabled = ReadBool(path, L"Video", L"CmaaNisEnabled");
    const int nisSharpness = ReadInt(path, L"Video", L"NisSharpness", 15);
    settings.nisSharpnessPercent = nisSharpness >= 0 && nisSharpness <= 100 ? nisSharpness : 15;
    if (settings.cmaaNisEnabled) settings.vsrEnabled = false;
    settings.preferredUpscaler = ReadInt(path, L"Video", L"PreferredUpscaler", 1) == 0
        ? UpscalingMethod::NvidiaVsr : UpscalingMethod::CmaaNis;
    settings.preferredUpscaler = SelectedUpscaler(settings);
    // Legacy profiles used Resolution for capture while VSR was off. Do not
    // activate a dormant VsrCaptureResolution when migrating those profiles.
    const std::wstring legacyCapture = settings.vsrEnabled
        ? ReadString(path, L"Video", L"VsrCaptureResolution", resolution.c_str()) : resolution;
    const std::wstring captureResolution = ReadString(path, L"Video", L"CaptureResolution", legacyCapture.c_str());
    settings.capturePreset = settings.videoPreset;
    if (captureResolution == L"1280x720") settings.capturePreset = VideoPreset::R1280x720;
    else if (captureResolution == L"1920x1080") settings.capturePreset = VideoPreset::R1920x1080;
    else if (captureResolution == L"2560x1440") settings.capturePreset = VideoPreset::R2560x1440;
    else if (captureResolution == L"3840x2160") settings.capturePreset = VideoPreset::R3840x2160;
    // Migrate only profiles without an explicit mode, after resolving legacy VSR capture sizes.
    // Equal old sizes follow capture without changing their current appearance;
    // explicit modes (including fixed at equal sizes) are never migrated again.
    const auto displayMode = ReadString(path, L"Video", L"DisplayMatchesCapture", L"__missing__");
    settings.displayMatchesCapture = displayMode == L"__missing__"
        ? (ReadString(path, L"Video", L"Resolution").empty() ||
           settings.capturePreset == settings.videoPreset)
        : ReadBool(path, L"Video", L"DisplayMatchesCapture");
    settings.showDiagnosticConsole =
        ReadBool(path, L"Diagnostics", L"ShowConsole");
    return result;
}

static DWORD WriteSettings(const std::wstring& file, const AppSettings& settings) {
    IniWriter path{file};
    const auto& bindings=shortcuts::Valid(settings.shortcuts) ? settings.shortcuts : shortcuts::Defaults;
    for (size_t i=0;i<shortcuts::Count;++i)
        WriteInt(path,L"Shortcuts",shortcuts::Names[i],static_cast<int>(bindings[i]));
    const wchar_t* language = L"Auto";
    if (settings.uiLanguage == UiLanguage::Korean) language = L"Korean";
    if (settings.uiLanguage == UiLanguage::English) language = L"English";
    WriteString(path, L"General", L"Language", language);
    WriteString(path, L"General", L"SettingsTheme", settings.settingsLightTheme ? L"Light" : L"Dark");
    WriteInt(path, L"General", L"SkipStartupSettings",
             settings.skipStartupSettings ? 1 : 0);
    WriteInt(path, L"General", L"CheckForUpdates",
             settings.checkForUpdates ? 1 : 0);
    WriteInt(path, L"General", L"PreventSleep", settings.preventSleep ? 1 : 0);
    WriteInt(path, L"General", L"AudioOnly", settings.audioOnly ? 1 : 0);
    WriteInt(path, L"Window", L"AudioOnlyWidth", settings.audioOnlyWidth);
    WriteInt(path, L"Window", L"AudioOnlyHeight", settings.audioOnlyHeight);
    WriteInt(path, L"Audio", L"ConsoleSurround51", settings.consoleSurround51 ? 1 : 0);

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
    WriteInt(path, L"Audio", L"PcmQueueDefaultsVersion", kPcmQueueDefaultsVersion);
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

    WriteString(path, L"ExclusiveEndpointCache", nullptr, nullptr);
    WriteInt(path, L"ExclusiveEndpointCache", L"ProbeVersion", kExclusiveProbeVersion);
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
                    entry.inconclusive ? L"RetryRequired" :
                    entry.supported ? L"Supported" : L"Unsupported");
        WriteInt(path, L"ExclusiveEndpointCache", bufferKey,
                 entry.recommendedBufferMs);
    }

    WriteInt(path, L"Video", L"ForceHdr10", settings.forceHdr10 ? 1 : 0);
    WriteString(path, L"Video", L"HdrChromaLocation",
                hdr::ChromaLocationName(settings.hdrChromaLocation));
    WriteString(path, L"Video", L"MjpegMatrix", MjpegMatrixSettingName(settings.mjpegMatrixOverride));
    WriteString(path, L"Video", L"SdrRange", SdrRangeSettingName(settings.sdrRangeOverride));
    WriteString(path, L"Video", L"LegacyMjpegRange",
        settings.sdrRangeOverride == video_color::RangeOverride::Auto &&
        settings.legacyMjpegRangeOverride != video_color::RangeOverride::Auto
            ? SdrRangeSettingName(settings.legacyMjpegRangeOverride) : nullptr);
    WriteString(path, L"Video", L"MjpegColor", nullptr);
    int width = 0;
    int height = 0;
    VideoDimensions(settings.videoPreset, width, height);
    wchar_t resolution[32]{};
    swprintf_s(resolution, L"%dx%d", width, height);
    WriteString(path, L"Video", L"Resolution", resolution);
    WriteInt(path, L"Video", L"DisplayMatchesCapture", settings.displayMatchesCapture ? 1 : 0);
    VideoDimensions(settings.capturePreset, width, height);
    swprintf_s(resolution, L"%dx%d", width, height);
    WriteString(path, L"Video", L"CaptureResolution", resolution);
    WriteString(path, L"Video", L"CaptureDeviceId",
                settings.captureDeviceId.c_str());
    WriteString(path, L"Video", L"CaptureAudioDeviceId",
                settings.captureAudioDeviceId.c_str());
    WriteString(path, L"Video", L"PixelFormat",
                PixelFormatSettingName(settings.pixelFormat));
    WriteInt(path, L"Video", L"FrameRate", settings.videoFrameRate);
    WriteInt(path, L"Video", L"Capture40Fps", settings.capture40Fps ? 1 : 0);
    WriteInt(path, L"Video", L"AllowUnlistedFps", settings.allowUnlistedFps ? 1 : 0);
    WriteString(path, L"Video", L"Presentation",
                settings.presentationMode == PresentationMode::ExclusiveFullscreen
                    ? L"ExclusiveFullscreen"
                    : settings.presentationMode == PresentationMode::Compatibility
                    ? L"Compatibility"
                    : settings.presentationMode == PresentationMode::VSync
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

    WriteString(path, L"Video", L"DisplayMonitor", settings.preferredDisplayMonitor.c_str());
    WriteInt(path, L"Video", L"FullscreenVideoPosition",
        static_cast<int>(FullscreenPositionFromIndex(static_cast<int>(settings.fullscreenVideoPosition))));
    WriteInt(path, L"Window", L"Snap", settings.windowSnap ? 1 : 0);
    WriteInt(path, L"Window", L"RoundedCorners", settings.roundedCorners ? 1 : 0);
    WriteInt(path, L"Diagnostics", L"SaveLog", settings.saveLog ? 1 : 0);
    WriteInt(path, L"Screenshot", L"CopyToClipboard", settings.screenshotClipboard ? 1 : 0);
    WriteInt(path, L"Video", L"VsrEnabled", settings.vsrEnabled && !settings.cmaaNisEnabled ? 1 : 0);
    WriteInt(path, L"Video", L"CmaaNisEnabled", settings.cmaaNisEnabled ? 1 : 0);
    WriteInt(path, L"Video", L"PreferredUpscaler", static_cast<int>(SelectedUpscaler(settings)));
    WriteInt(path, L"Video", L"NisSharpness", std::clamp(settings.nisSharpnessPercent, 0, 100));
    WriteInt(path, L"Diagnostics", L"ShowConsole",
             settings.showDiagnosticConsole ? 1 : 0);
    return path.error;
}

template<class Writer>
static bool UpdateIniAtomically(const std::wstring& path, Writer write,
                                unsigned long* error) {
    const auto fail = [error](DWORD code) {
        if (error) *error = code ? code : ERROR_WRITE_FAULT;
        return false;
    };
    if (error) *error = ERROR_SUCCESS;
    if (path.empty()) return fail(ERROR_INVALID_NAME);
    if (test_profile::Protected(path)) return fail(ERROR_ACCESS_DENIED);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const bool existing = attributes != INVALID_FILE_ATTRIBUTES;
    if (existing && (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)))
        return fail(ERROR_ACCESS_DENIED);
    if (!existing && GetLastError() != ERROR_FILE_NOT_FOUND)
        return fail(GetLastError());

    // Keep explicitly restricted profiles restricted, including during staging.
    // A rename would otherwise inherit the temporary file's directory ACL.
    std::vector<BYTE> security;
    SECURITY_ATTRIBUTES securityAttributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    SECURITY_INFORMATION securityFlags = DACL_SECURITY_INFORMATION;
    if (existing) {
        DWORD bytes = 0;
        GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &bytes);
        if (!bytes || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return fail(GetLastError());
        security.resize(bytes);
        if (!GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                             security.data(), bytes, &bytes)) return fail(GetLastError());
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(security.data(), &control, &revision))
            return fail(GetLastError());
        securityFlags |= (control & SE_DACL_PROTECTED)
            ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
        securityAttributes.lpSecurityDescriptor = security.data();
    }

    static std::atomic<unsigned long> sequence{0};
    wchar_t suffix[96]{};
    swprintf_s(suffix, L".tmp-%lu-%llu-%lu", GetCurrentProcessId(),
               GetTickCount64(), sequence.fetch_add(1, std::memory_order_relaxed));
    const std::wstring temporary = path + suffix;
    const HANDLE reservation = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0,
        existing ? &securityAttributes : nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reservation == INVALID_HANDLE_VALUE) return fail(GetLastError());
    // New profiles are UTF-16 so device names round-trip in every UI locale.
    const WORD bom = 0xfeff;
    DWORD written = 0;
    const BOOL initialized = WriteFile(reservation, &bom, sizeof(bom), &written, nullptr);
    const DWORD initializeError = GetLastError();
    CloseHandle(reservation);
    struct TemporaryFile {
        const std::wstring& name;
        ~TemporaryFile() { DeleteFileW(name.c_str()); }
    } cleanup{temporary};
    if (!initialized || written != sizeof(bom)) return fail(initializeError);
    // Preserve unknown keys and old profile encoding. Never partially overwrite
    // the live file (including Exclusive verification data) on a failed write.
    if (existing && !CopyFileW(path.c_str(), temporary.c_str(), FALSE))
        return fail(GetLastError());
    if (existing && !SetFileSecurityW(temporary.c_str(), securityFlags, security.data()))
        return fail(GetLastError());
    const DWORD writeError = write(temporary);
    if (writeError) return fail(writeError);
    // Flush the profile API's cache before file-level replacement.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.c_str());
    const HANDLE completed = CreateFileW(temporary.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (completed == INVALID_HANDLE_VALUE) return fail(GetLastError());
    const BOOL flushed = FlushFileBuffers(completed);
    const DWORD flushError = GetLastError();
    CloseHandle(completed);
    if (!flushed) return fail(flushError);
    // A sibling rename has no cross-volume copy/delete fallback. ReplaceFile
    // without a backup can remove the original before reporting error 1176.
    const BOOL replaced = MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_WRITE_THROUGH | (existing ? MOVEFILE_REPLACE_EXISTING : 0));
    if (!replaced) return fail(GetLastError());
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    return true;
}

bool SaveToIni(const std::wstring& path, const AppSettings& settings,
               unsigned long* error) {
    return UpdateIniAtomically(path, [&](const auto& temporary) {
        return WriteSettings(temporary, settings);
    }, error);
}

bool StartupReviewRequired(const std::wstring& path, const std::wstring& version) {
    if (version.empty() || test_profile::Protected(path)) return false;
    return ReadString(path,L"General",L"StartupSettingsVersion") != version;
}

bool MarkStartupSettingsShown(const std::wstring& path, const std::wstring& version,
                             unsigned long* error) {
    if (version.empty()) {
        if (error) *error=ERROR_INVALID_PARAMETER;
        return false;
    }
    return UpdateIniAtomically(path, [&](const auto& temporary) {
        IniWriter writer{temporary};
        WriteString(writer,L"General",L"StartupSettingsVersion",version.c_str());
        return writer.error;
    }, error);
}

}  // namespace llcv::settings
