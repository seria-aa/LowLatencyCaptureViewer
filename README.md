# Low Latency Capture Viewer

> [한국어](README.ko.md) · [Latest release](https://github.com/seria-aa/LowLatencyCaptureViewer/releases/latest)

A Windows x64 viewer for low-latency HDMI capture. It captures directly from a
DirectShow capture card, renders video with D3D11, and sends audio directly to
WASAPI. It is built and tested around the AVerMedia GC573; other compatible
NV12/YUY2 DirectShow cards, including USB devices with separate audio filters,
are supported experimentally.

No mpv, FFmpeg, third-party codec pack, or companion executable is required.

> **v1.1.0:** Adds selectable 30 fps capture, integer-scaled pixel-perfect
> fullscreen with black borders, and an F5 shortcut that restores exact 1:1
> size. MJPEG remains the only experimental compressed compatibility mode.

## Highlights

- Single DirectShow graph for video and audio capture, with automatic or manual
  selection of separately exposed USB audio filters
- Latest-frame rendering: stale video samples are discarded instead of queued
- D3D11 Video Processor, flip-discard swapchain, and maximum frame latency of 1
- Selectable immediate presentation or VSync, plus smooth or sharp scaling
- WASAPI Shared or Exclusive output, with default-device tracking or a fixed endpoint
- IAudioClient3 shared-mode period negotiation with automatic classic-WASAPI fallback
- Independent PCM safety target and optional clock-drift correction
- Automatic mode detection per selected capture device and resolution; choose from its available pixel formats and frame rates
- Experimental opt-in MJPEG decoding through Windows Media Foundation when a
  device exposes that DirectShow mode
- Pixel-perfect 1:1 mode, aspect-ratio-locked resizing, multi-monitor DPI support, and edge snap
- Tab diagnostics overlay, optional logs, volume HUD, and background auto-mute

## Requirements

- Windows 10 or 11 x64
- A DirectShow capture-card driver
- A supported video mode: progressive NV12 or YUY2. MJPEG is available as an
  experimental compressed compatibility mode when exposed by the device.
- A 48 kHz stereo PCM audio pin on the same capture filter or a separate
  DirectShow audio-input filter
- Clock-drift correction is recommended for long sessions when video and audio
  are sourced from separate device filters
- The x64 release package is self-contained and does not require a separate
  Visual C++ Redistributable installation.

GC573 is the recommended and tested device. Other cards are experimental.
MJPEG is a beta compatibility mode, while H.264/AVC, MPEG-4, P010, and
automatic device reconnect are not supported.

## Install and run

1. Run `LowLatencyCaptureViewer_v1.1.0_Setup.exe` and follow the wizard.
2. Launch **Low Latency Capture Viewer** from the Start menu or the installed
   executable.

A portable `LowLatencyCaptureViewer_v1.1.0_x64.zip` is also available. Extract
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

Enable **Start directly next time** to open the viewer with the saved capture,
audio, and window settings without first showing the settings dialog. Hold
**Shift** while launching to show settings for that launch, or press **F2** in
the viewer to close it safely and reopen the settings dialog. If capture
initialization fails, the viewer returns to settings after showing the error.

## Recommended starting settings

| Setting | Start with |
| --- | --- |
| Presentation | Immediate presentation for the lowest display latency; use VSync to prevent tearing. |
| Audio mode | WASAPI Shared for compatibility; Exclusive uses the endpoint directly when supported. |
| Output device | Follow the Windows default device unless a fixed device is required. |
| Clock drift correction | Off for unaltered PCM. Enable only when long-running playback shows repeated drift-related errors. |
| PCM target | 10 ms for minimum latency; raise it only if underruns repeat. |
| Video format | Auto/NV12 first; match capture FPS to the source's actual output rate. |
| Pixel-perfect | Enable for exact 1:1 display; disable for freely resizable windows. |
| Scaling mode | Smooth by default. Try Sharp when a non-pixel-perfect fullscreen image looks soft. |

### Video mode selection

Changing the capture device or resolution refreshes the supported format and
frame-rate list. A fresh configuration starts at 1920 x 1080 (FHD). The chosen
capture resolution is remembered and is never changed automatically when the
settings or viewer window moves to another monitor. Only modes advertised by
the device are shown, and Start is disabled only when no supported mode exists.

Auto pixel format prefers uncompressed NV12, followed by YUY2. When NV12 or
YUY2 reaches at least 30 fps at the selected resolution, MJPEG is hidden so the
tested raw path remains the clear default. **MJPEG (experimental compressed
compatibility)** appears for explicit selection when raw formats are absent or
all stay below 30 fps. H.264/AVC and MPEG-4 are not supported because
inter-frame references and decoder reordering cannot provide reliably low
latency.

30 fps appears when the device advertises an exact 30/29.97 media type or
includes 30 fps in its supported frame-interval range. Selecting it configures
the capture pin itself at 30 fps rather than receiving a faster stream and
dropping frames. This adds no application queue, but each frame spans 33.3 ms;
when the same source is stable at 60 or 120 fps, those modes provide lower video
latency and smoother motion.

For this viewer, **NV12 is the preferred low-latency format**. Its 12-bit-per-
pixel layout transfers 25% less frame data than 16-bit-per-pixel YUY2 during the
remaining system-memory-to-D3D11 upload, so it generally has lower bandwidth
and processing overhead. Both formats otherwise use the same latest-frame-only
D3D11 Video Processor path. The exact difference still depends on the capture
device and driver; choose YUY2 when its 4:2:2 chroma detail is more important or
when a particular device handles it better.

### Capture audio selection

`Capture audio device` uses an audio pin on the selected video device first.
For USB capture devices that expose audio separately, it then looks for a
matching DirectShow audio-input device by name. If automatic matching is not
enough, select that USB audio input manually. Diagnostic logs record the failed
initialization stage and enumerate the filter pins/media types for diagnosis.

When video and audio arrive from separate device filters, their hardware clocks
can differ slightly and move the PCM queue over long sessions. Enable clock-
drift correction only when audio repeatedly breaks up or the Tab overlay shows
a persistent and growing imbalance.

### Window, fullscreen, and scaling

`Pixel-perfect` fixes the client area to the selected capture resolution and
disables manual resizing. With it off, the window can be resized while keeping
the video aspect ratio. When capture and monitor resolutions match, the viewer
starts in borderless fullscreen regardless of the Pixel-perfect setting.

Pixel-perfect fullscreen uses the largest integer scale that fits and clears
unused space to black. For example, QHD input stays at 1x and is centered on a
4K display, while FHD input scales by exactly 2x to fill it. If the capture is
larger than the monitor, the complete picture is downscaled with its aspect
ratio preserved instead of being cropped.

`F5` restores an exact 1:1 client size on the current monitor without changing
the saved Pixel-perfect option. A matching capture/monitor resolution uses
fullscreen; if a 1:1 window cannot fit the monitor work area, the HUD reports
that it is unavailable. `F11` toggles borderless fullscreen independently of
the current window size.

`Keep relative window size when moving monitors` is independent of Pixel-
perfect. It preserves a similar screen-coverage ratio across displays with
different resolutions or DPI and restores that ratio on the next launch. A
genuinely smaller saved relative window takes priority over automatic
fullscreen, so a QHD window moved from 4K to FHD does not reopen as fullscreen.
Using this option with Pixel-perfect may break 1:1 after a monitor move; press
F5 whenever exact mapping is needed again.

`Scaling mode` affects non-1:1 video only. **Smooth** is the standard filtered
scale. **Sharp** enables the GPU video processor's edge-enhancement filter,
when the display driver supports it, without adding a frame queue. It can make
text and UI clearer when the image is enlarged; it cannot make a non-integer
scale mathematically pixel-perfect. If unsupported, Sharp safely uses Smooth.

`Hide title bar` removes the caption and border in windowed mode. Edge snap
works at both outer monitor edges and shared boundaries between monitors. Its
entry and release distance is 20 DIP after DPI scaling; hold Shift while
dragging to bypass snap temporarily.

Before starting, close other programs that may already be using the capture
device, such as OBS Studio or a vendor utility (for example Elgato 4K Capture
Utility). A device that is already in use can fail during capture initialization
even when its selected mode is supported.

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

With **Follow the Windows default output device**, changing the default endpoint
while the viewer is running reopens only WASAPI output and leaves video capture
running. Pending PCM is cleared after the switch so endpoint initialization time
does not become accumulated playback latency; a short silence or small click can
still occur during the transition.

`WASAPI output buffer` requests the endpoint period/buffer, while `PCM buffer
target` is the application's capture-to-render safety depth. Both are adjusted
independently from clock-drift correction. The 10 ms PCM target is the minimum-
latency choice; raise it to 15, 20, or 30 ms only when underruns repeat.

The mouse wheel changes volume in 5% steps. At 100%, PCM volume processing is
bypassed. Enabling **Allow volume boost above 100%** permits up to 200% digital
gain without adding an audio buffer, but loud signals may clip. Background auto-
mute changes only the output gain while capture and WASAPI consumption continue,
so returning to the viewer does not expose an accumulated PCM delay.

## Controls

| Control | Action |
| --- | --- |
| `F5` | Restore exact 1:1 client size; use matching-monitor fullscreen when available |
| `F11` | Toggle borderless fullscreen |
| `F2` | Close the viewer safely and reopen settings |
| `Tab` | Show or hide diagnostics |
| Mouse wheel over video | Change application volume in 5% steps |
| Drag near an edge or corner | Snap window without resizing it |
| `Shift` + drag | Temporarily bypass edge snap |
| `Esc` | Exit immediately from automatic matching-resolution fullscreen; leave manually entered F11 fullscreen, then exit from windowed mode |

## Diagnostics

The Tab overlay shows input and Present FPS, app processing time, discarded
frames, selected format, WASAPI buffer state, PCM queue depth, drift correction,
and underrun/overrun counts. Video statistics exclude the first two seconds and
audio error diagnostics exclude the first five seconds. Capture and playback
themselves begin immediately.

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
filters, plugins, and encoding. This application is a dedicated replacement for
using the OBS preview only to watch and hear a capture device. It omits scene
composition, recording, streaming, encoding, and FFmpeg/mpv to keep the preview
path focused. Use OBS when those production features are required.

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
capture. Use 120 fps for a 120 fps source, 60 fps for a 60 fps source, and 30 fps
for a 30 fps source. Although 30 fps reduces throughput, its 33.3 ms frame
interval reduces motion smoothness and responsiveness when a 60/120 fps source
is intentionally lowered to it. If the source offers several rates, choose the
one that also fits the target display cadence when possible (for example,
120 fps with 120/240 Hz). For VSync, a 120 Hz or 240 Hz display can provide more
even pacing for 120 fps video than a 144 Hz display.

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
