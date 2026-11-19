# LLCV v2.3.2 release candidate checks

Branch: `release/v2.3.2`, based on the v2.3.1 merge. This release adds
playback GPU selection and improves VFG availability guidance. The NVIDIA
runtime and supported architecture policy are unchanged.

- Built the x64 Release configuration with recording, VFG, and the pinned
  runtime's experimental SM 8.6 path. The CTest inventory matches all 203
  tests from v2.3.1 and adds `PlaybackGpuHardwareTests`.
- All 204 tests passed in the release build, with no skips or retries,
  including independent FFmpeg/ffprobe recording checks. The native hardware
  check enumerated one selectable RTX 3080 and opened the selected adapter
  with D3D11. These checks are not a full hybrid-GPU capture session.
- The isolated UI preview reproduced the duplicate RTX 3080 entry. It listed
  only Automatic before the fix; after the fix it showed Automatic and one
  named RTX 3080 next to the display monitor in the Video tab.
- Extracted the portable ZIP and passed the release package validator for
  v2.3.2. All 100 payload hashes match the installer build manifest, including
  multiplicities. Setup and the packaged EXE report 2.3.2.
- The packaged installer was not run. A separate Windows environment was not
  available for a clean install or in-place upgrade. Hybrid-GPU playback has
  not been tested on a physical iGPU + dGPU machine. No installed app profile
  or Windows graphics preference was modified.

| File | SHA-256 |
| --- | --- |
| Viewer EXE | `82EC78185453CA4606AA8FFF4503F4801F40CA8A9295B5F67E40F46F4A27DD3D` |
| Setup | `A5A1CAD36DDC28E803DFF7C00FE0E1D48AF82E6C3B53AB9BF711699AA2F99C01` |
| Portable ZIP | `DAF5CF460A1AA831713FEC4EBDF857613BF8E8BF2CE4D09B8CFE6436EED1A42E` |
