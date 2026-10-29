# RGB capture support (source build)

Device-advertised RGB24 and XRGB (DirectShow RGB32) are manual SDR inputs. The list is specific to the selected device, capture resolution and reported frame rate. Auto continues to prefer NV12, then YUY2 and UYVY; it does not select RGB. The GC573 advertisement inspected for this work contained 26 RGB24 modes and no RGB32 modes; availability is device and driver dependent.

RGB24 is native BGR, three bytes per pixel, with DWORD-padded rows. RGB32 is native BGRX, four bytes per pixel; X is ignored. RGB24 is uploaded to an R8_UINT byte texture and expanded once by a GPU compute shader to an internal RGBA8 surface before the video processor. RGB32 uses a BGRX surface directly. The internal RGBA texture is not the capture format. Positive DIB heights are bottom-up and negative heights top-down; GPU playback and source screenshots honor the connected orientation.

RGB playback and PNG screenshots use the SDR range override first, then valid device metadata, then Full by default. RGB inputs use BT.709/sRGB SDR or unspecified primaries/transfer with a BT.709 default; explicitly signaled PQ, HLG, BT.2020 and other unsupported tuples are rejected. No YUV matrix or chroma interpretation applies. Both RGB formats can request NVIDIA VSR directly through their existing Video Processor input, without an additional conversion pass. Driver request acceptance is not proof of RTX enhancement activation. Existing rejected-request/Blt fallback keeps ordinary playback available. SDR stereo recording converts native BGR/BGRX to BT.709 Limited NV12 (4:2:0) on the recording worker. Downscaling is fused into this conversion before existing encoding/muxing. Playback and PNG screenshots retain their RGB path and are not fed from the recording conversion. Recording conversion uses bounded reusable buffers; it adds CPU work and larger raw capture copies only while recording. No RGB conversion or allocation is added to NV12/YUY2/P010 playback. RGB does not issue the existing vendor hardware tone-mapping control; any device-side behavior is unverified. P010 HDR output and HDR screenshot tone mapping do not apply to RGB. Normal presentation and existing SDR scaling options remain available subject to driver support; their performance and every driver combination are not guaranteed.

Validation included synthetic hardware and WARP RGB24 repacking, RGB24/32 renderer and screenshot readback, and 60 selected CTests, all passing (`outputs/xrgb-support/final-ctest.log`). The existing SettingsView test has a 120-second CTest limit; direct runs completed successfully in 144.647 seconds for the previous release baseline and 148.931 seconds for the current local optimized build (`ui-baseline-direct.log` and `ui-current-release-direct.log`). These are local test builds, not a release or deployment.

On the actual GC573 and RTX 3080, 10-second isolated smoke runs at 2560x1440 and requested 60 fps connected successfully: native RGB24 captured/presented 269/269 frames and NV12 307/307 (`native-rgb24-smoke.log`, `native-nv12-smoke.log`). The RGB24 connection reported bottom-up native BGR24, stride 7680 and 11,059,200 bytes; the device advertised RGB24 1440p at 60, 120 and 144 fps and 26 RGB24 modes in total. RGB32 was not advertised and has not been tested on this board. The logs' single latest application-latency samples are not measurements or guarantees of physical HDMI-to-display latency, sustained throughput, or performance.

**Device control observation:** the NV12 smoke log recorded an automatic hardware HDR-to-SDR `requested=on` command accepted by the AVerMedia driver. Its prior device state is unknown; no compensating command or restoration was attempted, and the present hardware state is not inferred. The smoke-video override path now skips the vendor tone-mapping function entirely, including OFF commands. This guard is verified by source and unit tests only; it has not been retested on the physical device. Normal non-override app policy is unchanged.

For a future authorized real-device comparison, the bounded, isolated-profile commands below select matching conditions. Device testing after the guard change has not been performed:

```text
LowLatencyCaptureViewer.exe --smoke-test --smoke-test-video-format rgb24 --smoke-test-resolution 2560x1440 --smoke-test-fps 60
LowLatencyCaptureViewer.exe --smoke-test --smoke-test-video-format nv12 --smoke-test-resolution 2560x1440 --smoke-test-fps 60
```

The video overrides require the explicit `--smoke-test` switch, operate on its temporary profile, and do not save user settings. The default smoke run stops after ten seconds. `rgb32` is available as an override only for devices advertising that native format. A successful synthetic test or capability advertisement does not establish real-device accuracy or latency.

## RGB VSR and recording extension (2026-10-29, uncommitted source work)

The earlier validation above predates RGB recording/VSR support. The new path removes
the RGB VSR/recording exclusion, not the existing SDR/HDR color contract.
RGB24/RGB32 request VSR on NVIDIA at native size or larger, with the existing
same-frame OFF fallback on a rejected VSR blit. Synthetic ON/OFF tests exercise
both formats, custom F6 routing, retained resources, HUD state and exact OFF restoration.
No NVIDIA profile changes or physical capture runs are used for this extension.
An accepted extension request or changed pixels alone do not certify RTX model activation.
The final native-size live toggle run logged 42 pairs with no ON/OFF pixel difference,
including the existing NV12/YUY2/UYVY paths as well as RGB. This confirms request/state
handling and OFF restoration, not visible RTX enhancement or model activation.

Recording normalizes Full/Limited RGB, ignores X, corrects bottom-up storage, and
produces BT.709 Limited NV12. This is intentionally 4:2:0 recording, not lossless RGB.
Input copies keep the existing bounded capture queue. Conversion and fused bilinear
downscaling run only on the recording worker; original screenshot/playback buffers are
not replaced. Existing encoding, rate control, audio gain, timeline, pause, splitting,
storage and recovery are reused. Unsupported RGB HDR metadata is still rejected.

The scalar implementation uses fixed-point lookup tables. A CPUID/OSXSAVE/XCR0-gated,
non-inlined AVX2 kernel accelerates native-size and exact 2x downscale conversion.
Other CPUs and arbitrary scale ratios retain the scalar path. No global AVX2 minimum
or extra rendering pass is introduced. Tests compare SIMD/scalar within 1 code value
and use an independent floating-point BT.709 reference for orientation/range/color.

Local worker-only conversion samples (20 frames, before the full suite, not total
recording or display latency): RGB32 1080p 7.337 ms, native 4K 30.985 ms, 4K to
1080p 11.554 ms. These are one host/run, not throughput guarantees. Native 4K60
cannot be promised with this CPU conversion; use recording resolution/FPS caps.
An earlier unoptimized run and measurements overlapping a build/UI test are not used
as clean performance comparisons. Recording-off RGB rendering does not run this converter.

New tests cover converted RGB values, source orientation/range/X, short buffers,
scalar/SIMD parity, H.264 RGB24/RGB32 files, full frame counts, flash/tone sync,
pause/split/downscale, RGB32 HEVC and decoded color/metadata at native/downscaled sizes.
The settings view passed its direct executable run (1600 combinations and 58 golden
rectangles). Its existing 120-second CTest deadline is not relaxed; the direct run is
reported separately from CTest.

The first broad pass completed 190 tests in 824.46 seconds: 189 passed and one
existing HEVC storage-delay crash checker failed on a final file without a moov.
This dedicated binary does not include Recorder.cpp or the RGB converter; its
storage/container sources were unchanged. The old 2.1.8 binary passed three repeats,
so that alone does not reproduce or prove the timing failure. Source inspection showed
that the checker required even a just-opened final crash file to have its header
committed, despite the documented completed-prefix recovery contract.

The checker now permits only the last uninitialized file after a forced exit.
It still fails for an interior uninitialized file, any uninitialized file after
normal finalization, or corruption in an initialized file. Minimum retained video/audio
packet counts, timestamp/sync limits, segment boundaries and independent full decoding
of every initialized file are unchanged. A new policy unit test exercises the exclusion
and rejection cases. No production storage or recovery code was changed for this fix.

After the final CPU-feature guard, Full/Limited RGB VSR tests, recording-tab note and
checker changes, all **38/38 affected tests passed** in **232.75 seconds**, with no
skips. This includes the full independent segment/crash/decode checks, RGB recording
color/frame/sync/pause/split checks, conversion references, native RGB GPU pipelines,
SDR, settings/profile isolation and recording UI. These overlap the broad run, not
228 unique tests. Local evidence is in outputs/rgb-vsr-recording/: regression.log/.xml,
post-audit-tests.log, post-audit.xml, settings-view-direct.log and optimized-converter.log.

Tests used isolated profiles; no new physical capture test or NVIDIA profile write was
performed. The actual settings.ini SHA256 observed during this work remained
20AB4EEDB82DCB67D63274BDBCF57A7E26F93842395ECC17400965E4F6D3DF00;
recording-prototype.ini remained
2D1C8F5D136F8A812E78C5558C32C9BC690751923AAB20D8A77A0BEB144C406A.
The work remains uncommitted on the existing feature branch; no release/version bump,
installation, merge or publication was performed.

## Audit hardening (2026-10-29)

- RGB recording now measures the selected converter/resize route on its worker
  before opening the recording file: one warm-up plus up to six measured samples.
  Admission requires at least three samples, mean conversion at most 75% of the
  selected output frame budget, and no sample exceeding that budget. This reserves
  headroom but is not a sustained-throughput guarantee. No silent FPS reduction,
  frame dropping, capture-thread conversion, GPU readback or playback queue is added.
- RGB capture frame storage is bounded by 128 MiB, converted NV12 storage by
  192 MiB, each at most 16 leases. Standard sizes retain 16 encoder output leases;
  4K RGB32 capture uses four raw leases. The combined frame payload is 316.41 MiB
  for native 4K output and 174.02 MiB for the default 1080p output, down from
  696.09/553.71 MiB with both pools fixed at 16. Audio/encoder/GPU allocations are
  separate. A shorter raw queue absorbs less storage jitter; existing overload
  handling still stops recording safely, without changing playback.
- A rejected preflight reports measured time and resolution/FPS guidance in both
  languages without offering a nonexistent recording file. Cancellation releases
  conversion leases; scalar/SIMD pixel conversion remains unchanged.
- This developer report is excluded from CMake, portable and installer packages.
  README links now target the public recording guides, which describe the guard,
  memory budget and limitations without local paths or profile hashes.
- Crash verification now excludes only an empty final crash file or a recognizable
  incomplete MP4 initialization prefix. Malformed/ambiguous headers or media boxes
  before initialization fail rather than being hidden. Normal/interior files are
  never exempt; initialized files still undergo every packet, frame, sync and full
  decoder check. Ambiguous short torn headers intentionally fail closed.
- Controlled storage-delay crash runs: HEVC 5/5 and AV1 5/5 passed. Every run retained
  330 video / 516 audio packets; `uninitialized_crash_tail` was null in all ten.
  This is a controlled test observation, not an estimated real-world failure rate.
  Evidence: outputs/rgb-hardening/crash-{hevc,av1}-{1..5}/segment-verification.json.
- RGB layout rejection logs now distinguish header size (including V4/V5), bit
  depth, planes, compression, truncated/nonstandard masks, image size, row remainder
  and row alignment. Header/mask reads are bounds-checked. Capability probing,
  negotiation and connected-format startup forward these diagnostics; accepted
  layouts, automatic selection and per-frame rendering are unchanged.

New/extended tests cover admission thresholds, memory budgets, unchanged YUV/MJPEG
budgets, translated failures, converter cancellation/lease reuse/pixel parity,
strict MP4 tail classification and detailed malformed RGB header logs.
The initial admission subset passed 13/13. The broad post-hardening run passed
192/192 tests in 800.07 seconds, with no skips; logs are
outputs/rgb-hardening/regression.log and regression-details.log. The separate
settings view run and final failure-message layout checks are recorded below.

After the final concise failure-message change, the normal application and all
targets rebuilt successfully; 11/11 affected tests passed in 16.83 seconds
(message-tests.log/.xml). DirectWrite metrics confirm that the Korean and English
preflight messages fit the existing 356 x 84 text area without resizing the status
card. State transitions, no-file actions, converter cancellation/lease reuse,
format diagnostics, both packaging paths and profile protection passed again.
These eleven overlap the broad run; they are not eleven additional unique cases.

SettingsViewTests was run directly, retaining its existing CTest deadline:
24 control-factory profiles, eight compact work-area sizes/scales, 1,600 layout
combinations and 58 golden rectangles passed (settings-view-direct.log). Thus the
broad run plus this direct UI executable cover all currently registered test names.
The CTest timeout itself has not been fixed or relaxed by this work.

A final standalone converter run passed all independent color/orientation/range,
scalar/SIMD, cancellation and lease checks. Its worker-only means were RGB32 FHD
8.127 ms, native 4K 32.552 ms and 4K to 1080p 11.728 ms (converter-clean.log).
As with the earlier single run, these are not total display/recording latency or
a controlled before/after benchmark. Pixel math and the recording-off/YUV render
paths were not modified by the admission guard. Native 4K60 remains too expensive
on this host. An additional real-worker admission integration test uses synthetic
native 4K60 RGB32 input (no capture device or user profile): it logged conversion
89.079 ms mean / 98.382 ms maximum, frame budget 16.667 ms, 316.41 MiB frame pools,
`accepted=0`, dedicated result 0x80040403 and `file=0`. The test also allows a faster
host to admit the same request if its measured headroom satisfies the policy.
It then reuses the recorder for the existing playback-independent state scenarios;
the full integration executable passed (admission-integration.log). This worker
preflight uses its own textured probe/range/context and is not directly comparable
to the standalone constant-input converter benchmark above.

The real settings.ini and recording-prototype.ini hashes still match the values
above; the existing build-v2180-release executable also remains unchanged at
24BCC099FABB2544B24EFE97E0C96FB649E8AC524A626CABE7C423C2A8BCD331.
No new physical capture/VSR activation/RGB32-device validation, release, commit or
merge was performed. Existing missing-HDR-metadata limitations remain unchanged.
