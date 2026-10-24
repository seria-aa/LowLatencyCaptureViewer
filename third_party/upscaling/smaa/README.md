# SMAA 1x — private quality comparison

Unmodified official source and precomputed byte tables from
[iryoku/smaa](https://github.com/iryoku/smaa), pinned commit
`71c806a838bdd7d517df19192a20f0c61b3ca29d`.
The upstream MIT-style license is retained in `LICENSE.txt` and each source.

- `SMAA.hlsl`: official three-pass shader math.
- `AreaTex.h`: 160x560 R8G8_UNORM area table, 320-byte row pitch.
- `SearchTex.h`: 64x16 R8_UNORM packed search table, 64-byte row pitch.

`src/video/shaders/Smaa.hlsl` supplies explicit D3D11 resource bindings through
the official custom shader-language interface; no filter math/LUT edits.
Preset HIGH, luma edges, SMAA 1x (zero subsample indices), no temporal history,
predication, jitter or reprojection. Edge and blend targets are cleared every
frame. RGB neighborhood blending uses the documented non-sRGB/gamma-space
path. Color intermediates are FP16, edge/weight targets RGBA8.

HIGH/luma is used before NIS at native FHD and after NIS at QHD.
The extended comparison also offers ULTRA/color after NIS, with the same
gamma-space blending and official LUTs.
All files are private-prototype-only; nothing is added to normal packages.
