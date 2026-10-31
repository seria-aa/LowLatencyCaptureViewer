#include "update/UpdateChecker.h"
#include "update/UpdateCheckTask.h"
#include "update/UpdateTransport.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

void TestReleaseResponses() {
    using namespace llcv::update;
    const std::wstring notes=L"https://github.com/seria-aa/LowLatencyCaptureViewer/releases";
    for(const auto* label:{L"v2.2.1",L"2.2.1",L"V2.2.1",L"v2.2.1-vsr-test",L"v2.2.1-audio-recovery-diagnostic"})
        Expect(ReleaseNotesUrl(label)==notes+L"/tag/v2.2.1","notes link follows current version without diagnostic suffixes");
    for(const auto* label:{L"",L"preview",L"v2..1",L"v2.2",L"v2.2.1/other",L"https://example.invalid",L"v2.2.1?redirect=other",L"v2.2.1.0"})
        Expect(ReleaseNotesUrl(label)==notes,"invalid notes labels stay on the official releases list");
    CheckResult result;
    Expect(ParseLatestReleaseResponse(
               R"({"tag_name":"v2.0.1","assets":[{"browser_download_url":"https://github.com/seria-aa/LowLatencyCaptureViewer/releases/download/v2.0.1/LowLatencyCaptureViewer_v2.0.1_Setup.exe"}]})",
               L"v2.0.0", result) && result.success && result.newer &&
               result.installerUrl == L"https://github.com/seria-aa/LowLatencyCaptureViewer/releases/download/v2.0.1/LowLatencyCaptureViewer_v2.0.1_Setup.exe",
           "v2.0.0 users discover the v2.0.1 installer");
    Expect(!IsNewerReleaseTag(L"v2.0.0", L"v2.0.1") &&
           !IsNewerReleaseTag(L"v2.0.1", L"v2.0.1") &&
           IsNewerReleaseTag(L"v2.0.2", L"v2.0.1"),
           "v2.0.1 rejects older/equal releases and accepts a later patch");
    Expect(ParseLatestReleaseResponse(
               R"({"tag_name":"v2.0.0","assets":[{"browser_download_url":"https://github.com/seria-aa/LowLatencyCaptureViewer/releases/download/v2.0.0/LowLatencyCaptureViewer_v2.0.0_Setup.exe"}]})",
               L"v1.3.1", result) && result.success && result.newer &&
               result.installerUrl == L"https://github.com/seria-aa/LowLatencyCaptureViewer/releases/download/v2.0.0/LowLatencyCaptureViewer_v2.0.0_Setup.exe",
           "v1.3.1 users discover the v2.0.0 installer across the major version boundary");
    Expect(!IsNewerReleaseTag(L"v1.3.1", L"v2.0.0") &&
           !IsNewerReleaseTag(L"v2.0.0", L"v2.0.0") &&
           IsNewerReleaseTag(L"v2.0.1", L"v2.0.0"),
           "v2.0.0 rejects older/equal versions and accepts a later patch");
    Expect(ParseLatestReleaseResponse(
               R"({"tag_name":"v1.3.0","assets":[{"browser_download_url":"https://github.com/seria-aa/LowLatencyCaptureViewer/releases/download/v1.3.0/LowLatencyCaptureViewer_v1.3.0_Setup.exe"}]})",
               L"v1.2.12", result) && result.success && result.newer &&
               result.installerUrl == L"https://github.com/seria-aa/LowLatencyCaptureViewer/releases/download/v1.3.0/LowLatencyCaptureViewer_v1.3.0_Setup.exe",
           "v1.2.12 users discover the v1.3.0 installer");
    Expect(!IsNewerReleaseTag(L"v1.2.12", L"v1.3.0") &&
           !IsNewerReleaseTag(L"v1.3.0", L"v1.3.0"),
           "v1.3.0 does not offer an older or equal release");
    Expect(ParseLatestReleaseResponse(
               R"({"tag_name":"v1.2.4","assets":[]})", L"v1.2.3", result) &&
               result.success && result.newer && result.installerUrl.empty(),
           "a newer release without an installer must not be called up to date");
    Expect(ParseLatestReleaseResponse(
               R"({"tag_name":"v1.2.4","assets":[{"browser_download_url":"https://example.com/v1.2.4_Setup.exe"},{"browser_download_url":"https://github.com/seria-aa/LowLatencyCaptureViewer/releases/download/v1.2.4/App_Setup.exe.sha256"},{"browser_download_url":"https://github.com/seria-aa/LowLatencyCaptureViewer/releases/download/v1.2.4/App_Setup.exe"}]})",
               L"v1.2.3", result) && result.newer &&
               result.installerUrl == L"https://github.com/seria-aa/LowLatencyCaptureViewer/releases/download/v1.2.4/App_Setup.exe",
           "only the exact official installer asset is selected");
    Expect(ParseLatestReleaseResponse(
               R"({"tag_name":"v1.2.3","assets":[]})", L"v1.2.3", result) &&
               result.success && !result.newer && result.installerUrl.empty(),
           "equal release clears a previous installer result");
    Expect(!ParseLatestReleaseResponse(R"({"message":"Not Found"})",
                                      L"v1.2.3", result) && !result.success,
           "API errors cannot be successful update checks");
    Expect(!ParseLatestReleaseResponse(R"({"tag_name":""})",
                                      L"v1.2.3", result) && !result.success,
           "empty tags fail the update check");
    std::atomic<bool> cancelled{true};
    Expect(!FetchLatestRelease(L"v1.2.3", result, &cancelled) && !result.success,
           "a pre-cancelled check returns without opening a network request");
}

void TestTaskLifecycle() {
    using namespace llcv::update;
    using namespace std::chrono_literals;
    int fetchCount = 0;
    UpdateCheckTask task([&fetchCount](const wchar_t*, CheckResult& result,
                                      const std::atomic<bool>*) {
        ++fetchCount;
        result.success = true;
        result.latestTag = L"v1.2.4";
        return true;
    });

    Expect(task.Start(L"v1.2.3", []() {}, 1h), "delayed check starts");
    const auto started = std::chrono::steady_clock::now();
    task.CancelAndWait();
    Expect(std::chrono::steady_clock::now() - started < 2s,
           "cancel interrupts the startup delay");
    Expect(fetchCount == 0 && !task.IsRunning() && !task.TakeResult(),
           "delay cancellation neither fetches nor leaves a stale result");

    std::promise<void> completed;
    auto completion = completed.get_future();
    Expect(task.Start(L"v1.2.3", [&completed]() { completed.set_value(); }),
           "a cancelled task can be restarted");
    Expect(completion.wait_for(2s) == std::future_status::ready,
           "completed request sends an asynchronous notification");
    Expect(!task.Start(L"v1.2.3", []() {}),
           "pending results cannot be overwritten before notification handling");
    const auto result = task.TakeResult();
    Expect(result && result->success && result->latestTag == L"v1.2.4" &&
               !task.IsRunning() && fetchCount == 1,
           "the owning UI takes the result and joins the completed worker");
    Expect(!task.TakeResult(), "duplicate notifications do not repeat results");

    std::promise<void> abandoned;
    auto abandonedCompletion = abandoned.get_future();
    Expect(task.Start(L"v1.2.3", [&abandoned]() { abandoned.set_value(); }),
           "another check starts after consuming a result");
    Expect(abandonedCompletion.wait_for(2s) == std::future_status::ready,
           "result is pending before simulated window closure");
    task.CancelAndWait();
    Expect(!task.TakeResult() && !task.IsRunning(),
           "closing before dispatch discards the owned result safely");
}

void TestInFlightCancellation() {
    using namespace llcv::update;
    using namespace std::chrono_literals;
    std::promise<void> entered;
    auto enteredFuture = entered.get_future();
    std::atomic<int> notifications{0};
    bool observedStop = false;
    UpdateCheckTask task([&](const wchar_t*, CheckResult& result,
                             const std::atomic<bool>* stop) {
        entered.set_value();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!stop->load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        observedStop = stop->load(std::memory_order_acquire);
        result.success = true;
        return true;
    });
    Expect(task.Start(L"v1.2.3", [&]() { ++notifications; }),
           "in-flight cancellation test starts");
    Expect(enteredFuture.wait_for(2s) == std::future_status::ready,
           "fake fetch has started");
    task.CancelAndWait();
    Expect(observedStop && notifications.load() == 0 && !task.TakeResult(),
           "cancelling an active fetch suppresses late completion and result");
}

void TestFetchException() {
    using namespace llcv::update;
    using namespace std::chrono_literals;
    std::promise<void> completed;
    auto completion = completed.get_future();
    UpdateCheckTask task([](const wchar_t*, CheckResult&,
                            const std::atomic<bool>*) -> bool {
        throw std::runtime_error("fake transport failure");
    });
    Expect(task.Start(L"v1.2.3", [&]() { completed.set_value(); }),
           "throwing fetch starts");
    Expect(completion.wait_for(2s) == std::future_status::ready,
           "fetch exceptions still notify the UI");
    const auto result = task.TakeResult();
    Expect(result && !result->success && !task.IsRunning(),
           "fetch exceptions become retryable failures");
}

#include "UpdateTransportTests.inl"

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

    TestReleaseResponses();
    TestTaskLifecycle();
    TestInFlightCancellation();
    TestFetchException();
    TestAsyncTransport();

    if (failures == 0) {
        std::puts("UpdateCheckerTests passed");
    }
    return failures == 0 ? 0 : 1;
}
