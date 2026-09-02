/*
 * Low Latency Capture Viewer
 * Copyright (C) 2026 seria-aa
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <shellapi.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <avrt.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <dxva.h>

#include "audio/AudioMix.h"
#include "audio/AudioDeviceCapabilities.h"
#include "audio/AsioOutput.h"
#include "audio/CaptureAudioFormat.h"
#include "audio/PcmPipeline.h"
#include "audio/WasapiOutput.h"
#include "capture/DirectShowDevices.h"
#include "capture/AudioSampleGrabber.h"
#include "capture/DirectShowGraphResources.h"
#include "capture/LatestVideoSample.h"
#include "diagnostics/Logger.h"
#include "diagnostics/AudioErrorHistory.h"
#include "settings/AppSettings.h"
#include "settings/SettingsStore.h"
#include "ui/AudioOsdLayout.h"
#include "ui/PresentationModeUi.h"
#include "ui/WindowGeometry.h"
#include "update/UpdateChecker.h"
#include "video/CaptureColorMetadata.h"
#include "video/DirectShowVideoFormat.h"
#include "video/MjpegDecoder.h"
#include "video/VideoColor.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <future>
#include <functional>
#include <cstdarg>
#include <unordered_map>

// -----------------------------------------------------------------------------
// User-tested settings.
// -----------------------------------------------------------------------------

constexpr wchar_t kCaptureName[] = L"AVerMedia HD Capture GC573 1";
constexpr wchar_t kAudioPinName[] = L"Audio";
constexpr wchar_t kVideoPinName[] = L"Video";

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBitsPerSample = 16;
constexpr wchar_t kAppVersionLabel[] = L"v1.2.5.1";

constexpr int kRecommendedCaptureBufferMs = 20;
constexpr int kMaximumVolumePercent = 200;
constexpr int kAudioOsdWidth = llcv::audio_osd::kWidth;
constexpr int kAudioOsdHeight = llcv::audio_osd::kHeight;
static constexpr int kWasapiBufferOptionsMs[] = {5, 10, 15, 20, 30, 40};
// This viewer treats 40 ms as the upper edge of a useful low-latency
// Exclusive configuration. Higher values are deliberately not offered.
static constexpr int kExclusiveBufferOptionsMs[] = {5, 10, 15, 20, 30, 40};
constexpr size_t kMaximumExclusiveEndpointCacheEntries = 32;
constexpr int kRecommendedWasapiBufferMs = 20;
static constexpr int kPcmQueueOptionsMs[] = {10, 15, 20, 25, 30};
constexpr int kLowestPcmQueueMs = 10;
constexpr int kRecommendedPcmQueueMs = 20;
// Auto correction deliberately uses a wide hysteresis window and latches on
// for the rest of the session once sustained drift is observed. This avoids
// repeatedly inserting/removing the resampler while still leaving the normal
// path untouched for short-lived scheduling jitter.
constexpr double kAutoCorrectionEngageDeviationFrames = 96.0;
constexpr uint64_t kAutoCorrectionEngageHoldMs = 5000;

using AppSettings = llcv::settings::AppSettings;
using AudioClient3Support = llcv::audio_device::SharedModeSupport;
using AudioEndpointInfo = llcv::audio_device::EndpointInfo;
using AudioMode = llcv::settings::AudioMode;
using PcmRing = llcv::audio::PcmRing;
using SincDriftResampler = llcv::audio::SincDriftResampler;
using CaptureDeviceInfo = llcv::capture::DeviceInfo;
using DriftCorrectionMode = llcv::settings::DriftCorrectionMode;
using AudioErrorCause = llcv::diagnostics::AudioErrorCause;
using AudioErrorHistory = llcv::diagnostics::AudioErrorHistory;
using AudioErrorKind = llcv::diagnostics::AudioErrorKind;
using AudioPatternStats = llcv::diagnostics::AudioPatternStats;
using ExclusiveCompatibilityProbe = llcv::audio_device::ExclusiveProbe;
using ExclusiveEndpointCacheEntry =
    llcv::settings::ExclusiveEndpointCacheEntry;
using FullscreenCursorMode = llcv::settings::FullscreenCursorMode;
using PresentationMode = llcv::settings::PresentationMode;
using PixelFormatSupport = llcv::video::PixelFormatSupport;
using ScalingMode = llcv::settings::ScalingMode;
using UiLanguage = llcv::settings::UiLanguage;
using VideoPixelFormat = llcv::settings::VideoPixelFormat;
using VideoPreset = llcv::settings::VideoPreset;
using VolumeHudPosition = llcv::settings::VolumeHudPosition;

enum class InternalCaptureAudioState {
    Checking,
    Available,
    SeparateDeviceNeeded,
    Unknown,
};

struct InternalCaptureAudioProbe {
    InternalCaptureAudioState state = InternalCaptureAudioState::Checking;
    HRESULT result = S_OK;
};

using DirectShowColorMetadata = llcv::video::CaptureColorMetadata;

struct VideoPresetInfo {
    VideoPreset preset;
    int width;
    int height;
    int framerate;
    const wchar_t* label;
};

static constexpr VideoPresetInfo kVideoPresets[] = {
    {VideoPreset::R1920x1080, 1920, 1080, 120, L"1920 x 1080"},
    {VideoPreset::R2560x1440, 2560, 1440, 120, L"2560 x 1440"},
    {VideoPreset::R3840x2160, 3840, 2160, 60, L"3840 x 2160"},
};

static AppSettings g_settings{};
static bool g_exclusiveStartupFallback = false;
static AudioMode g_exclusiveStartupRequestedMode = AudioMode::WasapiShared;
static std::atomic<bool> g_fullscreen{false};
static std::atomic<uint64_t> g_outputConfigurationGeneration{0};

static bool IsEnglishUi() {
    if (g_settings.uiLanguage == UiLanguage::English) return true;
    if (g_settings.uiLanguage == UiLanguage::Korean) return false;
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(localeName, ARRAYSIZE(localeName)) > 0) {
        return _wcsnicmp(localeName, L"ko", 2) != 0;
    }
    return false;
}

static const wchar_t* UiText(const wchar_t* korean) {
    if (!korean || !IsEnglishUi()) return korean;
    // The map is intentionally keyed by the existing Korean source strings.
    // This keeps settings files backward-compatible and lets the UI switch
    // language without a second executable or a runtime translation service.
    static const std::unordered_map<std::wstring, std::wstring> english = {
        {L"Windows 기본 장치", L"Windows default device"},
        {L"선택 장치 없음", L"No selected device"},
        {L" (기본)", L" (default)"},
        {L"선택한 출력 장치", L"Selected output device"},
        {L" (기본 추적)", L" (following default)"},
        {L"음량  %d%%", L"Volume  %d%%"},
        {L"클리핑 없음", L"No clipping"},
        {L"클리핑 감지 중 (%llu회)", L"Clipping active (%llu events)"},
        {L"클리핑 기록 (%llu회)", L"Clipping recorded (%llu events)"},
        {L"%.2f ms (권장)", L"%.2f ms (recommended)"},
        {L"%.2f ms (최저)", L"%.2f ms (minimum)"},
        {L"%d ms (권장)", L"%d ms (recommended)"},
        {L"Shared 저지연 지원 확인 중…", L"Checking Shared low-latency support…"},
        {L"Shared 저지연 · %.2f~%.2f ms · 검사 %.1f ms", L"Shared low latency · %.2f~%.2f ms · probe %.1f ms"},
        {L"Shared 기본 모드 · 저지연 API 미지원", L"Shared basic mode · low-latency API unavailable"},
        {L"지원 모드 없음: 다른 장치 또는 해상도를 선택하세요.", L"No supported mode: choose another device or resolution."},
        {L"자동 인식: ", L"Detected: "},
        {L"지원 프레임 없음", L"No supported frame rate"},
        {L"자동 선택 (권장 프레임)", L"Auto select (recommended frame rate)"},
        {L"지원 포맷 없음", L"No supported format"},
        {L"자동 선택 (NV12 우선 · 권장)", L"Auto select (NV12 first · recommended)"},
        {L"P010 10-bit HDR10 (실험적)", L"P010 10-bit HDR10 (experimental)"},
        {L"P010 HDR10 강제 (메타데이터 없을 때 · 실험적)", L"Force P010 HDR10 (when metadata is missing · experimental)"},
        {L"MJPEG (실험적 압축 호환)", L"MJPEG (experimental compressed compatibility)"},
        {L"MJPEG 색상 해석", L"MJPEG color interpretation"},
        {L"자동 (권장)", L"Auto (recommended)"},
        {L"오디오 출력 모드", L"Audio output mode"},
        {L"WASAPI Shared (호환성 우선 · 권장)", L"WASAPI Shared (compatibility · recommended)"},
        {L"WASAPI Exclusive (지연 최소화 · 장치 독점)", L"WASAPI Exclusive (minimum latency · exclusive device)"},
        {L"ASIO (지연 최소화 · 드라이버 필요 · 실험적)", L"ASIO (minimum latency · driver required · experimental)"},
        {L"오디오 출력 장치", L"Audio output device"},
        {L"WASAPI 출력 장치", L"WASAPI output device"},
        {L"ASIO 출력 드라이버", L"ASIO output driver"},
        {L"Windows 기본 출력 장치 따라가기 (권장)", L"Follow Windows default output (recommended)"},
        {L" (현재 기본)", L" (current default)"},
        {L"오디오 출력 버퍼", L"Audio output buffer"},
        {L"ASIO 드라이버 선호 버퍼 (드라이버 설정 사용)", L"ASIO driver preferred buffer (driver setting)"},
        {L"ASIO 출력 · 드라이버 기본 버퍼 사용 · 앱 클록 보정 가능", L"ASIO output · driver buffer · app clock correction available"},
        {L"볼륨 HUD 위치", L"Volume HUD position"},
        {L"100% 이상 볼륨 증폭 허용 (최대 200%)", L"Allow volume boost above 100% (up to 200%)"},
        {L"출력", L"Output"},
        {L"재생 · 편의", L"Playback & convenience"},
        {L"동기화 · 안정성", L"Sync & stability"},
        {L"캡처", L"Capture"},
        {L"영상", L"Video"},
        {L"창", L"Window"},
        {L"오디오", L"Audio"},
        {L"영상 · 창", L"Video & window"},
        {L"단축키 · 진단", L"Shortcuts & diagnostics"},
        {L"단축키", L"Shortcuts"},
        {L"진단 · 문제 해결", L"Diagnostics & troubleshooting"},
        {L"로그 폴더 열기", L"Open logs folder"},
        {L"로그 폴더를 열지 못했습니다.", L"Could not open the logs folder."},
        {L"진단 로그", L"Diagnostic logs"},
        {L"시작을 누르면 현재 설정으로 뷰어를 엽니다.\r\n\r\nF2  설정 다시 열기\r\nF3  오디오 OSD\r\nF5  Pixel-perfect 크기로 맞추기\r\nF11  보더리스 전체화면 켜기/끄기\r\nTab  실시간 진단 표시\r\nEsc  전체화면 해제 또는 종료", L"Select Start to open the viewer with the current settings.\r\n\r\nF2  Reopen settings\r\nF3  Audio OSD\r\nF5  Restore Pixel-perfect size\r\nF11  Toggle borderless fullscreen\r\nTab  Live diagnostics\r\nEsc  Leave fullscreen or exit"},
        {L"F2  설정 다시 열기\r\nF3  오디오 OSD\r\nF5  Pixel-perfect 크기로 맞추기\r\nF11  보더리스 전체화면 켜기/끄기\r\nTab  실시간 진단 표시\r\nEsc  전체화면 해제 또는 종료", L"F2  Reopen settings\r\nF3  Audio OSD\r\nF5  Restore Pixel-perfect size\r\nF11  Toggle borderless fullscreen\r\nTab  Live diagnostics\r\nEsc  Leave fullscreen or exit"},
        {L"문제가 생길 때만 로그 저장을 켜고 같은 문제를 재현하세요.\r\n로그는 사용자 폴더의 logs에 저장됩니다.", L"Enable log saving only when a problem occurs, then reproduce it.\r\nLogs are saved in the user-data logs folder."},
        {L"업데이트", L"Updates"},
        {L"빠른 안내", L"Quick guide"},
        {L"이 창에서 설정을 저장한 뒤 시작할 수 있습니다.\r\n\r\nF2  설정 다시 열기\r\nF3  오디오 OSD\r\nF5  Pixel-perfect 크기로 맞추기\r\nF11  보더리스 전체화면 켜기/끄기\r\nTab  실시간 진단 표시\r\nEsc  전체화면 해제 또는 종료\r\n\r\n문제가 있으면 진단 로그를 켠 뒤 재현하고, 사용자 폴더의 logs 파일을 첨부해 주세요.", L"Save settings here, then start the viewer.\r\n\r\nF2  Reopen settings\r\nF3  Audio OSD\r\nF5  Restore Pixel-perfect size\r\nF11  Toggle borderless fullscreen\r\nTab  Live diagnostics\r\nEsc  Leave fullscreen or exit\r\n\r\nFor a problem report, enable diagnostic logging, reproduce the issue, and attach the log from the user-data logs folder."},
        {L"업데이트 확인", L"Update checks"},
        {L"자동 확인은 시작 후 백그라운드에서 최신 릴리스를 확인합니다. 새 버전이 있으면 공식 설치 파일 다운로드를 안내합니다.", L"Automatic checks run in the background after startup. When a new version is available, the app offers the official installer download."},
        {L"현재 버전", L"Current version"},
        {L"최신 버전 확인", L"Check for updates now"},
        {L"최신 버전 확인 중…", L"Checking for updates…"},
        {L"최신 버전입니다.", L"You are up to date."},
        {L"최신 버전: %s", L"Latest version: %s"},
        {L"새 버전 %s을(를) 찾았습니다. 공식 설치 파일을 다운로드하시겠습니까?", L"Version %s is available. Download the official installer?"},
        {L"업데이트를 확인하지 못했습니다. 인터넷 연결을 확인한 뒤 다시 시도하세요.", L"Could not check for updates. Check your internet connection and try again."},
        {L"▸ 고급 설정", L"▸ Advanced settings"},
        {L"⌄ 고급 설정 숨기기", L"⌄ Hide advanced settings"},
        {L"내부 오디오 확인 중…", L"Checking built-in audio…"},
        {L"영상 장치 내부 오디오 감지됨 · 자동 사용", L"Built-in audio detected · using automatically"},
        {L"별도 캡처 오디오 장치 선택", L"Select a separate capture audio device"},
        {L"내부 오디오 확인 불가 · 자동 선택", L"Built-in audio unavailable · automatic selection"},
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
        {L"20 ms (안정 권장)", L"20 ms (stable recommendation)"},
        {L"25 ms (안정 여유)", L"25 ms (extra stability)"},
        {L"30 ms (안정성 우선)", L"30 ms (stability first)"},
        {L"백그라운드에서 자동 음소거", L"Mute automatically in background"},
        {L"화면 표시 방식", L"Presentation mode"},
        {L"저지연", L"Immediate"},
        {L"화면 확대 방식", L"Scaling mode"},
        {L"부드럽게", L"Smooth"},
        {L"선명하게", L"Sharp"},
        {L"캡처 장치", L"Capture device"},
        {L"자동 선택 (GC573 우선 · 권장)", L"Auto select (GC573 first · recommended)"},
        {L"캡처 오디오 장치", L"Capture audio device"},
        {L"오디오 only 모드", L"Audio-only mode"},
        {L"오디오 only: 영상 형식 확인 안 함", L"Audio-only: video mode is not checked"},
        {L"자동 선택 (영상 장치 오디오 우선 · 권장)", L"Auto select (video-device audio first · recommended)"},
        {L"같은 캡처 장치 오디오를 우선 사용하고, 없으면 이름이 일치하는 별도 입력을 찾습니다.", L"Uses audio on the video device first, then finds a separately exposed matching input."},
        {L" (실험적)", L" (experimental)"},
        {L"캡처 해상도", L"Capture resolution"},
        {L"픽셀 포맷", L"Pixel format"},
        {L"프레임", L"Frame rate"},
        {L"지원 모드 확인 중...", L"Checking supported modes..."},
        {L"Pixel-perfect (1:1 · 창 크기 고정)", L"Pixel-perfect (1:1 · fixed window size)"},
        {L"모니터 이동 시 상대적 창 크기 유지 (독립 옵션)", L"Keep relative window size when moving monitors (independent)"},
        {L"※ Pixel-perfect와 함께 켜면 모니터 이동 시 1:1이 깨질 수 있습니다.", L"※ With Pixel-perfect, moving monitors may break 1:1 scaling."},
        {L"제목 표시줄 숨기기 (borderless 창)", L"Hide title bar (borderless window)"},
        {L"창을 모니터 가장자리에 스냅 (권장)", L"Snap window to monitor edges (recommended)"},
        {L"전체화면 커서", L"Fullscreen cursor"},
        {L"자동 숨김 (권장)", L"Auto-hide (recommended)"},
        {L"항상 표시", L"Always show"},
        {L"F11  보더리스 전체화면 켜기/끄기", L"F11  Toggle borderless fullscreen"},
        {L"진단 로그 파일 저장 (사용자 폴더)", L"Save diagnostic log (user folder)"},
        {L"진단 콘솔 창 표시", L"Show diagnostic console window"},
        {L"다음 실행부터 바로 시작", L"Start directly next time"},
        {L"저장된 설정으로 바로 실행 · Shift 실행 또는 F2로 설정 열기", L"Starts with saved settings · hold Shift at launch or press F2 for settings"},
        {L"업데이트 자동 확인 (시작 후 백그라운드)", L"Check for updates automatically (in background after startup)"},
        {L"새 버전이 있습니다. 공식 설치 파일을 다운로드하시겠습니까?", L"A new version is available. Open the official installer download?"},
        {L"업데이트 확인", L"Update check"},
        {L"업데이트를 확인할 수 없습니다.", L"Could not check for updates."},
        {L"언어 / Language", L"Language"},
        {L"Low Latency Capture Viewer 설정", L"Low Latency Capture Viewer Settings"},
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
        {L"오류 패턴", L"Error pattern"},
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
        {L"자동 리샘플링", L"Automatic resampling"},
        {L"끔 (원본 PCM)", L"Off (unaltered PCM)"},
        {L"Pixel-perfect 시작 · Monitor-relative 이동", L"Pixel-perfect start · monitor-relative move"},
        {L"Pixel-perfect (고정 크기)", L"Pixel-perfect (fixed size)"},
        {L"Scaled (비율 고정)", L"Scaled (fixed aspect ratio)"},
    };
    const auto it = english.find(korean);
    return it == english.end() ? korean : it->second.c_str();
}

#define UI_TEXT(text) UiText(text)

enum class ExclusiveEndpointState {
    Unknown,
    Testing,
    Supported,
    Unsupported,
};

struct ExclusiveEndpointVerification {
    ExclusiveEndpointState state = ExclusiveEndpointState::Unknown;
    int recommendedBufferMs = 0;
    std::wstring summary;
};

struct ExclusiveEndpointProbeMessage {
    size_t endpointIndex = 0;
    ExclusiveCompatibilityProbe probe;
};

// 500 ms ring capacity. The program deliberately does NOT wait for this much
// data; it is only headroom. Old data is dropped if the producer overruns.
constexpr size_t kRingFrames = 48000 / 2;

static HWND g_videoHost = nullptr;
static bool g_suppressSettingsSave = false;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_updateCheckStop{false};
static std::thread g_updateCheckThread;
static std::atomic<bool> g_restartToSettings{false};
static std::atomic<uint64_t> g_videoCapturedFrames{0};
static std::atomic<uint64_t> g_videoPresentedFrames{0};
static std::atomic<uint64_t> g_videoReplacedFrames{0};
static std::atomic<int64_t> g_videoAppLatencyUs{-1};
static std::atomic<UINT32> g_videoStride{0};
static std::atomic<int> g_videoConfiguredFps{0};
static std::atomic<bool> g_videoTearing{false};
static std::atomic<bool> g_directVideoActive{false};
static std::atomic<HRESULT> g_captureFailureHr{S_OK};
static std::atomic<UINT32> g_audioActualBufferFrames{0};
static std::atomic<UINT32> g_audioWasapiPaddingFrames{0};
static std::atomic<bool> g_asioAudioStarted{false};
static std::atomic<UINT32> g_audioCapturePacketFrames{0};
static std::atomic<int64_t> g_audioCaptureIntervalUs{0};
static std::atomic<LONG> g_audioCaptureAllocatorFrames{0};
static std::atomic<LONG> g_audioCaptureAllocatorBuffers{0};
static std::atomic<UINT32> g_audioRingFrames{0};
static std::atomic<UINT32> g_audioResamplerFrames{0};
static std::atomic<int> g_audioResamplePpm{0};
static std::atomic<bool> g_audioResamplerActive{false};
static std::atomic<uint64_t> g_audioResampledOutputFrames{0};
static std::atomic<uint64_t> g_audioCaptureCallbacks{0};
static std::atomic<uint64_t> g_audioCaptureFrames{0};
static std::atomic<uint64_t> g_audioCaptureIntervalTotalUs{0};
static std::atomic<uint64_t> g_audioUnderrunFrames{0};
static std::atomic<uint64_t> g_audioOverrunFrames{0};
static std::atomic<uint64_t> g_audioMonitorStartMs{0};
static std::atomic<uint64_t> g_audioLastUnderrunMs{0};
static std::atomic<uint64_t> g_audioLastOverrunMs{0};
static std::atomic<uint64_t> g_audioLastCaptureCallbackMs{0};
static std::atomic<uint64_t> g_audioLatePacketUnderruns{0};
static std::atomic<uint64_t> g_audioResamplerUnderruns{0};
static std::atomic<UINT32> g_audioMinimumPreRenderFrames{UINT32_MAX};
static std::atomic<UINT32> g_audioQueueTargetFrames{0};
static std::atomic<int> g_volumePercent{100};
static std::atomic<int> g_leftVolumePercent{100};
static std::atomic<int> g_rightVolumePercent{100};
static std::atomic<bool> g_backgroundAudioMuted{false};
static std::atomic<uint64_t> g_volumeHudUntilMs{0};
enum class TransientHudContent { Volume, OneToOne, OneToOneUnavailable };
static std::atomic<TransientHudContent> g_transientHudContent{
    TransientHudContent::Volume};
static std::atomic<uint64_t> g_overlayGeneration{1};
static std::atomic<uint64_t> g_overlayRenderedFrames{0};
static std::atomic<double> g_osdInputFps{0.0};
static std::atomic<double> g_osdPresentFps{0.0};
static std::atomic<bool> g_osdVisible{false};
static std::atomic<bool> g_audioOsdVisible{false};
static std::atomic<int> g_audioOsdHoverTarget{0}; // 0 = none, 1 = left, 2 = right
static std::atomic<int> g_audioPeakLeft{0};
static std::atomic<int> g_audioPeakRight{0};
static std::atomic<uint64_t> g_audioClipCount{0};
static std::atomic<uint64_t> g_audioClipUntilMs{0};
static constexpr uint64_t kOsdTrackingWarmupMs = 5000;
static constexpr uint64_t kAudioTrackingWarmupMs = 5000;
static std::atomic<uint64_t> g_osdTrackingStartMs{UINT64_MAX};
static std::atomic<uint64_t> g_audioTrackingStartMs{UINT64_MAX};
static std::atomic<bool> g_captureAudioAvailable{true};
static std::wstring g_activeCaptureDeviceName = kCaptureName;
static std::wstring g_activeCaptureAudioDeviceName;
static std::wstring g_activeAudioOutputName = L"Windows 기본 장치";
static std::atomic<int> g_activePixelFormat{
    static_cast<int>(VideoPixelFormat::Nv12)};
static std::atomic<bool> g_hdrOutputActive{false};
static std::atomic<int> g_activeVideoColorMatrix{
    static_cast<int>(llcv::video_color::Matrix::Bt709)};
static std::atomic<int> g_activeVideoColorRange{
    static_cast<int>(llcv::video_color::Range::Limited)};
static std::atomic<int> g_activeVideoColorMatrixSource{
    static_cast<int>(llcv::video_color::Source::Default)};
static std::atomic<int> g_activeVideoColorRangeSource{
    static_cast<int>(llcv::video_color::Source::Default)};
static llcv::diagnostics::Logger g_logger;
static std::mutex g_activeAudioOutputMutex;

// Error events are rare. A small bounded history is enough to distinguish a
// burst from occasional errors without adding work to the normal audio loop.
constexpr size_t kAudioErrorHistoryCapacity = 128;
static AudioErrorHistory g_audioErrorHistory{kAudioErrorHistoryCapacity};

static bool AudioTrackingActive();

static void RecordAudioErrorEvent(uint64_t timestampMs, uint32_t frames,
                                  AudioErrorKind kind,
                                  AudioErrorCause cause) {
    if (!timestampMs || !AudioTrackingActive()) return;
    g_audioErrorHistory.Record({timestampMs, frames, kind, cause});
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

template<class T>
static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static int TeeFwprintf(FILE* stream, const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = g_logger.PrintV(stream, format, args);
    va_end(args);
    return result;
}

#define fwprintf TeeFwprintf

static const wchar_t* PixelFormatName(VideoPixelFormat format) {
    return llcv::video::PixelFormatName(format);
}

static bool IsCompressedVideoFormat(VideoPixelFormat format) {
    return llcv::video::IsCompressedVideoFormat(format);
}

static bool IsAutoSelectableVideoFormat(VideoPixelFormat format) {
    return llcv::video::IsAutoSelectableVideoFormat(format);
}

static DXGI_FORMAT PixelFormatDxgi(VideoPixelFormat format) {
    switch (format) {
    case VideoPixelFormat::Yuy2: return DXGI_FORMAT_YUY2;
    case VideoPixelFormat::P010: return DXGI_FORMAT_P010;
    default: return DXGI_FORMAT_NV12;
    }
}

static bool OsdTrackingActive() {
    return GetTickCount64() >=
        g_osdTrackingStartMs.load(std::memory_order_acquire);
}

static bool AudioTrackingActive() {
    return GetTickCount64() >=
        g_audioTrackingStartMs.load(std::memory_order_acquire);
}

static bool AudioResamplerActive() {
    return g_audioResamplerActive.load(std::memory_order_acquire);
}

static void SetActiveAudioOutputName(const std::wstring& name) {
    std::lock_guard<std::mutex> lock(g_activeAudioOutputMutex);
    g_activeAudioOutputName = name;
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
}

static std::wstring ActiveAudioOutputName() {
    std::lock_guard<std::mutex> lock(g_activeAudioOutputMutex);
    return g_activeAudioOutputName;
}

static void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0) {
        CoTaskMemFree((PVOID)mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = nullptr;
    }
    if (mt.pUnk != nullptr) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

static std::wstring HrText(HRESULT hr) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, hr, 0,
                   reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring out = msg ? msg : L"(unknown HRESULT)";
    if (msg) LocalFree(msg);
    return out;
}

static void LogHr(const wchar_t* where, HRESULT hr) {
    fwprintf(stderr, L"%s failed: 0x%08X %s\n",
             where, static_cast<unsigned>(hr), HrText(hr).c_str());
}

static void LogD3DFailureEvent(HRESULT failureHr, HRESULT removedReason) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    fwprintf(
        stderr,
        L"[video-event] d3d-failure local=%04u-%02u-%02u "
        L"%02u:%02u:%02u.%03u uptime=%llu ms failure=0x%08X (%s) "
        L"device-removal=0x%08X (%s)\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond, now.wMilliseconds,
        static_cast<unsigned long long>(GetTickCount64()),
        static_cast<unsigned>(failureHr), HrText(failureHr).c_str(),
        static_cast<unsigned>(removedReason), HrText(removedReason).c_str());
}

static void LogDisplayChangeEvent(WPARAM wParam, LPARAM lParam) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    fwprintf(
        stderr,
        L"[display-event] display-change local=%04u-%02u-%02u "
        L"%02u:%02u:%02u.%03u uptime=%llu ms bpp=%llu mode=%ux%u\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond, now.wMilliseconds,
        static_cast<unsigned long long>(GetTickCount64()),
        static_cast<unsigned long long>(wParam), LOWORD(lParam),
        HIWORD(lParam));
}

static const wchar_t* DeviceChangeEventName(WPARAM event) {
    switch (event) {
    case DBT_DEVICEARRIVAL: return L"arrival";
    case DBT_DEVICEREMOVECOMPLETE: return L"remove-complete";
    case DBT_DEVICEREMOVEPENDING: return L"remove-pending";
    case DBT_DEVNODES_CHANGED: return L"devnodes-changed";
    default: return L"other";
    }
}

static void LogDeviceChangeEvent(WPARAM event) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    fwprintf(
        stderr,
        L"[display-event] device-change local=%04u-%02u-%02u "
        L"%02u:%02u:%02u.%03u uptime=%llu ms event=%llu (%s)\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
        now.wSecond, now.wMilliseconds,
        static_cast<unsigned long long>(GetTickCount64()),
        static_cast<unsigned long long>(event),
        DeviceChangeEventName(event));
}

using llcv::audio_device::ClosestSupportedSharedPeriod;

static void LogModuleMessage(const wchar_t* message);

static ExclusiveCompatibilityProbe ProbeExclusiveBufferRecommendation(
    const std::wstring& endpointId, const std::atomic<bool>* cancel) {
    return llcv::audio_device::ProbeExclusiveBufferRecommendation(
        endpointId, cancel, LogModuleMessage);
}
static std::wstring AppDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!n || n >= ARRAYSIZE(path)) return L".";

    std::wstring dir(path, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    dir.resize(slash);
    return dir;
}

// User-writable data belongs in LocalAppData so an installation under
// Program Files does not require elevation and portable copies do not mix
// machine-specific settings into the application directory.
static std::wstring UserDataDirectory() {
    static const std::wstring directory = [] {
        wchar_t buffer[32768]{};
        const DWORD length = GetEnvironmentVariableW(
            L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer));
        if (length > 0 && length < ARRAYSIZE(buffer)) {
            return std::wstring(buffer, length) +
                   L"\\LowLatencyCaptureViewer";
        }
        return AppDirectory();
    }();
    return directory;
}

static std::wstring LogDirectory() {
    return UserDataDirectory() + L"\\logs";
}

static void EnsureUserDataDirectory() {
    CreateDirectoryW(UserDataDirectory().c_str(), nullptr);
}

static void MigrateLegacySettings() {
    EnsureUserDataDirectory();
    const std::wstring destination = UserDataDirectory() + L"\\settings.ini";
    const std::wstring legacy = AppDirectory() + L"\\settings.ini";
    if (_wcsicmp(destination.c_str(), legacy.c_str()) == 0) return;
    const DWORD destinationAttributes = GetFileAttributesW(destination.c_str());
    const DWORD legacyAttributes = GetFileAttributesW(legacy.c_str());
    if (destinationAttributes == INVALID_FILE_ATTRIBUTES &&
        legacyAttributes != INVALID_FILE_ATTRIBUTES &&
        (legacyAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        // Keep the old file in place so a portable copy remains recoverable.
        CopyFileW(legacy.c_str(), destination.c_str(), FALSE);
    }
}

static void OpenSavedLog() {
    EnsureUserDataDirectory();
    g_logger.Open(g_settings.saveLog, LogDirectory());
}

static void CloseSavedLog() {
    g_logger.Close();
}

static std::vector<CaptureDeviceInfo> EnumerateCaptureDevices() {
    return llcv::capture::EnumerateVideoInputDevices();
}

static std::vector<CaptureDeviceInfo> EnumerateCaptureAudioDevices() {
    return llcv::capture::EnumerateAudioInputDevices();
}
using llcv::audio_device::IsExclusiveLowLatencyBuffer;
static const ExclusiveEndpointCacheEntry* FindExclusiveEndpointCache(
    const std::wstring& endpointId) {
    const auto it = std::find_if(
        g_settings.exclusiveEndpointCache.begin(),
        g_settings.exclusiveEndpointCache.end(),
        [&](const ExclusiveEndpointCacheEntry& entry) {
            return entry.endpointId == endpointId;
        });
    return it != g_settings.exclusiveEndpointCache.end() ? &*it : nullptr;
}

static bool HasVerifiedExclusiveEndpoint(const std::wstring& endpointId,
                                         int requestedBufferMs) {
    if (endpointId.empty()) return false;
    if (const auto* cached = FindExclusiveEndpointCache(endpointId)) {
        return cached->supported &&
               IsExclusiveLowLatencyBuffer(cached->recommendedBufferMs) &&
               requestedBufferMs >= cached->recommendedBufferMs;
    }
    // Compatibility with the first Exclusive prototype's single-endpoint
    // record. It is migrated into the full cache when settings are loaded.
    return endpointId == g_settings.exclusiveVerifiedEndpointId &&
           IsExclusiveLowLatencyBuffer(g_settings.exclusiveVerifiedBufferMs) &&
           requestedBufferMs >= g_settings.exclusiveVerifiedBufferMs;
}

static std::wstring ConfiguredAudioEndpointName(
    const std::wstring& endpointId) {
    const auto endpoints = llcv::audio_device::EnumerateRenderEndpoints();
    if (!endpointId.empty()) {
        for (const auto& endpoint : endpoints) {
            if (endpoint.id == endpointId) return endpoint.name;
        }
        return UI_TEXT(L"선택 장치 없음");
    }
    for (const auto& endpoint : endpoints) {
        if (endpoint.isDefault) return endpoint.name + UI_TEXT(L" (기본)");
    }
    return UI_TEXT(L"Windows 기본 장치");
}

static int RunExclusiveCompatibilityProbeCli(bool allEndpoints) {
    const auto endpoints = llcv::audio_device::EnumerateRenderEndpoints();
    std::vector<AudioEndpointInfo> targets;
    if (allEndpoints) {
        targets = endpoints;
    } else if (g_settings.audioOutputDeviceId.empty()) {
        const auto it = std::find_if(endpoints.begin(), endpoints.end(),
                                     [](const auto& endpoint) {
                                         return endpoint.isDefault;
                                     });
        if (it != endpoints.end()) targets.push_back(*it);
    } else {
        const auto it = std::find_if(
            endpoints.begin(), endpoints.end(), [](const auto& endpoint) {
                return endpoint.id == g_settings.audioOutputDeviceId;
            });
        if (it != endpoints.end()) targets.push_back(*it);
    }

    if (targets.empty()) {
        fwprintf(stderr, L"[audio][exclusive-probe] no active render endpoints found.\n");
        return 2;
    }

    int passed = 0;
    for (const auto& endpoint : targets) {
        fwprintf(stderr, L"[audio][exclusive-probe] testing: %s%s\n",
                 endpoint.name.c_str(), endpoint.isDefault ? L" (default)" : L"");
        const auto probe = ProbeExclusiveBufferRecommendation(
            endpoint.id, nullptr);
        fwprintf(stderr,
                 L"[audio][exclusive-probe] %s | requested=%u frames "
                 L"actual=%u frames events=%u supplied=%llu frames "
                 L"avg/max=%.2f/%.2f ms\n",
                 probe.summary.c_str(), probe.requestedFrames,
                 probe.actualBufferFrames, probe.events,
                 static_cast<unsigned long long>(probe.submittedFrames),
                 probe.averageEventMs, probe.maximumEventMs);
        if (probe.compatible) ++passed;
    }
    fwprintf(stderr, L"[audio][exclusive-probe] result: %d/%zu compatible.\n",
             passed, targets.size());
    return passed == static_cast<int>(targets.size()) ? 0 : 3;
}

static const VideoPresetInfo& CurrentVideoPreset() {
    for (const auto& info : kVideoPresets) {
        if (info.preset == g_settings.videoPreset) return info;
    }
    return kVideoPresets[0];
}

static int RequestedVideoFrameRate() {
    return g_settings.videoFrameRate > 0
               ? g_settings.videoFrameRate
               : CurrentVideoPreset().framerate;
}

static constexpr int kRelativeScaleUnit = 1'000'000;
static constexpr int kRelativeScaleSettingsVersion =
    llcv::settings::kRelativeWindowScaleVersion;

static int LegacyRelativeScaleForMonitor(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0;
    const int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
    const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
    if (monitorWidth <= 0 || monitorHeight <= 0) return 0;
    const auto& video = CurrentVideoPreset();
    const int widthScale = static_cast<int>(
        static_cast<int64_t>(video.width) * kRelativeScaleUnit /
        monitorWidth);
    const int heightScale = static_cast<int>(
        static_cast<int64_t>(video.height) * kRelativeScaleUnit /
        monitorHeight);
    return std::clamp((std::min)(widthScale, heightScale),
                      kRelativeScaleUnit / 4, kRelativeScaleUnit);
}

static int RelativeScaleForMonitor(HMONITOR monitor) {
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0;
    const int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
    const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
    if (monitorWidth <= 0 || monitorHeight <= 0) return 0;
    const auto& video = CurrentVideoPreset();
    const int widthScale = static_cast<int>(
        static_cast<int64_t>(video.width) * kRelativeScaleUnit /
        monitorWidth);
    const int heightScale = static_cast<int>(
        static_cast<int64_t>(video.height) * kRelativeScaleUnit /
        monitorHeight);
    // DesiredClientPixelsForMonitor treats the scale as an aspect-preserving
    // bounding box, so reproducing a given client size requires the larger
    // normalized dimension. RememberRelativeScaleFromWindow uses the same
    // definition. Using the smaller dimension double-shrank 1920x1080 on a
    // 1920x1200 display to 1728x972 (90%).
    return std::clamp((std::max)(widthScale, heightScale),
                      kRelativeScaleUnit / 4, kRelativeScaleUnit);
}

static std::wstring SettingsPath() {
    return UserDataDirectory() + L"\\settings.ini";
}

static std::wstring AsioDriverNameWide(const std::string& name) {
    if (name.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_ACP, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0);
    if (required <= 0) return std::wstring(name.begin(), name.end());
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_ACP, 0, name.c_str(),
                        static_cast<int>(name.size()), result.data(), required);
    return result;
}

static HMONITOR SavedViewerMonitor();

static void LoadSettings() {
    MigrateLegacySettings();
    llcv::settings::LoadResult loaded =
        llcv::settings::LoadFromIni(SettingsPath());
    g_settings = std::move(loaded.settings);

    // ASIO driver presence is machine state, not INI parsing. Keep this
    // validation at the application boundary and fall back safely when a
    // saved driver has been removed.
    if (g_settings.audioMode == AudioMode::Asio) {
        const auto drivers = llcv::asio::EnumerateDrivers();
        const bool found = std::any_of(
            drivers.begin(), drivers.end(), [&](const auto& driver) {
                return AsioDriverNameWide(driver.name) ==
                       g_settings.asioDriverName;
            });
        if (!found) {
            g_settings.audioMode = AudioMode::WasapiShared;
            g_settings.asioDriverName.clear();
        }
    }

    g_volumePercent.store(
        g_settings.volumePercent, std::memory_order_release);
    g_leftVolumePercent.store(
        g_settings.leftVolumePercent, std::memory_order_release);
    g_rightVolumePercent.store(
        g_settings.rightVolumePercent, std::memory_order_release);

    // Older builds used the smaller normalized dimension on mixed-aspect
    // displays. Correct only values that exactly match that legacy formula;
    // do not recompute every saved scale from the last monitor.
    if (loaded.relativeWindowScaleVersion <
            kRelativeScaleSettingsVersion &&
        g_settings.relativeWindowSize && g_settings.pixelPerfect &&
        g_settings.hasWindowPosition) {
        const HMONITOR savedViewerMonitor = SavedViewerMonitor();
        const int legacyScale =
            LegacyRelativeScaleForMonitor(savedViewerMonitor);
        const int correctedScale =
            RelativeScaleForMonitor(savedViewerMonitor);
        if (correctedScale > 0 &&
            (g_settings.relativeWindowScalePpm <= 0 ||
             (legacyScale != correctedScale &&
              g_settings.relativeWindowScalePpm == legacyScale))) {
            g_settings.relativeWindowScalePpm = correctedScale;
        }
    }
}

static void SaveSettings() {
    EnsureUserDataDirectory();
    llcv::settings::SaveToIni(SettingsPath(), g_settings);
}

// -----------------------------------------------------------------------------
// Bounded PCM storage and the optional drift resampler live in the audio
// module. Only rare tracked overruns call back into application diagnostics.
static bool OnPcmRingOverrun(void*, size_t droppedFrames) {
    if (!AudioTrackingActive()) return false;
    const uint64_t nowMs = GetTickCount64();
    g_audioOverrunFrames.fetch_add(droppedFrames, std::memory_order_relaxed);
    g_audioLastOverrunMs.store(nowMs, std::memory_order_release);
    RecordAudioErrorEvent(
        nowMs,
        static_cast<uint32_t>((std::min)(
            droppedFrames, static_cast<size_t>(UINT32_MAX))),
        AudioErrorKind::Overrun, AudioErrorCause::Overrun);
    return true;
}

static PcmRing g_ring{kRingFrames, &g_audioRingFrames,
                      OnPcmRingOverrun, nullptr};
static std::atomic<uint64_t> g_underruns{0};

// -----------------------------------------------------------------------------
// Sample Grabber callback
// -----------------------------------------------------------------------------

static bool CaptureAudioTrackingActive(void*) {
    return AudioTrackingActive();
}

static ISampleGrabberCB* CreateAudioSampleCallback(
    const llcv::capture_audio::Format& format) {
    llcv::capture::AudioSampleTelemetry telemetry{
        &g_audioMonitorStartMs,
        &g_audioCapturePacketFrames,
        &g_audioCaptureIntervalUs,
        &g_audioCaptureIntervalTotalUs,
        &g_audioLastCaptureCallbackMs,
        &g_audioCaptureCallbacks,
        &g_audioCaptureFrames,
    };
    return new llcv::capture::AudioSampleGrabberCallback(
        format, g_ring, telemetry, CaptureAudioTrackingActive, nullptr);
}

// -----------------------------------------------------------------------------
// DirectShow helpers
// -----------------------------------------------------------------------------

static HRESULT FindCaptureFilter(
    const std::wstring& selectedId, IBaseFilter** output,
    std::wstring* selectedName = nullptr) {
    return llcv::capture::FindVideoCaptureFilter(
        selectedId, kCaptureName, output, selectedName,
        LogModuleMessage);
}

static HRESULT FindCaptureAudioFilter(
    const std::wstring& selectedId, const std::wstring& videoName,
    IBaseFilter** output, std::wstring* selectedName = nullptr) {
    return llcv::capture::FindCaptureAudioFilter(
        selectedId, videoName, output, selectedName,
        LogModuleMessage);
}

static HRESULT FindOutputPinByName(
    IBaseFilter* filter, const wchar_t* name, IPin** output) {
    return llcv::capture::FindOutputPinByName(filter, name, output);
}

static HRESULT GetFirstPin(
    IBaseFilter* filter, PIN_DIRECTION wanted, IPin** output) {
    return llcv::capture::GetFirstPin(filter, wanted, output);
}

static HRESULT FindOutputPinByMajorType(
    IBaseFilter* filter, const GUID& majorType, IPin** output) {
    return llcv::capture::FindOutputPinByMajorType(
        filter, majorType, output);
}

static void LogFilterPins(IBaseFilter* filter, const wchar_t* label) {
    llcv::capture::LogFilterPins(
        filter, label, LogModuleMessage);
}

static void SuggestCaptureBuffer(IPin* audioPin, WORD blockAlign) {
    LONG suggestedBytes = 0;
    const HRESULT hr = llcv::capture_audio::SuggestCaptureBuffer(
        audioPin, blockAlign, kSampleRate, kRecommendedCaptureBufferMs,
        &suggestedBytes);
    if (hr == E_NOINTERFACE) {
        fwprintf(stderr, L"[audio] IAMBufferNegotiation unavailable; driver controls capture buffer.\n");
        return;
    }
    if (FAILED(hr)) LogHr(L"IAMBufferNegotiation::SuggestAllocatorProperties", hr);
    else fwprintf(stderr, L"[audio] requested DirectShow capture buffer: %d ms (%ld bytes)\n",
                  kRecommendedCaptureBufferMs, suggestedBytes);
}

static void ReportConnectedAudioAllocator(IPin* inputPin, WORD blockAlign) {
    const auto info = llcv::capture_audio::QueryConnectedAllocator(
        inputPin, blockAlign);
    if (SUCCEEDED(info.result)) {
        g_audioCaptureAllocatorFrames.store(info.framesPerBuffer,
                                            std::memory_order_release);
        g_audioCaptureAllocatorBuffers.store(info.bufferCount,
                                             std::memory_order_release);
        fwprintf(stderr,
                 L"[audio] actual DirectShow allocator: %ld buffers x "
                 L"%ld bytes (%ld frames / %.2f ms each)\n",
                 info.bufferCount, info.bufferBytes, info.framesPerBuffer,
                 1000.0 * info.framesPerBuffer / kSampleRate);
    } else {
        LogHr(L"DirectShow audio allocator query", info.result);
    }
}

// -----------------------------------------------------------------------------
// WASAPI render thread (user-selectable Shared or Exclusive mode)
// -----------------------------------------------------------------------------

static double TargetAudioVolumeGain() {
    if (g_backgroundAudioMuted.load(std::memory_order_acquire)) return 0.0;
    return g_volumePercent.load(std::memory_order_acquire) / 100.0;
}

static double TargetAudioChannelGain(int channel) {
    const int percent = channel == 0
        ? g_leftVolumePercent.load(std::memory_order_acquire)
        : g_rightVolumePercent.load(std::memory_order_acquire);
    return percent / 100.0;
}

static void PublishAudioPeak(std::atomic<int>& destination, int observed) {
    const int previous = destination.load(std::memory_order_relaxed);
    destination.store(llcv::audio::DecayAndHoldPeak(previous, observed),
                      std::memory_order_release);
}

struct AsioRenderState {
    SincDriftResampler driftResampler{g_ring, &g_audioResamplerFrames};
    double filteredQueuedFrames = -1.0;
    double correctionPpm = 0.0;
    uint64_t autoCandidateSinceMs = 0;
    bool audioStarted = false;
    bool autoCorrectionActive = false;
};

// ASIO supplies its own driver-sized output period. The driver still owns the
// output clock, while this callback can apply the same optional app-side
// resampler as WASAPI to keep the capture PCM queue near its target.
static size_t FillAsioPcm(void* user, int16_t* out, size_t frames) {
    if (!out || frames == 0) return 0;
    std::memset(out, 0, frames * kChannels * sizeof(int16_t));
    auto* state = static_cast<AsioRenderState*>(user);
    if (!state) return 0;
    const UINT32 targetFrames = g_audioQueueTargetFrames.load(
        std::memory_order_acquire);
    const size_t availableBeforeRender =
        g_ring.AvailableFrames() + state->driftResampler.BufferedFrames();
    if (state->audioStarted && AudioTrackingActive()) {
        UINT32 observed = static_cast<UINT32>((std::min)(
            availableBeforeRender, static_cast<size_t>(UINT32_MAX)));
        UINT32 previousMinimum = g_audioMinimumPreRenderFrames.load(
            std::memory_order_relaxed);
        while (observed < previousMinimum &&
               !g_audioMinimumPreRenderFrames.compare_exchange_weak(
                   previousMinimum, observed, std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
    }

    size_t got = 0;
    const bool correctionConfigured =
        g_settings.driftCorrection != DriftCorrectionMode::Off;
    bool correctionActive = false;
    if (correctionConfigured) {
        const double target = static_cast<double>(targetFrames);
        const double queued = static_cast<double>(
            g_audioRingFrames.load(std::memory_order_acquire) +
            static_cast<UINT32>((std::min)(
                state->driftResampler.BufferedFrames(),
                static_cast<size_t>(UINT32_MAX))));
        if (!state->audioStarted && queued >= target) {
            state->audioStarted = true;
            g_asioAudioStarted.store(true, std::memory_order_release);
        }
        if (state->filteredQueuedFrames < 0.0) {
            state->filteredQueuedFrames = queued;
        } else {
            state->filteredQueuedFrames +=
                (queued - state->filteredQueuedFrames) * 0.02;
        }

        if (g_settings.driftCorrection == DriftCorrectionMode::Auto &&
            !state->autoCorrectionActive && state->audioStarted &&
            AudioTrackingActive()) {
            const double deviation = std::abs(
                state->filteredQueuedFrames - target);
            const uint64_t nowMs = GetTickCount64();
            if (deviation >= kAutoCorrectionEngageDeviationFrames) {
                if (!state->autoCandidateSinceMs) {
                    state->autoCandidateSinceMs = nowMs;
                } else if (nowMs >= state->autoCandidateSinceMs &&
                           nowMs - state->autoCandidateSinceMs >=
                               kAutoCorrectionEngageHoldMs &&
                           queued >= static_cast<double>(frames)) {
                    state->autoCorrectionActive = true;
                    state->driftResampler.Reset();
                    state->correctionPpm = 0.0;
                    fwprintf(stderr,
                             L"[audio] ASIO auto clock-drift correction "
                             L"engaged after sustained queue drift.\n");
                }
            } else {
                state->autoCandidateSinceMs = 0;
            }
        }

        correctionActive =
            g_settings.driftCorrection == DriftCorrectionMode::Resample ||
            state->autoCorrectionActive;
        g_audioResamplerActive.store(correctionActive,
                                     std::memory_order_release);
        if (correctionActive) {
            const double requestedPpm = std::clamp(
                (state->filteredQueuedFrames - target) * 2.0,
                -1000.0, 1000.0);
            state->correctionPpm +=
                (requestedPpm - state->correctionPpm) * 0.02;
            const double ratio = 1.0 + state->correctionPpm / 1'000'000.0;
            if (state->audioStarted) {
                got = state->driftResampler.Render(out, frames, ratio);
            }
            g_audioResamplePpm.store(
                static_cast<int>(std::lround(state->correctionPpm)),
                std::memory_order_release);
            g_audioResampledOutputFrames.fetch_add(
                got, std::memory_order_relaxed);
        } else {
            if (state->audioStarted) got = g_ring.Pop(out, frames);
            state->correctionPpm = 0.0;
            g_audioResamplePpm.store(0, std::memory_order_release);
            g_audioResamplerFrames.store(0, std::memory_order_release);
        }
    } else {
        if (!state->audioStarted &&
            g_ring.AvailableFrames() >= targetFrames) {
            state->audioStarted = true;
            g_asioAudioStarted.store(true, std::memory_order_release);
        }
        if (state->audioStarted) got = g_ring.Pop(out, frames);
        state->correctionPpm = 0.0;
        g_audioResamplePpm.store(0, std::memory_order_release);
        g_audioResamplerFrames.store(0, std::memory_order_release);
        g_audioResamplerActive.store(false, std::memory_order_release);
    }

    static thread_local llcv::audio::StereoGain currentMix{};
    const bool measurePeaks =
        g_audioOsdVisible.load(std::memory_order_acquire);
    const llcv::audio::MixMetrics mix = llcv::audio::ProcessStereoPcm(
        out, got, currentMix,
        {TargetAudioVolumeGain() * TargetAudioChannelGain(0),
         TargetAudioVolumeGain() * TargetAudioChannelGain(1)},
        measurePeaks);
    if (measurePeaks) {
        PublishAudioPeak(g_audioPeakLeft, mix.peakLeft);
        PublishAudioPeak(g_audioPeakRight, mix.peakRight);
    }
    if (mix.clipped) {
        g_audioClipCount.fetch_add(1, std::memory_order_relaxed);
        g_audioClipUntilMs.store(GetTickCount64() + 1500,
                                 std::memory_order_release);
    }
    if (got < frames && state->audioStarted &&
        g_running.load(std::memory_order_acquire) && AudioTrackingActive()) {
        const UINT32 missing = static_cast<UINT32>(frames - got);
        const uint64_t nowMs = GetTickCount64();
        g_underruns.fetch_add(1, std::memory_order_relaxed);
        g_audioUnderrunFrames.fetch_add(missing, std::memory_order_relaxed);
        g_audioLastUnderrunMs.store(nowMs, std::memory_order_release);
        AudioErrorCause cause = AudioErrorCause::PcmDepletion;
        if (correctionActive && availableBeforeRender >= frames) {
            cause = AudioErrorCause::Resampler;
            g_audioResamplerUnderruns.fetch_add(1,
                                                 std::memory_order_relaxed);
        } else {
            const uint64_t callbackMs =
                g_audioLastCaptureCallbackMs.load(std::memory_order_acquire);
            const UINT32 packetFrames =
                g_audioCapturePacketFrames.load(std::memory_order_acquire);
            const uint64_t lateThresholdMs = packetFrames
                ? 5 + (1000ull * packetFrames / kSampleRate) : 15;
            if (callbackMs && GetTickCount64() > callbackMs + lateThresholdMs) {
                cause = AudioErrorCause::InputLate;
                g_audioLatePacketUnderruns.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        RecordAudioErrorEvent(nowMs, missing, AudioErrorKind::Underrun,
                              cause);
    }
    const UINT32 queued = static_cast<UINT32>((std::min)(
        g_audioRingFrames.load(std::memory_order_acquire) +
                state->driftResampler.BufferedFrames(),
        static_cast<size_t>(UINT32_MAX)));
    UINT32 previous = g_audioMinimumPreRenderFrames.load(
        std::memory_order_acquire);
    while (queued < previous &&
           !g_audioMinimumPreRenderFrames.compare_exchange_weak(
               previous, queued, std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
    return got;
}

struct WasapiRenderState {
    SincDriftResampler driftResampler{g_ring, &g_audioResamplerFrames};
    double filteredQueuedFrames = -1.0;
    double correctionPpm = 0.0;
    uint64_t autoCandidateSinceMs = 0;
    bool audioStarted = false;
    bool autoCorrectionActive = false;
    UINT32 queueTargetFrames = 0;
    llcv::audio::StereoGain currentMix{};
};

static llcv::wasapi::FillResult FillWasapiPcm(
    void* user, int16_t* output, size_t frames) {
    llcv::wasapi::FillResult result{};
    auto* state = static_cast<WasapiRenderState*>(user);
    if (!state || !output || frames == 0) return result;

    result.availableBeforeRender =
        g_ring.AvailableFrames() + state->driftResampler.BufferedFrames();
    if (state->audioStarted && AudioTrackingActive()) {
        UINT32 observed = static_cast<UINT32>((std::min)(
            result.availableBeforeRender,
            static_cast<size_t>(UINT32_MAX)));
        UINT32 previousMinimum = g_audioMinimumPreRenderFrames.load(
            std::memory_order_relaxed);
        while (observed < previousMinimum &&
               !g_audioMinimumPreRenderFrames.compare_exchange_weak(
                   previousMinimum, observed, std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
    }

    size_t got = 0;
    const bool correctionConfigured =
        g_settings.driftCorrection != DriftCorrectionMode::Off;
    if (correctionConfigured) {
        const double targetFrames =
            static_cast<double>(state->queueTargetFrames);
        const double queuedFrames = static_cast<double>(
            g_audioRingFrames.load(std::memory_order_acquire) +
            static_cast<UINT32>((std::min)(
                state->driftResampler.BufferedFrames(),
                static_cast<size_t>(UINT32_MAX))));
        if (!state->audioStarted && queuedFrames >= targetFrames) {
            state->audioStarted = true;
        }
        if (state->filteredQueuedFrames < 0.0) {
            state->filteredQueuedFrames = queuedFrames;
        } else {
            state->filteredQueuedFrames +=
                (queuedFrames - state->filteredQueuedFrames) * 0.02;
        }

        if (g_settings.driftCorrection == DriftCorrectionMode::Auto &&
            !state->autoCorrectionActive && state->audioStarted &&
            AudioTrackingActive()) {
            const double deviation =
                std::abs(state->filteredQueuedFrames - targetFrames);
            const uint64_t nowMs = GetTickCount64();
            if (deviation >= kAutoCorrectionEngageDeviationFrames) {
                if (!state->autoCandidateSinceMs) {
                    state->autoCandidateSinceMs = nowMs;
                } else if (
                    nowMs >= state->autoCandidateSinceMs &&
                    nowMs - state->autoCandidateSinceMs >=
                        kAutoCorrectionEngageHoldMs &&
                    queuedFrames >= static_cast<double>(frames)) {
                    state->autoCorrectionActive = true;
                    state->driftResampler.Reset();
                    state->correctionPpm = 0.0;
                    fwprintf(
                        stderr,
                        L"[audio] auto clock-drift correction engaged "
                        L"after sustained queue drift.\n");
                }
            } else {
                state->autoCandidateSinceMs = 0;
            }
        }

        const bool correctionActive =
            g_settings.driftCorrection == DriftCorrectionMode::Resample ||
            state->autoCorrectionActive;
        g_audioResamplerActive.store(
            correctionActive, std::memory_order_release);
        if (correctionActive) {
            const double requestedPpm = std::clamp(
                (state->filteredQueuedFrames - targetFrames) * 2.0,
                -1000.0, 1000.0);
            state->correctionPpm +=
                (requestedPpm - state->correctionPpm) * 0.02;
            const double ratio =
                1.0 + state->correctionPpm / 1'000'000.0;
            if (state->audioStarted) {
                got = state->driftResampler.Render(output, frames, ratio);
            }
            g_audioResamplePpm.store(
                static_cast<int>(std::lround(state->correctionPpm)),
                std::memory_order_release);
            g_audioResampledOutputFrames.fetch_add(
                got, std::memory_order_relaxed);
        } else {
            if (state->audioStarted) got = g_ring.Pop(output, frames);
            g_audioResamplePpm.store(0, std::memory_order_release);
            g_audioResamplerFrames.store(0, std::memory_order_release);
        }
    } else {
        if (!state->audioStarted &&
            g_ring.AvailableFrames() >= state->queueTargetFrames) {
            state->audioStarted = true;
        }
        if (state->audioStarted) got = g_ring.Pop(output, frames);
        g_audioResamplePpm.store(0, std::memory_order_release);
        g_audioResamplerFrames.store(0, std::memory_order_release);
        g_audioResamplerActive.store(false, std::memory_order_release);
    }

    const double targetVolumeGain = TargetAudioVolumeGain();
    const bool measurePeaks =
        g_audioOsdVisible.load(std::memory_order_acquire);
    const llcv::audio::MixMetrics mix = llcv::audio::ProcessStereoPcm(
        output, got, state->currentMix,
        {targetVolumeGain * TargetAudioChannelGain(0),
         targetVolumeGain * TargetAudioChannelGain(1)},
        measurePeaks);
    if (measurePeaks) {
        PublishAudioPeak(g_audioPeakLeft, mix.peakLeft);
        PublishAudioPeak(g_audioPeakRight, mix.peakRight);
    }
    if (mix.clipped) {
        g_audioClipCount.fetch_add(1, std::memory_order_relaxed);
        g_audioClipUntilMs.store(
            GetTickCount64() + 1500, std::memory_order_release);
    }

    result.writtenFrames = got;
    result.audioStarted = state->audioStarted;
    result.trackingActive = AudioTrackingActive();
    if (got < frames && state->audioStarted && result.trackingActive) {
        const UINT32 missingFrames =
            static_cast<UINT32>(frames - got);
        const uint64_t nowMs = GetTickCount64();
        AudioErrorCause cause = AudioErrorCause::PcmDepletion;
        g_underruns.fetch_add(1, std::memory_order_relaxed);
        g_audioUnderrunFrames.fetch_add(
            missingFrames, std::memory_order_relaxed);
        g_audioLastUnderrunMs.store(nowMs, std::memory_order_release);
        if (AudioResamplerActive() &&
            result.availableBeforeRender >= frames) {
            cause = AudioErrorCause::Resampler;
            g_audioResamplerUnderruns.fetch_add(
                1, std::memory_order_relaxed);
        } else {
            const uint64_t callbackMs =
                g_audioLastCaptureCallbackMs.load(std::memory_order_acquire);
            const UINT32 packetFrames =
                g_audioCapturePacketFrames.load(std::memory_order_acquire);
            const uint64_t lateThresholdMs = packetFrames
                ? 5 + (1000ull * packetFrames / kSampleRate) : 15;
            if (callbackMs && nowMs > callbackMs + lateThresholdMs) {
                cause = AudioErrorCause::InputLate;
                g_audioLatePacketUnderruns.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        RecordAudioErrorEvent(
            nowMs, missingFrames, AudioErrorKind::Underrun, cause);
    }

    result.queuedFrames = static_cast<UINT32>((std::min)(
        g_audioRingFrames.load(std::memory_order_acquire) +
                state->driftResampler.BufferedFrames(),
        static_cast<size_t>(UINT32_MAX)));
    result.queueTargetFrames = state->queueTargetFrames;
    result.resamplerActive = AudioResamplerActive();
    result.resamplePpm =
        g_audioResamplePpm.load(std::memory_order_acquire);
    return result;
}

static void OnWasapiEndpointChanged(
    void*, const std::wstring& name, bool followsDefault) {
    SetActiveAudioOutputName(
        name + (followsDefault ? L" (기본 추적)" : L""));
}

static void OnWasapiBufferChanged(void*, UINT32 frames) {
    g_audioActualBufferFrames.store(frames, std::memory_order_release);
}

static void OnWasapiPaddingChanged(void*, UINT32 frames) {
    g_audioWasapiPaddingFrames.store(frames, std::memory_order_release);
}

static void BeforeWasapiEndpointRestart(void*) {
    g_ring.Clear();
    g_audioResamplerFrames.store(0, std::memory_order_release);
    g_audioMinimumPreRenderFrames.store(
        UINT32_MAX, std::memory_order_release);
}

static void LogWasapiHresult(
    void*, const wchar_t* operation, HRESULT result) {
    LogHr(operation, result);
}

static bool AudioRenderThreadWasapi(
    AudioMode mode, bool reinitializingEndpoint) {
    WasapiRenderState state{};
    state.autoCorrectionActive =
        g_settings.driftCorrection == DriftCorrectionMode::Resample;
    state.queueTargetFrames = static_cast<UINT32>(
        g_settings.pcmQueueTargetMs * kSampleRate / 1000);
    const double initialVolumeGain = TargetAudioVolumeGain();
    state.currentMix = {
        initialVolumeGain * TargetAudioChannelGain(0),
        initialVolumeGain * TargetAudioChannelGain(1)};
    g_audioQueueTargetFrames.store(
        state.queueTargetFrames, std::memory_order_release);
    g_audioResamplerActive.store(
        state.autoCorrectionActive, std::memory_order_release);
    g_audioResamplePpm.store(0, std::memory_order_release);

    const wchar_t* correctionDescription =
        g_settings.driftCorrection == DriftCorrectionMode::Resample
        ? L"on (16-tap windowed-sinc, +/-1000 ppm)"
        : g_settings.driftCorrection == DriftCorrectionMode::Auto
            ? L"auto (observe first; latch on when sustained drift is detected)"
            : L"off (unaltered PCM samples)";
    llcv::wasapi::Configuration configuration{};
    configuration.mode = mode == AudioMode::WasapiExclusive
        ? llcv::wasapi::Mode::Exclusive : llcv::wasapi::Mode::Shared;
    configuration.endpointId = g_settings.audioOutputDeviceId;
    configuration.bufferMilliseconds = g_settings.wasapiBufferMs;
    configuration.sharedPeriodFrames =
        g_settings.wasapiSharedPeriodFrames;
    configuration.reinitializingEndpoint = reinitializingEndpoint;
    configuration.correctionDescription = correctionDescription;

    llcv::wasapi::Host host{};
    host.context = &state;
    host.running = &g_running;
    host.fill = &FillWasapiPcm;
    host.endpointChanged = &OnWasapiEndpointChanged;
    host.bufferChanged = &OnWasapiBufferChanged;
    host.paddingChanged = &OnWasapiPaddingChanged;
    host.beforeStart = &BeforeWasapiEndpointRestart;
    host.logHresult = &LogWasapiHresult;
    const bool restart = llcv::wasapi::Run(configuration, host);

    g_audioResamplerActive.store(false, std::memory_order_release);
    g_audioResamplePpm.store(0, std::memory_order_release);
    g_audioResamplerFrames.store(0, std::memory_order_release);
    return restart;
}


static bool AudioRenderThreadAsio() {
    std::string driverName;
    if (!g_settings.asioDriverName.empty()) {
        const int required = WideCharToMultiByte(
            CP_ACP, 0, g_settings.asioDriverName.c_str(), -1, nullptr, 0,
            nullptr, nullptr);
        if (required > 1) {
            driverName.resize(static_cast<size_t>(required));
            WideCharToMultiByte(CP_ACP, 0, g_settings.asioDriverName.c_str(), -1,
                                driverName.data(), required, nullptr, nullptr);
            driverName.pop_back();
        }
    }
    g_asioAudioStarted.store(false, std::memory_order_release);
    g_audioWasapiPaddingFrames.store(0, std::memory_order_release);
    const bool resamplerConfigured =
        g_settings.driftCorrection == DriftCorrectionMode::Resample;
    g_audioResamplerActive.store(resamplerConfigured,
                                 std::memory_order_release);
    g_audioResamplePpm.store(0, std::memory_order_release);
    g_audioResamplerFrames.store(0, std::memory_order_release);
    g_audioQueueTargetFrames.store(
        static_cast<UINT32>(g_settings.pcmQueueTargetMs * kSampleRate / 1000),
        std::memory_order_release);
    // ASIO's preferred buffer is only known after the driver is opened. A
    // conservative reservation keeps the callback allocation-free for normal
    // driver periods; the resampler retains this capacity across resets.
    AsioRenderState renderState;
    renderState.autoCorrectionActive = resamplerConfigured;
    renderState.driftResampler.Prepare(32768);
    llcv::asio::Output output(driverName, g_videoHost, &FillAsioPcm,
                              &renderState);
    if (!output.Start()) {
        fwprintf(stderr, L"[audio] ASIO start failed: %S\n",
                 output.Error().c_str());
        return false;
    }
    g_audioActualBufferFrames.store(static_cast<UINT32>(output.BufferFrames()),
                                    std::memory_order_release);
    SetActiveAudioOutputName(L"ASIO: " + g_settings.asioDriverName);
    fwprintf(stderr,
             L"[audio] ASIO render running: %s, buffer %ld frames (%.2f ms)\n",
             g_settings.asioDriverName.c_str(), output.BufferFrames(),
             1000.0 * output.BufferFrames() / kSampleRate);
    fwprintf(stderr, L"[audio] ASIO clock-drift correction: %s\n",
             g_settings.driftCorrection == DriftCorrectionMode::Resample
                 ? L"on (16-tap windowed-sinc, +/-1000 ppm)"
                 : g_settings.driftCorrection == DriftCorrectionMode::Auto
                       ? L"auto (observe first; latch on when sustained drift is detected)"
                       : L"off (unaltered PCM samples)");
    while (g_running.load(std::memory_order_acquire)) {
        Sleep(50);
    }
    output.Stop();
    g_asioAudioStarted.store(false, std::memory_order_release);
    g_audioActualBufferFrames.store(0, std::memory_order_release);
    g_audioResamplerActive.store(false, std::memory_order_release);
    g_audioResamplePpm.store(0, std::memory_order_release);
    return true;
}

static void AudioRenderThread() {
    if (g_settings.audioMode == AudioMode::Asio) {
        if (AudioRenderThreadAsio()) return;
        // A broken/unavailable ASIO driver must not leave the viewer silent.
        // Fall back to the unchanged WASAPI Shared path for this session.
        fwprintf(stderr,
                 L"[audio] ASIO unavailable; falling back to WASAPI Shared.\n");
        // Keep diagnostics and the next settings save truthful. The selected
        // driver may have disappeared or rejected 48 kHz, so do not continue
        // reporting ASIO while the actual renderer is Shared.
        g_settings.audioMode = AudioMode::WasapiShared;
        g_settings.asioDriverName.clear();
        SetActiveAudioOutputName(ConfiguredAudioEndpointName(
            g_settings.audioOutputDeviceId));
        AudioRenderThreadWasapi(AudioMode::WasapiShared, false);
        return;
    }
    bool reinitializingEndpoint = false;
    while (g_running.load(std::memory_order_acquire)) {
        const bool restart = AudioRenderThreadWasapi(
            g_settings.audioMode, reinitializingEndpoint);
        if (!restart || !g_running.load(std::memory_order_acquire)) break;
        reinitializingEndpoint = true;
    }
}

// -----------------------------------------------------------------------------
// Direct video path: DirectShow raw video (NV12/YUY2/P010) -> latest frame
// -> D3D11 video processor -> DXGI flip-discard swapchain. No decoder or
// external player is involved. P010 uses the separate HDR10 prototype output.
// -----------------------------------------------------------------------------

// Experimental compressed compatibility path. DirectShow still owns device
// capture and supplies the newest compressed access unit; a synchronous Media
// Foundation decoder expands it to NV12 for the existing D3D11 renderer.
// Keeping only the newest sample before decode prevents application-side
// queues from accumulating when a decoder cannot keep up.
static void LogModuleMessage(const wchar_t* message) {
    if (message) fwprintf(stderr, L"%s", message);
}

static bool ExtractVideoColorMetadata(
    const AM_MEDIA_TYPE* mediaType, DirectShowColorMetadata& metadata) {
    return llcv::video::ExtractDirectShowColorMetadata(mediaType, metadata);
}

static void MergeVideoColorMetadata(
    DirectShowColorMetadata& destination,
    const DirectShowColorMetadata& overrideValues) {
    llcv::video::MergeDirectShowColorMetadata(destination, overrideValues);
}

static void LogDirectShowColorMetadata(
    const wchar_t* source, const DirectShowColorMetadata& metadata) {
    llcv::video::LogDirectShowColorMetadata(
        source, metadata, LogModuleMessage);
}

static bool FindMatchingVideoColorMetadata(
    IPin* videoPin, VideoPixelFormat wantedFormat, int wantedWidth,
    int wantedHeight, int wantedFps, DirectShowColorMetadata& metadata) {
    return llcv::video::FindMatchingDirectShowColorMetadata(
        videoPin, wantedFormat, wantedWidth, wantedHeight, wantedFps,
        metadata);
}

static HRESULT ConfigureVideoPin(
    IPin* videoPin, int wantedWidth, int wantedHeight, int wantedFps,
    VideoPixelFormat wantedFormat, DWORD& imageBytes, UINT32& stride,
    int& configuredFps, VideoPixelFormat& configuredFormat) {
    return llcv::video::ConfigureVideoPin(
        videoPin, wantedWidth, wantedHeight, wantedFps, wantedFormat,
        imageBytes, stride, configuredFps, configuredFormat,
        LogModuleMessage);
}

static HRESULT GetActiveVideoPinFormat(
    IPin* videoPin, AM_MEDIA_TYPE** mediaType) {
    return llcv::video::GetActiveVideoPinFormat(videoPin, mediaType);
}

static std::vector<PixelFormatSupport> ProbePixelFormats(
    const std::wstring& captureDeviceId, int width, int height) {
    std::vector<PixelFormatSupport> result;
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initHr);
    if (initHr == RPC_E_CHANGED_MODE) initHr = S_OK;
    if (FAILED(initHr)) return result;

    IBaseFilter* capture = nullptr;
    IPin* videoPin = nullptr;
    HRESULT hr = FindCaptureFilter(captureDeviceId, &capture);
    if (SUCCEEDED(hr)) {
        hr = FindOutputPinByMajorType(capture, MEDIATYPE_Video, &videoPin);
    }
    if (SUCCEEDED(hr)) {
        result = llcv::video::ProbePixelFormats(videoPin, width, height);
    }
    SafeRelease(videoPin);
    SafeRelease(capture);
    if (uninitialize) CoUninitialize();
    return result;
}
// This runs only while the settings dialog is open. It enumerates the chosen
// DirectShow filter's advertised output pins without building or running a
// graph, so it cannot add capture-time latency or steady-state overhead.
static InternalCaptureAudioProbe ProbeInternalCaptureAudio(
    const std::wstring& captureDeviceId) {
    InternalCaptureAudioProbe probe{};
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initHr);
    if (initHr == RPC_E_CHANGED_MODE) initHr = S_OK;
    if (FAILED(initHr)) {
        probe.state = InternalCaptureAudioState::Unknown;
        probe.result = initHr;
        return probe;
    }

    IBaseFilter* capture = nullptr;
    IPin* audioPin = nullptr;
    HRESULT hr = FindCaptureFilter(captureDeviceId, &capture);
    if (SUCCEEDED(hr)) {
        hr = FindOutputPinByName(capture, kAudioPinName, &audioPin);
    }
    if (FAILED(hr) && capture) {
        hr = FindOutputPinByMajorType(capture, MEDIATYPE_Audio, &audioPin);
    }
    if (SUCCEEDED(hr) && audioPin) {
        probe.state = InternalCaptureAudioState::Available;
    } else if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        probe.state = InternalCaptureAudioState::SeparateDeviceNeeded;
    } else {
        probe.state = InternalCaptureAudioState::Unknown;
    }
    probe.result = hr;
    SafeRelease(audioPin);
    SafeRelease(capture);
    if (uninitialize) CoUninitialize();
    return probe;
}

static void UpdateConfiguredVideoTitle(HWND videoHost, int configuredFps) {
    HWND root = GetAncestor(videoHost, GA_ROOT);
    if (!root) return;
    const auto& video = CurrentVideoPreset();
    const wchar_t* audioLabel =
        g_settings.audioMode == AudioMode::WasapiExclusive
            ? L"WASAPI Exclusive"
            : g_settings.audioMode == AudioMode::Asio ? L"ASIO"
                                                       : L"WASAPI Shared";
    const wchar_t* presentationLabel =
        g_settings.presentationMode == PresentationMode::VSync
            ? L"VSync" : L"Immediate";
    const auto configuredFormat = static_cast<VideoPixelFormat>(
        g_activePixelFormat.load(std::memory_order_acquire));
    wchar_t title[512]{};
    const int requestedFps = RequestedVideoFrameRate();
    if (configuredFps == requestedFps) {
        swprintf_s(
            title,
            L"Low Latency Capture Viewer - %s - %dx%d @ %dfps %s - %s - %s",
            g_activeCaptureDeviceName.c_str(), video.width, video.height,
            configuredFps, PixelFormatName(configuredFormat), audioLabel,
            presentationLabel);
    } else {
        swprintf_s(
            title,
            L"Low Latency Capture Viewer - %s - %dx%d @ %dfps %s "
            L"(auto; requested %d) - %s - %s",
            g_activeCaptureDeviceName.c_str(), video.width, video.height,
            configuredFps, PixelFormatName(configuredFormat),
            requestedFps, audioLabel, presentationLabel);
    }
    SetWindowTextW(root, title);
}

static std::wstring BuildRuntimeOsdText(int outputWidth, int outputHeight);

struct DirectD3D11Renderer {
    static constexpr UINT kUploadSurfaceCount = 3;
    static constexpr UINT kOsdOverlayWidth = 700;
    static constexpr UINT kOsdOverlayHeight = 440;
    static constexpr float kOsdTextWidth = 668.0f;
    static constexpr float kOsdTextHeight = 414.0f;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11DeviceContext1* context1 = nullptr;
    ID3D11VideoDevice* videoDevice = nullptr;
    ID3D11VideoContext* videoContext = nullptr;
    ID3D11VideoContext1* videoContext1 = nullptr;
    IDXGISwapChain1* swapChain = nullptr;
    ID3D11Texture2D* nv12Textures[kUploadSurfaceCount]{};
    ID3D11VideoProcessorEnumerator* enumerator = nullptr;
    ID3D11VideoProcessor* processor = nullptr;
    ID3D11VideoProcessorInputView* inputViews[kUploadSurfaceCount]{};
    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* backBufferRenderTarget = nullptr;
    ID3D11VideoProcessorOutputView* outputView = nullptr;
    ID2D1Factory* d2dFactory = nullptr;
    ID2D1RenderTarget* osdCacheTarget = nullptr;
    ID2D1RenderTarget* volumeCacheTarget = nullptr;
    ID2D1RenderTarget* audioCacheTarget = nullptr;
    ID2D1SolidColorBrush* osdCacheBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* osdCacheTextBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheTextBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheBarBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheBarBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheTextBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheBarBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheBarBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheHighlightBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheClipBrush = nullptr;
    ID3D11Texture2D* osdOverlayTexture = nullptr;
    ID3D11Texture2D* volumeOverlayTexture = nullptr;
    ID3D11Texture2D* audioOverlayTexture = nullptr;
    ID3D11ShaderResourceView* osdOverlayShaderView = nullptr;
    ID3D11ShaderResourceView* volumeOverlayShaderView = nullptr;
    ID3D11ShaderResourceView* audioOverlayShaderView = nullptr;
    ID3D11VertexShader* overlayVertexShader = nullptr;
    ID3D11PixelShader* overlayPixelShader = nullptr;
    ID3D11Buffer* overlayRectBuffer = nullptr;
    ID3D11SamplerState* overlaySampler = nullptr;
    ID3D11BlendState* overlayBlendState = nullptr;
    IDWriteFactory* dwriteFactory = nullptr;
    IDWriteTextFormat* osdTextFormat = nullptr;
    IDWriteTextFormat* volumeTextFormat = nullptr;
    IDWriteTextFormat* audioTextFormat = nullptr;
    IDWriteTextLayout* osdTextLayout = nullptr;
    IDWriteTextLayout* volumeTextLayout = nullptr;
    UINT outputWidth = 0;
    UINT outputHeight = 0;
    bool pixelPerfectFullscreen = false;
    bool pixelPerfectBorders = false;
    uint64_t outputConfigurationGeneration = 0;
    uint64_t cachedOverlayGeneration = 0;
    UINT nextUploadSurface = 0;
    UINT activeUploadSurface = 0;
    bool allowTearing = false;
    bool sharpScalingActive = false;
    bool discardUpdateAvailable = false;
    bool occluded = false;
    bool occlusionLogged = false;
    uint64_t nextOcclusionTestMs = 0;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_NV12;
    bool hdrOutput = false;
    llcv::video_color::Configuration sdrColor{};

    void reset() {
        if (context) {
            context->ClearState();
            context->Flush();
        }
        SafeRelease(overlayBlendState);
        SafeRelease(overlaySampler);
        SafeRelease(overlayRectBuffer);
        SafeRelease(overlayPixelShader);
        SafeRelease(overlayVertexShader);
        SafeRelease(audioOverlayShaderView);
        SafeRelease(volumeOverlayShaderView);
        SafeRelease(osdOverlayShaderView);
        SafeRelease(audioOverlayTexture);
        SafeRelease(volumeOverlayTexture);
        SafeRelease(osdOverlayTexture);
        SafeRelease(audioCacheClipBrush);
        SafeRelease(audioCacheHighlightBrush);
        SafeRelease(audioCacheTextBrush);
        SafeRelease(audioCacheBackgroundBrush);
        SafeRelease(audioCacheBarBrush);
        SafeRelease(audioCacheBarBackgroundBrush);
        SafeRelease(volumeCacheBarBrush);
        SafeRelease(volumeCacheBarBackgroundBrush);
        SafeRelease(volumeCacheTextBrush);
        SafeRelease(volumeCacheBackgroundBrush);
        SafeRelease(osdCacheTextBrush);
        SafeRelease(osdCacheBackgroundBrush);
        SafeRelease(audioCacheTarget);
        SafeRelease(volumeCacheTarget);
        SafeRelease(osdCacheTarget);
        SafeRelease(volumeTextLayout);
        SafeRelease(osdTextLayout);
        SafeRelease(volumeTextFormat);
        SafeRelease(audioTextFormat);
        SafeRelease(osdTextFormat);
        SafeRelease(dwriteFactory);
        SafeRelease(d2dFactory);
        SafeRelease(outputView);
        SafeRelease(backBufferRenderTarget);
        SafeRelease(backBuffer);
        for (UINT i = 0; i < kUploadSurfaceCount; ++i) {
            SafeRelease(inputViews[i]);
        }
        SafeRelease(processor);
        SafeRelease(enumerator);
        for (UINT i = 0; i < kUploadSurfaceCount; ++i) {
            SafeRelease(nv12Textures[i]);
        }
        SafeRelease(swapChain);
        SafeRelease(videoContext);
        SafeRelease(videoContext1);
        SafeRelease(videoDevice);
        SafeRelease(context1);
        SafeRelease(context);
        SafeRelease(device);
        outputWidth = 0;
        outputHeight = 0;
        pixelPerfectFullscreen = false;
        pixelPerfectBorders = false;
        outputConfigurationGeneration = 0;
        cachedOverlayGeneration = 0;
        nextUploadSurface = 0;
        activeUploadSurface = 0;
        allowTearing = false;
        sharpScalingActive = false;
        discardUpdateAvailable = false;
        occluded = false;
        occlusionLogged = false;
        nextOcclusionTestMs = 0;
        inputFormat = DXGI_FORMAT_NV12;
        hdrOutput = false;
        sdrColor = {};
        g_hdrOutputActive.store(false, std::memory_order_release);
    }

    ~DirectD3D11Renderer() {
        reset();
    }

    HRESULT deviceRemovedReason() const {
        return device ? device->GetDeviceRemovedReason() : E_POINTER;
    }

    bool outputConfigurationChanged() const {
        return outputConfigurationGeneration !=
            g_outputConfigurationGeneration.load(std::memory_order_acquire);
    }

    HRESULT initialize(HWND hwnd, int width, int height, int fps,
                       VideoPixelFormat pixelFormat,
                       bool hdrInputMetadataAvailable = false,
                       llcv::video_color::Configuration color = {}) {
        reset();
        sdrColor = color;
        g_activeVideoColorMatrix.store(static_cast<int>(sdrColor.matrix),
                                       std::memory_order_release);
        g_activeVideoColorRange.store(static_cast<int>(sdrColor.range),
                                      std::memory_order_release);
        g_activeVideoColorMatrixSource.store(
            static_cast<int>(sdrColor.matrixSource),
            std::memory_order_release);
        g_activeVideoColorRangeSource.store(
            static_cast<int>(sdrColor.rangeSource),
            std::memory_order_release);
        const uint64_t configurationGeneration =
            g_outputConfigurationGeneration.load(std::memory_order_acquire);
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        outputWidth = static_cast<UINT>((std::max)(
            1L, clientRect.right - clientRect.left));
        outputHeight = static_cast<UINT>((std::max)(
            1L, clientRect.bottom - clientRect.top));
        pixelPerfectFullscreen =
            g_settings.pixelPerfect &&
            g_fullscreen.load(std::memory_order_acquire);
        outputConfigurationGeneration = configurationGeneration;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL featureLevel{};
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
            D3D11_SDK_VERSION, &device, &featureLevel, &context);
        if (FAILED(hr)) return hr;
        discardUpdateAvailable =
            SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1)));
        hr = device->QueryInterface(IID_PPV_ARGS(&videoDevice));
        if (FAILED(hr)) return hr;
        hr = context->QueryInterface(IID_PPV_ARGS(&videoContext));
        if (FAILED(hr)) return hr;
        if (pixelFormat == VideoPixelFormat::P010 &&
            hdrInputMetadataAvailable) {
            hr = context->QueryInterface(IID_PPV_ARGS(&videoContext1));
            if (FAILED(hr)) {
                fwprintf(stderr,
                         L"[hdr] ID3D11VideoContext1 unavailable; HDR10 path rejected.\n");
                return hr;
            }
            hdrOutput = true;
        } else if (pixelFormat == VideoPixelFormat::P010) {
            fwprintf(stderr,
                     L"[hdr] P010 color metadata unavailable; using "
                     L"BT.709 SDR output to avoid forced HDR color conversion.\n");
        }

        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            SafeRelease(factory);
            SafeRelease(adapter);
            SafeRelease(dxgiDevice);
            return hr;
        }

        IDXGIFactory5* factory5 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
            BOOL supported = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported,
                    sizeof(supported)))) {
                allowTearing = supported == TRUE;
            }
        }
        SafeRelease(factory5);

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = outputWidth;
        swapDesc.Height = outputHeight;
        swapDesc.Format = hdrOutput ? DXGI_FORMAT_R10G10B10A2_UNORM
                                    : DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (allowTearing) swapDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        hr = factory->CreateSwapChainForHwnd(device, hwnd, &swapDesc, nullptr,
                                             nullptr, &swapChain);
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        SafeRelease(factory);
        SafeRelease(adapter);
        SafeRelease(dxgiDevice);
        if (FAILED(hr)) return hr;

        if (hdrOutput) {
            IDXGISwapChain3* swapChain3 = nullptr;
            hr = swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3));
            if (SUCCEEDED(hr)) {
                hr = swapChain3->SetColorSpace1(
                    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            }
            SafeRelease(swapChain3);
            if (FAILED(hr)) {
                fwprintf(stderr,
                         L"[hdr] HDR10 swapchain color space unavailable; "
                         L"P010 path rejected (0x%08X).\n",
                         static_cast<unsigned>(hr));
                return hr;
            }
            IDXGISwapChain4* swapChain4 = nullptr;
            if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain4)))) {
                DXGI_HDR_METADATA_HDR10 metadata{};
                metadata.RedPrimary[0] = 34000;
                metadata.RedPrimary[1] = 16000;
                metadata.GreenPrimary[0] = 13250;
                metadata.GreenPrimary[1] = 34500;
                metadata.BluePrimary[0] = 7500;
                metadata.BluePrimary[1] = 3000;
                metadata.WhitePoint[0] = 15635;
                metadata.WhitePoint[1] = 16450;
                metadata.MaxMasteringLuminance = 10000000;
                metadata.MinMasteringLuminance = 1;
                metadata.MaxContentLightLevel = 1000;
                metadata.MaxFrameAverageLightLevel = 400;
                swapChain4->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_HDR10,
                                            sizeof(metadata), &metadata);
                SafeRelease(swapChain4);
            }
            fwprintf(stderr,
                     L"[hdr] experimental HDR10 output active: P010 -> "
                     L"BT.2020 PQ 10-bit swap chain; no frame queue.\n");
            g_hdrOutputActive.store(true, std::memory_order_release);
        }

        IDXGISwapChain2* swapChain2 = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain2)))) {
            hr = swapChain2->SetMaximumFrameLatency(1);
            SafeRelease(swapChain2);
            if (FAILED(hr)) return hr;
        }

        IDXGIDevice1* dxgiDevice1 = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice1)))) {
            dxgiDevice1->SetMaximumFrameLatency(1);
        }
        SafeRelease(dxgiDevice1);

        inputFormat = PixelFormatDxgi(pixelFormat);
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = static_cast<UINT>(width);
        textureDesc.Height = static_cast<UINT>(height);
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = inputFormat;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        for (UINT i = 0; i < kUploadSurfaceCount; ++i) {
            hr = device->CreateTexture2D(&textureDesc, nullptr,
                                         &nv12Textures[i]);
            if (FAILED(hr)) return hr;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {static_cast<UINT>(fps), 1};
        content.InputWidth = static_cast<UINT>(width);
        content.InputHeight = static_cast<UINT>(height);
        content.OutputFrameRate = {static_cast<UINT>(fps), 1};
        content.OutputWidth = outputWidth;
        content.OutputHeight = outputHeight;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        hr = videoDevice->CreateVideoProcessorEnumerator(&content, &enumerator);
        if (FAILED(hr)) return hr;
        UINT formatSupport = 0;
        hr = enumerator->CheckVideoProcessorFormat(inputFormat,
                                                   &formatSupport);
        if (FAILED(hr) ||
            (formatSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
            return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        hr = videoDevice->CreateVideoProcessor(enumerator, 0, &processor);
        if (FAILED(hr)) return hr;
        if (g_settings.scalingMode == ScalingMode::Sharp &&
            !g_settings.pixelPerfect) {
            D3D11_VIDEO_PROCESSOR_FILTER_RANGE sharpness{};
            if (SUCCEEDED(enumerator->GetVideoProcessorFilterRange(
                    D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, &sharpness)) &&
                sharpness.Maximum > sharpness.Minimum) {
                const int value = sharpness.Default +
                    (sharpness.Maximum - sharpness.Default) / 2;
                videoContext->VideoProcessorSetStreamFilter(
                    processor, 0, D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT,
                    TRUE, std::clamp(value, sharpness.Minimum,
                                     sharpness.Maximum));
                sharpScalingActive = true;
                fwprintf(stderr,
                         L"[video] scaling: sharp (Video Processor, value %d)\n",
                         std::clamp(value, sharpness.Minimum,
                                    sharpness.Maximum));
            } else {
                fwprintf(stderr,
                         L"[video] sharp scaling unavailable; using smooth scaling.\n");
            }
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.FourCC = 0;
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        for (UINT i = 0; i < kUploadSurfaceCount; ++i) {
            hr = videoDevice->CreateVideoProcessorInputView(
                nv12Textures[i], enumerator, &inputDesc, &inputViews[i]);
            if (FAILED(hr)) return hr;
        }
        hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) return hr;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = videoDevice->CreateVideoProcessorOutputView(
            backBuffer, enumerator, &outputDesc, &outputView);
        if (FAILED(hr)) return hr;
        hr = device->CreateRenderTargetView(
            backBuffer, nullptr, &backBufferRenderTarget);
        if (FAILED(hr)) return hr;

        D3D11_TEXTURE2D_DESC overlayDesc{};
        overlayDesc.Width = kOsdOverlayWidth;
        overlayDesc.Height = kOsdOverlayHeight;
        overlayDesc.MipLevels = 1;
        overlayDesc.ArraySize = 1;
        overlayDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        overlayDesc.SampleDesc.Count = 1;
        overlayDesc.Usage = D3D11_USAGE_DEFAULT;
        overlayDesc.BindFlags = D3D11_BIND_RENDER_TARGET |
                                D3D11_BIND_SHADER_RESOURCE;
        hr = device->CreateTexture2D(&overlayDesc, nullptr,
                                     &osdOverlayTexture);
        if (FAILED(hr)) return hr;
        overlayDesc.Width = 260;
        overlayDesc.Height = 82;
        hr = device->CreateTexture2D(&overlayDesc, nullptr,
                                     &volumeOverlayTexture);
        if (FAILED(hr)) return hr;
        overlayDesc.Width = kAudioOsdWidth;
        overlayDesc.Height = kAudioOsdHeight;
        hr = device->CreateTexture2D(&overlayDesc, nullptr,
                                     &audioOverlayTexture);
        if (FAILED(hr)) return hr;
        hr = device->CreateShaderResourceView(
            osdOverlayTexture, nullptr, &osdOverlayShaderView);
        if (SUCCEEDED(hr)) {
            hr = device->CreateShaderResourceView(
                volumeOverlayTexture, nullptr, &volumeOverlayShaderView);
        }
        if (SUCCEEDED(hr)) {
            hr = device->CreateShaderResourceView(
                audioOverlayTexture, nullptr, &audioOverlayShaderView);
        }
        if (FAILED(hr)) return hr;
        static constexpr char overlayVertexSource[] =
            "cbuffer RectBuffer : register(b0) { float4 rect; };"
            "struct VSOut { float4 position : SV_POSITION; "
            "float2 uv : TEXCOORD0; };"
            "VSOut main(uint id : SV_VertexID) {"
            "float2 positions[4] = { float2(0,0), float2(1,0), "
            "float2(0,1), float2(1,1) };"
            "VSOut o; float2 p = positions[id];"
            "o.position = float4(lerp(rect.x, rect.z, p.x), "
            "lerp(rect.y, rect.w, p.y), 0, 1); o.uv = p; return o; }";
        static constexpr char overlayPixelSource[] =
            "Texture2D overlayTexture : register(t0);"
            "SamplerState overlaySampler : register(s0);"
            "float4 main(float4 position : SV_POSITION, "
            "float2 uv : TEXCOORD0) : SV_TARGET {"
            "return overlayTexture.Sample(overlaySampler, uv); }";
        ID3DBlob* vertexBlob = nullptr;
        ID3DBlob* pixelBlob = nullptr;
        ID3DBlob* shaderErrors = nullptr;
        hr = D3DCompile(overlayVertexSource,
                        sizeof(overlayVertexSource) - 1, nullptr, nullptr,
                        nullptr, "main", "vs_4_0", 0, 0, &vertexBlob,
                        &shaderErrors);
        SafeRelease(shaderErrors);
        if (SUCCEEDED(hr)) {
            hr = D3DCompile(overlayPixelSource,
                            sizeof(overlayPixelSource) - 1, nullptr, nullptr,
                            nullptr, "main", "ps_4_0", 0, 0, &pixelBlob,
                            &shaderErrors);
        }
        SafeRelease(shaderErrors);
        if (SUCCEEDED(hr)) {
            hr = device->CreateVertexShader(
                vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                nullptr, &overlayVertexShader);
        }
        if (SUCCEEDED(hr)) {
            hr = device->CreatePixelShader(
                pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(),
                nullptr, &overlayPixelShader);
        }
        SafeRelease(pixelBlob);
        SafeRelease(vertexBlob);
        if (FAILED(hr)) return hr;

        D3D11_BUFFER_DESC constantBufferDesc{};
        constantBufferDesc.ByteWidth = sizeof(float) * 4;
        constantBufferDesc.Usage = D3D11_USAGE_DEFAULT;
        constantBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device->CreateBuffer(&constantBufferDesc, nullptr,
                                  &overlayRectBuffer);
        if (FAILED(hr)) return hr;
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&samplerDesc, &overlaySampler);
        if (FAILED(hr)) return hr;
        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend =
            D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha =
            D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&blendDesc, &overlayBlendState);
        if (FAILED(hr)) return hr;

        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               &d2dFactory);
        if (FAILED(hr)) return hr;
        const D2D1_RENDER_TARGET_PROPERTIES d2dProperties =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_HARDWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED));
        IDXGISurface* osdSurface = nullptr;
        IDXGISurface* volumeSurface = nullptr;
        IDXGISurface* audioSurface = nullptr;
        hr = osdOverlayTexture->QueryInterface(IID_PPV_ARGS(&osdSurface));
        if (SUCCEEDED(hr)) {
            hr = volumeOverlayTexture->QueryInterface(
                IID_PPV_ARGS(&volumeSurface));
        }
        if (SUCCEEDED(hr)) {
            hr = audioOverlayTexture->QueryInterface(
                IID_PPV_ARGS(&audioSurface));
        }
        if (SUCCEEDED(hr)) {
            hr = d2dFactory->CreateDxgiSurfaceRenderTarget(
                osdSurface, &d2dProperties, &osdCacheTarget);
        }
        if (SUCCEEDED(hr)) {
            hr = d2dFactory->CreateDxgiSurfaceRenderTarget(
                volumeSurface, &d2dProperties, &volumeCacheTarget);
        }
        if (SUCCEEDED(hr)) {
            hr = d2dFactory->CreateDxgiSurfaceRenderTarget(
                audioSurface, &d2dProperties, &audioCacheTarget);
        }
        SafeRelease(audioSurface);
        SafeRelease(volumeSurface);
        SafeRelease(osdSurface);
        if (FAILED(hr)) return hr;
        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&dwriteFactory));
        if (FAILED(hr)) return hr;
        hr = dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f,
            IsEnglishUi() ? L"en-US" : L"ko-KR", &osdTextFormat);
        if (FAILED(hr)) return hr;
        hr = dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 22.0f,
            IsEnglishUi() ? L"en-US" : L"ko-KR", &volumeTextFormat);
        if (FAILED(hr)) return hr;
        hr = dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f,
            IsEnglishUi() ? L"en-US" : L"ko-KR", &audioTextFormat);
        if (FAILED(hr)) return hr;
        if (SUCCEEDED(hr)) {
            hr = osdCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.055f, 0.063f, 0.078f, 0.90f),
                &osdCacheBackgroundBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = osdCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.91f, 0.93f, 0.95f, 1.0f),
                &osdCacheTextBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = volumeCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.055f, 0.063f, 0.078f, 0.90f),
                &volumeCacheBackgroundBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = volumeCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.91f, 0.93f, 0.95f, 1.0f),
                &volumeCacheTextBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = volumeCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.20f, 0.22f, 0.25f, 1.0f),
                &volumeCacheBarBackgroundBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = volumeCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.25f, 0.78f, 0.48f, 1.0f),
                &volumeCacheBarBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = audioCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.055f, 0.063f, 0.078f, 0.92f),
                &audioCacheBackgroundBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = audioCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.91f, 0.93f, 0.95f, 1.0f),
                &audioCacheTextBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = audioCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.20f, 0.22f, 0.25f, 1.0f),
                &audioCacheBarBackgroundBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = audioCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.25f, 0.78f, 0.48f, 1.0f),
                &audioCacheBarBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = audioCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.21f, 0.36f, 0.45f, 0.95f),
                &audioCacheHighlightBrush);
        }
        if (SUCCEEDED(hr)) {
            hr = audioCacheTarget->CreateSolidColorBrush(
                D2D1::ColorF(0.93f, 0.34f, 0.29f, 1.0f),
                &audioCacheClipBrush);
        }
        if (FAILED(hr)) return hr;

        RECT sourceRect{0, 0, width, height};
        RECT outputRect{0, 0, static_cast<LONG>(outputWidth),
                        static_cast<LONG>(outputHeight)};
        RECT videoRect = outputRect;
        // Scaled mode must preserve the capture aspect ratio even when the
        // user maximizes the window on an ultrawide monitor.  The window
        // sizing constraint covers interactive edge-resizing, but it cannot
        // constrain maximize/DPI transitions or arbitrary window rectangles.
        // Compute a fit rectangle here as the final rendering safeguard.
        pixelPerfectBorders = false;
        if (pixelPerfectFullscreen) {
            LONG displayWidth = 0;
            LONG displayHeight = 0;
            if (width <= static_cast<int>(outputWidth) &&
                height <= static_cast<int>(outputHeight)) {
                // Pixel-perfect means strict 1:1 mapping. Do not turn FHD
                // into a 2x 4K Video Processor upscale; center the original
                // capture pixels and clear the unused output to black.
                displayWidth = static_cast<LONG>(width);
                displayHeight = static_cast<LONG>(height);
                fwprintf(stderr,
                         L"[video] pixel-perfect fullscreen: strict 1:1, "
                         L"centered %ld x %ld in %u x %u.\n",
                         displayWidth, displayHeight,
                         outputWidth, outputHeight);
            } else {
                // Exact pixel mapping cannot fit when the capture is larger
                // than the display. Preserve the complete picture and aspect
                // ratio instead of cropping it.
                const double scale = (std::min)(
                    static_cast<double>(outputWidth) / width,
                    static_cast<double>(outputHeight) / height);
                displayWidth = (std::max)(
                    1L, static_cast<LONG>(std::lround(width * scale)));
                displayHeight = (std::max)(
                    1L, static_cast<LONG>(std::lround(height * scale)));
                fwprintf(stderr,
                         L"[video] pixel-perfect fullscreen cannot fit 1:1; "
                         L"aspect-preserving downscale to %ld x %ld in %u x %u.\n",
                         displayWidth, displayHeight, outputWidth, outputHeight);
            }
            videoRect.left =
                (static_cast<LONG>(outputWidth) - displayWidth) / 2;
            videoRect.top =
                (static_cast<LONG>(outputHeight) - displayHeight) / 2;
            videoRect.right = videoRect.left + displayWidth;
            videoRect.bottom = videoRect.top + displayHeight;
            pixelPerfectBorders =
                videoRect.left != outputRect.left ||
                videoRect.top != outputRect.top ||
                videoRect.right != outputRect.right ||
                videoRect.bottom != outputRect.bottom;
        } else {
            const double scale = (std::min)(
                static_cast<double>(outputWidth) / width,
                static_cast<double>(outputHeight) / height);
            const LONG displayWidth = (std::max)(
                1L, static_cast<LONG>(std::lround(width * scale)));
            const LONG displayHeight = (std::max)(
                1L, static_cast<LONG>(std::lround(height * scale)));
            videoRect.left =
                (static_cast<LONG>(outputWidth) - displayWidth) / 2;
            videoRect.top =
                (static_cast<LONG>(outputHeight) - displayHeight) / 2;
            videoRect.right = videoRect.left + displayWidth;
            videoRect.bottom = videoRect.top + displayHeight;
            pixelPerfectBorders =
                videoRect.left != outputRect.left ||
                videoRect.top != outputRect.top ||
                videoRect.right != outputRect.right ||
                videoRect.bottom != outputRect.bottom;
        }
        videoContext->VideoProcessorSetStreamSourceRect(processor, 0, TRUE,
                                                        &sourceRect);
        videoContext->VideoProcessorSetStreamDestRect(processor, 0, TRUE,
                                                      &videoRect);
        videoContext->VideoProcessorSetOutputTargetRect(processor, TRUE,
                                                        &outputRect);
        if (hdrOutput) {
            // P010 HDR10 prototype: use the explicit DXGI color-space APIs
            // instead of the legacy BT.709-only bitfield. This path is only
            // selected when the user explicitly chooses P010.
            videoContext1->VideoProcessorSetStreamColorSpace1(
                processor, 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020);
            videoContext1->VideoProcessorSetOutputColorSpace1(
                processor, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};
            inputColor.YCbCr_Matrix =
                sdrColor.matrix == llcv::video_color::Matrix::Bt709 ? 1u : 0u;
            inputColor.Nominal_Range =
                sdrColor.range == llcv::video_color::Range::Full
                    ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                    : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            videoContext->VideoProcessorSetStreamColorSpace(processor, 0,
                                                            &inputColor);
        }
        return S_OK;
    }

    void upload(const BYTE* pixels, UINT32 stride) {
        // Rotate upload targets so the CPU never updates the NV12 surface that
        // the GPU is still reading. This avoids UpdateSubresource's contended
        // two-copy path without introducing a video-frame queue.
        activeUploadSurface = nextUploadSurface;
        nextUploadSurface = (nextUploadSurface + 1) % kUploadSurfaceCount;
        ID3D11Texture2D* target = nv12Textures[activeUploadSurface];
        if (context1) {
            context1->UpdateSubresource1(target, 0, nullptr, pixels, stride, 0,
                                         D3D11_COPY_DISCARD);
        } else {
            context->UpdateSubresource(target, 0, nullptr, pixels, stride, 0);
        }
    }

    HRESULT refreshOverlayLayouts() {
        const uint64_t generation =
            g_overlayGeneration.load(std::memory_order_acquire);
        if (cachedOverlayGeneration == generation) return S_OK;
        cachedOverlayGeneration = generation;
        SafeRelease(osdTextLayout);
        SafeRelease(volumeTextLayout);

        // Keep the diagnostics panel at a stable size. Device names are kept
        // verbatim; the output device has its own line so long names do not
        // need an ellipsis just to share a line with the audio mode.
        std::wstring osdText;
        HRESULT hr = E_FAIL;
        osdText = BuildRuntimeOsdText(
            static_cast<int>(outputWidth), static_cast<int>(outputHeight));
        hr = dwriteFactory->CreateTextLayout(
            osdText.c_str(), static_cast<UINT32>(osdText.size()),
            osdTextFormat, kOsdTextWidth, kOsdTextHeight, &osdTextLayout);
        if (FAILED(hr)) return hr;

        const TransientHudContent hudContent =
            g_transientHudContent.load(std::memory_order_acquire);
        wchar_t volumeText[96]{};
        if (hudContent == TransientHudContent::OneToOne) {
            const auto& video = CurrentVideoPreset();
            swprintf_s(volumeText,
                       IsEnglishUi() ? L"1:1 Pixel-perfect\n%d x %d"
                                     : L"1:1 Pixel-perfect\n%d x %d",
                       video.width, video.height);
        } else if (hudContent ==
                   TransientHudContent::OneToOneUnavailable) {
            wcscpy_s(volumeText,
                     IsEnglishUi() ? L"1:1 unavailable\nLarger than this display"
                                   : L"1:1 표시 불가\n현재 모니터보다 큼");
        } else {
            swprintf_s(volumeText, UI_TEXT(L"음량  %d%%"),
                       g_volumePercent.load(std::memory_order_acquire));
        }
        hr = dwriteFactory->CreateTextLayout(
            volumeText, static_cast<UINT32>(wcslen(volumeText)),
            volumeTextFormat, 228.0f, 62.0f, &volumeTextLayout);
        if (FAILED(hr)) return hr;

        osdCacheTarget->BeginDraw();
        osdCacheTarget->Clear(D2D1::ColorF(0, 0.0f));
        osdCacheTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(
                                  0.0f, 0.0f,
                                  static_cast<float>(kOsdOverlayWidth),
                                  static_cast<float>(kOsdOverlayHeight)),
                              8.0f, 8.0f),
            osdCacheBackgroundBrush);
        osdCacheTarget->DrawTextLayout(
            D2D1::Point2F(16.0f, 12.0f), osdTextLayout,
            osdCacheTextBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        hr = osdCacheTarget->EndDraw();
        if (FAILED(hr)) return hr;

        volumeCacheTarget->BeginDraw();
        volumeCacheTarget->Clear(D2D1::ColorF(0, 0.0f));
        volumeCacheTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, 260.0f, 82.0f),
                              8.0f, 8.0f),
            volumeCacheBackgroundBrush);
        volumeCacheTarget->DrawTextLayout(
            D2D1::Point2F(16.0f, 6.0f), volumeTextLayout,
            volumeCacheTextBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (hudContent == TransientHudContent::Volume) {
            const D2D1_RECT_F volumeBarBackground =
                D2D1::RectF(16.0f, 57.0f, 244.0f, 66.0f);
            volumeCacheTarget->FillRectangle(
                volumeBarBackground, volumeCacheBarBackgroundBrush);
            D2D1_RECT_F volumeBar = volumeBarBackground;
            const int volumeBarMaximum = g_settings.allowVolumeBoost
                ? kMaximumVolumePercent : 100;
            volumeBar.right = volumeBar.left +
                (volumeBarBackground.right - volumeBarBackground.left) *
                    g_volumePercent.load(std::memory_order_acquire) /
                    static_cast<float>(volumeBarMaximum);
            if (volumeBar.right > volumeBar.left) {
                volumeCacheTarget->FillRectangle(volumeBar,
                                                 volumeCacheBarBrush);
            }
        }
        hr = volumeCacheTarget->EndDraw();
        if (FAILED(hr)) return hr;

        if (g_audioOsdVisible.load(std::memory_order_acquire)) {
            const int maximum = g_settings.allowVolumeBoost
                ? kMaximumVolumePercent : 100;
            constexpr int channelMaximum = 100;
            const int master = g_volumePercent.load(std::memory_order_acquire);
            const int left = g_leftVolumePercent.load(std::memory_order_acquire);
            const int right = g_rightVolumePercent.load(std::memory_order_acquire);
            const int hovered = g_audioOsdHoverTarget.load(
                std::memory_order_acquire);
            const double leftDb = llcv::audio::PeakToDbfs(
                g_audioPeakLeft.load(std::memory_order_acquire));
            const double rightDb = llcv::audio::PeakToDbfs(
                g_audioPeakRight.load(std::memory_order_acquire));
            const bool clipping = GetTickCount64() < g_audioClipUntilMs.load(
                std::memory_order_acquire);

            audioCacheTarget->BeginDraw();
            audioCacheTarget->Clear(D2D1::ColorF(0, 0.0f));
            audioCacheTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f,
                                               static_cast<float>(kAudioOsdWidth),
                                               static_cast<float>(kAudioOsdHeight)),
                                  8.0f, 8.0f),
                audioCacheBackgroundBrush);
            const wchar_t* title = IsEnglishUi() ? L"Audio" : L"오디오";
            audioCacheTarget->DrawTextW(title, static_cast<UINT32>(wcslen(title)),
                                         audioTextFormat,
                                         D2D1::RectF(16, 10, 120, 34),
                                         audioCacheTextBrush);
            wchar_t masterText[48]{};
            swprintf_s(masterText, IsEnglishUi() ? L"Master  %d%%" : L"마스터  %d%%",
                       master);
            audioCacheTarget->DrawTextW(masterText,
                                         static_cast<UINT32>(wcslen(masterText)),
                                         audioTextFormat,
                                         D2D1::RectF(16, 38, 310, 62),
                                         audioCacheTextBrush);
            const D2D1_RECT_F masterBar = D2D1::RectF(16, 64, 320, 71);
            audioCacheTarget->FillRectangle(masterBar, audioCacheBarBackgroundBrush);
            D2D1_RECT_F masterFill = masterBar;
            masterFill.right = masterFill.left +
                (masterBar.right - masterBar.left) * master / maximum;
            if (masterFill.right > masterFill.left) {
                audioCacheTarget->FillRectangle(masterFill, audioCacheBarBrush);
            }

            const auto drawChannel = [&](int channel, const wchar_t* label,
                                         int percent, double peakDb,
                                         float x0, float x1) {
                const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(
                    D2D1::RectF(x0, 84, x1, 168), 6.0f, 6.0f);
                if (hovered == channel) {
                    audioCacheTarget->FillRoundedRectangle(card,
                                                            audioCacheHighlightBrush);
                }
                audioCacheTarget->DrawRoundedRectangle(card,
                                                       audioCacheTextBrush, 1.0f);
                wchar_t text[96]{};
                swprintf_s(text, L"%s\n%d%%\n%.1f dBFS", label, percent, peakDb);
                audioCacheTarget->DrawTextW(text, static_cast<UINT32>(wcslen(text)),
                                             audioTextFormat,
                                             D2D1::RectF(x0 + 14, 92, x1 - 12, 157),
                                             audioCacheTextBrush);
                const D2D1_RECT_F bar = D2D1::RectF(x0 + 14, 157, x1 - 14, 163);
                audioCacheTarget->FillRectangle(bar, audioCacheBarBackgroundBrush);
                D2D1_RECT_F fill = bar;
                fill.right = fill.left + (bar.right - bar.left) * percent /
                    channelMaximum;
                if (fill.right > fill.left) audioCacheTarget->FillRectangle(
                    fill, audioCacheBarBrush);
            };
            drawChannel(1, L"L", left, leftDb, 16.0f, 160.0f);
            drawChannel(2, L"R", right, rightDb, 176.0f, 320.0f);
            const wchar_t* clipText = clipping
                ? (IsEnglishUi() ? L"CLIP" : L"클리핑")
                : (IsEnglishUi() ? L"No clipping" : L"클리핑 없음");
            audioCacheTarget->DrawTextW(
                clipText, static_cast<UINT32>(wcslen(clipText)), audioTextFormat,
                D2D1::RectF(16, 172, 320, 192),
                clipping ? audioCacheClipBrush : audioCacheTextBrush);
            hr = audioCacheTarget->EndDraw();
        }
        return hr;
    }

    HRESULT drawOverlayQuads() {
        const bool osdVisible =
            g_osdVisible.load(std::memory_order_acquire);
        const bool volumeVisible = GetTickCount64() <
            g_volumeHudUntilMs.load(std::memory_order_acquire);
        const bool audioVisible =
            g_audioOsdVisible.load(std::memory_order_acquire);
        if (!osdVisible && !volumeVisible && !audioVisible) return S_OK;
        HRESULT hr = refreshOverlayLayouts();
        if (FAILED(hr)) return hr;

        const D3D11_VIEWPORT viewport{
            0.0f, 0.0f, static_cast<float>(outputWidth),
            static_cast<float>(outputHeight), 0.0f, 1.0f};
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &backBufferRenderTarget, nullptr);
        const float blendFactor[4]{};
        context->OMSetBlendState(overlayBlendState, blendFactor,
                                 0xffffffffu);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(overlayVertexShader, nullptr, 0);
        context->VSSetConstantBuffers(0, 1, &overlayRectBuffer);
        context->PSSetShader(overlayPixelShader, nullptr, 0);
        context->PSSetSamplers(0, 1, &overlaySampler);

        auto draw = [&](ID3D11ShaderResourceView* texture,
                        LONG left, LONG top, LONG right, LONG bottom) {
            const float rectangle[4]{
                -1.0f + 2.0f * left / outputWidth,
                1.0f - 2.0f * top / outputHeight,
                -1.0f + 2.0f * right / outputWidth,
                1.0f - 2.0f * bottom / outputHeight};
            context->UpdateSubresource(overlayRectBuffer, 0, nullptr,
                                       rectangle, 0, 0);
            context->PSSetShaderResources(0, 1, &texture);
            context->Draw(4, 0);
        };

        constexpr LONG margin = 16;
        constexpr LONG osdWidth =
            static_cast<LONG>(kOsdOverlayWidth);
        constexpr LONG osdHeight =
            static_cast<LONG>(kOsdOverlayHeight);
        if (osdVisible) {
            draw(osdOverlayShaderView, margin, margin, margin + osdWidth,
                 margin + osdHeight);
        }

        if (volumeVisible) {
            constexpr LONG hudWidth = 260;
            constexpr LONG hudHeight = 82;
            constexpr LONG hudMargin = 24;
            LONG x = hudMargin;
            LONG y = hudMargin;
            const bool right =
                g_settings.volumeHudPosition ==
                    VolumeHudPosition::TopRight ||
                g_settings.volumeHudPosition ==
                    VolumeHudPosition::BottomRight;
            const bool bottom =
                g_settings.volumeHudPosition ==
                    VolumeHudPosition::BottomLeft ||
                g_settings.volumeHudPosition ==
                    VolumeHudPosition::BottomRight;
            if (right) {
                x = (std::max)(hudMargin,
                               static_cast<LONG>(outputWidth) - hudWidth -
                                   hudMargin);
            }
            if (bottom) {
                y = (std::max)(hudMargin,
                               static_cast<LONG>(outputHeight) - hudHeight -
                                   hudMargin);
            } else if (!right && osdVisible) {
                y = margin + osdHeight + 12;
            }
            draw(volumeOverlayShaderView, x, y, x + hudWidth,
                 y + hudHeight);
        }
        if (audioVisible) {
            const llcv::audio_osd::Rect rect =
                llcv::audio_osd::RectForClient(static_cast<int>(outputWidth));
            draw(audioOverlayShaderView, rect.left, rect.top,
                 rect.right, rect.bottom);
        }
        ID3D11ShaderResourceView* noTexture = nullptr;
        context->PSSetShaderResources(0, 1, &noTexture);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        g_overlayRenderedFrames.fetch_add(1,
                                          std::memory_order_relaxed);
        return S_OK;
    }

    HRESULT presentUploaded() {
        if (occluded) {
            const uint64_t nowMs = GetTickCount64();
            if (nowMs < nextOcclusionTestMs) return DXGI_STATUS_OCCLUDED;
            const HRESULT test = swapChain->Present(0, DXGI_PRESENT_TEST);
            if (test == DXGI_STATUS_OCCLUDED) {
                nextOcclusionTestMs = nowMs + 50;
                return test;
            }
            if (FAILED(test)) return test;
            occluded = false;
            if (occlusionLogged) {
                fwprintf(stderr,
                         L"[display-event] swapchain visible again after "
                         L"occlusion; uptime=%llu ms\n",
                         static_cast<unsigned long long>(nowMs));
                occlusionLogged = false;
            }
            nextOcclusionTestMs = 0;
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = inputViews[activeUploadSurface];
        if (pixelPerfectBorders) {
            const float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
            context->ClearRenderTargetView(backBufferRenderTarget, black);
        }
        HRESULT hr = videoContext->VideoProcessorBlt(
            processor, outputView, 0, 1, &stream);
        if (FAILED(hr)) return hr;
        hr = drawOverlayQuads();
        if (FAILED(hr)) return hr;
        const bool vsync =
            g_settings.presentationMode == PresentationMode::VSync;
        const UINT syncInterval = vsync ? 1u : 0u;
        const UINT flags = !vsync && allowTearing
                               ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        hr = swapChain->Present(syncInterval, flags);
        if (hr == DXGI_STATUS_OCCLUDED) {
            occluded = true;
            if (!occlusionLogged) {
                fwprintf(stderr,
                         L"[display-event] swapchain occluded; uptime=%llu ms "
                         L"present-mode=%s\n",
                         static_cast<unsigned long long>(GetTickCount64()),
                         vsync ? L"VSync" :
                             (allowTearing ? L"Tearing" : L"Immediate"));
                occlusionLogged = true;
            }
            nextOcclusionTestMs = GetTickCount64() + 50;
        }
        return hr;
    }
};

// Audio-only mode deliberately builds a graph with no video pin, renderer,
// swapchain, or Media Foundation decoder. It reuses the same exact PCM/float
// negotiation and callback path as the normal single graph.
static bool AudioOnlyCaptureLoop() {
    g_captureFailureHr.store(S_OK, std::memory_order_release);
    g_captureAudioAvailable.store(false, std::memory_order_release);
    g_directVideoActive.store(false, std::memory_order_release);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHr(L"CoInitializeEx(audio-only capture)", hr);
        g_captureFailureHr.store(hr, std::memory_order_release);
        return false;
    }

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

    llcv::capture::DirectShowGraphResources resources;
    auto*& graph = resources.graph;
    auto*& control = resources.control;
    auto*& mediaFilter = resources.mediaFilter;
    auto*& capture = resources.capture;
    auto*& audioCapture = resources.audioCapture;
    auto*& audioPin = resources.audioPin;
    auto*& audioGrabberFilter = resources.audioGrabberFilter;
    auto*& audioGrabber = resources.audioGrabber;
    auto*& audioNullRenderer = resources.audioNullRenderer;
    auto*& audioGrabberIn = resources.audioGrabberInput;
    auto*& audioGrabberOut = resources.audioGrabberOutput;
    auto*& audioNullIn = resources.audioNullInput;
    auto*& audioCallback = resources.audioCallback;
    auto*& selectedAudioType = resources.selectedAudioType;
    llcv::capture_audio::Format selectedAudioFormat{};
    bool initialized = false;
    const wchar_t* initializationStage = L"create audio-only DirectShow graph";

    do {
        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&graph));
        if (FAILED(hr)) break;
        hr = graph->QueryInterface(IID_PPV_ARGS(&control));
        if (FAILED(hr)) break;
        graph->QueryInterface(IID_PPV_ARGS(&mediaFilter));
        if (mediaFilter) mediaFilter->SetSyncSource(nullptr);

        g_activeCaptureAudioDeviceName.clear();
        if (g_settings.captureAudioDeviceId.empty()) {
            initializationStage = L"find selected capture device audio pin";
            hr = FindCaptureFilter(g_settings.captureDeviceId, &capture,
                                   &g_activeCaptureDeviceName);
            if (FAILED(hr)) break;
            hr = graph->AddFilter(capture, L"Selected Capture Device");
            if (FAILED(hr)) break;
            g_activeCaptureAudioDeviceName = g_activeCaptureDeviceName;
            hr = FindOutputPinByName(capture, kAudioPinName, &audioPin);
            if (FAILED(hr)) {
                hr = FindOutputPinByMajorType(capture, MEDIATYPE_Audio,
                                              &audioPin);
            }
        } else {
            g_activeCaptureDeviceName = L"(audio-only)";
            hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }

        if (FAILED(hr)) {
            initializationStage = L"find selected capture audio filter";
            hr = FindCaptureAudioFilter(g_settings.captureAudioDeviceId,
                                        g_activeCaptureDeviceName,
                                        &audioCapture,
                                        &g_activeCaptureAudioDeviceName);
            if (FAILED(hr)) break;
            hr = graph->AddFilter(audioCapture,
                                  L"Selected Capture Audio Device");
            if (FAILED(hr)) break;
            hr = FindOutputPinByMajorType(audioCapture, MEDIATYPE_Audio,
                                          &audioPin);
        }
        if (FAILED(hr)) break;
        g_captureAudioAvailable.store(true, std::memory_order_release);

        initializationStage = L"negotiate supported capture audio format";
        llcv::capture_audio::Rejection rejection =
            llcv::capture_audio::Rejection::Malformed;
        selectedAudioType = llcv::capture_audio::SelectSupportedType(
            audioPin, selectedAudioFormat, &rejection);
        if (!selectedAudioType) {
            fwprintf(stderr, L"[audio] capture input rejected: %s\n",
                     llcv::capture_audio::DescribeRejection(rejection).c_str());
            hr = VFW_E_TYPE_NOT_ACCEPTED;
            break;
        }
        fwprintf(stderr, L"[audio] capture input: %s\n",
                 llcv::capture_audio::Describe(selectedAudioFormat).c_str());
        SuggestCaptureBuffer(audioPin, selectedAudioFormat.blockAlign);

        initializationStage = L"build audio-only sample path";
        hr = CoCreateInstance(kSampleGrabberClassId, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&audioGrabberFilter));
        if (FAILED(hr)) break;
        hr = graph->AddFilter(audioGrabberFilter, L"PCM Latest Audio");
        if (FAILED(hr)) break;
        hr = audioGrabberFilter->QueryInterface(
            __uuidof(ISampleGrabber),
            reinterpret_cast<void**>(&audioGrabber));
        if (FAILED(hr)) break;
        hr = audioGrabber->SetMediaType(selectedAudioType);
        if (FAILED(hr)) break;
        audioGrabber->SetOneShot(FALSE);
        audioGrabber->SetBufferSamples(FALSE);
        audioCallback = CreateAudioSampleCallback(selectedAudioFormat);
        hr = audioGrabber->SetCallback(audioCallback, 0);
        if (FAILED(hr)) break;

        hr = CoCreateInstance(kNullRendererClassId, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&audioNullRenderer));
        if (FAILED(hr)) break;
        hr = graph->AddFilter(audioNullRenderer, L"Audio Null Renderer");
        if (FAILED(hr)) break;
        if (FAILED(hr = GetFirstPin(audioGrabberFilter, PINDIR_INPUT,
                                    &audioGrabberIn))) break;
        if (FAILED(hr = GetFirstPin(audioGrabberFilter, PINDIR_OUTPUT,
                                    &audioGrabberOut))) break;
        if (FAILED(hr = GetFirstPin(audioNullRenderer, PINDIR_INPUT,
                                    &audioNullIn))) break;
        if (FAILED(hr = graph->ConnectDirect(audioPin, audioGrabberIn,
                                             selectedAudioType))) break;
        ReportConnectedAudioAllocator(audioGrabberIn,
                                      selectedAudioFormat.blockAlign);
        if (FAILED(hr = graph->Connect(audioGrabberOut, audioNullIn))) break;

        initializationStage = L"start audio-only capture graph";
        hr = control->Run();
        if (FAILED(hr)) break;
        initialized = true;
        fwprintf(stderr,
                 L"[capture] audio-only graph running: %s · %s\n",
                 g_activeCaptureAudioDeviceName.c_str(),
                 llcv::capture_audio::Describe(selectedAudioFormat).c_str());
        while (g_running.load(std::memory_order_acquire)) Sleep(100);
        control->Stop();
    } while (false);

    if (!initialized) {
        const HRESULT failure = FAILED(hr) ? hr : E_FAIL;
        g_captureFailureHr.store(failure, std::memory_order_release);
        fwprintf(stderr, L"[capture] initialization stage: %s\n",
                 initializationStage);
        LogHr(L"Audio-only capture graph initialization", failure);
        LogFilterPins(capture, L"audio-only capture filter");
        if (audioCapture && audioCapture != capture) {
            LogFilterPins(audioCapture, L"separate capture audio filter");
        }
    }
    resources.Reset();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
    return initialized;
}

static bool UnifiedCaptureRenderLoop(HWND host) {
    const auto& preset = CurrentVideoPreset();
    g_captureFailureHr.store(S_OK, std::memory_order_release);
    g_captureAudioAvailable.store(false, std::memory_order_release);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHr(L"CoInitializeEx(direct video)", hr);
        g_captureFailureHr.store(hr, std::memory_order_release);
        return false;
    }

    DWORD videoTaskIndex = 0;
    HANDLE videoMmcss =
        AvSetMmThreadCharacteristicsW(L"Playback", &videoTaskIndex);
    if (videoMmcss) {
        AvSetMmThreadPriority(videoMmcss, AVRT_PRIORITY_HIGH);
    }

    llcv::capture::DirectShowGraphResources resources;
    auto*& graph = resources.graph;
    auto*& control = resources.control;
    auto*& mediaFilter = resources.mediaFilter;
    auto*& capture = resources.capture;
    auto*& audioCapture = resources.audioCapture;
    auto*& videoPin = resources.videoPin;
    auto*& audioPin = resources.audioPin;
    auto*& grabberFilter = resources.videoGrabberFilter;
    auto*& grabber = resources.videoGrabber;
    auto*& nullRenderer = resources.videoNullRenderer;
    auto*& grabberIn = resources.videoGrabberInput;
    auto*& grabberOut = resources.videoGrabberOutput;
    auto*& nullIn = resources.videoNullInput;
    auto*& callback = resources.videoCallback;
    auto*& audioGrabberFilter = resources.audioGrabberFilter;
    auto*& audioGrabber = resources.audioGrabber;
    auto*& audioNullRenderer = resources.audioNullRenderer;
    auto*& audioGrabberIn = resources.audioGrabberInput;
    auto*& audioGrabberOut = resources.audioGrabberOutput;
    auto*& audioNullIn = resources.audioNullInput;
    auto*& audioCallback = resources.audioCallback;
    auto*& activeVideoType = resources.activeVideoType;
    auto*& selectedAudioType = resources.selectedAudioType;
    llcv::capture_audio::Format selectedAudioFormat{};
    auto*& frameEvent = resources.frameEvent;
    bool initialized = false;
    const wchar_t* initializationStage = L"create DirectShow graph";
    DirectD3D11Renderer renderer;
    llcv::video::MjpegDecoder compressedDecoder;

    do {
        initializationStage = L"create DirectShow graph";
        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&graph));
        if (FAILED(hr)) break;
        hr = graph->QueryInterface(IID_PPV_ARGS(&control));
        if (FAILED(hr)) break;
        graph->QueryInterface(IID_PPV_ARGS(&mediaFilter));
        if (mediaFilter) mediaFilter->SetSyncSource(nullptr);

        initializationStage = L"find selected video capture filter";
        hr = FindCaptureFilter(g_settings.captureDeviceId, &capture,
                               &g_activeCaptureDeviceName);
        if (FAILED(hr)) break;
        initializationStage = L"add selected video capture filter";
        hr = graph->AddFilter(capture, L"Selected Capture Device");
        if (FAILED(hr)) break;
        initializationStage = L"find video output pin";
        hr = FindOutputPinByMajorType(capture, MEDIATYPE_Video, &videoPin);
        if (FAILED(hr)) break;

        DWORD imageBytes = 0;
        UINT32 stride = 0;
        int configuredFps = 0;
        VideoPixelFormat configuredFormat = VideoPixelFormat::Nv12;
        initializationStage = L"negotiate video resolution/FPS/pixel format";
        hr = ConfigureVideoPin(videoPin, preset.width, preset.height,
                               RequestedVideoFrameRate(),
                               g_settings.pixelFormat,
                               imageBytes, stride, configuredFps,
                               configuredFormat);
        if (FAILED(hr)) {
            LogHr(L"ConfigureVideoPin(exact format)", hr);
            break;
        }
        initializationStage = L"read active video capture format";
        hr = GetActiveVideoPinFormat(videoPin, &activeVideoType);
        if (FAILED(hr) || !activeVideoType) break;
        const bool compressedVideo = IsCompressedVideoFormat(configuredFormat);
        const VideoPixelFormat rendererInputFormat = compressedVideo
            ? VideoPixelFormat::Nv12 : configuredFormat;
        g_activePixelFormat.store(static_cast<int>(configuredFormat),
                                  std::memory_order_release);

        DirectShowColorMetadata directShowColorInfo{};
        const bool colorMetadataRelevant =
            configuredFormat == VideoPixelFormat::P010 || compressedVideo;
        bool directShowColorMetadataDetected = colorMetadataRelevant &&
            ExtractVideoColorMetadata(activeVideoType,
                                           directShowColorInfo);
        if (colorMetadataRelevant && !directShowColorMetadataDetected) {
            // A few capture drivers return a plain VIDEOINFOHEADER from
            // GetFormat but retain the extended color information on the
            // matching stream-capability entry.  Check that entry before
            // falling back to the format-specific default.
            DirectShowColorMetadata capabilityColorInfo{};
            if (FindMatchingVideoColorMetadata(
                    videoPin, configuredFormat, preset.width, preset.height,
                    configuredFps, capabilityColorInfo)) {
                directShowColorInfo = capabilityColorInfo;
                directShowColorMetadataDetected = true;
            }
        }
        const bool hdrInputMetadataDetected =
            configuredFormat == VideoPixelFormat::P010 &&
            directShowColorMetadataDetected && directShowColorInfo.hdr10();
        bool hdrInputMetadataAvailable = hdrInputMetadataDetected;
        if (configuredFormat == VideoPixelFormat::P010) {
            LogDirectShowColorMetadata(L"P010 selected format",
                                       directShowColorInfo);
            if (g_settings.forceHdr10) {
                hdrInputMetadataAvailable = true;
                fwprintf(stderr,
                         L"[hdr] P010 HDR10 output forced by user; DirectShow "
                         L"metadata: %s.\n",
                         hdrInputMetadataDetected ? L"available"
                                                   : L"unavailable");
            } else {
                fwprintf(stderr,
                         L"[hdr] P010 prototype selected; HDR10 metadata: %s.\n",
                         hdrInputMetadataDetected ? L"available"
                                                   : L"unavailable");
            }
        }
        if (compressedVideo) {
            LogDirectShowColorMetadata(L"MJPEG selected format",
                                       directShowColorInfo);
        }

        g_videoConfiguredFps.store(configuredFps,
                                   std::memory_order_release);
        llcv::video_color::Configuration sdrColor{};
        if (compressedVideo) {
            initializationStage = L"initialize Media Foundation compressed decoder";
            hr = compressedDecoder.initialize(
                preset.width, preset.height, configuredFps, activeVideoType,
                directShowColorMetadataDetected ? &directShowColorInfo : nullptr,
                g_settings.mjpegColorOverride, LogModuleMessage);
            if (FAILED(hr)) {
                LogHr(L"Media Foundation compressed decoder", hr);
                break;
            }
            sdrColor = compressedDecoder.colorConfiguration();
        }
        initializationStage = L"initialize D3D11 video renderer";
        hr = renderer.initialize(host, preset.width, preset.height,
                                 configuredFps, rendererInputFormat,
                                 hdrInputMetadataAvailable, sdrColor);
        if (FAILED(hr)) {
            LogHr(L"DirectD3D11Renderer::initialize", hr);
            break;
        }
        UpdateConfiguredVideoTitle(host, configuredFps);

        frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!frameEvent) { hr = HRESULT_FROM_WIN32(GetLastError()); break; }
        llcv::capture::LatestVideoSample latest(
            compressedVideo ? 0 : imageBytes, frameEvent,
            {&g_osdTrackingStartMs, &g_videoCapturedFrames,
             &g_videoReplacedFrames});

        initializationStage = L"build video sample path";
        hr = CoCreateInstance(kSampleGrabberClassId, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&grabberFilter));
        if (FAILED(hr)) break;
        hr = graph->AddFilter(grabberFilter, L"Latest Video Frame");
        if (FAILED(hr)) break;
        hr = grabberFilter->QueryInterface(
            __uuidof(ISampleGrabber),
            reinterpret_cast<void**>(&grabber));
        if (FAILED(hr)) break;
        AM_MEDIA_TYPE requested{};
        requested.majortype = MEDIATYPE_Video;
        requested.subtype = activeVideoType->subtype;
        requested.formattype = GUID_NULL;
        hr = grabber->SetMediaType(&requested);
        if (FAILED(hr)) break;
        grabber->SetOneShot(FALSE);
        grabber->SetBufferSamples(FALSE);
        callback = new llcv::capture::VideoSampleGrabberCallback(&latest);
        hr = grabber->SetCallback(callback, 0);
        if (FAILED(hr)) break;

        hr = CoCreateInstance(kNullRendererClassId, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&nullRenderer));
        if (FAILED(hr)) break;
        hr = graph->AddFilter(nullRenderer, L"Video Null Renderer");
        if (FAILED(hr)) break;
        if (FAILED(hr = GetFirstPin(grabberFilter, PINDIR_INPUT,
                                    &grabberIn))) break;
        if (FAILED(hr = GetFirstPin(grabberFilter, PINDIR_OUTPUT,
                                    &grabberOut))) break;
        if (FAILED(hr = GetFirstPin(nullRenderer, PINDIR_INPUT,
                                    &nullIn))) break;
        if (FAILED(hr = graph->ConnectDirect(videoPin, grabberIn,
                                             nullptr))) break;
        // Some drivers expose color information only on the negotiated
        // connection type, not on IAMStreamConfig::GetFormat. Inspect the
        // Sample Grabber's connected type before starting the graph. For
        // MJPEG, rebuild the decoder from that definitive type so its output
        // metadata and DirectShow's extended-color flags can both participate
        // in the automatic matrix/range decision.
        if (colorMetadataRelevant) {
            AM_MEDIA_TYPE connectedVideoType{};
            const HRESULT connectedTypeHr =
                grabber->GetConnectedMediaType(&connectedVideoType);
            if (SUCCEEDED(connectedTypeHr)) {
                DirectShowColorMetadata connectedColorInfo{};
                const bool connectedColorDetected =
                    ExtractVideoColorMetadata(&connectedVideoType,
                                                   connectedColorInfo);
                if (configuredFormat == VideoPixelFormat::P010 &&
                    connectedColorDetected) {
                    LogDirectShowColorMetadata(L"P010 connected media type",
                                               connectedColorInfo);
                    if (!hdrInputMetadataAvailable &&
                        connectedColorInfo.hdr10()) {
                        hdrInputMetadataAvailable = true;
                        hr = renderer.initialize(
                            host, preset.width, preset.height, configuredFps,
                            rendererInputFormat, true);
                        if (FAILED(hr)) {
                            LogHr(L"DirectD3D11Renderer::initialize(HDR metadata)",
                                  hr);
                        }
                    }
                }
                if (SUCCEEDED(hr) && compressedVideo) {
                    DirectShowColorMetadata effectiveColorInfo =
                        directShowColorInfo;
                    MergeVideoColorMetadata(effectiveColorInfo,
                                                 connectedColorInfo);
                    if (connectedColorDetected) {
                        LogDirectShowColorMetadata(
                            L"MJPEG connected media type", connectedColorInfo);
                    }
                    LogDirectShowColorMetadata(L"MJPEG effective metadata",
                                               effectiveColorInfo);
                    initializationStage =
                        L"confirm Media Foundation MJPEG decoder color";
                    hr = compressedDecoder.initialize(
                        preset.width, preset.height, configuredFps,
                        &connectedVideoType,
                        effectiveColorInfo.present ? &effectiveColorInfo
                                                   : nullptr,
                        g_settings.mjpegColorOverride,
                        LogModuleMessage);
                    if (SUCCEEDED(hr)) {
                        const auto connectedColor =
                            compressedDecoder.colorConfiguration();
                        if (!(connectedColor == sdrColor)) {
                            sdrColor = connectedColor;
                            hr = renderer.initialize(
                                host, preset.width, preset.height,
                                configuredFps, rendererInputFormat,
                                hdrInputMetadataAvailable, sdrColor);
                        }
                    }
                    if (FAILED(hr)) {
                        LogHr(L"MJPEG connected-type color initialization", hr);
                    }
                }
            }
            FreeMediaType(connectedVideoType);
            if (FAILED(hr)) break;
        }
        if (FAILED(hr = graph->ConnectDirect(grabberOut, nullIn,
                                             nullptr))) break;
        // Prefer an audio pin on the selected video filter. Many USB UVC
        // capture devices instead expose their capture audio as a separate
        // DirectShow audio-input filter, which is added to this same graph.
        IBaseFilter* audioSource = capture;
        g_activeCaptureAudioDeviceName = g_activeCaptureDeviceName;
        initializationStage = L"find audio output pin on video capture filter";
        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        if (g_settings.captureAudioDeviceId.empty()) {
            hr = FindOutputPinByName(capture, kAudioPinName, &audioPin);
            if (FAILED(hr)) {
                hr = FindOutputPinByMajorType(capture, MEDIATYPE_Audio, &audioPin);
            }
        }
        if (FAILED(hr)) {
            initializationStage = g_settings.captureAudioDeviceId.empty()
                ? L"find matching separate capture audio filter"
                : L"find selected separate capture audio filter";
            hr = FindCaptureAudioFilter(g_settings.captureAudioDeviceId,
                                        g_activeCaptureDeviceName,
                                        &audioCapture,
                                        &g_activeCaptureAudioDeviceName);
            if (FAILED(hr)) break;
            initializationStage = L"add separate capture audio filter";
            hr = graph->AddFilter(audioCapture, L"Selected Capture Audio Device");
            if (FAILED(hr)) break;
            audioSource = audioCapture;
            initializationStage = L"find audio output pin on separate capture filter";
            hr = FindOutputPinByMajorType(audioSource, MEDIATYPE_Audio, &audioPin);
        }
        if (FAILED(hr)) break;
        g_captureAudioAvailable.store(true, std::memory_order_release);

        initializationStage = L"negotiate supported capture audio format";
        llcv::capture_audio::Rejection audioFormatRejection =
            llcv::capture_audio::Rejection::Malformed;
        selectedAudioType = llcv::capture_audio::SelectSupportedType(
            audioPin, selectedAudioFormat, &audioFormatRejection);
        if (!selectedAudioType) {
            fwprintf(stderr, L"[audio] capture input rejected: %s\n",
                     llcv::capture_audio::DescribeRejection(
                         audioFormatRejection).c_str());
            hr = VFW_E_TYPE_NOT_ACCEPTED;
            break;
        }
        fwprintf(stderr, L"[audio] capture input: %s\n",
                 llcv::capture_audio::Describe(selectedAudioFormat).c_str());
        SuggestCaptureBuffer(audioPin, selectedAudioFormat.blockAlign);

        initializationStage = L"build supported PCM/float audio sample path";
        hr = CoCreateInstance(kSampleGrabberClassId, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&audioGrabberFilter));
        if (FAILED(hr)) break;
        hr = graph->AddFilter(audioGrabberFilter, L"PCM Latest Audio");
        if (FAILED(hr)) break;
        hr = audioGrabberFilter->QueryInterface(
            __uuidof(ISampleGrabber),
            reinterpret_cast<void**>(&audioGrabber));
        if (FAILED(hr)) break;

        hr = audioGrabber->SetMediaType(selectedAudioType);
        if (FAILED(hr)) break;
        audioGrabber->SetOneShot(FALSE);
        audioGrabber->SetBufferSamples(FALSE);
        audioCallback = CreateAudioSampleCallback(selectedAudioFormat);
        hr = audioGrabber->SetCallback(audioCallback, 0);
        if (FAILED(hr)) break;

        hr = CoCreateInstance(kNullRendererClassId, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&audioNullRenderer));
        if (FAILED(hr)) break;
        hr = graph->AddFilter(audioNullRenderer, L"Audio Null Renderer");
        if (FAILED(hr)) break;
        if (FAILED(hr = GetFirstPin(audioGrabberFilter, PINDIR_INPUT,
                                    &audioGrabberIn))) break;
        if (FAILED(hr = GetFirstPin(audioGrabberFilter, PINDIR_OUTPUT,
                                    &audioGrabberOut))) break;
        if (FAILED(hr = GetFirstPin(audioNullRenderer, PINDIR_INPUT,
                                    &audioNullIn))) break;
        if (FAILED(hr = graph->ConnectDirect(audioPin, audioGrabberIn,
                                             selectedAudioType))) break;
        ReportConnectedAudioAllocator(audioGrabberIn,
                                      selectedAudioFormat.blockAlign);
        if (FAILED(hr = graph->Connect(audioGrabberOut, audioNullIn))) break;

        initializationStage = L"connect and start capture graph";
        hr = control->Run();
        if (FAILED(hr)) break;
        initialized = true;
        g_videoStride.store(stride, std::memory_order_release);
        const bool tearingActive = renderer.allowTearing &&
            g_settings.presentationMode == PresentationMode::AllowTearing;
        g_videoTearing.store(tearingActive, std::memory_order_release);
        fwprintf(stderr,
                 L"[capture] graph running: video %s · audio %s · %s %dx%d @ %d + "
                 L"%s, stride %u, frame bytes %lu, present %s\n",
                 g_activeCaptureDeviceName.c_str(),
                 g_activeCaptureAudioDeviceName.c_str(),
                 PixelFormatName(configuredFormat), preset.width,
                 preset.height, configuredFps,
                 llcv::capture_audio::Describe(selectedAudioFormat).c_str(),
                 stride, imageBytes,
                 g_settings.presentationMode == PresentationMode::VSync
                     ? L"VSync" : tearingActive ? L"Tearing" : L"Immediate");
        fwprintf(stderr,
                 L"[video] upload ring: %u %s GPU surfaces; update: %s\n",
                 DirectD3D11Renderer::kUploadSurfaceCount,
                 PixelFormatName(rendererInputFormat),
                 renderer.discardUpdateAvailable
                     ? L"D3D11.1 discard" : L"D3D11 fallback");
        if (compressedVideo) {
            fwprintf(stderr,
                     L"[video] experimental compressed path: %s capture -> "
                     L"Media Foundation NV12 decode -> D3D11; latest frame only.\n",
                     PixelFormatName(configuredFormat));
        }

        auto recoverRenderer = [&](const wchar_t* failedStage,
                                   HRESULT failure) {
            LogHr(failedStage, failure);
            const HRESULT removedReason = renderer.deviceRemovedReason();
            LogD3DFailureEvent(failure, removedReason);
            if (FAILED(removedReason)) {
                LogHr(L"ID3D11Device::GetDeviceRemovedReason",
                      removedReason);
            }
            g_directVideoActive.store(false, std::memory_order_release);

            constexpr DWORD retryDelaysMs[] = {0, 100, 250};
            for (size_t attempt = 0;
                 attempt < ARRAYSIZE(retryDelaysMs) && g_running.load();
                 ++attempt) {
                if (retryDelaysMs[attempt]) Sleep(retryDelaysMs[attempt]);
                const HRESULT recoveryHr = renderer.initialize(
                    host, preset.width, preset.height, configuredFps,
                    rendererInputFormat, hdrInputMetadataAvailable, sdrColor);
                if (SUCCEEDED(recoveryHr)) {
                    const bool recoveredTearing = renderer.allowTearing &&
                        g_settings.presentationMode ==
                            PresentationMode::AllowTearing;
                    g_videoTearing.store(recoveredTearing,
                                         std::memory_order_release);
                    g_overlayGeneration.fetch_add(
                        1, std::memory_order_relaxed);
                    fwprintf(stderr,
                             L"[video] D3D11 renderer recovered on attempt "
                             L"%zu; capture graph and WASAPI remained active.\n",
                             attempt + 1);
                    return true;
                }
                LogHr(L"D3D11 renderer recovery", recoveryHr);
                hr = recoveryHr;
            }
            return false;
        };

        int64_t arrivalUs = 0;
        bool presentedAnyFrame = false;
        const auto firstFrameDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (g_running.load()) {
            if (WaitForSingleObject(frameEvent, 100) != WAIT_OBJECT_0) {
                if (!presentedAnyFrame &&
                    std::chrono::steady_clock::now() >= firstFrameDeadline) {
                    fwprintf(stderr,
                             L"[video] direct path received no %s frame "
                             L"within 3 seconds.\n",
                             PixelFormatName(configuredFormat));
                    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                    initialized = false;
                    break;
                }
                continue;
            }
            IMediaSample* videoSample = latest.TakeLatest(arrivalUs);
            if (!videoSample) continue;
            if (renderer.outputConfigurationChanged()) {
                hr = renderer.initialize(host, preset.width, preset.height,
                                         configuredFps,
                                         rendererInputFormat,
                                         hdrInputMetadataAvailable, sdrColor);
                if (FAILED(hr)) {
                    if (recoverRenderer(L"D3D11 output resize", hr)) {
                        hr = S_OK;
                    } else {
                        videoSample->Release();
                        initialized = false;
                        break;
                    }
                }
                g_overlayGeneration.fetch_add(1,
                                               std::memory_order_relaxed);
            }
            BYTE* sampleData = nullptr;
            IMFMediaBuffer* decodedBuffer = nullptr;
            if (compressedVideo) {
                hr = compressedDecoder.decode(videoSample, &decodedBuffer);
                if (SUCCEEDED(hr)) {
                    const auto decodedColor =
                        compressedDecoder.colorConfiguration();
                    if (!(decodedColor == sdrColor)) {
                        sdrColor = decodedColor;
                        fwprintf(stderr,
                                 L"[video] MJPEG decoder output color changed; "
                                 L"reconfiguring D3D11 renderer.\n");
                        hr = renderer.initialize(
                            host, preset.width, preset.height, configuredFps,
                            rendererInputFormat, hdrInputMetadataAvailable,
                            sdrColor);
                    }
                }
                if (SUCCEEDED(hr) && decodedBuffer) {
                    DWORD maximum = 0;
                    DWORD current = 0;
                    hr = decodedBuffer->Lock(&sampleData, &maximum, &current);
                    if (SUCCEEDED(hr) && sampleData && current != 0 &&
                        compressedDecoder.stride() > 0) {
                        renderer.upload(sampleData,
                                        static_cast<UINT32>(
                                            compressedDecoder.stride()));
                    } else if (SUCCEEDED(hr)) {
                        hr = E_FAIL;
                    }
                    if (sampleData) decodedBuffer->Unlock();
                }
            } else {
                hr = videoSample->GetPointer(&sampleData);
                if (SUCCEEDED(hr) && sampleData) {
                    renderer.upload(sampleData, stride);
                }
            }
            videoSample->Release();
            const bool hasDecodedFrame = decodedBuffer != nullptr;
            SafeRelease(decodedBuffer);
            if (FAILED(hr)) {
                LogHr(compressedVideo ? L"Media Foundation video decode/transfer"
                                      : L"D3D11 video transfer", hr);
                initialized = false;
                break;
            }
            // A compressed decoder can retain an access unit while waiting for
            // a complete picture. There is nothing to present until it emits
            // an NV12 frame; the next capture callback still replaces stale
            // compressed input rather than extending an application queue.
            if (compressedVideo && !hasDecodedFrame) continue;
            hr = renderer.presentUploaded();
            if (hr == DXGI_STATUS_OCCLUDED) {
                continue;
            }
            if (FAILED(hr)) {
                if (recoverRenderer(L"D3D11 direct Present", hr)) {
                    hr = S_OK;
                    continue;
                }
                initialized = false;
                break;
            }
            const int64_t presentedUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            if (OsdTrackingActive()) {
                g_videoAppLatencyUs.store(presentedUs - arrivalUs,
                                          std::memory_order_release);
                g_videoPresentedFrames.fetch_add(1,
                                                 std::memory_order_relaxed);
            }
            g_directVideoActive.store(true, std::memory_order_release);
            presentedAnyFrame = true;
        }
        control->Stop();
    } while (false);

    if (!initialized) {
        const HRESULT failure = FAILED(hr) ? hr : E_FAIL;
        g_captureFailureHr.store(failure, std::memory_order_release);
        fwprintf(stderr, L"[capture] initialization stage: %s\n",
                 initializationStage);
        LogHr(L"Single capture graph initialization", failure);
        LogFilterPins(capture, L"video capture filter");
        if (audioCapture && audioCapture != capture) {
            LogFilterPins(audioCapture, L"separate capture audio filter");
        }
    }
    resources.Reset();
    renderer.reset();
    compressedDecoder.reset();
    if (videoMmcss) AvRevertMmThreadCharacteristics(videoMmcss);
    CoUninitialize();
    return initialized;
}

// -----------------------------------------------------------------------------
// Startup settings dialog
// -----------------------------------------------------------------------------

constexpr int IDC_SETTINGS_AUDIO = 2001;
constexpr int IDC_SETTINGS_VIDEO = 2002;
constexpr int IDC_SETTINGS_PIXEL = 2003;
constexpr int IDC_SETTINGS_START = 2004;
constexpr int IDC_SETTINGS_CANCEL = 2005;
constexpr int IDC_SETTINGS_BUFFER = 2006;
constexpr int IDC_SETTINGS_BORDERLESS = 2007;
constexpr int IDC_SETTINGS_AUDIO_STATUS = 2008;
constexpr int IDC_SETTINGS_PRESENTATION = 2009;
constexpr int IDC_SETTINGS_VOLUME_HUD = 2010;
constexpr int IDC_SETTINGS_DRIFT = 2011;
constexpr int IDC_SETTINGS_WINDOW_SNAP = 2012;
constexpr int IDC_SETTINGS_RELATIVE_SIZE = 2013;
constexpr int IDC_SETTINGS_DRIFT_HELP = 2014;
constexpr int IDC_SETTINGS_PCM_QUEUE = 2015;
constexpr int IDC_SETTINGS_AUDIO_OUTPUT = 2016;
constexpr int IDC_SETTINGS_CAPTURE_DEVICE = 2017;
constexpr int IDC_SETTINGS_PIXEL_FORMAT = 2018;
constexpr int IDC_SETTINGS_SAVE_LOG = 2019;
constexpr int IDC_SETTINGS_FRAME_RATE = 2020;
constexpr int IDC_SETTINGS_MUTE_BACKGROUND = 2021;
constexpr int IDC_SETTINGS_PRESENTATION_HELP = 2022;
constexpr int IDC_SETTINGS_PCM_QUEUE_HELP = 2023;
constexpr int IDC_SETTINGS_LANGUAGE = 2024;
constexpr int IDC_SETTINGS_SHOW_CONSOLE = 2025;
constexpr int IDC_SETTINGS_CAPTURE_AUDIO_DEVICE = 2026;
constexpr int IDC_SETTINGS_SCALING = 2027;
constexpr int IDC_SETTINGS_SKIP_STARTUP = 2028;
constexpr int IDC_SETTINGS_VOLUME_BOOST = 2029;
constexpr int IDC_SETTINGS_VOLUME_BOOST_HELP = 2030;
constexpr int IDC_SETTINGS_AUDIO_ONLY = 2032;
constexpr int IDC_SETTINGS_FORCE_HDR10 = 2033;
constexpr int IDC_SETTINGS_FORCE_HDR10_HELP = 2034;
constexpr int IDC_SETTINGS_UPDATE_CHECK = 2035;
constexpr int IDC_SETTINGS_EXCLUSIVE_TEST = 2036;
constexpr int IDC_SETTINGS_TAB = 2037;
constexpr int IDC_SETTINGS_UPDATE_NOW = 2038;
constexpr int IDC_SETTINGS_OPEN_LOG_FOLDER = 2039;
constexpr int IDC_SETTINGS_FULLSCREEN_CURSOR = 2040;
constexpr int IDC_SETTINGS_MJPEG_COLOR = 2041;
constexpr int IDC_SETTINGS_MJPEG_COLOR_HELP = 2042;
constexpr UINT WM_AUDIOCLIENT3_PROBE_COMPLETE = WM_APP + 73;
constexpr UINT WM_SETTINGS_TOOLTIP_SHOW = WM_APP + 74;
constexpr UINT WM_SETTINGS_TOOLTIP_HIDE = WM_APP + 75;
constexpr UINT WM_CAPTURE_AUDIO_PROBE_COMPLETE = WM_APP + 76;
constexpr UINT WM_UPDATE_CHECK_COMPLETE = WM_APP + 77;
constexpr UINT WM_SETTINGS_UPDATE_CHECK_COMPLETE = WM_APP + 78;
constexpr UINT WM_EXCLUSIVE_ENDPOINT_PROBE_COMPLETE = WM_APP + 79;
constexpr UINT WM_EXCLUSIVE_SCAN_COMPLETE = WM_APP + 80;

enum class SettingsTab : int {
    Audio = 0,
    VideoWindow = 1,
    GuideDiagnostics = 2,
    Updates = 3,
};

using UpdateCheckResult = llcv::update::CheckResult;

struct SettingsDialogState {
    HWND tabControl = nullptr;
    HWND guideText = nullptr;
    HWND guideShortcutsTitle = nullptr;
    HWND guideDiagnosticsTitle = nullptr;
    HWND guideDiagnosticsText = nullptr;
    HWND guideLogFolderButton = nullptr;
    HWND updateTitle = nullptr;
    HWND updateText = nullptr;
    HWND updateNowButton = nullptr;
    HWND updateStatus = nullptr;
    HWND audioOutputSection = nullptr;
    HWND audioPlaybackSection = nullptr;
    HWND audioStabilitySection = nullptr;
    HWND videoCaptureSection = nullptr;
    HWND videoDisplaySection = nullptr;
    HWND videoWindowSection = nullptr;
    HWND languageLabel = nullptr;
    HWND languageCombo = nullptr;
    HWND audioLabel = nullptr;
    HWND bufferLabel = nullptr;
    HWND audioOutputLabel = nullptr;
    HWND volumeHudLabel = nullptr;
    HWND volumeBoostCheck = nullptr;
    HWND volumeBoostHelp = nullptr;
    HWND driftLabel = nullptr;
    HWND driftHelp = nullptr;
    HWND pcmQueueLabel = nullptr;
    HWND pcmQueueHelp = nullptr;
    HWND presentationLabel = nullptr;
    HWND presentationHelp = nullptr;
    HWND fullscreenCursorLabel = nullptr;
    HWND fullscreenCursorHint = nullptr;
    HWND scalingLabel = nullptr;
    HWND videoLabel = nullptr;
    HWND captureDeviceLabel = nullptr;
    HWND captureAudioDeviceLabel = nullptr;
    HWND captureAudioStatus = nullptr;
    HWND pixelFormatLabel = nullptr;
    HWND frameRateLabel = nullptr;
    HWND videoCapabilityStatus = nullptr;
    HWND audioCombo = nullptr;
    HWND bufferCombo = nullptr;
    HWND audioOutputCombo = nullptr;
    HWND volumeHudCombo = nullptr;
    HWND muteBackgroundCheck = nullptr;
    HWND audioOnlyCheck = nullptr;
    HWND forceHdr10Check = nullptr;
    HWND forceHdr10Help = nullptr;
    HWND mjpegColorLabel = nullptr;
    HWND mjpegColorCombo = nullptr;
    HWND mjpegColorHelp = nullptr;
    HWND driftCombo = nullptr;
    HWND pcmQueueCombo = nullptr;
    HWND audioStatus = nullptr;
    HWND exclusiveTestButton = nullptr;
    HWND presentationCombo = nullptr;
    HWND fullscreenCursorCombo = nullptr;
    HWND scalingCombo = nullptr;
    HWND videoCombo = nullptr;
    HWND captureDeviceCombo = nullptr;
    HWND captureAudioDeviceCombo = nullptr;
    HWND pixelFormatCombo = nullptr;
    HWND frameRateCombo = nullptr;
    HWND pixelCheck = nullptr;
    HWND relativeSizeCheck = nullptr;
    HWND relativeSizeWarning = nullptr;
    HWND borderlessCheck = nullptr;
    HWND windowSnapCheck = nullptr;
    HWND saveLogCheck = nullptr;
    HWND showConsoleCheck = nullptr;
    HWND skipStartupCheck = nullptr;
    HWND skipStartupHint = nullptr;
    HWND checkForUpdatesCheck = nullptr;
    HWND versionWatermark = nullptr;
    HWND startButton = nullptr;
    HWND cancelButton = nullptr;
    HWND tooltipWindow = nullptr;
    HWND activeTooltipTarget = nullptr;
    std::vector<HFONT> uiFonts;
    std::thread probeThread;
    std::thread updateCheckThread;
    std::thread exclusiveProbeThread;
    std::thread captureAudioProbeThread;
    std::atomic<bool> probeReady{false};
    std::atomic<bool> updateCheckStop{false};
    std::atomic<bool> updateCheckRunning{false};
    std::atomic<bool> exclusiveProbeStop{false};
    std::atomic<bool> exclusiveScanRunning{false};
    std::atomic<bool> captureAudioProbeReady{false};
    AudioClient3Support probe{};
    InternalCaptureAudioProbe captureAudioProbe{};
    std::vector<UINT32> sharedPeriodChoices;
    std::vector<CaptureDeviceInfo> captureDevices;
    std::vector<CaptureDeviceInfo> captureAudioDevices;
    std::vector<AudioEndpointInfo> audioEndpoints;
    std::vector<ExclusiveEndpointVerification> exclusiveEndpointResults;
    std::vector<llcv::asio::DriverInfo> asioDrivers;
    std::vector<PixelFormatSupport> pixelFormats;
    VideoPreset initialVideoPreset = VideoPreset::R1920x1080;
    HMONITOR viewerMonitor = nullptr;
    UINT32 selectedSharedPeriodFrames = 0;
    int selectedBufferMs = kRecommendedWasapiBufferMs;
    std::wstring exclusiveVerifiedEndpointId;
    size_t exclusiveScanCompleted = 0;
    int exclusiveVerifiedBufferMs = 0;
    bool bufferItemsAreSharedFrames = false;
    bool asioAvailable = false;
    SettingsTab activeTab = SettingsTab::Audio;
    bool accepted = false;
};

static int SettingsPixels(int dips, UINT dpi) {
    return MulDiv(dips, dpi ? dpi : USER_DEFAULT_SCREEN_DPI,
                  USER_DEFAULT_SCREEN_DPI);
}

static constexpr int kSettingsClientWidthDip = 950;
static constexpr int kSettingsTabbedClientHeightDip = 630;

static int SettingsClientHeightDip(const SettingsDialogState* state) {
    (void)state;
    return kSettingsTabbedClientHeightDip;
}

static SIZE SettingsDialogOuterSize(HWND hwnd, UINT dpi,
                                    const SettingsDialogState* state) {
    RECT rect{0, 0, SettingsPixels(kSettingsClientWidthDip, dpi),
              SettingsPixels(SettingsClientHeightDip(state), dpi)};
    const DWORD style =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if (!AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi)) {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

static void PlaceSettingsControl(HWND control, int x, int y, int width,
                                 int height, UINT dpi) {
    if (!control) return;
    SetWindowPos(control, nullptr, SettingsPixels(x, dpi),
                 SettingsPixels(y, dpi), SettingsPixels(width, dpi),
                 SettingsPixels(height, dpi),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

static BOOL CALLBACK SetSettingsChildFont(HWND child, LPARAM fontValue) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(fontValue), FALSE);
    return TRUE;
}

static void ApplySettingsFont(SettingsDialogState* state, HWND hwnd,
                              UINT dpi) {
    if (!state || !hwnd) return;
    HFONT font = CreateFontW(
        -MulDiv(9, dpi ? dpi : USER_DEFAULT_SCREEN_DPI, 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!font) return;
    state->uiFonts.push_back(font);
    EnumChildWindows(hwnd, SetSettingsChildFont,
                     reinterpret_cast<LPARAM>(font));

    // Section labels are deliberately subtle, but bold enough to make the
    // vertically grouped audio controls scannable at a glance.
    HFONT sectionFont = CreateFontW(
        -MulDiv(9, dpi ? dpi : USER_DEFAULT_SCREEN_DPI, 72),
        0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!sectionFont) return;
    state->uiFonts.push_back(sectionFont);
    for (HWND control : {state->audioOutputSection,
                         state->audioPlaybackSection,
                         state->audioStabilitySection,
                         state->videoCaptureSection,
                         state->videoDisplaySection,
                         state->videoWindowSection,
                         state->guideShortcutsTitle,
                         state->guideDiagnosticsTitle}) {
        if (control) {
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(sectionFont), FALSE);
        }
    }

}

// Checkbox captions vary substantially between Korean and English.  Measure
// the actual current UI font so a neighbouring help button stays attached to
// its option at every DPI instead of relying on a fragile hard-coded x value.
static int SettingsCheckboxWidthDip(HWND checkbox, UINT dpi) {
    if (!checkbox) return 250;
    wchar_t text[512]{};
    GetWindowTextW(checkbox, text, ARRAYSIZE(text));
    HDC hdc = GetDC(checkbox);
    if (!hdc) return 250;
    const HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(checkbox, WM_GETFONT, 0, 0));
    const HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
    SIZE size{};
    GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &size);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(checkbox, hdc);
    const int textWidthDip = MulDiv(
        size.cx, USER_DEFAULT_SCREEN_DPI,
        dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
    // Checkbox glyph plus caption.  Keeping the HWND no wider than this is
    // important: a wide checkbox would overlap a nearby help button and
    // steal its clicks even when the button looks visually separate.
    return std::min(18 + textWidthDip, 430);
}

static void LayoutSettingsControls(SettingsDialogState* state, UINT dpi) {
    if (!state) return;
    // The dialog is deliberately tabbed rather than expanded vertically. This
    // keeps the startup view small while leaving every setting reachable.
    PlaceSettingsControl(state->tabControl, 24, 16, 901, 31, dpi);

    // Global preferences remain fixed below every tab, especially direct-start.
    // Leave a clear visual break after the PCM-buffer group. Language and
    // quick-start are application preferences, not audio-tuning controls.
    PlaceSettingsControl(state->languageLabel, 24, 500, 160, 24, dpi);
    PlaceSettingsControl(state->languageCombo, 195, 496, 280, 120, dpi);
    PlaceSettingsControl(state->skipStartupCheck, 24, 540, 451, 28, dpi);
    PlaceSettingsControl(state->skipStartupHint, 44, 568, 500, 22, dpi);
    PlaceSettingsControl(state->versionWatermark, 24, 602, 260, 20, dpi);
    PlaceSettingsControl(state->startButton, 745, 568, 80, 30, dpi);
    PlaceSettingsControl(state->cancelButton, 835, 568, 80, 30, dpi);

    // Audio tab: output choice first, then everyday playback controls, then
    // the latency/stability controls that usually only need adjustment after
    // diagnostics report a problem.
    PlaceSettingsControl(state->audioOutputSection, 34, 58, 200, 20, dpi);
    PlaceSettingsControl(state->audioLabel, 34, 80, 160, 24, dpi);
    PlaceSettingsControl(state->audioCombo, 205, 76, 360, 120, dpi);
    // Exclusive endpoint verification configures the selected output mode,
    // so keep its explicit recheck action beside that mode instead of making
    // it look like a generic status-row operation.
    // Match the visible combobox field (rather than its dropdown height) so
    // the recheck action reads as part of the output-mode row.
    PlaceSettingsControl(state->exclusiveTestButton, 575, 76, 185, 22, dpi);
    PlaceSettingsControl(state->audioOutputLabel, 34, 116, 160, 24, dpi);
    PlaceSettingsControl(state->audioOutputCombo, 205, 112, 680, 220, dpi);
    PlaceSettingsControl(state->bufferLabel, 34, 152, 160, 24, dpi);
    PlaceSettingsControl(state->bufferCombo, 205, 148, 280, 180, dpi);
    PlaceSettingsControl(state->audioStatus, 34, 188, 580, 24, dpi);
    PlaceSettingsControl(state->audioPlaybackSection, 34, 222, 250, 20, dpi);
    PlaceSettingsControl(state->volumeHudLabel, 34, 246, 160, 24, dpi);
    PlaceSettingsControl(state->volumeHudCombo, 205, 242, 280, 160, dpi);
    const int volumeBoostWidth = SettingsCheckboxWidthDip(
        state->volumeBoostCheck, dpi);
    PlaceSettingsControl(state->volumeBoostCheck, 34, 282, volumeBoostWidth,
                         28, dpi);
    PlaceSettingsControl(state->volumeBoostHelp,
                         34 + volumeBoostWidth + 10,
                         284, 24, 24, dpi);
    PlaceSettingsControl(state->muteBackgroundCheck, 34, 318, 500, 28, dpi);
    PlaceSettingsControl(state->audioOnlyCheck, 34, 354, 500, 28, dpi);
    PlaceSettingsControl(state->audioStabilitySection, 34, 392, 250, 20, dpi);
    PlaceSettingsControl(state->driftLabel, 34, 416, 160, 24, dpi);
    PlaceSettingsControl(state->driftHelp, 170, 412, 24, 24, dpi);
    PlaceSettingsControl(state->driftCombo, 205, 412, 360, 120, dpi);
    PlaceSettingsControl(state->pcmQueueLabel, 34, 452, 160, 24, dpi);
    PlaceSettingsControl(state->pcmQueueHelp, 170, 448, 24, 24, dpi);
    PlaceSettingsControl(state->pcmQueueCombo, 205, 448, 280, 140, dpi);

    // Video & window tab: capture format on the left; how it is shown and
    // how the viewer window behaves on the right. HDR stays last because it
    // is an experimental override rather than a normal display choice.
    // Leave a real breathing gap below each section heading.  The previous
    // first-row placement was inherited from the no-heading layout and made
    // headings read like part of the option label.
    PlaceSettingsControl(state->captureDeviceLabel, 34, 84, 140, 24, dpi);
    PlaceSettingsControl(state->videoCaptureSection, 34, 58, 140, 20, dpi);
    PlaceSettingsControl(state->captureDeviceCombo, 190, 80, 270, 220, dpi);
    PlaceSettingsControl(state->captureAudioDeviceLabel, 34, 124, 140, 24, dpi);
    PlaceSettingsControl(state->captureAudioDeviceCombo, 190, 120, 270, 220, dpi);
    PlaceSettingsControl(state->captureAudioStatus, 190, 124, 300, 24, dpi);
    PlaceSettingsControl(state->videoLabel, 34, 164, 140, 24, dpi);
    PlaceSettingsControl(state->videoCombo, 190, 160, 270, 120, dpi);
    PlaceSettingsControl(state->pixelFormatLabel, 34, 204, 140, 24, dpi);
    PlaceSettingsControl(state->pixelFormatCombo, 190, 200, 270, 160, dpi);
    PlaceSettingsControl(state->frameRateLabel, 34, 244, 140, 24, dpi);
    PlaceSettingsControl(state->frameRateCombo, 190, 240, 270, 200, dpi);
    PlaceSettingsControl(state->videoCapabilityStatus, 34, 278, 430, 90, dpi);
    PlaceSettingsControl(state->presentationLabel, 505, 84, 95, 24, dpi);
    PlaceSettingsControl(state->videoDisplaySection, 505, 58, 140, 20, dpi);
    PlaceSettingsControl(state->presentationHelp, 604, 80, 24, 24, dpi);
    PlaceSettingsControl(state->presentationCombo, 630, 80, 255, 120, dpi);
    PlaceSettingsControl(state->pixelCheck, 505, 120, 380, 28, dpi);
    PlaceSettingsControl(state->scalingLabel, 505, 160, 120, 24, dpi);
    PlaceSettingsControl(state->scalingCombo, 630, 156, 255, 120, dpi);
    // The controls from here onward affect the viewer window itself rather
    // than captured video format or rendering policy.  When Pixel-perfect is
    // enabled the scaling row is hidden, so pull this section up by one grid
    // row instead of leaving an arbitrary empty gap.
    const bool pixelPerfect = state->pixelCheck &&
        SendMessageW(state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const int windowSectionY = pixelPerfect ? 168 : 204;
    const int windowOptionY = windowSectionY + 24;
    PlaceSettingsControl(state->videoWindowSection, 505, windowSectionY,
                         140, 20, dpi);
    PlaceSettingsControl(state->relativeSizeCheck, 505, windowOptionY,
                         400, 28, dpi);
    PlaceSettingsControl(state->relativeSizeWarning, 525, windowOptionY + 28,
                         370, 28, dpi);
    PlaceSettingsControl(state->borderlessCheck, 505, windowOptionY + 68,
                         400, 28, dpi);
    PlaceSettingsControl(state->windowSnapCheck, 505, windowOptionY + 104,
                         400, 28, dpi);
    PlaceSettingsControl(state->fullscreenCursorLabel, 505,
                         windowOptionY + 144, 120, 24, dpi);
    PlaceSettingsControl(state->fullscreenCursorCombo, 630,
                         windowOptionY + 140, 255, 120, dpi);
    PlaceSettingsControl(state->fullscreenCursorHint, 630,
                         windowOptionY + 172, 255, 24, dpi);
    // The combo's configured height includes its drop-down list rectangle.
    // Keep the adjacent hint above that sibling after every relayout so a
    // Pixel-perfect redraw cannot paint over it while the list is closed.
    if (state->fullscreenCursorHint) {
        SetWindowPos(state->fullscreenCursorHint, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // P010 and MJPEG use the same final capture-format row. Only the control
    // relevant to the selected input format is made visible.
    PlaceSettingsControl(state->forceHdr10Check, 34, 374, 360, 28, dpi);
    PlaceSettingsControl(state->forceHdr10Help, 402, 370, 24, 24, dpi);
    PlaceSettingsControl(state->mjpegColorLabel, 34, 374, 140, 24, dpi);
    PlaceSettingsControl(state->mjpegColorCombo, 190, 370, 240, 150, dpi);
    PlaceSettingsControl(state->mjpegColorHelp, 438, 370, 24, 24, dpi);

    // Guide and update tabs.
    PlaceSettingsControl(state->guideShortcutsTitle, 34, 58, 280, 20, dpi);
    PlaceSettingsControl(state->guideText, 34, 84, 400, 220, dpi);
    PlaceSettingsControl(state->guideDiagnosticsTitle, 505, 58, 320, 20, dpi);
    PlaceSettingsControl(state->guideDiagnosticsText, 505, 84, 360, 70, dpi);
    PlaceSettingsControl(state->saveLogCheck, 505, 170, 360, 28, dpi);
    PlaceSettingsControl(state->showConsoleCheck, 505, 206, 360, 28, dpi);
    PlaceSettingsControl(state->guideLogFolderButton, 505, 248, 165, 26, dpi);
    PlaceSettingsControl(state->updateTitle, 34, 76, 400, 24, dpi);
    PlaceSettingsControl(state->updateText, 34, 110, 760, 64, dpi);
    PlaceSettingsControl(state->checkForUpdatesCheck, 34, 190, 500, 28, dpi);
    PlaceSettingsControl(state->updateNowButton, 34, 230, 185, 30, dpi);
    PlaceSettingsControl(state->updateStatus, 235, 234, 650, 24, dpi);
}

static void SetSettingsControlVisible(HWND control, bool visible) {
    if (!control) return;
    ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    EnableWindow(control, visible ? TRUE : FALSE);
}

static VideoPixelFormat SelectedPixelFormat(
    const SettingsDialogState* state);
static bool SettingsUsesExclusiveMode(
    const SettingsDialogState* state);

static void UpdateScalingControlVisibility(SettingsDialogState* state) {
    if (!state) return;
    const bool pixelPerfect = state->pixelCheck &&
        SendMessageW(state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool visible = state->activeTab == SettingsTab::VideoWindow &&
                         !pixelPerfect;
    SetSettingsControlVisible(state->scalingLabel, visible);
    SetSettingsControlVisible(state->scalingCombo, visible);
}

static void UpdateWindowBehaviorVisibility(SettingsDialogState* state) {
    if (!state) return;
    const bool pixelPerfect = state->pixelCheck &&
        SendMessageW(state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool relativeSize = state->relativeSizeCheck &&
        SendMessageW(state->relativeSizeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

    // These are everyday window-behavior preferences, not advanced tuning.
    // Only show the caveat when the currently selected combination needs it.
    const bool visible = state->activeTab == SettingsTab::VideoWindow;
    SetSettingsControlVisible(state->relativeSizeCheck, visible);
    SetSettingsControlVisible(state->borderlessCheck, visible);
    SetSettingsControlVisible(state->fullscreenCursorLabel, visible);
    SetSettingsControlVisible(state->fullscreenCursorCombo, visible);
    SetSettingsControlVisible(state->fullscreenCursorHint, visible);
    SetSettingsControlVisible(state->relativeSizeWarning,
                              visible && pixelPerfect && relativeSize);
}

static void UpdateAdvancedControlVisibility(SettingsDialogState* state) {
    if (!state) return;
    const bool audio = state->activeTab == SettingsTab::Audio;
    const bool video = state->activeTab == SettingsTab::VideoWindow;
    const bool guide = state->activeTab == SettingsTab::GuideDiagnostics;
    const bool updates = state->activeTab == SettingsTab::Updates;
    for (HWND control : {state->tabControl, state->languageLabel,
                         state->languageCombo, state->skipStartupCheck,
                         state->skipStartupHint, state->versionWatermark,
                         state->startButton, state->cancelButton}) {
        SetSettingsControlVisible(control, true);
    }
    for (HWND control : {state->audioOutputSection,
                         state->audioPlaybackSection,
                         state->audioStabilitySection,
                         state->audioLabel, state->audioCombo,
                         state->audioOutputLabel, state->audioOutputCombo,
                         state->bufferLabel, state->bufferCombo,
                         state->audioStatus,
                         state->volumeHudLabel, state->volumeHudCombo,
                         state->volumeBoostCheck, state->volumeBoostHelp,
                         state->muteBackgroundCheck, state->audioOnlyCheck,
                         state->driftLabel, state->driftHelp, state->driftCombo,
                         state->pcmQueueLabel, state->pcmQueueHelp,
                         state->pcmQueueCombo}) {
        SetSettingsControlVisible(control, audio);
    }
    // The endpoint recheck belongs only to WASAPI Exclusive.  In Shared and
    // ASIO modes it is both irrelevant and misleading, even on the Audio tab.
    SetSettingsControlVisible(state->exclusiveTestButton,
                              audio && SettingsUsesExclusiveMode(state));
    for (HWND control : {state->videoCaptureSection,
                         state->videoDisplaySection,
                         state->videoWindowSection,
                         state->presentationLabel, state->presentationHelp,
                         state->presentationCombo, state->captureDeviceLabel,
                         state->captureDeviceCombo,
                         state->captureAudioDeviceLabel, state->videoLabel,
                         state->videoCombo, state->pixelFormatLabel,
                         state->pixelFormatCombo, state->frameRateLabel,
                         state->frameRateCombo, state->videoCapabilityStatus,
                         state->pixelCheck, state->windowSnapCheck,
                         state->fullscreenCursorLabel,
                         state->fullscreenCursorCombo,
                         state->fullscreenCursorHint}) {
        SetSettingsControlVisible(control, video);
    }
    // This row has two mutually exclusive controls: the device picker for a
    // separate capture endpoint, or the short "built-in audio" status. Keep
    // its existing video-tab choice intact; hide both together off-tab.
    if (!video) {
        SetSettingsControlVisible(state->captureAudioDeviceCombo, false);
        SetSettingsControlVisible(state->captureAudioStatus, false);
    }
    const bool p010Selected = SelectedPixelFormat(state) ==
        VideoPixelFormat::P010;
    const bool mjpegSelected = SelectedPixelFormat(state) ==
        VideoPixelFormat::Mjpeg;
    SetSettingsControlVisible(state->forceHdr10Check, video && p010Selected);
    SetSettingsControlVisible(state->forceHdr10Help, video && p010Selected);
    SetSettingsControlVisible(state->mjpegColorLabel, video && mjpegSelected);
    SetSettingsControlVisible(state->mjpegColorCombo, video && mjpegSelected);
    SetSettingsControlVisible(state->mjpegColorHelp, video && mjpegSelected);
    SetSettingsControlVisible(state->guideShortcutsTitle, guide);
    SetSettingsControlVisible(state->guideText, guide);
    SetSettingsControlVisible(state->guideDiagnosticsTitle, guide);
    SetSettingsControlVisible(state->guideDiagnosticsText, guide);
    SetSettingsControlVisible(state->guideLogFolderButton, guide);
    SetSettingsControlVisible(state->saveLogCheck, guide);
    SetSettingsControlVisible(state->showConsoleCheck, guide);
    SetSettingsControlVisible(state->updateTitle, updates);
    SetSettingsControlVisible(state->updateText, updates);
    SetSettingsControlVisible(state->checkForUpdatesCheck, updates);
    SetSettingsControlVisible(state->updateNowButton, updates);
    SetSettingsControlVisible(state->updateStatus, updates);
    UpdateScalingControlVisibility(state);
    UpdateWindowBehaviorVisibility(state);
}

static void SetSettingsUpdateStatus(SettingsDialogState* state,
                                    const std::wstring& text) {
    if (!state || !state->updateStatus) return;
    SetWindowTextW(state->updateStatus, text.c_str());
}

static void StartSettingsUpdateCheck(SettingsDialogState* state, HWND hwnd) {
    if (!state || !hwnd ||
        state->updateCheckRunning.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // A completed worker remains joinable until its result message has been
    // handled. Join it here before starting the next manual request.
    if (state->updateCheckThread.joinable()) {
        state->updateCheckThread.join();
    }
    state->updateCheckStop.store(false, std::memory_order_release);
    SetSettingsUpdateStatus(state, UI_TEXT(L"최신 버전 확인 중…"));
    EnableWindow(state->updateNowButton, FALSE);
    state->updateCheckThread = std::thread([state, hwnd]() {
        UpdateCheckResult result;
        llcv::update::FetchLatestRelease(kAppVersionLabel, result);
        if (state->updateCheckStop.load(std::memory_order_acquire) ||
            !IsWindow(hwnd)) {
            return;
        }
        auto* message = new UpdateCheckResult(std::move(result));
        if (!PostMessageW(hwnd, WM_SETTINGS_UPDATE_CHECK_COMPLETE, 0,
                          reinterpret_cast<LPARAM>(message))) {
            delete message;
        }
    });
}

static void TrackSettingsTooltip(HWND target, HWND tooltip, bool active) {
    if (!target || !tooltip) return;
    TOOLINFOW tool{};
    tool.cbSize = TTTOOLINFO_V1_SIZE;
    tool.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    tool.hwnd = GetParent(target);
    tool.uId = reinterpret_cast<UINT_PTR>(target);
    SendMessageW(tooltip, TTM_TRACKACTIVATE, active ? TRUE : FALSE,
                 reinterpret_cast<LPARAM>(&tool));
}

static void AddSettingsTooltip(SettingsDialogState* state, HWND owner,
                               HWND target, const wchar_t* text) {
    if (!state || !owner || !target || !text) return;
    if (!state->tooltipWindow) {
        state->tooltipWindow = CreateWindowExW(
            WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!state->tooltipWindow) return;
        SetWindowPos(state->tooltipWindow, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SendMessageW(state->tooltipWindow, TTM_SETMAXTIPWIDTH, 0, 430);
        SendMessageW(state->tooltipWindow, TTM_SETDELAYTIME,
                     TTDT_INITIAL, 250);
    }
    TOOLINFOW tool{};
    // The application intentionally has no Common Controls v6 manifest.  The
    // built-in v5 tooltip rejects the newer structure size on some Windows
    // installations, so register the compatible v1 fields explicitly.
    tool.cbSize = TTTOOLINFO_V1_SIZE;
    tool.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    tool.hwnd = owner;
    tool.uId = reinterpret_cast<UINT_PTR>(target);
    tool.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(state->tooltipWindow, TTM_ADDTOOLW, 0,
                 reinterpret_cast<LPARAM>(&tool));
}

static bool IsSettingsHelpControl(const SettingsDialogState* state,
                                  HWND target) {
    return state && (target == state->driftHelp ||
                     target == state->pcmQueueHelp ||
                      target == state->presentationHelp ||
                      target == state->volumeBoostHelp ||
                      target == state->forceHdr10Help ||
                      target == state->mjpegColorHelp);
}

enum class SettingsHelpTopic {
    Drift,
    PcmQueue,
    Presentation,
    VolumeBoost,
    ForceHdr10,
    MjpegColor,
};

static const wchar_t* SettingsHelpText(SettingsHelpTopic topic) {
    if (IsEnglishUi()) {
        switch (topic) {
        case SettingsHelpTopic::Drift:
            return L"Preventing audio tearing · deciding whether correction is needed\n\n"
                    L"The capture and output device clocks can run at slightly different rates. "
                    L"Auto mode watches the application PCM queue first and enables resampling only "
                    L"when a sustained imbalance is detected. It stays enabled for the rest of the "
                    L"session once triggered, avoiding repeated on/off clicks. Check the Tab OSD for "
                    L"10–30 minutes.\n\n"
                   L"'Stable · correction unnecessary' or 'Rare errors · Off can be kept' means "
                   L"you can leave it Off when the audio is clean. If 'Repeated imbalance · "
                    L"correction recommended' continues, choose Auto. Do not judge "
                   L"from errors immediately after startup.\n\n"
                   L"The resampler and PCM safety buffer are independent. 'Resampler correction "
                   L"limit approaching' indicates clock difference; 'Possible PCM buffer shortage' "
                   L"indicates a momentary lack of queued audio; 'Capture packet delay detected' "
                   L"indicates a late input callback. If the resampler is healthy but underruns "
                   L"continue, raise the PCM buffer target first.\n\n"
                   L"The imbalance ppm shown in the OSD is an estimate from accumulated underrun/"
                    L"overrun frames, not a direct hardware-clock measurement. When Auto activates, "
                    L"the resampler adds a small amount of audio buffering and changes PCM samples. "
                    L"Off always preserves the original PCM path; On always uses the resampler.";
        case SettingsHelpTopic::PcmQueue:
            return L"PCM buffer target\n\n"
                   L"The amount of captured audio kept inside the application before playback.\n"
                   L"10 ms is minimum latency, 15 ms is the low-latency target, 20 ms is the stable "
                   L"recommendation, 25 ms adds stability margin, and 30 ms prioritizes stability.\n\n"
                   L"Higher values absorb more scheduling jitter but add the same amount of audio "
                   L"latency. This is independent of the WASAPI output buffer and clock-drift correction.";
        case SettingsHelpTopic::Presentation:
            return llcv::presentation_ui::HelpText(true);
        case SettingsHelpTopic::VolumeBoost:
            return L"Volume boost above 100%\n\n"
                   L"Allows the mouse wheel to raise the app volume up to 200%. "
                   L"100% is the original PCM level; values above it apply digital gain only inside this app.\n\n"
                   L"No audio buffer or frame queue is added, so this option does not add audio latency. "
                   L"At high source volumes, boosting can clip peaks and cause distortion. Keep it off unless "
                   L"the capture audio is genuinely too quiet.";
        case SettingsHelpTopic::ForceHdr10:
            return L"Force HDR10 output\n\n"
                   L"Use this only when the source is confirmed to be HDR and the capture driver does not expose "
                   L"color metadata. It treats P010 as BT.2020/PQ and enables the HDR10 swap chain.\n\n"
                   L"If the source is SDR, or the monitor is not handling HDR correctly, colors can look strongly "
                   L"oversaturated or otherwise wrong. Turn it off in that case. This does not add a frame queue; "
                   L"it only changes the output color interpretation.";
        case SettingsHelpTopic::MjpegColor:
            return L"MJPEG color interpretation\n\n"
                   L"Auto uses decoder metadata first, then DirectShow metadata. If neither identifies "
                   L"the format, it uses JPEG Full range with BT.709 for HD or BT.601 for SD.\n\n"
                   L"Use a manual combination only when MJPEG colors still differ from another capture "
                   L"application. Full/Limited changes black and white levels; BT.709/BT.601 changes the "
                   L"YUV color matrix. This does not add a frame queue or increase latency.";
        }
    }
    switch (topic) {
    case SettingsHelpTopic::Drift:
         return L"소리 찢어짐 방지 · 보정 필요 확인\n\n"
                L"자동은 프로그램 내부 PCM 대기량을 관찰하다가 클록 불균형이 일정 시간 지속될 때만 "
                L"리샘플링을 켭니다. 한 번 켜지면 세션 중 반복해서 켰다 끄지 않아 소리 변화와 클릭을 "
                L"줄입니다. Tab OSD를 10~30분 확인하세요.\n\n"
               L"'안정 · 보정 불필요' 또는 '드문 오류 · 끔 유지 가능'이면 소리에 문제가 "
                L"없는 한 끔을 유지해도 됩니다. '반복 불균형 · 보정 권장'이 계속 보이면 자동을 "
                L"선택하세요. 시작 직후 오류만으로 판단하지 마세요.\n\n"
               L"리샘플러와 PCM 안전 대기량은 서로 독립입니다. '리샘플러 보정 한계 접근'은 "
               L"클록 차이, 'PCM 버퍼 부족 가능'은 순간 버퍼 여유 부족, '캡처 패킷 지연 감지'는 "
               L"입력 콜백 지연을 뜻합니다. 리샘플러가 정상인데 underrun이 나면 PCM 버퍼 "
               L"목표를 먼저 높이세요.\n\n"
                L"OSD의 불균형 ppm은 누적 underrun/overrun으로 계산한 참고값이며 실제 하드웨어 "
                L"클록을 직접 측정한 값은 아닙니다. 자동이 작동하면 작은 오디오 대기량을 추가하고 "
                L"PCM 샘플을 변경합니다. 끔은 원본 PCM을 유지하고, 켬은 항상 리샘플러를 사용합니다.";
    case SettingsHelpTopic::PcmQueue:
        return L"PCM 버퍼 목표 안내\n\n"
               L"캡처 오디오를 재생 전에 확보하는 프로그램 내부 대기량입니다.\n"
               L"10ms는 최저 지연, 15ms는 저지연 목표, 20ms는 안정 권장, 25ms는 안정 여유, "
               L"30ms는 안정성 우선 설정입니다.\n\n"
               L"값을 높이면 순간적인 입력 지연을 흡수할 여유가 커지지만, 그만큼 오디오 지연이 "
               L"늘어납니다. WASAPI 출력 버퍼와 클록 드리프트 보정과는 독립적으로 조정됩니다.";
    case SettingsHelpTopic::Presentation:
        return llcv::presentation_ui::HelpText(false);
    case SettingsHelpTopic::VolumeBoost:
        return L"100% 이상 볼륨 증폭 안내\n\n"
               L"마우스 휠로 앱 음량을 최대 200%까지 올릴 수 있게 합니다. 100%는 원본 PCM 크기이고, "
               L"그 이상은 이 앱 안에서만 디지털 증폭을 적용합니다.\n\n"
               L"추가 오디오 버퍼나 프레임 큐를 만들지 않으므로 오디오 지연은 늘지 않습니다. 다만 원본 "
               L"소리가 이미 큰 경우에는 피크가 잘려 왜곡될 수 있으니, 실제로 음량이 부족할 때만 켜세요.";
    case SettingsHelpTopic::ForceHdr10:
        return L"HDR10 강제 출력 안내\n\n"
               L"캡처 드라이버가 색공간 메타데이터를 제공하지 않지만 입력이 HDR임을 확인한 경우에만 사용하세요. "
               L"P010을 BT.2020/PQ로 처리하고 HDR10 출력으로 표시합니다.\n\n"
               L"입력이 SDR이거나 모니터의 HDR 처리가 맞지 않으면 색상이 과포화되거나 부정확해질 수 있습니다. "
               L"그 경우 이 옵션을 끄세요. 프레임 큐를 추가하지 않으므로 표시 지연은 늘지 않고 출력 색상 해석만 바뀝니다.";
    case SettingsHelpTopic::MjpegColor:
        return L"MJPEG 색상 해석 안내\n\n"
               L"자동은 디코더 메타데이터를 먼저 사용하고, 없으면 DirectShow 정보를 확인합니다. 양쪽 모두 "
               L"알려주지 않으면 JPEG Full range와 HD BT.709 또는 SD BT.601을 사용합니다.\n\n"
               L"자동 색상이 다른 캡처 프로그램과 계속 다를 때만 수동 조합을 선택하세요. Full/Limited는 "
               L"명암 범위를, BT.709/BT.601은 YUV 색상 행렬을 바꿉니다. 프레임 큐를 추가하지 않아 "
               L"표시 지연은 늘지 않습니다.";
    }
    return L"";
}

static bool SettingsUsesSharedMode(const SettingsDialogState* state) {
    return state && SendMessageW(state->audioCombo, CB_GETCURSEL, 0, 0) == 0;
}

static bool SettingsUsesExclusiveMode(const SettingsDialogState* state) {
    return state && SendMessageW(state->audioCombo, CB_GETCURSEL, 0, 0) == 1;
}

static bool SettingsUsesAsioMode(const SettingsDialogState* state) {
    return state && state->asioAvailable &&
           SendMessageW(state->audioCombo, CB_GETCURSEL, 0, 0) == 2;
}

static void UpdateAsioControlVisibility(SettingsDialogState* state) {
    if (!state) return;
    // ASIO owns the output clock, but the same optional app-side resampler is
    // available for long-run capture/output drift. Keep the control visible
    // and enabled in every output mode.
    if (state->driftCombo) EnableWindow(state->driftCombo, TRUE);
    if (state->driftHelp) EnableWindow(state->driftHelp, TRUE);
}

static void PopulateAudioOutputCombo(SettingsDialogState* state) {
    if (!state || !state->audioOutputCombo) return;
    const LRESULT previousSelection = SendMessageW(
        state->audioOutputCombo, CB_GETCURSEL, 0, 0);
    SendMessageW(state->audioOutputCombo, CB_RESETCONTENT, 0, 0);
    if (SettingsUsesAsioMode(state)) {
        SetWindowTextW(state->audioOutputLabel, UI_TEXT(L"ASIO 출력 드라이버"));
        LRESULT selected = 0;
        for (size_t i = 0; i < state->asioDrivers.size(); ++i) {
            const std::wstring name = AsioDriverNameWide(
                state->asioDrivers[i].name);
            const LRESULT index = SendMessageW(
                state->audioOutputCombo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(name.c_str()));
            if (name == g_settings.asioDriverName) selected = index;
        }
        SendMessageW(state->audioOutputCombo, CB_SETCURSEL, selected, 0);
        return;
    }

    SetWindowTextW(state->audioOutputLabel, UI_TEXT(L"오디오 출력 장치"));
    std::wstring defaultLabel = UI_TEXT(L"Windows 기본 출력 장치 따라가기 (권장)");
    if (SettingsUsesExclusiveMode(state)) {
        const auto defaultIt = std::find_if(
            state->audioEndpoints.begin(), state->audioEndpoints.end(),
            [](const AudioEndpointInfo& endpoint) { return endpoint.isDefault; });
        if (defaultIt != state->audioEndpoints.end()) {
            const size_t index = static_cast<size_t>(
                std::distance(state->audioEndpoints.begin(), defaultIt));
            if (index < state->exclusiveEndpointResults.size()) {
                const auto& result = state->exclusiveEndpointResults[index];
                if (result.state == ExclusiveEndpointState::Supported) {
                    wchar_t suffix[48]{};
                    swprintf_s(suffix, UI_TEXT(L" · 사용 가능 · %d ms"),
                               result.recommendedBufferMs);
                    defaultLabel += suffix;
                } else if (result.state == ExclusiveEndpointState::Testing) {
                    defaultLabel += UI_TEXT(L" · 검사 중");
                } else if (result.state == ExclusiveEndpointState::Unsupported) {
                    defaultLabel += UI_TEXT(L" · 사용 불가");
                }
            }
        }
    }
    SendMessageW(state->audioOutputCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(defaultLabel.c_str()));
    LRESULT selected = 0;
    for (size_t i = 0; i < state->audioEndpoints.size(); ++i) {
        std::wstring label = state->audioEndpoints[i].name;
        if (state->audioEndpoints[i].isDefault) {
            label += UI_TEXT(L" (현재 기본)");
        }
        if (SettingsUsesExclusiveMode(state) &&
            i < state->exclusiveEndpointResults.size()) {
            const auto& result = state->exclusiveEndpointResults[i];
            if (result.state == ExclusiveEndpointState::Supported) {
                wchar_t suffix[48]{};
                swprintf_s(suffix, UI_TEXT(
                    L" (사용 가능 · %d ms)"),
                           result.recommendedBufferMs);
                label += suffix;
            } else if (result.state == ExclusiveEndpointState::Testing) {
                label += UI_TEXT(L" (검사 중)");
            } else if (result.state == ExclusiveEndpointState::Unsupported) {
                label += UI_TEXT(L" (사용 불가)");
            }
        }
        const LRESULT index = SendMessageW(
            state->audioOutputCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (state->audioEndpoints[i].id == g_settings.audioOutputDeviceId) {
            selected = index;
        }
    }
    if (previousSelection >= 0 &&
        previousSelection <= static_cast<LRESULT>(state->audioEndpoints.size())) {
        selected = previousSelection;
    }
    SendMessageW(state->audioOutputCombo, CB_SETCURSEL, selected, 0);
}

static void RememberCurrentBufferChoice(SettingsDialogState* state) {
    if (!state || !state->bufferCombo) return;
    const LRESULT index = SendMessageW(state->bufferCombo, CB_GETCURSEL, 0, 0);
    if (index < 0) return;
    const LRESULT value = SendMessageW(state->bufferCombo, CB_GETITEMDATA,
                                       static_cast<WPARAM>(index), 0);
    if (value == CB_ERR) return;
    if (state->bufferItemsAreSharedFrames) {
        state->selectedSharedPeriodFrames = static_cast<UINT32>(value);
    } else {
        state->selectedBufferMs = static_cast<int>(value);
    }
}

static std::vector<UINT32> BuildSharedPeriodChoices(
    const AudioClient3Support& support) {
    std::vector<UINT32> choices;
    auto add = [&](UINT32 requested) {
        const UINT32 value = ClosestSupportedSharedPeriod(requested, support);
        if (value) choices.push_back(value);
    };

    add(support.minimumFrames);
    for (const int ms : kWasapiBufferOptionsMs) {
        add(static_cast<UINT32>(ms * kSampleRate / 1000));
    }
    add(support.defaultFrames);
    std::sort(choices.begin(), choices.end());
    choices.erase(std::unique(choices.begin(), choices.end()), choices.end());
    return choices;
}

static void PopulateSettingsBufferCombo(SettingsDialogState* state) {
    if (!state || !state->bufferCombo) return;
    SendMessageW(state->bufferCombo, CB_RESETCONTENT, 0, 0);

    if (SettingsUsesAsioMode(state)) {
        state->bufferItemsAreSharedFrames = false;
        SendMessageW(state->bufferCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(UI_TEXT(
                         L"ASIO 드라이버 선호 버퍼 (드라이버 설정 사용)")));
        SendMessageW(state->bufferCombo, CB_SETCURSEL, 0, 0);
        EnableWindow(state->bufferCombo, FALSE);
        return;
    }
    EnableWindow(state->bufferCombo, TRUE);

    const bool useSharedFrames = SettingsUsesSharedMode(state) &&
                                 state->probeReady.load(std::memory_order_acquire) &&
                                 state->probe.supported;
    state->bufferItemsAreSharedFrames = useSharedFrames;

    if (useSharedFrames) {
        state->sharedPeriodChoices = BuildSharedPeriodChoices(state->probe);
        UINT32 desired = state->selectedSharedPeriodFrames;
        if (!desired) {
            desired = static_cast<UINT32>(state->selectedBufferMs *
                                          kSampleRate / 1000);
        }
        size_t selected = 0;
        UINT32 smallestDifference = UINT32_MAX;
        for (size_t i = 0; i < state->sharedPeriodChoices.size(); ++i) {
            const UINT32 frames = state->sharedPeriodChoices[i];
            wchar_t label[96]{};
            const double ms = 1000.0 * frames / kSampleRate;
            if (frames == state->probe.defaultFrames) {
                swprintf_s(label, UI_TEXT(L"%.2f ms (권장)"), ms);
            } else if (frames == state->probe.minimumFrames) {
                swprintf_s(label, UI_TEXT(L"%.2f ms (최저)"), ms);
            } else {
                swprintf_s(label, L"%.2f ms", ms);
            }
            const LRESULT index = SendMessageW(
                state->bufferCombo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label));
            SendMessageW(state->bufferCombo, CB_SETITEMDATA,
                         static_cast<WPARAM>(index), frames);
            const UINT32 difference = frames > desired ? frames - desired
                                                        : desired - frames;
            if (difference < smallestDifference) {
                smallestDifference = difference;
                selected = i;
            }
        }
        if (!state->sharedPeriodChoices.empty()) {
            SendMessageW(state->bufferCombo, CB_SETCURSEL,
                         static_cast<WPARAM>(selected), 0);
            state->selectedSharedPeriodFrames =
                state->sharedPeriodChoices[selected];
        }
        return;
    }

    const bool exclusiveOptions = SettingsUsesExclusiveMode(state);
    const size_t optionCount = exclusiveOptions
        ? ARRAYSIZE(kExclusiveBufferOptionsMs)
        : ARRAYSIZE(kWasapiBufferOptionsMs);
    size_t selected = 0;
    for (size_t i = 0; i < optionCount; ++i) {
        const int optionMs = exclusiveOptions
            ? kExclusiveBufferOptionsMs[i] : kWasapiBufferOptionsMs[i];
        wchar_t label[64]{};
        // The useful Exclusive buffer is device-specific and comes from its
        // preflight verdict, so a global "20 ms recommended" label misleads.
        swprintf_s(label, L"%d ms", optionMs);
        const LRESULT index = SendMessageW(
            state->bufferCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label));
        SendMessageW(state->bufferCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(index), optionMs);
        if (optionMs == state->selectedBufferMs) selected = i;
    }
    SendMessageW(state->bufferCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selected), 0);
}

static void UpdateAudioClient3Status(SettingsDialogState* state) {
    if (!state || !state->audioStatus) return;
    if (SettingsUsesAsioMode(state)) {
        SetWindowTextW(state->audioStatus, UI_TEXT(
            L"ASIO 출력 · 드라이버 기본 버퍼 사용 · 앱 클록 보정 가능"));
        return;
    }
    if (SettingsUsesExclusiveMode(state)) {
        SetWindowTextW(state->audioStatus, UI_TEXT(
            L"WASAPI Exclusive 이벤트 진단 · 장치 독점 · IAudioClient3 미사용"));
        return;
    }
    if (!state->probeReady.load(std::memory_order_acquire)) {
        SetWindowTextW(state->audioStatus, UI_TEXT(L"Shared 저지연 지원 확인 중…"));
        return;
    }

    wchar_t status[200]{};
    if (state->probe.supported) {
        swprintf_s(status,
                   UI_TEXT(L"Shared 저지연 · %.2f~%.2f ms · 검사 %.1f ms"),
                   1000.0 * state->probe.minimumFrames / kSampleRate,
                   1000.0 * state->probe.maximumFrames / kSampleRate,
                   state->probe.probeMilliseconds);
    } else {
        // Keep the settings row actionable and short instead of exposing an
        // implementation HRESULT that does not help with device selection.
        swprintf_s(status, UI_TEXT(
            L"Shared 기본 모드 · 저지연 API 미지원"));
    }
    SetWindowTextW(state->audioStatus, status);
}

static void UpdateExclusiveProbeControl(SettingsDialogState* state) {
    if (!state || !state->exclusiveTestButton) return;
    const bool running = state->exclusiveScanRunning.load(
        std::memory_order_acquire);
    const bool supportedMode = SettingsUsesExclusiveMode(state);
    const bool visible = state->activeTab == SettingsTab::Audio &&
                         supportedMode;
    // Handle mode changes directly as well as tab changes.  Otherwise a
    // button hidden while Shared was selected can remain hidden after the
    // user switches to Exclusive without leaving the tab.
    SetSettingsControlVisible(state->exclusiveTestButton, visible);
    if (!visible) return;
    EnableWindow(state->exclusiveTestButton,
                 supportedMode && !running ? TRUE : FALSE);
    SetWindowTextW(state->exclusiveTestButton,
                   UI_TEXT(running ? L"장치 검사 중…" :
                                     L"전체 장치 다시 검사"));
}

static std::wstring SelectedAudioEndpointId(
    const SettingsDialogState* state) {
    if (SettingsUsesAsioMode(state)) return {};
    if (!state || !state->audioOutputCombo) return {};
    const LRESULT index = SendMessageW(
        state->audioOutputCombo, CB_GETCURSEL, 0, 0);
    if (index <= 0 || static_cast<size_t>(index - 1) >=
                          state->audioEndpoints.size()) return {};
    return state->audioEndpoints[static_cast<size_t>(index - 1)].id;
}

static std::wstring EffectiveSelectedAudioEndpointId(
    const SettingsDialogState* state) {
    const std::wstring selected = SelectedAudioEndpointId(state);
    if (!selected.empty()) return selected;
    if (!state) return {};
    for (const auto& endpoint : state->audioEndpoints) {
        if (endpoint.isDefault) return endpoint.id;
    }
    return {};
}

static const ExclusiveEndpointVerification* FindExclusiveVerification(
    const SettingsDialogState* state, const std::wstring& endpointId) {
    if (!state || endpointId.empty()) return nullptr;
    for (size_t i = 0; i < state->audioEndpoints.size() &&
                       i < state->exclusiveEndpointResults.size(); ++i) {
        if (state->audioEndpoints[i].id == endpointId) {
            return &state->exclusiveEndpointResults[i];
        }
    }
    return nullptr;
}

static int ExclusiveVerifiedBufferForSelection(const SettingsDialogState* state) {
    const std::wstring endpointId = EffectiveSelectedAudioEndpointId(state);
    const auto* result = FindExclusiveVerification(state, endpointId);
    return result && result->state == ExclusiveEndpointState::Supported
        ? result->recommendedBufferMs : 0;
}

static void PersistCompletedExclusiveEndpointResults(
    const SettingsDialogState* state) {
    if (!state) return;
    bool changed = false;
    for (size_t i = 0; i < state->audioEndpoints.size() &&
                       i < state->exclusiveEndpointResults.size(); ++i) {
        const auto& result = state->exclusiveEndpointResults[i];
        if (result.state != ExclusiveEndpointState::Supported &&
            result.state != ExclusiveEndpointState::Unsupported) {
            continue;
        }
        const bool supported = result.state == ExclusiveEndpointState::Supported;
        const int recommendedBufferMs = supported
            ? result.recommendedBufferMs : 0;
        auto it = std::find_if(
            g_settings.exclusiveEndpointCache.begin(),
            g_settings.exclusiveEndpointCache.end(),
            [&](const ExclusiveEndpointCacheEntry& entry) {
                return entry.endpointId == state->audioEndpoints[i].id;
            });
        if (it == g_settings.exclusiveEndpointCache.end()) {
            if (g_settings.exclusiveEndpointCache.size() >=
                kMaximumExclusiveEndpointCacheEntries) {
                continue;
            }
            g_settings.exclusiveEndpointCache.push_back({
                state->audioEndpoints[i].id, supported, recommendedBufferMs});
            changed = true;
        } else if (it->supported != supported ||
                   it->recommendedBufferMs != recommendedBufferMs) {
            it->supported = supported;
            it->recommendedBufferMs = recommendedBufferMs;
            changed = true;
        }
    }
    // Probe output is a completed user-requested diagnostic, not an unaccepted
    // settings edit. Persist it immediately so reopening the dialog reuses it.
    if (changed) SaveSettings();
}

static bool HasExclusiveVerificationForSelection(
    const SettingsDialogState* state) {
    if (!state || !SettingsUsesExclusiveMode(state)) return true;
    const int verifiedBufferMs = ExclusiveVerifiedBufferForSelection(state);
    return IsExclusiveLowLatencyBuffer(verifiedBufferMs) &&
           state->selectedBufferMs >= verifiedBufferMs;
}

static void UpdateExclusiveVerificationUi(SettingsDialogState* state) {
    if (!state || !state->startButton) return;
    const bool running = state->exclusiveScanRunning.load(
        std::memory_order_acquire);
    if (!SettingsUsesExclusiveMode(state)) {
        // Shared/ASIO must not be blocked by a diagnostic scan that is only
        // relevant to Exclusive. The scan is stopped when the mode changes.
        EnableWindow(state->startButton, TRUE);
        return;
    }
    const bool verified = HasExclusiveVerificationForSelection(state);
    EnableWindow(state->startButton, verified ? TRUE : FALSE);
    if (state->audioStatus) {
        const auto* selectedResult = FindExclusiveVerification(
            state, EffectiveSelectedAudioEndpointId(state));
        // Show the active all-device scan first. A provisional failure for
        // the selected endpoint must not look like the final UI state while
        // other endpoints are still being checked.
        if (running) {
            wchar_t status[160]{};
            swprintf_s(status, UI_TEXT(
                L"Exclusive 출력 장치 검사 중… %zu/%zu 완료"),
                state->exclusiveScanCompleted, state->audioEndpoints.size());
            SetWindowTextW(state->audioStatus, status);
        } else if (verified) {
            wchar_t status[160]{};
            swprintf_s(status, UI_TEXT(
                L"Exclusive 사용 가능 · 현재 출력 장치 · %d ms 이상"),
                ExclusiveVerifiedBufferForSelection(state));
            SetWindowTextW(state->audioStatus, status);
        } else if (selectedResult &&
                   selectedResult->state == ExclusiveEndpointState::Supported) {
            wchar_t status[160]{};
            swprintf_s(status, UI_TEXT(
                L"Exclusive 사용 가능 · %d ms 이상 선택 필요"),
                selectedResult->recommendedBufferMs);
            SetWindowTextW(state->audioStatus, status);
        } else if (selectedResult &&
                   selectedResult->state == ExclusiveEndpointState::Unsupported) {
            // Other endpoints may still be running, but this selected one has
            // a conclusive result already and should say so immediately.
            SetWindowTextW(state->audioStatus, UI_TEXT(
                L"Exclusive 사용 불가 · 현재 출력 장치"));
        } else {
            SetWindowTextW(state->audioStatus, UI_TEXT(
                L"Exclusive 검사 필요 · 현재 출력 장치"));
        }
    }
}

static void StartExclusiveEndpointScan(SettingsDialogState* state, HWND hwnd,
                                       bool forceRestart = false) {
    if (!state || state->exclusiveScanRunning.load(std::memory_order_acquire)) {
        return;
    }
    // Keep a completed scan for the lifetime of this settings dialog. Moving
    // to Shared/ASIO and back must not make the user wait through it again.
    if (!forceRestart && !state->audioEndpoints.empty() &&
        state->exclusiveScanCompleted >= state->audioEndpoints.size()) {
        return;
    }
    if (state->exclusiveProbeThread.joinable()) {
        state->exclusiveProbeThread.join();
    }

    if (forceRestart) {
        state->exclusiveEndpointResults.assign(
            state->audioEndpoints.size(), ExclusiveEndpointVerification{});
        state->exclusiveScanCompleted = 0;
    }
    // Reuse persisted results and only test endpoints with no verdict. An
    // explicit retry deliberately changes every row back to Testing.
    size_t completed = 0;
    for (auto& result : state->exclusiveEndpointResults) {
        if (result.state == ExclusiveEndpointState::Supported ||
            result.state == ExclusiveEndpointState::Unsupported) {
            ++completed;
        } else {
            result.state = ExclusiveEndpointState::Testing;
        }
    }
    state->exclusiveScanCompleted = completed;
    state->exclusiveProbeStop.store(false, std::memory_order_release);
    state->exclusiveScanRunning.store(true, std::memory_order_release);
    PopulateAudioOutputCombo(state);
    const int initialRecommendedBufferMs =
        ExclusiveVerifiedBufferForSelection(state);
    if (IsExclusiveLowLatencyBuffer(initialRecommendedBufferMs)) {
        state->selectedBufferMs = initialRecommendedBufferMs;
        PopulateSettingsBufferCombo(state);
    }
    UpdateExclusiveProbeControl(state);
    UpdateExclusiveVerificationUi(state);

    const std::vector<AudioEndpointInfo> endpoints = state->audioEndpoints;
    std::vector<size_t> scanOrder;
    const std::wstring preferredId = EffectiveSelectedAudioEndpointId(state);
    for (size_t i = 0; i < endpoints.size(); ++i) {
        if (state->exclusiveEndpointResults[i].state ==
                ExclusiveEndpointState::Testing &&
            endpoints[i].id == preferredId) {
            scanOrder.push_back(i);
            break;
        }
    }
    for (size_t i = 0; i < endpoints.size(); ++i) {
        if (state->exclusiveEndpointResults[i].state !=
            ExclusiveEndpointState::Testing) {
            continue;
        }
        if (scanOrder.empty() || i != scanOrder.front()) scanOrder.push_back(i);
    }
    state->exclusiveProbeThread = std::thread(
        [state, hwnd, endpoints, scanOrder]() {
            for (const size_t i : scanOrder) {
                if (state->exclusiveProbeStop.load(std::memory_order_acquire)) {
                    break;
                }
                auto* message = new ExclusiveEndpointProbeMessage{};
                message->endpointIndex = i;
                message->probe = ProbeExclusiveBufferRecommendation(
                    endpoints[i].id, &state->exclusiveProbeStop);
                if (!PostMessageW(hwnd, WM_EXCLUSIVE_ENDPOINT_PROBE_COMPLETE,
                                  reinterpret_cast<WPARAM>(message), 0)) {
                    delete message;
                    break;
                }
            }
            state->exclusiveScanRunning.store(false, std::memory_order_release);
            PostMessageW(hwnd, WM_EXCLUSIVE_SCAN_COMPLETE, 0, 0);
        });
}

static std::wstring SelectedAsioDriverName(const SettingsDialogState* state) {
    if (!SettingsUsesAsioMode(state) || !state->audioOutputCombo) return {};
    const LRESULT index = SendMessageW(state->audioOutputCombo, CB_GETCURSEL,
                                       0, 0);
    if (index < 0 || static_cast<size_t>(index) >= state->asioDrivers.size()) {
        return {};
    }
    const auto& name = state->asioDrivers[static_cast<size_t>(index)].name;
    return AsioDriverNameWide(name);
}

static std::wstring SelectedCaptureDeviceId(
    const SettingsDialogState* state) {
    if (!state || !state->captureDeviceCombo) return {};
    const LRESULT index = SendMessageW(
        state->captureDeviceCombo, CB_GETCURSEL, 0, 0);
    if (index <= 0 || static_cast<size_t>(index - 1) >=
                          state->captureDevices.size()) return {};
    return state->captureDevices[static_cast<size_t>(index - 1)].id;
}

static std::wstring SelectedCaptureAudioDeviceId(
    const SettingsDialogState* state) {
    if (!state || !state->captureAudioDeviceCombo) return {};
    const LRESULT index = SendMessageW(
        state->captureAudioDeviceCombo, CB_GETCURSEL, 0, 0);
    if (index <= 0 || static_cast<size_t>(index - 1) >=
                          state->captureAudioDevices.size()) return {};
    return state->captureAudioDevices[static_cast<size_t>(index - 1)].id;
}

static void UpdateCaptureAudioSelectionUi(SettingsDialogState* state) {
    if (!state || !state->captureAudioDeviceLabel ||
        !state->captureAudioDeviceCombo || !state->captureAudioStatus) {
        return;
    }
    const bool explicitSeparateDevice = !SelectedCaptureAudioDeviceId(state).empty();
    const bool onVideoTab =
        state->activeTab == SettingsTab::VideoWindow;
    const InternalCaptureAudioState probeState =
        state->captureAudioProbeReady.load(std::memory_order_acquire)
            ? state->captureAudioProbe.state
            : InternalCaptureAudioState::Checking;

    SetWindowTextW(state->captureAudioDeviceLabel,
                   UI_TEXT(L"캡처 오디오 장치"));
    if (explicitSeparateDevice ||
        probeState == InternalCaptureAudioState::SeparateDeviceNeeded ||
        probeState == InternalCaptureAudioState::Unknown) {
        SetSettingsControlVisible(state->captureAudioDeviceCombo, onVideoTab);
        SetSettingsControlVisible(state->captureAudioStatus, false);
        return;
    }

    SetSettingsControlVisible(state->captureAudioDeviceCombo, false);
    SetSettingsControlVisible(state->captureAudioStatus, onVideoTab);
    if (probeState == InternalCaptureAudioState::Available) {
        SetWindowTextW(state->captureAudioStatus,
                       UI_TEXT(L"영상 장치 내부 오디오 감지됨 · 자동 사용"));
    } else {
        SetWindowTextW(state->captureAudioStatus,
                       UI_TEXT(L"내부 오디오 확인 중…"));
    }
}

static void StartCaptureAudioProbe(SettingsDialogState* state, HWND hwnd) {
    if (!state || !hwnd) return;
    if (state->captureAudioProbeThread.joinable()) {
        state->captureAudioProbeThread.join();
    }
    state->captureAudioProbeReady.store(false, std::memory_order_release);
    EnableWindow(state->captureDeviceCombo, FALSE);
    UpdateCaptureAudioSelectionUi(state);
    const std::wstring captureDeviceId = SelectedCaptureDeviceId(state);
    state->captureAudioProbeThread = std::thread(
        [state, hwnd, captureDeviceId]() {
            const InternalCaptureAudioProbe probe =
                ProbeInternalCaptureAudio(captureDeviceId);
            state->captureAudioProbe = probe;
            state->captureAudioProbeReady.store(true,
                                                std::memory_order_release);
            PostMessageW(hwnd, WM_CAPTURE_AUDIO_PROBE_COMPLETE, 0, 0);
        });
}

static VideoPixelFormat SelectedPixelFormat(
    const SettingsDialogState* state) {
    if (!state || !state->pixelFormatCombo) return VideoPixelFormat::Auto;
    const LRESULT index = SendMessageW(
        state->pixelFormatCombo, CB_GETCURSEL, 0, 0);
    if (index < 0) return VideoPixelFormat::Auto;
    const LRESULT value = SendMessageW(
        state->pixelFormatCombo, CB_GETITEMDATA,
        static_cast<WPARAM>(index), 0);
    return value == CB_ERR ? VideoPixelFormat::Auto
                           : static_cast<VideoPixelFormat>(value);
}

static void UpdateVideoCapabilityStatus(SettingsDialogState* state) {
    if (!state) return;

    const bool supported = !state->pixelFormats.empty();
    const bool audioOnly = state->audioOnlyCheck &&
        SendMessageW(state->audioOnlyCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (state->pixelFormatCombo) {
        EnableWindow(state->pixelFormatCombo, supported ? TRUE : FALSE);
    }
    if (state->frameRateCombo) {
        EnableWindow(state->frameRateCombo, supported ? TRUE : FALSE);
    }
    if (state->startButton) {
        EnableWindow(state->startButton, (supported || audioOnly) ? TRUE : FALSE);
    }
    if (!state->videoCapabilityStatus) return;

    std::wstring message;
    if (audioOnly) {
        message = UI_TEXT(L"오디오 only: 영상 형식 확인 안 함");
    } else if (!supported) {
        message = UI_TEXT(L"지원 모드 없음: 다른 장치 또는 해상도를 선택하세요.");
    } else {
        message = IsEnglishUi() ? L"Detected:\r\n" : L"자동 인식:\r\n";
        bool firstFormat = true;
        for (const auto format : {VideoPixelFormat::Nv12,
                                  VideoPixelFormat::Yuy2,
                                  VideoPixelFormat::P010,
                                  VideoPixelFormat::Mjpeg}) {
            std::vector<int> frameRates;
            for (const auto& support : state->pixelFormats) {
                if (support.format == format) {
                    frameRates.push_back(support.selectedFps);
                }
            }
            if (frameRates.empty()) continue;
            std::sort(frameRates.begin(), frameRates.end(), std::greater<int>());
            frameRates.erase(std::unique(frameRates.begin(), frameRates.end()),
                             frameRates.end());
            if (!firstFormat) message += L"\r\n";
            message += PixelFormatName(format);
            message += L"  ";
            for (size_t i = 0; i < frameRates.size(); ++i) {
                if (i != 0) message += L"/";
                message += std::to_wstring(frameRates[i]);
            }
            message += L" fps";
            firstFormat = false;
        }
    }
    SetWindowTextW(state->videoCapabilityStatus, message.c_str());
}

static void PopulateFrameRateCombo(SettingsDialogState* state) {
    if (!state || !state->frameRateCombo) return;
    int desiredFrameRate = g_settings.videoFrameRate;
    const LRESULT oldIndex = SendMessageW(
        state->frameRateCombo, CB_GETCURSEL, 0, 0);
    if (oldIndex >= 0) {
        const LRESULT oldValue = SendMessageW(
            state->frameRateCombo, CB_GETITEMDATA,
            static_cast<WPARAM>(oldIndex), 0);
        if (oldValue != CB_ERR) desiredFrameRate = static_cast<int>(oldValue);
    }
    SendMessageW(state->frameRateCombo, CB_RESETCONTENT, 0, 0);
    if (state->pixelFormats.empty()) {
        const LRESULT noModeIndex = SendMessageW(
            state->frameRateCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(UI_TEXT(L"지원 프레임 없음")));
        SendMessageW(state->frameRateCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(noModeIndex), 0);
        SendMessageW(state->frameRateCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(noModeIndex), 0);
        return;
    }
    const LRESULT autoIndex = SendMessageW(
        state->frameRateCombo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(UI_TEXT(L"자동 선택 (권장 프레임)")));
    SendMessageW(state->frameRateCombo, CB_SETITEMDATA,
                 static_cast<WPARAM>(autoIndex), 0);
    LRESULT selectedIndex = autoIndex;
    const VideoPixelFormat selectedFormat = SelectedPixelFormat(state);
    std::vector<int> frameRates;
    for (const auto& support : state->pixelFormats) {
        if (selectedFormat == VideoPixelFormat::Auto) {
            if (!IsAutoSelectableVideoFormat(support.format)) continue;
        } else if (support.format != selectedFormat) {
            continue;
        }
        frameRates.push_back(support.selectedFps);
    }
    std::sort(frameRates.begin(), frameRates.end(), std::greater<int>());
    frameRates.erase(std::unique(frameRates.begin(), frameRates.end()),
                     frameRates.end());
    for (const int fps : frameRates) {
        wchar_t label[64]{};
        swprintf_s(label, L"%d fps", fps);
        const LRESULT index = SendMessageW(
            state->frameRateCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label));
        SendMessageW(state->frameRateCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(index), fps);
        if (fps == desiredFrameRate) selectedIndex = index;
    }
    SendMessageW(state->frameRateCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selectedIndex), 0);
}

static void PopulatePixelFormatCombo(SettingsDialogState* state) {
    if (!state || !state->pixelFormatCombo || !state->frameRateCombo ||
        !state->videoCombo) return;
    const LRESULT videoIndex = SendMessageW(
        state->videoCombo, CB_GETCURSEL, 0, 0);
    if (videoIndex < 0 || videoIndex >=
                            static_cast<LRESULT>(ARRAYSIZE(kVideoPresets))) {
        return;
    }
    const auto& preset = kVideoPresets[videoIndex];
    VideoPixelFormat desiredFormat = g_settings.pixelFormat;
    if (SendMessageW(state->pixelFormatCombo, CB_GETCOUNT, 0, 0) > 0) {
        desiredFormat = SelectedPixelFormat(state);
    }
    state->pixelFormats = ProbePixelFormats(
        SelectedCaptureDeviceId(state), preset.width, preset.height);
    SendMessageW(state->pixelFormatCombo, CB_RESETCONTENT, 0, 0);
    if (state->pixelFormats.empty()) {
        const LRESULT noModeIndex = SendMessageW(
            state->pixelFormatCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(UI_TEXT(L"지원 포맷 없음")));
        SendMessageW(state->pixelFormatCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(noModeIndex),
                     static_cast<LPARAM>(VideoPixelFormat::Auto));
        SendMessageW(state->pixelFormatCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(noModeIndex), 0);
        PopulateFrameRateCombo(state);
        UpdateVideoCapabilityStatus(state);
        return;
    }
    LRESULT autoIndex = SendMessageW(
        state->pixelFormatCombo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(UI_TEXT(L"자동 선택 (NV12 우선 · 권장)")));
    SendMessageW(state->pixelFormatCombo, CB_SETITEMDATA,
                 static_cast<WPARAM>(autoIndex),
                 static_cast<LPARAM>(VideoPixelFormat::Auto));
    LRESULT selectedIndex = autoIndex;
    for (const auto format : {VideoPixelFormat::Nv12,
                              VideoPixelFormat::Yuy2,
                              VideoPixelFormat::P010,
                              VideoPixelFormat::Mjpeg}) {
        // Visibility follows the device capability report. Auto-selection
        // preference is applied separately and must not hide manual choices.
        const bool available = std::any_of(
            state->pixelFormats.begin(), state->pixelFormats.end(),
            [format](const PixelFormatSupport& support) {
                return support.format == format;
            });
        if (!available) continue;
        const wchar_t* label = format == VideoPixelFormat::Nv12
            ? L"NV12 8-bit 4:2:0"
            : format == VideoPixelFormat::Yuy2
                ? L"YUY2 8-bit 4:2:2"
                : format == VideoPixelFormat::P010
                    ? UI_TEXT(L"P010 10-bit HDR10 (실험적)")
                    : UI_TEXT(L"MJPEG (실험적 압축 호환)");
        const LRESULT index = SendMessageW(
            state->pixelFormatCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label));
        SendMessageW(state->pixelFormatCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(index),
                     static_cast<LPARAM>(format));
        if (format == desiredFormat) selectedIndex = index;
    }
    SendMessageW(state->pixelFormatCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selectedIndex), 0);
    PopulateFrameRateCombo(state);
    UpdateVideoCapabilityStatus(state);
}

static void FinishSettingsDialog(HWND hwnd, SettingsDialogState* state, bool accepted) {
    if (!state) return;

    if (accepted) {
        const VideoPreset previousVideoPreset = g_settings.videoPreset;
        const bool previouslyRelative = g_settings.relativeWindowSize;
        RememberCurrentBufferChoice(state);
        const LRESULT audioIndex = SendMessageW(
            state->audioCombo, CB_GETCURSEL, 0, 0);
        const LRESULT videoIndex = SendMessageW(
            state->videoCombo, CB_GETCURSEL, 0, 0);
        const LRESULT presentationIndex = SendMessageW(
            state->presentationCombo, CB_GETCURSEL, 0, 0);
        const LRESULT scalingIndex = SendMessageW(
            state->scalingCombo, CB_GETCURSEL, 0, 0);
        const LRESULT fullscreenCursorIndex = SendMessageW(
            state->fullscreenCursorCombo, CB_GETCURSEL, 0, 0);
        const LRESULT volumeHudIndex = SendMessageW(
            state->volumeHudCombo, CB_GETCURSEL, 0, 0);
        const LRESULT driftIndex = SendMessageW(
            state->driftCombo, CB_GETCURSEL, 0, 0);
        const LRESULT pcmQueueIndex = SendMessageW(
            state->pcmQueueCombo, CB_GETCURSEL, 0, 0);
        const LRESULT pixelFormatIndex = SendMessageW(
            state->pixelFormatCombo, CB_GETCURSEL, 0, 0);
        const LRESULT mjpegColorIndex = SendMessageW(
            state->mjpegColorCombo, CB_GETCURSEL, 0, 0);
        const LRESULT frameRateIndex = SendMessageW(
            state->frameRateCombo, CB_GETCURSEL, 0, 0);
        const LRESULT languageIndex = SendMessageW(
            state->languageCombo, CB_GETCURSEL, 0, 0);
        if (audioIndex == 1 && !HasExclusiveVerificationForSelection(state)) {
            UpdateExclusiveVerificationUi(state);
            return;
        }
        if (languageIndex >= 0 && languageIndex <= 2) {
            g_settings.uiLanguage = static_cast<UiLanguage>(languageIndex);
        }
        if (audioIndex == 1) {
            g_settings.audioMode = AudioMode::WasapiExclusive;
        } else if (audioIndex == 2 && state->asioAvailable) {
            g_settings.audioMode = AudioMode::Asio;
        } else {
            g_settings.audioMode = AudioMode::WasapiShared;
        }
        if (g_settings.audioMode == AudioMode::Asio) {
            g_settings.asioDriverName = SelectedAsioDriverName(state);
            g_settings.audioOutputDeviceId.clear();
            if (g_settings.asioDriverName.empty()) {
                g_settings.audioMode = AudioMode::WasapiShared;
            }
        } else {
            g_settings.asioDriverName.clear();
        }
        if (videoIndex >= 0 && videoIndex < static_cast<LRESULT>(ARRAYSIZE(kVideoPresets))) {
            g_settings.videoPreset = kVideoPresets[videoIndex].preset;
        }
        g_settings.presentationMode = presentationIndex == 1
                                          ? PresentationMode::VSync
                                          : PresentationMode::AllowTearing;
        g_settings.scalingMode = scalingIndex == 1
            ? ScalingMode::Sharp : ScalingMode::Smooth;
        g_settings.fullscreenCursorMode = fullscreenCursorIndex == 1
            ? FullscreenCursorMode::AlwaysVisible
            : FullscreenCursorMode::AutoHide;
        g_settings.wasapiBufferMs = state->selectedBufferMs;
        if (volumeHudIndex >= 0 && volumeHudIndex <= 3) {
            g_settings.volumeHudPosition =
                static_cast<VolumeHudPosition>(volumeHudIndex);
        }
        if (driftIndex == 1) {
            g_settings.driftCorrection = DriftCorrectionMode::Auto;
        } else if (driftIndex == 2) {
            g_settings.driftCorrection = DriftCorrectionMode::Resample;
        } else {
            g_settings.driftCorrection = DriftCorrectionMode::Off;
        }
        if (pcmQueueIndex >= 0) {
            const LRESULT queueMs = SendMessageW(
                state->pcmQueueCombo, CB_GETITEMDATA,
                static_cast<WPARAM>(pcmQueueIndex), 0);
            if (queueMs != CB_ERR) {
                g_settings.pcmQueueTargetMs = static_cast<int>(queueMs);
            }
        }
        if (audioIndex == 0 && state->probeReady.load(std::memory_order_acquire) &&
            state->probe.supported) {
            g_settings.wasapiSharedPeriodFrames =
                state->selectedSharedPeriodFrames;
        }
        g_settings.audioOutputDeviceId = SelectedAudioEndpointId(state);
        if (audioIndex == 1) {
            // Persist the result for the endpoint the user actually chose.
            // The list may have tested other outputs in this dialog, but only
            // this endpoint can be used by the immediate-start profile.
            g_settings.exclusiveVerifiedEndpointId =
                EffectiveSelectedAudioEndpointId(state);
            g_settings.exclusiveVerifiedBufferMs =
                ExclusiveVerifiedBufferForSelection(state);
        } else {
            g_settings.exclusiveVerifiedEndpointId =
                state->exclusiveVerifiedEndpointId;
            g_settings.exclusiveVerifiedBufferMs =
                state->exclusiveVerifiedBufferMs;
        }
        g_settings.captureDeviceId = SelectedCaptureDeviceId(state);
        g_settings.captureAudioDeviceId = SelectedCaptureAudioDeviceId(state);
        if (pixelFormatIndex >= 0) {
            const LRESULT value = SendMessageW(
                state->pixelFormatCombo, CB_GETITEMDATA,
                static_cast<WPARAM>(pixelFormatIndex), 0);
            g_settings.pixelFormat = value == CB_ERR
                ? VideoPixelFormat::Auto
                : static_cast<VideoPixelFormat>(value);
        }
        if (frameRateIndex >= 0) {
            const LRESULT value = SendMessageW(
                state->frameRateCombo, CB_GETITEMDATA,
                static_cast<WPARAM>(frameRateIndex), 0);
            g_settings.videoFrameRate = value == CB_ERR
                                            ? 0 : static_cast<int>(value);
        }
        if (mjpegColorIndex >= 0) {
            const LRESULT value = SendMessageW(
                state->mjpegColorCombo, CB_GETITEMDATA,
                static_cast<WPARAM>(mjpegColorIndex), 0);
            if (value != CB_ERR) {
                g_settings.mjpegColorOverride =
                    static_cast<llcv::video_color::Override>(value);
            }
        }
        g_settings.saveLog = SendMessageW(
            state->saveLogCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.showDiagnosticConsole = SendMessageW(
            state->showConsoleCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.skipStartupSettings = SendMessageW(
            state->skipStartupCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.checkForUpdates = SendMessageW(
            state->checkForUpdatesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.muteWhenBackground = SendMessageW(
            state->muteBackgroundCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.audioOnly = SendMessageW(
            state->audioOnlyCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        // The override only has meaning on an explicit P010 selection. Clear
        // any older saved value when switching back to a normal SDR format.
        g_settings.forceHdr10 =
            g_settings.pixelFormat == VideoPixelFormat::P010 &&
            SendMessageW(state->forceHdr10Check, BM_GETCHECK, 0, 0) ==
                BST_CHECKED;
        g_settings.allowVolumeBoost = SendMessageW(
            state->volumeBoostCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!g_settings.allowVolumeBoost &&
            g_volumePercent.load(std::memory_order_acquire) > 100) {
            g_volumePercent.store(100, std::memory_order_release);
        }
        g_leftVolumePercent.store((std::min)(
            100, g_leftVolumePercent.load(std::memory_order_acquire)),
            std::memory_order_release);
        g_rightVolumePercent.store((std::min)(
            100, g_rightVolumePercent.load(std::memory_order_acquire)),
            std::memory_order_release);
        g_settings.volumePercent = g_volumePercent.load(
            std::memory_order_acquire);
        g_settings.leftVolumePercent = g_leftVolumePercent.load(
            std::memory_order_acquire);
        g_settings.rightVolumePercent = g_rightVolumePercent.load(
            std::memory_order_acquire);
        g_settings.pixelPerfect = SendMessageW(
            state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.relativeWindowSize = SendMessageW(
            state->relativeSizeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (g_settings.relativeWindowSize) {
            if (!previouslyRelative ||
                previousVideoPreset != g_settings.videoPreset ||
                g_settings.relativeWindowScalePpm <= 0) {
                // The settings dialog can be moved independently. Relative
                // viewer sizing must remain based on the monitor where the
                // viewer will reopen, not the dialog's current monitor.
                HMONITOR baselineMonitor = state->viewerMonitor;
                if (!baselineMonitor) {
                    baselineMonitor = SavedViewerMonitor();
                }
                if (!baselineMonitor) {
                    baselineMonitor = MonitorFromPoint(
                        POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
                }
                g_settings.relativeWindowScalePpm = RelativeScaleForMonitor(
                    baselineMonitor);
            }
        }
        g_settings.borderlessWindow = SendMessageW(
            state->borderlessCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.windowSnap = SendMessageW(
            state->windowSnapCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        // The user has explicitly accepted a new settings profile, so do not
        // preserve a stale immediate-start fallback from the previous run.
        g_exclusiveStartupFallback = false;
        SaveSettings();
    }

    // Stop the background per-endpoint scan only when the dialog is actually
    // closing. A disabled Start button must leave the scan running so its
    // result can eventually enable a compatible output.
    state->exclusiveProbeStop.store(true, std::memory_order_release);
    state->accepted = accepted;
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<SettingsDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (msg) {
    case WM_CTLCOLORSTATIC:
        if (state && reinterpret_cast<HWND>(lParam) ==
                         state->versionWatermark) {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, RGB(145, 145, 145));
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(
                GetSysColorBrush(COLOR_BTNFACE));
        }
        break;

    case WM_CREATE: {
        const HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;
        auto makeLabel = [&](const wchar_t* text, int x, int y) {
            return CreateWindowExW(0, L"STATIC", text,
                                   WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                   x, y, 160, 24, hwnd, nullptr, instance, nullptr);
        };

        state->tabControl = CreateWindowExW(
            0, WC_TABCONTROLW, nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FIXEDWIDTH,
            24, 16, 901, 31, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_TAB)),
            instance, nullptr);
        if (state->tabControl) {
            const wchar_t* labels[] = {
                UI_TEXT(L"오디오"), UI_TEXT(L"영상 · 창"),
                UI_TEXT(L"단축키 · 진단"), UI_TEXT(L"업데이트")};
            for (int i = 0; i < static_cast<int>(ARRAYSIZE(labels)); ++i) {
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = const_cast<LPWSTR>(labels[i]);
                TabCtrl_InsertItem(state->tabControl, i, &item);
            }
            TabCtrl_SetCurSel(state->tabControl,
                              static_cast<int>(state->activeTab));
        }

        state->audioOutputSection = makeLabel(UI_TEXT(L"출력"), 34, 62);
        state->audioPlaybackSection = makeLabel(UI_TEXT(L"재생 · 편의"), 34, 226);
        state->audioStabilitySection = makeLabel(UI_TEXT(L"동기화 · 안정성"), 34, 392);
        state->videoCaptureSection = makeLabel(UI_TEXT(L"캡처"), 34, 62);
        state->videoDisplaySection = makeLabel(UI_TEXT(L"영상"), 505, 62);
        state->videoWindowSection = makeLabel(UI_TEXT(L"창"), 505, 184);
        state->audioLabel = makeLabel(UI_TEXT(L"오디오 출력 모드"), 24, 24);
        state->audioCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 20, 210, 120, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_AUDIO)), instance, nullptr);
        SendMessageW(state->audioCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         UI_TEXT(L"WASAPI Shared (호환성 우선 · 권장)")));
        SendMessageW(state->audioCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(UI_TEXT(
                         L"WASAPI Exclusive (이벤트 진단 · 장치 독점)")));
        if (state->asioAvailable) {
            SendMessageW(state->audioCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(UI_TEXT(
                             L"ASIO (지연 최소화 · 드라이버 필요 · 실험적)")));
        }
        const LRESULT audioSelection =
            g_settings.audioMode == AudioMode::Asio && state->asioAvailable
                ? 2
                : g_settings.audioMode == AudioMode::WasapiExclusive ? 1 : 0;
        SendMessageW(state->audioCombo, CB_SETCURSEL,
                      audioSelection, 0);

        state->audioOutputLabel = makeLabel(UI_TEXT(L"오디오 출력 장치"), 24, 68);
        state->audioOutputCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            150, 64, 250, 220, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_AUDIO_OUTPUT)),
            instance, nullptr);
        PopulateAudioOutputCombo(state);

        state->selectedBufferMs = g_settings.wasapiBufferMs;
        state->selectedSharedPeriodFrames =
            g_settings.wasapiSharedPeriodFrames;

        state->bufferLabel = makeLabel(UI_TEXT(L"오디오 출력 버퍼"), 24, 68);
        state->bufferCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 64, 210, 180, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_BUFFER)),
            instance, nullptr);
        PopulateSettingsBufferCombo(state);

        state->audioStatus = CreateWindowExW(
            0, L"STATIC", UI_TEXT(L"Shared 저지연 지원 확인 중…"),
            WS_CHILD | WS_VISIBLE,
            24, 104, 370, 22, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_AUDIO_STATUS)),
            instance, nullptr);
        state->exclusiveTestButton = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"독점 버퍼 검사"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            350, 104, 125, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_EXCLUSIVE_TEST)),
            instance, nullptr);

        state->volumeHudLabel = makeLabel(UI_TEXT(L"볼륨 HUD 위치"), 24, 142);
        state->volumeHudCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 138, 210, 160, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_VOLUME_HUD)),
            instance, nullptr);
        for (const wchar_t* label : {UI_TEXT(L"좌측 상단 (기본)"), UI_TEXT(L"우측 상단"),
                                     UI_TEXT(L"좌측 하단"), UI_TEXT(L"우측 하단")}) {
            SendMessageW(state->volumeHudCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(label));
        }
        SendMessageW(
            state->volumeHudCombo, CB_SETCURSEL,
            static_cast<WPARAM>(g_settings.volumeHudPosition), 0);

        state->volumeBoostCheck = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"100% 이상 볼륨 증폭 허용 (최대 200%)"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 230, 400, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_VOLUME_BOOST)),
            instance, nullptr);
        SendMessageW(state->volumeBoostCheck, BM_SETCHECK,
                     g_settings.allowVolumeBoost
                         ? BST_CHECKED : BST_UNCHECKED, 0);
        state->volumeBoostHelp = CreateWindowExW(
            0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                BS_PUSHBUTTON | BS_NOTIFY,
            438, 226, 24, 24, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_VOLUME_BOOST_HELP)),
            instance, nullptr);
        AddSettingsTooltip(
            state, hwnd, state->volumeBoostHelp,
            SettingsHelpText(SettingsHelpTopic::VolumeBoost));

        state->driftLabel = makeLabel(UI_TEXT(L"클록 드리프트 보정"), 24, 226);
        state->driftHelp = CreateWindowExW(
            0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                BS_PUSHBUTTON,
            162, 222, 24, 24, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_DRIFT_HELP)),
            instance, nullptr);
        AddSettingsTooltip(
            state, hwnd, state->driftHelp,
            SettingsHelpText(SettingsHelpTopic::Drift));
        state->driftCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            192, 222, 228, 120, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_DRIFT)),
            instance, nullptr);
        SendMessageW(state->driftCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         UI_TEXT(L"끔 (원본 PCM · 음질 우선)")));
        SendMessageW(state->driftCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         UI_TEXT(L"자동 (권장 · 필요 시 보정)")));
        SendMessageW(state->driftCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         UI_TEXT(L"켬 (항상 리샘플링)")));
        SendMessageW(
            state->driftCombo, CB_SETCURSEL,
            g_settings.driftCorrection == DriftCorrectionMode::Resample
                ? 2
                : g_settings.driftCorrection == DriftCorrectionMode::Auto
                      ? 1
                      : 0,
            0);

        state->pcmQueueLabel = makeLabel(UI_TEXT(L"PCM 버퍼 목표"), 24, 274);
        state->pcmQueueHelp = CreateWindowExW(
            0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                BS_PUSHBUTTON,
            162, 270, 24, 24, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_PCM_QUEUE_HELP)),
            instance, nullptr);
        AddSettingsTooltip(
            state, hwnd, state->pcmQueueHelp,
            SettingsHelpText(SettingsHelpTopic::PcmQueue));
        state->pcmQueueCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 270, 210, 140, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_PCM_QUEUE)),
            instance, nullptr);
        const wchar_t* queueLabels[] = {
            UI_TEXT(L"10 ms (최저 지연)"),
            UI_TEXT(L"15 ms (저지연 목표)"),
            UI_TEXT(L"20 ms (안정 권장)"),
            UI_TEXT(L"25 ms (안정 여유)"),
            UI_TEXT(L"30 ms (안정성 우선)" )};
        size_t selectedQueue = 0;
        for (size_t i = 0; i < ARRAYSIZE(kPcmQueueOptionsMs); ++i) {
            const LRESULT index = SendMessageW(
                state->pcmQueueCombo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(queueLabels[i]));
            SendMessageW(state->pcmQueueCombo, CB_SETITEMDATA,
                         static_cast<WPARAM>(index),
                         kPcmQueueOptionsMs[i]);
            if (g_settings.pcmQueueTargetMs == kPcmQueueOptionsMs[i]) {
                selectedQueue = i;
            }
        }
        SendMessageW(state->pcmQueueCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(selectedQueue), 0);

        state->muteBackgroundCheck = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"백그라운드에서 자동 음소거"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 358, 390, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_MUTE_BACKGROUND)),
            instance, nullptr);
        SendMessageW(state->muteBackgroundCheck, BM_SETCHECK,
                     g_settings.muteWhenBackground
                         ? BST_CHECKED : BST_UNCHECKED, 0);

        state->audioOnlyCheck = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"오디오 only 모드"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 146, 451, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_AUDIO_ONLY)),
            instance, nullptr);
        SendMessageW(state->audioOnlyCheck, BM_SETCHECK,
                     g_settings.audioOnly ? BST_CHECKED : BST_UNCHECKED, 0);

        state->languageLabel = makeLabel(
            UI_TEXT(L"언어 / Language"), 24, 392);
        state->languageCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 388, 210, 120, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_LANGUAGE)),
            instance, nullptr);
        SendMessageW(state->languageCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Auto (Windows language)"));
        SendMessageW(state->languageCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"한국어"));
        SendMessageW(state->languageCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"English"));
        SendMessageW(state->languageCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(g_settings.uiLanguage), 0);

        state->skipStartupCheck = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"다음 실행부터 바로 시작"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 436, 451, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_SKIP_STARTUP)),
            instance, nullptr);
        SendMessageW(state->skipStartupCheck, BM_SETCHECK,
                     g_settings.skipStartupSettings
                         ? BST_CHECKED : BST_UNCHECKED, 0);
        state->skipStartupHint = CreateWindowExW(
            0, L"STATIC", UI_TEXT(
                L"저장된 설정으로 바로 실행 · Shift 실행 또는 F2로 설정 열기"),
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            44, 424, 431, 42, hwnd, nullptr, instance, nullptr);
        state->checkForUpdatesCheck = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"업데이트 자동 확인 (시작 후 백그라운드)"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 466, 451, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_UPDATE_CHECK)),
            instance, nullptr);
        SendMessageW(state->checkForUpdatesCheck, BM_SETCHECK,
                     g_settings.checkForUpdates
                         ? BST_CHECKED : BST_UNCHECKED, 0);

        state->guideShortcutsTitle = makeLabel(
            UI_TEXT(L"단축키"), 34, 62);
        state->guideText = CreateWindowExW(
            0, L"STATIC", UI_TEXT(
                L"F2  설정 다시 열기\r\nF3  오디오 OSD\r\n"
                L"F5  Pixel-perfect 크기로 맞추기\r\n"
                L"F11  보더리스 전체화면 켜기/끄기\r\n"
                L"Tab  실시간 진단 표시\r\nEsc  전체화면 해제 또는 종료"),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            34, 84, 400, 220, hwnd, nullptr, instance, nullptr);
        state->guideDiagnosticsTitle = makeLabel(
            UI_TEXT(L"진단 · 문제 해결"), 505, 62);
        state->guideDiagnosticsText = CreateWindowExW(
            0, L"STATIC", UI_TEXT(
                L"문제가 생길 때만 로그 저장을 켜고 같은 문제를 재현하세요.\r\n"
                L"로그는 사용자 폴더의 logs에 저장됩니다."),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            505, 84, 360, 70, hwnd, nullptr, instance, nullptr);
        state->guideLogFolderButton = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"로그 폴더 열기"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            505, 248, 165, 26, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_OPEN_LOG_FOLDER)),
            instance, nullptr);
        std::wstring versionLabel = UI_TEXT(L"현재 버전");
        versionLabel += L"  ";
        versionLabel += kAppVersionLabel;
        state->updateTitle = CreateWindowExW(
            0, L"STATIC", versionLabel.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            34, 76, 400, 24, hwnd, nullptr, instance, nullptr);
        state->updateText = CreateWindowExW(
            0, L"STATIC", UI_TEXT(
                L"자동 확인은 시작 후 백그라운드에서 최신 릴리스를 확인합니다. "
                L"새 버전이 있으면 공식 설치 파일 다운로드를 안내합니다."),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            34, 110, 760, 70, hwnd, nullptr, instance, nullptr);
        state->updateNowButton = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"최신 버전 확인"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            34, 230, 185, 30, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_UPDATE_NOW)),
            instance, nullptr);
        state->updateStatus = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            235, 234, 650, 24, hwnd, nullptr, instance, nullptr);
        state->versionWatermark = CreateWindowExW(
            0, L"STATIC", kAppVersionLabel,
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            24, 596, 260, 20, hwnd, nullptr, instance, nullptr);

        state->presentationLabel = makeLabel(UI_TEXT(L"화면 표시 방식"), 24, 274);
        state->presentationCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 270, 210, 120, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_PRESENTATION)),
            instance, nullptr);
        SendMessageW(state->presentationCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         llcv::presentation_ui::ImmediateLabel(
                             IsEnglishUi())));
        SendMessageW(state->presentationCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         llcv::presentation_ui::VSyncLabel(
                             IsEnglishUi())));
        SendMessageW(state->presentationCombo, CB_SETCURSEL,
                     g_settings.presentationMode == PresentationMode::VSync
                         ? 1 : 0, 0);
        state->presentationHelp = CreateWindowExW(
            0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                BS_PUSHBUTTON,
            604, 20, 24, 24, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_PRESENTATION_HELP)),
            instance, nullptr);
        AddSettingsTooltip(
            state, hwnd, state->presentationHelp,
            SettingsHelpText(SettingsHelpTopic::Presentation));

        state->captureDeviceLabel = makeLabel(UI_TEXT(L"캡처 장치"), 430, 68);
        state->captureDeviceCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            550, 64, 245, 220, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_CAPTURE_DEVICE)),
            instance, nullptr);
        SendMessageW(state->captureDeviceCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         UI_TEXT(L"자동 선택 (GC573 우선 · 권장)")));
        LRESULT selectedCaptureDevice = 0;
        for (size_t i = 0; i < state->captureDevices.size(); ++i) {
            std::wstring label = state->captureDevices[i].name;
            std::wstring lowered = label;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           ::towlower);
            if (lowered.find(L"gc573") == std::wstring::npos &&
                lowered.find(L"live gamer 4k") == std::wstring::npos) {
                label += UI_TEXT(L" (실험적)");
            }
            SendMessageW(state->captureDeviceCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(label.c_str()));
            if (state->captureDevices[i].id == g_settings.captureDeviceId) {
                selectedCaptureDevice = static_cast<LRESULT>(i + 1);
            }
        }
        SendMessageW(state->captureDeviceCombo, CB_SETCURSEL,
                     selectedCaptureDevice, 0);

        state->captureAudioDeviceLabel = makeLabel(
            UI_TEXT(L"캡처 오디오 장치"), 430, 112);
        state->captureAudioDeviceCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            550, 108, 245, 220, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_CAPTURE_AUDIO_DEVICE)),
            instance, nullptr);
        SendMessageW(state->captureAudioDeviceCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(UI_TEXT(
                         L"자동 선택 (영상 장치 오디오 우선 · 권장)")));
        LRESULT selectedCaptureAudioDevice = 0;
        for (size_t i = 0; i < state->captureAudioDevices.size(); ++i) {
            const std::wstring& label = state->captureAudioDevices[i].name;
            SendMessageW(state->captureAudioDeviceCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(label.c_str()));
            if (state->captureAudioDevices[i].id ==
                g_settings.captureAudioDeviceId) {
                selectedCaptureAudioDevice = static_cast<LRESULT>(i + 1);
            }
        }
        SendMessageW(state->captureAudioDeviceCombo, CB_SETCURSEL,
                     selectedCaptureAudioDevice, 0);
        state->captureAudioStatus = CreateWindowExW(
            0, L"STATIC", UI_TEXT(L"내부 오디오 확인 중…"),
            WS_CHILD | SS_LEFTNOWORDWRAP,
            550, 108, 245, 24, hwnd, nullptr, instance, nullptr);

        state->videoLabel = makeLabel(UI_TEXT(L"캡처 해상도"), 430, 156);
        state->videoCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            550, 152, 245, 120, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_VIDEO)), instance, nullptr);
        for (const auto& info : kVideoPresets) {
            SendMessageW(state->videoCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(info.label));
        }
        size_t selectedVideo = 0;
        for (size_t i = 0; i < ARRAYSIZE(kVideoPresets); ++i) {
            if (kVideoPresets[i].preset == state->initialVideoPreset) {
                selectedVideo = i;
                break;
            }
        }
        SendMessageW(state->videoCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(selectedVideo), 0);

        state->pixelFormatLabel = makeLabel(UI_TEXT(L"픽셀 포맷"), 430, 156);
        state->pixelFormatCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            550, 152, 245, 160, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_PIXEL_FORMAT)),
            instance, nullptr);
        state->frameRateLabel = makeLabel(UI_TEXT(L"프레임"), 430, 200);
        state->frameRateCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            550, 196, 245, 200, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_FRAME_RATE)),
            instance, nullptr);
        state->videoCapabilityStatus = CreateWindowExW(
            0, L"STATIC", UI_TEXT(L"지원 모드 확인 중..."),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            430, 234, 365, 90, hwnd, nullptr, instance, nullptr);
        PopulatePixelFormatCombo(state);

        state->scalingLabel = makeLabel(UI_TEXT(L"화면 확대 방식"), 430, 274);
        state->scalingCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            550, 270, 245, 120, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_SCALING)),
            instance, nullptr);
        SendMessageW(state->scalingCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(UI_TEXT(L"부드럽게")));
        SendMessageW(state->scalingCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(UI_TEXT(L"선명하게")));
        SendMessageW(state->scalingCombo, CB_SETCURSEL,
                     g_settings.scalingMode == ScalingMode::Sharp ? 1 : 0, 0);

        state->fullscreenCursorLabel = makeLabel(
            UI_TEXT(L"전체화면 커서"), 505, 336);
        state->fullscreenCursorCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                CBS_DROPDOWNLIST | WS_TABSTOP,
            630, 332, 255, 120, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(
                IDC_SETTINGS_FULLSCREEN_CURSOR)),
            instance, nullptr);
        SendMessageW(state->fullscreenCursorCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(UI_TEXT(L"자동 숨김 (권장)")));
        SendMessageW(state->fullscreenCursorCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(UI_TEXT(L"항상 표시")));
        SendMessageW(state->fullscreenCursorCombo, CB_SETCURSEL,
                     g_settings.fullscreenCursorMode ==
                             FullscreenCursorMode::AlwaysVisible
                         ? 1 : 0,
                     0);
        state->fullscreenCursorHint = CreateWindowExW(
            0, L"STATIC",
            UI_TEXT(L"F11  보더리스 전체화면 켜기/끄기"),
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            630, 364, 255, 24, hwnd, nullptr, instance, nullptr);

        state->forceHdr10Check = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(
                L"P010 HDR10 강제 (메타데이터 없을 때 · 실험적)"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            505, 376, 390, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_FORCE_HDR10)),
            instance, nullptr);
        SendMessageW(state->forceHdr10Check, BM_SETCHECK,
                     g_settings.forceHdr10 ? BST_CHECKED : BST_UNCHECKED, 0);
        state->forceHdr10Help = CreateWindowExW(
            0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                BS_PUSHBUTTON,
            900, 372, 24, 24, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_FORCE_HDR10_HELP)),
            instance, nullptr);
        AddSettingsTooltip(
            state, hwnd, state->forceHdr10Help,
            SettingsHelpText(SettingsHelpTopic::ForceHdr10));

        state->mjpegColorLabel = makeLabel(
            UI_TEXT(L"MJPEG 색상 해석"), 24, 376);
        state->mjpegColorCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            190, 372, 240, 150, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_MJPEG_COLOR)),
            instance, nullptr);
        const struct {
            const wchar_t* label;
            llcv::video_color::Override value;
        } mjpegColorChoices[] = {
            {UI_TEXT(L"자동 (권장)"), llcv::video_color::Override::Auto},
            {L"BT.709 · Full range", llcv::video_color::Override::Bt709Full},
            {L"BT.709 · Limited range", llcv::video_color::Override::Bt709Limited},
            {L"BT.601 · Full range", llcv::video_color::Override::Bt601Full},
            {L"BT.601 · Limited range", llcv::video_color::Override::Bt601Limited},
        };
        LRESULT selectedMjpegColor = 0;
        for (const auto& choice : mjpegColorChoices) {
            const LRESULT index = SendMessageW(
                state->mjpegColorCombo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(choice.label));
            SendMessageW(state->mjpegColorCombo, CB_SETITEMDATA,
                         static_cast<WPARAM>(index),
                         static_cast<LPARAM>(choice.value));
            if (choice.value == g_settings.mjpegColorOverride) {
                selectedMjpegColor = index;
            }
        }
        SendMessageW(state->mjpegColorCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(selectedMjpegColor), 0);
        state->mjpegColorHelp = CreateWindowExW(
            0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                BS_PUSHBUTTON,
            438, 372, 24, 24, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_MJPEG_COLOR_HELP)),
            instance, nullptr);
        AddSettingsTooltip(
            state, hwnd, state->mjpegColorHelp,
            SettingsHelpText(SettingsHelpTopic::MjpegColor));

        state->pixelCheck = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"Pixel-perfect (1:1 · 창 크기 고정)"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 362, 250, 28, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_PIXEL)), instance, nullptr);
        SendMessageW(state->pixelCheck, BM_SETCHECK,
                     g_settings.pixelPerfect ? BST_CHECKED : BST_UNCHECKED, 0);

        state->relativeSizeCheck = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"모니터 이동 시 상대적 창 크기 유지 (독립 옵션)"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 396, 390, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_RELATIVE_SIZE)),
            instance, nullptr);
        SendMessageW(state->relativeSizeCheck, BM_SETCHECK,
                     g_settings.relativeWindowSize
                         ? BST_CHECKED : BST_UNCHECKED, 0);

        state->relativeSizeWarning = CreateWindowExW(
            0, L"STATIC",
            UI_TEXT(L"※ Pixel-perfect와 함께 켜면 모니터 이동 시 1:1이 깨질 수 있습니다."),
            WS_CHILD | WS_VISIBLE,
            44, 424, 411, 24, hwnd, nullptr, instance, nullptr);

        state->borderlessCheck = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"제목 표시줄 숨기기 (borderless 창)"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 430, 300, 28, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_BORDERLESS)),
            instance, nullptr);
        SendMessageW(state->borderlessCheck, BM_SETCHECK,
                     g_settings.borderlessWindow ? BST_CHECKED : BST_UNCHECKED, 0);

        state->windowSnapCheck = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"창을 모니터 가장자리에 스냅 (권장)"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            24, 464, 330, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_WINDOW_SNAP)),
            instance, nullptr);
        SendMessageW(state->windowSnapCheck, BM_SETCHECK,
                     g_settings.windowSnap ? BST_CHECKED : BST_UNCHECKED, 0);

        state->saveLogCheck = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"진단 로그 파일 저장 (사용자 폴더)"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            430, 374, 365, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_SAVE_LOG)),
            instance, nullptr);
        SendMessageW(state->saveLogCheck, BM_SETCHECK,
                     g_settings.saveLog ? BST_CHECKED : BST_UNCHECKED, 0);

        state->showConsoleCheck = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"진단 콘솔 창 표시"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            430, 408, 365, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_SHOW_CONSOLE)),
            instance, nullptr);
        SendMessageW(state->showConsoleCheck, BM_SETCHECK,
                     g_settings.showDiagnosticConsole
                         ? BST_CHECKED : BST_UNCHECKED, 0);

        state->startButton = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"시작"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
            285, 520, 80, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_START)), instance, nullptr);
        state->cancelButton = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"취소"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            375, 520, 80, 30, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_CANCEL)), instance, nullptr);
        UpdateVideoCapabilityStatus(state);

        const UINT initialDpi = GetDpiForWindow(hwnd);
        ApplySettingsFont(state, hwnd, initialDpi);
        LayoutSettingsControls(state, initialDpi);
        UpdateCaptureAudioSelectionUi(state);
        UpdateAdvancedControlVisibility(state);
        UpdateAsioControlVisibility(state);
        UpdateExclusiveProbeControl(state);
        UpdateExclusiveVerificationUi(state);
        RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                         RDW_UPDATENOW);

        const std::wstring endpointId = SelectedAudioEndpointId(state);
        state->probeThread = std::thread([state, hwnd, endpointId]() {
            state->probe =
                llcv::audio_device::ProbeSharedModeSupport(endpointId);
            state->probeReady.store(true, std::memory_order_release);
            PostMessageW(hwnd, WM_AUDIOCLIENT3_PROBE_COMPLETE, 0, 0);
        });
        StartCaptureAudioProbe(state, hwnd);
        if (SettingsUsesExclusiveMode(state)) {
            StartExclusiveEndpointScan(state, hwnd);
        }
        return 0;
    }

    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ApplySettingsFont(state, hwnd, HIWORD(wParam));
        LayoutSettingsControls(state, HIWORD(wParam));
        RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                         RDW_UPDATENOW);
        return 0;
    }

    case WM_GETDPISCALEDSIZE:
        if (lParam) {
            *reinterpret_cast<SIZE*>(lParam) =
                SettingsDialogOuterSize(hwnd, static_cast<UINT>(wParam), state);
            return TRUE;
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                             RDW_UPDATENOW);
        }
        break;

    case WM_AUDIOCLIENT3_PROBE_COMPLETE:
        // The endpoint's Shared-mode capability probe still runs in the
        // background after an output-device change, but it must not overwrite
        // the meaningful Exclusive compatibility result shown to the user.
        if (SettingsUsesExclusiveMode(state)) {
            UpdateExclusiveVerificationUi(state);
        } else {
            UpdateAudioClient3Status(state);
        }
        if (SettingsUsesSharedMode(state)) {
            RememberCurrentBufferChoice(state);
            PopulateSettingsBufferCombo(state);
        }
        return 0;

    case WM_EXCLUSIVE_ENDPOINT_PROBE_COMPLETE: {
        std::unique_ptr<ExclusiveEndpointProbeMessage> message(
            reinterpret_cast<ExclusiveEndpointProbeMessage*>(wParam));
        if (!state || !message ||
            message->endpointIndex >= state->exclusiveEndpointResults.size()) {
            return 0;
        }
        auto& result = state->exclusiveEndpointResults[message->endpointIndex];
        result.state = message->probe.compatible
            ? ExclusiveEndpointState::Supported
            : ExclusiveEndpointState::Unsupported;
        result.recommendedBufferMs = message->probe.compatible
            ? static_cast<int>((message->probe.requestedFrames * 1000 +
                                kSampleRate / 2) / kSampleRate)
            : 0;
        result.summary = message->probe.summary;
        ++state->exclusiveScanCompleted;
        fwprintf(stderr,
                 L"[audio][exclusive-scan] %s: %s | requested=%u frames "
                 L"actual=%u frames\n",
                 state->audioEndpoints[message->endpointIndex].name.c_str(),
                 result.summary.c_str(), message->probe.requestedFrames,
                 message->probe.actualBufferFrames);
        PopulateAudioOutputCombo(state);
        const int recommendedBufferMs =
            ExclusiveVerifiedBufferForSelection(state);
        if (IsExclusiveLowLatencyBuffer(recommendedBufferMs)) {
            state->selectedBufferMs = recommendedBufferMs;
            PopulateSettingsBufferCombo(state);
        }
        UpdateExclusiveProbeControl(state);
        UpdateExclusiveVerificationUi(state);
        return 0;
    }

    case WM_EXCLUSIVE_SCAN_COMPLETE:
        if (state) {
            if (state->exclusiveProbeThread.joinable()) {
                state->exclusiveProbeThread.join();
            }
            if (state->exclusiveProbeStop.load(std::memory_order_acquire)) {
                // A canceled scan has no verdict for endpoints that did not
                // reach their probe yet. Never label them as unavailable or
                // completed merely because the user changed modes/closed UI.
                for (auto& result : state->exclusiveEndpointResults) {
                    if (result.state == ExclusiveEndpointState::Testing) {
                        result.state = ExclusiveEndpointState::Unknown;
                        result.summary.clear();
                    }
                }
            } else if (state->exclusiveScanCompleted >=
                       state->audioEndpoints.size()) {
                PersistCompletedExclusiveEndpointResults(state);
            }
            PopulateAudioOutputCombo(state);
            UpdateExclusiveProbeControl(state);
            UpdateExclusiveVerificationUi(state);
        }
        return 0;

    case WM_CAPTURE_AUDIO_PROBE_COMPLETE:
        if (state) {
            EnableWindow(state->captureDeviceCombo, TRUE);
            UpdateCaptureAudioSelectionUi(state);
        }
        return 0;

    case WM_SETTINGS_UPDATE_CHECK_COMPLETE: {
        std::unique_ptr<UpdateCheckResult> result(
            reinterpret_cast<UpdateCheckResult*>(lParam));
        if (!state) return 0;
        state->updateCheckRunning.store(false, std::memory_order_release);
        if (state->updateCheckThread.joinable()) {
            state->updateCheckThread.join();
        }
        EnableWindow(state->updateNowButton, TRUE);
        if (!result || !result->success) {
            SetSettingsUpdateStatus(
                state, UI_TEXT(L"업데이트를 확인하지 못했습니다. 인터넷 연결을 확인한 뒤 다시 시도하세요."));
            return 0;
        }
        if (!result->newer || result->installerUrl.empty()) {
            std::wstring status = UI_TEXT(L"최신 버전입니다.");
            if (!result->latestTag.empty()) {
                wchar_t detail[160]{};
                swprintf_s(detail, UI_TEXT(L"최신 버전: %s"),
                           result->latestTag.c_str());
                status += L"  ";
                status += detail;
            }
            SetSettingsUpdateStatus(state, status);
            return 0;
        }

        wchar_t status[200]{};
        swprintf_s(status, UI_TEXT(L"최신 버전: %s"),
                   result->latestTag.c_str());
        SetSettingsUpdateStatus(state, status);
        wchar_t message[360]{};
        swprintf_s(message,
                   UI_TEXT(L"새 버전 %s을(를) 찾았습니다. 공식 설치 파일을 다운로드하시겠습니까?"),
                   result->latestTag.c_str());
        if (MessageBoxW(hwnd, message, UI_TEXT(L"업데이트 확인"),
                        MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON1) == IDYES) {
            ShellExecuteW(hwnd, L"open", result->installerUrl.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

    case WM_SETTINGS_TOOLTIP_SHOW: {
        const HWND target = reinterpret_cast<HWND>(wParam);
        const HWND tooltip = reinterpret_cast<HWND>(lParam);
        if (state && IsSettingsHelpControl(state, target) &&
            tooltip == state->tooltipWindow) {
            if (state->activeTooltipTarget &&
                state->activeTooltipTarget != target) {
                TrackSettingsTooltip(state->activeTooltipTarget, tooltip,
                                     false);
            }
            POINT cursor{};
            GetCursorPos(&cursor);
            SendMessageW(tooltip, TTM_TRACKPOSITION, 0,
                         MAKELPARAM(cursor.x + 16, cursor.y + 20));
            TrackSettingsTooltip(target, tooltip, true);
            state->activeTooltipTarget = target;
        }
        return 0;
    }

    case WM_SETTINGS_TOOLTIP_HIDE: {
        const HWND target = reinterpret_cast<HWND>(wParam);
        const HWND tooltip = reinterpret_cast<HWND>(lParam);
        if (state && target == state->activeTooltipTarget &&
            IsSettingsHelpControl(state, target) &&
            tooltip == state->tooltipWindow) {
            RECT targetRect{};
            POINT cursor{};
            GetWindowRect(target, &targetRect);
            GetCursorPos(&cursor);
            if (PtInRect(&targetRect, cursor)) return 0;
            TrackSettingsTooltip(target, tooltip, false);
            state->activeTooltipTarget = nullptr;
        }
        return 0;
    }

    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (state && header && header->idFrom == IDC_SETTINGS_TAB &&
            header->code == TCN_SELCHANGE) {
            const int index = TabCtrl_GetCurSel(state->tabControl);
            if (index >= static_cast<int>(SettingsTab::Audio) &&
                index <= static_cast<int>(SettingsTab::Updates)) {
                state->activeTab = static_cast<SettingsTab>(index);
                const UINT dpi = GetDpiForWindow(hwnd);
                LayoutSettingsControls(state, dpi);
                UpdateAdvancedControlVisibility(state);
                UpdateCaptureAudioSelectionUi(state);
                RedrawWindow(hwnd, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                                 RDW_UPDATENOW);
            }
            return 0;
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SETTINGS_AUDIO &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            RememberCurrentBufferChoice(state);
            PopulateAudioOutputCombo(state);
            PopulateSettingsBufferCombo(state);
            UpdateAsioControlVisibility(state);
            UpdateAdvancedControlVisibility(state);
            UpdateAudioClient3Status(state);
            UpdateExclusiveProbeControl(state);
            UpdateExclusiveVerificationUi(state);
            if (SettingsUsesExclusiveMode(state)) {
                StartExclusiveEndpointScan(state, hwnd);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_AUDIO_OUTPUT &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            if (state->probeThread.joinable()) state->probeThread.join();
            state->probeReady.store(false, std::memory_order_release);
            UpdateAudioClient3Status(state);
            const std::wstring endpointId = SelectedAudioEndpointId(state);
            state->probeThread = std::thread([state, hwnd, endpointId]() {
                state->probe =
                    llcv::audio_device::ProbeSharedModeSupport(endpointId);
                state->probeReady.store(true, std::memory_order_release);
                PostMessageW(hwnd, WM_AUDIOCLIENT3_PROBE_COMPLETE, 0, 0);
            });
            const int recommendedBufferMs =
                ExclusiveVerifiedBufferForSelection(state);
            if (SettingsUsesExclusiveMode(state) &&
                IsExclusiveLowLatencyBuffer(recommendedBufferMs)) {
                state->selectedBufferMs = recommendedBufferMs;
                PopulateSettingsBufferCombo(state);
            }
            UpdateExclusiveVerificationUi(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_BUFFER &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            RememberCurrentBufferChoice(state);
            UpdateExclusiveVerificationUi(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_EXCLUSIVE_TEST &&
            HIWORD(wParam) == BN_CLICKED && state) {
            StartExclusiveEndpointScan(state, hwnd, true);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_UPDATE_NOW &&
            HIWORD(wParam) == BN_CLICKED && state) {
            StartSettingsUpdateCheck(state, hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_OPEN_LOG_FOLDER &&
            HIWORD(wParam) == BN_CLICKED) {
            // Create it on demand so users can find the stable location even
            // before their first diagnostic log has been written.
            EnsureUserDataDirectory();
            const std::wstring logDirectory = LogDirectory();
            CreateDirectoryW(logDirectory.c_str(), nullptr);
            const HINSTANCE result = ShellExecuteW(
                hwnd, L"open", logDirectory.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(result) <= 32) {
                MessageBoxW(hwnd,
                            UI_TEXT(L"로그 폴더를 열지 못했습니다."),
                            UI_TEXT(L"진단 로그"),
                            MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_CAPTURE_DEVICE &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            PopulatePixelFormatCombo(state);
            UpdateAdvancedControlVisibility(state);
            StartCaptureAudioProbe(state, hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_VIDEO &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            PopulatePixelFormatCombo(state);
            UpdateAdvancedControlVisibility(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_CAPTURE_AUDIO_DEVICE &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            UpdateCaptureAudioSelectionUi(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_AUDIO_ONLY &&
            HIWORD(wParam) == BN_CLICKED) {
            UpdateVideoCapabilityStatus(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PIXEL &&
            HIWORD(wParam) == BN_CLICKED) {
            LayoutSettingsControls(state, GetDpiForWindow(hwnd));
            UpdateScalingControlVisibility(state);
            UpdateWindowBehaviorVisibility(state);
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                             RDW_UPDATENOW);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_RELATIVE_SIZE &&
            HIWORD(wParam) == BN_CLICKED) {
            LayoutSettingsControls(state, GetDpiForWindow(hwnd));
            UpdateWindowBehaviorVisibility(state);
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                             RDW_UPDATENOW);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PIXEL_FORMAT &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            PopulateFrameRateCombo(state);
            UpdateAdvancedControlVisibility(state);
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                             RDW_UPDATENOW);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PRESENTATION_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::Presentation),
                UI_TEXT(L"화면 표시 방식"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_VOLUME_BOOST_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::VolumeBoost),
                UI_TEXT(L"100% 이상 볼륨 증폭"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_FORCE_HDR10_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::ForceHdr10),
                UI_TEXT(L"HDR10 강제 출력"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_MJPEG_COLOR_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::MjpegColor),
                UI_TEXT(L"MJPEG 색상 해석"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PCM_QUEUE_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::PcmQueue),
                UI_TEXT(L"PCM 버퍼 목표"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_DRIFT_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::Drift),
                UI_TEXT(L"클록 드리프트 보정"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_START) {
            FinishSettingsDialog(hwnd, state, true);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_CANCEL) {
            FinishSettingsDialog(hwnd, state, false);
            return 0;
        }
        break;

    case WM_CLOSE:
        FinishSettingsDialog(hwnd, state, false);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool ShowSettingsDialog(HINSTANCE hInst,
                               bool preferSavedViewerMonitor) {
    static const wchar_t kSettingsClass[] = L"LowLatencyCaptureViewerSettingsDialogClass";
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kSettingsClass;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        registered = true;
    }

    SettingsDialogState state{};
    state.captureDevices = EnumerateCaptureDevices();
    state.captureAudioDevices = EnumerateCaptureAudioDevices();
    state.audioEndpoints =
        llcv::audio_device::EnumerateRenderEndpoints();
    state.asioDrivers = llcv::asio::EnumerateDrivers();
    state.asioAvailable = !state.asioDrivers.empty();
    state.exclusiveVerifiedEndpointId =
        g_settings.exclusiveVerifiedEndpointId;
    state.exclusiveVerifiedBufferMs =
        g_settings.exclusiveVerifiedBufferMs;
    state.exclusiveEndpointResults.assign(
        state.audioEndpoints.size(), ExclusiveEndpointVerification{});
    for (size_t i = 0; i < state.audioEndpoints.size(); ++i) {
        const auto* cached = FindExclusiveEndpointCache(
            state.audioEndpoints[i].id);
        if (cached) {
            state.exclusiveEndpointResults[i].state = cached->supported
                ? ExclusiveEndpointState::Supported
                : ExclusiveEndpointState::Unsupported;
            state.exclusiveEndpointResults[i].recommendedBufferMs =
                cached->supported ? cached->recommendedBufferMs : 0;
            ++state.exclusiveScanCompleted;
        } else if (state.audioEndpoints[i].id ==
                       state.exclusiveVerifiedEndpointId &&
                   IsExclusiveLowLatencyBuffer(
                       state.exclusiveVerifiedBufferMs)) {
            state.exclusiveEndpointResults[i].state =
                ExclusiveEndpointState::Supported;
            state.exclusiveEndpointResults[i].recommendedBufferMs =
                state.exclusiveVerifiedBufferMs;
            ++state.exclusiveScanCompleted;
        }
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR savedViewerMonitor = SavedViewerMonitor();
    HMONITOR settingsMonitor = nullptr;
    if (preferSavedViewerMonitor && savedViewerMonitor) {
        // F2 closes the viewer before reopening settings in a child process.
        // PersistWindowPosition has just saved the viewer's monitor, so keep
        // the settings dialog with that viewer instead of following the mouse
        // cursor to another display.
        settingsMonitor = savedViewerMonitor;
    }
    if (!settingsMonitor) {
        settingsMonitor = MonitorFromPoint(
            cursor, MONITOR_DEFAULTTOPRIMARY);
    }
    state.viewerMonitor = savedViewerMonitor
        ? savedViewerMonitor
        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT settingsDpiX = GetDpiForSystem();
    UINT settingsDpiY = settingsDpiX;
    if (FAILED(GetDpiForMonitor(settingsMonitor, MDT_EFFECTIVE_DPI,
                                &settingsDpiX, &settingsDpiY))) {
        settingsDpiX = GetDpiForSystem();
    }
    const UINT settingsDpi = settingsDpiX;
    const DWORD settingsStyle = WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    const DWORD settingsExStyle = WS_EX_DLGMODALFRAME;
    RECT settingsRect{0, 0,
                      SettingsPixels(kSettingsClientWidthDip, settingsDpi),
                      SettingsPixels(SettingsClientHeightDip(&state), settingsDpi)};
    AdjustWindowRectExForDpi(&settingsRect, settingsStyle, FALSE,
                             settingsExStyle, settingsDpi);
    const SIZE settingsOuter{settingsRect.right - settingsRect.left,
                             settingsRect.bottom - settingsRect.top};
    MONITORINFO settingsMonitorInfo{sizeof(settingsMonitorInfo)};
    GetMonitorInfoW(settingsMonitor, &settingsMonitorInfo);
    state.initialVideoPreset = g_settings.videoPreset;
    const RECT work = settingsMonitorInfo.rcWork;
    const int settingsX = work.left +
        ((work.right - work.left) - settingsOuter.cx) / 2;
    const int settingsY = work.top +
        ((work.bottom - work.top) - settingsOuter.cy) / 2;
    HWND hwnd = CreateWindowExW(
        settingsExStyle,
        kSettingsClass, UI_TEXT(L"Low Latency Capture Viewer 설정"),
        settingsStyle,
        settingsX, settingsY, settingsOuter.cx, settingsOuter.cy,
        nullptr, nullptr, hInst, &state);
    if (!hwnd) return false;

    // F2 reopens settings in a new process after the capture threads have
    // stopped. Briefly promote the dialog while activating it, then return it
    // to the normal z-order so it cannot remain hidden behind the previously
    // foreground application and is never permanently topmost.
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    UpdateWindow(hwnd);
    MSG msg{};
    while (IsWindow(hwnd)) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) break;
        if (msg.message == WM_MOUSEMOVE && state.tooltipWindow) {
            const HWND target = IsSettingsHelpControl(&state, msg.hwnd)
                ? msg.hwnd : state.activeTooltipTarget;
            if (target) {
                SendMessageW(hwnd,
                             IsSettingsHelpControl(&state, msg.hwnd)
                                 ? WM_SETTINGS_TOOLTIP_SHOW
                                 : WM_SETTINGS_TOOLTIP_HIDE,
                             reinterpret_cast<WPARAM>(target),
                             reinterpret_cast<LPARAM>(state.tooltipWindow));
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (state.probeThread.joinable()) state.probeThread.join();
    state.updateCheckStop.store(true, std::memory_order_release);
    if (state.updateCheckThread.joinable()) {
        state.updateCheckThread.join();
    }
    state.exclusiveProbeStop.store(true, std::memory_order_release);
    if (state.exclusiveProbeThread.joinable()) {
        state.exclusiveProbeThread.join();
    }
    if (state.captureAudioProbeThread.joinable()) {
        state.captureAudioProbeThread.join();
    }
    for (HFONT font : state.uiFonts) DeleteObject(font);
    return state.accepted;
}

// -----------------------------------------------------------------------------
// Win32 UI
// -----------------------------------------------------------------------------

// Set when startup automatically fills a monitor whose resolution matches the
// selected capture resolution. Esc exits the viewer directly in that case;
// manually entered F11 fullscreen retains the usual first-Esc-to-windowed
// behavior.
static bool g_autoFullscreen = false;
static WINDOWPLACEMENT g_prevPlacement{ sizeof(g_prevPlacement) };
static LONG_PTR g_prevStyle = 0;
static RECT g_lastWindowedRect{};
static bool g_haveLastWindowedRect = false;
static bool g_windowPositionPersisted = false;
static constexpr int kWindowSnapDistanceDip = 20;
static constexpr int kWindowSnapReleaseDip = 20;

enum class HorizontalSnapEdge { None, Left, Right };
enum class VerticalSnapEdge { None, Top, Bottom };

struct WindowSnapState {
    HorizontalSnapEdge horizontal = HorizontalSnapEdge::None;
    VerticalSnapEdge vertical = VerticalSnapEdge::None;
    int horizontalCursorAnchor = 0;
    int verticalCursorAnchor = 0;
    int horizontalSnapCoordinate = 0;
    int verticalSnapCoordinate = 0;
    bool suppressHorizontal = false;
    bool suppressVertical = false;
};

static WindowSnapState g_windowSnapState{};

static void ResetWindowSnapState() {
    g_windowSnapState = {};
}

static UINT EffectiveMonitorDpi(HMONITOR monitor, HWND fallbackWindow) {
    UINT dpiX = 0;
    UINT dpiY = 0;
    if (monitor && SUCCEEDED(GetDpiForMonitor(
                       monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX) {
        return dpiX;
    }
    const UINT windowDpi = fallbackWindow ? GetDpiForWindow(fallbackWindow) : 0;
    return windowDpi ? windowDpi : USER_DEFAULT_SCREEN_DPI;
}

static SIZE OuterSizeForClientPixels(int clientWidth, int clientHeight,
                                     DWORD style, DWORD exStyle, UINT dpi) {
    RECT rect{0, 0, clientWidth, clientHeight};
    if (!AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi)) {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

static SIZE DesiredClientPixelsForMonitor(HMONITOR monitor) {
    const auto& video = CurrentVideoPreset();
    if (!g_settings.relativeWindowSize) {
        return g_settings.pixelPerfect
                   ? SIZE{video.width, video.height}
                   : SIZE{1280, 720};
    }

    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return SIZE{video.width, video.height};
    }
    int scale = g_settings.relativeWindowScalePpm;
    if (scale <= 0) scale = RelativeScaleForMonitor(monitor);
    const int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
    const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
    const int maximumWidth = (std::max)(320,
        MulDiv(monitorWidth, scale, kRelativeScaleUnit));
    const int maximumHeight = (std::max)(180,
        MulDiv(monitorHeight, scale, kRelativeScaleUnit));
    int clientWidth = maximumWidth;
    int clientHeight = MulDiv(clientWidth, video.height, video.width);
    if (clientHeight > maximumHeight) {
        clientHeight = maximumHeight;
        clientWidth = MulDiv(clientHeight, video.width, video.height);
    }
    return SIZE{(std::max)(1, clientWidth),
                (std::max)(1, clientHeight)};
}

static SIZE InitialClientPixelsForMonitor(HMONITOR monitor) {
    const auto& video = CurrentVideoPreset();
    // Monitor-relative sizing is independent from Pixel-perfect. When it is
    // enabled, restore the same monitor-relative scale that was used before
    // shutdown, including when the saved monitor is a smaller display.
    if (g_settings.relativeWindowSize) {
        return DesiredClientPixelsForMonitor(monitor);
    }
    if (g_settings.pixelPerfect) return SIZE{video.width, video.height};
    return DesiredClientPixelsForMonitor(monitor);
}

static SIZE DesiredWindowOuterSize(HWND hwnd, HMONITOR monitor, UINT dpi) {
    const SIZE client = DesiredClientPixelsForMonitor(monitor);
    const DWORD style =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    return OuterSizeForClientPixels(client.cx, client.cy,
                                    style, exStyle, dpi);
}

static void NormalizeWindowSize(HWND hwnd, bool clampToWorkArea) {
    // Only strict pixel-perfect without monitor-relative behavior is fixed.
    // When both options are enabled, relative sizing is allowed to change the
    // size programmatically as the window crosses monitors.
    if (!hwnd || !g_settings.pixelPerfect ||
        g_settings.relativeWindowSize || g_fullscreen) return;
    RECT current{};
    if (!GetWindowRect(hwnd, &current)) return;
    const HMONITOR currentMonitor =
        MonitorFromRect(&current, MONITOR_DEFAULTTONEAREST);
    const UINT dpi = EffectiveMonitorDpi(currentMonitor, hwnd);
    const SIZE desired = DesiredWindowOuterSize(
        hwnd, currentMonitor, dpi);
    int x = current.left;
    int y = current.top;
    if (clampToWorkArea) {
        MONITORINFO info{sizeof(info)};
        const HMONITOR monitor =
            MonitorFromRect(&current, MONITOR_DEFAULTTONEAREST);
        if (monitor && GetMonitorInfoW(monitor, &info)) {
            const int maximumX =
                (std::max)(info.rcWork.left, info.rcWork.right - desired.cx);
            const int maximumY =
                (std::max)(info.rcWork.top, info.rcWork.bottom - desired.cy);
            x = std::clamp(x, static_cast<int>(info.rcWork.left), maximumX);
            y = std::clamp(y, static_cast<int>(info.rcWork.top), maximumY);
        }
    }

    const int currentWidth = current.right - current.left;
    const int currentHeight = current.bottom - current.top;
    if (currentWidth != desired.cx || currentHeight != desired.cy ||
        current.left != x || current.top != y) {
        SetWindowPos(hwnd, nullptr, x, y, desired.cx, desired.cy,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static HMONITOR g_relativeMoveMonitor = nullptr;
static bool g_manualResizeInProgress = false;
static bool g_outputResizePending = false;
// Window-style changes synchronously emit WM_SIZE while F11/F5 is still
// updating the outer window. Coalesce those notifications so the render
// thread never tears down and recreates the flip-model swap chain halfway
// through a fullscreen transition.
static unsigned int g_outputTransitionDepth = 0;

static void BeginOutputTransition() {
    ++g_outputTransitionDepth;
}

static void EndOutputTransition(bool requestOutputUpdate) {
    if (requestOutputUpdate) g_outputResizePending = true;
    if (g_outputTransitionDepth > 0) --g_outputTransitionDepth;
    if (g_outputTransitionDepth == 0 && g_outputResizePending &&
        !g_manualResizeInProgress) {
        g_outputResizePending = false;
        g_outputConfigurationGeneration.fetch_add(
            1, std::memory_order_acq_rel);
    }
}

static void RememberRelativeScaleFromWindow(HWND hwnd) {
    if (!hwnd || !g_settings.relativeWindowSize ||
        g_settings.pixelPerfect) return;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return;
    const HMONITOR monitor =
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return;
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    const int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
    const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
    if (clientWidth <= 0 || clientHeight <= 0 ||
        monitorWidth <= 0 || monitorHeight <= 0) return;
    const int widthScale = static_cast<int>(
        static_cast<int64_t>(clientWidth) * kRelativeScaleUnit /
        monitorWidth);
    const int heightScale = static_cast<int>(
        static_cast<int64_t>(clientHeight) * kRelativeScaleUnit /
        monitorHeight);
    g_settings.relativeWindowScalePpm = std::clamp(
        (std::max)(widthScale, heightScale),
        kRelativeScaleUnit / 4, kRelativeScaleUnit);
}

static void ApplyRelativeSizeForMonitor(HWND hwnd, HMONITOR monitor,
                                        const POINT& cursor,
                                        RECT& movingRect) {
    if (!g_settings.relativeWindowSize || !monitor ||
        monitor == g_relativeMoveMonitor) {
        return;
    }
    const int oldWidth = movingRect.right - movingRect.left;
    const int oldHeight = movingRect.bottom - movingRect.top;
    if (oldWidth <= 0 || oldHeight <= 0) return;

    const UINT dpi = EffectiveMonitorDpi(monitor, hwnd);
    const SIZE desired = DesiredWindowOuterSize(hwnd, monitor, dpi);
    const double cursorRatioX = std::clamp(
        static_cast<double>(cursor.x - movingRect.left) / oldWidth,
        0.0, 1.0);
    const double cursorRatioY = std::clamp(
        static_cast<double>(cursor.y - movingRect.top) / oldHeight,
        0.0, 1.0);
    movingRect.left = cursor.x -
        static_cast<int>(std::lround(cursorRatioX * desired.cx));
    movingRect.top = cursor.y -
        static_cast<int>(std::lround(cursorRatioY * desired.cy));
    movingRect.right = movingRect.left + desired.cx;
    movingRect.bottom = movingRect.top + desired.cy;
    fwprintf(stderr,
             L"[video] monitor-relative move: outer %ld x %ld at %u dpi\n",
             desired.cx, desired.cy, dpi);
    g_relativeMoveMonitor = monitor;
}

struct MonitorLookup {
    std::wstring wantedDevice;
    HMONITOR match = nullptr;
    MONITORINFOEXW info{};
};

static BOOL CALLBACK FindMonitorCallback(HMONITOR monitor, HDC, LPRECT,
                                         LPARAM contextValue) {
    auto* context = reinterpret_cast<MonitorLookup*>(contextValue);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) &&
        _wcsicmp(info.szDevice, context->wantedDevice.c_str()) == 0) {
        context->match = monitor;
        context->info = info;
        return FALSE;
    }
    return TRUE;
}

static HMONITOR SavedViewerMonitor() {
    if (!g_settings.hasWindowPosition) return nullptr;
    if (!g_settings.monitorDevice.empty()) {
        MonitorLookup lookup{};
        lookup.wantedDevice = g_settings.monitorDevice;
        EnumDisplayMonitors(nullptr, nullptr, FindMonitorCallback,
                            reinterpret_cast<LPARAM>(&lookup));
        if (lookup.match) return lookup.match;
    }
    return MonitorFromPoint(
        POINT{g_settings.windowX, g_settings.windowY},
        MONITOR_DEFAULTTONULL);
}

static bool RestoredWindowOrigin(const SIZE& outerSize, POINT& origin) {
    if (!g_settings.hasWindowPosition) return false;
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = SavedViewerMonitor();
    if (monitor) GetMonitorInfoW(monitor, &monitorInfo);
    if (!monitor) {
        monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoW(monitor, &monitorInfo);
        origin.x = monitorInfo.rcWork.left +
                   ((monitorInfo.rcWork.right - monitorInfo.rcWork.left) -
                    outerSize.cx) / 2;
        origin.y = monitorInfo.rcWork.top +
                   ((monitorInfo.rcWork.bottom - monitorInfo.rcWork.top) -
                    outerSize.cy) / 2;
        return true;
    }

    const RECT work = monitorInfo.rcWork;
    const int maximumX = (std::max)(work.left, work.right - outerSize.cx);
    const int maximumY = (std::max)(work.top, work.bottom - outerSize.cy);
    origin.x = std::clamp(g_settings.windowX,
                          static_cast<int>(work.left), maximumX);
    origin.y = std::clamp(g_settings.windowY,
                          static_cast<int>(work.top), maximumY);
    return true;
}

static void PersistWindowPosition(HWND hwnd) {
    RECT rect{};
    if (g_fullscreen && g_haveLastWindowedRect) {
        rect = g_lastWindowedRect;
    } else if (!GetWindowRect(hwnd, &rect)) {
        return;
    }
    const HMONITOR monitor =
        MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfoW(monitor, &monitorInfo);
    g_settings.hasWindowPosition = true;
    g_settings.windowX = rect.left;
    g_settings.windowY = rect.top;
    g_settings.monitorDevice = monitorInfo.szDevice;

    EnsureUserDataDirectory();
    const std::wstring path = SettingsPath();
    wchar_t value[32]{};
    swprintf_s(value, L"%d", g_settings.windowX);
    WritePrivateProfileStringW(L"Window", L"X", value, path.c_str());
    swprintf_s(value, L"%d", g_settings.windowY);
    WritePrivateProfileStringW(L"Window", L"Y", value, path.c_str());
    WritePrivateProfileStringW(L"Window", L"Monitor",
                               g_settings.monitorDevice.c_str(),
                               path.c_str());
}

static void ApplyWindowEdgeSnap(HWND hwnd, RECT& movingRect) {
    if (!g_settings.windowSnap || g_fullscreen ||
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        ResetWindowSnapState();
        return;
    }

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    const HMONITOR monitor =
        MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return;

    const UINT dpi = EffectiveMonitorDpi(monitor, hwnd);
    const int snapDistance =
        (std::max)(1, MulDiv(kWindowSnapDistanceDip, dpi,
                            USER_DEFAULT_SCREEN_DPI));
    const int releaseDistance =
        (std::max)(1, MulDiv(kWindowSnapReleaseDip, dpi,
                            USER_DEFAULT_SCREEN_DPI));
    const int width = movingRect.right - movingRect.left;
    const int height = movingRect.bottom - movingRect.top;
    const RECT work = monitorInfo.rcWork;
    bool horizontalReleased = false;
    if (g_windowSnapState.horizontal == HorizontalSnapEdge::Left) {
        const int cursorDelta =
            cursor.x - g_windowSnapState.horizontalCursorAnchor;
        const bool release = std::abs(cursorDelta) >= releaseDistance;
        if (release) {
            g_windowSnapState.horizontal = HorizontalSnapEdge::None;
            g_windowSnapState.suppressHorizontal = true;
            horizontalReleased = true;
        } else {
            movingRect.left = g_windowSnapState.horizontalSnapCoordinate;
            movingRect.right = movingRect.left + width;
        }
    } else if (g_windowSnapState.horizontal == HorizontalSnapEdge::Right) {
        const int cursorDelta =
            cursor.x - g_windowSnapState.horizontalCursorAnchor;
        const bool release = std::abs(cursorDelta) >= releaseDistance;
        if (release) {
            g_windowSnapState.horizontal = HorizontalSnapEdge::None;
            g_windowSnapState.suppressHorizontal = true;
            horizontalReleased = true;
        } else {
            movingRect.right = g_windowSnapState.horizontalSnapCoordinate;
            movingRect.left = movingRect.right - width;
        }
    }

    if (g_windowSnapState.horizontal == HorizontalSnapEdge::None) {
        const bool outsideHorizontalZone =
            std::abs(movingRect.left - work.left) > snapDistance &&
            std::abs(movingRect.right - work.right) > snapDistance;
        if (g_windowSnapState.suppressHorizontal) {
            if (outsideHorizontalZone) {
                g_windowSnapState.suppressHorizontal = false;
            }
        } else if (!horizontalReleased) {
            if (std::abs(movingRect.left - work.left) <= snapDistance) {
                movingRect.left = work.left;
                movingRect.right = movingRect.left + width;
                g_windowSnapState.horizontal = HorizontalSnapEdge::Left;
                g_windowSnapState.horizontalCursorAnchor = cursor.x;
                g_windowSnapState.horizontalSnapCoordinate = work.left;
            } else if (std::abs(movingRect.right - work.right) <= snapDistance) {
                movingRect.right = work.right;
                movingRect.left = movingRect.right - width;
                g_windowSnapState.horizontal = HorizontalSnapEdge::Right;
                g_windowSnapState.horizontalCursorAnchor = cursor.x;
                g_windowSnapState.horizontalSnapCoordinate = work.right;
            }
        }
    }

    bool verticalReleased = false;
    if (g_windowSnapState.vertical == VerticalSnapEdge::Top) {
        const int cursorDelta =
            cursor.y - g_windowSnapState.verticalCursorAnchor;
        const bool release = std::abs(cursorDelta) >= releaseDistance;
        if (release) {
            g_windowSnapState.vertical = VerticalSnapEdge::None;
            g_windowSnapState.suppressVertical = true;
            verticalReleased = true;
        } else {
            movingRect.top = g_windowSnapState.verticalSnapCoordinate;
            movingRect.bottom = movingRect.top + height;
        }
    } else if (g_windowSnapState.vertical == VerticalSnapEdge::Bottom) {
        const int cursorDelta =
            cursor.y - g_windowSnapState.verticalCursorAnchor;
        const bool release = std::abs(cursorDelta) >= releaseDistance;
        if (release) {
            g_windowSnapState.vertical = VerticalSnapEdge::None;
            g_windowSnapState.suppressVertical = true;
            verticalReleased = true;
        } else {
            movingRect.bottom = g_windowSnapState.verticalSnapCoordinate;
            movingRect.top = movingRect.bottom - height;
        }
    }

    if (g_windowSnapState.vertical == VerticalSnapEdge::None) {
        const bool outsideVerticalZone =
            std::abs(movingRect.top - work.top) > snapDistance &&
            std::abs(movingRect.bottom - work.bottom) > snapDistance;
        if (g_windowSnapState.suppressVertical) {
            if (outsideVerticalZone) {
                g_windowSnapState.suppressVertical = false;
            }
        } else if (!verticalReleased) {
            if (std::abs(movingRect.top - work.top) <= snapDistance) {
                movingRect.top = work.top;
                movingRect.bottom = movingRect.top + height;
                g_windowSnapState.vertical = VerticalSnapEdge::Top;
                g_windowSnapState.verticalCursorAnchor = cursor.y;
                g_windowSnapState.verticalSnapCoordinate = work.top;
            } else if (std::abs(movingRect.bottom - work.bottom) <= snapDistance) {
                movingRect.bottom = work.bottom;
                movingRect.top = movingRect.bottom - height;
                g_windowSnapState.vertical = VerticalSnapEdge::Bottom;
                g_windowSnapState.verticalCursorAnchor = cursor.y;
                g_windowSnapState.verticalSnapCoordinate = work.bottom;
            }
        }
    }
}

static void ConstrainWindowRectToVideoAspect(HWND hwnd, RECT& sizingRect,
                                             UINT sizingEdge) {
    if (!hwnd || g_fullscreen || g_settings.pixelPerfect) return;
    const auto& video = CurrentVideoPreset();
    if (video.width <= 0 || video.height <= 0) return;

    const UINT dpi = GetDpiForWindow(hwnd);
    const DWORD style =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    RECT frame{0, 0, 0, 0};
    if (!AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, dpi)) {
        AdjustWindowRectEx(&frame, style, FALSE, exStyle);
    }
    llcv::window_geometry::ConstrainToAspect(
        sizingRect, sizingEdge,
        {static_cast<int>(frame.right - frame.left),
         static_cast<int>(frame.bottom - frame.top),
         video.width, video.height, 320, 180});
}

static LRESULT BorderlessHitTest(HWND hwnd, LPARAM lParam) {
    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return HTCLIENT;
    const POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    const UINT dpi = GetDpiForWindow(hwnd);
    const int grip = (std::max)(6, MulDiv(8, dpi, 96));
    return llcv::window_geometry::BorderlessHitTest(
        windowRect, cursor, grip, !g_settings.pixelPerfect);
}

constexpr UINT_PTR kFullscreenCursorTimerId = 2;
constexpr UINT kFullscreenCursorTimerPeriodMs = 250;
constexpr uint64_t kFullscreenCursorHideDelayMs = 2000;
static bool g_fullscreenCursorHidden = false;
static uint64_t g_lastFullscreenCursorActivityMs = 0;

static bool FullscreenCursorAutoHideActive() {
    return g_fullscreen.load(std::memory_order_acquire) &&
        g_settings.fullscreenCursorMode == FullscreenCursorMode::AutoHide;
}

static void SetFullscreenCursorVisible(bool visible) {
    // Avoid ShowCursor here. Its process-wide display count can become
    // unbalanced when a fullscreen window closes through an unusual path,
    // leaving the cursor hidden after the viewer exits.
    g_fullscreenCursorHidden = !visible;
    SetCursor(visible ? LoadCursorW(nullptr, IDC_ARROW) : nullptr);
}

static void NoteFullscreenCursorActivity() {
    if (!FullscreenCursorAutoHideActive()) return;
    g_lastFullscreenCursorActivityMs = GetTickCount64();
    if (g_fullscreenCursorHidden) SetFullscreenCursorVisible(true);
}

static void BeginFullscreenCursorTracking(HWND hwnd) {
    g_lastFullscreenCursorActivityMs = GetTickCount64();
    SetFullscreenCursorVisible(true);
    if (g_settings.fullscreenCursorMode == FullscreenCursorMode::AutoHide) {
        SetTimer(hwnd, kFullscreenCursorTimerId,
                 kFullscreenCursorTimerPeriodMs, nullptr);
    }
}

static void EndFullscreenCursorTracking(HWND hwnd) {
    if (hwnd) KillTimer(hwnd, kFullscreenCursorTimerId);
    g_lastFullscreenCursorActivityMs = 0;
    if (g_fullscreenCursorHidden) SetFullscreenCursorVisible(true);
}

static void UpdateFullscreenCursorIdleState() {
    if (!FullscreenCursorAutoHideActive() || g_fullscreenCursorHidden) return;
    const uint64_t now = GetTickCount64();
    if (now - g_lastFullscreenCursorActivityMs >=
        kFullscreenCursorHideDelayMs) {
        SetFullscreenCursorVisible(false);
    }
}

static LRESULT CALLBACK VideoHostSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSEWHEEL) {
        NoteFullscreenCursorActivity();
    }
    if (msg == WM_SETCURSOR && FullscreenCursorAutoHideActive() &&
        g_fullscreenCursorHidden) {
        SetCursor(nullptr);
        return TRUE;
    }
    if (msg == WM_NCHITTEST) {
        const HWND parent = GetParent(hwnd);
        if (parent && g_settings.borderlessWindow && !g_fullscreen) {
            // Let the same-thread parent perform the hit test. Returning an
            // edge code from the child would try to resize the child instead.
            return HTTRANSPARENT;
        }
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, VideoHostSubclassProc, 1);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void ToggleFullscreen(HWND hwnd, bool automaticStartup = false,
                             bool updateOutput = true) {
    BeginOutputTransition();
    if (!g_fullscreen.load(std::memory_order_acquire)) {
        GetWindowRect(hwnd, &g_lastWindowedRect);
        g_haveLastWindowedRect = true;
        g_prevStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
        GetWindowPlacement(hwnd, &g_prevPlacement);

        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);

        g_fullscreen.store(true, std::memory_order_release);
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_prevStyle & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        BeginFullscreenCursorTracking(hwnd);
        g_autoFullscreen = automaticStartup;
    } else {
        EndFullscreenCursorTracking(hwnd);
        g_fullscreen.store(false, std::memory_order_release);
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_prevStyle);
        SetWindowPlacement(hwnd, &g_prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
        g_autoFullscreen = false;
    }
    EndOutputTransition(updateOutput);
}

static bool SelectedResolutionMatchesMonitor(HMONITOR monitor) {
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return false;

    const auto& video = CurrentVideoPreset();
    const int monitorWidth = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
    const int monitorHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
    return video.width == monitorWidth && video.height == monitorHeight;
}

static bool ClientSizeFillsMonitor(const SIZE& client, HMONITOR monitor) {
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return false;
    const int monitorWidth =
        monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
    const int monitorHeight =
        monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
    return client.cx == monitorWidth && client.cy == monitorHeight;
}

static void ShowTransientHud(TransientHudContent content) {
    g_transientHudContent.store(content, std::memory_order_release);
    g_volumeHudUntilMs.store(GetTickCount64() + 1500,
                             std::memory_order_release);
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
}

static void RestoreOneToOneWindow(HWND hwnd) {
    if (!hwnd) return;
    const auto& video = CurrentVideoPreset();
    const HMONITOR monitor =
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return;

    const bool fullscreen =
        g_fullscreen.load(std::memory_order_acquire);
    const DWORD windowedStyle = static_cast<DWORD>(
        fullscreen ? g_prevStyle : GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    const UINT dpi = EffectiveMonitorDpi(monitor, hwnd);
    const SIZE outer = OuterSizeForClientPixels(
        video.width, video.height, windowedStyle, exStyle, dpi);
    const int workWidth =
        monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    const int workHeight =
        monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;

    // A capture that exactly matches the monitor is already the ideal 1:1
    // fullscreen case. Prefer true fullscreen even when a borderless window
    // would happen to fit an auto-hidden-taskbar work area.
    if (SelectedResolutionMatchesMonitor(monitor)) {
        if (g_settings.relativeWindowSize) {
            g_settings.relativeWindowScalePpm =
                RelativeScaleForMonitor(monitor);
        }
        if (!fullscreen) ToggleFullscreen(hwnd);
        ShowTransientHud(TransientHudContent::OneToOne);
        fwprintf(stderr,
                 L"[video] F5 restored 1:1 using matching-monitor fullscreen: "
                 L"%d x %d.\n",
                 video.width, video.height);
        return;
    }

    if (outer.cx <= workWidth && outer.cy <= workHeight) {
        BeginOutputTransition();
        if (g_settings.relativeWindowSize) {
            // F5 establishes this monitor's 1:1 window as the new relative
            // baseline as well, so subsequent monitor moves preserve it.
            g_settings.relativeWindowScalePpm =
                RelativeScaleForMonitor(monitor);
        }
        if (fullscreen) ToggleFullscreen(hwnd, false, false);
        const int x = monitorInfo.rcWork.left + (workWidth - outer.cx) / 2;
        const int y = monitorInfo.rcWork.top + (workHeight - outer.cy) / 2;
        SetWindowPos(hwnd, nullptr, x, y, outer.cx, outer.cy,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        EndOutputTransition(true);
        ShowTransientHud(TransientHudContent::OneToOne);
        fwprintf(stderr, L"[video] F5 restored 1:1 client: %d x %d.\n",
                 video.width, video.height);
        return;
    }

    ShowTransientHud(TransientHudContent::OneToOneUnavailable);
    fwprintf(stderr,
             L"[video] F5 1:1 unavailable: capture %d x %d exceeds the "
             L"current monitor work area %d x %d.\n",
             video.width, video.height, workWidth, workHeight);
}

static constexpr UINT WM_TOGGLE_RUNTIME_OSD = WM_APP + 91;
static constexpr UINT WM_OPEN_SETTINGS = WM_APP + 92;
static constexpr UINT WM_RESTORE_ONE_TO_ONE = WM_APP + 93;

static void StartBackgroundUpdateCheck(HWND hwnd) {
    if (!hwnd || !g_settings.checkForUpdates || g_updateCheckThread.joinable()) {
        return;
    }
    g_updateCheckStop.store(false, std::memory_order_release);
    g_updateCheckThread = std::thread([hwnd]() {
        Sleep(2000);
        if (g_updateCheckStop.load(std::memory_order_acquire) ||
            !g_running.load(std::memory_order_acquire) || !IsWindow(hwnd)) {
            return;
        }
        UpdateCheckResult result;
        if (!llcv::update::FetchLatestRelease(kAppVersionLabel, result) ||
            !result.newer ||
            result.installerUrl.empty() ||
            g_updateCheckStop.load(std::memory_order_acquire) ||
            !g_running.load(std::memory_order_acquire) || !IsWindow(hwnd)) {
            return;
        }
        auto* message = new UpdateCheckResult(std::move(result));
        if (!PostMessageW(hwnd, WM_UPDATE_CHECK_COMPLETE, 0,
                          reinterpret_cast<LPARAM>(message))) {
            delete message;
        }
    });
}

static void FormatAudioErrorAge(uint64_t lastErrorMs, uint64_t nowMs,
                                wchar_t* output, size_t outputCount) {
    if (!output || outputCount == 0) return;
    if (!lastErrorMs || nowMs < lastErrorMs) {
        wcscpy_s(output, outputCount, UI_TEXT(L"없음"));
        return;
    }
    const uint64_t ageSeconds = (nowMs - lastErrorMs) / 1000;
    if (ageSeconds < 2) {
        wcscpy_s(output, outputCount, UI_TEXT(L"방금"));
    } else if (ageSeconds < 60) {
        swprintf_s(output, outputCount, UI_TEXT(L"%llu초 전"),
                   static_cast<unsigned long long>(ageSeconds));
    } else if (ageSeconds < 3600) {
        swprintf_s(output, outputCount, UI_TEXT(L"%llu분 %llu초 전"),
                   static_cast<unsigned long long>(ageSeconds / 60),
                   static_cast<unsigned long long>(ageSeconds % 60));
    } else {
        swprintf_s(output, outputCount, UI_TEXT(L"%llu시간 %llu분 전"),
                   static_cast<unsigned long long>(ageSeconds / 3600),
                   static_cast<unsigned long long>((ageSeconds / 60) % 60));
    }
}

static std::wstring BuildRuntimeOsdText(int outputWidth, int outputHeight) {
    const auto& preset = CurrentVideoPreset();
    const std::wstring& captureName = g_activeCaptureDeviceName;
    const std::wstring outputName = ActiveAudioOutputName();
    const VideoPixelFormat activeFormat = static_cast<VideoPixelFormat>(
        g_activePixelFormat.load(std::memory_order_acquire));
    const bool compressedVideo = IsCompressedVideoFormat(activeFormat);
    const bool p010Video = activeFormat == VideoPixelFormat::P010;
    const bool hdrVideo = p010Video &&
        g_hdrOutputActive.load(std::memory_order_acquire);
    const auto activeColorMatrix = static_cast<llcv::video_color::Matrix>(
        g_activeVideoColorMatrix.load(std::memory_order_acquire));
    const auto activeColorRange = static_cast<llcv::video_color::Range>(
        g_activeVideoColorRange.load(std::memory_order_acquire));
    const auto activeColorMatrixSource = static_cast<llcv::video_color::Source>(
        g_activeVideoColorMatrixSource.load(std::memory_order_acquire));
    const auto activeColorRangeSource = static_cast<llcv::video_color::Source>(
        g_activeVideoColorRangeSource.load(std::memory_order_acquire));
    const wchar_t* chromaText = compressedVideo
                                    ? L"decode→4:2:0"
                                    : activeFormat == VideoPixelFormat::Yuy2
                                          ? L"4:2:2"
                                          : hdrVideo ? L"4:2:0 · BT.2020"
                                              : p010Video ? L"4:2:0 · P010"
                                                           : L"4:2:0";
    const wchar_t* bitDepthText = compressedVideo
                                      ? L"compressed"
                                      : p010Video ? L"10-bit" : L"8-bit";
    wchar_t compressedQualityText[256]{};
    if (compressedVideo) {
        const bool manualColor =
            activeColorMatrixSource == llcv::video_color::Source::UserOverride &&
            activeColorRangeSource == llcv::video_color::Source::UserOverride;
        swprintf_s(
            compressedQualityText,
            IsEnglishUi()
                ? (manualColor
                       ? L"%s · %s · MJPEG manual · D3D11 VP"
                       : L"%s · %s · MJPEG auto(%s) · D3D11 VP")
                : (manualColor
                       ? L"%s · %s · MJPEG 수동 · D3D11 VP"
                       : L"%s · %s · MJPEG 자동(%s) · D3D11 VP"),
            llcv::video_color::MatrixName(activeColorMatrix),
            llcv::video_color::RangeName(activeColorRange),
            llcv::video_color::CompactSourceName(activeColorMatrixSource,
                                                  activeColorRangeSource));
    }
    const wchar_t* qualityText = hdrVideo
        ? L"BT.2020 · PQ · HDR10 prototype"
        : p010Video
        ? L"P010 · HDR output unavailable"
        : compressedVideo
        ? compressedQualityText
        : IsEnglishUi()
        ? L"BT.709 · Limited range · D3D11 Video Processor"
        : L"BT.709 · Limited range · D3D11 Video Processor";
    const wchar_t* videoPath = hdrVideo
        ? L"DirectShow P010 → D3D11 HDR10"
        : compressedVideo
        ? L"DirectShow → Media Foundation → D3D11"
        : L"DirectShow → D3D11";
    const int configuredFps =
        g_videoConfiguredFps.load(std::memory_order_acquire) > 0
            ? g_videoConfiguredFps.load(std::memory_order_relaxed)
            : RequestedVideoFrameRate();
    const int64_t latencyUs =
        g_videoAppLatencyUs.load(std::memory_order_acquire);
    const UINT32 audioFrames =
        g_audioActualBufferFrames.load(std::memory_order_acquire);
    const UINT32 capturePacketFrames =
        g_audioCapturePacketFrames.load(std::memory_order_acquire);
    const int64_t captureIntervalUs =
        g_audioCaptureIntervalUs.load(std::memory_order_acquire);
    const UINT32 queuedFrames =
        g_audioRingFrames.load(std::memory_order_acquire) +
        g_audioResamplerFrames.load(std::memory_order_acquire);
    const UINT32 queueTargetFrames =
        g_audioQueueTargetFrames.load(std::memory_order_acquire);
    const UINT32 observedMinimumFrames =
        g_audioMinimumPreRenderFrames.load(std::memory_order_acquire);
    const UINT32 minimumPreRenderFrames =
        observedMinimumFrames == UINT32_MAX
            ? queuedFrames : observedMinimumFrames;
    const UINT32 paddingFrames =
        g_audioWasapiPaddingFrames.load(std::memory_order_acquire);
    const wchar_t* presentationText =
        g_settings.presentationMode == PresentationMode::VSync
            ? L"VSync"
            : g_videoTearing.load(std::memory_order_acquire)
                  ? UI_TEXT(L"저지연") : L"Immediate";
    wchar_t latencyText[64]{};
    if (latencyUs >= 0) {
        swprintf_s(latencyText, L"%.2f ms", latencyUs / 1000.0);
    } else {
        wcscpy_s(latencyText, UI_TEXT(L"측정 대기 중"));
    }

    const uint64_t underrunEvents =
        g_underruns.load(std::memory_order_relaxed);
    const uint64_t overrunEvents = g_ring.Overruns();
    const uint64_t underrunFrames =
        g_audioUnderrunFrames.load(std::memory_order_acquire);
    const uint64_t overrunFrames =
        g_audioOverrunFrames.load(std::memory_order_acquire);
    const uint64_t monitorStartMs =
        g_audioMonitorStartMs.load(std::memory_order_acquire);
    const uint64_t nowMs = GetTickCount64();
    const uint64_t elapsedMs = monitorStartMs && nowMs >= monitorStartMs
        ? nowMs - monitorStartMs : 0;
    const uint64_t lastErrorMs = (std::max)(
        g_audioLastUnderrunMs.load(std::memory_order_acquire),
        g_audioLastOverrunMs.load(std::memory_order_acquire));
    const uint64_t lastErrorAgeMs = lastErrorMs && nowMs >= lastErrorMs
        ? nowMs - lastErrorMs : UINT64_MAX;
    const uint64_t totalErrorEvents = underrunEvents + overrunEvents;
    const uint64_t latePacketUnderruns = (std::min)(
        g_audioLatePacketUnderruns.load(std::memory_order_acquire),
        underrunEvents);
    const uint64_t resamplerUnderruns = (std::min)(
        g_audioResamplerUnderruns.load(std::memory_order_acquire),
        underrunEvents - latePacketUnderruns);
    const uint64_t queueDepletionUnderruns =
        underrunEvents - latePacketUnderruns - resamplerUnderruns;
    const double elapsedSeconds = elapsedMs / 1000.0;
    const double eventRatePerHour = elapsedSeconds > 0.0
        ? totalErrorEvents * 3600.0 / elapsedSeconds : 0.0;
    const double imbalancePpm = elapsedSeconds > 0.0
        ? (static_cast<double>(underrunFrames) -
           static_cast<double>(overrunFrames)) * 1'000'000.0 /
              (elapsedSeconds * kSampleRate)
        : 0.0;
    wchar_t lastErrorText[64]{};
    FormatAudioErrorAge(lastErrorMs, nowMs, lastErrorText,
                        ARRAYSIZE(lastErrorText));

    const int activeCorrectionPpm =
        g_audioResamplePpm.load(std::memory_order_acquire);
    const bool trackingActive = AudioTrackingActive();
    const AudioPatternStats patternStats = g_audioErrorHistory.Analyze(
        nowMs, capturePacketFrames, kSampleRate);
    wchar_t patternLastText[64]{};
    FormatAudioErrorAge(patternStats.lastEventMs, nowMs, patternLastText,
                        ARRAYSIZE(patternLastText));
    const wchar_t* patternText = !trackingActive
        ? UI_TEXT(L"측정 중")
        : patternStats.recentUnderruns == 0
              ? UI_TEXT(L"없음")
              : patternStats.maxConsecutiveUnderruns >= 2
                    ? UI_TEXT(L"연속") : UI_TEXT(L"간헐적");
    const wchar_t* clockDiagnosis = trackingActive
        ? UI_TEXT(L"측정 중") : UI_TEXT(L"워밍업 · 시작 5초 제외");
    if (monitorStartMs) {
        if (AudioResamplerActive()) {
            if (resamplerUnderruns > 0 &&
                lastErrorAgeMs <= 10 * 60 * 1000) {
                clockDiagnosis = UI_TEXT(L"리샘플러 출력 부족 감지");
            } else if (std::abs(activeCorrectionPpm) >= 950) {
                clockDiagnosis = UI_TEXT(L"리샘플러 보정 한계 접근");
            } else if (totalErrorEvents == 0 ||
                       lastErrorAgeMs > 10 * 60 * 1000) {
                clockDiagnosis = UI_TEXT(L"리샘플러 정상 작동");
            } else {
                clockDiagnosis = UI_TEXT(L"보정 작동 · 오류 원인 아래 확인");
            }
        } else if (g_settings.driftCorrection == DriftCorrectionMode::Auto) {
            clockDiagnosis = trackingActive
                ? UI_TEXT(L"자동 관찰 중 · 원본 PCM")
                : UI_TEXT(L"측정 중");
        } else if (totalErrorEvents == 0) {
            clockDiagnosis = elapsedMs >= 2 * 60 * 1000
                ? UI_TEXT(L"안정 · 보정 불필요") : UI_TEXT(L"관찰 중");
        } else if (elapsedMs < 2 * 60 * 1000) {
            clockDiagnosis = UI_TEXT(L"초기 오류 · 더 관찰");
        } else if (lastErrorAgeMs > 10 * 60 * 1000) {
            clockDiagnosis = UI_TEXT(L"현재 안정 · 경과 관찰");
        } else if (underrunEvents > 0 && overrunEvents == 0 &&
                   latePacketUnderruns == underrunEvents) {
            clockDiagnosis = UI_TEXT(L"입력 지터 · 보정보다 대기량");
        } else if (std::abs(imbalancePpm) >= 50.0 ||
                   eventRatePerHour >= 12.0) {
            clockDiagnosis = UI_TEXT(L"반복 불균형 · 보정 권장");
        } else {
            clockDiagnosis = UI_TEXT(L"드문 오류 · 끔 유지 가능");
        }
    }

    const wchar_t* queueDiagnosis = trackingActive
        ? UI_TEXT(L"측정 중") : UI_TEXT(L"워밍업 · 시작 5초 제외");
    if (monitorStartMs) {
        if (underrunEvents == 0) {
            queueDiagnosis = g_settings.pcmQueueTargetMs ==
                                     kLowestPcmQueueMs
                ? UI_TEXT(L"최저 지연 · 오류 없음") : UI_TEXT(L"PCM 버퍼 여유 정상");
        } else if (lastErrorAgeMs > 10 * 60 * 1000) {
            queueDiagnosis = UI_TEXT(L"현재 안정 · 과거 오류 있음");
        } else if (queueDepletionUnderruns > 0) {
            queueDiagnosis = UI_TEXT(L"PCM 버퍼 부족 가능");
        } else if (resamplerUnderruns > 0) {
            queueDiagnosis = UI_TEXT(L"PCM 버퍼 있음 · 리샘플러 확인");
        } else {
            queueDiagnosis = UI_TEXT(L"캡처 패킷 지연 감지");
        }
    }

    const int logicalVolume =
        g_volumePercent.load(std::memory_order_acquire);
    const bool backgroundMuted =
        g_backgroundAudioMuted.load(std::memory_order_acquire);
    const wchar_t* volumeProcessing = backgroundMuted
        ? UI_TEXT(L"백그라운드 음소거 중")
        : logicalVolume == 100
              ? UI_TEXT(L"PCM 연산 우회")
              : logicalVolume == 0 ? UI_TEXT(L"음소거")
              : logicalVolume > 100 ? UI_TEXT(L"PCM 증폭 적용")
              : UI_TEXT(L"PCM 감쇠 적용");
    const uint64_t clipEvents =
        g_audioClipCount.load(std::memory_order_acquire);
    const bool clippingActive = nowMs <
        g_audioClipUntilMs.load(std::memory_order_acquire);
    wchar_t clippingText[96]{};
    if (clipEvents == 0) {
        wcscpy_s(clippingText, UI_TEXT(L"클리핑 없음"));
    } else if (clippingActive) {
        swprintf_s(clippingText, UI_TEXT(L"클리핑 감지 중 (%llu회)"),
                   static_cast<unsigned long long>(clipEvents));
    } else {
        swprintf_s(clippingText, UI_TEXT(L"클리핑 기록 (%llu회)"),
                   static_cast<unsigned long long>(clipEvents));
    }

    const wchar_t* scaleText =
        g_settings.pixelPerfect && g_settings.relativeWindowSize
            ? UI_TEXT(L"Pixel-perfect 시작 · Monitor-relative 이동")
            : g_settings.pixelPerfect
                  ? UI_TEXT(L"Pixel-perfect (고정 크기)")
                  : g_settings.relativeWindowSize
                        ? L"Scaled · Monitor-relative" : UI_TEXT(L"Scaled (비율 고정)");
    const wchar_t* correctionModeText =
        g_settings.driftCorrection == DriftCorrectionMode::Resample
            ? UI_TEXT(L"켬 · 리샘플러 사용")
            : g_settings.driftCorrection == DriftCorrectionMode::Auto
                  ? (AudioResamplerActive()
                         ? UI_TEXT(L"자동 · 보정 작동")
                         : UI_TEXT(L"자동 · 관찰 중"))
                  : UI_TEXT(L"끔 · 원본 PCM");
    const wchar_t* osdFormat = IsEnglishUi()
        ? L"Capture diagnostics                              [Tab close]\n"
          L"Path          %s · %s\n"
          L"Input         %d x %d @ %d fps · %s %s %s\n"
          L"Video quality %s\n"
          L"Display       %d x %d · %s · Flip-discard · %s\n"
          L"Actual FPS    Input %.1f · Present %.1f\n"
          L"App latency   %s  (not total HDMI latency)\n"
          L"Frames        Input %llu · Output %llu · Replaced %llu\n"
           L"Audio output  %s\n"
          L"Output device %s\n"
          L"Device buffer %.2f ms · queued %.2f ms · input packet %.2f ms · period %.2f ms\n"
          L"Clock drift   %s · applied %+d ppm · %s\n"
          L"App PCM queue current %.2f ms · target %.2f ms · observed min %.2f ms\n"
          L"Buffer diagnosis %s\n"
          L"Volume        %d%% · %s · %s\n"
          L"Audio errors  underrun %llu · missing audio %.2f ms · overrun %llu\n"
          L"Error causes  input late %llu · buffer shortage %llu · resampler %llu\n"
          L"Error pattern %s · last %s · max burst %llu\n"
          L"Error trend   %.1f/h · PCM imbalance %+.0f ppm (estimated) · last %s"
        : L"캡처 실시간 정보                              [Tab 닫기]\n"
          L"경로          %s · %s\n"
          L"입력          %d x %d @ %d fps · %s %s %s\n"
          L"영상 품질     %s\n"
          L"표시          %d x %d · %s · Flip-discard · %s\n"
          L"실제 FPS      입력 %.1f · Present %.1f\n"
          L"앱 처리 지연  %s  (총 HDMI 지연 아님)\n"
          L"프레임        입력 %llu · 출력 %llu · 최신화 건너뜀 %llu\n"
           L"오디오 출력   %s\n"
          L"출력 장치     %s\n"
          L"출력 버퍼(장치) %.2f ms · 현재 대기 %.2f ms · 입력 패킷 %.2f ms · 주기 %.2f ms\n"
          L"클록 보정     %s · 적용 %+d ppm · %s\n"
          L"앱 PCM 버퍼(대기) 현재 %.2f ms · 목표 %.2f ms · 관측 최저 %.2f ms\n"
          L"버퍼 진단     %s\n"
          L"음량          %d%% · %s · %s\n"
          L"오디오 오류   underrun %llu회 · 누락 오디오 %.2f ms · overrun %llu회\n"
          L"오류 원인     입력 늦음 %llu회 · 버퍼 부족 %llu회 · 리샘플러 %llu회\n"
          L"오류 패턴     %s · 최근 %s · 최대 연속 %llu회\n"
          L"오류 추세     %.1f회/h · PCM 불균형 %+.0f ppm(추정) · 최근 %s";
    wchar_t text[2300]{};
    swprintf_s(
        text, osdFormat,
        captureName.c_str(), videoPath, preset.width, preset.height, configuredFps,
        PixelFormatName(activeFormat), bitDepthText, chromaText, qualityText,
        outputWidth,
        outputHeight,
        scaleText,
        presentationText,
        g_osdInputFps.load(std::memory_order_acquire),
        g_osdPresentFps.load(std::memory_order_acquire), latencyText,
        static_cast<unsigned long long>(
            g_videoCapturedFrames.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_videoPresentedFrames.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_videoReplacedFrames.load(std::memory_order_relaxed)),
         g_settings.audioMode == AudioMode::WasapiExclusive
             ? L"WASAPI Exclusive"
             : g_settings.audioMode == AudioMode::Asio ? L"ASIO"
                                                        : L"WASAPI Shared",
        outputName.c_str(),
        1000.0 * audioFrames / kSampleRate,
        1000.0 * paddingFrames / kSampleRate,
        1000.0 * capturePacketFrames / kSampleRate,
        captureIntervalUs / 1000.0,
         correctionModeText,
        activeCorrectionPpm, clockDiagnosis,
        1000.0 * queuedFrames / kSampleRate,
        1000.0 * queueTargetFrames / kSampleRate,
        1000.0 * minimumPreRenderFrames / kSampleRate,
        queueDiagnosis,
        logicalVolume, volumeProcessing, clippingText,
        static_cast<unsigned long long>(underrunEvents),
        1000.0 * underrunFrames / kSampleRate,
        static_cast<unsigned long long>(overrunEvents),
        static_cast<unsigned long long>(latePacketUnderruns),
        static_cast<unsigned long long>(queueDepletionUnderruns),
        static_cast<unsigned long long>(resamplerUnderruns),
        patternText, patternLastText,
        static_cast<unsigned long long>(patternStats.maxConsecutiveUnderruns),
        eventRatePerHour, imbalancePpm, lastErrorText);
    return text;
}

static void UpdateOsdRates() {
    static uint64_t previousCaptured = 0;
    static uint64_t previousPresented = 0;
    static auto previousTime = std::chrono::steady_clock::now();
    static bool trackingStarted = false;
    const auto now = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(now - previousTime).count();
    if (seconds <= 0.0) return;
    const uint64_t captured =
        g_videoCapturedFrames.load(std::memory_order_relaxed);
    const uint64_t presented =
        g_videoPresentedFrames.load(std::memory_order_relaxed);
    if (!OsdTrackingActive()) {
        trackingStarted = false;
        previousCaptured = captured;
        previousPresented = presented;
        previousTime = now;
        g_osdInputFps.store(0.0, std::memory_order_release);
        g_osdPresentFps.store(0.0, std::memory_order_release);
        return;
    }
    if (!trackingStarted) {
        trackingStarted = true;
        previousCaptured = captured;
        previousPresented = presented;
        previousTime = now;
        return;
    }
    g_osdInputFps.store((captured - previousCaptured) / seconds,
                        std::memory_order_release);
    g_osdPresentFps.store((presented - previousPresented) / seconds,
                          std::memory_order_release);
    previousCaptured = captured;
    previousPresented = presented;
    previousTime = now;
}

static void ToggleRuntimeOsd() {
    const bool visible = !g_osdVisible.load(std::memory_order_acquire);
    g_osdVisible.store(visible, std::memory_order_release);
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
}

static void ToggleAudioOsd() {
    const bool visible = !g_audioOsdVisible.load(std::memory_order_acquire);
    g_audioOsdVisible.store(visible, std::memory_order_release);
    g_audioOsdHoverTarget.store(0, std::memory_order_release);
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
}

// Returns: 0 outside the OSD, 1 left card, 2 right card, 3 master row,
// 4 another part of the OSD. Cards deliberately have large hit areas; the
// small numbers themselves are not the interaction target.
static int AudioOsdHitTarget(HWND root, POINT screenPoint) {
    if (!root || !g_audioOsdVisible.load(std::memory_order_acquire)) return 0;
    POINT client = screenPoint;
    if (!ScreenToClient(root, &client)) return 0;
    RECT bounds{};
    if (!GetClientRect(root, &bounds)) return 0;
    return static_cast<int>(llcv::audio_osd::HitTest(
        bounds.right, bounds.bottom, client.x, client.y));
}

static bool AdjustVolumeFromWheel(HWND root, WPARAM wParam, LPARAM lParam) {
    const POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    HWND hovered = WindowFromPoint(screenPoint);
    if (!hovered || GetAncestor(hovered, GA_ROOT) != root) return false;

    static int wheelRemainder = 0;
    wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
    const int steps = wheelRemainder / WHEEL_DELTA;
    wheelRemainder %= WHEEL_DELTA;
    if (steps == 0) return true;

    const int target = AudioOsdHitTarget(root, screenPoint);
    const int maximum = g_settings.allowVolumeBoost
        ? kMaximumVolumePercent : 100;
    if (target == 1 || target == 2) {
        std::atomic<int>& channel = target == 1
            ? g_leftVolumePercent : g_rightVolumePercent;
        const int adjusted = std::clamp(
            channel.load(std::memory_order_acquire) + steps * 5, 0, 100);
        channel.store(adjusted, std::memory_order_release);
        if (target == 1) g_settings.leftVolumePercent = adjusted;
        else g_settings.rightVolumePercent = adjusted;
        g_audioOsdHoverTarget.store(target, std::memory_order_release);
    } else if (target == 0 || target == 3) {
        const int adjusted = std::clamp(
            g_volumePercent.load(std::memory_order_acquire) + steps * 5,
            0, maximum);
        g_volumePercent.store(adjusted, std::memory_order_release);
        g_settings.volumePercent = adjusted;
        if (!g_audioOsdVisible.load(std::memory_order_acquire)) {
            g_transientHudContent.store(TransientHudContent::Volume,
                                        std::memory_order_release);
            g_volumeHudUntilMs.store(GetTickCount64() + 1200,
                                     std::memory_order_release);
        }
    } else {
        return true;
    }
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
    return true;
}

static void UpdateBackgroundAudioMute(bool appActive) {
    const bool mute = g_settings.muteWhenBackground && !appActive;
    const bool previous = g_backgroundAudioMuted.exchange(
        mute, std::memory_order_acq_rel);
    if (previous == mute) return;
    fwprintf(stderr, L"[audio] background auto-mute: %s\n",
             mute ? L"on" : L"off");
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
}

// Audio-only mode has no video swap chain to composite the normal D2D OSD
// onto.  Paint the same compact audio panel directly into the small window
// instead.  This is UI-thread work only (30 Hz) and never blocks the capture
// or WASAPI render threads.
static void PaintAudioOnlyOsd(HDC dc, const RECT& client) {
    if (!g_audioOsdVisible.load(std::memory_order_acquire)) return;

    const int clientWidth = client.right - client.left;
    const llcv::audio_osd::Rect panel = llcv::audio_osd::RectForClient(
        clientWidth);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(232, 237, 242));

    LOGFONTW logFont{};
    logFont.lfHeight = -16;
    logFont.lfWeight = FW_SEMIBOLD;
    wcscpy_s(logFont.lfFaceName, L"Segoe UI");
    HFONT font = CreateFontIndirectW(&logFont);
    HGDIOBJ previousFont = SelectObject(dc, font);

    HBRUSH background = CreateSolidBrush(RGB(14, 16, 20));
    HBRUSH card = CreateSolidBrush(RGB(22, 26, 32));
    HBRUSH highlight = CreateSolidBrush(RGB(38, 66, 78));
    HBRUSH barBackground = CreateSolidBrush(RGB(51, 56, 64));
    HBRUSH bar = CreateSolidBrush(RGB(64, 199, 122));
    HBRUSH clipBrush = CreateSolidBrush(RGB(237, 87, 74));
    HPEN outline = CreatePen(PS_SOLID, 1, RGB(232, 237, 242));

    HGDIOBJ oldBrush = SelectObject(dc, background);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, panel.left, panel.top, panel.right, panel.bottom, 12, 12);
    SelectObject(dc, oldBrush);

    const auto textAt = [&](int x, int y, const wchar_t* text,
                            COLORREF color = RGB(232, 237, 242)) {
        SetTextColor(dc, color);
        TextOutW(dc, panel.left + x, panel.top + y, text,
                 static_cast<int>(wcslen(text)));
    };
    textAt(16, 10, IsEnglishUi() ? L"Audio" : L"오디오");

    wchar_t masterText[48]{};
    swprintf_s(masterText, IsEnglishUi() ? L"Master  %d%%" : L"마스터  %d%%",
               g_volumePercent.load(std::memory_order_acquire));
    textAt(16, 38, masterText);

    const int maximum = g_settings.allowVolumeBoost ? kMaximumVolumePercent : 100;
    const int master = g_volumePercent.load(std::memory_order_acquire);
    const RECT masterBar{panel.left + 16, panel.top + 64,
                         panel.left + 320, panel.top + 71};
    SelectObject(dc, barBackground);
    FillRect(dc, &masterBar, barBackground);
    RECT masterFill = masterBar;
    masterFill.right = masterFill.left + (masterBar.right - masterBar.left) *
        std::clamp(master, 0, maximum) / maximum;
    if (masterFill.right > masterFill.left) FillRect(dc, &masterFill, bar);

    const int hovered = g_audioOsdHoverTarget.load(std::memory_order_acquire);
    const int left = g_leftVolumePercent.load(std::memory_order_acquire);
    const int right = g_rightVolumePercent.load(std::memory_order_acquire);
    const double leftDb = llcv::audio::PeakToDbfs(
        g_audioPeakLeft.load(std::memory_order_acquire));
    const double rightDb = llcv::audio::PeakToDbfs(
        g_audioPeakRight.load(std::memory_order_acquire));

    const auto drawChannel = [&](int channel, const wchar_t* label,
                                 int percent, double peakDb, int x0, int x1) {
        RECT cardRect{panel.left + x0, panel.top + 84,
                      panel.left + x1, panel.top + 168};
        SelectObject(dc, channel == hovered ? highlight : card);
        RoundRect(dc, cardRect.left, cardRect.top, cardRect.right,
                  cardRect.bottom, 10, 10);
        SelectObject(dc, outline);
        RoundRect(dc, cardRect.left, cardRect.top, cardRect.right,
                  cardRect.bottom, 10, 10);
        wchar_t line[64]{};
        swprintf_s(line, L"%s   %d%%", label, percent);
        textAt(x0 + 14, 92, line);
        swprintf_s(line, L"%.1f dBFS", peakDb);
        textAt(x0 + 14, 119, line);
        RECT channelBar{panel.left + x0 + 14, panel.top + 157,
                        panel.left + x1 - 14, panel.top + 163};
        SelectObject(dc, barBackground);
        FillRect(dc, &channelBar, barBackground);
        RECT channelFill = channelBar;
        channelFill.right = channelFill.left +
            (channelBar.right - channelBar.left) * std::clamp(percent, 0, 100) / 100;
        if (channelFill.right > channelFill.left) FillRect(dc, &channelFill, bar);
    };
    drawChannel(1, L"L", left, leftDb, 16, 160);
    drawChannel(2, L"R", right, rightDb, 176, 320);

    const bool clipping = GetTickCount64() < g_audioClipUntilMs.load(
        std::memory_order_acquire);
    textAt(16, 172, clipping
               ? (IsEnglishUi() ? L"CLIP" : L"클리핑")
               : (IsEnglishUi() ? L"No clipping" : L"클리핑 없음"),
           clipping ? RGB(237, 87, 74) : RGB(232, 237, 242));

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    SelectObject(dc, previousFont);
    DeleteObject(outline);
    DeleteObject(clipBrush);
    DeleteObject(bar);
    DeleteObject(barBackground);
    DeleteObject(highlight);
    DeleteObject(card);
    DeleteObject(background);
    DeleteObject(font);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        if (!g_settings.audioOnly) {
            g_videoHost = CreateWindowExW(
                0, L"STATIC", nullptr,
                WS_CHILD | WS_VISIBLE | SS_BLACKRECT,
                0, 0, 100, 100,
                hwnd, nullptr,
                reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance,
                nullptr);
            if (g_videoHost) {
                SetWindowSubclass(g_videoHost, VideoHostSubclassProc, 1, 0);
            }
        }
        SetTimer(hwnd, 1, g_settings.audioOnly ? 33 : 500, nullptr);
        return 0;

    case WM_SIZE:
        if (g_videoHost) {
            MoveWindow(g_videoHost, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        if (!g_settings.audioOnly && wParam != SIZE_MINIMIZED) {
            // Recreate the HWND swapchain after a completed resize so its
            // backbuffer matches the new client area. During an interactive
            // drag, defer this until WM_EXITSIZEMOVE; rebuilding the D3D11
            // output for every sizing tick would cause needless stalls.
            if (g_manualResizeInProgress || g_outputTransitionDepth > 0) {
                g_outputResizePending = true;
            } else {
                g_outputConfigurationGeneration.fetch_add(
                    1, std::memory_order_acq_rel);
            }
        }
        if (g_settings.audioOnly) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT:
        if (g_settings.audioOnly) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client{};
            GetClientRect(hwnd, &client);
            const int width = client.right - client.left;
            const int height = client.bottom - client.top;
            HDC backDc = CreateCompatibleDC(dc);
            HBITMAP backBitmap = (backDc && width > 0 && height > 0)
                ? CreateCompatibleBitmap(dc, width, height) : nullptr;
            if (backDc && backBitmap) {
                HGDIOBJ oldBitmap = SelectObject(backDc, backBitmap);
                FillRect(backDc, &client,
                         reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                PaintAudioOnlyOsd(backDc, client);
                BitBlt(dc, 0, 0, width, height, backDc, 0, 0, SRCCOPY);
                SelectObject(backDc, oldBitmap);
                DeleteObject(backBitmap);
                DeleteDC(backDc);
            } else {
                if (backBitmap) DeleteObject(backBitmap);
                if (backDc) DeleteDC(backDc);
                FillRect(dc, &client,
                         reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                PaintAudioOnlyOsd(dc, client);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        if (g_settings.audioOnly) return 1;
        break;

    case WM_SIZING:
        if (lParam && !g_settings.audioOnly && !g_fullscreen &&
            !g_settings.pixelPerfect) {
            g_manualResizeInProgress = true;
            ConstrainWindowRectToVideoAspect(
                hwnd, *reinterpret_cast<RECT*>(lParam),
                static_cast<UINT>(wParam));
            return TRUE;
        }
        break;

    case WM_TIMER:
        if (wParam == kFullscreenCursorTimerId) {
            UpdateFullscreenCursorIdleState();
            return 0;
        }
        if (wParam == 1) {
            UpdateOsdRates();
            g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
            if (g_settings.audioOnly) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_ACTIVATEAPP:
        UpdateBackgroundAudioMute(wParam != FALSE);
        return 0;

    case WM_MOUSEMOVE:
        NoteFullscreenCursorActivity();
        break;

    case WM_SETCURSOR:
        if (FullscreenCursorAutoHideActive() && g_fullscreenCursorHidden) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;

    case WM_DISPLAYCHANGE:
        LogDisplayChangeEvent(wParam, lParam);
        break;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL ||
            wParam == DBT_DEVICEREMOVECOMPLETE ||
            wParam == DBT_DEVICEREMOVEPENDING ||
            wParam == DBT_DEVNODES_CHANGED) {
            LogDeviceChangeEvent(wParam);
        }
        break;

    case WM_GETDPISCALEDSIZE:
        if ((g_settings.pixelPerfect || g_settings.relativeWindowSize) &&
            !g_fullscreen && lParam) {
            const UINT pendingDpi = static_cast<UINT>(wParam);
            POINT cursor{};
            GetCursorPos(&cursor);
            const HMONITOR targetMonitor =
                MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            const SIZE desired = DesiredWindowOuterSize(
                hwnd, targetMonitor, pendingDpi);
            *reinterpret_cast<SIZE*>(lParam) = desired;
            fwprintf(stderr,
                     L"[video] DPI preflight: %u dpi -> outer %ld x %ld\n",
                     pendingDpi, desired.cx, desired.cy);
            return TRUE;
        }
        break;

    case WM_DPICHANGED:
        if ((g_settings.pixelPerfect || g_settings.relativeWindowSize) &&
            !g_fullscreen) {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            fwprintf(stderr,
                     L"[video] DPI changed: %u dpi, suggested outer %ld x %ld\n",
                     LOWORD(wParam), suggested->right - suggested->left,
                     suggested->bottom - suggested->top);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        break;

    case WM_ENTERSIZEMOVE:
        ResetWindowSnapState();
        g_manualResizeInProgress = false;
        g_relativeMoveMonitor =
            MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        return 0;

    case WM_MOVING:
        if (lParam) {
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                const HMONITOR monitor = MonitorFromPoint(
                    cursor, MONITOR_DEFAULTTONEAREST);
                ApplyRelativeSizeForMonitor(
                    hwnd, monitor, cursor,
                    *reinterpret_cast<RECT*>(lParam));
            }
            ApplyWindowEdgeSnap(hwnd, *reinterpret_cast<RECT*>(lParam));
            return TRUE;
        }
        break;

    case WM_EXITSIZEMOVE:
        ResetWindowSnapState();
        g_relativeMoveMonitor = nullptr;
        if (g_manualResizeInProgress) {
            RememberRelativeScaleFromWindow(hwnd);
        }
        g_manualResizeInProgress = false;
        NormalizeWindowSize(hwnd, true);
        if (g_outputResizePending) {
            g_outputResizePending = false;
            g_outputConfigurationGeneration.fetch_add(
                1, std::memory_order_acq_rel);
        }
        return 0;

    case WM_NCHITTEST:
        if (g_settings.borderlessWindow && !g_fullscreen) {
            return BorderlessHitTest(hwnd, lParam);
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_F2) {
            SendMessageW(hwnd, WM_OPEN_SETTINGS, 0, 0);
            return 0;
        }
        if (wParam == VK_F5) {
            SendMessageW(hwnd, WM_RESTORE_ONE_TO_ONE, 0, 0);
            return 0;
        }
        if (wParam == VK_TAB) {
            ToggleRuntimeOsd();
            return 0;
        }
        if (wParam == VK_F3) {
            ToggleAudioOsd();
            return 0;
        }
        if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (g_fullscreen && !g_autoFullscreen) {
                ToggleFullscreen(hwnd);
            } else {
                if (g_fullscreen) EndFullscreenCursorTracking(hwnd);
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        break;

    case WM_MOUSEWHEEL:
        NoteFullscreenCursorActivity();
        if (AdjustVolumeFromWheel(hwnd, wParam, lParam)) return 0;
        break;

    case WM_LBUTTONDBLCLK: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd, &point);
        const int target = AudioOsdHitTarget(hwnd, point);
        if (target == 1 || target == 2 || target == 3) {
            if (target == 1) {
                g_leftVolumePercent.store(100, std::memory_order_release);
                g_settings.leftVolumePercent = 100;
            } else if (target == 2) {
                g_rightVolumePercent.store(100, std::memory_order_release);
                g_settings.rightVolumePercent = 100;
            } else {
                g_volumePercent.store(100, std::memory_order_release);
                g_settings.volumePercent = 100;
            }
            g_audioOsdHoverTarget.store(target == 3 ? 0 : target,
                                        std::memory_order_release);
            g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        break;
    }

    case WM_TOGGLE_RUNTIME_OSD:
        ToggleRuntimeOsd();
        return 0;

    case WM_OPEN_SETTINGS:
        // The capture graph and WASAPI renderer are rebuilt only after this
        // window has closed and their threads have joined.
        g_restartToSettings.store(true, std::memory_order_release);
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case WM_RESTORE_ONE_TO_ONE:
        RestoreOneToOneWindow(hwnd);
        return 0;

    case WM_UPDATE_CHECK_COMPLETE: {
        std::unique_ptr<UpdateCheckResult> result(
            reinterpret_cast<UpdateCheckResult*>(lParam));
        if (!result || result->latestTag.empty() ||
            result->installerUrl.empty()) {
            return 0;
        }
        std::wstring message = UI_TEXT(
            L"새 버전이 있습니다. 공식 설치 파일을 다운로드하시겠습니까?");
        message += L"\n\n";
        message += result->latestTag;
        const int choice = MessageBoxW(
            hwnd, message.c_str(), UI_TEXT(L"업데이트 확인"),
            MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON1);
        if (choice == IDYES) {
            ShellExecuteW(hwnd, L"open", result->installerUrl.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }

    case WM_CLOSE:
        PersistWindowPosition(hwnd);
        g_windowPositionPersisted = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        EndFullscreenCursorTracking(hwnd);
        KillTimer(hwnd, 1);
        if (!g_windowPositionPersisted) PersistWindowPosition(hwnd);
        g_settings.volumePercent =
            g_volumePercent.load(std::memory_order_acquire);
        g_settings.leftVolumePercent =
            g_leftVolumePercent.load(std::memory_order_acquire);
        g_settings.rightVolumePercent =
            g_rightVolumePercent.load(std::memory_order_acquire);
        if (!g_suppressSettingsSave) {
            // A default-output change may force this session to Shared before
            // any renderer is opened. Preserve the user's Exclusive choice
            // in settings so it can become eligible again after a verified
            // device is selected, rather than silently rewriting it to Shared.
            const AudioMode runtimeMode = g_settings.audioMode;
            if (g_exclusiveStartupFallback) {
                g_settings.audioMode = g_exclusiveStartupRequestedMode;
            }
            SaveSettings();
            g_settings.audioMode = runtimeMode;
        }
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void RelaunchWithSettings() {
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable,
                                             ARRAYSIZE(executable));
    if (!length || length >= ARRAYSIZE(executable)) return;

    std::wstring commandLine = L"\"";
    commandLine += executable;
    commandLine += L"\" --force-settings";
    std::vector<wchar_t> mutableCommand(commandLine.begin(),
                                        commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
                       FALSE, 0, nullptr, nullptr, &startup, &process)) {
        // Let the child activate its settings dialog when Windows accepts the
        // foreground handoff. The dialog also has a short z-order promotion as
        // a fallback for systems that reject foreground activation here.
        AllowSetForegroundWindow(process.dwProcessId);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

static std::wstring CommandLineOptionValue(const wchar_t* option) {
    if (!option || !*option) return {};
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(
        GetCommandLineW(), &argumentCount);
    if (!arguments) return {};
    std::wstring value;
    for (int index = 1; index + 1 < argumentCount; ++index) {
        if (_wcsicmp(arguments[index], option) == 0) {
            value = arguments[index + 1];
            break;
        }
    }
    LocalFree(arguments);
    return value;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR commandLine, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX commonControls{
        sizeof(commonControls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&commonControls);

    LoadSettings();
    const std::wstring asioSmokeDriver =
        CommandLineOptionValue(L"--smoke-test-asio");
    if (!asioSmokeDriver.empty()) {
        // A smoke-only override avoids editing the user's saved output mode.
        // g_suppressSettingsSave below keeps the selected driver ephemeral.
        g_settings.audioMode = AudioMode::Asio;
        g_settings.asioDriverName = asioSmokeDriver;
        g_settings.skipStartupSettings = true;
    }
    const bool audioOnlySmokeTest = commandLine &&
        wcsstr(commandLine, L"--smoke-test-audio-only") != nullptr;
    if (audioOnlySmokeTest) {
        g_settings.audioOnly = true;
        g_settings.skipStartupSettings = true;
    }
    const bool smokeTest = commandLine &&
        (wcsstr(commandLine, L"--smoke-test") != nullptr ||
         audioOnlySmokeTest);
    const bool exclusiveProbeAll = commandLine &&
        wcsstr(commandLine, L"--exclusive-probe-all") != nullptr;
    const bool exclusiveProbeSelected = commandLine &&
        wcsstr(commandLine, L"--exclusive-probe") != nullptr;
    const bool exclusiveProbe = exclusiveProbeAll || exclusiveProbeSelected;
    g_suppressSettingsSave = smokeTest || exclusiveProbe;
    if (!smokeTest && !exclusiveProbe &&
        g_settings.audioMode == AudioMode::WasapiExclusive) {
        const std::wstring endpointId =
            llcv::audio_device::ResolveActiveEndpointId(
                g_settings.audioOutputDeviceId);
        if (!HasVerifiedExclusiveEndpoint(endpointId,
                                          g_settings.wasapiBufferMs)) {
            // Never let the immediate-start path open an untested Exclusive
            // stream. This preserves fast startup while keeping a changed
            // Windows-default device from producing broken audio.
            g_exclusiveStartupRequestedMode = g_settings.audioMode;
            g_settings.audioMode = AudioMode::WasapiShared;
            g_exclusiveStartupFallback = true;
        }
    }
    if (smokeTest && !audioOnlySmokeTest) {
        // Smoke tests must exercise the normal viewer even if a user's saved
        // profile currently selects audio-only mode.
        g_settings.audioOnly = false;
    }
    const bool longSmokeTest = commandLine &&
        wcsstr(commandLine, L"--smoke-test-60") != nullptr;
    const bool overlaySmokeTest = commandLine &&
        wcsstr(commandLine, L"--smoke-test-overlay") != nullptr;
    const bool audioOsdSmokeTest = commandLine &&
        wcsstr(commandLine, L"--smoke-test-audio-osd") != nullptr;
    const bool forceSettings = commandLine &&
        wcsstr(commandLine, L"--force-settings") != nullptr;
    if (overlaySmokeTest) {
        g_osdVisible.store(true, std::memory_order_release);
        g_volumeHudUntilMs.store(GetTickCount64() + 20'000,
                                 std::memory_order_release);
        g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
    }
    if (audioOsdSmokeTest) {
        g_audioOsdVisible.store(true, std::memory_order_release);
        g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
    }
    const bool shiftLaunch = !smokeTest &&
        (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool showStartupSettings = !smokeTest && !exclusiveProbe &&
        (forceSettings || shiftLaunch || !g_settings.skipStartupSettings);
    if (showStartupSettings &&
        !ShowSettingsDialog(hInst, forceSettings)) return 0;
    if (g_settings.audioOnly) {
        // Audio-only is an audio meter window, so the existing audio OSD is
        // visible from the first paint instead of a placeholder status label.
        g_audioOsdVisible.store(true, std::memory_order_release);
    }

    // Allocate a console for prototype diagnostics.
    const BOOL allocatedConsole = AllocConsole();
    if (allocatedConsole && !g_settings.showDiagnosticConsole) {
        const HWND console = GetConsoleWindow();
        if (console) ShowWindow(console, SW_HIDE);
    }
    if (allocatedConsole && g_settings.showDiagnosticConsole) {
        const HWND console = GetConsoleWindow();
        if (console) ShowWindow(console, SW_SHOW);
    }
    FILE* f = nullptr;
    if (smokeTest) {
        EnsureUserDataDirectory();
        CreateDirectoryW(LogDirectory().c_str(), nullptr);
        const std::wstring logPath = LogDirectory() + L"\\smoke-test.log";
        _wfreopen_s(&f, L"CONOUT$", L"w", stdout);
        FILE* logFile = nullptr;
        if (_wfreopen_s(&logFile, logPath.c_str(), L"w", stderr) != 0 ||
            !logFile) {
            // Keep diagnostics alive even when a restricted profile blocks
            // LocalAppData writes (for example, in a CI smoke-test runner).
            _wfreopen_s(&f, L"CONOUT$", L"w", stderr);
            fwprintf(stderr, L"[log] unable to open smoke-test log: %s\n",
                     logPath.c_str());
        }
    } else {
        _wfreopen_s(&f, L"CONOUT$", L"w", stdout);
        _wfreopen_s(&f, L"CONOUT$", L"w", stderr);
    }
    if (exclusiveProbe) {
        // A command-line compatibility run is deliberately logged even when
        // the user normally keeps diagnostics off, so its result can be
        // inspected after the temporary console has closed.
        g_settings.saveLog = true;
    }
    if (!smokeTest) OpenSavedLog();
    if (g_exclusiveStartupFallback) {
        fwprintf(stderr,
                 L"[audio] saved Exclusive profile was not verified for the "
                 L"current output; started with WASAPI Shared.\n");
    }
    SetActiveAudioOutputName(ConfiguredAudioEndpointName(
        g_settings.audioOutputDeviceId));

    if (exclusiveProbe) {
        const int result = RunExclusiveCompatibilityProbeCli(exclusiveProbeAll);
        CloseSavedLog();
        if (allocatedConsole) FreeConsole();
        return result;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.style = CS_DBLCLKS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"LowLatencyCaptureViewerClass";

    if (!RegisterClassW(&wc)) {
        CloseSavedLog();
        return 1;
    }

    const auto& video = CurrentVideoPreset();
    const wchar_t* audioLabel =
        g_settings.audioMode == AudioMode::WasapiExclusive
            ? L"WASAPI Exclusive"
            : g_settings.audioMode == AudioMode::Asio ? L"ASIO"
                                                       : L"WASAPI Shared";
    wchar_t title[256]{};
    const wchar_t* videoLabel =
        g_settings.presentationMode == PresentationMode::VSync
            ? L"Single Graph / Direct D3D11 / VSync"
            : L"Single Graph / Direct D3D11 / Tearing";
    if (g_settings.audioOnly) {
        swprintf_s(title, L"Low Latency Capture Viewer - Audio only - %s",
                   audioLabel);
    } else {
        swprintf_s(title,
                   L"Low Latency Capture Viewer - %dx%d @ %dfps - %s - %s",
                   video.width, video.height, RequestedVideoFrameRate(),
                   audioLabel, videoLabel);
    }

    const DWORD fixedWindowStyle =
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    const DWORD windowStyle = g_settings.audioOnly
                                  ? fixedWindowStyle
                                  : g_settings.borderlessWindow
                                  ? (WS_POPUP | WS_VISIBLE)
                                  : g_settings.pixelPerfect
                                        ? fixedWindowStyle
                                        : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    constexpr DWORD windowExStyle = 0;
    HMONITOR initialMonitor = SavedViewerMonitor();
    if (!initialMonitor) {
        initialMonitor = MonitorFromPoint(
            POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    const SIZE initialClient = g_settings.audioOnly
        ? SIZE{380, 230}
        : InitialClientPixelsForMonitor(initialMonitor);
    const UINT initialDpi = EffectiveMonitorDpi(initialMonitor, nullptr);
    const SIZE outerSize = OuterSizeForClientPixels(
        initialClient.cx, initialClient.cy, windowStyle, windowExStyle,
        initialDpi);
    if (g_settings.relativeWindowSize && !g_settings.audioOnly) {
        fwprintf(stderr,
                 L"[video] monitor-relative scale: %.2f%%, initial client %ld x %ld\n",
                 100.0 * g_settings.relativeWindowScalePpm /
                     kRelativeScaleUnit,
                 initialClient.cx, initialClient.cy);
    }
    POINT restoredOrigin{};
    const bool restoreOrigin =
        RestoredWindowOrigin(outerSize, restoredOrigin);

    HWND hwnd = CreateWindowExW(
        windowExStyle, wc.lpszClassName, title,
        windowStyle,
        restoreOrigin ? restoredOrigin.x : CW_USEDEFAULT,
        restoreOrigin ? restoredOrigin.y : CW_USEDEFAULT,
        outerSize.cx, outerSize.cy,
        nullptr, nullptr, hInst, nullptr);

    if (!hwnd) {
        CloseSavedLog();
        return 1;
    }
    if (!g_settings.audioOnly) NormalizeWindowSize(hwnd, true);
    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    if (g_settings.audioOnly) {
        fwprintf(stderr, L"[audio] audio-only OSD window: %ld x %ld\n",
                 clientRect.right - clientRect.left,
                 clientRect.bottom - clientRect.top);
    } else {
        fwprintf(stderr, L"[video] window client area: %ld x %ld%s\n",
                 clientRect.right - clientRect.left,
                 clientRect.bottom - clientRect.top,
                 g_settings.pixelPerfect
                     ? L" (pixel-perfect)"
                     : g_settings.relativeWindowSize
                           ? L" (monitor-relative)" : L"");
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    DWORD foregroundProcessId = 0;
    const HWND foregroundWindow = GetForegroundWindow();
    if (foregroundWindow) {
        GetWindowThreadProcessId(foregroundWindow, &foregroundProcessId);
    }
    UpdateBackgroundAudioMute(
        foregroundProcessId == GetCurrentProcessId());

    // Monitor-relative sizing restores the saved window ratio. Preserve a
    // genuinely smaller restored window, but do not suppress the established
    // auto-fullscreen behavior when that relative size already fills the
    // selected monitor. Use the monitor chosen before window creation so an
    // oversized decorated 4K window cannot make MonitorFromWindow pick a
    // neighboring display.
    const bool preserveSmallerRelativeWindow =
        !g_settings.audioOnly &&
        g_settings.relativeWindowSize &&
        !ClientSizeFillsMonitor(initialClient, initialMonitor);
    if (!g_settings.audioOnly && !preserveSmallerRelativeWindow &&
        SelectedResolutionMatchesMonitor(initialMonitor)) {
        fwprintf(stderr,
                 L"[video] selected resolution matches monitor; entering borderless fullscreen.\n");
        ToggleFullscreen(hwnd, true);
    }

    const uint64_t trackingStartMs = GetTickCount64();
    g_osdTrackingStartMs.store(trackingStartMs + kOsdTrackingWarmupMs,
                               std::memory_order_release);
    g_audioTrackingStartMs.store(trackingStartMs + kAudioTrackingWarmupMs,
                                 std::memory_order_release);
    fwprintf(stderr,
             L"[osd] video statistics warmup: first %llu ms excluded.\n",
             static_cast<unsigned long long>(kOsdTrackingWarmupMs));
    fwprintf(stderr,
             L"[osd] audio diagnostics warmup: first %llu ms excluded.\n",
             static_cast<unsigned long long>(kAudioTrackingWarmupMs));

    // One DirectShow graph owns one selected capture-filter instance and both
    // its video and audio branches. WASAPI remains an independent consumer.
    std::thread renderThread(AudioRenderThread);
    std::thread unifiedCaptureThread([hwnd, smokeTest]() {
        const bool initialized = g_settings.audioOnly
            ? AudioOnlyCaptureLoop()
            : UnifiedCaptureRenderLoop(g_videoHost);
        if (!initialized && g_running.load()) {
            fwprintf(stderr, g_settings.audioOnly
                         ? L"[capture] audio-only graph stopped.\n"
                         : L"[capture] single capture graph stopped.\n");
            if (!smokeTest) {
                if (g_settings.audioOnly) {
                    const HRESULT failure =
                        g_captureFailureHr.load(std::memory_order_acquire);
                    wchar_t message[512]{};
                    swprintf_s(message,
                               IsEnglishUi()
                                   ? L"Audio-only capture initialization failed.\n\nError: 0x%08X  %s\n\nSelect a compatible capture audio device or close other capture applications."
                                   : L"오디오 only 캡처 초기화에 실패했습니다.\n\n오류: 0x%08X  %s\n\n호환되는 캡처 오디오 장치를 선택하거나 다른 캡처 프로그램을 종료해 주세요.",
                               static_cast<unsigned int>(failure),
                               HrText(failure).c_str());
                    MessageBoxW(hwnd, message, L"Low Latency Capture Viewer",
                                MB_OK | MB_ICONERROR);
                    g_restartToSettings.store(true,
                                              std::memory_order_release);
                } else {
                const HRESULT failure =
                    g_captureFailureHr.load(std::memory_order_acquire);
                const std::wstring failureText = HrText(failure);
                wchar_t message[768]{};
                const bool compressedRequested = IsCompressedVideoFormat(
                    g_settings.pixelFormat);
                const wchar_t* failureFormat = compressedRequested
                    ? (IsEnglishUi()
                        ? L"Experimental compressed capture initialization failed.\n\n"
                          L"Error: 0x%08X  %s\n\n"
                          L"The selected device exposes a compressed %s stream, but "
                          L"Windows could not provide a compatible Media Foundation "
                          L"decoder or the device's compressed stream was not accepted.\n"
                          L"Close other capture applications, try the device's other "
                          L"compressed format or frame rate, and attach the diagnostic "
                          L"log when reporting the result. Raw NV12/YUY2 remains the "
                          L"recommended lowest-latency path."
                        : L"실험적 압축 캡처 초기화에 실패했습니다.\n\n"
                          L"오류: 0x%08X  %s\n\n"
                          L"선택한 장치가 %s 압축 스트림을 제공하지만, Windows에서 "
                          L"호환되는 Media Foundation 디코더를 찾지 못했거나 장치의 "
                          L"압축 스트림을 수락하지 못했습니다.\n"
                          L"다른 캡처 프로그램을 종료한 뒤, 장치의 다른 압축 포맷이나 "
                          L"프레임을 시도하고 결과를 제보할 때 진단 로그를 첨부해 주세요. "
                          L"최저 지연에는 기존 NV12/YUY2 원시 경로를 권장합니다.")
                    : IsEnglishUi()
                    ? L"Video capture initialization failed.\n\n"
                      L"Error: 0x%08X  %s\n\n"
                      L"The selected device may not provide the requested "
                      L"resolution/FPS/pixel format or a compatible 48 kHz "
                      L"mono/stereo PCM or 32-bit float capture-audio input.\n"
                      L"Close other apps that may be using the capture device "
                      L"(for example OBS or the vendor capture utility), then "
                      L"Try Auto pixel format, another resolution, or select "
                      L"a capture audio device. If logging "
                      L"is enabled, check the logs folder under LocalAppData."
                    : L"캡처 영상 초기화에 실패했습니다.\n\n"
                      L"오류: 0x%08X  %s\n\n"
                      L"선택한 장치가 지정한 해상도/FPS/픽셀 포맷 또는 "
                      L"호환되는 48 kHz mono/stereo PCM 또는 32-bit float "
                      L"캡처 오디오 입력을 "
                      L"제공하지 않을 수 있습니다.\n"
                      L"OBS 또는 제조사 캡처 프로그램처럼 캡처 장치를 사용 중인 "
                      L"다른 앱을 먼저 종료한 뒤, "
                      L"자동 픽셀 포맷, 다른 해상도 또는 캡처 오디오 장치를 "
                      L"선택해 다시 시도하고, "
                      L"로그 저장을 켠 경우 사용자 데이터 폴더의 logs를 확인해 주세요.";
                swprintf_s(
                    message,
                    failureFormat,
                    static_cast<unsigned int>(failure),
                    failureText.c_str(), PixelFormatName(g_settings.pixelFormat));
                MessageBoxW(hwnd, message, L"Low Latency Capture Viewer",
                            MB_OK | MB_ICONERROR);
                g_restartToSettings.store(true, std::memory_order_release);
                }
            }
            g_running.store(false);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
    });
    std::thread smokeTestStopper;
    if (smokeTest) {
        smokeTestStopper = std::thread([hwnd, longSmokeTest]() {
            const int tenths = longSmokeTest ? 600 : 100;
            for (int i = 0; i < tenths && g_running.load(); ++i) {
                Sleep(100);
            }
            if (g_running.load()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        });
    }
    if (!smokeTest) StartBackgroundUpdateCheck(hwnd);

    MSG m{};
    while (g_running.load() && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_MOUSEWHEEL) {
            // The message-loop fast path handles wheel volume before normal
            // dispatch, so it must also count as cursor activity.
            NoteFullscreenCursorActivity();
            if (AdjustVolumeFromWheel(hwnd, m.wParam, m.lParam)) {
                continue;
            }
        }
        // Keyboard focus can belong to the video-host child. Key messages do
        // not bubble to its parent, so route viewer shortcuts at the thread
        // message-loop level.
        if (m.message == WM_KEYDOWN && m.wParam == VK_TAB &&
            GetKeyState(VK_CONTROL) >= 0) {
            SendMessageW(hwnd, WM_TOGGLE_RUNTIME_OSD, 0, 0);
            continue;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_F2) {
            SendMessageW(hwnd, WM_OPEN_SETTINGS, 0, 0);
            continue;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_F3) {
            ToggleAudioOsd();
            continue;
        }
        if (m.message == WM_KEYDOWN && m.wParam == VK_F5) {
            SendMessageW(hwnd, WM_RESTORE_ONE_TO_ONE, 0, 0);
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g_running.store(false);
    g_updateCheckStop.store(true, std::memory_order_release);

    if (renderThread.joinable()) renderThread.join();
    if (unifiedCaptureThread.joinable()) unifiedCaptureThread.join();
    if (smokeTestStopper.joinable()) smokeTestStopper.join();
    if (g_updateCheckThread.joinable()) g_updateCheckThread.join();
    if (smokeTest) {
        fwprintf(stderr,
                 L"[smoke] captured=%llu presented=%llu replaced=%llu "
                 L"latest-app-latency=%.3f ms\n",
                 static_cast<unsigned long long>(
                     g_videoCapturedFrames.load(std::memory_order_acquire)),
                 static_cast<unsigned long long>(
                     g_videoPresentedFrames.load(std::memory_order_acquire)),
                 static_cast<unsigned long long>(
                     g_videoReplacedFrames.load(std::memory_order_acquire)),
                 g_videoAppLatencyUs.load(std::memory_order_acquire) / 1000.0);
        fwprintf(
            stderr,
            L"[smoke-audio] packet=%u frames/%.2f ms interval=%.2f ms "
            L"average-packet=%.2f ms average-interval=%.2f ms "
            L"ring=%.2f ms padding=%.2f ms correction=%+d ppm "
            L"underruns=%llu/%llu frames overruns=%llu/%llu frames\n",
            g_audioCapturePacketFrames.load(std::memory_order_acquire),
            1000.0 * g_audioCapturePacketFrames.load(
                         std::memory_order_relaxed) /
                kSampleRate,
            g_audioCaptureIntervalUs.load(std::memory_order_acquire) / 1000.0,
            g_audioCaptureCallbacks.load(std::memory_order_acquire) > 0
                ? 1000.0 * g_audioCaptureFrames.load(
                               std::memory_order_acquire) /
                      g_audioCaptureCallbacks.load(
                          std::memory_order_relaxed) /
                      kSampleRate
                : 0.0,
            g_audioCaptureCallbacks.load(std::memory_order_acquire) > 1
                ? g_audioCaptureIntervalTotalUs.load(
                      std::memory_order_acquire) /
                      1000.0 /
                      (g_audioCaptureCallbacks.load(
                           std::memory_order_relaxed) - 1)
                : 0.0,
            1000.0 *
                (g_audioRingFrames.load(std::memory_order_acquire) +
                 g_audioResamplerFrames.load(std::memory_order_acquire)) /
                kSampleRate,
            1000.0 * g_audioWasapiPaddingFrames.load(
                         std::memory_order_acquire) /
                kSampleRate,
            g_audioResamplePpm.load(std::memory_order_acquire),
            static_cast<unsigned long long>(
                g_underruns.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(
                g_audioUnderrunFrames.load(std::memory_order_acquire)),
            static_cast<unsigned long long>(
                g_ring.Overruns()),
            static_cast<unsigned long long>(
                g_audioOverrunFrames.load(std::memory_order_acquire)));
        fwprintf(stderr, L"[smoke-overlay] rendered=%llu same-swapchain=%s\n",
                 static_cast<unsigned long long>(
                     g_overlayRenderedFrames.load(
                         std::memory_order_acquire)),
                 g_overlayRenderedFrames.load(std::memory_order_relaxed) > 0
                     ? L"yes" : L"hidden");
        fwprintf(stderr, L"[smoke-volume] final=%d%%\n",
                 g_volumePercent.load(std::memory_order_acquire));
    }
    const bool restartToSettings =
        !smokeTest && g_restartToSettings.exchange(false,
                                                    std::memory_order_acq_rel);
    CloseSavedLog();
    if (restartToSettings) RelaunchWithSettings();
    return 0;
}
