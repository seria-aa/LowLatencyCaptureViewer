#pragma once
#include "ScreenshotPixels.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace llcv::screenshot {

struct Result {
    HRESULT fileResult = E_PENDING;
    HRESULT clipboardResult = S_FALSE;
    bool clipboardRequested = false;
    bool toneMapped = false;
    std::wstring path;
    double copyMs = 0;
};

std::wstring DefaultDirectory();
// Tests can encode/read back exactly the same pixels without opening hardware
// or modifying the user's clipboard. Existing destination files are rejected.
HRESULT WritePng(const std::wstring& path, unsigned width, unsigned height,
                 std::span<const std::uint8_t> bgra, const std::atomic<bool>& cancel,
                 unsigned pauseMs = 2);
HRESULT PublishClipboard(std::span<const std::uint8_t> bgra, unsigned width,
                         unsigned height, const std::atomic<bool>& cancel);

class Service {
public:
    Service() = default;
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    // Start/Stop and Submit belong to the capture thread. Request/TakeResult
    // belong to the UI thread. Stop only after the final Submit has returned.
    bool Start(unsigned width, unsigned height, Format format, std::wstring directory, bool topDown = true) noexcept;
    void Stop() noexcept;
    // UI can signal cancellation immediately; joining stays on capture thread.
    void CancelPending() noexcept;
    bool Request(bool clipboard) noexcept;
    void Submit(const Description& desc, const std::uint8_t* data,
                std::size_t length, unsigned stride) noexcept;
    bool TakeResult(Result& result);
    bool ExpireRequest() noexcept; // UI timer: 3 s with no valid video frame
private:
    enum class State { Unavailable, Idle, Requested, Copying, Processing, Complete };
    std::atomic<State> state_{State::Unavailable};
    std::atomic<bool> cancel_{false};
    std::atomic<ULONGLONG> requestedAt_{0};
    std::thread thread_;
    HANDLE event_ = nullptr;
    std::vector<std::uint8_t> raw_;
    Description desc_{};
    std::wstring directory_;
    bool clipboard_ = false;
    double copyMs_ = 0;
    std::mutex resultMutex_;
    Result result_;
    void Run() noexcept;
};
} // namespace llcv::screenshot
