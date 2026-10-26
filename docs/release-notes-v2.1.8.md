# LLCV v2.1.8

## 한국어

- **업스케일링 안정성:** NIS의 왼쪽·위쪽 가장자리 픽셀 오염을 수정했습니다. CMAA2 처리 목록의 범위 검사를 보완했으며, 극단적으로 조밀한 프레임이 버퍼 용량을 넘으면 그 프레임만 CMAA2를 생략하고 NIS 확대는 유지합니다.
- **UYVY/HDYC 입력:** 8비트 4:2:2 영상 재생과 원본 스크린샷을 지원합니다. 자동 선택은 기존 NV12·YUY2를 우선하며, UYVY/HDYC 녹화는 지원하지 않습니다.
- **지원 포맷 안내:** 지원하지 않는 픽셀 포맷만 제공되는 경우 빈 모드 목록과 구분해 설정 창에 안내하고, 상세 형식을 로그에 남깁니다.

[영상 설정](VIDEO.ko.md) · [업스케일링](USAGE.ko.md#cmaa2--nis-업스케일링) · [녹화 안내](RECORDING.ko.md)

## English

- **Upscaling stability:** fixed NIS pixel contamination at the left and top edges. CMAA2 now checks processing-list bounds; if an exceptionally dense frame exceeds buffer capacity, CMAA2 is skipped for that frame while NIS scaling continues.
- **UYVY/HDYC input:** adds 8-bit 4:2:2 playback and source screenshots. Auto selection still prefers NV12 and YUY2. Recording UYVY/HDYC is not supported.
- **Format diagnostics:** settings now distinguish unsupported-only pixel formats from empty mode lists, with detailed format information in the log.

[Video settings](VIDEO.md) · [Upscaling](USAGE.md#cmaa2--nis-upscaling) · [Recording guide](RECORDING.md)
