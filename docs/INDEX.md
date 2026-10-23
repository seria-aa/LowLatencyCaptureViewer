<img align="right" src="images/app-icon.png" width="40" height="40" alt="LLCV">

# LLCV documentation

[README](../README.md) · [한국어](INDEX.ko.md)

The current user guides cover **LLCV 2.1.5**. Historical development records are listed separately below.
Features, defaults, and test results in historical records apply to the version and environment tested.
User guides and their images are included in binary packages. Development and validation records are linked on GitHub and require an internet connection.

<br>

## User and developer guides

- [User guide](USAGE.md) — Installation, settings, sleep prevention, shortcuts, VSR, screenshots
- [Video and HDR](VIDEO.md) — Formats, frame rates, 40 fps capture, HDR10, scaling, window size
- [Recording](RECORDING.md) — codecs, quality, storage, splitting and file protection
- [Audio and 5.1](AUDIO.md) — Output modes, LPCM 5.1, buffers, clock correction
- [Troubleshooting](TROUBLESHOOTING.md) — Symptoms, checks, and issue reporting
- [Diagnostics and logs](DIAGNOSTICS.md) — Tab fields, buffer diagnosis, log management
- [Building from source](BUILDING.md) — Build requirements and packaging
- [Release procedure](RELEASING.md) — Versioning, tests, notices, pre-release checks

Each guide links to its Korean and English versions.

<br>

## Release notes

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
<summary>Earlier releases (1.x)</summary>

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

## Development and validation records

These records preserve implementation history and measurement conditions, not current usage instructions or compatibility guarantees for every device. Some are available in one language only.

- [Rounded viewer corners — development validation](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/rounded-corners-validation.md)
- [호환성 출력 비교 안내 — v1.2.7](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/TEST-BLT.ko.md)
- [VSR formats and 120fps investigation — development build](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/vsr-all-formats-validation.md)
- [NVIDIA VSR integration and experiment history](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/vsr-experiment.md)
- [Native HDR10 + NVIDIA VSR (development build)](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/docs/vsr-hdr-validation.md)

<details>
<summary>All test and release audit records</summary>

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
- [v2.1.5 release preparation and validation scope](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/tests/RELEASE_V2_1_5_AUDIT.md)
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

## Repository references

- [Design notes](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/DESIGN.txt)
- [Build information](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/BUILD_INFO.txt)
- [Dependencies](../DEPENDENCIES.txt)
- [Packaged launch instructions](../실행안내.txt)
- [Initial GitHub publication checklist (historical)](https://github.com/seria-aa/LowLatencyCaptureViewer/blob/main/GITHUB_PREP.md)
- [GNU GPL v3.0](../LICENSE)

---

[README](../README.md) · [한국어](INDEX.ko.md)
