# FHD HDMI input re-comparison — 2026-10-24

## Source correction

The user confirmed setting the console HDMI output to 1080p, 60 Hz, HDR off.
This is user confirmation, not a direct HDMI timing query. Each of the 12 app
logs separately verifies NV12 1920×1080 at 60 fps and the requested active kernel.
The original HDMI source was not FHD according to the user, so the earlier
live-1/live-2 images and timings are not pooled with these results.

The game internal render resolution is unknown. Setting its HDMI output to
1080p does not establish a native 1080p game framebuffer.

## Procedure and results

Unchanged prototype executable, GC573, RTX 3080, 2560×1440 output, same window
placement, all analysed presents Composed Flip. Two runs per method:
baseline → Bicubic → CAS → FSR 1 → NIS → SGSR 1, then the reverse order.
Warmup first 3 seconds and final 0.5 seconds excluded. All matched records
were within 0.25 ms of the app's Present QPC; unmatched = 0 in every run.

Values below are the min/max of two run statistics, not confidence intervals.
Timing is capture callback → Windows displayed event, not HDMI-to-photon.

| Method | Mean range, ms | p95 range, ms | Analysed displayed / dropped presents |
|---|---:|---:|---:|
| 기존 확대 | 14.14–14.53 | 18.82–18.97 | 752 / 0 |
| Bicubic | 14.27–14.39 | 18.84–18.99 | 749 / 0 |
| CAS | 14.28–14.56 | 18.83–20.44 | 744 / 0 |
| FSR 1 | 14.29–14.31 | 18.88–18.93 | 741 / 0 |
| NIS | 14.34–14.45 | 18.91–20.41 | 627 / 0 |
| SGSR 1 | 14.29–14.41 | 18.90–20.45 | 746 / 0 |

Total analysed displayed frames: 4359. Zero dropped
presents within those analysed records. Different frame counts reflect the
usable run windows; this does not certify that every HDMI source frame arrived.

No stable latency advantage over baseline was demonstrated. Baseline run-to-run
variation overlaps all candidates' mean results. Earlier GPU microbenchmarks
are unaffected by the HDMI correction, but cannot substitute for these live
display-event results. VSR was not rerun because its neural activation was
unverified in the previous comparison; no VSR speedup claim is made.

## Image comparison

One newly captured source frame from the first baseline run (game options
screen) was rendered through all six paths with the existing capture-fixture
test. The test passed and each requested kernel was active. Images use the
same BT.709 Limited conversion and no brightness adjustment. Text, title/border
and brick texture crops are available in the local HTML. These are still images;
moving-game quality and a ground-truth high-resolution reference are unavailable.

- Preview: `outputs/spatial-upscaling/comparison-fhd.html`
- Exact source: `outputs/spatial-upscaling/fhd-confirmed-1/baseline/frames.csv.nv12`
- All rendered images: `outputs/spatial-upscaling/fhd-captured/`
- App/PresentMon logs, CSV/JSON: `outputs/spatial-upscaling/fhd-confirmed-{1,2}/`

Existing settings file SHA256 remained
`B6B33F5448F16B4DC3D26DFD78D13B0FF910A436D87A48D05DA89E971474EB98`.
No application source or defaults changed during this re-comparison. The prior
31-test regression result is retained, not represented as a new full-suite run.
