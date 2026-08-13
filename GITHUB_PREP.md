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
binary. A target PC needs the capture-card driver and the Microsoft Visual C++
2015-2022 x64 Redistributable.
