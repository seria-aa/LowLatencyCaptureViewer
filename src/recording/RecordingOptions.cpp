#include "recording/RecordingOptions.h"
#include "settings/TestProfile.h"
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <atomic>
#include <memory>
#include <cerrno>
#include <climits>
#include <cwctype>
#include <type_traits>
namespace llcv::recording {
namespace {
std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
        return {};
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(path, CoTaskMemFree);
    return std::wstring(path);
}
} // namespace
std::wstring DefaultFolder() {
    if (test_profile::Enabled())
        return test_profile::Directory() + L"\\recordings";
    auto folder = Known(FOLDERID_Videos);
    return folder.empty() ? folder : folder + L"\\LLCV";
}
std::wstring ProfilePath() {
    if (test_profile::Enabled())
        return test_profile::Directory() + L"\\recording-prototype.ini";
    auto folder = Known(FOLDERID_LocalAppData);
    return folder.empty() ? folder : folder + L"\\LowLatencyCaptureViewer\\recording-prototype.ini";
}
Options LoadOptions(const std::wstring& path) {
    Options o;
    if (path.empty() || test_profile::Protected(path))
        return o;
    const bool existing = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    const Options defaults;
    const auto number = [&](const wchar_t* key, auto member, int missing, int invalid) {
        wchar_t value[64]{};
        const auto length =
            GetPrivateProfileStringW(L"Recording", key, L"", value, 64, path.c_str());
        wchar_t* end = nullptr;
        errno = 0;
        const long parsed = wcstol(value, &end, 10);
        const bool digits = end != value;
        while (end && iswspace(*end))
            ++end;
        const bool numeric = length < 63 && digits && end && !*end && errno != ERANGE &&
                             parsed >= INT_MIN && parsed <= INT_MAX;
        using Value = std::remove_reference_t<decltype(o.*member)>;
        Options candidate = o;
        candidate.*member = static_cast<Value>(!length ? missing : numeric ? parsed : invalid);
        // Recover only this field, never discard unrelated accepted preferences.
        if (!Valid(candidate))
            candidate.*member = static_cast<Value>(invalid);
        o = std::move(candidate);
    };
    // Missing caps in older profiles meant native capture size/rate.
    number(L"Codec", &Options::codec, 0, 0);
    number(L"MaxFps", &Options::maxFps, existing ? 0 : defaults.maxFps, defaults.maxFps);
    number(L"MaxHeight", &Options::maxHeight, existing ? 0 : defaults.maxHeight,
           defaults.maxHeight);
    number(L"SplitMode", &Options::splitMode, 0, 0);
    number(L"SplitSeconds", &Options::splitSeconds, 1800, 1800);
    number(L"SplitMiB", &Options::splitMiB, 4096, 4096);
    number(L"VideoMbps", &Options::videoMbps, 20, 20);
    number(L"AudioKbps", &Options::audioKbps, 192, 192);
    number(L"AudioVolumePercent", &Options::audioVolumePercent, 100, 100);
    // Older prototype profiles used only a target bitrate; preserve that intent.
    number(L"RateControl", &Options::rateControl, existing ? 1 : 0, existing ? 1 : 0);
    number(L"Quality", &Options::quality, 1, 1);
    wchar_t text[32768]{};
    GetPrivateProfileStringW(L"Recording", L"Folder", L"", text, 32768, path.c_str());
    o.folder = text;
    if (!Valid(o))
        o.folder.clear();
    GetPrivateProfileStringW(L"Recording", L"Encoder", L"", text, 32768, path.c_str());
    o.encoder = text;
    if (!Valid(o))
        o.encoder.clear();
    return o;
}
bool SaveOptions(const Options& o, const std::wstring& path) {
    if (!Valid(o))
        return false;
    if (path.empty() || test_profile::Protected(path))
        return false;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    if (error)
        return false;
    // Keep the established recording profile path for existing users.
    // Recording preferences remain separate from playback settings.
    // UTF-16 preserves all Windows folder names; replace atomically after flush.
    std::wstring section = L"\xFEFF[Recording]\r\n";
    auto add = [&](const wchar_t* key, const std::wstring& value) {
        section += key;
        section += L"=";
        section += value;
        section += L"\r\n";
    };
    add(L"Codec", std::to_wstring(static_cast<int>(o.codec)));
    add(L"MaxFps", std::to_wstring(o.maxFps));
    add(L"MaxHeight", std::to_wstring(o.maxHeight));
    add(L"SplitMode", std::to_wstring(static_cast<int>(o.splitMode)));
    add(L"SplitSeconds", std::to_wstring(o.splitSeconds));
    add(L"SplitMiB", std::to_wstring(o.splitMiB));
    add(L"VideoMbps", std::to_wstring(o.videoMbps));
    add(L"AudioKbps", std::to_wstring(o.audioKbps));
    add(L"AudioVolumePercent", std::to_wstring(o.audioVolumePercent));
    add(L"RateControl", std::to_wstring(static_cast<int>(o.rateControl)));
    add(L"Quality", std::to_wstring(static_cast<int>(o.quality)));
    add(L"Folder", o.folder);
    add(L"Encoder", o.encoder);
    static std::atomic<unsigned> sequence = 0;
    const auto temporary = path + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                           std::to_wstring(++sequence) + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(section.size() * sizeof(wchar_t));
    bool ok = WriteFile(file, section.data(), bytes, &written, nullptr) && written == bytes &&
              FlushFileBuffers(file);
    CloseHandle(file);
    if (ok)
        ok = MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok)
        DeleteFileW(temporary.c_str()); // Only our CREATE_NEW temporary file.
    return ok;
}
} // namespace llcv::recording
