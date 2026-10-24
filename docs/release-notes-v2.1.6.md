# LLCV v2.1.6

## 한국어

- **CMAA2 → NIS 업스케일링 추가:** SDR 영상의 계단현상을 보정한 뒤 확대합니다. 영상 탭에 **끄기 / CMAA2 → NIS / NVIDIA VSR** 선택 목록과 NIS 선명도 조절을 추가했습니다. 새 필터는 기본 꺼짐이며 선명도 기본값은 **0.15**입니다. NV12·YUY2·MJPEG SDR의 가로·세로 최대 2배 확대에 화면 표시용으로 적용합니다.
- **설정 UI 개선:** 설정 탭 전체의 간격, 글자·버튼 정렬과 하단 여백을 맞췄습니다. 업스케일링 선택에 필요한 옵션만 표시하고 자동 인식·스크린샷 영역의 배치도 다듬었습니다.
- **설정 화면 처리 개선:** 화면을 그릴 때 불필요한 임시 메모리 할당을 줄이고, 배치가 같을 때 중복으로 수행하던 컨트롤 종류 확인을 생략했습니다.
- **개발·검증 환경 정리:** 수동 미리보기·측정 도구를 기본 빌드에서 제외했습니다. 테스트·비공개 미리보기의 설정 경로를 별도 임시 프로필로 분리했습니다.

[사용 가이드](USAGE.ko.md) · [영상 설정](VIDEO.ko.md) · [녹화 안내](RECORDING.ko.md)

## English

- **CMAA2 → NIS upscaling:** reduces aliasing before enlarging SDR video. Adds **Off / CMAA2 → NIS / NVIDIA VSR** selection and NIS sharpness controls to the Video tab. The new filter is off by default, with sharpness **0.15**. Applies to NV12/YUY2/decoded MJPEG SDR enlargement up to 2× per axis, for display only.
- **Settings UI:** consistent spacing, text/button alignment and footer margins across settings tabs. Shows only relevant upscaling options and refines the detected-format and screenshot sections.
- **Settings processing:** reduces temporary allocations during painting and skips redundant control-class queries when the layout is unchanged.
- **Development and validation:** excludes manual preview/benchmark tools from the default build and gives tests/private previews separate temporary settings profiles.

[User guide](USAGE.md) · [Video settings](VIDEO.md) · [Recording guide](RECORDING.md)
