# Extended SDR spatial-upscaling comparison — 2026-10-24

## Outcome

Implemented the three requested families as four private modes: RAVU-Zoom AR,
FSRCNNX, Jinc AR and Lanczos AR. The normal viewer target, settings, default
renderer and release packages remain unchanged. Do not promote these modes to
the regular build based on this experiment: additional GPU cost is measurable,
and the live candidate runs show more dropped presents than the baseline.

Interactive same-frame comparison:
`outputs/spatial-upscaling/comparison-extended.html`.
It includes all ten modes (baseline plus nine candidates), a newly acquired
game frame and the previous FHD options frame, with matching crop locations.

The new game frame contains a pause menu over the game world. The character,
stone decoration and rock/wood background are usable for static comparisons;
this does not validate motion stability, camera pans or detailed 3D scenes.
The user confirmed HDMI 1080p60 SDR; the app logs confirm NV12 1920x1080 at 60 fps.

## Implementation and provenance

Official Magpie source snapshot:
`9e8e90274ba07f555bf87f09321dabdac97b1b4f`.
See `third_party/upscaling/magpie/README.md` for retained licenses, files and
upstream links. Model math and weights are unchanged.

| Mode | Implementation | Approximate new image-texture storage at 1080p → QHD |
|---|---|---:|
| RAVU | RGB radius 2, anti-ringing 0.8; one compute pass, two LUTs, final copy | 36.7 MiB |
| FSRCNNX | x2_8-0-4-1; six compute passes, FP32 math / FP16 intermediate storage; Lanczos reduction from 2x | 166.1 MiB |
| Jinc | 16-tap radial two-sine Jinc2 approximation, anti-ringing 0.5 | 7.9 MiB |
| Lanczos | Separable Lanczos3, anti-ringing 0.5; widened support for reduction | 7.9 MiB |

Storage estimates exclude driver allocation alignment, shaders and existing
renderer resources. Anti-ringing strengths are not perceptually equivalent.

The Magpie adapter provides explicit D3D11 bindings and dispatch dimensions.
FSRCNNX uses the pinned runtime's thread remapping and sequential `mad` helper,
not the simplified IDE-only stubs. RAVU DDS loading strictly accepts only the
pinned texture layout and exact payload size. Jinc uses all 16 distinct taps,
not the repeated last-row tap in the pinned reference shader.

All modes retain native-resolution production VP SDR conversion and its
BT.601/709 and Limited/Full choices. Overlays are drawn after scaling; capture
data used by recordings and screenshots is untouched. SDR enlargement from
1x to 2x per axis is eligible; HDR, native size, reduction and larger ratios
use the original renderer. No per-frame allocation/readback or frame queue is
added to `Pipeline::Draw`. The tracing snapshot is a one-off warmup readback,
outside the measured window; tracing is private-build-only.

## GPU cost

RTX 3080, 1080p synthetic pattern, 100 samples per resolution per run, three
runs. Numbers are arithmetic means of the three run means. GPU timestamps
include an output dependency; they are not physical input-to-display latency.

| Mode | QHD GPU ms | 4K GPU ms | QHD synthetic PSNR dB |
|---|---:|---:|---:|
| Baseline VP enlargement | 0.309 | 0.286 | 32.208 |
| SGSR 1 | 0.360 | 0.406 | 25.475 |
| Jinc AR | 0.514 | 0.916 | 32.414 |
| RAVU-Zoom AR | 1.026 | 1.812 | 34.319 |
| Lanczos AR | 1.087 | 2.149 | 33.245 |
| FSRCNNX | 3.383 | 3.153 | 33.288 |

PSNR refers only to the generated test pattern and is not a general game-quality
ranking. Jinc is the lightest new candidate. RAVU has the best score on this
pattern, but visual benefit needs to be judged against its additional cost.
FSRCNNX is the most expensive and was not taken into live latency tests.

Raw measurements: `outputs/spatial-upscaling/extended-benchmark/<mode>/timings.csv`
except FSRCNNX, whose final measurements are in
`extended-nativehelpers/fsrcnnx/timings.csv`. Its earlier benchmark used the
IDE-style helpers and is superseded. A trial precomputing Lanczos weights into
dynamic arrays was slower (QHD about 1.49 ms versus 1.09 ms) and was reverted;
`extended-optimized/` is retained as a rejected experiment, not final evidence.

## Live display events

Capture callback → OS display event, not HDMI arrival → panel emission.
First 3 seconds and final 0.5 second excluded. All app-to-PresentMon matches
were valid (zero unmatched records); every matched present was Composed Flip.

| Mode | Mean ms | p95 ms | Matched presents | Dropped presents |
|---|---:|---:|---:|---:|
| Baseline before | 17.732 | 22.141 | 376 | 3 (0.8%) |
| Baseline after | 19.397 | 24.006 | 375 | 2 (0.5%) |
| SGSR 1 | 17.470 | 24.525 | 373 | 45 (12.1%) |
| Jinc AR | 17.013 | 22.868 | 374 | 24 (6.4%) |
| RAVU-Zoom AR | 18.159 | 24.137 | 365 | 20 (5.5%) |
| Lanczos AR | 17.681 | 22.717 | 374 | 21 (5.6%) |

Dropped refers to PresentMon's Dropped field, not capture loss or missing
recorded frames. Latency statistics include displayed frames only, so a lower
mean with increased dropped presents is not evidence of a latency improvement.
There is one live run per candidate and two baselines; small differences cannot
establish a stable ranking. No additional live sweeps were performed after
this adverse result. Keep the production path unchanged.

Valid data:
- `outputs/spatial-upscaling/extended-game-source/baseline/` (source and baseline before)
- `outputs/spatial-upscaling/extended-game-live/` (candidates and baseline after)

The earlier `extended-live/` set straddled a disconnected-controller message
and user reconnection. It is excluded from the game comparison. The old FHD
options live results are a separate experiment and are not pooled here.

## Regression and recording verification

Final status: 31 selected regression cases passed after environment-only
rechecks. The initial suite passed 29/31; SettingsStoreTests and
ViewerLifecycleTests encountered Win32=5 access-denied errors when saving their
temporary profiles. Both passed when rerun with normal host permissions;
no application/test source was changed to obtain those passes.
Evidence: `outputs/spatial-extended-suite.log`,
`outputs/spatial-extended-settings-recheck.log`, and
`outputs/spatial-extended-lifecycle-recheck.log`.

The suite also covers regular SDR/HDR/VSR rendering, resize/placement/clear,
FPS selection/OSD, settings/cancel, viewer and exclusive-viewer lifecycle,
audio rendering/replay, playback mute, audio-device switching, HEVC sync,
MJPEG, recording mute/scale/color and recording indicators. Passing these
does not negate the separately observed increased live dropped presents.

The expanded GPU regression covers all ten modes, NV12/YUY2, BT.601/709 and
Limited/Full non-neutral colors, every output pixel's opaque alpha, OSD redraw,
no stale overlay on the next frame, centered black margins, F5 bypass/restore,
odd output sizes, native/reduction/HDR policy, Jinc/Lanczos mirror symmetry,
and partial compute blocks (642x362 → 963x543).

The recording integration test renders each mode while recording the same
60-frame input and stereo tone through the original H.264/AAC recording path.
All ten output files decoded successfully with 60 video frames and strictly
increasing video timestamps. Full decoded video/audio SHA256 values match
across all modes:

- Video: `2ef225147e119b3e26bc0de78249164d7cd982bd61d49c0370cd4e895fec5890`
- Audio: `3e109d04aedb74375626714c386ae09782d7cdde1c84658da734423b598987d6`

Evidence: `outputs/spatial-extended-recording-verification.json`;
recordings in the build directory's `spatial-recordings/44240/`.
This verifies unchanged recording content in the fixture, not unlimited
hardware load or every capture device.

The saved settings SHA256 before/after remains
`B6B33F5448F16B4DC3D26DFD78D13B0FF910A436D87A48D05DA89E971474EB98`.
No release, merge, default change or normal application installation was made.

## Reproduction

### NIS sharpness follow-up

`outputs/spatial-upscaling/comparison-nis-sharpness.html` compares NIS
strengths 0, 0.15, 0.3 and the original 0.5 on the same FHD menu/game fixtures.
The private pipeline accepts an initialization-only sharpness parameter;
its default remains 0.5. No settings/UI or normal application path changed.
Zero means the SDK's minimum sharpness, not disabling NIS enlargement.

Reproduce with `SpatialUpscalingTests --nis-capture <1080p-NV12-source> <output>`.
This checks distinct strength outputs, every pixel's opaque alpha, exact
agreement between explicit 0.5 and the previous default, and rejection of
out-of-range/NaN values. Output folders are `nis-sharpness-menu/` and
`nis-sharpness-game/`; logs are `outputs/spatial-nis-strength-{menu,game}.log`.
The normal spatial rendering/recording regressions are rerun in
`outputs/spatial-nis-strength-regression.log`. No new latency claim is made;
the prior live findings remain unchanged.

### SMAA ordering follow-up (quality only)

The user selected NIS 0.15 and requested quality comparison before deciding
whether to measure latency. No GPU benchmark, PresentMon/live capture run or
latency measurement was performed for this follow-up.

`outputs/spatial-upscaling/comparison-nis-smaa.html` shows three side-by-side
results: NIS 0.15 alone, SMAA 1x at FHD before NIS, and SMAA 1x at QHD after NIS.
Menu and paused-game fixtures are the same saved NV12 inputs used above. The
NIS-only BMP in each scene is byte-for-byte identical to the prior 0.15 output.
Both SMAA orderings produce distinct opaque results, not silent fallback.

The official source/LUT commit is
`71c806a838bdd7d517df19192a20f0c61b3ca29d`; provenance is in
`third_party/upscaling/smaa/README.md`. Both use HIGH preset, luma edge
detection, zero subsample indices (1x), and the documented gamma-space
neighborhood blend. No temporal history, predication or jitter. Edge and
weight textures are RGBA8 and cleared per frame; color results use FP16.
The wrapper sets per-pass dimensions/bindings and calls the unchanged official
functions. It does not replace SMAA with a generic blur.

SMAA ordering is an explicit private pipeline initialization parameter, off
by default. It is not exposed in the regular app, persisted settings or live
prototype mode selection. Other scalers reject non-disabled SMAA order.
AA is applied only to the video image; OSD is drawn afterward. No new capture,
recording, frame queue, per-frame CPU readback or shader compilation path.

Reproduce images with:

```text
SpatialUpscalingTests --smaa-capture <1080p-NV12-source> <output-directory>
```

Images: `nis-smaa-menu/` and `nis-smaa-game/`. Logs:
`outputs/spatial-smaa-menu.log`, `outputs/spatial-smaa-game.log`.
Expanded regression covers both orderings, synthetic edges, BT.601/709
Limited/Full flat-color preservation, opaque alpha, cleared stale edges,
unchanged source bytes, OSD redraw, HDR bypass and offset black margins.
Existing scaler/recording regression results are in
`outputs/spatial-smaa-regression.log`. Static screenshots do not validate
motion shimmer or establish a universal quality winner.

### Current-screen CMAA2 / SMAA Ultra quality comparison

The fresh frame in `outputs/spatial-upscaling/aa-current-source/frame.csv.nv12`
shows the game's Graphics and Audio settings, with character, rock and gold
samples. The app received 1920x1080 NV12 at 60 fps and applied BT.709 Limited;
this is not an independent measurement of the HDMI source timing.

`comparison-aa-current.html` compares NIS 0.15 alone, NIS 0.15 then CMAA2
High/color (Extra Sharp off), and NIS 0.15 then SMAA 1x Ultra/color.
The same fresh frame and QHD destination are used in all cases. Previous
High/luma SMAA is also retained as an image link on this same source.
No GPU timing, PresentMon or display-latency benchmark was run for this AA
comparison. Static images do not assess motion shimmer.

CMAA2 uses explicit BT.709 decode/encode around linear processing and the
upstream full-size safe buffer allocation. Its only marked shader adaptation
clamps single-sample source loads at image bounds: the initial upstream
out-of-bounds-zero behavior darkened a flat-color border in regression.
The original license and upstream attribution remain intact.

The focused `--aa-regression` passes all four AA modes across BT.601/709 and
Limited/Full, including flat colors, edge-to-flat transitions, alpha, OSD,
HDR bypass and placement. CMAA2 atomic list ordering produced a maximum
one-code-value difference on two synthetic-image channels in a repeat
check; the test permits at most one code value for CMAA2 only, retaining
exact checks for SMAA. Fresh captured outputs passed exact repeat checks.

Images are in `aa-current-images/`; commands:

```text
SpatialUpscalingTests --aa-capture <1080p-NV12-source> <output-directory>
SpatialUpscalingTests --aa-regression
```

Logs: `outputs/spatial-aa-current-images.log`,
`outputs/spatial-aa-current-focused.log` and
`outputs/spatial-aa-current-regression-final.log`.
Final focused CTest: SpatialUpscalingRegression and SpatialUpscalingRecording
both passed (2/2). These are functional checks, not latency measurements.
The independent NIS strength-sweep 0.15 image matches the new reference
byte-for-byte. Report JavaScript, assets and all region/zoom crop bounds pass.

### Same-frame AA ordering and single-view switcher

`outputs/spatial-upscaling/comparison-aa-toggle.html` adds CMAA2-before-NIS
and SMAA-Ultra-before-NIS to the same frame comparison. Source remains
`aa-current-source/frame.csv.nv12`; no new capture or latency test is used.
New output is in `aa-order-images/`, preserving prior results.
AA-before uses 1920x1080; AA-after uses 2560x1440. NIS stays at 0.15.
Normal app targets/defaults remain outside this private experiment.

Five methods share one canvas. Buttons, number keys, arrows and baseline
toggle preserve crop/zoom, with no cross-fade. All five images preload before
comparison. UI checks cover 90 method/region/zoom combinations, keyboard and
baseline toggling, asset dimensions and NIS-reference hash equality.

Expanded rendering regression includes all six AA modes for range/matrix,
flat and patterned images, source preservation, OSD, HDR bypass and margins.
CMAA2-before exposed nondeterminism beyond rounding on dense synthetic edges
in the upstream collect/expand work-rescheduling path. Both orders now use
the official direct-processing path (COLLECT_EXPAND_BLEND_ITEMS=0), retaining
the one-code-value CMAA2 rounding bound; SMAA retains exact repeat checks.
Temporary intermediate-readback diagnostics and unsuccessful fixed-point/
larger-list-limit experiments were removed. No visual-quality winner is inferred.
Logs: `outputs/spatial-aa-order-build.log`,
`outputs/spatial-aa-order-images.log`, `outputs/spatial-aa-order-regression-final.log`.
The final build without intermediate readback passes both focused CTest suites
(rendering and recording, 2/2). Earlier failed runs remain as diagnostic logs.

### CMAA2-before-NIS 0.15 latency measurement

RTX 3080; FHD60 NV12 SDR input to QHD borderless window. All live requests
used Composed: Flip, not exclusive fullscreen or independent flip.
Three repeats rotated baseline/NIS15/CMAA2-NIS15 order. GPU test used the same
stored graphics-menu frame (180 samples per method per repeat); live capture
showed that same menu with small animation. This is not a moving-game stress test.

| Method | GPU/output-dependent mean ms | Callback to OS display mean ms | Traced displayed / dropped |
|---|---:|---:|---:|
| Baseline VP enlargement | 0.5513 | 14.2534 | 1127 / 0 |
| NIS 0.15 | 1.6259 | 16.3797 | 980 / 0 |
| CMAA2 then NIS 0.15 | 2.3227 | 16.1246 | 951 / 0 |

Values are equal-weight averages of the three run means. CMAA2's incremental
GPU mean versus NIS is 0.6968 ms (per-repeat differences 0.8446, 0.3882,
0.8576 ms). GPU clock/scheduling variation was substantial: CMAA2 total run
means ranged 0.9826–3.0120 ms, so these are not universal processing costs.
CPU submit means were about 0.005/0.013/0.016 ms respectively.

Live CMAA2-minus-NIS differences were +0.5223, -0.3814, -0.9063 ms. Therefore
the -0.2551 ms grand-average difference is not evidence that adding CMAA2
reduces latency. No consistent extra OS-display delay versus NIS was resolved.
Versus baseline, CMAA2+NIS averaged +1.8712 ms (repeat range +1.0207–3.0891).
No additional whole-frame delay was observed in these short runs.

The first 3 seconds and final 0.5 second are excluded. Startup VSR-off HUD lasts
1.5 seconds and is outside the measured interval. All nine steady-state traces
show 60.00 observed frames/s and arrival-gap p95 below 16.72 ms. One CMAA2
present could not be matched within the 0.25-ms trace tolerance and is excluded;
zero dropped presents refers to matched requests, not a guarantee about HDMI
frame delivery. Different counts reflect startup/compilation duration within
the fixed-duration sessions, not a lower steady-state frame rate.

Live timing starts at the capture callback and ends at the Windows display
event. It excludes capture-board/console latency and physical monitor scanout/
pixel response. GPU timing includes an output dependency; CPU completion
measurements in the raw CSV are serialized waits and are not optical latency.
No power setting or normal viewer setting was changed; settings hash matched.

Private runtime presets `nis15` and `cmaa2-nis15` are checked active in logs.
Their GPU-test images match the quality comparison (NIS exactly, CMAA2 within
one code value on one channel). No per-frame environment lookup was added.

Reproduce with `SpatialUpscalingTests --aa-benchmark <NV12> <new-directory>`,
`tools/Measure-CmaaNisLatency.ps1` and `tools/Analyze-CmaaNisLatency.ps1`.
Raw results: `outputs/spatial-upscaling/aa-latency-gpu/timings.csv`,
`outputs/spatial-upscaling/aa-latency-live/round-*/summary.csv`,
`aa-latency-live/combined-summary.json`, and `aa-latency-live/settings-check.json`.

### General prototype commands (all scalers)

Build with `BUILD_TESTING=ON` and `LLCV_SPATIAL_PROTOTYPE=ON`; normal target
does not receive the spatial source or define. The private executable uses
shader source files in the configured source directory and is not portable.

```text
SpatialUpscalingTests --regression
SpatialUpscalingTests --recording <new-output-directory>
SpatialUpscalingTests --benchmark <new-output-directory> <method>
SpatialUpscalingTests --capture <1080p-NV12-source> <new-output-directory>
```

Additional method names: `ravu`, `fsrcnnx`, `jinc`, `lanczos`.
Live sessions use `tools/Compare-SpatialUpscaling.ps1`; analysis uses
`tools/Analyze-SpatialUpscaling.ps1`. Source images for the final HTML were
rendered by the final binary from `extended-game-source/baseline/frames.csv.nv12`
and `fhd-confirmed-1/baseline/frames.csv.nv12` into `extended-game/` and
`extended-menu/` respectively.
