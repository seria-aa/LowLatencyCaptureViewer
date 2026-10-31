#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace llcv::recording {
inline constexpr uint64_t kRgbCaptureBudget = 128ull * 1024 * 1024;
inline constexpr uint64_t kRgbOutputBudget = 192ull * 1024 * 1024;
inline size_t RgbBufferCount(uint64_t frameBytes, uint64_t budget) noexcept {
    return frameBytes && frameBytes <= budget
               ? static_cast<size_t>(std::min<uint64_t>(16, budget / frameBytes))
               : 0;
}
struct RgbPreflight {
    double meanMs = 0, maximumMs = 0, budgetMs = 0;
    unsigned samples = 0;
};
inline bool RgbHasHeadroom(const RgbPreflight& p) noexcept {
    // Reserve time for encoding/storage; this is not a throughput guarantee.
    return p.samples >= 3 && std::isfinite(p.meanMs) && std::isfinite(p.maximumMs) &&
           std::isfinite(p.budgetMs) && p.meanMs >= 0 && p.maximumMs >= p.meanMs &&
           p.budgetMs > 0 && p.meanMs <= p.budgetMs * .75 && p.maximumMs <= p.budgetMs;
}
}
