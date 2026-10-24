# LLCV 2.1.7 release validation

## Status

Validated on 2026-10-25. Version metadata, packaging defaults and Korean/English
documentation target 2.1.7. The production build and release packages passed the gates below.
Published 2.1.6 artifacts and actual user settings remain untouched.

## Scope

- F6 toggles the selected CMAA2 → NIS or NVIDIA VSR method, retaining NIS sharpness.
- Enabled legacy VSR profiles retain VSR. With no method history, the first F6 enables
  CMAA2 → NIS at default sharpness 0.15. A fresh launch still defaults to Off.
- The persisted shortcut action name remains Vsr so existing customized bindings survive.
- Help and HUD describe the selected method and distinguish Off, HDR/scale bypass and failure.
- A version-specific startup review overrides immediate start once. Its marker is written
  only after the settings window is displayed, including when subsequently cancelled.
  The immediate-start preference and unaccepted settings are not overwritten.
- Marker writes share the atomic INI replacement and real-profile protection used by settings.
  Failed acknowledgement leaves the previous file intact and retries next launch.

## Regression evidence before the version metadata update

The final functional re-audit passed **41/41 tests**, with zero failures or skips,
in 241.89 seconds. Local evidence: outputs/f6-upscaling-validation/reaudit.log and reaudit.xml.
The earlier 41-test pass additionally covered settings layout/theme and documentation/build checks;
these are overlapping runs, not 82 unique tests.

Covered legacy VSR migration, retained Off/On selection and sharpness, custom shortcuts,
startup version acknowledgement and save failures, real-profile access denial, settings cancellation,
SDR/HDR/VSR output, OSD, screenshots, FPS selection, exclusive-viewer lifecycle, audio callbacks,
recording gain/mute/device changes, pause/split, HEVC sync, independent HEVC/AV1 decoding,
and MJPEG/NV12/YUY2 forced-termination recovery.

The saved same-source SDR reference comparison had a maximum channel difference of 1/255,
within the existing CMAA2 rounding tolerance. Capture hardware and optical display latency
were not remeasured in this re-audit.

Actual profile SHA256 values matched before and after the re-audit:

- settings.ini: F02DE80CA9F4CEED2C5F7D354B2AD2102816855FEEC6026EC059B53463322882
- recording-prototype.ini: 2D1C8F5D136F8A812E78C5558C32C9BC690751923AAB20D8A77A0BEB144C406A

## Preparation checks after the version metadata update

The regular Release executable rebuilt successfully with recording enabled and diagnostic recovery
disabled. Its FileVersion and ProductVersion both read 2.1.7. The application was not launched.

All **7/7 preparation checks passed** in 27.60 seconds: settings storage, save failure,
empty-cache preservation, update checking, packaged documentation links, CMake-install package
contents/version checks and real-profile isolation. Evidence:
outputs/f6-upscaling-validation/v217-preparation.log.
The package test used a temporary staging directory, not a published installer or portable ZIP.
Actual settings hashes remained identical to the values above.

## Final release gates

A fresh production Release build in build-v2170-release completed with recording enabled,
audio-input recovery diagnostics disabled and private spatial prototypes disabled.
All **171/171 registered tests passed**, with zero failures, disabled tests or skips,
in 1013.37 seconds. Evidence: outputs/v2.1.7-validation/full-regression-unrestricted.log
and full-regression-unrestricted.xml. Tests used isolated profiles throughout.

The initial restricted run failed at RecordingShortcutTests' temporary-profile save and was
stopped. The identical executable passed that test in the normal execution environment without
code changes, then passed it again in the complete 171-test run. The restricted attempt is not
counted as a successful regression run.

The fresh build's approved same-source SDR image comparison passed with maximum channel
difference **0/255**. The two actual profile hashes still matched the baseline above.
No physical HDMI-to-display latency measurement was performed in this release validation.

The real portable ZIP was extracted to a separate validation directory and passed both
Test-ReleasePackage.ps1 and Test-PackageDocs.ps1. Its executable matches the production
build hash. FileVersion/ProductVersion are 2.1.7; the installer resource version is 2.1.7.0.
All 89 installer manifest source hashes matched. Neither package includes user settings,
test executables, compiler artifacts or private prototype assets. The installer was compiled
and its manifest checked, not installed over the user's application.

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| LowLatencyCaptureViewer.exe | 9368064 | B4A7CF19B999C7A9E21E4C0514490ABC4ACC53C0A4B5192FE520418204FE92E2 |
| LowLatencyCaptureViewer_v2.1.7_Setup.exe | 6172688 | 0ECA30774496991ED236D8A2CDDB548617E46A3E47F672C79A8608A97D57951F |
| LowLatencyCaptureViewer_v2.1.7_x64.zip | 5860367 | 1174ED6FD6A982B5FEA7C3B58F59BF71670E11FCBB3F9DF1E39BFCD5AB28EEE7 |
