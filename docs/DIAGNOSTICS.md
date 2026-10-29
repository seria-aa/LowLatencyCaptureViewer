<img align="right" src="images/app-icon.png" width="40" height="40" alt="LLCV">

# Diagnostics and logs

[README](../README.en.md) · [Documentation](INDEX.md) · [한국어](DIAGNOSTICS.ko.md)

[Video fields](#video-fields) · [Audio fields](#audio-fields) · [Buffer terminology](#buffer-terminology) · [Buffer diagnosis](#buffer-diagnosis) · [Log files](#log-files)

Press `Tab` in the viewer to show or hide the diagnostics overlay.

Keys in this guide are defaults. If you [change a shortcut](USAGE.md#changing-shortcuts),
use its assigned key instead.

In video mode, press `F3` to show or hide the audio meter OSD. Audio-only mode
uses a separate, always-visible view for master volume, L/R peak levels and
channel volume, and clipping status. Use the wheel over an L/R card to adjust that
channel. In audio-only mode, double-click the master or L/R region to reset its
volume to 100%. In video mode, window dragging can intercept double-clicks depending
on the window style, so do not assume the same reset behavior as audio-only mode.

<br>

## Video fields

- **Path:** selected capture device and DirectShow-to-D3D11 path
- **Input:** active resolution, FPS, and pixel format
- **Quality:** color range and D3D11 processing path
- **Display:** output size, scaling mode, swapchain, and presentation mode
- **Actual FPS:** measured Input and Present rates
- **App processing latency:** time inside the application, not total HDMI latency
- **Frames:** captured, presented, replaced, or discarded counts

Latest-frame replacement is expected when capture produces a newer sample before
the previous one is presented. A rising count does not automatically mean the
viewer is accumulating latency; the old sample is being discarded specifically
to prevent that queue.

Video statistics exclude the first five seconds after startup.

With 40 fps capture enabled, check the measured Input rate under **Actual FPS**,
not just the format's nominal FPS. This counts samples arriving at the app, not
unique game frames or the console's HDMI output Hz. Repeated pictures count as
input samples too. Frames generated later by LS are not part of LLCV's Present FPS.
Sustained input above an explicitly selected rate adds a warning on the same line and in the log.
This reports driver delivery behavior; it does not add a software frame limiter.

<br>

## Audio fields

- **Audio output:** active WASAPI/ASIO mode and device, output buffer, and WASAPI occupancy
- **Capture audio:** packet size and callback interval
- **Clock correction:** enabled state and applied rate adjustment
- **PCM buffer:** current, target, and observed minimum queue depth
- **Audio errors:** underrun and overrun counts and durations
- **Cause/trend:** recent diagnosis and estimated long-term imbalance

If ASIO startup falls back to WASAPI Shared, the overlay reports Shared and the
actual output device. The saved ASIO selection remains available for the next
attempt; it does not identify the active backend after a fallback.

The first five seconds are excluded from audio error diagnostics. Capture and
playback still begin immediately; only the statistics wait for warmup.

The ppm value is an operational estimate rather than a direct hardware-clock
measurement. Startup and scheduling stalls can affect it. Observe a stable
session for 10–30 minutes before enabling drift correction solely from ppm.

<br>

## Buffer terminology

- **Output buffer:** the WASAPI endpoint buffer or ASIO driver buffer
- **Output occupancy:** audio currently queued in the WASAPI endpoint
- **PCM buffer:** the application's capture-to-render safety queue

These are separate. Raising the PCM target adds application audio queueing;
changing the WASAPI request affects the output endpoint.

An occasional underrun is not necessarily audible or harmful. If underruns
repeat, first confirm that the output device is stable, then raise the PCM target
one step. Enable clock correction only for a persistent drift pattern rather
than an isolated scheduling interruption.

<br>

## Buffer diagnosis

Use the Tab overlay in this order:

- **Current PCM queue:** the app queue at the instant the OSD was refreshed. It can
  briefly look low just after a render callback.
- **Target:** the PCM queue the app tries to maintain.
- **Observed minimum:** the lowest queue depth observed at a render-callback
  boundary during the session. It is sampled at a different time from Current, so
  the two values do not have to match.
- **Underrun:** the number of times output actually ran out of PCM.

Use these rules:

1. **Zero underruns + normal buffer diagnosis:** keep the current setting.
2. Repeated **`buffer shortage`** or **`resampler output shortage`**: raise the PCM
   target one step at a time, `10 → 15 → 20 → 25 → 30 ms`. Both can indicate that the app's
   PCM safety margin is too small, so adjust the PCM target first.
3. Repeated **`input late`** alone points to capture-callback or system-scheduling
   delays; increasing the PCM target may not fix it.
4. Prefer the **recent error pattern and maximum consecutive count** over one old
   event. A single isolated event may never be audible; repeated or consecutive
   events justify a larger buffer.

With ASIO, the **app PCM queue and the ASIO driver output buffer are separate**. If
the app PCM diagnosis is normal but ASIO still breaks up, check the output buffer in
the ASIO driver's control panel as well.

New installations use a **25 ms** PCM target. A legacy saved 20 ms target is migrated
to 25 ms once; other saved values are retained, and you can select 20 ms again afterward.
After verifying stable operation, try `25 → 20 → 15 → 10 ms` only if lower latency is
needed. Raise the target one step at a time if errors recur; try 30 ms if 25 ms is insufficient.

<br>

## Log files

Enable **Save diagnostic log file** in the **Guide & logs** tab. Logs are
written to:

```text
%LOCALAPPDATA%\LowLatencyCaptureViewer\logs
```

The log records selected devices and modes, actual allocator sizes, WASAPI
initialization, fallback paths, capture-graph stages, and filter pin/media-type
details after an initialization failure.

Audio-processing diagnostics are queued or collected as counters and emitted
later on the UI thread, outside the audio callback/processing path. Audio-error
history uses bounded storage to reduce allocation. If the diagnostic queue fills
while the UI is busy, messages can be skipped and a skipped-message count is
logged. These changes do not establish a measured latency or FPS improvement.

For display interruptions, check `display-event` records for timestamped display
or device changes and swapchain occlusion/recovery. If D3D11 transfer, Present, or
output recreation fails, a `video-event` record includes the HRESULT and
`GetDeviceRemovedReason` result. Normal frames do not generate these event logs.

Saved logs are automatically bounded: each part is limited to 2 MB, and the
newest five managed log files are retained up to a combined 10 MB. Managed logs
older than seven days are removed when a new logging session starts. A long
session may create `..._part02.log`; attach every part from that session when
reporting a problem.

**Show diagnostic console** displays the same live diagnostic output in a
console window. It is disabled by default and is not required for file logging.

When reporting a problem, include the complete log and state the capture device,
source resolution/FPS, selected pixel format, capture audio device, and output
audio device.

---

[README](../README.en.md) · [Documentation](INDEX.md) · [한국어](DIAGNOSTICS.ko.md)
