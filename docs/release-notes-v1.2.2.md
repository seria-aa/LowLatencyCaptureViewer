## v1.2.2

### 한국어

- 선택한 캡처 장치와 해상도가 제공한다고 알린 NV12, YUY2, P010, MJPEG를 픽셀
  포맷 목록과 자동 인식 정보에 모두 표시합니다.
- 자동 선택은 기존과 동일하게 저지연 비압축 경로인 NV12를 먼저, 그다음 YUY2를
  사용합니다. 실험적 MJPEG는 자동으로 선택하지 않으며 사용자가 직접 선택해야
  합니다.
- 저가형·USB 대역폭 제약 장치가 비압축 포맷을 지원한다고 신고하지만 실제 전송이
  불안정한 경우 MJPEG를 직접 비교할 수 있습니다.
- 캡처, 디코딩, D3D11 표시와 오디오 처리 경로에는 변경이 없습니다.

### English

- The pixel-format selector and detected-capability summary now show every
  NV12, YUY2, P010, and MJPEG format reported by the selected capture device at
  the selected resolution.
- Auto selection is unchanged: it prefers the tested low-latency raw NV12 path,
  followed by YUY2. Experimental MJPEG remains opt-in and is never selected
  automatically.
- This makes it possible to compare MJPEG manually on low-cost or
  bandwidth-constrained USB devices that advertise raw modes but do not deliver
  them reliably in practice.
- The capture, decoding, D3D11 presentation, and audio processing paths are
  unchanged.
