# Video output debug audit — 2026-09-10

Historical GPU audit. Subsequent hardening and verification:
[VIDEO_HARDENING_VERIFICATION.md](VIDEO_HARDENING_VERIFICATION.md).

## Scope

Local v1.2.6.1-test working tree, NVIDIA GeForce RTX 3080. The opt-in
`AudioCallbackTests --presentation-debug` invokes the application's actual
renderer with D3D11_CREATE_DEVICE_DEBUG and reads ID3D11InfoQueue. Diagnostic
code is compiled only into this test target via LLCV_GPU_DIAGNOSTICS; it is
not enabled in the portable executable. No capture/audio device is opened.

## Results

- Synthetic 1920x1080 NV12 and YUY2, each in Immediate Flip, VSync Flip and
  Compatibility Blt: 120 frames per combination, 720 processing calls total.
- OSD switches every 30 frames to exercise direct video output and overlay draws.
- D3D11 info queue: **0 warnings, 0 errors**, including renderer initialization.
- Separate `--presentation-gpu` run: eight initialization cycles, resizing and
  Flip/Blt/Flip transitions succeeded; HDR10/Blt rejection succeeded.
- Default AudioCallbackTests regression run passed after instrumentation changes.
- The previously delivered portable ZIP was neither rebuilt nor replaced.

## Timing and interpretation limits

All windows were hidden. Every Present returned DXGI_STATUS_OCCLUDED. The
processing test deliberately clears the renderer's cached occlusion state before
each frame so video processing/overlay calls are exercised repeatedly rather
than skipped. This is NOT a reproduction of visible playback or scanout.

CPU API call durations (microseconds, mean / maximum) in this single run:

| Mode / input | Upload | VideoProcessorBlt | Overlay | Present |
| --- | ---: | ---: | ---: | ---: |
| Immediate / NV12 | 168.2 / 538.1 | 1.3 / 6.3 | 140.3 / 16282.4 | 918.4 / 9647.1 |
| Immediate / YUY2 | 231.7 / 664.3 | 1.7 / 6.5 | 98.4 / 11206.1 | 921.9 / 10887.2 |
| VSync / NV12 | 283.9 / 755.3 | 2.9 / 8.4 | 74.8 / 7972.0 | 6561.9 / 11891.3 |
| VSync / YUY2 | 361.1 / 622.8 | 3.3 / 10.8 | 77.2 / 8139.3 | 6424.1 / 9092.1 |
| Blt / NV12 | 208.8 / 868.8 | 2.4 / 6.1 | 80.4 / 8592.4 | 435.7 / 10302.8 |
| Blt / YUY2 | 235.4 / 680.5 | 2.5 / 6.5 | 90.5 / 9841.0 | 380.8 / 9954.4 |

These include initialization/warm-up effects and debug-layer overhead. They are
not GPU execution timings, display latency, or a valid comparison of visible
Blt vs Flip performance. In particular, the lower hidden-window Blt Present
duration does NOT mean lower display latency. No multi-second CPU call stall
was observed in this brief run.

This does not exercise Independent Flip, MPO, VRR, physical monitor signal
delivery, live capture timing, other GPUs/drivers, or the reported 10–30 minute
failure interval. It does not establish or rule out an application-triggered
display-link failure. Waitable-object pacing and teardown ordering remain
code-review candidates, not confirmed causes of monitor signal loss.
