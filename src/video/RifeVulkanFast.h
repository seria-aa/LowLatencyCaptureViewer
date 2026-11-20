#pragma once
// Measurement-only Vulkan backend for the RIFE Fast and Half presets. Built only with
// LLCV_RIFE_VULKAN and enabled only by the --rife-vulkan-fast option.
#include <windows.h>
#include <d3d11_4.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include "video/RifeVulkanGraph.h"

namespace llcv::rife_vulkan {
enum class HudTrial { None, Tiled, EmptyMask, WeakChroma, InputFlip };
inline constexpr int kWidth = 1920, kHeight = 1080;

// Owns the ncnn graph, the Vulkan views of two shared D3D11 RGBA8 textures
// and one worker thread. The caller signals waitValue on the shared fence
// after writing the input texture; the job signals signalValue after the
// interpolated midpoint has been written to the output texture.
class FastBackend {
public:
    FastBackend(ID3D11Device* device, ID3D11Texture2D* input, ID3D11Texture2D* output,
                ID3D11Fence* fence, const std::filesystem::path& models, Preset preset,
                bool hud = false, bool chroma = false, HudTrial trial = HudTrial::None, bool verifyHud = false);
    ~FastBackend();
    FastBackend(const FastBackend&) = delete;
    FastBackend& operator=(const FastBackend&) = delete;

    // False while a job is queued or running. Rethrows a worker failure.
    bool Free();
    // arrivalUs is the capture arrival time; the HUD detector uses its steps.
    void Enqueue(uint64_t waitValue, uint64_t signalValue, int64_t arrivalUs = 0, bool flipY = false);
    // HUD detector activity so far: retired jobs, jobs with a detected HUD core,
    // and the mean core fraction. False when protection is off.
    bool HudSummary(uint64_t& jobs, uint64_t& active, double& meanCore,
                    uint64_t& chromaJobs, double& meanChromaCore, uint64_t& chromaCapped);
    // Returns true once per finished job with its GPU time (ms, <0 if unknown).
    bool TakeGpuMs(double& ms);
    const std::wstring& DeviceName() const;
    static int CompareHud(const std::filesystem::path& models, const std::filesystem::path& report, HudTrial trial);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace llcv::rife_vulkan
