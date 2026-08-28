#include "audio/WasapiOutput.h"

#include "audio/AudioDeviceCapabilities.h"

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace llcv::wasapi {
namespace {

template<class T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

void LogHresult(const Host& host, const wchar_t* operation, HRESULT result) {
    if (host.logHresult) {
        host.logHresult(host.context, operation, result);
    } else {
        std::fwprintf(
            stderr, L"%s failed: 0x%08X\n", operation,
            static_cast<unsigned>(result));
    }
}

class DefaultEndpointNotification final : public IMMNotificationClient {
public:
    explicit DefaultEndpointNotification(std::atomic<uint64_t>* generation)
        : generation_(generation) {}

    STDMETHODIMP QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        if (id == IID_IUnknown || id == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    STDMETHODIMP OnDefaultDeviceChanged(
        EDataFlow flow, ERole role, LPCWSTR) override {
        if (generation_ && flow == eRender && role == eConsole) {
            generation_->fetch_add(1, std::memory_order_acq_rel);
        }
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    std::atomic<uint64_t>* generation_ = nullptr;
};

bool IsRunning(const Host& host) {
    return host.running && host.running->load(std::memory_order_acquire);
}

}  // namespace

bool Run(const Configuration& configuration, const Host& host) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHresult(host, L"CoInitializeEx(audio render)", hr);
        return false;
    }

    const bool exclusive = configuration.mode == Mode::Exclusive;
    const bool followDefault = configuration.endpointId.empty();
    std::atomic<uint64_t> defaultGeneration{0};
    uint64_t watchedDefaultGeneration = 0;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioClient3* client3 = nullptr;
    IAudioRenderClient* render = nullptr;
    IAudioClock* clock = nullptr;
    DefaultEndpointNotification* endpointNotification = nullptr;
    bool notificationRegistered = false;
    bool restartForDefaultChange = false;
    HANDLE eventHandle = nullptr;

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (exclusive && mmcss) {
        if (AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL)) {
            std::fwprintf(
                stderr, L"[audio][exclusive] MMCSS priority: Critical\n");
        } else {
            std::fwprintf(
                stderr,
                L"[audio][exclusive] MMCSS Critical priority request "
                L"failed (error %lu); using task default.\n",
                GetLastError());
        }
    }

    do {
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            LogHresult(host, L"CoCreateInstance(MMDeviceEnumerator)", hr);
            break;
        }

        if (followDefault) {
            endpointNotification =
                new DefaultEndpointNotification(&defaultGeneration);
            hr = enumerator->RegisterEndpointNotificationCallback(
                endpointNotification);
            if (SUCCEEDED(hr)) {
                notificationRegistered = true;
                watchedDefaultGeneration = defaultGeneration.load(
                    std::memory_order_acquire);
            } else {
                LogHresult(
                    host, L"RegisterEndpointNotificationCallback", hr);
                endpointNotification->Release();
                endpointNotification = nullptr;
            }
        }

        hr = audio_device::GetConfiguredEndpoint(
            enumerator, configuration.endpointId, &device);
        if (FAILED(hr)) {
            LogHresult(host, L"GetConfiguredAudioEndpoint", hr);
            break;
        }
        std::wstring outputName = audio_device::EndpointFriendlyName(device);
        if (outputName.empty()) {
            outputName = followDefault ? L"Windows 기본 장치"
                                       : L"선택한 출력 장치";
        }
        std::fwprintf(
            stderr, L"[audio] output endpoint: %s%s\n", outputName.c_str(),
            followDefault ? L" (following Windows default)" : L"");
        if (host.endpointChanged) {
            host.endpointChanged(host.context, outputName, followDefault);
        }

        hr = device->Activate(
            __uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void**>(&client));
        if (FAILED(hr)) {
            LogHresult(host, L"Activate(IAudioClient)", hr);
            break;
        }

        WAVEFORMATEX format = audio_device::PcmOutputFormat();
        WAVEFORMATEX* closest = nullptr;
        const AUDCLNT_SHAREMODE shareMode = exclusive
            ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
        hr = client->IsFormatSupported(shareMode, &format, &closest);
        if (exclusive) {
            if (closest) CoTaskMemFree(closest);
            closest = nullptr;
            if (hr != S_OK) {
                LogHresult(
                    host, L"IAudioClient::IsFormatSupported(exclusive)", hr);
                break;
            }
            std::fwprintf(
                stderr,
                L"[audio] exclusive exact format accepted: %u Hz / "
                L"%u-bit PCM / %u ch\n",
                format.nSamplesPerSec, format.wBitsPerSample,
                format.nChannels);

            WAVEFORMATEX* sharedMix = nullptr;
            const HRESULT mixHr = client->GetMixFormat(&sharedMix);
            if (SUCCEEDED(mixHr) && sharedMix) {
                const wchar_t* subtype = L"unknown";
                WORD validBits = sharedMix->wBitsPerSample;
                if (sharedMix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                    subtype = L"float";
                } else if (sharedMix->wFormatTag == WAVE_FORMAT_PCM) {
                    subtype = L"PCM";
                } else if (
                    sharedMix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    sharedMix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) -
                                             sizeof(WAVEFORMATEX)) {
                    const auto* extensible =
                        reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(
                            sharedMix);
                    validBits = extensible->Samples.wValidBitsPerSample;
                    if (extensible->SubFormat ==
                        KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                        subtype = L"float";
                    } else if (extensible->SubFormat ==
                               KSDATAFORMAT_SUBTYPE_PCM) {
                        subtype = L"PCM";
                    }
                }
                std::fwprintf(
                    stderr,
                    L"[audio] endpoint Shared mix format (not used by "
                    L"Exclusive): %u Hz / %u-bit %s (%u valid) / %u ch\n",
                    sharedMix->nSamplesPerSec, sharedMix->wBitsPerSample,
                    subtype, validBits, sharedMix->nChannels);
                CoTaskMemFree(sharedMix);
            } else {
                LogHresult(
                    host,
                    L"IAudioClient::GetMixFormat(exclusive diagnostic)",
                    mixHr);
            }
        } else if (FAILED(hr)) {
            if (closest) CoTaskMemFree(closest);
            LogHresult(
                host, L"IAudioClient::IsFormatSupported(shared)", hr);
            break;
        } else if (closest) {
            CoTaskMemFree(closest);
        }

        bool usingAudioClient3 = false;
        if (!exclusive) {
            audio_device::SharedModeSupport support{};
            hr = client->QueryInterface(IID_PPV_ARGS(&client3));
            if (SUCCEEDED(hr)) {
                AudioClientProperties properties{};
                properties.cbSize = sizeof(properties);
                properties.eCategory = AudioCategory_Media;
                hr = client3->SetClientProperties(&properties);
            }
            if (SUCCEEDED(hr)) {
                hr = client3->GetSharedModeEnginePeriod(
                    &format, &support.defaultFrames,
                    &support.fundamentalFrames, &support.minimumFrames,
                    &support.maximumFrames);
                support.supported = SUCCEEDED(hr);
            }
            if (support.supported) {
                UINT32 requestedFrames = configuration.sharedPeriodFrames;
                if (requestedFrames == 0) {
                    requestedFrames = static_cast<UINT32>(
                        configuration.bufferMilliseconds *
                        audio_device::kSampleRate / 1000);
                }
                const UINT32 selectedFrames =
                    audio_device::ClosestSupportedSharedPeriod(
                        requestedFrames, support);
                hr = client3->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, selectedFrames,
                    &format, nullptr);
                if (SUCCEEDED(hr)) {
                    usingAudioClient3 = true;
                    std::fwprintf(
                        stderr,
                        L"[audio] IAudioClient3 shared period: %u frames "
                        L"(%.2f ms)\n",
                        selectedFrames,
                        1000.0 * selectedFrames /
                            audio_device::kSampleRate);
                } else {
                    LogHresult(
                        host,
                        L"IAudioClient3::InitializeSharedAudioStream", hr);
                }
            } else {
                std::fwprintf(
                    stderr,
                    L"[audio] IAudioClient3 unavailable for this format; "
                    L"using classic Shared mode.\n");
            }

            if (!usingAudioClient3) {
                SafeRelease(client3);
                SafeRelease(client);
                hr = device->Activate(
                    __uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                    reinterpret_cast<void**>(&client));
                if (FAILED(hr)) {
                    LogHresult(host, L"Activate(IAudioClient fallback)", hr);
                    break;
                }
                const REFERENCE_TIME duration =
                    static_cast<REFERENCE_TIME>(
                        configuration.bufferMilliseconds) * 10'000;
                hr = client->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    duration, 0, &format, nullptr);
                if (FAILED(hr)) {
                    LogHresult(
                        host,
                        L"IAudioClient::Initialize(shared fallback/event)",
                        hr);
                    break;
                }
                std::fwprintf(
                    stderr,
                    L"[audio] classic WASAPI Shared fallback active.\n");
            }
        } else {
            REFERENCE_TIME defaultPeriod = 0;
            REFERENCE_TIME minimumPeriod = 0;
            const HRESULT periodHr = client->GetDevicePeriod(
                &defaultPeriod, &minimumPeriod);
            if (FAILED(periodHr)) {
                LogHresult(
                    host, L"IAudioClient::GetDevicePeriod(exclusive)",
                    periodHr);
            } else {
                std::fwprintf(
                    stderr,
                    L"[audio] exclusive endpoint period: default %.2f ms, "
                    L"minimum %.2f ms\n",
                    static_cast<double>(defaultPeriod) / 10'000.0,
                    static_cast<double>(minimumPeriod) / 10'000.0);
            }
            REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(
                configuration.bufferMilliseconds) * 10'000;
            std::fwprintf(
                stderr,
                L"[audio] exclusive request: buffer %.2f ms, event period "
                L"%.2f ms (same-duration event mode)\n",
                static_cast<double>(duration) / 10'000.0,
                static_cast<double>(duration) / 10'000.0);
            hr = client->Initialize(
                AUDCLNT_SHAREMODE_EXCLUSIVE,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, duration,
                &format, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 alignedFrames = 0;
                const HRESULT alignHr =
                    client->GetBufferSize(&alignedFrames);
                if (SUCCEEDED(alignHr) && alignedFrames > 0) {
                    duration = static_cast<REFERENCE_TIME>(
                        (10'000'000.0 * alignedFrames /
                         audio_device::kSampleRate) + 0.5);
                    std::fwprintf(
                        stderr,
                        L"[audio] WASAPI exclusive period aligned: %u "
                        L"frames (%.2f ms)\n",
                        alignedFrames,
                        1000.0 * alignedFrames /
                            audio_device::kSampleRate);
                    hr = client->Initialize(
                        AUDCLNT_SHAREMODE_EXCLUSIVE,
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration,
                        duration, &format, nullptr);
                } else {
                    LogHresult(
                        host,
                        L"WASAPI exclusive GetBufferSize(alignment)",
                        alignHr);
                }
            }
            if (FAILED(hr)) {
                LogHresult(
                    host,
                    L"IAudioClient::Initialize(exclusive/event)", hr);
                break;
            }
        }

        eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            std::fwprintf(stderr, L"Create audio wake handle failed.\n");
            break;
        }
        hr = client->SetEventHandle(eventHandle);
        if (FAILED(hr)) {
            LogHresult(host, L"SetEventHandle", hr);
            break;
        }
        hr = client->GetService(IID_PPV_ARGS(&render));
        if (FAILED(hr)) {
            LogHresult(host, L"GetService(IAudioRenderClient)", hr);
            break;
        }

        UINT64 exclusiveClockFrequency = 0;
        if (exclusive) {
            const HRESULT clockHr = client->GetService(IID_PPV_ARGS(&clock));
            if (SUCCEEDED(clockHr) && clock) {
                const HRESULT frequencyHr =
                    clock->GetFrequency(&exclusiveClockFrequency);
                if (FAILED(frequencyHr)) {
                    LogHresult(
                        host, L"IAudioClock::GetFrequency(exclusive)",
                        frequencyHr);
                    SafeRelease(clock);
                    exclusiveClockFrequency = 0;
                } else {
                    std::fwprintf(
                        stderr,
                        L"[audio][exclusive] endpoint clock frequency: "
                        L"%llu ticks/sec\n",
                        static_cast<unsigned long long>(
                            exclusiveClockFrequency));
                }
            } else {
                LogHresult(
                    host, L"GetService(IAudioClock exclusive)", clockHr);
            }
        }

        UINT32 bufferFrames = 0;
        hr = client->GetBufferSize(&bufferFrames);
        if (FAILED(hr)) {
            LogHresult(host, L"GetBufferSize", hr);
            break;
        }
        if (host.bufferChanged) {
            host.bufferChanged(host.context, bufferFrames);
        }
        std::fwprintf(
            stderr, L"[audio] WASAPI %s buffer: %u frames (%.2f ms)\n",
            exclusive ? L"exclusive" : L"shared", bufferFrames,
            1000.0 * bufferFrames / audio_device::kSampleRate);
        if (exclusive || !usingAudioClient3) {
            std::fwprintf(
                stderr, L"[audio] requested WASAPI buffer: %d ms\n",
                configuration.bufferMilliseconds);
        }

        BYTE* prime = nullptr;
        hr = render->GetBuffer(bufferFrames, &prime);
        if (FAILED(hr)) {
            LogHresult(host, L"GetBuffer(prime)", hr);
            break;
        }
        std::memset(
            prime, 0, static_cast<size_t>(bufferFrames) *
                          format.nBlockAlign);
        render->ReleaseBuffer(bufferFrames, 0);

        if (configuration.reinitializingEndpoint && host.beforeStart) {
            host.beforeStart(host.context);
            std::fwprintf(
                stderr,
                L"[audio] endpoint switch: discarded setup backlog "
                L"immediately before Start.\n");
        }

        hr = client->Start();
        if (FAILED(hr)) {
            LogHresult(host, L"IAudioClient::Start", hr);
            break;
        }
        std::fwprintf(
            stderr, L"[audio] WASAPI %s render running.\n",
            exclusive ? L"exclusive" : L"shared");
        std::fwprintf(
            stderr, L"[audio] clock-drift correction: %s\n",
            configuration.correctionDescription);

        std::vector<int16_t> temp(
            static_cast<size_t>(bufferFrames) * audio_device::kChannels);
        uint64_t windowStartMs = GetTickCount64();
        uint64_t lastEventMs = 0;
        uint64_t lastClockPosition = 0;
        uint64_t eventCount = 0;
        uint64_t eventIntervalTotalMs = 0;
        uint64_t eventIntervalMaxMs = 0;
        uint64_t lateEventCount = 0;
        uint64_t waitTimeoutCount = 0;
        uint64_t clockIntervalCount = 0;
        uint64_t clockIntervalTotalTicks = 0;
        uint64_t clockIntervalMaxTicks = 0;
        uint64_t lastLateLogMs = 0;
        uint64_t lastTimeoutLogMs = 0;
        uint64_t lastStarvationLogMs = 0;
        uint64_t requestedFrames = 0;
        uint64_t writtenFrames = 0;
        uint64_t missingFrames = 0;
        UINT32 minimumPadding = (std::numeric_limits<UINT32>::max)();
        UINT32 maximumPadding = 0;
        FillResult latestFill{};
        const uint64_t lateThresholdMs = static_cast<uint64_t>(
            std::ceil(
                2000.0 * bufferFrames / audio_device::kSampleRate)) + 2;

        const auto emitDiagnostics = [&](uint64_t nowMs) {
            if (!exclusive || nowMs < windowStartMs + 1000) return;
            const double averageEventMs = eventCount > 1
                ? static_cast<double>(eventIntervalTotalMs) /
                      static_cast<double>(eventCount - 1)
                : 0.0;
            const double averageClockMs = clockIntervalCount &&
                    exclusiveClockFrequency
                ? 1000.0 * static_cast<double>(clockIntervalTotalTicks) /
                      static_cast<double>(clockIntervalCount) /
                      static_cast<double>(exclusiveClockFrequency)
                : 0.0;
            const double maximumClockMs = exclusiveClockFrequency
                ? 1000.0 * static_cast<double>(clockIntervalMaxTicks) /
                      static_cast<double>(exclusiveClockFrequency)
                : 0.0;
            std::fwprintf(
                stderr,
                L"[audio][exclusive] 1s: event=%llu, interval avg/max "
                L"%.2f/%llu ms, late=%llu (threshold %llu ms), "
                L"timeout=%llu, padding min/max=%u/%u frames, "
                L"write=%llu/%llu frames, missing=%llu, queue=%u frames "
                L"(target %u), device-clock avg/max=%.2f/%.2f ms, "
                L"resampler=%s %+d ppm\n",
                static_cast<unsigned long long>(eventCount), averageEventMs,
                static_cast<unsigned long long>(eventIntervalMaxMs),
                static_cast<unsigned long long>(lateEventCount),
                static_cast<unsigned long long>(lateThresholdMs),
                static_cast<unsigned long long>(waitTimeoutCount),
                minimumPadding == (std::numeric_limits<UINT32>::max)()
                    ? 0 : minimumPadding,
                maximumPadding,
                static_cast<unsigned long long>(writtenFrames),
                static_cast<unsigned long long>(requestedFrames),
                static_cast<unsigned long long>(missingFrames),
                latestFill.queuedFrames, latestFill.queueTargetFrames,
                averageClockMs, maximumClockMs,
                latestFill.resamplerActive ? L"on" : L"off",
                latestFill.resamplePpm);
            windowStartMs = nowMs;
            eventCount = 0;
            eventIntervalTotalMs = 0;
            eventIntervalMaxMs = 0;
            lateEventCount = 0;
            waitTimeoutCount = 0;
            clockIntervalCount = 0;
            clockIntervalTotalTicks = 0;
            clockIntervalMaxTicks = 0;
            requestedFrames = 0;
            writtenFrames = 0;
            missingFrames = 0;
            minimumPadding = (std::numeric_limits<UINT32>::max)();
            maximumPadding = 0;
        };

        while (IsRunning(host)) {
            if (followDefault && notificationRegistered &&
                defaultGeneration.load(std::memory_order_acquire) !=
                    watchedDefaultGeneration) {
                restartForDefaultChange = true;
                std::fwprintf(
                    stderr,
                    L"[audio] Windows default output changed; "
                    L"reinitializing WASAPI only.\n");
                break;
            }
            const DWORD waitResult = WaitForSingleObject(eventHandle, 100);
            const uint64_t eventNowMs = GetTickCount64();
            if (waitResult != WAIT_OBJECT_0) {
                if (exclusive && waitResult == WAIT_TIMEOUT) {
                    ++waitTimeoutCount;
                    if (!lastTimeoutLogMs ||
                        eventNowMs >= lastTimeoutLogMs + 1000) {
                        std::fwprintf(
                            stderr,
                            L"[audio][exclusive] event wait timed out "
                            L"after 100 ms; renderer did not receive a "
                            L"buffer-ready signal.\n");
                        lastTimeoutLogMs = eventNowMs;
                    }
                    emitDiagnostics(eventNowMs);
                }
                continue;
            }

            if (exclusive) {
                if (clock && exclusiveClockFrequency) {
                    UINT64 position = 0;
                    if (SUCCEEDED(clock->GetPosition(&position, nullptr))) {
                        if (lastClockPosition && position >= lastClockPosition) {
                            const UINT64 delta = position - lastClockPosition;
                            ++clockIntervalCount;
                            clockIntervalTotalTicks += delta;
                            clockIntervalMaxTicks = (std::max)(
                                clockIntervalMaxTicks, delta);
                        }
                        lastClockPosition = position;
                    }
                }
                if (lastEventMs) {
                    const uint64_t intervalMs = eventNowMs - lastEventMs;
                    eventIntervalTotalMs += intervalMs;
                    eventIntervalMaxMs = (std::max)(
                        eventIntervalMaxMs, intervalMs);
                    if (intervalMs >= lateThresholdMs) {
                        ++lateEventCount;
                        if (!lastLateLogMs ||
                            eventNowMs >= lastLateLogMs + 1000) {
                            std::fwprintf(
                                stderr,
                                L"[audio][exclusive] late event: %llu ms "
                                L"since previous signal (threshold %llu "
                                L"ms).\n",
                                static_cast<unsigned long long>(intervalMs),
                                static_cast<unsigned long long>(
                                    lateThresholdMs));
                            lastLateLogMs = eventNowMs;
                        }
                    }
                }
                lastEventMs = eventNowMs;
                ++eventCount;
            }

            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) continue;
            if (host.paddingChanged) {
                host.paddingChanged(host.context, padding);
            }
            if (exclusive) {
                minimumPadding = (std::min)(minimumPadding, padding);
                maximumPadding = (std::max)(maximumPadding, padding);
            }
            const UINT32 writable = bufferFrames > padding
                ? bufferFrames - padding : 0;
            if (!writable) continue;

            BYTE* output = nullptr;
            hr = render->GetBuffer(writable, &output);
            if (FAILED(hr)) continue;
            latestFill = host.fill
                ? host.fill(host.context, temp.data(), writable)
                : FillResult{};
            const size_t supplied = (std::min)(
                latestFill.writtenFrames, static_cast<size_t>(writable));
            if (supplied) {
                std::memcpy(
                    output, temp.data(), supplied * format.nBlockAlign);
            }
            if (exclusive) {
                requestedFrames += writable;
                writtenFrames += supplied;
            }
            if (supplied < writable) {
                const UINT32 missing =
                    writable - static_cast<UINT32>(supplied);
                std::memset(
                    output + supplied * format.nBlockAlign, 0,
                    static_cast<size_t>(missing) * format.nBlockAlign);
                if (exclusive && latestFill.audioStarted &&
                    latestFill.trackingActive) {
                    missingFrames += missing;
                    if (!lastStarvationLogMs ||
                        eventNowMs >= lastStarvationLogMs + 1000) {
                        std::fwprintf(
                            stderr,
                            L"[audio][exclusive] source starvation: "
                            L"needed=%u, received=%zu, missing=%u frames; "
                            L"queue-before=%zu frames.\n",
                            writable, supplied, missing,
                            latestFill.availableBeforeRender);
                        lastStarvationLogMs = eventNowMs;
                    }
                }
            }
            render->ReleaseBuffer(writable, 0);
            emitDiagnostics(eventNowMs);
        }

        client->Stop();
    } while (false);

    if (eventHandle) CloseHandle(eventHandle);
    SafeRelease(clock);
    SafeRelease(render);
    SafeRelease(client3);
    SafeRelease(client);
    SafeRelease(device);
    if (enumerator && notificationRegistered && endpointNotification) {
        enumerator->UnregisterEndpointNotificationCallback(
            endpointNotification);
    }
    if (endpointNotification) endpointNotification->Release();
    SafeRelease(enumerator);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
    if (followDefault && notificationRegistered &&
        defaultGeneration.load(std::memory_order_acquire) !=
            watchedDefaultGeneration) {
        restartForDefaultChange = true;
    }
    return restartForDefaultChange;
}

}  // namespace llcv::wasapi
