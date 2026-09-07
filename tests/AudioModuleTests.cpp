#include "audio/AudioMix.h"
#include "audio/CaptureAudioFormat.h"
#include "audio/PcmPipeline.h"
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

    struct OverrunObservation {
        size_t dropped = 0;
    } overrun;
    std::atomic<UINT32> publishedFrames{0};
    audio::PcmRing ring(
        3, &publishedFrames,
        [](void* context, size_t dropped) {
            static_cast<OverrunObservation*>(context)->dropped += dropped;
            return true;
        },
        &overrun);
    const int16_t firstFrames[] = {1, -1, 2, -2, 3, -3};
    ring.Push(firstFrames, 3);
    int16_t popped[2]{};
    ok &= Check(ring.Pop(popped, 1) == 1 && popped[0] == 1 &&
                    popped[1] == -1,
                "PCM ring must preserve stereo frame order");
    const int16_t nextFrames[] = {4, -4, 5, -5};
    ring.Push(nextFrames, 2);
    int16_t remaining[6]{};
    ok &= Check(ring.Pop(remaining, 3) == 3 && remaining[0] == 3 &&
                    remaining[2] == 4 && remaining[4] == 5,
                "PCM ring overflow must discard only the oldest frame");
    ok &= Check(overrun.dropped == 1 && ring.Overruns() == 1 &&
                    publishedFrames.load() == 0,
                "PCM ring must publish queue depth and tracked overruns");

    ring.Push(firstFrames, 3);
    ring.Push(firstFrames, 3);
    ok &= Check(overrun.dropped == 4 && ring.Overruns() == 2,
                "full-capacity packet must report displaced queued audio");
    const int16_t oversized[] = {10,-10,11,-11,12,-12,13,-13,14,-14};
    ring.Push(oversized, 5);
    ok &= Check(overrun.dropped == 9 && ring.Overruns() == 3,
                "oversized packet must count both old queue and discarded input prefix");
    ok &= Check(ring.Pop(remaining, 3) == 3 && remaining[0] == 12 &&
                    remaining[2] == 13 && remaining[4] == 14,
                "oversized packet must retain the newest complete frames");
    ring.Push(firstFrames, 3);
    ring.PushConverted(reinterpret_cast<const BYTE*>(oversized), 5, direct.format);
    ok &= Check(overrun.dropped == 14 && ring.Overruns() == 4,
                "converted full packets must use the same overflow accounting");

    audio::PcmRing resampleRing(128);
    int16_t sourceFrames[128]{};
    for (int frame = 0; frame < 64; ++frame) {
        sourceFrames[frame * 2] = static_cast<int16_t>(frame * 100);
        sourceFrames[frame * 2 + 1] =
            static_cast<int16_t>(-frame * 100);
    }
    resampleRing.Push(sourceFrames, 64);
    std::atomic<UINT32> resamplerBuffered{0};
    audio::SincDriftResampler resampler(resampleRing, &resamplerBuffered);
    resampler.Prepare(16);
    int16_t resampled[32]{};
    ok &= Check(resampler.Render(resampled, 16, 1.0) == 16,
                "drift resampler must produce a full unity-ratio block");
    resampler.Reset();
    ok &= Check(resampler.BufferedFrames() == 0 &&
                    resamplerBuffered.load() == 0,
                "drift resampler reset must clear its published queue");

    // Golden outputs captured from the pre-optimization implementation.
    // Exercise differing stereo samples, changing block sizes, +/-1000 ppm,
    // and end-of-input starvation, not just a constant or unity-only signal.
    const double ratios[] = {0.999, 1.0, 1.001};
    const uint64_t hashes[] = {5023897314131432625ull, 4409121206642182641ull,
                               639501298115057817ull};
    const size_t counts[] = {80082, 80000, 79922};
    for (size_t test = 0; test < 3; ++test) {
        audio::PcmRing source(65536);
        audio::SincDriftResampler optimized(source);
        optimized.Prepare(960);
        std::vector<int16_t> noise(40000 * 2);
        uint32_t random = 1234567;
        for (auto& sample : noise) {
            random = random * 1664525u + 1013904223u;
            sample = static_cast<int16_t>(random >> 16);
        }
        source.Push(noise.data(), 40000);
        const size_t sizes[] = {1, 16, 127, 480, 960};
        uint64_t hash = 14695981039346656037ull;
        size_t count = 0;
        for (size_t block = 0; block < 140; ++block) {
            int16_t samples[1920]{};
            const size_t got = optimized.Render(samples, sizes[block % 5], ratios[test]);
            count += got * 2;
            for (size_t i = 0; i < got * 2; ++i) {
                hash ^= static_cast<uint16_t>(samples[i]);
                hash *= 1099511628211ull;
            }
        }
        ok &= Check(hash == hashes[test] && count == counts[test],
                    "optimized resampler must preserve legacy PCM sample output");
    }
    return ok ? 0 : 1;
}
