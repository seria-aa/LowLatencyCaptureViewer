#include "video/VideoColor.h"

#include <cstdlib>
#include <iostream>

namespace {

using llcv::video_color::Configuration;
using llcv::video_color::Matrix;
using llcv::video_color::Metadata;
using llcv::video_color::Override;
using llcv::video_color::Range;
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

    const Configuration mjpegHd = llcv::video_color::Resolve(
        true, 1920, 1080, {}, {});
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
        Override::Bt601Limited);
    Expect(manual.matrix == Matrix::Bt601 &&
               manual.range == Range::Limited,
           "manual MJPEG setting overrides detected metadata");
    Expect(manual.matrixSource == Source::UserOverride &&
               manual.rangeSource == Source::UserOverride,
           "manual MJPEG setting is reported as the source");

    const Configuration rawIgnoresManual = llcv::video_color::Resolve(
        false, 1920, 1080, {}, {}, Override::Bt601Full);
    Expect(rawIgnoresManual.matrix == Matrix::Bt709 &&
               rawIgnoresManual.range == Range::Limited,
           "MJPEG override cannot alter uncompressed formats");

    std::cout << "VideoColorTests passed\n";
    return 0;
}
