#include "audio/AudioDeviceCapabilities.h"

#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace llcv::audio_device {
namespace {

template<class T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

ExclusiveProbe ProbeExclusiveCompatibility(
    const std::wstring& endpointId, int requestedMs,
    const std::atomic<bool>* cancel) {
    ExclusiveProbe probe{};
    probe.requestedFrames = static_cast<UINT32>(
        std::max(1, requestedMs) * kSampleRate / 1000);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) hr = S_OK;
    if (FAILED(hr)) {
        probe.result = hr;
        probe.summary = L"COM 초기화 실패";
        return probe;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE eventHandle = nullptr;
    WAVEFORMATEX* closest = nullptr;
    bool started = false;
    uint64_t runBeganMs = 0;
    DWORD mmcssTaskIndex = 0;
    HANDLE mmcss =
        AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);

    do {
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(hr)) {
            hr = GetConfiguredEndpoint(enumerator, endpointId, &device);
        }
        if (SUCCEEDED(hr)) {
            hr = device->Activate(
                __uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                reinterpret_cast<void**>(&client));
        }
        if (FAILED(hr)) break;

        const WAVEFORMATEX format = PcmOutputFormat();
        hr = client->IsFormatSupported(
            AUDCLNT_SHAREMODE_EXCLUSIVE, &format, &closest);
        if (closest) {
            CoTaskMemFree(closest);
            closest = nullptr;
        }
        if (hr != S_OK) {
            if (SUCCEEDED(hr)) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
            break;
        }

        REFERENCE_TIME defaultPeriod = 0;
        REFERENCE_TIME minimumPeriod = 0;
        hr = client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
        if (FAILED(hr)) break;

        const REFERENCE_TIME requestedDuration =
            static_cast<REFERENCE_TIME>(std::max(1, requestedMs)) * 10'000;
        hr = client->Initialize(
            AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            requestedDuration, requestedDuration, &format, nullptr);
        if (FAILED(hr)) break;

        hr = client->GetBufferSize(&probe.actualBufferFrames);
        if (FAILED(hr)) break;
        probe.expectedPeriodMs =
            1000.0 * probe.actualBufferFrames / kSampleRate;

        eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        hr = client->SetEventHandle(eventHandle);
        if (FAILED(hr)) break;
        hr = client->GetService(IID_PPV_ARGS(&render));
        if (FAILED(hr)) break;

        BYTE* bytes = nullptr;
        hr = render->GetBuffer(probe.actualBufferFrames, &bytes);
        if (SUCCEEDED(hr)) {
            hr = render->ReleaseBuffer(
                probe.actualBufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        if (FAILED(hr)) break;
        hr = client->Start();
        if (FAILED(hr)) break;
        started = true;
        runBeganMs = GetTickCount64();

        constexpr uint64_t kProbeDurationMs = 5000;
        uint64_t previousEventMs = 0;
        double intervalTotalMs = 0.0;
        UINT32 intervalCount = 0;
        while (!cancel || !cancel->load(std::memory_order_acquire)) {
            const uint64_t now = GetTickCount64();
            if (now - runBeganMs >= kProbeDurationMs) break;
            const DWORD wait = WaitForSingleObject(eventHandle, 250);
            if (wait == WAIT_TIMEOUT) continue;
            if (wait != WAIT_OBJECT_0) {
                hr = HRESULT_FROM_WIN32(GetLastError());
                break;
            }

            const uint64_t eventNow = GetTickCount64();
            ++probe.events;
            if (previousEventMs) {
                const double interval =
                    static_cast<double>(eventNow - previousEventMs);
                intervalTotalMs += interval;
                ++intervalCount;
                probe.maximumEventMs =
                    std::max(probe.maximumEventMs, interval);
            }
            previousEventMs = eventNow;

            UINT32 padding = 0;
            hr = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;
            const UINT32 writable = probe.actualBufferFrames > padding
                ? probe.actualBufferFrames - padding : 0;
            if (writable) {
                bytes = nullptr;
                hr = render->GetBuffer(writable, &bytes);
                if (SUCCEEDED(hr)) {
                    hr = render->ReleaseBuffer(
                        writable, AUDCLNT_BUFFERFLAGS_SILENT);
                    if (SUCCEEDED(hr)) probe.submittedFrames += writable;
                }
                if (FAILED(hr)) break;
            }
        }
        if (runBeganMs) {
            probe.testDurationMs = GetTickCount64() - runBeganMs;
        }
        if (cancel && cancel->load(std::memory_order_acquire)) {
            hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            break;
        }
        if (SUCCEEDED(hr) && intervalCount) {
            probe.averageEventMs = intervalTotalMs / intervalCount;
        }
    } while (false);

    if (started) client->Stop();
    if (closest) CoTaskMemFree(closest);
    if (eventHandle) CloseHandle(eventHandle);
    SafeRelease(render);
    SafeRelease(client);
    SafeRelease(device);
    SafeRelease(enumerator);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (uninitialize) CoUninitialize();

    probe.result = hr;
    if (FAILED(hr)) {
        wchar_t message[128]{};
        swprintf_s(
            message, L"Exclusive 초기화/실행 실패 (0x%08X)",
            static_cast<unsigned>(hr));
        probe.summary = message;
        return probe;
    }

    const double requestedPeriodMs =
        1000.0 * probe.requestedFrames / kSampleRate;
    const double expectedEvents = 5000.0 / requestedPeriodMs;
    const double maximumAllowedInterval = requestedPeriodMs * 1.25 + 1.0;
    const bool bufferMatches =
        probe.actualBufferFrames <= probe.requestedFrames + 1;
    const bool eventsAreTimely =
        probe.events >= static_cast<UINT32>(
                            std::floor(expectedEvents * 0.90)) &&
        probe.maximumEventMs <= maximumAllowedInterval;
    const double expectedSubmittedFrames =
        kSampleRate * std::max(1.0, probe.testDurationMs / 1000.0);
    const bool outputSupplyMatchesClock =
        probe.submittedFrames >= static_cast<uint64_t>(
            std::floor(expectedSubmittedFrames * 0.98));
    probe.compatible =
        bufferMatches && eventsAreTimely && outputSupplyMatchesClock;

    wchar_t message[256]{};
    if (probe.compatible) {
        swprintf_s(
            message,
            L"통과 · 실제 %.2f ms · 이벤트 평균/최대 %.2f/%.2f ms",
            probe.expectedPeriodMs, probe.averageEventMs,
            probe.maximumEventMs);
    } else if (!bufferMatches) {
        swprintf_s(
            message, L"미통과 · 요청 %.2f ms, 실제 버퍼 %.2f ms",
            requestedPeriodMs, probe.expectedPeriodMs);
    } else if (!outputSupplyMatchesClock) {
        const double suppliedPercent = expectedSubmittedFrames > 0.0
            ? 100.0 * probe.submittedFrames / expectedSubmittedFrames : 0.0;
        swprintf_s(
            message, L"미통과 · 출력 공급 %.1f%% (목표 98%% 이상)",
            suppliedPercent);
    } else {
        swprintf_s(
            message,
            L"미통과 · 이벤트 %u회, 평균/최대 %.2f/%.2f ms "
            L"(목표 %.2f ms)",
            probe.events, probe.averageEventMs, probe.maximumEventMs,
            requestedPeriodMs);
    }
    probe.summary = message;
    return probe;
}

}  // namespace

WAVEFORMATEX PcmOutputFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = kBitsPerSample;
    format.nBlockAlign =
        format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec =
        format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

UINT32 ClosestSupportedSharedPeriod(
    UINT32 requestedFrames, const SharedModeSupport& support) {
    if (!support.supported || support.fundamentalFrames == 0) return 0;
    const UINT32 fundamental = support.fundamentalFrames;
    const UINT32 firstMultiple =
        (support.minimumFrames + fundamental - 1) / fundamental;
    const UINT32 lastMultiple = support.maximumFrames / fundamental;
    if (firstMultiple > lastMultiple) return 0;
    UINT32 requestedMultiple =
        (requestedFrames + fundamental / 2) / fundamental;
    requestedMultiple = std::max(firstMultiple, requestedMultiple);
    requestedMultiple = std::min(lastMultiple, requestedMultiple);
    return requestedMultiple * fundamental;
}

bool IsExclusiveLowLatencyBuffer(int bufferMs) {
    return std::find(
               kExclusiveBufferOptionsMs.begin(),
               kExclusiveBufferOptionsMs.end(), bufferMs) !=
           kExclusiveBufferOptionsMs.end();
}

std::wstring EndpointFriendlyName(IMMDevice* device) {
    IPropertyStore* store = nullptr;
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring result;
    if (device &&
        SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
        SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        result = value.pwszVal;
    }
    PropVariantClear(&value);
    SafeRelease(store);
    return result;
}

std::vector<EndpointInfo> EnumerateRenderEndpoints() {
    std::vector<EndpointInfo> endpoints;
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initHr);
    if (initHr == RPC_E_CHANGED_MODE) initHr = S_OK;
    if (FAILED(initHr)) return endpoints;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    IMMDevice* defaultDevice = nullptr;
    LPWSTR defaultIdRaw = nullptr;
    std::wstring defaultId;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
                eRender, eConsole, &defaultDevice)) &&
            SUCCEEDED(defaultDevice->GetId(&defaultIdRaw)) && defaultIdRaw) {
            defaultId = defaultIdRaw;
        }
        hr = enumerator->EnumAudioEndpoints(
            eRender, DEVICE_STATE_ACTIVE, &collection);
    }
    UINT count = 0;
    if (collection) collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
        IMMDevice* device = nullptr;
        LPWSTR idRaw = nullptr;
        if (SUCCEEDED(collection->Item(index, &device)) &&
            SUCCEEDED(device->GetId(&idRaw)) && idRaw) {
            EndpointInfo info{};
            info.id = idRaw;
            info.name = EndpointFriendlyName(device);
            info.isDefault = info.id == defaultId;
            if (!info.name.empty()) endpoints.push_back(std::move(info));
        }
        CoTaskMemFree(idRaw);
        SafeRelease(device);
    }
    CoTaskMemFree(defaultIdRaw);
    SafeRelease(defaultDevice);
    SafeRelease(collection);
    SafeRelease(enumerator);
    if (uninitialize) CoUninitialize();
    return endpoints;
}

std::wstring ResolveActiveEndpointId(const std::wstring& configuredId) {
    const auto endpoints = EnumerateRenderEndpoints();
    if (!configuredId.empty()) {
        for (const auto& endpoint : endpoints) {
            if (endpoint.id == configuredId) return endpoint.id;
        }
        return {};
    }
    for (const auto& endpoint : endpoints) {
        if (endpoint.isDefault) return endpoint.id;
    }
    return {};
}

HRESULT GetConfiguredEndpoint(
    IMMDeviceEnumerator* enumerator, const std::wstring& endpointId,
    IMMDevice** device) {
    if (!enumerator || !device) return E_POINTER;
    *device = nullptr;
    return endpointId.empty()
        ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device)
        : enumerator->GetDevice(endpointId.c_str(), device);
}

SharedModeSupport QuerySharedModeSupport(IMMDevice* device) {
    SharedModeSupport support{};
    if (!device) {
        support.result = E_POINTER;
        return support;
    }
    const auto begin = std::chrono::steady_clock::now();
    IAudioClient* baseClient = nullptr;
    IAudioClient3* client3 = nullptr;
    HRESULT hr = device->Activate(
        __uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
        reinterpret_cast<void**>(&baseClient));
    if (SUCCEEDED(hr)) hr = baseClient->QueryInterface(IID_PPV_ARGS(&client3));
    if (SUCCEEDED(hr)) {
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.eCategory = AudioCategory_Media;
        const HRESULT propertiesHr = client3->SetClientProperties(&properties);
        if (FAILED(propertiesHr)) hr = propertiesHr;
    }
    if (SUCCEEDED(hr)) {
        const WAVEFORMATEX format = PcmOutputFormat();
        hr = client3->GetSharedModeEnginePeriod(
            &format, &support.defaultFrames, &support.fundamentalFrames,
            &support.minimumFrames, &support.maximumFrames);
    }
    support.result = hr;
    support.supported =
        SUCCEEDED(hr) && support.defaultFrames != 0 &&
        support.fundamentalFrames != 0 && support.minimumFrames != 0 &&
        support.maximumFrames >= support.minimumFrames;
    support.probeMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
    SafeRelease(client3);
    SafeRelease(baseClient);
    return support;
}

SharedModeSupport ProbeSharedModeSupport(const std::wstring& endpointId) {
    SharedModeSupport support{};
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) hr = S_OK;
    if (FAILED(hr)) {
        support.result = hr;
        return support;
    }
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = GetConfiguredEndpoint(enumerator, endpointId, &device);
    }
    if (SUCCEEDED(hr)) {
        support = QuerySharedModeSupport(device);
    } else {
        support.result = hr;
    }
    SafeRelease(device);
    SafeRelease(enumerator);
    if (uninitialize) CoUninitialize();
    return support;
}

ExclusiveProbe ProbeExclusiveBufferRecommendation(
    const std::wstring& endpointId, const std::atomic<bool>* cancel,
    LogCallback logCallback) {
    ExclusiveProbe last{};
    for (const int candidateMs : kExclusiveBufferOptionsMs) {
        if (cancel && cancel->load(std::memory_order_acquire)) break;
        auto result =
            ProbeExclusiveCompatibility(endpointId, candidateMs, cancel);
        if (logCallback) {
            wchar_t message[512]{};
            swprintf_s(
                message,
                L"[audio][exclusive-probe] candidate=%d ms: %s | "
                L"events=%u avg/max=%.2f/%.2f ms\n",
                candidateMs, result.summary.c_str(), result.events,
                result.averageEventMs, result.maximumEventMs);
            logCallback(message);
        }
        if (result.compatible) {
            wchar_t summary[256]{};
            swprintf_s(
                summary,
                L"테스트 권장 %d ms · 실제 %.2f ms · 이벤트 평균/최대 "
                L"%.2f/%.2f ms",
                candidateMs, result.expectedPeriodMs,
                result.averageEventMs, result.maximumEventMs);
            result.summary = summary;
            return result;
        }
        last = std::move(result);
    }
    if (cancel && cancel->load(std::memory_order_acquire)) {
        last.result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        last.summary = L"독점 버퍼 검사 취소됨";
        return last;
    }
    if (last.summary.empty()) {
        last.summary = L"검사할 Exclusive 버퍼가 없음";
    } else {
        last.summary = L"Exclusive 저지연 미지원 (5–40 ms 모두 미통과)";
    }
    return last;
}

}  // namespace llcv::audio_device
