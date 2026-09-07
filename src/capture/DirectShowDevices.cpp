#include "capture/DirectShowDevices.h"

#include <algorithm>
#include <cwctype>

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

std::wstring MonikerFriendlyName(IMoniker* moniker) {
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

}  // namespace llcv::capture
