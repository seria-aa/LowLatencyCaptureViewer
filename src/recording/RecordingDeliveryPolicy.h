#pragma once
#include <cstddef>

namespace llcv::recording {
struct DeliveryPolicy {
    size_t videoPackets, videoBuffers;
    size_t audioPackets=32, audioBuffers=64;
};
// Packet metadata may use the already committed capture-buffer budget. Actual
// buffer leases remain the hard bound, including samples held by the encoder.
// No extra pixel memory or capture-thread allocation is needed for short bursts.
inline constexpr DeliveryPolicy RecordingDeliveryPolicy(bool mjpeg) noexcept {
    return mjpeg ? DeliveryPolicy{8,8} : DeliveryPolicy{16,16};
}
}
