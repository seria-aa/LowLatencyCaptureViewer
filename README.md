# Low Latency Capture Viewer

> [한국어](README.ko.md) · [Download the latest release](https://github.com/seria-aa/LowLatencyCaptureViewer/releases/latest)

A lightweight Windows viewer for showing HDMI capture-device video and audio
with low latency. It receives video through DirectShow, presents it directly
with D3D11, and sends capture audio to the chosen output device. Stale video
frames are discarded in favor of the latest frame, making it well suited to a
capture-device window used alongside other work or viewed directly.

No FFmpeg, codec pack, or separate Visual C++ Redistributable is required.

## Download

Get one of the files from the [latest release](https://github.com/seria-aa/LowLatencyCaptureViewer/releases/latest):

- **Setup.exe — recommended:** installs shortcuts and an uninstaller.
- **x64.zip — portable:** extract it, then run `LowLatencyCaptureViewer.exe`.

Windows 10/11 x64 and a capture-device driver are required.

## First use

1. Close OBS, the vendor capture utility, and any other application using the
   capture device.
2. Start the app.
3. Confirm **Capture device**, then select **Start**.

The defaults are a good first test: 1080p, low-latency presentation, WASAPI
Shared, a 20 ms PCM buffer, and automatic clock-drift correction.

If video works but audio does not, check **Capture audio device**. USB capture
devices can expose separate video and audio devices; in that case, choose the
matching audio input manually. If the app shows “Internal audio detected · use
automatically,” no separate selection is needed.

## What should I choose?

### Video

| Setting | Good starting choice |
| --- | --- |
| Capture resolution | **1920 × 1080**; change it to match the source and capture device |
| Pixel format | **Auto (NV12 preferred)** |
| Frame rate | **Auto**, or the source's actual output rate |
| Presentation | **Low latency**; choose VSync when avoiding tearing matters more |
| Pixel-perfect | On for exact 1:1 output; off for a freely resizable window |

A 120 fps capture mode does not create extra visual information when the game
does not output a new frame at that rate. Choose only frame rates that the
source and capture device actually support.

### Audio

| Setting | Good starting choice |
| --- | --- |
| Audio output mode | **WASAPI Shared** |
| Output device | **Follow the Windows default output device** |
| Output buffer | The value marked as recommended in settings |
| PCM buffer target | **20 ms** |
| Clock-drift correction | **Auto** |

WASAPI Shared is the default mode for compatibility with other applications
and Windows effects. ASIO is experimental and appears only when an ASIO driver
is installed. WASAPI Exclusive is available only on output devices that pass
the app's playback-event check. Use WASAPI Shared unless you have a specific
reason to choose another mode.

If sound occasionally breaks up, open the Tab diagnostics overlay. Raise the
PCM target from `20` to `25` to `30 ms` only when **buffer shortage** or **resampler
output shortage** repeats. Leave it alone when there are no errors.

## Everyday controls

| Control | Action |
| --- | --- |
| `F2` | Reopen settings |
| `Tab` | Show/hide live diagnostics |
| `F3` | Show/hide the audio meter |
| `F5` | Restore pixel-perfect size |
| `F11` | Toggle borderless fullscreen |
| `Esc` | Exit automatic fullscreen, or leave F11 fullscreen |
| Mouse wheel over viewer | Change app volume in 5% steps |
| Mouse wheel over the L/R card after `F3` | Change that channel only |
| `Shift` + drag | Temporarily bypass edge snap |

Pixel-perfect maps one video pixel to one display pixel for a sharper image,
but fixes the window size. With it off, the window can be resized freely while
keeping the aspect ratio; choose Smooth or Sharp scaling in settings.

In fullscreen, the cursor hides after two seconds of inactivity and reappears
when you move the mouse or scroll. Choose **Always show** in the Video & window
tab if you prefer a visible cursor.

Enable **Start directly next time** to skip the settings window. Hold `Shift`
while launching, or press `F2` from the viewer, to open it again.

## Troubleshooting

| Problem | Try this first |
| --- | --- |
| No video | Close OBS/vendor tools, then set capture device, resolution, and format back to Auto |
| No audio | Select the audio input that belongs to the chosen video device |
| Occasional audio breakup | Check for buffer shortage in Tab diagnostics, then raise the PCM target in 5 ms steps and test again |
| Need more evidence | Enable logging in Help & diagnostics, reproduce the issue, then send the newest `.log` file from **Open log folder** together with screenshots of settings and Tab diagnostics |

Settings and optional logs are stored in `%LOCALAPPDATA%\LowLatencyCaptureViewer`.
The uninstaller can remove this user data on request.

## Compatibility and experimental features

AVerMedia GC573 is the primary development and test device. Other DirectShow
capture devices are supported, but driver differences mean that every model
cannot be guaranteed.

- Standard support: 48 kHz stereo PCM audio and progressive NV12/YUY2 video
- Compatibility mode: MJPEG, shown only when raw NV12/YUY2 at 30 fps or above
  is unavailable at the selected resolution
- Experimental: ASIO output and P010 10-bit HDR10
- Not supported: H.264/AVC, MPEG-4, automatic device reconnect

Use P010 HDR10 only when the device supplies trustworthy BT.2020/PQ metadata.
For normal SDR use, leave pixel format on Auto.

## Learn more

- [Video formats, scaling, and fullscreen](docs/VIDEO.md)
- [Audio modes, buffers, and clock correction](docs/AUDIO.md)
- [Reading diagnostics and logs](docs/DIAGNOSTICS.md)
- [Detailed troubleshooting](docs/TROUBLESHOOTING.md)
- [Build from source](docs/BUILDING.md)

## License

Copyright (C) 2026 seria-aa. Licensed under the
[GNU General Public License v3.0 or later](LICENSE).
