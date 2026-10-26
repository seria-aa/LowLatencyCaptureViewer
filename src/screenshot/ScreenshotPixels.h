#pragma once

#include "video/VideoColor.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace llcv::screenshot {

enum class Format { Nv12, Yuy2, P010, Uyvy };
struct Description {
    unsigned width = 0, height = 0;
    Format format = Format::Nv12;
    video_color::Configuration color{};
    // Resolved input siting, not the user's Auto selection. P010 is PQ/2020
    // limited only, after the capture path has validated its HDR policy.
    bool topLeftChroma = false;
};

// Bounded to the viewer's maximum capture resolution; positive top-down stride.
std::size_t PackedSize(const Description& desc) noexcept;
bool CopyFrame(const Description& desc, const std::uint8_t* source,
               std::size_t length, unsigned stride,
               std::span<std::uint8_t> destination) noexcept;

// Worker-only conversion, output is opaque top-down BGRA/sRGB. HDR uses
// PQ -> linear BT.2020 -> linear BT.709 -> gamut compression -> max-RGB
// soft-knee (identity <=100 nit max-RGB, shoulder toward 203 nit) -> sRGB.
// Fixed export policy, not scene/mastering metadata or Windows brightness.
// HDR highlights are compressed, not saved as HDR. LUT construction happens
// on the worker, never while holding a driver's capture sample.
bool ConvertRows(const Description& desc, std::span<const std::uint8_t> packed,
                 unsigned firstRow, unsigned rowCount,
                 std::span<std::uint8_t> bgra) noexcept;

} // namespace llcv::screenshot
