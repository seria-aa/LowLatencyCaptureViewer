# HDR 처리 점검 — 2026-09-16

대상: v1.2.7 릴리스 커밋 f9acd0376fb2fe4fd91e397db17f9be94d43c71f.
소스는 릴리스 커밋과 동일함을 확인했다. 앱 코드, 설정, 디스플레이 모드,
공개 릴리스는 수정하지 않았다. 이 문서는 수정 전 진단 기록이다.

## 결론

P010/PQ 영상 출력 경로는 존재하지만 색 정확도까지 검증된 HDR 구현은 아니다.
잘못된 정적 메타데이터, HDR에 맞지 않는 OSD 합성, 입력 판정/출력 상태 확인의
공백이 확인됐다. ezcap 401 사용자의 전체 화면 밝기 차이와 어느 항목이 직접
연결되는지는 현재 자료만으로 확정할 수 없다.

## 확인된 결함과 우선순위

### 1. P1 — 잘못된 밝기 단위와 임의의 정적 HDR 정보

src/main.cpp:2069~2085에서 mastering primaries를 고정 P3/D65,
MaxMasteringLuminance=10000000, MaxCLL=1000, MaxFALL=400으로 지정한다.
원본 mastering/content-light 메타데이터를 전달받아 복원하는 코드가 아니다.

DXGI_HDR_METADATA_HDR10에서 MaxMasteringLuminance는 정수 nit 단위다.
1000nit를 나타내려면 1000이어야 하며 MinMasteringLuminance에만 1/10000nit
단위를 사용한다. 기존 최댓값은 1000nit가 아니라 1000만nit를 뜻한다.
단, 고정값을 1000으로 바꾸는 것만으로 원본과 동일한 출력이 되지는 않는다.
SetHDRMetaData 반환값도 버려 실패/무시 여부를 알 수 없다.

권장: 실제 정보와 가정값을 분리하고, 정보가 없을 때 임의의 mastering 정보를
전송하지 않는 정책을 검토한다. 메타데이터를 생략하는 것이 모든 화면에서
동일한 밝기를 보장하지는 않는다. Microsoft는 이 API가 무시될 수 있고 실제
모니터 전달도 보장되지 않는다고 명시한다.

### 2. P1 — SDR UI를 PQ 버퍼에 그대로 쓰며 비선형 공간에서 합성

src/main.cpp:2221 부근 overlayPixelSource는 BGRA8 텍스처 값을 그대로 출력한다.
동일한 셰이더/ONE, INV_SRC_ALPHA 블렌드를 HDR10 R10G10B10A2 버퍼에 사용한다.
SDR sRGB -> linear -> BT.2020/PQ 변환과 UI 기준 밝기가 없다.

실제 앱 셰이더와 블렌드 상태로 GPU 읽기 검증:

- 불투명 UI 흰색 1.0 -> PQ 코드 1023/1023. PQ 신호상 10000nit에 해당하며
  모니터가 실제로 그 밝기를 출력한다는 뜻은 아니다.
- 100nit를 나타내는 PQ 배경에 50% 검정 UI를 합성 -> 코드 261/1023.
  PQ 해석상 약 5.54nit다. 선형 광량 기준 절반인 50nit와 다르다.

이는 UI/반투명 패널 영역의 오류를 확인한 것이다. Tab을 닫아도 영상 전체가
어두운 증상을 직접 설명하지는 않는다. 실제 흰 글씨도 0.91~0.95 등 SDR 값이
그대로 사용되므로 정상적인 SDR UI 기준 밝기와 일치하지 않는다.

권장: HDR 전용 UI 기준 밝기/색 변환과 선형 합성을 설계한다. 임시 감마/밝기
증가로 덮지 않는다. 정확한 반투명 합성은 배경 접근 또는 중간 버퍼가 필요할 수
있으므로 구현 전에 HDR 경로의 비용을 따로 비교한다.

### 3. P1 — 입력 색 정보가 bool로 축약되어 잘못된 해석을 방지하지 못함

- CaptureColorMetadata::hdr10()는 primaries/transfer/matrix로만 HDR을 판정한다.
- HDR 렌더러는 nominalRange/chromaSubsampling 값을 받지 않고 항상
  YCBCR_STUDIO_G2084_TOPLEFT_P2020으로 처리한다 (src/main.cpp:2491).
- Full-range P010/PQ라고 명시된 데이터도 같은 Limited 변환에 들어간다.
  실제 ezcap 입력이 Full이라는 증거는 없으며 일반적인 Limited 입력에는
  이 문제가 발생하지 않는다.
- P010에서 정보가 부족하면 무조건 BT.709 SDR 처리한다 (src/main.cpp:1996).
  SDR P010이면 가능하지만 PQ임이 일부 확인됐거나 HLG 등 다른 전송함수일 때도
  같은 결과가 된다. 이것은 HDR->SDR 톤매핑이 아니다.
- 연결 후 얻은 P010 색 정보는 false->true HDR 승격만 처리한다
  (src/main.cpp:3265). 사전 정보와 최종 연결 정보가 모순되는 경우의 재판정이 없다.

권장: 입력 판정을 HDR10/SDR/불명/지원하지 않는 HDR로 나누고 최종 연결 정보를
반영한다. 정보가 없다는 이유만으로 HDR을 확정하거나 알려진 PQ를 SDR로
오해석하지 않는다. Full PQ용 YCbCr 색공간 enum이 현재 SDK에 없으므로 단순
enum 교체로 지원할 수 있다고 가정하지 말고, 미지원 안내 또는 명시적 변환을
선택한다. 강제 HDR 옵션은 확인된 PQ 소스에만 쓰는 가정임을 유지한다.

### 4. P2 — 앱 HDR 표시가 실제 모니터 HDR 상태를 뜻하지 않음

src/main.cpp:2052~2089는 SetColorSpace1 성공을 근거로 HDR active를 표시한다.
GetContainingOutput/IDXGIOutput6::GetDesc1 등의 실제 출력 상태 조회가 없다.

로컬 검증에서 실제 출력 ColorSpace=0(SDR), BitsPerColor=8인 상태에서도
HDR 초기화 성공, g_hdrOutputActive=1이었다. SetColorSpace1 호출 뒤 PQ 지원
조회는 3을 반환했다. 공식 문서도 설정 성공 후 지원 조회는 해당 색공간을
지원한다고 보고하므로 이것만으로 HDR 디스플레이를 판정할 수 없다고 설명한다.

따라서 사용자 정보창의 BT.2020/PQ/HDR10 표시는 앱의 출력 신호 형식을 보여주는
것이며 실제 모니터가 HDR 상태라는 증거가 아니다. 예전 전체 회귀 검사 통과도
그 사실을 보장하지 않는다.

권장: 앱 신호 형식과 현재 디스플레이 HDR 상태를 분리하여 표시/기록한다.
SDR 화면용 정상 톤매핑 또는 명확한 안내 정책이 필요하다. 시작 시뿐 아니라
모니터 이동/Windows HDR 상태 변경 때도 확인한다. WM_DISPLAYCHANGE는 현재
로그만 남기며 HDR 출력 상태를 재판정하지 않는다.

### 5. P2 — HDR 변환 조합 지원 확인 부족

입력 P010의 CheckVideoProcessorFormat만 검사하고
CheckVideoProcessorFormatConversion으로 P010/PQ -> RGB10/PQ 조합을 검사하지
않는다 (src/main.cpp:2133). SetStream/OutputColorSpace1는 반환값이 없는 API다.
인터페이스 존재 또는 입력 포맷 지원이 정확한 변환 조합 지원을 보장하지 않는다.

이번 로컬 RTX 3080에서는 실제 조합 지원=TRUE이고 픽셀 읽기 검사도 통과했다.
다른 GPU/드라이버에서 실패한다는 실측 결과는 없지만, 미지원 조건에 대한
명확한 진단과 안전한 중단/대체 처리가 빠져 있다.

## 정상으로 확인한 부분

- P010은 16비트 컨테이너에 10비트 값을 담아 업로드하며, 별도 HDR swapchain은
  R10G10B10A2 + RGB_FULL_G2084_NONE_P2020을 사용한다.
- HDR10 판별에 쓰는 BT.2020/PQ의 숫자값 자체가 틀린 것은 아니다.
- HDR 상태에서 Blt 호환성 출력을 거절하는 기존 방어가 있다.
- 실제 Limited 중성 회색 입력의 YUV->RGB 변환에서 추가 감마나 균일한
  밝기 저하를 관측하지 않았다. 한 GPU의 회색 입력 결과일 뿐 색 패치·
  크로마 위치·HDR 실물 밝기의 정확성을 보장하지 않는다.

## 실행한 검증

1. 릴리스와 동일한 소스를 사용하는 6개 기존 검사 재실행: 6/6 통과, 1.36초.
   AudioCallbackTests, MonitorMoveTests, VideoColorTests,
   DirectShowVideoFormatTests, VideoFormatFaults, VideoLayoutStress.
   기존 검사는 HDR PQ 픽셀 정확도 및 UI 광량 검증을 포함하지 않는다.
2. build-v1270-release/HdrAudit.cpp 임시 하네스:
   앱 렌더러/실제 오버레이 셰이더를 직접 사용하고 기존 모듈 객체와 연결.
   숨긴 창, 합성 P010 입력, 실제 GPU staging readback.
   캡처/오디오 장치와 디스플레이 모드는 변경하지 않음.
3. 로컬 RTX 3080: P010/PQ -> RGB10/PQ 변환 지원=1.
   설정된 입력/출력 색공간=16/12. GPU 경고/오류=0.
4. 8개 중성 회색 단계:

| 입력 Y(10bit) | 출력 R(10bit PQ) |
| --- | --- |
| 0 | 0 |
| 64 | 0 |
| 128 | 75 |
| 256 | 224 |
| 512 | 523 |
| 768 | 822 |
| 940 | 1023 |
| 1023 | 1023 |

Limited [64,940] -> Full [0,1023] 변환과 맞는다. Full 소스라고 가정하면
양끝 값이 잘리는 결과이므로 입력 범위를 보존/검사해야 한다.

추가 테스트가 필요한 항목: PQ 기준 패치와 BT.2020 유색 패치 자동 비교,
최종 연결 메타데이터 모순/불완전 정보, HDR/SDR 모니터 전환 모의 검사,
UI 반투명 광량 검사, 미지원 변환/메타데이터 API 실패 주입.
실기 없이는 ezcap 401의 실제 전달 픽셀, Windows/드라이버/모니터의 최종
톤매핑과 패스스루 대비 밝기 일치를 확정할 수 없다.

## 수정 순서와 성능 주의

1. 임의/잘못된 메타데이터 정책, 입력 판정, 변환 지원 검사, 진단부터 수정.
   초기화/전환 시 검사로 제한하면 정상 프레임마다 큰 처리를 추가할 이유가 없다.
2. HDR UI 변환과 합성을 분리해서 수정. SDR 경로는 유지하고 HDR UI가 표시될
   때의 GPU 비용과 임시 텍스처 필요량을 비교한다.
3. 회색/색 패치 수치 검사를 통과한 뒤 실기 사용자에게 밝기 비교 요청.
   리쉐이드·전체 밝기 배율·일괄 감마 보정은 원인 확인을 대신하지 않는다.

## 근거

- [HDR10 메타데이터 단위](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/ns-dxgi1_5-dxgi_hdr_metadata_hdr10)
- [SetHDRMetaData의 전달 보장/권장 사항](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgiswapchain4-sethdrmetadata)
- [HDR/SDR 혼합 콘텐츠 렌더링](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range)
- [변환 조합 지원 검사](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11videoprocessorenumerator1-checkvideoprocessorformatconversion)
- [스왑체인 색공간 지원 조회의 한계](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-checkcolorspacesupport)
- [실제 출력 색공간 조회](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/ns-dxgi1_6-dxgi_output_desc1)

## 후속 보완 — 같은 날, 로컬 개발본

사용자의 구현 요청 이후 다음을 적용했다. 위의 진단 결과는 수정 전 기록이며,
공개 v1.2.7 릴리스·태그·브랜치는 변경하지 않았다.

- HdrPolicy: 연결이 끝난 P010 메타데이터를 기준으로 HDR10/SDR/불명/미지원
  판정. PQ/BT.2020 모순, HLG, Full/기타 HDR 범위와 미지원 색차 위치를 거절.
  명시된 left/top-left를 변환에 반영하며 기본값 가정은 로그에 기록.
- 최종 연결 정보가 HDR에서 SDR로 바뀌는 경우도 반영. P010 렌더러를 연결 후에
  만들므로 사전 메타데이터로 잘못된 HDR 스왑체인을 먼저 만들지 않음.
- HDR swapchain 색공간 및 정확한 P010/PQ→RGB10/PQ 변환 조합 검사.
  HDR Video Processor 자동 화질 처리를 끄고 PQ 영상 값을 보존.
- 임의의 mastering/MaxCLL/MaxFALL 값 제거. SetHDRMetaData(NONE)의 결과 기록.
- HDR OSD: sRGB premultiplied UI를 복원·선형화한 뒤 BT.2020으로 변환하고,
  PQ 배경을 선형 광량으로 합성 후 재인코딩. Windows UI white 또는 203nit 사용.
- HDR OSD가 처음 나타날 때만 700×440×4 = 1,232,000byte의 GPU scratch texture를
  만들고, 각 패널의 실제 표시 영역만 복사. 여러 패널이 겹쳐도 순서대로 합성.
  해제/재초기화 시 반환. 전체 프레임 복사나 추가 영상 큐는 없음.
- 현재 모니터의 DXGI HDR 상태 및 Windows SDR UI white 조회. 시작 때와 HDR
  실행 중 UI 타이머(2초 간격)에서만 조회하며 정상 프레임 루프에서는 하지 않음.
  앱 PQ 출력과 물리 출력 상태를 Tab 정보창에서 구분. Windows 화면 모드는
  변경하지 않으며 자체 HDR→SDR 톤매핑은 추가하지 않음.
- 한/영 영상 설명과 미지원 HDR 오류 안내 보완.

### 수정 후 검증

- 전체 CTest 25/25 통과 (143.68초). 기존 23개 + HDR 정책/GPU 검사 2개.
- 마지막 연결 메타데이터 확인 보완 후 전체 타깃 재빌드 및 관련 6/6 재검사 통과
  (1.99초). HDR GPU 검사는 skip이 아니라 실제 실행·통과.
- 실제 앱 렌더러로 회색 8단계, BT.2020 유색 4패치, UI 흰색 203/80nit,
  반투명 검정, 중첩·투명·화면 밖 패널, 음수 좌표 클리핑, SDR 재초기화,
  left 색차 설정을 검증. D3D11 debug warning/error 0.
- 100nit 영상 위 50% 검정 UI는 약 50nit, 두 번 겹치면 약 25nit의 PQ 코드로
  나옴. 불투명 UI 흰색은 더 이상 PQ 10000nit 코드가 아님.
- RTX 3080, 700×440 패널, 200회 GPU timestamp 평균: HDR 선형 합성+부분 복사
  0.0209ms, 기존 SDR 합성 0.0023ms. 단일 GPU의 합성 구간 측정이며 전체 앱
  지연, 다른 GPU, 실제 HDR 패널의 성능 보장은 아님.
- 테스트/진단 소스는 설치 대상이 아님. 새 런타임 DLL/외부 프레임워크 없음.
  로컬 EXE 643,072byte: 기존 631,808byte 대비 11,264byte(11KiB) 증가.
  SHA256: 4CDF67103B5099918F7C849B28B6E2AEAEDD2D20924E445C4DD21C685A0E0056.

### 남은 실기 확인과 범위

HDR 모니터의 실제 밝기/색, Windows HDR 활성 상태의 UI white 조회,
HDR↔SDR 물리 모니터 이동, ezcap 전달 픽셀과 패스스루 대비 일치는 미확인이다.
GPU 픽셀 검증은 이것을 대신하지 않는다. Full HDR·HLG·자체 HDR→SDR 톤매핑은
지원하지 않는다. 실행 도중 캡처 소스가 HDR/SDR 형식을 바꾸는 경우의 자동
재협상/복구도 이번 보완 범위가 아니며 설정을 다시 열어 재시작해야 한다.

## 표준 대조 후 마무리 — 2026-09-16

사용자 요청대로 입력 전환 기능/실물 비교를 완료 조건으로 추가하지 않고,
현재 지원하는 Limited P010/PQ HDR10의 신호 해석과 합성 계산에 집중했다.

- ITU-R BT.2100-3의 Table 4(PQ EOTF와 역함수), 6(BT.2020 NCL),
  8(기본 top-left 색차 위치), 9(10bit narrow 양자화)를 소스와 대조했다.
  해당 수식·계수·기본값을 바꿀 불일치는 찾지 않았다.
- Microsoft DXGI/DXVA의 색공간/색차 위치, SDR UI white 단위 및 HDR 메타데이터
  API 동작도 대조했다. BT2020_10/12는 모두 NCL이며 ICtCp와 혼동하지 않도록
  메타데이터 정책 검사를 확대했다.
- 한 가지 추가 보완: Windows UI white의 80..1000nit 임의 허용 범위를 제거.
  API가 성공하면 공식 단위(값×0.08nit)를 그대로 반영한다. 0과 PQ 표현 범위를
  넘는 무효 값만 거절하며 fallback은 유지한다. 프레임 루프 변경은 없다.
- GPU 검사 확대: 전체 P010 Y 코드 0..1023의 회색·단조성, sRGB 회색 0..255,
  배경 밝기 10종(0..10000nit)×알파 6종(60조합), 실제 D2D premultiplied
  컬러 패널의 선형 BT.2020/PQ 합성을 CPU 기준식과 비교해 통과했다.
  양자화 오차를 고려하며 영상/UI 허용 오차는 기존과 동일하다.
- 로컬 앱 재빌드 및 관련 6개 검사 6/6 통과(2.81초). GPU debug warning/error 0.
  전환 기능·톤매핑·새 런타임 의존성은 추가하지 않았다. 릴리스하지 않았다.
- 이번 마무리 후 EXE는 643,072byte로 이전 보완본과 크기가 같다.
  SHA256: D4698B4EC97E8DF640CBCAFDF08AE98666596813DCB6CE778B07EF7C3E59EFAE.

표준 대조의 범위는 지원하는 신호 경로의 수학/API 구현이다. 별도의 인증이나
미지원 HDR 형식까지 준수한다는 뜻은 아니다.

근거:

- [ITU-R BT.2100-3](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-3-202502-I!!PDF-E.pdf)
- [MF BT.2020 행렬 정의](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/ne-mfobjects-mfvideotransfermatrix)
- [DXVA 색차 위치 정의](https://learn.microsoft.com/en-us/windows/win32/api/dxva2api/ne-dxva2api-dxva2_videochromasubsampling)
- [Windows SDR UI white 단위](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_sdr_white_level)
