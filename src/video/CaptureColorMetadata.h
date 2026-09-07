#pragma once

#include <windows.h>

namespace llcv::video {

// DirectShow stores extended color information in the upper 24 bits of
// VIDEOINFOHEADER2::dwControlFlags. Keep the decoded fields independent from
// the capture graph so compressed decoders and renderers can share them.
struct CaptureColorMetadata {
    bool present = false;
    DWORD controlFlags = 0;
    UINT chromaSubsampling = 0;
    UINT nominalRange = 0;
    UINT transferMatrix = 0;
    UINT lighting = 0;
    UINT primaries = 0;
    UINT transferFunction = 0;

    bool hdr10() const {
        return present && primaries == 9 && transferFunction == 15 &&
               (transferMatrix == 4 || transferMatrix == 5);
    }
};

}  // namespace llcv::video
