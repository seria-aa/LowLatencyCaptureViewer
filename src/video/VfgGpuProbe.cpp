#include "VfgGpuProbe.h"
#include <cuda.h>
#include <cstring>

namespace llcv::vfg {
GpuSupport ProbeGpuSupport(UINT vendor, const LUID& adapter) noexcept {
    if (vendor != 0x10de) return GpuSupport::Unavailable;
    struct Module {
        HMODULE handle = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        ~Module() { if (handle) FreeLibrary(handle); }
    } driver;
    if (!driver.handle) return GpuSupport::Unavailable;
    const auto init = reinterpret_cast<decltype(&cuInit)>(GetProcAddress(driver.handle, "cuInit"));
    const auto count = reinterpret_cast<decltype(&cuDeviceGetCount)>(GetProcAddress(driver.handle, "cuDeviceGetCount"));
    const auto device = reinterpret_cast<decltype(&cuDeviceGet)>(GetProcAddress(driver.handle, "cuDeviceGet"));
    const auto luid = reinterpret_cast<decltype(&cuDeviceGetLuid)>(GetProcAddress(driver.handle, "cuDeviceGetLuid"));
    const auto attribute = reinterpret_cast<decltype(&cuDeviceGetAttribute)>(GetProcAddress(driver.handle, "cuDeviceGetAttribute"));
    int total = 0;
    if (!init || !count || !device || !luid || !attribute ||
        init(0) != CUDA_SUCCESS || count(&total) != CUDA_SUCCESS) return GpuSupport::Unavailable;
    for (int ordinal = 0; ordinal < total; ++ordinal) {
        CUdevice gpu{};
        char identity[sizeof(LUID)]{};
        unsigned nodeMask = 0;
        if (device(&gpu, ordinal) != CUDA_SUCCESS || luid(identity, &nodeMask, gpu) != CUDA_SUCCESS ||
            std::memcmp(identity, &adapter, sizeof(adapter)) != 0) continue;
        int major = 0, minor = 0;
        if (attribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, gpu) == CUDA_SUCCESS &&
            attribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, gpu) == CUDA_SUCCESS)
            return SupportForArchitecture(major, minor);
        break;
    }
    return GpuSupport::Unavailable;
}
}
