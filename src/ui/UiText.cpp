#include "UiText.h"

#include <string_view>
#include <unordered_map>

namespace llcv::ui_text {

const wchar_t* VsrSetupGuide(bool useEnglish) {
    return useEnglish
        ? L"Setup\n"
          L"1. In NVIDIA App > System > Video, enable RTX Video Super Resolution.\n"
          L"2. Select NVIDIA VSR under Video > Upscaling. F6 toggles the selected method.\n"
          L"3. Check NVIDIA App's active indicator while playing. This viewer cannot verify activation.\n\n"
          L"Supported input\n"
          L"NV12/YUY2 SDR, MJPEG (decoded to NV12), or P010 HDR10; display at source size or larger. "
          L"RGB24 and XRGB (RGB32) can also request VSR; actual activation depends on the driver. Downscaling bypasses VSR. HDR requires a recent NVIDIA driver; "
          L"native HDR10 is preserved, without SDR-to-HDR conversion.\n\n"
          L"Keep in mind\n"
          L"Capture resolution selects input size; Display resolution selects video size, including fullscreen, with or without VSR. "
          L"F6 keeps both sizes unchanged. F5 restores source-size display.\n"
          L"GPU processing can increase latency. Screenshots remain unfiltered source images."
        : L"설정 방법\n"
          L"1. NVIDIA App > 시스템 > 비디오에서 RTX Video Super Resolution을 켜세요.\n"
          L"2. 영상 > 업스케일링에서 NVIDIA VSR을 선택하세요. F6은 선택한 방식만 켜고 끕니다.\n"
          L"3. NVIDIA App의 활성 표시를 확인하세요. 이 뷰어는 실제 적용 여부를 확인하지 못합니다.\n\n"
          L"지원 조건\n"
          L"NV12/YUY2 SDR, MJPEG(NV12로 디코딩), P010 HDR10을 원본과 같거나 크게 표시할 때 사용합니다. "
          L"RGB24와 XRGB (RGB32)도 VSR을 요청할 수 있으며 실제 활성화는 드라이버에 따라 다릅니다. 축소 표시에는 적용하지 않습니다. HDR은 최신 NVIDIA 드라이버를 권장하며, "
          L"SDR→HDR 변환 없이 원래 HDR10을 유지합니다.\n\n"
          L"참고\n"
          L"캡처 해상도는 입력 크기, 표시 해상도는 전체화면을 포함한 영상 크기이며 VSR을 꺼도 유지됩니다. "
          L"F6은 두 해상도를 유지합니다. F5는 입력 크기로 표시합니다.\n"
          L"GPU 처리로 지연이 늘 수 있습니다. 스크린샷에는 VSR이 적용되지 않습니다.";
}

const wchar_t* Translate(const wchar_t* korean, bool useEnglish) {
    if (!korean || !useEnglish) return korean;
    // Keys and translations borrow process-lifetime literals. Lookup accepts a
    // string view, avoiding a temporary owning string on every English OSD/UI
    // refresh. Only the dictionary's first initialization allocates storage.
    static const std::unordered_map<std::wstring_view, const wchar_t*> english = {
        {L" · 사용 가능 · %d ms", L" · available · %d ms"},
        {L" (사용 가능 · %d ms)", L" (available · %d ms)"},
        {L" · 검사 중", L" · checking"},
        {L" (검사 중)", L" (checking)"},
        {L" · 사용 불가", L" · unavailable"},
        {L" (사용 불가)", L" (unavailable)"},
        {L"장치 검사 중…", L"Checking devices…"},
        {L"전체 장치 다시 검사", L"Recheck all devices"},
        {L"Exclusive 출력 장치 검사 중… %zu/%zu 완료", L"Checking Exclusive outputs… %zu/%zu complete"},
        {L"Exclusive 사용 가능 · 현재 출력 장치 · %d ms 이상", L"Exclusive available · selected output · %d ms or more"},
        {L"Exclusive 사용 가능 · %d ms 이상 선택 필요", L"Exclusive available · select %d ms or more"},
        {L"Exclusive 사용 불가 · 현재 출력 장치", L"Exclusive unavailable · selected output"},
        {L"Exclusive 검사 필요 · 현재 출력 장치", L"Exclusive check required · selected output"},
        {L" · 확인 보류", L" · retry required"},
        {L" (확인 보류)", L" (retry required)"},
        {L"Exclusive 확인 보류 · 장치 상태 확인 후 다시 검사해 주세요", L"Exclusive unverified · check the device, then retry manually"},
        {L"스크린샷 (F12)", L"Screenshots (F12)"},
        {L"Windows 기본 장치", L"Windows default device"},
        {L"선택 장치 없음", L"No selected device"},
        {L"WASAPI: 출력 사용 불가 · F2로 설정 확인", L"WASAPI: output unavailable (F2 for settings)"},
        {L"WASAPI: 출력 복구 실패 · F2로 설정 확인", L"WASAPI: recovery failed (F2 for settings)"},
        {L" (기본)", L" (default)"},
        {L"선택한 출력 장치", L"Selected output device"},
        {L" (기본 추적)", L" (following default)"},
        {L"음량  %d%%", L"Volume  %d%%"},
        {L"클리핑 없음", L"No clipping"},
        {L"클리핑 감지 중 (%llu회)", L"Clipping active (%llu events)"},
        {L"클리핑 기록 (%llu회)", L"Clipping recorded (%llu events)"},
        {L"%.2f ms (권장)", L"%.2f ms (recommended)"},
        {L"%.2f ms (최저)", L"%.2f ms (minimum)"},
        {L"Shared 저지연 지원 확인 중…", L"Checking Shared low-latency support…"},
        {L"Shared 저지연 · %.2f~%.2f ms · 검사 %.1f ms", L"Shared low latency · %.2f~%.2f ms · probe %.1f ms"},
        {L"Shared 기본 모드 · 저지연 API 미지원", L"Shared basic mode · low-latency API unavailable"},
        {L"지원 모드 없음: 다른 장치 또는 해상도를 선택하세요.", L"No supported mode: choose another device or resolution."},
        {L"지원 프레임 없음", L"No supported frame rate"},
        {L"지원 포맷 없음", L"No supported format"},
        {L"자동 선택 (NV12 우선 · 권장)", L"Auto select (NV12 first · recommended)"},
        {L"P010 10-bit HDR10", L"P010 10-bit HDR10"},
        {L"P010 HDR10 강제 (색 정보가 틀릴 때)", L"Force P010 HDR10 (incorrect color metadata)"},
        {L"HDR 색차 배치", L"HDR chroma"},
        {L"Top-left (호환성 해석)", L"Top-left (compatibility)"},
        {L"Left (호환성 해석)", L"Left (compatibility)"},
        {L"MJPEG (압축 호환)", L"MJPEG (compressed compatibility)"},
        {L"MJPEG 색상 해석", L"MJPEG color interpretation"},
        {L"자동 (권장)", L"Auto (recommended)"},
        {L"오디오 출력 모드", L"Audio output mode"},
        {L"오디오 출력 장치", L"Audio output device"},
        {L"ASIO 출력 드라이버", L"ASIO output driver"},
        {L"Windows 기본 출력 장치 따라가기 (권장)", L"Follow Windows default output (recommended)"},
        {L" (현재 기본)", L" (current default)"},
        {L"오디오 출력 버퍼", L"Audio output buffer"},
        {L"ASIO 드라이버 선호 버퍼 (드라이버 설정 사용)", L"ASIO driver preferred buffer (driver setting)"},
        {L"ASIO 출력 · 드라이버 기본 버퍼 사용 · 앱 클록 보정 가능", L"ASIO output · driver buffer · app clock correction available"},
        {L"100% 이상 볼륨 증폭 허용 (최대 200%)", L"Allow volume boost above 100% (up to 200%)"},
        {L"출력", L"Output"},
        {L"재생 · 편의", L"Playback & convenience"},
        {L"동기화 · 안정성", L"Sync & stability"},
        {L"캡처", L"Capture"},
        {L"영상", L"Video"},
        {L"창", L"Window"},
        {L"오디오", L"Audio"},
        {L"로그 폴더 열기", L"Open logs folder"},
        {L"로그 폴더를 열지 못했습니다.", L"Could not open the logs folder."},
        {L"진단 로그", L"Diagnostic logs"},
        {L"업데이트 확인", L"Update checks"},
        {L"현재 버전", L"Current version"},
        {L"최신 버전 확인", L"Check for updates now"},
        {L"최신 버전 확인 중…", L"Checking for updates…"},
        {L"최신 버전입니다.", L"You are up to date."},
        {L"최신 버전: %s", L"Latest version: %s"},
        {L"새 버전 %s을(를) 찾았습니다. 공식 설치 파일을 다운로드하시겠습니까?", L"Version %s is available. Download the official installer?"},
        {L"업데이트를 확인하지 못했습니다. 인터넷 연결을 확인한 뒤 다시 시도하세요.", L"Could not check for updates. Check your internet connection and try again."},
        {L"내부 오디오 확인 중…", L"Checking built-in audio…"},
        {L"영상 장치 내부 오디오 감지됨 · 자동 사용", L"Built-in audio detected · using automatically"},
        {L"좌측 상단 (기본)", L"Top-left (default)"},
        {L"우측 상단", L"Top-right"},
        {L"좌측 하단", L"Bottom-left"},
        {L"우측 하단", L"Bottom-right"},
        {L"클록 드리프트 보정", L"Clock-drift correction"},
        {L"끔 (원본 PCM · 음질 우선)", L"Off (unaltered PCM · quality first)"},
        {L"자동 (권장 · 필요 시 보정)", L"Auto (recommended · correct only when needed)"},
        {L"켬 (항상 리샘플링)", L"On (always resample)"},
        {L"PCM 버퍼 목표", L"PCM buffer target"},
        {L"10 ms (최저 지연)", L"10 ms (minimum latency)"},
        {L"15 ms (저지연 목표)", L"15 ms (low-latency target)"},
        {L"20 ms (안정 목표)", L"20 ms (stability target)"},
        {L"25 ms (권장 · 기본)", L"25 ms (recommended · default)"},
        {L"30 ms (안정성 우선)", L"30 ms (stability first)"},
        {L"백그라운드에서 자동 음소거", L"Mute automatically in background"},
        {L"화면 표시 방식", L"Presentation mode"},
        {L"저지연", L"Immediate"},
        {L"화면 확대 방식", L"Scaling mode"},
        {L"부드럽게", L"Smooth"},
        {L"선명하게", L"Sharp"},
        {L"캡처 장치", L"Capture device"},
        {L"자동 선택 (권장)", L"Auto select (recommended)"},
        {L"캡처 오디오 장치", L"Capture audio device"},
        {L"오디오 only: 영상 형식 확인 안 함", L"Audio-only: video mode is not checked"},
        {L"자동 선택 (영상 장치 오디오 우선 · 권장)", L"Auto select (video-device audio first · recommended)"},
        {L"캡처 해상도", L"Capture resolution"},
        {L"픽셀 포맷", L"Pixel format"},
        {L"프레임", L"Frame rate"},
        {L"지원 모드 확인 중...", L"Checking supported modes..."},
        {L"※ Pixel-perfect와 함께 켜면 모니터 이동 시 1:1이 깨질 수 있습니다.", L"※ With Pixel-perfect, moving monitors may break 1:1 scaling."},
        {L"전체화면 커서", L"Fullscreen cursor"},
        {L"자동 숨김 (권장)", L"Auto-hide (recommended)"},
        {L"항상 표시", L"Always show"},
        {L"F11  전체화면 켜기/끄기", L"F11  Toggle fullscreen"},
        {L"진단 콘솔 창 표시", L"Show diagnostic console window"},
        {L"다음 실행부터 바로 시작", L"Start directly next time"},
        {L"새 버전이 있습니다. 공식 설치 파일을 다운로드하시겠습니까?", L"A new version is available. Open the official installer download?"},
        {L"언어 / Language", L"Language"},
        {L"LLCV 설정", L"LLCV Settings"},
        {L"시작", L"Start"},
        {L"취소", L"Cancel"},
        {L"없음", L"None"},
        {L"방금", L"just now"},
        {L"%llu초 전", L"%llu seconds ago"},
        {L"%llu분 %llu초 전", L"%llu minutes %llu seconds ago"},
        {L"%llu시간 %llu분 전", L"%llu hours %llu minutes ago"},
        {L"측정 대기 중", L"Waiting for measurement"},
        {L"측정 중", L"Measuring"},
        {L"워밍업 · 시작 5초 제외", L"Warm-up · first 5 seconds excluded"},
        {L"리샘플러 출력 부족 감지", L"Resampler output shortage detected"},
        {L"리샘플러 보정 한계 접근", L"Resampler correction limit approaching"},
        {L"리샘플러 정상 작동", L"Resampler operating normally"},
        {L"보정 작동 · 오류 원인 아래 확인", L"Correction active · see error cause below"},
        {L"안정 · 보정 불필요", L"Stable · correction unnecessary"},
        {L"관찰 중", L"Observing"},
        {L"초기 오류 · 더 관찰", L"Initial error · observe longer"},
        {L"현재 안정 · 경과 관찰", L"Currently stable · continue observing"},
        {L"입력 지터 · 보정보다 대기량", L"Input jitter · increase buffering before correction"},
        {L"반복 불균형 · 보정 권장", L"Repeated imbalance · correction recommended"},
        {L"드문 오류 · 끔 유지 가능", L"Rare errors · Off can be kept"},
        {L"최저 지연 · 오류 없음", L"Minimum latency · no errors"},
        {L"PCM 버퍼 여유 정상", L"PCM buffer headroom normal"},
        {L"현재 안정 · 과거 오류 있음", L"Currently stable · previous errors"},
        {L"PCM 버퍼 부족 가능", L"Possible PCM buffer shortage"},
        {L"PCM 버퍼 있음 · 리샘플러 확인", L"PCM buffer available · check resampler"},
        {L"캡처 패킷 지연 감지", L"Capture packet delay detected"},
        {L"간헐적", L"Intermittent"},
        {L"연속", L"Burst"},
        {L"자동 관찰 중 · 원본 PCM", L"Auto observing · original PCM"},
        {L"자동 · 보정 작동", L"Auto · correction active"},
        {L"자동 · 관찰 중", L"Auto · observing"},
        {L"켬 · 리샘플러 사용", L"On · resampler active"},
        {L"끔 · 원본 PCM", L"Off · original PCM"},
        {L"백그라운드 음소거 중", L"Background mute active"},
        {L"PCM 연산 우회", L"PCM processing bypassed"},
        {L"음소거", L"Muted"},
        {L"PCM 감쇠 적용", L"PCM attenuation applied"},
        {L"PCM 증폭 적용", L"PCM boost applied"},
        {L"Pixel-perfect 시작 · Monitor-relative 이동", L"Pixel-perfect start · monitor-relative move"},
        {L"Pixel-perfect (고정 크기)", L"Pixel-perfect (fixed size)"},
        {L"Scaled (비율 고정)", L"Scaled (fixed aspect ratio)"},
    };
    const auto it = english.find(korean);
    return it == english.end() ? korean : it->second;
}

} // namespace llcv::ui_text
