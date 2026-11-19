#pragma once

#include "settings/AppSettings.h"
#include "video/RifeHalfTest.h"

namespace llcv::vfg {
inline void RestorePreferences(const settings::AppSettings& saved, bool gpuAvailable) {
    using namespace rife_test;
    SelectScale(saved.vfgLowQuality ? Scale::VfgLow : Scale::VfgMedium);
    SetQualityComparison(saved.vfgWaitForFrame);
    EnableComponents(saved.vfgEnabled && gpuAvailable);
    if (!saved.vfgPlaybackEnabled) Request(false);
}

inline void AcceptPreferences(settings::AppSettings& saved, bool gpuAvailable, bool checkboxEdited) {
    if (!gpuAvailable) return;
    // F7 changes requested, not the checkbox that opts into VFG.
    saved.vfgEnabled = rife_test::componentsEnabled.load();
    if (checkboxEdited) saved.vfgPlaybackEnabled = saved.vfgEnabled;
    saved.vfgLowQuality = rife_test::selectedScale.load() == rife_test::Scale::VfgLow;
    saved.vfgWaitForFrame = rife_test::qualityComparison.load();
}
}
