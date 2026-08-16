#include "diagnostics/Logger.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <io.h>
#include <vector>

namespace llcv::diagnostics {
namespace {

bool IsDigits(std::wstring_view value) noexcept {
    return !value.empty() && std::all_of(
        value.begin(), value.end(),
        [](wchar_t value) { return value >= L'0' && value <= L'9'; });
}

FILE* OpenExclusiveUtf8(const std::wstring& path) noexcept {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    const int descriptor = _open_osfhandle(
        reinterpret_cast<intptr_t>(handle), _O_BINARY | _O_WRONLY);
    if (descriptor < 0) {
        CloseHandle(handle);
        return nullptr;
    }
    FILE* file = _fdopen(descriptor, "wb");
    if (!file) _close(descriptor);
    return file;
}

}  // namespace

bool IsManagedLogFileName(std::wstring_view name) noexcept {
    constexpr std::wstring_view prefix = L"LowLatencyCapture_";
    if (!name.starts_with(prefix) || !name.ends_with(L".log")) return false;
    const std::wstring_view body = name.substr(
        prefix.size(), name.size() - prefix.size() - 4);
    if (body.size() == 15 && body[8] == L'_' &&
        IsDigits(body.substr(0, 8)) && IsDigits(body.substr(9, 6))) {
        return true;
    }
    constexpr std::wstring_view partMarker = L"_part";
    const size_t part = body.find(partMarker);
    return part == 15 && body.size() == 22 && body[8] == L'_' &&
        IsDigits(body.substr(0, 8)) && IsDigits(body.substr(9, 6)) &&
        IsDigits(body.substr(20, 2));
}

Logger::~Logger() {
    Close();
}

int Logger::Print(FILE* stream, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = PrintV(stream, format, args);
    va_end(args);
    return result;
}

int Logger::PrintV(FILE* stream, const wchar_t* format, va_list args) {
    if (!stream || !format) return -1;
    std::lock_guard<std::mutex> lock(mutex_);
    return PrintVLocked(stream, format, args);
}

int Logger::PrintLocked(FILE* stream, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = PrintVLocked(stream, format, args);
    va_end(args);
    return result;
}

int Logger::PrintVLocked(FILE* stream, const wchar_t* format, va_list args) {
    va_list copyForFile;
    va_copy(copyForFile, args);
    const int result = vfwprintf(stream, format, args);
    fflush(stream);

    if (!savedLogFile_ || savedLogFile_ == stream) {
        va_end(copyForFile);
        return result;
    }

    try {
        va_list measure;
        va_copy(measure, copyForFile);
        const int characterCount = _vscwprintf(format, measure);
        va_end(measure);
        if (characterCount < 0) {
            DisableSavedLogLocked();
            va_end(copyForFile);
            return result;
        }
        std::vector<wchar_t> text(static_cast<size_t>(characterCount) + 1);
        va_list render;
        va_copy(render, copyForFile);
        const int written = vswprintf_s(text.data(), text.size(), format,
                                        render);
        va_end(render);
        if (written < 0) {
            DisableSavedLogLocked();
            va_end(copyForFile);
            return result;
        }
        const int byteCount = WideCharToMultiByte(
            CP_UTF8, 0, text.data(), characterCount, nullptr, 0, nullptr,
            nullptr);
        if (byteCount <= 0) {
            DisableSavedLogLocked();
            va_end(copyForFile);
            return result;
        }
        std::vector<char> bytes(static_cast<size_t>(byteCount));
        if (WideCharToMultiByte(CP_UTF8, 0, text.data(), characterCount,
                                bytes.data(), byteCount, nullptr, nullptr) !=
            byteCount) {
            DisableSavedLogLocked();
            va_end(copyForFile);
            return result;
        }
        const uint64_t recordBytes = bytes.size();
        if (recordBytes > limits_.maxPartBytes ||
            recordBytes > limits_.maxTotalBytes) {
            // A pathological one-off record cannot fit in the configured
            // limits. Keep console output and the current saved session alive
            // instead of treating that record as a logging failure.
            va_end(copyForFile);
            return result;
        }
        const uint64_t currentPartBytes = CurrentPartBytesLocked();
        const bool needsNewPart =
            currentPartBytes > limits_.maxPartBytes - recordBytes;
        const bool exceedsTotal =
            managedTotalBytes_ > limits_.maxTotalBytes - recordBytes;
        if ((needsNewPart && !OpenNextPartLocked(recordBytes)) ||
            (!needsNewPart && exceedsTotal && !PruneLocked(recordBytes))) {
            DisableSavedLogLocked();
            va_end(copyForFile);
            return result;
        }
        if (fwrite(bytes.data(), 1, bytes.size(), savedLogFile_) !=
                bytes.size() ||
            fflush(savedLogFile_) != 0) {
            DisableSavedLogLocked();
        } else {
            managedTotalBytes_ += recordBytes;
        }
    } catch (...) {
        DisableSavedLogLocked();
    }
    va_end(copyForFile);
    return result;
}

bool Logger::Open(bool enabled, const std::wstring& directory) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled) {
        DisableSavedLogLocked();
        return true;
    }
    if (savedLogFile_) return true;
    if (directory.empty() || !CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    directory_ = directory;
    if (!PruneLocked(0, 1)) return false;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stem[80]{};
    swprintf_s(stem, L"LowLatencyCapture_%04u%02u%02u_%02u%02u%02u",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
               time.wSecond);
    sessionStem_ = stem;
    if (!OpenUniquePartLocked(1)) return false;
    PrintLocked(stderr, L"[log] saving diagnostics: %s\n",
                currentPath_.c_str());
    return savedLogFile_ != nullptr;
}

bool Logger::OpenNextPartLocked(uint64_t firstRecordBytes) {
    if (!savedLogFile_ || directory_.empty()) return false;
    DisableSavedLogLocked();
    if (!PruneLocked(firstRecordBytes, 1)) return false;
    return OpenUniquePartLocked(part_ + 1);
}

bool Logger::OpenUniquePartLocked(size_t firstPart) {
    for (size_t candidate = firstPart; candidate <= 99; ++candidate) {
        const std::wstring path = candidate == 1
            ? directory_ + L"\\" + sessionStem_ + L".log"
            : directory_ + L"\\" + sessionStem_ + L"_part" +
                  (candidate < 10 ? L"0" : L"") +
                  std::to_wstring(candidate) + L".log";
        FILE* file = OpenExclusiveUtf8(path);
        if (file) {
            savedLogFile_ = file;
            currentPath_ = path;
            part_ = candidate;
            ++managedFileCount_;
            return true;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return false;
}

uint64_t Logger::CurrentPartBytesLocked() const {
    if (!savedLogFile_) return 0;
    const __int64 position = _ftelli64(savedLogFile_);
    return position < 0 ? limits_.maxPartBytes
                         : static_cast<uint64_t>(position);
}

bool Logger::PruneLocked(uint64_t reservedBytes, size_t reservedFiles) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (directory_.empty()) return false;
    const DWORD directoryAttributes = GetFileAttributesW(directory_.c_str());
    if (directoryAttributes == INVALID_FILE_ATTRIBUTES ||
        (directoryAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;

    struct File {
        fs::path path;
        fs::file_time_type modified;
        uint64_t bytes;
    };
    std::vector<File> files;
    const auto cutoff = fs::file_time_type::clock::now() -
        std::chrono::hours(24 * limits_.maxAgeDays);
    fs::directory_iterator iterator(directory_, error);
    const fs::directory_iterator end;
    if (error) return false;
    while (iterator != end) {
        const fs::directory_entry entry = *iterator;
        const std::wstring name = entry.path().filename().wstring();
        if (IsManagedLogFileName(name)) {
            const DWORD attributes = GetFileAttributesW(entry.path().c_str());
            const fs::file_status status = entry.symlink_status(error);
            if (attributes == INVALID_FILE_ATTRIBUTES || error) return false;
            if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                status.type() == fs::file_type::regular) {
                const auto modified = entry.last_write_time(error);
                if (error) return false;
                if (modified < cutoff && entry.path().wstring() != currentPath_) {
                    if (!fs::remove(entry.path(), error) || error) return false;
                } else {
                    const uintmax_t size = entry.file_size(error);
                    if (error || size > UINT64_MAX) return false;
                    files.push_back({entry.path(), modified,
                                     static_cast<uint64_t>(size)});
                }
            }
        }
        iterator.increment(error);
        if (error) return false;
    }
    std::sort(files.begin(), files.end(), [](const File& left,
                                             const File& right) {
        return left.modified != right.modified
            ? left.modified < right.modified
            : left.path.native() < right.path.native();
    });
    uint64_t total = 0;
    for (const File& file : files) {
        if (file.bytes > UINT64_MAX - total) return false;
        total += file.bytes;
    }
    if (reservedBytes > UINT64_MAX - total) return false;
    while (!files.empty() &&
           (files.size() + reservedFiles > limits_.maxFiles ||
            total + reservedBytes > limits_.maxTotalBytes)) {
        const auto candidate = std::find_if(
            files.begin(), files.end(), [this](const File& file) {
                return file.path.wstring() != currentPath_;
            });
        if (candidate == files.end()) return false;
        if (!fs::remove(candidate->path, error) || error) return false;
        total -= candidate->bytes;
        files.erase(candidate);
    }
    if (total + reservedBytes > limits_.maxTotalBytes ||
        files.size() + reservedFiles > limits_.maxFiles) {
        return false;
    }
    managedTotalBytes_ = total;
    managedFileCount_ = files.size();
    return true;
}

void Logger::DisableSavedLogLocked() noexcept {
    if (savedLogFile_) fclose(savedLogFile_);
    savedLogFile_ = nullptr;
    currentPath_.clear();
}

void Logger::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    DisableSavedLogLocked();
}

}  // namespace llcv::diagnostics
