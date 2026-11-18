# v2.3.0 release candidate

**Publication blocked.** Work starts from the audio-fallback and MP4-duration commits after v2.2.1. The release branch must be named `release/v2.3.0` before tagging or publishing. Do not rename historical branch references.

- Regular build: VFG is the only interpolation option; it is opt-in. RIFE remains an experiment, not a public option. No fixed private starting profile or private captures. Dolby experiments remain outside this release.
- The base installer/portable must run with **no NVIDIA add-on directory**. Proprietary NVIDIA binaries and models, private captures and settings files are not included.
- The candidate now includes experimental `LLCV_VFG_AMPERE_COMPAT=ON` for SM 8.6, with the exact runtime identity check and source attribution. On RTX 40/50 the code takes the native path. At the maintainer's direction, [Veyra source/runtime comparison](VFG_VEYRA_COMPARISON.md) replaces the local RTX 40/50 hardware requirement for candidate preparation; it does not count as a hardware pass.
- Complete component provenance, redistribution eligibility, version/install procedure, source availability and third-party notices before making any add-on public. Do not publish a build advertised as usable with a feature for which no supported installation path is ready. See [the component-by-component distribution audit](INTERPOLATION_DISTRIBUTION_AUDIT.md).
- Verify fresh and upgrade installs in isolated locations, full regression (including recording recovery and independent media probes), and packaged executable version and imports. Use test-owned profiles only; do not change installed settings.

## Verified during preparation

- Fresh x64 Release build with `LLCV_INTERPOLATION_RELEASE=ON`, private toggles off. PE inspection shows CUDA driver/runtime imports are delay-loaded; no static NVIDIA VFG or TensorRT runtime imports.
- Historical GC573 / RTX 3080 FHD NV12 smoke: the VFG-only build displayed originals without add-ons and rejected VFG on RTX 3080 with the vendor components present. A previous private RIFE prototype generated 272 intermediate frames, but that is not evidence for the current public build.
- The first regression pass exposed two extra diagnostics lines with components disabled. Only show interpolation diagnostics after opt-in. The corrected layouts (OFF and ON, KO/EN), capture-FPS diagnostics, HDR pipeline, screenshot worker and interpolation policy passed targeted re-tests. For Windows file/PNG cases use a short test-owned `TEMP` directory.
- Historical no-Ampere candidate: `outputs/v2.3.0-validation-candidate-20261119-final` (not current). Fresh-install/upgrade testing of Setup has not been run.
- The earlier Ampere rejection markers are superseded for 2.3.0. The post-link SHA-256 stamp identifies the new VFG + SM86 build; CMake install, Inno and ZIP reject missing or stale stamps. The Dolby preview SDK remains separate and excluded from normal packaging.
- A fresh test-disabled app build passed from a separate source snapshot consisting of tracked source plus the explicitly listed release files. No local captures, experiment trees, or previous build outputs were copied. This was a working-tree snapshot, not a checked-out release commit.
- VFG now has an opt-in direct NVIDIA download in Settings; it does not require NGC or an LLCV mirror. The loader keeps the previous app-local fallback. Review permitted end-user use of the pinned package before public release. Veyra comparison does not establish redistribution rights or validation of LLCV's Windows display path.
- Final local regression on 2026-11-19: 199 of 200 tests passed on the full run, including MJPEG/NV12/YUY2 crash recovery, HEVC/AV1 independent media checks and `ReleasePackageTests`. `SettingsViewTests` exposed stale spacing and geometry expectations for the redesigned diagnostics section. Only the test expectations changed; the final isolated rerun passed. The original full run and rerun are saved as `regression-desktop.log` and `settings-view-final.log` under `outputs/v2.3.0-release-validation-20261119/`.
- Isolated fresh/upgrade installs and verification of the eventual exact release commit remain pending. No branch, tag, release or PR was pushed.

## RTX 30-inclusive candidate

- Local artifacts: `outputs/v2.3.0-validation-candidate-sm86/`. Setup and the extracted ZIP contain the same release-stamped EXE (SHA-256 `3A93B09DE84066FF12DBA75805C903529B5BF9A1E2738A7D9FD8823FC224A80B`). Both include the compatibility notice and exclude NVIDIA libraries/models. Inno and the CMake validator rejected an old official-only EXE paired with the new stamp.
- Packaged EXE on GC573 / RTX 3080, FHD NV12 60fps, separate test profiles: without NVIDIA components it presented 306/306 original frames in the monitored window. With the exact known runtime loaded privately, VFG Medium generated 533 frames during the full smoke run, with no late/busy events. This does not qualify other SM 8.6 GPUs or an end-user runtime installer.
- Targeted regression: 11/12 passed initially; the package docs test needed the source mapping for the new notice. After updating the test, both `PackageDocsTests` and `ReleasePackageTests` passed. Full 200-test run above is the preceding no-SM86 baseline, not a full rerun of the changed candidate.

## Source staging

The opt-in download implementation lives in `src/video/VfgRuntimeInstall.cpp/.h`;
include its test in the source manifest. Downloads go directly to NVIDIA, not
to an LLCV mirror. Review the permitted end-user use of the pinned NVIDIA
package before publication; no vendor DLL/model is bundled.

The downloader candidate is in `outputs/v2.3.0-vfg-autodownload-candidate/`.
An isolated offline test installed exactly five files from the pinned wheel.
The online test deliberately closed the first connection at 2,097,152 bytes,
resumed with HTTP 206, completed both archive and DLL hash checks, and cleaned
up the archive. All four focused installer, packaging, documentation and
interpolation tests passed. See `outputs/v2.3.0-vfg-autodownload-validation/`.

When staging the release, include the runtime integration sources under `src/video`
(`RifeHalfTest.cpp/.h`, `RifeHistoryPolicy.h`, `RifeDeadlineTrace.inl`, `VfgGpuProbe.cpp/.h`,
`VfgLive.inl`, `CudaContextScope.h`), the explicit dependency
`experiments/frame-generation/VfgAmpereCompat.h` and its upstream notice
`third_party/frame-generation/VFG-RTX30-NOTICE.txt`, and the policy test. Do not blanket-stage
`experiments/`, DLLs, model caches or personal capture data. Build from the
reviewed release source before publication.

## Final candidate: opt-in download

- Fresh temporary profile, packaged EXE: the Settings button prompted before fetching the pinned wheel from NVIDIA, required no sign-in, and installed only the five verified DLLs. The installed status and the independent VFG checkbox were visible. A duplicate control ID found in this test was removed; the release policy test rejects a collision with the interpolation controls.
- On GC573 / RTX 3080 (FHD60 NV12, Medium), the first candidate with the corrected control ID generated 548 frames, with one late frame and no busy events. The runtime came from the Settings download, not an app-local copy. See `outputs/v2.3.0-release-gate/ui-download-playback.log`. This is a short real-device smoke test, not a driver or series-wide guarantee.
- A caption flicker reported during the download was addressed by keeping themed updates for this button on its buffered paint path. The change is UI-only. The rebuilt ZIP and Setup are in `outputs/v2.3.0-release-gate-final/`; the extracted ZIP passed package inspection and its EXE matches the Setup manifest (SHA-256 `44EE37795660DD3F3D69DC13F223E2A8F498170E8A4842E4517D42D3E7B6406F`).
- Follow-up QA fixes: reserve the VFG quality help rectangle, avoid REC/volume overlay collisions, fit Tab diagnostics to small clients, localize VFG status, check actual CUDA support for the matching D3D11 adapter, verify cached runtime DLLs asynchronously before reporting Ready, retry initialization after installation, recover VSync VFG after a missed second slot, and stop deferred ASIO startup when video-only is selected.
- The rebuilt portable candidate in `outputs/v2.3.0-qa-fixed/` passed 12 focused tests, including a native release-variant VFG settings fixture. Using the extracted EXE, the quality help remained operable, the 489×275 Tab panel fit the client, and the volume HUD no longer covered REC or the saved card. A short H.264/AAC recording made from the GC573 no-signal feed reported 5.93 seconds in both ffprobe and Explorer. A valid copy of the previously downloaded runtime passed the new background verification. With the same input, the RTX 3080 generated 982 VSync VFG frames and responded to F7 off/on; 34 frames exceeded the generation deadline. No second-slot timeout occurred in that run, so recovery from that specific condition was exercised only with simulated timing.
- Final QA candidate regression: **203/203 passed**, including the independent recording and crash recovery cases (`outputs/v2.3.0-qa-fixed/full-regression.log`, 1144.74 s). This is the tested working-tree build, not a rebuild from a release commit.
- Portable contents: all 98 archive members matched their extracted files byte-for-byte; the package check passed. The Setup build manifest contains the same 98 payload hashes (including multiplicities). Packaged EXE SHA-256: `B9DE481A2F729392378CB15B2440A0D43368E2311116BE161B7D0442362B4033`. Setup: `FEF727DDDEC1507779D34CED8B9C782F8E95C1DE9610D242039E1D6B24832290`; ZIP: `EC7578F971EFFBEC01A06D1F2E524D1597390582C3EEAD5A773BCA33FDF11332`. Manifest comparison is not an install test.
- Still required for publication: an isolated fresh Setup install and an in-place 2.2.1 → 2.3.0 upgrade, verification of the exact scoped release commit, and confirmation that the pinned NVIDIA package can be used through the offered end-user path. Successful direct download is not evidence of licensed use or redistribution. No real user installation or profile was used for these checks.

## Candidate with portable FFmpeg build paths

Use `outputs/rc-path-clean-230/release/` in place of the preceding candidate:
the earlier FFmpeg DLLs and configuration exposed the build machine's profile
path. The mux dependency was rebuilt from the same verified FFmpeg source;
all 3,122 configuration, architecture and capability switches matched.
The viewer EXE is byte-identical. The new package check rejected the old
runtime and accepted the new one. All 98 ZIP payload hashes matched the
Setup manifest. ZIP SHA-256: `580FC81910C1B0A3E861F5549621AA3053EA1847471265A0ADB37FE057C5BA87`;
Setup: `5D7D85553DA395D90351AFF30DEA61D167CF0746202E3E5FD79E2BD63CA9B9BC`.

The mux fault test and four independent media fixtures passed with the rebuilt
DLLs: AV1, HEVC, HEVC with B-frames and HEVC with closed-GOP B-frames, each
media fixture checking completed and interrupted recordings. The 203-test
run above predates this dependency rebuild. Fresh-install and upgrade tests
remain pending; the matching installer test kit is under
`outputs/rc-path-clean-230/setup-check/`. The publication gates above still apply.
