## v1.1.7.1

### 한국어

- 디스플레이 변경, 장치 연결·분리, 스왑체인 가시성 변화와 D3D11 장치 오류를
  진단 로그에 기록합니다.
- 이벤트 로그에는 현지 시각, 실행 후 경과 시간, 오류 코드와 장치 제거 사유를
  함께 남겨 화면이 잠시 꺼지거나 신호가 끊긴 원인을 확인하기 쉽게 했습니다.
- 이벤트 기록은 상태가 바뀔 때만 수행하며 프레임마다 기록하지 않으므로 영상·오디오
  처리 지연을 추가하지 않습니다.

### English

- Diagnostic logs now record display changes, device arrival/removal, swap-chain
  occlusion/visibility transitions, and D3D11 device failures.
- Event entries include local time, uptime, HRESULT values, and device-removal
  reasons to help identify brief blank screens or signal interruptions.
- Events are logged only on state changes, never once per frame, so this adds no
  capture, video, or audio processing latency.
