# v1.2.8 release verification — 2026-09-16

- MSVC x64 Release build: build-v1280-release, successful.
- Final CTest: 25/25 passed, 246.51 seconds, including HDR GPU readback,
  settings, output transitions, Shared audio replay and extended replay.
- HDR panels use 90% opacity; text and linear-light composition are retained.
  SDR panel opacity remains 90% / 90% / 92%.
- App: 643,072 bytes, file version 1.2.8.
  SHA-256: 8EA3846A5D362DD6E276525C83CED0F974FE3F294307C22AEEADD421E0ACEDCB
- Portable ZIP: 405,535 bytes. All 42 file hashes matched packaging inputs.
  Required application/ASIO notices included; no settings, runtime logs,
  build directories, debug symbols or test executables included.
  SHA-256: B9B0D903189600B954CA8FA33398ABB0D8122374A3AC0020604D71A40EA04877
- Inno Setup 7.1.0 installer: 2,357,314 bytes, file version 1.2.8.0.
  All 42 compiler-manifest source hashes matched. Application LICENSE and
  both ASIO notices are included by the installer mapping.
  SHA-256: 4B6F18442824A9B6AC3C5BF2CFB3A9858A884B019EC85E5B235B5D867A43997D
- Whitespace validation passed. Installation and physical HDR visual validation
  were not performed. Automated tests are not long-session device validation.
- Earlier HDR audit entries describe intermediate builds, not these final hashes.
- Assets remain local pending release-note review; no publication in this step.
