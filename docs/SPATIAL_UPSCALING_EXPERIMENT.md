# SDR spatial upscaling prototype — 2026-10-24

Additional RAVU, FSRCNNX and Jinc/Lanczos results are in
[the extended comparison](SPATIAL_UPSCALING_EXTENDED_COMPARISON.md).
This document retains the initial five-candidate experiment below.

> Correction: the user subsequently reported that the original HDMI source was
> not FHD. The original live results below verified only the capture-board output
> size, not the HDMI source resolution. They must not be presented as a native-FHD
> HDMI comparison. The new run after the user set the console to 1080p60 SDR
> is complete: see [FHD re-comparison](SPATIAL_UPSCALING_FHD_RECOMPARISON.md).
> The synthetic GPU benchmarks are unaffected.

## Decision

Five current-frame candidates were implemented in a separate development target:
Bicubic, CAS, FSR 1, NIS and SGSR 1. None reduced measured display-event latency
relative to the existing Video Processor enlargement. SGSR 1 had the smallest
additional GPU cost of the shader candidates in this experiment. Bicubic and CAS
remain inexpensive visual alternatives. These results do not establish a universal
quality winner or justify changing the application's default renderer.

The normal application, saved settings and release packaging are unchanged.
No release or merge is part of this experiment.

## Research and implementation

- [AMD CAS](https://gpuopen.com/fidelityfx-cas/): spatial sharpening and scaling.
- [AMD FSR 1](https://gpuopen.com/fidelityfx-superresolution/): EASU plus RCAS.
- [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling): spatial scaler/sharpener.
- [Qualcomm SGSR 1](https://github.com/SnapdragonGameStudios/snapdragon-gsr/tree/main/sgsr/v1): single-frame edge-adaptive scaler.
- [SGSR 2](https://www.qualcomm.com/developer/blog/2024/10/introducing-snapdragon-game-super-resolution-2) is a different temporal approach. Temporal reconstruction requiring engine motion/depth data is not applicable to ordinary captured video without additional estimation work.

Official shader snapshots, pinned revisions and licenses are in
`third_party/upscaling/README.md`. Bicubic uses a Catmull-Rom kernel without
additional sharpening. CAS/NIS sharpness is 0.5, FSR RCAS is 1 stop, and SGSR 1
uses its default RGBA coefficients. These are not perceptually equal settings.
The SGSR mobile HLSL `half` operations use FP32 on this D3D11/SM5 implementation;
published mobile FP16 timing is not applicable.

All shader paths retain the production Video Processor's native-resolution YUV
conversion, including BT.601/709 and SDR Limited/Full policy. NIS's built-in NV12
conversion has fixed coefficients and was deliberately not used. Upscaling runs
on that BGRA image, before the existing OSD and indicators. FSR uses a second
sharpening pass; NIS uses a compute output plus a final copy pass. Others write
directly to the backbuffer. No per-frame CPU conversion, extra frame queue,
allocation or readback was added to `Pipeline::Draw`.

Eligibility is SDR, enlargement, 1–2x per axis. Native size, downscaling, larger
ratios and HDR retain the original renderer. Initialization failure falls back
to the original path. Custom scaling suppresses VSR and VP edge enhancement to
avoid double processing. Capture, recording input and Present policy are not
replaced. Existing screenshot behavior is not intentionally changed.

## Build and reproduce

Configure the existing project with `BUILD_TESTING=ON` and
`LLCV_SPATIAL_PROTOTYPE=ON` (default OFF). Build
`LowLatencyCaptureViewer_Spatial` and `SpatialUpscalingTests`.
The production target does not receive the prototype define or source file.
Shaders are compiled from the configured source directory: this is a local
development build, not a portable binary package.

Prototype-only environment variable `LLCV_SPATIAL_UPSCALER` accepts
`baseline`, `bicubic`, `cas`, `fsr1`, `nis`, `sgsr`, `ravu`, `fsrcnnx`, `jinc`
or `lanczos`. Unknown values use baseline.
The prototype suppresses all settings saves/migrations. `--spatial-live-test yes`
sets session-only 1080p60 NV12 capture and QHD output. Tests can be run with:

```text
SpatialUpscalingTests --regression
SpatialUpscalingTests --recording <new-output-directory>
SpatialUpscalingTests --benchmark <new-output-directory> [method]
SpatialUpscalingTests --capture <1080p-NV12-file> <output-directory>
```

`tools/Compare-SpatialUpscaling.ps1` runs bounded live sessions with PresentMon.
`tools/Analyze-SpatialUpscaling.ps1` joins app and PresentMon records and validates
matching. Measurement output directories must be new; existing results are not
overwritten. Live tracing buffers records in memory and writes at shutdown.
One source/output snapshot occurs during warmup, before the measurement cutoff.

## Actual capture results

GC573 / RTX 3080, NV12 1920×1080 at 60 fps → 2560×1440, same window placement,
Composed Flip, no recording. Input was the Switch home screen. First 3 seconds
and final 0.5 seconds excluded. Present QPC matched within 0.25 ms; no unmatched
records. These are capture-callback → Windows displayed-event times, **not**
HDMI input-to-photon latency. Small run-to-run differences are not a reliable ranking.

| Method | Mean ms | p95 ms | Analysed displayed frames / dropped presents |
|---|---:|---:|---:|
| Existing VP | 13.70–13.83 | 19.48–19.77 | 750 / 0 |
| Bicubic | 14.09–14.26 | 19.56–19.83 | 746 / 0 |
| CAS | 13.91 | 19.88 | 373 / 0 |
| FSR 1 | 14.78 | 19.96 | 370 / 0 |
| NIS | 14.65 | 20.01 | 328 / 0 |
| SGSR 1 | 13.74 | 19.46 | 372 / 0 |

Baseline/Bicubic ranges cover two runs; others had one. Different analysed frame
counts reflect measurement windows and do not establish capture-source frame loss.
Zero dropped presents is only a statement about the analysed PresentMon records.

VSR ON was also requested and accepted, but its captured output was byte-identical
to the existing VP result rendered from exactly the same captured input. Actual
neural processing was therefore not established, and that timing is excluded from
the VSR performance comparison. Do not claim a measured speedup over active VSR.

## Synthetic GPU comparison

100 samples × three rotated-order runs per resolution, 60 fps pacing. End timestamp
depends on a small copy from the final image so a separate video engine cannot be
mistaken for zero processing time. A subsequent readback verifies completion.
These dependency timings are **not pure GPU ALU duration or display latency**.

| Method | 1080p → QHD dependency mean, ms | 1080p → 4K dependency mean, ms |
|---|---:|---:|
| Existing VP | 0.27 | 0.29 |
| Bicubic | 0.38 | 0.47 |
| CAS | 0.35 | 0.46 |
| FSR 1 | 0.48 | 0.78 |
| NIS | 0.66 | 0.95 |
| SGSR 1 | 0.33 | 0.37 |

CPU completion measurements also include upload, driver and readback overhead;
they must not be described as the production path's added delay. Initial naive
timestamp results in `outputs/spatial-upscaling/timings.csv` are invalid for
performance conclusions; use `validated/timings.csv` and
`sgsr-benchmark/timings.csv` instead. Synthetic PSNR describes only the generated
pattern, not general game quality. Captured-frame comparisons have no ground-truth
high-resolution reference and are not assigned a quality score.

## Verification

Release x64 full build passed. Focused CTest suite: **31/31 passed**, 152.80 seconds.
Coverage includes new kernels, NV12 BT.601/709 Limited/Full conversion, YUY2,
post-scale OSD, odd dimensions, black borders, native-size bypass/restore,
HDR bypass policy, existing HDR/VSR pipelines, renderer resize, placement,
FPS monitoring, settings/cancel, exclusive-viewer lifecycle, audio replay/mute,
recording audio-device changes, MJPEG, HEVC sync and recording scale/color.
This is not a fresh physical HDR, every-GPU or long-duration stress certification.

New recording test renders each candidate alongside recording original NV12 input.
All six H.264/AAC files independently decoded successfully with 60 video frames,
strictly increasing video timestamps and 47 AAC packets/frames. Video duration was
1.000000 s and audio 1.002667 s. Decoded content was identical across all methods:

- Video SHA256: `2ef225147e119b3e26bc0de78249164d7cd982bd61d49c0370cd4e895fec5890`
- Audio SHA256: `3e109d04aedb74375626714c386ae09782d7cdde1c84658da734423b598987d6`

Saved user settings SHA256 before/after live tests matched:
`B6B33F5448F16B4DC3D26DFD78D13B0FF910A436D87A48D05DA89E971474EB98`.

## Local evidence

- `outputs/spatial-upscaling/comparison.html`: same-source image comparison and timing table.
- `outputs/spatial-upscaling/live-{1,2}/`: app traces, PresentMon records and summaries.
- `outputs/spatial-upscaling/captured/`: native image and all five candidate images.
- `outputs/spatial-upscaling/validated/`, `sgsr-benchmark/`: valid GPU samples.
- `outputs/spatial-regression-suite.log`, `spatial-build-all.log`: verification logs.

The HTML and image files were inspected as source/pixels; embedded-browser
navigation to the local file was blocked, so interactive browser rendering was
not verified. Open the local HTML with its sibling image folders intact.
