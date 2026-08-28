#include "update/UpdateChecker.h"

#include <cstdio>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

}  // namespace

int main() {
    using llcv::update::IsNewerReleaseTag;

    Expect(IsNewerReleaseTag(L"v1.2.4", L"v1.2.3"),
           "patch release should be newer");
    Expect(IsNewerReleaseTag(L"1.3.0", L"v1.2.99"),
           "minor release should be newer");
    Expect(IsNewerReleaseTag(L"v2.0", L"v1.99.99"),
           "major release should be newer");
    Expect(!IsNewerReleaseTag(L"v1.2.3", L"v1.2.3"),
           "same release should not be newer");
    Expect(!IsNewerReleaseTag(L"v1.2.3.0", L"v1.2.3"),
           "trailing zero should compare equal");
    Expect(!IsNewerReleaseTag(L"v1.2.2", L"v1.2.3"),
           "older release should not be newer");

    if (failures == 0) {
        std::puts("UpdateCheckerTests passed");
    }
    return failures == 0 ? 0 : 1;
}
