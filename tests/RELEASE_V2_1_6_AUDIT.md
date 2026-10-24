# LLCV 2.1.6 release preparation

## Status

Prepared on 2026-10-25 from v2.1.5 plus the current working-tree changes.
This record covers local preparation, not publication or merge.
Version metadata, packaging defaults and Korean/English documentation target 2.1.6.
The installed application and actual user profiles are not replaced.

## Scope

- Opt-in CMAA2 before NIS for SDR enlargement, default sharpness 0.15.
  Off/CMAA2 → NIS/NVIDIA VSR share one selector; existing VSR preferences and F6 behavior remain.
- HDR, native size, downscaling and enlargement beyond 2× per axis bypass the new filter.
  SDR preference is retained across HDR playback. OSD is drawn after filtering.
  Recording and screenshots continue to use the capture source.
- Consistent settings-tab spacing, alignment and footer margins, including auxiliary controls,
  detected-format text, screenshots and conditional sharpness controls. No scrolling is introduced.
- Removed redundant shader creation/binding and temporary UI allocations without changing shader math,
  pass count or capture queues. Removed unused HDR-filter code.
- Explicit manual preview/benchmark targets, precise shader dependencies and package report exclusions.
  Shipped NIS/CMAA2 notices are retained in installer, portable and CMake install paths.
- Isolated test/private-preview profiles. The regular application's non-diagnostic launch remains unchanged.

## Verification before the version metadata update

Cleanup re-audit: **59/59 passed, zero failed or skipped**, in two invocations (52 + 7).
Includes settings storage/migration/cancellation, shortcuts, UI layout/theme,
SDR/HDR/VSR rendering, screenshots, OSD, playback mute/recording isolation,
audio-device changes, MJPEG playback during recording/failure, pause/split, HEVC sync,
archive safety, build boundaries and package contents.

The approved same-source CMAA2 → NIS 0.15 reference comparison passed with maximum
channel difference 1/255, within the existing CMAA2 rounding tolerance.
Synthetic FHD-to-QHD GPU processing averaged 0.351055 ms off and 0.952806 ms on
over three alternating rounds of 120 samples per mode: incremental cost 0.601751 ms.
This is GPU processing, not capture-to-display latency or a paired cleanup-before/after benchmark.

Local evidence (generated, not shipped):

- outputs/cleanup-recheck-20261025/result.json
- outputs/cleanup-recheck-20261025/regression.log
- outputs/cleanup-recheck-20261025/extra-regression.log
- outputs/cleanup-recheck-20261025/gpu/timings.csv

Actual settings SHA256 values were identical before and after:

- settings.ini: B6B33F5448F16B4DC3D26DFD78D13B0FF910A436D87A48D05DA89E971474EB98
- recording-prototype.ini: 2D1C8F5D136F8A812E78C5558C32C9BC690751923AAB20D8A77A0BEB144C406A

## Development storage cleanup

Historical generated BMP snapshots were archived with manifests and verified SHA256 payloads
before original removal. Extract an archive into the repository root to restore its outputs/... tree.
Current comparison images, UI previews and non-BMP evidence were retained.
Obsolete build-object files were removed only from validated historical build directories;
they can be regenerated. These changes recovered 9.573 GiB in the development workspace,
not in the downloadable application.

The archive tool defaults to a dry run and limits changes to a fixed historical allowlist.
It is not run during normal application startup, build or installation.
Use tools/Compress-TestSnapshots.ps1 for the inventory and tests/TestSnapshotArchiveTests.ps1
for its isolated safety fixtures.

## Release gates

- Incremental Release rebuild succeeded for the application, CmaaNisTests and documentation-rendering tools.
  The application FileVersion and ProductVersion are both 2.1.6.
- Post-update focused checks passed **8/8**, zero failed or skipped, in 69.95 seconds:
  SettingsViewTests, CmaaNisRegression, CmaaNisHdr, CmaaNisRecording,
  PackageDocsTests, ReleasePackageTests, ManualToolBuildTests and TestProfileTests.
  Log: outputs/readme-v2.1.6/preparation-regression.log.
- Korean/English video, audio, recording and shortcut images were regenerated from isolated native UI fixtures
  and visually inspected. Video examples select CMAA2 → NIS at 0.15 with FHD capture and QHD display.
  Help-image export also passed its lifecycle/keyboard checks.
- Actual user-profile hashes remain identical to the values above.
- After the final image/text changes, PackageDocsTests and ReleasePackageTests passed again (2/2, 7.33 seconds).
  Log: outputs/readme-v2.1.6/final-package-docs.log. The whitespace check also passed.
- The final release gates below supersede the pending full-suite/package checks from preparation.
- No tag, release, branch publication or merge was performed during documentation preparation.
- Preserve existing work and review explicit source/test paths before staging; do not stage all of outputs.

## Final release verification (2026-10-25)

- Clean MSVC Release/Ninja build in `build-v2160-release` succeeded (780 build steps).
  Recording is enabled; spatial prototype, audio recovery diagnostic, HDR frame audit,
  scRGB prototype and VSR experiment are all disabled. The initial restricted compiler
  probe failed with C1902; the same toolchain succeeded with normal build permissions.
  No application-code workaround was required.
- Full configured CTest suite: **171/171 passed, zero failures, zero skipped or disabled**,
  1007.22 seconds. Includes settings/migration/cancellation, shortcuts, all settings tabs,
  SDR/HDR/VSR/OSD, capture FPS selection, audio replay, recording audio-device changes,
  MJPEG/NV12/YUY2, pause/split, HEVC sync, storage delay and crash recovery.
  Independent media checks include HEVC/AV1 containers, B-frames and stream offsets.
- The clean-build approved SDR reference passed again: maximum channel difference **1/255**.
  Three alternating synthetic FHD-to-QHD GPU rounds averaged **0.384145 ms off** and
  **0.877483 ms with CMAA2 → NIS 0.15**, incremental **0.493339 ms**.
  These are GPU processing timings, not optical latency or a paired v2.1.5 comparison.
- Portable ZIP was independently extracted and passed the release-package validator.
  Application and all three mux DLL hashes match the clean build.
  Inno Setup compilation succeeded; all **88** manifest source hashes match the inputs,
  required runtime/license files are present, and test/configuration artifacts are absent.
  Installer numeric version is **2.1.6.0**; application FileVersion/ProductVersion are **2.1.6**.
  The installer was not run and the installed application was not replaced.
- Package sizes: installer **6,169,892 bytes**, portable ZIP **5,762,678 bytes**.
  SHA256:
  - EXE: `D2F8DC21BB7B7E45D0AFC9D9C2768EE027063A3ABF8059C9507ED7920FDB7233`
  - Installer: `E8A596B1F61C65D26491C3D36C4B6A32BCE9A67CE8BC64281E9BE12BA7430878`
  - ZIP: `FFC5C258BDA3C256032FECD999793C21D75D0BCF9B3A379EA2788D0C4E1DBBB4`
- Actual viewer and recording profile hashes remain identical to the baseline above.
  No capture hardware session or ordinary app launch was used in this final gate.
- First-party whitespace checks pass. Existing formatting in pinned third-party snapshots
  is preserved, including upstream trailing whitespace. Generated outputs are not staged.

Evidence (local, not shipped): `outputs/v2.1.6-validation/` contains build/package logs,
`ctest.xml`, `ctest.log`, `approved-reference.log`, `gpu/timings.csv` and the extracted ZIP.
