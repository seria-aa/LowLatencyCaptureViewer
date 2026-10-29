<img align="right" src="images/app-icon.png" width="40" height="40" alt="LLCV">

# Troubleshooting

[README](../README.en.md) · [Documentation](INDEX.md) · [한국어](TROUBLESHOOTING.ko.md)

[Capture initialization fails](#capture-initialization-fails) · [Unlisted 30 fps](#unlisted-30-fps-request-fails) · [40 fps startup](#40-fps-capture-will-not-start) · [Playback stops or input disappears](#playback-stops-or-input-disappears) · [Devices or supported modes look out of date](#devices-or-supported-modes-look-out-of-date) · [Video works but audio does not](#video-works-but-audio-does-not) · [The picture is soft](#the-picture-is-soft) · [MJPEG color differs from another application](#mjpeg-color-differs-from-another-application) · [Frame drops or stutter](#frame-drops-or-stutter) · [Settings window is skipped](#settings-window-is-skipped) · [A shortcut does not work](#a-shortcut-does-not-work) · [Settings could not be saved](#settings-could-not-be-saved) · [Sleep or screen-off behavior](#sleep-or-screen-off-behavior) · [Collecting a useful report](#collecting-a-useful-report)

Keys in this guide are defaults. If you [change a shortcut](USAGE.md#changing-shortcuts),
use its assigned key instead.

<br>

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
48 kHz mono/stereo uncompressed PCM or 32-bit float audio path. The stage and
pin diagnostics in the log are more useful than the code by itself.

<br>

## Unlisted 30 fps request fails

**Allow unlisted FPS** and the colored 30 fps request line do not guarantee support.
Acceptance is checked after selecting **Start**.

1. Turn off **40 fps capture** and confirm 30 fps is selected.
2. If it fails, disable **Allow unlisted FPS** and retry Auto or a previously working rate.
3. If the normal mode works, the driver may reject 30 fps for that resolution/format.
   Supporting 60 fps does not guarantee 30 fps. Compare only formats the driver actually offers.

Even after startup succeeds, check **Actual FPS** in Tab. Include the device name, resolution, format and diagnostic log when reporting a problem.

<br>

## 40 fps capture will not start

Enabling **40 fps capture** does not add 40 fps to the detected modes. A driver
may accept an unlisted rate, but an available checkbox is not proof of support.
The app checks acceptance and the connected format when you select **Start**.

1. Disable 40 fps capture and retry the normal frame-rate selection.
2. If normal capture works, check 40 fps support for that resolution/pixel format.
   Try only other formats actually offered by the driver.
3. If it still fails, include a diagnostic log, capture-device name, resolution,
   and pixel format in your report.

The app does not silently replace a failed 40 fps request with 60/120 fps.
Even after successful startup, the reported format and actual delivery rate are
separate: check **Actual FPS** in Tab. Check the console's HDMI input Hz separately
in the capture-card utility.

<br>

## Playback stops or input disappears

If the capture device reports a disconnect or input-path error, playback stops
and the app shows restart guidance before returning to settings. Reconnect the
device, close other capture applications if needed, then select **Start** again.
The app does not repeatedly rescan devices or automatically restart capture.

If the source explicitly completes all capture streams, playback ends normally,
sleep-prevention requests are released, and settings reopen. This is distinct
from temporary no-signal or a pause in new frames, which alone does not end
playback. Check the source and HDMI connection; if input does not recover, use
`F2` to return to settings, verify the mode, and start again manually.

A reported video/audio format change can also stop playback to avoid incorrect
output. Check the source's HDR, resolution, and audio settings before restarting.
Attach a diagnostic log if any of these stops repeat unexpectedly.

<br>

## Devices or supported modes look out of date

Video, capture-audio, and WASAPI Shared capability queries run in the background
and reuse results while settings remain open. After reconnecting a device or
changing its driver/settings, select **Refresh** and wait for queries to finish.
Start remains unavailable while required video capabilities are pending or failed;
Refresh also retries a failed query. This is a manual settings action, not a
playback-time rescan loop.

Refreshing while ASIO is selected also invalidates the retained WASAPI device's
Shared capability results for the next switch back. It does not repeat completed
Exclusive endpoint tests.

<br>

## Video works but audio does not

First check the speaker button's mute state and master/L/R volume. Press **M**
(default) to unmute. If background auto-mute is enabled, activate the LLCV window.

Some USB devices expose video and audio as separate DirectShow filters. Choose
the matching USB audio input manually under **Capture audio device**. The audio
input must provide 48 kHz mono/stereo uncompressed PCM or 32-bit float. 16-bit
stereo PCM remains the direct, conversion-free path.

If audio breaks up only after a long session and separate filters are selected,
observe the PCM trend in the Tab overlay and try clock-drift correction. For
immediate, repeated underruns, increase the PCM target one step instead.

<br>

## The picture is soft

Pixel-perfect gives exact 1:1 mapping when the window can use the capture
resolution directly. In a scaled window or fullscreen mode, try **Sharp**. It
uses supported D3D11 Video Processor edge enhancement without adding a frame
queue and falls back to Smooth when unavailable.

Also confirm that the source and capture resolution match. Capturing a 1440p
source as 4K does not create additional detail.

<br>

## MJPEG color differs from another application

Some MJPEG devices omit Full/Limited range or BT.601/709 information. The
viewer resolves it from the Media Foundation decoder, DirectShow metadata, and
finally the MJPEG fallback. Color metadata from the final connected capture format
is also used for playback, screenshots and recording. Check **Video quality** in the Tab overlay and the
`[video] MJPEG color` log line for the applied result. If another application
still differs, start from Auto: check **SDR color range → Limited / Full** for
black/white levels, or **MJPEG matrix → BT.709 / BT.601** for color differences.
The matrix control appears only for an explicit MJPEG selection. The range
control also applies to NV12/YUY2, but never to P010/HDR. If no setting matches,
include screenshots of the same scene with the diagnostic log.

<br>

## Frame drops or stutter

If using 40 fps capture with LS, first disable LS and the 40 fps option and compare normal capture.
For 40 fps or LS 3× output, try 120 Hz on a monitor that supports it. Since 144 is
not an integer multiple of 40, fixed-refresh presentation can have uneven spacing.
Matching refresh rate does not remove the game's own variation between 30 and
40 fps. See the [40 fps capture guide](VIDEO.md#40-fps-capture).

Confirm that Input FPS matches the selected source rate and compare it with
Present FPS in the Tab overlay. Try NV12 before YUY2 or MJPEG, reduce resolution
or FPS if the device or USB link cannot sustain the mode, and test on the target
monitor.

Mixed monitor refresh rates and DWM composition can change windowed frame
pacing. Compare Low latency and VSync on the monitor where the viewer will stay.

<br>

## Settings window is skipped

If **Start directly next time** is enabled, hold Shift while launching. You can
also press `F2` from the viewer to close it safely and return to settings.

<br>

## A shortcut does not work

Click the viewer to focus LLCV, then check your bindings under **Guide & logs**.
Choose **Start** after editing them; **Cancel** discards unapplied changes.
Settings controls keep their normal input/navigation keys, and video-only
actions remain unavailable in audio-only mode.
In settings, Help and Recording shortcuts work only when assigned to **F1–F24 except F4**,
with or without Ctrl/Alt/Shift. For a different assigned key, focus the viewer to use it.

If you forget the Settings shortcut, hold **Shift** while launching. Use
**Reset defaults → Start** to restore the standard bindings.
See [Changing shortcuts](USAGE.md#changing-shortcuts).

<br>

## Settings could not be saved

On a settings-save failure, the previous settings file is preserved; unsaved
changes apply only to the current session. Check available disk space and write
permissions for `%LOCALAPPDATA%\LowLatencyCaptureViewer` and its `settings.ini`,
then save your choices again. Include the displayed error code and diagnostic
log if the warning persists.

<br>

## Sleep or screen-off behavior

**App → Prevent sleep during playback** is on by default. Video playback requests
that both the screen and system stay awake; audio-only requests that the system
stay awake but allows the screen to turn off. Requests end when playback ends,
fails, or closes; the settings dialog alone does not keep the PC awake.

Explicit sleep, lid-close actions, and Windows power policies take precedence.
If the user returns from sleep and playback continues, requests are restored;
an unattended automatic wake does not rearm them. The option does not change
Windows power settings or disable screen savers or security locks. Turn it off
if you want normal idle sleep during playback.

<br>

## Collecting a useful report

Enable **Save diagnostic log file**, reproduce the failure once, and attach the
newest file from:

```text
%LOCALAPPDATA%\LowLatencyCaptureViewer\logs
```

Include the capture device, source resolution/FPS, selected format, capture
audio device, output endpoint, Windows version, and whether another capture
application had been running.

<br>

## Recording fails to start or stops

Check for SDR/stereo input and select a supported codec, encoder and rate-control mode.
If **Encoder check could not finish** appears, close and reopen settings to retry.
This differs from an unsupported codec; you do not need to wait for the check to finish before closing settings.
Check free space and folder write permissions. For overload, lower recording resolution/rate limits.
Read the failure notice for save status. Keep files after an unexpected exit and check them in a compatible player.
See [recording support and file protection](RECORDING.md).

---

[README](../README.en.md) · [Documentation](INDEX.md) · [한국어](TROUBLESHOOTING.ko.md)
