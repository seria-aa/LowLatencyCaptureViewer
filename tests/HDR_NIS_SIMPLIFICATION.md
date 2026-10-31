# HDR NIS-only path (2026-11-01)

## Decision and evidence scope

Use CMAA2 -> NIS unchanged for SDR. In HDR10 feed the native RGB10 PQ/BT.2020
image directly to PQ NIS, then retain the existing original-reference contour
guard. Do not allocate AA textures, linked lists, indirect buffers or shaders
for HDR. Preserve the saved High/Preserve preference for return to SDR.

The user explicitly accepted the current graphics/audio menu for this decision.
The newly captured frame is bit-identical to the earlier source, **not a second
independent gameplay scene**. Source SHA256:
`495720EAE72343FF2FDCF031755DC24713A24EB73464A5F3951E79D2A5385CD0`.
FHD P010 to QHD RGB10, RTX 3080, sharpness 0.15. Crops include the character,
rock/pot, wall and reported glyph; these do not establish equivalence for every
game, motion sequence, display or GPU. Local capture output reported Windows
SDR; no physical HDR-monitor/scanout latency claim is made.

HIGH + guard compared with NIS + guard, 10-bit PQ RGB channel samples:

| Region | Mean absolute code difference | Edge samples differing by >2 codes | Maximum |
| --- | ---: | ---: | ---: |
| Character | 0.301 | 6.72% | 24 |
| Rocks / pot | 0.180 | 3.14% | 21 |
| Stone wall | 0.112 | 0.58% | 16 |
| Reported glyph | 0.168 | 6.95% | 19 |

Whole-frame HIGH difference: mean 0.135 codes, edge mean 0.216, maximum 27.
The edge mask is a 4x4 local-range diagnostic (>24 codes), not the exact NIS
kernel footprint. Crops use a fixed mapping for inspection, not an HDR display
simulation. Gradient contrast is a proxy, not a percentage quality improvement.

## Why the guard remains

Slider zero is not zero USM. At HDR sharpness zero the SDK config has
`kSharpStrengthMin=0`, `kSharpStrengthScale=0.625`. A test-only mutation sets
**both** to zero without changing PQ thresholds or scaling coefficients.
Reported glyph pixel, NIS without guard:

| Mode | Output luminance |
| --- | ---: |
| OFF video processor | ~26.06 nit |
| Slider 0 | 6.95 nit |
| Slider 0.15 | 3.27 nit |
| Genuine zero USM | 16.42 nit |

Thus USM increases the damage, but scaling-only darkening remains. Removing AA
is a cost/benefit simplification, **not** a root fix to the unguarded NIS scaler.
Keep the total-output bound versus original bilinear PQ: per channel
`reference +/- max(reference*0.02,0.5/1023)` in the existing final pass.
Default sharpness remains 0.15; no setting migration or silent enable change.

## Implementation and test isolation

- HDR skips CMAA2 allocation and dispatch; SDR resources and math stay the same.
- The final HDR triangle binds its own vertex shader, independent of AA/OSD state.
- P010 settings show `NIS (HDR10)` and disable the retained SDR-only CMAA field.
  Returning to SDR restores the name and field without changing any selection.
- HDR HUD/Tab identify NIS, not an inactive CMAA mode. F6 keeps method/strength.
- Former HDR AA/PQ transfer shaders are comparison-only dependencies of
  `CmaaNisTests`, outside the normal app/library shader list.
- `--hdr-regions` compares the legacy AA reference and NIS without repeating
  the GPU benchmark. `--hdr-nis-timing` compares production, legacy AA and a
  manually driven NIS reference; the manual NIS output must match production.
- `--hdr-root` retains the SDK PQ config when disabling USM. The fixture-specific
  assertion is intentionally for the source hash above, not arbitrary captures.
- All test entry points use the compile-time isolated test profile. No user
  settings load/import/save and no normal application executable replacement.
- Recording remains SDR/stereo; HDR recording was not added. Capture input,
  raw screenshots/recording, audio and OSD composition order are unchanged.

## Local evidence

Generated, optional diagnostics (not repository/package dependencies):

- `outputs/hdr-nis-decision-20261101/root/root.csv`
- `outputs/hdr-nis-decision-20261101/accepted-frame-regions/{regions.csv,guarded-aa-difference.csv}`
- Earlier timing folders, including `outputs/hdr-nis-timing-20261101/gpu.csv`
  and `product-timing/`, used an 8-bit completion texture with RGB10 output.
  They are superseded by the format-correct results below, especially for the
  OFF video-engine dependency. No old OFF overhead estimate is carried forward.

## Final checks

The product/UI build succeeded. The 21-case gate passed, 0 failures (309.59 s),
including SDR/HDR/RGB upscaling, hardware/WARP boundary safety, settings/UI,
F6/VSR, resize, OSD, screenshots, recording, cancel and audio-device switching.
Evidence: `build-v2200-release/hdr-nis-simplification.xml`.

After adding the timer's real-RGB10-pixel check, the latest rebuilt
`CmaaNisRegression` and `CmaaNisHdr` also passed (19.09 s), recorded in
`build-v2200-release/hdr-nis-final-diagnostic.xml`. An intermediate test-only
failure retained the timer's old-device resources across same-window swapchain
recreation; the timer is now scoped/released before that transition. Product
code and the delivered UI EXE were not changed by this diagnostic correction.

All 14 SDR shader headers are byte-identical to the pre-change
`build-v2190-release/generated/upscaling/` artifacts. The optional Oct-25
`sdr-text-diagnosis-03/cmaa-nis15.bmp` reference still fails its strict <=1 code
assertion: maximum 5, 1,033 RGB channels >1, **all within the outer 8 pixels**;
the interior is exactly identical (maximum 0). It predates the committed Oct-26
NIS boundary fix (`d1fd5b7`). The assertion was not relaxed and the reference
was not overwritten. This historical artifact is not used as a passing
whole-frame baseline for the current product.

The HDR timer now allocates a completion pixel in the **actual RGB10 output
format**, checks exact format/sample compatibility before the copy and tests
the returned pixel against the same rendered frame. D3D11 requires compatible
copy formats: [CopySubresourceRegion documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion).

Format-correct final product timing: 8 rotating rounds, 120 samples per mode
per round, continuous 500 ms warmup, no competing GPU regression test. GPU
upload/VP/filter/completion-copy time, not Present/scanout/physical latency:

| Mode | Mean ms | Median ms | p95 ms |
| --- | ---: | ---: | ---: |
| OFF | 1.1311 | 0.8909 | 2.0326 |
| Production NIS + guard | 1.3148 | 1.0721 | 2.2340 |
| Legacy HDR CMAA2 -> NIS + guard | 1.5286 | 1.3084 | 2.3951 |
| Manual NIS + guard reference | 1.3298 | 1.1159 | 2.2319 |

Mean saved vs legacy AA: ~0.214 ms, paired round differences 0.113–0.263 ms.
Variability is substantial; this is one source/GPU, not a universal or physical
display-latency guarantee. Production vs manual NIS is **bit-identical**,
glyph pixel 24.1297 nit, zero contour-bound violations over the saved frame.
Evidence: `outputs/hdr-nis-decision-20261101/product-timing-format-correct/`.

Actual settings hashes were preserved:
`settings.ini = ED5259CD8E169E212D27A886C6B21B5BA6FE621F4FE79877E828AC4FF0BF53F8`,
`recording-prototype.ini = 2D1C8F5D136F8A812E78C5558C32C9BC690751923AAB20D8A77A0BEB144C406A`.
The normal release EXE was not replaced. Fresh isolated UI/full-viewer package:
`outputs/LLCV_HDR_NIS_Test_20261101.zip`.

## Follow-up: screenshot keyboard order

A subsequent external full-suite review found that the screenshot group had
moved to App settings visually, but remained in the Video sibling/reading order.
The earlier 21-case gate did not include SettingsThemeTests/RecordingThemeTests.
Move the entire screenshot group after Updates in the factory order; do not
change geometry, settings, shortcuts or rendering/recording code.

Update the native Tab/Shift+Tab expected sequence accordingly. Expand it to
328 Korean/English/DPI/page/format/upscaler profiles, including disabled HDR
CMAA correction and ordinary scaling fields. Check static reading-group
adjacency as well as forward/reverse/wrap focus movement. This verifies native
ordering, not a claim of testing every screen reader implementation.

The deferred-paint fixture also still selected Video before manipulating the
now-hidden screenshot checkbox. Start it on App settings, explicitly require
visibility, retain the 50-to-1 paint assertion and include App in the 50 repeated
page transitions. Do not relax the painting or lifecycle checks.

All 15 selected regression tests passed, 0 failures (245.85 s), including the
previously failing SettingsThemeTests/RecordingThemeTests, SettingsViewTests,
SDR/HDR upscaling/playback, VSR/F6, OSD, recording, settings cancellation,
playback mute, capture-audio device switching and test-profile isolation.
This is a targeted rerun, not a claim that all 199 tests were rerun here.
Evidence: `build-v2200-release/screenshot-tab-order-regression.xml`.
The actual settings hashes matched this follow-up's start/end values:
`settings.ini = 772A6BD518A5DB4C2D9D94F70C30B49E4491465CB8786C4AFF326D088B03328B`,
`recording-prototype.ini = 2D1C8F5D136F8A812E78C5558C32C9BC690751923AAB20D8A77A0BEB144C406A`.
The updated isolated package is `outputs/LLCV_HDR_NIS_TabOrder_Test_20261101.zip`.
