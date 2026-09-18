#pragma once

#include <windows.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>

#include <cstdint>
#include <string>

namespace llcv::capture_audio {

enum class Encoding { IntegerPcm, FloatPcm };
enum class Path { Direct16BitStereo, ConvertTo16BitStereo, ConvertToSurround51 };
enum class Rejection {
    None,
    NotAudio,
    Malformed,
    SampleRate,
    Channels,
    Encoding,
    Bits,
};

// The default renderer stays 48 kHz / 16-bit / stereo. The opt-in console
// path uses six channels in FL, FR, FC, LFE, SL, SR order, in the same queue.
struct Format {
    Encoding encoding = Encoding::IntegerPcm;
    Path path = Path::ConvertTo16BitStereo;
    uint16_t containerBits = 0;
    uint16_t validBits = 0;
    uint16_t channels = 0;
    uint16_t blockAlign = 0;
    uint32_t channelMask = 0;
};

struct Classification {
    bool supported = false;
    Format format{};
    Rejection rejection = Rejection::Malformed;
};

struct AllocatorInfo {
    HRESULT result = E_FAIL;
    LONG bufferCount = 0;
    LONG bufferBytes = 0;
    LONG framesPerBuffer = 0;
};

Classification Classify(const AM_MEDIA_TYPE& mediaType,
                        bool surround51 = false) noexcept;
AM_MEDIA_TYPE* SelectSupportedType(
    IPin* audioPin, Format& selectedFormat,
    Rejection* rejection = nullptr, bool surround51 = false);
// Finds a pin that actually advertises the requested format (not Audio Mixer
// merely because it is the first audio pin). Returns an owned pin reference.
HRESULT FindSurroundPin(IBaseFilter* filter, IPin** output);
bool MatchesSurroundFormat(const AM_MEDIA_TYPE& mediaType, const Format& expected) noexcept;
HRESULT VerifySurroundConnection(IPin* input, const Format& expected);
HRESULT SuggestCaptureBuffer(IPin* audioPin, WORD blockAlign,
                             int sampleRate, int bufferMs,
                             LONG* suggestedBytes = nullptr) noexcept;
AllocatorInfo QueryConnectedAllocator(IPin* inputPin,
                                      WORD blockAlign) noexcept;
int16_t ConvertSample(const BYTE* source, const Format& format) noexcept;
void ConvertFrame(const BYTE* source, const Format& format, int16_t& left,
                  int16_t& right) noexcept;
void ConvertSurroundFrame(const BYTE* source, const Format& format,
                          int16_t* output) noexcept;
std::wstring Describe(const Format& format);
std::wstring DescribeRejection(Rejection rejection);

}  // namespace llcv::capture_audio
