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
void SaveToIni(const std::wstring& path, const AppSettings& settings);

}  // namespace llcv::settings
