# Low Latency Capture Viewer — v0.16.0 single-graph direct viewer

Windows x64 viewer optimized for an AVerMedia GC573, with experimental support
for DirectShow capture cards exposing uncompressed NV12 or YUY2. The runtime
contains no external media player, demuxer, decoder, or FFmpeg path.

## Runtime architecture

One DirectShow Filter Graph
→ one user-selected capture-filter instance (automatic selection prefers GC573)

Video branch:
progressive uncompressed NV12 or YUY2 pin
→ exact `IAMStreamConfig` resolution selection with supported-FPS fallback
→ Sample Grabber `IMediaSample` callback
→ one replaceable latest-sample slot (stale samples are released, never queued)
→ matching D3D11 NV12/YUY2 texture
→ D3D11 Video Processor (BT.709 limited-range conversion)
→ DXGI flip-discard swapchain
→ swapchain and D3D device maximum frame latency = 1
→ selectable VSync or tearing-allowed Present

Audio branch in the same graph and capture-filter instance:
GC573 PCM Audio pin
→ Sample Grabber callback
→ bounded PCM ring
→ selectable WASAPI Shared (`IAudioClient3` when supported) or Exclusive
→ optional 16-tap windowed-sinc clock-drift correction (±1000 ppm)
→ user-selected WASAPI playback endpoint (or the Windows default endpoint)

There is no A/V synchronizer and no video timestamp scheduling. Audio cannot
cause video frames to queue. The DirectShow graph has no reference clock.

## Startup choices

- Audio: WASAPI Shared or Exclusive
- WASAPI output device: follow the Windows default during execution, or keep a
  specific active render endpoint fixed
- Audio period/buffer: endpoint-supported Shared periods, or 5/10/15/20/30/40 ms Exclusive
- Clock-drift correction: Off (unaltered PCM, default) or automatic resampling
- PCM safety target, independent of resampling: 10/15/20/30 ms
- Volume HUD position: top-left (default), top-right, bottom-left, or bottom-right
- Presentation: `Tearing 허용 (초저지연 권장)` or `VSync (찢어짐 방지 · 지연 약간 상승 가능)`
- Capture device: automatic GC573-first selection or a specific DirectShow device
- Capture: 1920x1080 @ up to 120, 2560x1440 @ up to 120, or 3840x2160 @ up to 60
- Pixel format/frame rate per selected device and resolution: Auto (NV12-first)
  or an exact exposed combination such as NV12/120, NV12/60, or YUY2/60
- Pixel-perfect 1:1 client area with manual resizing disabled
- With Pixel-perfect off, mouse resizing is enabled and the selected video
  aspect ratio is preserved
- Optional monitor-relative window size: keeps the same screen-coverage ratio
  when dragged between monitors of different resolutions
- Optional borderless normal window
- Optional work-area edge snap (on by default)
- Optional timestamped UTF-8 diagnostic log in the executable's `logs` folder

The settings are saved beside the executable in `settings.ini`. If the selected
resolution equals the current monitor resolution, Pixel-perfect mode enters
borderless fullscreen automatically. Pixel-perfect and monitor-relative sizing
are independent options. If both are enabled, the window starts at exact 1:1,
but moving it to a monitor with a different size can intentionally break 1:1
to preserve the same screen-coverage ratio.

Monitor-relative sizing records the selected video's fraction of the monitor
when the option is enabled. For example, a 2560x1440 window occupying two thirds
of a 3840x2160 monitor becomes 1280x720 on a 1920x1080 monitor and returns to
2560x1440 when dragged back. The video aspect ratio is preserved on monitors
whose aspect ratio differs.

Capture-driver capabilities can differ between PCs. The viewer always preserves
the selected resolution for pixel-perfect output, prefers the requested frame
rate, then tries the highest lower NV12 frame rate exposed by that driver. The
actual selected rate is shown in the window title, OSD, and diagnostic log.
Automatic device search accepts the exact GC573 name first, then names containing
`GC573` or `Live Gamer 4K`, then the first other DirectShow video device.

## v0.15 device support boundary

GC573 remains the tested and recommended path. Other capture devices are marked
experimental. They must expose an uncompressed progressive NV12 or YUY2 video
mode at one of the three selectable resolutions, plus a 48 kHz stereo PCM audio
output pin on the same DirectShow capture filter. Compressed formats, P010, and
separate USB-audio filters are not connected by this version. No automatic
device-reconnect loop is included; restart the viewer after a device is removed
or reconnected.

The format combo is capability-driven. Changing capture device or resolution
queries that filter's `IAMStreamConfig` modes once in the settings window and
shows the selected usable FPS beside NV12/YUY2. This probe does not run during
capture and therefore does not add render latency.

## Controls

- `F11`: toggle borderless fullscreen
- `Tab`: toggle runtime information OSD
- Mouse wheel over video: adjust application volume by 5%
- Drag the window near an edge/corner: snap without changing window size
- Hold `Shift` while dragging: temporarily bypass application edge snap
- `Esc`: leave fullscreen; press again in window mode to exit

## OSD interpretation

The OSD reports callback FPS, successful Present FPS, callback-to-Present
application processing time, replaced frames, active format/presentation mode,
actual WASAPI allocation/padding, GC573 packet size/period, PCM queue depth,
resampling correction in ppm, and detailed audio health. Each underrun/overrun
shows both event count and the actual missing/dropped PCM duration. A separate
clock diagnosis reports cumulative events/hour, the most recent error age, and
an estimated PCM imbalance in ppm.

The estimated imbalance is not a direct hardware-clock measurement. It includes
Windows scheduling stalls and startup transients. The automatic diagnosis waits
for two minutes, treats an error older than ten minutes as currently stable, and
only recommends correction when recent errors repeat at least 12 times/hour or
the cumulative imbalance reaches 50 ppm. Observe for 10–30 minutes before making
a final choice. The processing time is not total HDMI-to-display latency;
card-internal buffering and monitor scanout require an external latency sensor.

v0.14 separates clock correction from the internal PCM buffer target. The 10 ms target is
the lowest-latency/riskier option, 15 ms is recommended for low-latency
resampling, 20 ms is the stable default recommendation, and 30 ms is intended
for PCs with larger scheduling or driver jitter. The target is measured just
before a WASAPI render request; the current PCM buffer shown after rendering can
therefore be roughly one render period lower. “Output buffer” and “output
occupancy” refer to WASAPI; “PCM buffer” refers to the program's internal audio
ring, so they are not the same buffer.

The OSD separates underruns into three causes: a GC573 input callback arriving
more than one packet period plus 5 ms late, ordinary safety-queue depletion, or
a sinc-resampler output shortfall despite enough raw PCM being present. It also
distinguishes normal resampler operation from correction-limit approach. These
are operational diagnoses rather than hardware-clock measurements.

v0.14.1 expands the cached OSD GPU texture and its text layout from 700x350 to
700x420 pixels so every v0.14 diagnostic line is visible without clipping. The
texture size, DirectWrite layout, background rectangle, swapchain composite,
and top-left volume-HUD offset share the same constants to prevent them from
drifting apart again.

v0.14.2 standardizes the user-facing audio terms: “출력 버퍼” is the WASAPI
device buffer, “출력 점유” is the amount currently occupied in that output
buffer, and “PCM 버퍼” is the program's internal capture-to-render ring. The
PCM setting is labelled “PCM 버퍼 목표”.

v0.14.4 was rebuilt directly from the v0.14.2 source. Pixel-perfect now means
an exact, non-resizable 1:1 client area. Turning it off enables aspect-locked
mouse resizing. Monitor-relative sizing is independent, and the settings UI
warns that combining it with Pixel-perfect may change 1:1 after a monitor move.
Borderless resizing uses custom edge/corner hit testing without adding a native
thick frame, so no title strip or bright top border is introduced.

v0.14.5 fixes a stale monitor-relative scale when Pixel-perfect and relative
sizing are enabled together. In that combination the baseline is always
recomputed from the selected capture resolution and the restored start monitor,
so an old manually saved 25% scale cannot shrink 2560x1440 to 960x540 on 4K.

v0.14.6 increases the snap release threshold from 3 DIP to 8 DIP while keeping
the 18-DIP entry distance. Small post-snap cursor movement no longer detaches
the window, but a deliberate drag still releases without holding Shift.

v0.14.7 increases snap release from 8 DIP to 15 DIP after real-use feedback.
The 18-DIP entry threshold is unchanged.

v0.15.0 adds persistent capture-device, WASAPI output-device, and capability-
filtered NV12/YUY2 selection. It also adds optional timestamped diagnostic log
files. The Tab OSD identifies the active capture device, pixel format, and audio
endpoint. Device reconnect is deliberately not implemented.

v0.15.1 makes the default-output choice follow Windows changes while the viewer
is running. Only WASAPI is stopped and reinitialized; the DirectShow capture
graph and video presentation keep running. PCM accumulated during the switch is
discarded so the new endpoint resumes live audio without retaining added delay.
A short mute or small click can still occur at the transition. Selecting a
specific endpoint keeps the previous fixed-device behavior. The clock-drift
label/help/combo layout was also widened so the full Korean label remains
visible at scaled DPI.

v0.15.2 fixes endpoint-switch latency accumulation. PCM is now discarded after
the replacement WASAPI client has finished initialization and immediately before
its `Start` call, so audio captured during device setup cannot remain as a
permanent queue. The clock-drift label group is positioned from its measured
text width, keeping the full label, help button, and combo compact at every DPI.

v0.15.3 changes the capability selector to `픽셀 포맷 / 프레임`. Every unique
NV12/YUY2 frame-rate combination exposed by the selected device at the selected
resolution is listed separately. Selecting 60 fps configures the DirectShow pin
for 60 fps instead of capturing 120 and discarding frames, reducing capture,
upload, and presentation load. The settings window is widened to 900 DIP and
both columns use aligned label/combo positions; the clock-drift help button now
sits in the label gutter without shifting its combo out of line.

v0.15.4 makes settings-window DPI deterministic. The monitor under the cursor is
selected before creation; its effective DPI sizes both the 900x600-DIP client
and every child control, and the window is centered in that monitor's work area.
All controls use an explicit DPI-scaled 9-point Segoe UI font. Moving the dialog
between monitors rebuilds the font and applies the same DIP layout together with
the DPI-sized outer window, preventing primary-monitor DPI from mixing with the
actual monitor's control scale.

v0.15.5 removes the custom settings-font lifetime introduced in v0.15.4. On
some focus/DPI repaint sequences, transparent STATIC controls retained pixels
from their previous font and produced overlapping text. The dialog now uses the
stable Windows control font and explicitly erases/repaints the parent and every
child after creation, DPI changes, and reactivation. Monitor-specific initial
DPI sizing and the fixed 900x600-DIP layout remain.

Pixel format and frame rate are now separate capability-driven combos. Format
offers Auto/NV12/YUY2 according to the selected device and resolution; Frame
offers Auto plus each supported rate such as 120 or 60 fps. Either can remain
automatic while the other is fixed.

v0.15.6 fixes the remaining 100%-DPI layout mismatch. The settings client is
950x600 DIP, with wider combo columns, and all child controls again receive one
explicit DPI-scaled 9-point Segoe UI font. Unlike v0.15.4, every font handle is
retained until the dialog and all children are destroyed; font assignment uses
no immediate redraw, then one complete erased parent/child redraw follows the
layout. This prevents both cross-DPI clipping and stale glyph remnants.

v0.15.7 makes snap release consistent at every monitor edge. The 18-DIP entry
and 15-DIP release distances are unchanged, but a 15-DIP drag in either
direction now releases even a desktop-outer edge. The window can be moved
off-screen without Shift, and the existing re-snap suppression remains active.

v0.15.8 changes mouse-wheel volume to 5-percent steps and adds an optional
`백그라운드에서 자동 음소거` checkbox. When enabled, losing application focus
ramps only the render gain to zero. Capture, PCM queue consumption, and WASAPI
continue normally, so returning to the app restores the saved volume without
an audio restart or accumulated latency. The option is disabled by default.

v0.15.9 hardens return from another application or a covered window. A
transient D3D11 video-processing, overlay, or Present failure now records the
device-removal reason and rebuilds only the D3D11 renderer/swapchain. DirectShow
capture and WASAPI keep running. A covered/occluded swapchain is tested every
50 ms and resumes without being treated as an error. These branches run only
after an error or occlusion; the normal low-latency frame path is unchanged.

OSD statistics now have a two-second startup warmup. Video/FPS counters, audio
callback statistics, underrun/overrun classification, and observed minimum PCM
queue depth begin only after the warmup. Capture, presentation, and audio output
still start immediately; the warmup affects diagnostics only.

The Tab OSD and volume HUD are cached into small GPU textures and composited
into the same D3D11 swapchain immediately before Present. No GDI overlay child
window is created, so showing an overlay does not switch the presentation to a
separate window-composition path. The measured callback-to-Present cost on the
test PC was 0.491 ms hidden and 0.589 ms with both overlays visible.

## Build

From a Visual Studio 2022 x64 developer shell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

No companion executable is required. Run `LowLatencyCaptureViewer.exe` directly.

## Current implementation boundary

The viewer retains the newest `IMediaSample` by reference instead of copying its NV12
contents to a second CPU buffer. At 2560x1440 @ 120 this removes about 633 MiB/s
of CPU memory copying. The sample is returned to the DirectShow allocator
immediately after upload, before a VSync Present can wait. v0.7.1 rotates three
NV12 GPU surfaces and uses a D3D11.1 discard update when supported, preventing
the next upload from contending with the surface the GPU is still processing.

One system-memory-to-D3D11 transfer remains. Testing confirmed that GC573
samples expose no Direct3D surface and that the filter replaces a negotiated
mapped-D3D11 allocator with its own system-memory allocator. The remaining
transfer is therefore a GC573 DirectShow driver boundary, not an application
queue that can be removed safely.

## Audio, volume, and drift notes

The tested GC573 driver ignored different `IAMBufferNegotiation` sizes and
always produced 480-frame (10 ms) PCM packets. The misleading input-buffer
choice has therefore been removed. The app keeps one fixed compatibility hint
and exposes only the actual callback packet size and interval in the OSD.

Drift correction is disabled by default, so PCM samples reach WASAPI without
resampling. When enabled, the viewer waits for a small PCM safety margin and
uses a 16-tap windowed-sinc interpolator to adjust consumption by no more than
±1000 ppm. This favors long-running stability but adds queueing and changes
samples, so Off remains the low-latency/unaltered-audio choice.

Application volume ranges from 0 to 100 percent. At 100 percent the PCM gain
loop is bypassed completely. Other levels use a short per-buffer gain ramp to
avoid clicks; volume is attenuation-only and never boosts above the source.
Wheel changes use 5-percent steps. Background auto-mute uses the same ramp while
preserving the logical volume and continuing real-time PCM consumption.

The last window-mode position and monitor are saved at normal close. On the
next run the selected pixel-perfect size is recalculated, while the saved
origin is restored and clamped to a currently available monitor. Automatic
borderless fullscreen still remembers the pre-fullscreen window position.

Window edge snap uses the nearest monitor's work area, so the taskbar is not
covered. Its 18-DIP entry threshold follows the window DPI and works on every
edge and corner in multi-monitor layouts. Once attached, moving 15 DIP in either
direction releases that axis immediately. This passes naturally into an adjacent
display and also permits deliberate movement beyond an outer desktop boundary.
The released axis cannot reattach until it first leaves the entry zone,
preventing sticky repeated snap.

Per-monitor DPI transitions use `WM_GETDPISCALEDSIZE` to declare the exact
pixel-perfect outer size before Windows generates `WM_DPICHANGED`. The suggested
rectangle is then applied exactly once. A final move-end check keeps the selected
client resolution exact without accumulating scale changes across monitor trips.

The startup settings window uses an 820 x 600 DIP two-column client layout and scales every
control with the active monitor DPI. The text size is unchanged; the window and
control widths/heights are enlarged so long labels and choices are not clipped.

The VSync choice explicitly notes its possible small latency increase. A `?`
button beside clock-drift correction explains how to read `안정`, `드문 오류`,
and `반복 불균형` in the Tab OSD and when resampling is recommended. Clicking it
opens the complete guide, including the meaning and limitations of the estimated
ppm value. These settings controls exist only before capture starts and do not
alter the render path.
