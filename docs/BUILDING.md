# Building from source

> [한국어](BUILDING.ko.md) · [Back to README](../README.md)

## Requirements

- Windows 10 or 11 x64
- Visual Studio 2022 with **Desktop development with C++**
- Windows SDK and CMake tools for Windows
- Inno Setup 7 only when building the installer

The project uses Windows system APIs and libraries: Win32, DirectShow, D3D11,
DXGI, Media Foundation for experimental MJPEG decoding, and WASAPI. It does not
require FFmpeg or a third-party codec pack.

## Release build

Open an x64 Visual Studio developer PowerShell in the repository root:

```powershell
chcp.com 65001 > $null
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is generated at:

```text
build\Release\LowLatencyCaptureViewer.exe
```

The release configuration uses the static MSVC runtime, so the packaged
executable does not require a separate Visual C++ Redistributable installation.

## Installer and portable ZIP

After the Release build, run these commands from the repository root. These
examples package v1.2.12 using the `build\Release` output above; the build-directory
override is required because the release scripts normally use a versioned
build directory.

```powershell
chcp.com 65001 > $null
& "C:\Program Files\Inno Setup 7\ISCC.exe" `
  "--define=BuildDir=..\build\Release" ".\installer\LowLatencyCaptureViewer.iss"
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-v1.ps1 `
  -Version 1.2.12 -BuildDir ..\build\Release -OutputDir ..\outputs\v1.2.12
```

The outputs are `..\outputs\v1.2.12\LowLatencyCaptureViewer_v1.2.12_Setup.exe`
and `..\outputs\v1.2.12\LowLatencyCaptureViewer_v1.2.12_x64.zip`. When preparing
another version, update the versioned release files first; changing a ZIP name
does not update the executable. See the [release checklist](RELEASING.md).

Both packages must include `LICENSE`, `ASIO-SDK-LICENSE.txt`, and
`ASIO-HOST-LICENSE.txt`. They must not contain machine-specific `settings.ini`
files, diagnostic logs, test executables, PDB files, or build directories.
Automated tests do not replace real-device or long-duration playback checks;
record separately which hardware checks were actually performed.

Runtime settings and logs are stored per user under:

```text
%LOCALAPPDATA%\LowLatencyCaptureViewer
```

An older `settings.ini` beside the executable is copied to that location on the
first launch and is not deleted automatically.
