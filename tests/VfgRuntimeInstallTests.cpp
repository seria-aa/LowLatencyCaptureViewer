#include "video/VfgRuntimeInstall.h"
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace llcv::vfg_runtime;

static_assert(CanStartDownload(Stage::Idle) && CanStartDownload(Stage::Failed) &&
              !CanStartDownload(Stage::Ready));
static_assert(InProgress(Stage::Verifying) && InProgress(Stage::Downloading) &&
              InProgress(Stage::Installing) && !InProgress(Stage::Failed));
static_assert(!CanRetryWithInstalledRuntime(false, Stage::Ready) &&
              !CanRetryWithInstalledRuntime(true, Stage::Downloading) &&
              !CanRetryWithInstalledRuntime(true, Stage::Verifying) &&
              !CanRetryWithInstalledRuntime(true, Stage::Installing) &&
              !CanRetryWithInstalledRuntime(true, Stage::Failed) &&
              CanRetryWithInstalledRuntime(true, Stage::Ready));

bool VerifyExistingAndWait(Stage expected) {
    const auto deadline = GetTickCount64() + 10000;
    for (;;) {
        const auto stage = GetStatus().stage;
        if (!InProgress(stage)) return stage == expected;
        if (GetTickCount64() >= deadline) return false;
        Sleep(2);
    }
}

int wmain(int argc, wchar_t** argv) {
    GUID id{};
    wchar_t name[40]{};
    if (FAILED(CoCreateGuid(&id)) || StringFromGUID2(id, name, ARRAYSIZE(name)) <= 0) return 1;
    const fs::path root = fs::temp_directory_path() / (std::wstring(L"llcv-vfg-test-") + name);
    if (!fs::create_directory(root)) return 2;
    struct Clean { fs::path root; ~Clean(){ std::error_code ec; fs::remove_all(root, ec); } } clean{root};
    const auto store = root / L"vfg-runtime";
    const auto bad = root / L"bad.whl";
    std::ofstream(bad) << "not a wheel";
    try { InstallLocalArchiveForTest(bad, store); return 3; } catch (...) {}
    for (const auto& entry : fs::directory_iterator(store))
        if (entry.is_directory()) return 4; // nothing uploaded to the active directory
    const bool partial = argc == 2 && std::wstring(argv[1]) == L"--missing-component";
    if (argc == 1 || partial) {
        // Partial and complete-looking but altered installations must never
        // publish Ready, even without a renderer attempt.
        const auto fake = store / L"0.2.0.0-win-x64";
        fs::create_directory(fake);
        for (auto component : {L"cudart64_12.dll", L"NVCVImage.dll", L"nvngxruntime.dll",
                          L"NVVideoEffects.dll", L"nvVFXVideoFrameGeneration.dll"}) {
            if (partial && std::wstring(component) == L"nvVFXVideoFrameGeneration.dll") continue;
            std::ofstream(fake / component) << "not a verified component";
        }
        Initialize(root);
        const auto first = GetStatus().stage;
        if ((first != Stage::Verifying && first != Stage::Failed) ||
            !VerifiedDirectory().empty() || !VerifyExistingAndWait(Stage::Failed) ||
            !CanStartDownload(GetStatus().stage)) return 5;
        std::puts(partial ? "Rejected a partial per-user payload before any renderer attempt."
                          : "Rejected invalid archive and altered per-user payload before any renderer attempt.");
        return 0;
    }
    if (argc != 2 && argc != 3) return 6;
    const std::wstring mode = argv[1];
    if (argc == 3 && mode == L"--verify-installed-copy") {
        try { fs::copy(argv[2], store / L"0.2.0.0-win-x64", fs::copy_options::recursive); }
        catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 9; }
        Initialize(root);
        if (!VerifyExistingAndWait(Stage::Ready)) return 13;
    } else if (argc != 2) {
        return 6;
    } else if (mode == L"--download" || mode == L"--download-resume") {
        Initialize(root);
        if (!StartDownload(mode == L"--download-resume") || StartDownload()) return 7;
        unsigned last = 101;
        for (;;) {
            const auto status = GetStatus();
            if (status.stage == Stage::Failed) return 8;
            if (status.stage == Stage::Ready) break;
            if (status.percent != last) {
                last = status.percent;
                if (last % 10 == 0) { std::printf("Downloaded: %u%%\n", last); std::fflush(stdout); }
            }
            Sleep(100);
        }
    } else {
        try { InstallLocalArchiveForTest(argv[1], store); }
        catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 9; }
        Initialize(root);
        if (!VerifyExistingAndWait(Stage::Ready)) return 13;
    }
    const auto payload = VerifiedDirectory();
    if (payload.empty() || GetStatus().stage != Stage::Ready || StartDownload()) return 10;
    if (std::distance(fs::directory_iterator(payload), fs::directory_iterator{}) != 5) return 11;
    for (const auto& entry : fs::directory_iterator(store))
        if (entry.is_directory() && entry.path() != payload) return 12;
    std::puts("Verified and installed exactly five NVIDIA runtime DLLs; no download staged.");
    return 0;
}
