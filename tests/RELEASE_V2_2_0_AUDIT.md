# v2.2.0 release audit

Date: 2026-10-31 (KST)
Branch: `release/v2.2.0` (local branch created for this preparation)
Base HEAD: `42d2be5` (main after Korean-first README merge)

Status: final local release gates passed. This audit is finalized before the
release commit, publication and merge; their commit/tag/PR records are maintained
in Git and GitHub rather than predicting identifiers in this document.

## Scope

- Local diagnostic report ZIPs, report-local ID/path masking and manual sharing.
- Crossbar connection during applicable settings mode queries and playback startup:
  existing input routing is retained; ambiguous/audio input handling, medium-list
  bounds checks and failure rollback are guarded. Playback and settings queries
  reject a graph that could not be restored.
- Adaptive no-scroll F1 help across work areas/DPI/resolution changes.
- Short NIS sharpness presets with preservation of custom saved values.
- Settings probe lifetime/cancellation, bounded startup gate waits and asynchronous
  recording-folder space queries.
- Field-wise recording preference recovery without overwriting the source profile.
- Flip VSync readiness before latest-sample selection; default low-latency,
  exclusive and compatibility pacing are not changed by that wait.
- Deferred playback diagnostics and app-created console QuickEdit handling.
- Internal cleanup and additional regression cases. RGB capture, SDR-only
  recording conditions, upscaler support rules and 1080p60 recording defaults remain.

Diagnostic output is not fully asynchronous: UI draining and some startup/failure
records still perform synchronous I/O. Do not describe this release as immune to
blocked consoles/disks. No new physical display-latency result is claimed.

## Version and documentation preparation

- Align CMake, regular/diagnostic app labels, VERSIONINFO and installer to 2.2.0.
- Set package, validation and documentation-export defaults to
  `build-v2200-release` and `outputs/v2.2.0`.
- Add bilingual release notes; update Korean-first README, its exact README.ko.md
  mirror, English README, indexes, quick-start/dependency/build/release guides.
- Document crossbar scope, existing input routing, diagnostic ZIP privacy limits,
  startup/query safeguards and VSync-only admission.
- Use the actual `release/v2.2.0` branch in both release guides. Historical
  audits retain their original `agent/…` branch names; merge history is not rewritten.
- Settings screenshots keep their explicit 2.1.6 example captions; they are not
  presented as newly captured 2.2.0 images.

## Validation status

Earlier development runs and the user's separate regression checks are not
counted as a fresh 2.2.0 release-gate pass.

| Gate | Status |
| --- | --- |
| Static version/default-path consistency | Passed: 23 metadata/default assertions, bilingual release-guide paths and actual branch name |
| Korean README mirror and language/document links | Passed: README.md and README.ko.md match exactly; latest release links use 2.2.0 |
| Packaged documentation staging/link checks | Passed: final package has 73 documents and 454 local file links, including mux notices and normal exclusions |
| PowerShell syntax and whitespace checks | Passed: three updated tools parse; git diff --check passes (line-ending notices only) |
| Fresh normal Release x64 build in build-v2200-release | Passed: all 809 default build steps; post-test build reports no work |
| Full registered CTest run, including decode/sync/crash recovery tools | Passed: 199/199, zero failures/skips/disabled tests, 1157.76 seconds |
| Portable ZIP and installer generation | Passed: regular 2.2.0 ZIP and Inno Setup 7 installer |
| Test-ReleasePackage on staging and a separately extracted ZIP | Passed on both; all 93 extracted files match staging |
| Installer payload/version verification | Passed: 93 source hashes/sizes and payload hash multiset match ZIP; numeric installer version 2.2.0.0 |
| Final physical capture/display checks | Not performed; native GPU/software and lifecycle regressions are not new HDMI latency or device qualification |

For final validation, use isolated test profiles. Do not launch the normal app
against the actual user profile. Confirm regular builds exclude recovery/HDR/VSR/
upscaling diagnostic prototypes and contain the required recording runtime/notices.
Record skipped or unregistered tests and hardware limitations separately.

Preparation evidence is under
`outputs/v2.2.0-preparation/check-fec6b62b4ef94e5bb11a5975a407f3d2/`:
`metadata-check.log`, `readme-mirror.log`, `package-docs.log`,
`powershell-syntax.log` and `diff-check.log`. These ignored local records are not
shipped. The link checker validates packaged file destinations, not live external
URLs or every Markdown anchor. Newly added local guide anchors were checked against
their headings; GitHub links to this new audit become available after publication
of the corresponding source changes.

Historical branch-only edits in five old audit documents match the base HEAD again.
The regular app was not launched against the actual user profile, no native capture
test was performed and actual playback/recording profile contents were not changed.

## Final release gate and assets

- `outputs/v2.2.0/LowLatencyCaptureViewer_v2.2.0_Setup.exe`
- `outputs/v2.2.0/LowLatencyCaptureViewer_v2.2.0_x64.zip`

Fresh configuration explicitly enables recording/testing and disables private
recovery, upscaling, spatial, HDR and VSR diagnostic prototypes. Independent
FFmpeg/FFprobe and Python tests were registered; no registered test was skipped.
The existing pinned mux-only FFmpeg 8.1.3 runtime is reused and package DLL hashes
match its install directory. The source archive SHA256 matches the pinned build
script value. No dependency version was changed for this release.

Final evidence is under `outputs/v2.2.0-validation/`:
`configure-final.log`, `full-build.log`, `final-build.log`,
`final-regression.log`, `final-regression.xml`, `package-final.log`,
`installer-final.log` and `artifacts-final.log`. The first restricted compiler
check stalled; only that task's compiler processes were stopped and configuration
was rerun successfully in the normal build environment. An initial artifact-check
comparison treated the installer's padded version string as a mismatch; numeric
VERSIONINFO fields verified 2.2.0.0. Neither issue required product code changes.

The complete successful suite includes settings, shortcuts, help/IME/display
layouts, native GPU/WARP color/upscaling/OSD, audio device switching, playback mute
versus recording, RGB/MJPEG/YUV recording, storage delays, pause/split behavior,
independent A/V decoding and crash recovery, profile isolation and diagnostic
build boundaries. These are automated checks, not new physical HDR color or
end-to-end display latency measurements.

All 93 installer manifest source hashes/sizes match the packaging inputs, and
the installer/ZIP payload hash multisets match. The installer was compiled and
inspected, not installed. Actual user profiles still match their pre-gate SHA256:

- settings.ini: A2040ED256FED8DDAAEE5C6D56D2F0076CB81EBD427AE72B42D3974BDE813D5B
- recording-prototype.ini: 2D1C8F5D136F8A812E78C5558C32C9BC690751923AAB20D8A77A0BEB144C406A

| Artifact | Bytes | SHA256 |
| --- | ---: | --- |
| LowLatencyCaptureViewer.exe | 9551360 | A99760A185465663EB685F81A06E820583484B8789229740E8747FD0BA1DA264 |
| LowLatencyCaptureViewer_v2.2.0_Setup.exe | 6232785 | B999748BABDE44CFD1266692A2DE619C284E42CA5C7FD78940CD3D7FCF6EA7A1 |
| LowLatencyCaptureViewer_v2.2.0_x64.zip | 5954499 | A07A00968383DF546FD9A04E9B3284B47CCAB4A6C108A8A07A2D7E58009FA921 |

Published tags/assets for previous versions remain unchanged.
