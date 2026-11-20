#pragma once

#include "settings/AppSettings.h"

namespace llcv::interpolation {
enum class Provider { Unavailable, NvidiaVfg, RifeVulkan };

constexpr Provider ChooseProvider(settings::InterpolationMethod method, bool vfgGpuSupported,
                                  bool rifeBundled) noexcept {
    switch (method) {
    case settings::InterpolationMethod::NvidiaVfg:
        // Keep an explicit VFG choice even if it still needs its download.
        return vfgGpuSupported ? Provider::NvidiaVfg : Provider::Unavailable;
    case settings::InterpolationMethod::RifeVulkan:
        return rifeBundled ? Provider::RifeVulkan : Provider::Unavailable;
    }
    return Provider::Unavailable;
}

constexpr bool UseRifeCaptureOverride(Provider provider, bool optedIn, bool audioOnly,
                                      settings::VideoPixelFormat format) noexcept {
    // F7 skips generation but does not reconnect the capture device.
    return optedIn && !audioOnly && provider==Provider::RifeVulkan && format!=settings::VideoPixelFormat::P010;
}

inline constexpr int kRifeMinCaptureRate = 24;
inline constexpr int kRifeMaxCaptureRate = 60;
constexpr bool RifeCaptureRateSupported(int fps) noexcept {
    return fps >= kRifeMinCaptureRate && fps <= kRifeMaxCaptureRate;
}

constexpr int RifeCaptureRate(int overrideRate, int originalRate, bool capture40) noexcept {
    if (overrideRate == 0 || RifeCaptureRateSupported(overrideRate)) return overrideRate;
    if (capture40) return 40;
    return RifeCaptureRateSupported(originalRate) ? originalRate : 0;
}

inline int RifeCaptureRate(const settings::AppSettings& settings) noexcept {
    return RifeCaptureRate(settings.rifeCaptureFrameRate, settings.videoFrameRate, settings.capture40Fps);
}
}
