#pragma once

#include "update/UpdateChecker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace llcv::update {

// Owned by the UI thread. Completion messages carry no allocated payload:
// an unhandled notification can be discarded when its window closes without
// leaking the result. Start/TakeResult/CancelAndWait run on the owner thread;
// notify runs on the worker and must only enqueue an asynchronous notification.
class UpdateCheckTask {
public:
    using Fetch = std::function<bool(
        const wchar_t*, CheckResult&, const std::atomic<bool>*)>;

    UpdateCheckTask() : UpdateCheckTask(FetchLatestRelease) {}
    explicit UpdateCheckTask(Fetch fetch)
        : fetch_(std::move(fetch)) {}
    ~UpdateCheckTask() { CancelAndWait(); }

    bool IsRunning() const { return running_; }

    bool Start(std::wstring currentVersion, std::function<void()> notify,
               std::chrono::milliseconds delay = std::chrono::milliseconds{0}) {
        // A completed request remains pending until its notification is handled.
        // Otherwise a second request could consume the first request's message.
        if (running_) return false;
        CancelAndWait();
        stop_.store(false, std::memory_order_release);
        running_ = true;
        try {
            worker_ = std::thread(
                [this, version = std::move(currentVersion),
                 notify = std::move(notify), delay]() {
                    {
                        std::unique_lock<std::mutex> lock(mutex_);
                        if (wake_.wait_for(lock, delay, [this]() {
                                return stop_.load(std::memory_order_acquire);
                            })) return;
                    }
                    CheckResult result;
                    try {
                        if (!fetch_(version.c_str(), result, &stop_)) result = {};
                    } catch (...) {
                        result = {};
                    }
                    if (stop_.load(std::memory_order_acquire)) return;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        result_ = std::move(result);
                    }
                    if (notify) notify();
                });
        } catch (...) {
            running_ = false;
            stop_.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }

    std::optional<CheckResult> TakeResult() {
        std::optional<CheckResult> result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result.swap(result_);
        }
        if (!result) return std::nullopt;
        if (worker_.joinable()) worker_.join();
        running_ = false;
        return result;
    }

    void CancelAndWait() {
        {
            // Serialize with wait_for's predicate check to avoid a lost wakeup
            // if cancellation lands exactly as the startup delay begins.
            std::lock_guard<std::mutex> lock(mutex_);
            stop_.store(true, std::memory_order_release);
        }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        result_.reset();
        running_ = false;
    }

private:
    Fetch fetch_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<CheckResult> result_;
    bool running_ = false;
};

}  // namespace llcv::update
