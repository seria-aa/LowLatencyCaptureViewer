#include "VfgGpuProbe.h"
#include <cuda.h>
#include <cstring>

namespace llcv::vfg {
namespace {
PlaybackGpuStatus Inspect(UINT vendor, const LUID& adapter, bool checkAlternates) noexcept {
    constexpr UINT nvidiaVendor = 0x10de;
    PlaybackGpuStatus result;
    result.currentCheckFailed = vendor == nvidiaVendor || vendor == 0;
    if (vendor != nvidiaVendor && !checkAlternates) return result;
    struct Module {
        HMODULE handle = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        ~Module() { if (handle) FreeLibrary(handle); }
    } driver;
    if (!driver.handle) return result;
    const auto init = reinterpret_cast<decltype(&cuInit)>(GetProcAddress(driver.handle, "cuInit"));
    const auto count = reinterpret_cast<decltype(&cuDeviceGetCount)>(GetProcAddress(driver.handle, "cuDeviceGetCount"));
    const auto device = reinterpret_cast<decltype(&cuDeviceGet)>(GetProcAddress(driver.handle, "cuDeviceGet"));
    const auto luid = reinterpret_cast<decltype(&cuDeviceGetLuid)>(GetProcAddress(driver.handle, "cuDeviceGetLuid"));
    const auto attribute = reinterpret_cast<decltype(&cuDeviceGetAttribute)>(GetProcAddress(driver.handle, "cuDeviceGetAttribute"));
    int total = 0;
    if (!init || !count || !device || !luid || !attribute ||
        init(0) != CUDA_SUCCESS || count(&total) != CUDA_SUCCESS) return result;
    for (int ordinal = 0; ordinal < total; ++ordinal) {
        CUdevice gpu{};
        char identity[sizeof(LUID)]{};
        unsigned nodeMask = 0;
        if (device(&gpu, ordinal) != CUDA_SUCCESS || luid(identity, &nodeMask, gpu) != CUDA_SUCCESS) continue;
        const bool current = vendor == nvidiaVendor &&
            std::memcmp(identity, &adapter, sizeof(identity)) == 0;
        if (!current && !checkAlternates) continue;
        int major = 0, minor = 0;
        if (attribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, gpu) == CUDA_SUCCESS &&
            attribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, gpu) == CUDA_SUCCESS) {
            const auto support = SupportForArchitecture(major, minor);
            if (current) {
                result.current = support;
                result.currentCheckFailed = false;
                if (support != GpuSupport::Unavailable || !checkAlternates) return result;
            } else if (support != GpuSupport::Unavailable) {
                result.otherCompatibleGpu = true;
            }
        } else if (current && !checkAlternates) {
            break;
        }
    }
    return result;
}
}

GpuSupport ProbeGpuSupport(UINT vendor, const LUID& adapter) noexcept {
    return Inspect(vendor, adapter, false).current;
}

PlaybackGpuStatus InspectPlaybackGpu(UINT vendor, const LUID& adapter) noexcept {
    return Inspect(vendor, adapter, true);
}
}
