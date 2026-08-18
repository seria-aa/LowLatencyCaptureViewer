## v1.1.4

### 한국어

- 영상과 오디오 진단 워밍업을 모두 5초로 통일했습니다. 워밍업은 실제 출력 시작을 늦추지 않고 초기 통계만 제외합니다.
- 오디오 오류 패턴 진단을 추가했습니다. 최근 오류가 간헐적인지 연속적인지, 마지막 발생 시점과 최대 연속 underrun 횟수를 확인할 수 있습니다.
- 오류 패턴 기록은 실제 오디오 오류가 발생할 때만 수행하며, 정상적인 캡처·재생 경로에는 추가 버퍼나 프레임 큐를 넣지 않았습니다.
- 기존 DirectShow·D3D11·WASAPI 저지연 경로는 그대로 유지했습니다.

### English

- Unified video and audio diagnostic warm-up to 5 seconds. Warm-up excludes initial statistics; it does not delay actual playback startup.
- Added audio error-pattern diagnostics showing whether recent errors are intermittent or burst-like, when the last error occurred, and the maximum consecutive underrun count.
- Pattern history is recorded only on actual audio errors. No extra buffer or frame queue was added to the normal capture or playback path.
- The existing DirectShow, D3D11, and WASAPI low-latency paths remain unchanged.
