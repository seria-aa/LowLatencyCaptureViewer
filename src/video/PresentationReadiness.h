#pragma once
#include <windows.h>

namespace llcv::presentation {
// Render-thread-owned DXGI handle and one admission credit. The credit survives
// a decoder producing no picture and ResizeBuffers on the same swap chain.
// Only a real Present consumes it; a new swap chain gets a new owned handle.
class Readiness {
  public:
    ~Readiness() { Reset(); }
    Readiness() = default;
    Readiness(const Readiness&) = delete;
    Readiness& operator=(const Readiness&) = delete;
    void Reset(HANDLE owned = nullptr) {
        if (handle_)
            CloseHandle(handle_);
        handle_ = owned;
        ready_ = false;
        bypassed_ = false;
    }
    void Bypass() { bypassed_ = true; } // Keep ownership until swap-chain reset.
    bool Enabled() const { return handle_ != nullptr && !bypassed_; }
    bool Ready() const { return !Enabled() || ready_; }
    HANDLE WaitHandle() const { return Ready() ? nullptr : handle_; }
    void Signaled() {
        if (handle_)
            ready_ = true;
    }
    void Presented() { ready_ = false; }
    HRESULT Poll() {
        if (Ready())
            return S_OK;
        const DWORD result = WaitForSingleObject(handle_, 0);
        if (result == WAIT_OBJECT_0) {
            ready_ = true;
            return S_OK;
        }
        return result == WAIT_TIMEOUT  ? S_FALSE
               : result == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError())
                                       : E_UNEXPECTED;
    }

  private:
    HANDLE handle_ = nullptr;
    bool ready_ = false;
    bool bypassed_ = false;
};
} // namespace llcv::presentation
