# Low Latency Capture Viewer

> [한국어](README.ko.md) · [Download the latest release](https://github.com/seria-aa/LowLatencyCaptureViewer/releases/latest)

A low-latency Windows viewer designed to carry video and audio from an HDMI
capture device through the shortest practical paths. It avoids viewer-irrelevant
processing such as scene composition and encoding, allowing it to remain light
and responsive.

Video is captured through DirectShow and presented with D3D11. Audio is sent
directly to WASAPI. An experimental ASIO output is also available when an ASIO
driver is installed. No FFmpeg, third-party codec pack, or separate Visual C++
Redistributable installation is required.

## Download

Open the [latest release](https://github.com/seria-aa/LowLatencyCaptureViewer/releases/latest):

- **Setup.exe** — recommended for most users; installs Start menu shortcuts and
  an uninstaller.
- **x64.zip** — portable version; extract it and run
  `LowLatencyCaptureViewer.exe`.

Windows 10 or 11 x64 and a capture-device driver are required.

## Quick start

1. Close OBS Studio, Elgato 4K Capture Utility, and any other application that
   may already be using the capture device.
2. Start Low Latency Capture Viewer.
3. Confirm the capture device, capture audio device, resolution, format, frame
   rate, and audio output.
4. Select **Start**.

For the first test, use the recommended values shown in the settings window.
If a USB device exposes video and audio separately, choose its matching audio
input under **Capture audio device**.

To listen without displaying video, enable **Audio-only mode**. The viewer then
skips the video pin, D3D11 renderer, and video presentation path and runs only
capture audio plus the selected audio output. The window shows the same L/R peak, dBFS,
channel/master volume, and clipping OSD as the video viewer. `F2` opens
settings, `F3` shows or hides the audio meter OSD, and `Esc` exits.

## Highlights

- Latest-frame-only video path that discards stale frames instead of queueing them
- D3D11 Video Processor, flip-discard presentation, and maximum frame latency of 1
- Immediate low-latency presentation or VSync
- WASAPI Shared output with `IAudioClient3` low-period support
- Experimental ASIO output shown only when an installed ASIO driver is detected
- Automatic device mode detection for resolution, NV12/YUY2/MJPEG/P010, and FPS
- Experimental P010 10-bit HDR10 path with metadata checks and SDR fallback
- Pixel-perfect 1:1 display, aspect-ratio resizing, borderless fullscreen, and F5 restore
- Multi-monitor DPI-aware window sizing and edge snap
- Volume control, background auto-mute, logs, and a Tab diagnostics overlay
- **Audio-only mode** that opens only capture audio and the selected output

## Recommended starting settings

| Setting | Recommended value |
| --- | --- |
| Presentation | **Low latency**; use VSync when preventing tearing is more important |
| Audio mode | **WASAPI Shared** for compatibility |
| Output device | **Follow the Windows default output device** |
| Capture format | **Auto / NV12 preferred** |
| Capture FPS | Match the source's actual output rate |
| PCM buffer target | **10 ms**; increase only if underruns repeat |
| Clock-drift correction | **Off** by default; choose **Auto** when long-session drift is observed, or On to always resample |
| Pixel-perfect | On for exact 1:1 output; off for a freely resizable window |

WASAPI Exclusive is temporarily hidden from the settings UI while that backend is
being investigated. Existing profiles saved with Exclusive are migrated to WASAPI
Shared on the next run.

ASIO appears in the settings only when an ASIO driver is detected. ASIO drivers
are not bundled; the mode uses the buffer size chosen by the driver. The current
prototype requires a 48 kHz driver. ASIO still supports the app's Off/Auto/On
clock-drift correction choices: the driver owns the output clock, while the
optional app resampler adjusts the capture PCM rate without adding a separate
queue. Use WASAPI Shared when Windows audio effects are needed. If ASIO
initialization fails, that run falls back to WASAPI Shared.

### PCM buffer diagnosis summary

**Current PCM queue** is an instantaneous value, while **Observed minimum** is the
session low recorded at a render-callback boundary, so the two numbers can differ.
Keep the setting when underruns are zero and the diagnosis is normal. Only when
`buffer shortage` or `resampler output shortage` repeats, raise the PCM target in
the order `10 → 15 → 20 ms`; repeated `input late` points instead to capture
callback or system-scheduling delays. Prefer the recent pattern and maximum
consecutive count over one isolated event. ASIO's app PCM queue and driver output
buffer are separate, so check the ASIO driver's buffer settings too when needed.

## Compatibility

AVerMedia GC573 is the primary tested device. Other DirectShow capture devices
are supported experimentally.

- **Video:** progressive NV12 or YUY2; MJPEG is experimental compatibility, and P010 HDR10 is a separate experimental path.
- **Audio:** 48 kHz stereo PCM from the video device or a separate DirectShow audio input.
- **Experimental:** P010 HDR10. It must be selected explicitly; HDR output is enabled only when trustworthy color-space metadata is available.
- **Not supported:** H.264/AVC, MPEG-4, and automatic device reconnect.

The P010 path is enabled only by selecting `P010 10-bit HDR10 (experimental)` in the
pixel-format list. Auto selection still prefers NV12/YUY2. During startup the viewer
checks the active DirectShow type, the matching stream-capability entry, and the
negotiated sample type for BT.2020/PQ metadata. If the capture driver does not expose
that metadata, it falls back to BT.709 SDR output to avoid forced HDR saturation.
The HDR path is a prototype and has not been validated across all HDR sources and displays.
If a confirmed HDR source still reports no DirectShow color metadata, the advanced
settings include an explicit `Force P010 HDR10` option. It is off by default: enabling it
treats P010 as BT.2020/PQ, so SDR input can appear strongly oversaturated.

MJPEG is shown only when the selected resolution has no raw NV12/YUY2 mode at
30 fps or higher. Close any vendor capture utility before starting the viewer;
many devices cannot be opened by two applications at once.

## Controls

| Control | Action |
| --- | --- |
| `F2` | Close the viewer safely and reopen settings |
| `F3` | Show or hide the audio meter OSD (L/R levels, dBFS, and volume) |
| `F5` | Restore exact 1:1 size; use fullscreen when capture and monitor resolutions match |
| `F11` | Toggle borderless fullscreen |
| `Tab` | Show or hide diagnostics |
| `Esc` | Exit automatic fullscreen; manually entered F11 fullscreen is left first |
| Mouse wheel | Change application volume in 5% steps |
| `Shift` + drag | Temporarily bypass edge snap |

For independent channel volume, press `F3` to show the audio meter OSD, then
place the pointer over the **L** or **R** card and use the mouse wheel. Each
step changes only that channel by 5%. Scrolling over the master row or outside
the channel cards changes the master volume. L/R channel gain is limited to
100%; the optional 200% boost applies to the master volume only.

Enable **Start directly next time** to skip the settings window on later runs.
Hold **Shift** while launching, or press **F2** in the viewer, to open settings
again.

**Check for updates automatically** is off by default. When enabled, the viewer
checks the latest GitHub release in a background thread after startup and asks
before opening the official installer download when a newer version exists. It
never installs or launches an update silently, and the check is separate from
video/audio initialization.

## Detailed documentation

- [Video modes, scaling, and fullscreen](docs/VIDEO.md)
- [Low-latency audio](docs/AUDIO.md)
- [Diagnostics and logs](docs/DIAGNOSTICS.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Building from source](docs/BUILDING.md)

Settings and optional logs are stored in
`%LOCALAPPDATA%\LowLatencyCaptureViewer`. The uninstaller asks whether this
user data should also be removed.

## License

Copyright (C) 2026 seria-aa. Licensed under the
[GNU General Public License v3.0 or later](LICENSE).
