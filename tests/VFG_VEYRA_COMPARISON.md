# LLCV 2.3.0 VFG reference comparison (RTX 40/50)

This is a source and component check, not a physical RTX 40 or RTX 50 run of LLCV.
Reference: [Veyra 2.1.9 native backend](https://github.com/Likely7/Veyra-NRVideo/blob/v2.1.9/src/pipeline/VfgBackend.cpp)
and its Windows NVIDIA portable build. The local executable SHA-256
`C8A53DC778331C696F5A488400885EF842ADB849E1B22A9FBEA07520A9ECB8F0`
matches its bundled validation record. That record references RTX 5070 work;
it is not a RTX 40 test or verification of every RTX 50 model.

| Check | Result |
| --- | --- |
| Runtime components | The local LLCV test runtime and Veyra match byte-for-byte by SHA-256 for `cudart64_12.dll`, `NVCVImage.dll`, `nvngxruntime.dll`, `NVVideoEffects.dll`, and `nvVFXVideoFrameGeneration.dll`. |
| Native API contract | Both create `VideoFrameGeneration`, set CUDA stream/dimensions/quality and input images before loading, then bind the output and use frame multiplier/index plus asynchronous run. LLCV uses multiplier 2, midpoint index 1 and SDR RGBA8, consistent with the [NVIDIA VFG API](https://docs.nvidia.com/maxine/vfx/latest/Filters/VideoFrameGeneration.html). |
| Adapter and architecture | Both select a CUDA device by graphics adapter LUID. The compatibility branch is for SM 8.6; LLCV's Ada/Blackwell path does not install the compatibility hook. |
| Does not transfer from Veyra | Veyra uses D3D12 buffers and fences; LLCV uses D3D11 textures/fence integration. Component discovery, driver/model initialization, format conversion, VSync and exclusive presentation remain LLCV-specific. |

Do not label this an LLCV RTX 40/50 hardware pass. No NVIDIA component is
included in the base package, and matching hashes do not establish permission
to redistribute the runtime. See [distribution audit](INTERPOLATION_DISTRIBUTION_AUDIT.md).
