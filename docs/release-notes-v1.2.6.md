## v1.2.6

### 한국어

- WASAPI Shared에서 장시간 사용 중 클록 차이와 입력 패킷 도착 간격 변화로
  PCM 버퍼가 부족해지는 상황에 대한 보정과 복구를 개선했습니다.
- **PCM 버퍼 목표 기본값을 25 ms로 변경했습니다.** 이전 버전에 저장된 20 ms는
  첫 실행 때 한 번만 25 ms로 변경합니다. 다른 저장값은 유지하며, 이후 사용자가
  직접 20 ms를 선택하면 그 값도 유지합니다.
- 진단창과 로그에서 PCM 부족·넘침, 버퍼 재충전, 출력 처리 지연 의심을 구분해
  끊김 원인을 확인하기 쉽게 했습니다. 출력 지연 의심 수치는 실제 누락된 오디오
  길이와 구분해 표시합니다.
- WASAPI 장치 오류·중단 후 복구와 ASIO 재설정 알림 처리를 보강했습니다.
  실패가 반복되면 무한 재시도하지 않고 중단하며, 종료 후 진단 상태도 정리합니다.
- v1.2.5.1의 F11/F5 출력 전환 개선을 포함합니다. 전체화면 전환 중 여러 크기
  변경을 한 번의 출력 갱신으로 합치며, F11을 길게 눌러도 전환이 반복되지 않습니다.
- 캡처 시작이 중간에 실패해도 콜백이 멈춘 뒤 프레임 데이터를 해제하도록
  정리 순서를 수정했습니다.
- 업데이트 확인·Exclusive 장치 검사 중 설정 창을 닫을 때 결과 데이터가 남는
  문제와 완료 처리 전 검사 재시작 경합을 수정했습니다. 설치 파일이 아직 없는
  새 릴리스는 현재 버전이 최신인 것으로 잘못 안내하지 않습니다.
- 설치 파일과 포터블 ZIP에 ASIO SDK 라이선스 파일을 포함했습니다.

> 25 ms는 기존 20 ms보다 약 5 ms의 목표 대기 여유를 추가합니다. 실제 입력이
> 부족해진 뒤에는 버퍼를 다시 채우는 동안 짧은 무음이 생길 수 있으며, 일부
> 조건에서는 이전보다 중단 시간이 길어질 수 있습니다. 자동 테스트와 가상 재생
> 검증을 수행했지만, 실제 장치의 장시간 청취 검증이나 모든 끊김 제거를 보장하지는
> 않습니다. MJPEG·P010 HDR·ASIO의 기존 실험적 지원 범위는 유지됩니다.

### English

- Improved WASAPI Shared clock/queue control and recovery when clock mismatch or
  changing packet-arrival timing consumes PCM reserve during long sessions.
- **Changed the default PCM buffer target to 25 ms.** Previously unmigrated legacy
  20 ms settings are upgraded once on first launch. Other saved values remain
  unchanged, and selecting 20 ms afterward is respected.
- Diagnostics now distinguish PCM shortages/overflow, reserve refilling, and
  suspected output scheduling delays. Scheduling estimates are not reported as
  measured missing-audio duration.
- Hardened WASAPI recovery after device errors/stalls and ASIO reset notifications.
  Repeated failures stop after a bounded retry budget instead of retrying forever,
  and stale diagnostic state is cleared on shutdown.
- Includes the v1.2.5.1 F11/F5 output-transition fixes. Multiple size notifications
  during one transition produce one output update, and holding F11 no longer
  repeatedly toggles fullscreen.
- Fixed capture cleanup after a partially failed startup so callbacks stop before
  their frame storage is released.
- Fixed pending-result ownership when closing settings during update checks or
  Exclusive scans, and prevented scan restart before old completion handling.
  A newer release without an installer is no longer described as up to date.
- Included the ASIO SDK license in both installer and portable packages.

> The 25 ms target adds approximately 5 ms of intended queue reserve versus 20 ms.
> Recovery after missing input can introduce brief silence while refilling, and
> may lengthen some interruptions compared with previous behavior. Automated and
> simulated playback tests were performed, but extended physical-device listening
> validation was not. This is not a guarantee of dropout-free playback. Existing
> experimental MJPEG, P010 HDR, and ASIO support remains unchanged.
