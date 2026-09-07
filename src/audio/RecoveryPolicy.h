#pragma once

#include <cstdint>

namespace llcv::audio {
// Owner-thread policy: never spin or retry indefinitely on a broken driver.
// Only explicit evidence of a successfully running session restores the small
// retry budget. Time spent in a failed device open, teardown or backoff does
// not count as healthy operation, however long a driver takes to return.
class RecoveryPolicy {
public:
    void ObserveSuccessfulRuntime(uint64_t runtimeMilliseconds) {
        if (runtimeMilliseconds >= 30000) attempts_ = 0;
    }
    unsigned NextDelay() {
        if (attempts_ >= 3) return 0;
        return 250u << attempts_++;
    }
private:
    unsigned attempts_ = 0;
};
}  // namespace llcv::audio
