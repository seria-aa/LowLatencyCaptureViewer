# Low Latency Capture Viewer

> [English](README.md) · [최신 릴리스](https://github.com/seria-aa/LowLatencyCaptureViewer/releases/latest)

Windows x64용 초저지연 HDMI 캡처 뷰어입니다. DirectShow 캡처카드에서 영상과
오디오를 직접 받아 D3D11로 영상을 표시하고 WASAPI로 오디오를 출력합니다.
AVerMedia GC573를 기준으로 개발·검증했으며, 비압축 NV12/YUY2를 제공하는 다른
DirectShow 캡처카드는 실험적으로 지원합니다.

mpv, FFmpeg, 디코더, 별도 실행 파일은 필요하지 않습니다.

## 주요 기능

- 영상·오디오를 하나의 DirectShow 그래프에서 캡처
- 최신 프레임만 표시하고 지연을 만드는 오래된 프레임은 버림
- D3D11 Video Processor, flip-discard 스왑체인, 최대 프레임 지연 1
- 저지연 즉시 표시 또는 VSync 선택
- WASAPI Shared(호환성 우선) / Exclusive(장치 독점·지연 최소화) 출력
  및 기본 장치 추적 또는 특정 장치 고정
- PCM 안전 대기량과 클록 드리프트 보정을 독립적으로 설정
- 캡처카드가 실제 지원하는 해상도·픽셀 포맷·프레임 선택
- 픽셀 퍼펙트 1:1, 비율 고정 창 크기 조절, 멀티 모니터 DPI, 가장자리 스냅
- Tab 실시간 정보창, 선택적 진단 로그, 볼륨 HUD, 백그라운드 자동 음소거

## 요구 사항

- Windows 10/11 x64
- DirectShow 캡처카드 드라이버
- progressive 비압축 NV12 또는 YUY2 영상 모드
- 같은 캡처 필터의 48 kHz 스테레오 PCM 오디오 핀
- Microsoft Visual C++ 2015–2022 Redistributable x64

GC573가 테스트·권장 장치입니다. 다른 장치는 실험적이며, 압축 포맷, P010,
별도 USB 오디오 필터, 자동 장치 재연결은 지원하지 않습니다.

## 설치 및 실행

1. [Releases](https://github.com/seria-aa/LowLatencyCaptureViewer/releases)에서 `LowLatencyCaptureViewer_v*_x64.zip`을 받습니다.
2. 쓰기 가능한 폴더에 압축을 풉니다.
3. `LowLatencyCaptureViewer.exe`를 실행하고 캡처·오디오 옵션을 선택합니다.

설정은 실행 파일 옆의 `settings.ini`에 저장됩니다. 진단 로그를 켜면 `logs`
폴더에 기록됩니다.

## 권장 시작 설정

| 항목 | 권장 시작값 |
| --- | --- |
| 화면 표시 방식 | 최저 표시 지연은 저지연 즉시 표시. 화면 찢어짐을 막으려면 VSync. |
| 오디오 모드 | Shared는 시스템과 공유해 호환성이 높고, Exclusive는 장치를 독점해 지연을 줄일 수 있습니다. |
| 출력 장치 | 고정 장치가 꼭 필요하지 않으면 Windows 기본 출력 장치 따라가기. |
| 클록 드리프트 보정 | 원본 PCM을 유지하려면 끔. 장시간 사용 중 관련 오류가 반복될 때만 켬. |
| PCM 버퍼 목표 | 최소 지연은 10 ms. underrun이 반복될 때만 올림. |
| 영상 포맷 | 자동/NV12 우선. 120 fps가 유지되지 않으면 60 fps 선택. |

`Pixel-perfect`를 켜면 선택한 캡처 해상도와 같은 1:1 클라이언트 영역으로
고정되고 마우스 크기 조절이 꺼집니다. 해제하면 영상 비율을 유지한 채 창 크기를
조절할 수 있습니다. 모니터 이동 시 상대적 창 크기 유지는 독립 옵션이므로, 다른
크기의 모니터로 옮긴 뒤에는 1:1 크기가 바뀔 수 있습니다.

## 조작법

| 조작 | 기능 |
| --- | --- |
| `F11` | borderless 전체화면 전환 |
| `Tab` | 실시간 정보창 표시/숨기기 |
| 영상 위 마우스 휠 | 애플리케이션 볼륨을 5% 단위로 조절 |
| 창을 가장자리·모서리로 드래그 | 창 크기 변경 없이 스냅 |
| `Shift` + 드래그 | 일시적으로 스냅 무시 |
| `Esc` | 전체화면 해제. 창 모드에서 다시 누르면 종료 |

## 실시간 정보창(OSD)

Tab 정보창에는 입력/Present FPS, 앱 처리 시간, 버린 프레임, 실제 선택된 포맷,
WASAPI 버퍼 상태, PCM 대기량, 클록 보정, underrun/overrun 횟수가 표시됩니다.
통계는 시작 후 2초 워밍업 뒤부터 집계하지만, 캡처와 오디오 출력은 즉시 시작됩니다.

ppm 값은 하드웨어 클록을 직접 측정한 결과가 아니라 스케줄링 지연과 시작 구간도
포함한 동작상 추정값입니다. 클록 보정 여부는 10~30분 관찰 후 판단하세요.

OSD 용어:

- **출력 버퍼**: WASAPI 장치 버퍼
- **출력 점유**: 그 장치 버퍼에 현재 들어 있는 오디오 양
- **PCM 버퍼**: 캡처와 재생 사이의 프로그램 내부 안전 큐

가끔 한 번 발생한 underrun은 곧바로 소리 문제를 뜻하지 않습니다. 반복된다면,
원본 PCM 보존이 우선일 때는 리샘플링보다 먼저 PCM 버퍼 목표를 올리세요.

## 소스에서 빌드

Visual Studio 2022 x64 개발자 PowerShell에서 실행합니다.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

생성 파일은 `build\Release\LowLatencyCaptureViewer.exe`입니다.

## 라이선스

Copyright (C) 2026 seria-aa. 이 프로젝트는
[GNU General Public License v3.0 또는 이후 버전](LICENSE)으로 배포됩니다.
Windows 구성요소, Visual C++ Redistributable, 캡처카드 드라이버는 별도 구성요소이며
각자의 라이선스를 따릅니다.
