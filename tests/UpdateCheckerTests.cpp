#include "update/UpdateChecker.h"
#include "update/UpdateCheckTask.h"

#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

void TestReleaseResponses() {
    using namespace llcv::update;
    CheckResult result;
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

    if (failures == 0) {
        std::puts("UpdateCheckerTests passed");
    }
    return failures == 0 ? 0 : 1;
}
