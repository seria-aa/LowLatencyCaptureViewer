#include "ScreenshotPixels.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace llcv::screenshot {
namespace {
unsigned RowBytes(const Description& d) noexcept {
    switch (d.format) {
    case Format::Nv12: return d.width;
    case Format::Rgb24: return d.width * 3u;
    case Format::Rgb32: return d.width * 4u;
    default: return d.width * 2u;
    }
}
unsigned Rows(const Description& d) noexcept {
    return d.format == Format::Nv12 || d.format == Format::P010 ? d.height + d.height / 2 : d.height;
}
float Word(const std::uint8_t* p) noexcept {
    return static_cast<float>((unsigned(p[0]) | (unsigned(p[1]) << 8)) >> 6);
}
struct HdrTables {
    static constexpr unsigned count = 16384;
    std::array<float, count + 1> pq{};
    std::array<float, count + 1> srgb{};
    HdrTables() noexcept {
        for (unsigned i = 0; i <= count; ++i) {
            const double x = double(i) / count;
            const double p = std::pow(x, 32.0 / 2523.0);
            pq[i] = static_cast<float>(10000.0 * std::pow(
                std::max(p - 3424.0 / 4096.0, 0.0) /
                (2413.0 / 128.0 - 2392.0 / 128.0 * p), 16384.0 / 2610.0));
            srgb[i] = static_cast<float>(x <= 0.0031308 ? 12.92 * x :
                1.055 * std::pow(x, 1.0 / 2.4) - 0.055);
        }
    }
    static float Lookup(const std::array<float, count + 1>& table, float x) noexcept {
        const float at = std::clamp(x, 0.0f, 1.0f) * count;
        const unsigned i = std::min(static_cast<unsigned>(at), count - 1);
        return table[i] + (table[i + 1] - table[i]) * (at - i);
    }
};
void ToneMap(float& r, float& g, float& b) noexcept {
    static const HdrTables table;
    const float r20 = HdrTables::Lookup(table.pq, r);
    const float g20 = HdrTables::Lookup(table.pq, g);
    const float b20 = HdrTables::Lookup(table.pq, b);
    r = 1.660491f*r20 - .587641f*g20 - .072850f*b20;
    g = -.124550f*r20 + 1.132900f*g20 - .008350f*b20;
    b = -.018151f*r20 - .100579f*g20 + 1.118730f*b20;
    // Bring out-of-gamut negative components toward equal-luminance neutral
    // before compressing highlights. Do not clip each channel independently.
    const float low = std::min({r, g, b});
    if (low < 0) {
        const float gray = std::max(0.0f, .2126f*r + .7152f*g + .0722f*b);
        const float chroma = gray / (gray - low);
        r = gray + (r-gray)*chroma;
        g = gray + (g-gray)*chroma;
        b = gray + (b-gray)*chroma;
    }
    // Preserve the lower/mid range instead of applying Reinhard to every
    // pixel. These are export-policy constants, not detected mastering data:
    // 203 nit SDR reference, identity through 100 nit max-RGB, then a rational
    // shoulder. Value and first derivative are continuous at the knee.
    // The shoulder approaches white without clipping at a guessed source peak.
    // A common scale preserves linear RGB ratios after gamut compression.
    constexpr float referenceWhite = 203.0f;
    constexpr float knee = 100.0f;
    constexpr float headroom = referenceWhite - knee;
    const float peak = std::max({r, g, b, 0.0f});
    float scale = 1.0f / referenceWhite;
    if (peak > knee) {
        const float excess = peak - knee;
        const float mapped = knee + headroom * (excess / (headroom + excess));
        scale = mapped / (referenceWhite * peak);
    }
    r = HdrTables::Lookup(table.srgb, r*scale);
    g = HdrTables::Lookup(table.srgb, g*scale);
    b = HdrTables::Lookup(table.srgb, b*scale);
}
std::uint8_t Byte(float v) noexcept {
    return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f)*255.0f + .5f);
}
}

std::size_t PackedSize(const Description& d) noexcept {
    if (!d.width || !d.height || d.width > 3840 || d.height > 2160 ||
        ((d.format != Format::Rgb24 && d.format != Format::Rgb32) &&
         ((d.width & 1) || (d.height & 1))) ||
        (d.format != Format::Nv12 && d.format != Format::Yuy2 &&
         d.format != Format::P010 && d.format != Format::Uyvy &&
         d.format != Format::Rgb24 && d.format != Format::Rgb32))
        return 0;
    return std::size_t(RowBytes(d)) * Rows(d);
}

bool CopyFrame(const Description& d, const std::uint8_t* source,
               std::size_t length, unsigned stride,
               std::span<std::uint8_t> destination) noexcept {
    const auto size = PackedSize(d);
    if (!size || !source || destination.size() < size || stride < RowBytes(d)) return false;
    const auto rows = Rows(d);
    if (rows > 1 && std::size_t(stride) > (std::numeric_limits<std::size_t>::max() - RowBytes(d)) / (rows - 1)) return false;
    const auto required = std::size_t(stride)*(rows-1) + RowBytes(d);
    if (length < required) return false;
    if (stride == RowBytes(d)) std::memcpy(destination.data(), source, size);
    else for (unsigned y = 0; y < rows; ++y)
        std::memcpy(destination.data() + std::size_t(y)*RowBytes(d),
                    source + std::size_t(y)*stride, RowBytes(d));
    return true;
}

bool ConvertRows(const Description& d, std::span<const std::uint8_t> packed,
                 unsigned firstRow, unsigned rowCount,
                 std::span<std::uint8_t> bgra) noexcept {
    const auto size = PackedSize(d);
    if (!size || packed.size() < size || firstRow > d.height ||
        rowCount > d.height-firstRow || bgra.size() < std::size_t(d.width)*rowCount*4) return false;
    const unsigned rowBytes = RowBytes(d);
    if (d.format == Format::Rgb24 || d.format == Format::Rgb32) {
        const unsigned bytesPerPixel = d.format == Format::Rgb24 ? 3u : 4u;
        const bool full = d.color.range == video_color::Range::Full;
        for (unsigned y = firstRow; y < firstRow + rowCount; ++y) {
            const unsigned sourceY = d.topDown ? y : d.height - 1 - y;
            const auto* row = packed.data() + std::size_t(sourceY) * rowBytes;
            auto* out = bgra.data() + std::size_t(y - firstRow) * d.width * 4;
            for (unsigned x = 0; x < d.width; ++x) {
                for (unsigned c = 0; c < 3; ++c) {
                    const float value = float(row[x * bytesPerPixel + c]);
                    out[x * 4 + c] = full ? static_cast<std::uint8_t>(value) : Byte((value - 16.0f) / 219.0f);
                }
                out[x * 4 + 3] = 255;
            }
        }
        return true;
    }
    const bool hdr = d.format == Format::P010;
    const bool planar = d.format != Format::Yuy2 && d.format != Format::Uyvy;
    // UYVY/HDYC retains its original byte order until this existing worker
    // conversion. No extra capture copy or intermediate YUY2 frame is needed.
    const unsigned lumaOffset = d.format == Format::Uyvy ? 1u : 0u;
    const unsigned chromaOffset = d.format == Format::Uyvy ? 0u : 1u;
    const auto* uv = packed.data() + std::size_t(rowBytes)*d.height;
    const auto component = [&](unsigned cx, unsigned cy, unsigned c) {
        if (!planar) return float(packed[std::size_t(cy)*rowBytes + cx*4 + c*2 + chromaOffset]);
        const auto* p = uv + std::size_t(cy)*rowBytes + (cx*2+c)*(hdr ? 2 : 1);
        return hdr ? Word(p) : float(*p);
    };
    const bool full = !hdr && d.color.range == video_color::Range::Full;
    const float kr = hdr ? .2627f : d.color.matrix == video_color::Matrix::Bt601 ? .299f : .2126f;
    const float kb = hdr ? .0593f : d.color.matrix == video_color::Matrix::Bt601 ? .114f : .0722f;
    for (unsigned y = firstRow; y < firstRow + rowCount; ++y) {
        // Bilinear chroma reconstruction with edge clamping. NV12/YUY2/UYVY use
        // left siting; P010 uses the capture path's resolved vertical siting.
        const float fy = planar ? std::max(0.0f, (float(y) - (hdr && d.topLeftChroma ? 0.0f : .5f))*.5f) : float(y);
        const unsigned y0 = static_cast<unsigned>(fy);
        const unsigned y1 = std::min(y0+1, planar ? d.height/2-1 : d.height-1);
        for (unsigned x = 0; x < d.width; ++x) {
            const unsigned x0 = x/2, x1 = std::min(x0+1, d.width/2-1);
            const float tx = (x & 1) * .5f, ty = fy-y0;
            float chroma[2]{};
            for (unsigned c = 0; c < 2; ++c) {
                const float a = std::lerp(component(x0,y0,c), component(x1,y0,c), tx);
                const float b = std::lerp(component(x0,y1,c), component(x1,y1,c), tx);
                chroma[c] = (std::lerp(a,b,ty) - (hdr ? 512.0f : 128.0f)) /
                            (hdr ? 896.0f : full ? 255.0f : 224.0f);
            }
            const auto* p = packed.data() + std::size_t(y)*rowBytes + x*(d.format == Format::Nv12 ? 1 : 2) + lumaOffset;
            const float luma = ((hdr ? Word(p) : float(*p)) - (hdr ? 64.0f : full ? 0.0f : 16.0f)) /
                               (hdr ? 876.0f : full ? 255.0f : 219.0f);
            float r = luma + 2*(1-kr)*chroma[1];
            float b = luma + 2*(1-kb)*chroma[0];
            float g = (luma-kr*r-kb*b)/(1-kr-kb);
            if (hdr) ToneMap(r,g,b);
            auto* out = bgra.data() + (std::size_t(y-firstRow)*d.width+x)*4;
            out[0]=Byte(b); out[1]=Byte(g); out[2]=Byte(r); out[3]=255;
        }
    }
    return true;
}
} // namespace llcv::screenshot
