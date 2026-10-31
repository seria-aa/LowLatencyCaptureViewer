#include "settings/SettingsStore.h"

#include <windows.h>
#include <sddl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    ++failures;
    return false;
}

bool CheckWin32(BOOL condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s (Win32=%lu)\n", message, GetLastError());
    ++failures;
    return false;
}

bool CheckSaved(const std::wstring& path, const llcv::settings::AppSettings& settings,
                const char* message) {
    unsigned long error = ERROR_SUCCESS;
    if (llcv::settings::SaveToIni(path, settings, &error) && error == ERROR_SUCCESS)
        return true;
    std::fprintf(stderr, "FAILED: %s (save error=%lu)\n", message, error);
    ++failures;
    return false;
}

void CheckExclusiveCacheRoundTrip(const llcv::settings::AppSettings& loaded) {
    // A failed save/load must report an assertion, never dereference an empty
    // vector. Keep the count check separate so the failure stays diagnostic.
    if (!Check(loaded.exclusiveEndpointCache.size() == 3,
               "exclusive cache count round trip")) return;
    Check(loaded.exclusiveEndpointCache.back().inconclusive &&
          !loaded.exclusiveEndpointCache.back().supported,
          "temporary failure persists as explicit retry, not unsupported");
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
    Check(!loaded.settings.settingsLightTheme, "legacy and fresh profiles retain dark settings");
    Check(!loaded.settings.screenshotClipboard, "screenshot clipboard is opt-in");
    Check(!loaded.settings.capture40Fps, "40fps is opt-in for fresh and legacy profiles");
    Check(!loaded.settings.allowUnlistedFps, "unlisted FPS is opt-in for fresh and legacy profiles");
    Check(!loaded.settings.vsrEnabled, "VSR defaults OFF for fresh/legacy settings");
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
    Check(loaded.settings.displayMatchesCapture &&
              llcv::settings::EffectiveDisplayPreset(loaded.settings) == loaded.settings.capturePreset,
          "fresh profiles follow capture resolution by default");
    Check(loaded.settings.checkForUpdates,
          "automatic update check defaults on");
    Check(loaded.settings.preventSleep, "sleep prevention defaults on for old/new profiles");
    Check(loaded.settings.roundedCorners, "rounded corners default on for old/new profiles");
    Check(loaded.settings.hdrChromaLocation == llcv::hdr::ChromaLocation::Auto,
          "old and fresh profiles keep automatic HDR chroma");
    Check(loaded.settings.sdrRangeOverride == llcv::video_color::RangeOverride::Auto &&
          loaded.settings.legacyMjpegRangeOverride == llcv::video_color::RangeOverride::Auto &&
          loaded.settings.mjpegMatrixOverride == llcv::video_color::MatrixOverride::Auto,
          "old and fresh profiles keep automatic SDR color interpretation");
    WritePrivateProfileStringW(L"General", L"CheckForUpdates", L"0", path.c_str());
    Check(llcv::settings::LoadFromIni(path).settings.preventSleep,
          "legacy profile without the new key defaults to sleep prevention");
    DeleteFileW(path.c_str()); // Preserve the following new-profile test's precondition.
}

void TestNewUnicodeProfile(const std::wstring& path) {
    using namespace llcv::settings;
    // TestDefaults removed only this test's unique GetTempFileName result.
    // Exercise a genuinely new profile, not an existing empty ANSI file.
    const bool missing = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetLastError() == ERROR_FILE_NOT_FOUND;
    Check(missing, "Unicode test starts with a nonexistent temporary profile");
    if (!missing) return;
    AppSettings settings;
    settings.asioDriverName = L"ASIO 한글 드라이버 Ω";
    settings.captureDeviceId = L"캡처 장치 日本語";
    settings.captureAudioDeviceId = L"캡처 오디오 장치 Ω";
    settings.audioOutputDeviceId = L"출력 장치 日本語";
    unsigned long error = 0;
    const bool saved = SaveToIni(path, settings, &error);
    Check(saved && error == ERROR_SUCCESS, "new Unicode profile saves successfully");
    if (!saved) return;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (CheckWin32(file != INVALID_HANDLE_VALUE, "open newly saved profile for BOM check")) {
        BYTE bom[2]{};
        DWORD bytes = 0;
        if (CheckWin32(ReadFile(file, bom, sizeof(bom), &bytes, nullptr), "read profile BOM"))
            Check(bytes == sizeof(bom) && bom[0] == 0xff && bom[1] == 0xfe,
                  "a new profile is UTF-16 little endian");
        CloseHandle(file);
    }
    for (int pass = 0; pass < 2; ++pass) {
        const auto loaded = LoadFromIni(path).settings;
        Check(loaded.asioDriverName == settings.asioDriverName &&
              loaded.captureDeviceId == settings.captureDeviceId &&
              loaded.captureAudioDeviceId == settings.captureAudioDeviceId &&
              loaded.audioOutputDeviceId == settings.audioOutputDeviceId,
              "non-ASCII driver and device names survive initial save and replacement");
        if (pass == 0) {
            settings.volumePercent = 90;
            if (!CheckSaved(path, settings, "existing Unicode profile can be replaced")) return;
        }
    }
}

void TestInvalidNumericSettings(const std::wstring& path) {
    using namespace llcv::settings;
    if (!CheckSaved(path, AppSettings{}, "reset profile for numeric validation")) return;
    const struct { const wchar_t* value; int expectedVolume; } cases[] = {
        {L"2147483647", 100}, {L"-2147483648", 0},
        {L"2147483648", 100}, {L"-2147483649", 100},
        {L"999999999999999999999999", 100},
        {L"invalid", 100}, {L"0oops", 100}, {L"", 100},
        {L"0", 0}, {L"73", 75}, {L" 75 ", 75}, {L"+50", 50},
    };
    for (const auto& item : cases) {
        for (const wchar_t* key : {L"Volume", L"LeftVolume", L"RightVolume"})
            if (!CheckWin32(WritePrivateProfileStringW(L"Audio", key, item.value, path.c_str()),
                            "write numeric volume test value")) return;
        const auto loaded = LoadFromIni(path).settings;
        Check(loaded.volumePercent == item.expectedVolume &&
              loaded.leftVolumePercent == item.expectedVolume &&
              loaded.rightVolumePercent == item.expectedVolume,
              "volume values reject malformed integers and clamp before rounding");
    }
    for (const wchar_t* value : {L"invalid", L"0oops", L"999999999999999999999999"}) {
        if (!CheckWin32(WritePrivateProfileStringW(L"General", L"PreventSleep", value, path.c_str()),
                        "write malformed boolean value")) return;
        if (!CheckWin32(WritePrivateProfileStringW(L"Audio", L"BufferMs", value, path.c_str()),
                        "write malformed buffer value")) return;
        const auto loaded = LoadFromIni(path).settings;
        Check(loaded.preventSleep && loaded.wasapiBufferMs == AppSettings{}.wasapiBufferMs,
              "malformed numeric and boolean settings retain their defaults");
    }
    CheckSaved(path, AppSettings{}, "restore profile after numeric validation");
}

std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!CheckWin32(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token),
                    "query current user token for temporary-file DACL")) return {};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (!bytes) {
        CheckWin32(FALSE, "size current user SID");
        CloseHandle(token);
        return {};
    }
    std::vector<BYTE> storage(bytes);
    const BOOL queried = GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes);
    if (!CheckWin32(queried, "read current user SID")) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);
    LPWSTR text = nullptr;
    if (!CheckWin32(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid,
                                         &text), "format current user SID")) return {};
    const std::wstring sid(text);
    LocalFree(text);
    return sid;
}

std::wstring ReadDacl(const std::wstring& path) {
    DWORD bytes = 0;
    GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &bytes);
    if (!bytes) {
        CheckWin32(FALSE, "size temporary-profile DACL");
        return {};
    }
    std::vector<BYTE> storage(bytes);
    if (!CheckWin32(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
        storage.data(), bytes, &bytes), "read temporary-profile DACL")) return {};
    LPWSTR text = nullptr;
    if (!CheckWin32(ConvertSecurityDescriptorToStringSecurityDescriptorW(storage.data(),
        SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr),
        "format temporary-profile DACL")) return {};
    const std::wstring dacl(text);
    LocalFree(text);
    return dacl;
}

void TestDaclPreservation(const std::wstring& path) {
    const auto sid = CurrentUserSid();
    if (sid.empty()) return;
    // Only the unique temporary INI is changed. Keep full access for its current
    // user, SYSTEM, and administrators; explicit protection makes ACL loss visible.
    const std::wstring policy = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!CheckWin32(ConvertStringSecurityDescriptorToSecurityDescriptorW(policy.c_str(),
        SDDL_REVISION_1, &descriptor, nullptr), "create custom temporary-file DACL")) return;
    const BOOL applied = SetFileSecurityW(path.c_str(),
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    const bool ok = CheckWin32(applied, "apply custom DACL only to the temporary profile");
    LocalFree(descriptor);
    if (!ok) return;
    const auto before = ReadDacl(path);
    if (before.empty()) return;
    Check(before.find(L"D:P") == 0, "test DACL has inheritance disabled");
    auto settings = llcv::settings::LoadFromIni(path).settings;
    settings.volumePercent = 65;
    if (!CheckSaved(path, settings, "save with a custom DACL succeeds")) return;
    const auto after = ReadDacl(path);
    Check(after == before, "replacement preserves explicit ACEs and DACL protection");
    if (after != before) {
        std::fwprintf(stderr, L"DACL before: %ls\nDACL after: %ls\n", before.c_str(), after.c_str());
    }
    Check(llcv::settings::LoadFromIni(path).settings.volumePercent == 65,
          "DACL-preserving save also updates settings");
}

void TestResolutionRoundTrips(const std::wstring& path) {
    using namespace llcv::settings;
    const struct { VideoPreset preset; const wchar_t* text; } cases[] = {
        {VideoPreset::R1280x720, L"1280x720"},
        {VideoPreset::R1920x1080, L"1920x1080"},
        {VideoPreset::R2560x1440, L"2560x1440"},
        {VideoPreset::R3840x2160, L"3840x2160"},
    };
    for (const auto& item : cases) {
        for (int fps : {0, 30, 40, 60, 120}) {
            AppSettings saved{};
            saved.videoPreset = item.preset;
            saved.capturePreset = item.preset;
            saved.videoFrameRate = fps;
            saved.pixelFormat = VideoPixelFormat::Nv12;
            if (!CheckSaved(path, saved, "save resolution fixture")) return;
            wchar_t value[32]{};
            GetPrivateProfileStringW(L"Video", L"Resolution", L"", value,
                                     ARRAYSIZE(value), path.c_str());
            Check(std::wstring(value) == item.text, "exact resolution persisted");
            const auto loaded = LoadFromIni(path).settings;
            Check(loaded.videoPreset == item.preset &&
                  loaded.capturePreset == item.preset &&
                  loaded.videoFrameRate == fps &&
                  loaded.pixelFormat == VideoPixelFormat::Nv12,
                  "all resolutions retain FPS and NV12 through save/load");
        }
        WritePrivateProfileStringW(L"Video", L"CaptureResolution", nullptr, path.c_str());
        WritePrivateProfileStringW(L"Video", L"VsrCaptureResolution", nullptr, path.c_str());
        Check(LoadFromIni(path).settings.capturePreset == item.preset,
              "legacy profiles keep their existing capture size");
        WritePrivateProfileStringW(L"Video", L"VsrCaptureResolution", L"invalid", path.c_str());
        Check(LoadFromIni(path).settings.capturePreset == item.preset,
              "invalid VSR size falls back to existing resolution");
    }
    for (bool enabled : {false, true}) {
        for (const auto& display : cases) for (const auto& capture : cases) {
            WritePrivateProfileStringW(L"Video", L"Resolution", display.text, path.c_str());
            WritePrivateProfileStringW(L"Video", L"VsrCaptureResolution", capture.text, path.c_str());
            WritePrivateProfileStringW(L"Video", L"CaptureResolution", nullptr, path.c_str());
            WritePrivateProfileStringW(L"Video", L"DisplayMatchesCapture", nullptr, path.c_str());
            WritePrivateProfileStringW(L"Video", L"VsrEnabled", enabled ? L"1" : L"0", path.c_str());
            auto migrated = LoadFromIni(path).settings;
            Check(migrated.videoPreset == display.preset &&
                  migrated.capturePreset == (enabled ? capture.preset : display.preset),
                  "migration preserves active legacy capture/display with VSR on or off");
            Check(migrated.displayMatchesCapture ==
                      (migrated.capturePreset == migrated.videoPreset) &&
                      EffectiveDisplayPreset(migrated) == display.preset,
                  "legacy equal sizes follow capture without changing initial display size");
            migrated.capturePreset = capture.preset;
            if (!CheckSaved(path, migrated, "save independent capture/display fixture")) return;
            const auto loaded = LoadFromIni(path).settings;
            Check(loaded.capturePreset == capture.preset && loaded.videoPreset == display.preset &&
                  loaded.vsrEnabled == enabled,
                  "independent capture/display round trip with VSR on or off");
            WritePrivateProfileStringW(L"Video", L"VsrEnabled", enabled ? L"0" : L"1", path.c_str());
            const auto toggled = LoadFromIni(path).settings;
            Check(toggled.capturePreset == capture.preset && toggled.videoPreset == display.preset,
                  "toggling saved VSR never reinterprets either resolution");
        }
    }
}

void TestDisplayMatchesCapturePersistence(const std::wstring& path) {
    using namespace llcv::settings;
    AppSettings fresh{};
    fresh.capturePreset = VideoPreset::R3840x2160;
    Check(fresh.displayMatchesCapture && EffectiveDisplayPreset(fresh) == fresh.capturePreset,
          "in-memory defaults follow capture edits");
    DeleteFileW(path.c_str());
    WritePrivateProfileStringW(L"Video", L"CaptureResolution", L"2560x1440", path.c_str());
    auto first = LoadFromIni(path).settings;
    Check(first.displayMatchesCapture && EffectiveDisplayPreset(first) == first.capturePreset,
          "profile without display preference follows capture");
    if (!CheckSaved(path, first, "save new capture-matched default")) return;
    Check(LoadFromIni(path).settings.displayMatchesCapture,
          "new capture-matched default survives first save");
    WritePrivateProfileStringW(L"Video", L"DisplayMatchesCapture", L"0", path.c_str());
    WritePrivateProfileStringW(L"Video", L"Resolution", nullptr, path.c_str());
    Check(!LoadFromIni(path).settings.displayMatchesCapture,
          "explicit fixed preference wins even without a resolution key");
    const VideoPreset presets[]{VideoPreset::R1280x720, VideoPreset::R1920x1080,
                                VideoPreset::R2560x1440, VideoPreset::R3840x2160};
    unsigned caseIndex = 0;
    for (const auto display : presets) for (const auto capture : presets) {
        AppSettings saved{};
        saved.videoPreset = display;
        saved.capturePreset = capture;
        saved.fullscreenVideoPosition = FullscreenPositionFromIndex(
            static_cast<int>(caseIndex % kFullscreenVideoPositionCount));
        saved.vsrEnabled = (caseIndex & 1) != 0;
        // Save enabled then disabled to exercise replacement, not just new keys.
        for (const bool matches : {true, false}) {
            saved.displayMatchesCapture = matches;
            if (!CheckSaved(path, saved, "save capture-matched display preference")) return;
            const auto loaded = LoadFromIni(path).settings;
            Check(loaded.displayMatchesCapture == matches &&
                      loaded.videoPreset == display && loaded.capturePreset == capture &&
                      loaded.fullscreenVideoPosition == saved.fullscreenVideoPosition &&
                      loaded.vsrEnabled == saved.vsrEnabled,
                  "matching capture preserves fixed display, capture, position and VSR on save/load");
            Check(EffectiveDisplayPreset(loaded) == (matches ? capture : display),
                  "effective display resolves capture only when explicitly enabled");
            auto changedCapture = loaded;
            changedCapture.capturePreset = presets[(caseIndex + 1) % ARRAYSIZE(presets)];
            Check(EffectiveDisplayPreset(changedCapture) ==
                      (matches ? changedCapture.capturePreset : display) &&
                      changedCapture.videoPreset == display,
                  "capture edits follow automatically without overwriting fixed display selection");
            changedCapture.displayMatchesCapture = false;
            Check(EffectiveDisplayPreset(changedCapture) == display,
                  "leaving capture matching restores the preserved fixed display selection");
        }
        ++caseIndex;
    }
    for (const wchar_t* value : {L"invalid", L"1x"}) {
        WritePrivateProfileStringW(L"Video", L"DisplayMatchesCapture", value, path.c_str());
        const auto loaded = LoadFromIni(path).settings;
        Check(!loaded.displayMatchesCapture && EffectiveDisplayPreset(loaded) == loaded.videoPreset,
              "malformed explicit capture matching keeps fixed display selection");
    }
    const wchar_t* sizes[]{L"1280x720", L"1920x1080", L"2560x1440", L"3840x2160"};
    for (const auto* display : sizes) for (const auto* capture : sizes) {
        WritePrivateProfileStringW(L"Video", L"Resolution", display, path.c_str());
        WritePrivateProfileStringW(L"Video", L"CaptureResolution", capture, path.c_str());
        WritePrivateProfileStringW(L"Video", L"DisplayMatchesCapture", nullptr, path.c_str());
        const auto migrated = LoadFromIni(path).settings;
        const bool same = migrated.capturePreset == migrated.videoPreset;
        Check(migrated.displayMatchesCapture == same &&
              EffectiveDisplayPreset(migrated) == migrated.videoPreset,
              "legacy migration preserves appearance and follows only equal resolutions");
        if (!CheckSaved(path, migrated, "save migrated display mode")) return;
        Check(LoadFromIni(path).settings.displayMatchesCapture == same,
              "migration persists its resolved mode");
        WritePrivateProfileStringW(L"Video", L"DisplayMatchesCapture", L"0", path.c_str());
        Check(!LoadFromIni(path).settings.displayMatchesCapture,
              "explicit fixed selection remains fixed even when sizes match");
    }
}

void TestCaptureFrameRatePersistence(const std::wstring& path) {
    using namespace llcv::settings;
    // Selection and the 40 fps override are separate preferences. Applying or
    // removing the override must not destroy the rate restored afterward.
    for (const auto format : {VideoPixelFormat::Auto, VideoPixelFormat::Nv12,
                              VideoPixelFormat::Yuy2, VideoPixelFormat::Mjpeg,
                              VideoPixelFormat::P010, VideoPixelFormat::Uyvy, VideoPixelFormat::Rgb24, VideoPixelFormat::Rgb32}) {
        for (int fps : {0, 30, 60, 120}) {
            AppSettings saved{};
            saved.capturePreset = VideoPreset::R2560x1440;
            saved.videoPreset = VideoPreset::R1920x1080;
            saved.pixelFormat = format;
            saved.videoFrameRate = fps;
            saved.allowUnlistedFps = fps == 30;
            for (const bool capture40 : {true, false}) {
                saved.capture40Fps = capture40;
                if (!CheckSaved(path, saved, "save FPS and override fixture")) return;
                const auto loaded = LoadFromIni(path).settings;
                Check(loaded.videoFrameRate == fps && loaded.pixelFormat == format &&
                      loaded.capture40Fps == capture40 &&
                      loaded.allowUnlistedFps == saved.allowUnlistedFps &&
                      loaded.capturePreset == saved.capturePreset &&
                      loaded.videoPreset == saved.videoPreset,
                      "all capture formats preserve FPS independently of 40 fps and display size");
            }
        }
    }

    AppSettings saved{};
    saved.videoFrameRate = 60;
    saved.allowUnlistedFps = true;
    if (!CheckSaved(path, saved, "reset legacy FPS parser fixture")) return;
    const struct { const wchar_t* text; VideoPixelFormat expected; } formatCases[] = {
        {L"Auto", VideoPixelFormat::Auto}, {L"NV12", VideoPixelFormat::Nv12},
        {L"YUY2", VideoPixelFormat::Yuy2}, {L"MJPEG", VideoPixelFormat::Mjpeg},
        {L"P010", VideoPixelFormat::P010}, {L"UYVY", VideoPixelFormat::Uyvy},
        {L"RGB24", VideoPixelFormat::Rgb24}, {L"rgb32", VideoPixelFormat::Rgb32}, {L"XRGB", VideoPixelFormat::Rgb32},
        {L"uyvy", VideoPixelFormat::Uyvy}, {L"HDYC", VideoPixelFormat::Uyvy},
        {L"unsupported", VideoPixelFormat::Auto}, {nullptr, VideoPixelFormat::Auto},
    };
    for (const auto& item : formatCases) {
        if (!CheckWin32(WritePrivateProfileStringW(L"Video", L"PixelFormat", item.text,
                path.c_str()), "write existing or new capture format fixture")) return;
        const auto loaded = LoadFromIni(path).settings;
        Check(loaded.pixelFormat == item.expected && loaded.videoFrameRate == 60 &&
            loaded.allowUnlistedFps && loaded.capturePreset == saved.capturePreset &&
            loaded.videoPreset == saved.videoPreset,
            "UYVY support preserves legacy format names and unrelated capture settings");
    }
    saved.pixelFormat = VideoPixelFormat::Uyvy;
    if (!CheckSaved(path, saved, "save canonical UYVY format")) return;
    wchar_t formatText[32]{};
    GetPrivateProfileStringW(L"Video", L"PixelFormat", L"", formatText,
        ARRAYSIZE(formatText), path.c_str());
    Check(std::wstring(formatText) == L"UYVY",
        "UYVY and HDYC UI selection persist under one stable canonical format name");
    const struct { const wchar_t* text; int expected; } cases[] = {
        {L"30", 30}, {L"60", 60}, {L"120", 120}, {L"0", 0},
        {L" 30 ", 30}, {L"+60", 60},
        {L"30fps", 0}, {L"60.0", 0}, {L"invalid", 0}, {L"", 0},
        {L"-30", 0}, {L"2147483648", 0},
        {L"999999999999999999999999", 0},
        {nullptr, 0},
    };
    for (const auto& item : cases) {
        if (!CheckWin32(WritePrivateProfileStringW(
                L"Video", L"FrameRate", item.text, path.c_str()),
                "write legacy or damaged FPS fixture")) return;
        const auto loaded = LoadFromIni(path).settings;
        Check(loaded.videoFrameRate == item.expected && loaded.allowUnlistedFps &&
              !loaded.capture40Fps,
              "valid legacy FPS remains exact; invalid text does not affect other options");
    }
    // A previous malformed profile must not poison a subsequent UI selection.
    for (int fps : {30, 60}) {
        auto recovered = LoadFromIni(path).settings;
        recovered.videoFrameRate = fps;
        if (!CheckSaved(path, recovered, "replace invalid FPS with explicit selection")) return;
        wchar_t value[32]{};
        GetPrivateProfileStringW(L"Video", L"FrameRate", L"", value,
                                 ARRAYSIZE(value), path.c_str());
        Check(std::wstring(value) == std::to_wstring(fps) &&
              LoadFromIni(path).settings.videoFrameRate == fps,
              "an explicit FPS selection saves plain decimal and survives reopening");
    }
}

void TestRoundTrip(const std::wstring& path) {
    using namespace llcv::settings;
    AppSettings saved{};
    saved.consoleSurround51 = true;
    saved.vsrEnabled = true;
    saved.capturePreset = VideoPreset::R1280x720;
    saved.preferredDisplayMonitor = L"interface:monitor-test-id";
    saved.uiLanguage = UiLanguage::English;
    saved.settingsLightTheme = true;
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
        {L"busy-id", false, 0, true},
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
    saved.mjpegMatrixOverride = llcv::video_color::MatrixOverride::Bt709;
    saved.sdrRangeOverride = llcv::video_color::RangeOverride::Full;
    saved.pixelPerfect = false;
    saved.relativeWindowSize = true;
    saved.relativeWindowScalePpm = 666'667;
    saved.borderlessWindow = true;
    saved.roundedCorners = false;
    saved.windowSnap = false;
    saved.saveLog = true;
    saved.screenshotClipboard = true;
    saved.capture40Fps = true;
    saved.allowUnlistedFps = true;
    saved.showDiagnosticConsole = true;
    saved.skipStartupSettings = true;
    saved.checkForUpdates = false;
    saved.preventSleep = false;
    saved.audioOnly = true;
    saved.audioOnlyWidth = 640;
    saved.audioOnlyHeight = 360;

    if (!CheckSaved(path, saved, "save complete round-trip fixture")) return;
    const LoadResult result = LoadFromIni(path);
    Check(result.settings.consoleSurround51, "surround preference round trip");
    const AppSettings& loaded = result.settings;
    Check(!loaded.roundedCorners, "rounded corners OFF persists");
    Check(loaded.audioOnlyWidth == 640 && loaded.audioOnlyHeight == 360,
          "audio-only size round trip independent from video scale");
    Check(loaded.hdrChromaLocation == saved.hdrChromaLocation, "HDR chroma round trip");
    Check(loaded.preferredDisplayMonitor == saved.preferredDisplayMonitor, "display monitor round trip");
    Check(loaded.uiLanguage == saved.uiLanguage, "language round trip");
    Check(loaded.settingsLightTheme, "light settings theme round trip");
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
    CheckExclusiveCacheRoundTrip(loaded);
    Check(loaded.asioDriverName == saved.asioDriverName,
          "ASIO driver round trip");
    Check(loaded.vsrEnabled == saved.vsrEnabled, "VSR preference saved/restored");
    Check(loaded.capturePreset == saved.capturePreset, "VSR capture resolution saved separately");
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
    Check(loaded.mjpegMatrixOverride == saved.mjpegMatrixOverride &&
          loaded.sdrRangeOverride == saved.sdrRangeOverride,
          "independent MJPEG matrix and SDR range round trip");
    Check(loaded.relativeWindowScalePpm == saved.relativeWindowScalePpm,
          "relative scale round trip");
    Check(result.relativeWindowScaleVersion == kRelativeWindowScaleVersion,
          "relative scale version round trip");
    Check(loaded.saveLog && loaded.showDiagnosticConsole,
          "diagnostics round trip");
    Check(loaded.screenshotClipboard, "screenshot clipboard round trip");
    Check(loaded.capture40Fps, "40fps opt-in round trip");
    Check(loaded.allowUnlistedFps, "unlisted FPS opt-in round trip");
    Check(loaded.skipStartupSettings && !loaded.checkForUpdates &&
              loaded.audioOnly && !loaded.preventSleep,
          "general settings round trip");
}

void TestPresentationModes(const std::wstring& path) {
    using namespace llcv::settings;
    for (const auto mode : {PresentationMode::AllowTearing,
                            PresentationMode::VSync,
                            PresentationMode::Compatibility,
                            PresentationMode::ExclusiveFullscreen}) {
        AppSettings settings{};
        settings.presentationMode = mode;
        if (!CheckSaved(path, settings, "save presentation-mode fixture")) return;
        Check(LoadFromIni(path).settings.presentationMode == mode,
              "all presentation modes round trip");
        Check(LoadFromIni(path).settings.preventSleep, "enabled sleep preference round trips");
    }
    WritePrivateProfileStringW(L"Video", L"Presentation", L"exclusivefullscreen", path.c_str());
    Check(LoadFromIni(path).settings.presentationMode == PresentationMode::ExclusiveFullscreen,
          "exclusive mode name remains case insensitive");
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
        if (!CheckSaved(path, saved, "save PCM default fixture")) return;
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
        if (!CheckSaved(path, saved, "save legacy PCM migration fixture")) return;
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
    if (!CheckSaved(path, chosen, "save user-selected PCM value")) return;
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

void TestSdrColorSettings(const std::wstring& path) {
    using namespace llcv;
    for (auto matrix : {video_color::MatrixOverride::Auto, video_color::MatrixOverride::Bt709,
                        video_color::MatrixOverride::Bt601})
    for (auto range : {video_color::RangeOverride::Auto, video_color::RangeOverride::Limited,
                       video_color::RangeOverride::Full}) {
        settings::AppSettings saved{};
        saved.mjpegMatrixOverride = matrix;
        saved.sdrRangeOverride = range;
        saved.forceHdr10 = true;
        saved.hdrChromaLocation = hdr::ChromaLocation::Left;
        if (!CheckSaved(path, saved, "save SDR matrix/range combination")) return;
        const auto loaded = settings::LoadFromIni(path).settings;
        Check(loaded.mjpegMatrixOverride == matrix && loaded.sdrRangeOverride == range &&
              loaded.legacyMjpegRangeOverride == video_color::RangeOverride::Auto,
              "all independent SDR color choices survive save/load");
        Check(loaded.forceHdr10 && loaded.hdrChromaLocation == hdr::ChromaLocation::Left,
              "SDR settings do not overwrite HDR preferences");
    }
    for (const auto* matrix : {L"Bt709", L"Bt601"})
    for (const auto* range : {L"Limited", L"Full"}) {
        DeleteFileW(path.c_str());
        WritePrivateProfileStringW(L"Video", L"MjpegColor", (std::wstring(matrix) + range).c_str(), path.c_str());
        WritePrivateProfileStringW(L"Video", L"PixelFormat", L"NV12", path.c_str());
        auto migrated = settings::LoadFromIni(path).settings;
        const auto expectedMatrix = std::wcscmp(matrix, L"Bt709") == 0
            ? video_color::MatrixOverride::Bt709 : video_color::MatrixOverride::Bt601;
        const auto expectedRange = std::wcscmp(range, L"Limited") == 0
            ? video_color::RangeOverride::Limited : video_color::RangeOverride::Full;
        Check(migrated.mjpegMatrixOverride == expectedMatrix &&
              settings::EffectiveSdrRange(migrated, true) == expectedRange &&
              settings::EffectiveSdrRange(migrated, false) == video_color::RangeOverride::Auto,
              "legacy MJPEG correction never becomes a raw SDR override");
        if (!CheckSaved(path, migrated, "save untouched legacy MJPEG correction")) return;
        const auto preserved = settings::LoadFromIni(path).settings;
        Check(preserved.mjpegMatrixOverride == expectedMatrix &&
              settings::EffectiveSdrRange(preserved, true) == expectedRange &&
              settings::EffectiveSdrRange(preserved, false) == video_color::RangeOverride::Auto,
              "unrelated save keeps legacy MJPEG appearance and raw automatic range");
        wchar_t obsolete[64]{};
        GetPrivateProfileStringW(L"Video", L"MjpegColor", L"missing", obsolete, ARRAYSIZE(obsolete), path.c_str());
        Check(std::wcscmp(obsolete, L"missing") == 0, "save retires duplicate combined MJPEG setting");
        migrated.sdrRangeOverride = video_color::RangeOverride::Full;
        if (!CheckSaved(path, migrated, "save explicit shared SDR range")) return;
        const auto common = settings::LoadFromIni(path).settings;
        Check(common.legacyMjpegRangeOverride == video_color::RangeOverride::Auto &&
              settings::EffectiveSdrRange(common, false) == video_color::RangeOverride::Full &&
              settings::EffectiveSdrRange(common, true) == video_color::RangeOverride::Full,
              "explicit common range supersedes legacy MJPEG-only range");
    }
    for (const auto* value : {L"garbage", L"3", L"-1", L"Bt709Full", L""}) {
        WritePrivateProfileStringW(L"Video", L"MjpegMatrix", value, path.c_str());
        WritePrivateProfileStringW(L"Video", L"SdrRange", value, path.c_str());
        WritePrivateProfileStringW(L"Video", L"LegacyMjpegRange", value, path.c_str());
        WritePrivateProfileStringW(L"Video", L"MjpegColor", L"Bt601Full", path.c_str());
        const auto invalid = settings::LoadFromIni(path).settings;
        Check(invalid.mjpegMatrixOverride == video_color::MatrixOverride::Auto &&
              invalid.sdrRangeOverride == video_color::RangeOverride::Auto &&
              invalid.legacyMjpegRangeOverride == video_color::RangeOverride::Auto,
              "malformed new values safely select Auto without resurrecting legacy keys");
    }
    WritePrivateProfileStringW(L"Video", L"MjpegMatrix", L"bt601", path.c_str());
    WritePrivateProfileStringW(L"Video", L"SdrRange", L"full", path.c_str());
    const auto lowercase = settings::LoadFromIni(path).settings;
    Check(lowercase.mjpegMatrixOverride == video_color::MatrixOverride::Bt601 &&
          lowercase.sdrRangeOverride == video_color::RangeOverride::Full,
          "new color settings are case insensitive");
}

void TestHdrChroma(const std::wstring& path) {
    using namespace llcv;
    for (auto mode : {hdr::ChromaLocation::Auto, hdr::ChromaLocation::TopLeft, hdr::ChromaLocation::Left}) {
        settings::AppSettings saved{};
        saved.pixelFormat = settings::VideoPixelFormat::P010;
        saved.hdrChromaLocation = mode;
        if (!CheckSaved(path, saved, "save HDR placement fixture")) return;
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

void TestSaveFailures(const std::wstring& path) {
    using namespace llcv::settings;
    AppSettings original;
    original.volumePercent = 35;
    original.exclusiveEndpointCache = {{L"keep-verified", true, 10, false}};
    unsigned long error = 99;
    if (!CheckSaved(path, original, "checked save succeeds")) return;
    if (!CheckWin32(WritePrivateProfileStringW(L"Future", L"PreservedKey", L"keep", path.c_str()),
                    "prepare unknown setting")) return;
    auto changed = original;
    changed.volumePercent = 90;
    changed.exclusiveEndpointCache.clear();
    if (!CheckWin32(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY),
                    "make profile read-only")) return;
    Check(!SaveToIni(path, changed, &error) && error == ERROR_ACCESS_DENIED,
          "read-only save reports failure");
    Check(LoadFromIni(path).settings.volumePercent == 35 &&
          LoadFromIni(path).settings.exclusiveEndpointCache.size() == 1,
          "failed save preserves volume and verification cache");
    if (!CheckWin32(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL),
                    "restore writable temporary profile")) return;
    const HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!CheckWin32(held != INVALID_HANDLE_VALUE,
                    "hold original profile without delete sharing")) return;
    Check(!SaveToIni(path, changed, &error) && error != ERROR_SUCCESS,
          "locked destination reports atomic replacement failure");
    CloseHandle(held);
    Check(LoadFromIni(path).settings.volumePercent == 35,
          "replacement failure never partially updates original");
    WIN32_FIND_DATAW entry{};
    const HANDLE remaining = FindFirstFileW((path + L".tmp-*").c_str(), &entry);
    Check(remaining == INVALID_HANDLE_VALUE, "failed save cleans only its owned temporary file");
    if (remaining != INVALID_HANDLE_VALUE) FindClose(remaining);
    if (!CheckSaved(path, changed, "save recovers once file lock is released")) return;
    wchar_t unknown[32]{};
    GetPrivateProfileStringW(L"Future", L"PreservedKey", L"", unknown, 32, path.c_str());
    Check(std::wstring(unknown) == L"keep", "atomic save preserves unknown settings");
    Check(!SaveToIni(path + L"\\missing\\settings.ini", changed, &error) && error != 0,
          "missing directory is a visible save failure");
    Check(!SaveToIni(L"", changed, &error) && error == ERROR_INVALID_NAME,
          "empty profile path is rejected");
}

}  // namespace

int main(int argc, char** argv) {
    // CTest runs these in isolation with WILL_FAIL: exactly one recorded
    // assertion must produce exit 1. A crash still fails CTest, and any missing
    // or cascading assertion returns 0 so WILL_FAIL rejects that regression.
    if (argc == 2 && std::strcmp(argv[1], "--save-failure-probe") == 0) {
        TestRoundTrip(L""); // Deterministic ERROR_INVALID_NAME, no filesystem writes.
        return failures == 1 ? 1 : 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--empty-cache-probe") == 0) {
        CheckExclusiveCacheRoundTrip(llcv::settings::AppSettings{});
        return failures == 1 ? 1 : 0;
    }
    if (argc != 1) {
        std::fprintf(stderr, "Unknown SettingsStoreTests argument.\n");
        return 2;
    }
    const std::wstring path = TemporaryIniPath();
    if (path.empty()) {
        std::fprintf(stderr, "FAILED: could not create temporary INI path\n");
        return 1;
    }
    TestDefaults(path);
    TestNewUnicodeProfile(path);
    TestInvalidNumericSettings(path);
    TestResolutionRoundTrips(path);
    TestDisplayMatchesCapturePersistence(path);
    TestCaptureFrameRatePersistence(path);
    TestRoundTrip(path);
    for (const wchar_t* version : {L"1", L"0"}) {
        WritePrivateProfileStringW(L"ExclusiveEndpointCache", L"ProbeVersion", version, path.c_str());
        auto migrated = llcv::settings::LoadFromIni(path).settings;
        Check(migrated.exclusiveEndpointCache.empty() &&
              migrated.exclusiveVerifiedEndpointId.empty() && migrated.exclusiveVerifiedBufferMs == 0,
              "old probe and legacy verification invalidated together");
        migrated.exclusiveEndpointCache = {{L"checked-once", false, 0, true}};
        if (!CheckSaved(path, migrated, "save migrated endpoint cache")) continue;
        const auto reopened = llcv::settings::LoadFromIni(path).settings;
        Check(reopened.exclusiveEndpointCache.size() == 1 &&
              reopened.exclusiveEndpointCache.front().inconclusive,
              "new version preserves completed attempt across launches");
    }
    {
        auto settings = llcv::settings::LoadFromIni(path).settings;
        settings.roundedCorners = true;
        if (CheckSaved(path, settings, "save enabled rounded corners"))
            Check(llcv::settings::LoadFromIni(path).settings.roundedCorners, "rounded corners ON persists");
    }
    for (const wchar_t* value : {L"Dark", L"invalid", L"LIGHT"}) {
        WritePrivateProfileStringW(L"General", L"SettingsTheme", value, path.c_str());
        Check(llcv::settings::LoadFromIni(path).settings.settingsLightTheme ==
            (_wcsicmp(value, L"Light") == 0), "theme values are bounded and case insensitive");
    }
    WritePrivateProfileStringW(L"Window", L"AudioOnlyWidth", L"-1", path.c_str());
    WritePrivateProfileStringW(L"Window", L"AudioOnlyHeight", L"999999", path.c_str());
    const auto bounded = llcv::settings::LoadFromIni(path).settings;
    Check(bounded.audioOnlyWidth == 380 && bounded.audioOnlyHeight == 16384,
          "invalid audio-only dimensions are bounded");
    TestHdrChroma(path);
    TestSdrColorSettings(path);
    {
        auto settings=llcv::settings::LoadFromIni(path).settings;
        Check(settings.shortcuts==llcv::shortcuts::Defaults,"legacy profiles keep default shortcuts");
        settings.shortcuts[0]=llcv::shortcuts::Ctrl|'H';
        settings.shortcuts[4]=llcv::shortcuts::Alt|llcv::shortcuts::Shift|'V';
        settings.shortcuts[static_cast<size_t>(llcv::shortcuts::Action::Mute)]=llcv::shortcuts::Ctrl|'M';
        settings.shortcuts[static_cast<size_t>(llcv::shortcuts::Action::Exit)]=llcv::shortcuts::Ctrl|'Q';
        const auto custom=settings.shortcuts;
        if (CheckSaved(path,settings,"custom shortcuts saved"))
            Check(llcv::settings::LoadFromIni(path).settings.shortcuts==custom,"all modifier bits and actions round-trip");
        WritePrivateProfileStringW(L"Shortcuts",L"Exit",L"0",path.c_str());
        Check(llcv::settings::LoadFromIni(path).settings.shortcuts==llcv::shortcuts::Defaults,"empty exit recovers to usable defaults");
        CheckSaved(path,settings,"restore custom shortcuts");
        WritePrivateProfileStringW(L"Shortcuts",L"Help",L"113",path.c_str());
        Check(llcv::settings::LoadFromIni(path).settings.shortcuts==llcv::shortcuts::Defaults,"duplicate bindings recover to defaults");
        settings.shortcuts=llcv::shortcuts::Defaults;
        CheckSaved(path,settings,"restore default shortcuts");
        Check(llcv::settings::LoadFromIni(path).settings.shortcuts==llcv::shortcuts::Defaults,"default reset persists");
        WritePrivateProfileStringW(L"Shortcuts",L"Mute",nullptr,path.c_str());
        WritePrivateProfileStringW(L"Shortcuts",L"Help",L"77",path.c_str());
        const auto migrated=llcv::settings::LoadFromIni(path).settings.shortcuts;
        Check(migrated[0]=='M' && llcv::shortcuts::Valid(migrated) &&
              migrated[static_cast<size_t>(llcv::shortcuts::Action::Mute)]!='M',
              "adding mute preserves legacy M binding and chooses a free key");
        for (size_t i=1;i<static_cast<size_t>(llcv::shortcuts::Action::Mute);++i)
            Check(migrated[i]==llcv::shortcuts::Defaults[i],"mute migration preserves every other action");
        CheckSaved(path,settings,"restore defaults after mute migration");
    }
    {
        using namespace llcv::settings;
        AppSettings settings;
        Check(!settings.cmaaNisEnabled && !settings.vsrEnabled && settings.nisSharpnessPercent==15 &&
            SelectedUpscaler(settings)==UpscalingMethod::CmaaNis,"default OFF, first F6 selects CMAA/NIS 0.15");
        for(bool cmaa:{false,true}) for(bool vsr:{false,true}) for(int sharp:{0,15,100}) {
            settings.cmaaNisEnabled=cmaa;settings.vsrEnabled=vsr;settings.nisSharpnessPercent=sharp;
            CheckSaved(path,settings,"save upscaler");
            const auto loaded=LoadFromIni(path).settings;
            Check(loaded.cmaaNisEnabled==cmaa && loaded.vsrEnabled==(vsr&&!cmaa) &&
                loaded.nisSharpnessPercent==sharp,"upscaling modes exclusive and sharpness round trips");
        }
        for(const wchar_t* value:{L"-1",L"101",L"bad",L"15oops",L"999999999999"}) {
            WritePrivateProfileStringW(L"Video",L"NisSharpness",value,path.c_str());
            Check(LoadFromIni(path).settings.nisSharpnessPercent==15,"damaged sharpness defaults to 0.15");
        }
        for(const auto method:{UpscalingMethod::NvidiaVsr,UpscalingMethod::CmaaNis}) {
            settings.cmaaNisEnabled=false;settings.vsrEnabled=false;
            settings.preferredUpscaler=method;settings.nisSharpnessPercent=37;
            settings.shortcuts[static_cast<size_t>(llcv::shortcuts::Action::Vsr)]=llcv::shortcuts::Ctrl|'U';
            CheckSaved(path,settings,"save disabled upscaling preference");
            const auto disabled=LoadFromIni(path).settings;
            Check(!disabled.cmaaNisEnabled && !disabled.vsrEnabled &&
                SelectedUpscaler(disabled)==method && disabled.nisSharpnessPercent==37 &&
                disabled.shortcuts==settings.shortcuts,"OFF preserves method sharpness and existing key binding");
        }
        WritePrivateProfileStringW(L"Video",L"PreferredUpscaler",L"bad",path.c_str());
        Check(LoadFromIni(path).settings.preferredUpscaler==UpscalingMethod::CmaaNis,"invalid method uses CMAA/NIS default");
        WritePrivateProfileStringW(L"Video",L"PreferredUpscaler",nullptr,path.c_str());
        WritePrivateProfileStringW(L"Video",L"VsrEnabled",L"0",path.c_str());
        WritePrivateProfileStringW(L"Video",L"CmaaNisEnabled",L"0",path.c_str());
        Check(LoadFromIni(path).settings.preferredUpscaler==UpscalingMethod::CmaaNis,"legacy OFF without selection defaults to CMAA/NIS");
        WritePrivateProfileStringW(L"Video",L"CmaaNisEnabled",L"1",path.c_str());
        Check(LoadFromIni(path).settings.preferredUpscaler==UpscalingMethod::CmaaNis,"legacy active CMAA selection migrates");
        WritePrivateProfileStringW(L"Video",L"CmaaNisEnabled",nullptr,path.c_str());
        WritePrivateProfileStringW(L"Video",L"NisSharpness",nullptr,path.c_str());
        WritePrivateProfileStringW(L"Video",L"VsrEnabled",L"1",path.c_str());
        const auto old=LoadFromIni(path).settings;
        Check(old.vsrEnabled&&!old.cmaaNisEnabled&&old.nisSharpnessPercent==15 &&
            old.preferredUpscaler==UpscalingMethod::NvidiaVsr,"legacy VSR enabled preference preserved");
        auto oldVsrOff=old;
        oldVsrOff.vsrEnabled=false;
        CheckSaved(path,oldVsrOff,"save migrated VSR after toggling OFF");
        const auto migratedOff=LoadFromIni(path).settings;
        Check(!migratedOff.vsrEnabled && !migratedOff.cmaaNisEnabled &&
            SelectedUpscaler(migratedOff)==UpscalingMethod::NvidiaVsr,
            "legacy VSR selection survives OFF and restart");
    }
    {
        using namespace llcv::settings;
        AppSettings immediate;
        immediate.skipStartupSettings=true;
        immediate.vsrEnabled=true;
        immediate.nisSharpnessPercent=37;
        CheckSaved(path,immediate,"startup review fixture");
        WritePrivateProfileStringW(L"General",L"StartupSettingsVersion",nullptr,path.c_str());
        WritePrivateProfileStringW(L"Future",L"Unknown",L"preserve",path.c_str());
        Check(StartupReviewRequired(path,L"v2.1.6"),"legacy immediate-start profile requires one review");
        unsigned long error=0;
        Check(MarkStartupSettingsShown(path,L"v2.1.6",&error) && !error,"acknowledge displayed settings");
        Check(!StartupReviewRequired(path,L"v2.1.6"),"same-version next launch resumes immediate start");
        Check(StartupReviewRequired(path,L"v2.1.7"),"next update requests a new review");
        const auto afterReview=LoadFromIni(path).settings;
        Check(afterReview.skipStartupSettings && afterReview.vsrEnabled &&
            afterReview.nisSharpnessPercent==37,"review marker never changes startup or upscaler preferences");
        wchar_t unknown[32]{};
        GetPrivateProfileStringW(L"Future",L"Unknown",L"",unknown,32,path.c_str());
        Check(wcscmp(unknown,L"preserve")==0,"review preserves unknown settings on cancel");
        CheckSaved(path,afterReview,"normal save after review");
        Check(!StartupReviewRequired(path,L"v2.1.6"),"normal saves retain review marker");
        CheckWin32(SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_READONLY),"read-only review fixture");
        Check(!MarkStartupSettingsShown(path,L"v2.1.7",&error) && error==ERROR_ACCESS_DENIED,
            "failed marker save preserves old profile");
        Check(StartupReviewRequired(path,L"v2.1.7") && !StartupReviewRequired(path,L"v2.1.6"),
            "failed acknowledgement retries without consuming first launch");
        CheckWin32(SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL),"restore review fixture attributes");
        Check(!MarkStartupSettingsShown(path,L"",&error) && error==ERROR_INVALID_PARAMETER,
            "empty version cannot erase review marker");
        WritePrivateProfileStringW(L"General",L"StartupSettingsVersion",nullptr,path.c_str());
    }
    {
        using namespace llcv::settings;
        for(bool preserve:{false,true}) {
            AppSettings settings;settings.cmaaPreserve=preserve;settings.nisSharpnessPercent=37;
            for(bool enabled:{false,true}) {
                settings.cmaaNisEnabled=enabled;CheckSaved(path,settings,"save CMAA correction");
                const auto loaded=LoadFromIni(path).settings;
                Check(loaded.cmaaPreserve==preserve && loaded.nisSharpnessPercent==37 &&
                    loaded.cmaaNisEnabled==enabled,"CMAA quality survives enabled/OFF save and restart");
            }
        }
        for(const wchar_t* invalid:{L"-1",L"2",L"invalid",static_cast<const wchar_t*>(nullptr)}) {
            WritePrivateProfileStringW(L"Video",L"CmaaPreserve",invalid,path.c_str());
            Check(!LoadFromIni(path).settings.cmaaPreserve,"legacy/malformed CMAA choice keeps HIGH default");
        }
    }
    TestPresentationModes(path);
    {
        using namespace llcv::settings;
        for (int i = 0; i < kFullscreenVideoPositionCount; ++i) {
            AppSettings settings;
            settings.fullscreenVideoPosition = FullscreenPositionFromIndex(i);
            if (CheckSaved(path, settings, "save fullscreen position"))
                Check(LoadFromIni(path).settings.fullscreenVideoPosition == settings.fullscreenVideoPosition,
                      "all fullscreen positions round-trip");
        }
        for (const wchar_t* value : {L"-1", L"9", L"99999", L"invalid", static_cast<const wchar_t*>(nullptr)}) {
            WritePrivateProfileStringW(L"Video", L"FullscreenVideoPosition", value, path.c_str());
            Check(LoadFromIni(path).settings.fullscreenVideoPosition == FullscreenVideoPosition::Center,
                  "missing or invalid fullscreen position defaults to center");
        }
    }
    TestPcmDefaultAndPreservation(path);
    TestLegacyPcmMigration(path);
    TestSaveFailures(path);
    TestDaclPreservation(path);
    DeleteFileW(path.c_str());
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
    std::puts("Settings store tests passed.");
    return 0;
}
