# 릴리스 가이드

> [English](RELEASING.md) · [README로 돌아가기](../README.ko.md)

버전 표기, 빌드, 패키지와 GitHub 릴리스를 일관되게 유지하기 위한 체크리스트입니다.
릴리스 작업은 기존 `agent/release-v1.0.0` 브랜치에서 진행합니다.
아래 실행 명령은 모두 **v1.2.11**의 실제 예시이며 빌드 폴더는 현재 관례인
`build-v12110-release`를 사용합니다. 다른 버전은 버전·빌드 폴더·출력 경로를
함께 변경합니다. 빌드 폴더에 점이 있는 버전을 넣고 패키지 기본 경로는 그대로
두지 않습니다.

## 1. 버전 표기

Git 태그와 앱 버전 표시에는 `v1.2.11`을 사용합니다. 실행 파일 리소스와 설치
프로그램의 버전 필드는 `v` 없는 숫자를 사용합니다. 다음 위치의 버전을 맞춥니다.

- `CMakeLists.txt`의 `project(... VERSION ...)`
- `src/main.cpp`의 `kAppVersionLabel` (업데이트 User-Agent는 이 값에서 생성)
- `src/app.rc`의 `APP_VERSION_NUMBER` 및 `APP_VERSION_STRING`
- `installer/LowLatencyCaptureViewer.iss`의 앱 버전, 빌드 폴더, 출력 이름,
  `VersionInfoVersion`
- `tools/package-v1.ps1`의 기본 빌드·출력 경로
- `BUILD_INFO.txt`, `DEPENDENCIES.txt`, `실행안내.txt`의 현재 버전 머리말
- `docs/release-notes-v1.2.11.md`

변경 후 현재 버전 표기에 이전 문자열이 남지 않았는지 확인합니다.
과거 릴리스 노트와 변경 기록의 버전 번호는 그대로 보존합니다.

```powershell
rg -n "1\.2\.5\.1|1251" CMakeLists.txt src installer tools DEPENDENCIES.txt 실행안내.txt
```

## 2. 빌드와 검사

저장소 루트의 x64 Visual Studio 개발자 PowerShell에서 콘솔을 UTF-8로 설정합니다.
Release x64 빌드를 만들고 등록된 모든 테스트와 공백 검사를 통과해야 합니다.

```powershell
chcp.com 65001 > $null
cmake -S . -B build-v12110-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-v12110-release
ctest --test-dir build-v12110-release --output-on-failure
git diff --check
```

가능하면 실제 장치에서 Shared/ASIO, 선택한 캡처 포맷, Tab 진단창, F2 설정, 종료와
재실행도 짧게 확인합니다.
수행하지 못한 실제 장치 검사는 명시합니다. 자동 회귀 테스트와 시간 모의실험은
실제 장치 호환성이나 장시간 무중단 재생을 보장하지 않습니다.

## 3. 패키지

공개할 릴리스 자산은 아래 설치 파일과 포터블 ZIP 두 개입니다.

```text
LowLatencyCaptureViewer_v1.2.11_Setup.exe
LowLatencyCaptureViewer_v1.2.11_x64.zip
```

```powershell
chcp.com 65001 > $null
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-v1.ps1 `
  -Version 1.2.11 -BuildDir ..\build-v12110-release -OutputDir ..\outputs\v1.2.11
& "C:\Program Files\Inno Setup 7\ISCC.exe" `
  "--define=BuildDir=..\build-v12110-release" ".\installer\LowLatencyCaptureViewer.iss"
```

`settings.ini`, `%LOCALAPPDATA%` 로그, `build-*` 폴더, PDB, ILK와 테스트 실행
파일은 패키지에 넣지 않습니다. ZIP 안의 EXE와 설치 파일 버전이 `1.2.11` 또는
`1.2.11.0`인지 확인합니다. 파일명만 새 버전이고 내부 EXE는 구버전이면 안 됩니다.

두 패키지의 최상위에는 아래 ASIO 고지가 모두 있어야 합니다.

- `ASIO-SDK-LICENSE.txt`: 원본 `third_party/asio/LICENSE.txt`
- `ASIO-HOST-LICENSE.txt`: 원본 `third_party/asio/HOST-LICENSE.txt`

앱의 `LICENSE`도 필요합니다. 준비 폴더뿐 아니라 실제 ZIP을 검사합니다.
다음 읽기 전용 검사는 필수 고지 누락과 흔한 빌드·설정 파일 혼입을 확인합니다.

```powershell
Add-Type -AssemblyName System.IO.Compression.FileSystem
$releaseArchive = [IO.Compression.ZipFile]::OpenRead(
  (Resolve-Path '..\outputs\v1.2.11\LowLatencyCaptureViewer_v1.2.11_x64.zip').Path)
try {
  foreach ($required in @('LowLatencyCaptureViewer.exe', 'LICENSE',
      'ASIO-SDK-LICENSE.txt', 'ASIO-HOST-LICENSE.txt',
      'README.md', 'README.ko.md', 'DEPENDENCIES.txt', '실행안내.txt')) {
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

생성된 설치 파일의 내용 목록도 확인하여 두 ASIO 고지와 `LICENSE`가 들어 있는지
검증합니다. `cmake --install`은 별도 경로이므로 그것만 성공했다고 ZIP이나
Inno Setup의 구성까지 확인된 것은 아닙니다.

## 4. GitHub 공개

커밋 전에는 변경 파일과 스테이징 내용을 확인하고, 확인된 경로만 추가합니다.

```powershell
git status --short
git diff --cached --check
git commit -m "Prepare v1.2.11 release"
git push origin agent/release-v1.0.0
```

릴리스 노트를 사용해 브랜치 HEAD를 태그 대상으로 공개합니다. 베타가 명시된 경우에만
`--prerelease`를 추가합니다. **v1.2.11은 정식 릴리스**이므로 해당 옵션을 쓰지 않습니다.

```powershell
gh release create v1.2.11 `
  "..\outputs\v1.2.11\LowLatencyCaptureViewer_v1.2.11_Setup.exe" `
  "..\outputs\v1.2.11\LowLatencyCaptureViewer_v1.2.11_x64.zip" `
  --repo seria-aa/LowLatencyCaptureViewer `
  --target agent/release-v1.0.0 `
  --title "Low Latency Capture Viewer v1.2.11" `
  --notes-file ".\docs\release-notes-v1.2.11.md"
```

공개된 태그·자산은 덮어쓰지 않습니다. 문제가 생기면 다음 패치 버전으로 수정합니다.
