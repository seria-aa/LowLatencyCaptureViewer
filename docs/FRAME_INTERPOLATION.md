# Frame interpolation

[README](../README.en.md) · [User guide](USAGE.md) · [한국어](FRAME_INTERPOLATION.ko.md)

Frame interpolation inserts a generated frame between two captured frames. It smooths preview motion but does not increase the game's frame rate or improve input response. It does not change recordings or source screenshots.

| Method | Capture input | GPU and components |
| --- | --- | --- |
| NVIDIA VFG | FHD, QHD or 4K UHD SDR, 24–60 fps | NVIDIA Ada or Blackwell supported by the official Windows x64 runtime |

- Opt in under **Settings → Frame interpolation**. VFG works with standard low-latency output, VSync, and exclusive fullscreen. F7 toggles playback without clearing the checkbox. The last on/off choice survives reopening settings with F2 and restarting the app.
- With VSync, the display needs at least twice the capture rate (e.g. 30 fps at 60 Hz, or 60 fps at 120 Hz). If refresh or frame-admission timing is unavailable, playback uses original frames. Legacy Compatibility (Blt) output is not supported.
- Ada and Blackwell use NVIDIA's native path. RTX 30 (SM 8.6) is experimental exact-runtime compatibility, verified on RTX 3080. This is not official support or a guarantee for every RTX 30 model.
- **Tab diagnostics** reports generated and late jobs. LLCV displays an original frame when a job misses its deadline. High-resolution support does not guarantee doubled output at every rate.
- On first use, open **Settings → Frame interpolation → Download VFG components**. Confirm to download the pinned package (about 416 MiB) from the [NVIDIA package server](https://pypi.nvidia.com/nvidia-vfx/). LLCV verifies it and installs the required DLLs for the current Windows user. No sign-in or admin access is required. Enable NVIDIA VFG after installation. The base installer does not bundle NVIDIA DLLs or models.
- Playback works if the download or verification fails. Retry from the same button. Ordinary playback and F7 never start a download.
