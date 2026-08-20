## v1.1.6

### 한국어

- WASAPI Shared / Exclusive와 ASIO(설치된 ASIO 드라이버가 있을 때만 표시) 출력 경로를 정리했습니다.
- WASAPI Exclusive는 안정성 점검을 위해 설정 UI에서 임시로 숨겼습니다. 기존 Exclusive 설정은 Shared로 전환됩니다.
- 클록 드리프트 보정에 끔·자동·켬 선택을 제공하고, ASIO에서도 동일한 선택을 사용할 수 있습니다.
- 오디오 OSD에 L/R 피크와 클리핑 상태를 표시합니다.
- 기존 DirectShow·D3D11 최신 프레임 표시 경로와 저지연 영상 경로는 변경하지 않았습니다.

### English

- Refined WASAPI Shared / Exclusive and optional ASIO output paths. ASIO is shown only when an installed ASIO driver is detected.
- WASAPI Exclusive is temporarily hidden from the settings UI while its stability is being investigated. Existing Exclusive profiles migrate to Shared.
- Clock-drift correction now offers Off, Auto, and On modes, including for ASIO output.
- The audio OSD reports L/R peaks and clipping status.
- The existing DirectShow, D3D11 latest-frame, and low-latency video paths are unchanged.
