#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace llcv::settings_ui {
struct ModeQueryKey {
    std::wstring device;
    int width = 0, height = 0;
    bool operator==(const ModeQueryKey&) const = default;
};

// Queries own their inputs, never the dialog or controls. An uninterruptible
// driver call may outlive Stop, but cannot publish to a closed/reused HWND.
// A shared gate serializes old/new sessions of the same result type.
template <class Result> class SettingsModeCache {
  public:
    using Query = std::function<Result(const ModeQueryKey&)>;
    using CancellableQuery = std::function<Result(const ModeQueryKey&, const std::atomic<bool>&)>;
    SettingsModeCache(Query query, std::function<void()> notify)
        : SettingsModeCache([query = std::move(query)](const ModeQueryKey& key,
                                                       const auto&) { return query(key); },
                            std::move(notify)) {}
    SettingsModeCache(CancellableQuery query, std::function<void()> notify)
        : state_(std::make_shared<State>(std::move(query), std::move(notify))),
          worker_([state = state_, gate = Gate()] { Run(state, gate); }) {}
    ~SettingsModeCache() { Stop(); }
    SettingsModeCache(const SettingsModeCache&) = delete;
    SettingsModeCache& operator=(const SettingsModeCache&) = delete;
    std::optional<Result> Request(const ModeQueryKey& key) {
        auto& s = *state_;
        std::lock_guard lock(s.mutex);
        if (s.stopping)
            return std::nullopt;
        for (const auto& entry : s.cache)
            if (entry.first == key) {
                s.pending.reset();
                return entry.second;
            }
        if (!s.active || s.active->key != key || s.active->epoch != s.epoch) {
            s.pending = Work{key, s.epoch};
            s.wake.notify_one();
        } else {
            s.pending.reset();
        }
        return std::nullopt;
    }
    void Invalidate() {
        std::lock_guard lock(state_->mutex);
        ++state_->epoch;
        state_->cache.clear();
        state_->pending.reset();
    }
    void InvalidateDevice(const std::wstring& device) {
        std::lock_guard lock(state_->mutex);
        ++state_->epoch;
        std::erase_if(state_->cache,
                      [&](const auto& entry) { return entry.first.device == device; });
        state_->pending.reset();
    }
    void Stop() {
        {
            std::lock_guard lock(state_->mutex);
            state_->stopping = true;
            state_->pending.reset();
            state_->notify = {}; // Serialized with delivery, including HWND reuse.
        }
        state_->wake.notify_one();
        // Shared state stays alive until the driver returns. No thread termination
        // or driver wait on the UI thread, including destruction after WM_DESTROY.
        if (worker_.joinable())
            worker_.detach();
    }

  private:
    struct Work {
        ModeQueryKey key;
        unsigned epoch;
    };
    struct State {
        State(CancellableQuery q, std::function<void()> n)
            : query(std::move(q)), notify(std::move(n)) {}
        CancellableQuery query;
        std::function<void()> notify;
        std::mutex mutex;
        std::condition_variable wake;
        std::atomic<bool> stopping{false};
        unsigned epoch = 0;
        std::optional<Work> pending, active;
        std::vector<std::pair<ModeQueryKey, Result>> cache;
    };
    static std::shared_ptr<std::timed_mutex> Gate() {
        static const auto gate = std::make_shared<std::timed_mutex>();
        return gate;
    }
    static void Run(const std::shared_ptr<State>& s,
                    const std::shared_ptr<std::timed_mutex>& gate) {
        for (;;) {
            Work work;
            {
                std::unique_lock lock(s->mutex);
                s->wake.wait(lock, [&] { return s->stopping || s->pending.has_value(); });
                if (s->stopping)
                    return;
                work = *s->pending;
                s->pending.reset();
                s->active = work;
            }
            std::unique_lock operation(*gate, std::defer_lock);
            while (!operation.try_lock_for(std::chrono::milliseconds(20))) {
                if (s->stopping)
                    return;
            }
            if (s->stopping)
                return;
            Result result = s->query(work.key, s->stopping);
            operation.unlock();
            std::lock_guard lock(s->mutex);
            s->active.reset();
            if (!s->stopping && work.epoch == s->epoch) {
                s->cache.emplace_back(work.key, std::move(result));
                // Notifications only post messages, never call Request or Stop.
                if (s->notify)
                    s->notify();
            }
        }
    }
    std::shared_ptr<State> state_;
    std::thread worker_;
};
} // namespace llcv::settings_ui
