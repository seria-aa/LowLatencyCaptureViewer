#pragma once

#include <cstdint>

namespace llcv::video_color {

enum class Matrix : std::uint8_t {
    Bt601,
    Bt709,
};

enum class Range : std::uint8_t {
    Limited,
    Full,
};

enum class Source : std::uint8_t {
    Default,
    DirectShow,
    MediaFoundation,
    MjpegFallback,
    UserOverride,
};

enum class Override : std::uint8_t {
    Auto,
    Bt709Full,
    Bt709Limited,
    Bt601Full,
    Bt601Limited,
};

struct Metadata {
    unsigned matrix = 0;
    unsigned range = 0;
};

struct Configuration {
    Matrix matrix = Matrix::Bt709;
    Range range = Range::Limited;
    Source matrixSource = Source::Default;
    Source rangeSource = Source::Default;

    bool operator==(const Configuration&) const = default;
};

// DirectShow's DXVA extended-color values and Media Foundation's equivalent
// enums intentionally use the same values for the SDR matrix/range entries:
// matrix 1=BT.709, 2=BT.601; range 1=0-255, 2=16-235.
Configuration Resolve(bool mjpeg, int width, int height,
                      Metadata mediaFoundation,
                      Metadata directShow,
                      Override overrideMode = Override::Auto);

const wchar_t* MatrixName(Matrix matrix);
const wchar_t* RangeName(Range range);
const wchar_t* SourceName(Source source);
const wchar_t* CompactSourceName(Source matrixSource, Source rangeSource);

}  // namespace llcv::video_color
