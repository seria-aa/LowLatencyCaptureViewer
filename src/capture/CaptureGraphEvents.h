#pragma once

#include <dshow.h>
#include <wrl/client.h>

namespace llcv::capture {

// The graph's event handle is borrowed, not closed by the caller. Waiting on it
// alongside the frame event avoids per-frame COM polling and device rescans.
class CaptureGraphEvents {
public:
    enum class WaitReason { Notification, Frame, Timeout, Wake, Completed, Error, PresentationReady };

    HRESULT Initialize(IGraphBuilder* graph) {
        Reset();
        if (!graph) return E_POINTER;
        Microsoft::WRL::ComPtr<IMediaEvent> events;
        const HRESULT hr = graph->QueryInterface(IID_PPV_ARGS(&events));
        return FAILED(hr) ? hr : Initialize(events.Get());
    }
    HRESULT Initialize(IMediaEvent* events) {
        Reset();
        if (!events) return E_POINTER;
        OAEVENT handle = 0;
        const HRESULT hr = events->GetEventHandle(&handle);
        if (FAILED(hr)) return hr;
        if (!handle) return E_UNEXPECTED;
        events_ = events;
        handle_ = reinterpret_cast<HANDLE>(handle);
        return S_OK;
    }
    void Reset() {
        handle_ = nullptr;
        events_.Reset();
        completionPending_ = completed_ = false;
        lastWaitReason_ = WaitReason::Notification;
    }

    WaitReason LastWaitReason() const { return lastWaitReason_; }
    bool Completed() const { return completed_; }

    // S_FALSE means a normal timeout; LastWaitReason distinguishes successful
    // frame/notification/wake/completion results. No-input timeouts alone are
    // not device-loss evidence. A completed graph stays completed until reset.
    HRESULT Wait(HANDLE frame, DWORD timeout, HANDLE wake = nullptr, HANDLE presentation = nullptr) {
        if (completed_) return FinishWait(S_OK, WaitReason::Completed);
        if (!handle_) return FailWait(E_UNEXPECTED);
        HANDLE handles[4]{handle_};
        DWORD handleCount = 1;
        const DWORD wakeIndex = wake ? handleCount++ : MAXDWORD;
        if (wake) handles[wakeIndex] = wake;
        const DWORD frameIndex = frame ? handleCount++ : MAXDWORD;
        if (frame) handles[frameIndex] = frame;
        const DWORD presentationIndex = presentation ? handleCount++ : MAXDWORD;
        if (presentation) handles[presentationIndex] = presentation;
        // Graph events win over lifecycle wakeups, which win over ready video.
        // With no wake handle this remains the original graph/frame wait.
        const DWORD status = WaitForMultipleObjects(handleCount, handles, FALSE, timeout);
        if (status == WAIT_TIMEOUT) return FinishWait(S_FALSE, WaitReason::Timeout);
        if (status == WAIT_FAILED) return FailWait(HRESULT_FROM_WIN32(GetLastError()));
        if (wake && status == WAIT_OBJECT_0 + wakeIndex) return FinishWait(S_OK, WaitReason::Wake);
        if (frame && status == WAIT_OBJECT_0 + frameIndex) return FinishWait(S_OK, WaitReason::Frame);
        if (presentation && status == WAIT_OBJECT_0 + presentationIndex)
            return FinishWait(S_OK, WaitReason::PresentationReady);
        if (status != WAIT_OBJECT_0) return FailWait(E_UNEXPECTED);
        // Bound a notification burst. The graph keeps its event signaled until
        // all queued events are consumed, so the next wait continues draining.
        HRESULT failure = S_OK;
        bool drained = false;
        for (unsigned count = 0; count < 64; ++count) {
            long code = 0;
            LONG_PTR first = 0, second = 0;
            const HRESULT hr = events_->GetEvent(&code, &first, &second, 0);
            if (hr == E_ABORT) { drained = true; break; } // no queued event
            if (FAILED(hr)) return FailWait(hr);
            if (code == EC_DEVICE_LOST) {
                // Even an available-again notification requires a new graph;
                // the old filter state cannot be reused after unplug/replug.
                failure = HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
            } else if ((code == EC_ERRORABORT || code == EC_STREAM_ERROR_STOPPED) &&
                       SUCCEEDED(failure)) {
                const HRESULT reported = static_cast<HRESULT>(first);
                failure = FAILED(reported) ? reported : E_FAIL;
            } else if (code == EC_COMPLETE) {
                const HRESULT reported = static_cast<HRESULT>(first);
                if (FAILED(reported) && SUCCEEDED(failure)) failure = reported;
                else if (SUCCEEDED(reported)) completionPending_ = true;
            }
            // Includes event-owned interface pointers (notably DEVICE_LOST).
            events_->FreeEventParams(code, first, second);
        }
        if (FAILED(failure)) return FailWait(failure);
        if (completionPending_) {
            // Do not report normal completion before an error queued beyond
            // this batch has been inspected. Checking the borrowed event at
            // the boundary avoids another COM call and never resets it.
            if (!drained) {
                const DWORD pending = WaitForSingleObject(handle_, 0);
                if (pending == WAIT_FAILED) return FailWait(HRESULT_FROM_WIN32(GetLastError()));
                if (pending != WAIT_OBJECT_0 && pending != WAIT_TIMEOUT) return FailWait(E_UNEXPECTED);
                drained = pending == WAIT_TIMEOUT;
            }
            if (drained) {
                completionPending_ = false;
                completed_ = true;
                return FinishWait(S_OK, WaitReason::Completed);
            }
        }
        return FinishWait(S_OK, WaitReason::Notification);
    }

private:
    HRESULT FinishWait(HRESULT result, WaitReason reason) {
        lastWaitReason_ = reason;
        return result;
    }
    HRESULT FailWait(HRESULT error) {
        completionPending_ = false;
        return FinishWait(error, WaitReason::Error);
    }
    Microsoft::WRL::ComPtr<IMediaEvent> events_;
    HANDLE handle_ = nullptr;
    bool completionPending_ = false;
    bool completed_ = false;
    WaitReason lastWaitReason_ = WaitReason::Notification;
};

} // namespace llcv::capture
