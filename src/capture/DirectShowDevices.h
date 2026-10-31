#pragma once

#include <dshow.h>
#include <ks.h>

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
    LogCallback logCallback, std::wstring* selectedDevicePath = nullptr);
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

struct CrossbarAttachResult {
    // S_OK: connected. S_FALSE: not applicable or skipped. Failure: no
    // crossbar was connected.
    HRESULT result = S_FALSE;
    // False only when a failed attempt could not be undone. The graph must not
    // be started then; every other outcome leaves the original graph usable.
    bool graphIntact = true;
};

// Multi-input WDM capture filters (for example HDMI + component) expose an
// input pin fed by a separate crossbar filter. Only for a filter with an
// unconnected (video) input pin, connect that upstream crossbar the way OBS
// libdshowcapture does for video: the Capture Graph Builder search, then a
// direct connection of the crossbar it found, then a KS medium match with a
// direct connection. Success requires that video input pin to be connected;
// another input alone does not count. A filter whose video input cannot be
// identified (audio-typed pins are excluded, and only a single untyped input
// qualifies) is skipped with a log line. The current routing is never
// changed. Filters without an input pin, such as single-input HDMI devices,
// are not touched. The filter must already be in the graph. On failure, filters
// added by the attempt are removed; if the graph cannot be listed first,
// nothing is attempted. IAMCrossbar is used only for the routing log.
CrossbarAttachResult AttachUpstreamCrossbar(IGraphBuilder* graph, IBaseFilter* filter,
                                            const wchar_t* label, LogCallback logCallback);
// True when AttachUpstreamCrossbar would try a connection for this filter.
bool NeedsUpstreamCrossbar(IBaseFilter* filter);
const wchar_t* PhysicalConnectorName(long type);

// Building blocks of AttachUpstreamCrossbar, exposed for tests.
// Selects the first non-standard medium after checking Count against Size.
bool SelectPinMedium(const KSMULTIPLE_ITEM* items, REGPINMEDIUM& medium);
// AddRef'd filters currently in the graph. Fails, leaving the list empty,
// unless the whole graph was enumerated; a partial list is never returned.
HRESULT SnapshotGraphFilters(IGraphBuilder* graph, std::vector<IBaseFilter*>& filters);
// Removes every filter not in the snapshot, then releases the snapshot.
// Fails when the graph cannot be listed or a RemoveFilter call fails.
HRESULT RemoveFiltersAddedSince(IGraphBuilder* graph, std::vector<IBaseFilter*>& before);

int RelatedCaptureAudioScore(
    const std::wstring& videoName, const std::wstring& audioName);

}  // namespace llcv::capture
