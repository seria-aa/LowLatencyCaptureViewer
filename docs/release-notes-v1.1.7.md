## v1.1.7 (development)

### 한국어

- 설정에 `업데이트 자동 확인 (시작 후 백그라운드)` 옵션을 추가했습니다. 기본값은
  꺼짐입니다.
- 옵션을 켜면 시작 후 GitHub 최신 릴리스를 확인하고 새 버전이 있을 때 공식
  설치 파일 다운로드를 묻습니다.
- 업데이트 확인은 WinHTTP 백그라운드 스레드에서 수행하며 영상·오디오 초기화,
  캡처 프레임 경로와 분리되어 실행 중 지연을 추가하지 않습니다.
- 자동 설치나 무음 실행은 하지 않습니다. 사용자가 확인했을 때만 공식 GitHub
  설치 파일 링크를 엽니다.

### English

- Added `Check for updates automatically (in background after startup)` to Settings.
  It is off by default.
- When enabled, the viewer checks the latest GitHub release after startup and asks
  before opening the official installer download when a newer version is found.
- The check runs in a WinHTTP background thread, separate from capture, video, and
  audio initialization, so it does not add runtime latency.
- Updates are never installed or launched silently; the official GitHub installer
  link is opened only after user confirmation.
