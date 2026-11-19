# LLCV v2.3.2

- **재생 그래픽카드 선택:** 설정의 영상 → 화면 표시에서 표시 모니터와 함께 재생 GPU를 지정할 수 있습니다. 자동은 외장 그래픽을 우선합니다.
- **내장·외장 그래픽 호환:** 호환 NVIDIA GPU를 직접 선택하면 Windows 설정을 바꾸지 않고 VFG를 사용할 수 있습니다. 드라이버가 같은 그래픽카드를 여러 번 보고해도 목록에서 빠지지 않습니다.
- **독점 전체화면:** 표시 모니터에 연결된 GPU를 우선합니다. 지정한 GPU로 시작할 수 없으면 다른 GPU로 재생을 시도합니다.

## English

- **Playback GPU:** Select a GPU next to the display monitor in Video settings. Automatic prefers discrete graphics.
- **Hybrid graphics:** Select a compatible NVIDIA GPU for VFG without changing Windows settings. Duplicate driver entries no longer hide a graphics card from the list.
- **Exclusive fullscreen:** Prefers a GPU connected to the display. If the selected GPU cannot start playback, LLCV tries a fallback.
