#include "audio/AudioMix.h"
#include "audio/CaptureAudioFormat.h"
#include "ui/AudioOsdLayout.h"

#include <cmath>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace {
bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}
}

int main() {
    using namespace llcv;
    bool ok = true;

    int16_t untouched[] = {1000, -2000};
    audio::StereoGain gain{};
    const auto bypass = audio::ProcessStereoPcm(untouched, 1, gain, gain, false);
    ok &= Check(untouched[0] == 1000 && untouched[1] == -2000,
                "unity mix must preserve samples");
    ok &= Check(!bypass.clipped, "unity mix must not clip");

    int16_t mixed[] = {20000, 20000};
    const auto result = audio::ProcessStereoPcm(
        mixed, 1, gain, {2.0, 0.5}, true);
    ok &= Check(mixed[0] == INT16_MAX && mixed[1] == 10000,
                "per-channel gain must clamp independently");
    ok &= Check(result.clipped && result.peakLeft == INT16_MAX &&
                    result.peakRight == 10000,
                "mix metrics must report output peak and clipping");
    ok &= Check(audio::DecayAndHoldPeak(1000, 0) == 950,
                "peak hold decay must be deterministic");
    ok &= Check(std::abs(audio::PeakToDbfs(32767)) < 0.001,
                "full scale must be 0 dBFS");

    const auto rect = audio_osd::RectForClient(1920);
    ok &= Check(rect.left == 1568 && rect.top == 16,
                "audio OSD must anchor to the top-right client edge");
    ok &= Check(audio_osd::HitTest(1920, 1080, 1600, 120) ==
                    audio_osd::HitTarget::Left,
                "left card must have a large hit region");
    ok &= Check(audio_osd::HitTest(1920, 1080, 1780, 120) ==
                    audio_osd::HitTarget::Right,
                "right card must have a large hit region");
    ok &= Check(audio_osd::HitTest(1920, 1080, 1600, 70) ==
                    audio_osd::HitTarget::Master,
                "master row must be independently selectable");

    WAVEFORMATEX pcm16{};
    pcm16.wFormatTag = WAVE_FORMAT_PCM;
    pcm16.nChannels = 2;
    pcm16.nSamplesPerSec = 48000;
    pcm16.wBitsPerSample = 16;
    pcm16.nBlockAlign = 4;
    pcm16.nAvgBytesPerSec = 192000;
    AM_MEDIA_TYPE pcm16Type{};
    pcm16Type.majortype = MEDIATYPE_Audio;
    pcm16Type.subtype = MEDIASUBTYPE_PCM;
    pcm16Type.formattype = FORMAT_WaveFormatEx;
    pcm16Type.cbFormat = sizeof(pcm16);
    pcm16Type.pbFormat = reinterpret_cast<BYTE*>(&pcm16);
    const auto direct = capture_audio::Classify(pcm16Type);
    ok &= Check(direct.supported && direct.format.path ==
                    capture_audio::Path::Direct16BitStereo,
                "48 kHz 16-bit stereo PCM must retain the direct path");

    WAVEFORMATEX pcm24{};
    pcm24.wFormatTag = WAVE_FORMAT_PCM;
    pcm24.nChannels = 2;
    pcm24.nSamplesPerSec = 48000;
    pcm24.wBitsPerSample = 24;
    pcm24.nBlockAlign = 6;
    pcm24.nAvgBytesPerSec = 288000;
    AM_MEDIA_TYPE pcm24Type = pcm16Type;
    pcm24Type.cbFormat = sizeof(pcm24);
    pcm24Type.pbFormat = reinterpret_cast<BYTE*>(&pcm24);
    const auto converted24 = capture_audio::Classify(pcm24Type);
    const BYTE min24[] = {0x00, 0x00, 0x80};
    const BYTE max24[] = {0xFF, 0xFF, 0x7F};
    ok &= Check(converted24.supported &&
                    converted24.format.path ==
                        capture_audio::Path::ConvertTo16BitStereo &&
                    capture_audio::ConvertSample(min24, converted24.format) ==
                        INT16_MIN &&
                    capture_audio::ConvertSample(max24, converted24.format) ==
                        INT16_MAX,
                "24-bit PCM must convert to full-scale 16-bit safely");

    WAVEFORMATEXTENSIBLE pcm32Valid24{};
    pcm32Valid24.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    pcm32Valid24.Format.cbSize =
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    pcm32Valid24.Format.nChannels = 2;
    pcm32Valid24.Format.nSamplesPerSec = 48000;
    pcm32Valid24.Format.wBitsPerSample = 32;
    pcm32Valid24.Format.nBlockAlign = 8;
    pcm32Valid24.Format.nAvgBytesPerSec = 384000;
    pcm32Valid24.Samples.wValidBitsPerSample = 24;
    pcm32Valid24.SubFormat = MEDIASUBTYPE_PCM;
    AM_MEDIA_TYPE pcm32Valid24Type = pcm16Type;
    pcm32Valid24Type.cbFormat = sizeof(pcm32Valid24);
    pcm32Valid24Type.pbFormat = reinterpret_cast<BYTE*>(&pcm32Valid24);
    const auto converted32 = capture_audio::Classify(pcm32Valid24Type);
    const BYTE max32Valid24[] = {0x00, 0x00, 0xFF, 0x7F};
    ok &= Check(converted32.supported &&
                    capture_audio::ConvertSample(max32Valid24,
                                                  converted32.format) ==
                        INT16_MAX,
                "24 valid bits in a 32-bit PCM container must be MSB-scaled");

    WAVEFORMATEX floatMono{};
    floatMono.wFormatTag = 0x0003;  // WAVE_FORMAT_IEEE_FLOAT
    floatMono.nChannels = 1;
    floatMono.nSamplesPerSec = 48000;
    floatMono.wBitsPerSample = 32;
    floatMono.nBlockAlign = 4;
    floatMono.nAvgBytesPerSec = 192000;
    AM_MEDIA_TYPE floatType = pcm16Type;
    floatType.subtype = MEDIASUBTYPE_IEEE_FLOAT;
    floatType.cbFormat = sizeof(floatMono);
    floatType.pbFormat = reinterpret_cast<BYTE*>(&floatMono);
    const auto floatFormat = capture_audio::Classify(floatType);
    const float half = 0.5f;
    BYTE halfBytes[sizeof(half)]{};
    std::memcpy(halfBytes, &half, sizeof(half));
    int16_t floatLeft = 0;
    int16_t floatRight = 0;
    capture_audio::ConvertFrame(halfBytes, floatFormat.format, floatLeft,
                                floatRight);
    ok &= Check(floatFormat.supported && floatLeft == 16384 &&
                    floatRight == 16384,
                "32-bit float mono must convert and duplicate to stereo");

    pcm16.nSamplesPerSec = 44100;
    pcm16.nAvgBytesPerSec = 176400;
    ok &= Check(capture_audio::Classify(pcm16Type).rejection ==
                    capture_audio::Rejection::SampleRate,
                "non-48 kHz input must remain rejected without resampling");
    return ok ? 0 : 1;
}
