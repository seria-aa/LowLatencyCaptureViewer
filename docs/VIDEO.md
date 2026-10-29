<img align="right" src="images/app-icon.png" width="40" height="40" alt="LLCV">

# Video modes, scaling, and fullscreen

[README](../README.en.md) · [Documentation](INDEX.md) · [한국어](VIDEO.ko.md)

[Capture path](#capture-path) · [Mode detection](#mode-detection) · [NV12, YUY2, and MJPEG](#nv12-yuy2-and-mjpeg) · [Frame rate](#frame-rate) · [Unlisted 30 fps](#unlisted-30-fps-requests) · [40 fps capture](#40-fps-capture) · [Presentation](#presentation) · [P010 HDR10](#p010-hdr10) · [Pixel-perfect and resizing](#pixel-perfect-and-resizing) · [Scaling and multi-monitor behavior](#scaling-and-multi-monitor-behavior)

Keys in this guide are defaults. If you [change a shortcut](USAGE.md#changing-shortcuts),
use its assigned key instead.

<br>

## Capture path

The viewer captures through DirectShow, keeps only the newest video sample, and
presents it through the D3D11 Video Processor and, by default, a flip-discard swapchain with
maximum frame latency set to 1. Stale samples are discarded instead of being
allowed to form a playback queue.

If the device announces a changed video layout or color interpretation during
playback, the viewer stops and asks you to restart capture instead of displaying
frames using stale settings. Equivalent format confirmations continue normally.
Restart capture if prompted after changing the console's HDR or resolution.
This does not detect source changes that the device never reports.

Reported device loss or input-path errors also stop playback and provide restart
guidance; the app does not repeatedly rescan or reconnect the device. An explicit
end of all capture streams returns to settings and releases sleep-prevention
requests. Temporary no-signal alone is not treated as stream completion. See
[playback troubleshooting](TROUBLESHOOTING.md#playback-stops-or-input-disappears).

The app-processing value in the Tab overlay covers this application path. It is
not the total HDMI-to-display latency; the capture device, source, desktop
composition, monitor, and input device add latency outside the application.

<br>

## Mode detection

Changing the capture device or resolution updates the pixel-format and
frame-rate lists using background queries and cached results. Results are reused
while settings remain open; **Refresh** requests a fresh device/capability query
after a device or driver change. Start is unavailable while required video
capabilities are being checked or could not be queried.

A fresh configuration starts at 1920 x 1080. The selected
resolution is saved and does not change when the settings or viewer window is
moved to another monitor.

Auto pixel format prefers uncompressed NV12, followed by YUY2, then UYVY/HDYC. Every NV12,
YUY2, UYVY/HDYC, P010, MJPEG, RGB24 and XRGB (RGB32) format reported by the device at the selected resolution
is listed regardless of performance. **MJPEG (compressed
compatibility)** is never chosen by Auto and must be selected explicitly.

H.264/AVC and MPEG-4 are not supported. Their inter-frame references and decoder
reordering make a consistently low-latency path difficult to guarantee.

<br>

## RGB24 and XRGB (RGB32)

Manually select RGB SDR when the device advertises it at the chosen resolution/FPS.
Auto retains NV12 → YUY2 → UYVY/HDYC priority.
RGB24 stores BGR bytes; XRGB stores BGRX and ignores X.
Playback, source screenshots and recording honor orientation and the SDR range setting.

RGB24 is GPU-repacked for display; XRGB feeds the existing video processor directly.
NVIDIA VSR can be selected, mutually exclusive with CMAA2→NIS. Effects can differ by
input format, and accepted requests do not certify enhancement activation.
Source PNGs exclude VSR and OSD.

Recording converts RGB on its worker to BT.709 Limited NV12 (4:2:0).
See [recording safeguards](RECORDING.md) for headroom checks and memory limits.
RGB capture/recording is SDR-only, not an HDR-console capture workaround.
NV12 remains the recommended general low-latency input.

<br>

## NV12, YUY2, and MJPEG

**UYVY / HDYC (8-bit 4:2:2)** is also supported for playback and source screenshots.
The GPU reorders its bytes into YUY2 on the existing upload surface; the capture
sample is unchanged. HDYC defaults to BT.709 when color metadata is absent.
Explicit device color metadata and the user's SDR range setting remain effective.
UYVY/HDYC recording is not supported: select NV12, YUY2 or MJPEG to record.
This uses DirectShow, not the separate Blackmagic DeckLink SDK; a device name
appearing in the list does not guarantee a compatible mode at every resolution.
v210 (10-bit 4:2:2) input is not supported.

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
Decode buffers are reused to reduce repeated allocation. This does not change
the latest-frame policy or guarantee lower end-to-end latency on every device.
Playback, screenshots and recording use the same color interpretation from the
final connection, retaining earlier hints when later types omit them.

**SDR color range** offers **Auto / Limited / Full**. Auto is the recommended
default and preserves the existing policy: NV12/YUY2 use device metadata and
fall back to Limited; MJPEG prefers decoder metadata, then DirectShow metadata,
and falls back to Full. Use a manual range only to match the actual captured
samples when blacks are crushed or the picture looks washed out. This does not
change the HDMI output range or recover already-clipped detail, and **does not
apply to P010/HDR**. The Tab overlay and diagnostic log show the applied result
and its source.

An explicit MJPEG selection also exposes **MJPEG matrix: Auto / BT.709 / BT.601**;
range selection is handled only by the common control above. Auto prefers
decoder/device metadata, then BT.709 for HD or BT.601 for SD. Existing manual
MJPEG corrections are preserved until a new common range is explicitly selected.

<br>

## Frame rate

An explicit rate such as 30 or 60 fps is never silently replaced with another rate.
Auto pixel format chooses among formats supporting that rate. A different rate
reported by the driver stops startup with a notice. **Auto** frame rate retains
the existing format preference and supported-mode fallback. A selection missing
after a query, resolution or format change remains marked **unavailable**; choose
another rate or format before starting.

This requests a capture rate, not a limit on LS-generated frames. Compare input
and Present under **Tab → Actual FPS**. Sustained input above the requested rate
raises a diagnostic/log warning, even if driver metadata reports the expected
rate. The warning adds no waits or software frame cap.

Use **Frame rate → More** in Video settings for unlisted FPS requests and 40 fps
capture. Escape or Close dismisses only these options, preserving the pending
choices; Cancel in the main settings window still discards changes.
SDR color range sits below pixel format. MJPEG matrix and HDR options appear
only for their corresponding formats. Small work areas retain the existing
whole-option pages.

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

<br>

### Unlisted 30 fps requests

Enable **Video → Allow unlisted FPS**, select **Frame rate → 30 fps**, then **Start**.
The option is off by default and applies to the selected resolution/pixel format, not only QHD.

The separately colored **Direct request: 30 fps** line below mode detection is not a support confirmation.
If the driver rejects the request or the connected format reports another rate, startup stops with a notice.
After starting, check **Actual FPS** in Tab as well. This does not receive 60 fps and discard frames in the app.

Disabling the option resets an added 30 fps selection to Auto; driver-listed 30 fps remains available.
**40 fps capture** takes precedence, so turn it off before selecting 30 fps.
Support varies by device and mode. [Startup troubleshooting](TROUBLESHOOTING.md#unlisted-30-fps-request-fails)

<br>

### 40 fps capture

**40 fps capture** is off by default. Enable it under **More** beside Frame rate in Video settings
to request 40 fps from the capture driver instead of the normal frame-rate selection. Disabling it
restores that selection. Mode detection does not insert synthetic 40 fps entries.
An unlisted rate may work, but support depends on the device, resolution and pixel
format. If the driver rejects the request or the connected format does not report
40 fps, startup fails rather than silently substituting another rate. The check
runs on **Start**; toggling the option alone does not open or test the device.
Driver-reported format and actual delivery rate can differ, so check the measured
input FPS in the Tab overlay too.

The option requests a capture rate within a supported resolution/format combination.
Which NV12, YUY2, P010, or MJPEG combinations accept 40 fps depends on the driver.
The app does not interpolate frames or convert a faster stream to 40 fps in software.
Each 40 fps frame spans 25 ms, so input changes may appear later than with
60/120 fps capture. The option alone does not reduce latency or smooth motion.

Keep the normal frame-rate selection for ordinary viewing. A 40 fps game can
also be viewed by capturing its supported 120 Hz output at 120 fps; the game's
40 fps rate alone is not a reason to enable this option. Use it only when a
specific capture rate is needed.

A game varying between 30 and 40 fps can produce repeated frames in fixed 40 fps
capture, for example held for 25 or 50 ms, even without losing unique frames.
This does not lock the game to 40 fps or replace VRR. The app also includes
**Guide & logs → 40 fps capture guide** for the basic behavior and cautions.

#### Usage example: Lossless Scaling

This is an example of supplying 40 fps input to an external frame-generation app,
not a built-in LS integration or a required combination.

**This assumes a game holding a steady 40 fps.** If it fluctuates between 30 and
40 fps, repeated frames can leave stutter intact or make it more noticeable with LS.

For a console's 40 fps game mode, use 120 Hz HDMI output when supported and confirm
the signal in the capture card's input information. HDMI 120 Hz does not imply
120 unique game frames. A 60 Hz signal cannot preserve evenly spaced 40 fps motion.

[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) is
a separate app, not a built-in LLCV feature. If you choose to use it, leave GPU
headroom and apply its frame generation to the LLCV window. For example, 40 fps multiplied by three targets
120 fps; compare on a 120 Hz viewing monitor. LLCV does not change the monitor
refresh rate or configure LS automatically.

LS can add latency and visual artifacts; actual game/LS
results are not guaranteed. If worse, disable this option and LS, then compare
normal capture.

<br>

## Presentation

**Low latency (standard)** and **Low latency (exclusive fullscreen)** both present without
an additional VSync wait. Standard is the default and supports a window or borderless
fullscreen alongside other apps. Exclusive fullscreen may further reduce display latency,
depending on the system, but other windows cannot share that screen while exclusive output is active.

**Low latency (exclusive fullscreen)** is opt-in. It releases exclusive output for Alt+Tab and settings/help
dialogs, then restores it on return; failure falls back to windowed output. OSD, HDR and VSR keep
their existing support conditions. Use Low latency (standard) for working with other windows.
Direct display and a fixed latency improvement are not guaranteed on every system.


Both low-latency modes may show tearing. **VSync** reduces tearing by
waiting for display synchronization and can add a refresh interval of latency.

On desktops with mixed monitor refresh rates, Windows DWM composition can alter
frame pacing or introduce occasional micro-stutter when the window moves between
displays. Low-latency presentation avoids an extra VSync wait but cannot remove
every DWM pacing difference. Compare Input FPS and Present FPS in the Tab overlay
on the monitor that will actually be used.

**Compatibility (Blt + VSync)** uses a different swapchain path from low latency and
VSync's Flip output. It can add latency and GPU load and does not support HDR10 output.
Capture and audio buffering remain unchanged.

**Display monitor** chooses the startup location. Auto restores the last position;
an explicit selection starts on that monitor, or the primary monitor if it is
disconnected. The choice does not lock the window or GPU output route: you can
still move the window and use F11 on its current monitor.

<br>

### OBS and exclusive fullscreen comparison

Reference results from a test PC on 2026-10-17. Windows ETW was used to compare
the **source display event to the viewer display event for the same frame**,
including HDMI/capture transport. This is neither a camera/photodiode measurement
of physical input-to-photon latency nor the app-processing value in the Tab overlay.

| LLCV presentation mode | OBS mean latency | LLCV mean latency | Reduction vs. OBS |
| --- | ---: | ---: | ---: |
| Windowed | 32.9ms | 27.9ms | 5.0ms |
| Borderless fullscreen | 33.3ms | 27.6ms | 5.7ms |
| Exclusive fullscreen | 33.3ms | **11.4ms** | **21.9ms** |

OBS 32.2.2 used a windowed preview projector for the windowed row; both fullscreen
rows share the same fullscreen preview projector baseline. OBS was not measured
in exclusive fullscreen. GC573 captured 2560×1440 120fps NV12 SDR, shown on the
same 3840×2160 144Hz monitor. The window had a 1280×720 video area; fullscreen
output was 3840×2160. Differences between rows cannot be attributed solely to
window decorations because output size also changed.

VSR, OSD and recording were off. OBS used a 120fps composition, capture buffering
disabled and its main preview disabled. Each path ran twice with a 25-second
steady window per run. Means pool matched frames: windowed OBS 5,811 and LLCV
5,921; fullscreen OBS 5,420, LLCV borderless 6,000 and LLCV exclusive 5,220.

**Limitations:** The same GPU generated the HDMI test source and ran the viewer.
Some repeats in both paths missed unique frames; averages include only matched
displayed frames. Source-side drops were observed, but the remaining missing
frames could not be attributed separately. This can bias the comparison: the
results do not establish lossless playback or a universal 21.9ms advantage.
They do not cover every OBS output path/configuration or audio latency and sync.

<br>

## P010 HDR10

HDR viewing requires **Windows HDR on the monitor displaying the app**, not just
an HDR capture source or passthrough display. Tab distinguishes the app's PQ
output from the current display's HDR state. If it reports SDR or unconfirmed
HDR, check Windows and monitor settings first. Display moves and HDR setting
changes are checked periodically.

- The supported path is **P010 / BT.2020 / PQ / Limited → Flip HDR10**.
  Input chroma location selects left or top-left sampling.
- P010 is **HDR-only**. Input reported as SDR is rejected with guidance instead
  of rendered through an SDR P010 path. Select NV12/YUY2 for SDR.
- Final connected color metadata takes priority. When PQ is known but some
  fields are missing, HDR10 defaults are used and logged as assumptions.
- P010 without transfer/gamut metadata **assumes PQ/BT.2020 HDR10**, like the
  OBS DirectShow P010 default. This is an assumption, not successful HDR
  detection, and is identified in the log. Explicit SDR/HLG or incomplete
  BT.2020 metadata is not replaced by this fallback. Range/chroma guards remain.
- Use **Force HDR10** only for confirmed PQ/BT.2020 input whose metadata is
  incorrect or incomplete. It does not convert SDR into HDR.
- **HDR chroma placement** appears below Force HDR10 when P010 is selected.
  Leave it on **Auto (recommended)** normally. **Top-left** and **Left** are
  manual compatibility interpretations for missing or incorrect placement
  metadata. Compare fine colored edges and text with a reference; opening the
  video does not by itself establish correct placement. These choices do not
  reconstruct staggered chroma planes or change HDR range validation.
- HLG, Full-range HDR, and Blt HDR are unsupported, not silently reinterpreted
  as SDR. Use NV12/YUY2 for SDR sources without color metadata. Turning Force
  HDR10 off does not disable the automatic HDR assumption for untagged P010.
- Video PQ values are preserved without inventing mastering or peak-light
  metadata. The app does not provide its own HDR-to-SDR tone mapper for **live viewing**.
  [Screenshot saving](USAGE.md#screenshots) separately uses SDR sRGB tone mapping.
- HDR overlays use linear-light composition with Windows' SDR UI white level,
  or a 203-nit reference when that query is unavailable. This does not adjust
  the video's brightness.
- The HDR `Tab` diagnostics panel uses a black background at **90% opacity**
  (about 10% scene-light transmission). Text uses the same UI white level;
  SDR panel appearance is unchanged.

Capture-device processing and display tone mapping may still differ from
passthrough. Final luminance/color validation on actual HDR hardware remains
necessary; these changes do not guarantee an exact match on every device.

### Capture-card hardware tone mapping

At capture startup, supported AVerMedia and Elgato devices are asked
once to turn internal HDR-to-SDR conversion **off for P010** and **on for
NV12/YUY2/MJPEG**. Support is queried on the selected capture filter rather than
inferred from a product-name list. AVerMedia UVC extension control validates the
selected filter's extension-node response before writing. P010 alone does not
prove the signal is HDR.

Unsupported or failed commands do not stop capture. `[capture-hdr]` logs the
result; command acceptance is not verification of the actual input encoding.
This adds no per-frame processing stage or frame queue.

Elgato USB/HID control is restricted to the published HD60 S+, HD60 X and HD60 X
Rev.2 device IDs. The video and HID interfaces must belong to the same physical
Windows device container; ambiguous matches are skipped. No guessed commands
are sent to other models or vendors. This is not a real-hardware compatibility
guarantee for every card. Avoid concurrent capture-device use by multiple apps.

<br>

## Pixel-perfect and resizing

**Capture resolution** and **Display resolution** are independent of VSR.
Choose **Same as capture** for the display size to follow changes to the capture resolution.
This is the default for new profiles. On upgrade, equal saved capture/display sizes follow capture;
different sizes remain fixed. An explicitly saved fixed/matched mode always takes precedence.
**Pixel-perfect / Lock display size** locks the selected display size. This is source 1:1 only
when both resolutions match. Disable it for aspect-preserving manual resizing.
F6 toggles the selected upscaling method while retaining its sharpness; F5 restores input size.

Resizing updates only the necessary video-output resources and reuses resources
that do not depend on window size. This reduces recreation work; it does not
change capture resolution or buffering, and does not guarantee a measured
latency improvement.

Borderless and exclusive fullscreen retain the selected display size and fill margins with black.
For example, 1920×1080 on a 4K monitor remains 1080p-sized. **Window → Fullscreen video position**
selects the center, an edge or a corner. Oversized output is aspect-fitted without cropping.

`F5` restores exact 1:1 client size without changing the saved Pixel-perfect
option. If capture and monitor resolutions match, it uses fullscreen. If a 1:1
window cannot fit the monitor work area, the HUD reports that it is unavailable.
`F11` toggles fullscreen using the selected presentation mode independently.

The fullscreen cursor defaults to **Auto-hide**: it disappears after two seconds
without pointer activity and returns immediately on mouse movement or wheel
input. Select **Always show** in the **Window** page when a persistent
pointer is preferred.

When capture and monitor resolutions match, the viewer starts in borderless
fullscreen regardless of the Pixel-perfect setting. A genuinely smaller saved
relative window takes priority so that a window intentionally moved and resized
on another monitor does not unexpectedly reopen fullscreen.

<br>

## Scaling and multi-monitor behavior

Select **Off / CMAA2 → NIS / NVIDIA VSR** under **Video → Upscaling**.
CMAA2 → NIS smooths SDR source edges before enlargement up to 2× per axis,
with default sharpness **0.15**. It is opt-in and mutually exclusive with VSR.
HDR, native-size output, downscaling and enlargement beyond 2× use the original path.
Increase display size if it currently matches capture.
Recordings and screenshots are not filtered; OSD is drawn afterward.
See [selection and requirements](USAGE.md#cmaa2--nis-upscaling).

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

---

[README](../README.en.md) · [Documentation](INDEX.md) · [한국어](VIDEO.ko.md)
