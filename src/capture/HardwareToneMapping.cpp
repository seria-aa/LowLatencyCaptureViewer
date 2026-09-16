#include "HardwareToneMapping.h"

#include <dshow.h>
#include <ks.h>
#include <ksproxy.h>
#include <wrl/client.h>
#include <cstddef>
#include <cwchar>
#include <cstring>

namespace llcv::capture {
namespace {

// AVerMedia driver protocol also used by OBS libdshowcapture:
// source/device-vendor.cpp, SetTonemapperAvermedia (property 2).
// This is NOT the GC553 USB extension-unit protocol or a generic HDR property.
constexpr GUID kAverHdrProperty = {
    0x8a80d56f, 0xfac5, 0x4692, {0xa4, 0x16, 0xcf, 0x20, 0xd4, 0xa1, 0x8f, 0x47}
};
constexpr DWORD kToneMappingProperty = 2;
struct ToneMappingPayload {
    KSPROPERTY header;
    DWORD enable;
};
static_assert(offsetof(ToneMappingPayload, enable) == 24);
static_assert(sizeof(ToneMappingPayload) == 32);

bool IsAvermedia(std::wstring_view name) {
    constexpr std::wstring_view vendor = L"avermedia";
    // Friendly-name gate plus property capability check: never send this
    // vendor-specific write to arbitrary capture devices.
    for (size_t i = 0; i + vendor.size() <= name.size(); ++i) {
        bool matches = true;
        for (size_t j = 0; j < vendor.size(); ++j) {
            wchar_t ch = name[i + j];
            if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
            if (ch != vendor[j]) { matches = false; break; }
        }
        if (matches) return true;
    }
    return false;
}

const wchar_t* StatusText(ToneMappingStatus status) {
    switch (status) {
    case ToneMappingStatus::Applied: return L"command accepted (input encoding not verified)";
    case ToneMappingStatus::NoInterface: return L"skipped: property interface unavailable";
    case ToneMappingStatus::Unsupported: return L"skipped: property SET unsupported";
    case ToneMappingStatus::QueryFailed: return L"skipped: capability query failed";
    case ToneMappingStatus::SetFailed: return L"failed: device rejected command";
    default: return L"not applicable";
    }
}

} // namespace

ToneMappingResult ConfigureHardwareToneMapping(
    IUnknown* selectedFilter, std::wstring_view selectedName,
    settings::VideoPixelFormat negotiatedFormat, void (*log)(const wchar_t*)) {
    ToneMappingResult result;
    using Format = settings::VideoPixelFormat;
    if (!IsAvermedia(selectedName)) return result;
    switch (negotiatedFormat) {
    case Format::P010: result.enable = false; break;
    case Format::Nv12:
    case Format::Yuy2:
    case Format::Mjpeg: result.enable = true; break;
    default: return result; // Never act on an unresolved Auto/invalid format.
    }

    Microsoft::WRL::ComPtr<IKsPropertySet> properties;
    result.result = selectedFilter
        ? selectedFilter->QueryInterface(IID_PPV_ARGS(properties.GetAddressOf()))
        : E_POINTER;
    if (FAILED(result.result) || !properties) {
        if (SUCCEEDED(result.result)) result.result = E_NOINTERFACE;
        result.status = ToneMappingStatus::NoInterface;
    } else {
        result.result = properties->QuerySupported(
            kAverHdrProperty, kToneMappingProperty, &result.support);
        if (result.result != S_OK) {
            const bool unsupported = result.result == E_NOTIMPL ||
                result.result == E_PROP_SET_UNSUPPORTED ||
                result.result == E_PROP_ID_UNSUPPORTED;
            result.status = unsupported ? ToneMappingStatus::Unsupported
                                        : ToneMappingStatus::QueryFailed;
        } else if (!(result.support & KSPROPERTY_SUPPORT_SET)) {
            result.status = ToneMappingStatus::Unsupported;
            result.result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        } else {
            ToneMappingPayload payload{};
            // KSPROPERTY is 8-byte aligned: the x64 payload has four trailing
            // padding bytes. Those bytes are part of the vendor ABI too.
            std::memset(&payload, 0, sizeof(payload));
            payload.enable = result.enable ? 1u : 0u;
            // Match the vendor's native layout, including tail padding:
            // instance = payload after KSPROPERTY, data = complete payload.
            result.result = properties->Set(kAverHdrProperty, kToneMappingProperty,
                &payload.enable, sizeof(payload) - sizeof(payload.header), &payload, sizeof(payload));
            result.status = result.result == S_OK ? ToneMappingStatus::Applied
                                                 : ToneMappingStatus::SetFailed;
        }
    }
    if (log) {
        wchar_t message[512]{};
        swprintf_s(message,
            L"[capture-hdr] AVerMedia hardware HDR-to-SDR: requested=%s (%s); %s; result=0x%08lX support=0x%lX.\n",
            result.enable ? L"on" : L"off",
            result.enable ? L"SDR capture format" : L"P010; preserve input transfer/gamut",
            StatusText(result.status), static_cast<unsigned long>(result.result), result.support);
        log(message);
    }
    return result;
}

} // namespace llcv::capture
