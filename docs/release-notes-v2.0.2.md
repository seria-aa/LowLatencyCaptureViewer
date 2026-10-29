## v2.0.2

### 한국어

**2026-10-06 설치파일 갱신:** 시작 메뉴 바로가기를 자동으로 생성하고 설치 화면 정렬을
개선했습니다. 바탕화면 바로가기는 선택 사항입니다. 앱 버전은 2.0.2로 유지하며,
앱 실행파일과 포터블 ZIP은 변경하지 않았습니다. 기존 사용자는 다시 설치할 필요가 없습니다.

앱 이름과 안내를 간결하게 정리하고, WASAPI Exclusive 사용 경험과 설치 편의성,
오디오 처리 효율을 개선했습니다.

#### 간결한 이름과 안내

- 앱·설정·F1 정보창·설치 프로그램의 표시 이름을 **LLCV**로 통일했습니다.
- 영상과 오디오 전용 창의 제목은 **LLCV**로 간결하게 표시합니다. 캡처 장치·해상도·프레임 등 상세 정보는 **Tab**에서 확인할 수 있습니다.
- 캡처 장치의 자동 선택 안내를 간결하게 정리하고, 장치 목록에는 드라이버가 제공하는 이름을 표시합니다.
- HDR10·MJPEG·ASIO·콘솔 LPCM 5.1의 실험 표기를 제거하고 사용 조건 중심으로 안내합니다. ASIO는 호환 드라이버가 필요하며, LPCM 5.1은 지원 장비에서 WASAPI Shared로 사용합니다. NVIDIA VSR은 실험 기능으로 유지합니다.

#### WASAPI Exclusive 오디오

- 장치 검사를 실제 재생과 같은 버퍼 정렬·패킷 처리 경로에 맞추고, 장치의 실제 재생 주기를 기준으로 판정하도록 개선했습니다.
- 검사 결과를 장치별로 저장합니다. 다른 장치를 검사하는 도중 창을 닫아도 이미 완료된 결과는 유지하고, 모드 전환·다음 실행에서 재사용합니다.
- 장치 사용 중·분리·타이밍 불안정 등은 **확인 보류**로 구분합니다. 완료된 검사를 자동으로 반복하지 않으며, **전체 장치 다시 검사**로 직접 다시 확인할 수 있습니다.
- 검사 결과가 도착하거나 출력 장치를 바꿀 때 유효한 버퍼 선택을 유지합니다. 검증된 최소값보다 작은 값 등 유효하지 않은 선택만 조정합니다.
- 장치 상태와 재검사 안내의 영어 표시를 보완하고, 검사 결과 저장·종료·화면 갱신 처리를 일관되게 정리했습니다.

#### 설치 편의

- **시작 메뉴 바로가기**는 자동으로 생성하며, **바탕화면 바로가기**는 설치 중 선택할 수 있습니다.
- Windows 표시 배율에 맞춰 설치 선택 항목의 여백을 다듬었습니다.

#### 처리 효율

- 오디오 버퍼를 연속 구간 단위로 복사하여 반복 계산을 줄였습니다. 음량·리샘플링·버퍼 크기와 데이터 순서는 유지합니다.
- 파일 로그와 진단 콘솔을 모두 끄면 Exclusive의 상세 진단용 시계 조회와 통계를 생략합니다. Tab 정보와 오류 복구 동작은 유지합니다.
- 사용하지 않는 통계·이전 UI 문구·중복 판정과 로그 처리를 정리하고, 영문 문구 조회의 임시 메모리 할당을 줄였습니다.
- VSR 측정용 통계 저장 공간은 별도 시험용 빌드에서만 사용합니다. 최신 프레임 우선 표시와 영상 처리 경로는 유지합니다.

#### 업데이트 안내

기존 설정과 설치 경로를 그대로 사용합니다. 실행 파일 이름은 업데이트 호환성을 위해
`LowLatencyCaptureViewer.exe`로 유지하며, 최신 프레임 우선 표시와 오디오 기본값도 유지합니다.
이전 방식의 Exclusive 검사 기록은 한 번 갱신하며, 새 기준으로 완료한 결과부터 재사용합니다.
기능별 사용 조건은 [한국어 README](../README.ko.md)를 참고하세요.

### English

**Installer refreshed on 2026-10-06:** restored automatic Start Menu shortcut creation
and refined installer alignment. The desktop shortcut remains optional. The version
remains 2.0.2; the app executable and portable ZIP are unchanged. Existing users do not
need to reinstall.

This update simplifies app naming and guidance, improves the WASAPI Exclusive
experience and installation choices, and reduces unnecessary audio-processing work.

#### Clearer naming and guidance

- Unified the display name across the app, settings, F1 help, and installer as **LLCV**.
- Video and audio-only viewer titles now show **LLCV**. Press **Tab** for capture-device, resolution, frame-rate, and other playback details.
- Simplified automatic capture-device guidance and kept the driver-provided names in the device list.
- Removed the experimental labels for HDR10, MJPEG, ASIO, and console LPCM 5.1 in favor of clearer requirements. ASIO requires a compatible driver; LPCM 5.1 uses WASAPI Shared on supported equipment. NVIDIA VSR remains experimental.

#### WASAPI Exclusive audio

- Aligned device checks with the playback path's buffer alignment and packet handling, assessing timing against the device's actual playback period.
- Saved results per device. Closing settings while other devices are still being checked retains completed results for reuse across mode switches and launches.
- Distinguished busy/disconnected devices and unstable timing as **retry required**. Completed checks are not automatically repeated; choose **Recheck all devices** to retry manually.
- Preserved valid buffer choices when results arrive or the output device changes. Only invalid choices, such as values below the verified minimum, are adjusted.
- Completed the English device-status and rescan guidance and unified result saving, shutdown, and UI refresh handling.

#### Installation

- A **Start Menu shortcut** is created automatically; the **desktop shortcut** is optional during installation.
- Improved task-list spacing across Windows display scales.

#### Efficiency

- Reduced repeated audio-buffer calculations by copying contiguous spans, retaining volume, resampling, buffer sizes, and sample order.
- Skipped detailed Exclusive diagnostic clock reads and statistics when both file logging and the diagnostic console are off. Tab information and error recovery remain available.
- Removed unused statistics, obsolete UI captions, and duplicate checks/logging, and reduced temporary allocations during English text lookup.
- Restricted VSR measurement histogram storage to private test builds, retaining latest-frame-first presentation and the existing video-processing path.

#### Upgrading

Existing settings and installation paths are retained. The executable remains
`LowLatencyCaptureViewer.exe` for update compatibility, with the same latest-frame-first
presentation and audio defaults. Older Exclusive probe records are refreshed once;
results completed under the new criteria are reused afterward.
See the [English README](../README.en.md) for feature requirements.
