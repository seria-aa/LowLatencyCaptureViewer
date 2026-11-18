# LLCV v2.3.0

## 한국어

- **프레임 보간:** NVIDIA VFG는 공식 지원 GPU(Ada·Blackwell)에서 FHD·QHD·4K UHD SDR 입력을 지원합니다. 일반 저지연, VSync, 독점 전체화면에서 사용할 수 있습니다. VSync에서는 캡처 FPS의 두 배 이상 주사율이 필요합니다. 기본은 꺼짐이며, NVIDIA 실행 파일은 기본 설치판에 포함되지 않습니다. F7로 켜고 끌 수 있고 단축키를 변경할 수 있습니다.
  - RTX 30(SM 8.6)의 실험적 호환 경로도 포함합니다. 지정된 런타임이 필요하며 RTX 3080에서 검증했습니다. NVIDIA 공식 지원은 아닙니다.
  - **VFG 구성품 받기:** 설정에서 한 번 확인하면 NVIDIA 서버의 지정 버전을 내려받아 현재 Windows 사용자 폴더에 설치합니다.
- **녹화 표시:** 일반 출력과 독점 전체화면에서 영상 위에 작고 반투명한 REC 표시와 경과 시간이 나타납니다. 저장 완료 시 영상이나 폴더를 열 수 있습니다. 표시는 저장 영상에 들어가지 않습니다.
- **MP4 재생 길이:** 정상 종료한 녹화와 정상적으로 닫힌 분할 파일의 길이를 Windows 탐색기가 읽을 수 있습니다. 강제 종료된 조각이나 기존 파일은 자동으로 고치지 않습니다.
- **오디오 연결 실패 대응:** 캡처 오디오를 연결하지 못했을 때 영상만 시작할 수 있습니다. 오디오와 함께 다시 시작할 때까지 녹화는 사용할 수 없습니다.
- **단축키와 안내:** F1에서 실제로 지정된 단축키를 모두 확인할 수 있습니다. 미지정 키는 숨기고, F2의 도움말 · 진단에서 키를 변경할 수 있습니다.

[프레임 보간 안내](FRAME_INTERPOLATION.ko.md) · [녹화 안내](RECORDING.ko.md)

## English

- **Frame interpolation:** optional NVIDIA VFG on Ada/Blackwell for FHD, QHD and 4K UHD SDR. It works with standard low-latency output, VSync, and exclusive fullscreen. VSync requires at least twice the capture frame rate in display refresh. It is off by default, and the base installer does not include the NVIDIA runtime. F7 toggles interpolation and can be remapped.
  - Experimental RTX 30 (SM 8.6) compatibility is included for an exact runtime, verified on RTX 3080. It is not officially supported by NVIDIA.
  - **Download VFG components:** confirm once in Settings to fetch the pinned runtime from NVIDIA and install it for the current Windows user.
- **Recording indicator:** standard and exclusive output show a compact translucent REC badge with an elapsed timer. Open the video or folder when saving is complete. The badge is not recorded.
- **MP4 duration:** Windows Explorer can read the length of normally completed recordings and splits. Interrupted fragments and existing files are not automatically rewritten.
- **Audio connection failure:** you can start video-only when capture audio cannot connect. Recording is unavailable until playback is restarted with audio.
- **Shortcuts and help:** F1 lists all assigned shortcuts and hides unassigned actions. Edit shortcuts under F2 → Guide & diagnostics.

[Frame interpolation](FRAME_INTERPOLATION.md) · [Recording guide](RECORDING.md)
