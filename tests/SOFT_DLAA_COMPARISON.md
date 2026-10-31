# 약한 cDLAA와 보존형 CMAA2 화질 비교

2026-10-31. 테스트 전용 비교이며 정식 필터·실제 설정은 변경하지 않았다.

## 조건

- 동일 FHD SDR NV12 BT.709 Limited 입력. QHD/4K 출력, NIS 0.15 고정.
- 기존 cDLAA 100%, 보정량 50% 및 25%, 보존형 CMAA2, NIS 단독, 끔.
- 50%·25%는 cDLAA의 검출 임계값이나 NIS 선명도가 아니다.
  FHD에서 `원본 + 적용량 × (cDLAA 결과 − 원본)`으로 처리한 뒤 NIS를 수행한다.
  추가 선명화나 CSS 효과를 사용하지 않는다. 필터 계산량 감소를 뜻하지도 않는다.
- cDLAA의 나머지 설정은 이전 비교와 동일하며 Retain Fine Details를 유지한다.
- 보존형 CMAA2는 HIGH / ExtraSharpness=1. 제품 기본 매크로는 변경하지 않았다.
- 이전과 동일한 합성 AA 글씨·계단현상 글씨, 저장된 게임 일시정지 화면 사용.
  참조 이미지는 합성 자료에만 있으며, 게임의 고해상도 정답은 없다.
- 이번에는 화질만 비교했다. 지연·움직임·새 캡처 장치 검증은 수행하지 않았다.

## 참고 수치

QHD, 동일 고해상도 합성 참조 대비 8비트 채널 평균 절대 오차.
이 예시에 대한 수치이며 일반 화질 순위가 아니다.

| 방식 | 이미 AA가 있는 글씨 | AA가 없는 사선 |
| --- | ---: | ---: |
| NIS 단독 | 1.798 | 4.048 |
| 보존형 CMAA2→NIS | 1.897 | 3.508 |
| cDLAA 100%→NIS | 2.426 | 3.241 |
| cDLAA 50%→NIS | 1.976 | 3.558 |
| cDLAA 25%→NIS | 1.844 | 3.784 |

보정량을 낮추면 원본 보존과 사선 보정 효과가 함께 달라진다.
특정 적용량을 정식 기본값으로 채택하지 않았다.

## 검증

- 세 원본 × 두 출력 해상도에서 실제 GPU PNG 생성 및 방식 전환 검사 통과.
- cDLAA 적용량 0%와 NIS 단독 출력 차이 최대 1 이내.
- FHD 단계의 50%·25% 결과가 원본/100% 결과 보간과 채널 오차 1 이내로 일치.
- 평탄 프레임 및 복잡→평탄 전환, 반복 방식 전환은 최대 채널 오차 1 이내.
- 원본 입력 보존 및 필터 끔 복원 완전 일치.
- 이전 끔/NIS/보존형 CMAA2 합성 PNG 12개와 비교, 최대 채널 오차 1 이내.
- 관련 회귀 7/7 통과, 실패·건너뜀 0, 21.92초:
  CmaaNisRegression, CmaaNisHdr, CmaaNisRgb, CmaaNisRecording,
  UpscalingSafetyHardware, UpscalingSafetyWarp, TestProfileTests.
- 전체 비교 페이지 244가지 조합의 파일·크롭 범위 검사, 대화 내 비교 12가지
  전환 및 상태 복원 검사 통과. 브라우저 실기 확인과는 구분한다.
- 실제 재생/녹화 설정 및 출시 EXE/업스케일링 라이브러리의 작업 전후 SHA256 동일.

최종 자료: `outputs/text-aa-study/soft-dlaa-1793438372389/`.
`comparison.html`에 기존 cDLAA 100% 기준과 합성 참조, 전체 게임 화면도 포함한다.
`synthetic/metrics.csv`, 각 `method.txt`, `baseline-parity.json` 및 무손실 PNG 보존.
로그: `outputs/text-aa-study/{build-soft-aa,soft-aa-synthetic,soft-aa-game,soft-aa-related}.log`,
`soft-aa-related.xml`.

재현: CmaaNisTests의 `--spatial-aa-soft <tests/shaders/SpatialAaComparison.hlsl> <새 출력 경로>`.
네 번째 인자로 저장된 FHD NV12 파일을 넘기면 같은 게임 원본 비교를 실행한다.
실제 사용자 설정이나 장치 경로를 넘기지 않는다.
