# UYVY / HDYC 지원과 영상 포맷 진단

## 미지원 포맷 진단

설정의 영상 모드 조회 결과를 다음과 같이 구분한다.

- 조회 실패: 기존 HRESULT와 새로고침 안내를 유지한다. 일부 항목만 조회에 실패해도 미지원 포맷만 있다고 단정하지 않는다.
- 조회 성공·선택한 해상도의 모드 없음: 다른 장치 또는 해상도 선택 안내.
- 선택한 해상도에서 미지원 픽셀 포맷만 발견: 한국어/영어로 미지원 포맷 이름을 안내한다. 다른 해상도의 포맷으로 현재 해상도를 판정하지 않는다.

`ProbePixelFormats`에서 제외한 항목의 FourCC(또는 GUID), 해상도, 원래 프레임 간격과 제외 이유를 기록한다.
이 조회는 설정 창에서만 실행되며 재생 프레임 처리에 로그·검사·대기를 추가하지 않는다.
UI에는 최대 세 가지 이름을 표시하고 전체 목록은 진단 로그에 남긴다.

### A1 검증

2026-10-26: `DirectShowVideoFormatTests`, `VideoFormatFaults`, `VideoLayoutStress`,
`SettingsModeCacheTests`, `SettingsControllerTests` **5/5 통과**(10.79초).
가짜 `IAMStreamConfig`로 미지원 UYVY/HDYC 목록, 혼합 목록, 빈 목록, 다른 해상도,
잘못된 헤더, 조회 실패, GUID 식별을 검사했다. 숨겨진 설정 창에서 한국어/영어 안내,
시작 차단·오디오 전용 예외·새로고침 후 상태 초기화를 확인했다.
일반 앱과 프로필 격리 진단 앱 모두 빌드했다. 사용자 설정을 읽거나 변경하지 않았다.
분리 보존한 A1 실행 파일: `outputs/uyvy-support/a1-diagnostics/LLCV_FormatDiagnostic.exe`.
이 파일은 UYVY 지원을 추가하기 전의 진단 단계이며 일반 배포 대상이 아니다.

## 알려진 별도 과제: 인터레이스와 소수 프레임 표시

현재 설정 목록의 `PixelFormatSupport`는 포맷과 정수 FPS만 보관한다. 예를 들어 프레임 간격
333667(100ns)이면 약 29.97fps를 30으로 표시하며, 인터레이스 플래그를 목록의 식별자로 쓰지 않는다.
따라서 1080i59.94와 1080p29.97이 같은 ‘1080 / 30fps’ 항목으로 합쳐질 수 있다.
연결 후 `VideoSampleFormat`은 인터레이스 플래그를 보관하고 녹화는 인터레이스 입력을 거절한다.
이번 작업은 이 목록 표현이나 재생 디인터레이싱 정책을 변경하지 않는다. 별도 설계·검증 대상이다.

## UYVY/HDYC 처리

- 설정 값은 기존 문자열을 유지하고 `UYVY`를 추가한다. `HDYC`도 읽을 수 있으며 저장은 `UYVY`로 통일한다. 기존 enum 값은 변경하지 않는다.
- 자동 선택은 NV12 → YUY2 → UYVY 순서다. MJPEG와 P010은 기존처럼 수동 선택한다.
- HDYC의 기본 행렬은 BT.709다. 기존 raw-SDR 기본 판정을 이용하며 가짜 장치 메타데이터를 만들지 않는다. 그래야 협상에서 받은 BT.601 등 명시적 정보가 연결 과정에서 사라져도 유지된다. 실제 UYVY/HDYC 서브타입이 다른 후보의 색 정보는 가져오지 않는다. 활성 형식의 명시적 정보는 필드별로 우선하며, 행렬·범위 중 생략된 필드는 같은 서브타입에서 받은 정보로 유지한다. 사용자 SDR 범위가 가장 우선하며 MJPEG 전용 행렬 설정은 적용하지 않는다.
- UYVY 원본을 3개 업로드 슬롯에 올린 다음 compute 한 번으로 YUY2 바이트 순서로 바꾼다. 같은 텍스처를 다시 표시할 때는 변환하지 않는다. 새 CPU 변환·복사·대기·프레임 큐를 넣지 않는다. UYVY에서만 슬롯별 입력/출력 GPU 텍스처를 분리하며, 추가 GPU 원시 데이터 크기는 FHD 약 11.9 MiB, 4K 약 47.5 MiB다(드라이버 정렬 비용 제외).
- 기존 YUY2 비디오 프로세서, VSR, CMAA2 → NIS, OSD 경로를 사용한다. 원본 스크린샷은 원래 UYVY 바이트를 기존 PNG 작업 스레드에서 직접 해석한다.
- 녹화는 원본 샘플을 사용하므로 UYVY/HDYC 입력을 시작 전에 차단한다. 한·영 안내를 제공하며 파일이나 녹화 스레드를 만들지 않는다. NV12/YUY2/MJPEG 녹화는 유지한다.
- GPU의 정수 텍스처 SRV 읽기, YUY2 텍스처 UAV 쓰기와 비디오 프로세서 지원을 확인한다. 지원하지 않으면 영상 시작을 중단하고 안내한다.
- DirectShow 드라이버가 UYVY/HDYC 형식을 제공해야 한다. 별도 DeckLink SDK, XRGB/RGB32, v210 입력 지원은 추가하지 않았다.

Microsoft의 [YUY2 뷰 규칙](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format)과
[typed UAV load 지원](https://learn.microsoft.com/en-us/windows/win32/direct3d11/typed-unordered-access-view-loads)에 따라
원래는 R32_UINT in-place 읽기·쓰기를 검토했다. 합성 검사에서 YUY2 UAV의 `GetDimensions`가
packed word 수가 아닌 전체 픽셀 너비를 반환함을 확인했고, 너비를 보정해도 RTX 3080에서
일부 행의 in-place 처리 결과가 일치하지 않았다. 이 경로는 제품에서 제거했다.
최종 구현은 일반 R8G8B8A8_UINT 입력 SRV를 읽어 별도 YUY2 출력의 R8G8B8A8_UINT UAV에
쓰기만 한다. UAV typed load가 필요 없고 변환은 여전히 단일 GPU 패스다.

## 재현 가능한 검사

기존 Windows/MSVC 빌드 안내에 따라 `BUILD_TESTING=ON`, `LLCV_RECORDING=ON`으로
구성한다. 업스케일링 진단 검사까지 포함하려면 `LLCV_UPSCALING_DIAGNOSTIC=ON`도 사용한다.
일반 앱을 실행할 필요 없이 가짜 DirectShow 핀과 합성 GPU 프레임으로 검사한다.

```text
cmake --build build-upscaling-safety
ctest --test-dir build-upscaling-safety -R "^(DirectShowVideoFormatTests|AudioCallbackTests|VideoFormatFaults|VideoLayoutStress|UyvyRepackerHardware|UyvyRepackerWarp|CmaaNisRegression|VsrExperimentTests|ScreenshotTests|RecordingStateTests|RecordingStatusTests)$" --output-on-failure
```

- GPU 검사는 2×1부터 3840×2160까지, 패딩 stride·모서리·홀수 높이·3개 슬롯·9프레임 반복을 포함한다. 변환된 모든 유효 바이트를 동등한 YUY2 입력과 비교하고 CPU/GPU 원본 보존도 검사한다.
- 앱 통합 검사는 세 표시 경로, BT.601/BT.709, Full/Limited, 재업로드 없는 재표시, 업스케일링·OSD·원본 스크린샷을 포함한다.
- 색 정보 검사는 UYVY↔HDYC 교체, 후보/활성 형식의 정보 생략·부분 선언·우선순위, 30/40/60fps와 기존 VideoInfo 헤더를 포함한다.
- 녹화 거부 검사는 전용 한·영 안내, 파일·작업 스레드 미생성, 기존 파일 보존을 검사한다. 기존 NV12/YUY2/MJPEG 녹화와 HDR 출력은 별도 회귀로 확인한다.

읽기 대기와 GPU readback은 테스트에만 있다. 이 검사로 특정 캡처보드의 실제 DirectShow
노출 형식이나 HDMI 전체 지연을 확인했다고 해석하지 않는다. Intensity Pro 4K 실기는 사용하지 않았다.

## 최종 검증 결과 (2026-10-26)

일반 앱·전체 기본 빌드 대상과 별도 프로필 격리 진단 앱을 빌드했다.
첫 in-place 구현에서는 GPU/렌더러 검사 3개가 실패했고, 원본 업로드와 변환 결과를
따로 확인한 뒤 입력/출력 표면을 분리했다. 오차 허용치를 늘려 통과시키지 않았다.

최종 **80/80 통과**(416.38초). 이어 UYVY의 VSR 전환 검사를 추가해 해당 테스트를
다시 빌드하고 **1/1 통과**(3.87초)했다. 기존 19개 업스케일링 관련 검사와 새 보고서
오류 검사는 이 목록에 포함된다.

| 범위 | 확인 내용 |
|---|---|
| 포맷·색 정보 | NV12→YUY2→UYVY 자동 선택, HDYC 기본709, 명시적 색 정보와 사용자 범위 우선, alias 사이 색 정보 격리, 30/40fps·오디오 전용 시작 |
| GPU 정확도 | 하드웨어 RTX 3080·WARP, 4K 포함 YUY2 전 바이트 일치, 입력 보존·패딩·반복 프레임 |
| 설정·UI·단축키 | 구버전 설정, 한·영 미지원 안내, 5개 포맷 목록·높은 DPI·간격, 기존 키 지정·F6·설정 취소 |
| 표시·스크린샷 | SDR/HDR 출력 경로, VSR·CMAA2→NIS·OSD, 원본 PNG의 행렬·범위 일치 |
| 녹화 | 기존 NV12/YUY2/MJPEG 색상, 음소거·오디오 장치 전환·일시정지·분할, HEVC 싱크, HEVC/AV1 강제 종료 복구, UYVY 녹화 사전 차단 |
| 격리 | 테스트 프로필, 일반 빌드의 진단 코드 제외, 컴파일 의존성·선택 빌드 대상 |

VSR 검사는 요청·전환·OFF 복원·리소스 유지·OSD를 확인한다. 드라이버의 실제 화질 향상
활성 여부를 S_OK만으로 단정하지 않는다. HDR·녹화 싱크 검사는 합성 입력 기반이다.
원시 결과는 커밋하지 않는 `outputs/uyvy-support/verified-regression-tests.log`와
`uyvy-vsr-tests.log`에 있으며, 재현할 테스트 소스와 요약은 저장소에 남긴다.

### 성능 비교와 설정 보존

RTX 3080에서 FHD UYVY 바이트 재배열만 5라운드×120회 측정한 GPU 평균은
라운드별 **0.0134~0.0207ms**였다. 업로드·비디오 처리·표시를 제외한 단일 compute 비용이다.

UYVY 추가 직전 빌드와 최종 빌드의 기존 NV12 경로를 같은 합성 FHD→QHD 입력으로
교차 비교했다. 빌드별 ON/OFF 각 12라운드, 라운드마다 25프레임 준비 후 120프레임 측정했다.
첫 묶음은 이전→최종→최종→이전, 둘째 묶음은 최종→이전→최종→이전 순서다.

| GPU 처리 시간 | 추가 전 평균 (라운드 평균 범위) | 최종 평균 (라운드 평균 범위) |
|---|---:|---:|
| 필터 끔 | 0.292ms (0.257~0.349) | 0.316ms (0.248~0.437) |
| CMAA2 → NIS 0.15 | 0.904ms (0.827~1.078) | 0.911ms (0.773~1.081) |

첫 묶음에서는 최종 빌드 평균이 높았지만, 순서를 바꾼 둘째 묶음에서는 같은 증가가
반복되지 않았다. 전체 평균 차이는 OFF +0.024ms, ON +0.007ms이며 라운드 변동 범위가
겹친다. 이 측정에서 일관된 저하를 확인하지 못했지만 정확히 무비용이라고 단정하지 않는다.
NV12/YUY2/P010에는 새 변환 표면이나 compute dispatch를 추가하지 않았다.
기존 타이머의 출력 완료 확인용 GPU 1픽셀 복사 비용은 양쪽에 포함되며, CPU 업로드와
HDMI부터 모니터까지의 총지연은 이 표의 측정 범위가 아니다.
원시 자료: `outputs/uyvy-support/repack-benchmark.log`, `perf-before-1..4/`, `perf-after-1..4/`.

일반 앱은 빌드만 했으며, 실행한 테스트/진단 코드는 별도 임시 프로필을 사용한다.
실제 설정 및 기존 릴리스 파일의 SHA-256은 작업 전후 동일했다.

- `settings.ini`: `9F43DF235D5AF61C853D318DB2F01CD71788639DC5361D72A46D97E4D8C599D0`
- `recording-prototype.ini`: `2D1C8F5D136F8A812E78C5558C32C9BC690751923AAB20D8A77A0BEB144C406A`
- 기존 v2.1.7 실행 파일: `B4A7CF19B999C7A9E21E4C0514490ABC4ACC53C0A4B5192FE520418204FE92E2`
