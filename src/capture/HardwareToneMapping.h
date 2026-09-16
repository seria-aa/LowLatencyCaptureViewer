#pragma once

#include "settings/AppSettings.h"
#include <unknwn.h>
#include <string_view>

namespace llcv::capture {

enum class ToneMappingStatus {
    NotApplicable, NoInterface, Unsupported, QueryFailed, SetFailed, Applied
};

struct ToneMappingResult {
    ToneMappingStatus status = ToneMappingStatus::NotApplicable;
    bool enable = false;
    HRESULT result = S_FALSE;
    DWORD support = 0;
};

// Startup only, on the selected video filter after SetFormat and before
// connecting/running the graph. Does not enumerate or open any other device.
// P010 requests unmodified wide-range input (even without Force HDR10).
// SDR capture formats request hardware HDR->SDR conversion when supported.
// A successful Set is command acceptance, not proof of the incoming encoding.
ToneMappingResult ConfigureHardwareToneMapping(
    IUnknown* selectedFilter, std::wstring_view selectedName,
    settings::VideoPixelFormat negotiatedFormat,
    void (*log)(const wchar_t*) = nullptr);

} // namespace llcv::capture
