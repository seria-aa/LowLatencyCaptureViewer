# Low-latency audio

> [한국어](AUDIO.ko.md) · [Back to README](../README.md)

## Capture audio selection

Automatic selection first looks for a 48 kHz stereo PCM audio pin inside the
selected video capture filter. If a USB device exposes audio as a separate
DirectShow input, the viewer tries to match it by name. Select the corresponding
audio input manually when automatic matching is not correct.

Video and audio from separate filters can use slightly different hardware
clocks. Over a long session this can move the PCM queue away from its target.
Clock-drift correction is normally unnecessary for a device with a shared
clock, but may be useful for separate USB video and audio paths.

## Audio-only mode

Enable **Audio-only mode** to skip the video pin, D3D11 renderer, and video
presentation path. The app runs only the capture-audio-to-selected-output path. If audio
is exposed on the video filter, only that audio pin is connected; when a
separate capture-audio device is selected, only its DirectShow audio filter is
used. WASAPI mode, PCM target, and clock-drift correction remain available with
the same behavior as normal mode. The window shows the same L/R peak, dBFS,
channel/master volume, and clipping OSD as the video viewer.

## WASAPI Shared and Exclusive

In this release, WASAPI Exclusive is temporarily hidden from the settings UI
while that backend is being investigated. Existing Exclusive profiles are
migrated to WASAPI Shared on the next run. The Exclusive notes below are kept
for reference if the backend is re-enabled later.

**WASAPI Shared** allows other applications to use the output device and keeps
Windows shared-mode effects available. It is the recommended default.

When the endpoint supports `IAudioClient3`, the viewer reads its valid shared
engine periods and starts an event-driven stream with the closest supported
low-latency period through `InitializeSharedAudioStream`. The settings window
shows the supported range and the Tab overlay reports the active period.

If `IAudioClient3` is unavailable for the endpoint or format, the viewer falls
back automatically to classic WASAPI Shared. This is a compatibility fallback,
not an initialization failure.

**WASAPI Exclusive** opens the output endpoint directly. It can reduce the
shared-mixer path, but other applications may be unable to use that endpoint at
the same time. Shared-mode APO/DSP processing such as Equalizer APO, HeSuVi, or
Windows spatial effects may also be bypassed. Use Shared when those effects or
general compatibility are required.

## ASIO (experimental)

ASIO appears in the settings only when an installed ASIO driver is detected.
Driver DLLs are not bundled; the selected driver's own buffer size is used. The
capture path supplies 48 kHz PCM, so a driver that cannot run at 48 kHz is
rejected and that run falls back to WASAPI Shared. This prototype follows the
ASIO driver's output clock, but it supports the same app-side Off/Auto/On
clock-drift correction choices as WASAPI. When enabled, the resampler adjusts
the capture PCM rate without adding a separate queue. Use WASAPI Shared when
Windows APO/DSP effects are required.

## Output-device switching

With **Follow the Windows default output device**, changing the Windows default
while the viewer is running reopens only WASAPI output. Video capture remains
running. Pending PCM is cleared so that endpoint initialization time does not
become accumulated playback latency; a short silence or small click can occur
during the switch.

Choose a fixed endpoint when the viewer should remain on one device regardless
of Windows default-device changes.

## WASAPI buffer and PCM target

These controls affect different parts of the path:

- **WASAPI output buffer** requests the endpoint's output period or buffer.
- **PCM buffer target** is the application's capture-to-render safety depth.
- **Clock-drift correction** changes the long-run sample-rate relationship; it
  does not replace either buffer setting.

Start with a 10 ms PCM target. Increase it to 15, 20, or 30 ms only when
underruns repeat. A larger target improves scheduling tolerance but adds the
same amount of audio queueing. A single occasional underrun is not necessarily
audible or a reason to raise the target.

The DirectShow allocator reported in the log is controlled partly by the
capture driver. Its block size or buffer count does not necessarily equal the
amount of audio queued by this application.

## Clock-drift correction

**Off** leaves PCM samples unaltered. **Auto** watches the PCM queue and starts
correction only after an imbalance has remained for five seconds; once started,
it stays on for that session instead of repeatedly toggling. **On** uses the
resampler from startup. Choose Auto when a long-running session shows repeated
breakup or a persistent, growing PCM queue imbalance—especially when video and
audio use separate capture filters.

Correction makes small continuous rate adjustments to keep the PCM queue near
its target. It does not intentionally add another frame or packet queue. Use the
Tab overlay for 10–30 minutes before deciding from the estimated ppm value;
short startup or scheduling disturbances can distort the estimate.

## Volume and background mute

The mouse wheel changes volume in 5% steps. At 100%, PCM volume processing is
bypassed. **Allow volume boost above 100%** permits up to 200% digital gain
without adding an audio buffer, but loud signals may clip.

To adjust channels independently, press `F3` to show the audio meter OSD and
hover the pointer over the **L** or **R** card while scrolling. Only the
hovered channel changes, in 5% steps. Scrolling over the master row or outside
the channel cards changes the master volume. Independent L/R gain remains
limited to 100%; the optional 200% boost applies only to the master volume.

Background auto-mute changes output gain only. Capture and WASAPI consumption
continue while the window is inactive, preventing accumulated PCM latency when
the viewer becomes active again.
