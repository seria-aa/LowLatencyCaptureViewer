# CMAA2 오버플로 빈도 진단 (비배포용)

일반 앱에는 집계 코드를 넣지 않는다. 별도 `llcv_upscaling_diagnostic` 라이브러리와
`LowLatencyCaptureViewer_UpscalingDiagnostic.exe`만 비동기 읽기를 사용한다.
이 진단은 안전 우회가 실제 영상에서 얼마나 자주 발생하는지 알아보기 위한 것이며,
오버플로 경계의 화질 전환을 완화하거나 버퍼를 늘리는 수정은 아니다.

## 실행

Windows/MSVC Release 빌드에서 `BUILD_TESTING=ON`, `LLCV_UPSCALING_DIAGNOSTIC=ON`으로 구성한다.
일반 실행 파일과 설치 대상은 그대로이고 진단 exe는 명시적으로 빌드해야 한다.

```text
cmake --build build-upscaling-safety --target LowLatencyCaptureViewer_UpscalingDiagnostic UpscalingTelemetryTests
```

진단 exe를 직접 실행하면 기존 UI의 설정 창이 열린다. 실제 앱 프로필을 읽거나 복사하지 않고
매 실행마다 별도 임시 설정을 사용한다. SDR 캡처와 CMAA2 → NIS를 선택하고, 표시 해상도를
캡처보다 크게 설정한다(지원 범위는 최대 2배). HDR·필터 끔·1:1·축소에서는 계측되지 않는다.
실제 앱을 비롯한 다른 캡처 프로그램은 닫은 뒤 실행한다.

60초 고정 조건 확인은 다음과 같다. 이 값들은 해당 진단 프로세스에만 적용된다.

```text
LowLatencyCaptureViewer_UpscalingDiagnostic.exe --smoke-test-60 --overflow-live yes --overflow-no-dialog yes
```

- FHD 60fps NV12 캡처 → QHD 표시, CMAA2 → NIS 0.15.
- 일반 창으로 표시하며 60초 뒤 정상 종료한다. 실제 캡처 장치의 협상 결과는 임시 로그로 확인한다.
- 종료 후 exe 옆 `overflow-results/overflow-날짜-시간-PID.txt`에 집계 결과를 저장한다.
- `--overflow-no-dialog yes`를 생략하면 종료 시 저장 경로를 알려준다.
- `--overflow-no-dialog yes`에서 저장이 실패하면 창을 띄우지 않고 stderr에 오류를 기록한다. 뷰어가 성공했더라도 종료 코드 4를 반환하며, 기존 뷰어 오류가 있으면 그 코드를 유지한다.
- 기본 설정 경로와 로그는 Windows 임시 폴더의 `LLCV-TestProfile-...` 아래다.
- `--overflow-sampling off`는 필터를 유지하면서 계측만 끄는 진단 비교 옵션이다.
- 강제 종료 시에는 종료 보고서를 보장하지 않는다. 보고서를 얻기 위해 정상 종료해야 한다.

## 비동기 동작 및 해석

8개 슬롯에 제어 word 0(4바이트)을 복사하고 이벤트 쿼리를 기록한다.
최소 3회의 필터 렌더 호출 뒤 오래된 결과부터 확인한다.
`GetData(DONOTFLUSH)`로 준비 여부를 확인하고 `Map(DO_NOT_WAIT)`으로 읽는다.
준비되지 않았으면 다음 호출로 넘긴다. 한 호출에서 최대 8개 슬롯만 처리하며,
측정 경로에는 `Flush`, 기다리는 반복문, 파일 저장, 동적 할당이 없다.
자원이 부족하면 측정만 비활성화하고 렌더러 초기화는 실패시키지 않는다.
파일 저장은 뷰어 종료 후에만 한다.

이 정책은 Microsoft의 [GetData 플래그](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_async_getdata_flag)와
[DO_NOT_WAIT](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_map_flag)에 따른다.
CPU에서 GPU 완료를 기다리지 않는다는 뜻이지, GPU 복사나 API 호출 비용까지 0이라는 뜻은 아니다.

| 보고 항목 | 의미 |
|---|---|
| frames / collected | 필터 렌더 호출 수 / 결과를 읽은 수. 서로 다른 캡처 프레임 수와 동일하다고 가정하지 않는다. |
| overflow | 읽은 결과 중 하나 이상의 목록이 초과한 수 |
| candidate/item/location_overflow | 원인별 수. 한 프레임에서 여러 원인이 발생할 수 있다. |
| unknown | 읽지 못한 수. 종료 시 남은 결과·슬롯 포화·오류 등을 포함하며 정상으로 간주하지 않는다. |
| coverage_percent | 전체 필터 호출 중 결과를 확보한 비율 |
| observed_overflow_percent | 확보한 결과에서의 초과 비율. 누락이 있으면 전체 빈도와 다를 수 있다. |
| overflow_lower/upper_bound_percent | 미확인 결과가 모두 정상 / 모두 초과인 경우의 전체 비율 범위 |
| adjacent_sample_pairs / observed_transitions | 연속해서 확보한 호출 쌍 / 정상↔초과 변화. 중간 누락이나 파이프라인 재설정은 연결하지 않는다. |
| mean/max_read_age | 결과를 읽을 때까지 지난 필터 호출 수. 밀리초 단위 지연이 아니다. |

버퍼가 가득 차도 미처 읽지 않은 슬롯을 덮어쓰지 않는다. 종료·재설정 때 GPU를 비우기 위해 기다리지 않는다.
수집 결과가 0이면 '초과 0건'이 아니라 측정 불가다. 샘플 수와 커버리지를 함께 확인해야 한다.
한 장면에서 초과가 없었다고 다른 게임·해상도·MJPEG에도 없다고 일반화하지 않는다.

## 회귀 검사

```text
ctest --test-dir build-upscaling-safety -R "^Upscaling(Safety|Telemetry)(Hardware|Warp)$" -V
ctest --test-dir build-upscaling-safety -R "^UpscalingTelemetryBoundary$" --output-on-failure
ctest --test-dir build-upscaling-safety -R "^UpscalingOverflowReportTests$" --output-on-failure
UpscalingTelemetryTests.exe --benchmark
```

- 하드웨어/WARP에서 픽셀 보존, 3호출 지연, 실제 셰이더의 원인별 집계, 중복 없는 수집,
  상태 전환, 종료 시 미확인, 자원 생성 실패, 슬롯 포화 시 건너뛰기를 확인한다.
- 정상 라이브러리의 심볼과 앱 빌드 정의에 진단 코드가 없고, 진단 앱에는 프로필 격리 정의가
  있는지 검사한다. 진단 exe가 설치 목록에 들어가지 않는지도 검사한다.
- 활성 목록 검사에는 서로 다른 quad의 동일 항목 공유 및 꼬리 공유 검출을 추가했다.
- 수정 전 셰이더로 실패를 확인한 것은 최초 테스트 버전이다. 현재 보완된 검사 전체를
  수정 전 셰이더로 재실행한 것으로 설명하지 않는다.

2026-10-26 최종 관련 **19개 모두 통과**(68.09초): 기존 15개 화질·HDR 우회·OSD·설정·단축키·
재생·녹화·프로필 검사에 `UpscalingTelemetryHardware`, `UpscalingTelemetryWarp`,
`UpscalingTelemetryBoundary`, `PackageDocsTests`를 더했다.
진단용 문서도 일반 배포 문서 묶음에서 제외했다. 기존 릴리스 exe와 실제 설정 두 파일의 해시는 유지됐다.
로컬 원시 결과는 `outputs/upscaling-safety-fix/telemetry-tests.log`, `telemetry-regression.log`,
`telemetry-benchmark.log`에 보존한다. 원시 파일은 커밋 대상이 아니며 위 명령으로 재현할 수 있다.

### 계측 비용 관측

RTX 3080, 합성 FHD CMAA 단계, 60Hz 호스트 간격으로 계측 OFF/ON 순서를 교차했다.
각 상태 3라운드, 라운드마다 준비 25회 후 120회 측정했다. NIS·표시·HDMI 전체 지연은 측정 대상이 아니다.
타임스탬프 준비 대기는 벤치마크 호스트에서만 수행하며 CPU Draw 측정 구간 밖에 둔다.

| 평균값의 3라운드 범위 | 계측 OFF | 계측 ON |
|---|---:|---:|
| CPU Draw 호출 시간 | 0.007355~0.008117 ms | 0.013236~0.014461 ms |
| GPU CMAA 구간 | 1.919~5.488 ms | 1.058~4.998 ms |

CPU 평균 증가는 약 0.006 ms였다. GPU 값은 OFF에서도 크게 변동하고 범위가 겹쳐,
추가 GPU 비용을 안정적으로 분리하지 못했다. 무부하·속도 향상·실제 표시 지연 무변화를 주장하지 않는다.
ON 각 라운드는 145호출 중 142개를 수집하고, 포화 건너뛰기는 0이었다. 나머지 3개는 종료 시 미확인이다.
초기 비교는 전체 FHD 이미지 복사·변환을 기다리는 방식이었으므로, 해당 작업을 제거한 위 재측정과 구분한다.

일반 빌드는 진단 라이브러리와 이 계측 비용을 포함하지 않는다.

### 커밋 전 최종 재검증

2026-10-26: 최종 CMake·헤더 변경을 포함해 일반 앱과 전체 기본 빌드 대상, 별도 진단 앱을 다시 빌드했다.
새 보고서 테스트를 처음 연결할 때 콘솔 진입점 설정 누락으로 링크에 실패했고, `WIN32_EXECUTABLE FALSE`로 수정 후 전체 빌드를 완료했다.
기존 19개 검사와 `UpscalingOverflowReportTests` **20/20 통과**(71.91초).
저장 실패 시 headless 오류 출력·실패 종료, 기존 뷰어 오류 보존 및 성공/대화상자 동작을 검사한다.
활성 목록의 중복 조건만 합쳤으며 순환·quad 간 항목 공유 검사는 유지한다.
로그: `outputs/upscaling-safety-fix/final-before-commit-tests.log`.

## 실제 입력 60초 결과 (2026-10-26)

사용자가 준비한 SDR 게임 입력으로 별도 진단 뷰어를 실행했다.
연결 로그에서 NV12 1920×1080, duration 166666(100ns 단위, 약 60fps),
CMAA2 → NIS active=1 / HDR=0 / sharpness=0.15를 확인했다.

| 항목 | 결과 |
|---|---:|
| 필터 렌더 호출 | 3,601 |
| 수집 | 3,598 (99.9167%) |
| 오버플로 (후보 / 항목 / 위치) | 0 (0 / 0 / 0) |
| 종료 시 미확인 | 3 |
| 슬롯 포화 / 조회·매핑·초기화 오류 | 모두 0 |
| 읽기 시점 | 평균·최대 모두 3호출 뒤 |
| 관측한 정상↔초과 전환 | 0 |

확인한 3,598개 결과에서는 발생하지 않았다. 미확인 3개까지 포함한 전체 호출의 비율 범위는
0~0.08331%다. 현재 장면의 60초 관측이며 다른 장면·장치·MJPEG에서의 빈도까지 의미하지 않는다.
초과 경계에서 화질이 튀는 합성 결과와 모순되지 않으며, 버퍼 증설 필요성이 확인됐다고도 볼 수 없다.

실제 `settings.ini` 및 `recording-prototype.ini`의 SHA-256은 실행 전후 동일했다.
로컬 결과는 `build-upscaling-safety/overflow-results/overflow-20261026-191441-282-3884.txt`다.
원시 로그에는 개인 장치명이 포함될 수 있어 자동 업로드하지 않는다. 위 표와 테스트 소스로 결과를 기록한다.
