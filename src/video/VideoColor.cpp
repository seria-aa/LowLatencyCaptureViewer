#include "video/VideoColor.h"

namespace llcv::video_color {
namespace {

bool DecodeMatrix(unsigned value, Matrix& matrix) {
    if (value == 1) {
        matrix = Matrix::Bt709;
        return true;
    }
    if (value == 2) {
        matrix = Matrix::Bt601;
        return true;
    }
    return false;
}

bool DecodeRange(unsigned value, Range& range) {
    if (value == 1) {
        range = Range::Full;
        return true;
    }
    if (value == 2) {
        range = Range::Limited;
        return true;
    }
    return false;
}

}  // namespace

Configuration Resolve(bool mjpeg, int width, int height,
                      Metadata mediaFoundation,
                      Metadata directShow,
                      Override overrideMode) {
    Configuration result;

    if (DecodeMatrix(mediaFoundation.matrix, result.matrix)) {
        result.matrixSource = Source::MediaFoundation;
    } else if (DecodeMatrix(directShow.matrix, result.matrix)) {
        result.matrixSource = Source::DirectShow;
    } else if (mjpeg) {
        // Match the practical video default used by capture applications:
        // SD JPEG-derived video uses BT.601; HD defaults to BT.709 when the
        // decoder reports no matrix.
        result.matrix = width >= 1280 || height > 576
                            ? Matrix::Bt709 : Matrix::Bt601;
        result.matrixSource = Source::MjpegFallback;
    }

    if (DecodeRange(mediaFoundation.range, result.range)) {
        result.rangeSource = Source::MediaFoundation;
    } else if (DecodeRange(directShow.range, result.range)) {
        result.rangeSource = Source::DirectShow;
    } else if (mjpeg) {
        // FFmpeg/OBS treats a decoded MJPEG frame marked as JPEG range as
        // full-range. Windows' MJPEG MFT does not consistently expose a
        // nominal-range attribute, so use that JPEG convention as fallback.
        result.range = Range::Full;
        result.rangeSource = Source::MjpegFallback;
    }

    if (mjpeg && overrideMode != Override::Auto) {
        switch (overrideMode) {
        case Override::Bt709Full:
            result.matrix = Matrix::Bt709;
            result.range = Range::Full;
            break;
        case Override::Bt709Limited:
            result.matrix = Matrix::Bt709;
            result.range = Range::Limited;
            break;
        case Override::Bt601Full:
            result.matrix = Matrix::Bt601;
            result.range = Range::Full;
            break;
        case Override::Bt601Limited:
            result.matrix = Matrix::Bt601;
            result.range = Range::Limited;
            break;
        default:
            break;
        }
        result.matrixSource = Source::UserOverride;
        result.rangeSource = Source::UserOverride;
    }

    return result;
}

const wchar_t* MatrixName(Matrix matrix) {
    return matrix == Matrix::Bt601 ? L"BT.601" : L"BT.709";
}

const wchar_t* RangeName(Range range) {
    return range == Range::Full ? L"Full range" : L"Limited range";
}

const wchar_t* SourceName(Source source) {
    switch (source) {
    case Source::DirectShow: return L"DirectShow metadata";
    case Source::MediaFoundation: return L"Media Foundation metadata";
    case Source::MjpegFallback: return L"MJPEG fallback";
    case Source::UserOverride: return L"user override";
    default: return L"SDR default";
    }
}

const wchar_t* CompactSourceName(Source matrixSource, Source rangeSource) {
    if (matrixSource != rangeSource) return L"mixed";
    switch (matrixSource) {
    case Source::DirectShow: return L"DirectShow";
    case Source::MediaFoundation: return L"MF";
    case Source::MjpegFallback: return L"fallback";
    case Source::UserOverride: return L"manual";
    default: return L"default";
    }
}

}  // namespace llcv::video_color
