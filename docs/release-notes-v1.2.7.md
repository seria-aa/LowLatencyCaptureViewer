## v1.2.7

### 한국어

- **시작할 표시 모니터 선택**을 추가했습니다. 영상·창 탭에서 지정할 수 있으며,
  선택한 모니터가 연결되어 있지 않으면 주 모니터에서 시작합니다. 실행 후 창 이동은
  자유롭고, 기본 자동 모드는 마지막 위치를 복원합니다.
- 듀얼 모니터 이동, 서로 다른 화면 배율, 창 크기 조절 및 전체화면 전환 처리를
  보완했습니다. 중복 출력 갱신을 줄이고, 사용자가 조절한 크기가 이전 비율로
  되돌아가거나 경계에서 창 위치·크기가 흔들리는 조건을 수정했습니다.
- **호환성 출력(Blt · VSync)**을 추가했습니다. 기존 저지연과 VSync는 Flip 경로를
  유지하며, 두 모드에서 모두 표시 문제가 생길 때 Blt 경로를 비교할 수 있습니다.
  호환성 출력은 지연·GPU 부하가 늘 수 있고 **HDR10 출력은 지원하지 않습니다**.
  VSync 설명도 찢어짐 완화 중심으로 구분했습니다.
- 캡처 형식 조회 실패와 미지원 상태를 구분하고, 장치가 보고한 프레임 간격 범위의
  일반적인 FPS도 후보에 포함합니다. 실제 사용 가능 여부는 장치와의 형식 협상으로
  확인하며, 잘못된 후보는 제외하고 정상 대체 후보를 시도합니다.
- 캡처 시작 시 해상도·픽셀 포맷·프레임 데이터 크기 검증을 보강했습니다.
  첫 입력 대기는 3초에서 10초로 늘리고, 입력 도착과 화면 표시를 따로 추적하여
  창이 가려진 상태를 입력 실패로 오인하지 않도록 했습니다. 재생 버퍼를 10초로
  늘리는 변경은 아닙니다.
- 설정 화면·번역·출력 전환 상태를 모듈로 정리하고 회귀 검사를 확대했습니다.
  기존 오디오 처리와 PCM 버퍼 기본값, 기본 저지연 출력은 유지합니다.

> 자동·가상 회귀 검사와 로컬 GPU 검사를 수행했습니다. 실제 장치 조합별 장시간
> 사용 검증을 대신하지는 않습니다.

### English

- Added a **startup display monitor** selector in Video & window. Missing selected
  monitors fall back to the primary display. Windows remain freely movable after
  startup; the default Auto option restores the last position.
- Hardened dual-monitor moves, mixed-DPI transitions, manual resizing and
  fullscreen transitions. Coalesced redundant output updates and fixed conditions
  that restored an old size ratio or shifted window geometry at monitor boundaries.
- Added **Compatibility (Blt + VSync)** as an alternative when both existing Flip
  modes have display problems. It may increase latency/GPU load and **does not
  support HDR10 output**. VSync wording now focuses on reduced tearing.
- Distinguished capture-capability query failures from unsupported modes. Common
  frame rates within driver-reported intervals are considered, but actual format
  negotiation still determines acceptance. Invalid candidates are skipped before
  trying valid alternatives.
- Strengthened startup validation of resolution, pixel format and frame layout.
  Increased the first-input allowance from 3 to 10 seconds and separated input
  arrival from successful presentation so occluded windows do not look like
  missing input. This is not a 10-second playback buffer.
- Modularized settings views, translations and output-transition state, with
  expanded regression checks. Existing audio processing, the PCM buffer default,
  and default low-latency presentation remain unchanged.

> Automated/simulated regression checks and local GPU checks were performed.
> These do not replace long-session validation on each physical-device setup.
