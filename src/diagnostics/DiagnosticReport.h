#pragma once
#include "diagnostics/Logger.h"
#include "settings/TestProfile.h"
#include "diagnostics/ReportDevicePrivacy.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <cwctype>
#include <stdexcept>
#include <vector>

namespace llcv::diagnostics {
struct ReportRequest {
    std::wstring directory; // Application data directory, never an arbitrary search root.
    std::wstring summary;
    std::vector<std::pair<std::wstring, std::wstring>> redactions;
    std::vector<std::wstring> deviceIds;
};
struct ReportResult {
    HRESULT status = E_FAIL;
    std::wstring path;
    unsigned logs = 0, settings = 0;
};
namespace report_detail {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct BackgroundMode {
    bool enabled = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
    ~BackgroundMode() {
        if (enabled)
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
};
inline std::wstring Decode(const std::string& bytes) {
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
        static_cast<unsigned char>(bytes[1]) == 0xfe) {
        std::wstring text((bytes.size() - 2) / 2, L'\0');
        memcpy(text.data(), bytes.data() + 2, text.size() * 2);
        return text;
    }
    const bool bom = bytes.starts_with("\xef\xbb\xbf");
    const char* data = bytes.data() + (bom ? 3 : 0);
    const int size = static_cast<int>(bytes.size() - (bom ? 3 : 0));
    const bool valid =
        !size || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, size, nullptr, 0) != 0;
    const int count = MultiByteToWideChar(CP_UTF8, 0, data, size, nullptr, 0);
    std::wstring result(count, L'\0');
    if (count)
        MultiByteToWideChar(CP_UTF8, 0, data, size, result.data(), count);
    if (!valid)
        result += L"\r\n[Invalid or incomplete UTF-8 sequences replaced]\r\n";
    return result;
}
inline std::string Utf8(const std::wstring& text) {
    const int size = static_cast<int>(text.size());
    const int count =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    if (count)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), size, result.data(), count, nullptr, nullptr);
    return result;
}
inline std::wstring Redact(std::wstring text, const ReportRequest& request) {
    // Longest prefixes first; case-insensitive and slash-insensitive Windows paths.
    auto rules = request.redactions;
    std::sort(rules.begin(), rules.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    auto normalized = text;
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    for (const auto& [from, to] : rules) {
        if (from.empty())
            continue;
        auto needle = from;
        std::replace(needle.begin(), needle.end(), L'/', L'\\');
        for (size_t offset = 0; offset + from.size() <= text.size();) {
            const int found =
                FindStringOrdinal(FIND_FROMSTART, normalized.data() + offset,
                                  static_cast<int>(normalized.size() - offset), needle.data(),
                                  static_cast<int>(needle.size()), TRUE);
            if (found < 0)
                break;
            offset += static_cast<size_t>(found);
            text.replace(offset, from.size(), to);
            auto replacement = to;
            std::replace(replacement.begin(), replacement.end(), L'/', L'\\');
            normalized.replace(offset, from.size(), replacement);
            offset += to.size();
        }
    }
    return text;
}
inline bool ReadText(const std::filesystem::path& path, size_t limit, std::wstring& text) {
    Handle file{CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER length{};
    if (!GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        !GetFileSizeEx(file.value, &length) || length.QuadPart < 0 ||
        static_cast<uint64_t>(length.QuadPart) > limit)
        return false;
    std::string bytes(static_cast<size_t>(length.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
        return false;
    bytes.resize(read);
    text = Decode(bytes);
    return true;
}
struct Entry {
    std::string name, bytes;
    uint32_t crc = 0, offset = 0;
};
inline uint32_t Crc(const std::string& data) {
    uint32_t value = ~0u;
    for (unsigned char byte : data) {
        value ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
    }
    return ~value;
}
inline void Word(std::string& out, uint32_t value, int size) {
    for (int i = 0; i < size; ++i)
        out.push_back(static_cast<char>(value >> (i * 8)));
}
// Bounded, dependency-free ZIP STORE. File names are fixed ASCII, data is UTF-8.
inline std::string Zip(std::vector<Entry>& files) {
    std::string out;
    size_t capacity = 22;
    for (const auto& f : files)
        capacity += 76 + f.name.size() * 2 + f.bytes.size();
    out.reserve(capacity);
    for (auto& f : files) {
        f.offset = static_cast<uint32_t>(out.size());
        f.crc = Crc(f.bytes);
        Word(out, 0x04034b50, 4);
        Word(out, 20, 2);
        Word(out, 0x800, 2);
        Word(out, 0, 2);
        Word(out, 0, 2);
        Word(out, 0x21, 2);
        Word(out, f.crc, 4);
        Word(out, static_cast<uint32_t>(f.bytes.size()), 4);
        Word(out, static_cast<uint32_t>(f.bytes.size()), 4);
        Word(out, static_cast<uint32_t>(f.name.size()), 2);
        Word(out, 0, 2);
        out += f.name;
        out += f.bytes;
    }
    const auto central = static_cast<uint32_t>(out.size());
    for (const auto& f : files) {
        Word(out, 0x02014b50, 4);
        Word(out, 20, 2);
        Word(out, 20, 2);
        Word(out, 0x800, 2);
        Word(out, 0, 2);
        Word(out, 0, 2);
        Word(out, 0x21, 2);
        Word(out, f.crc, 4);
        Word(out, static_cast<uint32_t>(f.bytes.size()), 4);
        Word(out, static_cast<uint32_t>(f.bytes.size()), 4);
        Word(out, static_cast<uint32_t>(f.name.size()), 2);
        Word(out, 0, 2);
        Word(out, 0, 2);
        Word(out, 0, 2);
        Word(out, 0, 2);
        Word(out, 0, 4);
        Word(out, f.offset, 4);
        out += f.name;
    }
    const auto size = static_cast<uint32_t>(out.size()) - central;
    Word(out, 0x06054b50, 4);
    Word(out, 0, 2);
    Word(out, 0, 2);
    Word(out, static_cast<uint32_t>(files.size()), 2);
    Word(out, static_cast<uint32_t>(files.size()), 2);
    Word(out, size, 4);
    Word(out, central, 4);
    Word(out, 0, 2);
    return out;
}
} // namespace report_detail

// Invoked only by an explicit user action on a background worker. Never probes
// devices, modifies settings, reads recordings, or makes a network request.
inline ReportResult CreateDiagnosticReport(const ReportRequest& request,
                                           const std::atomic<bool>& stop) noexcept {
    using namespace report_detail;
    namespace fs = std::filesystem;
    ReportResult result;
    BackgroundMode background; // Lower CPU/I/O priority during an explicit export.
    std::wstring temporary;
    try {
        if (stop) {
            result.status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return result;
        }
        if (request.directory.empty())
            return result;
        if (test_profile::Protected(request.directory)) {
            result.status = E_ACCESSDENIED;
            return result;
        }
        const fs::path root(request.directory);
        std::vector<Entry> files;
        ReportDevicePrivacy devicePrivacy(request.deviceIds);
        ReportRequest masking = request;
        masking.redactions.insert(masking.redactions.end(), devicePrivacy.Rules().begin(),
                                  devicePrivacy.Rules().end());
        std::wstring notes;
        SYSTEM_INFO system{};
        GetNativeSystemInfo(&system);
        OSVERSIONINFOW version{sizeof(version)};
        using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
        const auto getVersion = reinterpret_cast<GetVersion>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (getVersion && getVersion(&version) == 0)
            notes += L"Windows: " + std::to_wstring(version.dwMajorVersion) + L"." +
                     std::to_wstring(version.dwMinorVersion) + L" build " +
                     std::to_wstring(version.dwBuildNumber) + L"\r\n";
        notes += L"Process bits: " + std::to_wstring(sizeof(void*) * 8) +
                 L"\r\nLogical processors: " + std::to_wstring(system.dwNumberOfProcessors) +
                 L"\r\n";
        size_t payloadBytes = 0;
        const auto add = [&](std::string name, std::wstring text, bool metadata = false) {
            auto data = Utf8(devicePrivacy.Apply(Redact(std::move(text), masking)));
            if (!metadata && payloadBytes + data.size() > 11u * 1024 * 1024) {
                notes += L"Omitted: report size limit\r\n";
                return false;
            }
            if (!metadata)
                payloadBytes += data.size();
            files.push_back({std::move(name), std::move(data)});
            return true;
        };
        for (const auto* name : {L"settings.ini", L"recording-prototype.ini"}) {
            if (stop) {
                result.status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                return result;
            }
            std::wstring text;
            if (ReadText(root / name, 256 * 1024, text)) {
                if (add(name == std::wstring_view(L"settings.ini") ? "settings.ini"
                                                                   : "recording.ini",
                        std::move(text)))
                    ++result.settings;
            } else
                notes +=
                    std::wstring(name) + L": missing, inaccessible, too large or reparse point\r\n";
        }
        struct Log {
            fs::path path;
            fs::file_time_type modified;
        };
        std::vector<Log> logs;
        std::error_code error;
        const auto logRoot = root / L"logs";
        const auto logAttributes = GetFileAttributesW(logRoot.c_str());
        if (logAttributes != INVALID_FILE_ATTRIBUTES &&
            !(logAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            unsigned enumerated = 0;
            for (fs::directory_iterator it(logRoot, error), end; !error && it != end && !stop;
                 it.increment(error)) {
                if (++enumerated > 1024) {
                    notes += L"Log enumeration limit reached\r\n";
                    break;
                }
                if (!IsManagedLogFileName(it->path().filename().wstring()))
                    continue;
                auto time = it->last_write_time(error);
                if (!error)
                    logs.push_back({it->path(), time});
            }
        }
        std::sort(logs.begin(), logs.end(),
                  [](const auto& a, const auto& b) { return a.modified > b.modified; });
        for (const auto& log : logs) {
            if (stop || result.logs >= 5)
                break;
            std::wstring text;
            if (ReadText(log.path, 2 * 1024 * 1024 + 65536, text)) {
                if (add("logs/log-" + std::to_string(result.logs + 1) + ".txt", std::move(text)))
                    ++result.logs;
            } else
                notes += L"Log omitted: inaccessible, too large or reparse point\r\n";
        }
        // Reserve manifest/review instructions even if copied text hits its cap.
        add("report.txt",
            request.summary.substr(0, 65536) + L"\r\n\r\nSaved settings: " +
                std::to_wstring(result.settings) + L"\r\nRecent logs: " +
                std::to_wstring(result.logs) + L"\r\n" + notes,
            true);
        add("READ-ME.txt",
            L"LLCV diagnostic report / 진단자료\r\n\r\n"
            L"자동 전송되지 않습니다. 압축을 풀어 내용을 확인한 뒤 직접 첨부하세요.\r\n"
            L"장치 이름과 설정·로그가 포함됩니다. 확인된 장치 ID·일반적인 USB/오디오 고유 ID·사용자 프로필 경로는 가렸지만 다른 개인정보는 직접 확인하세요.\r\n"
            L"문제 재현 순서, 발생 시각, 캡처보드·드라이버 버전, HDMI 소스의 실제 해상도/FPS/HDR, 다른 앱 실행 여부를 함께 적어주세요.\r\n"
            L"로그가 없으면 설정의 로그 저장을 켜고 증상을 재현한 뒤 다시 만드세요.\r\n\r\n"
            L"Not uploaded. Known device IDs/common USB and endpoint instances are aliased. Review other information before sharing.\r\n"
            L"User-profile paths are masked; review other personal information yourself. No recordings/screenshots are included.\r\n"
            L"Include reproduction steps/time, capture-card and driver versions, source resolution/FPS/HDR and other capture apps.\r\n"
            L"If logs are missing, enable log saving, reproduce the issue, and create another report.\r\n",
            true);
        if (stop) {
            result.status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return result;
        }
        auto zip = Zip(files);
        if (stop) {
            result.status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return result;
        }
        const fs::path reports = root / L"reports";
        fs::create_directories(reports);
        if (GetFileAttributesW(reports.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            return result;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t stem[128]{};
        static std::atomic<unsigned> serial{0};
        swprintf_s(stem, L"LLCV-report-%04u%02u%02u-%02u%02u%02u-%lu-%u.zip", now.wYear, now.wMonth,
                   now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId(), ++serial);
        const auto destination = (reports / stem).wstring();
        if (stop) {
            result.status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return result;
        }
        temporary = destination + L".partial";
        {
            Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (file.value == INVALID_HANDLE_VALUE) {
                temporary.clear();
                result.status = HRESULT_FROM_WIN32(GetLastError());
                return result;
            }
            DWORD written = 0;
            if (!WriteFile(file.value, zip.data(), static_cast<DWORD>(zip.size()), &written,
                           nullptr) ||
                written != zip.size() || !FlushFileBuffers(file.value))
                throw std::runtime_error("report write failed");
        }
        if (stop)
            result.status = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        else if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            temporary.clear();
            result.path = destination;
            result.status = S_OK;
        } else
            result.status = HRESULT_FROM_WIN32(GetLastError());
    } catch (...) {
        result.status = E_FAIL;
    }
    if (!temporary.empty())
        DeleteFileW(temporary.c_str()); // Only this invocation's CREATE_NEW file.
    return result;
}
} // namespace llcv::diagnostics
