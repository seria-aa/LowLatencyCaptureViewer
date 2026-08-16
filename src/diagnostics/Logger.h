#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

namespace llcv::diagnostics {

struct LoggerLimits {
    uint64_t maxPartBytes = 2ull * 1024 * 1024;
    size_t maxFiles = 5;
    uint64_t maxTotalBytes = 10ull * 1024 * 1024;
    int maxAgeDays = 7;
};

bool IsManagedLogFileName(std::wstring_view name) noexcept;

// Console output always continues if optional saved logging cannot be opened,
// rotated, or pruned. This object is never called from a capture callback.
class Logger {
public:
    Logger() = default;
    explicit Logger(LoggerLimits limits) : limits_(limits) {}
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool Open(bool enabled, const std::wstring& directory);
    void Close();
    int Print(FILE* stream, const wchar_t* format, ...);
    int PrintV(FILE* stream, const wchar_t* format, va_list args);

private:
    int PrintVLocked(FILE* stream, const wchar_t* format, va_list args);
    int PrintLocked(FILE* stream, const wchar_t* format, ...);
    bool OpenNextPartLocked(uint64_t firstRecordBytes);
    bool OpenUniquePartLocked(size_t firstPart);
    bool PruneLocked(uint64_t reservedBytes = 0, size_t reservedFiles = 0);
    uint64_t CurrentPartBytesLocked() const;
    void DisableSavedLogLocked() noexcept;

    FILE* savedLogFile_ = nullptr;
    LoggerLimits limits_{};
    std::wstring directory_;
    std::wstring sessionStem_;
    std::wstring currentPath_;
    size_t part_ = 0;
    // Cached after open/rotation/pruning so ordinary diagnostic records do
    // not enumerate the log directory.
    uint64_t managedTotalBytes_ = 0;
    size_t managedFileCount_ = 0;
    std::mutex mutex_;
};

}  // namespace llcv::diagnostics
