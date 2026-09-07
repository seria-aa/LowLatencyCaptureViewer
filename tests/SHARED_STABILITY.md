# Shared audio stability verification

These are development tests, not installed application assets. No driver,
capture device, system audio setting, or persisted user setting is touched.
This records the development stages leading to v1.2.6. Test runs themselves
do not publish releases; the final versioned-build check is recorded below.

## Reproduce

Use an x64 Visual Studio developer command prompt, with UTF-8 output enabled
**before configuring a fresh Ninja build directory**:

```bat
chcp 65001
cmake -S . -B build-shared-stability -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-shared-stability
ctest --test-dir build-shared-stability --output-on-failure
build-shared-stability\SharedReplayTests.exe --long
build-shared-stability\SharedReplayTests.exe --extended --long
```

On the Korean MSVC installation used for verification, configuring under a
different code page previously corrupted Ninja's localized `/showIncludes`
prefix. That silently missed included-header dependencies. A fresh UTF-8
configuration restored dependency tracking. Do not rely on an older build
directory with a corrupted prefix for regression verification.

## What is tested

- `AudioCallbackTests`: actual application callbacks, Shared re-prime boundaries,
  warmup, diagnostic delivery to English/Korean OSD, ASIO notification behavior,
  and bounded device-recovery policy. Seven continuous-clock controller tests
  each simulate ten hours at -800/-300/-100/0/+100/+300/+800 ppm.
- `SharedReplayTests`: includes the actual application PCM callback with a virtual
  wall clock. The real ring, resampler and mixer process 48 kHz stereo samples.
  Device padding/consumption and capture packet arrivals are simulated.
- The 18 replay cases cover Auto/Off/On, 15/20/25 ms targets, 10/20 ms input
  packets, 10/2.667 ms output periods, positive/negative clock mismatch, delayed
  packets, lost packets, a 40 ms output-thread stall, and repeated arrival jitter.
  Normal CTest limits each case to 240 virtual seconds; `--long` uses the declared
  2/10/30/60-minute durations (5 hours 12 minutes combined simulated playback).
- Assertions distinguish zero-loss reference conditions from fault injections.
  An injected output stall **must** produce output-deadline evidence even if the
  PCM callback supplies every requested frame. It must not inflate PCM underruns.
- A too-small 15 ms target is not required to become lossless; the test requires
  that isolated shortages do not cause additional full blocks of re-prime silence.
- At -800 ppm, the explicitly documented Auto cold-start allowance is one missing
  block fragment of at most 1 ms in the first 30 seconds, then no recurring loss.
  This is a known boundary, not a claim of lossless playback for every device.

## Changes and tradeoffs

Shared uses proportional queue control plus an integral **shortage** bias,
with anti-windup and bounded ±1000 ppm output. The integral term is non-positive:
negative clock mismatch no longer consumes reserve at steady state, while positive
clock mismatch retains the original proportional controller's extra headroom.
A symmetric PI prototype regressed the positive-clock arrival-jitter replay
(legacy 0 losses versus 180 tiny losses in 240 seconds); it was rejected.
Filter/controller response scales with the output block duration. A fixed
16-frame (0.333 ms) DSP reserve covers fractional source demand; the user setting
is not increased adaptively. Actual packet-quantized queue depth still fluctuates.

Auto remains latched once enabled. When only one output block remains, it can
engage without the additional five-second observation, initially slowing source
consumption conservatively. Exclusive and ASIO retain their existing controller.

After at least one output block is missing (possibly accumulated over consecutive
short writes), Shared waits for the configured reserve before resuming. A single
small loss does not trigger that wait. This can add a brief intentional silence
after a genuine starvation; it does not make missing input reappear. Ring data,
resampler history and the learned clock rate are retained, without target growth.

Output deadline monitoring samples QPC and pending output frames. Empty padding
alone is not an error; exceeding the previously available time by more than 2 ms
is reported as **suspected scheduling delay**, not measured audible loss. Slow
work between padding observation and buffer release is covered too. The basis
for pending-frame accounting is Microsoft's
[GetCurrentPadding contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getcurrentpadding).

New audio-thread notifications use cumulative atomics. The UI timer coalesces
changed diagnostics at most once per second; it performs the file/console writes.
There is no new worker, timer, polling output path, unbounded event queue, runtime
dependency, or per-callback diagnostic allocation. Existing Exclusive diagnostics
remain outside this Shared-specific change.

## Reading the new log

`[audio][shared] health` reports independently:

- `output-deadline-suspected`: output scheduling/queue-exhaustion evidence;
  `last-output-overdue` is a scheduling estimate, not missing audio duration.
- `PCM-underrun` / `PCM-missing`: source samples unavailable during rendering.
- `PCM-overrun`: input queue overflow.
- `reprime` / `reprime-silence` / `refilling`: recovery incidents and intentional
  silence frames. Always include these when judging total audible interruption;
  fewer underrun events alone does not demonstrate a shorter gap.
- Current queue, target, capture packet, output padding and correction values.
  These are deferred snapshots and need not match the precise incident instant.

OSD distinguishes re-prime, recent suspected output delay and PCM overflow without
adding a new line. The output-delay warning is not proof that the driver failed.

## Limits

Synthetic tests do not validate physical-device DPC latency, Windows mixer/APOs,
USB traffic, endpoint notification timing, audible quality, or an affected user's
exact cause. Real input stalls longer than available reserve still cause gaps.
Twenty-millisecond capture packets or a 15 ms queue target can still need more
reserve. Correction Off can retain extra queued latency after a delayed burst.
No claim of universal dropout elimination or physical multi-hour validation is made.

## Instrumentation and cost checks (2026-09-07)

Both application-callback and 18-case short replay tests also completed under an
MSVC AddressSanitizer build without a reported memory error. **Coverage is
limited:** this machine's sanitizer failed in `interception_win.cpp:171` before
test entry under default settings. The run required the process-local
`ASAN_WIN_CONTINUE_ON_INTERCEPTION_FAILURE=1` workaround. This is supplementary
evidence, not a clean full-interceptor sanitizer certification. The ordinary test
and application builds have no sanitizer dependency or workaround.

A separate optimized microbenchmark of queue control plus the two new QPC timing
probes measured about 45 ns per callback here (about 43 ns above the legacy control
calculation), over three one-million-iteration passes. This is **not** a benchmark
of the entire renderer, real driver scheduling, audible latency, or other machines.
The only intentional steady-state extra queue allowance is the 16-frame DSP guard;
starvation re-prime is a separate, explicitly logged recovery tradeoff.

## Extended verification (2026-09-07)

`SharedReplayExtended` is an additional CTest entry (13 entries total). It
executes `SharedReplayTests --extended`: 26 cases capped at 240 virtual seconds,
plus six 180-second recovery boundaries. `--extended --long` uses 600 seconds for
the first 26 cases: 4 hours 38 minutes combined virtual playback. The six boundary
cases can also be reproduced separately with `--boundaries`.

The extension runs the actual callback, ring, mixer and resampler. A separately
advanced source clock generates 10 ms packets. Deterministic xorshift32 seeds
1, 0x12345678 and 0xdeadbeef add 0–8 ms FIFO-preserving delivery jitter. The output
engine consumes independently at 10 or 2.667 ms. Clock profiles reverse between
+/-300 or +/-800 ppm every minute, or ramp between +/-300 ppm over two minutes.
Repeated 40 ms input/output stalls occur at 60 seconds and every 120 seconds
thereafter, leaving at least 60 seconds after the last injected interruption.
Input pauses test both delayed delivery and discarded packets, with Auto/Off/On.
Boundary tests discard packets generated within 5/15/120 ms pause windows.

Assertions cover frame accounting (including intentional silence), bounded queue
and correction, unchanged user target, latched Auto, recovery completion, no
secondary reprime from one injected input interruption, no post-recovery losses
in isolated fixed-clock fault cases, and independent output-deadline evidence.
The lossless envelope includes 20 ms with moderate clock changes/8 ms jitter,
and 30 ms with the tested extreme reversal. The 20 ms extreme-reversal test
instead requires each consecutive short-write burst to stay below 1 ms and
not insert additional re-prime silence. This boundary is explicit, not hidden
by a generic passing-test claim.

Results:

- All three random-arrival seeds, both periods and both targets: no missing PCM.
- +/-300 ppm reversal and gradual ramp: no missing PCM at 20 or 30 ms.
- +/-800 ppm reversal at 20 ms: 10 shortfalls / 3.854 ms total over ten minutes,
  largest short-write burst 0.604 ms; at 30 ms: none. A matched HEAD callback
  reference had 20 shortfalls / 53.312 ms at 20 ms. This is improvement, not a
  lossless guarantee for extreme clock changes.
- Repeated fixed-clock input faults: no missing PCM outside the injection plus
  two-second recovery window, no extra recovery cycles, no queue overflow.
- Repeated 40 ms output-thread stalls: 5 independent deadline suspicions and
  140 ms modeled endpoint loss; no PCM shortages. Diagnosis does not fix a
  stalled output thread.
- With correction Off, a delayed burst retained a 50 ms pre-render queue at a
  20 ms target. The HEAD callback did too. Do not promise latency restoration
  with correction disabled.

The recovery tradeoff is measurable. Five 40 ms discarded-input incidents with
Auto produced 198.646 ms total missing/intentional silence versus 150 ms for the
matched HEAD callback (about 9.7 ms more per incident). A 15 ms packet-loss window
at the 2.667 ms output period produced 14.583 ms versus 7.354 ms. The newer callback
restores reserve sooner, but does **not** minimize every individual interruption.
The matching reference uses HEAD 490b4ed's PCM callback with the same virtual
clock, packet schedule and current bit-identical DSP, not physical old/new app
measurements. Diagnostic/error counters alone must not be presented as evidence
of a shorter audible gap. `max-short-write-burst` sums missing frames in adjacent
short writes; it is not a measurement of the continuous physical sound waveform.
The replay's `final` field is the controller's filtered pre-render queue estimate;
it is not meaningful for the reference Off path, which leaves that field unset.

Normal CTest: 13/13 passed. The 32-case short extension and callback tests also
completed under AddressSanitizer without a memory-error report, subject to the
same interceptor limitation described above. Run sanitizer executables from the
Visual Studio developer environment so its runtime DLL can be found; a launch
outside that environment failed before test entry with 0xC0000135.

This stage adds only developer tests and documentation. The application remains
599,040 bytes, SHA256
`91DEC841EA1CC039BB0121F8EA1FBBF575F5FEACA5D5DF68DB24CCE5B92B0E51`.
No new production processing, dependency, setting change or release is added.

## 25 ms default follow-up (2026-09-07)

The subsequent default-setting change is checked with
`SharedReplayTests --target25 --long`: the same extended schedules, replacing
20/30 ms with 25 ms. This runs 17 ten-minute cases and six three-minute boundaries
(3 hours 8 minutes combined virtual playback). All 23 cases passed. CTest now
also includes `SharedReplayTarget25`, using the shorter durations.

At 25 ms, the random-arrival seeds, +/-300 and +/-800 ppm reversals and gradual
clock ramp all had zero missing PCM. Injected input/output interruptions are
still allowed to cause loss, but recovered without secondary interruptions
outside the recovery window, unbounded queue growth or repeated Auto switching.
This verifies this envelope only; physical hardware remains untested here.

The extra reserve is a tradeoff, not universally shorter glitches: five 40 ms
discarded-input incidents in Auto gave 223.312 ms total shortage plus intentional
silence at 25 ms versus 198.646 ms at 20 ms (about 5 ms extra per recovery). For
delayed rather than discarded packets the same comparison improved from
148.646 to 123.312 ms. Packet timing and refill quantization matter.

`AppSettings` now defaults to 25 ms, and the INI reader uses that default when
the key is missing or invalid. Current-format saved 10/15/20/25/30 ms values are kept;
`SettingsStoreTests` covers every value as well as fresh/missing/invalid cases.
The WASAPI device buffer default is unchanged. UI labels, both help languages
and current audio documentation agree with the new PCM default. Historical
release notes are intentionally unchanged.

At the user's subsequent request, unversioned legacy 20 ms settings are now
upgraded to 25 ms once. Startup persists only `PcmQueueTargetMs` and
`PcmQueueDefaultsVersion=1`, before direct-start handling. Normal saves also
record the marker, so selecting 20 ms afterward is preserved. Other legacy PCM
values and unrelated/unknown INI keys remain untouched. Loading remains read-only
and supplies the migrated value in memory if persistence fails. Tests cover all
five legacy values, repeated migration, direct-start settings, unrelated keys,
post-migration 20 ms, future markers and nonexistent files.

Before the subsequent one-time migration change, the rebuilt test application was 599,040 bytes, SHA256
`AB286AF25E66506F37D2800F1CC72D79DDE1FEC0EBEAA7D86BE9F6E0320F6DBC`.
The PCM target adds approximately 5 ms of intended queue reserve versus 20 ms;
it adds no new processing path or runtime dependency. Actual instantaneous queue
depth is packet-quantized, so this is not an exact measured end-to-end delay.

After rebuilding with the new default, the full CTest suite passed 14/14,
including existing settings preservation and the dedicated 25 ms replay.

After adding the one-time legacy migration, the application rebuilt successfully
and 11 non-replay CTest entries passed, including the new migration coverage.
The long audio replays were not rerun for this startup/settings-only change.

## Whole-application follow-up (2026-09-07)

The following review expands coverage beyond Shared audio. These changes are
local development work on the existing branch; no release, version bump, user
configuration update, physical device probe or installer run is part of testing.

- Existing fullscreen/resize transition coalescing is tested through the actual
  window message handlers, including nested transitions, interactive resize,
  minimization and later independent resizes. Repeated F11 key-down messages
  are now ignored so holding the key cannot repeatedly rebuild video output.
- Window geometry tests cover 72 resize combinations across all eight handles,
  decorated/borderless frame profiles, minimum sizes and negative coordinates.
- Recovery budgets now reset only after evidence of a successfully running
  session, not time spent in slow failed opens or teardown. WASAPI reports time
  through its last successful render iteration; terminal cleanup clears the
  Shared refilling flag. Short-lived sessions do not accumulate healthy time.
- Update checks own their results instead of posting allocated pointers to an
  HWND. Closing a window can discard a notification without leaking the result.
  The startup delay is interruptible; cancellation is checked between synchronous
  WinHTTP calls. An already-active call still relies on the existing timeout;
  this is not a promise of immediate network cancellation.
- Failed/incomplete HTTP transfers are not parsed as successful update checks.
  A newer release with no installer is distinguished from being up to date, and
  installer links require the official repository path and exact `_Setup.exe`
  suffix. Offline fixtures and a fake fetcher exercise result/cancellation races.
- Exclusive scan results similarly belong to the dialog, not its message queue.
  The UI joins the old worker and consumes pending results before declaring the
  scan idle, preventing restart while an old completion notification is pending.
- Diagnostic command-line runs now suppress legacy INI migration before loading,
  not just subsequent saves. Normal startup's one-time 20-to-25 ms upgrade is
  unchanged; tests write only temporary INI fixtures.
- The latest-video mailbox is owned by the graph resources through Stop and
  callback detachment. Previously, an early break after a partially failed Run
  could destroy the callback's target before stopping the graph. DirectShow
  explicitly permits some filters to have started when
  [IMediaControl::Run returns an error](https://learn.microsoft.com/en-us/windows/win32/api/control/nf-control-imediacontrol-run).
  A teardown test with fake COM objects checks the actual cleanup order without
  opening a capture device.

This follow-up does not change the pixel conversion, MJPEG/HDR interpretation,
capture queue depth or configured audio reserve. Mailbox storage is allocated
once at graph setup, not once per frame. Test executables/sources are excluded
from the explicit application and installer file lists. No external runtime
dependency was added.

### Remaining boundaries

These tests cannot certify real F11/display-driver transitions, physical ASIO
or Exclusive compatibility, HDR/MJPEG hardware, or multi-hour audible quality.
The earlier synthetic audio test envelope and recovery tradeoffs still apply.

Separate existing settings-UI issues were identified but not changed by this
lifecycle patch: the fixed-size settings window can exceed a small work area
at high DPI, and the custom message loop does not implement full dialog-style
Tab/Enter/Escape routing. Ordinary INI saves still do not surface every write
failure. A failed completion PostMessage (for example, a full Windows message
queue) can leave a check appearing busy until its window is reopened; owned
results nevertheless clean up on close. These are not claims of a bug-free app.

### Follow-up verification evidence

- The optimized x64 application rebuilt successfully with `/W4` and no reported
  compiler warning. `git diff --check` passed.
- After the final capture-lifetime and scan-ownership changes, the entire CTest
  suite passed 15/15 in 187.10 seconds, including all three short Shared replay
  suites and settings migration. Long replays from the earlier audio stage were
  not repeated for this lifecycle-only change.
- The latest-video/partial-Run teardown suite and update-check lifecycle suite
  each passed 25 consecutive executions. The former processes 25,000 samples per
  run (625,000 across the repetition), checking identity order, latest-frame
  retention, timestamps and exact COM reference transfer/release.
- Latest-video teardown, actual audio/window/Exclusive-scan callbacks, update
  lifecycle and window geometry also passed under MSVC AddressSanitizer with no
  reported memory error. The same process-local interceptor workaround described
  above was necessary; this remains supplementary, incomplete sanitizer coverage.
- The application is 610,304 bytes, SHA256
  `D54333420A39C2E8671B0DEF6700BDF0286C1F2B8749771B4E066E38B3298604`.
  Compared with the immediately preceding 599,552-byte migration test build,
  this is +10,752 bytes (10.5 KiB, about 1.8%). Import inspection shows only the
  existing Windows system libraries; test and sanitizer runtimes are not linked
  into the ordinary application. No whole-app physical latency or CPU benchmark
  was performed for this lifecycle-only follow-up.

## v1.2.6 release build verification (2026-09-07)

A fresh UTF-8 x64 Release build in `build-v1260-release` passed all 15 CTest
entries in 187.24 seconds. The executable reports 1.2.6 (numeric 1.2.6.0), is
610,304 bytes, and has SHA256
`9AA73CF9D7C65758BA8236D695EC8A1CE41C7161D760DFB6B9C8A27350661A23`.
The clean build reports existing third-party ASIO SDK warnings (ANSI/Unicode
macro overrides, deprecated string APIs and size conversions); unlike the
earlier incremental build, it is not described as warning-free. No physical
device or extended listening test was added for release packaging.

Portable packaging rejects an executable whose file version does not match
the requested package version before creating or removing output paths. Both
binary package manifests now include the ASIO SDK license and the separate
embedded host-helper notices. These packaging changes do not alter playback.
