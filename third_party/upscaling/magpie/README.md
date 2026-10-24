# Additional private upscaling candidates

Snapshot of [Blinue/Magpie](https://github.com/Blinue/Magpie), commit
`9e8e90274ba07f555bf87f09321dabdac97b1b4f`, downloaded from the official raw
repository. Files retain original contents; GPL-3.0 license is included.
RAVU's header offers GPL-3.0-or-later. This application's existing license is GPL-3.0.
No new assets are added to regular application packages.

- `RAVU/RAVU_Zoom_AR_R2_RGB.hlsl`, `prescalers.hlsli`, both DDS weights:
  single-pass RGB RAVU-Zoom, radius 2, anti-ringing 0.8.
- `FSRCNNX/FSRCNNX.hlsl`: official Magpie port of FSRCNNX_x2_8-0-4-1,
  six compute passes, FP32 arithmetic and FP16 feature storage in this prototype.
  Native 2x output is reduced to the requested size with antialiased Lanczos3.
- `Jinc.hlsl`, `Lanczos.hlsl`, `StubDefs.hlsli` are preserved reference
  sources, not directly compiled. The app has a small pixel-shader implementation
  of Jinc2's radial sine approximation and separable Lanczos3, anti-ringing 0.5.

`CompileLearned` supplies explicit D3D11 resource bindings, block dimensions,
size helpers and an entry point around each unchanged RAVU/FSRCNNX pass.
The thread remap and fused multiply-add helpers match the pinned
`src/Magpie.Core/EffectCompiler.cpp` runtime, not the simplified IDE stubs.
There is no runtime source rewriting on the render path. Only Configure compiles
shaders. The two RAVU LUT samplers alias one identical linear sampler.

The analytic Jinc implementation deliberately does not reproduce the pinned
Magpie Jinc source's repeated `src[2][3]` in its last-row sum (in place of
`src[1][3]`). All 16 taps are distinct; mirror-symmetry regression covers this.
Lanczos widens support for the FSRCNNX downsample instead of treating reduction
as enlargement. No learned model weights are changed.

Origin references:
- https://github.com/bjin/mpv-prescalers
- https://github.com/hauuau/magpie-prescalers
- https://github.com/igv/FSRCNN-TensorFlow
- https://github.com/libretro/common-shaders/tree/master/windowed/shaders
