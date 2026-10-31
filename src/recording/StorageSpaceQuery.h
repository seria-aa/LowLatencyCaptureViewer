#pragma once
#include "ui/SettingsModeCache.h"
#include <windows.h>
#include <filesystem>

namespace llcv::recording {
struct StorageSpace {
    bool known = false;
    uint64_t bytes = 0;
};
// Worker-only. A selected subfolder may not exist yet; query its nearest
// existing parent. Never resolve a path or call a drive from a UI refresh.
inline StorageSpace QueryStorageSpace(const settings_ui::ModeQueryKey& key,
                                      const std::atomic<bool>& stop) {
    struct ErrorMode {
        DWORD previous = 0;
        bool changed =
            SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous) != FALSE;
        ~ErrorMode() {
            if (changed)
                SetThreadErrorMode(previous, nullptr);
        }
    } errorMode; // A disconnected/removable drive must not open an OS error dialog.
    std::filesystem::path path(key.device);
    std::error_code error;
    while (!path.empty() && !stop) {
        if (std::filesystem::exists(path, error)) {
            ULARGE_INTEGER free{};
            const bool known = !stop && GetDiskFreeSpaceExW(path.c_str(), &free, nullptr, nullptr);
            return {known, known ? free.QuadPart : 0};
        }
        // Access errors are not evidence that a parent has the same space.
        if (error)
            return {};
        const auto parent = path.parent_path();
        if (parent == path)
            break;
        path = parent;
    }
    return {};
}
} // namespace llcv::recording
