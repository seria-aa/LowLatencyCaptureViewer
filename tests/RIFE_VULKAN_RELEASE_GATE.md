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
