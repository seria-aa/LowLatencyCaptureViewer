## v1.2.11

### 한국어

- NV12·YUY2 영상에서 캡처 장치가 제공하는 BT.601/BT.709 색 정보와 Full/Limited 범위를 반영하도록 개선했습니다.
- SDR 영상의 GPU 자동 보정을 비활성화하여 일관된 화면 표시를 지원합니다.
- Sharp는 화면을 확대·축소할 때 적용하고, 1:1 표시에서는 원본 픽셀을 유지하도록 다듬었습니다.
- 기존 저지연 처리 구조를 유지하며, 추가 프레임 큐나 외부 코덱·런타임 없이 개선했습니다.

### English

- Improved NV12/YUY2 rendering to use device-provided BT.601/BT.709 color metadata and Full/Limited range information.
- Disabled automatic GPU enhancements for consistent SDR presentation.
- Refined Sharp processing to apply when scaling while preserving unsharpened 1:1 presentation.
- Retained the low-latency pipeline without adding frame queues, external codecs, or runtimes.
