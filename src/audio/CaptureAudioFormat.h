#pragma once

#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>

#include <cstdint>
#include <string>

namespace llcv::capture_audio {

enum class Encoding { IntegerPcm, FloatPcm };
enum class Path { Direct16BitStereo, ConvertTo16BitStereo };
enum class Rejection {
    None,
    NotAudio,
    Malformed,
    SampleRate,
    Channels,
    Encoding,
    Bits,
};

// The renderer intentionally stays 48 kHz / 16-bit / stereo. Non-direct
// capture formats are converted at the capture callback into that existing
// ring buffer; no resampler or second queue is introduced here.
struct Format {
    Encoding encoding = Encoding::IntegerPcm;
    Path path = Path::ConvertTo16BitStereo;
    uint16_t containerBits = 0;
    uint16_t validBits = 0;
    uint16_t channels = 0;
    uint16_t blockAlign = 0;
};

struct Classification {
    bool supported = false;
    Format format{};
    Rejection rejection = Rejection::Malformed;
};

Classification Classify(const AM_MEDIA_TYPE& mediaType) noexcept;
int16_t ConvertSample(const BYTE* source, const Format& format) noexcept;
void ConvertFrame(const BYTE* source, const Format& format, int16_t& left,
                  int16_t& right) noexcept;
std::wstring Describe(const Format& format);
std::wstring DescribeRejection(Rejection rejection);

}  // namespace llcv::capture_audio
