# Video settings inline-options regression — 2026-10-23

## Scope

- Retain the existing 1000 × 650 DIP two-column settings layout, sidebar,
  footer, display/VSR and screenshot controls. No scrolling was introduced.
- Place SDR range below pixel format; show MJPEG matrix only for MJPEG and
  HDR controls only for P010. Preserve the existing range migration behavior.
- Add a themed **More / 추가 설정** button beside frame rate. Its local flyout
  contains the original unlisted-FPS and 40fps checkboxes, with their original
  IDs, values and command handlers. Controls are never reparented or duplicated.
- Opening/closing the flyout does not commit settings. Escape closes only the
  flyout; main Cancel retains its previous behavior. Tab changes, deactivation,
  DPI changes and outside commands dismiss the transient flyout.
- Small work areas retain the existing whole-option pages, fixed text size and
  complete access to the frame options instead of a clipped flyout.

## Added coverage

- Actual native control factory: four formats, Korean/English, 96/120/144/192
  DPI, button caption fit, flyout bounds above the footer, retained FPS/range/
  matrix/chroma selections, close/toggle/Escape and page changes.
- Production controller: enable unlisted 30fps inside the flyout, preserve the
  selected rate and startup eligibility, Escape without accepting/canceling the
  main dialog, and Alt+Tab/deactivation without committing settings.
- Updated native forward/reverse Tab order and geometry expectations.
- Idempotent visibility refresh with the flyout both closed and open: no
  repeated enable/disable churn and no bypass of capability gates.
- Native theme snapshots include open/closed Korean/English, light/dark states.

## Verification

- Recording-enabled Release application build succeeded.
- Playback-only Release application build succeeded.
- Non-GPU regression suite: **147/147 passed**, including recording pause/split,
  storage delays, HEVC/AV1 crash recovery, audio-device switches, FPS selection,
  settings save/load/cancel, viewer lifecycle and optional build boundaries.
- Final UI rerun: **1,600 state combinations**, 57 geometry anchors, 24 factory
  profiles, eight compact work areas/scales; all passed.
- Final theme rerun: native state/notifications/Tab order and 32 lifecycle
  cycles passed. GDI **20 → 20**, USER **2 → 2**.
- GPU regression suite: **10/10 passed**, covering SDR/HDR/VSR rendering,
  overlays, resize, MJPEG playback/recording and HEVC timestamp/arrival-clock
  sync. Together with the non-GPU suite: **157/157 tests passed**.
- Evidence: build folder `recording-input-switch-build`,
  files `inline-video-regression.xml`, `inline-gpu-regression.xml`, `inline-final-view.log`,
  `inline-final-theme.log`; visual captures in
  `outputs/inline-video-ui-20261023/`.

## Performance boundary

This change adds only settings-window controls, transient state and UI message
handling. It adds no capture/decoder/renderer/audio/recording work, timers,
workers or queues. Automated rendering/recording regressions are distinct from
a new capture-card end-to-end latency measurement; no new physical latency or
console A/V-sync measurement was performed for this UI-only change.

## Flyout redraw and row spacing follow-up (2026-10-24)

- The production flyout transition was missing the redraw scope already used
  by the preview. Showing the 40fps help button before hiding overlapping
  SDR/MJPEG/HDR fields could expose a disabled/intermediate state. Production
  now batches the full visibility update with `SettingsVisualUpdate`, including
  the final repaint of child controls. No timer or theme rebuild is added.
- Pixel-format label/field now use the same 64-DIP row cadence as capture
  devices, resolution and SDR range: 4 DIP from label to field, 10 DIP from
  the field to the next label. HDR force controls move with that row so their
  6-DIP leading gap is preserved; SDR/MJPEG settings and summary stay in place.
- Added a visible, offscreen production-controller regression, distinct from
  the already-batched preview: 60 open/close cycles across NV12, MJPEG and P010
  assert that help-button show/hide/enable operations occur under redraw
  suppression, no intermediate paint occurs, the final help repaint is queued,
  the pending format is unchanged, and hidden owners remain hidden.
- Added DPI-aware assertions for uniform capture-row spacing, SDR/MJPEG
  baselines, HDR spacing and help-button centering. Updated native snapshots
  are in `outputs/flyout-fix-ui-20261024/` (Korean/English, dark/light).
- Final verification: all recording-enabled build targets and the playback-only
  application built successfully; the focused 31-test settings/FPS/shortcut/
  playback/recording suite passed after rebuilding all affected targets
  (215.14 seconds). UI coverage includes 1,600 combinations and four DPI scales;
  production flyout redraw coverage includes all 60 open/close cycles.
  Evidence: `flyout-fix-all-build.log`, `flyout-fix-final-regression.log` and
  `flyout-fix-final-regression.xml` in the recording-enabled build directory.
  No capture-card latency measurement was needed or performed for this
  settings-only paint/geometry correction.

## Flyout margins and capture-matched display (2026-10-24)

- Frame options now span the capture column (184..560 DIP), with 16-DIP
  internal padding, aligned title/close and checkbox/help rows, and a 48-DIP
  area for the three-line hint. Painting and outside-click dismissal share
  the measured padded bounds; the batched redraw fix remains in use.
- Display resolution adds **Same as capture / 캡처 해상도와 동일** after the
  existing fixed presets. Existing row indices and capture choices stay intact.
  A display-only item marker, distinct from `CB_ERR`, identifies the new row.
- `DisplayMatchesCapture` defaults to false, preserving old profiles and
  startup behavior. The fixed `Resolution` value is retained when automatic
  matching is selected. `EffectiveDisplayPreset` supplies the current capture
  preference to existing window, fullscreen, placement and VSR size policies.
  Manual resize and monitor-relative sizing retain their existing semantics.
- Settings acceptance compares effective display sizes when rebasing relative
  geometry. It uses the new capture preference rather than a previous session's
  latched capture. Diagnostic fixed-resolution experiments explicitly disable
  capture matching for their session without modifying user profiles.
- Added checks for legacy profiles, automatic/fixed save/load, retained fixed
  preferences, cancel, VSR/FPS invariance, no device rescans, bilingual caption
  fit, 48 capture/frame/DPI/VSR window-geometry cases, and fullscreen native
  video-processor rectangles/pixel parity/resource reuse. The new geometry
  test's expected-value multiplication uses 64-bit arithmetic to cover 4K.
- Visual evidence: `outputs/display-match-ui-20261024/`, including matched
  display choices and open frame options in Korean/English, dark/light themes.
- Final verification: the 38-test settings/FPS/shortcut/window/audio/recording
  suite passed (255.03 seconds), followed by all 10 GPU regressions (74.47
  seconds), including SDR/HDR/VSR, overlay, MJPEG/HEVC recording and renderer
  resize coverage. Recording-enabled targets and the playback-only application
  both built successfully. `git diff --check` passed.
  Evidence in the recording-enabled build directory: `display-match-build.log`,
  `display-match-regression.log` / `.xml`, and `display-match-gpu.log` / `.xml`.
  These include synthetic GPU rendering and window-state/geometry tests, not
  a new physical capture-card latency or exclusive-mode hardware measurement.
  No recording/audio hot path, timer, worker, or device query was added.

## 2.1.5 default and legacy migration follow-up

- New AppSettings/profiles default to capture-matched display sizing.
- With no explicit matching-mode key, equal resolved legacy capture/display sizes migrate
  to matching; different sizes stay fixed. Resolve legacy VSR capture size first.
- Explicitly saved fixed/matched mode wins, including fixed mode with equal sizes.
- Tests cover all four resolution pairs, old VSR on/off, first-save persistence,
  explicit fixed mode and malformed flags. Fixed-size policy fixtures select fixed mode explicitly.
- Final release-preparation regression: 20/20 passed (246.40 seconds), including settings
  persistence/controller/UI/theme, window/monitor geometry, SDR/HDR/VSR, resize and package checks.
  See RELEASE_V2_1_5_AUDIT.md for intermediate failures and validation limits.
