# GitHub publication checklist

This repository is prepared as **Low Latency Capture Viewer**. The tested
capture device is still identified as AVerMedia GC573 in the technical notes;
that is device compatibility information, not the application name.

Before making the repository public:

1. This repository is licensed under the GNU General Public License v3.0 or
   any later version. Keep the root `LICENSE` file with every source release.
2. Review the author name, repository URL, and release notes.
3. Build from a clean x64 Developer PowerShell using the commands in README.md.
4. Do not commit `build-*`, `logs/`, local binaries, or machine-specific
   `settings.ini` changes.

The source tree does not require mpv.exe, FFmpeg, or any third-party runtime
binary. Release builds use the static MSVC runtime. A target PC still needs
the capture-card driver and the Windows multimedia components listed in
`DEPENDENCIES.txt`.

The v1.0.6.1 Beta distribution has two forms: an Inno Setup installer for Program
Files and a portable x64 ZIP. Both keep settings and optional logs under
`%LOCALAPPDATA%\\LowLatencyCaptureViewer`; they do not ship a machine-specific
`settings.ini`.

The application is a single executable with Auto, Korean, and English UI
selection. It does not install duplicate language executables.
