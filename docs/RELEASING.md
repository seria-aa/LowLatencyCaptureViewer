# Release guide

> [한국어](RELEASING.ko.md) · [Back to README](../README.md)

Use this checklist to keep versioning, builds, packages, and GitHub releases
consistent. Do not create a separate release branch; use the existing
`agent/release-v1.0.0` branch.

## 1. Choose the version

Use the same version in the Git tag and file names, with a `v` prefix only on
the tag:

```text
Example: 1.1.7.1  →  tag v1.1.7.1
```

Keep these locations in sync:

- `project(... VERSION ...)` in `CMakeLists.txt`
- `kAppVersionLabel` and the update-check User-Agent in `src/main.cpp`
- app version, build directory, output name, and `VersionInfoVersion` in
  `installer/LowLatencyCaptureViewer.iss`
- version-specific default build/output paths and ZIP name in
  `tools/package-v1.ps1`
- `docs/release-notes-v<version>.md`

After editing, search for stale version strings:

```powershell
rg -n "1\.1\.7|v1\.1\.7" CMakeLists.txt src installer tools docs
```

## 2. Build and test

Use the existing release branch and only fast-forward it:

```powershell
git switch agent/release-v1.0.0
git pull --ff-only origin agent/release-v1.0.0
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
git diff --check
```

The Release executable, `AudioModuleTests`, and `LoggerTests` must be produced
and all tests must pass. If hardware is available, briefly verify Shared/ASIO
audio, the selected capture format, the Tab diagnostics overlay, F2 settings,
and clean exit/restart.

## 3. Build the packages

Update the version-specific defaults in `tools/package-v1.ps1`, then run it.
The script puts the README files, LICENSE, dependency notes, docs, and the
executable into the portable ZIP.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-v1.ps1
```

Build the installer with Inno Setup 7 (or 6 or newer):

```powershell
& "C:\Program Files\Inno Setup 7\ISCC.exe" ".\installer\LowLatencyCaptureViewer.iss"
```

Upload exactly these two assets:

```text
LowLatencyCaptureViewer_v<version>_Setup.exe
LowLatencyCaptureViewer_v<version>_x64.zip
```

Do not include the developer machine's `settings.ini`, `%LOCALAPPDATA%` logs,
`build-*` directories, PDB files, or ILK files. Check the installer file
version and the executable inside the ZIP before uploading.

## 4. Commit and publish with GitHub CLI

Review the worktree and staged content before committing:

```powershell
git status --short
git diff --cached --check
git commit -m "Prepare v<version> release"
git push origin agent/release-v1.0.0
```

Create the release from the existing branch HEAD and use the release-notes
file as the body:

```powershell
gh release create v<version> `
  "..\outputs\v<version>\LowLatencyCaptureViewer_v<version>_Setup.exe" `
  "..\outputs\v<version>\LowLatencyCaptureViewer_v<version>_x64.zip" `
  --repo seria-aa/LowLatencyCaptureViewer `
  --target agent/release-v1.0.0 `
  --title "Low Latency Capture Viewer v<version>" `
  --notes-file ".\docs\release-notes-v<version>.md"
```

Releases are stable by default. Add `--prerelease` only when a beta or RC is
explicitly intended. Verify the uploaded assets and tag target:

```powershell
gh release view v<version> --repo seria-aa/LowLatencyCaptureViewer `
  --json tagName,targetCommitish,isDraft,isPrerelease,assets,url
```

## 5. Release-note rules

Write Korean and English sections in the same file, in this order:

1. User-visible features and UI changes
2. Capture/audio format, device, or compatibility changes
3. Bug fixes and diagnostic-log changes
4. Whether performance or latency is affected
5. Known limitations and test scope

For observability-only changes such as event logging, state explicitly that
events are not logged per frame and that normal video/audio processing latency
is unchanged.

## 6. After publication

Do not overwrite public assets or force-move a published tag. If a problem is
found after publication, fix it in the next patch version and document the
impact and workaround. Replace assets only while a release is still a draft.
