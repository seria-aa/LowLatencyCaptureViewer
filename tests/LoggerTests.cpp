#include "diagnostics/Logger.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", message);
    std::abort();
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        wchar_t temporaryPath[MAX_PATH]{};
        const DWORD length = GetTempPathW(ARRAYSIZE(temporaryPath),
                                          temporaryPath);
        Require(length > 0 && length < ARRAYSIZE(temporaryPath),
                "temporary directory must be available");
        wchar_t name[MAX_PATH]{};
        Require(GetTempFileNameW(temporaryPath, L"LLC", 0, name) != 0,
                "temporary file name must be available");
        Require(DeleteFileW(name) != 0, "temporary file must be removable");
        Require(CreateDirectoryW(name, nullptr) != 0,
                "temporary directory must be creatable");
        path_ = name;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

std::vector<std::filesystem::path> ManagedFiles(
    const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() &&
            llcv::diagnostics::IsManagedLogFileName(
                entry.path().filename().wstring())) {
            result.push_back(entry.path());
        }
    }
    return result;
}

uint64_t TotalBytes(const std::vector<std::filesystem::path>& files) {
    uint64_t result = 0;
    for (const auto& file : files) {
        result += static_cast<uint64_t>(std::filesystem::file_size(file));
    }
    return result;
}

bool ContainsAscii(const std::filesystem::path& path, const char* text) {
    std::ifstream file(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>{file}, {}};
    return bytes.find(text) != std::string::npos;
}

void WriteFile(const std::filesystem::path& path, const char* text) {
    FILE* file = nullptr;
    Require(_wfopen_s(&file, path.c_str(), L"wb") == 0 && file,
            "test file must open");
    Require(std::fputs(text, file) >= 0, "test file must write");
    Require(std::fclose(file) == 0, "test file must close");
}

}  // namespace

int main() {
    using namespace llcv::diagnostics;
    Require(IsManagedLogFileName(L"LowLatencyCapture_20260816_120000.log"),
            "base session log name must be managed");
    Require(IsManagedLogFileName(
                L"LowLatencyCapture_20260816_120000_part02.log"),
            "rotated session log name must be managed");
    Require(!IsManagedLogFileName(L"smoke-test.log"),
            "smoke test log must not be pruned");
    Require(!IsManagedLogFileName(L"user-notes.log"),
            "user files must not be pruned");

    TemporaryDirectory temporary;
    LoggerLimits limits{};
    limits.maxPartBytes = 100;
    limits.maxFiles = 3;
    limits.maxTotalBytes = 200;
    limits.maxAgeDays = 7;
    Logger logger(limits);
    Require(logger.Open(true, temporary.path()), "logger must open");
    FILE* sink = nullptr;
    Require(tmpfile_s(&sink) == 0 && sink, "sink must open");
    for (int index = 0; index < 16; ++index) {
        Require(logger.Print(sink, L"record-%02d-한글-0123456789\n",
                             index) >= 0,
                "logger must continue console output while rotating");
    }
    logger.Close();
    std::fclose(sink);
    const auto rotated = ManagedFiles(temporary.path());
    Require(rotated.size() <= limits.maxFiles,
            "managed log count must stay bounded");
    Require(TotalBytes(rotated) <= limits.maxTotalBytes,
            "managed log bytes must stay bounded");
    for (const auto& file : rotated) {
        Require(std::filesystem::file_size(file) <= limits.maxPartBytes,
                "each log part must stay bounded");
    }

    const auto unrelated = std::filesystem::path(temporary.path()) /
        L"user-notes.log";
    WriteFile(unrelated, "do not remove");
    const auto expired = std::filesystem::path(temporary.path()) /
        L"LowLatencyCapture_20000101_000000.log";
    WriteFile(expired, "expired");
    std::error_code error;
    std::filesystem::last_write_time(
        expired, std::filesystem::file_time_type::clock::now() -
            std::chrono::hours(24 * 30), error);
    Require(!error, "test must age the managed log");
    Logger agePruner;
    Require(agePruner.Open(true, temporary.path()),
            "logger must open while pruning old logs");
    agePruner.Close();
    Require(!std::filesystem::exists(expired),
            "expired managed log must be removed");
    Require(std::filesystem::exists(unrelated),
            "unrelated user file must remain");

    Logger contentLogger;
    Require(contentLogger.Open(true, temporary.path()),
            "logger must reopen after pruning");
    FILE* contentSink = nullptr;
    Require(tmpfile_s(&contentSink) == 0 && contentSink,
            "content sink must open");
    Require(contentLogger.Print(contentSink, L"format-marker=%d\n", 42) >= 0,
            "logger must write a regular record");
    contentLogger.Close();
    std::fclose(contentSink);
    bool markerFound = false;
    for (const auto& file : ManagedFiles(temporary.path())) {
        markerFound = markerFound || ContainsAscii(file, "format-marker=42");
    }
    Require(markerFound, "saved UTF-8 log must retain the original message text");
    return 0;
}
