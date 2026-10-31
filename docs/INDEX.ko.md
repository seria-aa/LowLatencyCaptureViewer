<img align="right" src="images/app-icon.png" width="40" height="40" alt="LLCV">

# LLCV 문서

[README](../README.md) · [English](INDEX.md)

현재 사용 가이드는 **LLCV 2.2.0** 기준입니다. 평소 사용에는 아래 가이드를 참고하세요.
과거 기록에 적힌 기능·기본값·테스트 결과는 해당 버전과 시험 환경을 기준으로 합니다.
사용 가이드와 필요한 이미지는 배포본에 포함합니다. 개발·검증 기록은 GitHub 링크로 제공하며 인터넷 연결이 필요합니다.

<br>

## 사용 및 개발 가이드

- [사용 가이드](USAGE.ko.md) — 설치, 설정, 절전 방지, 단축키, 업스케일링·VSR, 스크린샷
- [영상·HDR](VIDEO.ko.md) — 포맷, 프레임, 40fps 캡처, HDR10, 확대, 창 크기
- [녹화](RECORDING.ko.md) — 코덱·품질, 저장·분할, 파일 보호
- [오디오·5.1](AUDIO.ko.md) — 출력 모드, LPCM 5.1, 버퍼, 클록 보정
- [문제 해결](TROUBLESHOOTING.ko.md) — 증상별 확인 순서와 문제 제보
- [진단·로그](DIAGNOSTICS.ko.md) — Tab 수치, 버퍼 진단, 로그 관리
- [소스 빌드](BUILDING.ko.md) — 빌드 환경과 패키지 생성
- [릴리스 절차](RELEASING.ko.md) — 버전, 테스트, 라이선스 고지, 공개 전 점검

각 가이드 상단에서 한국어·영어를 전환할 수 있습니다.

<br>

## 릴리스 노트

- [v2.2.0](release-notes-v2.2.0.md)
- [v2.1.9](release-notes-v2.1.9.md)
- [v2.1.8](release-notes-v2.1.8.md)
- [v2.1.7](release-notes-v2.1.7.md)
- [v2.1.6](release-notes-v2.1.6.md)
- [v2.1.5](release-notes-v2.1.5.md)
- [v2.1.4](release-notes-v2.1.4.md)
- [v2.1.3](release-notes-v2.1.3.md)
- [v2.1.2](release-notes-v2.1.2.md)
- [v2.1.1](release-notes-v2.1.1.md)
- [v2.1.0](release-notes-v2.1.0.md)
- [v2.0.5](release-notes-v2.0.5.md)
- [v2.0.4](release-notes-v2.0.4.md)
- [v2.0.3](release-notes-v2.0.3.md)
- [v2.0.2](release-notes-v2.0.2.md)
- [v2.0.1](release-notes-v2.0.1.md)
- [v2.0.0](release-notes-v2.0.0.md)

<details>
<summary>이전 릴리스 (1.x)</summary>

- [v1.3.1](release-notes-v1.3.1.md)
- [v1.3.0](release-notes-v1.3.0.md)
- [v1.2.12](release-notes-v1.2.12.md)
- [v1.2.11](release-notes-v1.2.11.md)
- [v1.2.10](release-notes-v1.2.10.md)
- [v1.2.9](release-notes-v1.2.9.md)
- [v1.2.8](release-notes-v1.2.8.md)
- [v1.2.7](release-notes-v1.2.7.md)
- [v1.2.6](release-notes-v1.2.6.md)
- [v1.2.5](release-notes-v1.2.5.md)
- [v1.2.5.1 Pre-release](release-notes-v1.2.5.1.md)
- [v1.2.4](release-notes-v1.2.4.md)
- [v1.2.3](release-notes-v1.2.3.md)
- [v1.2.2](release-notes-v1.2.2.md)
- [v1.2.1](release-notes-v1.2.1.md)
- [v1.2.0](release-notes-v1.2.0.md)
- [v1.1.7 (development)](release-notes-v1.1.7.md)
- [v1.1.7.2 (Pre-release)](release-notes-v1.1.7.2.md)
- [v1.1.7.1](release-notes-v1.1.7.1.md)
- [v1.1.6](release-notes-v1.1.6.md)
- [v1.1.4](release-notes-v1.1.4.md)
- [v1.1.3](release-notes-v1.1.3.md)
- [v1.1.2.1 Beta](release-notes-v1.1.2.1.md)
- [한국어](release-notes-v1.1.1.md)
- [한국어](release-notes-v1.1.0.md)
- [한국어](release-notes-v1.0.6.2.md)
- [한국어](release-notes-v1.0.6.1.md)

</details>

<br>

## 개발·검증 기록

과거의 구현 과정과 측정 조건을 남긴 기록입니다. 현재 버전의 사용 가이드나 모든 장치에 대한 호환성 보증은 아닙니다. 일부 기록은 한 언어로만 제공됩니다.

- [Rounded viewer corners — development validation](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/rounded-corners-validation.md)
- [호환성 출력 비교 안내 — v1.2.7](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/TEST-BLT.ko.md)
- [VSR formats and 120fps investigation — development build](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/vsr-all-formats-validation.md)
- [NVIDIA VSR integration and experiment history](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/vsr-experiment.md)
- [Native HDR10 + NVIDIA VSR (development build)](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/vsr-hdr-validation.md)

<details>
<summary>테스트·릴리스 검증 기록 전체</summary>

- [추가 가상 검증 — 2026-09-11](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/ADDITIONAL_FAULT_AUDIT.ko.md)
- [Capture capability and startup replay — 2026-09-10](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/CAPTURE_STATE_REPLAY.md)
- [추가 결함 보완 및 main.cpp 모듈화 검토 — 2026-09-11](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/FAULT_FIX_AND_MODULE_REVIEW.ko.md)
- [AVerMedia 하드웨어 톤 매핑 초기화 검증](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/HARDWARE_TONE_MAPPING_AUDIT.ko.md)
- [HDR 처리 점검 — 2026-09-16](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/HDR_AUDIT.ko.md)
- [P010 HDR 색차 배치 호환성 선택 — 2026-09-16](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/HDR_CHROMA_COMPATIBILITY_AUDIT.ko.md)
- [OBS HDR 비교 및 P010 기본 판정 검증](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/HDR_OBS_COMPARISON.ko.md)
- [v1.2.8 과포화 제보 재검증 — 2026-09-16](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/HDR_SATURATION_AUDIT.ko.md)
- [OBS 방식의 캡처 시작 시 톤매핑 요청 및 P010 HDR 전용화](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/HDR_VENDOR_CONTROL_AUDIT.ko.md)
- [모니터 이동 / DPI 이벤트 가상 검증 (2026-09-11)](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/MONITOR_MOVE_AUDIT.ko.md)
- [v1.2.12 release verification](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V1_2_12_AUDIT.md)
- [v1.3.0 release verification](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V1_3_0_AUDIT.md)
- [v1.3.1 local release-readiness audit](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V1_3_1_AUDIT.md)
- [v2.0.0 release-readiness audit](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V2_0_0_AUDIT.md)
- [v2.0.1 release-readiness audit](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V2_0_1_AUDIT.md)
- [v2.0.2 release-readiness audit](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V2_0_2_AUDIT.md)
- [v2.2.0 릴리스 준비·검증 범위](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V2_2_0_AUDIT.md)
- [v2.1.9 릴리스 준비·검증 범위](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V2_1_9_AUDIT.md)
- [RGB 입력·녹화 구현 및 검증](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/RGB_CAPTURE_SUPPORT.md)
- [v2.1.8 릴리스 준비·검증 범위](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V2_1_8_AUDIT.md)
- [업스케일링 경계 안전성 검증](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/UPSCALING_BOUNDARY_SAFETY.md)
- [설정과 분리된 오버플로 진단](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/UPSCALING_OVERFLOW_DIAGNOSTIC.md)
- [UYVY/HDYC 구현·검증](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/UYVY_HDYC_SUPPORT.md)
- [v2.1.7 릴리스 준비·검증 범위](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V2_1_7_AUDIT.md)
- [v2.1.6 릴리스 준비·검증 범위](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V2_1_6_AUDIT.md)
- [v2.1.5 릴리스 준비·검증 범위](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V2_1_5_AUDIT.md)
- [v1.2.7 release verification — 2026-09-11](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE-v1.2.7.md)
- [v1.2.8 release verification — 2026-09-16](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE-v1.2.8.md)
- [v1.2.9 release verification — 2026-09-16](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE-v1.2.9.md)
- [직접 P010 → scRGB 비교 프로토타입 (2026-09-16)](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/SCRGB_PROTOTYPE_AUDIT.ko.md)
- [SDR 화질 변경 릴리스 검증 (2026-09-19)](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/SDR_FIDELITY_RELEASE_AUDIT.ko.md)
- [Shared audio stability verification](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/SHARED_STABILITY.md)
- [콘솔 LPCM 5.1 개발 빌드 검증 (2026-09-18)](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/SURROUND_AUDIO_AUDIT.ko.md)
- [Video edge-case audit — 2026-09-10](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/VIDEO_EDGE_CASE_AUDIT.md)
- [Video hardening verification — 2026-09-10/11](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/VIDEO_HARDENING_VERIFICATION.md)
- [Video output debug audit — 2026-09-10](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/VIDEO_OUTPUT_AUDIT.md)

</details>

<br>

## 저장소 참고 자료

- [설계 기록](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/DESIGN.txt)
- [빌드 정보](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/BUILD_INFO.txt)
- [의존성](../DEPENDENCIES.txt)
- [배포본 실행 안내](../실행안내.txt)
- [초기 GitHub 공개 체크리스트 (과거 기록)](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/GITHUB_PREP.md)
- [GNU GPL v3.0](../LICENSE)

---

[README](../README.md) · [English](INDEX.md)
