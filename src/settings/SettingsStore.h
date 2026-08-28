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
void SaveToIni(const std::wstring& path, const AppSettings& settings);

}  // namespace llcv::settings
