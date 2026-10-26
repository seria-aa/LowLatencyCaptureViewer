# LLCV 2.1.8 release validation

## Status and scope

Validated on 2026-10-26. Version metadata, packaging defaults and Korean/English
documentation target 2.1.8. The production build and both release assets passed
the final gates below.
The regular app is built but not launched; verification uses isolated test profiles.

- NIS signed-coordinate edge handling and CMAA2 processing-list bounds are corrected.
  An overflowing dense frame skips CMAA2 while retaining NIS scaling.
- UYVY/HDYC 8-bit 4:2:2 playback and source screenshots are supported. Auto selection
  still prefers NV12, then YUY2, before UYVY. Explicit format/color metadata and
  manual SDR range choices are preserved; HDYC supplies a BT.709 default.
- UYVY is repacked on the GPU using separate input/output surfaces. Existing
  NV12/YUY2/P010 paths do not allocate these surfaces or issue that compute pass.
- UYVY/HDYC recording is rejected before creating a recording file or worker.
  Existing NV12/YUY2/MJPEG recording remains supported.
- Unsupported-only format lists are distinguished from empty lists/query failures
  in settings and diagnostic logs.
- Overflow diagnostics remain opt-in, profile-isolated tools, excluded from the
  regular release. Development records remain in the repository, not the package.

## Functional evidence before the version bump

The final functional regression run passed **80/80** tests in **416.38 seconds**.
The subsequently expanded UYVY VSR test passed **1/1** in **3.87 seconds**, and the
final documentation check passed **1/1** in **5.32 seconds**. These overlap the
80-test run and must not be added together as unique tests.

Coverage includes capture format/color negotiation, hardware/WARP byte-exact GPU
repacking through 4K, settings/UI/shortcuts, SDR/HDR output, VSR/CMAA2-NIS/OSD,
source screenshots, existing recording paths, pause/split, HEVC sync and
HEVC/AV1 forced-termination recovery. Media and HDR checks use synthetic inputs.
Intensity Pro 4K hardware compatibility and physical display latency were not measured.

Local raw evidence (generated, not committed):

- outputs/uyvy-support/verified-regression-tests.log
- outputs/uyvy-support/uyvy-vsr-tests.log
- outputs/uyvy-support/precommit-doc-tests.log

Reproduction details and measured performance ranges are in
[UYVY/HDYC validation](../docs/UYVY_HDYC_SUPPORT.md),
[upscaling safety](../docs/UPSCALING_BOUNDARY_SAFETY.md) and
[overflow diagnostics](../docs/UPSCALING_OVERFLOW_DIAGNOSTIC.md).
The NV12 GPU before/after comparison did not show a repeatable slowdown;
this is not a claim of zero cost or a physical HDMI-to-display latency measurement.

## Preparation checks after the version bump

The regular Release executable built successfully in build-v2180-release with
recording enabled, upscaling diagnostics, audio-recovery diagnostics and spatial
prototypes disabled. FileVersion and ProductVersion both read 2.1.8.
The application was not launched.

All **10/10 preparation tests passed** in **33.05 seconds**:

- SettingsStoreTests, SettingsStoreSaveFailureTests, SettingsStoreEmptyCacheTests
- UpdateCheckerTests and TestProfileTests
- PackageDocsTests and ReleasePackageTests
- DirectShowVideoFormatTests, UyvyRepackerHardware and UyvyRepackerWarp

Evidence: outputs/v2.1.8-validation/preparation-build.log and preparation.log.
The package test staged files temporarily; it did not produce or publish an
installer or portable ZIP. It verified matching release notes, excluded development
records, runtime dependencies and version metadata. Independent recording tools
are configured for the later complete suite; they were not run in this preparation pass.

## Preserved files

SHA256 values unchanged before and after preparation and the full regression run:

- Actual settings.ini: 9F43DF235D5AF61C853D318DB2F01CD71788639DC5361D72A46D97E4D8C599D0
- Actual recording-prototype.ini: 2D1C8F5D136F8A812E78C5558C32C9BC690751923AAB20D8A77A0BEB144C406A
- Published 2.1.7 build executable: B4A7CF19B999C7A9E21E4C0514490ABC4ACC53C0A4B5192FE520418204FE92E2

## Final release gates

All default targets built successfully in build-v2180-release. All **175/175**
registered tests passed, with zero failures or skips, in **862.60 seconds**.
Independent recording decode tools were configured and their tests ran.
CTest used up to four concurrent tests while respecting RUN_SERIAL isolation.
Evidence: outputs/v2.1.8-validation/full-regression.log and full-regression.xml.

This includes existing settings/shortcuts, SDR/HDR playback, VSR/CMAA2-NIS/OSD,
UYVY hardware/WARP conversion, screenshots, audio-device changes, recording color
and gain/mute isolation, pause/split, storage delays, HEVC sync, HEVC/AV1 independent
decoding and NV12/YUY2/MJPEG/HEVC/AV1 forced-termination recovery. Hardware capture
compatibility and physical display latency were not remeasured in this release pass.
Actual settings and the published 2.1.7 executable retained the hashes above.

The actual portable ZIP was extracted separately and passed release-package and
documentation validation: 70 documents and 413 local links. All 90 installer
manifest source hashes matched the current source/build files; every payload hash
was also present in the ZIP, and vice versa. The ZIP executable matches the regular
Release build. App FileVersion/ProductVersion are 2.1.8; installer FileVersion is
2.1.8.0. Neither package includes user settings, test executables or development-only
upscaling/UYVY reports. The installer was compiled and inspected, not installed.

The initial local artifact-check script compared the padded Inno Setup version string
without trimming and rejected it. Its numeric parts were already 2.1.8.0. Trimming
the resource string corrected the checker; the installer was not changed or rebuilt.

Evidence: outputs/v2.1.8-validation/full-build.log, installer-build.log and artifacts.log.

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| LowLatencyCaptureViewer.exe | 9381888 | 24BCC099FABB2544B24EFE97E0C96FB649E8AC524A626CABE7C423C2A8BCD331 |
| LowLatencyCaptureViewer_v2.1.8_Setup.exe | 6179071 | 16AF26D53AD8213A454AE0D6256A686918FFD773B346CDDFF5799A0311E7DD5D |
| LowLatencyCaptureViewer_v2.1.8_x64.zip | 5869313 | A9F738EC59A0D8459A8F3AF2B1EA11259ECC7C31962C042091162C3B7CFE28A3 |
