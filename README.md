# Low Latency Capture Viewer

> [한국어](README.ko.md) · [Latest release](https://github.com/seria-aa/LowLatencyCaptureViewer/releases/latest)

A Windows x64 viewer for low-latency HDMI capture. It captures directly from a
DirectShow capture card, renders video with D3D11, and sends audio directly to
WASAPI. It is built and tested around the AVerMedia GC573; other compatible
NV12/YUY2 DirectShow cards, including USB devices with separate audio filters,
are supported experimentally.

No mpv, FFmpeg, decoder, or companion executable is required.

## Highlights

- Single DirectShow graph for video and audio capture, with automatic or manual
  selection of separately exposed USB audio filters
- Latest-frame rendering: stale video samples are discarded instead of queued
- D3D11 Video Processor, flip-discard swapchain, and maximum frame latency of 1
- Selectable immediate presentation or VSync
- WASAPI Shared or Exclusive output, with default-device tracking or a fixed endpoint
- IAudioClient3 shared-mode period negotiation with automatic classic-WASAPI fallback
- Independent PCM safety target and optional clock-drift correction
- Automatic mode detection per selected capture device and resolution; choose from its available pixel formats and frame rates
- Pixel-perfect 1:1 mode, aspect-ratio-locked resizing, multi-monitor DPI support, and edge snap
- Tab diagnostics overlay, optional logs, volume HUD, and background auto-mute

## Requirements

- Windows 10 or 11 x64
- A DirectShow capture-card driver
- A supported video mode: progressive, uncompressed NV12 or YUY2
- A 48 kHz stereo PCM audio pin on the same capture filter or a separate
  DirectShow audio-input filter
- Clock-drift correction is recommended for long sessions when video and audio
  are sourced from separate device filters
- The x64 release package is self-contained and does not require a separate
  Visual C++ Redistributable installation.

GC573 is the recommended and tested device. Other cards are experimental;
compressed formats, P010, and automatic device reconnect are not supported.

## Install and run

1. Run `LowLatencyCaptureViewer_v1.0.3_Setup.exe` and follow the wizard.
2. Launch **Low Latency Capture Viewer** from the Start menu or the installed
   executable.

A portable `LowLatencyCaptureViewer_v1.0.3_x64.zip` is also available. Extract
it anywhere and run `LowLatencyCaptureViewer.exe`.

Settings and diagnostic logs are stored per user in
`%LOCALAPPDATA%\\LowLatencyCaptureViewer`. An older `settings.ini` beside the
executable is copied there automatically on first launch; it is not deleted.
The uninstaller keeps this data by default and asks whether to remove it. Choose
**No** to preserve preferences and diagnostics for a future reinstall, or
**Yes** to delete them permanently.

The application contains Korean and English UI strings in one executable. The
settings dialog offers **Auto (Windows language)**, **한국어**, and **English**;
the selected language is saved per user and does not install a duplicate
language executable.

## Recommended starting settings

| Setting | Start with |
| --- | --- |
| Presentation | Immediate presentation for the lowest display latency; use VSync to prevent tearing. |
| Audio mode | WASAPI Shared for compatibility; Exclusive uses the endpoint directly when supported. |
| Output device | Follow the Windows default device unless a fixed device is required. |
| Clock drift correction | Off for unaltered PCM. Enable only when long-running playback shows repeated drift-related errors. |
| PCM target | 10 ms for minimum latency; raise it only if underruns repeat. |
| Video format | Auto/NV12 first for the lowest upload overhead; select 60 fps when 120 fps is not sustainable. |

Changing the capture device or resolution refreshes the supported format and
frame-rate list. While the settings dialog is moved to another monitor, its
recommended capture resolution follows that monitor (1080p on FHD, QHD on
larger displays) and refreshes the capability list once at the monitor
boundary. This happens only in the settings dialog and does not add work to
the active capture or render path. The settings window shows the detected
combinations, and disables Start when the selected device has no supported
uncompressed mode.

`Capture audio device` uses an audio pin on the selected video device first.
For USB capture devices that expose audio separately, it then looks for a
matching DirectShow audio-input device by name. If automatic matching is not
enough, select that USB audio input manually. Diagnostic logs record the failed
initialization stage and enumerate the filter pins/media types for diagnosis.

For this viewer, **NV12 is the preferred low-latency format**. Its 12-bit-per-
pixel layout transfers 25% less frame data than 16-bit-per-pixel YUY2 during the
remaining system-memory-to-D3D11 upload, so it generally has lower bandwidth and
processing overhead. Both formats otherwise use the same latest-frame-only
D3D11 Video Processor path. The exact latency difference still depends on the
capture device and driver; choose YUY2 when its 4:2:2 chroma detail is more
important or when a particular device handles it better.

`Pixel-perfect` fixes the client area to the selected capture resolution and
disables manual resizing. With it off, the window can be resized while keeping
the video aspect ratio. Monitor-relative sizing is independent: when enabled,
the saved monitor-relative scale is also applied on the next launch, so a QHD
window moved to an FHD display does not reopen at the full QHD size there.

## Low-latency audio

When **WASAPI Shared** is selected, the settings window checks whether the
chosen endpoint supports `IAudioClient3`. When available, the viewer reads the
endpoint's supported shared engine periods and starts an event-driven shared
stream with the closest valid low-latency period using
`InitializeSharedAudioStream`. It never assumes that a requested 5 or 10 ms
period is actually available: the supported range is shown in the settings
window and the active period is reported in the Tab overlay.

If `IAudioClient3` is unavailable for the endpoint or PCM format, the viewer
automatically falls back to classic WASAPI Shared mode. Exclusive mode remains
a separate direct-endpoint option; it is not an `IAudioClient3` requirement.

## Controls

| Control | Action |
| --- | --- |
| `F11` | Toggle borderless fullscreen |
| `Tab` | Show or hide diagnostics |
| Mouse wheel over video | Change application volume in 5% steps |
| Drag near an edge or corner | Snap window without resizing it |
| `Shift` + drag | Temporarily bypass edge snap |
| `Esc` | Leave fullscreen; press again in windowed mode to exit |

## Diagnostics

The Tab overlay shows input and Present FPS, app processing time, discarded
frames, selected format, WASAPI buffer state, PCM queue depth, drift correction,
and underrun/overrun counts. Statistics begin after a two-second warm-up; capture
and playback begin immediately.

![Tab diagnostics overlay](docs/images/tab-diagnostics.png)

*Example Tab diagnostics overlay. Values vary by device, display, and driver.*

The reported ppm is an operational estimate, not a direct hardware-clock
measurement. It includes scheduling stalls and startup behavior. Observe it for
10–30 minutes before enabling resampling solely for drift correction.

Terms used by the overlay:

- **Output buffer**: the WASAPI device buffer.
- **Output occupancy**: audio currently queued in that device buffer.
- **PCM buffer**: the application's capture-to-render safety queue.

An occasional underrun is not necessarily audible or harmful. If it repeats,
raise the PCM target before enabling resampling when preserving the original PCM
samples is more important than long-run clock correction.

## Practical compatibility notes

### Viewer vs. OBS

OBS Studio is a general-purpose production tool for scenes, recording, streaming,
filters, plugins, and encoding. This viewer is a dedicated preview path: it does
not compose scenes, encode, record, or require FFmpeg/mpv. That smaller path is
useful when preview latency and predictable behavior matter, but it is not an
OBS benchmark or replacement. Use OBS when production features are required.

### WASAPI Exclusive and system DSP

Use **WASAPI Shared** when Windows-wide effects or third-party APO/DSP software
such as Equalizer APO or HeSuVi must remain active. **WASAPI Exclusive** opens the
endpoint directly and can reduce the shared-mixer path, but it typically bypasses
shared-mode APO/DSP effects and prevents other applications from using that
endpoint at the same time. Choose based on whether compatibility or minimum
audio-path latency is the priority.

### Mixed monitor refresh rates

In a windowed desktop with displays running at different refresh rates, Windows
DWM composition can produce different frame pacing or occasional micro-stutter
when the window crosses monitors. Immediate presentation avoids an additional
VSync wait and is the low-latency choice, at the cost of possible tearing; it
does not remove all pacing differences caused by DWM. VSync reduces tearing but
adds a wait and its pacing depends on the current display. When choosing a
capture rate, first stay within the source's supported rates, then prefer a
rate that has an even cadence with the target monitor when one is available.
Compare the Input FPS and Present FPS in the Tab overlay after moving the
window.

### Capture frame rate

Choose a capture rate at or below the source's actual maximum rate. This source
limit takes priority over the monitor refresh rate: a 120 fps source has no
additional visual information at 144 fps, and a capture card may only repeat or
re-time frames in that mode. A 144 Hz display therefore does not require 144 fps
capture. Use 120 fps for a 120 fps source and 60 fps for a 60 fps source. If the
source offers several rates, choose the one that also fits the target display
cadence when possible (for example, 120 fps with 120/240 Hz). For VSync, a
120 Hz or 240 Hz display can provide more even pacing for 120 fps video than a
144 Hz display.

### Overhead and input-latency expectations

The viewer avoids an encoder, scene graph, and queued video pipeline; it keeps
only the newest frame and performs the required D3D11 upload and presentation.
Actual CPU/GPU usage still depends on resolution, FPS, driver, monitor, and
desktop composition, so fixed hardware-independent percentages are not promised.
Likewise, the OSD app-processing value is not end-to-end HDMI or display latency.
Rhythm games and other timing-sensitive use cases should be validated on the
capture card and monitor that will actually be used.

## Build from source

Use a Visual Studio 2022 x64 developer shell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable is `build\Release\LowLatencyCaptureViewer.exe`.

The installer script is `installer\LowLatencyCaptureViewer.iss` and is built
with Inno Setup 6 or newer. Neither the installer nor the portable package includes
machine-specific settings.

## License

Copyright (C) 2026 seria-aa. This project is licensed under the
[GNU General Public License v3.0 or later](LICENSE). Windows components and
capture-card drivers are separate dependencies and remain under their own
licenses.
