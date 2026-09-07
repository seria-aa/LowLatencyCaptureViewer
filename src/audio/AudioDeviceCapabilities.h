#pragma once

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace llcv::audio_device {

constexpr int kSampleRate = 48'000;
constexpr int kChannels = 2;
constexpr int kBitsPerSample = 16;
constexpr std::array<int, 6> kExclusiveBufferOptionsMs{
    5, 10, 15, 20, 30, 40};

using LogCallback = void (*)(const wchar_t* message);

struct EndpointInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct SharedModeSupport {
    bool supported = false;
    HRESULT result = E_FAIL;
    UINT32 defaultFrames = 0;
    UINT32 fundamentalFrames = 0;
    UINT32 minimumFrames = 0;
    UINT32 maximumFrames = 0;
    double probeMilliseconds = 0.0;
};

struct ExclusiveProbe {
    bool compatible = false;
    HRESULT result = E_FAIL;
    UINT32 requestedFrames = 0;
    UINT32 actualBufferFrames = 0;
    UINT32 events = 0;
    uint64_t submittedFrames = 0;
    uint64_t testDurationMs = 0;
    double expectedPeriodMs = 0.0;
    double averageEventMs = 0.0;
    double maximumEventMs = 0.0;
    std::wstring summary;
};

WAVEFORMATEX PcmOutputFormat();
UINT32 ClosestSupportedSharedPeriod(
    UINT32 requestedFrames, const SharedModeSupport& support);
bool IsExclusiveLowLatencyBuffer(int bufferMs);

std::wstring EndpointFriendlyName(IMMDevice* device);
std::vector<EndpointInfo> EnumerateRenderEndpoints();
std::wstring ResolveActiveEndpointId(const std::wstring& configuredId);
HRESULT GetConfiguredEndpoint(
    IMMDeviceEnumerator* enumerator, const std::wstring& endpointId,
    IMMDevice** device);

SharedModeSupport QuerySharedModeSupport(IMMDevice* device);
SharedModeSupport ProbeSharedModeSupport(const std::wstring& endpointId);
ExclusiveProbe ProbeExclusiveBufferRecommendation(
    const std::wstring& endpointId, const std::atomic<bool>* cancel,
    LogCallback logCallback);

}  // namespace llcv::audio_device
