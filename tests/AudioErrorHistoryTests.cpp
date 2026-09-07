#include "diagnostics/AudioErrorHistory.h"

#include <cstdio>

namespace {
bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}
}

int main() {
    using namespace llcv::diagnostics;
    bool ok = true;
    AudioErrorHistory history(4);
    history.Record({1000, 480, AudioErrorKind::Underrun,
                    AudioErrorCause::PcmDepletion});
    history.Record({1015, 480, AudioErrorKind::Underrun,
                    AudioErrorCause::PcmDepletion});
    history.Record({1040, 480, AudioErrorKind::Underrun,
                    AudioErrorCause::InputLate});
    auto stats = history.Analyze(2000, 480, 48000);
    ok &= Check(stats.recentUnderruns == 3 &&
                    stats.maxConsecutiveUnderruns == 2 &&
                    stats.lastEventMs == 1040,
                "audio history must distinguish bursts from isolated errors");

    history.Record({1050, 480, AudioErrorKind::Overrun,
                    AudioErrorCause::Overrun});
    history.Record({1060, 480, AudioErrorKind::Underrun,
                    AudioErrorCause::Resampler});
    stats = history.Analyze(2000, 480, 48000);
    ok &= Check(history.Size() == 4 && stats.recentEvents == 4 &&
                    stats.maxConsecutiveUnderruns == 1,
                "bounded history must evict oldest entries and reset at overrun");

    stats = history.Analyze(400000, 480, 48000);
    ok &= Check(stats.recentEvents == 0 && stats.lastEventMs == 0,
                "events outside the diagnostic window must be ignored");
    return ok ? 0 : 1;
}
