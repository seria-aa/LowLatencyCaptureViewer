# Release guide

> [한국어](RELEASING.ko.md) · [Back to README](../README.md)

Use this checklist to keep versioning, builds, packages, and GitHub releases
consistent. Release work stays on the existing `agent/release-v1.0.0` branch.
All executable examples below are for **v1.2.8**, using the compact build-folder
name `build-v1280-release`. For another version, update the version, build
folder, and output paths together; do not substitute a dotted version into
the build-folder name and leave the packaging defaults unchanged.

## 1. Versioning

Use `v1.2.8` for the Git tag and app version label. Resource and installer
version fields use the numeric version without `v`. Keep these locations aligned:

- `project(... VERSION ...)` in `CMakeLists.txt`
- `kAppVersionLabel` in `src/main.cpp` (the updater derives its User-Agent from it)
- `APP_VERSION_NUMBER` and `APP_VERSION_STRING` in `src/app.rc`
- the version, build directory, output name, and `VersionInfoVersion` in
  `installer/LowLatencyCaptureViewer.iss`
- default build/output paths in `tools/package-v1.ps1`
- current-version headers in `BUILD_INFO.txt`, `DEPENDENCIES.txt`, and `실행안내.txt`
- `docs/release-notes-v1.2.8.md`

Search for stale active version strings after editing. Historical release
notes and changelog entries keep their original version numbers.

```powershell
rg -n "1\.2\.5\.1|1251" CMakeLists.txt src installer tools DEPENDENCIES.txt 실행안내.txt
```

## 2. Build and verify

Use an x64 Visual Studio developer PowerShell at the repository root. Set the
console to UTF-8, produce a Release x64 build, pass every registered test, and
check the diff.

```powershell
chcp.com 65001 > $null
cmake -S . -B build-v1280-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-v1280-release
ctest --test-dir build-v1280-release --output-on-failure
git diff --check
```

When hardware is available, briefly verify Shared/ASIO, the selected capture
format, Tab diagnostics, F2 settings, and a clean exit/restart.
Report any hardware checks not performed. Automated regressions and simulated
long runs do not establish real-device compatibility or guarantee uninterrupted
long-duration playback.

## 3. Package

Build exactly these two release assets:

```text
LowLatencyCaptureViewer_v1.2.8_Setup.exe
LowLatencyCaptureViewer_v1.2.8_x64.zip
```

```powershell
chcp.com 65001 > $null
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-v1.ps1 `
  -Version 1.2.8 -BuildDir ..\build-v1280-release -OutputDir ..\outputs\v1.2.8
& "C:\Program Files\Inno Setup 7\ISCC.exe" `
  "--define=BuildDir=..\build-v1280-release" ".\installer\LowLatencyCaptureViewer.iss"
```

Do not ship `settings.ini`, `%LOCALAPPDATA%` logs, `build-*` directories, PDB
files, ILK files, or test executables. Verify the executable inside the ZIP and
the installer version before uploading: v1.2.8 uses a numeric file version of
`1.2.8` or `1.2.8.0`, not the previous release's executable under a new ZIP name.

Both packages must contain these ASIO notices at their top level:

- `ASIO-SDK-LICENSE.txt`, copied from `third_party/asio/LICENSE.txt`
- `ASIO-HOST-LICENSE.txt`, copied from `third_party/asio/HOST-LICENSE.txt`

The application `LICENSE` is required as well. Check the actual ZIP, not just
the staging directory; the following read-only check catches missing notices
and common accidental build/settings files.

```powershell
Add-Type -AssemblyName System.IO.Compression.FileSystem
$releaseArchive = [IO.Compression.ZipFile]::OpenRead(
  (Resolve-Path '..\outputs\v1.2.8\LowLatencyCaptureViewer_v1.2.8_x64.zip').Path)
try {
  foreach ($required in @('LowLatencyCaptureViewer.exe', 'LICENSE',
      'ASIO-SDK-LICENSE.txt', 'ASIO-HOST-LICENSE.txt',
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

Also inspect the generated installer's payload/file list and confirm that both
ASIO notices and `LICENSE` are included. `cmake --install` is a separate path;
its success alone does not verify the ZIP or Inno Setup contents.

## 4. Publish

Review the worktree and stage only confirmed files before committing.

```powershell
git status --short
git diff --cached --check
git commit -m "Prepare v1.2.8 release"
git push origin agent/release-v1.0.0
```

Publish from the branch HEAD using the matching release-notes file. Add
`--prerelease` only when a beta is intended; **v1.2.8 is a regular release**.

```powershell
gh release create v1.2.8 `
  "..\outputs\v1.2.8\LowLatencyCaptureViewer_v1.2.8_Setup.exe" `
  "..\outputs\v1.2.8\LowLatencyCaptureViewer_v1.2.8_x64.zip" `
  --repo seria-aa/LowLatencyCaptureViewer `
  --target agent/release-v1.0.0 `
  --title "Low Latency Capture Viewer v1.2.8" `
  --notes-file ".\docs\release-notes-v1.2.8.md"
```

Never overwrite published tags or assets. Fix a published-release issue in the
next patch version instead.
