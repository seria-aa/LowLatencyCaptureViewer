#include "audio/AudioDeviceCapabilities.h"

#include <cstdio>

int main() {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* message) {
        if (condition) return;
        std::fprintf(stderr, "FAILED: %s\n", message);
        ++failures;
    };

    const WAVEFORMATEX format = llcv::audio_device::PcmOutputFormat();
    check(format.wFormatTag == WAVE_FORMAT_PCM, "PCM output format tag");
    check(format.nSamplesPerSec == 48'000, "PCM output sample rate");
    check(format.nChannels == 2, "PCM output channel count");
    check(format.wBitsPerSample == 16, "PCM output bit depth");
    check(format.nBlockAlign == 4, "PCM output block alignment");
    check(format.nAvgBytesPerSec == 192'000,
          "PCM output average byte rate");

    llcv::audio_device::SharedModeSupport support{};
    support.supported = true;
    support.minimumFrames = 96;
    support.maximumFrames = 480;
    support.fundamentalFrames = 48;
    check(llcv::audio_device::ClosestSupportedSharedPeriod(200, support) ==
              192,
          "Shared period rounds to the nearest fundamental multiple");
    check(llcv::audio_device::ClosestSupportedSharedPeriod(1, support) == 96,
          "Shared period clamps to the endpoint minimum");
    check(llcv::audio_device::ClosestSupportedSharedPeriod(900, support) ==
              480,
          "Shared period clamps to the endpoint maximum");

    support.supported = false;
    check(llcv::audio_device::ClosestSupportedSharedPeriod(200, support) == 0,
          "Unsupported Shared endpoint returns no period");

    for (const int bufferMs : {5, 10, 15, 20, 30, 40}) {
        check(llcv::audio_device::IsExclusiveLowLatencyBuffer(bufferMs),
              "Known Exclusive buffer is accepted");
    }
    for (const int bufferMs : {0, 25, 50}) {
        check(!llcv::audio_device::IsExclusiveLowLatencyBuffer(bufferMs),
              "Unknown Exclusive buffer is rejected");
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
    std::puts("Audio device capability tests passed.");
    return 0;
}
