# cDLAA / FXAA 수동 화질 비교

날짜: 2026-10-31. 기존 합성 글씨 비교를 확장한 테스트 전용 작업.
정식 렌더러·필터·기본값·사용자 설정을 변경하지 않았다.

## 비교 방법

- 필터 끔 / NIS 단독 / 출시 CMAA2→NIS / cDLAA→NIS / 약한 FXAA→NIS.
- NIS 0.15, FHD SDR NV12 BT.709 Limited 입력, QHD 및 4K 출력.
- 새 AA는 업스케일 이전의 FHD 감마 인코딩 영상에 적용한다.
- 기존 `TEXT_AA_COMPARISON.md`의 같은 마스터·글꼴·회색조 자료를 사용한다.
  이미 AA가 있는 입력과 점 샘플링한 계단현상 입력을 각각 처리했다.
- 이전에 저장된 컬러 게임 일시정지 화면도 같은 원본으로 처리했다.
  이번 입력을 새로 캡처하지 않았으며 게임 내부 렌더 해상도는 알 수 없다.
- 고해상도 참조·수치 오차는 합성 자료에만 있다. 게임에 임의의 정답을 만들지 않았다.
- 지연 측정·움직임 잔상·HDR·새 실기 캡처 검증은 이번 작업 범위가 아니다.

## 새 AA의 출처와 조건

[CShade 고정 소스](https://github.com/papadanku/CShade/tree/1e9cc77e1c2d30c22ff44a1b347747011c923954)의
`shaders/cDLAA.fx`, `shaders/cFXAA.fx`를 테스트용 D3D11 셰이더로 옮겼다.
ReShade 바인딩·정점 입력·미분 기반 픽셀 크기·선택적 합성 처리를 교체하고
원본의 필터 계산식을 유지했다. HLSL의 두 검색 루프 변수 충돌 경고는
두 번째 루프의 지역 변수 이름만 바꿔 해결했다.

- cDLAA: 짧은 경계 배율 3, 임계값 1/8, 긴 경계 임계값 1/6,
  Retain Fine Details 켬. 중간 RGBA8, 선형 Mirror 샘플러.
- FXAA: 원본 기본 검출값 1/16 및 1/6. 서브픽셀 보정만 기본 0.75에서
  0.25(Very Low)로 낮춤. NIS 선명도를 바꾼 것이 아니다.
- 원본은 선형 Clamp 샘플러, AA 출력은 RGBA8. NIS는 출시 라이브러리를 사용.
- NVIDIA의 AI DLAA를 사용한 결과가 아니다.
- 라이선스: `tests/shaders/CShade-LICENSE.txt` 및 셰이더 내 원저작자 고지 유지.
  테스트 소스/도구는 일반 앱에 링크하거나 설치하지 않는다.

## 결과

이미 AA가 있는 어두운 배경 글씨의 고해상도 합성 참조 대비 평균 절대 오차.
8비트 채널 단위이며 이 참조에 한해서 작을수록 가깝다.

| 방식 | QHD | 4K |
| --- | ---: | ---: |
| 필터 끔 | 2.384 | 3.162 |
| NIS 단독 | 1.798 | 2.402 |
| 현재 CMAA2→NIS | 2.108 | 2.712 |
| cDLAA→NIS | 2.426 | 3.020 |
| 약한 FXAA→NIS | 2.439 | 3.001 |

AA가 없는 사선 영역의 QHD 오차는 NIS 4.048, CMAA2 3.448,
cDLAA 3.241, 약한 FXAA 3.114였다.

새 두 후보도 글씨 보존과 사선 완화 사이의 절충을 보였다.
이 자료만으로 CMAA2의 상위 호환이나 범용 대체재라고 결론 내리지 않는다.
정지 화면 오차는 시간적 안정성·모든 실제 게임 화질의 순위가 아니다.

## 검증

- 합성 2종 및 저장된 게임 1종 × QHD/4K에서 두 후보의 실제 GPU 출력 생성.
- 필터 무처리 어댑터와 NIS 단독 차이 최대 1 이내.
- 각 후보가 실제 AA를 수행함을 NIS 이전/이후 차이로 확인.
- 평탄→복잡→평탄, 반복 방식 전환 후 출력 차이 최대 1 이내.
- 원본 입력 보존 및 필터 끔 복원은 완전 일치.
- 이전 합성 비교의 끔/NIS/CMAA2 PNG 12개와 픽셀 비교:
  모두 최대 1 이내, 1을 초과한 채널 없음. PNG 해시 완전 일치라고 표현하지 않는다.
- 관련 회귀 7/7 통과, 실패·건너뜀 0, 30.45초:
  CmaaNisRegression, CmaaNisHdr, CmaaNisRgb, CmaaNisRecording,
  UpscalingSafetyHardware, UpscalingSafetyWarp, TestProfileTests.
- 마지막 조건 표기 수정 후 테스트 도구 재빌드 성공. 필터 계산식 수정 없음.
- 비교 페이지 208가지 선택 조합의 파일·크롭 범위 검사 통과.
  대화 내 비교 12가지 선택 및 상태 복원 검사 통과. 브라우저 실기 검증은 아님.
- 출시 EXE/업스케일링 라이브러리 및 실제 재생/녹화 설정 SHA256은 작업 전후 동일.
  전체 199개 재실행이나 새 성능 측정을 했다는 뜻은 아니다.

## 산출물과 재현

최종 폴더: `outputs/text-aa-study/spatial-aa-1793437681043/`.
`comparison.html`에서 화면·영역·해상도·방식을 전환한다.
`synthetic/metrics.csv`, 각 `method.txt`, `baseline-parity.json` 및 PNG 보존.
화질 비교에는 CSS 필터·손 보정·손실 압축·서로 다른 원본을 사용하지 않았다.

```powershell
cmake --build build-v2200-release --target CmaaNisTests --parallel 4
.\build-v2200-release\CmaaNisTests.exe --spatial-aa-fixture tests/shaders/SpatialAaComparison.hlsl outputs/new-aa/synthetic
.\build-v2200-release\CmaaNisTests.exe --spatial-aa-fixture tests/shaders/SpatialAaComparison.hlsl outputs/new-aa/saved-game outputs/spatial-upscaling/extended-game-source/baseline/frames.csv.nv12
```

출력은 새 경로여야 한다. 실제 사용자 프로필이나 캡처 장치 경로를 넘기지 않는다.
저장 게임 원본 SHA256:
`6118D8204DF5FDBEEF929DDED2239D4F247EF92C7B99BB11B7120A4532CDEF46`.

로그: `outputs/text-aa-study/{build-candidates-final,spatial-aa-synthetic,spatial-aa-saved-game,spatial-aa-related}.log`,
`spatial-aa-related.xml`. 결과가 달라진 앞선 실행은 최종 자료에 합산하지 않는다.
