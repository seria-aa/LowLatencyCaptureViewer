## 한국어

### v1.0.6.2 Beta

- 시작 설정을 기본 화면과 `고급 설정`으로 정리했습니다. 오디오 출력, 백그라운드
  자동 음소거, 언어, 바로 시작, 캡처 장치·해상도·포맷·프레임, Pixel-perfect,
  상대적 창 크기, borderless 창은 기본 화면에서 바로 조정할 수 있습니다.
- 선택한 영상 DirectShow 필터의 내부 오디오 출력 핀을 설정창에서 사전 확인합니다.
  내부 핀이 있으면 자동 사용 상태를 표시하고, 별도 USB 오디오 필터가 필요한
  경우에는 캡처 오디오 장치 선택기를 표시합니다. 별도 장치를 직접 선택한 설정은
  그대로 유지됩니다.
- 이 사전 확인은 핀을 열거할 뿐 캡처 그래프를 연결하거나 실행하지 않습니다. 영상
  프레임 큐, DirectShow 콜백, PCM 버퍼, WASAPI 렌더 경로에는 추가 작업이 없으므로
  실행 중 표시/오디오 지연을 늘리지 않습니다.
- NV12 또는 YUY2가 제공되는 장치·해상도에서는 MJPEG/H.264/MPEG-4 베타 포맷을
  목록에서 숨겨 검증된 원시 저지연 경로를 우선합니다. 원시 포맷이 없는 경우에만
  압축 호환 모드가 나타납니다.
- 창 동작과 진단 옵션의 레이아웃을 정리하고, 설정을 접거나 펼칠 때 화면을 다시
  그려 텍스트가 겹쳐 보일 수 있던 문제를 보완했습니다.

Assets: installer, portable x64 ZIP, and source ZIP.

## English

### v1.0.6.2 Beta

- Reorganized startup settings into a compact essential view and an expandable
  **Advanced settings** view. Common audio, display, window behavior, language,
  and direct-start choices remain immediately available.
- The selected DirectShow video filter is preflighted in the settings dialog
  for a built-in audio output pin. Automatic mode reports built-in audio when
  present; otherwise it shows the separate capture-audio selector. A manually
  selected separate audio device is preserved.
- This preflight only enumerates filter pins; it neither connects nor runs a
  capture graph. It adds no work to the present loop, DirectShow callback, PCM
  ring, or WASAPI render path, and therefore adds no steady-state latency.
- When NV12 or YUY2 is available for the selected device and resolution, beta
  MJPEG/H.264/MPEG-4 choices are hidden to make the tested raw low-latency path
  the clear default. Compressed compatibility modes appear only when raw modes
  are unavailable.
- Refined window-behavior and diagnostics layout, including a full redraw when
  the settings view changes to prevent stale or overlapping text.

Assets: installer, portable x64 ZIP, and source ZIP.
