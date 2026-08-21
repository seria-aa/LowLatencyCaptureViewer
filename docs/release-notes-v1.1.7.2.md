## v1.1.7.2 (Pre-release)

### 한국어

- WASAPI Exclusive에 장치별 이벤트 경로 사전 검사를 추가했습니다.
  일부 드라이버는 Exclusive 초기화는 받아들이지만 실제 이벤트 타이밍이 불안정할 수
  있으므로, 5~40 ms 범위에서 실제 재생 이벤트를 확인한 장치만 사용할 수 있도록
  표시합니다.
- 출력 장치 목록에는 `사용 가능 · n ms`, `사용 불가`, `검사 중` 상태가 나타납니다.
  검사 결과는 장치 ID별로 저장되어 다음 실행이나 설정 창 재진입 때 다시 검사하지
  않으며, `전체 장치 다시 검사` 버튼으로만 강제로 갱신할 수 있습니다.
- Shared 저지연 상태 문구와 버퍼 표시를 간결하게 정리했습니다.

> 이 버전의 WASAPI Exclusive는 드라이버별 실제 동작을 검증하기 위한 프리릴리스입니다.
> 사용 가능한 것으로 표시된 장치에서도 문제를 발견하면 Shared 또는 ASIO로 전환하고
> 진단 로그를 첨부해 주세요.

### English

- Added a per-endpoint event-path preflight for WASAPI Exclusive. Some drivers
  accept Exclusive initialization but provide unstable real event timing, so
  the app tests actual playback events from 5 to 40 ms before offering it.
- Output devices now show `Available · n ms`, `Unavailable`, or `Checking`.
  Results are cached by endpoint ID and reused on later launches; only **Rescan
  all devices** forces a new measurement.
- Simplified the Shared low-latency status and buffer labels.

> WASAPI Exclusive remains a pre-release feature because real behavior differs
> by driver. If a device marked available still misbehaves, use Shared or ASIO
> and attach a diagnostic log.
