#pragma once

#include <dxgi1_6.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace llcv::video_gpu {
struct Adapter {
    std::wstring id;
    std::wstring name;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> device;
};

// A LUID is useful to match the live D3D11 and CUDA devices, but must not
// be persisted. Identical PCI tuples are ambiguous; never guess between them.
inline const Adapter* Find(const std::vector<Adapter>& adapters, std::wstring_view id) {
    if (id.empty()) return nullptr;
    const Adapter* match = nullptr;
    for (const auto& adapter : adapters) {
        if (adapter.id != id) continue;
        if (match) return nullptr;
        match = &adapter;
    }
    return match;
}

inline std::wstring PciLocation(const LUID& luid) {
    D3DKMT_OPENADAPTERFROMLUID open{luid, 0};
    if (D3DKMTOpenAdapterFromLuid(&open) < 0) return {};
    D3DKMT_ADAPTERADDRESS address{};
    D3DKMT_QUERYADAPTERINFO query{open.hAdapter, KMTQAITYPE_ADAPTERADDRESS, &address, sizeof(address)};
    const auto status = D3DKMTQueryAdapterInfo(&query);
    const D3DKMT_CLOSEADAPTER close{open.hAdapter};
    D3DKMTCloseAdapter(&close);
    if (status < 0 || address.BusNumber > 255 || address.DeviceNumber > 31 || address.FunctionNumber > 7)
        return {};
    wchar_t location[16]{};
    swprintf_s(location, L"%02X:%02X.%u", address.BusNumber, address.DeviceNumber, address.FunctionNumber);
    return location;
}

inline bool HasAttachedOutput(IDXGIAdapter* device) {
    if (!device) return false;
    for (UINT index = 0; ; ++index) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        if (FAILED(device->EnumOutputs(index, &output))) return false;
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) return true;
    }
}

inline std::vector<Adapter> Enumerate() {
    std::vector<Adapter> result;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
    for (UINT index = 0; ; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(index, &adapter))) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        wchar_t id[64]{};
        swprintf_s(id, L"%08X:%08X:%08X:%08X", desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision);
        const auto location = PciLocation(desc.AdapterLuid);
        const std::wstring key = location.empty() ? std::wstring(id) : std::wstring(id) + L"@" + location;
        const auto duplicate = std::find_if(result.begin(), result.end(),
            [&](const auto& entry) { return entry.id == key; });
        if (duplicate != result.end()) {
            // Display and render aliases can share one physical PCI location.
            // Prefer the alias with the active output over a render-only one.
            if (!HasAttachedOutput(duplicate->device.Get()) && HasAttachedOutput(adapter.Get()))
                duplicate->device = adapter;
            continue;
        }
        result.push_back({key, desc.Description, adapter});
    }
    // A driver may expose an alias whose physical location cannot be read.
    // Do not list an unidentifiable copy alongside entries with a known location.
    std::vector<std::wstring> knownModels;
    for (const auto& entry : result)
        if (const auto split = entry.id.find(L'@'); split != std::wstring::npos)
            knownModels.push_back(entry.id.substr(0, split));
    std::erase_if(result, [&](const auto& entry) {
        return std::find(knownModels.begin(), knownModels.end(), entry.id) != knownModels.end();
    });
    std::vector<std::wstring> displayNames;
    for (const auto& entry : result) displayNames.push_back(entry.name);
    for (auto& entry : result) {
        const auto split = entry.id.find(L'@');
        if (split != std::wstring::npos && std::count(displayNames.begin(), displayNames.end(), entry.name) > 1)
            entry.name += L" (PCI " + entry.id.substr(split + 1) + L")";
    }
    return result;
}

inline bool OwnsMonitor(IDXGIAdapter* adapter, HMONITOR monitor) {
    if (!adapter || !monitor) return false;
    for (UINT index = 0; ; ++index) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(index, &output))) return false;
        DXGI_OUTPUT_DESC desc{};
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop && desc.Monitor == monitor)
            return true;
    }
}

inline Microsoft::WRL::ComPtr<IDXGIAdapter1> Automatic(HMONITOR exclusiveMonitor = nullptr) {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
    for (UINT index = 0; ; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter)))) return {};
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if (!exclusiveMonitor || OwnsMonitor(adapter.Get(), exclusiveMonitor)) return adapter;
    }
}
}
