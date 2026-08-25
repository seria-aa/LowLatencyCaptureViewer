## v1.2.3

### 한국어

- MJPEG 디코더가 제공하는 색 범위와 BT.601/709 행렬 정보를 자동으로 적용합니다.
- 디코더 정보가 없으면 DirectShow 확장 색상 정보를 사용하며, 양쪽 모두 없으면
  JPEG 관례에 따라 Full range로 처리합니다. 행렬 정보가 없을 때는 HD 입력에
  BT.709, SD 입력에 BT.601을 사용합니다.
- Tab 정보창과 진단 로그에 실제 적용한 MJPEG 색상 설정과 판정 출처를 표시합니다.
- MJPEG를 직접 선택하면 **MJPEG 색상 해석** 옵션이 나타나며, 자동 판정이 맞지
  않는 장치에서 BT.709/BT.601과 Full/Limited 조합을 수동 지정할 수 있습니다.
  P010 선택 시에는 기존 HDR10 강제 옵션이, NV12/YUY2 선택 시에는 둘 다
  나타나지 않습니다.
- NV12, YUY2, P010, 오디오와 자동 비압축 포맷 선택 경로는 변경하지 않았습니다.

### English

- MJPEG now applies the color range and BT.601/709 matrix reported by the
  decoder automatically.
- DirectShow extended color information is used when decoder metadata is
  absent. If neither source identifies the range, the viewer follows JPEG full
  range; matrix fallback is BT.709 for HD and BT.601 for SD.
- The Tab overlay and diagnostic log show the applied MJPEG color setting and
  its detection source.
- Explicitly selecting MJPEG reveals **MJPEG color interpretation**, allowing
  manual BT.709/BT.601 and Full/Limited combinations for devices whose metadata
  is wrong. P010 continues to show the HDR10 force option; NV12/YUY2 show
  neither control.
- NV12, YUY2, P010, audio, and automatic raw-format selection are unchanged.
