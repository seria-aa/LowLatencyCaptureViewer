#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace llcv::settings_ui {
// Startup only, never used from a frame/sample processing loop. The gate and
// in-flight driver's resources stay owned even when the caller times out.
class InitializationLease {
  public:
    enum class Result { Acquired, Cancelled, TimedOut };
    std::shared_ptr<std::timed_mutex> gate;
    std::unique_lock<std::timed_mutex> lock;
    InitializationLease(std::shared_ptr<std::timed_mutex> value, const std::atomic<bool>& running,
                        std::chrono::milliseconds budget = std::chrono::seconds(5))
        : gate(std::move(value)), lock(*gate, std::defer_lock) {
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + budget;
        while (running.load()) {
            if (lock.try_lock()) {
                if (!running.load()) {
                    lock.unlock();
                    return;
                }
                result_ = Result::Acquired;
                return;
            }
            const auto remaining = deadline - Clock::now();
            if (remaining <= Clock::duration::zero()) {
                result_ = Result::TimedOut;
                return;
            }
            if (lock.try_lock_for(std::min(remaining, std::chrono::duration_cast<Clock::duration>(
                                                          std::chrono::milliseconds(20))))) {
                if (!running.load()) {
                    lock.unlock();
                    return;
                }
                result_ = Result::Acquired;
                return;
            }
        }
    }
    Result Status() const { return result_; }
    bool TimedOut() const { return result_ == Result::TimedOut; }
    void Release() {
        if (lock.owns_lock())
            lock.unlock();
    }

  private:
    Result result_ = Result::Cancelled;
};
} // namespace llcv::settings_ui
