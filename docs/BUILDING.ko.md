<img align="right" src="images/app-icon.png" width="40" height="40" alt="LLCV">

# 소스에서 빌드

[README](../README.md) · [문서 목록](INDEX.ko.md) · [English](BUILDING.md)

[필요 환경](#필요-환경) · [Release 빌드](#release-빌드) · [설치 프로그램과 포터블 ZIP](#설치-프로그램과-포터블-zip)

<br>

## 필요 환경

- Windows 10/11 x64
- **C++를 사용한 데스크톱 개발**이 설치된 Visual Studio 2022
- Windows SDK(셰이더 빌드용 fxc 포함)와 Windows용 CMake 도구
- FFmpeg 저장 라이브러리 빌드용 Git for Windows (Git Bash), curl·tar
- 설치 프로그램을 만들 때만 Inno Setup 7

Win32, DirectShow, D3D11, DXGI, MJPEG 디코딩용 Media Foundation과
WASAPI 등 Windows 시스템 API를 사용합니다. 녹화에는 고정 버전 FFmpeg 저장 전용
공유 라이브러리가 필요합니다. 인코딩은 Media Foundation을 사용하며 FFmpeg 실행 파일이나 코덱 팩은 필요하지 않습니다.

<br>

## Release 빌드

저장소 루트에서 x64 Visual Studio 개발자 PowerShell을 열고 실행합니다.

```powershell
chcp.com 65001 > $null
.\tools\Build-RecordingFFmpeg.ps1
cmake -S . -B build -A x64 -DLLCV_RECORDING_FFMPEG_ROOT="$PWD/outputs/dependencies/recording-ffmpeg/install"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

실행 파일은 다음 위치에 생성됩니다.

```text
build\Release\LowLatencyCaptureViewer.exe
```

Release 구성은 정적 MSVC 런타임을 사용하므로 패키지 실행에 별도 Visual C++
재배포 패키지가 필요하지 않습니다.

2.1.8에는 NIS 가장자리·CMAA2 목록 안전성, UYVY/HDYC 협상·색 정보·GPU 변환과
녹화 차단·원본 스크린샷 검증을 추가했습니다. 기존 설정·단축키·재생·녹화 검사는 유지합니다.
검증 범위는 tests/RELEASE_V2_1_8_AUDIT.md를 참고하세요.

2.1.9에는 RGB 형식·방향·GPU 재배열·원본 PNG·VSR 경로·녹화 색·부하·메모리 검사와
파일 미생성 거부·복구 판정·패키지 제외 회귀를 추가했습니다.
해당 검증 범위는 tests/RELEASE_V2_1_9_AUDIT.md에 있습니다.

2.2.0에는 진단자료 ZIP·개인정보 가림, 도움말 배치, 설정 조회 수명·시작 대기 제한,
녹화 설정 항목별 복원, VSync 준비 대기와 크로스바 연결 안전장치 관련 검사를 보완했습니다.
2.2.1에는 SDR 보정 선택, HDR NIS 윤곽 보호, 설정 키보드·읽기 순서와
현재 버전 릴리스 노트 링크의 오프라인 검증을 추가했습니다.
현재 준비 상태와 남은 릴리스 게이트는 tests/RELEASE_V2_2_1_AUDIT.md에 기록합니다.
이전 개발 중 검사를 최종 릴리스 검증 통과로 간주하지 않습니다.

NIS·CMAA2·UYVY·RGB24 변환 셰이더는 빌드할 때 실행 파일에 포함합니다. 실행 시 별도 셰이더 컴파일러나
소스 파일은 필요하지 않습니다. NIS/CMAA2 소스·수정 내역과 라이선스는 third_party/upscaling에,
UYVY 변환 셰이더는 src/video에 있습니다.
셰이더별 의존성을 구분하므로 NIS 변경만으로 CMAA2까지 다시 빌드하지 않습니다.

SettingsPreview, RecordingIndicatorPreview, RecordingQualityBenchmark는 기본 빌드에서 제외한
수동 도구입니다. 필요할 때 해당 이름을 cmake --build의 --target으로 지정하세요.
자동 회귀 테스트는 기존대로 기본 빌드에 포함합니다.

릴리스 빌드는 LLCV_UPSCALING_DIAGNOSTIC=OFF로 둡니다. 이 옵션을 켜면 별도 프로필을
쓰는 오버플로 진단 대상만 추가되며, 일반 앱에는 GPU 상태를 읽어오는 진단 코드를 넣지 않습니다.

테스트와 비공개 미리보기는 매 실행마다 별도 임시 프로필을 사용합니다.
실제 설정을 불러오거나 저장하지 않으며, 일반 앱의 기본 실행은 기존 사용자 설정을 사용합니다.
수동 검증에는 새로 빌드한 테스트 도구를 사용하세요. 자세한 범위는 tests/PROFILE_ISOLATION.md에 있습니다.

기존 검사에는 기본 설정 단축키, 열린 녹화 설정창에서의 전환 취소, 녹화 입력 장치 변경·큰 오디오 묶음,
종료 직전 입력 보존, 잘못된 숫자 설정과 오디오 리샘플링 결과 동일성 회귀 검사를 추가했습니다.
취소 검사는 숨겨진 창·합성 입력·모의 표시 전환 응답을 사용하며 실제 독점 화면 전환이나 지연을 측정하지 않습니다.
기존 재생 음소거·녹화 소리 분리, 단축키 이전·복원과 한국어·영어 고배율 UI 검사도 유지합니다.
기존 검사에는 별도 프로세스의 인코더 확인 취소·실패, 녹화 중 남은 공간 갱신,
최종 MJPEG 연결 색 정보, 녹화 시작 오디오 겹침 처리, 설정 단축키와 최소화 창 위치
회귀 검사가 있습니다.
MJPEG 녹화·녹화 음량·30fps 직접 요청, 녹화 시작·종료·설정·색 정보와
파일 분할·일시정지·저장 지연·강제 종료 파일 보존·독점 전체화면·영상 배치 회귀 검사가 포함됩니다.
기존 40fps 형식 협상·색 정보 보존·설정 페이지·장치 전환·저장 실패·스트림 종료·
절전 요청 수명 검사도 포함됩니다.
일부 파일 저장 테스트는 쓰기·이름 변경이 가능한 임시 폴더가
필요합니다. GPU나 Windows 기능이 없어 건너뛴 검사는 통과한 검사와 구분해 기록하세요.
절전 API 검사는 PC를 실제로 재우지 않으며, 실제 절전·복귀 검증을 대신하지 않습니다.

독립 디코딩·분할·싱크 검사는 Python 3과 테스트용 ffmpeg/ffprobe도 필요합니다.
PATH에 두거나 CMake의 `LLCV_TEST_FFMPEG`·`LLCV_TEST_FFPROBE`로 지정하세요.
없으면 해당 검사가 등록되지 않으므로 전체 검증을 완료했다고 간주하지 않습니다.
이 도구들은 앱 배포본에 넣지 않습니다.

<br>

재생 전용 개발 빌드는 `-DLLCV_RECORDING=OFF`로 FFmpeg 없이 만들 수 있지만 2.2.1 배포본과는 다릅니다.
실행 파일 옆의 저장 라이브러리 DLL과 `ffmpeg-licenses` 폴더를 패키지에 유지하세요.

보류한 오디오 입력 자동 복구 시제품은 **기본 꺼짐**입니다. 비공개 진단이 필요할 때만 별도 빌드 폴더에서
`-DLLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC=ON`을 지정하세요.
진단용 버전 표시가 붙은 `LowLatencyCaptureViewer_AudioRecovery_Diagnostic.exe`가 생성됩니다.
일반 빌드에는 입력 감시·재시도 코드가 들어가지 않으며 기존 WASAPI·ASIO 출력 복구는 유지합니다.
진단 옵션은 HDR·VSR 실험과 함께 켤 수 없고 정식 설치 파일·포터블 패키지로 만들 수 없습니다.
캡처보드 무음 문제를 해결했다고 확인된 기능은 아닙니다.

## 설치 프로그램과 포터블 ZIP

Release 빌드 뒤 저장소 루트에서 아래 명령을 실행합니다. 다음은 위에서 만든
`build\Release` 실행 파일을 v2.2.1으로 패키징하는 예시입니다. 릴리스 스크립트의
기본 경로는 버전별 빌드 폴더이므로 아래 빌드 경로 재정의가 필요합니다.

```powershell
chcp.com 65001 > $null
& "C:\Program Files\Inno Setup 7\ISCC.exe" `
  "--define=BuildDir=..\build\Release" ".\installer\LowLatencyCaptureViewer.iss"
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-v1.ps1 `
  -Version 2.2.1 -BuildDir ..\build\Release -OutputDir outputs\v2.2.1
```

생성 파일은 `outputs\v2.2.1\LowLatencyCaptureViewer_v2.2.1_Setup.exe`와
`outputs\v2.2.1\LowLatencyCaptureViewer_v2.2.1_x64.zip`입니다. 다른 버전은
먼저 릴리스 관련 버전 표기를 맞춰야 합니다. ZIP 이름만 바꿔도 실행 파일 버전이
바뀌지는 않습니다. 자세한 절차는 [릴리스 체크리스트](RELEASING.ko.md)를 따릅니다.

두 패키지에는 `LICENSE`, `ASIO-SDK-LICENSE.txt`, `ASIO-HOST-LICENSE.txt`, `PRETENDARD-LICENSE.txt`,
`NIS-LICENSE.txt`, `CMAA2-LICENSE.txt`, `UPSCALING-NOTICE.txt`가
모두 있어야 합니다. 컴퓨터별 `settings.ini`, 진단 로그, 테스트 실행 파일,
PDB와 빌드 폴더는 포함하지 않습니다. 자동 테스트는 실제 장치나 장시간 재생
검증을 대신하지 않으므로 실제 수행한 하드웨어 검사를 별도로 기록합니다.

실행 중 설정과 로그는 사용자별 다음 폴더에 저장됩니다.

```text
%LOCALAPPDATA%\LowLatencyCaptureViewer
```

이전 버전의 실행 파일 옆에 `settings.ini`가 있으면 첫 실행 때 사용자 폴더로
복사하며, 기존 파일은 자동으로 삭제하지 않습니다.

---

[README](../README.md) · [문서 목록](INDEX.ko.md) · [English](BUILDING.md)
