# CMAA2 integration

Official source snapshot from GameTechDev/CMAA2, pinned commit
`071c6b0857559f4e36f614362e6d2aab1b61938a`.
`vaCMAA2DX11.cpp`, `README.md`, `license.txt` are unmodified.
`CMAA2.hlsl` has marked LLCV adapter changes: single-sample source
loads clamp signed coordinates to the image bounds, rather than treating
out-of-bounds zero as a black edge. Regression caught darkening at the
first pixel of an otherwise flat color without this change.
The collection-path define is guarded to allow choosing the official
direct-processing path (`CMAA2_COLLECT_EXPAND_BLEND_ITEMS=0`).
The collection path produced larger repeated-output differences on dense
synthetic patterns before NIS; direct processing passes the one-code-value
CMAA2 rounding bound. Both before/after comparisons use this same path.
No fixed-point blend or increased linked-list iteration limit is retained.
Apache-2.0 license retained; embedded Microsoft packing helpers retain their
upstream credit. Only the HLSL is compiled; the C++ file is an integration reference.

Host adapter: `CmaaPipeline` in `src/video/CmaaNisUpscaler.cpp`, shared by
the regular viewer and private comparisons. Build-time FXC output is embedded.
High preset, color edge detection (path 0), Extra Sharp off, FP32 math, 1 sample.
Official five-dispatch sequence with both indirect stages and UAV/SRV unbinding.
Full-size candidate/item/location buffers follow the reference's safe variant,
not its reduced-memory heuristic that can silently omit dense edges.

RGB scratch images are FP16. `UpscaleTransfer.hlsl` linearizes BT.709-encoded
SDR before CMAA2 and restores the same encoding afterward. The unsuccessful
HDR transfer/sharpening experiment has been removed; HDR bypasses this pipeline
and retains the original HDR renderer. No tone mapping or gamut conversion
occurs here; only the SDR CMAA2 and NIS variants are compiled.
Typed float UAV output does not add an sRGB store conversion.

NIS preprocessing and postprocessing are available for this quality experiment.
Preprocessing uses native FHD dimensions; postprocessing uses QHD dimensions.
Both use the same SDR CMAA2 settings, transfer, and border handling. The regular
viewer exposes only CMAA2 before NIS for SDR. Native-size,
downscaled, or beyond-2x views use the original processor. Private post-AA and
other filter comparisons are not linked into the normal executable.
