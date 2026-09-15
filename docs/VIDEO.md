# Video modes, scaling, and fullscreen

> [한국어](VIDEO.ko.md) · [Back to README](../README.md)

## Capture path

The viewer captures through DirectShow, keeps only the newest video sample, and
presents it through the D3D11 Video Processor and, by default, a flip-discard swapchain with
maximum frame latency set to 1. Stale samples are discarded instead of being
allowed to form a playback queue.

The app-processing value in the Tab overlay covers this application path. It is
not the total HDMI-to-display latency; the capture device, source, desktop
composition, monitor, and input device add latency outside the application.

## Mode detection

Changing the capture device or resolution refreshes the pixel-format and
frame-rate lists. A fresh configuration starts at 1920 x 1080. The selected
resolution is saved and does not change when the settings or viewer window is
moved to another monitor.

Auto pixel format prefers uncompressed NV12, followed by YUY2. Every NV12,
YUY2, P010, and MJPEG format reported by the device at the selected resolution
is listed regardless of performance. **MJPEG (experimental compressed
compatibility)** is never chosen by Auto and must be selected explicitly.

H.264/AVC and MPEG-4 are not supported. Their inter-frame references and decoder
reordering make a consistently low-latency path difficult to guarantee.

## NV12, YUY2, and MJPEG

**NV12 is the recommended low-latency format.** It transfers 12 bits per pixel,
compared with 16 bits per pixel for YUY2, during the system-memory-to-D3D11
upload. This generally reduces bandwidth and processing overhead. Both raw
formats then use the same latest-frame-only D3D11 Video Processor path.

YUY2 preserves 4:2:2 chroma detail and may be preferable when color detail is
more important or when a particular device handles it better. The actual
difference depends on the capture device and driver.

MJPEG uses Windows Media Foundation for decoding and is intended for devices
that cannot provide a useful raw mode. Decoding does not add an application
frame queue, but the device encoder and decoder path can still add more latency
and load than NV12 or YUY2.
The viewer prefers the decoder-reported color range and BT.601/709 matrix, then
DirectShow metadata, and falls back to JPEG full range when neither is present.
The Tab overlay and diagnostic log report the applied result and its source.
Start with **Auto (recommended)**. If colors still differ from another
application, an explicit MJPEG selection exposes **MJPEG color interpretation**
with manual BT.709/BT.601 and Full/Limited combinations. This override applies
only to MJPEG and neither appears for nor affects NV12/YUY2.

## Frame rate

Choose the rate the source actually outputs. A 120 fps source contains no new
visual information when captured at 144 fps; a device may repeat or re-time
frames. Use 120 fps for a 120 fps source, 60 fps for a 60 fps source, and 30 fps
for a 30 fps source.

The app lists 30 fps when the device advertises an exact 30/29.97 media type or
includes it in a supported frame-interval range. Selecting it configures the
capture pin itself at that rate instead of receiving a faster stream and
dropping frames. It adds no application queue, but each frame spans 33.3 ms, so
a stable 60 or 120 fps source provides lower video latency and smoother motion.

When several valid source rates exist, a rate with an even cadence on the target
display can improve VSync pacing—for example, 120 fps on a 120 or 240 Hz display.
The source's real maximum rate still takes priority over monitor refresh rate.

## Presentation

**Low latency** presents without waiting for an additional VSync interval. It is
the minimum-latency choice but may show tearing. **VSync** reduces tearing by
waiting for display synchronization and can add a refresh interval of latency.

On desktops with mixed monitor refresh rates, Windows DWM composition can alter
frame pacing or introduce occasional micro-stutter when the window moves between
displays. Low-latency presentation avoids an extra VSync wait but cannot remove
every DWM pacing difference. Compare Input FPS and Present FPS in the Tab overlay
on the monitor that will actually be used.

**Compatibility (Blt + VSync)** uses a different swapchain path from the two Flip
modes above. It can add latency and GPU load and does not support HDR10 output.
Capture and audio buffering remain unchanged.

**Display monitor** chooses the startup location. Auto restores the last position;
an explicit selection starts on that monitor, or the primary monitor if it is
disconnected. The choice does not lock the window or GPU output route: you can
still move the window and use F11 on its current monitor.

## P010 HDR10 (experimental)

HDR viewing requires **Windows HDR on the monitor displaying the app**, not just
an HDR capture source or passthrough display. Tab distinguishes the app's PQ
output from the current display's HDR state. If it reports SDR or unconfirmed
HDR, check Windows and monitor settings first. Display moves and HDR setting
changes are checked periodically.

- The supported path is **P010 / BT.2020 / PQ / Limited → Flip HDR10**.
  Input chroma location selects left or top-left sampling.
- Final connected color metadata takes priority. When PQ is known but some
  fields are missing, HDR10 defaults are used and logged as assumptions.
- Use **Force HDR10** only for confirmed PQ/BT.2020 input whose metadata is
  missing or incorrect. It does not convert SDR into HDR.
- HLG, Full-range HDR, and Blt HDR are unsupported, not silently reinterpreted
  as SDR. P010 with completely absent color metadata retains the SDR assumption;
  for an HDR source, check the force option or device settings.
- Video PQ values are preserved without inventing mastering or peak-light
  metadata. The app does not provide its own HDR-to-SDR tone mapper.
- HDR overlays use linear-light composition with Windows' SDR UI white level,
  or a 203-nit reference when that query is unavailable. This does not adjust
  the video's brightness.

Capture-device processing and display tone mapping may still differ from
passthrough. Final luminance/color validation on actual HDR hardware remains
necessary; these changes do not guarantee an exact match on every device.

## Pixel-perfect and resizing

With **Pixel-perfect** enabled, the client area is fixed to the selected capture
resolution and mouse resizing is disabled. With it disabled, the window remains
freely resizable while preserving the video aspect ratio.

Pixel-perfect fullscreen keeps a strict 1:1 mapping whenever the capture fits
the monitor, centers the original image, and clears unused space to black. Both
QHD and FHD input therefore remain at their original size on a 4K display. If
the capture is larger than the monitor, it is downscaled without cropping and
with its aspect ratio preserved.

`F5` restores exact 1:1 client size without changing the saved Pixel-perfect
option. If capture and monitor resolutions match, it uses fullscreen. If a 1:1
window cannot fit the monitor work area, the HUD reports that it is unavailable.
`F11` toggles borderless fullscreen independently.

The fullscreen cursor defaults to **Auto-hide**: it disappears after two seconds
without pointer activity and returns immediately on mouse movement or wheel
input. Select **Always show** in the Video & window tab when a persistent
pointer is preferred.

When capture and monitor resolutions match, the viewer starts in borderless
fullscreen regardless of the Pixel-perfect setting. A genuinely smaller saved
relative window takes priority so that a window intentionally moved and resized
on another monitor does not unexpectedly reopen fullscreen.

## Scaling and multi-monitor behavior

**Scaling mode** affects only non-1:1 video. Smooth uses standard filtered
scaling. Sharp uses the GPU Video Processor's edge-enhancement filter when the
driver supports it and does not add a frame queue. If unsupported, it falls back
to Smooth.

**Keep relative window size when moving monitors** preserves a similar fraction
of the limiting display dimension across monitors with different resolutions,
DPI, or aspect ratios and restores that ratio on the next launch. It preserves
video aspect ratio without an extra shrink on 16:10, 21:9, and 32:9 displays.
It is independent of Pixel-perfect and may break 1:1 after a monitor move;
press `F5` to restore exact mapping.

**Hide title bar** removes the caption and border in windowed mode. Edge snap
works at outer monitor edges and shared boundaries. Entry and release distance
is 20 DIP after DPI scaling. Hold Shift while dragging to bypass snap.
