# Video edge-case audit — 2026-09-10

Historical pre-fix audit. Implemented follow-up and current verification:
[VIDEO_HARDENING_VERIFICATION.md](VIDEO_HARDENING_VERIFICATION.md).

Follow-up to `CAPTURE_STATE_REPLAY.md`. This audit covers all four proposed
areas, but does not reproduce the remote user's physical monitor signal loss.
No fix, version increment, installer or release is included in this task.

## 1. Input arrival versus first successful presentation

The existing startup predicate was extracted verbatim into
`StartupPresentationWaitExpired` in main.cpp. The call remains only in the
non-signaled frame-wait branch. No deadline or decision was changed. The actual
predicate is tested at 10,001 timestamps (0–10,000ms) in each of its before/after
successful presentation states, including the exact 3,000ms boundary.

**Confirmed limitation:** `presentedAnyFrame` remains false after
`DXGI_STATUS_OCCLUDED`, even if a valid frame was received/uploaded. A later
non-event wait after the deadline can therefore report no input frame and stop
the graph. With continuously signaled events, the startup guard is not evaluated
at all. This is a conditional false-diagnosis path, not a claim that merely
hiding any running window always stops capture.

The timestamp test covers the actual predicate, not the complete graph loop.
The earlier gated real-callback test covers delayed surface probing independently.

## 2. Selected, active, connected and sample formats

100 simulated negotiations select NV12 1920x1080@120, followed by a deliberately
inconsistent driver's `GetFormat` result of NV12 1280x720@60. Production format
functions return both results without reconciling the earlier size/stride/FPS.
Four correctly declared NV12 row paddings (0/64/128/256 bytes) produce the expected
upload stride. Existing callback tests reject undersized input and accept later
valid input.

**Confirmed missing validation by code inspection:** UnifiedCaptureRenderLoop
reads GetFormat after SetFormat but does not reconcile the raw-video layout.
GetConnectedMediaType is read only in the P010/MJPEG color metadata branch, not
for ordinary NV12/YUY2 layout validation. The mailbox checks actual sample byte
length against the earlier expected image size; undersized samples are silently
discarded. Reading per-sample media-type changes is also absent from this path.

The inconsistent fake driver is fault injection, not evidence that GC573 actually
returned this combination. A complete real DirectShow connection renegotiation
was not simulated. Proposed next step: validate definitive connected layout and
report unexpected changes rather than silently treating them as absent input.

## 3. Settings capability refresh and stale completion

A test-only probe seam (compiled solely under LLCV_GPU_DIAGNOSTICS) replaces
hardware discovery; production ProbePixelFormats still parses the fake COM pin.
Real hidden Win32 controls and PopulatePixelFormatCombo are exercised through
1,000 device/resolution/error/recovery changes. Results, frame selections and
control enabled states are checked. No real device is opened, and nothing is
written to user settings.

**Not supported as a cause:** video capability refresh is synchronous on the
settings caller/UI thread. It does not have a later worker result capable of
overwriting a subsequent video selection. The separate asynchronous internal
capture-audio probe must not be confused with this list.

**Confirmed diagnostic limitation:** a query failure becomes an empty list,
displayed as no supported modes with Start disabled. A subsequent successful
query restores the list and enables controls. Errors and genuine lack of modes
cannot currently be distinguished by that UI.

## 4. Output transitions and live callbacks

The actual WndProc and transition helpers run through 1,000 additional cycles:
five nested transition size notifications coalesce to one generation update;
two identical standalone WM_SIZE notifications each increment the generation.
Actual renderer generation comparisons identify both stale and current states.

**Confirmed inefficiency:** standalone duplicate size notifications request
rebuilds even with identical dimensions. The render loop may coalesce them if
they arrive before its next check, so this is not proof of one rebuild per event.
No evidence ties these events to the user's no-resize blackout report.

An opt-in `AudioCallbackTests --presentation-recreate` uses the actual renderer,
D3D11 debug layer and a real in-process DirectShow allocator, with synthetic
NV12 samples published concurrently through the real callback/mailbox. It
performs 60 alternating Immediate Flip/VSync Flip/Blt initializations with four
window sizes, checks upload/Present and scans each device's debug queue.

Local RTX 3080 result: 60 cycles, 5,628 concurrent samples, **0 debug warnings or
errors**. All 60 Present calls returned **DXGI_STATUS_OCCLUDED** because windows
were deliberately hidden. This checks local resource/API behavior only; it does
not exercise visible scanout, physical monitor transfers, HDR, DP, MPO or the
user's RTX 4070 Ti/monitor combination. Existing failed-Run/Stop lifetime tests
continue to cover in-flight callbacks at graph teardown separately.

## Release implications

- Address first-input versus presentation diagnostics and negotiated raw-format
  validation before advertising this work as a stability fix.
- Preserve enumeration/SetFormat failure details from the preceding audit.
- Duplicate-size suppression is bounded hardening, not a demonstrated signal-loss
  fix; explicit fullscreen/output transitions must still request required rebuilds.
- Real monitor signal loss remains unreproduced. Passing characterization tests
  documents current behavior, including limitations; it does not certify it.
- The delivered portable ZIP is unchanged. This task rebuilt test targets only.

## Execution results

- Full CTest suite: **15/15 passed**, 79.90s.
- AudioCallbackTests, DirectShowVideoFormatTests and LatestVideoSampleTests:
  **10 repeated runs each passed**. Repetition reuses deterministic scenarios;
  it does not represent additional independent hardware environments.
- Manual local GPU recreation replay: passed as described above.
- `git diff --check`: passed.
- Portable SHA-256 remains
  `7C7058A660169EAE2E94CBDB4A26C8B8E54B9C9BC2028AB2422EB72A4A45EC41`.
