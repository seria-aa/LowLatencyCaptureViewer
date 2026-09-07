## v1.2.4

### 한국어

- 장치 탐색, 캡처 콜백, PCM 버퍼·리샘플러, 설정 저장, 업데이트 확인,
  창 배치, 오디오 진단과 영상 포맷 처리를 각각의 모듈로 정리했습니다.
  기존 실행 경로와 사용자 설정 형식은 유지됩니다.
- 오디오 장치 기능, PCM 버퍼, DirectShow 장치·포맷, 설정 저장,
  업데이트 버전 비교, 창 크기 계산과 오디오 오류 패턴을 검증하는 회귀 테스트를
  추가했습니다.
- 앱 종료가 시작된 뒤 ASIO 드라이버가 요청한 마지막 불완전 버퍼를 실제 재생 중
  underrun으로 잘못 기록하지 않도록 진단 집계를 수정했습니다.
- 저지연 화면 표시 경로에서 드물게 모니터 신호가 끊기는 환경은 VSync
  호환성 모드로 전환하도록 문제 해결 안내를 보강했습니다.
- WASAPI Shared, ASIO, NV12, YUY2, MJPEG 및 실험적 P010 경로의 동작은
  위 ASIO 종료 진단 수정을 제외하고 기존과 같습니다.

### English

- Device discovery, capture callbacks, PCM buffering/resampling, settings,
  update checks, window geometry, audio diagnostics, and video-format handling
  are now separated into focused modules. Existing runtime paths and the user
  settings format are preserved.
- Added regression coverage for audio device capabilities, PCM buffering,
  DirectShow devices and formats, settings persistence, update-version
  comparison, window geometry, and audio error-pattern analysis.
- A final partial buffer requested by an ASIO driver after shutdown begins is
  no longer counted as a runtime underrun.
- Troubleshooting now recommends VSync compatibility mode on systems where the
  immediate tearing path causes intermittent monitor signal loss.
- WASAPI Shared, ASIO, NV12, YUY2, MJPEG, and experimental P010 behavior is
  unchanged apart from the ASIO shutdown diagnostic correction.
