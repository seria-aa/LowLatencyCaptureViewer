# v1.2.12 release verification

- Clean x64 Release build: MSVC 19.39, static runtime, Ninja.
- All 30 registered CTest tests passed, including SDR/HDR GPU tests,
  WASAPI Shared replay, surround replay, settings, and monitor-move regressions.
- Audio-only native offscreen rendering inspected at 380x230 and 760x460.
- Native font metrics checked at 100%, 125%, 150%, 175%, 200%, 250%, 300% DPI.
- Verified frame/card/hover colors, boost on/off, clipping, and 1,000 redraws
  without an increase in GDI object count.
- Simulated real window handlers cover drag from all interior regions,
  click/double-click separation, capture cancellation/loss, borderless and
  decorated windows, fixed-aspect resize, DPI changes, and saved dimensions.
- Existing video-mode geometry and audio processing regressions passed.
- Diagnostic HDR frame audit, scRGB prototype, and vendor tone-map flags OFF.
- Installer and ZIP executable hashes match the tested release executable.
- Both packages include application and ASIO license notices. ZIP excludes
  settings, logs, test executables, PDBs, and build outputs.
- EXE version: 1.2.12; installer numeric version: 1.2.12.0.

## Scope limits

This pass did not repeat physical capture-device playback or long-duration
hardware tests. Window-message simulation does not exercise the real Windows
modal drag loop. Automated tests and offscreen rendering do not guarantee
behavior on every device, monitor, or driver.
