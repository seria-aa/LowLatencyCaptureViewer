#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace llcv::diagnostics {

enum class AudioErrorKind { Underrun, Overrun };
enum class AudioErrorCause { InputLate, PcmDepletion, Resampler, Overrun };

struct AudioErrorEvent {
    uint64_t timestampMs = 0;
    uint32_t frames = 0;
    AudioErrorKind kind = AudioErrorKind::Underrun;
    AudioErrorCause cause = AudioErrorCause::PcmDepletion;
};

struct AudioPatternStats {
    size_t recentEvents = 0;
    size_t recentUnderruns = 0;
    size_t maxConsecutiveUnderruns = 0;
    uint64_t lastEventMs = 0;
};

class AudioErrorHistory {
public:
    explicit AudioErrorHistory(size_t capacity = 128) noexcept;

    void Record(const AudioErrorEvent& event);
    AudioPatternStats Analyze(uint64_t nowMs, uint32_t packetFrames,
                              uint32_t sampleRate,
                              uint64_t windowMs = 5 * 60 * 1000) const;
    size_t Size() const;

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<AudioErrorEvent> events_;
};

}  // namespace llcv::diagnostics
