# LLCV v2.1.5

## 한국어

캡처 설정과 녹화 안정성을 다듬은 업데이트입니다.

- **캡처 FPS 직접 선택:** 드라이버가 다른 FPS로 적용하면 안내하고 시작하지 않습니다. 지원되지 않는 선택값을 자동으로 바꾸지 않고 표시하며, 실제 입력 FPS가 지속적으로 높으면 진단창·로그에 경고합니다. **자동 FPS 선택은 기존대로 유지**합니다.
- **SDR 색 범위:** 자동 / Limited / Full을 재생·스크린샷·녹화에 함께 적용합니다. 기본값은 자동이며 P010/HDR에는 적용하지 않습니다. MJPEG 색 공간은 별도로 선택하고 중복 범위 옵션은 통합했습니다.
- **영상 설정 정리:** 목록에 없는 FPS 요청·40fps 캡처를 프레임 옆 **추가 설정**에 모았습니다. 기존 스타일과 스크롤 없는 구성을 유지하고 여백·정렬·열릴 때의 깜빡임을 보완했습니다.
- **표시 해상도 연동:** **캡처 해상도와 동일**을 선택하면 창·전체화면에서 캡처 설정에 맞는 표시 크기를 사용합니다. 신규 설정의 기본값입니다. 이전 캡처·표시 크기가 같으면 연동으로 이전하고, 서로 다르면 고정 크기를 유지합니다. 새 버전에서 직접 저장한 고정/연동 선택은 바꾸지 않습니다.
- **녹화 안정성:** 남은 저장 공간 조회가 인코딩 작업을 막지 않도록 하고, 이미 확보한 영상 버퍼를 더 활용하도록 보완했습니다. HEVC를 포함한 전체 저장 프레임 수, 일시정지·분할·저장 지연·파일 복구 검증도 강화했습니다.

기존 설정·설치 경로·단축키와 녹화 기본 상한 **1080p·60fps**는 유지합니다.

[사용 가이드](USAGE.ko.md) · [영상 설정](VIDEO.ko.md) · [녹화 안내](RECORDING.ko.md)

## English

An update focused on capture settings and recording reliability.

- **Explicit capture FPS:** reject driver rate substitutions instead of silently falling back. Keep unsupported selections visible, and warn in diagnostics/logs about sustained excess input FPS. **Auto FPS selection is unchanged.**
- **SDR range:** Auto / Limited / Full is shared by playback, screenshots and recording. Auto remains the default; P010/HDR is unaffected. MJPEG matrix selection remains separate, with duplicate range controls consolidated.
- **Video settings:** unlisted FPS requests and 40 fps capture are grouped under **More** beside Frame rate. Retains the existing style and scroll-free layout, with refined margins/alignment and batched flyout redraw.
- **Display sizing:** **Same as capture** follows the capture setting in windowed and fullscreen modes. New profiles and equal legacy capture/display sizes use matching; different legacy sizes remain fixed. Explicitly saved fixed/matched modes take precedence.
- **Recording reliability:** periodic free-space queries no longer block the encoder worker, and delivery uses more of the existing video-buffer budget. Expanded total-frame, HEVC, pause/split, storage-delay and recovery checks.

Existing settings, installation paths, shortcuts and **1080p/60fps** recording limits are retained.

[User guide](USAGE.md) · [Video settings](VIDEO.md) · [Recording guide](RECORDING.md)
