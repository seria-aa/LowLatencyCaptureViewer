#include "audio/CaptureAudioFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace llcv::capture_audio {
namespace {

constexpr WORD kWaveFormatIeeeFloat = 0x0003;
constexpr DWORD kExpectedSampleRate = 48'000;

bool SupportedIntegerBits(WORD bits) noexcept {
    return bits == 16 || bits == 24 || bits == 32;
}

Classification Reject(Rejection rejection) noexcept {
    Classification result{};
    result.rejection = rejection;
    return result;
}

int32_t ReadInteger(const BYTE* source, uint16_t containerBits) noexcept {
    if (containerBits == 16) {
        const uint16_t raw = static_cast<uint16_t>(source[0]) |
            (static_cast<uint16_t>(source[1]) << 8);
        return static_cast<int16_t>(raw);
    }
    if (containerBits == 24) {
        int32_t raw = static_cast<int32_t>(source[0]) |
            (static_cast<int32_t>(source[1]) << 8) |
            (static_cast<int32_t>(source[2]) << 16);
        if ((raw & 0x00800000) != 0) raw |= ~0x00FFFFFF;
        return raw;
    }
    int32_t raw = 0;
    std::memcpy(&raw, source, sizeof(raw));
    return raw;
}

}  // namespace

Classification Classify(const AM_MEDIA_TYPE& mediaType) noexcept {
    if (mediaType.majortype != MEDIATYPE_Audio ||
        mediaType.formattype != FORMAT_WaveFormatEx || !mediaType.pbFormat ||
        mediaType.cbFormat < sizeof(WAVEFORMATEX)) {
        return Reject(Rejection::NotAudio);
    }

    const auto& wave = *reinterpret_cast<const WAVEFORMATEX*>(
        mediaType.pbFormat);
    if (wave.nSamplesPerSec != kExpectedSampleRate) {
        return Reject(Rejection::SampleRate);
    }
    if (wave.nChannels != 1 && wave.nChannels != 2) {
        return Reject(Rejection::Channels);
    }
    const WORD sampleBytes = static_cast<WORD>((wave.wBitsPerSample + 7) / 8);
    if (wave.wBitsPerSample == 0 ||
        wave.nBlockAlign != wave.nChannels * sampleBytes ||
        wave.nAvgBytesPerSec != wave.nSamplesPerSec * wave.nBlockAlign) {
        return Reject(Rejection::Malformed);
    }

    Encoding encoding{};
    WORD validBits = wave.wBitsPerSample;
    if (wave.wFormatTag == WAVE_FORMAT_PCM &&
        mediaType.subtype == MEDIASUBTYPE_PCM) {
        encoding = Encoding::IntegerPcm;
    } else if (wave.wFormatTag == kWaveFormatIeeeFloat &&
               mediaType.subtype == MEDIASUBTYPE_IEEE_FLOAT) {
        encoding = Encoding::FloatPcm;
    } else if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               wave.cbSize >=
                   sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) &&
               mediaType.cbFormat >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const auto& extensible = *reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(
            mediaType.pbFormat);
        if (extensible.SubFormat == MEDIASUBTYPE_PCM &&
            mediaType.subtype == MEDIASUBTYPE_PCM) {
            encoding = Encoding::IntegerPcm;
        } else if (extensible.SubFormat == MEDIASUBTYPE_IEEE_FLOAT &&
                   mediaType.subtype == MEDIASUBTYPE_IEEE_FLOAT) {
            encoding = Encoding::FloatPcm;
        } else {
            return Reject(Rejection::Encoding);
        }
        validBits = extensible.Samples.wValidBitsPerSample;
    } else {
        return Reject(Rejection::Encoding);
    }

    if ((encoding == Encoding::IntegerPcm &&
         (!SupportedIntegerBits(wave.wBitsPerSample) ||
          !SupportedIntegerBits(validBits) || validBits > wave.wBitsPerSample)) ||
        (encoding == Encoding::FloatPcm &&
         (wave.wBitsPerSample != 32 || validBits != 32))) {
        return Reject(Rejection::Bits);
    }

    Classification result{};
    result.supported = true;
    result.rejection = Rejection::None;
    result.format = {encoding,
                     encoding == Encoding::IntegerPcm &&
                             wave.wBitsPerSample == 16 && wave.nChannels == 2
                         ? Path::Direct16BitStereo
                         : Path::ConvertTo16BitStereo,
                     wave.wBitsPerSample, validBits, wave.nChannels,
                     wave.nBlockAlign};
    return result;
}

int16_t ConvertSample(const BYTE* source, const Format& format) noexcept {
    if (!source) return 0;
    if (format.encoding == Encoding::FloatPcm) {
        float value = 0.0f;
        std::memcpy(&value, source, sizeof(value));
        if (!std::isfinite(value)) return 0;
        if (value >= 1.0f) return std::numeric_limits<int16_t>::max();
        if (value <= -1.0f) return std::numeric_limits<int16_t>::min();
        return static_cast<int16_t>(value * 32768.0f);
    }

    int32_t value = ReadInteger(source, format.containerBits);
    // WAVEFORMATEXTENSIBLE PCM valid bits are MSB-aligned in their container.
    // Scale the container to the 16-bit renderer representation with an
    // arithmetic shift; the valid-bit count is checked by Classify().
    const int shift = static_cast<int>(format.containerBits) - 16;
    if (shift > 0) value >>= shift;
    value = std::clamp(value, static_cast<int32_t>(INT16_MIN),
                       static_cast<int32_t>(INT16_MAX));
    return static_cast<int16_t>(value);
}

void ConvertFrame(const BYTE* source, const Format& format, int16_t& left,
                  int16_t& right) noexcept {
    left = ConvertSample(source, format);
    right = format.channels == 1
        ? left
        : ConvertSample(source + format.containerBits / 8, format);
}

std::wstring Describe(const Format& format) {
    std::wstring result = L"48 kHz / ";
    if (format.encoding == Encoding::FloatPcm) {
        result += L"32-bit float";
    } else {
        result += std::to_wstring(format.validBits);
        result += L"-bit PCM";
        if (format.validBits != format.containerBits) {
            result += L" (" + std::to_wstring(format.containerBits) +
                      L"-bit container)";
        }
    }
    result += format.channels == 1 ? L" / mono" : L" / stereo";
    result += format.path == Path::Direct16BitStereo
        ? L" / direct" : L" / converted";
    return result;
}

std::wstring DescribeRejection(Rejection rejection) {
    switch (rejection) {
    case Rejection::SampleRate:
        return L"only 48 kHz capture audio is supported";
    case Rejection::Channels:
        return L"only mono or stereo capture audio is supported";
    case Rejection::Encoding:
        return L"only uncompressed PCM or 32-bit float is supported";
    case Rejection::Bits:
        return L"supported input is 16/24/32-bit PCM or 32-bit float";
    case Rejection::Malformed:
        return L"the device reported an invalid audio format";
    case Rejection::NotAudio:
        return L"the selected pin is not a WaveFormatEx audio stream";
    case Rejection::None:
        break;
    }
    return L"unsupported capture audio format";
}

}  // namespace llcv::capture_audio
