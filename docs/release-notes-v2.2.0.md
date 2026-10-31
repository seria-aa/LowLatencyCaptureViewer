# LLCV v2.2.0

## 한국어

- **진단자료 ZIP:** 도움말·진단에서 최근 로그와 저장된 설정, 버전·오류 정보를 모을 수 있습니다. 일부 경로·장치 식별자를 가리며 자동 전송하지 않습니다. 공유 전에 내용을 확인해 주세요.
- **캡처 장치 호환성:** 다중 입력 WDM 장치의 상위 크로스바 연결을 재생 시작과 설정의 모드 조회에 적용했습니다. 기존 입력 선택을 유지하고, 연결 실패 후 그래프를 복구하지 못하면 시작을 중단합니다.
- **도움말·설정 UI:** F1 안내창이 모니터 해상도·배율·작업 영역에 맞춰 스크롤 없이 표시됩니다. NIS 선명도는 주요 값 위주로 고르며, 기존에 저장한 사용자 값도 유지합니다.
- **설정·녹화 안정성:** 장치 조회 중 설정창을 닫거나 재생을 시작하는 처리를 보완했습니다. 녹화 탭의 저장 공간 조회를 비동기로 처리하고, 잘못된 녹화 설정은 해당 항목만 기본값으로 복원합니다.
- **VSync·진단 로그:** Flip VSync에서 표시 준비 뒤 최신 프레임을 선택하도록 개선했습니다. 재생 중 진단 로그의 지연 기록과 앱이 생성한 콘솔의 QuickEdit 비활성화로 출력 대기 위험을 줄였습니다.
- **정리·검증:** 내부 중복·정리 대상 코드를 다듬고 도움말 배치, 설정 수명, 진단자료, 캡처 연결 관련 회귀 검사 항목을 보완했습니다.

기본 저지연·독점 전체화면 출력 방식과 녹화 기본 상한 1080p·60fps는 유지합니다.

[영상 설정](VIDEO.ko.md) · [진단자료 안내](DIAGNOSTICS.ko.md) · [녹화 안내](RECORDING.ko.md)

## English

- **Diagnostic report ZIPs:** collect recent logs, saved settings, versions and error details from Guide & logs. Some paths/device identifiers are masked; nothing is uploaded automatically. Review the contents before sharing.
- **Capture compatibility:** connect upstream crossbars for applicable multi-input WDM devices during playback startup and settings mode queries. Existing input routing is preserved; startup stops if a failed attempt cannot restore its graph.
- **Help and settings UI:** F1 help fits without scrolling and adapts to monitor resolution, DPI and work-area changes. NIS sharpness uses a shorter preset list while preserving existing custom saved values.
- **Settings and recording stability:** improve closing settings or starting playback during device queries. Query idle recording-folder space asynchronously and recover invalid recording preferences per field rather than discarding other valid choices.
- **VSync and diagnostics:** select the latest frame after swapchain readiness in Flip VSync. Deferred playback diagnostics and disabling QuickEdit for app-created consoles reduce risks from blocked diagnostic output.
- **Cleanup and verification:** streamline internal code and extend regression coverage for help layout, settings lifetimes, reports and capture connections.

Default low-latency/exclusive presentation and 1080p60 recording caps are unchanged.

[Video settings](VIDEO.md) · [Diagnostic reports](DIAGNOSTICS.md) · [Recording guide](RECORDING.md)
