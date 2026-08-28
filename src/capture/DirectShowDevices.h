#pragma once

#include <dshow.h>

#include <string>
#include <vector>

namespace llcv::capture {

using LogCallback = void (*)(const wchar_t* message);

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
};

std::vector<DeviceInfo> EnumerateVideoInputDevices();
std::vector<DeviceInfo> EnumerateAudioInputDevices();

HRESULT FindVideoCaptureFilter(
    const std::wstring& selectedId, const wchar_t* preferredDeviceName,
    IBaseFilter** output, std::wstring* selectedName,
    LogCallback logCallback);
HRESULT FindCaptureAudioFilter(
    const std::wstring& selectedId, const std::wstring& videoName,
    IBaseFilter** output, std::wstring* selectedName,
    LogCallback logCallback);

HRESULT FindOutputPinByName(
    IBaseFilter* filter, const wchar_t* name, IPin** output);
HRESULT GetFirstPin(
    IBaseFilter* filter, PIN_DIRECTION wantedDirection, IPin** output);
HRESULT FindOutputPinByMajorType(
    IBaseFilter* filter, const GUID& majorType, IPin** output);
void LogFilterPins(
    IBaseFilter* filter, const wchar_t* label, LogCallback logCallback);

int RelatedCaptureAudioScore(
    const std::wstring& videoName, const std::wstring& audioName);

}  // namespace llcv::capture
