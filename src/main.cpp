/*
 * Low Latency Capture Viewer
 * Copyright (C) 2026 seria-aa
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <windows.h>
#include <windowsx.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <future>
#include <functional>
#include <cstdarg>

// -----------------------------------------------------------------------------
// Minimal Sample Grabber declarations.
// qedit.h is no longer shipped in modern Windows SDKs, so the interfaces and
// CLSIDs are declared locally. The COM component itself is part of legacy
// DirectShow on many desktop Windows installations.
// -----------------------------------------------------------------------------

struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85")) ISampleGrabberCB : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample* pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) = 0;
};

struct __declspec(uuid("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")) ISampleGrabber : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};

static const CLSID CLSID_SampleGrabber =
{0xC1F400A0,0x3F08,0x11D3,{0x9F,0x0B,0x00,0x60,0x08,0x03,0x9E,0x37}};

static const CLSID CLSID_NullRenderer =
{0xC1F400A4,0x3F08,0x11D3,{0x9F,0x0B,0x00,0x60,0x08,0x03,0x9E,0x37}};

// -----------------------------------------------------------------------------
// User-tested settings.
// -----------------------------------------------------------------------------

constexpr wchar_t kCaptureName[] = L"AVerMedia HD Capture GC573 1";
constexpr wchar_t kAudioPinName[] = L"Audio";
constexpr wchar_t kVideoPinName[] = L"Video";

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBitsPerSample = 16;

constexpr int kRecommendedCaptureBufferMs = 20;
static constexpr int kWasapiBufferOptionsMs[] = {5, 10, 15, 20, 30, 40};
constexpr int kRecommendedWasapiBufferMs = 20;
static constexpr int kPcmQueueOptionsMs[] = {10, 15, 20, 30};
constexpr int kLowestPcmQueueMs = 10;

enum class AudioMode {
    WasapiShared,
    WasapiExclusive,
};

enum class DriftCorrectionMode {
    Off,
    Resample,
};

enum class VolumeHudPosition {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

enum class VideoPreset {
    R1920x1080,
    R2560x1440,
    R3840x2160,
};

enum class PresentationMode {
    AllowTearing,
    VSync,
};

enum class VideoPixelFormat {
    Auto,
    Nv12,
    Yuy2,
};

struct CaptureDeviceInfo {
    std::wstring id;
    std::wstring name;
};

struct AudioEndpointInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct PixelFormatSupport {
    VideoPixelFormat format = VideoPixelFormat::Auto;
    int selectedFps = 0;
};

struct AppSettings {
    AudioMode audioMode = AudioMode::WasapiShared;
    int wasapiBufferMs = kRecommendedWasapiBufferMs;
    UINT32 wasapiSharedPeriodFrames = 0;
    DriftCorrectionMode driftCorrection = DriftCorrectionMode::Off;
    int pcmQueueTargetMs = kLowestPcmQueueMs;
    int volumePercent = 100;
    VolumeHudPosition volumeHudPosition = VolumeHudPosition::TopLeft;
    bool muteWhenBackground = false;
    PresentationMode presentationMode = PresentationMode::AllowTearing;
    VideoPreset videoPreset = VideoPreset::R2560x1440;
    VideoPixelFormat pixelFormat = VideoPixelFormat::Auto;
    int videoFrameRate = 0;
    std::wstring captureDeviceId;
    std::wstring audioOutputDeviceId;
    bool saveLog = false;
    bool pixelPerfect = true;
    bool relativeWindowSize = false;
    int relativeWindowScalePpm = 0;
    bool borderlessWindow = false;
    bool windowSnap = true;
    bool hasWindowPosition = false;
    int windowX = 0;
    int windowY = 0;
    std::wstring monitorDevice;
};

struct VideoPresetInfo {
    VideoPreset preset;
    int width;
    int height;
    int framerate;
    const wchar_t* label;
};

static constexpr VideoPresetInfo kVideoPresets[] = {
    {VideoPreset::R1920x1080, 1920, 1080, 120, L"1920 x 1080"},
    {VideoPreset::R2560x1440, 2560, 1440, 120, L"2560 x 1440"},
    {VideoPreset::R3840x2160, 3840, 2160, 60, L"3840 x 2160"},
};

static AppSettings g_settings{};

struct AudioClient3Support {
    bool supported = false;
    HRESULT result = E_FAIL;
    UINT32 defaultFrames = 0;
    UINT32 fundamentalFrames = 0;
    UINT32 minimumFrames = 0;
    UINT32 maximumFrames = 0;
    double probeMilliseconds = 0.0;
};

// 500 ms ring capacity. The program deliberately does NOT wait for this much
// data; it is only headroom. Old data is dropped if the producer overruns.
constexpr size_t kRingFrames = 48000 / 2;

static HWND g_videoHost = nullptr;
static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_videoCapturedFrames{0};
static std::atomic<uint64_t> g_videoPresentedFrames{0};
static std::atomic<uint64_t> g_videoReplacedFrames{0};
static std::atomic<int64_t> g_videoAppLatencyUs{-1};
static std::atomic<UINT32> g_videoStride{0};
static std::atomic<int> g_videoConfiguredFps{0};
static std::atomic<bool> g_videoTearing{false};
static std::atomic<bool> g_directVideoActive{false};
static std::atomic<HRESULT> g_captureFailureHr{S_OK};
static std::atomic<UINT32> g_audioActualBufferFrames{0};
static std::atomic<UINT32> g_audioWasapiPaddingFrames{0};
static std::atomic<UINT32> g_audioCapturePacketFrames{0};
static std::atomic<int64_t> g_audioCaptureIntervalUs{0};
static std::atomic<LONG> g_audioCaptureAllocatorFrames{0};
static std::atomic<LONG> g_audioCaptureAllocatorBuffers{0};
static std::atomic<UINT32> g_audioRingFrames{0};
static std::atomic<UINT32> g_audioResamplerFrames{0};
static std::atomic<int> g_audioResamplePpm{0};
static std::atomic<uint64_t> g_audioResampledOutputFrames{0};
static std::atomic<uint64_t> g_audioCaptureCallbacks{0};
static std::atomic<uint64_t> g_audioCaptureFrames{0};
static std::atomic<uint64_t> g_audioCaptureIntervalTotalUs{0};
static std::atomic<uint64_t> g_audioUnderrunFrames{0};
static std::atomic<uint64_t> g_audioOverrunFrames{0};
static std::atomic<uint64_t> g_audioMonitorStartMs{0};
static std::atomic<uint64_t> g_audioLastUnderrunMs{0};
static std::atomic<uint64_t> g_audioLastOverrunMs{0};
static std::atomic<uint64_t> g_audioLastCaptureCallbackMs{0};
static std::atomic<uint64_t> g_audioLatePacketUnderruns{0};
static std::atomic<uint64_t> g_audioResamplerUnderruns{0};
static std::atomic<UINT32> g_audioMinimumPreRenderFrames{UINT32_MAX};
static std::atomic<UINT32> g_audioQueueTargetFrames{0};
static std::atomic<int> g_volumePercent{100};
static std::atomic<bool> g_backgroundAudioMuted{false};
static std::atomic<uint64_t> g_volumeHudUntilMs{0};
static std::atomic<uint64_t> g_overlayGeneration{1};
static std::atomic<uint64_t> g_overlayRenderedFrames{0};
static std::atomic<double> g_osdInputFps{0.0};
static std::atomic<double> g_osdPresentFps{0.0};
static std::atomic<bool> g_osdVisible{false};
static constexpr uint64_t kOsdTrackingWarmupMs = 2000;
static std::atomic<uint64_t> g_osdTrackingStartMs{UINT64_MAX};
static std::atomic<bool> g_captureAudioAvailable{true};
static std::wstring g_activeCaptureDeviceName = kCaptureName;
static std::wstring g_activeAudioOutputName = L"Windows 기본 장치";
static std::atomic<int> g_activePixelFormat{
    static_cast<int>(VideoPixelFormat::Nv12)};
static std::atomic<uint64_t> g_defaultAudioEndpointGeneration{0};
static FILE* g_savedLogFile = nullptr;
static std::mutex g_logMutex;
static std::mutex g_activeAudioOutputMutex;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

template<class T>
static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static int TeeFwprintf(FILE* stream, const wchar_t* format, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    va_list args;
    va_start(args, format);
    va_list fileArgs;
    va_copy(fileArgs, args);
    const int result = vfwprintf(stream, format, args);
    fflush(stream);
    if (g_savedLogFile && g_savedLogFile != stream) {
        vfwprintf(g_savedLogFile, format, fileArgs);
        fflush(g_savedLogFile);
    }
    va_end(fileArgs);
    va_end(args);
    return result;
}

#define fwprintf TeeFwprintf

static const wchar_t* PixelFormatName(VideoPixelFormat format) {
    switch (format) {
    case VideoPixelFormat::Yuy2: return L"YUY2";
    case VideoPixelFormat::Nv12: return L"NV12";
    default: return L"Auto";
    }
}

static const GUID& PixelFormatSubtype(VideoPixelFormat format) {
    return format == VideoPixelFormat::Yuy2
               ? MEDIASUBTYPE_YUY2 : MEDIASUBTYPE_NV12;
}

static DXGI_FORMAT PixelFormatDxgi(VideoPixelFormat format) {
    return format == VideoPixelFormat::Yuy2
               ? DXGI_FORMAT_YUY2 : DXGI_FORMAT_NV12;
}

static bool OsdTrackingActive() {
    return GetTickCount64() >=
        g_osdTrackingStartMs.load(std::memory_order_acquire);
}

static void SetActiveAudioOutputName(const std::wstring& name) {
    std::lock_guard<std::mutex> lock(g_activeAudioOutputMutex);
    g_activeAudioOutputName = name;
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
}

static std::wstring ActiveAudioOutputName() {
    std::lock_guard<std::mutex> lock(g_activeAudioOutputMutex);
    return g_activeAudioOutputName;
}

static void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0) {
        CoTaskMemFree((PVOID)mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk != nullptr) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

static std::wstring HrText(HRESULT hr) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, 0,
                   reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring out = msg ? msg : L"(unknown HRESULT)";
    if (msg) LocalFree(msg);
    return out;
}

static void LogHr(const wchar_t* where, HRESULT hr) {
    fwprintf(stderr, L"%s failed: 0x%08X %s\n",
             where, static_cast<unsigned>(hr), HrText(hr).c_str());
}

static WAVEFORMATEX PcmOutputFormat() {
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = kChannels;
    wf.nSamplesPerSec = kSampleRate;
    wf.wBitsPerSample = kBitsPerSample;
    wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    return wf;
}

static UINT32 ClosestSupportedSharedPeriod(
    UINT32 requestedFrames, const AudioClient3Support& support) {
    if (!support.supported || support.fundamentalFrames == 0) return 0;

    const UINT32 fundamental = support.fundamentalFrames;
    const UINT32 firstMultiple =
        (support.minimumFrames + fundamental - 1) / fundamental;
    const UINT32 lastMultiple = support.maximumFrames / fundamental;
    if (firstMultiple > lastMultiple) return 0;

    UINT32 requestedMultiple =
        (requestedFrames + fundamental / 2) / fundamental;
    requestedMultiple = (std::max)(firstMultiple, requestedMultiple);
    requestedMultiple = (std::min)(lastMultiple, requestedMultiple);
    return requestedMultiple * fundamental;
}

static AudioClient3Support QueryAudioClient3Support(IMMDevice* device) {
    AudioClient3Support support{};
    const auto begin = std::chrono::steady_clock::now();

    IAudioClient* baseClient = nullptr;
    IAudioClient3* client3 = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                                  nullptr,
                                  reinterpret_cast<void**>(&baseClient));
    if (SUCCEEDED(hr)) {
        hr = baseClient->QueryInterface(IID_PPV_ARGS(&client3));
    }
    if (SUCCEEDED(hr)) {
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.eCategory = AudioCategory_Media;
        const HRESULT propertiesHr = client3->SetClientProperties(&properties);
        if (FAILED(propertiesHr)) hr = propertiesHr;
    }
    if (SUCCEEDED(hr)) {
        const WAVEFORMATEX wf = PcmOutputFormat();
        hr = client3->GetSharedModeEnginePeriod(
            &wf, &support.defaultFrames, &support.fundamentalFrames,
            &support.minimumFrames, &support.maximumFrames);
    }

    support.result = hr;
    support.supported = SUCCEEDED(hr) && support.defaultFrames != 0 &&
                        support.fundamentalFrames != 0 &&
                        support.minimumFrames != 0 &&
                        support.maximumFrames >= support.minimumFrames;
    const auto end = std::chrono::steady_clock::now();
    support.probeMilliseconds =
        std::chrono::duration<double, std::milli>(end - begin).count();

    SafeRelease(client3);
    SafeRelease(baseClient);
    return support;
}

static HRESULT GetConfiguredAudioEndpoint(IMMDeviceEnumerator* enumerator,
                                          const std::wstring& endpointId,
                                          IMMDevice** device) {
    if (!enumerator || !device) return E_POINTER;
    *device = nullptr;
    return endpointId.empty()
               ? enumerator->GetDefaultAudioEndpoint(
                     eRender, eConsole, device)
               : enumerator->GetDevice(endpointId.c_str(), device);
}

static AudioClient3Support ProbeAudioClient3Support(
    const std::wstring& endpointId) {
    AudioClient3Support support{};
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) hr = S_OK;
    if (FAILED(hr)) {
        support.result = hr;
        return support;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = GetConfiguredAudioEndpoint(enumerator, endpointId, &device);
    }
    if (SUCCEEDED(hr)) {
        support = QueryAudioClient3Support(device);
    } else {
        support.result = hr;
    }

    SafeRelease(device);
    SafeRelease(enumerator);
    if (uninitialize) CoUninitialize();
    return support;
}

static std::wstring AppDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!n || n >= ARRAYSIZE(path)) return L".";

    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    dir.resize(slash);
    return dir;
}

static void OpenSavedLog() {
    if (!g_settings.saveLog || g_savedLogFile) return;
    const std::wstring directory = AppDirectory() + L"\\logs";
    CreateDirectoryW(directory.c_str(), nullptr);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t name[96]{};
    swprintf_s(name, L"LowLatencyCapture_%04u%02u%02u_%02u%02u%02u.log",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
               time.wSecond);
    const std::wstring path = directory + L"\\" + name;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wt, ccs=UTF-8") == 0 && file) {
        g_savedLogFile = file;
        fwprintf(stderr, L"[log] saving diagnostics: %s\n", path.c_str());
    }
}

static void CloseSavedLog() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_savedLogFile) {
        fclose(g_savedLogFile);
        g_savedLogFile = nullptr;
    }
}

static std::wstring MonikerDisplayName(IMoniker* moniker) {
    if (!moniker) return {};
    IBindCtx* bindContext = nullptr;
    if (FAILED(CreateBindCtx(0, &bindContext))) return {};
    LPOLESTR value = nullptr;
    std::wstring result;
    if (SUCCEEDED(moniker->GetDisplayName(bindContext, nullptr, &value)) &&
        value) {
        result = value;
    }
    CoTaskMemFree(value);
    bindContext->Release();
    return result;
}

static std::wstring MonikerFriendlyName(IMoniker* moniker) {
    if (!moniker) return {};
    IPropertyBag* bag = nullptr;
    VARIANT value;
    VariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(moniker->BindToStorage(
            nullptr, nullptr, IID_PPV_ARGS(&bag))) &&
        SUCCEEDED(bag->Read(L"FriendlyName", &value, nullptr)) &&
        value.vt == VT_BSTR && value.bstrVal) {
        result = value.bstrVal;
    }
    VariantClear(&value);
    SafeRelease(bag);
    return result;
}

static std::vector<CaptureDeviceInfo> EnumerateCaptureDevices() {
    std::vector<CaptureDeviceInfo> devices;
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initHr);
    if (initHr == RPC_E_CHANGED_MODE) initHr = S_OK;
    if (FAILED(initHr)) return devices;
    ICreateDevEnum* deviceEnumerator = nullptr;
    IEnumMoniker* monikers = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&deviceEnumerator));
    if (SUCCEEDED(hr)) {
        hr = deviceEnumerator->CreateClassEnumerator(
            CLSID_VideoInputDeviceCategory, &monikers, 0);
    }
    IMoniker* moniker = nullptr;
    while (monikers && monikers->Next(1, &moniker, nullptr) == S_OK) {
        CaptureDeviceInfo info{};
        info.id = MonikerDisplayName(moniker);
        info.name = MonikerFriendlyName(moniker);
        if (!info.id.empty() && !info.name.empty()) {
            devices.push_back(std::move(info));
        }
        SafeRelease(moniker);
    }
    SafeRelease(monikers);
    SafeRelease(deviceEnumerator);
    if (uninitialize) CoUninitialize();
    return devices;
}

static std::wstring AudioEndpointFriendlyName(IMMDevice* device) {
    IPropertyStore* store = nullptr;
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring result;
    if (device && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
        SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        result = value.pwszVal;
    }
    PropVariantClear(&value);
    SafeRelease(store);
    return result;
}

static std::vector<AudioEndpointInfo> EnumerateAudioEndpoints() {
    std::vector<AudioEndpointInfo> endpoints;
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initHr);
    if (initHr == RPC_E_CHANGED_MODE) initHr = S_OK;
    if (FAILED(initHr)) return endpoints;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    IMMDevice* defaultDevice = nullptr;
    LPWSTR defaultIdRaw = nullptr;
    std::wstring defaultId;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_INPROC_SERVER,
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
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        LPWSTR idRaw = nullptr;
        if (SUCCEEDED(collection->Item(i, &device)) &&
            SUCCEEDED(device->GetId(&idRaw)) && idRaw) {
            AudioEndpointInfo info{};
            info.id = idRaw;
            info.name = AudioEndpointFriendlyName(device);
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

static std::wstring ConfiguredAudioEndpointName(
    const std::wstring& endpointId) {
    const auto endpoints = EnumerateAudioEndpoints();
    if (!endpointId.empty()) {
        for (const auto& endpoint : endpoints) {
            if (endpoint.id == endpointId) return endpoint.name;
        }
        return L"선택 장치 없음";
    }
    for (const auto& endpoint : endpoints) {
        if (endpoint.isDefault) return endpoint.name + L" (기본)";
    }
    return L"Windows 기본 장치";
}

static const VideoPresetInfo& CurrentVideoPreset() {
    for (const auto& info : kVideoPresets) {
        if (info.preset == g_settings.videoPreset) return info;
    }
    return kVideoPresets[1];
}

static int RequestedVideoFrameRate() {
    return g_settings.videoFrameRate > 0
               ? g_settings.videoFrameRate
               : CurrentVideoPreset().framerate;
}

static constexpr int kRelativeScaleUnit = 1'000'000;

static int RelativeScaleForMonitor(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0;
    const int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
    const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
    if (monitorWidth <= 0 || monitorHeight <= 0) return 0;
    const auto& video = CurrentVideoPreset();
    const int widthScale = static_cast<int>(
        static_cast<int64_t>(video.width) * kRelativeScaleUnit /
        monitorWidth);
    const int heightScale = static_cast<int>(
        static_cast<int64_t>(video.height) * kRelativeScaleUnit /
        monitorHeight);
    return std::clamp((std::min)(widthScale, heightScale),
                      kRelativeScaleUnit / 4, kRelativeScaleUnit);
}

static std::wstring SettingsPath() {
    return AppDirectory() + L"\\settings.ini";
}

static void LoadSettings() {
    const std::wstring path = SettingsPath();
    wchar_t audio[32]{};
    wchar_t bufferMs[16]{};
    wchar_t sharedPeriodFrames[16]{};
    wchar_t driftCorrection[32]{};
    wchar_t pcmQueueTargetMs[16]{};
    wchar_t volume[16]{};
    wchar_t volumeHudPosition[32]{};
    wchar_t muteWhenBackground[8]{};
    wchar_t audioOutputDeviceId[1024]{};
    wchar_t resolution[32]{};
    wchar_t captureDeviceId[1024]{};
    wchar_t pixelFormat[32]{};
    wchar_t frameRate[16]{};
    wchar_t presentation[32]{};
    wchar_t pixelPerfect[8]{};
    wchar_t relativeWindowSize[8]{};
    wchar_t relativeWindowScale[16]{};
    wchar_t borderlessWindow[8]{};
    wchar_t windowSnap[8]{};
    wchar_t windowX[32]{};
    wchar_t windowY[32]{};
    wchar_t monitorDevice[64]{};
    wchar_t saveLog[8]{};

    GetPrivateProfileStringW(L"Audio", L"Mode", L"Shared", audio,
                             ARRAYSIZE(audio), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"BufferMs", L"20", bufferMs,
                             ARRAYSIZE(bufferMs), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"SharedPeriodFrames", L"0",
                             sharedPeriodFrames, ARRAYSIZE(sharedPeriodFrames),
                             path.c_str());
    GetPrivateProfileStringW(L"Audio", L"DriftCorrection", L"Off",
                             driftCorrection, ARRAYSIZE(driftCorrection),
                             path.c_str());
    GetPrivateProfileStringW(L"Audio", L"PcmQueueTargetMs", L"",
                             pcmQueueTargetMs, ARRAYSIZE(pcmQueueTargetMs),
                             path.c_str());
    GetPrivateProfileStringW(L"Audio", L"Volume", L"100", volume,
                             ARRAYSIZE(volume), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"VolumeHudPosition", L"TopLeft",
                             volumeHudPosition,
                             ARRAYSIZE(volumeHudPosition), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"MuteWhenBackground", L"0",
                             muteWhenBackground,
                             ARRAYSIZE(muteWhenBackground), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"OutputDeviceId", L"",
                             audioOutputDeviceId,
                             ARRAYSIZE(audioOutputDeviceId), path.c_str());
    GetPrivateProfileStringW(L"Video", L"Resolution", L"2560x1440", resolution,
                             ARRAYSIZE(resolution), path.c_str());
    GetPrivateProfileStringW(L"Video", L"CaptureDeviceId", L"",
                             captureDeviceId, ARRAYSIZE(captureDeviceId),
                             path.c_str());
    GetPrivateProfileStringW(L"Video", L"PixelFormat", L"Auto",
                             pixelFormat, ARRAYSIZE(pixelFormat), path.c_str());
    GetPrivateProfileStringW(L"Video", L"FrameRate", L"0", frameRate,
                             ARRAYSIZE(frameRate), path.c_str());
    GetPrivateProfileStringW(L"Video", L"Presentation", L"AllowTearing",
                             presentation, ARRAYSIZE(presentation), path.c_str());
    GetPrivateProfileStringW(L"Video", L"PixelPerfect", L"1", pixelPerfect,
                             ARRAYSIZE(pixelPerfect), path.c_str());
    GetPrivateProfileStringW(L"Video", L"RelativeWindowSize", L"0",
                             relativeWindowSize,
                             ARRAYSIZE(relativeWindowSize), path.c_str());
    GetPrivateProfileStringW(L"Video", L"RelativeWindowScalePpm", L"0",
                             relativeWindowScale,
                             ARRAYSIZE(relativeWindowScale), path.c_str());
    GetPrivateProfileStringW(L"Video", L"BorderlessWindow", L"0", borderlessWindow,
                             ARRAYSIZE(borderlessWindow), path.c_str());
    GetPrivateProfileStringW(L"Window", L"Snap", L"1", windowSnap,
                             ARRAYSIZE(windowSnap), path.c_str());
    GetPrivateProfileStringW(L"Window", L"X", L"", windowX,
                             ARRAYSIZE(windowX), path.c_str());
    GetPrivateProfileStringW(L"Window", L"Y", L"", windowY,
                             ARRAYSIZE(windowY), path.c_str());
    GetPrivateProfileStringW(L"Window", L"Monitor", L"", monitorDevice,
                             ARRAYSIZE(monitorDevice), path.c_str());
    GetPrivateProfileStringW(L"Diagnostics", L"SaveLog", L"0", saveLog,
                             ARRAYSIZE(saveLog), path.c_str());

    g_settings.audioMode = (_wcsicmp(audio, L"Exclusive") == 0)
                               ? AudioMode::WasapiExclusive
                               : AudioMode::WasapiShared;
    const int requestedBufferMs = static_cast<int>(wcstol(bufferMs, nullptr, 10));
    g_settings.wasapiBufferMs = kRecommendedWasapiBufferMs;
    for (const int option : kWasapiBufferOptionsMs) {
        if (requestedBufferMs == option) {
            g_settings.wasapiBufferMs = option;
            break;
        }
    }
    g_settings.wasapiSharedPeriodFrames =
        static_cast<UINT32>(wcstoul(sharedPeriodFrames, nullptr, 10));
    g_settings.driftCorrection =
        _wcsicmp(driftCorrection, L"Resample") == 0
            ? DriftCorrectionMode::Resample
            : DriftCorrectionMode::Off;
    // v0.13 and older implicitly used 20 ms with resampling and the first
    // available 10 ms packet without it. Preserve that behavior when the new
    // independent key is absent.
    g_settings.pcmQueueTargetMs =
        g_settings.driftCorrection == DriftCorrectionMode::Resample
            ? 20 : kLowestPcmQueueMs;
    if (pcmQueueTargetMs[0]) {
        const int requestedQueueMs = static_cast<int>(
            wcstol(pcmQueueTargetMs, nullptr, 10));
        for (const int option : kPcmQueueOptionsMs) {
            if (requestedQueueMs == option) {
                g_settings.pcmQueueTargetMs = option;
                break;
            }
        }
    }
    const int loadedVolume = std::clamp(
        static_cast<int>(wcstol(volume, nullptr, 10)), 0, 100);
    g_settings.volumePercent =
        std::clamp(((loadedVolume + 2) / 5) * 5, 0, 100);
    if (_wcsicmp(volumeHudPosition, L"TopRight") == 0) {
        g_settings.volumeHudPosition = VolumeHudPosition::TopRight;
    } else if (_wcsicmp(volumeHudPosition, L"BottomLeft") == 0) {
        g_settings.volumeHudPosition = VolumeHudPosition::BottomLeft;
    } else if (_wcsicmp(volumeHudPosition, L"BottomRight") == 0) {
        g_settings.volumeHudPosition = VolumeHudPosition::BottomRight;
    } else {
        g_settings.volumeHudPosition = VolumeHudPosition::TopLeft;
    }
    g_settings.muteWhenBackground =
        wcstol(muteWhenBackground, nullptr, 10) != 0;
    g_volumePercent.store(g_settings.volumePercent,
                          std::memory_order_release);
    g_settings.presentationMode = (_wcsicmp(presentation, L"VSync") == 0)
                                      ? PresentationMode::VSync
                                      : PresentationMode::AllowTearing;
    g_settings.audioOutputDeviceId = audioOutputDeviceId;
    g_settings.captureDeviceId = captureDeviceId;
    if (_wcsicmp(pixelFormat, L"NV12") == 0) {
        g_settings.pixelFormat = VideoPixelFormat::Nv12;
    } else if (_wcsicmp(pixelFormat, L"YUY2") == 0) {
        g_settings.pixelFormat = VideoPixelFormat::Yuy2;
    } else {
        g_settings.pixelFormat = VideoPixelFormat::Auto;
    }
    const int savedFrameRate = static_cast<int>(
        wcstol(frameRate, nullptr, 10));
    g_settings.videoFrameRate = savedFrameRate > 0 ? savedFrameRate : 0;
    g_settings.saveLog = wcstol(saveLog, nullptr, 10) != 0;

    if (_wcsicmp(resolution, L"1920x1080") == 0) {
        g_settings.videoPreset = VideoPreset::R1920x1080;
    } else if (_wcsicmp(resolution, L"3840x2160") == 0) {
        g_settings.videoPreset = VideoPreset::R3840x2160;
    } else {
        g_settings.videoPreset = VideoPreset::R2560x1440;
    }
    g_settings.pixelPerfect = (wcstol(pixelPerfect, nullptr, 10) != 0);
    g_settings.relativeWindowSize =
        (wcstol(relativeWindowSize, nullptr, 10) != 0);
    g_settings.relativeWindowScalePpm = std::clamp(
        static_cast<int>(wcstol(relativeWindowScale, nullptr, 10)),
        0, kRelativeScaleUnit);
    g_settings.borderlessWindow = (wcstol(borderlessWindow, nullptr, 10) != 0);
    g_settings.windowSnap = (wcstol(windowSnap, nullptr, 10) != 0);
    if (windowX[0] && windowY[0]) {
        g_settings.hasWindowPosition = true;
        g_settings.windowX = static_cast<int>(wcstol(windowX, nullptr, 10));
        g_settings.windowY = static_cast<int>(wcstol(windowY, nullptr, 10));
        g_settings.monitorDevice = monitorDevice;
    }
}

static void SaveSettings() {
    const std::wstring path = SettingsPath();
    WritePrivateProfileStringW(
        L"Audio", L"Mode",
        g_settings.audioMode == AudioMode::WasapiExclusive ? L"Exclusive" : L"Shared",
        path.c_str());
    wchar_t bufferMs[16]{};
    swprintf_s(bufferMs, L"%d", g_settings.wasapiBufferMs);
    WritePrivateProfileStringW(L"Audio", L"BufferMs", bufferMs, path.c_str());
    wchar_t sharedPeriodFrames[16]{};
    swprintf_s(sharedPeriodFrames, L"%u", g_settings.wasapiSharedPeriodFrames);
    WritePrivateProfileStringW(L"Audio", L"SharedPeriodFrames",
                               sharedPeriodFrames, path.c_str());
    WritePrivateProfileStringW(
        L"Audio", L"DriftCorrection",
        g_settings.driftCorrection == DriftCorrectionMode::Resample
            ? L"Resample" : L"Off",
        path.c_str());
    wchar_t pcmQueueTargetMs[16]{};
    swprintf_s(pcmQueueTargetMs, L"%d", g_settings.pcmQueueTargetMs);
    WritePrivateProfileStringW(L"Audio", L"PcmQueueTargetMs",
                               pcmQueueTargetMs, path.c_str());
    wchar_t volume[16]{};
    swprintf_s(volume, L"%d", g_settings.volumePercent);
    WritePrivateProfileStringW(L"Audio", L"Volume", volume, path.c_str());
    const wchar_t* hudPosition = L"TopLeft";
    switch (g_settings.volumeHudPosition) {
    case VolumeHudPosition::TopRight: hudPosition = L"TopRight"; break;
    case VolumeHudPosition::BottomLeft: hudPosition = L"BottomLeft"; break;
    case VolumeHudPosition::BottomRight: hudPosition = L"BottomRight"; break;
    default: break;
    }
    WritePrivateProfileStringW(L"Audio", L"VolumeHudPosition", hudPosition,
                               path.c_str());
    WritePrivateProfileStringW(L"Audio", L"MuteWhenBackground",
                               g_settings.muteWhenBackground ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"Audio", L"OutputDeviceId",
                               g_settings.audioOutputDeviceId.c_str(),
                               path.c_str());

    const auto& video = CurrentVideoPreset();
    wchar_t resolution[32]{};
    swprintf_s(resolution, L"%dx%d", video.width, video.height);
    WritePrivateProfileStringW(L"Video", L"Resolution", resolution, path.c_str());
    WritePrivateProfileStringW(L"Video", L"CaptureDeviceId",
                               g_settings.captureDeviceId.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Video", L"PixelFormat",
                               PixelFormatName(g_settings.pixelFormat),
                               path.c_str());
    wchar_t frameRate[16]{};
    swprintf_s(frameRate, L"%d", g_settings.videoFrameRate);
    WritePrivateProfileStringW(L"Video", L"FrameRate", frameRate,
                               path.c_str());
    WritePrivateProfileStringW(
        L"Video", L"Presentation",
        g_settings.presentationMode == PresentationMode::VSync
            ? L"VSync" : L"AllowTearing",
        path.c_str());
    WritePrivateProfileStringW(L"Video", L"PixelPerfect",
                               g_settings.pixelPerfect ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Video", L"RelativeWindowSize",
                               g_settings.relativeWindowSize ? L"1" : L"0",
                               path.c_str());
    wchar_t relativeScale[16]{};
    swprintf_s(relativeScale, L"%d", g_settings.relativeWindowScalePpm);
    WritePrivateProfileStringW(L"Video", L"RelativeWindowScalePpm",
                               relativeScale, path.c_str());
    WritePrivateProfileStringW(L"Video", L"BorderlessWindow",
                               g_settings.borderlessWindow ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Window", L"Snap",
                               g_settings.windowSnap ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Diagnostics", L"SaveLog",
                               g_settings.saveLog ? L"1" : L"0",
                               path.c_str());
}

// -----------------------------------------------------------------------------
// Lock-protected PCM ring buffer.
// The callback copies only the delivered PCM packet and never blocks capture.
// -----------------------------------------------------------------------------

class PcmRing {
public:
    PcmRing() : data_(kRingFrames * kChannels) {}

    void push(const int16_t* samples, size_t frames) {
        std::lock_guard<std::mutex> lock(mu_);

        if (frames >= kRingFrames) {
            samples += (frames - kRingFrames) * kChannels;
            frames = kRingFrames;
            readFrame_ = 0;
            writeFrame_ = 0;
            available_ = 0;
        }

        while (available_ + frames > kRingFrames) {
            // Drop the oldest frame(s), never block capture.
            const size_t drop = std::min(available_ + frames - kRingFrames, available_);
            readFrame_ = (readFrame_ + drop) % kRingFrames;
            available_ -= drop;
            if (OsdTrackingActive()) {
                overruns_++;
                g_audioOverrunFrames.fetch_add(drop,
                                               std::memory_order_relaxed);
                g_audioLastOverrunMs.store(GetTickCount64(),
                                           std::memory_order_release);
            }
        }

        for (size_t i = 0; i < frames; ++i) {
            const size_t dst = ((writeFrame_ + i) % kRingFrames) * kChannels;
            const size_t src = i * kChannels;
            data_[dst] = samples[src];
            data_[dst + 1] = samples[src + 1];
        }

        writeFrame_ = (writeFrame_ + frames) % kRingFrames;
        available_ += frames;
        g_audioRingFrames.store(static_cast<UINT32>(available_),
                                std::memory_order_release);
    }

    size_t pop(int16_t* out, size_t frames) {
        std::lock_guard<std::mutex> lock(mu_);
        const size_t n = std::min(frames, available_);

        for (size_t i = 0; i < n; ++i) {
            const size_t src = ((readFrame_ + i) % kRingFrames) * kChannels;
            const size_t dst = i * kChannels;
            out[dst] = data_[src];
            out[dst + 1] = data_[src + 1];
        }

        readFrame_ = (readFrame_ + n) % kRingFrames;
        available_ -= n;
        g_audioRingFrames.store(static_cast<UINT32>(available_),
                                std::memory_order_release);
        return n;
    }

    size_t availableFrames() const {
        std::lock_guard<std::mutex> lock(mu_);
        return available_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        readFrame_ = 0;
        writeFrame_ = 0;
        available_ = 0;
        g_audioRingFrames.store(0, std::memory_order_release);
    }

    uint64_t overruns() const { return overruns_.load(); }

private:
    std::vector<int16_t> data_;
    mutable std::mutex mu_;
    size_t readFrame_ = 0;
    size_t writeFrame_ = 0;
    size_t available_ = 0;
    std::atomic<uint64_t> overruns_{0};
};

static PcmRing g_ring;
static std::atomic<uint64_t> g_underruns{0};

// Optional clock-drift correction. A short windowed-sinc interpolator changes
// the consumption rate by at most +/-1000 ppm. With correction disabled this
// class is bypassed completely, preserving the original integer PCM samples.
class SincDriftResampler {
public:
    size_t render(int16_t* output, size_t outputFrames, double ratio) {
        if (!output || outputFrames == 0) return 0;
        ratio = std::clamp(ratio, 0.999, 1.001);

        if (!primed_) {
            const size_t wanted = outputFrames + kHalfTaps * 2 + 2;
            const size_t pulled = appendFromRing(wanted);
            if (pulled == 0) return 0;
            const int16_t firstLeft = source_[0];
            const int16_t firstRight = source_[1];
            source_.insert(source_.begin(), kHistoryFrames * kChannels, 0);
            for (int i = 0; i < kHistoryFrames; ++i) {
                source_[static_cast<size_t>(i) * kChannels] = firstLeft;
                source_[static_cast<size_t>(i) * kChannels + 1] = firstRight;
            }
            position_ = static_cast<double>(kHistoryFrames);
            primed_ = true;
        }

        const double lastPosition =
            position_ + ratio * static_cast<double>(outputFrames - 1);
        const size_t requiredFrames =
            static_cast<size_t>(std::floor(lastPosition)) + 1;
        const size_t currentFrames = source_.size() / kChannels;
        if (requiredFrames > currentFrames) {
            appendFromRing(requiredFrames - currentFrames);
        }

        size_t produced = 0;
        const size_t sourceFrames = source_.size() / kChannels;
        while (produced < outputFrames) {
            const double samplePosition =
                position_ - static_cast<double>(kHalfTaps);
            const size_t center =
                static_cast<size_t>(std::floor(samplePosition));
            if (center < static_cast<size_t>(kHalfTaps - 1) ||
                center + kHalfTaps >= sourceFrames) {
                break;
            }
            const double fraction =
                samplePosition - static_cast<double>(center);
            for (int channel = 0; channel < kChannels; ++channel) {
                double sum = 0.0;
                double normalization = 0.0;
                for (int tap = -kHalfTaps + 1; tap <= kHalfTaps; ++tap) {
                    const double distance =
                        static_cast<double>(tap) - fraction;
                    const double weight = WindowedSinc(distance);
                    const size_t index =
                        (center + static_cast<size_t>(tap + kHalfTaps - 1) -
                         static_cast<size_t>(kHalfTaps - 1)) * kChannels +
                        static_cast<size_t>(channel);
                    sum += static_cast<double>(source_[index]) * weight;
                    normalization += weight;
                }
                if (std::abs(normalization) > 1.0e-12) {
                    sum /= normalization;
                }
                const long sample = std::lround(std::clamp(
                    sum, static_cast<double>(INT16_MIN),
                    static_cast<double>(INT16_MAX)));
                output[produced * kChannels + static_cast<size_t>(channel)] =
                    static_cast<int16_t>(sample);
            }
            ++produced;
            position_ += ratio;
        }

        compactHistory();
        g_audioResamplerFrames.store(
            static_cast<UINT32>((std::min)(bufferedFrames(),
                                           static_cast<size_t>(UINT32_MAX))),
            std::memory_order_release);
        return produced;
    }

    size_t bufferedFrames() const {
        const size_t frames = source_.size() / kChannels;
        const size_t consumed = static_cast<size_t>(std::floor(position_));
        return frames > consumed ? frames - consumed : 0;
    }

private:
    static constexpr int kHalfTaps = 8;
    static constexpr int kHistoryFrames = kHalfTaps * 2;
    static constexpr double kPi = 3.14159265358979323846;

    static double Sinc(double value) {
        if (std::abs(value) < 1.0e-12) return 1.0;
        const double radians = kPi * value;
        return std::sin(radians) / radians;
    }

    static double WindowedSinc(double distance) {
        if (std::abs(distance) >= static_cast<double>(kHalfTaps)) return 0.0;
        return Sinc(distance) *
               Sinc(distance / static_cast<double>(kHalfTaps));
    }

    size_t appendFromRing(size_t wantedFrames) {
        const size_t available = g_ring.availableFrames();
        const size_t frames = (std::min)(wantedFrames, available);
        if (frames == 0) return 0;
        transfer_.resize(frames * kChannels);
        const size_t pulled = g_ring.pop(transfer_.data(), frames);
        source_.insert(source_.end(), transfer_.begin(),
                       transfer_.begin() +
                           static_cast<ptrdiff_t>(pulled * kChannels));
        return pulled;
    }

    void compactHistory() {
        const size_t integerPosition =
            static_cast<size_t>(std::floor(position_));
        if (integerPosition <= static_cast<size_t>(kHistoryFrames)) return;
        const size_t dropFrames =
            integerPosition - static_cast<size_t>(kHistoryFrames);
        source_.erase(source_.begin(),
                      source_.begin() +
                          static_cast<ptrdiff_t>(dropFrames * kChannels));
        position_ -= static_cast<double>(dropFrames);
    }

    std::vector<int16_t> source_;
    std::vector<int16_t> transfer_;
    double position_ = 0.0;
    bool primed_ = false;
};

// -----------------------------------------------------------------------------
// Sample Grabber callback
// -----------------------------------------------------------------------------

class SampleGrabberCB final : public ISampleGrabberCB {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ISampleGrabberCB)) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++ref_;
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP SampleCB(double, IMediaSample* pSample) override {
        if (!pSample) return E_POINTER;

        BYTE* p = nullptr;
        HRESULT hr = pSample->GetPointer(&p);
        if (FAILED(hr) || !p) return hr;

        const long bytes = pSample->GetActualDataLength();
        constexpr long bytesPerFrame = kChannels * (kBitsPerSample / 8);
        if (bytes <= 0 || bytes % bytesPerFrame != 0) return S_OK;

        const size_t frames = static_cast<size_t>(bytes / bytesPerFrame);
        const uint64_t nowMs = GetTickCount64();
        const bool track = OsdTrackingActive();
        if (track) {
            uint64_t unsetStart = 0;
            g_audioMonitorStartMs.compare_exchange_strong(
                unsetStart, nowMs, std::memory_order_release,
                std::memory_order_relaxed);
            g_audioCapturePacketFrames.store(static_cast<UINT32>(frames),
                                             std::memory_order_release);
        }
        const auto now = std::chrono::steady_clock::now();
        if (hasPreviousCallback_) {
            const int64_t intervalUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now - previousCallback_).count();
            if (track) {
                g_audioCaptureIntervalUs.store(intervalUs,
                                               std::memory_order_release);
                g_audioCaptureIntervalTotalUs.fetch_add(
                    static_cast<uint64_t>((std::max)(int64_t{0}, intervalUs)),
                    std::memory_order_relaxed);
            }
        }
        previousCallback_ = now;
        hasPreviousCallback_ = true;
        g_audioLastCaptureCallbackMs.store(nowMs,
                                           std::memory_order_release);
        if (track) {
            g_audioCaptureCallbacks.fetch_add(1, std::memory_order_relaxed);
            g_audioCaptureFrames.fetch_add(frames, std::memory_order_relaxed);
        }
        g_ring.push(reinterpret_cast<const int16_t*>(p), frames);
        return S_OK;
    }

    STDMETHODIMP BufferCB(double, BYTE*, long) override {
        return E_NOTIMPL;
    }

private:
    std::atomic<ULONG> ref_{1};
    std::chrono::steady_clock::time_point previousCallback_{};
    bool hasPreviousCallback_ = false;
};

// -----------------------------------------------------------------------------
// DirectShow helpers
// -----------------------------------------------------------------------------

static HRESULT FindCaptureFilter(const std::wstring& selectedId,
                                 IBaseFilter** out,
                                 std::wstring* selectedName = nullptr) {
    if (!out) return E_POINTER;
    *out = nullptr;

    ICreateDevEnum* devEnum = nullptr;
    IEnumMoniker* enumMon = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&devEnum));
    if (FAILED(hr)) return hr;

    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMon, 0);
    if (hr != S_OK) {
        SafeRelease(devEnum);
        return E_FAIL;
    }

    IMoniker* mon = nullptr;
    IMoniker* selectedMoniker = nullptr;
    std::wstring chosenName;
    int chosenRank = 0;
    while (enumMon->Next(1, &mon, nullptr) == S_OK) {
        const std::wstring name = MonikerFriendlyName(mon);
        const std::wstring id = MonikerDisplayName(mon);
        if (!name.empty()) {
            fwprintf(stderr, L"[capture] video device: %s\n", name.c_str());
        }
        int rank = 0;
        if (!selectedId.empty()) {
            if (id == selectedId) rank = 100;
        } else {
            std::wstring lowerName(name);
            std::transform(lowerName.begin(), lowerName.end(),
                           lowerName.begin(), [](wchar_t value) {
                               return static_cast<wchar_t>(std::towlower(value));
                           });
            if (_wcsicmp(name.c_str(), kCaptureName) == 0) {
                rank = 30;
            } else if (lowerName.find(L"gc573") != std::wstring::npos ||
                       lowerName.find(L"live gamer 4k") !=
                           std::wstring::npos) {
                rank = 20;
            } else {
                rank = 10;
            }
        }
        if (rank > chosenRank) {
            SafeRelease(selectedMoniker);
            selectedMoniker = mon;
            selectedMoniker->AddRef();
            chosenName = name;
            chosenRank = rank;
        }
        SafeRelease(mon);
    }

    if (selectedMoniker) {
        hr = selectedMoniker->BindToObject(nullptr, nullptr,
                                           IID_PPV_ARGS(out));
        if (SUCCEEDED(hr)) {
            fwprintf(stderr,
                     L"[capture] selected device: %s%s\n",
                     chosenName.c_str(),
                     selectedId.empty() ? L" (auto)" : L"");
            if (selectedName) *selectedName = chosenName;
        }
    } else {
        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    SafeRelease(selectedMoniker);
    SafeRelease(enumMon);
    SafeRelease(devEnum);
    return hr;
}

static HRESULT FindOutputPinByName(IBaseFilter* filter, const wchar_t* name, IPin** out) {
    if (!filter || !out) return E_POINTER;
    *out = nullptr;

    IEnumPins* e = nullptr;
    HRESULT hr = filter->EnumPins(&e);
    if (FAILED(hr)) return hr;

    IPin* pin = nullptr;
    while (e->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION dir{};
        PIN_INFO info{};
        if (SUCCEEDED(pin->QueryDirection(&dir)) && dir == PINDIR_OUTPUT &&
            SUCCEEDED(pin->QueryPinInfo(&info))) {
            if (info.pFilter) info.pFilter->Release();
            if (_wcsicmp(info.achName, name) == 0) {
                *out = pin;
                e->Release();
                return S_OK;
            }
        }
        pin->Release();
    }

    e->Release();
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static HRESULT GetFirstPin(IBaseFilter* filter, PIN_DIRECTION wanted, IPin** out) {
    if (!filter || !out) return E_POINTER;
    *out = nullptr;

    IEnumPins* e = nullptr;
    HRESULT hr = filter->EnumPins(&e);
    if (FAILED(hr)) return hr;

    IPin* pin = nullptr;
    while (e->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION dir{};
        if (SUCCEEDED(pin->QueryDirection(&dir)) && dir == wanted) {
            *out = pin;
            e->Release();
            return S_OK;
        }
        pin->Release();
    }

    e->Release();
    return E_FAIL;
}

static void DeleteMediaType(AM_MEDIA_TYPE* mediaType);

static HRESULT FindOutputPinByMajorType(IBaseFilter* filter,
                                        const GUID& majorType, IPin** out) {
    if (!filter || !out) return E_POINTER;
    *out = nullptr;
    IEnumPins* pins = nullptr;
    HRESULT hr = filter->EnumPins(&pins);
    if (FAILED(hr)) return hr;

    IPin* pin = nullptr;
    while (pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION direction{};
        if (SUCCEEDED(pin->QueryDirection(&direction)) &&
            direction == PINDIR_OUTPUT) {
            IEnumMediaTypes* types = nullptr;
            if (SUCCEEDED(pin->EnumMediaTypes(&types))) {
                AM_MEDIA_TYPE* type = nullptr;
                while (types->Next(1, &type, nullptr) == S_OK) {
                    const bool matches = type && type->majortype == majorType;
                    DeleteMediaType(type);
                    if (matches) {
                        SafeRelease(types);
                        *out = pin;
                        SafeRelease(pins);
                        return S_OK;
                    }
                }
                SafeRelease(types);
            }
        }
        SafeRelease(pin);
    }
    SafeRelease(pins);
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static void SuggestCaptureBuffer(IPin* audioPin) {
    IAMBufferNegotiation* neg = nullptr;
    if (FAILED(audioPin->QueryInterface(IID_PPV_ARGS(&neg)))) {
        fwprintf(stderr, L"[audio] IAMBufferNegotiation unavailable; driver controls capture buffer.\n");
        return;
    }

    ALLOCATOR_PROPERTIES props{};
    props.cBuffers = 4;
    props.cbBuffer = (kSampleRate * kChannels * (kBitsPerSample / 8) *
                      kRecommendedCaptureBufferMs) / 1000;
    props.cbAlign = 1;
    props.cbPrefix = 0;

    HRESULT hr = neg->SuggestAllocatorProperties(&props);
    if (FAILED(hr)) LogHr(L"IAMBufferNegotiation::SuggestAllocatorProperties", hr);
    else fwprintf(stderr, L"[audio] requested DirectShow capture buffer: %d ms (%ld bytes)\n",
                  kRecommendedCaptureBufferMs, props.cbBuffer);

    neg->Release();
}

static void ReportConnectedAudioAllocator(IPin* inputPin) {
    IMemInputPin* memoryInput = nullptr;
    IMemAllocator* allocator = nullptr;
    ALLOCATOR_PROPERTIES properties{};
    HRESULT hr = inputPin
                     ? inputPin->QueryInterface(IID_PPV_ARGS(&memoryInput))
                     : E_POINTER;
    if (SUCCEEDED(hr)) hr = memoryInput->GetAllocator(&allocator);
    if (SUCCEEDED(hr)) hr = allocator->GetProperties(&properties);
    if (SUCCEEDED(hr)) {
        constexpr LONG bytesPerFrame =
            kChannels * (kBitsPerSample / 8);
        const LONG frames = bytesPerFrame > 0
                                ? properties.cbBuffer / bytesPerFrame : 0;
        g_audioCaptureAllocatorFrames.store(frames,
                                            std::memory_order_release);
        g_audioCaptureAllocatorBuffers.store(properties.cBuffers,
                                             std::memory_order_release);
        fwprintf(stderr,
                 L"[audio] actual DirectShow allocator: %ld buffers x "
                 L"%ld bytes (%ld frames / %.2f ms each)\n",
                 properties.cBuffers, properties.cbBuffer, frames,
                 1000.0 * frames / kSampleRate);
    } else {
        LogHr(L"DirectShow audio allocator query", hr);
    }
    SafeRelease(allocator);
    SafeRelease(memoryInput);
}

// -----------------------------------------------------------------------------
// WASAPI render thread (user-selectable Shared or Exclusive mode)
// -----------------------------------------------------------------------------

class DefaultAudioEndpointNotification final : public IMMNotificationClient {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
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
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                         LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            g_defaultAudioEndpointGeneration.fetch_add(
                1, std::memory_order_acq_rel);
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
};

static double TargetAudioVolumeGain() {
    if (g_backgroundAudioMuted.load(std::memory_order_acquire)) return 0.0;
    return g_volumePercent.load(std::memory_order_acquire) / 100.0;
}

static bool AudioRenderThreadWasapi(AudioMode mode,
                                    bool reinitializingEndpoint) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHr(L"CoInitializeEx(audio render)", hr);
        return false;
    }

    const bool exclusive = mode == AudioMode::WasapiExclusive;
    const bool followDefault = g_settings.audioOutputDeviceId.empty();
    uint64_t watchedDefaultGeneration =
        g_defaultAudioEndpointGeneration.load(std::memory_order_acquire);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioClient3* client3 = nullptr;
    IAudioRenderClient* render = nullptr;
    DefaultAudioEndpointNotification* endpointNotification = nullptr;
    bool notificationRegistered = false;
    bool restartForDefaultChange = false;
    HANDLE eventHandle = nullptr;

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&en));
        if (FAILED(hr)) { LogHr(L"CoCreateInstance(MMDeviceEnumerator)", hr); break; }

        if (followDefault) {
            endpointNotification = new DefaultAudioEndpointNotification();
            hr = en->RegisterEndpointNotificationCallback(
                endpointNotification);
            if (SUCCEEDED(hr)) {
                notificationRegistered = true;
                watchedDefaultGeneration =
                    g_defaultAudioEndpointGeneration.load(
                        std::memory_order_acquire);
            } else {
                LogHr(L"RegisterEndpointNotificationCallback", hr);
                endpointNotification->Release();
                endpointNotification = nullptr;
            }
        }

        hr = GetConfiguredAudioEndpoint(
            en, g_settings.audioOutputDeviceId, &dev);
        if (FAILED(hr)) { LogHr(L"GetConfiguredAudioEndpoint", hr); break; }
        std::wstring outputName = AudioEndpointFriendlyName(dev);
        if (outputName.empty()) {
            outputName = g_settings.audioOutputDeviceId.empty()
                ? L"Windows 기본 장치" : L"선택한 출력 장치";
        }
        fwprintf(stderr, L"[audio] output endpoint: %s%s\n",
                 outputName.c_str(),
                 followDefault ? L" (following Windows default)" : L"");
        SetActiveAudioOutputName(
            outputName + (followDefault ? L" (기본 추적)" : L""));

        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                           reinterpret_cast<void**>(&client));
        if (FAILED(hr)) { LogHr(L"Activate(IAudioClient)", hr); break; }

        WAVEFORMATEX wf = PcmOutputFormat();

        WAVEFORMATEX* closest = nullptr;
        const AUDCLNT_SHAREMODE shareMode = exclusive
                                                 ? AUDCLNT_SHAREMODE_EXCLUSIVE
                                                 : AUDCLNT_SHAREMODE_SHARED;
        hr = client->IsFormatSupported(shareMode, &wf, &closest);
        if (exclusive) {
            if (closest) CoTaskMemFree(closest);
            if (hr != S_OK) {
                LogHr(L"IAudioClient::IsFormatSupported(exclusive)", hr);
                break;
            }
        } else if (FAILED(hr)) {
            // Shared mode can still accept the requested PCM format through
            // the Windows Audio Engine's built-in converter.
            if (closest) CoTaskMemFree(closest);
            LogHr(L"IAudioClient::IsFormatSupported(shared)", hr);
            break;
        } else if (closest) {
            CoTaskMemFree(closest);
        }

        bool usingAudioClient3 = false;
        UINT32 selectedSharedPeriodFrames = 0;
        if (!exclusive) {
            AudioClient3Support support{};
            hr = client->QueryInterface(IID_PPV_ARGS(&client3));
            if (SUCCEEDED(hr)) {
                AudioClientProperties properties{};
                properties.cbSize = sizeof(properties);
                properties.eCategory = AudioCategory_Media;
                hr = client3->SetClientProperties(&properties);
            }
            if (SUCCEEDED(hr)) {
                hr = client3->GetSharedModeEnginePeriod(
                    &wf, &support.defaultFrames, &support.fundamentalFrames,
                    &support.minimumFrames, &support.maximumFrames);
                support.supported = SUCCEEDED(hr);
            }
            if (support.supported) {
                UINT32 requestedFrames = g_settings.wasapiSharedPeriodFrames;
                if (requestedFrames == 0) {
                    requestedFrames = static_cast<UINT32>(
                        g_settings.wasapiBufferMs * kSampleRate / 1000);
                }
                selectedSharedPeriodFrames =
                    ClosestSupportedSharedPeriod(requestedFrames, support);
                hr = client3->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    selectedSharedPeriodFrames, &wf, nullptr);
                if (SUCCEEDED(hr)) {
                    usingAudioClient3 = true;
                    fwprintf(stderr,
                             L"[audio] IAudioClient3 shared period: %u frames "
                             L"(%.2f ms)\n",
                             selectedSharedPeriodFrames,
                             1000.0 * selectedSharedPeriodFrames / kSampleRate);
                } else {
                    LogHr(L"IAudioClient3::InitializeSharedAudioStream", hr);
                }
            } else {
                fwprintf(stderr,
                         L"[audio] IAudioClient3 unavailable for this format; "
                         L"using classic Shared mode.\n");
            }

            if (!usingAudioClient3) {
                // A failed Initialize call can leave an audio client unusable.
                // Reactivate it before falling back to classic Shared mode.
                SafeRelease(client3);
                SafeRelease(client);
                hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                                   nullptr,
                                   reinterpret_cast<void**>(&client));
                if (FAILED(hr)) {
                    LogHr(L"Activate(IAudioClient fallback)", hr);
                    break;
                }
                const REFERENCE_TIME hns =
                    static_cast<REFERENCE_TIME>(g_settings.wasapiBufferMs) *
                    10'000;
                hr = client->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    hns, 0, &wf, nullptr);
                if (FAILED(hr)) {
                    LogHr(L"IAudioClient::Initialize(shared fallback/event)",
                          hr);
                    break;
                }
                fwprintf(stderr,
                         L"[audio] classic WASAPI Shared fallback active.\n");
            }
        } else {
            const REFERENCE_TIME hns =
                static_cast<REFERENCE_TIME>(g_settings.wasapiBufferMs) * 10'000;
            hr = client->Initialize(
                AUDCLNT_SHAREMODE_EXCLUSIVE,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                hns, hns, &wf, nullptr);
            if (FAILED(hr)) {
                LogHr(L"IAudioClient::Initialize(exclusive/event)", hr);
                break;
            }
        }

        eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            fwprintf(stderr, L"CreateEvent failed.\n");
            break;
        }

        hr = client->SetEventHandle(eventHandle);
        if (FAILED(hr)) { LogHr(L"SetEventHandle", hr); break; }

        hr = client->GetService(IID_PPV_ARGS(&render));
        if (FAILED(hr)) { LogHr(L"GetService(IAudioRenderClient)", hr); break; }

        UINT32 bufferFrames = 0;
        hr = client->GetBufferSize(&bufferFrames);
        if (FAILED(hr)) { LogHr(L"GetBufferSize", hr); break; }
        g_audioActualBufferFrames.store(bufferFrames,
                                        std::memory_order_release);

        fwprintf(stderr, L"[audio] WASAPI %s buffer: %u frames (%.2f ms)\n",
                  exclusive ? L"exclusive" : L"shared", bufferFrames,
                  1000.0 * bufferFrames / kSampleRate);
        if (exclusive || !usingAudioClient3) {
            fwprintf(stderr, L"[audio] requested WASAPI buffer: %d ms\n",
                     g_settings.wasapiBufferMs);
        }

        BYTE* p = nullptr;
        hr = render->GetBuffer(bufferFrames, &p);
        if (FAILED(hr)) { LogHr(L"GetBuffer(prime)", hr); break; }
        memset(p, 0, static_cast<size_t>(bufferFrames) * wf.nBlockAlign);
        render->ReleaseBuffer(bufferFrames, 0);

        if (reinitializingEndpoint) {
            // Capture remains live while a new WASAPI client is constructed.
            // Drop everything accumulated during that construction immediately
            // before Start, otherwise every switch permanently adds that setup
            // time to the live audio delay.
            g_ring.clear();
            g_audioResamplerFrames.store(0, std::memory_order_release);
            g_audioMinimumPreRenderFrames.store(UINT32_MAX,
                                                std::memory_order_release);
            fwprintf(stderr,
                     L"[audio] endpoint switch: discarded setup backlog "
                     L"immediately before Start.\n");
        }

        hr = client->Start();
        if (FAILED(hr)) { LogHr(L"IAudioClient::Start", hr); break; }

        fwprintf(stderr, L"[audio] WASAPI %s render running.\n",
                  exclusive ? L"exclusive" : L"shared");
        fwprintf(stderr, L"[audio] clock-drift correction: %s\n",
                 g_settings.driftCorrection ==
                         DriftCorrectionMode::Resample
                     ? L"16-tap windowed-sinc resampling (+/-1000 ppm)"
                     : L"off (unaltered PCM samples)");

        std::vector<int16_t> temp(static_cast<size_t>(bufferFrames) * kChannels);
        SincDriftResampler driftResampler;
        double filteredQueuedFrames = -1.0;
        double correctionPpm = 0.0;
        double currentVolumeGain = TargetAudioVolumeGain();
        bool audioStarted = false;
        const UINT32 queueTargetFrames = static_cast<UINT32>(
            g_settings.pcmQueueTargetMs * kSampleRate / 1000);
        g_audioQueueTargetFrames.store(queueTargetFrames,
                                       std::memory_order_release);
        while (g_running.load()) {
            if (followDefault && notificationRegistered &&
                g_defaultAudioEndpointGeneration.load(
                    std::memory_order_acquire) !=
                    watchedDefaultGeneration) {
                restartForDefaultChange = true;
                fwprintf(stderr,
                         L"[audio] Windows default output changed; "
                         L"reinitializing WASAPI only.\n");
                break;
            }
            const DWORD waitResult = WaitForSingleObject(eventHandle, 100);
            if (waitResult != WAIT_OBJECT_0) continue;

            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) continue;
            g_audioWasapiPaddingFrames.store(padding,
                                             std::memory_order_release);

            const UINT32 writable = bufferFrames > padding ? bufferFrames - padding : 0;
            if (!writable) continue;

            BYTE* out = nullptr;
            hr = render->GetBuffer(writable, &out);
            if (FAILED(hr)) continue;

            size_t got = 0;
            const size_t availableBeforeRender =
                g_ring.availableFrames() + driftResampler.bufferedFrames();
            if (audioStarted && OsdTrackingActive()) {
                UINT32 observed = static_cast<UINT32>((std::min)(
                    availableBeforeRender,
                    static_cast<size_t>(UINT32_MAX)));
                UINT32 previousMinimum = g_audioMinimumPreRenderFrames.load(
                    std::memory_order_relaxed);
                while (observed < previousMinimum &&
                       !g_audioMinimumPreRenderFrames.compare_exchange_weak(
                           previousMinimum, observed,
                           std::memory_order_release,
                           std::memory_order_relaxed)) {}
            }
            if (g_settings.driftCorrection ==
                DriftCorrectionMode::Resample) {
                const double targetFrames =
                    static_cast<double>(queueTargetFrames);
                const double queuedFrames = static_cast<double>(
                    g_audioRingFrames.load(std::memory_order_acquire) +
                    static_cast<UINT32>((std::min)(
                        driftResampler.bufferedFrames(),
                        static_cast<size_t>(UINT32_MAX))));
                if (!audioStarted && queuedFrames >= targetFrames) {
                    audioStarted = true;
                }
                if (filteredQueuedFrames < 0.0) {
                    filteredQueuedFrames = queuedFrames;
                } else {
                    filteredQueuedFrames +=
                        (queuedFrames - filteredQueuedFrames) * 0.02;
                }
                const double requestedPpm = std::clamp(
                    (filteredQueuedFrames - targetFrames) * 2.0,
                    -1000.0, 1000.0);
                correctionPpm += (requestedPpm - correctionPpm) * 0.02;
                const double ratio = 1.0 + correctionPpm / 1'000'000.0;
                if (audioStarted) {
                    got = driftResampler.render(temp.data(), writable, ratio);
                }
                g_audioResamplePpm.store(
                    static_cast<int>(std::lround(correctionPpm)),
                    std::memory_order_release);
                g_audioResampledOutputFrames.fetch_add(
                    got, std::memory_order_relaxed);
            } else {
                if (!audioStarted &&
                    g_ring.availableFrames() >= queueTargetFrames) {
                    audioStarted = true;
                }
                if (audioStarted) {
                    got = g_ring.pop(temp.data(), writable);
                }
                g_audioResamplePpm.store(0, std::memory_order_release);
                g_audioResamplerFrames.store(0, std::memory_order_release);
            }
            const double targetVolumeGain = TargetAudioVolumeGain();
            if (got && (currentVolumeGain != 1.0 ||
                        targetVolumeGain != 1.0)) {
                const double gainStep =
                    (targetVolumeGain - currentVolumeGain) /
                    static_cast<double>(got);
                double gain = currentVolumeGain;
                for (size_t frame = 0; frame < got; ++frame) {
                    gain += gainStep;
                    for (int channel = 0; channel < kChannels; ++channel) {
                        const size_t index =
                            frame * kChannels +
                            static_cast<size_t>(channel);
                        const long scaled = std::lround(
                            static_cast<double>(temp[index]) * gain);
                        temp[index] = static_cast<int16_t>(std::clamp(
                            scaled, static_cast<long>(INT16_MIN),
                            static_cast<long>(INT16_MAX)));
                    }
                }
            }
            currentVolumeGain = targetVolumeGain;
            if (got) memcpy(out, temp.data(), got * wf.nBlockAlign);
            if (got < writable) {
                memset(out + got * wf.nBlockAlign, 0,
                       (writable - static_cast<UINT32>(got)) * wf.nBlockAlign);
                if (audioStarted && OsdTrackingActive()) {
                    g_underruns++;
                    g_audioUnderrunFrames.fetch_add(
                        writable - static_cast<UINT32>(got),
                        std::memory_order_relaxed);
                    g_audioLastUnderrunMs.store(
                        GetTickCount64(), std::memory_order_release);
                    if (g_settings.driftCorrection ==
                            DriftCorrectionMode::Resample &&
                        availableBeforeRender >= writable) {
                        // PCM existed for this render request, but the sinc
                        // filter could not produce every output frame (usually
                        // insufficient look-ahead at the lowest queue target).
                        g_audioResamplerUnderruns.fetch_add(
                            1, std::memory_order_relaxed);
                    } else {
                        const uint64_t callbackMs =
                            g_audioLastCaptureCallbackMs.load(
                                std::memory_order_acquire);
                        const UINT32 packetFrames =
                            g_audioCapturePacketFrames.load(
                                std::memory_order_acquire);
                        const uint64_t lateThresholdMs = packetFrames
                            ? 5 + (1000ull * packetFrames / kSampleRate)
                            : 15;
                        const uint64_t nowMs = GetTickCount64();
                        if (callbackMs &&
                            nowMs > callbackMs + lateThresholdMs) {
                            g_audioLatePacketUnderruns.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                }
            }
            render->ReleaseBuffer(writable, 0);
        }

        client->Stop();
    } while (false);

    if (eventHandle) CloseHandle(eventHandle);
    SafeRelease(render);
    SafeRelease(client3);
    SafeRelease(client);
    SafeRelease(dev);
    if (en && notificationRegistered && endpointNotification) {
        en->UnregisterEndpointNotificationCallback(endpointNotification);
    }
    if (endpointNotification) endpointNotification->Release();
    SafeRelease(en);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
    if (followDefault && notificationRegistered &&
        g_defaultAudioEndpointGeneration.load(std::memory_order_acquire) !=
            watchedDefaultGeneration) {
        restartForDefaultChange = true;
    }
    return restartForDefaultChange;
}

static void AudioRenderThread() {
    bool reinitializingEndpoint = false;
    while (g_running.load(std::memory_order_acquire)) {
        const bool restart = AudioRenderThreadWasapi(
            g_settings.audioMode, reinitializingEndpoint);
        if (!restart || !g_running.load(std::memory_order_acquire)) break;
        reinitializingEndpoint = true;
    }
}

// -----------------------------------------------------------------------------
// Direct video path: DirectShow NV12 -> latest frame -> D3D11 video processor
// -> DXGI flip-discard swapchain. No decoder or external player is involved.
// -----------------------------------------------------------------------------

class LatestNv12Sample {
public:
    LatestNv12Sample(size_t expectedBytes, HANDLE readyEvent)
        : expectedBytes_(expectedBytes), readyEvent_(readyEvent) {}

    ~LatestNv12Sample() {
        if (latest_) latest_->Release();
    }

    void push(IMediaSample* sample) {
        if (!sample || sample->GetActualDataLength() <
                           static_cast<long>(expectedBytes_)) return;
        const int64_t arrivalUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        sample->AddRef();
        IMediaSample* replaced = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            replaced = latest_;
            latest_ = sample;
            latestArrivalUs_ = arrivalUs;
        }
        if (replaced) {
            replaced->Release();
            if (OsdTrackingActive()) {
                g_videoReplacedFrames.fetch_add(1,
                                                std::memory_order_relaxed);
            }
        }
        if (OsdTrackingActive()) {
            g_videoCapturedFrames.fetch_add(1, std::memory_order_relaxed);
        }
        SetEvent(readyEvent_);
    }

    IMediaSample* takeLatest(int64_t& arrivalUs) {
        std::lock_guard<std::mutex> lock(mu_);
        IMediaSample* sample = latest_;
        latest_ = nullptr;
        if (!sample) return nullptr;
        arrivalUs = latestArrivalUs_;
        return sample;
    }

private:
    std::mutex mu_;
    IMediaSample* latest_ = nullptr;
    size_t expectedBytes_ = 0;
    HANDLE readyEvent_ = nullptr;
    int64_t latestArrivalUs_ = 0;
};

class VideoSampleGrabberCB final : public ISampleGrabberCB {
public:
    explicit VideoSampleGrabberCB(LatestNv12Sample* sampleSlot)
        : sampleSlot_(sampleSlot) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ISampleGrabberCB)) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG value = --ref_;
        if (!value) delete this;
        return value;
    }
    STDMETHODIMP SampleCB(double, IMediaSample* sample) override {
        if (!surfaceCapabilityProbed_.exchange(true,
                                                std::memory_order_acq_rel)) {
            IMediaSample2Config* surfaceConfig = nullptr;
            IUnknown* surface = nullptr;
            const HRESULT configHr = sample->QueryInterface(
                IID_PPV_ARGS(&surfaceConfig));
            const HRESULT surfaceHr = SUCCEEDED(configHr)
                ? surfaceConfig->GetSurface(&surface) : configHr;
            fwprintf(stderr,
                     L"[video] DirectShow VRAM sample surface: %s "
                     L"(interface 0x%08X, surface 0x%08X)\n",
                     SUCCEEDED(surfaceHr) && surface ? L"available"
                                                     : L"not available",
                     static_cast<unsigned>(configHr),
                     static_cast<unsigned>(surfaceHr));
            SafeRelease(surface);
            SafeRelease(surfaceConfig);
        }
        if (sampleSlot_) sampleSlot_->push(sample);
        return S_OK;
    }
    STDMETHODIMP BufferCB(double, BYTE*, long) override { return E_NOTIMPL; }

private:
    std::atomic<ULONG> ref_{1};
    std::atomic<bool> surfaceCapabilityProbed_{false};
    LatestNv12Sample* sampleSlot_ = nullptr;
};

static void DeleteMediaType(AM_MEDIA_TYPE* mediaType) {
    if (!mediaType) return;
    FreeMediaType(*mediaType);
    CoTaskMemFree(mediaType);
}

static bool VideoFormatDetails(const AM_MEDIA_TYPE* mediaType, int& width,
                               int& height, REFERENCE_TIME& frameDuration,
                               DWORD& imageBytes,
                               VideoPixelFormat* pixelFormat = nullptr) {
    if (!mediaType || mediaType->majortype != MEDIATYPE_Video ||
        (mediaType->subtype != MEDIASUBTYPE_NV12 &&
         mediaType->subtype != MEDIASUBTYPE_YUY2)) {
        return false;
    }
    if (pixelFormat) {
        *pixelFormat = mediaType->subtype == MEDIASUBTYPE_YUY2
                           ? VideoPixelFormat::Yuy2
                           : VideoPixelFormat::Nv12;
    }
    const BITMAPINFOHEADER* bitmap = nullptr;
    frameDuration = 0;
    if (mediaType->formattype == FORMAT_VideoInfo2 &&
        mediaType->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        const auto* info =
            reinterpret_cast<const VIDEOINFOHEADER2*>(mediaType->pbFormat);
        bitmap = &info->bmiHeader;
        frameDuration = info->AvgTimePerFrame;
    } else if (mediaType->formattype == FORMAT_VideoInfo &&
               mediaType->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        const auto* info =
            reinterpret_cast<const VIDEOINFOHEADER*>(mediaType->pbFormat);
        bitmap = &info->bmiHeader;
        frameDuration = info->AvgTimePerFrame;
    }
    if (!bitmap) return false;
    width = static_cast<int>(bitmap->biWidth);
    height = std::abs(static_cast<int>(bitmap->biHeight));
    imageBytes = bitmap->biSizeImage;
    return width > 0 && height > 0;
}

static HRESULT ConfigureVideoPin(IPin* videoPin, int wantedWidth,
                                 int wantedHeight, int wantedFps,
                                 VideoPixelFormat wantedFormat,
                                 DWORD& imageBytes, UINT32& stride,
                                 int& configuredFps,
                                 VideoPixelFormat& configuredFormat) {
    IAMStreamConfig* config = nullptr;
    HRESULT hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    if (FAILED(hr)) return hr;

    int count = 0;
    int capBytes = 0;
    hr = config->GetNumberOfCapabilities(&count, &capBytes);
    if (FAILED(hr) || capBytes < static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS))) {
        SafeRelease(config);
        return FAILED(hr) ? hr : E_FAIL;
    }

    struct FormatCandidate {
        AM_MEDIA_TYPE* mediaType = nullptr;
        int fps = 0;
        DWORD imageBytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Nv12;
    };
    std::vector<BYTE> caps(static_cast<size_t>(capBytes));
    std::vector<FormatCandidate> candidates;
    for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* candidate = nullptr;
        hr = config->GetStreamCaps(i, &candidate, caps.data());
        if (FAILED(hr) || !candidate) continue;

        int width = 0;
        int height = 0;
        REFERENCE_TIME duration = 0;
        DWORD bytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Nv12;
        const bool details = VideoFormatDetails(candidate, width, height,
                                                duration, bytes, &format);
        const int fps = duration > 0
                            ? static_cast<int>((10'000'000 + duration / 2) /
                                               duration)
                            : 0;
        const bool formatMatches =
            wantedFormat == VideoPixelFormat::Auto || format == wantedFormat;
        if (details && formatMatches && width == wantedWidth &&
            height == wantedHeight && fps > 0) {
            candidates.push_back({candidate, fps, bytes, format});
            continue;
        }
        DeleteMediaType(candidate);
    }

    if (candidates.empty()) {
        SafeRelease(config);
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    // Prefer the requested rate. Otherwise keep the exact pixel dimensions
    // and select the highest supported rate not exceeding the request. If the
    // driver exposes only higher rates, select the lowest of those.
    std::stable_sort(
        candidates.begin(), candidates.end(),
        [wantedFps, wantedFormat](const FormatCandidate& a,
                                 const FormatCandidate& b) {
            if (wantedFormat == VideoPixelFormat::Auto &&
                a.format != b.format) {
                return a.format == VideoPixelFormat::Nv12;
            }
            if (a.fps == wantedFps || b.fps == wantedFps) {
                return a.fps == wantedFps && b.fps != wantedFps;
            }
            const bool aWithin = a.fps < wantedFps;
            const bool bWithin = b.fps < wantedFps;
            if (aWithin != bWithin) return aWithin;
            return aWithin ? a.fps > b.fps : a.fps < b.fps;
        });

    hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    FormatCandidate* selected = nullptr;
    for (auto& candidate : candidates) {
        hr = config->SetFormat(candidate.mediaType);
        if (SUCCEEDED(hr)) {
            selected = &candidate;
            break;
        }
    }

    if (selected) {
        imageBytes = selected->imageBytes;
        configuredFps = selected->fps;
        configuredFormat = selected->format;
        if (!imageBytes) {
            imageBytes = static_cast<DWORD>(
                configuredFormat == VideoPixelFormat::Yuy2
                    ? wantedWidth * wantedHeight * 2
                    : wantedWidth * wantedHeight * 3 / 2);
        }
        const uint64_t derivedStride = configuredFormat ==
                                                VideoPixelFormat::Yuy2
            ? static_cast<uint64_t>(imageBytes) / wantedHeight
            : (static_cast<uint64_t>(imageBytes) * 2) /
                  (static_cast<uint64_t>(wantedHeight) * 3);
        const UINT32 minimumStride = configuredFormat ==
                                             VideoPixelFormat::Yuy2
            ? static_cast<UINT32>(wantedWidth * 2)
            : static_cast<UINT32>(wantedWidth);
        stride = static_cast<UINT32>(derivedStride >=
                                             minimumStride
                                         ? derivedStride
                                         : minimumStride);
        if (configuredFps != wantedFps) {
            fwprintf(stderr,
                     L"[video] requested %s %dx%d @ %d unavailable; "
                     L"using %d fps at the exact resolution.\n",
                     PixelFormatName(configuredFormat), wantedWidth,
                     wantedHeight, wantedFps, configuredFps);
        }
    }
    for (auto& candidate : candidates) {
        DeleteMediaType(candidate.mediaType);
    }
    SafeRelease(config);
    return hr;
}

static std::vector<PixelFormatSupport> ProbePixelFormats(
    const std::wstring& captureDeviceId, int width, int height) {
    std::vector<PixelFormatSupport> result;
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initHr);
    if (initHr == RPC_E_CHANGED_MODE) initHr = S_OK;
    if (FAILED(initHr)) return result;
    IBaseFilter* capture = nullptr;
    IPin* videoPin = nullptr;
    IAMStreamConfig* config = nullptr;
    HRESULT hr = FindCaptureFilter(captureDeviceId, &capture);
    if (SUCCEEDED(hr)) {
        hr = FindOutputPinByMajorType(capture, MEDIATYPE_Video, &videoPin);
    }
    if (SUCCEEDED(hr)) hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    int count = 0;
    int capBytes = 0;
    if (SUCCEEDED(hr)) hr = config->GetNumberOfCapabilities(&count, &capBytes);
    std::vector<BYTE> caps(capBytes > 0 ? static_cast<size_t>(capBytes) : 1);
    for (int i = 0; SUCCEEDED(hr) && i < count; ++i) {
        AM_MEDIA_TYPE* mediaType = nullptr;
        if (FAILED(config->GetStreamCaps(i, &mediaType, caps.data())) ||
            !mediaType) continue;
        int candidateWidth = 0;
        int candidateHeight = 0;
        REFERENCE_TIME duration = 0;
        DWORD bytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Auto;
        if (VideoFormatDetails(mediaType, candidateWidth, candidateHeight,
                               duration, bytes, &format) &&
            candidateWidth == width && candidateHeight == height &&
            duration > 0) {
            const int fps = static_cast<int>(
                (10'000'000 + duration / 2) / duration);
            const bool duplicate = std::any_of(
                result.begin(), result.end(),
                [format, fps](const PixelFormatSupport& support) {
                    return support.format == format &&
                           support.selectedFps == fps;
                });
            if (!duplicate) {
                result.push_back({format, fps});
            }
        }
        DeleteMediaType(mediaType);
    }
    std::sort(result.begin(), result.end(),
              [](const PixelFormatSupport& left,
                 const PixelFormatSupport& right) {
                  if (left.format != right.format) {
                      return left.format == VideoPixelFormat::Nv12;
                  }
                  return left.selectedFps > right.selectedFps;
              });
    SafeRelease(config);
    SafeRelease(videoPin);
    SafeRelease(capture);
    if (uninitialize) CoUninitialize();
    return result;
}

static void UpdateConfiguredVideoTitle(HWND videoHost, int configuredFps) {
    HWND root = GetAncestor(videoHost, GA_ROOT);
    if (!root) return;
    const auto& video = CurrentVideoPreset();
    const wchar_t* audioLabel =
        g_settings.audioMode == AudioMode::WasapiExclusive
            ? L"WASAPI Exclusive" : L"WASAPI Shared";
    const wchar_t* presentationLabel =
        g_settings.presentationMode == PresentationMode::VSync
            ? L"VSync" : L"Immediate";
    const auto configuredFormat = static_cast<VideoPixelFormat>(
        g_activePixelFormat.load(std::memory_order_acquire));
    wchar_t title[512]{};
    const int requestedFps = RequestedVideoFrameRate();
    if (configuredFps == requestedFps) {
        swprintf_s(
            title,
            L"Low Latency Capture Viewer - %s - %dx%d @ %dfps %s - %s - %s",
            g_activeCaptureDeviceName.c_str(), video.width, video.height,
            configuredFps, PixelFormatName(configuredFormat), audioLabel,
            presentationLabel);
    } else {
        swprintf_s(
            title,
            L"Low Latency Capture Viewer - %s - %dx%d @ %dfps %s "
            L"(auto; requested %d) - %s - %s",
            g_activeCaptureDeviceName.c_str(), video.width, video.height,
            configuredFps, PixelFormatName(configuredFormat),
            requestedFps, audioLabel, presentationLabel);
    }
    SetWindowTextW(root, title);
}

static std::wstring BuildRuntimeOsdText(int outputWidth, int outputHeight);

struct DirectD3D11Renderer {
    static constexpr UINT kUploadSurfaceCount = 3;
    static constexpr UINT kOsdOverlayWidth = 700;
    static constexpr UINT kOsdOverlayHeight = 420;
    static constexpr float kOsdTextWidth = 668.0f;
    static constexpr float kOsdTextHeight = 394.0f;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11DeviceContext1* context1 = nullptr;
    ID3D11VideoDevice* videoDevice = nullptr;
    ID3D11VideoContext* videoContext = nullptr;
    IDXGISwapChain1* swapChain = nullptr;
    ID3D11Texture2D* nv12Textures[kUploadSurfaceCount]{};
    ID3D11VideoProcessorEnumerator* enumerator = nullptr;
    ID3D11VideoProcessor* processor = nullptr;
    ID3D11VideoProcessorInputView* inputViews[kUploadSurfaceCount]{};
    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* backBufferRenderTarget = nullptr;
    ID3D11VideoProcessorOutputView* outputView = nullptr;
    ID2D1Factory* d2dFactory = nullptr;
    ID2D1RenderTarget* osdCacheTarget = nullptr;
    ID2D1RenderTarget* volumeCacheTarget = nullptr;
    ID2D1SolidColorBrush* osdCacheBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* osdCacheTextBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheTextBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheBarBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheBarBrush = nullptr;
    ID3D11Texture2D* osdOverlayTexture = nullptr;
    ID3D11Texture2D* volumeOverlayTexture = nullptr;
    ID3D11ShaderResourceView* osdOverlayShaderView = nullptr;
    ID3D11ShaderResourceView* volumeOverlayShaderView = nullptr;
    ID3D11VertexShader* overlayVertexShader = nullptr;
    ID3D11PixelShader* overlayPixelShader = nullptr;
    ID3D11Buffer* overlayRectBuffer = nullptr;
    ID3D11SamplerState* overlaySampler = nullptr;
    ID3D11BlendState* overlayBlendState = nullptr;
    IDWriteFactory* dwriteFactory = nullptr;
    IDWriteTextFormat* osdTextFormat = nullptr;
    IDWriteTextFormat* volumeTextFormat = nullptr;
    IDWriteTextLayout* osdTextLayout = nullptr;
    IDWriteTextLayout* volumeTextLayout = nullptr;
    UINT outputWidth = 0;
    UINT outputHeight = 0;
    uint64_t cachedOverlayGeneration = 0;
    UINT nextUploadSurface = 0;
    UINT activeUploadSurface = 0;
    bool allowTearing = false;
    bool discardUpdateAvailable = false;
    bool occluded = false;
    uint64_t nextOcclusionTestMs = 0;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_NV12;

    void reset() {
        if (context) {
            context->ClearState();
            context->Flush();
        }
        SafeRelease(overlayBlendState);
        SafeRelease(overlaySampler);
        SafeRelease(overlayRectBuffer);
        SafeRelease(overlayPixelShader);
        SafeRelease(overlayVertexShader);
        SafeRelease(volumeOverlayShaderView);
        SafeRelease(osdOverlayShaderView);
        SafeRelease(volumeOverlayTexture);
        SafeRelease(osdOverlayTexture);
        SafeRelease(volumeCacheBarBrush);
        SafeRelease(volumeCacheBarBackgroundBrush);
        SafeRelease(volumeCacheTextBrush);
        SafeRelease(volumeCacheBackgroundBrush);
        SafeRelease(osdCacheTextBrush);
        SafeRelease(osdCacheBackgroundBrush);
        SafeRelease(volumeCacheTarget);
        SafeRelease(osdCacheTarget);
        SafeRelease(volumeTextLayout);
        SafeRelease(osdTextLayout);
        SafeRelease(volumeTextFormat);
        SafeRelease(osdTextFormat);
        SafeRelease(dwriteFactory);
        SafeRelease(d2dFactory);
        SafeRelease(outputView);
        SafeRelease(backBufferRenderTarget);
        SafeRelease(backBuffer);
        for (UINT i = 0; i < kUploadSurfaceCount; ++i) {
            SafeRelease(inputViews[i]);
        }
        SafeRelease(processor);
        SafeRelease(enumerator);
        for (UINT i = 0; i < kUploadSurfaceCount; ++i) {
            SafeRelease(nv12Textures[i]);
        }
        SafeRelease(swapChain);
        SafeRelease(videoContext);
        SafeRelease(videoDevice);
        SafeRelease(context1);
        SafeRelease(context);
        SafeRelease(device);
        outputWidth = 0;
        outputHeight = 0;
        cachedOverlayGeneration = 0;
        nextUploadSurface = 0;
        activeUploadSurface = 0;
        allowTearing = false;
        discardUpdateAvailable = false;
        occluded = false;
        nextOcclusionTestMs = 0;
        inputFormat = DXGI_FORMAT_NV12;
    }

    ~DirectD3D11Renderer() {
        reset();
    }

    HRESULT deviceRemovedReason() const {
        return device ? device->GetDeviceRemovedReason() : E_POINTER;
    }

    HRESULT initialize(HWND hwnd, int width, int height, int fps,
                       VideoPixelFormat pixelFormat) {
        reset();
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        outputWidth = static_cast<UINT>((std::max)(
            1L, clientRect.right - clientRect.left));
        outputHeight = static_cast<UINT>((std::max)(
            1L, clientRect.bottom - clientRect.top));
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL featureLevel{};
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
            D3D11_SDK_VERSION, &device, &featureLevel, &context);
        if (FAILED(hr)) return hr;
        discardUpdateAvailable =
            SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1)));
        hr = device->QueryInterface(IID_PPV_ARGS(&videoDevice));
        if (FAILED(hr)) return hr;
        hr = context->QueryInterface(IID_PPV_ARGS(&videoContext));
        if (FAILED(hr)) return hr;

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            SafeRelease(factory);
            SafeRelease(adapter);
            SafeRelease(dxgiDevice);
            return hr;
        }

        IDXGIFactory5* factory5 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
            BOOL supported = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported,
                    sizeof(supported)))) {
                allowTearing = supported == TRUE;
            }
        }
        SafeRelease(factory5);

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = outputWidth;
        swapDesc.Height = outputHeight;
        swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (allowTearing) swapDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        hr = factory->CreateSwapChainForHwnd(device, hwnd, &swapDesc, nullptr,
                                             nullptr, &swapChain);
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        SafeRelease(factory);
        SafeRelease(adapter);
        SafeRelease(dxgiDevice);
        if (FAILED(hr)) return hr;

        IDXGISwapChain2* swapChain2 = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain2)))) {
            hr = swapChain2->SetMaximumFrameLatency(1);
            SafeRelease(swapChain2);
            if (FAILED(hr)) return hr;
        }

        IDXGIDevice1* dxgiDevice1 = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice1)))) {
            dxgiDevice1->SetMaximumFrameLatency(1);
        }
        SafeRelease(dxgiDevice1);

        inputFormat = PixelFormatDxgi(pixelFormat);
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = static_cast<UINT>(width);
        textureDesc.Height = static_cast<UINT>(height);
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = inputFormat;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        for (UINT i = 0; i < kUploadSurfaceCount; ++i) {
            hr = device->CreateTexture2D(&textureDesc, nullptr,
                                         &nv12Textures[i]);
            if (FAILED(hr)) return hr;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {static_cast<UINT>(fps), 1};
        content.InputWidth = static_cast<UINT>(width);
        content.InputHeight = static_cast<UINT>(height);
        content.OutputFrameRate = {static_cast<UINT>(fps), 1};
        content.OutputWidth = outputWidth;
        content.OutputHeight = outputHeight;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        hr = videoDevice->CreateVideoProcessorEnumerator(&content, &enumerator);
        if (FAILED(hr)) return hr;
        UINT formatSupport = 0;
        hr = enumerator->CheckVideoProcessorFormat(inputFormat,
                                                   &formatSupport);
        if (FAILED(hr) ||
            (formatSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
            return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        hr = videoDevice->CreateVideoProcessor(enumerator, 0, &processor);
        if (FAILED(hr)) return hr;

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.FourCC = 0;
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        for (UINT i = 0; i < kUploadSurfaceCount; ++i) {
            hr = videoDevice->CreateVideoProcessorInputView(
                nv12Textures[i], enumerator, &inputDesc, &inputViews[i]);
            if (FAILED(hr)) return hr;
        }
        hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) return hr;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = videoDevice->CreateVideoProcessorOutputView(
            backBuffer, enumerator, &outputDesc, &outputView);
        if (FAILED(hr)) return hr;
        hr = device->CreateRenderTargetView(
            backBuffer, nullptr, &backBufferRenderTarget);
        if (FAILED(hr)) return hr;

        D3D11_TEXTURE2D_DESC overlayDesc{};
        overlayDesc.Width = kOsdOverlayWidth;
        overlayDesc.Height = kOsdOverlayHeight;
        overlayDesc.MipLevels = 1;
        overlayDesc.ArraySize = 1;
        overlayDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        overlayDesc.SampleDesc.Count = 1;
        overlayDesc.Usage = D3D11_USAGE_DEFAULT;
        overlayDesc.BindFlags = D3D11_BIND_RENDER_TARGET |
                                D3D11_BIND_SHADER_RESOURCE;
        hr = device->CreateTexture2D(&overlayDesc, nullptr,
                                     &osdOverlayTexture);
        if (FAILED(hr)) return hr;
        overlayDesc.Width = 260;
        overlayDesc.Height = 82;
        hr = device->CreateTexture2D(&overlayDesc, nullptr,
                                     &volumeOverlayTexture);
        if (FAILED(hr)) return hr;
        hr = device->CreateShaderResourceView(
            osdOverlayTexture, nullptr, &osdOverlayShaderView);
        if (SUCCEEDED(hr)) {
            hr = device->CreateShaderResourceView(
                volumeOverlayTexture, nullptr, &volumeOverlayShaderView);
        }
        if (FAILED(hr)) return hr;
        static constexpr char overlayVertexSource[] =
            "cbuffer RectBuffer : register(b0) { float4 rect; };"
            "struct VSOut { float4 position : SV_POSITION; "
            "float2 uv : TEXCOORD0; };"
            "VSOut main(uint id : SV_VertexID) {"
            "float2 positions[4] = { float2(0,0), float2(1,0), "
            "float2(0,1), float2(1,1) };"
            "VSOut o; float2 p = positions[id];"
            "o.position = float4(lerp(rect.x, rect.z, p.x), "
            "lerp(rect.y, rect.w, p.y), 0, 1); o.uv = p; return o; }";
        static constexpr char overlayPixelSource[] =
            "Texture2D overlayTexture : register(t0);"
            "SamplerState overlaySampler : register(s0);"
            "float4 main(float4 position : SV_POSITION, "
            "float2 uv : TEXCOORD0) : SV_TARGET {"
            "return overlayTexture.Sample(overlaySampler, uv); }";
        ID3DBlob* vertexBlob = nullptr;
        ID3DBlob* pixelBlob = nullptr;
        ID3DBlob* shaderErrors = nullptr;
        hr = D3DCompile(overlayVertexSource,
                        sizeof(overlayVertexSource) - 1, nullptr, nullptr,
                        nullptr, "main", "vs_4_0", 0, 0, &vertexBlob,
                        &shaderErrors);
        SafeRelease(shaderErrors);
        if (SUCCEEDED(hr)) {
            hr = D3DCompile(overlayPixelSource,
                            sizeof(overlayPixelSource) - 1, nullptr, nullptr,
                            nullptr, "main", "ps_4_0", 0, 0, &pixelBlob,
                            &shaderErrors);
        }
        SafeRelease(shaderErrors);
        if (SUCCEEDED(hr)) {
            hr = device->CreateVertexShader(
                vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                nullptr, &overlayVertexShader);
        }
        if (SUCCEEDED(hr)) {
            hr = device->CreatePixelShader(
                pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(),
                nullptr, &overlayPixelShader);
        }
        SafeRelease(pixelBlob);
        SafeRelease(vertexBlob);
        if (FAILED(hr)) return hr;

        D3D11_BUFFER_DESC constantBufferDesc{};
        constantBufferDesc.ByteWidth = sizeof(float) * 4;
        constantBufferDesc.Usage = D3D11_USAGE_DEFAULT;
        constantBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device->CreateBuffer(&constantBufferDesc, nullptr,
                                  &overlayRectBuffer);
        if (FAILED(hr)) return hr;
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&samplerDesc, &overlaySampler);
        if (FAILED(hr)) return hr;
        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend =
            D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha =
            D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&blendDesc, &overlayBlendState);
        if (FAILED(hr)) return hr;

        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               &d2dFactory);
        if (FAILED(hr)) return hr;
        const D2D1_RENDER_TARGET_PROPERTIES d2dProperties =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_HARDWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED));
        IDXGISurface* osdSurface = nullptr;
        IDXGISurface* volumeSurface = nullptr;
        hr = osdOverlayTexture->QueryInterface(IID_PPV_ARGS(&osdSurface));
        if (SUCCEEDED(hr)) {
            hr = volumeOverlayTexture->QueryInterface(
                IID_PPV_ARGS(&volumeSurface));
        }
        if (SUCCEEDED(hr)) {
            hr = d2dFactory->CreateDxgiSurfaceRenderTarget(
                osdSurface, &d2dProperties, &osdCacheTarget);
        }
        if (SUCCEEDED(hr)) {
            hr = d2dFactory->CreateDxgiSurfaceRenderTarget(
                volumeSurface, &d2dProperties, &volumeCacheTarget);
        }
        SafeRelease(volumeSurface);
        SafeRelease(osdSurface);
        if (FAILED(hr)) return hr;
        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&dwriteFactory));
        if (FAILED(hr)) return hr;
        hr = dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f,
            L"ko-KR", &osdTextFormat);
        if (FAILED(hr)) return hr;
        hr = dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 22.0f,
            L"ko-KR", &volumeTextFormat);
        if (FAILED(hr)) return hr;
        if (SUCCEEDED(hr)) {
            hr = osdCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.055f, 0.063f, 0.078f, 0.90f),
                &osdCacheBackgroundBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = osdCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.91f, 0.93f, 0.95f, 1.0f),
                &osdCacheTextBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = volumeCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.055f, 0.063f, 0.078f, 0.90f),
                &volumeCacheBackgroundBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = volumeCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.91f, 0.93f, 0.95f, 1.0f),
                &volumeCacheTextBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = volumeCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.20f, 0.22f, 0.25f, 1.0f),
                &volumeCacheBarBackgroundBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = volumeCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.25f, 0.78f, 0.48f, 1.0f),
                &volumeCacheBarBrush);
        }
        if (FAILED(hr)) return hr;

        RECT sourceRect{0, 0, width, height};
        RECT outputRect{0, 0, static_cast<LONG>(outputWidth),
                        static_cast<LONG>(outputHeight)};
        videoContext->VideoProcessorSetStreamSourceRect(processor, 0, TRUE,
                                                        &sourceRect);
        videoContext->VideoProcessorSetStreamDestRect(processor, 0, TRUE,
                                                      &outputRect);
        videoContext->VideoProcessorSetOutputTargetRect(processor, TRUE,
                                                        &outputRect);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};
        inputColor.YCbCr_Matrix = 1; // BT.709
        inputColor.Nominal_Range = 1; // studio 16-235
        videoContext->VideoProcessorSetStreamColorSpace(processor, 0,
                                                        &inputColor);
        return S_OK;
    }

    void upload(const BYTE* pixels, UINT32 stride) {
        // Rotate upload targets so the CPU never updates the NV12 surface that
        // the GPU is still reading. This avoids UpdateSubresource's contended
        // two-copy path without introducing a video-frame queue.
        activeUploadSurface = nextUploadSurface;
        nextUploadSurface = (nextUploadSurface + 1) % kUploadSurfaceCount;
        ID3D11Texture2D* target = nv12Textures[activeUploadSurface];
        if (context1) {
            context1->UpdateSubresource1(target, 0, nullptr, pixels, stride, 0,
                                         D3D11_COPY_DISCARD);
        } else {
            context->UpdateSubresource(target, 0, nullptr, pixels, stride, 0);
        }
    }

    HRESULT refreshOverlayLayouts() {
        const uint64_t generation =
            g_overlayGeneration.load(std::memory_order_acquire);
        if (cachedOverlayGeneration == generation) return S_OK;
        cachedOverlayGeneration = generation;
        SafeRelease(osdTextLayout);
        SafeRelease(volumeTextLayout);

        const std::wstring osdText = BuildRuntimeOsdText(
            static_cast<int>(outputWidth), static_cast<int>(outputHeight));
        HRESULT hr = dwriteFactory->CreateTextLayout(
            osdText.c_str(), static_cast<UINT32>(osdText.size()),
            osdTextFormat, kOsdTextWidth, kOsdTextHeight,
            &osdTextLayout);
        if (FAILED(hr)) return hr;

        wchar_t volumeText[64]{};
        swprintf_s(volumeText, L"음량  %d%%",
                   g_volumePercent.load(std::memory_order_acquire));
        hr = dwriteFactory->CreateTextLayout(
            volumeText, static_cast<UINT32>(wcslen(volumeText)),
            volumeTextFormat, 228.0f, 36.0f, &volumeTextLayout);
        if (FAILED(hr)) return hr;

        osdCacheTarget->BeginDraw();
        osdCacheTarget->Clear(D2D1::ColorF(0, 0.0f));
        osdCacheTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(
                                  0.0f, 0.0f,
                                  static_cast<float>(kOsdOverlayWidth),
                                  static_cast<float>(kOsdOverlayHeight)),
                              8.0f, 8.0f),
            osdCacheBackgroundBrush);
        osdCacheTarget->DrawTextLayout(
            D2D1::Point2F(16.0f, 12.0f), osdTextLayout,
            osdCacheTextBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        hr = osdCacheTarget->EndDraw();
        if (FAILED(hr)) return hr;

        volumeCacheTarget->BeginDraw();
        volumeCacheTarget->Clear(D2D1::ColorF(0, 0.0f));
        volumeCacheTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, 260.0f, 82.0f),
                              8.0f, 8.0f),
            volumeCacheBackgroundBrush);
        volumeCacheTarget->DrawTextLayout(
            D2D1::Point2F(16.0f, 10.0f), volumeTextLayout,
            volumeCacheTextBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        const D2D1_RECT_F volumeBarBackground =
            D2D1::RectF(16.0f, 57.0f, 244.0f, 66.0f);
        volumeCacheTarget->FillRectangle(
            volumeBarBackground, volumeCacheBarBackgroundBrush);
        D2D1_RECT_F volumeBar = volumeBarBackground;
        volumeBar.right = volumeBar.left +
            (volumeBarBackground.right - volumeBarBackground.left) *
                g_volumePercent.load(std::memory_order_acquire) / 100.0f;
        if (volumeBar.right > volumeBar.left) {
            volumeCacheTarget->FillRectangle(volumeBar,
                                             volumeCacheBarBrush);
        }
        hr = volumeCacheTarget->EndDraw();
        return hr;
    }

    HRESULT drawOverlayQuads() {
        const bool osdVisible =
            g_osdVisible.load(std::memory_order_acquire);
        const bool volumeVisible = GetTickCount64() <
            g_volumeHudUntilMs.load(std::memory_order_acquire);
        if (!osdVisible && !volumeVisible) return S_OK;
        HRESULT hr = refreshOverlayLayouts();
        if (FAILED(hr)) return hr;

        const D3D11_VIEWPORT viewport{
            0.0f, 0.0f, static_cast<float>(outputWidth),
            static_cast<float>(outputHeight), 0.0f, 1.0f};
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &backBufferRenderTarget, nullptr);
        const float blendFactor[4]{};
        context->OMSetBlendState(overlayBlendState, blendFactor,
                                 0xffffffffu);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(overlayVertexShader, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &overlayRectBuffer);
        context->PSSetShader(overlayPixelShader, nullptr, 0);
        context->PSSetSamplers(0, 1, &overlaySampler);

        auto draw = [&](ID3D11ShaderResourceView* texture,
                        LONG left, LONG top, LONG right, LONG bottom) {
            const float rectangle[4]{
                -1.0f + 2.0f * left / outputWidth,
                1.0f - 2.0f * top / outputHeight,
                -1.0f + 2.0f * right / outputWidth,
                1.0f - 2.0f * bottom / outputHeight};
            context->UpdateSubresource(overlayRectBuffer, 0, nullptr,
                                       rectangle, 0, 0);
            context->PSSetShaderResources(0, 1, &texture);
            context->Draw(4, 0);
        };

        constexpr LONG margin = 16;
        constexpr LONG osdWidth =
            static_cast<LONG>(kOsdOverlayWidth);
        constexpr LONG osdHeight =
            static_cast<LONG>(kOsdOverlayHeight);
        if (osdVisible) {
            draw(osdOverlayShaderView, margin, margin, margin + osdWidth,
                 margin + osdHeight);
        }

        if (volumeVisible) {
            constexpr LONG hudWidth = 260;
            constexpr LONG hudHeight = 82;
            constexpr LONG hudMargin = 24;
            LONG x = hudMargin;
            LONG y = hudMargin;
            const bool right =
                g_settings.volumeHudPosition ==
                    VolumeHudPosition::TopRight ||
                g_settings.volumeHudPosition ==
                    VolumeHudPosition::BottomRight;
            const bool bottom =
                g_settings.volumeHudPosition ==
                    VolumeHudPosition::BottomLeft ||
                g_settings.volumeHudPosition ==
                    VolumeHudPosition::BottomRight;
            if (right) {
                x = (std::max)(hudMargin,
                               static_cast<LONG>(outputWidth) - hudWidth -
                                   hudMargin);
            }
            if (bottom) {
                y = (std::max)(hudMargin,
                               static_cast<LONG>(outputHeight) - hudHeight -
                                   hudMargin);
            } else if (!right && osdVisible) {
                y = margin + osdHeight + 12;
            }
            draw(volumeOverlayShaderView, x, y, x + hudWidth,
                 y + hudHeight);
        }
        ID3D11ShaderResourceView* noTexture = nullptr;
        context->PSSetShaderResources(0, 1, &noTexture);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        g_overlayRenderedFrames.fetch_add(1,
                                          std::memory_order_relaxed);
        return S_OK;
    }

    HRESULT presentUploaded() {
        if (occluded) {
            const uint64_t nowMs = GetTickCount64();
            if (nowMs < nextOcclusionTestMs) return DXGI_STATUS_OCCLUDED;
            const HRESULT test = swapChain->Present(0, DXGI_PRESENT_TEST);
            if (test == DXGI_STATUS_OCCLUDED) {
                nextOcclusionTestMs = nowMs + 50;
                return test;
            }
            if (FAILED(test)) return test;
            occluded = false;
            nextOcclusionTestMs = 0;
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = inputViews[activeUploadSurface];
        HRESULT hr = videoContext->VideoProcessorBlt(
            processor, outputView, 0, 1, &stream);
        if (FAILED(hr)) return hr;
        hr = drawOverlayQuads();
        if (FAILED(hr)) return hr;
        const bool vsync =
            g_settings.presentationMode == PresentationMode::VSync;
        const UINT syncInterval = vsync ? 1u : 0u;
        const UINT flags = !vsync && allowTearing
                               ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        hr = swapChain->Present(syncInterval, flags);
        if (hr == DXGI_STATUS_OCCLUDED) {
            occluded = true;
            nextOcclusionTestMs = GetTickCount64() + 50;
        }
        return hr;
    }
};

static bool UnifiedCaptureRenderLoop(HWND host) {
    const auto& preset = CurrentVideoPreset();
    g_captureFailureHr.store(S_OK, std::memory_order_release);
    g_captureAudioAvailable.store(false, std::memory_order_release);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHr(L"CoInitializeEx(direct video)", hr);
        g_captureFailureHr.store(hr, std::memory_order_release);
        return false;
    }

    DWORD videoTaskIndex = 0;
    HANDLE videoMmcss =
        AvSetMmThreadCharacteristicsW(L"Playback", &videoTaskIndex);
    if (videoMmcss) {
        AvSetMmThreadPriority(videoMmcss, AVRT_PRIORITY_HIGH);
    }

    IGraphBuilder* graph = nullptr;
    IMediaControl* control = nullptr;
    IMediaFilter* mediaFilter = nullptr;
    IBaseFilter* capture = nullptr;
    IPin* videoPin = nullptr;
    IPin* audioPin = nullptr;
    IBaseFilter* grabberFilter = nullptr;
    ISampleGrabber* grabber = nullptr;
    IBaseFilter* nullRenderer = nullptr;
    IPin* grabberIn = nullptr;
    IPin* grabberOut = nullptr;
    IPin* nullIn = nullptr;
    VideoSampleGrabberCB* callback = nullptr;
    IBaseFilter* audioGrabberFilter = nullptr;
    ISampleGrabber* audioGrabber = nullptr;
    IBaseFilter* audioNullRenderer = nullptr;
    IPin* audioGrabberIn = nullptr;
    IPin* audioGrabberOut = nullptr;
    IPin* audioNullIn = nullptr;
    SampleGrabberCB* audioCallback = nullptr;
    HANDLE frameEvent = nullptr;
    bool initialized = false;
    DirectD3D11Renderer renderer;

    do {
        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&graph));
        if (FAILED(hr)) break;
        hr = graph->QueryInterface(IID_PPV_ARGS(&control));
        if (FAILED(hr)) break;
        graph->QueryInterface(IID_PPV_ARGS(&mediaFilter));
        if (mediaFilter) mediaFilter->SetSyncSource(nullptr);

        hr = FindCaptureFilter(g_settings.captureDeviceId, &capture,
                               &g_activeCaptureDeviceName);
        if (FAILED(hr)) break;
        hr = graph->AddFilter(capture, L"Selected Capture Device");
        if (FAILED(hr)) break;
        hr = FindOutputPinByMajorType(capture, MEDIATYPE_Video, &videoPin);
        if (FAILED(hr)) break;

        DWORD imageBytes = 0;
        UINT32 stride = 0;
        int configuredFps = 0;
        VideoPixelFormat configuredFormat = VideoPixelFormat::Nv12;
        hr = ConfigureVideoPin(videoPin, preset.width, preset.height,
                               RequestedVideoFrameRate(),
                               g_settings.pixelFormat,
                               imageBytes, stride, configuredFps,
                               configuredFormat);
        if (FAILED(hr)) {
            LogHr(L"ConfigureVideoPin(uncompressed exact format)", hr);
            break;
        }
        g_activePixelFormat.store(static_cast<int>(configuredFormat),
                                  std::memory_order_release);

        g_videoConfiguredFps.store(configuredFps,
                                   std::memory_order_release);
        hr = renderer.initialize(host, preset.width, preset.height,
                                 configuredFps, configuredFormat);
        if (FAILED(hr)) {
            LogHr(L"DirectD3D11Renderer::initialize", hr);
            break;
        }
        UpdateConfiguredVideoTitle(host, configuredFps);

        frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!frameEvent) { hr = HRESULT_FROM_WIN32(GetLastError()); break; }
        LatestNv12Sample latest(imageBytes, frameEvent);

        hr = CoCreateInstance(CLSID_SampleGrabber, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&grabberFilter));
        if (FAILED(hr)) break;
        hr = graph->AddFilter(grabberFilter, L"Latest Video Frame");
        if (FAILED(hr)) break;
        hr = grabberFilter->QueryInterface(
            __uuidof(ISampleGrabber),
            reinterpret_cast<void**>(&grabber));
        if (FAILED(hr)) break;
        AM_MEDIA_TYPE requested{};
        requested.majortype = MEDIATYPE_Video;
        requested.subtype = PixelFormatSubtype(configuredFormat);
        requested.formattype = GUID_NULL;
        hr = grabber->SetMediaType(&requested);
        if (FAILED(hr)) break;
        grabber->SetOneShot(FALSE);
        grabber->SetBufferSamples(FALSE);
        callback = new VideoSampleGrabberCB(&latest);
        hr = grabber->SetCallback(callback, 0);
        if (FAILED(hr)) break;

        hr = CoCreateInstance(CLSID_NullRenderer, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&nullRenderer));
        if (FAILED(hr)) break;
        hr = graph->AddFilter(nullRenderer, L"Video Null Renderer");
        if (FAILED(hr)) break;
        if (FAILED(hr = GetFirstPin(grabberFilter, PINDIR_INPUT,
                                    &grabberIn))) break;
        if (FAILED(hr = GetFirstPin(grabberFilter, PINDIR_OUTPUT,
                                    &grabberOut))) break;
        if (FAILED(hr = GetFirstPin(nullRenderer, PINDIR_INPUT,
                                    &nullIn))) break;
        if (FAILED(hr = graph->ConnectDirect(videoPin, grabberIn,
                                             nullptr))) break;
        if (FAILED(hr = graph->ConnectDirect(grabberOut, nullIn,
                                             nullptr))) break;

        // Build the capture audio branch inside this same graph and filter
        // instance. There is no second device graph and no separate clock.
        hr = FindOutputPinByName(capture, kAudioPinName, &audioPin);
        if (FAILED(hr)) {
            hr = FindOutputPinByMajorType(capture, MEDIATYPE_Audio, &audioPin);
        }
        if (FAILED(hr)) break;
        g_captureAudioAvailable.store(true, std::memory_order_release);
        SuggestCaptureBuffer(audioPin);

        hr = CoCreateInstance(CLSID_SampleGrabber, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&audioGrabberFilter));
        if (FAILED(hr)) break;
        hr = graph->AddFilter(audioGrabberFilter, L"PCM Latest Audio");
        if (FAILED(hr)) break;
        hr = audioGrabberFilter->QueryInterface(
            __uuidof(ISampleGrabber),
            reinterpret_cast<void**>(&audioGrabber));
        if (FAILED(hr)) break;

        WAVEFORMATEX audioFormat = PcmOutputFormat();
        AM_MEDIA_TYPE requestedAudio{};
        requestedAudio.majortype = MEDIATYPE_Audio;
        requestedAudio.subtype = MEDIASUBTYPE_PCM;
        requestedAudio.bFixedSizeSamples = TRUE;
        requestedAudio.bTemporalCompression = FALSE;
        requestedAudio.lSampleSize = audioFormat.nBlockAlign;
        requestedAudio.formattype = FORMAT_WaveFormatEx;
        requestedAudio.cbFormat = sizeof(audioFormat);
        requestedAudio.pbFormat =
            reinterpret_cast<BYTE*>(&audioFormat);
        hr = audioGrabber->SetMediaType(&requestedAudio);
        if (FAILED(hr)) break;
        audioGrabber->SetOneShot(FALSE);
        audioGrabber->SetBufferSamples(FALSE);
        audioCallback = new SampleGrabberCB();
        hr = audioGrabber->SetCallback(audioCallback, 0);
        if (FAILED(hr)) break;

        hr = CoCreateInstance(CLSID_NullRenderer, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&audioNullRenderer));
        if (FAILED(hr)) break;
        hr = graph->AddFilter(audioNullRenderer, L"Audio Null Renderer");
        if (FAILED(hr)) break;
        if (FAILED(hr = GetFirstPin(audioGrabberFilter, PINDIR_INPUT,
                                    &audioGrabberIn))) break;
        if (FAILED(hr = GetFirstPin(audioGrabberFilter, PINDIR_OUTPUT,
                                    &audioGrabberOut))) break;
        if (FAILED(hr = GetFirstPin(audioNullRenderer, PINDIR_INPUT,
                                    &audioNullIn))) break;
        if (FAILED(hr = graph->Connect(audioPin, audioGrabberIn))) break;
        ReportConnectedAudioAllocator(audioGrabberIn);
        if (FAILED(hr = graph->Connect(audioGrabberOut, audioNullIn))) break;

        hr = control->Run();
        if (FAILED(hr)) break;
        initialized = true;
        g_videoStride.store(stride, std::memory_order_release);
        const bool tearingActive = renderer.allowTearing &&
            g_settings.presentationMode == PresentationMode::AllowTearing;
        g_videoTearing.store(tearingActive, std::memory_order_release);
        fwprintf(stderr,
                 L"[capture] single graph running: %s · %s %dx%d @ %d + "
                 L"PCM 48k stereo, stride %u, frame bytes %lu, present %s\n",
                 g_activeCaptureDeviceName.c_str(),
                 PixelFormatName(configuredFormat), preset.width,
                 preset.height, configuredFps, stride, imageBytes,
                 g_settings.presentationMode == PresentationMode::VSync
                     ? L"VSync" : tearingActive ? L"Tearing" : L"Immediate");
        fwprintf(stderr,
                 L"[video] upload ring: %u %s GPU surfaces; update: %s\n",
                 DirectD3D11Renderer::kUploadSurfaceCount,
                 PixelFormatName(configuredFormat),
                 renderer.discardUpdateAvailable
                     ? L"D3D11.1 discard" : L"D3D11 fallback");

        auto recoverRenderer = [&](const wchar_t* failedStage,
                                   HRESULT failure) {
            LogHr(failedStage, failure);
            const HRESULT removedReason = renderer.deviceRemovedReason();
            if (FAILED(removedReason)) {
                LogHr(L"ID3D11Device::GetDeviceRemovedReason",
                      removedReason);
            }
            g_directVideoActive.store(false, std::memory_order_release);

            constexpr DWORD retryDelaysMs[] = {0, 100, 250};
            for (size_t attempt = 0;
                 attempt < ARRAYSIZE(retryDelaysMs) && g_running.load();
                 ++attempt) {
                if (retryDelaysMs[attempt]) Sleep(retryDelaysMs[attempt]);
                const HRESULT recoveryHr = renderer.initialize(
                    host, preset.width, preset.height, configuredFps,
                    configuredFormat);
                if (SUCCEEDED(recoveryHr)) {
                    const bool recoveredTearing = renderer.allowTearing &&
                        g_settings.presentationMode ==
                            PresentationMode::AllowTearing;
                    g_videoTearing.store(recoveredTearing,
                                         std::memory_order_release);
                    g_overlayGeneration.fetch_add(
                        1, std::memory_order_relaxed);
                    fwprintf(stderr,
                             L"[video] D3D11 renderer recovered on attempt "
                             L"%zu; capture graph and WASAPI remained active.\n",
                             attempt + 1);
                    return true;
                }
                LogHr(L"D3D11 renderer recovery", recoveryHr);
                hr = recoveryHr;
            }
            return false;
        };

        int64_t arrivalUs = 0;
        bool presentedAnyFrame = false;
        const auto firstFrameDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (g_running.load()) {
            if (WaitForSingleObject(frameEvent, 100) != WAIT_OBJECT_0) {
                if (!presentedAnyFrame &&
                    std::chrono::steady_clock::now() >= firstFrameDeadline) {
                    fwprintf(stderr,
                             L"[video] direct path received no %s frame "
                             L"within 3 seconds.\n",
                             PixelFormatName(configuredFormat));
                    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                    initialized = false;
                    break;
                }
                continue;
            }
            IMediaSample* videoSample = latest.takeLatest(arrivalUs);
            if (!videoSample) continue;
            BYTE* sampleData = nullptr;
            hr = videoSample->GetPointer(&sampleData);
            if (SUCCEEDED(hr) && sampleData) {
                renderer.upload(sampleData, stride);
            }
            videoSample->Release();
            if (FAILED(hr)) {
                LogHr(L"D3D11 video transfer", hr);
                initialized = false;
                break;
            }
            hr = renderer.presentUploaded();
            if (hr == DXGI_STATUS_OCCLUDED) {
                continue;
            }
            if (FAILED(hr)) {
                if (recoverRenderer(L"D3D11 direct Present", hr)) {
                    hr = S_OK;
                    continue;
                }
                initialized = false;
                break;
            }
            const int64_t presentedUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            if (OsdTrackingActive()) {
                g_videoAppLatencyUs.store(presentedUs - arrivalUs,
                                          std::memory_order_release);
                g_videoPresentedFrames.fetch_add(1,
                                                 std::memory_order_relaxed);
            }
            g_directVideoActive.store(true, std::memory_order_release);
            presentedAnyFrame = true;
        }
        control->Stop();
    } while (false);

    if (!initialized) {
        const HRESULT failure = FAILED(hr) ? hr : E_FAIL;
        g_captureFailureHr.store(failure, std::memory_order_release);
        LogHr(L"Single capture graph initialization", failure);
    }
    if (audioGrabber) audioGrabber->SetCallback(nullptr, 0);
    if (audioCallback) audioCallback->Release();
    if (grabber) grabber->SetCallback(nullptr, 0);
    if (callback) callback->Release();
    if (frameEvent) CloseHandle(frameEvent);
    SafeRelease(nullIn);
    SafeRelease(audioNullIn);
    SafeRelease(audioGrabberOut);
    SafeRelease(audioGrabberIn);
    SafeRelease(audioNullRenderer);
    SafeRelease(audioGrabber);
    SafeRelease(audioGrabberFilter);
    SafeRelease(grabberOut);
    SafeRelease(grabberIn);
    SafeRelease(nullRenderer);
    SafeRelease(grabber);
    SafeRelease(grabberFilter);
    SafeRelease(videoPin);
    SafeRelease(audioPin);
    SafeRelease(capture);
    SafeRelease(mediaFilter);
    SafeRelease(control);
    SafeRelease(graph);
    renderer.reset();
    if (videoMmcss) AvRevertMmThreadCharacteristics(videoMmcss);
    CoUninitialize();
    return initialized;
}

// -----------------------------------------------------------------------------
// Startup settings dialog
// -----------------------------------------------------------------------------

constexpr int IDC_SETTINGS_AUDIO = 2001;
constexpr int IDC_SETTINGS_VIDEO = 2002;
constexpr int IDC_SETTINGS_PIXEL = 2003;
constexpr int IDC_SETTINGS_START = 2004;
constexpr int IDC_SETTINGS_CANCEL = 2005;
constexpr int IDC_SETTINGS_BUFFER = 2006;
constexpr int IDC_SETTINGS_BORDERLESS = 2007;
constexpr int IDC_SETTINGS_AUDIO_STATUS = 2008;
constexpr int IDC_SETTINGS_PRESENTATION = 2009;
constexpr int IDC_SETTINGS_VOLUME_HUD = 2010;
constexpr int IDC_SETTINGS_DRIFT = 2011;
constexpr int IDC_SETTINGS_WINDOW_SNAP = 2012;
constexpr int IDC_SETTINGS_RELATIVE_SIZE = 2013;
constexpr int IDC_SETTINGS_DRIFT_HELP = 2014;
constexpr int IDC_SETTINGS_PCM_QUEUE = 2015;
constexpr int IDC_SETTINGS_AUDIO_OUTPUT = 2016;
constexpr int IDC_SETTINGS_CAPTURE_DEVICE = 2017;
constexpr int IDC_SETTINGS_PIXEL_FORMAT = 2018;
constexpr int IDC_SETTINGS_SAVE_LOG = 2019;
constexpr int IDC_SETTINGS_FRAME_RATE = 2020;
constexpr int IDC_SETTINGS_MUTE_BACKGROUND = 2021;
constexpr int IDC_SETTINGS_PRESENTATION_HELP = 2022;
constexpr int IDC_SETTINGS_PCM_QUEUE_HELP = 2023;
constexpr UINT WM_AUDIOCLIENT3_PROBE_COMPLETE = WM_APP + 73;
constexpr UINT WM_SETTINGS_TOOLTIP_SHOW = WM_APP + 74;
constexpr UINT WM_SETTINGS_TOOLTIP_HIDE = WM_APP + 75;

struct SettingsDialogState {
    HWND audioLabel = nullptr;
    HWND bufferLabel = nullptr;
    HWND audioOutputLabel = nullptr;
    HWND volumeHudLabel = nullptr;
    HWND driftLabel = nullptr;
    HWND driftHelp = nullptr;
    HWND pcmQueueLabel = nullptr;
    HWND pcmQueueHelp = nullptr;
    HWND presentationLabel = nullptr;
    HWND presentationHelp = nullptr;
    HWND videoLabel = nullptr;
    HWND captureDeviceLabel = nullptr;
    HWND pixelFormatLabel = nullptr;
    HWND frameRateLabel = nullptr;
    HWND videoCapabilityStatus = nullptr;
    HWND audioCombo = nullptr;
    HWND bufferCombo = nullptr;
    HWND audioOutputCombo = nullptr;
    HWND volumeHudCombo = nullptr;
    HWND muteBackgroundCheck = nullptr;
    HWND driftCombo = nullptr;
    HWND pcmQueueCombo = nullptr;
    HWND audioStatus = nullptr;
    HWND presentationCombo = nullptr;
    HWND videoCombo = nullptr;
    HWND captureDeviceCombo = nullptr;
    HWND pixelFormatCombo = nullptr;
    HWND frameRateCombo = nullptr;
    HWND pixelCheck = nullptr;
    HWND relativeSizeCheck = nullptr;
    HWND relativeSizeWarning = nullptr;
    HWND borderlessCheck = nullptr;
    HWND windowSnapCheck = nullptr;
    HWND saveLogCheck = nullptr;
    HWND startButton = nullptr;
    HWND cancelButton = nullptr;
    HWND tooltipWindow = nullptr;
    HWND activeTooltipTarget = nullptr;
    std::vector<HFONT> uiFonts;
    std::thread probeThread;
    std::atomic<bool> probeReady{false};
    AudioClient3Support probe{};
    std::vector<UINT32> sharedPeriodChoices;
    std::vector<CaptureDeviceInfo> captureDevices;
    std::vector<AudioEndpointInfo> audioEndpoints;
    std::vector<PixelFormatSupport> pixelFormats;
    VideoPreset initialVideoPreset = VideoPreset::R2560x1440;
    UINT32 selectedSharedPeriodFrames = 0;
    int selectedBufferMs = kRecommendedWasapiBufferMs;
    bool bufferItemsAreSharedFrames = false;
    bool accepted = false;
};

static int SettingsPixels(int dips, UINT dpi) {
    return MulDiv(dips, dpi ? dpi : USER_DEFAULT_SCREEN_DPI,
                  USER_DEFAULT_SCREEN_DPI);
}

static constexpr int kSettingsClientWidthDip = 950;
static constexpr int kSettingsClientHeightDip = 600;

static SIZE SettingsDialogOuterSize(HWND hwnd, UINT dpi) {
    RECT rect{0, 0, SettingsPixels(kSettingsClientWidthDip, dpi),
              SettingsPixels(kSettingsClientHeightDip, dpi)};
    const DWORD style =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if (!AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi)) {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

static void PlaceSettingsControl(HWND control, int x, int y, int width,
                                 int height, UINT dpi) {
    if (!control) return;
    SetWindowPos(control, nullptr, SettingsPixels(x, dpi),
                 SettingsPixels(y, dpi), SettingsPixels(width, dpi),
                 SettingsPixels(height, dpi),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static BOOL CALLBACK SetSettingsChildFont(HWND child, LPARAM fontValue) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(fontValue), FALSE);
    return TRUE;
}

static void ApplySettingsFont(SettingsDialogState* state, HWND hwnd,
                              UINT dpi) {
    if (!state || !hwnd) return;
    HFONT font = CreateFontW(
        -MulDiv(9, dpi ? dpi : USER_DEFAULT_SCREEN_DPI, 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!font) return;
    state->uiFonts.push_back(font);
    EnumChildWindows(hwnd, SetSettingsChildFont,
                     reinterpret_cast<LPARAM>(font));
}

static void LayoutSettingsControls(SettingsDialogState* state, UINT dpi) {
    if (!state) return;
    PlaceSettingsControl(state->audioLabel, 24, 24, 160, 24, dpi);
    PlaceSettingsControl(state->audioCombo, 195, 20, 280, 120, dpi);
    PlaceSettingsControl(state->audioOutputLabel, 24, 68, 160, 24, dpi);
    PlaceSettingsControl(state->audioOutputCombo, 195, 64, 280, 220, dpi);
    PlaceSettingsControl(state->bufferLabel, 24, 112, 160, 24, dpi);
    PlaceSettingsControl(state->bufferCombo, 195, 108, 280, 180, dpi);
    PlaceSettingsControl(state->audioStatus, 24, 148, 451, 28, dpi);
    PlaceSettingsControl(state->volumeHudLabel, 24, 190, 160, 24, dpi);
    PlaceSettingsControl(state->volumeHudCombo, 195, 186, 280, 160, dpi);
    PlaceSettingsControl(state->driftLabel, 24, 234, 140, 24, dpi);
    PlaceSettingsControl(state->driftHelp, 168, 230, 24, 24, dpi);
    PlaceSettingsControl(state->driftCombo, 195, 230, 280, 120, dpi);
    PlaceSettingsControl(state->pcmQueueLabel, 24, 278, 160, 24, dpi);
    PlaceSettingsControl(state->pcmQueueHelp, 168, 274, 24, 24, dpi);
    PlaceSettingsControl(state->pcmQueueCombo, 195, 274, 280, 140, dpi);
    PlaceSettingsControl(state->muteBackgroundCheck, 24, 318, 451, 28, dpi);

    PlaceSettingsControl(state->presentationLabel, 505, 24, 95, 24, dpi);
    PlaceSettingsControl(state->presentationHelp, 604, 20, 24, 24, dpi);
    PlaceSettingsControl(state->presentationCombo, 630, 20, 295, 120, dpi);
    PlaceSettingsControl(state->captureDeviceLabel, 505, 68, 120, 24, dpi);
    PlaceSettingsControl(state->captureDeviceCombo, 630, 64, 295, 220, dpi);
    PlaceSettingsControl(state->videoLabel, 505, 112, 120, 24, dpi);
    PlaceSettingsControl(state->videoCombo, 630, 108, 295, 120, dpi);
    PlaceSettingsControl(state->pixelFormatLabel, 505, 156, 120, 24, dpi);
    PlaceSettingsControl(state->pixelFormatCombo, 630, 152, 295, 160, dpi);
    PlaceSettingsControl(state->frameRateLabel, 505, 200, 120, 24, dpi);
    PlaceSettingsControl(state->frameRateCombo, 630, 196, 295, 200, dpi);
    PlaceSettingsControl(state->videoCapabilityStatus, 505, 234, 420, 24, dpi);
    PlaceSettingsControl(state->pixelCheck, 505, 266, 420, 28, dpi);
    PlaceSettingsControl(state->relativeSizeCheck, 505, 300, 420, 28, dpi);
    PlaceSettingsControl(state->relativeSizeWarning, 525, 328, 400, 36, dpi);
    PlaceSettingsControl(state->borderlessCheck, 505, 370, 420, 28, dpi);
    PlaceSettingsControl(state->windowSnapCheck, 505, 404, 420, 28, dpi);
    PlaceSettingsControl(state->saveLogCheck, 505, 438, 420, 28, dpi);
    PlaceSettingsControl(state->startButton, 745, 552, 80, 30, dpi);
    PlaceSettingsControl(state->cancelButton, 835, 552, 80, 30, dpi);
}

static void TrackSettingsTooltip(HWND target, HWND tooltip, bool active) {
    if (!target || !tooltip) return;
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFO_V1_SIZE;
    tool.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    tool.hwnd = GetParent(target);
    tool.uId = reinterpret_cast<UINT_PTR>(target);
    SendMessageW(tooltip, TTM_TRACKACTIVATE, active ? TRUE : FALSE,
                 reinterpret_cast<LPARAM>(&tool));
}

static void AddSettingsTooltip(SettingsDialogState* state, HWND owner,
                               HWND target, const wchar_t* text) {
    if (!state || !owner || !target || !text) return;
    if (!state->tooltipWindow) {
        state->tooltipWindow = CreateWindowExW(
            WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!state->tooltipWindow) return;
        SetWindowPos(state->tooltipWindow, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SendMessageW(state->tooltipWindow, TTM_SETMAXTIPWIDTH, 0, 430);
        SendMessageW(state->tooltipWindow, TTM_SETDELAYTIME,
                     TTDT_INITIAL, 250);
    }
    TOOLINFOW tool{};
    // The application intentionally has no Common Controls v6 manifest.  The
    // built-in v5 tooltip rejects the newer structure size on some Windows
    // installations, so register the compatible v1 fields explicitly.
    tool.cbSize = TTTOOLINFO_V1_SIZE;
    tool.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    tool.hwnd = owner;
    tool.uId = reinterpret_cast<UINT_PTR>(target);
    tool.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(state->tooltipWindow, TTM_ADDTOOLW, 0,
                 reinterpret_cast<LPARAM>(&tool));
}

static bool IsSettingsHelpControl(const SettingsDialogState* state,
                                  HWND target) {
    return state && (target == state->driftHelp ||
                     target == state->pcmQueueHelp ||
                     target == state->presentationHelp);
}

static bool SettingsUsesSharedMode(const SettingsDialogState* state) {
    return state && SendMessageW(state->audioCombo, CB_GETCURSEL, 0, 0) == 0;
}

static void RememberCurrentBufferChoice(SettingsDialogState* state) {
    if (!state || !state->bufferCombo) return;
    const LRESULT index = SendMessageW(state->bufferCombo, CB_GETCURSEL, 0, 0);
    if (index < 0) return;
    const LRESULT value = SendMessageW(state->bufferCombo, CB_GETITEMDATA,
                                       static_cast<WPARAM>(index), 0);
    if (value == CB_ERR) return;
    if (state->bufferItemsAreSharedFrames) {
        state->selectedSharedPeriodFrames = static_cast<UINT32>(value);
    } else {
        state->selectedBufferMs = static_cast<int>(value);
    }
}

static std::vector<UINT32> BuildSharedPeriodChoices(
    const AudioClient3Support& support) {
    std::vector<UINT32> choices;
    auto add = [&](UINT32 requested) {
        const UINT32 value = ClosestSupportedSharedPeriod(requested, support);
        if (value) choices.push_back(value);
    };

    add(support.minimumFrames);
    for (const int ms : kWasapiBufferOptionsMs) {
        add(static_cast<UINT32>(ms * kSampleRate / 1000));
    }
    add(support.defaultFrames);
    std::sort(choices.begin(), choices.end());
    choices.erase(std::unique(choices.begin(), choices.end()), choices.end());
    return choices;
}

static void PopulateSettingsBufferCombo(SettingsDialogState* state) {
    if (!state || !state->bufferCombo) return;
    SendMessageW(state->bufferCombo, CB_RESETCONTENT, 0, 0);

    const bool useSharedFrames = SettingsUsesSharedMode(state) &&
                                 state->probeReady.load(std::memory_order_acquire) &&
                                 state->probe.supported;
    state->bufferItemsAreSharedFrames = useSharedFrames;

    if (useSharedFrames) {
        state->sharedPeriodChoices = BuildSharedPeriodChoices(state->probe);
        UINT32 desired = state->selectedSharedPeriodFrames;
        if (!desired) {
            desired = static_cast<UINT32>(state->selectedBufferMs *
                                          kSampleRate / 1000);
        }
        size_t selected = 0;
        UINT32 smallestDifference = UINT32_MAX;
        for (size_t i = 0; i < state->sharedPeriodChoices.size(); ++i) {
            const UINT32 frames = state->sharedPeriodChoices[i];
            wchar_t label[96]{};
            const double ms = 1000.0 * frames / kSampleRate;
            if (frames == state->probe.defaultFrames) {
                swprintf_s(label, L"%.2f ms (권장)", ms);
            } else if (frames == state->probe.minimumFrames) {
                swprintf_s(label, L"%.2f ms (최저)", ms);
            } else {
                swprintf_s(label, L"%.2f ms", ms);
            }
            const LRESULT index = SendMessageW(
                state->bufferCombo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label));
            SendMessageW(state->bufferCombo, CB_SETITEMDATA,
                         static_cast<WPARAM>(index), frames);
            const UINT32 difference = frames > desired ? frames - desired
                                                        : desired - frames;
            if (difference < smallestDifference) {
                smallestDifference = difference;
                selected = i;
            }
        }
        if (!state->sharedPeriodChoices.empty()) {
            SendMessageW(state->bufferCombo, CB_SETCURSEL,
                         static_cast<WPARAM>(selected), 0);
            state->selectedSharedPeriodFrames =
                state->sharedPeriodChoices[selected];
        }
        return;
    }

    size_t selected = 0;
    for (size_t i = 0; i < ARRAYSIZE(kWasapiBufferOptionsMs); ++i) {
        wchar_t label[64]{};
        if (kWasapiBufferOptionsMs[i] == kRecommendedWasapiBufferMs) {
            swprintf_s(label, L"%d ms (권장)", kWasapiBufferOptionsMs[i]);
        } else {
            swprintf_s(label, L"%d ms", kWasapiBufferOptionsMs[i]);
        }
        const LRESULT index = SendMessageW(
            state->bufferCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label));
        SendMessageW(state->bufferCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(index), kWasapiBufferOptionsMs[i]);
        if (kWasapiBufferOptionsMs[i] == state->selectedBufferMs) selected = i;
    }
    SendMessageW(state->bufferCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selected), 0);
}

static void UpdateAudioClient3Status(SettingsDialogState* state) {
    if (!state || !state->audioStatus) return;
    if (!state->probeReady.load(std::memory_order_acquire)) {
        SetWindowTextW(state->audioStatus, L"Shared 저지연 지원 확인 중…");
        return;
    }

    wchar_t status[200]{};
    if (state->probe.supported) {
        swprintf_s(status,
                   L"IAudioClient3 지원됨 · Shared %.2f–%.2f ms · 검사 %.1f ms",
                   1000.0 * state->probe.minimumFrames / kSampleRate,
                   1000.0 * state->probe.maximumFrames / kSampleRate,
                   state->probe.probeMilliseconds);
    } else {
        swprintf_s(status,
                   L"IAudioClient3 사용 불가 · 기본 Shared로 자동 전환 (0x%08X)",
                   static_cast<unsigned>(state->probe.result));
    }
    SetWindowTextW(state->audioStatus, status);
}

static std::wstring SelectedAudioEndpointId(
    const SettingsDialogState* state) {
    if (!state || !state->audioOutputCombo) return {};
    const LRESULT index = SendMessageW(
        state->audioOutputCombo, CB_GETCURSEL, 0, 0);
    if (index <= 0 || static_cast<size_t>(index - 1) >=
                          state->audioEndpoints.size()) return {};
    return state->audioEndpoints[static_cast<size_t>(index - 1)].id;
}

static std::wstring SelectedCaptureDeviceId(
    const SettingsDialogState* state) {
    if (!state || !state->captureDeviceCombo) return {};
    const LRESULT index = SendMessageW(
        state->captureDeviceCombo, CB_GETCURSEL, 0, 0);
    if (index <= 0 || static_cast<size_t>(index - 1) >=
                          state->captureDevices.size()) return {};
    return state->captureDevices[static_cast<size_t>(index - 1)].id;
}

static VideoPixelFormat SelectedPixelFormat(
    const SettingsDialogState* state) {
    if (!state || !state->pixelFormatCombo) return VideoPixelFormat::Auto;
    const LRESULT index = SendMessageW(
        state->pixelFormatCombo, CB_GETCURSEL, 0, 0);
    if (index < 0) return VideoPixelFormat::Auto;
    const LRESULT value = SendMessageW(
        state->pixelFormatCombo, CB_GETITEMDATA,
        static_cast<WPARAM>(index), 0);
    return value == CB_ERR ? VideoPixelFormat::Auto
                           : static_cast<VideoPixelFormat>(value);
}

static void UpdateVideoCapabilityStatus(SettingsDialogState* state) {
    if (!state) return;

    const bool supported = !state->pixelFormats.empty();
    if (state->pixelFormatCombo) {
        EnableWindow(state->pixelFormatCombo, supported ? TRUE : FALSE);
    }
    if (state->frameRateCombo) {
        EnableWindow(state->frameRateCombo, supported ? TRUE : FALSE);
    }
    if (state->startButton) {
        EnableWindow(state->startButton, supported ? TRUE : FALSE);
    }
    if (!state->videoCapabilityStatus) return;

    std::wstring message;
    if (!supported) {
        message = L"지원 모드 없음: 다른 장치 또는 해상도를 선택하세요.";
    } else {
        message = L"자동 인식: ";
        bool firstFormat = true;
        for (const auto format : {VideoPixelFormat::Nv12,
                                  VideoPixelFormat::Yuy2}) {
            std::vector<int> frameRates;
            for (const auto& support : state->pixelFormats) {
                if (support.format == format) {
                    frameRates.push_back(support.selectedFps);
                }
            }
            if (frameRates.empty()) continue;
            std::sort(frameRates.begin(), frameRates.end(), std::greater<int>());
            frameRates.erase(std::unique(frameRates.begin(), frameRates.end()),
                             frameRates.end());
            if (!firstFormat) message += L"  ·  ";
            message += PixelFormatName(format);
            message += L" ";
            for (size_t i = 0; i < frameRates.size(); ++i) {
                if (i != 0) message += L"/";
                message += std::to_wstring(frameRates[i]);
            }
            message += L" fps";
            firstFormat = false;
        }
    }
    SetWindowTextW(state->videoCapabilityStatus, message.c_str());
}

static void PopulateFrameRateCombo(SettingsDialogState* state) {
    if (!state || !state->frameRateCombo) return;
    int desiredFrameRate = g_settings.videoFrameRate;
    const LRESULT oldIndex = SendMessageW(
        state->frameRateCombo, CB_GETCURSEL, 0, 0);
    if (oldIndex >= 0) {
        const LRESULT oldValue = SendMessageW(
            state->frameRateCombo, CB_GETITEMDATA,
            static_cast<WPARAM>(oldIndex), 0);
        if (oldValue != CB_ERR) desiredFrameRate = static_cast<int>(oldValue);
    }
    SendMessageW(state->frameRateCombo, CB_RESETCONTENT, 0, 0);
    if (state->pixelFormats.empty()) {
        const LRESULT noModeIndex = SendMessageW(
            state->frameRateCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"지원 프레임 없음"));
        SendMessageW(state->frameRateCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(noModeIndex), 0);
        SendMessageW(state->frameRateCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(noModeIndex), 0);
        return;
    }
    const LRESULT autoIndex = SendMessageW(
        state->frameRateCombo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"자동 선택 (권장 프레임)"));
    SendMessageW(state->frameRateCombo, CB_SETITEMDATA,
                 static_cast<WPARAM>(autoIndex), 0);
    LRESULT selectedIndex = autoIndex;
    const VideoPixelFormat selectedFormat = SelectedPixelFormat(state);
    std::vector<int> frameRates;
    for (const auto& support : state->pixelFormats) {
        if (selectedFormat != VideoPixelFormat::Auto &&
            support.format != selectedFormat) continue;
        frameRates.push_back(support.selectedFps);
    }
    std::sort(frameRates.begin(), frameRates.end(), std::greater<int>());
    frameRates.erase(std::unique(frameRates.begin(), frameRates.end()),
                     frameRates.end());
    for (const int fps : frameRates) {
        wchar_t label[64]{};
        swprintf_s(label, L"%d fps", fps);
        const LRESULT index = SendMessageW(
            state->frameRateCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label));
        SendMessageW(state->frameRateCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(index), fps);
        if (fps == desiredFrameRate) selectedIndex = index;
    }
    SendMessageW(state->frameRateCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selectedIndex), 0);
}

static void PopulatePixelFormatCombo(SettingsDialogState* state) {
    if (!state || !state->pixelFormatCombo || !state->frameRateCombo ||
        !state->videoCombo) return;
    const LRESULT videoIndex = SendMessageW(
        state->videoCombo, CB_GETCURSEL, 0, 0);
    if (videoIndex < 0 || videoIndex >=
                            static_cast<LRESULT>(ARRAYSIZE(kVideoPresets))) {
        return;
    }
    const auto& preset = kVideoPresets[videoIndex];
    VideoPixelFormat desiredFormat = g_settings.pixelFormat;
    if (SendMessageW(state->pixelFormatCombo, CB_GETCOUNT, 0, 0) > 0) {
        desiredFormat = SelectedPixelFormat(state);
    }
    state->pixelFormats = ProbePixelFormats(
        SelectedCaptureDeviceId(state), preset.width, preset.height);
    SendMessageW(state->pixelFormatCombo, CB_RESETCONTENT, 0, 0);
    if (state->pixelFormats.empty()) {
        const LRESULT noModeIndex = SendMessageW(
            state->pixelFormatCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"지원 포맷 없음"));
        SendMessageW(state->pixelFormatCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(noModeIndex),
                     static_cast<LPARAM>(VideoPixelFormat::Auto));
        SendMessageW(state->pixelFormatCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(noModeIndex), 0);
        PopulateFrameRateCombo(state);
        UpdateVideoCapabilityStatus(state);
        return;
    }
    LRESULT autoIndex = SendMessageW(
        state->pixelFormatCombo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"자동 선택 (NV12 우선 · 권장)"));
    SendMessageW(state->pixelFormatCombo, CB_SETITEMDATA,
                 static_cast<WPARAM>(autoIndex),
                 static_cast<LPARAM>(VideoPixelFormat::Auto));
    LRESULT selectedIndex = autoIndex;
    for (const auto format : {VideoPixelFormat::Nv12,
                              VideoPixelFormat::Yuy2}) {
        const bool available = std::any_of(
            state->pixelFormats.begin(), state->pixelFormats.end(),
            [format](const PixelFormatSupport& support) {
                return support.format == format;
            });
        if (!available) continue;
        const wchar_t* label = format == VideoPixelFormat::Nv12
            ? L"NV12 8-bit 4:2:0" : L"YUY2 8-bit 4:2:2";
        const LRESULT index = SendMessageW(
            state->pixelFormatCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label));
        SendMessageW(state->pixelFormatCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(index),
                     static_cast<LPARAM>(format));
        if (format == desiredFormat) selectedIndex = index;
    }
    SendMessageW(state->pixelFormatCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selectedIndex), 0);
    PopulateFrameRateCombo(state);
    UpdateVideoCapabilityStatus(state);
}

static void FinishSettingsDialog(HWND hwnd, SettingsDialogState* state, bool accepted) {
    if (!state) return;

    if (accepted) {
        const VideoPreset previousVideoPreset = g_settings.videoPreset;
        const bool previouslyRelative = g_settings.relativeWindowSize;
        RememberCurrentBufferChoice(state);
        const LRESULT audioIndex = SendMessageW(
            state->audioCombo, CB_GETCURSEL, 0, 0);
        const LRESULT videoIndex = SendMessageW(
            state->videoCombo, CB_GETCURSEL, 0, 0);
        const LRESULT presentationIndex = SendMessageW(
            state->presentationCombo, CB_GETCURSEL, 0, 0);
        const LRESULT volumeHudIndex = SendMessageW(
            state->volumeHudCombo, CB_GETCURSEL, 0, 0);
        const LRESULT driftIndex = SendMessageW(
            state->driftCombo, CB_GETCURSEL, 0, 0);
        const LRESULT pcmQueueIndex = SendMessageW(
            state->pcmQueueCombo, CB_GETCURSEL, 0, 0);
        const LRESULT pixelFormatIndex = SendMessageW(
            state->pixelFormatCombo, CB_GETCURSEL, 0, 0);
        const LRESULT frameRateIndex = SendMessageW(
            state->frameRateCombo, CB_GETCURSEL, 0, 0);
        if (audioIndex == 1) {
            g_settings.audioMode = AudioMode::WasapiExclusive;
        } else {
            g_settings.audioMode = AudioMode::WasapiShared;
        }
        if (videoIndex >= 0 && videoIndex < static_cast<LRESULT>(ARRAYSIZE(kVideoPresets))) {
            g_settings.videoPreset = kVideoPresets[videoIndex].preset;
        }
        g_settings.presentationMode = presentationIndex == 1
                                          ? PresentationMode::VSync
                                          : PresentationMode::AllowTearing;
        g_settings.wasapiBufferMs = state->selectedBufferMs;
        if (volumeHudIndex >= 0 && volumeHudIndex <= 3) {
            g_settings.volumeHudPosition =
                static_cast<VolumeHudPosition>(volumeHudIndex);
        }
        g_settings.driftCorrection = driftIndex == 1
                                         ? DriftCorrectionMode::Resample
                                         : DriftCorrectionMode::Off;
        if (pcmQueueIndex >= 0) {
            const LRESULT queueMs = SendMessageW(
                state->pcmQueueCombo, CB_GETITEMDATA,
                static_cast<WPARAM>(pcmQueueIndex), 0);
            if (queueMs != CB_ERR) {
                g_settings.pcmQueueTargetMs = static_cast<int>(queueMs);
            }
        }
        if (audioIndex == 0 && state->probeReady.load(std::memory_order_acquire) &&
            state->probe.supported) {
            g_settings.wasapiSharedPeriodFrames =
                state->selectedSharedPeriodFrames;
        }
        g_settings.audioOutputDeviceId = SelectedAudioEndpointId(state);
        g_settings.captureDeviceId = SelectedCaptureDeviceId(state);
        if (pixelFormatIndex >= 0) {
            const LRESULT value = SendMessageW(
                state->pixelFormatCombo, CB_GETITEMDATA,
                static_cast<WPARAM>(pixelFormatIndex), 0);
            g_settings.pixelFormat = value == CB_ERR
                ? VideoPixelFormat::Auto
                : static_cast<VideoPixelFormat>(value);
        }
        if (frameRateIndex >= 0) {
            const LRESULT value = SendMessageW(
                state->frameRateCombo, CB_GETITEMDATA,
                static_cast<WPARAM>(frameRateIndex), 0);
            g_settings.videoFrameRate = value == CB_ERR
                                            ? 0 : static_cast<int>(value);
        }
        g_settings.saveLog = SendMessageW(
            state->saveLogCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.muteWhenBackground = SendMessageW(
            state->muteBackgroundCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.pixelPerfect = SendMessageW(
            state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.relativeWindowSize = SendMessageW(
            state->relativeSizeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (g_settings.relativeWindowSize) {
            if (g_settings.pixelPerfect ||
                !previouslyRelative ||
                previousVideoPreset != g_settings.videoPreset ||
                g_settings.relativeWindowScalePpm <= 0) {
                HMONITOR baselineMonitor = nullptr;
                if (g_settings.hasWindowPosition) {
                    baselineMonitor = MonitorFromPoint(
                        POINT{g_settings.windowX, g_settings.windowY},
                        MONITOR_DEFAULTTONULL);
                }
                if (!baselineMonitor) {
                    baselineMonitor = MonitorFromWindow(
                        hwnd, MONITOR_DEFAULTTONEAREST);
                }
                g_settings.relativeWindowScalePpm = RelativeScaleForMonitor(
                    baselineMonitor);
            }
        }
        g_settings.borderlessWindow = SendMessageW(
            state->borderlessCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.windowSnap = SendMessageW(
            state->windowSnapCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SaveSettings();
    }

    state->accepted = accepted;
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<SettingsDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (msg) {
    case WM_CREATE: {
        const HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;
        auto makeLabel = [&](const wchar_t* text, int x, int y) {
            return CreateWindowExW(0, L"STATIC", text,
                                   WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                   x, y, 160, 24, hwnd, nullptr, instance, nullptr);
        };

        state->audioLabel = makeLabel(L"오디오 출력 모드", 24, 24);
        state->audioCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 20, 210, 120, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_AUDIO)), instance, nullptr);
        SendMessageW(state->audioCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         L"WASAPI Shared (호환성 우선 · 권장)"));
        SendMessageW(state->audioCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         L"WASAPI Exclusive (지연 최소화 · 장치 독점)"));
        SendMessageW(state->audioCombo, CB_SETCURSEL,
                     g_settings.audioMode == AudioMode::WasapiExclusive ? 1 : 0, 0);

        state->audioOutputLabel = makeLabel(L"WASAPI 출력 장치", 24, 68);
        state->audioOutputCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            150, 64, 250, 220, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_AUDIO_OUTPUT)),
            instance, nullptr);
        SendMessageW(state->audioOutputCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         L"Windows 기본 출력 장치 따라가기 (권장)"));
        LRESULT selectedAudioEndpoint = 0;
        for (size_t i = 0; i < state->audioEndpoints.size(); ++i) {
            std::wstring label = state->audioEndpoints[i].name;
            if (state->audioEndpoints[i].isDefault) label += L" (현재 기본)";
            SendMessageW(state->audioOutputCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(label.c_str()));
            if (state->audioEndpoints[i].id ==
                g_settings.audioOutputDeviceId) {
                selectedAudioEndpoint = static_cast<LRESULT>(i + 1);
            }
        }
        SendMessageW(state->audioOutputCombo, CB_SETCURSEL,
                     selectedAudioEndpoint, 0);

        state->selectedBufferMs = g_settings.wasapiBufferMs;
        state->selectedSharedPeriodFrames =
            g_settings.wasapiSharedPeriodFrames;

        state->bufferLabel = makeLabel(L"WASAPI 출력 버퍼", 24, 68);
        state->bufferCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 64, 210, 180, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_BUFFER)),
            instance, nullptr);
        PopulateSettingsBufferCombo(state);

        state->audioStatus = CreateWindowExW(
            0, L"STATIC", L"Shared 저지연 지원 확인 중…",
            WS_CHILD | WS_VISIBLE,
            24, 104, 370, 22, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_AUDIO_STATUS)),
            instance, nullptr);

        state->volumeHudLabel = makeLabel(L"볼륨 HUD 위치", 24, 142);
        state->volumeHudCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 138, 210, 160, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_VOLUME_HUD)),
            instance, nullptr);
        for (const wchar_t* label : {L"좌측 상단 (기본)", L"우측 상단",
                                     L"좌측 하단", L"우측 하단"}) {
            SendMessageW(state->volumeHudCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(label));
        }
        SendMessageW(
            state->volumeHudCombo, CB_SETCURSEL,
            static_cast<WPARAM>(g_settings.volumeHudPosition), 0);

        state->driftLabel = makeLabel(L"클록 드리프트 보정", 24, 186);
        state->driftHelp = CreateWindowExW(
            0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                BS_PUSHBUTTON,
            162, 182, 24, 24, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_DRIFT_HELP)),
            instance, nullptr);
        AddSettingsTooltip(
            state, hwnd, state->driftHelp,
            L"소리 찢어짐 방지 · 보정 필요 확인\n\n"
            L"캡처 장치와 출력 장치의 클록 차이를 자동 리샘플링으로 "
            L"보정합니다. Tab OSD를 10~30분 확인하세요.\n\n"
            L"'안정 · 보정 불필요' 또는 '드문 오류'이면 소리에 문제가 "
            L"없는 한 끔을 유지해도 됩니다. '반복 불균형 · 보정 권장'이 "
            L"계속 보이면 자동 리샘플링을 권장합니다. 시작 직후 오류만으로 "
            L"판단하지 마세요.\n\n"
            L"리샘플러와 PCM 안전 대기량은 서로 독립입니다. OSD에서 "
            L"'리샘플러 보정 한계 접근'은 클록 차이, 'PCM 버퍼 부족 "
            L"가능'은 순간 버퍼 여유 부족, '캡처 패킷 지연 감지'는 입력 "
            L"콜백 지연을 뜻합니다. '리샘플러 출력 부족'은 PCM은 있었지만 "
            L"필터가 출력 프레임을 모두 만들지 못한 경우입니다. "
            L"리샘플러가 정상인데 underrun이 나면 "
            L"먼저 PCM 버퍼 목표를 높이세요.\n\n"
            L"OSD의 불균형 ppm은 underrun/overrun 누적량으로 계산한 "
            L"참고값이며 실제 하드웨어 클록을 직접 측정한 값은 아닙니다. "
            L"자동 보정은 작은 오디오 대기량을 추가하고 PCM 샘플을 "
            L"변경합니다.");
        state->driftCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            192, 182, 228, 120, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_DRIFT)),
            instance, nullptr);
        SendMessageW(state->driftCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         L"끔 (원본 PCM · 음질 우선)"));
        SendMessageW(state->driftCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         L"자동 리샘플링 (장시간 안정성 권장)"));
        SendMessageW(
            state->driftCombo, CB_SETCURSEL,
            g_settings.driftCorrection == DriftCorrectionMode::Resample
                ? 1 : 0,
            0);

        state->pcmQueueLabel = makeLabel(L"PCM 버퍼 목표", 24, 230);
        state->pcmQueueHelp = CreateWindowExW(
            0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                BS_PUSHBUTTON,
            162, 226, 24, 24, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_PCM_QUEUE_HELP)),
            instance, nullptr);
        AddSettingsTooltip(
            state, hwnd, state->pcmQueueHelp,
            L"PCM 버퍼 목표 안내\n\n"
            L"캡처 오디오를 재생 전에 확보하는 프로그램 내부 대기량입니다.\n"
            L"10ms는 최저 지연, 15ms는 저지연 목표, 20ms는 안정 권장, "
            L"30ms는 안정성 우선 설정입니다.\n\n"
            L"값을 높이면 순간적인 입력 지연을 흡수할 여유가 커지지만, "
            L"그만큼 오디오 지연이 늘어납니다. WASAPI 출력 버퍼와 클록 "
            L"드리프트 보정과는 독립적으로 조정됩니다.");
        state->pcmQueueCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 226, 210, 140, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_PCM_QUEUE)),
            instance, nullptr);
        const wchar_t* queueLabels[] = {
            L"10 ms (최저 지연)",
            L"15 ms (저지연 목표)",
            L"20 ms (안정 권장)",
            L"30 ms (안정성 우선)"};
        size_t selectedQueue = 0;
        for (size_t i = 0; i < ARRAYSIZE(kPcmQueueOptionsMs); ++i) {
            const LRESULT index = SendMessageW(
                state->pcmQueueCombo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(queueLabels[i]));
            SendMessageW(state->pcmQueueCombo, CB_SETITEMDATA,
                         static_cast<WPARAM>(index),
                         kPcmQueueOptionsMs[i]);
            if (g_settings.pcmQueueTargetMs == kPcmQueueOptionsMs[i]) {
                selectedQueue = i;
            }
        }
        SendMessageW(state->pcmQueueCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(selectedQueue), 0);

        state->muteBackgroundCheck = CreateWindowExW(
            0, L"BUTTON", L"백그라운드에서 자동 음소거",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 318, 390, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_MUTE_BACKGROUND)),
            instance, nullptr);
        SendMessageW(state->muteBackgroundCheck, BM_SETCHECK,
                     g_settings.muteWhenBackground
                         ? BST_CHECKED : BST_UNCHECKED, 0);

        state->presentationLabel = makeLabel(L"화면 표시 방식", 24, 274);
        state->presentationCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 270, 210, 120, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_PRESENTATION)),
            instance, nullptr);
        SendMessageW(state->presentationCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"저지연"));
        SendMessageW(state->presentationCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"VSync"));
        SendMessageW(state->presentationCombo, CB_SETCURSEL,
                     g_settings.presentationMode == PresentationMode::VSync
                         ? 1 : 0, 0);
        state->presentationHelp = CreateWindowExW(
            0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                BS_PUSHBUTTON,
            604, 20, 24, 24, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_PRESENTATION_HELP)),
            instance, nullptr);
        AddSettingsTooltip(
            state, hwnd, state->presentationHelp,
            L"화면 표시 방식 안내\n\n"
            L"저지연: VSync 대기 없이 최신 프레임을 즉시 표시합니다. "
            L"표시 지연을 줄이는 대신 화면 경계가 맞지 않을 때 찢어짐이 "
            L"보일 수 있습니다.\n\n"
            L"VSync: 모니터 주기에 맞춰 표시해 찢어짐을 줄입니다. "
            L"대신 다음 표시 주기까지 기다릴 수 있어 지연이 늘어날 수 "
            L"있고, 주사율이 다른 모니터에서는 프레임 페이싱이 달라질 수 "
            L"있습니다.");

        state->captureDeviceLabel = makeLabel(L"캡처 장치", 430, 68);
        state->captureDeviceCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            550, 64, 245, 220, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_CAPTURE_DEVICE)),
            instance, nullptr);
        SendMessageW(state->captureDeviceCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         L"자동 선택 (GC573 우선 · 권장)"));
        LRESULT selectedCaptureDevice = 0;
        for (size_t i = 0; i < state->captureDevices.size(); ++i) {
            std::wstring label = state->captureDevices[i].name;
            std::wstring lowered = label;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           ::towlower);
            if (lowered.find(L"gc573") == std::wstring::npos &&
                lowered.find(L"live gamer 4k") == std::wstring::npos) {
                label += L" (실험적)";
            }
            SendMessageW(state->captureDeviceCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(label.c_str()));
            if (state->captureDevices[i].id == g_settings.captureDeviceId) {
                selectedCaptureDevice = static_cast<LRESULT>(i + 1);
            }
        }
        SendMessageW(state->captureDeviceCombo, CB_SETCURSEL,
                     selectedCaptureDevice, 0);

        state->videoLabel = makeLabel(L"캡처 해상도", 24, 318);
        state->videoCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 314, 210, 120, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_VIDEO)), instance, nullptr);
        for (const auto& info : kVideoPresets) {
            SendMessageW(state->videoCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(info.label));
        }
        size_t selectedVideo = 1;
        for (size_t i = 0; i < ARRAYSIZE(kVideoPresets); ++i) {
            if (kVideoPresets[i].preset == state->initialVideoPreset) {
                selectedVideo = i;
                break;
            }
        }
        SendMessageW(state->videoCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(selectedVideo), 0);

        state->pixelFormatLabel = makeLabel(L"픽셀 포맷", 430, 156);
        state->pixelFormatCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            550, 152, 245, 160, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_PIXEL_FORMAT)),
            instance, nullptr);
        state->frameRateLabel = makeLabel(L"프레임", 430, 200);
        state->frameRateCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            550, 196, 245, 200, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_FRAME_RATE)),
            instance, nullptr);
        state->videoCapabilityStatus = CreateWindowExW(
            0, L"STATIC", L"지원 모드 확인 중...",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            430, 234, 365, 24, hwnd, nullptr, instance, nullptr);
        PopulatePixelFormatCombo(state);

        state->pixelCheck = CreateWindowExW(
            0, L"BUTTON", L"Pixel-perfect (1:1 · 창 크기 고정)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 362, 250, 28, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_PIXEL)), instance, nullptr);
        SendMessageW(state->pixelCheck, BM_SETCHECK,
                     g_settings.pixelPerfect ? BST_CHECKED : BST_UNCHECKED, 0);

        state->relativeSizeCheck = CreateWindowExW(
            0, L"BUTTON", L"모니터 이동 시 상대적 창 크기 유지 (독립 옵션)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 396, 390, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_RELATIVE_SIZE)),
            instance, nullptr);
        SendMessageW(state->relativeSizeCheck, BM_SETCHECK,
                     g_settings.relativeWindowSize
                         ? BST_CHECKED : BST_UNCHECKED, 0);

        state->relativeSizeWarning = CreateWindowExW(
            0, L"STATIC",
            L"※ Pixel-perfect와 함께 켜면 모니터 이동 시 1:1이 깨질 수 있습니다.",
            WS_CHILD | WS_VISIBLE,
            44, 424, 411, 24, hwnd, nullptr, instance, nullptr);

        state->borderlessCheck = CreateWindowExW(
            0, L"BUTTON", L"제목 표시줄 숨기기 (borderless 창)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 430, 300, 28, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_BORDERLESS)),
            instance, nullptr);
        SendMessageW(state->borderlessCheck, BM_SETCHECK,
                     g_settings.borderlessWindow ? BST_CHECKED : BST_UNCHECKED, 0);

        state->windowSnapCheck = CreateWindowExW(
            0, L"BUTTON", L"창을 모니터 가장자리에 스냅 (권장)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 464, 330, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_WINDOW_SNAP)),
            instance, nullptr);
        SendMessageW(state->windowSnapCheck, BM_SETCHECK,
                     g_settings.windowSnap ? BST_CHECKED : BST_UNCHECKED, 0);

        state->saveLogCheck = CreateWindowExW(
            0, L"BUTTON", L"진단 로그 파일 저장 (logs 폴더)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            430, 374, 365, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_SAVE_LOG)),
            instance, nullptr);
        SendMessageW(state->saveLogCheck, BM_SETCHECK,
                     g_settings.saveLog ? BST_CHECKED : BST_UNCHECKED, 0);

        state->startButton = CreateWindowExW(
            0, L"BUTTON", L"시작", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
            285, 520, 80, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_START)), instance, nullptr);
        state->cancelButton = CreateWindowExW(
            0, L"BUTTON", L"취소", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            375, 520, 80, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_CANCEL)), instance, nullptr);
        UpdateVideoCapabilityStatus(state);

        const UINT initialDpi = GetDpiForWindow(hwnd);
        ApplySettingsFont(state, hwnd, initialDpi);
        LayoutSettingsControls(state, initialDpi);
        RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                         RDW_UPDATENOW);

        const std::wstring endpointId = SelectedAudioEndpointId(state);
        state->probeThread = std::thread([state, hwnd, endpointId]() {
            state->probe = ProbeAudioClient3Support(endpointId);
            state->probeReady.store(true, std::memory_order_release);
            PostMessageW(hwnd, WM_AUDIOCLIENT3_PROBE_COMPLETE, 0, 0);
        });
        return 0;
    }

    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ApplySettingsFont(state, hwnd, HIWORD(wParam));
        LayoutSettingsControls(state, HIWORD(wParam));
        RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                         RDW_UPDATENOW);
        return 0;
    }

    case WM_GETDPISCALEDSIZE:
        if (lParam) {
            *reinterpret_cast<SIZE*>(lParam) =
                SettingsDialogOuterSize(hwnd, static_cast<UINT>(wParam));
            return TRUE;
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                             RDW_UPDATENOW);
        }
        break;

    case WM_AUDIOCLIENT3_PROBE_COMPLETE:
        UpdateAudioClient3Status(state);
        if (SettingsUsesSharedMode(state)) {
            RememberCurrentBufferChoice(state);
            PopulateSettingsBufferCombo(state);
        }
        return 0;

    case WM_SETTINGS_TOOLTIP_SHOW: {
        const HWND target = reinterpret_cast<HWND>(wParam);
        const HWND tooltip = reinterpret_cast<HWND>(lParam);
        if (state && IsSettingsHelpControl(state, target) &&
            tooltip == state->tooltipWindow) {
            if (state->activeTooltipTarget &&
                state->activeTooltipTarget != target) {
                TrackSettingsTooltip(state->activeTooltipTarget, tooltip,
                                     false);
            }
            POINT cursor{};
            GetCursorPos(&cursor);
            SendMessageW(tooltip, TTM_TRACKPOSITION, 0,
                         MAKELPARAM(cursor.x + 16, cursor.y + 20));
            TrackSettingsTooltip(target, tooltip, true);
            state->activeTooltipTarget = target;
        }
        return 0;
    }

    case WM_SETTINGS_TOOLTIP_HIDE: {
        const HWND target = reinterpret_cast<HWND>(wParam);
        const HWND tooltip = reinterpret_cast<HWND>(lParam);
        if (state && target == state->activeTooltipTarget &&
            IsSettingsHelpControl(state, target) &&
            tooltip == state->tooltipWindow) {
            RECT targetRect{};
            POINT cursor{};
            GetWindowRect(target, &targetRect);
            GetCursorPos(&cursor);
            if (PtInRect(&targetRect, cursor)) return 0;
            TrackSettingsTooltip(target, tooltip, false);
            state->activeTooltipTarget = nullptr;
        }
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SETTINGS_AUDIO &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            RememberCurrentBufferChoice(state);
            PopulateSettingsBufferCombo(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_AUDIO_OUTPUT &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            if (state->probeThread.joinable()) state->probeThread.join();
            state->probeReady.store(false, std::memory_order_release);
            UpdateAudioClient3Status(state);
            const std::wstring endpointId = SelectedAudioEndpointId(state);
            state->probeThread = std::thread([state, hwnd, endpointId]() {
                state->probe = ProbeAudioClient3Support(endpointId);
                state->probeReady.store(true, std::memory_order_release);
                PostMessageW(hwnd, WM_AUDIOCLIENT3_PROBE_COMPLETE, 0, 0);
            });
            return 0;
        }
        if ((LOWORD(wParam) == IDC_SETTINGS_CAPTURE_DEVICE ||
             LOWORD(wParam) == IDC_SETTINGS_VIDEO) &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            PopulatePixelFormatCombo(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PIXEL_FORMAT &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            PopulateFrameRateCombo(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PRESENTATION_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                L"저지연: VSync 대기 없이 최신 프레임을 즉시 표시합니다. "
                L"표시 지연을 줄이는 대신 화면 경계가 맞지 않을 때 "
                L"찢어짐이 보일 수 있습니다.\n\n"
                L"VSync: 모니터 주기에 맞춰 표시해 찢어짐을 줄입니다. "
                L"대신 다음 표시 주기까지 기다릴 수 있어 지연이 늘어날 수 "
                L"있고, 주사율이 다른 모니터에서는 프레임 페이싱이 달라질 "
                L"수 있습니다.",
                L"화면 표시 방식", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PCM_QUEUE_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                L"PCM 버퍼 목표는 캡처 오디오를 재생 전에 확보하는 "
                L"프로그램 내부 대기량입니다.\n\n"
                L"10ms는 최저 지연, 15ms는 저지연 목표, 20ms는 안정 권장, "
                L"30ms는 안정성 우선 설정입니다.\n\n"
                L"값을 높이면 순간적인 입력 지연을 흡수할 여유가 커지지만, "
                L"그만큼 오디오 지연이 늘어납니다. WASAPI 출력 버퍼와 "
                L"클록 드리프트 보정과는 독립적으로 조정됩니다.",
                L"PCM 버퍼 목표", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_DRIFT_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                L"소리 찢어짐 방지 · 보정 필요 확인\n\n"
                L"캡처 장치와 출력 장치의 클록은 아주 조금씩 다른 "
                L"속도로 진행될 수 있습니다. 자동 보정은 리샘플링으로 "
                L"그 차이를 따라가 장시간 재생 중 끊김이나 갈라짐을 "
                L"줄입니다.\n\n"
                L"판단 방법\n"
                L"1. 프로그램을 10~30분 실행하고 Tab OSD를 확인합니다.\n"
                L"2. '안정 · 보정 불필요': 끔을 유지해도 됩니다.\n"
                L"3. '드문 오류 · 끔 유지 가능': 실제 소리 문제가 없다면 "
                L"끔을 유지합니다.\n"
                L"4. '반복 불균형 · 보정 권장': 자동 리샘플링을 "
                L"권장합니다.\n"
                L"5. 보정 중에도 오류가 계속되면 PCM 버퍼 목표를 한 단계 "
                L"높입니다.\n\n"
                L"원인 구분\n"
                L"- 리샘플러 보정 한계 접근: 클록 속도 차이가 큽니다.\n"
                L"- PCM 버퍼 부족 가능: 리샘플링 여부와 별개로 PCM 버퍼 "
                L"여유가 소진됐습니다. PCM 버퍼 목표를 높입니다.\n"
                L"- 캡처 패킷 지연 감지: 입력 콜백이 평소 패킷 주기보다 "
                L"5ms 이상 늦었습니다. PCM 버퍼 목표를 높여 순간 지터를 "
                L"흡수합니다.\n"
                L"- 리샘플러 정상 작동인데 underrun 발생: 보정 자체보다 "
                L"PCM 버퍼 문제를 먼저 의심합니다.\n\n"
                L"- 리샘플러 출력 부족 감지: PCM 양은 있었지만 필터의 "
                L"look-ahead가 부족했습니다. PCM 버퍼 목표를 높입니다.\n\n"
                L"클록 보정과 PCM 버퍼 목표는 독립 설정입니다. 10ms는 "
                L"최저 지연, 15ms는 리샘플링 저지연 권장, 20ms는 안정 "
                L"권장입니다.\n\n"
                L"OSD의 underrun/overrun에는 횟수와 실제 부족/폐기 시간이 "
                L"함께 표시됩니다. 오류 추세의 회/h는 누적 빈도이고, "
                L"불균형 ppm(추정)은 누적 PCM 차이를 실행 시간으로 나눈 "
                L"참고값입니다. 스케줄링 지연도 포함되므로 하드웨어 클록 "
                L"측정값으로 해석하면 안 됩니다. 시작 직후 한두 번만 발생한 "
                L"오류는 장시간 반복 여부를 더 확인하세요.\n\n"
                L"대신 작은 오디오 대기량이 추가되고 PCM 샘플이 "
                L"변경되므로, 원본 음질과 최저 지연이 우선이면 끔을 "
                L"사용하세요.",
                L"클록 드리프트 보정", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_START) {
            FinishSettingsDialog(hwnd, state, true);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_CANCEL) {
            FinishSettingsDialog(hwnd, state, false);
            return 0;
        }
        break;

    case WM_CLOSE:
        FinishSettingsDialog(hwnd, state, false);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool ShowSettingsDialog(HINSTANCE hInst) {
    static const wchar_t kSettingsClass[] = L"LowLatencyCaptureViewerSettingsDialogClass";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kSettingsClass;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        registered = true;
    }

    SettingsDialogState state{};
    state.captureDevices = EnumerateCaptureDevices();
    state.audioEndpoints = EnumerateAudioEndpoints();
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR settingsMonitor = MonitorFromPoint(
        cursor, MONITOR_DEFAULTTOPRIMARY);
    UINT settingsDpiX = GetDpiForSystem();
    UINT settingsDpiY = settingsDpiX;
    if (FAILED(GetDpiForMonitor(settingsMonitor, MDT_EFFECTIVE_DPI,
                                &settingsDpiX, &settingsDpiY))) {
        settingsDpiX = GetDpiForSystem();
    }
    const UINT settingsDpi = settingsDpiX;
    const DWORD settingsStyle = WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    const DWORD settingsExStyle = WS_EX_DLGMODALFRAME;
    RECT settingsRect{0, 0,
                      SettingsPixels(kSettingsClientWidthDip, settingsDpi),
                      SettingsPixels(kSettingsClientHeightDip, settingsDpi)};
    AdjustWindowRectExForDpi(&settingsRect, settingsStyle, FALSE,
                             settingsExStyle, settingsDpi);
    const SIZE settingsOuter{settingsRect.right - settingsRect.left,
                             settingsRect.bottom - settingsRect.top};
    MONITORINFO settingsMonitorInfo{sizeof(settingsMonitorInfo)};
    GetMonitorInfoW(settingsMonitor, &settingsMonitorInfo);
    const int monitorWidth = settingsMonitorInfo.rcMonitor.right -
                             settingsMonitorInfo.rcMonitor.left;
    const int monitorHeight = settingsMonitorInfo.rcMonitor.bottom -
                              settingsMonitorInfo.rcMonitor.top;
    state.initialVideoPreset =
        monitorWidth <= 1920 && monitorHeight <= 1080
            ? VideoPreset::R1920x1080
            : VideoPreset::R2560x1440;
    const RECT work = settingsMonitorInfo.rcWork;
    const int settingsX = work.left +
        ((work.right - work.left) - settingsOuter.cx) / 2;
    const int settingsY = work.top +
        ((work.bottom - work.top) - settingsOuter.cy) / 2;
    HWND hwnd = CreateWindowExW(
        settingsExStyle,
        kSettingsClass, L"Low Latency Capture Viewer 설정",
        settingsStyle,
        settingsX, settingsY, settingsOuter.cx, settingsOuter.cy,
        nullptr, nullptr, hInst, &state);
    if (!hwnd) return false;

    SetForegroundWindow(hwnd);
    MSG msg{};
    while (IsWindow(hwnd)) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) break;
        if (msg.message == WM_MOUSEMOVE && state.tooltipWindow) {
            const HWND target = IsSettingsHelpControl(&state, msg.hwnd)
                ? msg.hwnd : state.activeTooltipTarget;
            if (target) {
                SendMessageW(hwnd,
                             IsSettingsHelpControl(&state, msg.hwnd)
                                 ? WM_SETTINGS_TOOLTIP_SHOW
                                 : WM_SETTINGS_TOOLTIP_HIDE,
                             reinterpret_cast<WPARAM>(target),
                             reinterpret_cast<LPARAM>(state.tooltipWindow));
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (state.probeThread.joinable()) state.probeThread.join();
    for (HFONT font : state.uiFonts) DeleteObject(font);
    return state.accepted;
}

// -----------------------------------------------------------------------------
// Win32 UI
// -----------------------------------------------------------------------------

static bool g_fullscreen = false;
static WINDOWPLACEMENT g_prevPlacement{ sizeof(g_prevPlacement) };
static LONG_PTR g_prevStyle = 0;
static RECT g_lastWindowedRect{};
static bool g_haveLastWindowedRect = false;
static bool g_windowPositionPersisted = false;
static constexpr int kWindowSnapDistanceDip = 20;
static constexpr int kWindowSnapReleaseDip = 20;

enum class HorizontalSnapEdge { None, Left, Right };
enum class VerticalSnapEdge { None, Top, Bottom };

struct WindowSnapState {
    HorizontalSnapEdge horizontal = HorizontalSnapEdge::None;
    VerticalSnapEdge vertical = VerticalSnapEdge::None;
    int horizontalCursorAnchor = 0;
    int verticalCursorAnchor = 0;
    int horizontalSnapCoordinate = 0;
    int verticalSnapCoordinate = 0;
    bool suppressHorizontal = false;
    bool suppressVertical = false;
};

static WindowSnapState g_windowSnapState{};

static void ResetWindowSnapState() {
    g_windowSnapState = {};
}

static UINT EffectiveMonitorDpi(HMONITOR monitor, HWND fallbackWindow) {
    UINT dpiX = 0;
    UINT dpiY = 0;
    if (monitor && SUCCEEDED(GetDpiForMonitor(
                       monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX) {
        return dpiX;
    }
    const UINT windowDpi = fallbackWindow ? GetDpiForWindow(fallbackWindow) : 0;
    return windowDpi ? windowDpi : USER_DEFAULT_SCREEN_DPI;
}

static SIZE OuterSizeForClientPixels(int clientWidth, int clientHeight,
                                     DWORD style, DWORD exStyle, UINT dpi) {
    RECT rect{0, 0, clientWidth, clientHeight};
    if (!AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi)) {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

static SIZE DesiredClientPixelsForMonitor(HMONITOR monitor) {
    const auto& video = CurrentVideoPreset();
    if (!g_settings.relativeWindowSize) {
        return g_settings.pixelPerfect
                   ? SIZE{video.width, video.height}
                   : SIZE{1280, 720};
    }

    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return SIZE{video.width, video.height};
    }
    int scale = g_settings.relativeWindowScalePpm;
    if (scale <= 0) scale = RelativeScaleForMonitor(monitor);
    const int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
    const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
    const int maximumWidth = (std::max)(320,
        MulDiv(monitorWidth, scale, kRelativeScaleUnit));
    const int maximumHeight = (std::max)(180,
        MulDiv(monitorHeight, scale, kRelativeScaleUnit));
    int clientWidth = maximumWidth;
    int clientHeight = MulDiv(clientWidth, video.height, video.width);
    if (clientHeight > maximumHeight) {
        clientHeight = maximumHeight;
        clientWidth = MulDiv(clientHeight, video.width, video.height);
    }
    return SIZE{(std::max)(1, clientWidth),
                (std::max)(1, clientHeight)};
}

static SIZE InitialClientPixelsForMonitor(HMONITOR monitor) {
    const auto& video = CurrentVideoPreset();
    // Pixel-perfect defines the initial 1:1 size. Monitor-relative sizing is
    // independent and may change that size later when another monitor is
    // entered, but it must not override the initial pixel-perfect contract.
    if (g_settings.pixelPerfect) return SIZE{video.width, video.height};
    return DesiredClientPixelsForMonitor(monitor);
}

static SIZE DesiredWindowOuterSize(HWND hwnd, HMONITOR monitor, UINT dpi) {
    const SIZE client = DesiredClientPixelsForMonitor(monitor);
    const DWORD style =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    return OuterSizeForClientPixels(client.cx, client.cy,
                                    style, exStyle, dpi);
}

static void NormalizeWindowSize(HWND hwnd, bool clampToWorkArea) {
    // Only strict pixel-perfect without monitor-relative behavior is fixed.
    // When both options are enabled, relative sizing is allowed to change the
    // size programmatically as the window crosses monitors.
    if (!hwnd || !g_settings.pixelPerfect ||
        g_settings.relativeWindowSize || g_fullscreen) return;
    RECT current{};
    if (!GetWindowRect(hwnd, &current)) return;
    const HMONITOR currentMonitor =
        MonitorFromRect(&current, MONITOR_DEFAULTTONEAREST);
    const UINT dpi = EffectiveMonitorDpi(currentMonitor, hwnd);
    const SIZE desired = DesiredWindowOuterSize(
        hwnd, currentMonitor, dpi);
    int x = current.left;
    int y = current.top;
    if (clampToWorkArea) {
        MONITORINFO info{sizeof(info)};
        const HMONITOR monitor =
            MonitorFromRect(&current, MONITOR_DEFAULTTONEAREST);
        if (monitor && GetMonitorInfoW(monitor, &info)) {
            const int maximumX =
                (std::max)(info.rcWork.left, info.rcWork.right - desired.cx);
            const int maximumY =
                (std::max)(info.rcWork.top, info.rcWork.bottom - desired.cy);
            x = std::clamp(x, static_cast<int>(info.rcWork.left), maximumX);
            y = std::clamp(y, static_cast<int>(info.rcWork.top), maximumY);
        }
    }

    const int currentWidth = current.right - current.left;
    const int currentHeight = current.bottom - current.top;
    if (currentWidth != desired.cx || currentHeight != desired.cy ||
        current.left != x || current.top != y) {
        SetWindowPos(hwnd, nullptr, x, y, desired.cx, desired.cy,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static HMONITOR g_relativeMoveMonitor = nullptr;
static bool g_manualResizeInProgress = false;

static void RememberRelativeScaleFromWindow(HWND hwnd) {
    if (!hwnd || !g_settings.relativeWindowSize ||
        g_settings.pixelPerfect) return;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return;
    const HMONITOR monitor =
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return;
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    const int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
    const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
    if (clientWidth <= 0 || clientHeight <= 0 ||
        monitorWidth <= 0 || monitorHeight <= 0) return;
    const int widthScale = static_cast<int>(
        static_cast<int64_t>(clientWidth) * kRelativeScaleUnit /
        monitorWidth);
    const int heightScale = static_cast<int>(
        static_cast<int64_t>(clientHeight) * kRelativeScaleUnit /
        monitorHeight);
    g_settings.relativeWindowScalePpm = std::clamp(
        (std::max)(widthScale, heightScale),
        kRelativeScaleUnit / 4, kRelativeScaleUnit);
}

static void ApplyRelativeSizeForMonitor(HWND hwnd, HMONITOR monitor,
                                        const POINT& cursor,
                                        RECT& movingRect) {
    if (!g_settings.relativeWindowSize || !monitor ||
        monitor == g_relativeMoveMonitor) {
        return;
    }
    const int oldWidth = movingRect.right - movingRect.left;
    const int oldHeight = movingRect.bottom - movingRect.top;
    if (oldWidth <= 0 || oldHeight <= 0) return;

    const UINT dpi = EffectiveMonitorDpi(monitor, hwnd);
    const SIZE desired = DesiredWindowOuterSize(hwnd, monitor, dpi);
    const double cursorRatioX = std::clamp(
        static_cast<double>(cursor.x - movingRect.left) / oldWidth,
        0.0, 1.0);
    const double cursorRatioY = std::clamp(
        static_cast<double>(cursor.y - movingRect.top) / oldHeight,
        0.0, 1.0);
    movingRect.left = cursor.x -
        static_cast<int>(std::lround(cursorRatioX * desired.cx));
    movingRect.top = cursor.y -
        static_cast<int>(std::lround(cursorRatioY * desired.cy));
    movingRect.right = movingRect.left + desired.cx;
    movingRect.bottom = movingRect.top + desired.cy;
    fwprintf(stderr,
             L"[video] monitor-relative move: outer %ld x %ld at %u dpi\n",
             desired.cx, desired.cy, dpi);
    g_relativeMoveMonitor = monitor;
}

struct MonitorLookup {
    std::wstring wantedDevice;
    HMONITOR match = nullptr;
    MONITORINFOEXW info{};
};

static BOOL CALLBACK FindMonitorCallback(HMONITOR monitor, HDC, LPRECT,
                                         LPARAM contextValue) {
    auto* context = reinterpret_cast<MonitorLookup*>(contextValue);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) &&
        _wcsicmp(info.szDevice, context->wantedDevice.c_str()) == 0) {
        context->match = monitor;
        context->info = info;
        return FALSE;
    }
    return TRUE;
}

static bool RestoredWindowOrigin(const SIZE& outerSize, POINT& origin) {
    if (!g_settings.hasWindowPosition) return false;
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = nullptr;
    if (!g_settings.monitorDevice.empty()) {
        MonitorLookup lookup{};
        lookup.wantedDevice = g_settings.monitorDevice;
        EnumDisplayMonitors(nullptr, nullptr, FindMonitorCallback,
                            reinterpret_cast<LPARAM>(&lookup));
        monitor = lookup.match;
        if (monitor) monitorInfo = lookup.info;
    }
    if (!monitor) {
        const POINT saved{g_settings.windowX, g_settings.windowY};
        monitor = MonitorFromPoint(saved, MONITOR_DEFAULTTONULL);
        if (monitor) GetMonitorInfoW(monitor, &monitorInfo);
    }
    if (!monitor) {
        monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(monitor, &monitorInfo);
        origin.x = monitorInfo.rcWork.left +
                   ((monitorInfo.rcWork.right - monitorInfo.rcWork.left) -
                    outerSize.cx) / 2;
        origin.y = monitorInfo.rcWork.top +
                   ((monitorInfo.rcWork.bottom - monitorInfo.rcWork.top) -
                    outerSize.cy) / 2;
        return true;
    }

    const RECT work = monitorInfo.rcWork;
    const int maximumX = (std::max)(work.left, work.right - outerSize.cx);
    const int maximumY = (std::max)(work.top, work.bottom - outerSize.cy);
    origin.x = std::clamp(g_settings.windowX,
                          static_cast<int>(work.left), maximumX);
    origin.y = std::clamp(g_settings.windowY,
                          static_cast<int>(work.top), maximumY);
    return true;
}

static void PersistWindowPosition(HWND hwnd) {
    RECT rect{};
    if (g_fullscreen && g_haveLastWindowedRect) {
        rect = g_lastWindowedRect;
    } else if (!GetWindowRect(hwnd, &rect)) {
        return;
    }
    const HMONITOR monitor =
        MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfoW(monitor, &monitorInfo);
    g_settings.hasWindowPosition = true;
    g_settings.windowX = rect.left;
    g_settings.windowY = rect.top;
    g_settings.monitorDevice = monitorInfo.szDevice;

    const std::wstring path = SettingsPath();
    wchar_t value[32]{};
    swprintf_s(value, L"%d", g_settings.windowX);
    WritePrivateProfileStringW(L"Window", L"X", value, path.c_str());
    swprintf_s(value, L"%d", g_settings.windowY);
    WritePrivateProfileStringW(L"Window", L"Y", value, path.c_str());
    WritePrivateProfileStringW(L"Window", L"Monitor",
                               g_settings.monitorDevice.c_str(),
                               path.c_str());
}

static void ApplyWindowEdgeSnap(HWND hwnd, RECT& movingRect) {
    if (!g_settings.windowSnap || g_fullscreen ||
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        ResetWindowSnapState();
        return;
    }

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const HMONITOR monitor =
        MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return;

    const UINT dpi = EffectiveMonitorDpi(monitor, hwnd);
    const int snapDistance =
        (std::max)(1, MulDiv(kWindowSnapDistanceDip, dpi,
                            USER_DEFAULT_SCREEN_DPI));
    const int releaseDistance =
        (std::max)(1, MulDiv(kWindowSnapReleaseDip, dpi,
                            USER_DEFAULT_SCREEN_DPI));
    const int width = movingRect.right - movingRect.left;
    const int height = movingRect.bottom - movingRect.top;
    const RECT work = monitorInfo.rcWork;
    bool horizontalReleased = false;
    if (g_windowSnapState.horizontal == HorizontalSnapEdge::Left) {
        const int cursorDelta =
            cursor.x - g_windowSnapState.horizontalCursorAnchor;
        const bool release = std::abs(cursorDelta) >= releaseDistance;
        if (release) {
            g_windowSnapState.horizontal = HorizontalSnapEdge::None;
            g_windowSnapState.suppressHorizontal = true;
            horizontalReleased = true;
        } else {
            movingRect.left = g_windowSnapState.horizontalSnapCoordinate;
            movingRect.right = movingRect.left + width;
        }
    } else if (g_windowSnapState.horizontal == HorizontalSnapEdge::Right) {
        const int cursorDelta =
            cursor.x - g_windowSnapState.horizontalCursorAnchor;
        const bool release = std::abs(cursorDelta) >= releaseDistance;
        if (release) {
            g_windowSnapState.horizontal = HorizontalSnapEdge::None;
            g_windowSnapState.suppressHorizontal = true;
            horizontalReleased = true;
        } else {
            movingRect.right = g_windowSnapState.horizontalSnapCoordinate;
            movingRect.left = movingRect.right - width;
        }
    }

    if (g_windowSnapState.horizontal == HorizontalSnapEdge::None) {
        const bool outsideHorizontalZone =
            std::abs(movingRect.left - work.left) > snapDistance &&
            std::abs(movingRect.right - work.right) > snapDistance;
        if (g_windowSnapState.suppressHorizontal) {
            if (outsideHorizontalZone) {
                g_windowSnapState.suppressHorizontal = false;
            }
        } else if (!horizontalReleased) {
            if (std::abs(movingRect.left - work.left) <= snapDistance) {
                movingRect.left = work.left;
                movingRect.right = movingRect.left + width;
                g_windowSnapState.horizontal = HorizontalSnapEdge::Left;
                g_windowSnapState.horizontalCursorAnchor = cursor.x;
                g_windowSnapState.horizontalSnapCoordinate = work.left;
            } else if (std::abs(movingRect.right - work.right) <= snapDistance) {
                movingRect.right = work.right;
                movingRect.left = movingRect.right - width;
                g_windowSnapState.horizontal = HorizontalSnapEdge::Right;
                g_windowSnapState.horizontalCursorAnchor = cursor.x;
                g_windowSnapState.horizontalSnapCoordinate = work.right;
            }
        }
    }

    bool verticalReleased = false;
    if (g_windowSnapState.vertical == VerticalSnapEdge::Top) {
        const int cursorDelta =
            cursor.y - g_windowSnapState.verticalCursorAnchor;
        const bool release = std::abs(cursorDelta) >= releaseDistance;
        if (release) {
            g_windowSnapState.vertical = VerticalSnapEdge::None;
            g_windowSnapState.suppressVertical = true;
            verticalReleased = true;
        } else {
            movingRect.top = g_windowSnapState.verticalSnapCoordinate;
            movingRect.bottom = movingRect.top + height;
        }
    } else if (g_windowSnapState.vertical == VerticalSnapEdge::Bottom) {
        const int cursorDelta =
            cursor.y - g_windowSnapState.verticalCursorAnchor;
        const bool release = std::abs(cursorDelta) >= releaseDistance;
        if (release) {
            g_windowSnapState.vertical = VerticalSnapEdge::None;
            g_windowSnapState.suppressVertical = true;
            verticalReleased = true;
        } else {
            movingRect.bottom = g_windowSnapState.verticalSnapCoordinate;
            movingRect.top = movingRect.bottom - height;
        }
    }

    if (g_windowSnapState.vertical == VerticalSnapEdge::None) {
        const bool outsideVerticalZone =
            std::abs(movingRect.top - work.top) > snapDistance &&
            std::abs(movingRect.bottom - work.bottom) > snapDistance;
        if (g_windowSnapState.suppressVertical) {
            if (outsideVerticalZone) {
                g_windowSnapState.suppressVertical = false;
            }
        } else if (!verticalReleased) {
            if (std::abs(movingRect.top - work.top) <= snapDistance) {
                movingRect.top = work.top;
                movingRect.bottom = movingRect.top + height;
                g_windowSnapState.vertical = VerticalSnapEdge::Top;
                g_windowSnapState.verticalCursorAnchor = cursor.y;
                g_windowSnapState.verticalSnapCoordinate = work.top;
            } else if (std::abs(movingRect.bottom - work.bottom) <= snapDistance) {
                movingRect.bottom = work.bottom;
                movingRect.top = movingRect.bottom - height;
                g_windowSnapState.vertical = VerticalSnapEdge::Bottom;
                g_windowSnapState.verticalCursorAnchor = cursor.y;
                g_windowSnapState.verticalSnapCoordinate = work.bottom;
            }
        }
    }
}

static void ConstrainWindowRectToVideoAspect(HWND hwnd, RECT& sizingRect,
                                             UINT sizingEdge) {
    if (!hwnd || g_fullscreen || g_settings.pixelPerfect) return;
    const auto& video = CurrentVideoPreset();
    if (video.width <= 0 || video.height <= 0) return;

    const UINT dpi = GetDpiForWindow(hwnd);
    const DWORD style =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    RECT frame{0, 0, 0, 0};
    if (!AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, dpi)) {
        AdjustWindowRectEx(&frame, style, FALSE, exStyle);
    }
    const int frameWidth = static_cast<int>(frame.right - frame.left);
    const int frameHeight = static_cast<int>(frame.bottom - frame.top);
    constexpr int kMinimumClientWidth = 320;
    constexpr int kMinimumClientHeight = 180;
    const int minimumOuterWidth = frameWidth + kMinimumClientWidth;
    const int minimumOuterHeight = frameHeight + kMinimumClientHeight;
    const int proposedWidth = (std::max)(
        minimumOuterWidth,
        static_cast<int>(sizingRect.right - sizingRect.left));
    const int proposedHeight = (std::max)(
        minimumOuterHeight,
        static_cast<int>(sizingRect.bottom - sizingRect.top));

    auto heightFromWidth = [&](int outerWidth) {
        const int clientWidth =
            (std::max)(kMinimumClientWidth, outerWidth - frameWidth);
        return frameHeight + MulDiv(clientWidth, video.height, video.width);
    };
    auto widthFromHeight = [&](int outerHeight) {
        const int clientHeight =
            (std::max)(kMinimumClientHeight, outerHeight - frameHeight);
        return frameWidth + MulDiv(clientHeight, video.width, video.height);
    };

    const bool verticalEdge =
        sizingEdge == WMSZ_TOP || sizingEdge == WMSZ_BOTTOM;
    const bool horizontalEdge =
        sizingEdge == WMSZ_LEFT || sizingEdge == WMSZ_RIGHT;
    int outerWidth = proposedWidth;
    int outerHeight = proposedHeight;
    if (verticalEdge) {
        outerWidth = widthFromHeight(proposedHeight);
    } else if (horizontalEdge) {
        outerHeight = heightFromWidth(proposedWidth);
    } else {
        const int widthDrivenHeight = heightFromWidth(proposedWidth);
        const int heightDrivenWidth = widthFromHeight(proposedHeight);
        if (std::abs(widthDrivenHeight - proposedHeight) <=
            std::abs(heightDrivenWidth - proposedWidth)) {
            outerHeight = widthDrivenHeight;
        } else {
            outerWidth = heightDrivenWidth;
        }
    }

    const LONG left = sizingRect.left;
    const LONG top = sizingRect.top;
    const LONG right = sizingRect.right;
    const LONG bottom = sizingRect.bottom;
    switch (sizingEdge) {
    case WMSZ_LEFT:
        sizingRect.left = right - outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    case WMSZ_RIGHT:
        sizingRect.right = left + outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    case WMSZ_TOP:
        sizingRect.top = bottom - outerHeight;
        sizingRect.right = left + outerWidth;
        break;
    case WMSZ_BOTTOM:
        sizingRect.bottom = top + outerHeight;
        sizingRect.right = left + outerWidth;
        break;
    case WMSZ_TOPLEFT:
        sizingRect.left = right - outerWidth;
        sizingRect.top = bottom - outerHeight;
        break;
    case WMSZ_TOPRIGHT:
        sizingRect.right = left + outerWidth;
        sizingRect.top = bottom - outerHeight;
        break;
    case WMSZ_BOTTOMLEFT:
        sizingRect.left = right - outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    case WMSZ_BOTTOMRIGHT:
        sizingRect.right = left + outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    default:
        break;
    }
}

static LRESULT BorderlessHitTest(HWND hwnd, LPARAM lParam) {
    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return HTCLIENT;
    const POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (!g_settings.pixelPerfect) {
        const UINT dpi = GetDpiForWindow(hwnd);
        const int grip = (std::max)(6, MulDiv(8, dpi, 96));
        const bool left = cursor.x >= windowRect.left &&
                          cursor.x < windowRect.left + grip;
        const bool right = cursor.x < windowRect.right &&
                           cursor.x >= windowRect.right - grip;
        const bool top = cursor.y >= windowRect.top &&
                         cursor.y < windowRect.top + grip;
        const bool bottom = cursor.y < windowRect.bottom &&
                            cursor.y >= windowRect.bottom - grip;
        if (left && top) return HTTOPLEFT;
        if (right && top) return HTTOPRIGHT;
        if (left && bottom) return HTBOTTOMLEFT;
        if (right && bottom) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    return HTCAPTION;
}

static LRESULT CALLBACK VideoHostSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR) {
    if (msg == WM_NCHITTEST) {
        const HWND parent = GetParent(hwnd);
        if (parent && g_settings.borderlessWindow && !g_fullscreen) {
            // Let the same-thread parent perform the hit test. Returning an
            // edge code from the child would try to resize the child instead.
            return HTTRANSPARENT;
        }
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, VideoHostSubclassProc, 1);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void ToggleFullscreen(HWND hwnd) {
    if (!g_fullscreen) {
        GetWindowRect(hwnd, &g_lastWindowedRect);
        g_haveLastWindowedRect = true;
        g_prevStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
        GetWindowPlacement(hwnd, &g_prevPlacement);

        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);

        SetWindowLongPtrW(hwnd, GWL_STYLE, g_prevStyle & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        ShowCursor(FALSE);
        g_fullscreen = true;
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_prevStyle);
        SetWindowPlacement(hwnd, &g_prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
        ShowCursor(TRUE);
        g_fullscreen = false;
    }
}

static bool SelectedResolutionMatchesMonitor(HWND hwnd) {
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return false;

    const auto& video = CurrentVideoPreset();
    const int monitorWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
    const int monitorHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
    return video.width == monitorWidth && video.height == monitorHeight;
}

static constexpr UINT WM_TOGGLE_RUNTIME_OSD = WM_APP + 91;

static void FormatAudioErrorAge(uint64_t lastErrorMs, uint64_t nowMs,
                                wchar_t* output, size_t outputCount) {
    if (!output || outputCount == 0) return;
    if (!lastErrorMs || nowMs < lastErrorMs) {
        wcscpy_s(output, outputCount, L"없음");
        return;
    }
    const uint64_t ageSeconds = (nowMs - lastErrorMs) / 1000;
    if (ageSeconds < 2) {
        wcscpy_s(output, outputCount, L"방금");
    } else if (ageSeconds < 60) {
        swprintf_s(output, outputCount, L"%llu초 전",
                   static_cast<unsigned long long>(ageSeconds));
    } else if (ageSeconds < 3600) {
        swprintf_s(output, outputCount, L"%llu분 %llu초 전",
                   static_cast<unsigned long long>(ageSeconds / 60),
                   static_cast<unsigned long long>(ageSeconds % 60));
    } else {
        swprintf_s(output, outputCount, L"%llu시간 %llu분 전",
                   static_cast<unsigned long long>(ageSeconds / 3600),
                   static_cast<unsigned long long>((ageSeconds / 60) % 60));
    }
}

static std::wstring BuildRuntimeOsdText(int outputWidth, int outputHeight) {
    const auto& preset = CurrentVideoPreset();
    auto compactName = [](const std::wstring& value, size_t maximum) {
        if (value.size() <= maximum) return value;
        return value.substr(0, maximum > 1 ? maximum - 1 : 0) + L"…";
    };
    const std::wstring captureName = compactName(g_activeCaptureDeviceName, 34);
    const std::wstring outputName = compactName(ActiveAudioOutputName(), 26);
    const VideoPixelFormat activeFormat = static_cast<VideoPixelFormat>(
        g_activePixelFormat.load(std::memory_order_acquire));
    const wchar_t* chromaText = activeFormat == VideoPixelFormat::Yuy2
                                    ? L"4:2:2" : L"4:2:0";
    const int configuredFps =
        g_videoConfiguredFps.load(std::memory_order_acquire) > 0
            ? g_videoConfiguredFps.load(std::memory_order_relaxed)
            : RequestedVideoFrameRate();
    const int64_t latencyUs =
        g_videoAppLatencyUs.load(std::memory_order_acquire);
    const UINT32 audioFrames =
        g_audioActualBufferFrames.load(std::memory_order_acquire);
    const UINT32 capturePacketFrames =
        g_audioCapturePacketFrames.load(std::memory_order_acquire);
    const int64_t captureIntervalUs =
        g_audioCaptureIntervalUs.load(std::memory_order_acquire);
    const UINT32 queuedFrames =
        g_audioRingFrames.load(std::memory_order_acquire) +
        g_audioResamplerFrames.load(std::memory_order_acquire);
    const UINT32 queueTargetFrames =
        g_audioQueueTargetFrames.load(std::memory_order_acquire);
    const UINT32 observedMinimumFrames =
        g_audioMinimumPreRenderFrames.load(std::memory_order_acquire);
    const UINT32 minimumPreRenderFrames =
        observedMinimumFrames == UINT32_MAX
            ? queuedFrames : observedMinimumFrames;
    const UINT32 paddingFrames =
        g_audioWasapiPaddingFrames.load(std::memory_order_acquire);
    const wchar_t* presentationText =
        g_settings.presentationMode == PresentationMode::VSync
            ? L"VSync"
            : g_videoTearing.load(std::memory_order_acquire)
                  ? L"저지연" : L"Immediate";
    wchar_t latencyText[64]{};
    if (latencyUs >= 0) {
        swprintf_s(latencyText, L"%.2f ms", latencyUs / 1000.0);
    } else {
        wcscpy_s(latencyText, L"측정 대기 중");
    }

    const uint64_t underrunEvents =
        g_underruns.load(std::memory_order_relaxed);
    const uint64_t overrunEvents = g_ring.overruns();
    const uint64_t underrunFrames =
        g_audioUnderrunFrames.load(std::memory_order_acquire);
    const uint64_t overrunFrames =
        g_audioOverrunFrames.load(std::memory_order_acquire);
    const uint64_t monitorStartMs =
        g_audioMonitorStartMs.load(std::memory_order_acquire);
    const uint64_t nowMs = GetTickCount64();
    const uint64_t elapsedMs = monitorStartMs && nowMs >= monitorStartMs
        ? nowMs - monitorStartMs : 0;
    const uint64_t lastErrorMs = (std::max)(
        g_audioLastUnderrunMs.load(std::memory_order_acquire),
        g_audioLastOverrunMs.load(std::memory_order_acquire));
    const uint64_t lastErrorAgeMs = lastErrorMs && nowMs >= lastErrorMs
        ? nowMs - lastErrorMs : UINT64_MAX;
    const uint64_t totalErrorEvents = underrunEvents + overrunEvents;
    const uint64_t latePacketUnderruns = (std::min)(
        g_audioLatePacketUnderruns.load(std::memory_order_acquire),
        underrunEvents);
    const uint64_t resamplerUnderruns = (std::min)(
        g_audioResamplerUnderruns.load(std::memory_order_acquire),
        underrunEvents - latePacketUnderruns);
    const uint64_t queueDepletionUnderruns =
        underrunEvents - latePacketUnderruns - resamplerUnderruns;
    const double elapsedSeconds = elapsedMs / 1000.0;
    const double eventRatePerHour = elapsedSeconds > 0.0
        ? totalErrorEvents * 3600.0 / elapsedSeconds : 0.0;
    const double imbalancePpm = elapsedSeconds > 0.0
        ? (static_cast<double>(underrunFrames) -
           static_cast<double>(overrunFrames)) * 1'000'000.0 /
              (elapsedSeconds * kSampleRate)
        : 0.0;
    wchar_t lastErrorText[64]{};
    FormatAudioErrorAge(lastErrorMs, nowMs, lastErrorText,
                        ARRAYSIZE(lastErrorText));

    const int activeCorrectionPpm =
        g_audioResamplePpm.load(std::memory_order_acquire);
    const bool trackingActive = OsdTrackingActive();
    const wchar_t* clockDiagnosis = trackingActive
        ? L"측정 중" : L"워밍업 · 시작 2초 제외";
    if (monitorStartMs) {
        if (g_settings.driftCorrection ==
            DriftCorrectionMode::Resample) {
            if (resamplerUnderruns > 0 &&
                lastErrorAgeMs <= 10 * 60 * 1000) {
                clockDiagnosis = L"리샘플러 출력 부족 감지";
            } else if (std::abs(activeCorrectionPpm) >= 950) {
                clockDiagnosis = L"리샘플러 보정 한계 접근";
            } else if (totalErrorEvents == 0 ||
                       lastErrorAgeMs > 10 * 60 * 1000) {
                clockDiagnosis = L"리샘플러 정상 작동";
            } else {
                clockDiagnosis = L"보정 작동 · 오류 원인 아래 확인";
            }
        } else if (totalErrorEvents == 0) {
            clockDiagnosis = elapsedMs >= 2 * 60 * 1000
                ? L"안정 · 보정 불필요" : L"관찰 중";
        } else if (elapsedMs < 2 * 60 * 1000) {
            clockDiagnosis = L"초기 오류 · 더 관찰";
        } else if (lastErrorAgeMs > 10 * 60 * 1000) {
            clockDiagnosis = L"현재 안정 · 경과 관찰";
        } else if (underrunEvents > 0 && overrunEvents == 0 &&
                   latePacketUnderruns == underrunEvents) {
            clockDiagnosis = L"입력 지터 · 보정보다 대기량";
        } else if (std::abs(imbalancePpm) >= 50.0 ||
                   eventRatePerHour >= 12.0) {
            clockDiagnosis = L"반복 불균형 · 보정 권장";
        } else {
            clockDiagnosis = L"드문 오류 · 끔 유지 가능";
        }
    }

    const wchar_t* queueDiagnosis = trackingActive
        ? L"측정 중" : L"워밍업 · 시작 2초 제외";
    if (monitorStartMs) {
        if (underrunEvents == 0) {
            queueDiagnosis = g_settings.pcmQueueTargetMs ==
                                     kLowestPcmQueueMs
                ? L"최저 지연 · 오류 없음" : L"PCM 버퍼 여유 정상";
        } else if (lastErrorAgeMs > 10 * 60 * 1000) {
            queueDiagnosis = L"현재 안정 · 과거 오류 있음";
        } else if (queueDepletionUnderruns > 0) {
            queueDiagnosis = L"PCM 버퍼 부족 가능";
        } else if (resamplerUnderruns > 0) {
            queueDiagnosis = L"PCM 버퍼 있음 · 리샘플러 확인";
        } else {
            queueDiagnosis = L"캡처 패킷 지연 감지";
        }
    }

    const int logicalVolume =
        g_volumePercent.load(std::memory_order_acquire);
    const bool backgroundMuted =
        g_backgroundAudioMuted.load(std::memory_order_acquire);
    const wchar_t* volumeProcessing = backgroundMuted
        ? L"백그라운드 음소거 중"
        : logicalVolume == 100
              ? L"PCM 연산 우회"
              : logicalVolume == 0 ? L"음소거" : L"PCM 감쇠 적용";

    wchar_t text[2300]{};
    swprintf_s(
        text,
        L"캡처 실시간 정보                              [Tab 닫기]\n"
        L"경로          %s · DirectShow → D3D11\n"
        L"입력          %d x %d @ %d fps · %s 8-bit %s\n"
        L"영상 품질     BT.709 · Limited range · D3D11 Video Processor\n"
        L"표시          %d x %d · %s · Flip-discard · %s\n"
        L"실제 FPS      입력 %.1f · Present %.1f\n"
        L"앱 처리 지연  %s  (총 HDMI 지연 아님)\n"
        L"프레임        입력 %llu · 출력 %llu · 최신화 건너뜀 %llu\n"
        L"오디오 출력   WASAPI %s · %s · 버퍼 %.2f ms · 점유 %.2f ms\n"
        L"캡처 오디오   입력 패킷 %.2f ms · 콜백 주기 %.2f ms\n"
        L"클록 보정     %s · 적용 %+d ppm · %s\n"
        L"PCM 버퍼      현재 %.2f ms · 목표 %.2f ms(렌더 전) · 최저 %.2f ms\n"
        L"PCM 버퍼 진단 %s\n"
        L"음량          %d%% · %s\n"
        L"오디오 오류   underrun %llu회 / %.2f ms · overrun %llu회 / %.2f ms\n"
        L"오류 원인     입력 지연 %llu회 · PCM 버퍼 소진 %llu회 · 리샘플러 %llu회\n"
        L"오류 추세     %.1f회/h · PCM 불균형 %+.0f ppm(추정) · 최근 %s",
        captureName.c_str(), preset.width, preset.height, configuredFps,
        PixelFormatName(activeFormat), chromaText, outputWidth,
        outputHeight,
        g_settings.pixelPerfect && g_settings.relativeWindowSize
            ? L"Pixel-perfect 시작 · Monitor-relative 이동"
            : g_settings.pixelPerfect
                  ? L"Pixel-perfect (고정 크기)"
                  : g_settings.relativeWindowSize
                        ? L"Scaled · Monitor-relative" : L"Scaled (비율 고정)",
        presentationText,
        g_osdInputFps.load(std::memory_order_acquire),
        g_osdPresentFps.load(std::memory_order_acquire), latencyText,
        static_cast<unsigned long long>(
            g_videoCapturedFrames.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_videoPresentedFrames.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_videoReplacedFrames.load(std::memory_order_relaxed)),
        g_settings.audioMode == AudioMode::WasapiExclusive
            ? L"Exclusive" : L"Shared",
        outputName.c_str(),
        1000.0 * audioFrames / kSampleRate,
        1000.0 * paddingFrames / kSampleRate,
        1000.0 * capturePacketFrames / kSampleRate,
        captureIntervalUs / 1000.0,
        g_settings.driftCorrection == DriftCorrectionMode::Resample
            ? L"자동 리샘플링" : L"끔 (원본 PCM)",
        activeCorrectionPpm, clockDiagnosis,
        1000.0 * queuedFrames / kSampleRate,
        1000.0 * queueTargetFrames / kSampleRate,
        1000.0 * minimumPreRenderFrames / kSampleRate,
        queueDiagnosis,
        logicalVolume, volumeProcessing,
        static_cast<unsigned long long>(underrunEvents),
        1000.0 * underrunFrames / kSampleRate,
        static_cast<unsigned long long>(overrunEvents),
        1000.0 * overrunFrames / kSampleRate,
        static_cast<unsigned long long>(latePacketUnderruns),
        static_cast<unsigned long long>(queueDepletionUnderruns),
        static_cast<unsigned long long>(resamplerUnderruns),
        eventRatePerHour, imbalancePpm, lastErrorText);
    return text;
}

static void UpdateOsdRates() {
    static uint64_t previousCaptured = 0;
    static uint64_t previousPresented = 0;
    static auto previousTime = std::chrono::steady_clock::now();
    static bool trackingStarted = false;
    const auto now = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(now - previousTime).count();
    if (seconds <= 0.0) return;
    const uint64_t captured =
        g_videoCapturedFrames.load(std::memory_order_relaxed);
    const uint64_t presented =
        g_videoPresentedFrames.load(std::memory_order_relaxed);
    if (!OsdTrackingActive()) {
        trackingStarted = false;
        previousCaptured = captured;
        previousPresented = presented;
        previousTime = now;
        g_osdInputFps.store(0.0, std::memory_order_release);
        g_osdPresentFps.store(0.0, std::memory_order_release);
        return;
    }
    if (!trackingStarted) {
        trackingStarted = true;
        previousCaptured = captured;
        previousPresented = presented;
        previousTime = now;
        return;
    }
    g_osdInputFps.store((captured - previousCaptured) / seconds,
                        std::memory_order_release);
    g_osdPresentFps.store((presented - previousPresented) / seconds,
                          std::memory_order_release);
    previousCaptured = captured;
    previousPresented = presented;
    previousTime = now;
}

static void ToggleRuntimeOsd() {
    const bool visible = !g_osdVisible.load(std::memory_order_acquire);
    g_osdVisible.store(visible, std::memory_order_release);
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
}

static bool AdjustVolumeFromWheel(HWND root, WPARAM wParam, LPARAM lParam) {
    const POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    HWND hovered = WindowFromPoint(screenPoint);
    if (!hovered || GetAncestor(hovered, GA_ROOT) != root) return false;

    static int wheelRemainder = 0;
    wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
    const int steps = wheelRemainder / WHEEL_DELTA;
    wheelRemainder %= WHEEL_DELTA;
    if (steps == 0) return true;

    const int current = g_volumePercent.load(std::memory_order_acquire);
    const int adjusted = std::clamp(current + steps * 5, 0, 100);
    g_volumePercent.store(adjusted, std::memory_order_release);
    g_settings.volumePercent = adjusted;
    g_volumeHudUntilMs.store(GetTickCount64() + 1200,
                             std::memory_order_release);
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
    return true;
}

static void UpdateBackgroundAudioMute(bool appActive) {
    const bool mute = g_settings.muteWhenBackground && !appActive;
    const bool previous = g_backgroundAudioMuted.exchange(
        mute, std::memory_order_acq_rel);
    if (previous == mute) return;
    fwprintf(stderr, L"[audio] background auto-mute: %s\n",
             mute ? L"on" : L"off");
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_videoHost = CreateWindowExW(
            0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_BLACKRECT,
            0, 0, 100, 100,
            hwnd, nullptr,
            reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance,
            nullptr);
        if (g_videoHost) {
            SetWindowSubclass(g_videoHost, VideoHostSubclassProc, 1, 0);
        }
        SetTimer(hwnd, 1, 500, nullptr);
        return 0;

    case WM_SIZE:
        if (g_videoHost) {
            MoveWindow(g_videoHost, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;

    case WM_SIZING:
        if (lParam && !g_fullscreen && !g_settings.pixelPerfect) {
            g_manualResizeInProgress = true;
            ConstrainWindowRectToVideoAspect(
                hwnd, *reinterpret_cast<RECT*>(lParam),
                static_cast<UINT>(wParam));
            return TRUE;
        }
        break;

    case WM_TIMER:
        if (wParam == 1) {
            UpdateOsdRates();
            g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        break;

    case WM_ACTIVATEAPP:
        UpdateBackgroundAudioMute(wParam != FALSE);
        return 0;

    case WM_GETDPISCALEDSIZE:
        if ((g_settings.pixelPerfect || g_settings.relativeWindowSize) &&
            !g_fullscreen && lParam) {
            const UINT pendingDpi = static_cast<UINT>(wParam);
            POINT cursor{};
            GetCursorPos(&cursor);
            const HMONITOR targetMonitor =
                MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            const SIZE desired = DesiredWindowOuterSize(
                hwnd, targetMonitor, pendingDpi);
            *reinterpret_cast<SIZE*>(lParam) = desired;
            fwprintf(stderr,
                     L"[video] DPI preflight: %u dpi -> outer %ld x %ld\n",
                     pendingDpi, desired.cx, desired.cy);
            return TRUE;
        }
        break;

    case WM_DPICHANGED:
        if ((g_settings.pixelPerfect || g_settings.relativeWindowSize) &&
            !g_fullscreen) {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            fwprintf(stderr,
                     L"[video] DPI changed: %u dpi, suggested outer %ld x %ld\n",
                     LOWORD(wParam), suggested->right - suggested->left,
                     suggested->bottom - suggested->top);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        break;

    case WM_ENTERSIZEMOVE:
        ResetWindowSnapState();
        g_manualResizeInProgress = false;
        g_relativeMoveMonitor =
            MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        return 0;

    case WM_MOVING:
        if (lParam) {
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                const HMONITOR monitor = MonitorFromPoint(
                    cursor, MONITOR_DEFAULTTONEAREST);
                ApplyRelativeSizeForMonitor(
                    hwnd, monitor, cursor,
                    *reinterpret_cast<RECT*>(lParam));
            }
            ApplyWindowEdgeSnap(hwnd, *reinterpret_cast<RECT*>(lParam));
            return TRUE;
        }
        break;

    case WM_EXITSIZEMOVE:
        ResetWindowSnapState();
        g_relativeMoveMonitor = nullptr;
        if (g_manualResizeInProgress) {
            RememberRelativeScaleFromWindow(hwnd);
        }
        g_manualResizeInProgress = false;
        NormalizeWindowSize(hwnd, true);
        return 0;

    case WM_NCHITTEST:
        if (g_settings.borderlessWindow && !g_fullscreen) {
            return BorderlessHitTest(hwnd, lParam);
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_TAB) {
            ToggleRuntimeOsd();
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (g_fullscreen) ToggleFullscreen(hwnd);
            else PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;

    case WM_MOUSEWHEEL:
        if (AdjustVolumeFromWheel(hwnd, wParam, lParam)) return 0;
        break;

    case WM_TOGGLE_RUNTIME_OSD:
        ToggleRuntimeOsd();
        return 0;

    case WM_CLOSE:
        PersistWindowPosition(hwnd);
        g_windowPositionPersisted = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (!g_windowPositionPersisted) PersistWindowPosition(hwnd);
        g_settings.volumePercent =
            g_volumePercent.load(std::memory_order_acquire);
        SaveSettings();
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR commandLine, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX commonControls{
        sizeof(commonControls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&commonControls);

    LoadSettings();
    const bool smokeTest = commandLine &&
        wcsstr(commandLine, L"--smoke-test") != nullptr;
    const bool longSmokeTest = commandLine &&
        wcsstr(commandLine, L"--smoke-test-60") != nullptr;
    const bool overlaySmokeTest = commandLine &&
        wcsstr(commandLine, L"--smoke-test-overlay") != nullptr;
    if (overlaySmokeTest) {
        g_osdVisible.store(true, std::memory_order_release);
        g_volumeHudUntilMs.store(GetTickCount64() + 20'000,
                                 std::memory_order_release);
        g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
    }
    if (!smokeTest && !ShowSettingsDialog(hInst)) return 0;

    // Allocate a console for prototype diagnostics.
    AllocConsole();
    FILE* f = nullptr;
    if (smokeTest) {
        const std::wstring logPath = AppDirectory() + L"\\smoke-test.log";
        _wfreopen_s(&f, L"CONOUT$", L"w", stdout);
        _wfreopen_s(&f, logPath.c_str(), L"w", stderr);
    } else {
        _wfreopen_s(&f, L"CONOUT$", L"w", stdout);
        _wfreopen_s(&f, L"CONOUT$", L"w", stderr);
    }
    if (!smokeTest) OpenSavedLog();
    SetActiveAudioOutputName(ConfiguredAudioEndpointName(
        g_settings.audioOutputDeviceId));

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"LowLatencyCaptureViewerClass";

    if (!RegisterClassW(&wc)) {
        CloseSavedLog();
        return 1;
    }

    const auto& video = CurrentVideoPreset();
    const wchar_t* audioLabel =
        g_settings.audioMode == AudioMode::WasapiExclusive
            ? L"WASAPI Exclusive" : L"WASAPI Shared";
    wchar_t title[256]{};
    const wchar_t* videoLabel =
        g_settings.presentationMode == PresentationMode::VSync
            ? L"Single Graph / Direct D3D11 / VSync"
            : L"Single Graph / Direct D3D11 / Tearing";
    swprintf_s(title,
               L"Low Latency Capture Viewer - %dx%d @ %dfps - %s - %s",
               video.width, video.height, RequestedVideoFrameRate(), audioLabel,
               videoLabel);

    const DWORD fixedWindowStyle =
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    const DWORD windowStyle = g_settings.borderlessWindow
                                  ? (WS_POPUP | WS_VISIBLE)
                                  : g_settings.pixelPerfect
                                        ? fixedWindowStyle
                                        : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    constexpr DWORD windowExStyle = 0;
    const POINT initialMonitorPoint = g_settings.hasWindowPosition
        ? POINT{g_settings.windowX, g_settings.windowY}
        : POINT{0, 0};
    const HMONITOR initialMonitor = MonitorFromPoint(
        initialMonitorPoint, MONITOR_DEFAULTTOPRIMARY);
    const SIZE initialClient = InitialClientPixelsForMonitor(initialMonitor);
    const UINT initialDpi = EffectiveMonitorDpi(initialMonitor, nullptr);
    const SIZE outerSize = OuterSizeForClientPixels(
        initialClient.cx, initialClient.cy, windowStyle, windowExStyle,
        initialDpi);
    if (g_settings.relativeWindowSize) {
        fwprintf(stderr,
                 L"[video] monitor-relative scale: %.2f%%, initial client %ld x %ld\n",
                 100.0 * g_settings.relativeWindowScalePpm /
                     kRelativeScaleUnit,
                 initialClient.cx, initialClient.cy);
    }
    POINT restoredOrigin{};
    const bool restoreOrigin =
        RestoredWindowOrigin(outerSize, restoredOrigin);

    HWND hwnd = CreateWindowExW(
        windowExStyle, wc.lpszClassName, title,
        windowStyle,
        restoreOrigin ? restoredOrigin.x : CW_USEDEFAULT,
        restoreOrigin ? restoredOrigin.y : CW_USEDEFAULT,
        outerSize.cx, outerSize.cy,
        nullptr, nullptr, hInst, nullptr);

    if (!hwnd) {
        CloseSavedLog();
        return 1;
    }
    NormalizeWindowSize(hwnd, true);
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    fwprintf(stderr, L"[video] window client area: %ld x %ld%s\n",
             clientRect.right - clientRect.left,
             clientRect.bottom - clientRect.top,
             g_settings.pixelPerfect
                 ? L" (pixel-perfect)"
                 : g_settings.relativeWindowSize
                       ? L" (monitor-relative)" : L"");
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    DWORD foregroundProcessId = 0;
    const HWND foregroundWindow = GetForegroundWindow();
    if (foregroundWindow) {
        GetWindowThreadProcessId(foregroundWindow, &foregroundProcessId);
    }
    UpdateBackgroundAudioMute(
        foregroundProcessId == GetCurrentProcessId());

    if (g_settings.pixelPerfect && SelectedResolutionMatchesMonitor(hwnd)) {
        fwprintf(stderr,
                 L"[video] selected resolution matches monitor; entering borderless fullscreen.\n");
        ToggleFullscreen(hwnd);
    }

    g_osdTrackingStartMs.store(GetTickCount64() + kOsdTrackingWarmupMs,
                               std::memory_order_release);
    fwprintf(stderr,
             L"[osd] statistics warmup: first %llu ms excluded.\n",
             static_cast<unsigned long long>(kOsdTrackingWarmupMs));

    // One DirectShow graph owns one selected capture-filter instance and both
    // its video and audio branches. WASAPI remains an independent consumer.
    std::thread renderThread(AudioRenderThread);
    std::thread unifiedCaptureThread([hwnd, smokeTest]() {
        if (!UnifiedCaptureRenderLoop(g_videoHost) && g_running.load()) {
            fwprintf(stderr, L"[capture] single capture graph stopped.\n");
            if (!smokeTest) {
                const HRESULT failure =
                    g_captureFailureHr.load(std::memory_order_acquire);
                const std::wstring failureText = HrText(failure);
                wchar_t message[768]{};
                swprintf_s(
                    message,
                    L"캡처 영상 초기화에 실패했습니다.\n\n"
                    L"오류: 0x%08X  %s\n\n"
                    L"선택한 장치가 지정한 해상도/FPS/픽셀 포맷 또는 "
                    L"48 kHz 스테레오 PCM 오디오 핀을 제공하지 않을 수 "
                    L"있습니다.\n"
                    L"자동 픽셀 포맷이나 다른 해상도로 다시 시도하고, "
                    L"로그 저장을 켠 경우 logs 폴더를 확인해 주세요.",
                    static_cast<unsigned int>(failure),
                    failureText.c_str());
                MessageBoxW(hwnd, message, L"Low Latency Capture Viewer",
                            MB_OK | MB_ICONERROR);
            }
            g_running.store(false);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
    });
    std::thread smokeTestStopper;
    if (smokeTest) {
        smokeTestStopper = std::thread([hwnd, longSmokeTest]() {
            const int tenths = longSmokeTest ? 600 : 100;
            for (int i = 0; i < tenths && g_running.load(); ++i) {
                Sleep(100);
            }
            if (g_running.load()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        });
    }

    MSG m{};
    while (g_running.load() && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_MOUSEWHEEL &&
            AdjustVolumeFromWheel(hwnd, m.wParam, m.lParam)) {
            continue;
        }
        // Keyboard focus can belong to the video-host child. Key messages do
        // not bubble to its parent, so intercept Tab at the thread message-loop
        // level and route one toggle command to the main window.
        if (m.message == WM_KEYDOWN && m.wParam == VK_TAB &&
            GetKeyState(VK_CONTROL) >= 0) {
            SendMessageW(hwnd, WM_TOGGLE_RUNTIME_OSD, 0, 0);
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g_running.store(false);

    if (renderThread.joinable()) renderThread.join();
    if (unifiedCaptureThread.joinable()) unifiedCaptureThread.join();
    if (smokeTestStopper.joinable()) smokeTestStopper.join();
    if (smokeTest) {
        fwprintf(stderr,
                 L"[smoke] captured=%llu presented=%llu replaced=%llu "
                 L"latest-app-latency=%.3f ms\n",
                 static_cast<unsigned long long>(
                     g_videoCapturedFrames.load(std::memory_order_acquire)),
                 static_cast<unsigned long long>(
                     g_videoPresentedFrames.load(std::memory_order_acquire)),
                 static_cast<unsigned long long>(
                     g_videoReplacedFrames.load(std::memory_order_acquire)),
                 g_videoAppLatencyUs.load(std::memory_order_acquire) / 1000.0);
        fwprintf(
            stderr,
            L"[smoke-audio] packet=%u frames/%.2f ms interval=%.2f ms "
            L"average-packet=%.2f ms average-interval=%.2f ms "
            L"ring=%.2f ms padding=%.2f ms correction=%+d ppm "
            L"underruns=%llu/%llu frames overruns=%llu/%llu frames\n",
            g_audioCapturePacketFrames.load(std::memory_order_acquire),
            1000.0 * g_audioCapturePacketFrames.load(
                         std::memory_order_relaxed) /
                kSampleRate,
            g_audioCaptureIntervalUs.load(std::memory_order_acquire) / 1000.0,
            g_audioCaptureCallbacks.load(std::memory_order_acquire) > 0
                ? 1000.0 * g_audioCaptureFrames.load(
                               std::memory_order_acquire) /
                      g_audioCaptureCallbacks.load(
                          std::memory_order_relaxed) /
                      kSampleRate
                : 0.0,
            g_audioCaptureCallbacks.load(std::memory_order_acquire) > 1
                ? g_audioCaptureIntervalTotalUs.load(
                      std::memory_order_acquire) /
                      1000.0 /
                      (g_audioCaptureCallbacks.load(
                           std::memory_order_relaxed) - 1)
                : 0.0,
            1000.0 *
                (g_audioRingFrames.load(std::memory_order_acquire) +
                 g_audioResamplerFrames.load(std::memory_order_acquire)) /
                kSampleRate,
            1000.0 * g_audioWasapiPaddingFrames.load(
                         std::memory_order_acquire) /
                kSampleRate,
            g_audioResamplePpm.load(std::memory_order_acquire),
            static_cast<unsigned long long>(
                g_underruns.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(
                g_audioUnderrunFrames.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(
                g_ring.overruns()),
            static_cast<unsigned long long>(
                g_audioOverrunFrames.load(std::memory_order_acquire)));
        fwprintf(stderr, L"[smoke-overlay] rendered=%llu same-swapchain=%s\n",
                 static_cast<unsigned long long>(
                     g_overlayRenderedFrames.load(
                         std::memory_order_acquire)),
                 g_overlayRenderedFrames.load(std::memory_order_relaxed) > 0
                     ? L"yes" : L"hidden");
        fwprintf(stderr, L"[smoke-volume] final=%d%%\n",
                 g_volumePercent.load(std::memory_order_acquire));
    }
    CloseSavedLog();
    return 0;
}
