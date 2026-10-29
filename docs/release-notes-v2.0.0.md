## v2.0.0

### 한국어

2.0.0은 저지연 캡처 뷰어의 설정과 사용 경험을 새롭게 정리하고,
선택적으로 사용할 수 있는 NVIDIA VSR을 더한 업데이트입니다.

#### 새로 정리한 설정과 테마

- **영상 · 오디오 · 창 · 도움말·진단 · 앱 설정** 사이드바로 기능을 나누고, 역할별 배치와 여백을 정리했습니다.
- **다크·라이트 테마**와 Pretendard 글꼴을 설정·F1 안내창·오디오 전용 화면에 함께 적용했습니다.
- 드롭다운 정렬, 비활성 설명의 가독성, DPI별 글자 배치, 오디오 출력 모드 전환 시 화면 갱신을 다듬었습니다.
- 장치별 포맷·프레임 조회를 백그라운드에서 처리하고 같은 설정 창 안에서 재사용합니다. 필요한 경우 **새로고침**으로 다시 확인할 수 있습니다.

#### NVIDIA VSR — 실험적

- **영상 → NVIDIA VSR**과 **F6**으로 보정을 요청할 수 있습니다. 기본값은 OFF입니다.
- **VSR 캡처 해상도**와 **표시 해상도**를 따로 선택합니다. 1080p 입력을 1440p/4K 크기로 표시하거나, 입력과 표시를 같게 맞춰 동일 해상도 보정을 요청할 수 있습니다.
- **1280 × 720** 캡처 해상도를 추가했습니다. 해상도와 프레임의 실제 지원 여부는 캡처 장치에 따라 달라집니다.
- **표시 크기 고정**으로 선택한 창 크기를 유지하고, **F5**로 입력 해상도의 1:1 크기를 복원할 수 있습니다.
- **F6**은 캡처 재시작이나 해상도 변경 없이 효과만 전환합니다. ON/OFF 지연을 맞추기 위한 대기나 추가 프레임 큐를 넣지 않았습니다.
- **Tab**에서 VSR 요청 상태와 입력 → 실제 영상 표시 크기를 확인할 수 있습니다. 설정 안내도 함께 제공합니다.

VSR은 NVIDIA GPU의 **NV12 SDR 경로(MJPEG의 NV12 변환 포함)**에서 원본과 같거나
큰 크기로 표시할 때 요청합니다. HDR/P010·YUY2와 축소 표시에는 적용하지 않습니다.
요청 성공이나 동일 해상도 선택만으로 실제 보정이 활성화됐다고 보장하지 않으며,
NVIDIA App/제어판에서 VSR 설정과 재생 중 활성 표시를 확인해야 합니다.
VSR 처리로 GPU 사용량과 지연이 늘 수 있습니다. Tab에는 검증되지 않은 추가 지연 수치를
표시하지 않으며, 스크린샷은 VSR이 적용되지 않은 입력 영상을 저장합니다.

#### 더 일관된 안내와 창 조작

- F1과 도움말·진단의 단축키를 같은 카드형 구성으로 정리했습니다.
- F3 오디오 미터·Tab 진단·일시 알림의 글꼴과 패널을 앱 디자인에 맞췄습니다. 영상 위 OSD는 어두운 색상을 유지합니다.
- 최대 200% 음량 옵션을 켠 경우 오디오 전용 화면과 F3 미터에서 범위를 쉽게 확인하고, 100%를 넘는 마스터 음량은 주황색으로 구분합니다.
- VSR·스크린샷·1:1 복원 알림의 높이를 내용에 맞춰 다듬었습니다.
- 창 가장자리에 붙이기와 떼기가 같은 거리 기준을 따르도록 개선했습니다. Shift로 스냅을 무시하는 조작은 유지합니다.
- VSR의 입력·표시 해상도가 다를 때도 F5로 복원한 크기가 모니터 상대 크기 기준에 일관되게 반영됩니다.

기존 최신 프레임 우선 처리, 오디오 출력·PCM 버퍼 기본값, 실험적 HDR10·콘솔 LPCM 5.1,
F12 PNG·클립보드 저장을 유지합니다. HDR 스크린샷은 계속 SDR로 변환해 저장합니다.
사용 조건과 조작 방법은 [한국어 README](../README.ko.md)를 참고하세요.

### English

2.0.0 refreshes the settings and everyday experience of the low-latency capture
viewer and adds optional NVIDIA VSR integration.

#### Redesigned settings and themes

- Organized settings into **Video, Audio, Window, Guide & logs, and App**, with clearer groups and spacing.
- Added shared **dark/light themes** and Pretendard typography across settings, F1 help, and the audio-only view.
- Refined dropdown alignment, disabled-text readability, DPI-aware text layout, and repainting when switching audio output modes.
- Queries supported formats/frame rates in the background and reuses results within the settings session. **Refresh** requests a new query when needed.

#### NVIDIA VSR — experimental

- Request enhancement through **Video → NVIDIA VSR** or **F6**. The default is OFF.
- Select **VSR capture** and **Display resolution** independently: show 1080p input at a 1440p/4K size, or choose matching sizes to request native-resolution processing.
- Added **1280 × 720** capture. Available resolution/frame-rate combinations depend on the capture device.
- Use **Lock display size** to retain the selected window size, or **F5** to restore source-resolution 1:1 size.
- **F6** switches only the effect, without restarting capture or changing resolutions. No delay or extra frame queue is added to equalize ON/OFF latency.
- **Tab** shows the VSR request state and input → actual displayed video size. A setup guide is available in settings.

Requests are limited to the **NV12 SDR path on NVIDIA GPUs (including MJPEG decoded
to NV12)** at native size or larger. HDR/P010, YUY2, and downscaled output bypass VSR.
A successful request or matching resolutions does not guarantee actual enhancement;
check the VSR setting and active indicator in NVIDIA App/Control Panel.
VSR processing can increase GPU use and latency. Tab does not present an unverified
added-latency value, and screenshots retain the input image without VSR.

#### Consistent guidance and window controls

- Matched the shortcut cards in F1 and Guide & logs.
- Restyled the F3 audio meter, Tab diagnostics, and transient notifications to follow the app's typography and panels. In-video overlays remain dark.
- Made the optional 200% range clear in audio-only and F3 views, with orange master-volume values above 100%.
- Fitted VSR, screenshot, and 1:1 notifications to their text height.
- Applied the same edge-distance threshold when snapping and releasing windows, retaining Shift-drag bypass.
- Made F5's restored source size consistent with the monitor-relative sizing baseline when VSR capture/display resolutions differ.

Retains latest-frame-first playback, existing audio-output/PCM-buffer defaults,
experimental HDR10 and console LPCM 5.1, and F12 PNG/clipboard capture.
HDR screenshots continue to be converted to SDR. See the [English README](../README.en.md)
for usage and requirements.
