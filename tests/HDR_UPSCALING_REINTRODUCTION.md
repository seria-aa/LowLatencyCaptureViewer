# HDR10 CMAA2 → NIS validation (2026-11-01)

Historical validation of the initial guarded HDR AA implementation. The current
product path has since been simplified to **NIS + the same contour guard**,
without HDR CMAA2 resources. See [HDR_NIS_SIMPLIFICATION.md](HDR_NIS_SIMPLIFICATION.md)
for the decision, subsequent checks and current behavior. Results below describe
the earlier implementation, not the later NIS-only path.

## Design and prior failure

The earlier USM-only mitigation was visually rejected. Saved native/CMAA/NIS
stages showed contour changes before sharpening as well as the negative USM
lobe. Changing CMAA edge detection to PQ or using linear NIS had already been
tried; neither is presented here as a new cure.

The new path retains native RGB10 PQ/BT.2020 from the existing P010 Video
Processor conversion. ST 2084 decode/encode surrounds CMAA, whose separate
HIGH/Preserve shaders use the official HDR-range temporary packing. NIS uses
matching PQ shader and host configuration, for both 128/256-thread dispatches.
SDR shaders and their transfer functions remain unchanged.

In the existing final HDR copy, a bilinear sample of the **original pre-AA PQ
image** bounds every output channel to reference ± max(reference × 0.02,
0.5/1023). This bounds the entire AA/reconstruction/sharpening result rather
than only the USM contribution. Correction is intentionally more conservative
in HDR. There is one extra bilinear sample in an existing pass, no new pass,
readback, frame queue, capture buffer, or per-frame allocation.

This is an explicit contour-change bound, not a claim that all morphological
AA contour changes are bugs or that subjective HDR quality is now approved.

Official integration references:

- [CMAA2 HDR formats and settings](https://github.com/GameTechDev/CMAA2)
- [NIS color ranges and HDR permutations](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling#color-space-and-ranges)

## Reported glyph

`HdrGlyphFixture.h` contains a neutral 24×24 Limited P010 luma crop from the
reported glyph, not a new synthetic substitute. Source origin (996,270),
1920×1080. Capture SHA256:
`495720EAE72343FF2FDCF031755DC24713A24EB73464A5F3951E79D2A5385CD0`.
The full original is an optional local diagnostic input, not a repository test
dependency. The independent CPU bilinear reference checks every channel of
every output pixel, allowing one final 10-bit code of interpolation/rounding.

At the previously measured output pixel (1340,378), the full saved input gives:

| Correction | Original VP | Unguarded HDR filter | Original-reference guard |
| --- | ---: | ---: | ---: |
| HIGH, NIS 0.15 | 26.0649 nit | 3.4210 nit | 24.1297 nit |
| Preserve, NIS 0.15 | 26.0649 nit | 3.3707 nit | 24.1297 nit |

The repository fixture checks both correction modes and sharpness 0/0.15/0.5/1.
Replacing only the final shader with its unguarded counterpart causes over
100 contour-bound violations, proving the new assertion detects the failure.
It also checks exact OFF restoration, source immutability, offset viewport
mapping/black margins and native/downscale/beyond-2× bypass.

## Regression results

Latest-code `SettingsViewTests` passed: 1,600 combinations, Korean/English,
fractional DPI, small work areas and no-scroll pagination. Help buttons are
right-aligned again; CMAA mode no longer displays an SDR-only badge.

The subsequent 20-case gate passed (133.27 s): ScreenshotTests;
CmaaNisRegression/Hdr/Rgb/Recording; UpscalingSafetyHardware/Warp;
VsrExperimentTests; RecordingSettingsCancelTests; PlaybackMuteRecordingTests;
RecordingAudioDeviceSwitchTests; HdrPipelineTests; HdrVsrPipelineTests;
SdrPipelineTests; Rgb32PipelineTests; Rgb24PipelineTests; OverlayUiTests;
SettingsStoreTests; RendererResizeTests; TestProfileTests.

HDR coverage includes flat neutral/chroma colors across PQ levels, both chroma
locations, both NIS dispatch sizes, glyph cores at 100/159/160/161/400/1000/
4000/10000 nit, repeat stability, F6 ON/OFF, OSD and SDR→HDR→SDR retention.
Existing recording support remains SDR/stereo; HDR recording was not added.

Local raw evidence: `build-v2200-release/hdr-upscaling-regression.xml` and
`outputs/hdr-upscaling-20261101/captured/{glyph.txt,gpu.csv}`. Generated local
evidence is not a package or repository dependency.

## GPU cost and limits

Archived measurements: the old HDR timing helper used an 8-bit completion
texture for RGB10 output. Use the format-correct results in
[HDR_NIS_SIMPLIFICATION.md](HDR_NIS_SIMPLIFICATION.md) for current costs;
do not reuse these numbers to estimate overhead versus OFF.

RTX 3080, saved FHD P010 → QHD RGB10, NIS 0.15, same renderer; three paired
AB/BA rounds, 120 timestamps per variant/round after continuous 500 ms warmup.
Both variants perform the whole HDR filter; only the final guard differs.

| Correction | Without guard | With guard |
| --- | ---: | ---: |
| HIGH | ~1.40 ms | ~1.38 ms |
| Preserve | ~1.40 ms | ~1.41 ms |

This short run does not establish a speedup or zero cost. It measures GPU work
including upload/VP/filter/output dependency, not capture-to-display or optical
latency. The 128/256-thread variants ran on this NVIDIA GPU, not physical AMD
or Intel hardware. Windows HDR was off; no OS display settings were changed.
On 2026-11-01 the user reported that the updated appearance looked improved
("괜찮아졌네"). This is visual feedback on the reported case, not exhaustive
certification of every HDR scene, display or GPU.
Real viewer/recording settings hashes and the installed release were preserved.
