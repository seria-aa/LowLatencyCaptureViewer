# v2.1.9 release preparation audit

Date: 2026-10-29 (Asia/Seoul)

## Scope

Changes since v2.1.8: manually selected RGB24/XRGB SDR playback, source screenshots,
VSR requests and SDR/stereo recording; worker-only RGB conversion/downscaling;
bounded RGB frame pools and pre-file conversion admission; detailed layout rejection
logs; strict initialization-tail recovery checks and developer-document exclusion.

No new HDR recording, HDR CMAA2→NIS, CPU AV1 or audio automatic recovery is included.
Auto pixel-format priority, default 1080p60 recording caps and shortcut defaults remain unchanged.
RGB recording stores BT.709 Limited NV12 (4:2:0), not lossless RGB.

## Evidence before the version/document bump

- Broad regression: 192/192 passed, zero skipped, 800.07 seconds.
  outputs/rgb-hardening/regression.log, regression-details.log, regression.xml.
- Final affected rerun: 11/11 passed, 16.83 seconds.
  outputs/rgb-hardening/message-tests.log/.xml.
- SettingsViewTests direct executable: 24 factory profiles, eight compact work-area
  sizes/scales, 1,600 combinations and 58 golden rectangles passed.
  outputs/rgb-hardening/settings-view-direct.log.
  Its existing 120-second CTest timeout was not relaxed; this direct result is
  separate from the 192-test CTest pass.
- HEVC and AV1 controlled storage-delay crash checks: five runs each passed;
  all retained 330 video / 516 audio packets. Uninitialized tail exclusion was
  unused in all ten runs. Packet continuity, timestamps, sync and independent
  full decoding remained strict. This is not a real-world failure-rate estimate.
- Added real-worker native 4K60 RGB32 admission integration passed: insufficient
  conversion headroom produced the dedicated rejection, 316.41 MiB frame-pool
  budget and no output file. Faster hosts may pass admission without changing FPS.
  outputs/rgb-hardening/admission-integration.log.

## Version/document preparation checks

- CMake, app resource, runtime labels, installer, package/validation/export defaults
  use 2.1.9; future official build/output defaults use build-v2190-release and
  outputs/v2.1.9. Historical release notes remain unchanged.
- Normal application rebuilt successfully in build-xrgb-stage4. PE file version:
  2.1.9. No diagnostic/profile-isolation object is substituted in this normal EXE.
- Post-bump preparation regression: 8/8 passed in 30.65 seconds: startup recording
  options, DirectShow formats, settings persistence, RGB admission policy,
  recovery-tail policy, portable documentation, CMake package staging/validation,
  and test-profile protection. outputs/rgb-hardening/v219-preparation-tests.log.
- Portable documentation checks: 140 passed. Developer RGB report excluded from
  portable/CMake/installer packages; README links target public guides.
- Source diff whitespace check passed.

## Preserved state and limitations

Real profile SHA256 values remained:
- settings.ini: 20AB4EEDB82DCB67D63274BDBCF57A7E26F93842395ECC17400965E4F6D3DF00
- recording-prototype.ini: 2D1C8F5D136F8A812E78C5558C32C9BC690751923AAB20D8A77A0BEB144C406A

Existing hardware coverage remains GC573 RGB24, not a physical RGB32 device.
The user observed a VSR visual difference on RGB24; request acceptance and OSD state
alone are not instrumented certification of RTX model activation.
No new physical end-to-end latency, RGB HDR or sustained native 4K60 qualification
is claimed. A short preflight does not guarantee throughput under changing load.

This is preparation only: no new installer/portable release assets were published,
no commit/tag/PR/release/merge was created, and existing user edits were retained.
The final release gate should rebuild the official release directory, run final
regressions and validate both portable and installer assets before publication.

## Final official release gate

All default targets built successfully in build-v2190-release.
All **193/193 registered CTest tests passed**, zero failures/skips, in
**1014.06 seconds**. SettingsViewTests passed inside CTest in 119.67 seconds;
RecordingStateTests, including the full new real-worker RGB admission scenario,
passed in 8.10 seconds. Independent FFmpeg/FFprobe recording checks were configured.
Evidence: outputs/v2.1.9-validation/{configure,full-build,final-build,final-regression}.log
and final-regression.xml.

An initial fresh-build parallel run timed out in desktop-interacting UI fixtures.
Recording settings, shortcuts, help/IME and settings-theme tests now use RUN_SERIAL;
their assertions and individual time limits remain unchanged. The affected five-test
rerun passed in 85.97 seconds before the complete successful rerun. The 1,600-case
settings matrix deadline was raised from 120 to 300 seconds to accommodate observed
~150-second previous/direct runs, without reducing coverage. The failed attempt is
preserved in initial-parallel-tests.log; it is not reported as a passing run.

The Korean release procedure, BUILD_INFO current entry and both documentation
indexes now include v2.1.9, including absolute GitHub links for developer-only reports.
Historical notes/audits remain intact.

The actual final portable ZIP was extracted separately and passed package/docs checks.
All 91 installer manifest source hashes match current source/build files, and the
installer and ZIP payload hash multisets match exactly. Normal EXE/installer versions
are 2.1.9 / 2.1.9.0. No real settings, logs, test executables or developer RGB report
are shipped. The installer was compiled and inspected, not installed.
Evidence: outputs/v2.1.9-validation/{package-final,installer-final,artifacts-final}.log.

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| LowLatencyCaptureViewer.exe | 9409024 | A28B5E80EED8346A243F20DF05CFF1A68A4722CAC959C227786A009390D9F5A4 |
| LowLatencyCaptureViewer_v2.1.9_Setup.exe | 6189558 | A56B396E0F46CA35380B16CCB72AE9171966260EE3F1E2487C1BF7188EE60D88 |
| LowLatencyCaptureViewer_v2.1.9_x64.zip | 5887444 | 03B062EF6506CD79426AEB169848DA44D6943780E1AD4DD9DF2BC4DE7B56B26D |

Actual profile hashes above and the published v2.1.8 EXE hash
24BCC099FABB2544B24EFE97E0C96FB649E8AC524A626CABE7C423C2A8BCD331
remained unchanged. No new physical HDMI latency or RGB32 hardware qualification
is claimed. These completed gates supersede the preparation-only status above.
