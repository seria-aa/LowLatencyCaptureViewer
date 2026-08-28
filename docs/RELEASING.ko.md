# 릴리스 가이드

> [English](RELEASING.md) · [README로 돌아가기](../README.ko.md)

버전 표기, 빌드, 패키지와 GitHub 릴리스를 일관되게 유지하기 위한 체크리스트입니다.
릴리스 작업은 기존 `agent/release-v1.0.0` 브랜치에서 진행합니다.

## 1. 버전 표기

태그에는 `v`를 붙이고, 파일명·앱 버전에는 숫자 버전만 사용합니다. 다음 위치를 같은
버전으로 맞춥니다.

- `CMakeLists.txt`의 `project(... VERSION ...)`
- `src/main.cpp`의 `kAppVersionLabel` 및 업데이트 확인 User-Agent
- `src/app.rc`의 `APP_VERSION_NUMBER` 및 `APP_VERSION_STRING`
- `installer/LowLatencyCaptureViewer.iss`의 앱 버전, 빌드 폴더, 출력 이름,
  `VersionInfoVersion`
- `tools/package-v1.ps1`의 기본 빌드·출력 경로
- `docs/release-notes-v<version>.md`

변경 후 이전 버전 문자열이 남지 않았는지 확인합니다.

```powershell
rg -n "이전버전|v이전버전" CMakeLists.txt src installer tools docs
```

## 2. 빌드와 검사

Release x64 빌드를 만들고 등록된 모든 테스트와 공백 검사를 통과해야 합니다.

```powershell
cmake -S . -B build-v<version>-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-v<version>-release
ctest --test-dir build-v<version>-release --output-on-failure
git diff --check
```

가능하면 실제 장치에서 Shared/ASIO, 선택한 캡처 포맷, Tab 진단창, F2 설정, 종료와
재실행도 짧게 확인합니다.

## 3. 패키지

포터블 ZIP과 설치 파일에는 아래 두 자산만 포함합니다.

```text
LowLatencyCaptureViewer_v<version>_Setup.exe
LowLatencyCaptureViewer_v<version>_x64.zip
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-v1.ps1
& "C:\Program Files\Inno Setup 7\ISCC.exe" ".\installer\LowLatencyCaptureViewer.iss"
```

`settings.ini`, `%LOCALAPPDATA%` 로그, `build-*` 폴더, PDB, ILK는 패키지에 넣지
않습니다. ZIP 안의 EXE와 설치 파일의 버전을 확인합니다.

## 4. GitHub 공개

커밋 전에는 변경 파일과 스테이징 내용을 확인하고, 확인된 경로만 추가합니다.

```powershell
git status --short
git diff --cached --check
git commit -m "Prepare v<version> release"
git push origin agent/release-v1.0.0
```

릴리스 노트를 사용해 브랜치 HEAD를 태그 대상으로 공개합니다. 베타가 명시된 경우에만
`--prerelease`를 추가합니다.

```powershell
gh release create v<version> `
  "..\outputs\v<version>\LowLatencyCaptureViewer_v<version>_Setup.exe" `
  "..\outputs\v<version>\LowLatencyCaptureViewer_v<version>_x64.zip" `
  --repo seria-aa/LowLatencyCaptureViewer `
  --target agent/release-v1.0.0 `
  --title "Low Latency Capture Viewer v<version>" `
  --notes-file ".\docs\release-notes-v<version>.md"
```

공개된 태그·자산은 덮어쓰지 않습니다. 문제가 생기면 다음 패치 버전으로 수정합니다.
