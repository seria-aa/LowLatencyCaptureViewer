# Capture capability and startup replay — 2026-09-10

Historical pre-fix audit. Implemented follow-up and current verification:
[VIDEO_HARDENING_VERIFICATION.md](VIDEO_HARDENING_VERIFICATION.md).

## Scope and method

The reported RTX 4070 Ti / GC573 environment temporarily omitted FHD high
frame rates, negotiated NV12 1080p60 instead of 120, and timed out on the
Compatibility Blt path. The log records successful Blt swap-chain creation,
then a 3-second frame wait timeout, then the first surface-capability log.
Later the user reported both restored high-rate entries and no display loss.
That correlation does not establish a common cause.

Tests added here call the production `ProbePixelFormats`, `ConfigureVideoPin`,
`VideoSampleGrabberCallback` and `LatestVideoSample` implementations. Only the
COM driver/sample boundary is simulated. No physical capture device, display,
registry setting or packaged application is changed.

## Scenarios

`DirectShowVideoFormatTests` uses seed 20260910 for 1,000 cycles:

- Explicit NV12 60/120/144/240 modes in shuffled order, duplicates and a
  different-resolution entry: all four FHD rates survive, without duplicates.
- The same fake endpoint returns only 60: the new result contains only 60.
- One randomly chosen high-rate entry fails `GetStreamCaps`: that rate is
  omitted while the remaining entries survive.
- Restore that entry: all rates return on a fresh probe.
- Fail `GetNumberOfCapabilities`, then restore it: empty result, then recovery.
- Stream-config reference count returns to its initial value.

Additional negotiation cases:

- Advertise a 60fps media type with a 60–240fps interval range: the current
  application exposes only 60 and does not attempt 120 when requested.
- Advertise 120 and 60, reject `SetFormat(120)`: 60 is selected.
- Advertise only 60: the same `120 unavailable; using 60 fps` summary appears.
- Restore and accept 120: the requested rate is selected again.
- Reject all candidates or return no modes: configuration returns failure.

These are **characterization tests**, including known limitations, not proof
that silently omitting modes is desirable behavior or that any fix was made.
The fake range is a controlled input; it is NOT a measured GC573 response and
does not establish that every intermediate rate would work on real hardware.

`LatestVideoSampleTests` adds 100 gated callback/recovery cycles:

- Pause the first sample's surface `QueryInterface` after callback entry.
- The render-side event remains unset and the mailbox stays empty.
- In one cycle hold this for 3.1 real seconds; remaining cycles use gates.
- Release the pause: the surface log appears and a valid frame is delivered.
- Empty and short samples do not signal a valid frame; a later correctly sized
  sample does. Surface probing remains one-shot and all sample objects release.

This exercises the actual callback path but does NOT run the complete renderer
startup loop. It demonstrates that the observed log order is possible under
injected probe delay; it does not prove such a delay happened on the user's PC.
The existing failed-Run/Stop tests also show an in-flight callback may be delivered
during graph teardown, so the post-timeout log is not an arrival timestamp.

## Findings and next steps (not implemented)

1. Log count/entry query failures and the raw media-type duration plus interval
   bounds, before interpreting an absent mode as unsupported.
2. Log individual `SetFormat` results so missing and rejected modes are distinct.
3. Record first callback entry, valid sample publication and first presentation
   separately. The current startup guard checks `presentedAnyFrame`, not just
   input arrival; its no-frame message can overstate what is known.
4. Keep optional surface probing/diagnostics from obscuring startup progress.
   Any ordering/thread change requires its own lifetime and hardware review.
5. A longer startup deadline may tolerate a transient delay, but by itself
   neither identifies the delay nor fixes physical monitor signal loss.

## Interpretation limits

Repeated synthetic cases are not independent hardware trials or a confidence
estimate. They cannot model DP link training, monitor firmware, NVIDIA scanout,
MPO transitions, power states or the actual GC573 driver. No physical
blackout/no-signal event was reproduced. No production source or portable
binary was modified during this replay task.

Run the two test executables directly for scenario summaries; run the complete
CTest suite in `build-v1261-test` for the existing regression coverage.

## Execution results

- Both new replay suites passed directly.
- Full existing CTest suite including the new cases: **15/15 passed**, 77.27s.
- Both modified suites repeated with `--repeat until-fail:10`: **10/10 runs
  each passed**, 31.42s. This repeats the same seeded capability inputs (10,000
  capability cycles) and gated callback inputs (1,000 callback cycles), not
  10,000 different driver models. Scheduling of the existing concurrent
  mailbox stress test also varies between runs.
- `git diff --check`: no whitespace errors (existing CRLF warnings only).
- Delivered portable ZIP SHA-256 remains
  `7C7058A660169EAE2E94CBDB4A26C8B8E54B9C9BC2028AB2422EB72A4A45EC41`.
