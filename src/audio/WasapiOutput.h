#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

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
    HresultLogCallback logHresult = nullptr;
};

// Runs one event-driven WASAPI session on the calling thread. Returns true
// only when the Windows default render endpoint changed and the caller should
// immediately construct another session. Other exits are final for the
// current renderer invocation.
bool Run(const Configuration& configuration, const Host& host);

}  // namespace llcv::wasapi
