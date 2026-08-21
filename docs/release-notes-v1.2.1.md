## v1.2.1

### 한국어

- PCM 버퍼 목표에 **25 ms (안정 여유)** 단계를 추가했습니다. 낮은 지연 설정에서
  underrun이 반복될 때 `10 → 15 → 20 → 25 → 30 ms` 순서로 조정할 수 있습니다.
- 한국어·영어 README와 상세 오디오/진단 문서를 현재 UI, 5초 워밍업, 진단 로그 위치에
  맞게 정리했습니다.
- 오래된 Tab 진단창 예시 이미지를 제거해 현재 OSD와 맞지 않는 정보를 패키지에 넣지
  않도록 했습니다.

### English

- Added a **25 ms (extra stability)** PCM buffer target. When underruns repeat
  at lower-latency settings, targets can now be adjusted in the sequence
  `10 → 15 → 20 → 25 → 30 ms`.
- Updated the Korean and English README and detailed audio/diagnostics guides
  to match the current UI, five-second warmup, and diagnostic-log location.
- Removed outdated Tab diagnostics example images so future packages do not
  ship information that no longer matches the current OSD.
