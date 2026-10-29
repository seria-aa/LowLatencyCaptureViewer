## v2.0.1

### 한국어

VSR 적용 범위를 넓히고, 창 외형과 OSD 갱신을 다듬은 업데이트입니다.

#### NVIDIA VSR — 실험적

- 기존 NV12/MJPEG SDR에 더해 **YUY2 SDR과 P010 HDR10**에서도 VSR 보정을 요청할 수 있습니다.
- HDR에서는 기존 **10비트 BT.2020/PQ 출력**을 유지합니다. SDR 중간 변환이나 RTX Video HDR(SDR→HDR 변환)을 추가하지 않습니다.
- YUY2는 별도의 앱 내 변환 패스 없이 기존 영상 처리기에 직접 전달합니다.
- Intel/AMD 렌더링 GPU에서는 VSR 설정과 F6을 비활성화하고 이유를 표시합니다. GPU 확인 실패도 별도로 안내합니다.
- GPU 확인은 설정 화면이 뜨기 전 시작 시 한 번 수행하며, 탭 전환이나 재생 중 반복 검사하지 않습니다.
- VSR 처리 실패 시 효과를 끄고 같은 프레임을 한 번 다시 처리하는 경로를 보완했습니다.
- VSR 알림과 Tab 상태를 **ON/OFF** 중심으로 간결하게 표시합니다. ON은 앱에서 기능을 켰다는 뜻이며, 드라이버의 실제 활성 여부를 검출했다는 뜻은 아닙니다.

입력보다 가로나 세로가 작게 표시되는 경우에는 VSR을 적용하지 않습니다.
실제 효과는 NVIDIA GPU·드라이버·설정에 따라 달라지며, NVIDIA App/제어판의
설정과 활성 표시를 확인해야 합니다. VSR은 GPU 부하와 지연을 늘릴 수 있습니다.
F6은 캡처 재시작 없이 효과만 전환하고, 스크린샷은 계속 VSR 적용 전 입력 영상을 저장합니다.

#### 창과 OSD

- **창 → 둥근 모서리 (Windows 11)** 옵션을 추가했습니다. 영상·오디오 전용 창에 함께 적용하며 기본값은 켜짐입니다.
- 창 모드와 보더리스 창에 같은 시스템 곡률을 적용합니다. 일반 창의 테두리는 유지하고, 둥근 모서리 옵션을 켠 보더리스 창에서만 DWM 외곽선을 숨깁니다. 전체화면·최대화에서는 네모난 모서리를 유지합니다.
- 보이지 않는 진단 패널과 일시 알림의 불필요한 텍스트 배치·그리기 작업을 줄였습니다. 다시 표시할 때는 최신 내용으로 갱신합니다.

기존 최신 프레임 우선 처리와 오디오 기본값을 유지하며, 추가 프레임 큐나
ON/OFF 지연을 맞추기 위한 대기는 넣지 않았습니다. 둥근 모서리 적용 여부는 Windows 환경에 따라 달라질 수 있습니다.
자세한 사용 조건은 [한국어 README](../README.ko.md)를 참고하세요.

### English

This update expands VSR input support and refines viewer windows and overlay updates.

#### NVIDIA VSR — experimental

- Added VSR requests for **YUY2 SDR and P010 HDR10**, alongside NV12/MJPEG SDR.
- Preserved native **10-bit BT.2020/PQ output** for HDR, without an SDR intermediate or RTX Video HDR conversion.
- YUY2 goes directly through the existing video processor without an extra app-side conversion pass.
- Disabled VSR settings and F6 on Intel/AMD rendering adapters, with a reason shown. Adapter identification failures have a separate message.
- Adapter identification runs once before startup settings, not on tab changes or per frame.
- Refined fallback to disable VSR and retry the same frame once if VSR frame processing fails.
- Simplified notifications and Tab status to **ON/OFF**. ON means enabled in the viewer, not independently confirmed driver activation.

VSR is bypassed when either displayed dimension is smaller than the input.
Actual enhancement depends on the NVIDIA GPU, driver and settings; check the
setting and active indicator in NVIDIA App/Control Panel. VSR may increase GPU
use and latency. F6 changes only the effect without restarting capture, and
screenshots continue to save the pre-VSR input image.

#### Windows and overlays

- Added **Window → Rounded corners (Windows 11)**, enabled by default for both video and audio-only viewers.
- Uses the same system corner radius for restored regular/borderless windows. Regular windows keep their system border; only borderless windows hide the DWM outline while rounding is enabled. Fullscreen and maximized windows remain square.
- Reduced unnecessary text layout and painting for hidden diagnostics and transient notifications. Panels refresh when shown again.

Retains latest-frame-first playback and existing audio defaults, with no added
frame queue or delay to equalize ON/OFF latency. Windows may suppress rounded
corners in some environments. See the [English README](../README.en.md) for requirements.
