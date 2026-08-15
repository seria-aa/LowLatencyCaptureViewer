## 한국어

### v1.1.0

- 30fps 캡처 선택을 추가했습니다. 장치가 정확한 30/29.97fps 타입을 제공하는
  경우뿐 아니라, UVC 드라이버가 지원 프레임 범위로만 30fps를 알리는 경우도
  인식합니다. 선택 시 높은 프레임을 받아 버리지 않고 캡처 핀 자체를 30fps로
  설정합니다.
- Pixel-perfect 전체화면은 화면에 들어가는 가장 큰 정수 배율로 표시하고 남는
  영역을 검게 처리합니다. 캡처 해상도가 모니터보다 크면 화면을 자르지 않고
  비율을 유지해 전체 영상을 축소 표시합니다.
- F5를 누르면 저장된 Pixel-perfect 설정을 바꾸지 않고 현재 모니터에서 정확한
  1:1 창 크기로 복원합니다. 캡처와 모니터 해상도가 같으면 borderless
  전체화면을 사용하며, 1:1 크기가 모니터보다 크면 HUD로 안내합니다.
- 캡처 해상도와 모니터 해상도가 같으면 Pixel-perfect 설정과 관계없이 자동
  borderless 전체화면으로 시작합니다. 상대적 창 크기로 저장된 창이 실제로 더
  작은 경우에는 저장된 비율을 계속 우선합니다.
- 전체화면/F5 전환 시 D3D11 출력 리소스만 다시 구성합니다. DirectShow 캡처와
  WASAPI는 계속 실행되며 지속적인 프레임·오디오 큐를 추가하지 않습니다.
- H.264/AVC와 MPEG-4 압축 캡처 지원을 제거했습니다. 프레임 참조와 디코더
  재정렬로 저지연을 안정적으로 보장할 수 없기 때문입니다. MJPEG만 실험적 압축
  호환 모드로 유지하며, 선택 해상도의 NV12/YUY2가 모두 30fps 미만일 때도
  선택지에 표시합니다.

Assets: installer, portable x64 ZIP, and source ZIP.

## English

### v1.1.0

- Added selectable 30 fps capture. Exact 30/29.97 media types are recognized,
  including UVC drivers that expose 30 fps only through their supported frame
  interval range. The capture pin itself runs at 30 fps instead of receiving a
  faster stream and dropping frames.
- Pixel-perfect fullscreen now uses the largest integer scale that fits and
  fills unused space with black. Captures larger than the display retain the
  complete picture with aspect-preserving downscale instead of cropping.
- F5 restores an exact 1:1 client size on the current monitor without changing
  the saved Pixel-perfect setting. A matching monitor uses borderless
  fullscreen; an oversized 1:1 request is reported through the HUD.
- Matching capture and monitor resolutions automatically enter borderless
  fullscreen regardless of the Pixel-perfect setting. A genuinely smaller
  saved monitor-relative window continues to take priority.
- Fullscreen and F5 transitions rebuild only D3D11 output resources. DirectShow
  capture and WASAPI remain running, with no persistent frame or audio queue.
- Removed H.264/AVC and MPEG-4 compressed capture because inter-frame decode
  and output reordering cannot provide reliably low latency. MJPEG remains the
  sole experimental compressed compatibility mode and is still available when
  every raw NV12/YUY2 mode at the selected resolution stays below 30 fps.

Assets: installer, portable x64 ZIP, and source ZIP.
