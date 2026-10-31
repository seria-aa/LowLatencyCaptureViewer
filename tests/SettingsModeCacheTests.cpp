#include "ui/SettingsModeCache.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>

using namespace std::chrono_literals;
using namespace llcv::settings_ui;
static void Check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
int main() {
    std::mutex mutex;
    std::condition_variable changed;
    bool allow = false;
    int calls = 0, completed = 0;
    const auto uiThread = std::this_thread::get_id();
    const ModeQueryKey a{L"card-a", 1280, 720}, b{L"card-a", 1920, 1080}, c{L"card-b", 3840, 2160};
    SettingsModeCache<int> cache(
        [&](const ModeQueryKey& key) {
            Check(std::this_thread::get_id() != uiThread, "driver query stays off UI thread");
            std::unique_lock lock(mutex);
            ++calls;
            changed.notify_all();
            changed.wait(lock, [&] { return allow; });
            return key.width;
        },
        [&] {
            std::lock_guard lock(mutex);
            ++completed;
            changed.notify_all();
        });
    auto wait = [&](auto predicate) {
        std::unique_lock lock(mutex);
        Check(changed.wait_for(lock, 2s, predicate), "worker completes within test deadline");
    };
    auto release = [&] {
        std::lock_guard lock(mutex);
        allow = true;
        changed.notify_all();
    };
    const auto begin = std::chrono::steady_clock::now();
    Check(!cache.Request(a), "first request is pending");
    Check(std::chrono::steady_clock::now() - begin < 500ms, "UI does not wait for driver");
    wait([&] { return calls == 1; });
    for (int i = 0; i < 100; ++i)
        Check(!cache.Request(a), "coalesce identical pending request");
    cache.Request(b);
    cache.Request(c);
    release();
    wait([&] { return completed == 2; });
    Check(calls == 2, "only latest pending resolution is queried");
    for (int i = 0; i < 100; ++i) {
        Check(cache.Request(a) == 1280, "first device cached");
        Check(cache.Request(c) == 3840, "second device cached");
    }
    Check(calls == 2, "cached option changes never rescan");
    cache.InvalidateDevice(a.device);
    Check(cache.Request(c) == 3840, "device refresh retains unrelated device cache");
    cache.Invalidate();
    Check(!cache.Request(c), "manual refresh invalidates cached result");
    wait([&] { return completed == 3; });
    Check(cache.Request(c) == 3840, "fresh result cached");
    {
        std::lock_guard lock(mutex);
        allow = false;
    }
    cache.Invalidate();
    cache.Request(a);
    wait([&] { return calls == 4; });
    cache.Invalidate();
    cache.Request(c);
    release();
    wait([&] { return completed == 4; });
    Check(calls == 5 && cache.Request(c) == 3840,
          "obsolete in-flight query cannot repopulate cache");
    Check(!cache.Request(a), "obsolete first result was discarded");
    wait([&] { return completed == 5; });
    cache.Stop();
    // A driver that ignores cancellation must not hold a closed dialog alive,
    // publish a late callback, or overlap the next dialog's query.
    struct Blocked {
        std::promise<void> entered, exited;
        std::promise<void> release;
        std::atomic<int> notifications{0};
    };
    auto blocked = std::make_shared<Blocked>();
    auto released = blocked->release.get_future().share();
    auto old = std::make_unique<SettingsModeCache<int>>(
        [blocked, released](const auto&) {
            blocked->entered.set_value();
            released.wait();
            blocked->exited.set_value();
            return 1;
        },
        [blocked] { ++blocked->notifications; });
    old->Request(a);
    Check(blocked->entered.get_future().wait_for(2s) == std::future_status::ready,
          "blocked driver entered");
    const auto closing = std::chrono::steady_clock::now();
    old.reset();
    Check(std::chrono::steady_clock::now() - closing < 100ms,
          "closing never joins an in-flight driver");
    auto nextCalls = std::make_shared<std::atomic<int>>(0);
    std::promise<void> nextDone;
    SettingsModeCache<int> reopened(
        [nextCalls](const auto&) {
            ++*nextCalls;
            return 2;
        },
        [&] { nextDone.set_value(); });
    reopened.Request(a);
    Check(nextDone.get_future().wait_for(60ms) == std::future_status::timeout && *nextCalls == 0,
          "new session does not overlap blocked old driver");
    blocked->release.set_value();
    Check(blocked->exited.get_future().wait_for(2s) == std::future_status::ready,
          "owned query survives cache destruction");
    for (int attempt = 0; attempt < 200 && !reopened.Request(a); ++attempt)
        std::this_thread::sleep_for(5ms);
    Check(reopened.Request(a) == 2 && *nextCalls == 1 && blocked->notifications == 0,
          "late results discarded and new session resumes");
    reopened.Stop();
    auto cancelled = std::make_shared<std::promise<void>>();
    auto cancellation = cancelled->get_future();
    auto entered = std::make_shared<std::promise<void>>();
    auto entry = entered->get_future();
    SettingsModeCache<long> cooperative(
        [cancelled, entered](const ModeQueryKey&, const std::atomic<bool>& stop) {
            entered->set_value();
            while (!stop)
                std::this_thread::yield();
            cancelled->set_value();
            return 0L;
        },
        [] {});
    cooperative.Request(a);
    Check(entry.wait_for(2s) == std::future_status::ready,
          "cooperative query entered before cancellation");
    cooperative.Stop();
    Check(cancellation.wait_for(2s) == std::future_status::ready,
          "cancellable query receives lifetime-safe stop token");
    Check(!cooperative.Request(a), "stopped cache cannot restart");
    std::puts(
        "PASS: coalescing, epochs, non-blocking close, owned lifetime, cancellation, no late notifications or overlapping sessions.");
}
