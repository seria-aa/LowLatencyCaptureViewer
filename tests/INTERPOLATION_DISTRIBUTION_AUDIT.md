# 2.3.0 frame interpolation distribution audit

Status: do not add either tested provider's binaries to the **public** installer
or ZIP. This is a release gate, not a conclusion that these components can
never be licensed for distribution.

| Component | Verified evidence | Decision |
| --- | --- | --- |
| RIFE 4.25 model | [Practical-RIFE's trained-model section](https://github.com/hzwer/Practical-RIFE#trained-model) explicitly places the linked 4.25 weights under MIT. Our ONNX is a conversion from the [vs-mlrt external-models release](https://github.com/AmusementClub/vs-mlrt/releases/tag/external-models). The local source archive SHA-256 is `172FE975C1775134BB87108E4EC6D1A89E861CC5D3BE2AC23BF08AFE5ED626B8`, matching the existing conversion record. | The original weights are not the licensing blocker. Retain the MIT notice and verify the exact conversion, engine and plugin source chain; these engines are not qualified as universal across GPU models. |
| TensorRT 10.6.0.post2 | The tested `nvinfer_10.dll` matches the runtime from the local NVIDIA wheel (`2B0DDC1ABD6283D431D0A793FF43594FD101F0BE9B9E36CD5EDCC5A5FB519CB5`). Its **own** NVIDIA SLA §12.1 conditionally permits the named runtime libraries as part of an application; it is not the Apache license for the public headers. The agreement limits recipients' further distribution and prohibits making the NVIDIA software subject to an open-source license (§2.1). | Do not assume it can ship under LLCV's GPL. Confirm the applicable NVIDIA grant and the GPL linking/distribution basis before bundling. |
| CUDA runtime | NVIDIA's [CUDA 12.6 redistribution terms](https://docs.nvidia.com/cuda/archive/12.6.3/eula/index.html#attachment-a) list the toolkit runtime under conditions. The preview `cudart64_12.dll` (`663B5FE65AA6C654B8B085A879F49DEB19F2CB033B8F2FF8DFFD06CDEFE0D1DB`) is byte-identical to the VFG wheel's copy, **not** the CUDA 12.6 Toolkit installation's copy (`3DF50CF48718712F17B8C77AD72C45A1F43FF94781E302A4BE7F1D7A9B66C746`). | For a later RIFE bundle, qualify a documented toolkit redistributable with its own notices and retest. Do not copy the preview runtime under a different license assumption or include the driver. |
| VFG | The exact Windows wheel is [`nvidia-vfx 0.2.0.0`](https://pypi.org/project/nvidia-vfx/0.2.0.0/) (proprietary, alpha; SHA-256 `5AAF6A42BC6B6DBBF52FCB714194C994A6893CBBF7ADA38BC2165A1F83E4A6FC`). The bundled May 2025 SLA and AI terms limit distribution to the applicable grant and exclude developer-program software from production distribution. The accompanying model license does not identify the VFG binary as redistributable. A legacy VFX SDK redistributable guide does not establish rights for these current wheel DLLs. | Keep the four proprietary VFG DLLs out. Obtain NVIDIA confirmation that the exact version may be bundled with this type of Windows desktop app, then qualify official VFG on supported hardware. |

## Release decision

LLCV is GPLv3-or-later. Do not assume that a delay-loaded NVIDIA runtime is a
GPL System Library or that separate downloads alone settle release
compatibility. See [GNU GPL FAQ](https://www.gnu.org/licenses/gpl-faq.en.html#GPLIncompatibleLibs).
No applicable linking exception has been verified. Only the necessary rights
holders can grant one. Confirm both license sides, or use a GPL-compatible
runtime for the public RIFE path and qualify it again. Do not relicense any
existing source or agree to vendor terms on anyone's behalf.

The current public package allowlist remains unchanged. Copying only RIFE
models, CUDA or support notices would not give the user a working
interpolation feature, so do not add a partial payload. The candidate's
local previews and previous smoke-test runtimes are not public assets.

NVIDIA provides a separate [NGC feature bundle](https://catalog.ngc.nvidia.com/orgs/nvidia/maxine/collections/nvvfxvideoframegeneration/-)
and a [Windows SDK installation guide](https://docs.nvidia.com/maxine/vfx/latest/WindowsVFXSDK/InstalltheVFXSDK.html).
The [SDK Core download](https://catalog.ngc.nvidia.com/orgs/nvidia/maxine/resources/vfx_sdk_core/-)
is gated by NVIDIA access requirements; it is not an anonymous consumer add-on.
This is a vendor distribution path, not a verified LLCV installation path or a
grant to repackage the wheel runtime. LLCV currently looks for app-local
`vfg/` DLLs and `nvidia-rife/cudart64_12.dll`; it does not discover NGC's SDK
feature layout. Match the runtime version, establish a supported setup flow,
and validate native VFG initialization before publishing an enablement guide.
