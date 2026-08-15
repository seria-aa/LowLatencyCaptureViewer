## 한국어

### v1.1.1

- Pixel-perfect 전체화면의 의미를 엄격한 1:1 표시로 통일했습니다. 캡처 영상이
  모니터 안에 들어가면 확대하지 않고 중앙에 표시하며 남는 영역은 검게
  처리합니다. 캡처가 모니터보다 큰 경우에만 비율을 유지해 축소합니다.
- 해상도와 DPI뿐 아니라 화면비가 다른 모니터 사이의 상대적 창 크기 계산을
  바로잡았습니다. 1920x1200 같은 16:10 및 21:9/32:9 화면에서 창이 불필요하게
  한 번 더 작아지지 않습니다.
- 기존에 저장된 상대 창 크기는 그대로 보존하며, 구버전의 잘못된 화면비
  계산값과 정확히 일치하는 설정만 새 기준으로 한 번 교정합니다.
- F2로 설정을 열면 마우스 위치와 관계없이 기존 뷰어가 있던 모니터에 설정 창이
  표시되고 앞쪽으로 활성화됩니다. 설정 창을 다른 모니터로 옮겨도 뷰어의 저장된
  모니터와 상대 크기 기준은 바뀌지 않습니다.
- 저장된 모니터 장치 정보를 먼저 사용해 뷰어 위치를 복원하도록 보강했습니다.
- F5로 1:1 크기를 복원하면 그 크기가 상대적 창 크기의 새 기준에도 반영됩니다.
- 캡처, 최신 프레임 유지, D3D11 표시와 WASAPI 오디오 경로는 변경하지 않았으며
  새로운 프레임·오디오 큐를 추가하지 않았습니다.

Assets: installer, portable x64 ZIP, and source ZIP.

## English

### v1.1.1

- Pixel-perfect fullscreen now consistently means strict 1:1 output. When the
  capture fits the monitor, it remains unscaled and centered with unused space
  cleared to black. Downscaling occurs only when the capture is larger than the
  monitor.
- Corrected relative window sizing across monitors with different resolutions,
  DPI, and aspect ratios. Windows are no longer shrunk a second time on 16:10,
  21:9, or 32:9 displays.
- Existing user-defined relative sizes are preserved. Only settings that
  exactly match the old incorrect aspect-ratio formula are migrated once.
- F2 now opens and activates Settings on the viewer's saved monitor regardless
  of mouse position. Moving Settings to another monitor no longer changes the
  viewer monitor or its relative-size baseline.
- Viewer restoration now prefers the saved monitor device identity before
  falling back to saved coordinates.
- F5 updates the relative-size baseline when restoring exact 1:1 output.
- Capture, latest-frame-only D3D11 presentation, and WASAPI audio paths are
  unchanged, with no new video or audio queue.

Assets: installer, portable x64 ZIP, and source ZIP.
