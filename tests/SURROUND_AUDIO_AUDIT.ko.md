# 콘솔 LPCM 5.1 개발 빌드 검증 (2026-09-18)

## 범위

- 기본은 기존 스테레오. Shared 전용 선택 옵션이며 Exclusive/ASIO는 변경하지 않음.
- 48kHz PCM 16/24/32bit 또는 float32, 명시된 6채널(0x3f/0x60f)과
  8채널(0x63f)만 허용. 8채널 뒤/옆은 각각 -3dB로 5.1 서라운드에 합산.
- 내부 큐/클록 보정/볼륨은 6채널, 48kHz, PCM16. 센터/LFE 보존.
- Shared 출력에 6채널 WAVEFORMATEXTENSIBLE 전달. 정확한 형식 지원이 없으면
  IAudioClient3 대신 기존 AUTOCONVERTPCM 경로로 스피커 배치/다운믹스를 요청.
  [Windows 변환 계약](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants),
  [채널 마스크 정의](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/channel-mask).
- 연결된 입력 형식 재확인, 실행 중 샘플 형식 변경 시 오래된 stride로 읽지 않음.
  형식이 변경된 스트림은 재시작 필요. Dolby/DTS 디코더는 없음.

## 자동 검증 결과

Release x64 / MSVC / Ninja, `build-surround51-test`.
`ctest --test-dir build-surround51-test --output-on-failure -j 3`:
**28/28 통과, 136.93초**.

- 채널 수/마스크 조합 20,480개. 지원/거부 정책과 기본 스테레오 분리.
- 채널별 단독 신호, PCM 컨테이너 폭, float 비정상 값, 포화/클램프, 배열 경계.
- 버퍼 순환/과다 입력/읽기 10,000회 무작위 대조.
- 6채널 리샘플링을 독립 스테레오 3쌍 결과와 비트 단위 비교:
  비율 0.999/1/1.001, 다양한 블록 길이, 입력 소진 포함.
- 실제 캡처 콜백의 8채널→5.1, 부분 패킷 거부, 동적 형식 변경 차단.
- 실제 WASAPI fill 콜백의 6채널, 보정 off/on, 전체 채널 음소거 및 경계.
- 5.1 Shared 가상 시계 재생 18개 시나리오: 클록 오차, 작은 출력 주기,
  입력/출력 중단, 패킷 누락, 지터, 큰 입력 패킷. 각 120~180초.
- 기존 스테레오 샘플 해시, ASIO 콜백, 장시간 Shared 재생, 설정 저장,
  한국어/영어 UI, 영상/HDR/모니터 전환 회귀 테스트 통과.

## 비용 및 미확인 범위

- 외부 라이브러리/두 번째 PCM 큐/추가 목표 대기 시간 없음.
- EXE 647,680 → 658,944 bytes (+11,264 bytes). 기본 스테레오 큐 크기 동일.
- 5.1 사용 시 채널 수에 비례해 메모리·DSP 연산량 증가. 실제 CPU/GPU 부하와
  종단 간 지연은 측정하지 않았으므로 '성능 변화 없음'을 의미하지 않음.
- GC573의 Audio 핀에서 8ch/48kHz/16bit/0x63f PCM 노출만 별도 읽기 조회로 확인.
  실제 6/8채널 캡처 시작, 물리 스피커 배치, 출력 장치 전환, 음질은 미검증.
  하드웨어 앱을 자동 실행하거나 캡처 장치 설정을 변경하지 않음.
- 개발 빌드 제공만 완료. 버전 승격, 커밋, 푸시, 릴리스는 수행하지 않음.

테스트 EXE SHA-256:
`1AAEC1DD5AB171BE2EC932B2C0077CF934B184A51FF001DE5C353CD36F19A4F2`

## v1.2.10 릴리스 재검증 (2026-09-18)

- 런타임 입력 형식 변경을 캡처 소유 스레드에 알리고 설정으로 복귀하도록 보완.
  오디오-only와 영상+오디오 경로 모두 적용. 스트라이드를 추측하지 않음.
- 다채널 패킷의 실제 길이가 할당된 샘플 크기보다 크면 읽기 전에 거부.
- 64회 스테레오↔5.1 세션 전환, 리샘플러 초기화, 생산/소비/출력 전환 시 큐
  비우기의 동시 실행을 추가 검사. 채널별 샘플 일관성과 배열 경계 검증.
- 보완 후 `build-surround51-test`: 28/28 통과 (115.96초).
- 깨끗한 Release `build-v12100-release`: 28/28 통과 (145.39초).
- 같은 릴리스 빌드의 SurroundAudioTests 30회 연속 통과.
- 최종 SettingsViewTests에 Shared 전용 활성화 조건 추가 후 통과 (24.13초).
  최초 추가 검사에서 ASIO 미설치 시 Shared로 돌아가는 기존 동작을 테스트
  기대값이 반영하지 못해 실패했으며, 실제 선택된 모드 기준으로 기대값을 수정.
- AddressSanitizer 빌드는 성공했으나 MSVC 19.39 검사 런타임이
  `interception_win.cpp:171`에서 테스트 본문 실행 전에 중단됨. 제한 밖에서도
  동일하므로 메모리 검사 통과로 계산하지 않음. 경계 표식/무작위/동시성 검사는
  일반 Release 실행 결과임. 물리 5.1 청취/실제 장치 전환은 여전히 미검증.
- HDR 진단·scRGB·제조사 실험 옵션 모두 OFF. DLL 의존성은 Windows 구성 요소뿐.
- EXE 659,456 bytes (v1.2.9 대비 +11,776 bytes). 추가 오디오 큐/기본 대기시간 없음.
- ZIP 45개 항목과 설치 manifest 확인: 실행 파일 일치, 라이선스/한글 안내문 포함,
  설정·로그·테스트 바이너리·디버그 파일 미포함. Windows PowerShell에서 한글
  파일명을 보존하도록 패키징 스크립트 UTF-8 BOM 및 필수 입력 존재 검사 유지.

릴리스 파일 SHA-256:

- EXE: `702DBEE4520055A7012D233CE31490C4E96302C5B0528EAF0161A9A51F9B7980`
- Setup: `46DD736EE6091881BBDD57C2CF2DC28AD630C53C1C8682C9FC76A0306D53F80A`
- ZIP: `3D8BE2AA368F36FA7E3E537FBF96B3A8E60CAD62994CE1239C366646418D1F05`
