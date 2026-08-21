# 릴리스 가이드

> [English](RELEASING.md) · [README로 돌아가기](../README.ko.md)

이 문서는 버전 표기, 빌드, 패키징, GitHub 업로드 절차를 매번 동일하게
수행하기 위한 체크리스트입니다. 릴리스 브랜치는 별도로 만들지 않고 기존
`agent/release-v1.0.0` 브랜치를 사용합니다.

## 1. 버전 정하기

버전은 Git 태그와 파일 이름에 `v`를 붙인 동일한 값을 사용합니다.

```text
예: 1.1.7.1  →  태그 v1.1.7.1
```

다음 위치의 버전이 모두 일치해야 합니다.

- `CMakeLists.txt`의 `project(... VERSION ...)`
- `src/main.cpp`의 `kAppVersionLabel` 및 업데이트 요청 User-Agent
- `installer/LowLatencyCaptureViewer.iss`의 앱 버전, 빌드 폴더, 출력 이름,
  `VersionInfoVersion`
- `tools/package-v1.ps1`의 기본 빌드·출력 경로와 ZIP 이름
- `docs/release-notes-v<버전>.md`

변경 후 다음 검색으로 이전 버전이 남아 있지 않은지 확인합니다.

```powershell
rg -n "1\.1\.7|v1\.1\.7" CMakeLists.txt src installer tools docs
```

## 2. 빌드와 테스트

기존 작업 브랜치를 확인하고 fast-forward 방식으로만 동기화합니다.

```powershell
git switch agent/release-v1.0.0
git pull --ff-only origin agent/release-v1.0.0
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
git diff --check
```

Release 실행 파일과 `AudioModuleTests`, `LoggerTests`가 생성되고 모든 테스트가
통과해야 합니다. 실제 장치가 있는 경우 Shared/ASIO 오디오, 선택한 캡처 포맷,
Tab 진단창, F2 설정창, 종료·재실행까지 짧게 확인합니다.

## 3. 패키지 만들기

먼저 `tools/package-v1.ps1`의 버전별 기본 경로를 새 버전에 맞춘 뒤 실행합니다.
스크립트는 README, LICENSE, 의존성 안내, docs와 실행 파일을 포터블 ZIP에 넣습니다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-v1.ps1
```

Inno Setup 7(또는 6 이상)으로 설치판을 만듭니다.

```powershell
& "C:\Program Files\Inno Setup 7\ISCC.exe" ".\installer\LowLatencyCaptureViewer.iss"
```

업로드할 파일은 다음 두 개입니다.

```text
LowLatencyCaptureViewer_v<버전>_Setup.exe
LowLatencyCaptureViewer_v<버전>_x64.zip
```

패키지에는 개발 PC의 `settings.ini`, `%LOCALAPPDATA%` 로그, `build-*` 폴더,
PDB/ILK 파일을 포함하지 않습니다. 설치판의 파일 버전과 ZIP 내부 실행 파일의
수정 시각·크기도 확인합니다.

## 4. 커밋과 GitHub CLI 업로드

커밋 전에 작업 트리와 스테이징 내용을 검토합니다.

```powershell
git status --short
git diff --cached --check
git commit -m "Prepare v<버전> release"
git push origin agent/release-v1.0.0
```

릴리스 노트 파일을 사용해 기존 브랜치 HEAD를 태그 대상으로 지정합니다.

```powershell
gh release create v<버전> `
  "..\outputs\v<버전>\LowLatencyCaptureViewer_v<버전>_Setup.exe" `
  "..\outputs\v<버전>\LowLatencyCaptureViewer_v<버전>_x64.zip" `
  --repo seria-aa/LowLatencyCaptureViewer `
  --target agent/release-v1.0.0 `
  --title "Low Latency Capture Viewer v<버전>" `
  --notes-file ".\docs\release-notes-v<버전>.md"
```

기본은 정식 릴리스입니다. 베타나 RC는 사용자가 명시한 경우에만
`--prerelease`를 추가합니다. 업로드 후 자산 상태와 태그 대상을 확인합니다.

```powershell
gh release view v<버전> --repo seria-aa/LowLatencyCaptureViewer `
  --json tagName,targetCommitish,isDraft,isPrerelease,assets,url
```

## 5. 릴리스 노트 규칙

한국어와 영어를 같은 파일에 작성하고, 다음 순서로 짧게 정리합니다.

1. 사용자에게 보이는 기능·UI 변경
2. 지원 포맷·장치·호환성 변경
3. 버그 수정과 진단 로그 변경
4. 성능·지연에 영향을 주는지 여부
5. 알려진 제한과 테스트 범위

로그 강화처럼 관찰성만 바뀐 경우에는 **프레임마다 로그를 기록하지 않으며
정상 영상·오디오 경로에 처리 지연을 추가하지 않는다**는 점을 명시합니다.

## 6. 릴리스 후 문제 처리

태그를 이미 공개한 뒤에는 기존 자산을 덮어쓰거나 태그를 강제로 이동하지
않습니다. 기능·설정·패키지 문제가 발견되면 다음 패치 버전으로 수정하고,
릴리스 노트에 영향과 해결 방법을 적습니다. 초안 단계에서만 자산을 교체합니다.
