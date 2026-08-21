# 소스에서 빌드

> [English](BUILDING.md) · [README로 돌아가기](../README.ko.md)

## 필요 환경

- Windows 10/11 x64
- **C++를 사용한 데스크톱 개발**이 설치된 Visual Studio 2022
- Windows SDK와 Windows용 CMake 도구
- 설치 프로그램을 만들 때만 Inno Setup 7

Win32, DirectShow, D3D11, DXGI, 실험적 MJPEG 디코딩용 Media Foundation과
WASAPI 등 Windows 시스템 API를 사용합니다. FFmpeg나 서드파티 코덱 팩은
필요하지 않습니다.

## Release 빌드

저장소 루트에서 x64 Visual Studio 개발자 PowerShell을 열고 실행합니다.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

실행 파일은 다음 위치에 생성됩니다.

```text
build\Release\LowLatencyCaptureViewer.exe
```

Release 구성은 정적 MSVC 런타임을 사용하므로 패키지 실행에 별도 Visual C++
재배포 패키지가 필요하지 않습니다.

## 설치 프로그램

Inno Setup 스크립트는 다음 위치에 있습니다.

```text
installer\LowLatencyCaptureViewer.iss
```

Release 실행 파일을 만든 뒤 Inno Setup 7로 빌드합니다. 설치판과
포터블판에는 컴퓨터별 `settings.ini`나 진단 로그를 포함하지 않아야 합니다.

실행 중 설정과 로그는 사용자별 다음 폴더에 저장됩니다.

```text
%LOCALAPPDATA%\LowLatencyCaptureViewer
```

이전 버전의 실행 파일 옆에 `settings.ini`가 있으면 첫 실행 때 사용자 폴더로
복사하며, 기존 파일은 자동으로 삭제하지 않습니다.
