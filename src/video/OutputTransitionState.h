#pragma once

namespace llcv::video {

// UI-thread-only policy for coalescing synchronous size notifications during
// F11/F5 and interactive resize. No HWND, GPU resources, locks or allocations.
// A true result requests one output-generation increment from the caller.
class OutputTransitionState {
public:
    void ResetClientSize() noexcept { width_ = height_ = -1; }
    void Begin() noexcept { ++depth_; }

    [[nodiscard]] bool End(bool requestUpdate) noexcept {
        if (requestUpdate) pending_ = true;
        if (depth_ > 0) --depth_;
        return !manualResize_ && TakePendingUpdate();
    }

    [[nodiscard]] bool OnClientSize(int width, int height) noexcept {
        if (width <= 0 || height <= 0 ||
            (width_ == width && height_ == height)) return false;
        width_ = width;
        height_ = height;
        if (manualResize_ || depth_ > 0) {
            pending_ = true;
            return false;
        }
        return true;
    }

    // Clearing this flag does not flush: normalize the window geometry first,
    // then consume the pending request after WM_EXITSIZEMOVE.
    void SetManualResize(bool active) noexcept { manualResize_ = active; }
    [[nodiscard]] bool TakePendingUpdate() noexcept {
        if (depth_ != 0 || !pending_) return false;
        pending_ = false;
        return true;
    }

    [[nodiscard]] unsigned Depth() const noexcept { return depth_; }
    [[nodiscard]] bool Pending() const noexcept { return pending_; }
    [[nodiscard]] bool ManualResize() const noexcept { return manualResize_; }
    [[nodiscard]] int ClientWidth() const noexcept { return width_; }
    [[nodiscard]] int ClientHeight() const noexcept { return height_; }

private:
    unsigned depth_ = 0;
    bool pending_ = false;
    bool manualResize_ = false;
    int width_ = -1;
    int height_ = -1;
};

} // namespace llcv::video
