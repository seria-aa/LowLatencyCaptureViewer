# Low Latency Capture Viewer

> [English](README.md) · [최신 버전 다운로드](https://github.com/seria-aa/LowLatencyCaptureViewer/releases/latest)

HDMI 캡처보드의 영상과 소리를 최대한 짧은 경로로 전달하도록 설계한 Windows용
초저지연 뷰어입니다. 장면 합성이나 인코딩처럼 뷰어에 불필요한 처리 단계를 두지
않고 캡처와 표시에 집중해 가볍고 빠르게 동작합니다.

영상은 DirectShow에서 받아 D3D11로 표시하고, 오디오는 WASAPI로 직접
출력합니다. FFmpeg, 서드파티 코덱 팩이나 별도 Visual C++ 재배포 패키지는
필요하지 않습니다.

![Tab 실시간 정보창](docs/images/tab-diagnostics.png)

## 다운로드

[최신 릴리스](https://github.com/seria-aa/LowLatencyCaptureViewer/releases/latest)에서 받을 수 있습니다.

- **Setup.exe** — 일반 사용자에게 권장합니다. 시작 메뉴 바로가기와 제거
  프로그램을 설치합니다.
- **x64.zip** — 포터블판입니다. 압축을 풀고
  `LowLatencyCaptureViewer.exe`를 실행하면 됩니다.

Windows 10/11 x64와 캡처보드 드라이버가 필요합니다.

## 빠른 사용법

1. OBS Studio, Elgato 4K Capture Utility 등 캡처보드를 사용 중인 다른
   프로그램을 종료합니다.
2. Low Latency Capture Viewer를 실행합니다.
3. 캡처 장치, 캡처 오디오 장치, 해상도, 포맷, 프레임과 오디오 출력 장치를
   확인합니다.
4. **시작**을 누릅니다.

처음에는 설정 창에 표시되는 권장값을 사용하면 됩니다. USB 캡처보드의 영상과
오디오가 별도 장치로 나타난다면 **캡처 오디오 장치**에서 대응하는 입력을
선택하세요.

## 주요 기능

- 오래된 프레임을 쌓지 않고 최신 프레임만 표시
- D3D11 Video Processor, flip-discard 표시, 최대 프레임 지연 1
- 저지연 표시 또는 VSync 선택
- WASAPI Shared/Exclusive와 `IAudioClient3` 저주기 출력 지원
- 장치별 해상도, NV12/YUY2/MJPEG와 프레임 자동 인식
- Pixel-perfect 1:1, 비율 고정 크기 조절, 보더리스 전체화면과 F5 복원
- 멀티 모니터 DPI 대응 창 크기 유지와 가장자리 스냅
- 볼륨 조절, 백그라운드 자동 음소거, 로그와 Tab 실시간 정보창

## 권장 시작 설정

| 항목 | 권장값 |
| --- | --- |
| 화면 표시 방식 | **저지연**. 찢어짐 방지가 더 중요하면 VSync |
| 오디오 출력 모드 | 호환성이 높은 **WASAPI Shared** |
| 출력 장치 | **Windows 기본 출력 장치 따라가기** |
| 캡처 포맷 | **자동 선택 / NV12 우선** |
| 캡처 프레임 | 입력 기기가 실제 출력하는 프레임과 동일하게 설정 |
| PCM 버퍼 목표 | **10ms**. underrun이 반복될 때만 높임 |
| 클록 드리프트 보정 | 기본은 **끔**. 분리된 영상·오디오 장치가 장시간 사용 중 어긋날 때 켬 |
| Pixel-perfect | 정확한 1:1은 켬, 자유로운 창 크기 조절은 끔 |

## 호환성

AVerMedia GC573에서 주로 개발·검증했습니다. 다른 DirectShow 캡처보드는
실험적으로 지원합니다.

- **영상:** progressive NV12 또는 YUY2. MJPEG는 실험적 호환 모드입니다.
- **오디오:** 영상 장치 또는 별도 DirectShow 입력의 48kHz 스테레오 PCM.
- **미지원:** H.264/AVC, MPEG-4, P010/HDR, 자동 장치 재연결.

선택한 해상도에서 30fps 이상의 NV12/YUY2가 없을 때만 MJPEG가 나타납니다.
또한 많은 캡처보드는 두 프로그램에서 동시에 열 수 없으므로 실행 전에 제조사
캡처 프로그램을 종료해야 합니다.

## 조작법

| 조작 | 기능 |
| --- | --- |
| `F2` | 뷰어를 안전하게 종료하고 설정 창 다시 열기 |
| `F5` | 정확한 1:1 크기 복원. 캡처와 모니터 해상도가 같으면 전체화면 사용 |
| `F11` | 보더리스 전체화면 전환 |
| `Tab` | 실시간 정보창 표시/숨기기 |
| `Esc` | 자동 전체화면은 바로 종료. F11 전체화면은 먼저 창 모드로 전환 |
| 마우스 휠 | 앱 음량을 5% 단위로 조절 |
| `Shift` + 드래그 | 가장자리 스냅을 일시적으로 무시 |

**다음 실행부터 바로 시작**을 켜면 이후에는 설정 창을 건너뜁니다. 실행할 때
**Shift**를 누르거나 뷰어에서 **F2**를 누르면 설정 창을 다시 열 수 있습니다.

## 자세한 설명

- [영상 포맷, 확대와 전체화면](docs/VIDEO.ko.md)
- [저지연 오디오](docs/AUDIO.ko.md)
- [실시간 정보창과 로그](docs/DIAGNOSTICS.ko.md)
- [문제 해결](docs/TROUBLESHOOTING.ko.md)
- [소스에서 빌드](docs/BUILDING.ko.md)

설정과 선택적 진단 로그는 `%LOCALAPPDATA%\LowLatencyCaptureViewer`에
저장됩니다. 제거할 때 사용자 데이터도 함께 삭제할지 선택할 수 있습니다.

## 라이선스

Copyright (C) 2026 seria-aa. 이 프로젝트는
[GNU General Public License v3.0 또는 이후 버전](LICENSE)으로 배포됩니다.
