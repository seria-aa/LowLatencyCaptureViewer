#include "capture/LatestVideoSample.h"

#include <dvdmedia.h>

#include <chrono>
#include <cstdio>

namespace llcv::capture {
namespace {

template<class T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

}  // namespace

LatestVideoSample::LatestVideoSample(
    size_t expectedBytes, HANDLE readyEvent, VideoSampleTelemetry telemetry)
    : expectedBytes_(expectedBytes),
      readyEvent_(readyEvent),
      telemetry_(telemetry) {}

LatestVideoSample::~LatestVideoSample() {
    SafeRelease(latest_);
}

bool LatestVideoSample::TrackingActive() const {
    return telemetry_.trackingStartMilliseconds &&
        GetTickCount64() >= telemetry_.trackingStartMilliseconds->load(
            std::memory_order_acquire);
}

void LatestVideoSample::Push(IMediaSample* sample) {
    if (!sample || sample->GetActualDataLength() <= 0 ||
        (expectedBytes_ != 0 && sample->GetActualDataLength() <
                                  static_cast<long>(expectedBytes_))) {
        return;
    }
    const int64_t arrivalMicroseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    sample->AddRef();
    IMediaSample* replaced = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        replaced = latest_;
        latest_ = sample;
        latestArrivalMicroseconds_ = arrivalMicroseconds;
    }
    if (replaced) {
        replaced->Release();
        if (TrackingActive() && telemetry_.replacedFrames) {
            telemetry_.replacedFrames->fetch_add(
                1, std::memory_order_relaxed);
        }
    }
    if (TrackingActive() && telemetry_.capturedFrames) {
        telemetry_.capturedFrames->fetch_add(1, std::memory_order_relaxed);
    }
    SetEvent(readyEvent_);
}

IMediaSample* LatestVideoSample::TakeLatest(
    int64_t& arrivalMicroseconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    IMediaSample* sample = latest_;
    latest_ = nullptr;
    if (!sample) return nullptr;
    arrivalMicroseconds = latestArrivalMicroseconds_;
    return sample;
}

VideoSampleGrabberCallback::VideoSampleGrabberCallback(
    LatestVideoSample* sampleSlot)
    : sampleSlot_(sampleSlot) {}

STDMETHODIMP VideoSampleGrabberCallback::QueryInterface(
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

STDMETHODIMP_(ULONG) VideoSampleGrabberCallback::AddRef() {
    return ++references_;
}

STDMETHODIMP_(ULONG) VideoSampleGrabberCallback::Release() {
    const ULONG value = --references_;
    if (!value) delete this;
    return value;
}

STDMETHODIMP VideoSampleGrabberCallback::SampleCB(
    double, IMediaSample* sample) {
    if (!sample) return E_POINTER;
    if (!surfaceCapabilityProbed_.exchange(
            true, std::memory_order_acq_rel)) {
        IMediaSample2Config* surfaceConfig = nullptr;
        IUnknown* surface = nullptr;
        const HRESULT configResult = sample->QueryInterface(
            IID_PPV_ARGS(&surfaceConfig));
        const HRESULT surfaceResult = SUCCEEDED(configResult)
            ? surfaceConfig->GetSurface(&surface) : configResult;
        std::fwprintf(
            stderr,
            L"[video] DirectShow VRAM sample surface: %s "
            L"(interface 0x%08X, surface 0x%08X)\n",
            SUCCEEDED(surfaceResult) && surface ? L"available"
                                                : L"not available",
            static_cast<unsigned>(configResult),
            static_cast<unsigned>(surfaceResult));
        SafeRelease(surface);
        SafeRelease(surfaceConfig);
    }
    if (sampleSlot_) sampleSlot_->Push(sample);
    return S_OK;
}

STDMETHODIMP VideoSampleGrabberCallback::BufferCB(double, BYTE*, long) {
    return E_NOTIMPL;
}

}  // namespace llcv::capture
