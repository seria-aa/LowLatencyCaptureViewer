#pragma once
#include <cstdint>
#include <filesystem>

namespace llcv::vfg_runtime {
inline constexpr auto PackageHost = L"pypi.nvidia.com";
inline constexpr auto PackagePath = L"/nvidia-vfx/nvidia_vfx-0.2.0.0-cp311-cp311-win_amd64.whl";
inline constexpr uint64_t PackageBytes = 435809365;
enum class Stage { Idle, Downloading, Verifying, Installing, Ready, Failed };
constexpr bool InProgress(Stage stage) noexcept {
    return stage == Stage::Downloading || stage == Stage::Verifying ||
           stage == Stage::Installing;
}
constexpr bool CanStartDownload(Stage stage) noexcept {
    return stage == Stage::Idle || stage == Stage::Failed;
}
// A renderer failure can be retried once the missing runtime becomes usable,
// but an initialization/inference failure against a verified runtime stays
// latched rather than repeatedly resetting CUDA on every capture frame.
constexpr bool CanRetryWithInstalledRuntime(bool failedWithoutVerifiedRuntime, Stage stage) noexcept {
    return failedWithoutVerifiedRuntime && stage == Stage::Ready;
}
struct Status {
    Stage stage = Stage::Idle;
    unsigned percent = 0;
};

// Initialize once, before creating the settings window or renderer. Pass the
// app's already-resolved user data directory (also respects test isolation).
void Initialize(std::filesystem::path appData);
Status GetStatus() noexcept;
bool StartDownload(bool interruptOnceForTest = false) noexcept;
// Returns the verified per-user payload, or empty to keep the legacy app-local
// layout. Never hashes files or starts network activity on the calling thread.
std::filesystem::path VerifiedDirectory();

// Offline test entry against the same size/hash/extraction/activation policy.
void InstallLocalArchiveForTest(const std::filesystem::path& archive,
                                const std::filesystem::path& store);
} // namespace llcv::vfg_runtime
