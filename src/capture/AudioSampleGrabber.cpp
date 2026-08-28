#include "capture/AudioSampleGrabber.h"

#include <algorithm>

namespace llcv::capture {

AudioSampleGrabberCallback::AudioSampleGrabberCallback(
    capture_audio::Format format, audio::PcmRing& ring,
    AudioSampleTelemetry telemetry, TrackingPredicate trackingPredicate,
    void* trackingContext) noexcept
    : format_(format),
      ring_(ring),
      telemetry_(telemetry),
      trackingPredicate_(trackingPredicate),
      trackingContext_(trackingContext) {}

STDMETHODIMP AudioSampleGrabberCallback::QueryInterface(
    REFIID id, void** object) {
    if (!object) return E_POINTER;
    if (id == IID_IUnknown || id == __uuidof(ISampleGrabberCB)) {
        *object = static_cast<ISampleGrabberCB*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) AudioSampleGrabberCallback::AddRef() {
    return ++references_;
}

STDMETHODIMP_(ULONG) AudioSampleGrabberCallback::Release() {
    const ULONG remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
}

STDMETHODIMP AudioSampleGrabberCallback::SampleCB(
    double, IMediaSample* sample) {
    if (!sample) return E_POINTER;

    BYTE* data = nullptr;
    const HRESULT result = sample->GetPointer(&data);
    if (FAILED(result) || !data) return result;

    const long bytes = sample->GetActualDataLength();
    if (bytes <= 0 || format_.blockAlign == 0 ||
        bytes % format_.blockAlign != 0) {
        return S_OK;
    }

    const size_t frames = static_cast<size_t>(bytes / format_.blockAlign);
    const uint64_t nowMs = GetTickCount64();
    const bool track = TrackingActive();
    if (track) {
        if (telemetry_.monitorStartMs) {
            uint64_t unsetStart = 0;
            telemetry_.monitorStartMs->compare_exchange_strong(
                unsetStart, nowMs, std::memory_order_release,
                std::memory_order_relaxed);
        }
        if (telemetry_.packetFrames) {
            telemetry_.packetFrames->store(
                static_cast<UINT32>((std::min)(
                    frames, static_cast<size_t>(UINT32_MAX))),
                std::memory_order_release);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (hasPreviousCallback_) {
        const int64_t intervalUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                now - previousCallback_).count();
        if (track) {
            if (telemetry_.intervalUs) {
                telemetry_.intervalUs->store(intervalUs,
                                             std::memory_order_release);
            }
            if (telemetry_.intervalTotalUs) {
                telemetry_.intervalTotalUs->fetch_add(
                    static_cast<uint64_t>((std::max)(int64_t{0}, intervalUs)),
                    std::memory_order_relaxed);
            }
        }
    }
    previousCallback_ = now;
    hasPreviousCallback_ = true;
    if (telemetry_.lastCallbackMs) {
        telemetry_.lastCallbackMs->store(nowMs, std::memory_order_release);
    }
    if (track) {
        if (telemetry_.callbackCount) {
            telemetry_.callbackCount->fetch_add(1, std::memory_order_relaxed);
        }
        if (telemetry_.capturedFrames) {
            telemetry_.capturedFrames->fetch_add(
                frames, std::memory_order_relaxed);
        }
    }

    if (format_.path == capture_audio::Path::Direct16BitStereo) {
        ring_.Push(reinterpret_cast<const int16_t*>(data), frames);
    } else {
        ring_.PushConverted(data, frames, format_);
    }
    return S_OK;
}

STDMETHODIMP AudioSampleGrabberCallback::BufferCB(double, BYTE*, long) {
    return E_NOTIMPL;
}

bool AudioSampleGrabberCallback::TrackingActive() const noexcept {
    return !trackingPredicate_ || trackingPredicate_(trackingContext_);
}

}  // namespace llcv::capture
