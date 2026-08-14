# Low Latency Capture Viewer

> [한국어](README.ko.md) · [Latest release](https://github.com/seria-aa/LowLatencyCaptureViewer/releases/latest)

A Windows x64 viewer for low-latency HDMI capture. It captures directly from a
DirectShow capture card, renders video with D3D11, and sends audio directly to
WASAPI. It is built and tested around the AVerMedia GC573; compatible NV12/YUY2
DirectShow cards are supported experimentally.

No mpv, FFmpeg, decoder, or companion executable is required.

## Highlights

- Single DirectShow graph for video and audio capture
- Latest-frame rendering: stale video samples are discarded instead of queued
- D3D11 Video Processor, flip-discard swapchain, and maximum frame latency of 1
- Selectable immediate presentation or VSync
- WASAPI Shared or Exclusive output, with default-device tracking or a fixed endpoint
- Independent PCM safety target and optional clock-drift correction
- Automatic mode detection per selected capture device and resolution; choose from its available pixel formats and frame rates
- Pixel-perfect 1:1 mode, aspect-ratio-locked resizing, multi-monitor DPI support, and edge snap
- Tab diagnostics overlay, optional logs, volume HUD, and background auto-mute

## Requirements

- Windows 10 or 11 x64
- A DirectShow capture-card driver
- A supported video mode: progressive, uncompressed NV12 or YUY2
- A 48 kHz stereo PCM audio pin on the same capture filter
- Microsoft Visual C++ 2015–2022 Redistributable (x64)

GC573 is the recommended and tested device. Other cards are experimental;
compressed formats, P010, separate USB-audio filters, and automatic device
reconnect are not supported.

## Install and run

1. Download `LowLatencyCaptureViewer_v*_x64.zip` from [Releases](https://github.com/seria-aa/LowLatencyCaptureViewer/releases).
2. Extract it to a writable folder.
3. Run `LowLatencyCaptureViewer.exe` and choose the capture and audio settings.

Settings are saved beside the executable in `settings.ini`. Diagnostic logs,
when enabled, are written to the `logs` folder.

## Recommended starting settings

| Setting | Start with |
| --- | --- |
| Presentation | Immediate presentation for the lowest display latency; use VSync to prevent tearing. |
| Audio mode | WASAPI Shared for compatibility; Exclusive uses the endpoint directly when supported. |
| Output device | Follow the Windows default device unless a fixed device is required. |
| Clock drift correction | Off for unaltered PCM. Enable only when long-running playback shows repeated drift-related errors. |
| PCM target | 10 ms for minimum latency; raise it only if underruns repeat. |
| Video format | Auto/NV12 first; select 60 fps when 120 fps is not sustainable. |

Changing the capture device or resolution refreshes the supported format and
frame-rate list. The settings window shows the detected combinations, and
disables Start when the selected device has no supported uncompressed mode.

`Pixel-perfect` fixes the client area to the selected capture resolution and
disables manual resizing. With it off, the window can be resized while keeping
the video aspect ratio. Monitor-relative sizing is independent and can change
the 1:1 size after moving to a differently sized display.

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
when the window crosses monitors. Immediate presentation avoids waiting for a
VSync interval and is the low-latency choice, at the cost of possible tearing.
VSync reduces tearing but adds a wait and its pacing depends on the current
display. Match capture FPS to the target monitor when possible and compare the
Input FPS and Present FPS in the Tab overlay.

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

## License

Copyright (C) 2026 seria-aa. This project is licensed under the
[GNU General Public License v3.0 or later](LICENSE). Windows components,
the Visual C++ Redistributable, and capture-card drivers are separate
dependencies and remain under their own licenses.
