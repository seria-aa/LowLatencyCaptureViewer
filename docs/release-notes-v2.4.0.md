# LLCV v2.4.0

- **RIFE Vulkan 프레임 보간:** 저지연 출력에서 FHD SDR 입력을 보간합니다. Fast 모델을 동봉하고 선택한 SDR 픽셀 포맷을 유지합니다. 영상 탭에서 자동 또는 장치가 지원하는 24~60fps를 선택할 수 있습니다. P010 HDR은 보간하지 않습니다.
- **보간 방식 선택:** RIFE Vulkan과 NVIDIA VFG 중에서 선택합니다. 새 설정에서는 보간이 꺼져 있고, 기본 방식은 RIFE Vulkan입니다. 기존 VFG 사용자의 설정은 유지되며, VFG 구성품은 기존처럼 따로 받습니다.
- **RIFE HUD 보호:** 기본으로 밝기와 색 윤곽을 보호해 하트나 글자 등 고정된 화면 요소가 보간 중 흐트러지는 현상을 줄입니다.

[프레임 보간 안내](FRAME_INTERPOLATION.ko.md)

## English

- **RIFE Vulkan frame interpolation:** Interpolates FHD SDR in low-latency output. The Fast model is bundled, and the selected SDR pixel format is kept. The Video tab offers Auto or a supported capture rate from 24 to 60 fps. P010 HDR is not interpolated.
- **Interpolation method:** Choose RIFE Vulkan or NVIDIA VFG. Interpolation is off in new profiles, with RIFE Vulkan selected by default. Existing VFG preferences are preserved, and VFG components remain a separate download.
- **RIFE HUD protection:** Protects luma and color edges by default to reduce interpolation artifacts on fixed elements such as hearts and text.

[Frame interpolation guide](FRAME_INTERPOLATION.md)
