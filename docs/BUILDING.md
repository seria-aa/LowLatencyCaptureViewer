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
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable is generated at:

```text
build\Release\LowLatencyCaptureViewer.exe
```

The release configuration uses the static MSVC runtime, so the packaged
executable does not require a separate Visual C++ Redistributable installation.

## Installer

The Inno Setup script is:

```text
installer\LowLatencyCaptureViewer.iss
```

Build it with Inno Setup 7 after producing the Release executable.
Neither the installer nor the portable archive should contain machine-specific
`settings.ini` files or diagnostic logs.

Runtime settings and logs are stored per user under:

```text
%LOCALAPPDATA%\LowLatencyCaptureViewer
```

An older `settings.ini` beside the executable is copied to that location on the
first launch and is not deleted automatically.
