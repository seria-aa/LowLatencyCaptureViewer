#pragma once

#include <atomic>
#include <string>

namespace llcv::update {

struct CheckResult {
    bool success = false;
    bool newer = false;
    std::wstring latestTag;
    std::wstring installerUrl;
};

// Compares dotted release tags such as v1.2.3 without depending on a network
// response. Exposed separately so release ordering stays unit-testable.
bool IsNewerReleaseTag(const std::wstring& latestTag,
                       const std::wstring& currentTag);

// Parses a complete successful API response without network access. A newer
// release need not have an installer; installerUrl stays empty in that case.
bool ParseLatestReleaseResponse(const std::string& json,
                                const wchar_t* currentVersion,
                                CheckResult& result);

// Queries the official GitHub latest-release endpoint and accepts installer
// assets only from this project's releases/download path.
// Cancellation is checked between bounded synchronous WinHTTP operations.
// The caller must keep stop alive until this function returns.
bool FetchLatestRelease(const wchar_t* currentVersion, CheckResult& result,
                        const std::atomic<bool>* stop = nullptr);

}  // namespace llcv::update
