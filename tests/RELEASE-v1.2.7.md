# v1.2.7 release verification — 2026-09-11

- Fresh x64 MSVC Release configure/build: build-v1270-release, successful.
- CTest: 23/23 passed, 122.34 seconds.
- Local RTX 3080 D3D11 debug replay: 60 rebuild/present cycles, 7,187 concurrent
  synthetic samples, 60 occluded presentations, zero GPU warnings/errors.
  This is a hidden-window API/resource-lifetime check, not visible scanout or
  long-session physical-device validation.
- Application file version: 1.2.7; installer file version: 1.2.7.0.
- Application: 631,808 bytes. SHA-256:
  734642A003026FD21DC2B4414BA08BF84D4FB53C2C163745D1A4B78444F2DA7B
- Portable ZIP: 42 entries; every file hash matched its packaging input.
  Required notices present; no settings.ini, runtime logs, build directories,
  PDB/ILK/object files or test executables.
- Inno Setup 7 compiled-content manifest: 41 unique source entries, all hashes
  matched. App LICENSE and both ASIO notices included. The compiler manifest
  stays local and is not an uploaded asset. Installation was not performed.
- Setup SHA-256:
  6EEEE25D5C0DDF9D2B6991679DF8AF5F60A49777C0A2AD59051A4AD874DA576B
- ZIP SHA-256:
  4D19B0EC95519AAA83CEA3A61FD90494B024E12BAEAD23B409DE4727FC8BCF26
- Whitespace validation passed. Existing ASIO/compiler warnings are not described
  as a warning-free build.
- No new physical MJPEG/P010 HDR/ASIO or long-duration capture tests were performed
  during release packaging. Earlier audit documents are chronological records;
  their intermediate binary sizes and test counts are not final release totals.
