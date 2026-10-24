#pragma once

#include "settings/AppSettings.h"

#include <string>

namespace llcv::settings {

constexpr int kRelativeWindowScaleVersion = 4;

struct LoadResult {
    AppSettings settings;
    int relativeWindowScaleVersion = 0;
};

LoadResult LoadFromIni(const std::wstring& path);
// One-time 20 -> 25 ms upgrade; touches only the PCM value and its marker.
// Returns false on persistence failure. LoadFromIni still supplies 25 in memory.
bool MigrateLegacyPcmQueueTarget(const std::wstring& path);
// Write a complete sibling file, then atomically replace the profile. Failure
// leaves the last saved profile intact; error receives a Win32 error code.
bool SaveToIni(const std::wstring& path, const AppSettings& settings,
               unsigned long* error = nullptr);
// Version-specific startup review. Acknowledgement changes only its marker,
// never the immediate-start preference or unaccepted dialog values.
bool StartupReviewRequired(const std::wstring& path, const std::wstring& version);
bool MarkStartupSettingsShown(const std::wstring& path, const std::wstring& version,
                             unsigned long* error = nullptr);

}  // namespace llcv::settings
