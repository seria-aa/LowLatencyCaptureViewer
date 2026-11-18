#pragma once
#include <windows.h>

namespace llcv::vfg {

enum class GpuSupport { Unavailable, Official, ExperimentalAmpere };

constexpr bool OfficialArchitecture(int major, int minor) {
    return (major == 8 && minor == 9) || major == 12;
}

constexpr GpuSupport SupportForArchitecture(int major, int minor) {
#ifdef LLCV_VFG_AMPERE_COMPAT
    if (major == 8 && minor == 6) return GpuSupport::ExperimentalAmpere;
#endif
    return OfficialArchitecture(major, minor) ? GpuSupport::Official : GpuSupport::Unavailable;
}

// Queries the system driver, not the optional VFG/CUDA runtime package.
// Match the D3D11 adapter exactly; another GPU in the system cannot qualify it.
GpuSupport ProbeGpuSupport(UINT vendor, const LUID& adapter) noexcept;

}
