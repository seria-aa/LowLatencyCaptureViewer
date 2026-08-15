# Troubleshooting

> [한국어](TROUBLESHOOTING.ko.md) · [Back to README](../README.md)

## Capture initialization fails

Close every application that may already hold the capture device, including
OBS Studio, vendor utilities, browsers, video-call software, and virtual-camera
tools. Some drivers report a resource error rather than explicitly saying that
the device is already in use.

Then try these checks in order:

1. Disconnect and reconnect a USB capture device, or restart the PC for a PCIe device.
2. Confirm that the source is outputting a supported progressive resolution and frame rate.
3. Select the capture device manually instead of Auto.
4. Try Auto/NV12, then YUY2, then MJPEG if it is offered.
5. Try a lower supported resolution or frame rate.
6. Enable diagnostic file logging and inspect the final initialization stage.

`0x800705AA` means Windows reported insufficient system resources. With capture
devices this can also be returned when another application or vendor service
already owns the device, or when a driver rejects the requested allocation.

`0x80070490` means an expected element was not found. Common causes are an
unavailable requested mode, a missing compatible capture pin, or no compatible
48 kHz stereo PCM audio path. The stage and pin diagnostics in the log are more
useful than the code by itself.

## Video works but audio does not

Some USB devices expose video and audio as separate DirectShow filters. Choose
the matching USB audio input manually under **Capture audio device**. The audio
input must provide 48 kHz stereo PCM.

If audio breaks up only after a long session and separate filters are selected,
observe the PCM trend in the Tab overlay and try clock-drift correction. For
immediate, repeated underruns, increase the PCM target one step instead.

## The picture is soft

Pixel-perfect gives exact 1:1 mapping when the window can use the capture
resolution directly. In a scaled window or fullscreen mode, try **Sharp**. It
uses supported D3D11 Video Processor edge enhancement without adding a frame
queue and falls back to Smooth when unavailable.

Also confirm that the source and capture resolution match. Capturing a 1440p
source as 4K does not create additional detail.

## Frame drops or stutter

Confirm that Input FPS matches the selected source rate and compare it with
Present FPS in the Tab overlay. Try NV12 before YUY2 or MJPEG, reduce resolution
or FPS if the device or USB link cannot sustain the mode, and test on the target
monitor.

Mixed monitor refresh rates and DWM composition can change windowed frame
pacing. Compare Low latency and VSync on the monitor where the viewer will stay.

## Settings window is skipped

If **Start directly next time** is enabled, hold Shift while launching. You can
also press `F2` from the viewer to close it safely and return to settings.

## Collecting a useful report

Enable **Save diagnostic log file**, reproduce the failure once, and attach the
newest file from:

```text
%LOCALAPPDATA%\LowLatencyCaptureViewer\logs
```

Include the capture device, source resolution/FPS, selected format, capture
audio device, output endpoint, Windows version, and whether another capture
application had been running.
