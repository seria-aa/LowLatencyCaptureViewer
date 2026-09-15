#include "HdrDisplay.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>
#include <cwchar>

namespace llcv::hdr {
using Microsoft::WRL::ComPtr;

DisplayState QueryDisplay(HWND window) {
    DisplayState result;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!monitor) return result;
    // A fresh factory is intentional: cached output descriptions can be stale
    // after Windows HDR changes. Only initialization and the UI timer call this;
    // the steady-state capture/render loop never enumerates displays.
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
    bool found = false;
    for (UINT a = 0; !found; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(a, &adapter))) break;
        for (UINT o = 0; !found; ++o) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(o, &output))) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || desc.Monitor != monitor) continue;
            found = true;
            wcscpy_s(result.name, desc.DeviceName);
            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc1{};
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&desc1))) {
                result.hdr = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 1 : 0;
                result.bits = desc1.BitsPerColor;
            }
        }
    }
    if (!found || result.hdr != 1) return result;
    // Match the DXGI output's GDI source name to its display-config target.
    // Both APIs are read-only; never enable HDR or change the monitor mode here.
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathsCount = 0, modesCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathsCount, &modesCount) != ERROR_SUCCESS)
            break;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathsCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modesCount);
        const LONG status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathsCount,
            paths.data(), &modesCount, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) continue;
        if (status != ERROR_SUCCESS) break;
        for (UINT32 i = 0; i < pathsCount; ++i) {
            const auto& path = paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                             path.sourceInfo.adapterId, path.sourceInfo.id};
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                _wcsicmp(source.viewGdiDeviceName, result.name) != 0) continue;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
            white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(white),
                            path.targetInfo.adapterId, path.targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) {
                float nits = 0.0f;
                if (DecodeSdrWhiteLevel(white.SDRWhiteLevel, nits)) {
                    result.uiWhiteNits = nits;
                    result.systemWhite = true;
                }
            }
            return result;
        }
        break;
    }
    return result;
}
}
