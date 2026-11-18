# Completed recording duration

## Change

All recording codecs use the pinned FFmpeg 8.1.3 packet muxer's
`hybrid_fragmented` mode. Media Foundation still encodes the same samples.
During capture, the file contains the recoverable initialization and fragment
sequence. Normal finalization appends a complete sample index, then updates the
outer media header to expose a conventional MP4. Each completed split follows
the same path without restarting the encoder.

Finalization does not read back, move, copy or re-encode the media payload. The muxer retains
per-sample index metadata until a file closes, so metadata memory and final index
size grow with the number of packets in that file. Splitting bounds that growth.
This is not a claim of constant memory or zero finalization cost for arbitrarily
long recordings.

Playback, capture, settings serialization and shortcut code are unchanged by
this fix. Existing recordings are not rewritten. An interrupted recording can
remain fragmented and lack Explorer duration; the existing cache/disk/final
fragment protection limits still apply.

## Regression coverage

- `RecordingContainerTests`: H.264 encode, complete frame/timestamp checks,
  Explorer duration, interrupted capture and a torn fragmented tail.
- `RecordingModern_*_Recovery`: HEVC, HEVC reordered pictures and AV1, including
  normal finalization, complete packet counts, native timelines and interruption.
- `RecordingSegments_*`: time/size splitting, every packet saved exactly once,
  keyframe boundaries and Explorer duration for every completed part.
- `RecordingIndependent_*`: independent ffprobe/decoder checks of timestamps,
  frame counts, audio/video sync and nonzero track origins.
- `RecordingMuxFaultTests`: write/seek/flush failures and a short failing write
  at the final sample-index boundary. The failure remains an error and earlier
  fragments remain readable through a real MF byte stream. The test double has
  no asynchronous reads, so recovery uses a test-owned temporary file.
- Storage-delay, storage-crash, recording queue and source-format regressions
  exercise the existing bounded delivery/storage paths.

Completed files must have one top-level `moov`, one `mdat` and no exposed `moof`.
The duration assertion uses Windows `IPropertyStore` / `PKEY_Media_Duration`,
not just ffprobe. It requires a nonempty duration within 200 ms of the tracks.
Torn-fragment fixtures extract the pristine fragmented sequence retained inside
the hybrid media box; real process-termination tests independently exercise the
unfinalized writer. Truncating a completed regular MP4's final index would not
model a recording interruption.

## Reproduction

Use the configured recording SDK and test tools described in `RELEASING.md`:

```powershell
cmake --build build-v2210-release --parallel 4
ctest --test-dir build-v2210-release --output-on-failure -j 2
```

No capture device or production settings are needed. Tests use synthetic or
repository-owned recording fixtures and isolated settings paths.

## Results (2026-11-17)

- Full app/test build succeeded; the final incremental build had no pending work.
- All 199 registered tests have passing results across the regression run and
  targeted reruns after the test-harness corrections below. This was not a
  single uninterrupted 199/199 invocation.
- A final explicit-name rerun of the two checked-handle tests and four split/
  storage crash tests passed 6/6 in 56.31 seconds (`final-checked-rerun.log`).
- The AV1 12-second fixture returned `120213334` in Windows' 100-ns duration
  units (about 12.0213 seconds). The extracted interrupted/torn fragmented
  version had an empty Windows duration property, reproducing the distinction.
- Completed H.264/HEVC/AV1 and split files passed the Windows duration assertion;
  independent packet counts, decode, timestamps, color and A/V sync passed.
- Real playback and recording preference file hashes were unchanged. No normal
  app instance was launched and no existing recording was modified.

The initial test environment's long redirected temporary path caused unrelated
profile/image/archive file-operation failures. Repeats used a short test-only
directory under the writable workspace (`TMP` and `TEMP`), with no production
settings involved.

The existing closed-handle assertion could terminate with `STATUS_INVALID_HANDLE`
in a checked-handle environment, including the mailbox stress test. Its test-only
helper now treats precisely that status as the expected closed-handle result;
other exceptions still propagate. Native reruns passed without a debugger.

The split recovery classifier originally required `moov` before media and thus
rejected correctly finalized hybrid parts. It now recognizes a complete media
box followed by its final index. New policy cases verify that a missing/torn
index after media is invalid, not an ignorable uninitialized crash tail. The
full packet/decode requirements and interior-file checks are unchanged.

Local logs are in `outputs/recording-duration-20261117/` (generated artifacts,
not distributed with the app). The above test commands reproduce the checks;
the checked-in tests do not depend on those local logs.
