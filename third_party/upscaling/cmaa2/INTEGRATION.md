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
CMAA2 rounding bound when its working storage is not exceeded.
No fixed-point blend or increased linked-list iteration limit is retained.
Apache-2.0 license retained; embedded Microsoft packing helpers retain their
upstream credit. Only the HLSL is compiled; the C++ file is an integration reference.

Host adapter: `CmaaPipeline` in `src/video/CmaaNisUpscaler.cpp`, shared by
the regular viewer and private comparisons. Build-time FXC output is embedded.
High preset, color edge detection (path 0), Extra Sharp off, FP32 math, 1 sample.
Official five-dispatch sequence with both indirect stages and UAV/SRV unbinding.
Candidate/item/location capacities remain unchanged, but pixel count is NOT
an upper bound on blend items: complex shapes emit several items per candidate.
Every list write checks capacity before publishing an index. ComputeDispatchArgs
records overflow in control word 0 (bits 1=candidates, 2=items, 4=locations) and
sets the AA apply count to zero for the whole affected frame. The original
linear image therefore proceeds to NIS without partial/random AA. The host's
existing per-frame control-buffer clear restores normal processing next frame.
The final apply also rejects invalid indices or a list still unfinished after
the existing iteration cap. No additional allocation, CPU readback, temporal
state, dispatch, or frame queue is introduced. Extreme dense scenes can bypass
CMAA2 for that frame; NIS remains active and recording is unaffected.

`UpscalingSafetyTests` runs the embedded shaders on hardware and WARP: dense
overflow, deliberately tiny capacities for all three lists, exact-capacity
boundaries, active linked-index validity on non-overflow frames, repeat
stability, and next-frame recovery. Overflow clears the append counts, so the
test checks disabled AA apply and preserved output, not erased list lengths.
Poisoned inactive heads and flat/complex/overflow transitions guard against
false-positive list traversal. A stock-capacity synthetic temporal sequence
measures AA bypass changes and NIS amplification of FP16 repeat differences;
see docs/UPSCALING_BOUNDARY_SAFETY.md for results and limits. This is not live
gameplay overflow telemetry and does not resolve threshold flicker.
The optional `llcv_upscaling_diagnostic` library adds bounded delayed sampling
without changing the release library or these shaders. It uses an isolated
viewer profile and reports missing samples explicitly; see
docs/UPSCALING_OVERFLOW_DIAGNOSTIC.md. No diagnostic executable is installed.
It also independently tests NIS opposite-edge isolation and offset viewport
sentinels instead of relying only on an image produced by the same SDK.

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
