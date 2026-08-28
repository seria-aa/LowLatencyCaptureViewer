# Release guide

> [한국어](RELEASING.ko.md) · [Back to README](../README.md)

Use this checklist to keep versioning, builds, packages, and GitHub releases
consistent. Release work stays on the existing `agent/release-v1.0.0` branch.

## 1. Versioning

Use a `v` prefix only for the Git tag. Keep the numeric version synchronized in:

- `project(... VERSION ...)` in `CMakeLists.txt`
- `kAppVersionLabel` and the update-check User-Agent in `src/main.cpp`
- `APP_VERSION_NUMBER` and `APP_VERSION_STRING` in `src/app.rc`
- the version, build directory, output name, and `VersionInfoVersion` in
  `installer/LowLatencyCaptureViewer.iss`
- default build/output paths in `tools/package-v1.ps1`
- `docs/release-notes-v<version>.md`

Search for stale version strings after editing.

```powershell
rg -n "old-version|vold-version" CMakeLists.txt src installer tools docs
```

## 2. Build and verify

Produce a Release x64 build, pass every registered test, and check the diff.

```powershell
cmake -S . -B build-v<version>-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-v<version>-release
ctest --test-dir build-v<version>-release --output-on-failure
git diff --check
```

When hardware is available, briefly verify Shared/ASIO, the selected capture
format, Tab diagnostics, F2 settings, and a clean exit/restart.

## 3. Package

Build exactly these two release assets:

```text
LowLatencyCaptureViewer_v<version>_Setup.exe
LowLatencyCaptureViewer_v<version>_x64.zip
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-v1.ps1
& "C:\Program Files\Inno Setup 7\ISCC.exe" ".\installer\LowLatencyCaptureViewer.iss"
```

Do not ship `settings.ini`, `%LOCALAPPDATA%` logs, `build-*` directories, PDB
files, or ILK files. Verify the executable inside the ZIP and the installer
version before uploading.

## 4. Publish

Review the worktree and stage only confirmed files before committing.

```powershell
git status --short
git diff --cached --check
git commit -m "Prepare v<version> release"
git push origin agent/release-v1.0.0
```

Publish from the branch HEAD using the matching release-notes file. Add
`--prerelease` only when a beta is intended.

```powershell
gh release create v<version> `
  "..\outputs\v<version>\LowLatencyCaptureViewer_v<version>_Setup.exe" `
  "..\outputs\v<version>\LowLatencyCaptureViewer_v<version>_x64.zip" `
  --repo seria-aa/LowLatencyCaptureViewer `
  --target agent/release-v1.0.0 `
  --title "Low Latency Capture Viewer v<version>" `
  --notes-file ".\docs\release-notes-v<version>.md"
```

Never overwrite published tags or assets. Fix a published-release issue in the
next patch version instead.
