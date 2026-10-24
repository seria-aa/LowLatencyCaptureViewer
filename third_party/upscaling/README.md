# Display upscaling dependencies

Official shader/header snapshots with adapter changes documented per directory;
MIT/BSD/GPL/Apache notices are retained in each directory.
The normal application embeds only CMAA2 and NIS shader bytecode; their license
notices are included in release packages. The other directories are retained
only to reproduce private comparisons behind `LLCV_SPATIAL_PROTOTYPE` (off by
default), and are not linked or copied into the normal application package.

| Directory | Upstream | Pinned commit |
|---|---|---|
| nis | https://github.com/NVIDIAGameWorks/NVIDIAImageScaling | `35e13ba316c98eeecf16f37eae70ce88019911f6` |
| fsr | https://github.com/GPUOpen-Effects/FidelityFX-FSR | `a21ffb8f6c13233ba336352bdff293894c706575` |
| cas | https://github.com/GPUOpen-Effects/FidelityFX-CAS | `9fabcc9a2c45f958aff55ddfda337e74ef894b7f` |
| sgsr | https://github.com/SnapdragonGameStudios/snapdragon-gsr | `d926f074bcb9d714e179f1ce0fcb9ee2eeb5074e` |
| magpie | https://github.com/Blinue/Magpie | `9e8e90274ba07f555bf87f09321dabdac97b1b4f` |
| smaa | https://github.com/iryoku/smaa | `71c806a838bdd7d517df19192a20f0c61b3ca29d` |
| cmaa2 | https://github.com/GameTechDev/CMAA2 | `071c6b0857559f4e36f614362e6d2aab1b61938a` |

The [Magpie snapshot](magpie/README.md) provides RAVU-Zoom and FSRCNNX
passes/weights and reference Jinc/Lanczos implementations. Its GPL license,
runtime adaptations and reference-only files are documented separately.

Production: CMAA2 High before NIS, SDR only, default NIS sharpness 0.15.
HDR bypasses this filter. See [CMAA2 integration](cmaa2/INTEGRATION.md).

Initial private comparison settings (not current application defaults):
NIS: SDK 1.0.3, FP32 Shader Model 5 path, architecture-specific group size.
FSR 1: EASU + RCAS (1 stop); CAS: scaling with sharpness 0.5 and the documented
gamma-2 linearization approximation. NIS sharpness is 0.5. These values are not
perceptually equivalent strength settings. Screenshots must accompany performance comparisons.
SGSR 1 uses the official HLSL mobile kernel, default RGBA operation, edge threshold
8/255, sharpness 2, no optional edge-direction branch. Its `half` types compile to
FP32 on this Shader Model 5/D3D11 path; mobile FP16 performance claims do not transfer.

All candidates use the production Video Processor color conversion at native input size.
NIS's stock NV12 conversion has fixed coefficients, so it is deliberately not used for the
app's configurable BT.601/709 and Limited/Full inputs. HDR, native-size and downscaling
remain on the original renderer. No temporal reconstruction or generated frames.
