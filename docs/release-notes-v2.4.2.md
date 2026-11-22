# LLCV v2.4.2

- **FPS만 표시:** 영상 재생 중 `F`로 FPS 숫자만 표시하거나 숨깁니다. 화면에 제출한 보간 프레임도 포함합니다. `Tab`은 전체 진단으로 유지합니다.
- **보간 알림:** 잠시 원본 재생으로 전환됐다가 보간이 다시 동작할 때 ‘켜짐’ 알림을 반복하지 않습니다. 현재 상태는 `Tab`에서 볼 수 있습니다.
- **NVIDIA VFG ‘낮음’:** 연속 입력에서 이전 프레임의 GPU 버퍼를 재사용해 입력 복사를 한 번 줄였습니다. ‘중간’의 처리 방식과 표시 타이밍은 그대로입니다.

[사용 가이드](USAGE.ko.md) · [프레임 보간 안내](FRAME_INTERPOLATION.ko.md)

## English

- **FPS only:** Press `F` in video playback to show or hide the FPS counter. It includes generated frames submitted for display. `Tab` still opens full diagnostics.
- **Interpolation notifications:** The app no longer repeats the On notice when interpolation resumes after a temporary fallback. Check the current state with `Tab`.
- **NVIDIA VFG Low:** Reuses the previous GPU input on consecutive frames, avoiding one input copy. VFG Medium and presentation timing are unchanged.

[User guide](USAGE.md) · [Frame interpolation guide](FRAME_INTERPOLATION.md)
