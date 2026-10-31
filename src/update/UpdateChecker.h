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

// Current-version notes on the fixed official repository. Diagnostic suffixes
// are ignored; invalid labels fall back to its releases list. No network query.
std::wstring ReleaseNotesUrl(const std::wstring& currentVersion);

// Parses a complete successful API response without network access. A newer
// release need not have an installer; installerUrl stays empty in that case.
bool ParseLatestReleaseResponse(const std::string& json,
                                const wchar_t* currentVersion,
                                CheckResult& result);

// Queries the official GitHub latest-release endpoint and accepts installer
// assets only from this project's releases/download path.
// Network operations are asynchronous. Cancellation closes the request after
// at most one short completion-wait poll, without waiting for a network timeout.
// Late WinHTTP callbacks own their isolated buffers until HANDLE_CLOSING.
// The caller must keep stop alive until this function returns.
bool FetchLatestRelease(const wchar_t* currentVersion, CheckResult& result,
                        const std::atomic<bool>* stop = nullptr);

}  // namespace llcv::update
