# v2.2.1 release preparation

Date: 2026-11-01 (Asia/Seoul).

## Repository and scope

- Actual preparation branch: `release/v2.2.1`, created from local
  `test/text-aa-comparison` at base HEAD `ed418c6` (merged v2.2.0 main).
- Existing AA/HDR/UI work is preserved. The preparation stage did not commit,
  publish or merge. The subsequent release request authorizes those operations
  after the final gate below, followed by cleanup of merged work branches.
- Historical audits/merge records retain their original branch names.

Changes from the merged 2.2.0 baseline:

1. SDR High/Preserve CMAA2 selection, retaining default High and NIS 0.15.
2. HDR10 NIS + original-reference contour protection, without HDR CMAA2
   allocation/dispatch. SDR correction preferences survive HDR/F6 transitions.
3. Side-by-side AA/sharpness fields; screenshot preferences in App settings;
   consistent spacing and native forward/reverse keyboard/reading order.
4. App settings Release notes button beside the manual update button. It
   opens a fixed official GitHub URL for the current version in the browser,
   without an update query, automatic download, installation or settings write.

The notes link ignores internal diagnostic suffixes and falls back to the
official releases list for invalid labels. Tests check URL generation without
opening a browser. The v2.2.1 tag page does not exist until publication.
The button stays available while an update check disables its own button.
Compact layouts put the buttons side by side or stack them on narrower pages,
retaining no-scroll pagination. Tab order places it before screenshot controls.

## Version and documentation alignment

- CMake, app label, numeric/string resources and installer: 2.2.1 / 2.2.1.0.
- Packaging/default validation/docs-export paths: `build-v2210-release`,
  `outputs/v2.2.1`; diagnostic suffixes remain explicitly separate.
- README.md remains Korean and exactly matches README.ko.md. README.en.md,
  bilingual usage/video/build/release guides, indexes, launch notes and build
  information point at the new notes/audit without rewriting history.
- Release notes describe only the current changes, not abandoned HDR AA
  prototypes, earlier releases or unverified speed claims. HDR recording is
  not advertised. Existing 2.1.6 images keep their truthful historical caption.

## Existing evidence, not a final v2.2.1 release gate

- `tests/HDR_NIS_SIMPLIFICATION.md`: HDR contour/root-cause/quality and corrected
  GPU timing evidence. One accepted saved menu frame, not an independent
  gameplay sequence or physical HDR display-latency certification.
- Previous targeted 21-case gate and 15-case screenshot-order follow-up passed.
- User-supplied external results: initial suite 198/199 passed;
  RecordingYuy2CrashRecovery subsequently passed 5/5 isolated runs (latest
  6.86 s). These are reported results, not an independently replayed run here.

## Preparation validation

Fresh `build-v2210-release`: Release x64, recording/testing ON, private recovery,
spatial, overflow, HDR audit/scRGB and VSR experiment flags OFF. Independent
FFmpeg/FFprobe/Python test tools are explicitly registered using the existing
local dependencies; no dependency version is changed.

The normal app and selected isolated test/preview targets built successfully.
Normal and UI-preview executable file/product versions both report `2.2.1`.
The fresh configuration registers all 199 tests; only the selected preparation
gate below was executed, not the entire suite.

All 9 selected tests passed, zero failures (255.29 s): SettingsViewTests,
SettingsThemeTests, RecordingThemeTests, VsrExperimentTests,
RecordingSettingsCancelTests, SettingsStoreTests, UpdateCheckerTests,
PackageDocsTests and TestProfileTests. Evidence:
`build-v2210-release/v221-preparation-regression.xml`.
PackageDocsTests also passed directly (169 checks). README.md/README.ko.md
are byte-for-byte content mirrors; `git diff --check` passed.

The regular app was not launched; GUI tests use compile-time isolated profiles,
never actual settings. Start/end actual profile hashes were unchanged:
`settings.ini = 772A6BD518A5DB4C2D9D94F70C30B49E4491465CB8786C4AFF326D088B03328B`,
`recording-prototype.ini = 2D1C8F5D136F8A812E78C5558C32C9BC690751923AAB20D8A77A0BEB144C406A`.

Optional isolated manual UI preview (not official release assets):
`outputs/LLCV_v2.2.1_UI_Preview.zip`, executable under
`outputs/v2.2.1-ui-preview/LLCV_Upscaling_UI_Test.exe`. Its executable SHA256
matches the fresh UI target:
`63880D2C758CEE37A9C4776D1DA6F25EA8E267FF8747A09E91C3505E15285B87`.
No browser was opened by automated tests; the future v2.2.1 release tag page
has not been published.

## Final release gate

The complete default-target build passed (607 remaining build steps after the
preparation builds); the post-test build reports `ninja: no work to do`.
All **199/199** registered tests passed in one sequential run, **zero failures,
skips or disabled tests**, 1562.91 seconds. No failed case was retried into this
result. RecordingYuy2CrashRecovery passed in 6.29 seconds;
RecordingStorageCrash_hevc passed in 13.07 seconds.

The suite includes native settings/keyboard/theme/help layouts, SDR/HDR/RGB
rendering, hardware/WARP upscaling safety, OSD, VSR, recording pause/split,
audio-device switching, playback mute versus recording audio, independent
decode/A/V timing, storage delays/crash recovery, package docs and test-profile/
diagnostic build boundaries. These are automated tests, not new physical HDMI
latency or HDR monitor qualification.

| Release check | Result |
| --- | --- |
| Normal Release x64 / private diagnostic flags | Passed; all private flags OFF |
| Complete registered regression | Passed, 199/199 in one run |
| Staging and separately extracted ZIP validation | Passed, 94 files with identical hashes |
| Installer payload | Passed; all 94 manifest source hashes/sizes and hash multiset match ZIP |
| App / installer numeric versions | Passed, 2.2.1 / 2.2.1.0 |
| Runtime and notices | Passed; pinned mux DLL hashes and required licenses match |
| Actual playback/recording profiles | Same hashes before/after the complete gate |
| Installer execution / signing | Not installed; unsigned, as with the existing build process |

No actual settings, logs, test EXEs, shader sources or comparison data are in
the release payload. The normal app was not launched against the real profile.
The published notes body uses absolute `blob/v2.2.1/docs/...` links so its
relative documentation links do not break on the GitHub release page.

Local evidence: `outputs/v2.2.1-validation/{full-build.log,final-build.log,
final-regression.log,final-regression.xml,LastTest.log,package-final.log,
installer-final.log,artifacts-final.log,artifacts-post-gate.log}`.
The generated evidence is not a package/source dependency.

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| LowLatencyCaptureViewer.exe | 9635840 | 65828E6BB187C17FD1A0C13606C8DDC295DD7FFE66E3256BA1339D8BDAC398C1 |
| LowLatencyCaptureViewer_v2.2.1_Setup.exe | 6236419 | AA251014CD62EE5D15DA905A69322F4E0D89DE760B0A3E10286894F9D6B17EE4 |
| LowLatencyCaptureViewer_v2.2.1_x64.zip | 5966374 | 06BAAF6683142E679A32DD602DE2E75DE644022BD76F10CF26608F7C87CF8AD1 |

Only reviewed source/documentation paths are staged. Previously published
tags/assets and historical branch names remain unchanged. After publication,
merge the verified release head into main and remove only the merged
`release/v2.2.1` and `test/text-aa-comparison` work branches.
