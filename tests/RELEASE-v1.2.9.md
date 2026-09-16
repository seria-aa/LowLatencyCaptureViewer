# v1.2.9 release verification — 2026-09-16

- Clean MSVC x64 Release build: `build-v1290-release`, successful.
- Final CTest: 26/26 passed, 93.61 seconds; GPU HDR test 4.14 seconds.
  Includes 76,800 HDR-policy combinations, settings/visibility/persistence,
  synthetic GPU pixel readback, monitor/output transitions, and audio replay.
- Pre-version-change HDR policy/GPU/settings tests also passed five repeated runs.
- HDR Tab panel: neutral black, 90% opacity in linear-light composition;
  BGRA8 alpha quantization, UI-white variation, hide/show/cache refresh,
  outside-video preservation and SDR theme restoration checked.
- Private frame audit, scRGB prototype and vendor tone-mapping control are OFF.
  Corresponding diagnostic markers are absent from the release executable.
  Existing audio/video defaults and processing paths are retained.
- App: 647,680 bytes; file version 1.2.9.
  SHA-256: 8848E6F1AC157B81DF7FB6B8834CE65489CD252CA4915CBC957AE4DB08A24676
- Portable ZIP: 408,796 bytes; all 43 file hashes match packaging inputs.
  Required application and ASIO notices present. No user settings, logs,
  debug symbols, build directories or test executables included.
  SHA-256: 8446BB0CA4BC8AD38B5310677D1595F576D7F7A8E15F1FB96B869EEA04F3498F
- Inno Setup 7.1.0 installer: 2,359,125 bytes; file version 1.2.9.0.
  All 43 compiler-manifest source hashes verified; application and ASIO notices
  included. SHA-256: 9B38E0202D7FD8B127BAC10FE57991EEF008A4E6B9FD347451B6260C635C7DC6
- Installation, physical-device capture and physical HDR visual testing were
  not performed for this release. Automated tests do not establish actual
  device color accuracy or end-to-end latency. P010 HDR10 remains experimental.
