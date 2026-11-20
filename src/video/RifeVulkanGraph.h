#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <vector>

// The Win32 bridge uses the full Vulkan headers; the ncnn graph uses ncnn's
// simplevk declarations. Only native handles cross this boundary.
namespace llcv::rife_vulkan {
struct NativeBuffer {
    uintptr_t handle;
    size_t offset, bytes;
    int planeStride;
};
struct NativeGraph {
    void *instance, *physical, *device;
    uint32_t computeFamily;
    int estimateWidth, estimateHeight;
    NativeBuffer full, low, endpoint, output;
    NativeBuffer hud; // Detector state (fp32); handle 0 when HUD protection is off.
};
// HUD detector state layout in floats, at 960x540 (n = kHudPlane).
inline constexpr int kHudWidth = 960, kHudHeight = 540, kHudPlane = kHudWidth * kHudHeight;
inline constexpr int kHudWeight = 10 * kHudPlane;           // protection weight plane
inline constexpr int kHudFloats = 29 * kHudPlane + 16;      // planes, age and count slots
inline constexpr int kHudChromaFloats = kHudFloats + 4 * kHudPlane;
enum class Preset { Fast, Half };
enum class JobKind { Generate, HistoryOnly };
using MatchDevice = bool (*)(void*, void*, void*);
using ExtendDevice = int (*)(void*, uintptr_t, void*, const void*, const void*, void*);
using WrappedSubmit = int (*)(void*, uintptr_t, void*, uint32_t, const void*, uintptr_t);
using WrappedWait = int (*)(void*, uintptr_t, void*, uint32_t, const void*, uint32_t, uint64_t);

class FastGraph {
public:
    virtual ~FastGraph() = default;
    virtual NativeGraph Native() const = 0;
    virtual std::vector<uint32_t> Compile(const char* source) const = 0;
    virtual void Record() = 0;
    // The bridge tracks submission and places a bound on the worker's fence wait.
    virtual double Execute(JobKind, WrappedSubmit, WrappedWait, void* cookie) = 0;
};

// One playback owner at a time. Each graph owns its logical Vulkan device;
// a retry never reuses a device left in the ncnn cache by a failed startup.
// modelDirectory holds the selected preset's fold-cache and encoder model pairs.
std::unique_ptr<FastGraph> CreateFastGraph(MatchDevice, ExtendDevice, void* cookie,
    const std::filesystem::path& modelDirectory, Preset preset, bool hud, bool chroma, std::stop_token stop,
    bool injectStartupDeviceLoss = false);
} // namespace llcv::rife_vulkan
