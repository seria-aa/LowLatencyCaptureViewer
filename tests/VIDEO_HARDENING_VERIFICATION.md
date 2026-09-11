# Video hardening verification — 2026-09-10/11

This follow-up implements bounded fixes from `CAPTURE_STATE_REPLAY.md` and
`VIDEO_EDGE_CASE_AUDIT.md`. Those files preserve the pre-fix observations.
This is a local working-tree change, not a release or a confirmed fix for the
remote user's physical monitor reporting “no signal”.

## Changes and matching checks

| Finding | Change | Verification |
| --- | --- | --- |
| Capability-query failure looked like unsupported hardware | Preserve whole/partial query errors, expose them in settings, log entry/count failures | Real settings controls with fake COM capabilities: 1,000 device/resolution/error/recovery cycles; separate successful-empty, failed and partial results |
| Interval-supported frame rates were omitted | Include common rates within the driver's reported interval; attempt the requested rate while retaining the original media-type fallback | Seeded capability replay, interval-only 60–240 fps, requested-rate rejection/fallback, recovery and duplicate filtering |
| First callback could block on unused VRAM probing/logging | Remove the unused interface query and log I/O from the callback | Actual callback/mailbox tests, gated producer recovery and concurrent ownership/teardown tests; no optional interface query |
| Occlusion could be diagnosed as no input | Track first valid input independently of first successful presentation; drain a pending sample before checking the deadline | Startup predicate at 20,001 timestamps in both input states; valid input plus occlusion does not expire |
| Three-second startup allowance was short and ambiguous | Allow ten seconds for the first valid input; report stage, rejected samples and first input/presentation separately | Deadline boundary tests and existing delayed-publication replay; this is not a ten-second playback buffer |
| Advertised, active and connected layouts could disagree | Validate active and final connected dimensions/subtype/payload/stride before capture starts; use final FPS and mailbox size | 100 inconsistent active-format negotiations, padded NV12 rows, NV12/YUY2/P010/MJPEG boundaries, malformed sizes/heights/null payloads |
| Strict validation could reject omitted metadata | Derive absent raw `biSizeImage`; retain negotiated FPS when connected timing is zero | All four input subtypes; zero timing with/without a known rate and negative timing |
| Repeated identical size notifications requested redundant rebuilds | Ignore duplicate client dimensions while preserving explicit fullscreen/output transitions | Actual window procedure and transition helpers through 1,000 nested/duplicate/changed-size cycles |

Capability entries derived from an advertised interval are not a hardware
certification. The capture driver's actual `SetFormat` result still decides
whether a requested rate is accepted, and individual results are now logged.
Unexpected resolution/subtype changes fail clearly instead of feeding an old
layout into the renderer. Automatic mid-stream format renegotiation is not added.

## Performance and size

- No audio source changes, extra PCM buffering, new production threads, new
  runtime libraries, or new per-frame video conversion passes in this follow-up.
- Layout checks and detailed format logs run during discovery/startup, not for
  every frame. Only rejected samples increment the new rejection counter.
- The normal callback no longer performs its previous optional VRAM probe.
  A standalone `/O2` comparison uses the actual callback/mailbox and an in-process
  DirectShow allocator. Each run measures seven rounds of one million valid
  callback calls after warm-up; runs are ordered before/after/after/before.

| Run | Minimum ns/call | Median ns/call | Maximum ns/call |
| --- | ---: | ---: | ---: |
| Before 1 | 321.4 | 343.0 | 360.6 |
| After 1 | 314.6 | 315.3 | 353.8 |
| After 2 | 315.0 | 315.9 | 350.5 |
| Before 2 | 324.3 | 328.0 | 329.7 |

No callback slowdown was observed. This single-producer microbenchmark is not
an end-to-end capture/display latency measurement or proof of a general speedup.
Benchmark executables and baseline copies remain only in the ignored build
directory; they are not application dependencies or package contents.

The local Release EXE is 621,568 bytes versus 613,376 bytes in the previously
delivered test portable: **+8,192 bytes (8 KiB, about 1.34%)**. This comparison
includes the accumulated verification/hardening work since that portable.

## GPU and regression scope

A clean Release build completed successfully. The complete CTest suite covers
audio callbacks/modules, Shared replay including extended and 25 ms cases,
capabilities, formats/color, settings, geometry, logging, updates and histories.

- Final complete suite: **15/15 passed**, 98.86 seconds.
- `AudioCallbackTests`, `DirectShowVideoFormatTests` and
  `LatestVideoSampleTests`: **10 repeated runs each passed**, 31.47 seconds.
  Repeats reuse the seeded cases; they are not independent hardware trials.
- `git diff --check`: passed.

The opt-in `AudioCallbackTests --presentation-recreate` passed 60 actual local
D3D11 renderer rebuild/upload/Present cycles with 5,480 concurrent synthetic
NV12 samples and zero debug-layer warnings/errors. All 60 presentations were
occluded because the test windows were hidden. This checks resource/API and
callback lifetime behavior, not visible scanout latency or monitor signaling.

These tests cannot reproduce DP link training, MPO/Independent Flip, monitor
firmware, live HDMI signal changes, the remote RTX 4070 Ti configuration, or a
multi-hour physical-device session. MJPEG/P010 parsing tests are not physical
capture/HDR playback validation. The intermittent monitor failure remains
unreproduced and its root cause is not established.

## Artifact safety

No commit, upload, release or installer replacement was performed. The existing
portable ZIP remains unchanged, SHA-256:

`7C7058A660169EAE2E94CBDB4A26C8B8E54B9C9BC2028AB2422EB72A4A45EC41`
