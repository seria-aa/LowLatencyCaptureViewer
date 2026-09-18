#include "audio/CaptureAudioFormat.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
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

void DeleteMediaType(AM_MEDIA_TYPE*& mediaType) noexcept {
    if (!mediaType) return;
    if (mediaType->cbFormat != 0) {
        CoTaskMemFree(mediaType->pbFormat);
        mediaType->cbFormat = 0;
        mediaType->pbFormat = nullptr;
    }
    if (mediaType->pUnk) {
        mediaType->pUnk->Release();
        mediaType->pUnk = nullptr;
    }
    CoTaskMemFree(mediaType);
    mediaType = nullptr;
}

}  // namespace

Classification Classify(const AM_MEDIA_TYPE& mediaType, bool surround51) noexcept {
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
    if (surround51 ? (wave.nChannels != 6 && wave.nChannels != 8)
                   : (wave.nChannels != 1 && wave.nChannels != 2)) {
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
    DWORD channelMask = 0;
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
        channelMask = extensible.dwChannelMask;
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

    // Do not guess ordering for an unspecified/proprietary multichannel mask.
    if (surround51 && !((wave.nChannels == 6 &&
                         (channelMask == 0x3f || channelMask == 0x60f)) ||
                        (wave.nChannels == 8 && channelMask == 0x63f))) {
        return Reject(Rejection::Channels);
    }
    Classification result{};
    result.supported = true;
    result.rejection = Rejection::None;
    result.format = {encoding,
                     surround51 ? Path::ConvertToSurround51 :
                     encoding == Encoding::IntegerPcm &&
                             wave.wBitsPerSample == 16 && wave.nChannels == 2
                         ? Path::Direct16BitStereo
                         : Path::ConvertTo16BitStereo,
                     wave.wBitsPerSample, validBits, wave.nChannels,
                     wave.nBlockAlign, channelMask};
    return result;
}

AM_MEDIA_TYPE* SelectSupportedType(
    IPin* audioPin, Format& selectedFormat, Rejection* rejection, bool surround51) {
    if (rejection) *rejection = Rejection::Malformed;
    if (!audioPin) return nullptr;

    IEnumMediaTypes* types = nullptr;
    if (FAILED(audioPin->EnumMediaTypes(&types)) || !types) return nullptr;

    AM_MEDIA_TYPE* fallback = nullptr;
    Format fallbackFormat{};
    Rejection firstRejection = Rejection::Malformed;
    AM_MEDIA_TYPE* type = nullptr;
    while (types->Next(1, &type, nullptr) == S_OK) {
        const auto classification = Classify(*type, surround51);
        if (classification.supported) {
            if (classification.format.path == Path::Direct16BitStereo) {
                DeleteMediaType(fallback);
                selectedFormat = classification.format;
                types->Release();
                return type;
            }
            if (!fallback || (surround51 &&
                             classification.format.channels < fallbackFormat.channels)) {
                DeleteMediaType(fallback);
                fallback = type;
                fallbackFormat = classification.format;
                type = nullptr;
            }
        } else if (firstRejection == Rejection::Malformed ||
                   classification.rejection == Rejection::SampleRate) {
            firstRejection = classification.rejection;
        }
        DeleteMediaType(type);
    }
    types->Release();
    if (fallback) {
        selectedFormat = fallbackFormat;
    } else if (rejection) {
        *rejection = firstRejection;
    }
    return fallback;
}

HRESULT FindSurroundPin(IBaseFilter* filter, IPin** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!filter) return E_POINTER;
    IEnumPins* pins = nullptr;
    HRESULT hr = filter->EnumPins(&pins);
    if (FAILED(hr)) return hr;
    IPin* pin = nullptr;
    while (pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION direction{};
        if (SUCCEEDED(pin->QueryDirection(&direction)) && direction == PINDIR_OUTPUT) {
            Format format{};
            AM_MEDIA_TYPE* type = SelectSupportedType(pin, format, nullptr, true);
            if (type) {
                DeleteMediaType(type);
                *output = pin;
                pins->Release();
                return S_OK;
            }
        }
        pin->Release();
    }
    pins->Release();
    return VFW_E_TYPE_NOT_ACCEPTED;
}

bool MatchesSurroundFormat(const AM_MEDIA_TYPE& type, const Format& expected) noexcept {
    const auto result = Classify(type, true);
    const auto& actual = result.format;
    return result.supported && expected.path == Path::ConvertToSurround51 &&
        actual.encoding == expected.encoding && actual.containerBits == expected.containerBits &&
        actual.validBits == expected.validBits && actual.channels == expected.channels &&
        actual.blockAlign == expected.blockAlign && actual.channelMask == expected.channelMask;
}

HRESULT VerifySurroundConnection(IPin* input, const Format& expected) {
    if (!input) return E_POINTER;
    AM_MEDIA_TYPE type{};
    HRESULT hr = input->ConnectionMediaType(&type);
    if (SUCCEEDED(hr) && !MatchesSurroundFormat(type, expected)) hr = VFW_E_TYPE_NOT_ACCEPTED;
    CoTaskMemFree(type.pbFormat);
    if (type.pUnk) type.pUnk->Release();
    return hr;
}

HRESULT SuggestCaptureBuffer(IPin* audioPin, WORD blockAlign,
                             int sampleRate, int bufferMs,
                             LONG* suggestedBytes) noexcept {
    if (suggestedBytes) *suggestedBytes = 0;
    if (!audioPin || blockAlign == 0 || sampleRate <= 0 || bufferMs <= 0) {
        return E_INVALIDARG;
    }

    IAMBufferNegotiation* negotiation = nullptr;
    HRESULT result = audioPin->QueryInterface(IID_PPV_ARGS(&negotiation));
    if (FAILED(result)) return result;

    ALLOCATOR_PROPERTIES properties{};
    properties.cBuffers = 4;
    properties.cbBuffer = (sampleRate * blockAlign * bufferMs) / 1000;
    properties.cbAlign = 1;
    result = negotiation->SuggestAllocatorProperties(&properties);
    negotiation->Release();
    if (suggestedBytes) *suggestedBytes = properties.cbBuffer;
    return result;
}

AllocatorInfo QueryConnectedAllocator(IPin* inputPin,
                                      WORD blockAlign) noexcept {
    AllocatorInfo info{};
    if (!inputPin || blockAlign == 0) {
        info.result = E_INVALIDARG;
        return info;
    }

    IMemInputPin* memoryInput = nullptr;
    IMemAllocator* allocator = nullptr;
    ALLOCATOR_PROPERTIES properties{};
    info.result = inputPin->QueryInterface(IID_PPV_ARGS(&memoryInput));
    if (SUCCEEDED(info.result)) {
        info.result = memoryInput->GetAllocator(&allocator);
    }
    if (SUCCEEDED(info.result)) {
        info.result = allocator->GetProperties(&properties);
    }
    if (SUCCEEDED(info.result)) {
        info.bufferCount = properties.cBuffers;
        info.bufferBytes = properties.cbBuffer;
        info.framesPerBuffer = properties.cbBuffer / blockAlign;
    }
    if (allocator) allocator->Release();
    if (memoryInput) memoryInput->Release();
    return info;
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
    if (format.channels > 2) {
        result += L" / " + std::to_wstring(format.channels) + L" ch -> 5.1";
        wchar_t mask[32]{};
        swprintf_s(mask, L" / mask=0x%lx", static_cast<unsigned long>(format.channelMask));
        result += mask;
        if (format.channels == 8) result += L" / back+side fold (-3 dB each)";
    } else result += format.channels == 1 ? L" / mono" : L" / stereo";
    result += format.path == Path::Direct16BitStereo
        ? L" / direct" : L" / converted";
    return result;
}

std::wstring DescribeRejection(Rejection rejection) {
    switch (rejection) {
    case Rejection::SampleRate:
        return L"only 48 kHz capture audio is supported";
    case Rejection::Channels:
        return L"stereo mode requires mono/stereo; 5.1 requires explicit 5.1 or 7.1 PCM channel layout";
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

void ConvertSurroundFrame(const BYTE* source, const Format& format,
                          int16_t* output) noexcept {
    if (!output) return;
    std::fill_n(output, 6, int16_t{0});
    if (!source || format.path != Path::ConvertToSurround51 ||
        (format.channels != 6 && format.channels != 8)) return;
    const size_t step = format.containerBits / 8;
    for (size_t channel = 0; channel < 6; ++channel)
        output[channel] = ConvertSample(source + channel * step, format);
    // 7.1 back + side pairs are folded into the corresponding 5.1 surround
    // channel at -3 dB each. Never discard the final pair (5.1 may live there).
    // Center and LFE are copied, not mixed into the front speakers.
    if (format.channels == 8) {
        for (size_t channel = 4; channel < 6; ++channel) {
            const double sum = (static_cast<double>(output[channel]) +
                ConvertSample(source + (channel + 2) * step, format)) * 0.7071067811865476;
            output[channel] = static_cast<int16_t>(std::clamp(
                std::lround(sum), static_cast<long>(INT16_MIN), static_cast<long>(INT16_MAX)));
        }
    }
}

}  // namespace llcv::capture_audio
