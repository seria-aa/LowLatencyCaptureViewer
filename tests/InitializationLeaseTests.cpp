#include "ui/InitializationLease.h"
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
using namespace std::chrono_literals;
using llcv::settings_ui::InitializationLease;
static void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
int main() {
    auto gate = std::make_shared<std::timed_mutex>();
    std::atomic<bool> running = true;
    {
        InitializationLease lease(gate, running);
        Check(lease.Status() == InitializationLease::Result::Acquired,
              "free gate starts immediately");
        lease.Release();
        Check(!lease.lock.owns_lock(), "explicit release");
    }
    running = false;
    {
        InitializationLease lease(gate, running);
        Check(lease.Status() == InitializationLease::Result::Cancelled && !lease.lock.owns_lock(),
              "pre-cancel never takes free gate");
    }
    running = true;
    std::promise<void> held, release;
    auto done = release.get_future();
    std::thread driver([&] {
        std::unique_lock lock(*gate);
        held.set_value();
        done.wait();
    });
    held.get_future().wait();
    const auto begin = std::chrono::steady_clock::now();
    InitializationLease timed(gate, running, 25ms);
    Check(timed.TimedOut() && !timed.lock.owns_lock(), "blocked driver has a startup deadline");
    Check(std::chrono::steady_clock::now() - begin >= 20ms &&
              std::chrono::steady_clock::now() - begin < 500ms,
          "bounded wait, not a silent indefinite stall");
    std::promise<void> entering;
    auto entered = entering.get_future();
    std::thread cancelled([&] {
        entering.set_value();
        InitializationLease lease(gate, running, 5s);
        Check(lease.Status() == InitializationLease::Result::Cancelled,
              "shutdown cancels pending startup");
    });
    entered.wait();
    running = false;
    cancelled.join();
    release.set_value();
    driver.join();
    running = true;
    {
        InitializationLease retry(gate, running);
        Check(retry.lock.owns_lock(), "explicit retry works once driver releases resources");
    }
    std::puts(
        "PASS: immediate start, pre-cancel, bounded timeout, shutdown cancellation and retry; no hardware/profile access.");
}
