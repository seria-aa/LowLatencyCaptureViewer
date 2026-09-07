#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include "diagnostics/LogSink.h"

namespace llcv::wasapi {

enum class Mode {
    Shared,
    Exclusive,
};

struct Configuration {
    Mode mode = Mode::Shared;
    std::wstring endpointId;
    int bufferMilliseconds = 20;
    UINT32 sharedPeriodFrames = 0;
    bool reinitializingEndpoint = false;
    const wchar_t* correctionDescription = L"off";
};

struct FillResult {
    size_t writtenFrames = 0;
    size_t availableBeforeRender = 0;
    UINT32 queuedFrames = 0;
    UINT32 queueTargetFrames = 0;
    bool audioStarted = false;
    bool trackingActive = false;
    bool resamplerActive = false;
    int resamplePpm = 0;
};

using FillCallback = FillResult (*)(
    void* context, int16_t* output, size_t frames);
using EndpointCallback = void (*)(
    void* context, const std::wstring& name, bool followsDefault);
using BufferCallback = void (*)(void* context, UINT32 frames);
using BeforeStartCallback = void (*)(void* context);
using DeadlineCallback = void (*)(void* context, double overdueSeconds, bool duringFill);
using HresultLogCallback = void (*)(
    void* context, const wchar_t* operation, HRESULT result);

struct Host {
    void* context = nullptr;
    const std::atomic<bool>* running = nullptr;
    FillCallback fill = nullptr;
    EndpointCallback endpointChanged = nullptr;
    BufferCallback bufferChanged = nullptr;
    BufferCallback paddingChanged = nullptr;
    BeforeStartCallback beforeStart = nullptr;
    // Nonblocking notification; the UI persists diagnostics later.
    DeadlineCallback outputDeadlineSuspected = nullptr;
    HresultLogCallback logHresult = nullptr;
    diagnostics::LogSink log = nullptr;
};

enum class RunResult { Stopped, EndpointChanged, Retry, Failed };
// A session releases all COM resources before returning. The owner applies
// a bounded retry policy for Retry; the audio callback never reopens a device.
// Optional runtime evidence spans a successful Start through the last
// successful render iteration. Failed setup, failed API calls and cleanup
// cannot extend it; it remains zero if no render iteration succeeds.
RunResult Run(const Configuration& configuration, const Host& host,
              uint64_t* successfulRuntimeMilliseconds = nullptr);

}  // namespace llcv::wasapi
