# LLCV 2.1.5 release preparation

## Status

Prepared on 2026-10-24 from v2.1.4 (`797c351`) plus the current working-tree fixes.
This is the pre-publication validation record; release and merge were authorized on 2026-10-24.
Publication follows the commit containing this record. No installed application or user settings were replaced.
Version metadata, packaging defaults and Korean/English guides target 2.1.5.

## Scope

- Strict explicit capture FPS negotiation/readback, retained unavailable selections and sustained actual-rate warnings.
  Auto selection remains unchanged; this does not cap LS-generated frames.
- Shared SDR Auto/Limited/Full interpretation for playback, screenshots and recording; no HDR override.
  Legacy MJPEG corrections remain compatible; matrix and range controls are separated.
- Scroll-free inline frame options, batched redraw and consistent margins.
- Capture-matched display sizing in windowed/fullscreen modes.
  New profiles follow capture. Legacy profiles without an explicit mode migrate to matching only when
  resolved capture/display sizes are equal (or no display preference exists).
  Different legacy sizes stay fixed; explicitly saved fixed/matched modes win, even when sizes are equal.
  Migration occurs after legacy VSR capture resolution is resolved.
- Asynchronous periodic free-space queries on the existing storage worker, bounded delivery queues
  using the existing pixel-buffer budget, and full stored-frame verification.

## Evidence before release metadata/default migration update

- Latest recording follow-up: 110/110 tests passed, no skipped cases, 607.72 seconds.
- Independent strict video/audio decoding: 36 scenarios, 46 files, 7,389/7,389 decoded frames.
- Includes H.264/HEVC recording, MJPEG/NV12/YUY2 input, gain/mute isolation, device switching,
  pause/split, space/Flush/Close delay, low space, recovery and eight GPU-labelled tests.
- Earlier final recording-fix verification: 165 distinct tests have passing final results.
  This is a union of the 164-test run and follow-up runs, not one clean 165-test invocation.
  Two initial failures were missing compiler/CMake runner environment and passed on corrected reruns.
- Earlier UI/display verification: 38 focused tests and 10 GPU tests passed.
- FPS Auto differential: 30,080 synthetic device cases matched the v2.1.4 baseline.

Local evidence:
`outputs/hevc-followup-review-20261024.txt`,
`outputs/hevc-followup-decoded-20261024.json`,
`outputs/hevc-recording-regression-20261024.txt`,
`tests/FPS_SELECTION_HARDENING_AUDIT.md`,
`tests/VIDEO_SETTINGS_INLINE_UI_AUDIT.md`.
These records describe the tested revision; earlier notes describing matching as opt-in are historical.

## Release preparation gates

- Version/default migration rebuild: succeeded for all configured targets; executable FileVersion/ProductVersion are 2.1.5.
- Final targeted regression: 20/20 passed, zero skipped, 246.40 seconds; five GPU-labelled checks included.
  Covers settings UI/theme/controller, saved-profile migration (four resolutions, equal/unequal sizes,
  legacy VSR, first save and explicit fixed preference), window/monitor placement, exclusive lifecycle,
  recording-settings cancellation, SDR/HDR/VSR rendering and resize.
- PackageDocsTests and ReleasePackageTests passed, including document targets and temporary install-stage version/payload validation.
  Final evidence: recording-input-switch-build/v215-final-build.log, v215-final.log and v215-final.xml.
- Intermediate runs were not clean: an old fixed-preset fixture assumed matching was disabled by default;
  it now explicitly selects fixed mode. A settings-loader object built during the migration edit retained
  the earlier behavior; rebuilding all consumers from the final source resolved the mismatch.
  Standalone SettingsStoreTests and the final complete 20-test run both passed afterward.
- Distributable ZIP/installer generation and verification: completed in the final release gate below.
- Publication/merge authorized by the user after preparation.
- Preserve existing dirty changes; explicitly review/stage source and tests, not all of outputs.

## Final release gate (2026-10-24)

- Clean Release rebuild of all 671 build steps succeeded. Normal recording is enabled;
  audio-input recovery diagnostics, HDR audit/scrgb prototypes and VSR experiment builds are disabled.
- Full 165-test invocation: 164 passed, one failed, zero skipped (1,189.60 seconds).
  `VideoClearRegression` still assumed the old fixed-display default while selecting 720p.
  It now explicitly selects fixed mode; its pixel/OSD/border comparisons are unchanged.
  This final adjustment changes only the test fixture, not the application.
- Rebuilt and reran 14 relevant regressions, including all 10 GPU-labelled checks:
  **14/14 passed, zero skipped, 82.44 seconds**. Every one of the 165 registered tests
  has a passing final result across the full run and this follow-up; the first full run was not clean.
  A restricted intermediate follow-up reported settings-save access denied (5). The same
  settings test passed in the full run and again with normal temporary-file/security access;
  no settings implementation change was made for this runner permission failure.
- Independent strict decoding of final-run recordings: **36 scenarios, 46 files,
  7,389/7,389 video frames**, no video/audio decode errors.
- Portable ZIP independently extracted and checked; installer input manifest and extracted ZIP
  match for all **84 files**. Both contain the tested application and required runtime/licenses/docs;
  no settings, diagnostic logs or test executables are shipped.
- Application FileVersion/ProductVersion: 2.1.5; installer FileVersion: 2.1.5.0.
  Application SHA-256 is unchanged after the test-only correction:
  `0A6AE557D4334ED522479DCB3DEABC75A6F11ED598B3ACA98AE83D559204A304`.
- Release asset SHA-256:
  - Setup: `E7F8E7EEF0CA0DA8C64A30F1A252C98818C846A51CA0BA4C671FEC172B0C3829`
  - ZIP: `E7114A8980F1F03F2D95171A2096FFAC366CD248A59E0E0BB2DDE90C38803092`
- Evidence: `recording-input-switch-build/v215-release-clean-build.log`,
  `v215-release-full.log/.xml`, `v215-release-followup.log/.xml`, and
  `outputs/v215-validation/{full-LastTest.log,full-decoded.json,assets-check.log,followup-restricted.log}`.

## Limits

No new physical HDMI/display latency, calibrated HDR, real console AV-sync or physical
exclusive-fullscreen transition measurement. Synthetic GPU/decoder tests do not establish these.
Long storage stalls can still exhaust bounded queues or delay finalization. Crash recovery
preserves completed fragments, not unwritten tails or arbitrary disk failures.
Delay-injection coverage currently lacks a separate invocation-count assertion.
Queue tests exercise real producer code plus a manual consumer; integration tests run the actual
worker/encoder/storage, but do not force every saturated-queue interleaving.
Existing screenshots remain accurately labelled as 2.1.3 examples, not new captures.
