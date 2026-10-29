# LLCV v2.1.9

## 한국어

- **RGB 입력 지원:** 장치가 제공하는 RGB24·XRGB (RGB32) SDR 영상의 재생과 원본 PNG 스크린샷을 지원합니다. 자동 선택 순서는 그대로이며, RGB는 직접 선택합니다. NVIDIA VSR 요청과 SDR·스테레오 녹화도 지원합니다.
- **RGB 녹화 안전장치:** 녹화 파일은 BT.709 Limited NV12(4:2:0)로 변환해 저장합니다. 시작 전 변환 속도를 확인하고, 처리 여유가 부족하면 파일을 만들지 않고 해상도·프레임 상한 조정을 안내합니다. 선택한 FPS를 임의로 낮추지 않으며, RGB 프레임 버퍼 메모리를 제한합니다.
- **진단·검증 보완:** 입력 형식의 헤더·채널 마스크·행 간격 등 거부 원인을 상세히 기록합니다. 강제 종료 복구 검증에서 초기화 중인 마지막 파일과 손상 파일을 구분하도록 보완하고, 개발용 RGB 검증 문서는 배포본에서 제외했습니다.

RGB는 SDR 전용입니다. VSR의 실제 효과는 GPU·드라이버·입력 형식에 따라 달라질 수 있습니다. 기본 녹화 상한은 기존과 같은 1080p·60fps입니다.

[영상 설정](VIDEO.ko.md) · [녹화 안내](RECORDING.ko.md)

## English

- **RGB input:** adds device-advertised RGB24 and XRGB (RGB32) SDR playback and source PNG screenshots. RGB is selected manually; Auto priority is unchanged. NVIDIA VSR requests and SDR/stereo recording are also supported.
- **RGB recording safeguards:** converts recordings to BT.709 Limited NV12 (4:2:0). A worker-side conversion check runs before file creation; insufficient headroom prompts lower recording resolution/FPS caps without silently changing the selected FPS. RGB frame-buffer memory is bounded.
- **Diagnostics and verification:** logs specific input-layout rejection reasons, including header, channel-mask and row-layout issues. Crash-recovery verification now distinguishes an initializing final file from malformed files. The developer-only RGB validation report is excluded from packages.

RGB is SDR-only. Actual VSR effects depend on GPU, driver and input format. Default recording caps remain 1080p at 60 fps.

[Video settings](VIDEO.md) · [Recording guide](RECORDING.md)
