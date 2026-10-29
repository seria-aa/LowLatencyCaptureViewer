<img align="right" src="images/app-icon.png" width="40" height="40" alt="LLCV">

# Release guide

[README](../README.md) · [Documentation](INDEX.md) · [한국어](RELEASING.ko.md)

[1. Versioning](#1-versioning) · [2. Build and verify](#2-build-and-verify) · [3. Package](#3-package) · [4. Publish](#4-publish)

Use this checklist to keep versioning, builds, packages, and GitHub releases
consistent. The `agent/release-v2.1.9` branch below is an example name, not a
confirmation that the branch exists. Before publication, confirm the actual
release branch and update the commands to match.
All executable examples below are for **v2.1.9**, using the compact build-folder
name `build-v2190-release`. For another version, update the version, build
folder, and output paths together; do not substitute a dotted version into
the build-folder name and leave the packaging defaults unchanged.

<br>

## 1. Versioning

Use `v2.1.9` for the Git tag and app version label. Resource and installer
version fields use the numeric version without `v`. Keep these locations aligned:

- `project(... VERSION ...)` in `CMakeLists.txt`
- `kAppVersionLabel` in `src/main.cpp` (the updater derives its User-Agent from it)
- `APP_VERSION_NUMBER` and `APP_VERSION_STRING` in `src/app.rc`
- the version, build directory, output name, and `VersionInfoVersion` in
  `installer/LowLatencyCaptureViewer.iss`
- default build/output paths in `tools/package-v1.ps1`
- current-version headers in `BUILD_INFO.txt`, `DEPENDENCIES.txt`, and `실행안내.txt`
- `docs/release-notes-v2.1.9.md`
- current version/changes in both READMEs and guides, settings example images and captions

Search for stale active version strings after editing. Historical release
notes and changelog entries keep their original version numbers.

```powershell
rg -n "2\.1\.8|build-v2180-release" CMakeLists.txt src installer tools DEPENDENCIES.txt 실행안내.txt
```

<br>

## 2. Build and verify

Keep LLCV_UPSCALING_DIAGNOSTIC and LLCV_SPATIAL_PROTOTYPE disabled in the release
build directory. Overflow telemetry and manual comparison tools are not release features.

Keep `LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC=OFF` for release builds. The held prototype
is compiled only in a separate opt-in diagnostic build with its own executable name/version.
Installer, portable and CMake-install paths reject that build; a stale regular EXE in a
diagnostic build directory must not be reused. Verification is recorded in
`tests/RELEASE_V2_1_4_AUDIT.md`.

Use newly built isolated-profile test/private-preview executables for validation.
Launching the regular app without diagnostic arguments still uses actual user settings.
See `tests/RELEASE_V2_1_9_AUDIT.md` for the current validation scope and
`tests/PROFILE_ISOLATION.md` for the isolation policy.

Use an x64 Visual Studio developer PowerShell at the repository root. Set the
console to UTF-8, produce a Release x64 build, pass every registered test, and
check the diff.

```powershell
chcp.com 65001 > $null
.\tools\Build-RecordingFFmpeg.ps1
cmake -S . -B build-v2190-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC=OFF -DLLCV_RECORDING_FFMPEG_ROOT="$PWD/outputs/dependencies/recording-ffmpeg/install"
cmake --build build-v2190-release
ctest --test-dir build-v2190-release --output-on-failure
git diff --check
```

When hardware is available, briefly verify Shared/ASIO, the selected capture
format, diagnostics and settings (default keys: Tab/F2), changed shortcuts,
default restoration, and a clean exit/restart.
Report any hardware checks not performed. Automated regressions and simulated
long runs do not establish real-device compatibility or guarantee uninterrupted
long-duration playback.

<br>

## 3. Package

Build exactly these two release assets:

```text
LowLatencyCaptureViewer_v2.1.9_Setup.exe
LowLatencyCaptureViewer_v2.1.9_x64.zip
```

```powershell
chcp.com 65001 > $null
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-v1.ps1 `
  -Version 2.1.9 -BuildDir ..\build-v2190-release -OutputDir outputs\v2.1.9
& "C:\Program Files\Inno Setup 7\ISCC.exe" `
  "--define=BuildDir=..\build-v2190-release" ".\installer\LowLatencyCaptureViewer.iss"
```

Do not ship `settings.ini`, `%LOCALAPPDATA%` logs, `build-*` directories, PDB
files, ILK files, or test executables. Verify the executable inside the ZIP and
the installer version before uploading: v2.1.9 uses a numeric file version of
`2.1.9` or `2.1.9.0`, not the previous release's executable under a new ZIP name.

Keep upscaling experiment reports, comparison images, shader sources and manual test tools out of
user packages. Production NIS/CMAA2 shaders are embedded; their notices must still be shipped.

Both packages must contain these third-party notices at their top level:

- `ASIO-SDK-LICENSE.txt`, copied from `third_party/asio/LICENSE.txt`
- `ASIO-HOST-LICENSE.txt`, copied from `third_party/asio/HOST-LICENSE.txt`
- `avformat-62.dll`, `avcodec-62.dll`, `avutil-60.dll`: pinned mux runtime
- `ffmpeg-licenses/`: LGPL license, FFmpeg license summary, source/build notice and configuration
- `PRETENDARD-LICENSE.txt`, copied from `third_party/pretendard/LICENSE.txt`
- `NIS-LICENSE.txt`, `CMAA2-LICENSE.txt`, `UPSCALING-NOTICE.txt`, from the shipped components in `third_party/upscaling`

The application `LICENSE` is required as well. Check the actual ZIP, not just
the staging directory; the following read-only check catches missing notices
and common accidental build/settings files.

```powershell
Add-Type -AssemblyName System.IO.Compression.FileSystem
$releaseArchive = [IO.Compression.ZipFile]::OpenRead(
  (Resolve-Path 'outputs\v2.1.9\LowLatencyCaptureViewer_v2.1.9_x64.zip').Path)
try {
  foreach ($required in @('LowLatencyCaptureViewer.exe', 'LICENSE',
      'ASIO-SDK-LICENSE.txt', 'ASIO-HOST-LICENSE.txt', 'PRETENDARD-LICENSE.txt',
      'NIS-LICENSE.txt', 'CMAA2-LICENSE.txt', 'UPSCALING-NOTICE.txt',
      'README.md', 'README.ko.md', 'DEPENDENCIES.txt', '실행안내.txt')) {
    if ($null -eq $releaseArchive.GetEntry($required)) {
      throw "Missing package file: $required"
    }
  }
  $unexpected = $releaseArchive.Entries | Where-Object {
    $_.FullName -match '(^|[\\/])(settings\.ini|logs|build|build-[^\\/]+)([\\/]|$)|\.(pdb|ilk|obj|log)$|(^|[\\/])[^\\/]*Tests\.exe$'
  }
  if ($unexpected) { throw "Unexpected package files: $($unexpected.FullName -join ', ')" }
} finally {
  $releaseArchive.Dispose()
}
```

Also inspect the generated installer's payload/file list and confirm that the
ASIO/Pretendard notices and `LICENSE` are included. `cmake --install` is a separate path;
its success alone does not verify the ZIP or Inno Setup contents.

<br>

Run `tools/Test-ReleasePackage.ps1` on the staged package and again on a separately extracted ZIP.
It checks recording DLLs, notices, required guides and executable version. Confirm the same DLLs/notices in the installer manifest.

## 4. Publish

Review the worktree and stage only confirmed files before committing.

```powershell
git status --short
git diff --cached --check
git commit -m "Prepare v2.1.9 release"
git push origin agent/release-v2.1.9
```

Publish from the branch HEAD using the matching release-notes file. Add
`--prerelease` only when a beta is intended; **v2.1.9 is a regular release**.

```powershell
gh release create v2.1.9 `
  "outputs\v2.1.9\LowLatencyCaptureViewer_v2.1.9_Setup.exe" `
  "outputs\v2.1.9\LowLatencyCaptureViewer_v2.1.9_x64.zip" `
  --repo seria-aa/LowLatencyCaptureViewer `
  --target agent/release-v2.1.9 `
  --title "LLCV v2.1.9" `
  --notes-file ".\docs\release-notes-v2.1.9.md"
```

Never overwrite published tags or assets. Fix a published-release issue in the
next patch version instead.

---

[README](../README.md) · [Documentation](INDEX.md) · [한국어](RELEASING.ko.md)
