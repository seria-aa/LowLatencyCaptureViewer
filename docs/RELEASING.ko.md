<img align="right" src="images/app-icon.png" width="40" height="40" alt="LLCV">

# 릴리스 가이드

[README](../README.md) · [문서 목록](INDEX.ko.md) · [English](RELEASING.md)

[1. 버전 표기](#1-버전-표기) · [2. 빌드와 검사](#2-빌드와-검사) · [3. 패키지](#3-패키지) · [4. GitHub 공개](#4-github-공개)

버전 표기, 빌드, 패키지와 GitHub 릴리스를 일관되게 유지하기 위한 체크리스트입니다.
이번 준비에는 로컬 `release/v2.2.0` 브랜치를 사용합니다. 빌드·게시 전에 현재 브랜치를
확인하세요. 다른 복제본에서는 같은 이름의 브랜치를 먼저 만들거나 체크아웃합니다.
새 명명 규칙에 맞추려고 과거 감사 문서나 병합 기록의 브랜치 이름을 바꾸지 않습니다.
아래 실행 명령은 모두 **v2.2.0** 기준이며 빌드 폴더는 현재 관례인
`build-v2200-release`를 사용합니다. 다른 버전은 버전·빌드 폴더·출력 경로를
함께 변경합니다. 빌드 폴더에 점이 있는 버전을 넣고 패키지 기본 경로는 그대로
두지 않습니다.

<br>

## 1. 버전 표기

Git 태그와 앱 버전 표시에는 `v2.2.0`를 사용합니다. 실행 파일 리소스와 설치
프로그램의 버전 필드는 `v` 없는 숫자를 사용합니다. 다음 위치의 버전을 맞춥니다.

- `CMakeLists.txt`의 `project(... VERSION ...)`
- `src/main.cpp`의 `kAppVersionLabel` (업데이트 User-Agent는 이 값에서 생성)
- `src/app.rc`의 `APP_VERSION_NUMBER` 및 `APP_VERSION_STRING`
- `installer/LowLatencyCaptureViewer.iss`의 앱 버전, 빌드 폴더, 출력 이름,
  `VersionInfoVersion`
- `tools/package-v1.ps1`의 기본 빌드·출력 경로
- `BUILD_INFO.txt`, `DEPENDENCIES.txt`, `실행안내.txt`의 현재 버전 머리말
- `docs/release-notes-v2.2.0.md`
- 한영 README·가이드의 현재 버전과 변경 내용, 설정 예시 이미지·캡션

변경 후 현재 버전 표기에 이전 문자열이 남지 않았는지 확인합니다.
과거 릴리스 노트와 변경 기록의 버전 번호는 그대로 보존합니다.

```powershell
rg -n "2\.1\.9|build-v2190-release" CMakeLists.txt src installer tools DEPENDENCIES.txt 실행안내.txt
```

<br>

## 2. 빌드와 검사

릴리스 빌드 폴더에서는 LLCV_UPSCALING_DIAGNOSTIC과 LLCV_SPATIAL_PROTOTYPE을 끕니다.
오버플로 진단 계측과 수동 비교 도구는 일반 배포에 포함하지 않습니다.

배포 빌드는 `LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC=OFF`를 유지합니다. 보류한 자동 복구 시제품은
명시적으로 켠 별도 진단 빌드에서만 컴파일되며 실행 파일명·버전 표시도 구분됩니다.
설치 파일·포터블·CMake 설치 경로는 진단 빌드를 거부합니다. 진단 빌드 폴더에 남은 이전 정식 EXE도
재사용하지 마세요. 검증 기록은 `tests/RELEASE_V2_1_4_AUDIT.md`에서 확인합니다.

테스트·비공개 미리보기는 새로 빌드한 격리 프로필 버전을 사용하세요.
일반 앱을 옵션 없이 실행하면 실제 사용자 설정을 사용합니다.
현재 검증 범위는 `tests/RELEASE_V2_2_0_AUDIT.md`, 설정 분리 정책은
`tests/PROFILE_ISOLATION.md`에 기록합니다.

저장소 루트의 x64 Visual Studio 개발자 PowerShell에서 콘솔을 UTF-8로 설정합니다.
Release x64 빌드를 만들고 등록된 모든 테스트와 공백 검사를 통과해야 합니다.

```powershell
chcp.com 65001 > $null
if ((git branch --show-current) -ne 'release/v2.2.0') { throw 'release/v2.2.0 브랜치를 먼저 확인하세요.' }
.\tools\Build-RecordingFFmpeg.ps1
cmake -S . -B build-v2200-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DLLCV_RECORDING=ON -DLLCV_UPSCALING_DIAGNOSTIC=OFF -DLLCV_SPATIAL_PROTOTYPE=OFF -DLLCV_HDR_FRAME_AUDIT=OFF -DLLCV_HDR_SCRGB_PROTOTYPE=OFF -DLLCV_VSR_EXPERIMENT=OFF -DLLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC=OFF -DLLCV_RECORDING_FFMPEG_ROOT="$PWD/outputs/dependencies/recording-ffmpeg/install"
cmake --build build-v2200-release
ctest --test-dir build-v2200-release --output-on-failure
git diff --check
```

가능하면 실제 장치에서 Shared/ASIO, 선택한 캡처 포맷, 진단창·설정(기본 Tab/F2),
단축키 변경·기본값 복원, 종료와 재실행도 짧게 확인합니다.
수행하지 못한 실제 장치 검사는 명시합니다. 자동 회귀 테스트와 시간 모의실험은
실제 장치 호환성이나 장시간 무중단 재생을 보장하지 않습니다.

<br>

## 3. 패키지

공개할 릴리스 자산은 아래 설치 파일과 포터블 ZIP 두 개입니다.

```text
LowLatencyCaptureViewer_v2.2.0_Setup.exe
LowLatencyCaptureViewer_v2.2.0_x64.zip
```

```powershell
chcp.com 65001 > $null
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-v1.ps1 `
  -Version 2.2.0 -BuildDir ..\build-v2200-release -OutputDir outputs\v2.2.0
& "C:\Program Files\Inno Setup 7\ISCC.exe" `
  "--define=BuildDir=..\build-v2200-release" ".\installer\LowLatencyCaptureViewer.iss"
```

`settings.ini`, `%LOCALAPPDATA%` 로그, `build-*` 폴더, PDB, ILK와 테스트 실행
파일은 패키지에 넣지 않습니다. ZIP 안의 EXE와 설치 파일 버전이 `2.2.0` 또는
`2.2.0.0`인지 확인합니다. 파일명만 새 버전이고 내부 EXE는 구버전이면 안 됩니다.

업스케일링 실험 보고서·비교 이미지와 셰이더 소스·수동 테스트 도구는 사용자 패키지에서 제외합니다.
정식 NIS·CMAA2 셰이더는 실행 파일에 포함하고 아래 라이선스 고지는 함께 배포합니다.

두 패키지의 최상위에는 아래 외부 구성요소 고지가 모두 있어야 합니다.

- `ASIO-SDK-LICENSE.txt`: 원본 `third_party/asio/LICENSE.txt`
- `ASIO-HOST-LICENSE.txt`: 원본 `third_party/asio/HOST-LICENSE.txt`
- `ffmpeg-licenses/`: COPYING.LGPLv2.1, LICENSE.md, recording-ffmpeg-NOTICE.txt, config.mak
- `avformat-62.dll`, `avcodec-62.dll`, `avutil-60.dll`: pinned mux runtime
- `PRETENDARD-LICENSE.txt`: 원본 `third_party/pretendard/LICENSE.txt`
- `NIS-LICENSE.txt`, `CMAA2-LICENSE.txt`, `UPSCALING-NOTICE.txt`: `third_party/upscaling`의 사용 구성요소 고지

앱의 `LICENSE`도 필요합니다. 준비 폴더뿐 아니라 실제 ZIP을 검사합니다.
다음 읽기 전용 검사는 필수 고지 누락과 흔한 빌드·설정 파일 혼입을 확인합니다.

```powershell
Add-Type -AssemblyName System.IO.Compression.FileSystem
$releaseArchive = [IO.Compression.ZipFile]::OpenRead(
  (Resolve-Path 'outputs\v2.2.0\LowLatencyCaptureViewer_v2.2.0_x64.zip').Path)
try {
  foreach ($required in @('LowLatencyCaptureViewer.exe', 'LICENSE',
      'ASIO-SDK-LICENSE.txt', 'ASIO-HOST-LICENSE.txt', 'PRETENDARD-LICENSE.txt',
      'NIS-LICENSE.txt', 'CMAA2-LICENSE.txt', 'UPSCALING-NOTICE.txt',
      'README.md', 'README.ko.md', 'README.en.md', 'DEPENDENCIES.txt', '실행안내.txt')) {
    if ($null -eq $releaseArchive.GetEntry($required)) {
      throw "Missing package file: $required"
    }
  }
  $unexpected = $releaseArchive.Entries | Where-Object {
    $_.FullName -match '(^|[\\/])(settings\.ini|logs|build|build-[^\\/]+)([\\/]|$)|\.(pdb|ilk|obj|log)$|(^|[\\/])[^\\/]*Tests\.exe$'
  }
  if ($unexpected) { throw "Unexpected package files: $($unexpected.FullName -join ', ')" }
} finally {
  $releaseArchive.Dispose()
}
```

생성된 설치 파일의 내용 목록도 확인하여 ASIO·Pretendard 고지와 `LICENSE`가 들어 있는지
검증합니다. `cmake --install`은 별도 경로이므로 그것만 성공했다고 ZIP이나
Inno Setup의 구성까지 확인된 것은 아닙니다.

<br>

녹화 DLL·고지·필수 문서와 실행 파일 버전은 `tools/Test-ReleasePackage.ps1`로 추가 검증합니다.
배포 ZIP을 별도 임시 폴더에 풀어 이 검사를 다시 실행하고, 설치 파일의 manifest에서도 같은 DLL·고지를 확인하세요.

## 4. GitHub 공개

커밋 전에는 변경 파일과 스테이징 내용을 확인하고, 확인된 경로만 추가합니다.

```powershell
git status --short
git diff --cached --check
git commit -m "Prepare v2.2.0 release"
git push origin release/v2.2.0
```

릴리스 노트를 사용해 브랜치 HEAD를 태그 대상으로 공개합니다. 베타가 명시된 경우에만
`--prerelease`를 추가합니다. **v2.2.0는 정식 릴리스**이므로 해당 옵션을 쓰지 않습니다.

```powershell
gh release create v2.2.0 `
  "outputs\v2.2.0\LowLatencyCaptureViewer_v2.2.0_Setup.exe" `
  "outputs\v2.2.0\LowLatencyCaptureViewer_v2.2.0_x64.zip" `
  --repo seria-aa/LowLatencyCaptureViewer `
  --target release/v2.2.0 `
  --title "LLCV v2.2.0" `
  --notes-file ".\docs\release-notes-v2.2.0.md"
```

공개된 태그·자산은 덮어쓰지 않습니다. 문제가 생기면 다음 패치 버전으로 수정합니다.

---

[README](../README.md) · [문서 목록](INDEX.ko.md) · [English](RELEASING.md)
