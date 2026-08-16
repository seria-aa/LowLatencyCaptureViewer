## v1.1.3

### 한국어

- 21:9, 32:9, 16:10 등 비표준 화면 비율에서 창을 확대하거나 최대화해도
  캡처 영상이 늘어나지 않도록 종횡비 보존 표시를 강화했습니다.
- Pixel-perfect가 꺼진 확대 모드에서도 영상 전체를 중앙에 맞추고 남는 영역은
  레터박스/필러박스로 표시합니다.
- 창 크기 변경이 끝난 뒤에만 D3D11 출력 크기를 다시 구성하도록 해, 드래그 중
  반복적인 출력 재구성으로 인한 불필요한 끊김을 줄였습니다.
- 기존 오디오 only OSD, PCM 입력 포맷 확장, 로그 순환 보관 기능을 정식 버전으로
  포함했습니다.
- 캡처·최신 프레임 유지·D3D11·WASAPI 경로에는 프레임 큐나 리샘플러를 추가하지
  않았습니다.

### English

- Strengthened aspect-ratio preservation on 21:9, 32:9, 16:10, and other
  non-standard displays so enlarged or maximized windows do not stretch the
  capture image.
- Scaled mode now centers the complete video and uses letterbox/pillarbox bars
  for any unused area when Pixel-perfect is disabled.
- The D3D11 output is reconfigured once after an interactive resize completes,
  avoiding repeated output rebuilds while the window is being dragged.
- Promotes the existing audio-only OSD, extended PCM input negotiation, and
  bounded rotating diagnostics logs to the stable release.
- No frame queue or resampler was added to the capture, latest-frame-only
  D3D11, or WASAPI paths.
