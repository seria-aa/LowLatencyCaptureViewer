## v1.2.8

### 한국어

- **P010 HDR10 색 정보 처리**를 보완했습니다. 실제 연결된 캡처 형식의 색공간,
  전송 특성, 색상 범위와 크로마 위치를 확인해 HDR 출력에 반영합니다.
- **HDR 화면의 OSD 밝기와 합성**을 개선했습니다. 실시간 정보창과 볼륨 표시를
  HDR 영상에 맞는 선형 밝기 기준으로 합성하고 Windows의 SDR 흰색 밝기를 반영합니다.
- HDR 출력 지원 여부와 표시 모니터의 HDR 상태 확인을 보강하고, 관련 정보를
  실시간 정보창과 진단 로그에서 확인할 수 있도록 했습니다.
- HDR 색 변환·밝기·OSD 합성 자동 검사를 추가했습니다. 기존 오디오 처리와
  기본 설정은 유지하며, 새로운 영상 프레임 큐나 외부 런타임 의존성을 추가하지 않았습니다.

> P010 HDR10은 계속 **실험적 지원**입니다. HDR 표시에는 호환되는 캡처 장치와
> GPU·모니터 및 Windows HDR 활성화가 필요합니다. 자동·로컬 GPU 검사는 수행했으며,
> 실제 HDR 디스플레이를 통한 시각 검증은 수행하지 않았습니다.

### English

- Refined **P010 HDR10 color handling**, using the connected capture format's
  color space, transfer characteristics, range and chroma placement for HDR output.
- Improved **OSD brightness and compositing in HDR**. Diagnostics and volume
  overlays are blended in linear light and use the Windows SDR white level.
- Expanded HDR output-capability and display-state checks, with details available
  in the on-screen diagnostics and diagnostic log.
- Added automated HDR color, luminance and overlay-compositing checks. Existing
  audio processing and defaults are retained, with no new video-frame queue or
  external runtime dependency.

> P010 HDR10 remains **experimental**. A compatible capture device, GPU and HDR
> display with Windows HDR enabled are required. Automated and local GPU checks
> were performed; visual validation on a physical HDR display was not performed.
