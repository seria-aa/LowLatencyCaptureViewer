#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace llcv::rife_test {
// Medium was more sensitive to binding changes in paced RTX 3080
// tests. Keep its fixed bindings pending live-playback validation.
constexpr bool VfgInputReuseEnabled(bool medium) {
    return !medium;
}

// A CUDA array address stays the same when D3D11 overwrites its contents.
// Match by capture generation, not by texture pointer or presentation result.
struct VfgInputCache {
    std::array<uint64_t,2> copied{};

    constexpr bool NeedsCopy(std::size_t slot,uint64_t frame) const {
        return frame==0||copied[slot]!=frame;
    }
    constexpr void MarkCopied(std::size_t slot,uint64_t frame) {copied[slot]=frame;}
};
}
