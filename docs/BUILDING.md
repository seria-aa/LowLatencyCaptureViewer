<img align="right" src="images/app-icon.png" width="40" height="40" alt="LLCV">

# Building from source

[README](../README.md) · [Documentation](INDEX.md) · [한국어](BUILDING.ko.md)

[Requirements](#requirements) · [Release build](#release-build) · [Installer and portable ZIP](#installer-and-portable-zip)

<br>

## Requirements

- Windows 10 or 11 x64
- Visual Studio 2022 with **Desktop development with C++**
- Windows SDK (including fxc for shader compilation) and CMake tools for Windows
- Git for Windows (Git Bash), curl and tar to build the FFmpeg mux dependency
- Inno Setup 7 only when building the installer

The project uses Windows system APIs and libraries: Win32, DirectShow, D3D11,
DXGI, Media Foundation and WASAPI. Recording also requires a pinned mux-only FFmpeg
shared-library build. Media Foundation provides encoding; no FFmpeg program or codec pack is required.

<br>

## Release build

Open an x64 Visual Studio developer PowerShell in the repository root:

```powershell
chcp.com 65001 > $null
.\tools\Build-RecordingFFmpeg.ps1
cmake -S . -B build -A x64 -DLLCV_RECORDING_FFMPEG_ROOT="$PWD/outputs/dependencies/recording-ffmpeg/install"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is generated at:

```text
build\Release\LowLatencyCaptureViewer.exe
```

The release configuration uses the static MSVC runtime, so the packaged
executable does not require a separate Visual C++ Redistributable installation.

The 2.1.6 suite adds CMAA2 → NIS SDR quality, HDR bypass, OSD/recording isolation,
settings layout, isolated test profiles and manual-tool build boundaries.
See tests/RELEASE_V2_1_6_AUDIT.md for validation scope.

NIS/CMAA2 shaders are compiled into the executable at build time. Runtime shader compilation
and source files are not required. Sources, modifications and licenses are in third_party/upscaling.
Per-shader dependencies prevent an NIS-only change from rebuilding CMAA2 shaders.

SettingsPreview, RecordingIndicatorPreview and RecordingQualityBenchmark are manual tools
excluded from the default build. Request them explicitly with cmake --build and --target.
Automated regression tests remain part of the default build.

Tests and private previews use a fresh temporary profile per process and do not load or save
actual app settings. The regular application still uses its normal user profile.
Use newly built test tools for manual validation; see tests/PROFILE_ISOLATION.md.

Existing tests cover default-settings shortcut routing, recording cancellation with a live
settings window, recording input-device changes and large audio packets, final queue draining,
invalid numeric settings and bit-identical audio resampling regressions.
The cancellation test uses hidden windows, synthetic media and simulated presentation
acknowledgements; it does not measure physical exclusive-display transitions or latency.
Existing tests cover playback mute/recorded-audio isolation, shortcut migration/reset
and Korean/English high-DPI UI layout.
Existing checks cover isolated encoder-probe cancellation/failure, recording
free-space updates, connected MJPEG color metadata, initial audio-overlap trimming,
settings shortcut routing and minimized-window placement regressions.
The suite also covers MJPEG recording, recording volume, unlisted 30 fps requests,
recording startup/settings/color metadata, splitting, pause/resume, storage delays, crash-tolerant files,
exclusive fullscreen and video placement. Existing checks cover 40 fps negotiation, color
metadata, settings pages, device transitions, save failures, stream completion,
and power-request lifetimes. File-save tests need a temporary
directory that permits writing and renaming files. Record tests skipped because
a GPU or Windows feature is unavailable separately from passes. Power API tests
do not put the PC to sleep and do not replace real sleep/resume validation.

Independent decode, segment and sync checks also require Python 3 and test-only
ffmpeg/ffprobe programs. Put them on PATH or set `LLCV_TEST_FFMPEG` and
`LLCV_TEST_FFPROBE` in CMake. Without them these checks are not registered; do not
report that as complete coverage. These programs are never included in packages.

<br>

A playback-only developer build can use `-DLLCV_RECORDING=OFF` without FFmpeg; it is not the 2.1.6 release package.
The mux runtime and `ffmpeg-licenses` folder must remain beside the executable.

The held audio-input recovery prototype is **off by default**. For private diagnosis only,
use a separate build directory with `-DLLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC=ON`.
This produces `LowLatencyCaptureViewer_AudioRecovery_Diagnostic.exe` with a diagnostic
version label. Regular builds do not compile its watchdog or retries; existing WASAPI/ASIO
output recovery is unchanged. The diagnostic option cannot be combined with HDR/VSR
experiments or used to create a regular installer/portable package.
It is not a confirmed fix for capture-device silence.

## Installer and portable ZIP

After the Release build, run these commands from the repository root. These
examples package v2.1.6 using the `build\Release` output above; the build-directory
override is required because the release scripts normally use a versioned
build directory.

```powershell
chcp.com 65001 > $null
& "C:\Program Files\Inno Setup 7\ISCC.exe" `
  "--define=BuildDir=..\build\Release" ".\installer\LowLatencyCaptureViewer.iss"
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-v1.ps1 `
  -Version 2.1.6 -BuildDir ..\build\Release -OutputDir outputs\v2.1.6
```

The outputs are `outputs\v2.1.6\LowLatencyCaptureViewer_v2.1.6_Setup.exe`
and `outputs\v2.1.6\LowLatencyCaptureViewer_v2.1.6_x64.zip`. When preparing
another version, update the versioned release files first; changing a ZIP name
does not update the executable. See the [release checklist](RELEASING.md).

Both packages must include `LICENSE`, `ASIO-SDK-LICENSE.txt`,
`ASIO-HOST-LICENSE.txt`, `PRETENDARD-LICENSE.txt`, `NIS-LICENSE.txt`,
`CMAA2-LICENSE.txt`, and `UPSCALING-NOTICE.txt`. They must not contain machine-specific `settings.ini`
files, diagnostic logs, test executables, PDB files, or build directories.
Automated tests do not replace real-device or long-duration playback checks;
record separately which hardware checks were actually performed.

Runtime settings and logs are stored per user under:

```text
%LOCALAPPDATA%\LowLatencyCaptureViewer
```

An older `settings.ini` beside the executable is copied to that location on the
first launch and is not deleted automatically.

---

[README](../README.md) · [Documentation](INDEX.md) · [한국어](BUILDING.ko.md)
