#include "audio/CaptureAudioFormat.h"
#include "audio/PcmPipeline.h"
#include "audio/AudioMix.h"
#include "capture/AudioSampleGrabber.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <cmath>
#include <thread>
#include <stdexcept>

using namespace llcv;
static void Check(bool ok, const char* text) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", text); std::abort(); }
}
struct Media {
    WAVEFORMATEXTENSIBLE wave{};
    AM_MEDIA_TYPE type{};
    Media(WORD channels, DWORD mask, WORD bits = 16, bool floating = false) {
        wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wave.Format.cbSize = sizeof(wave) - sizeof(WAVEFORMATEX);
        wave.Format.nSamplesPerSec = 48000;
        wave.Format.nChannels = channels;
        wave.Format.wBitsPerSample = bits;
        wave.Format.nBlockAlign = channels * (bits / 8);
        wave.Format.nAvgBytesPerSec = 48000 * wave.Format.nBlockAlign;
        wave.Samples.wValidBitsPerSample = bits;
        wave.dwChannelMask = mask;
        wave.SubFormat = floating ? MEDIASUBTYPE_IEEE_FLOAT : MEDIASUBTYPE_PCM;
        type.majortype = MEDIATYPE_Audio;
        type.subtype = wave.SubFormat;
        type.formattype = FORMAT_WaveFormatEx;
        type.cbFormat = sizeof(wave);
        type.pbFormat = reinterpret_cast<BYTE*>(&wave);
    }
};

static void FormatsAndMapping() {
    unsigned cases = 0;
    for (WORD channels = 1; channels <= 10; ++channels) {
        for (DWORD mask = 0; mask < 2048; ++mask) {
            Media media(channels, mask);
            const bool expected = (channels == 6 && (mask == 0x3f || mask == 0x60f)) ||
                (channels == 8 && mask == 0x63f);
            Check(capture_audio::Classify(media.type, true).supported == expected,
                "only explicit 5.1/7.1 speaker masks are accepted");
            Check(capture_audio::Classify(media.type).supported == (channels <= 2),
                "stereo default must not select multichannel");
            ++cases;
        }
    }
    for (WORD channels : {WORD{6}, WORD{8}}) for (WORD bits : {WORD{16}, WORD{24}, WORD{32}}) {
        Media media(channels, channels == 6 ? 0x60f : 0x63f, bits);
        auto result = capture_audio::Classify(media.type, true);
        Check(result.supported, "integer widths supported");
        for (size_t sourceChannel = 0; sourceChannel < channels; ++sourceChannel) {
            std::array<BYTE, 32> bytes{};
            // 10000 in the upper 16 bits of each signed PCM container.
            const size_t index = sourceChannel * (bits / 8) + bits / 8 - 2;
            bytes[index] = 0x10; bytes[index + 1] = 0x27;
            std::array<int16_t, 8> output{};
            output[0] = 1234; output[7] = 5678;
            capture_audio::ConvertSurroundFrame(bytes.data(), result.format, output.data() + 1);
            Check(output[0] == 1234 && output[7] == 5678, "six-channel conversion bounds");
            const size_t destination = sourceChannel >= 6 ? sourceChannel - 2 : sourceChannel;
            for (size_t c = 0; c < 6; ++c) Check(output[c + 1] ==
                (c == destination ? (channels == 8 && c >= 4 ? 7071 : 10000) : 0),
                "each source channel reaches only its designated speaker");
        }
        media.wave.Format.nSamplesPerSec = 44100;
        Check(!capture_audio::Classify(media.type, true).supported, "reject changed sample rate");
        media.wave.Format.nSamplesPerSec = 48000;
        media.wave.Format.nBlockAlign += 1;
        Check(!capture_audio::Classify(media.type, true).supported, "reject malformed frame stride");
        media.wave.Format.nBlockAlign -= 1;
        media.type.cbFormat = sizeof(WAVEFORMATEX);
        Check(!capture_audio::Classify(media.type, true).supported, "reject truncated extension");
    }
    Media floatMedia(6, 0x3f, 32, true);
    const auto fp = capture_audio::Classify(floatMedia.type, true);
    Check(fp.supported, "float multichannel accepted");
    const float samples[] = {0.5f, -0.5f, NAN, INFINITY, 2.0f, -2.0f};
    int16_t out[6]{};
    capture_audio::ConvertSurroundFrame(reinterpret_cast<const BYTE*>(samples), fp.format, out);
    Check(out[0] == 16384 && out[1] == -16384 && out[2] == 0 && out[3] == 0 &&
          out[4] == INT16_MAX && out[5] == INT16_MIN, "float finite conversion/clamp");
    Media eight(8, 0x63f);
    const auto fmt = capture_audio::Classify(eight.type, true).format;
    const int16_t maximum[] = {1, 2, 3, 4, INT16_MAX, INT16_MIN, INT16_MAX, INT16_MIN};
    capture_audio::ConvertSurroundFrame(reinterpret_cast<const BYTE*>(maximum), fmt, out);
    Check(out[2] == 3 && out[3] == 4 && out[4] == INT16_MAX && out[5] == INT16_MIN,
        "7.1 fold saturates without wrap or center/LFE contamination");
    std::printf("Format/mask cases: %u; all channel impulses and sample widths passed.\n", cases);
}

static void RingAndResampler() {
    audio::PcmRing ring(31);
    ring.ConfigureChannels(6);
    std::deque<std::array<int16_t, 6>> reference;
    std::mt19937 random(192573);
    for (int iteration = 0; iteration < 10000; ++iteration) {
        const size_t frames = random() % 80;
        std::vector<int16_t> input(frames * 6);
        for (auto& sample : input) sample = static_cast<int16_t>(random());
        ring.Push(input.data(), frames);
        for (size_t f = 0; f < frames; ++f) {
            std::array<int16_t, 6> frame{};
            std::copy_n(input.data() + f * 6, 6, frame.data());
            reference.push_back(frame);
        }
        while (reference.size() > 31) reference.pop_front();
        const size_t wanted = random() % 50;
        std::vector<int16_t> output(wanted * 6 + 2, 12345);
        const size_t received = ring.Pop(output.data() + 1, wanted);
        Check(received == (std::min)(wanted, reference.size()), "frame count independent of channels");
        for (size_t f = 0; f < received; ++f) {
            for (size_t c = 0; c < 6; ++c)
                Check(output[1 + f * 6 + c] == reference.front()[c], "ring wrap/overrun preserves all channels");
            reference.pop_front();
        }
        Check(output.front() == 12345 && output[1 + received * 6] == 12345, "pop boundaries");
    }
    for (double ratio : {0.999, 1.0, 1.001}) {
        audio::PcmRing multi(65536);
        multi.ConfigureChannels(6);
        audio::SincDriftResampler six(multi);
        six.Prepare(960);
        std::vector<int16_t> noise(24000 * 6);
        for (auto& value : noise) value = static_cast<int16_t>(random());
        multi.Push(noise.data(), 24000);
        audio::PcmRing pair0(65536), pair1(65536), pair2(65536);
        audio::PcmRing* rings[]{&pair0, &pair1, &pair2};
        audio::SincDriftResampler r0(pair0), r1(pair1), r2(pair2);
        audio::SincDriftResampler* resamplers[]{&r0, &r1, &r2};
        for (size_t p = 0; p < 3; ++p) {
            std::vector<int16_t> pair(24000 * 2);
            for (size_t f = 0; f < 24000; ++f) for (size_t c = 0; c < 2; ++c)
                pair[f * 2 + c] = noise[f * 6 + p * 2 + c];
            rings[p]->Push(pair.data(), 24000);
            resamplers[p]->Prepare(960);
        }
        for (size_t block = 0; block < 150; ++block) {
            const size_t sizes[]{1, 16, 127, 480, 960};
            const size_t frames = sizes[block % 5];
            std::vector<int16_t> out(frames * 6 + 1, 12345);
            const size_t got = six.Render(out.data(), frames, ratio);
            Check(out[got * 6] == 12345, "resampler bounds including starvation");
            for (size_t p = 0; p < 3; ++p) {
                std::vector<int16_t> stereo(frames * 2);
                Check(resamplers[p]->Render(stereo.data(), frames, ratio) == got,
                    "all channels share exactly the same clock");
                for (size_t f = 0; f < got; ++f) for (size_t c = 0; c < 2; ++c)
                    Check(stereo[f * 2 + c] == out[f * 6 + p * 2 + c],
                        "six-channel correction matches independent stereo reference bit-for-bit");
            }
        }
    }
    std::puts("Ring: 10000 randomized wraps/overruns; resampler +/-1000 ppm/channel isolation passed.");
}

static void Gain() {
    int16_t samples[]{1000, 2000, 3000, 4000, 5000, 6000};
    std::array<double, 6> gains{1, 1, 1, 1, 1, 1};
    audio::ProcessSurroundPcm(samples, 1, gains, 1.0, {1, 1}, false);
    for (int c = 0; c < 6; ++c) Check(samples[c] == 1000 * (c + 1), "unity exact bypass");
    const auto metrics = audio::ProcessSurroundPcm(samples, 1, gains, 1.0, {0.5, 0.25}, true);
    Check(samples[0] == 500 && samples[1] == 500 && samples[2] == 3000 &&
          samples[3] == 4000 && samples[4] == 2500 && samples[5] == 1500,
          "side controls preserve center/LFE while master controls everything");
    Check(metrics.peakLeft == 4000 && metrics.peakRight == 4000, "meters include center/LFE");
    audio::ProcessSurroundPcm(samples, 1, gains, 0, {1, 1}, false);
    for (auto value : samples) Check(value == 0, "mute includes every channel");
}

static void SessionAndConcurrency() {
    std::atomic<UINT32> published{0};
    audio::PcmRing ring(257, &published);
    for (unsigned session = 0; session < 64; ++session) {
        const size_t channels = session % 2 ? 2 : 6;
        ring.ConfigureChannels(channels);
        Check(ring.Channels() == channels && published == 0 && ring.AvailableFrames() == 0,
            "session transition clears queue and publishes zero before threads start");
        audio::SincDriftResampler resampler(ring);
        resampler.Prepare(32);
        std::vector<int16_t> input(128 * channels, 1000);
        ring.Push(input.data(), 128);
        std::vector<int16_t> output(32 * channels + 1, 12345);
        Check(resampler.Render(output.data(), 32, 1.0) == 32 && output.back() == 12345,
            "new session resampler uses the new channel width");
        ring.Clear(); resampler.Reset();
        Check(published == 0 && resampler.BufferedFrames() == 0,
            "endpoint recovery resets ring and resampler history");
    }
    bool rejected = false;
    try { ring.ConfigureChannels(8); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected && ring.Channels() == 2, "unsupported queue width cannot mutate active configuration");
    for (size_t channels : {size_t{2}, size_t{6}}) {
        ring.ConfigureChannels(channels);
        std::atomic<bool> done{false};
        std::thread producer([&] {
            for (int f = 1; f <= 20000; ++f) {
                int16_t frame[6]{};
                for (size_t c = 0; c < channels; ++c) frame[c] = static_cast<int16_t>(f + c * 1000);
                ring.Push(frame, 1);
                if (f % 113 == 0) std::this_thread::yield();
            }
            done.store(true, std::memory_order_release);
        });
        int last = 0;
        size_t blocks = 0;
        while (!done.load(std::memory_order_acquire) || ring.AvailableFrames()) {
            std::array<int16_t, 31 * 6 + 1> output{};
            output.fill(30000);
            const size_t got = ring.Pop(output.data(), 31);
            Check(output[got * channels] == 30000, "concurrent pop bounds");
            for (size_t f = 0; f < got; ++f) {
                const int first = output[f * channels];
                Check(first > last, "concurrent overflow/reset preserves frame order");
                last = first;
                for (size_t c = 0; c < channels; ++c)
                    Check(output[f * channels + c] == first + c * 1000,
                        "concurrent producer/consumer cannot tear speaker channels");
            }
            if (++blocks % 17 == 0) ring.Clear(); // endpoint-switch discard while capture runs
            if (!got) std::this_thread::yield();
        }
        producer.join();
        Check(published == 0, "concurrent queue drains completely");
    }
    std::puts("64 stereo/5.1 sessions and concurrent capture/render/endpoint-clear passed.");
}

class AudioSample final : public IMediaSample {
public:
    std::array<int16_t, 8> samples{1000,2000,3000,4000,5000,6000,7000,8000};
    Media* changed = nullptr;
    long bytes = 16;
    ULONG refs = 1;
    STDMETHODIMP QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override { return --refs; }
    STDMETHODIMP GetPointer(BYTE** p) override { *p = reinterpret_cast<BYTE*>(samples.data()); return S_OK; }
    STDMETHODIMP_(long) GetSize() override { return sizeof(samples); }
    STDMETHODIMP GetTime(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    STDMETHODIMP SetTime(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    STDMETHODIMP IsSyncPoint() override { return S_OK; }
    STDMETHODIMP SetSyncPoint(BOOL) override { return S_OK; }
    STDMETHODIMP IsPreroll() override { return S_FALSE; }
    STDMETHODIMP SetPreroll(BOOL) override { return S_OK; }
    STDMETHODIMP_(long) GetActualDataLength() override { return bytes; }
    STDMETHODIMP SetActualDataLength(long n) override { bytes = n; return S_OK; }
    STDMETHODIMP GetMediaType(AM_MEDIA_TYPE** p) override {
        *p = nullptr;
        if (!changed) return S_FALSE;
        auto* type = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        if (!type) return E_OUTOFMEMORY;
        *type = changed->type;
        type->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(type->cbFormat));
        if (!type->pbFormat) { CoTaskMemFree(type); return E_OUTOFMEMORY; }
        std::memcpy(type->pbFormat, changed->type.pbFormat, type->cbFormat);
        *p = type; return S_OK;
    }
    STDMETHODIMP SetMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP IsDiscontinuity() override { return S_FALSE; }
    STDMETHODIMP SetDiscontinuity(BOOL) override { return S_OK; }
    STDMETHODIMP GetMediaTime(LONGLONG*, LONGLONG*) override { return E_NOTIMPL; }
    STDMETHODIMP SetMediaTime(LONGLONG*, LONGLONG*) override { return E_NOTIMPL; }
};

static void CaptureCallback() {
    Media eight(8, 0x63f), six(6, 0x60f);
    const auto format = capture_audio::Classify(eight.type, true).format;
    audio::PcmRing ring(100);
    ring.ConfigureChannels(6);
    std::atomic<UINT32> packet{0};
    std::atomic<bool> rejected{false};
    capture::AudioSampleTelemetry telemetry{};
    telemetry.packetFrames = &packet;
    telemetry.surroundFormatRejected = &rejected;
    auto* callback = new capture::AudioSampleGrabberCallback(format, ring, telemetry);
    AudioSample sample;
    Check(callback->SampleCB(0, &sample) == S_OK && ring.AvailableFrames() == 1 && packet == 1,
        "capture callback interprets 16 bytes as one 8-channel frame, not four stereo frames");
    int16_t output[6]{};
    ring.Pop(output, 1);
    Check(output[0] == 1000 && output[2] == 3000 && output[3] == 4000 &&
          output[4] == 8485 && output[5] == 9899, "production capture downmix routing");
    sample.bytes = 15;
    Check(callback->SampleCB(0, &sample) == S_OK && ring.AvailableFrames() == 0,
        "partial multichannel packet rejected without corrupting ring");
    sample.bytes = 32;
    Check(callback->SampleCB(0, &sample) == E_INVALIDARG && ring.AvailableFrames() == 0,
        "driver length larger than allocated sample is rejected before reading");
    sample.bytes = 16; sample.changed = &eight;
    Check(callback->SampleCB(0, &sample) == S_OK, "matching repeated media type accepted");
    ring.Clear(); sample.changed = &six;
    Check(callback->SampleCB(0, &sample) == VFW_E_TYPE_NOT_ACCEPTED && ring.AvailableFrames() == 0,
        "runtime channel count change cannot be interpreted with old stride");
    Check(rejected.load(), "format change notifies capture owner instead of failing silently");
    sample.changed = nullptr;
    Check(callback->SampleCB(0, &sample) == VFW_E_TYPE_NOT_ACCEPTED,
        "rejected stream cannot silently resume with unlabelled packets");
    callback->Release();
    std::puts("Real capture callback: 8ch conversion, partial packet and dynamic format guard passed.");
}

int main() {
    FormatsAndMapping(); RingAndResampler(); Gain(); CaptureCallback(); SessionAndConcurrency();
    std::puts("Surround audio tests passed (no capture or playback hardware used).");
}
