## 한국어

### v1.0.6.1 Beta

- MJPEG, H.264/AVC, MPEG-4 캡처 포맷을 Windows Media Foundation으로
  디코딩하는 **실험적 호환 모드**를 추가했습니다.
- 이 모드는 픽셀 포맷에서 직접 선택해야 하며, 검증된 NV12/YUY2 저지연 경로를
  자동으로 대체하지 않습니다.
- 장치가 내보내는 비트스트림·시퀀스 헤더·Windows 디코더에 따라 동작이 달라질 수
  있으므로, 문제가 있으면 NV12/YUY2를 사용하고 진단 로그를 첨부해 주세요.
- 설정에 `100% 이상 볼륨 증폭 허용 (최대 200%)`을 추가했습니다. 100% 초과 시
  앱 내부 디지털 증폭만 적용하며, 추가 오디오 버퍼나 프레임 큐는 만들지 않습니다.
  원본 음량이 큰 경우 왜곡될 수 있습니다.
- 설정 창 좌측 하단에 현재 버전 워터마크를 표시합니다.
- Graphite + Coral 스타일의 새 앱 아이콘을 적용했습니다.

Assets: installer, portable x64 ZIP, and source ZIP.

## English

### v1.0.6.1 Beta

- Added **experimental compatibility modes** for MJPEG, H.264/AVC, and
  MPEG-4 capture formats. They are decoded through Windows Media Foundation.
- These modes are opt-in in the pixel-format selector; they do not replace the
  tested NV12/YUY2 low-latency path selected by Auto mode.
- Actual compatibility depends on each device's bitstream, sequence headers,
  and available Windows decoder. Use NV12/YUY2 and attach diagnostics if an
  experimental format does not work.
- Added an off-by-default `Allow volume boost above 100% (up to 200%)` option.
  It applies in-app digital gain without adding an audio buffer or frame queue;
  loud source material can clip.
- The lower-left corner of the settings dialog now shows the current version.
- Applied a new Graphite + Coral application icon.

Assets: installer, portable x64 ZIP, and source ZIP.
