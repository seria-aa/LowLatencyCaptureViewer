## v1.2.9

### 한국어

- P010 선택 시 **HDR 색차 배치** 옵션을 추가했습니다. 기본값은 **자동(권장)**이며,
  필요할 때 **Top-left / Left** 호환성 해석을 직접 선택할 수 있습니다.
- HDR에서 **Tab 정보창 배경을 검정·불투명도 90%**로 조정했습니다.
  글자 밝기 기준과 SDR 정보창의 기존 모양은 유지합니다.
- 색차 배치 설정 저장·복원, HDR 색 정보 판정과 정보창 합성 검사를 보강했습니다.
  기존 오디오 처리와 기본 설정을 유지하며, 새로운 영상 프레임 큐나 외부 런타임을 추가하지 않았습니다.

> P010 HDR10은 계속 **실험적 지원**입니다. 수동 색차 배치는 장치가 제공하는
> 정보의 해석을 바꾸는 옵션이며, 모든 장치의 실제 배치를 지원한다는 의미는 아닙니다.
> 수동 선택 후에는 가는 색 경계와 글자를 기준 화면과 비교해 주세요.

### English

- Added **HDR chroma placement** for P010: **Auto (recommended)** remains the
  default, with optional **Top-left / Left** compatibility interpretations.
- Set the HDR **Tab diagnostics background to black at 90% opacity**.
  Text luminance handling and the existing SDR panel appearance are retained.
- Expanded checks for placement settings, HDR color interpretation and overlay
  compositing. Existing audio processing and defaults remain unchanged, with
  no new video-frame queue or external runtime dependency.

> P010 HDR10 remains **experimental**. Manual chroma placement overrides the
> interpretation of device metadata; it does not implement every actual chroma
> layout. Compare fine colored edges and text against a reference after selecting
> a manual interpretation.
