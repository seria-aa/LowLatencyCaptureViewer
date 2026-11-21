#pragma once
// Vulkan backend for the bundled RIFE Fast preset and diagnostic Fast/Half runs.
#include <windows.h>
#include <d3d11_4.h>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include "video/RifeVulkanGraph.h"

namespace llcv::rife_vulkan {
enum class HudTrial { None, Tiled, EmptyMask, WeakChroma, InputFlip, HistoryOnly, WorkerFailure, StartupDeviceLoss };
inline constexpr int kWidth = 1920, kHeight = 1080;
struct BackendRetiring {}; // Retry initialization on a later capture frame.
struct SubmissionTiming {
    uint64_t ticket = 0;
    int64_t enqueuedUs = 0, inputWaitBeginUs = 0, inputReadyUs = 0, submitBeginUs = 0, submitEndUs = 0;
    JobKind kind = JobKind::Generate;
    bool expiredBeforeGraph = false;
    int32_t submitResult = 0; // Valid only when submitEndUs is recorded.
};

// Owns the ncnn graph, the Vulkan views of two shared D3D11 RGBA8 textures
// and one worker thread. The caller signals waitValue on the shared fence
// after writing the input texture; the job signals signalValue after the
// interpolated midpoint has been written to the output texture.
class FastBackend {
public:
    FastBackend(ID3D11Device* device, ID3D11Texture2D* input, ID3D11Texture2D* output,
                ID3D11Fence* fence, const std::filesystem::path& models, Preset preset,
                bool hud = false, bool chroma = false, HudTrial trial = HudTrial::None, bool verifyHud = false,
                bool traceSubmission = false, std::stop_token startupStop = {});
    ~FastBackend();
    FastBackend(const FastBackend&) = delete;
    FastBackend& operator=(const FastBackend&) = delete;
    // Returns an invalid future if another initialization is still running.
    // Only device objects are shared; the render thread owns its immediate context.
    static std::future<std::unique_ptr<FastBackend>> TryInitializeAsync(
        ID3D11Device*, ID3D11Texture2D*, ID3D11Texture2D*, ID3D11Fence*,
        std::filesystem::path models, Preset preset, bool hud, bool chroma, bool traceSubmission,
        std::stop_source cancel = {});

    // False while a job is queued or running. Rethrows a worker failure.
    bool Free();
    bool HasFailed() const noexcept;
    // A retired history-only ticket does not invalidate the original fallback.
    bool GeneratesOutputFor(uint64_t ticket) const noexcept;
    // arrivalUs is the capture arrival time; the HUD detector uses its steps.
    void Enqueue(uint64_t waitValue, uint64_t signalValue, int64_t arrivalUs = 0, bool flipY = false,
                 JobKind kind = JobKind::Generate, int64_t deadlineUs = 0);
    // HUD detector activity so far: retired jobs, jobs with a detected HUD core,
    // and the mean core fraction. False when protection is off.
    bool HudSummary(uint64_t& jobs, uint64_t& active, double& meanCore,
                    uint64_t& chromaJobs, double& meanChromaCore, uint64_t& chromaCapped);
    // Returns true once per finished job with its GPU time (ms, <0 if unknown).
    bool TakeGpuMs(double& ms);
    bool TakeSubmissionTiming(SubmissionTiming& timing);
    const std::wstring& DeviceName() const;
    static int CompareHud(const std::filesystem::path& models, const std::filesystem::path& report, HudTrial trial);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace llcv::rife_vulkan
