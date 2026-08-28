#pragma once

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

// Queries the official GitHub latest-release endpoint and accepts installer
// assets only from this project's releases/download path.
bool FetchLatestRelease(const wchar_t* currentVersion, CheckResult& result);

}  // namespace llcv::update
