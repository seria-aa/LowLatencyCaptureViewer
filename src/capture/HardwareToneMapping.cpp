#include "HardwareToneMapping.h"
#include "ElgatoHidToneMapping.h"
#include <dshow.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <vidcap.h>
#include <wrl/client.h>
#include <array>
#include <cstddef>
#include <cwchar>
#include <cstring>

namespace llcv::capture {
namespace {
// Official Elgato protocol: capture-device-support fe9630974d47f51bf54826e72fb8b654e620aa93.
// AVerMedia protocols: OBS libdshowcapture c13d4b7b0c66979396ba0a9060c9aafc15bb7b22.
// Only protocol layouts are used; no vendor SDK or background processing.
constexpr GUID kElgato = {0xd1e5209f,0x68fd,0x4529,{0xbe,0xe0,0x5e,0x7a,0x1f,0x47,0x92,0x26}};
constexpr GUID kAver = {0x8a80d56f,0xfac5,0x4692,{0xa4,0x16,0xcf,0x20,0xd4,0xa1,0x8f,0x47}};
constexpr GUID kAverUvc = {0xc835261b,0xff1c,0x4c9a,{0xb2,0xf7,0x93,0xc9,0x1f,0xcf,0xbe,0x77}};
struct AverPayload { KSPROPERTY header; DWORD enable; };
static_assert(offsetof(AverPayload, enable) == 24 && sizeof(AverPayload) == 32);

bool IsUnsupported(HRESULT hr) {
    return hr == E_NOTIMPL || hr == E_PROP_SET_UNSUPPORTED || hr == E_PROP_ID_UNSUPPORTED ||
        hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) || hr == HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
bool CanTryAlternative(const ToneMappingResult& result) {
    // Never retry after a SET, ambiguous discovery, or unexpected driver error.
    return result.status == ToneMappingStatus::Unsupported || result.status == ToneMappingStatus::NoInterface;
}
ToneMappingResult PropertyRequest(IUnknown* filter, bool enable, ToneMappingVendor vendor) {
    ToneMappingResult result;
    result.enable = enable; result.vendor = vendor;
    Microsoft::WRL::ComPtr<IKsPropertySet> properties;
    result.result = filter ? filter->QueryInterface(IID_PPV_ARGS(properties.GetAddressOf())) : E_POINTER;
    if (result.result != S_OK || !properties) {
        if (result.result == S_OK) result.result = E_NOINTERFACE;
        result.status = result.result == E_NOINTERFACE || result.result == E_POINTER
            ? ToneMappingStatus::NoInterface : ToneMappingStatus::QueryFailed;
        return result;
    }
    const bool aver = vendor == ToneMappingVendor::AverMedia;
    const auto& guid = aver ? kAver : kElgato;
    const DWORD id = aver ? 2u : 722u;
    result.result = properties->QuerySupported(guid, id, &result.support);
    if (result.result != S_OK) {
        result.status = IsUnsupported(result.result) ? ToneMappingStatus::Unsupported : ToneMappingStatus::QueryFailed;
        return result;
    }
    if (!(result.support & KSPROPERTY_SUPPORT_SET)) {
        result.result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        result.status = ToneMappingStatus::Unsupported;
        return result;
    }
    if (aver) {
        AverPayload payload;
        std::memset(&payload, 0, sizeof(payload)); // Includes native ABI tail padding.
        payload.enable = enable ? 1u : 0u;
        result.result = properties->Set(guid, id, &payload.enable,
            sizeof(payload) - sizeof(payload.header), &payload, sizeof(payload));
    } else {
        DWORD value = enable ? 1u : 0u;
        result.result = properties->Set(guid, id, nullptr, 0, &value, sizeof(value));
    }
    result.status = result.result == S_OK ? ToneMappingStatus::Applied : ToneMappingStatus::SetFailed;
    return result;
}
ToneMappingResult UvcRequest(IUnknown* filter, bool enable) {
    ToneMappingResult result;
    result.enable = enable; result.vendor = ToneMappingVendor::AverMediaUvc;
    Microsoft::WRL::ComPtr<IKsControl> control;
    Microsoft::WRL::ComPtr<IKsTopologyInfo> topology;
    result.result = filter ? filter->QueryInterface(IID_PPV_ARGS(control.GetAddressOf())) : E_POINTER;
    if (result.result == S_OK && control)
        result.result = filter->QueryInterface(IID_PPV_ARGS(topology.GetAddressOf()));
    if (result.result != S_OK || !control || !topology) {
        if (result.result == S_OK) result.result = E_NOINTERFACE;
        result.status = result.result == E_NOINTERFACE || result.result == E_POINTER
            ? ToneMappingStatus::NoInterface : ToneMappingStatus::QueryFailed;
        return result;
    }
    DWORD count = 0;
    result.result = topology->get_NumNodes(&count);
    if (result.result != S_OK || count > 64) {
        if (result.result == S_OK) result.result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        result.status = ToneMappingStatus::QueryFailed;
        return result;
    }
    KSP_NODE selected{};
    std::array<unsigned char, 20> saved{};
    bool found = false;
    for (DWORD node = 0; node < count; ++node) {
        GUID type{};
        result.result = topology->get_NodeType(node, &type);
        if (result.result != S_OK) { result.status = ToneMappingStatus::QueryFailed; return result; }
        if (type != KSNODETYPE_DEV_SPECIFIC) continue;
        KSP_NODE request{};
        request.Property.Set = kAverUvc; request.Property.Id = 11;
        request.Property.Flags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_TOPOLOGY;
        request.NodeId = node;
        std::array<unsigned char, 20> data{};
        ULONG returned = 0;
        result.result = control->KsProperty(&request.Property, sizeof(request),
            data.data(), static_cast<ULONG>(data.size()), &returned);
        if (IsUnsupported(result.result)) continue;
        if (result.result != S_OK || returned < 18 || returned > data.size()) {
            if (result.result == S_OK) result.result = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            result.status = ToneMappingStatus::QueryFailed;
            return result;
        }
        if (found) {
            result.status = ToneMappingStatus::AmbiguousDevice;
            result.result = HRESULT_FROM_WIN32(ERROR_DUP_NAME);
            return result; // No writes until discovery has finished.
        }
        found = true; selected = request; saved = data;
    }
    if (!found) {
        result.status = ToneMappingStatus::Unsupported;
        result.result = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        return result;
    }
    saved[15] = 0x02; saved[17] = enable ? 1 : 0;
    selected.Property.Flags = KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_TOPOLOGY;
    ULONG returned = 0;
    result.result = control->KsProperty(&selected.Property, sizeof(selected),
        saved.data(), static_cast<ULONG>(saved.size()), &returned);
    result.status = result.result == S_OK ? ToneMappingStatus::Applied : ToneMappingStatus::SetFailed;
    return result;
}
void Report(const ToneMappingResult& result, void (*log)(const wchar_t*)) {
    if (!log) return;
    const wchar_t* vendor = L"unknown";
    switch (result.vendor) {
    case ToneMappingVendor::Elgato: vendor = L"Elgato driver"; break;
    case ToneMappingVendor::AverMedia: vendor = L"AVerMedia driver"; break;
    case ToneMappingVendor::AverMediaUvc: vendor = L"AVerMedia UVC extension"; break;
    case ToneMappingVendor::ElgatoHid: vendor = L"Elgato selected USB/HID"; break;
    default: break;
    }
    const wchar_t* status = L"not applicable";
    switch (result.status) {
    case ToneMappingStatus::Applied: status = L"command accepted (input encoding not verified)"; break;
    case ToneMappingStatus::NoInterface: status = L"skipped: interface unavailable"; break;
    case ToneMappingStatus::Unsupported: status = L"skipped: protocol unsupported"; break;
    case ToneMappingStatus::QueryFailed: status = L"skipped: capability/identity query failed"; break;
    case ToneMappingStatus::SetFailed: status = L"failed: device rejected command"; break;
    case ToneMappingStatus::AmbiguousDevice: status = L"skipped: ambiguous device or control node"; break;
    default: break;
    }
    wchar_t text[512]{};
    swprintf_s(text, L"[capture-hdr] %s hardware HDR-to-SDR: requested=%s; %s; result=0x%08lX support=0x%lX; selected device only.\n",
        vendor, result.enable ? L"on" : L"off", status, static_cast<unsigned long>(result.result), result.support);
    log(text);
}
bool AverName(std::wstring_view name) {
    constexpr std::wstring_view vendor = L"avermedia";
    for (size_t i = 0; i + vendor.size() <= name.size(); ++i)
        if (_wcsnicmp(name.data() + i, vendor.data(), vendor.size()) == 0) return true;
    return false;
}
}
ToneMappingResult ConfigureHardwareToneMapping(IUnknown* filter, std::wstring_view name,
    settings::VideoPixelFormat format, void (*log)(const wchar_t*), std::wstring_view videoPath) {
    bool enable = false;
    switch (format) {
    case settings::VideoPixelFormat::P010: break;
    case settings::VideoPixelFormat::Nv12:
    case settings::VideoPixelFormat::Yuy2:
    case settings::VideoPixelFormat::Uyvy:
    case settings::VideoPixelFormat::Mjpeg: enable = true; break;
    default: return {};
    }
    // Name affects probe order only. No model-name support table for KS controls.
    const auto first = AverName(name) ? ToneMappingVendor::AverMedia : ToneMappingVendor::Elgato;
    const auto second = first == ToneMappingVendor::AverMedia ? ToneMappingVendor::Elgato : ToneMappingVendor::AverMedia;
    auto primary = PropertyRequest(filter, enable, first);
    Report(primary, log);
    if (!filter || !CanTryAlternative(primary)) return primary;
    auto alternate = PropertyRequest(filter, enable, second);
    if (!CanTryAlternative(alternate)) { Report(alternate, log); return alternate; }
    alternate = UvcRequest(filter, enable);
    if (!CanTryAlternative(alternate)) { Report(alternate, log); return alternate; }
    // The HID helper refuses missing/unknown identity before any device access.
    if (!videoPath.empty()) {
        alternate = ConfigureElgatoHidToneMapping(videoPath, enable);
        Report(alternate, log);
        if (alternate.status != ToneMappingStatus::NotApplicable) return alternate;
    }
    return primary;
}
}
