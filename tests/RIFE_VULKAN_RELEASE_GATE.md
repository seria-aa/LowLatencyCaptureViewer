# RIFE Vulkan release gate

- Configure the VFG release build with `LLCV_RIFE_VULKAN=ON`, the Vulkan headers
  and static-CRT replay-enabled ncnn SDK. Set `LLCV_RIFE_VULKAN_MODELS` to the
  directory containing the four reference Fast files and `LLCV_RIFE_NCNN_SOURCE`
  to the patched source tree including its glslang submodule. Leave both model
  and source paths empty for a diagnostics-only build.
- CMake pins all four Fast files by SHA-256. Installers and ZIPs include only
  these four files and the three license notices. The NVIDIA package stays
  separate. The exact ncnn replay patch is in `third_party/frame-generation`.
  Preserve model conversion inputs and reproducible instructions with the
  release sources.
- Test fresh profiles and upgrades: frame interpolation remains off for a new
  profile. A legacy enabled VFG preference must remain VFG, including a saved
  F7-off state. The default method in a fresh profile is RIFE; preview profiles
  with the retired Automatic method also resolve to RIFE. The method remains
  independent of VFG download and hardware readiness.
- Test the real settings window, F7 persistence, and original-frame fallback
  for unsupported formats, missing models, and unsupported Vulkan devices.
  RIFE is restricted to FHD SDR in low-latency presentation. Verify the
  selected SDR formats and confirm that P010 leaves capture unchanged.
  Check that the RIFE rate picker lists only Auto and supported 24–60 fps
  options and leaves the ordinary capture rate untouched.

## Vulkan submission trace

- Use a new CSV path with `--rife-vulkan-trace` in the isolated
  `--rife-vulkan-fast` diagnostic run. A prior CSV with different columns
  is rejected, not appended to.
- Join the renderer and worker timestamps by job ticket within the same
  playback session. Compare `d3d_input_ready_us - d3d_input_wait_begin_us`
  with `d3d_input_wait_begin_us - worker_enqueued_us`. The latter includes
  the worker handoff and command preparation. `vk_submit_begin_us` is when
  the Vulkan call begins, not when the GPU starts executing.
- Count jobs marked `generate` with `vk_submit_result=0` whose Vulkan submission
  begins at or after their deadline. A zero timestamp means the event was not recorded. Keep
  the on-time-ready rate separate from the rate of accepted generated
  Presents, which uses all inputs inspected by the interpolation session,
  including busy and seed inputs. Present acceptance is not scan-out.
- `expired_before_graph=1` identifies a job accepted for generation on the
  render thread that was already late after the worker's D3D11 input wait.
  It advances history without writing or presenting a midpoint. Also verify
  that a fresh playback session continues showing originals while the backend
  loads, and does not adopt an abandoned initialization after renderer reset.
- In the worker-failure diagnostic, inject device loss before the first
  startup upload and verify that the next backend creates a fresh device.
  A preparation failure or the 10-second UI limit must keep original
  playback running. Cancellation prevents further initialization work at
  safe points; an already submitted Vulkan command still owns its resources
  until completion or confirmed device loss. The UI limit is not a process
  exit timeout for an unresponsive driver.

## v2.4.1 preparation

- Release branch: `release/v2.4.1`. Use a fresh Release x64 build,
  the full CTest suite, and independently verify the portable ZIP and
  installer manifest before publication.
- The fresh `build-v2410-release` build registered 204 tests, including the
  independent recording media and crash-recovery tests. The portable ZIP
  passed the release check after independent extraction (28 required files).
  The installer reported file version 2.4.1.0, and all 109 packaged files
  matched by installed path and SHA-256 between the ZIP and installer manifest.
- The full Release x64 CTest run passed 204/204, including the recording
  crash-recovery tests and both document/package checks. The installer
  was not run against an installed or clean Windows system.
- The EXE extracted from the release ZIP loaded the bundled Fast model
  with color-edge HUD protection on GC573 + RTX 3080 (FHD60 NV12). In the
  10-second smoke, 514 of 525 pairs were ready in time; 11 used original
  playback. The local settings file was unchanged. This smoke covers
  packaging and normal playback, not a performance comparison.
- Keep test scope distinct: an injected Vulkan device loss verifies the
  startup recovery path, while a GC573 run on RTX 3080 verifies normal
  playback. Neither is a real driver reset or Radeon hardware validation.
- No change to supported inputs, presentation modes, RIFE model hashes,
  or the VFG component download. The capture-based pacing comparison
  remains a diagnostic option and is not enabled in release playback.

## v2.4.0 preparation

- The Release x64 application and test targets build together. The bundled
  settings fixture checks RIFE and VFG as the only methods, FPS staying
  independent of the ordinary capture setting, and access to the FPS picker
  in desktop, compact and narrow layouts.
- Built the portable ZIP and Inno Setup executable. The independently
  extracted ZIP passed the package check (28 required files). The installer
  manifest matches the ZIP for the executable, recording DLLs, the four
  pinned RIFE model hashes and the associated notices.
- The isolated GC573 smoke test on the preceding integration build selected
  Fast in Automatic mode with no VFG components. The 541 attempted jobs
  met the deadline, one frame was skipped while busy, and the real settings
  hash did not change.
- The release build registers 204 tests with the recording media tools.
  The full run passed 201. The compact-layout failure was fixed, and the
  remaining two compiler checks were rerun with the Visual Studio SDK
  environment initialized. The three affected tests passed on the final
  candidate. The real settings file was unchanged by the full run.
- Final candidate device playback was skipped because another capture
  application was active. Fresh-install and upgrade execution on a separate
  Windows system were not performed.
