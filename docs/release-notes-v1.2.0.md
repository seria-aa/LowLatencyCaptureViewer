## v1.2.0

### 한국어

- 설정 창을 **오디오**, **영상 · 창**, **안내 · 진단**, **업데이트** 탭으로 재구성해
  필요한 항목을 더 쉽게 찾을 수 있게 했습니다. 로그 폴더 열기, 현재 버전, 수동
  업데이트 확인도 설정 창에서 바로 사용할 수 있습니다.
- 새 설치의 권장 기본값은 1920 × 1080 캡처, 저지연 표시, 클록 드리프트 보정 자동,
  PCM 버퍼 목표 20ms, 업데이트 자동 확인 켬입니다. 기존에 저장된 설정은 유지됩니다.
- 전체화면 커서 옵션을 추가했습니다. 기본 **자동 숨김**은 2초 동안 입력이 없으면
  커서를 숨기고, 마우스 이동이나 휠 입력이 있으면 즉시 다시 표시합니다. 필요하면
  **항상 표시**를 선택할 수 있습니다.
- WASAPI Exclusive는 출력 장치별 실제 재생 이벤트 사전 검사를 통과한 경우에만
  선택할 수 있습니다. 결과는 `사용 가능 · n ms`, `사용 불가`, `검사 중`으로
  표시되고 장치 ID별로 저장됩니다.
- 전체화면 커서 타이머와 설정 UI는 캡처 콜백, PCM 큐, WASAPI 렌더 스레드, D3D11
  최신 프레임 표시 경로와 분리되어 있습니다. 정상 영상·오디오 처리에 새 프레임
  큐나 오디오 버퍼를 추가하지 않습니다.

> WASAPI Exclusive의 실제 이벤트 동작은 오디오 드라이버별로 다릅니다. 장치가
> `사용 가능`으로 표시되어도 문제가 있으면 WASAPI Shared 또는 ASIO를 사용하고
> 진단 로그를 첨부해 주세요. P010 HDR10은 계속 실험적 기능입니다.

### English

- Reorganized the settings window into **Audio**, **Video & window**, **Guide &
  diagnostics**, and **Updates** tabs. The settings UI now also provides direct
  access to the log folder, current version, and a manual update check.
- New installations default to 1920 × 1080 capture, low-latency presentation,
  automatic clock-drift correction, a 20 ms PCM target, and automatic update
  checks. Existing saved settings remain unchanged.
- Added a fullscreen cursor choice. The default **Auto-hide** mode hides the
  cursor after two seconds without input and immediately restores it on mouse
  movement or wheel input. **Always show** is also available.
- WASAPI Exclusive is offered only after the selected output endpoint passes a
  real playback-event preflight. Results are shown as `Available · n ms`,
  `Unavailable`, or `Checking` and are cached per endpoint ID.
- The fullscreen cursor timer and settings UI are separate from the capture
  callback, PCM queue, WASAPI render thread, and latest-frame D3D11 path. No
  frame queue or audio buffer is added to normal video or audio processing.

> WASAPI Exclusive event behavior varies by audio driver. If an endpoint marked
> `Available` still misbehaves, use WASAPI Shared or ASIO and attach a
> diagnostic log. P010 HDR10 remains experimental.
