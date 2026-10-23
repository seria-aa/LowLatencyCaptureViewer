#include "video/VideoColor.h"

#include <cstdlib>
#include <iostream>

namespace {

using llcv::video_color::Configuration;
using llcv::video_color::Matrix;
using llcv::video_color::Metadata;
using llcv::video_color::MatrixOverride;
using llcv::video_color::Range;
using llcv::video_color::RangeOverride;
using llcv::video_color::Source;

void Expect(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    const Configuration raw = llcv::video_color::Resolve(
        false, 1920, 1080, {}, {});
    Expect(raw.matrix == Matrix::Bt709 && raw.range == Range::Limited,
           "uncompressed SDR default remains BT.709 Limited");
    for (unsigned unknown : {0u, 3u, 255u}) {
        const auto fallback = llcv::video_color::Resolve(false, 3840, 2160, {}, {unknown, unknown});
        Expect(fallback == raw, "unknown raw metadata preserves existing SDR default");
    }
    const auto rawPartial = llcv::video_color::Resolve(false, 3840, 2160, {}, {0, 1});
    Expect(rawPartial.matrix == Matrix::Bt709 && rawPartial.matrixSource == Source::Default &&
           rawPartial.range == Range::Full && rawPartial.rangeSource == Source::DirectShow,
           "partial raw metadata changes only the supplied field");

    const Configuration mjpegHd = llcv::video_color::Resolve(
        true, 1920, 1080, {}, {});
    for (unsigned matrix : {1u, 2u}) for (unsigned range : {1u, 2u}) {
        const auto rawMetadata = llcv::video_color::Resolve(false, 3840, 2160, {}, {matrix, range});
        Expect(rawMetadata.matrix == (matrix == 1 ? Matrix::Bt709 : Matrix::Bt601) &&
               rawMetadata.range == (range == 1 ? Range::Full : Range::Limited) &&
               rawMetadata.matrixSource == Source::DirectShow && rawMetadata.rangeSource == Source::DirectShow,
               "raw 4K SDR uses explicit device matrix and range");
    }
    Expect(mjpegHd.matrix == Matrix::Bt709 && mjpegHd.range == Range::Full,
           "metadata-free HD MJPEG uses BT.709 Full fallback");
    Expect(mjpegHd.rangeSource == Source::MjpegFallback,
           "MJPEG range fallback is reported");

    const Configuration mjpegSd = llcv::video_color::Resolve(
        true, 720, 480, {}, {});
    Expect(mjpegSd.matrix == Matrix::Bt601,
           "metadata-free SD MJPEG uses BT.601 matrix fallback");

    const Configuration directShow = llcv::video_color::Resolve(
        true, 1920, 1080, {}, Metadata{2, 2});
    Expect(directShow.matrix == Matrix::Bt601 &&
               directShow.range == Range::Limited,
           "DirectShow metadata overrides MJPEG fallback");
    Expect(directShow.matrixSource == Source::DirectShow &&
               directShow.rangeSource == Source::DirectShow,
           "DirectShow sources are reported");

    const Configuration mediaFoundation = llcv::video_color::Resolve(
        true, 1920, 1080, Metadata{1, 1}, Metadata{2, 2});
    Expect(mediaFoundation.matrix == Matrix::Bt709 &&
               mediaFoundation.range == Range::Full,
           "Media Foundation output metadata has highest priority");
    Expect(mediaFoundation.matrixSource == Source::MediaFoundation &&
               mediaFoundation.rangeSource == Source::MediaFoundation,
           "Media Foundation sources are reported");

    const Configuration partialMetadata = llcv::video_color::Resolve(
        true, 1920, 1080, Metadata{0, 1}, Metadata{2, 0});
    Expect(partialMetadata.matrix == Matrix::Bt601 &&
               partialMetadata.range == Range::Full,
           "matrix and range resolve independently");
    Expect(std::wstring(llcv::video_color::CompactSourceName(
               partialMetadata.matrixSource, partialMetadata.rangeSource)) ==
               L"mixed",
           "mixed metadata sources are compactly identified");
    Expect(std::wstring(llcv::video_color::CompactSourceName(
               mediaFoundation.matrixSource,
               mediaFoundation.rangeSource)) == L"MF",
           "matching Media Foundation sources use the short OSD name");

    const Configuration manual = llcv::video_color::Resolve(
        true, 1920, 1080, Metadata{1, 1}, Metadata{1, 1},
        MatrixOverride::Bt601, RangeOverride::Limited);
    Expect(manual.matrix == Matrix::Bt601 &&
               manual.range == Range::Limited,
           "manual MJPEG setting overrides detected metadata");
    Expect(manual.matrixSource == Source::UserOverride &&
               manual.rangeSource == Source::UserOverride,
           "manual MJPEG setting is reported as the source");

    const Configuration rawIgnoresManual = llcv::video_color::Resolve(
        false, 1920, 1080, {}, {}, MatrixOverride::Bt601);
    Expect(rawIgnoresManual.matrix == Matrix::Bt709 &&
               rawIgnoresManual.range == Range::Limited,
           "MJPEG matrix override cannot alter uncompressed formats");

    // Snapshot the pre-split Auto rules independently of Resolve. Every
    // metadata precedence/fallback combination must retain the same result,
    // including reported source labels, after adding manual range selection.
    unsigned parityCases = 0;
    for (bool mjpeg : {false, true}) for (int width : {720, 1280})
        for (unsigned mfMatrix : {0u, 1u, 2u, 255u})
        for (unsigned mfRange : {0u, 1u, 2u, 255u})
        for (unsigned dsMatrix : {0u, 1u, 2u, 255u})
        for (unsigned dsRange : {0u, 1u, 2u, 255u}) {
            const Metadata mf{mfMatrix, mfRange}, ds{dsMatrix, dsRange};
            Configuration expected;
            const auto valid = [](unsigned value) { return value == 1 || value == 2; };
            if (valid(mfMatrix) || valid(dsMatrix)) {
                expected.matrix = (valid(mfMatrix) ? mfMatrix : dsMatrix) == 1
                                      ? Matrix::Bt709 : Matrix::Bt601;
                expected.matrixSource = valid(mfMatrix) ? Source::MediaFoundation : Source::DirectShow;
            } else if (mjpeg) {
                expected.matrix = width >= 1280 ? Matrix::Bt709 : Matrix::Bt601;
                expected.matrixSource = Source::MjpegFallback;
            }
            if (valid(mfRange) || valid(dsRange)) {
                expected.range = (valid(mfRange) ? mfRange : dsRange) == 1
                                     ? Range::Full : Range::Limited;
                expected.rangeSource = valid(mfRange) ? Source::MediaFoundation : Source::DirectShow;
            } else if (mjpeg) {
                expected.range = Range::Full;
                expected.rangeSource = Source::MjpegFallback;
            }
            const auto automatic = llcv::video_color::Resolve(mjpeg, width, 480, mf, ds);
            Expect(automatic == expected, "Auto exactly preserves pre-split precedence, fallbacks and sources");
            for (auto matrix : {MatrixOverride::Auto, MatrixOverride::Bt709, MatrixOverride::Bt601})
                for (auto range : {RangeOverride::Auto, RangeOverride::Limited, RangeOverride::Full}) {
                    auto manualExpected = expected;
                    if (mjpeg && matrix != MatrixOverride::Auto) {
                        manualExpected.matrix = matrix == MatrixOverride::Bt601 ? Matrix::Bt601 : Matrix::Bt709;
                        manualExpected.matrixSource = Source::UserOverride;
                    }
                    if (range != RangeOverride::Auto) {
                        manualExpected.range = range == RangeOverride::Full ? Range::Full : Range::Limited;
                        manualExpected.rangeSource = Source::UserOverride;
                    }
                    Expect(llcv::video_color::Resolve(mjpeg, width, 480, mf, ds, matrix, range) == manualExpected,
                           "matrix and range controls are independent for raw SDR and MJPEG");
                }
            Expect(llcv::video_color::Resolve(mjpeg, width, 480, mf, ds,
                       static_cast<MatrixOverride>(255), static_cast<RangeOverride>(255)) == automatic,
                   "invalid override values fall back to Auto without false manual source labels");
            ++parityCases;
        }
    Expect(parityCases == 1024, "all Auto metadata parity combinations covered");

    std::cout << "VideoColorTests passed\n";
    return 0;
}
