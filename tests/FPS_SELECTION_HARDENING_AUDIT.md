# Capture FPS selection hardening — 2026-10-22

Working-tree fixes on top of v2.1.4 (`797c351`); not a published release.
The reported LS/100+ FPS symptom has not been reproduced on the reporter's hardware.
The defects below are established from code and synthetic driver/UI tests, not
asserted to be the root cause of that individual report.

## Scope and changes

| Area | Confirmed weakness | Change |
| --- | --- | --- |
| Explicit FPS | Ordinary explicit rates could silently fall back; Auto format could select NV12 120 instead of available YUY2 60 | Require the requested rate for every explicit FPS, validate driver readback and connected media type, keep Auto FPS fallback unchanged |
| Direct requests | The strict check and permission to request absent timings were coupled | Separate strict verification from unlisted permission; only opted-in 30 and the existing 40 override request unlisted timing |
| Settings refresh | An absent selected FPS or format became Auto, including after a failed query | Preserve the choice, mark unavailable and block Start until the user changes it or capabilities recover |
| Start guard | Nonempty capabilities alone could permit Auto with only manual HDR/MJPEG modes | Check the selected format/rate combination; audio-only and existing output verification retain their own rules |
| Actual delivery | Driver metadata could claim the target while callbacks arrived faster | Observe existing input counters on the existing UI timer; sustained overdelivery produces a diagnostic/log warning, not a throttle |
| Failure cleanup | Failed color-capability enumeration could retain an allocated media type; negative capability count was not rejected consistently | Release failed entries and reject invalid counts before attempting configuration |

Settings storage accepts normal legacy numeric 30/60 values unchanged. The strict
numeric parser is retained; malformed strings are not reinterpreted as valid rates.
F2 retains its requested default-settings behavior. No speculative LS-specific
window or process lifetime change was made.

## Added regression coverage

- 132 explicit rate/format/resolution combinations, competing Auto formats,
  driver refusal, silent timing substitution, changed size/format on readback,
  driver interval support, native fractional rates and accepted color metadata.
- Selection persistence through changed/missing formats, failed/recovered
  asynchronous queries, unavailable choices, Enter/Start guards, explicit user
  re-selection, 40 fps override and unlisted-request opt-in.
- Saved FPS across resolutions, formats, legacy profiles, 40 fps override and
  independent display size; malformed values and subsequent deliberate re-selection.
- Dynamic media-type FPS changes across NV12/YUY2/P010/MJPEG, including rejection
  latching and acceptance of equivalent 29.97/59.94 timings.
- Deterministic actual-rate observation: grace period, sustained excess/recovery,
  jitter, bursts, no input, counter reset/wrap, timer stalls and Auto mode.
- Failed enumeration allocation ownership and invalid count handling.

## Verification

- Recording-enabled Release build: all configured targets compiled and linked.
- Focused checks passed for negotiation, mailbox/callbacks, settings controller,
  recording-settings cancellation and the rate monitor. SettingsStoreTests first
  failed with access denied under the restricted runner; rerunning with temporary
  file/security-descriptor access passed (12.78 seconds). This is recorded separately,
  not treated as a product fix or a passing first run.
- Full non-GPU run: 144/146 passed in 803.55 seconds. The two failures were runner
  setup errors: `OptionalBuildSyntaxTests` lacked the Windows SDK include environment
  (`windows.h`), and `ReleasePackageTests` could not find `cmake` on PATH.
- After restoring the MSVC/CMake environment and rebuilding the supplementary test
  changes, all 12 final checks passed in 43.03 seconds, including both failed items,
  the real asynchronous format rebuild fixture and failed-allocation ownership tests.
  Across the complete run and these final reruns, all **146 distinct non-GPU checks**
  have a passing final result. This is not a claim that the first full run was clean.
- Normal recording-enabled, recording-disabled and audio-recovery-diagnostic product
  targets all compiled and linked. No version change, installed-app replacement,
  publication or merge was performed.
- `git diff --check` passed. Source changes were independently reviewed after integration.

Evidence is retained in `outputs/fps-hardening-*-20261022.log` and the regression XML.

## Performance and validation limits

No video callback, render loop, encoder, audio processing or recording queue was
changed. No pacing sleep, software cap, additional frame copy or per-frame log was
introduced. Extra driver readback happens during startup; rate monitoring reuses
existing counters on the existing 500 ms UI timer and logs only state transitions.

The actual-rate warning does not enforce a software FPS cap or repair a driver that
lies about delivery. It does not measure LS-generated frames. This pass does not
measure live HDMI/display latency, physical AV sync, capture-card compatibility or
LS operation across an F2 process/window replacement. Ten GPU-labelled checks were
excluded, not counted as passes: VsrExperimentTests, VideoClearRegression,
MjpegRecordingPlaybackTests, HdrPipelineTests, HdrVsrPipelineTests, SdrPipelineTests,
OverlayUiTests, RecordingHevcSyncTests, RecordingHevcArrivalSyncTests and RendererResizeTests.
Live hardware validation remains separate from this regression suite.

## Follow-up: FPS warning visibility and layout

Two display regressions were independently reproduced after the initial review:

- Changing from an unsupported FPS to a valid FPS, switching tabs, and selecting
  the retained unsupported FPS could leave Start disabled with its explanation
  hidden. The FPS-change handler now refreshes visibility and compact layout as
  well as text, matching the adjacent format/40 FPS handlers.
- Appending a warning beneath the fixed diagnostics panel increased its height
  from 400 to 420 pixels (414-pixel clip area, including the VSR line). The warning
  now stays on the existing actual-FPS line. All other diagnostic text and the
  panel dimensions remain unchanged. Auto FPS and audio-only suppress stale
  manual-rate warning flags.

Regression coverage uses the production settings message handlers, hidden child
window visibility, Start state, tab roundtrips, and compact-page alignment/bounds.
`CaptureFrameRateOsdTests` uses the production text builder, bundled fonts and
CPU-only DirectWrite for 160 Korean/English FPS/warning/VSR combinations. It checks
the complete text bounds, final warning glyph, line count and unchanged non-warning
diagnostics; maximum measured height is 400/414 pixels. Both added checks failed
before the display fixes and passed afterward. An initial OSD fixture build used
the wrong Auto preset expectation; that test setup was corrected before collecting
the pre-fix failure evidence.

The v2.1.4/current automatic-negotiation differential was rerun for 30,080 synthetic
device cases: selected format/FPS, layout, request ordering and query counts remain
byte-identical. This display follow-up does not modify negotiation, frame callbacks,
audio processing, recording, timer cadence or overlay caching. FPS UI changes do
not initiate hardware probes or restart capture.

Evidence: `outputs/fps-display-fix-before-20261022.log`,
`outputs/fps-display-fix-focused-20261022.{log,xml}`, and
`outputs/fps-auto-recheck/{baseline-results,working-results}.txt`.

Final follow-up validation: **53/53 checks passed, no skips** (321.45 seconds),
including settings layout/theme/controller, recording settings/shortcuts, viewer
and exclusive-mode lifecycle, capture-format faults/stress, audio device switching,
playback mute isolation, recording pause/split/storage delay, HEVC/AV1 container
recovery and independent decode, MJPEG/NV12/YUY2 forced-exit recovery, and build/
package boundaries. Normal recording-enabled, recording-disabled and private audio
diagnostic product targets all compiled and linked after these display changes.
Results: `outputs/fps-display-fix-regression-20261022.{log,xml}`.

No live HDMI/display-latency or external LS measurement was added by this follow-up;
synthetic delivery, decode/sync and layout checks are not a substitute for those.
