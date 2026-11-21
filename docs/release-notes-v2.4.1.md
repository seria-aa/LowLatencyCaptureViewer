# LLCV v2.4.1

- **RIFE 시작 안정화:** 보간을 켜면 모델을 백그라운드에서 준비하고, 그동안 원본 영상을 계속 재생합니다.
- **늦은 작업 처리:** GPU 추론을 시작하기 전에 이미 기한이 지난 작업은 이력만 갱신하고 원본을 표시합니다. 이전 보간 프레임을 다시 표시하지 않습니다.
- **장치 오류 복구:** 초기화 중 장치가 유실된 RIFE는 재시도할 때 새 Vulkan 장치를 사용합니다. NVIDIA VFG는 GPU 장치 제거를 보간 완료로 처리하지 않습니다.
- **HUD 보호 및 진단:** HUD 보호의 Vulkan 동기화를 바로잡고, 로그에서 기한 내 완료와 화면 제출을 구분합니다.

지원 해상도·픽셀 포맷과 구성품 설치 방식은 v2.4.0과 같습니다. [프레임 보간 안내](FRAME_INTERPOLATION.ko.md)

## English

- **RIFE startup:** Original frames keep playing while the model loads in the background.
- **Late jobs:** If the deadline has already passed before inference starts, RIFE updates only its history and shows the original frame instead of reusing an old interpolation result.
- **GPU recovery:** On retry, RIFE creates a new Vulkan device if the previous startup lost its device. NVIDIA VFG no longer treats device removal as a completed interpolation job.
- **HUD protection and diagnostics:** Corrected Vulkan synchronization for HUD protection. Logs distinguish on-time results from frames submitted for display.

Supported resolutions, pixel formats, and component installation are unchanged from v2.4.0. [Frame interpolation guide](FRAME_INTERPOLATION.md)
