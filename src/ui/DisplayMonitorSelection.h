#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cwchar>

namespace llcv::display {
struct MonitorChoice {
    HMONITOR handle{};
    std::wstring id;
    std::wstring label;
};
// Prefer the monitor device interface to DISPLAYn, whose number can change
// when displays are disconnected. Fallback is only for drivers without an ID.
inline std::wstring MonitorId(const MONITORINFOEXW& info, std::wstring* model=nullptr) {
    DISPLAY_DEVICEW device{sizeof(device)};
    if (EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME)) {
        if (model) *model=device.DeviceString;
        if (device.DeviceID[0]) return std::wstring(L"interface:")+device.DeviceID;
    }
    return std::wstring(L"output:")+info.szDevice;
}
inline std::vector<MonitorChoice> EnumerateMonitors(bool english) {
    struct Context { bool english; std::vector<MonitorChoice> choices; } context{english,{}};
    EnumDisplayMonitors(nullptr,nullptr,
        [](HMONITOR handle,HDC,LPRECT,LPARAM value)->BOOL {
            auto& context=*reinterpret_cast<Context*>(value);
            MONITORINFOEXW info{};info.cbSize=sizeof(info);
            if (!GetMonitorInfoW(handle,&info)) return TRUE;
            std::wstring model;
            auto id=MonitorId(info,&model);
            wchar_t dimensions[80]{};
            swprintf_s(dimensions,L" · %ld x %ld",
                info.rcMonitor.right-info.rcMonitor.left,
                info.rcMonitor.bottom-info.rcMonitor.top);
            std::wstring label=info.szDevice;
            if(label.rfind(L"\\\\.\\",0)==0) label.erase(0,4);
            if(!model.empty()) label+=L" · "+model;
            label+=dimensions;
            if(info.dwFlags&MONITORINFOF_PRIMARY)
                label+=context.english?L" (Primary)":L" (주 모니터)";
            context.choices.push_back({handle,std::move(id),std::move(label)});
            return TRUE;
        },reinterpret_cast<LPARAM>(&context));
    return context.choices;
}
inline HMONITOR FindMonitor(const std::vector<MonitorChoice>& monitors,const std::wstring& id) {
    if(id.empty()) return nullptr;
    for(const auto& monitor:monitors)
        if(_wcsicmp(monitor.id.c_str(),id.c_str())==0) return monitor.handle;
    return nullptr;
}
} // namespace llcv::display
