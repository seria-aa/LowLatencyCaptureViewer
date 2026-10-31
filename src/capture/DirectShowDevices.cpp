#include "capture/DirectShowDevices.h"

#include <ks.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <vector>

namespace llcv::capture {
namespace {

template<class T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

void FreeMediaType(AM_MEDIA_TYPE& mediaType) {
    if (mediaType.cbFormat != 0) {
        CoTaskMemFree(mediaType.pbFormat);
        mediaType.cbFormat = 0;
        mediaType.pbFormat = nullptr;
    }
    if (mediaType.pUnk) {
        mediaType.pUnk->Release();
        mediaType.pUnk = nullptr;
    }
}

void DeleteMediaType(AM_MEDIA_TYPE* mediaType) {
    if (!mediaType) return;
    FreeMediaType(*mediaType);
    CoTaskMemFree(mediaType);
}

std::wstring MonikerDisplayName(IMoniker* moniker) {
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

std::wstring MonikerProperty(IMoniker* moniker, const wchar_t* property) {
    if (!moniker) return {};
    IPropertyBag* bag = nullptr;
    VARIANT value;
    VariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(moniker->BindToStorage(
            nullptr, nullptr, IID_PPV_ARGS(&bag))) &&
        bag && SUCCEEDED(bag->Read(property, &value, nullptr)) &&
        value.vt == VT_BSTR && value.bstrVal) {
        result = value.bstrVal;
    }
    VariantClear(&value);
    SafeRelease(bag);
    return result;
}

std::wstring MonikerFriendlyName(IMoniker* moniker) {
    return MonikerProperty(moniker, L"FriendlyName");
}

std::vector<DeviceInfo> EnumerateInputDevices(const CLSID& category) {
    std::vector<DeviceInfo> devices;
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initHr);
    if (initHr == RPC_E_CHANGED_MODE) initHr = S_OK;
    if (FAILED(initHr)) return devices;

    ICreateDevEnum* deviceEnumerator = nullptr;
    IEnumMoniker* monikers = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&deviceEnumerator));
    if (SUCCEEDED(hr)) {
        hr = deviceEnumerator->CreateClassEnumerator(category, &monikers, 0);
    }
    IMoniker* moniker = nullptr;
    while (monikers && monikers->Next(1, &moniker, nullptr) == S_OK) {
        DeviceInfo info{};
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

std::wstring NormalizedDeviceName(const std::wstring& value) {
    std::wstring normalized;
    bool previousWasSpace = true;
    for (const wchar_t character : value) {
        if (std::iswalnum(character)) {
            normalized.push_back(
                static_cast<wchar_t>(std::towlower(character)));
            previousWasSpace = false;
        } else if (!previousWasSpace) {
            normalized.push_back(L' ');
            previousWasSpace = true;
        }
    }
    while (!normalized.empty() && normalized.back() == L' ') {
        normalized.pop_back();
    }
    return normalized;
}

void Log(LogCallback callback, const wchar_t* message) {
    if (callback && message) callback(message);
}

}  // namespace

std::vector<DeviceInfo> EnumerateVideoInputDevices() {
    return EnumerateInputDevices(CLSID_VideoInputDeviceCategory);
}

std::vector<DeviceInfo> EnumerateAudioInputDevices() {
    return EnumerateInputDevices(CLSID_AudioInputDeviceCategory);
}

int RelatedCaptureAudioScore(
    const std::wstring& videoName, const std::wstring& audioName) {
    const std::wstring video = NormalizedDeviceName(videoName);
    const std::wstring audio = NormalizedDeviceName(audioName);
    if (video.empty() || audio.empty()) return 0;
    if (video == audio) return 1000;
    if (video.find(audio) != std::wstring::npos ||
        audio.find(video) != std::wstring::npos) {
        return 800;
    }

    int score = 0;
    size_t start = 0;
    while (start < video.size()) {
        const size_t end = video.find(L' ', start);
        const std::wstring token = video.substr(
            start,
            end == std::wstring::npos ? std::wstring::npos : end - start);
        if (token.size() >= 3 && token != L"avermedia" &&
            token != L"elgato" && token != L"capture" &&
            token != L"video" && token != L"audio" &&
            audio.find(token) != std::wstring::npos) {
            score += 100;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return score;
}

HRESULT FindVideoCaptureFilter(
    const std::wstring& selectedId, const wchar_t* preferredDeviceName,
    IBaseFilter** output, std::wstring* selectedName,
    LogCallback logCallback, std::wstring* selectedDevicePath) {
    if (selectedDevicePath) selectedDevicePath->clear();
    if (!output) return E_POINTER;
    *output = nullptr;

    ICreateDevEnum* deviceEnumerator = nullptr;
    IEnumMoniker* monikers = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&deviceEnumerator));
    if (FAILED(hr)) return hr;
    hr = deviceEnumerator->CreateClassEnumerator(
        CLSID_VideoInputDeviceCategory, &monikers, 0);
    if (hr != S_OK) {
        SafeRelease(deviceEnumerator);
        return E_FAIL;
    }

    IMoniker* moniker = nullptr;
    IMoniker* selectedMoniker = nullptr;
    std::wstring chosenName;
    int chosenRank = 0;
    while (monikers->Next(1, &moniker, nullptr) == S_OK) {
        const std::wstring name = MonikerFriendlyName(moniker);
        const std::wstring id = MonikerDisplayName(moniker);
        if (!name.empty() && logCallback) {
            wchar_t message[512]{};
            swprintf_s(message, L"[capture] video device: %s\n", name.c_str());
            Log(logCallback, message);
        }
        int rank = 0;
        if (!selectedId.empty()) {
            if (id == selectedId) rank = 100;
        } else {
            std::wstring lowerName(name);
            std::transform(
                lowerName.begin(), lowerName.end(), lowerName.begin(),
                [](wchar_t value) {
                    return static_cast<wchar_t>(std::towlower(value));
                });
            if (preferredDeviceName &&
                _wcsicmp(name.c_str(), preferredDeviceName) == 0) {
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
            selectedMoniker = moniker;
            selectedMoniker->AddRef();
            chosenName = name;
            chosenRank = rank;
        }
        SafeRelease(moniker);
    }

    if (selectedMoniker) {
        hr = selectedMoniker->BindToObject(
            nullptr, nullptr, IID_PPV_ARGS(output));
        if (SUCCEEDED(hr)) {
            if (logCallback) {
                wchar_t message[512]{};
                swprintf_s(
                    message, L"[capture] selected device: %s%s\n",
                    chosenName.c_str(), selectedId.empty() ? L" (auto)" : L"");
                Log(logCallback, message);
            }
            if (selectedName) *selectedName = chosenName;
            if (selectedDevicePath) *selectedDevicePath = MonikerProperty(selectedMoniker, L"DevicePath");
        }
    } else {
        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    SafeRelease(selectedMoniker);
    SafeRelease(monikers);
    SafeRelease(deviceEnumerator);
    return hr;
}

HRESULT FindCaptureAudioFilter(
    const std::wstring& selectedId, const std::wstring& videoName,
    IBaseFilter** output, std::wstring* selectedName,
    LogCallback logCallback) {
    if (!output) return E_POINTER;
    *output = nullptr;

    ICreateDevEnum* deviceEnumerator = nullptr;
    IEnumMoniker* monikers = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&deviceEnumerator));
    if (FAILED(hr)) return hr;
    hr = deviceEnumerator->CreateClassEnumerator(
        CLSID_AudioInputDeviceCategory, &monikers, 0);
    if (hr != S_OK) {
        SafeRelease(deviceEnumerator);
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    IMoniker* moniker = nullptr;
    IMoniker* chosen = nullptr;
    std::wstring chosenName;
    int chosenScore = 0;
    while (monikers->Next(1, &moniker, nullptr) == S_OK) {
        const std::wstring name = MonikerFriendlyName(moniker);
        const std::wstring id = MonikerDisplayName(moniker);
        if (!name.empty() && logCallback) {
            wchar_t message[512]{};
            swprintf_s(message, L"[capture] audio input: %s\n", name.c_str());
            Log(logCallback, message);
        }
        const int score = !selectedId.empty()
            ? (id == selectedId ? 10000 : 0)
            : RelatedCaptureAudioScore(videoName, name);
        if (score > chosenScore) {
            SafeRelease(chosen);
            chosen = moniker;
            chosen->AddRef();
            chosenName = name;
            chosenScore = score;
        }
        SafeRelease(moniker);
    }

    if (chosen) {
        hr = chosen->BindToObject(nullptr, nullptr, IID_PPV_ARGS(output));
        if (SUCCEEDED(hr)) {
            if (logCallback) {
                wchar_t message[512]{};
                swprintf_s(
                    message, L"[capture] selected audio input: %s%s\n",
                    chosenName.c_str(),
                    selectedId.empty() ? L" (auto match)" : L"");
                Log(logCallback, message);
            }
            if (selectedName) *selectedName = chosenName;
        }
    } else {
        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    SafeRelease(chosen);
    SafeRelease(monikers);
    SafeRelease(deviceEnumerator);
    return hr;
}

HRESULT FindOutputPinByName(
    IBaseFilter* filter, const wchar_t* name, IPin** output) {
    if (!filter || !output) return E_POINTER;
    *output = nullptr;
    IEnumPins* pins = nullptr;
    HRESULT hr = filter->EnumPins(&pins);
    if (FAILED(hr)) return hr;

    IPin* pin = nullptr;
    while (pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION direction{};
        PIN_INFO info{};
        if (SUCCEEDED(pin->QueryDirection(&direction)) &&
            direction == PINDIR_OUTPUT &&
            SUCCEEDED(pin->QueryPinInfo(&info))) {
            if (info.pFilter) info.pFilter->Release();
            if (_wcsicmp(info.achName, name) == 0) {
                *output = pin;
                pins->Release();
                return S_OK;
            }
        }
        pin->Release();
    }
    pins->Release();
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT GetFirstPin(
    IBaseFilter* filter, PIN_DIRECTION wantedDirection, IPin** output) {
    if (!filter || !output) return E_POINTER;
    *output = nullptr;
    IEnumPins* pins = nullptr;
    HRESULT hr = filter->EnumPins(&pins);
    if (FAILED(hr)) return hr;

    IPin* pin = nullptr;
    while (pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION direction{};
        if (SUCCEEDED(pin->QueryDirection(&direction)) &&
            direction == wantedDirection) {
            *output = pin;
            pins->Release();
            return S_OK;
        }
        pin->Release();
    }
    pins->Release();
    return E_FAIL;
}

HRESULT FindOutputPinByMajorType(
    IBaseFilter* filter, const GUID& majorType, IPin** output) {
    if (!filter || !output) return E_POINTER;
    *output = nullptr;
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
                        *output = pin;
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

void LogFilterPins(
    IBaseFilter* filter, const wchar_t* label, LogCallback logCallback) {
    if (!filter || !label || !logCallback) return;
    IEnumPins* pins = nullptr;
    if (FAILED(filter->EnumPins(&pins))) return;
    wchar_t message[512]{};
    swprintf_s(message, L"[capture] %s pin diagnostics:\n", label);
    Log(logCallback, message);

    IPin* pin = nullptr;
    while (pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_INFO info{};
        PIN_DIRECTION direction{};
        pin->QueryDirection(&direction);
        const bool hasInfo = SUCCEEDED(pin->QueryPinInfo(&info));
        swprintf_s(
            message, L"[capture]   %s: %s\n",
            hasInfo ? info.achName : L"(unnamed pin)",
            direction == PINDIR_OUTPUT ? L"output" : L"input");
        Log(logCallback, message);
        if (hasInfo && info.pFilter) info.pFilter->Release();

        IEnumMediaTypes* types = nullptr;
        if (SUCCEEDED(pin->EnumMediaTypes(&types))) {
            int index = 0;
            AM_MEDIA_TYPE* type = nullptr;
            while (index < 12 && types->Next(1, &type, nullptr) == S_OK) {
                wchar_t major[48]{};
                wchar_t subtype[48]{};
                StringFromGUID2(type->majortype, major, ARRAYSIZE(major));
                StringFromGUID2(type->subtype, subtype, ARRAYSIZE(subtype));
                swprintf_s(
                    message, L"[capture]     type %d: %s / %s\n",
                    index + 1, major, subtype);
                Log(logCallback, message);
                DeleteMediaType(type);
                ++index;
            }
            SafeRelease(types);
        }
        SafeRelease(pin);
    }
    SafeRelease(pins);
}

const wchar_t* PhysicalConnectorName(long type) {
    switch (type) {
    case PhysConn_Video_Tuner: return L"video tuner";
    case PhysConn_Video_Composite: return L"composite";
    case PhysConn_Video_SVideo: return L"S-Video";
    case PhysConn_Video_RGB: return L"RGB";
    case PhysConn_Video_YRYBY: return L"component";
    case PhysConn_Video_SerialDigital: return L"serial digital";
    case PhysConn_Video_ParallelDigital: return L"parallel digital";
    case PhysConn_Video_SCSI: return L"SCSI";
    case PhysConn_Video_AUX: return L"video AUX";
    case PhysConn_Video_1394: return L"1394";
    case PhysConn_Video_USB: return L"USB";
    case PhysConn_Video_VideoDecoder: return L"video decoder";
    case PhysConn_Video_VideoEncoder: return L"video encoder";
    case PhysConn_Video_SCART: return L"SCART";
    case PhysConn_Video_Black: return L"black";
    case PhysConn_Audio_Tuner: return L"audio tuner";
    case PhysConn_Audio_Line: return L"audio line";
    case PhysConn_Audio_Mic: return L"microphone";
    case PhysConn_Audio_AESDigital: return L"AES digital";
    case PhysConn_Audio_SPDIFDigital: return L"S/PDIF";
    case PhysConn_Audio_SCSI: return L"audio SCSI";
    case PhysConn_Audio_AUX: return L"audio AUX";
    case PhysConn_Audio_1394: return L"audio 1394";
    case PhysConn_Audio_USB: return L"audio USB";
    case PhysConn_Audio_AudioDecoder: return L"audio decoder";
    default: return L"vendor-defined";
    }
}

namespace {

// Spelled out so the KS helpers need no additional import library.
constexpr GUID kKsMediumSetStandard = {
    0x4747B320, 0x62CE, 0x11CF, {0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00}};
constexpr GUID kIidKsPin = {
    0xB61178D1, 0xA2D9, 0x11CF, {0x9E, 0x53, 0x00, 0xAA, 0x00, 0xA2, 0x16, 0xA1}};

// Leading method of the IKsPin vtable. ksproxy.h declares IKsPin only for
// builds using the DirectShow base classes; only KsQueryMediums is called.
struct IKsPin : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE KsQueryMediums(KSMULTIPLE_ITEM** mediumList) = 0;
};

bool IsPinConnected(IPin* pin) {
    IPin* peer = nullptr;
    const bool connected = pin && SUCCEEDED(pin->ConnectedTo(&peer));
    SafeRelease(peer);
    return connected;
}

bool PinOffersMajorType(IPin* pin, const GUID& first, const GUID& second) {
    IEnumMediaTypes* types = nullptr;
    if (FAILED(pin->EnumMediaTypes(&types))) return false;
    bool found = false;
    AM_MEDIA_TYPE* type = nullptr;
    while (!found && types->Next(1, &type, nullptr) == S_OK) {
        found = type && (type->majortype == first || type->majortype == second);
        DeleteMediaType(type);
    }
    SafeRelease(types);
    return found;
}

// The input pin a crossbar video decoder output feeds. A filter with separate
// video and audio inputs must not count an audio-only connection as success.
// An unconnected pin may legitimately fail to enumerate types, so a pin with no
// advertised type is used only when it is the single such input; pins that
// advertise audio are never candidates. Otherwise the input is ambiguous and
// nullptr is returned with *ambiguous set.
IPin* FindVideoInputPin(IBaseFilter* filter, bool* ambiguous) {
    *ambiguous = false;
    IEnumPins* pins = nullptr;
    if (FAILED(filter->EnumPins(&pins))) return nullptr;
    IPin* video = nullptr;
    IPin* untyped = nullptr;
    unsigned untypedCount = 0;
    unsigned inputCount = 0;
    IPin* pin = nullptr;
    while (!video && pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION direction{};
        if (SUCCEEDED(pin->QueryDirection(&direction)) && direction == PINDIR_INPUT) {
            ++inputCount;
            if (PinOffersMajorType(pin, MEDIATYPE_AnalogVideo, MEDIATYPE_Video)) {
                video = pin;
                pin = nullptr;
            } else if (!PinOffersMajorType(pin, MEDIATYPE_AnalogAudio, MEDIATYPE_Audio) &&
                       ++untypedCount == 1) {
                untyped = pin;
                pin = nullptr;
            }
        }
        SafeRelease(pin);
    }
    SafeRelease(pins);
    if (video) {
        SafeRelease(untyped);
        return video;
    }
    if (untypedCount == 1) return untyped;
    SafeRelease(untyped);
    *ambiguous = inputCount > 0;
    return nullptr;
}

bool GetPinMedium(IPin* pin, REGPINMEDIUM& medium) {
    IKsPin* ksPin = nullptr;
    if (FAILED(pin->QueryInterface(kIidKsPin, reinterpret_cast<void**>(&ksPin))) || !ksPin)
        return false;
    KSMULTIPLE_ITEM* items = nullptr;
    const HRESULT hr = ksPin->KsQueryMediums(&items);
    SafeRelease(ksPin);
    if (FAILED(hr) || !items) return false;
    const bool found = SelectPinMedium(items, medium);
    CoTaskMemFree(items);
    return found;
}

bool HasPinWithMedium(IBaseFilter* filter, const REGPINMEDIUM& medium) {
    IEnumPins* pins = nullptr;
    if (FAILED(filter->EnumPins(&pins))) return false;
    bool found = false;
    IPin* pin = nullptr;
    while (!found && pins->Next(1, &pin, nullptr) == S_OK) {
        REGPINMEDIUM current{};
        found = GetPinMedium(pin, current) && std::memcmp(&current, &medium, sizeof(medium)) == 0;
        SafeRelease(pin);
    }
    SafeRelease(pins);
    return found;
}

HRESULT FindCrossbarByMedium(const REGPINMEDIUM& medium, IBaseFilter** output) {
    *output = nullptr;
    ICreateDevEnum* devices = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&devices));
    IEnumMoniker* monikers = nullptr;
    if (SUCCEEDED(hr)) hr = devices->CreateClassEnumerator(AM_KSCATEGORY_CROSSBAR, &monikers, 0);
    SafeRelease(devices);
    if (hr != S_OK || !monikers) {
        SafeRelease(monikers);
        return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    IMoniker* moniker = nullptr;
    while (!*output && monikers->Next(1, &moniker, nullptr) == S_OK) {
        IBaseFilter* candidate = nullptr;
        if (SUCCEEDED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&candidate))) &&
            candidate && HasPinWithMedium(candidate, medium)) {
            *output = candidate;
            candidate = nullptr;
        }
        SafeRelease(candidate);
        SafeRelease(moniker);
    }
    SafeRelease(monikers);
    return *output ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

// Connect every free crossbar output to a free capture input, like the OBS
// DirectConnectFilters helper.
void DirectConnectFilters(IGraphBuilder* graph, IBaseFilter* source, IBaseFilter* sink) {
    IEnumPins* outputs = nullptr;
    if (FAILED(source->EnumPins(&outputs))) return;
    IPin* output = nullptr;
    while (outputs->Next(1, &output, nullptr) == S_OK) {
        PIN_DIRECTION direction{};
        IEnumPins* inputs = nullptr;
        if (SUCCEEDED(output->QueryDirection(&direction)) && direction == PINDIR_OUTPUT &&
            !IsPinConnected(output) && SUCCEEDED(sink->EnumPins(&inputs))) {
            IPin* input = nullptr;
            bool linked = false;
            while (!linked && inputs->Next(1, &input, nullptr) == S_OK) {
                linked = SUCCEEDED(input->QueryDirection(&direction)) &&
                    direction == PINDIR_INPUT && !IsPinConnected(input) &&
                    graph->ConnectDirect(output, input, nullptr) == S_OK;
                SafeRelease(input);
            }
            SafeRelease(inputs);
        }
        SafeRelease(output);
    }
    SafeRelease(outputs);
}

// Same fallback as OBS libdshowcapture: match the video input pin's KS medium
// against AM_KSCATEGORY_CROSSBAR filters, add the match and connect directly.
// The caller removes everything this added when the video input stays unfed.
HRESULT ConnectCrossbarByMedium(IGraphBuilder* graph, IBaseFilter* filter, IPin* videoInput,
                                IBaseFilter** crossbarOutput) {
    *crossbarOutput = nullptr;
    REGPINMEDIUM medium{};
    if (!GetPinMedium(videoInput, medium)) return E_NOINTERFACE;
    IBaseFilter* crossbar = nullptr;
    HRESULT hr = FindCrossbarByMedium(medium, &crossbar);
    if (FAILED(hr)) return hr;
    hr = graph->AddFilter(crossbar, L"Crossbar Filter");
    if (FAILED(hr)) {
        SafeRelease(crossbar);
        return hr;
    }
    DirectConnectFilters(graph, crossbar, filter);
    if (!IsPinConnected(videoInput)) {
        SafeRelease(crossbar);
        return VFW_E_CANNOT_CONNECT;
    }
    *crossbarOutput = crossbar;
    return S_OK;
}

}  // namespace

bool SelectPinMedium(const KSMULTIPLE_ITEM* items, REGPINMEDIUM& medium) {
    // Trust neither field alone: Count must fit inside the reported Size.
    if (!items || items->Size < sizeof(KSMULTIPLE_ITEM)) return false;
    const size_t capacity = (items->Size - sizeof(KSMULTIPLE_ITEM)) / sizeof(REGPINMEDIUM);
    if (items->Count > capacity) return false;
    const auto* current = reinterpret_cast<const REGPINMEDIUM*>(items + 1);
    for (ULONG index = 0; index < items->Count; ++index, ++current) {
        if (!IsEqualGUID(current->clsMedium, GUID_NULL) &&
            !IsEqualGUID(current->clsMedium, kKsMediumSetStandard)) {
            medium = *current;
            return true;
        }
    }
    return false;
}

HRESULT SnapshotGraphFilters(IGraphBuilder* graph, std::vector<IBaseFilter*>& filters) {
    for (IBaseFilter*& filter : filters) SafeRelease(filter);
    filters.clear();
    if (!graph) return E_POINTER;
    IEnumFilters* enumerator = nullptr;
    HRESULT hr = graph->EnumFilters(&enumerator);
    if (FAILED(hr)) return hr;
    IBaseFilter* filter = nullptr;
    while ((hr = enumerator->Next(1, &filter, nullptr)) == S_OK) filters.push_back(filter);
    SafeRelease(enumerator);
    if (hr == S_FALSE) return S_OK;
    // A partial list (for example VFW_E_ENUM_OUT_OF_SYNC) would make original
    // filters look newly added, so it is never returned.
    for (IBaseFilter*& partial : filters) SafeRelease(partial);
    filters.clear();
    return FAILED(hr) ? hr : E_UNEXPECTED;
}

HRESULT RemoveFiltersAddedSince(IGraphBuilder* graph, std::vector<IBaseFilter*>& before) {
    // RemoveFilter disconnects every pin, so partial connections into the
    // original filters are undone as well.
    std::vector<IBaseFilter*> after;
    HRESULT hr = SnapshotGraphFilters(graph, after);
    for (IBaseFilter*& added : after) {
        if (std::find(before.begin(), before.end(), added) == before.end()) {
            const HRESULT removed = graph->RemoveFilter(added);
            if (FAILED(removed) && SUCCEEDED(hr)) hr = removed;
        }
        SafeRelease(added);
    }
    for (IBaseFilter*& filter : before) SafeRelease(filter);
    before.clear();
    return hr;
}

bool NeedsUpstreamCrossbar(IBaseFilter* filter) {
    if (!filter) return false;
    bool ambiguous = false;
    IPin* videoInput = FindVideoInputPin(filter, &ambiguous);
    const bool needed = videoInput && !IsPinConnected(videoInput);
    SafeRelease(videoInput);
    return needed;
}

CrossbarAttachResult AttachUpstreamCrossbar(IGraphBuilder* graph, IBaseFilter* filter,
                                            const wchar_t* label, LogCallback logCallback) {
    if (!graph || !filter) return {E_POINTER, true};
    if (!label) label = L"capture filter";
    // Gate on an unconnected video input pin. Every other filter keeps its
    // graph exactly as before, without a Capture Graph Builder search.
    wchar_t message[512]{};
    bool ambiguous = false;
    IPin* videoInput = FindVideoInputPin(filter, &ambiguous);
    if (!videoInput) {
        if (ambiguous) {
            swprintf_s(message, L"[capture] %s has input pins but none is identifiable as the "
                       L"video input; crossbar not attached.\n", label);
            Log(logCallback, message);
        }
        return {S_FALSE, true};
    }
    if (IsPinConnected(videoInput)) {
        SafeRelease(videoInput);
        return {S_FALSE, true};
    }

    // Without a complete list of the original filters a failed attempt could
    // not be undone safely, so the graph is not touched at all.
    std::vector<IBaseFilter*> before;
    HRESULT hr = SnapshotGraphFilters(graph, before);
    if (FAILED(hr)) {
        SafeRelease(videoInput);
        swprintf_s(message, L"[capture] %s graph filters could not be listed (0x%08X); "
                   L"crossbar not attached.\n", label, static_cast<unsigned>(hr));
        Log(logCallback, message);
        return {hr, true};
    }
    // Undo everything added since `before`. Only a failed undo damages the graph.
    const auto fail = [&](HRESULT failure) -> CrossbarAttachResult {
        SafeRelease(videoInput);
        const HRESULT cleanup = RemoveFiltersAddedSince(graph, before);
        if (FAILED(cleanup)) {
            swprintf_s(message, L"[capture] %s crossbar attempt failed (0x%08X) and the graph "
                       L"could not be restored (0x%08X); capture start stopped.\n", label,
                       static_cast<unsigned>(failure), static_cast<unsigned>(cleanup));
            Log(logCallback, message);
            return {cleanup, false};
        }
        swprintf_s(message, L"[capture] %s has an input pin but no upstream crossbar was "
                   L"connected (0x%08X); continuing with the original graph.\n", label,
                   static_cast<unsigned>(failure));
        Log(logCallback, message);
        return {failure, true};
    };

    // 1. Capture Graph Builder: adds (and usually connects) the upstream KS crossbar.
    ICaptureGraphBuilder2* builder = nullptr;
    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr,
                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder));
    if (SUCCEEDED(hr)) hr = builder->SetFiltergraph(graph);
    IAMCrossbar* crossbar = nullptr;
    if (SUCCEEDED(hr)) {
        hr = builder->FindInterface(&LOOK_UPSTREAM_ONLY, nullptr, filter,
                                    IID_IAMCrossbar, reinterpret_cast<void**>(&crossbar));
    }
    SafeRelease(builder);
    const wchar_t* method = L"capture graph builder";
    // As OBS does with a crossbar it found: connect it directly when the
    // builder left the video input unfed.
    if (SUCCEEDED(hr) && crossbar && !IsPinConnected(videoInput)) {
        IBaseFilter* found = nullptr;
        if (SUCCEEDED(crossbar->QueryInterface(IID_PPV_ARGS(&found))) && found) {
            FILTER_INFO info{};
            bool inGraph = SUCCEEDED(found->QueryFilterInfo(&info)) && info.pGraph;
            SafeRelease(info.pGraph);
            if (!inGraph) inGraph = SUCCEEDED(graph->AddFilter(found, L"Crossbar Filter"));
            if (inGraph) DirectConnectFilters(graph, found, filter);
            if (IsPinConnected(videoInput)) method = L"capture graph builder + direct connection";
        }
        SafeRelease(found);
    }
    // Success means the video input is actually fed, not only that an
    // IAMCrossbar was found upstream or that some other input was connected.
    if (FAILED(hr) || !crossbar || !IsPinConnected(videoInput)) {
        SafeRelease(crossbar);
        // `before` is consumed here; never run fail() against an empty list.
        const HRESULT cleanup = RemoveFiltersAddedSince(graph, before);
        if (FAILED(cleanup)) {
            SafeRelease(videoInput);
            swprintf_s(message, L"[capture] %s crossbar attempt could not restore the graph "
                       L"(0x%08X); capture start stopped.\n", label, static_cast<unsigned>(cleanup));
            Log(logCallback, message);
            return {cleanup, false};
        }
        hr = SnapshotGraphFilters(graph, before);
        if (FAILED(hr)) {
            SafeRelease(videoInput);
            swprintf_s(message, L"[capture] %s graph filters could not be listed again (0x%08X); "
                       L"continuing with the original graph.\n", label, static_cast<unsigned>(hr));
            Log(logCallback, message);
            return {hr, true};
        }
        // 2. KS medium fallback, as in OBS. A connected video input is the
        //    success condition; IAMCrossbar is only needed for the routing log.
        IBaseFilter* crossbarFilter = nullptr;
        hr = ConnectCrossbarByMedium(graph, filter, videoInput, &crossbarFilter);
        if (FAILED(hr)) return fail(hr);
        crossbarFilter->QueryInterface(IID_PPV_ARGS(&crossbar));
        SafeRelease(crossbarFilter);
        method = L"KS medium";
    }
    SafeRelease(videoInput);
    for (IBaseFilter*& existing : before) SafeRelease(existing);
    before.clear();

    if (!crossbar) {
        swprintf_s(message, L"[capture] %s upstream crossbar connected via %s; routing "
                   L"unchanged (routing details unavailable).\n", label, method);
        Log(logCallback, message);
        return {S_OK, true};
    }
    long outputs = 0, inputs = 0;
    hr = crossbar->get_PinCounts(&outputs, &inputs);
    swprintf_s(message, L"[capture] %s upstream crossbar connected via %s: %ld output(s), "
               L"%ld input(s); routing unchanged.\n", label, method,
               SUCCEEDED(hr) ? outputs : 0L, SUCCEEDED(hr) ? inputs : 0L);
    Log(logCallback, message);
    for (long output = 0; SUCCEEDED(hr) && output < outputs && output < 16; ++output) {
        long related = 0, outputType = 0, routed = -1, inputType = 0;
        if (FAILED(crossbar->get_CrossbarPinInfo(FALSE, output, &related, &outputType))) continue;
        const bool hasRoute = SUCCEEDED(crossbar->get_IsRoutedTo(output, &routed)) && routed >= 0 &&
            SUCCEEDED(crossbar->get_CrossbarPinInfo(TRUE, routed, &related, &inputType));
        if (hasRoute) {
            swprintf_s(message, L"[capture]   crossbar output %ld (%s) <- input %ld (%s, 0x%lX)\n",
                       output, PhysicalConnectorName(outputType), routed,
                       PhysicalConnectorName(inputType), inputType);
        } else {
            swprintf_s(message, L"[capture]   crossbar output %ld (%s) has no input route\n",
                       output, PhysicalConnectorName(outputType));
        }
        Log(logCallback, message);
    }
    SafeRelease(crossbar);
    return {S_OK, true};
}

}  // namespace llcv::capture
