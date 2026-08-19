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
#include <dshow.h>
#include <dvdmedia.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
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
#include <functiondiscoverykeys_devpkey.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <dxva.h>

#include "audio/AudioMix.h"
#include "audio/CaptureAudioFormat.h"
#include "diagnostics/Logger.h"
#include "ui/AudioOsdLayout.h"

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
#include <deque>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <future>
#include <functional>
#include <cstdarg>
#include <unordered_map>

// -----------------------------------------------------------------------------
// Minimal Sample Grabber declarations.
// qedit.h is no longer shipped in modern Windows SDKs, so the interfaces and
// CLSIDs are declared locally. The COM component itself is part of legacy
// DirectShow on many desktop Windows installations.
// -----------------------------------------------------------------------------

struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85")) ISampleGrabberCB : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample* pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) = 0;
};

struct __declspec(uuid("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")) ISampleGrabber : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};

static const CLSID CLSID_SampleGrabber =
{0xC1F400A0,0x3F08,0x11D3,{0x9F,0x0B,0x00,0x60,0x08,0x03,0x9E,0x37}};

static const CLSID CLSID_NullRenderer =
{0xC1F400A4,0x3F08,0x11D3,{0x9F,0x0B,0x00,0x60,0x08,0x03,0x9E,0x37}};

// -----------------------------------------------------------------------------
// User-tested settings.
// -----------------------------------------------------------------------------

constexpr wchar_t kCaptureName[] = L"AVerMedia HD Capture GC573 1";
constexpr wchar_t kAudioPinName[] = L"Audio";
constexpr wchar_t kVideoPinName[] = L"Video";

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBitsPerSample = 16;
constexpr wchar_t kAppVersionLabel[] = L"v1.1.5";

constexpr int kRecommendedCaptureBufferMs = 20;
constexpr int kMaximumVolumePercent = 200;
constexpr int kAudioOsdWidth = llcv::audio_osd::kWidth;
constexpr int kAudioOsdHeight = llcv::audio_osd::kHeight;
static constexpr int kWasapiBufferOptionsMs[] = {5, 10, 15, 20, 30, 40};
constexpr int kRecommendedWasapiBufferMs = 20;
static constexpr int kPcmQueueOptionsMs[] = {10, 15, 20, 30};
constexpr int kLowestPcmQueueMs = 10;

enum class AudioMode {
    WasapiShared,
    WasapiExclusive,
};

enum class DriftCorrectionMode {
    Off,
    Resample,
};

enum class VolumeHudPosition {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

enum class VideoPreset {
    R1920x1080,
    R2560x1440,
    R3840x2160,
};

enum class PresentationMode {
    AllowTearing,
    VSync,
};

enum class ScalingMode {
    Smooth,
    Sharp,
};

enum class VideoPixelFormat {
    Auto,
    Nv12,
    Yuy2,
    Mjpeg,
    P010,
};

enum class UiLanguage {
    Auto,
    Korean,
    English,
};

enum class InternalCaptureAudioState {
    Checking,
    Available,
    SeparateDeviceNeeded,
    Unknown,
};

struct CaptureDeviceInfo {
    std::wstring id;
    std::wstring name;
};

struct AudioEndpointInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

struct PixelFormatSupport {
    VideoPixelFormat format = VideoPixelFormat::Auto;
    int selectedFps = 0;
};

struct InternalCaptureAudioProbe {
    InternalCaptureAudioState state = InternalCaptureAudioState::Checking;
    HRESULT result = S_OK;
};

struct AppSettings {
    UiLanguage uiLanguage = UiLanguage::Auto;
    AudioMode audioMode = AudioMode::WasapiShared;
    int wasapiBufferMs = kRecommendedWasapiBufferMs;
    UINT32 wasapiSharedPeriodFrames = 0;
    DriftCorrectionMode driftCorrection = DriftCorrectionMode::Off;
    int pcmQueueTargetMs = kLowestPcmQueueMs;
    int volumePercent = 100;
    int leftVolumePercent = 100;
    int rightVolumePercent = 100;
    bool allowVolumeBoost = false;
    VolumeHudPosition volumeHudPosition = VolumeHudPosition::TopLeft;
    bool muteWhenBackground = false;
    PresentationMode presentationMode = PresentationMode::AllowTearing;
    ScalingMode scalingMode = ScalingMode::Smooth;
    VideoPreset videoPreset = VideoPreset::R1920x1080;
    VideoPixelFormat pixelFormat = VideoPixelFormat::Auto;
    int videoFrameRate = 0;
    std::wstring captureDeviceId;
    // Empty means: use an audio pin on the video filter when available, then
    // try a clearly matching DirectShow audio-capture filter.
    std::wstring captureAudioDeviceId;
    std::wstring audioOutputDeviceId;
    bool saveLog = false;
    bool showDiagnosticConsole = false;
    bool skipStartupSettings = false;
    bool audioOnly = false;
    bool forceHdr10 = false;
    bool pixelPerfect = true;
    bool relativeWindowSize = false;
    int relativeWindowScalePpm = 0;
    bool borderlessWindow = false;
    bool windowSnap = true;
    bool hasWindowPosition = false;
    int windowX = 0;
    int windowY = 0;
    std::wstring monitorDevice;
};

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
        {L"%.2f ms (권장)", L"%.2f ms (recommended)"},
        {L"%.2f ms (최저)", L"%.2f ms (minimum)"},
        {L"%d ms (권장)", L"%d ms (recommended)"},
        {L"Shared 저지연 지원 확인 중…", L"Checking Shared low-latency support…"},
        {L"IAudioClient3 지원됨 · Shared %.2f–%.2f ms · 검사 %.1f ms", L"IAudioClient3 available · Shared %.2f–%.2f ms · probe %.1f ms"},
        {L"IAudioClient3 사용 불가 · 기본 Shared로 자동 전환 (0x%08X)", L"IAudioClient3 unavailable · using classic Shared (0x%08X)"},
        {L"지원 모드 없음: 다른 장치 또는 해상도를 선택하세요.", L"No supported mode: choose another device or resolution."},
        {L"자동 인식: ", L"Detected: "},
        {L"지원 프레임 없음", L"No supported frame rate"},
        {L"자동 선택 (권장 프레임)", L"Auto select (recommended frame rate)"},
        {L"지원 포맷 없음", L"No supported format"},
        {L"자동 선택 (NV12 우선 · 권장)", L"Auto select (NV12 first · recommended)"},
        {L"P010 10-bit HDR10 (실험적)", L"P010 10-bit HDR10 (experimental)"},
        {L"P010 HDR10 강제 (메타데이터 없을 때 · 실험적)", L"Force P010 HDR10 (when metadata is missing · experimental)"},
        {L"MJPEG (실험적 압축 호환)", L"MJPEG (experimental compressed compatibility)"},
        {L"오디오 출력 모드", L"Audio output mode"},
        {L"WASAPI Shared (호환성 우선 · 권장)", L"WASAPI Shared (compatibility · recommended)"},
        {L"WASAPI Exclusive (지연 최소화 · 장치 독점)", L"WASAPI Exclusive (minimum latency · exclusive device)"},
        {L"WASAPI 출력 장치", L"WASAPI output device"},
        {L"Windows 기본 출력 장치 따라가기 (권장)", L"Follow Windows default output (recommended)"},
        {L" (현재 기본)", L" (current default)"},
        {L"WASAPI 출력 버퍼", L"WASAPI output buffer"},
        {L"볼륨 HUD 위치", L"Volume HUD position"},
        {L"100% 이상 볼륨 증폭 허용 (최대 200%)", L"Allow volume boost above 100% (up to 200%)"},
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
        {L"자동 리샘플링 (장시간 안정성 권장)", L"Automatic resampling (recommended for long sessions)"},
        {L"PCM 버퍼 목표", L"PCM buffer target"},
        {L"10 ms (최저 지연)", L"10 ms (minimum latency)"},
        {L"15 ms (저지연 목표)", L"15 ms (low-latency target)"},
        {L"20 ms (안정 권장)", L"20 ms (stable recommendation)"},
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
        {L"진단 로그 파일 저장 (사용자 폴더)", L"Save diagnostic log (user folder)"},
        {L"진단 콘솔 창 표시", L"Show diagnostic console window"},
        {L"다음 실행부터 바로 시작", L"Start directly next time"},
        {L"저장된 설정으로 바로 실행 · Shift 실행 또는 F2로 설정 열기", L"Starts with saved settings · hold Shift at launch or press F2 for settings"},
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

struct AudioClient3Support {
    bool supported = false;
    HRESULT result = E_FAIL;
    UINT32 defaultFrames = 0;
    UINT32 fundamentalFrames = 0;
    UINT32 minimumFrames = 0;
    UINT32 maximumFrames = 0;
    double probeMilliseconds = 0.0;
};

// 500 ms ring capacity. The program deliberately does NOT wait for this much
// data; it is only headroom. Old data is dropped if the producer overruns.
constexpr size_t kRingFrames = 48000 / 2;

static HWND g_videoHost = nullptr;
static bool g_suppressSettingsSave = false;
static std::atomic<bool> g_running{true};
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
static std::atomic<UINT32> g_audioCapturePacketFrames{0};
static std::atomic<int64_t> g_audioCaptureIntervalUs{0};
static std::atomic<LONG> g_audioCaptureAllocatorFrames{0};
static std::atomic<LONG> g_audioCaptureAllocatorBuffers{0};
static std::atomic<UINT32> g_audioRingFrames{0};
static std::atomic<UINT32> g_audioResamplerFrames{0};
static std::atomic<int> g_audioResamplePpm{0};
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
static std::atomic<uint64_t> g_defaultAudioEndpointGeneration{0};
static llcv::diagnostics::Logger g_logger;
static std::mutex g_activeAudioOutputMutex;

enum class AudioErrorKind {
    Underrun,
    Overrun,
};

enum class AudioErrorCause {
    InputLate,
    PcmDepletion,
    Resampler,
    Overrun,
};

struct AudioErrorEvent {
    uint64_t timestampMs = 0;
    uint32_t frames = 0;
    AudioErrorKind kind = AudioErrorKind::Underrun;
    AudioErrorCause cause = AudioErrorCause::PcmDepletion;
};

// Error events are rare. A small bounded history is enough to distinguish a
// burst from occasional errors without adding work to the normal audio loop.
constexpr size_t kAudioErrorHistoryCapacity = 128;
static std::mutex g_audioErrorHistoryMutex;
static std::deque<AudioErrorEvent> g_audioErrorHistory;

static bool AudioTrackingActive();

static void RecordAudioErrorEvent(uint64_t timestampMs, uint32_t frames,
                                  AudioErrorKind kind,
                                  AudioErrorCause cause) {
    if (!timestampMs || !AudioTrackingActive()) return;
    std::lock_guard<std::mutex> lock(g_audioErrorHistoryMutex);
    if (g_audioErrorHistory.size() >= kAudioErrorHistoryCapacity) {
        g_audioErrorHistory.pop_front();
    }
    g_audioErrorHistory.push_back(
        AudioErrorEvent{timestampMs, frames, kind, cause});
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
    switch (format) {
    case VideoPixelFormat::Mjpeg: return L"MJPEG";
    case VideoPixelFormat::Yuy2: return L"YUY2";
    case VideoPixelFormat::Nv12: return L"NV12";
    case VideoPixelFormat::P010: return L"P010";
    default: return L"Auto";
    }
}

static const GUID& PixelFormatSubtype(VideoPixelFormat format) {
    switch (format) {
    case VideoPixelFormat::Mjpeg: return MEDIASUBTYPE_MJPG;
    case VideoPixelFormat::Yuy2: return MEDIASUBTYPE_YUY2;
    case VideoPixelFormat::P010: return MFVideoFormat_P010;
    default: return MEDIASUBTYPE_NV12;
    }
}

static bool IsCompressedVideoFormat(VideoPixelFormat format) {
    return format == VideoPixelFormat::Mjpeg;
}

static VideoPixelFormat VideoPixelFormatFromSubtype(const GUID& subtype) {
    if (subtype == MEDIASUBTYPE_NV12) return VideoPixelFormat::Nv12;
    if (subtype == MEDIASUBTYPE_YUY2) return VideoPixelFormat::Yuy2;
    if (subtype == MEDIASUBTYPE_MJPG || subtype == MFVideoFormat_MJPG) {
        return VideoPixelFormat::Mjpeg;
    }
    if (subtype == MFVideoFormat_P010) return VideoPixelFormat::P010;
    return VideoPixelFormat::Auto;
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

static WAVEFORMATEX PcmOutputFormat() {
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = kChannels;
    wf.nSamplesPerSec = kSampleRate;
    wf.wBitsPerSample = kBitsPerSample;
    wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    return wf;
}

static UINT32 ClosestSupportedSharedPeriod(
    UINT32 requestedFrames, const AudioClient3Support& support) {
    if (!support.supported || support.fundamentalFrames == 0) return 0;

    const UINT32 fundamental = support.fundamentalFrames;
    const UINT32 firstMultiple =
        (support.minimumFrames + fundamental - 1) / fundamental;
    const UINT32 lastMultiple = support.maximumFrames / fundamental;
    if (firstMultiple > lastMultiple) return 0;

    UINT32 requestedMultiple =
        (requestedFrames + fundamental / 2) / fundamental;
    requestedMultiple = (std::max)(firstMultiple, requestedMultiple);
    requestedMultiple = (std::min)(lastMultiple, requestedMultiple);
    return requestedMultiple * fundamental;
}

static AudioClient3Support QueryAudioClient3Support(IMMDevice* device) {
    AudioClient3Support support{};
    const auto begin = std::chrono::steady_clock::now();

    IAudioClient* baseClient = nullptr;
    IAudioClient3* client3 = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                                  nullptr,
                                  reinterpret_cast<void**>(&baseClient));
    if (SUCCEEDED(hr)) {
        hr = baseClient->QueryInterface(IID_PPV_ARGS(&client3));
    }
    if (SUCCEEDED(hr)) {
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.eCategory = AudioCategory_Media;
        const HRESULT propertiesHr = client3->SetClientProperties(&properties);
        if (FAILED(propertiesHr)) hr = propertiesHr;
    }
    if (SUCCEEDED(hr)) {
        const WAVEFORMATEX wf = PcmOutputFormat();
        hr = client3->GetSharedModeEnginePeriod(
            &wf, &support.defaultFrames, &support.fundamentalFrames,
            &support.minimumFrames, &support.maximumFrames);
    }

    support.result = hr;
    support.supported = SUCCEEDED(hr) && support.defaultFrames != 0 &&
                        support.fundamentalFrames != 0 &&
                        support.minimumFrames != 0 &&
                        support.maximumFrames >= support.minimumFrames;
    const auto end = std::chrono::steady_clock::now();
    support.probeMilliseconds =
        std::chrono::duration<double, std::milli>(end - begin).count();

    SafeRelease(client3);
    SafeRelease(baseClient);
    return support;
}

static HRESULT GetConfiguredAudioEndpoint(IMMDeviceEnumerator* enumerator,
                                          const std::wstring& endpointId,
                                          IMMDevice** device) {
    if (!enumerator || !device) return E_POINTER;
    *device = nullptr;
    return endpointId.empty()
               ? enumerator->GetDefaultAudioEndpoint(
                     eRender, eConsole, device)
               : enumerator->GetDevice(endpointId.c_str(), device);
}

static AudioClient3Support ProbeAudioClient3Support(
    const std::wstring& endpointId) {
    AudioClient3Support support{};
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) hr = S_OK;
    if (FAILED(hr)) {
        support.result = hr;
        return support;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = GetConfiguredAudioEndpoint(enumerator, endpointId, &device);
    }
    if (SUCCEEDED(hr)) {
        support = QueryAudioClient3Support(device);
    } else {
        support.result = hr;
    }

    SafeRelease(device);
    SafeRelease(enumerator);
    if (uninitialize) CoUninitialize();
    return support;
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

static std::wstring MonikerDisplayName(IMoniker* moniker) {
    if (!moniker) return {};
    IBindCtx* bindContext = nullptr;
    if (FAILED(CreateBindCtx(0, &bindContext))) return {};
    LPOLESTR value = nullptr;
    std::wstring result;
    if (SUCCEEDED(moniker->GetDisplayName(bindContext, nullptr, &value)) &&
        value) {
        result = value;
    }
    CoTaskMemFree(value);
    bindContext->Release();
    return result;
}

static std::wstring MonikerFriendlyName(IMoniker* moniker) {
    if (!moniker) return {};
    IPropertyBag* bag = nullptr;
    VARIANT value;
    VariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(moniker->BindToStorage(
            nullptr, nullptr, IID_PPV_ARGS(&bag))) &&
        SUCCEEDED(bag->Read(L"FriendlyName", &value, nullptr)) &&
        value.vt == VT_BSTR && value.bstrVal) {
        result = value.bstrVal;
    }
    VariantClear(&value);
    SafeRelease(bag);
    return result;
}

static std::vector<CaptureDeviceInfo> EnumerateInputDevices(
    const CLSID& category) {
    std::vector<CaptureDeviceInfo> devices;
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initHr);
    if (initHr == RPC_E_CHANGED_MODE) initHr = S_OK;
    if (FAILED(initHr)) return devices;
    ICreateDevEnum* deviceEnumerator = nullptr;
    IEnumMoniker* monikers = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&deviceEnumerator));
    if (SUCCEEDED(hr)) {
        hr = deviceEnumerator->CreateClassEnumerator(category, &monikers, 0);
    }
    IMoniker* moniker = nullptr;
    while (monikers && monikers->Next(1, &moniker, nullptr) == S_OK) {
        CaptureDeviceInfo info{};
        info.id = MonikerDisplayName(moniker);
        info.name = MonikerFriendlyName(moniker);
        if (!info.id.empty() && !info.name.empty()) {
            devices.push_back(std::move(info));
        }
        SafeRelease(moniker);
    }
    SafeRelease(monikers);
    SafeRelease(deviceEnumerator);
    if (uninitialize) CoUninitialize();
    return devices;
}

static std::vector<CaptureDeviceInfo> EnumerateCaptureDevices() {
    return EnumerateInputDevices(CLSID_VideoInputDeviceCategory);
}

static std::vector<CaptureDeviceInfo> EnumerateCaptureAudioDevices() {
    return EnumerateInputDevices(CLSID_AudioInputDeviceCategory);
}

static std::wstring AudioEndpointFriendlyName(IMMDevice* device) {
    IPropertyStore* store = nullptr;
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring result;
    if (device && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
        SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        result = value.pwszVal;
    }
    PropVariantClear(&value);
    SafeRelease(store);
    return result;
}

static std::vector<AudioEndpointInfo> EnumerateAudioEndpoints() {
    std::vector<AudioEndpointInfo> endpoints;
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(initHr);
    if (initHr == RPC_E_CHANGED_MODE) initHr = S_OK;
    if (FAILED(initHr)) return endpoints;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    IMMDevice* defaultDevice = nullptr;
    LPWSTR defaultIdRaw = nullptr;
    std::wstring defaultId;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) {
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
                eRender, eConsole, &defaultDevice)) &&
            SUCCEEDED(defaultDevice->GetId(&defaultIdRaw)) && defaultIdRaw) {
            defaultId = defaultIdRaw;
        }
        hr = enumerator->EnumAudioEndpoints(
            eRender, DEVICE_STATE_ACTIVE, &collection);
    }
    UINT count = 0;
    if (collection) collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        LPWSTR idRaw = nullptr;
        if (SUCCEEDED(collection->Item(i, &device)) &&
            SUCCEEDED(device->GetId(&idRaw)) && idRaw) {
            AudioEndpointInfo info{};
            info.id = idRaw;
            info.name = AudioEndpointFriendlyName(device);
            info.isDefault = info.id == defaultId;
            if (!info.name.empty()) endpoints.push_back(std::move(info));
        }
        CoTaskMemFree(idRaw);
        SafeRelease(device);
    }
    CoTaskMemFree(defaultIdRaw);
    SafeRelease(defaultDevice);
    SafeRelease(collection);
    SafeRelease(enumerator);
    if (uninitialize) CoUninitialize();
    return endpoints;
}

static std::wstring ConfiguredAudioEndpointName(
    const std::wstring& endpointId) {
    const auto endpoints = EnumerateAudioEndpoints();
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
static constexpr int kRelativeScaleSettingsVersion = 4;

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

static HMONITOR SavedViewerMonitor();

static void LoadSettings() {
    MigrateLegacySettings();
    const std::wstring path = SettingsPath();
    wchar_t language[16]{};
    wchar_t audio[32]{};
    wchar_t bufferMs[16]{};
    wchar_t sharedPeriodFrames[16]{};
    wchar_t driftCorrection[32]{};
    wchar_t pcmQueueTargetMs[16]{};
    wchar_t volume[16]{};
    wchar_t leftVolume[16]{};
    wchar_t rightVolume[16]{};
    wchar_t allowVolumeBoost[8]{};
    wchar_t volumeHudPosition[32]{};
    wchar_t muteWhenBackground[8]{};
    wchar_t audioOutputDeviceId[1024]{};
    wchar_t resolution[32]{};
    wchar_t captureDeviceId[1024]{};
    wchar_t captureAudioDeviceId[1024]{};
    wchar_t pixelFormat[32]{};
    wchar_t frameRate[16]{};
    wchar_t presentation[32]{};
    wchar_t pixelPerfect[8]{};
    wchar_t relativeWindowSize[8]{};
    wchar_t relativeWindowScale[16]{};
    wchar_t relativeWindowScaleVersion[16]{};
    wchar_t borderlessWindow[8]{};
    wchar_t windowSnap[8]{};
    wchar_t windowX[32]{};
    wchar_t windowY[32]{};
    wchar_t monitorDevice[64]{};
    wchar_t saveLog[8]{};
    wchar_t showDiagnosticConsole[8]{};
    wchar_t skipStartupSettings[8]{};
    wchar_t audioOnly[8]{};
    wchar_t forceHdr10[8]{};

    GetPrivateProfileStringW(L"General", L"Language", L"Auto", language,
                             ARRAYSIZE(language), path.c_str());
    GetPrivateProfileStringW(L"General", L"SkipStartupSettings", L"0",
                             skipStartupSettings,
                             ARRAYSIZE(skipStartupSettings), path.c_str());
    GetPrivateProfileStringW(L"General", L"AudioOnly", L"0", audioOnly,
                             ARRAYSIZE(audioOnly), path.c_str());
    GetPrivateProfileStringW(L"Video", L"ForceHdr10", L"0", forceHdr10,
                             ARRAYSIZE(forceHdr10), path.c_str());

    GetPrivateProfileStringW(L"Audio", L"Mode", L"Shared", audio,
                             ARRAYSIZE(audio), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"BufferMs", L"20", bufferMs,
                             ARRAYSIZE(bufferMs), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"SharedPeriodFrames", L"0",
                             sharedPeriodFrames, ARRAYSIZE(sharedPeriodFrames),
                             path.c_str());
    GetPrivateProfileStringW(L"Audio", L"DriftCorrection", L"Off",
                             driftCorrection, ARRAYSIZE(driftCorrection),
                             path.c_str());
    GetPrivateProfileStringW(L"Audio", L"PcmQueueTargetMs", L"",
                             pcmQueueTargetMs, ARRAYSIZE(pcmQueueTargetMs),
                             path.c_str());
    GetPrivateProfileStringW(L"Audio", L"Volume", L"100", volume,
                             ARRAYSIZE(volume), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"LeftVolume", L"100", leftVolume,
                             ARRAYSIZE(leftVolume), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"RightVolume", L"100", rightVolume,
                             ARRAYSIZE(rightVolume), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"AllowVolumeBoost", L"0",
                             allowVolumeBoost, ARRAYSIZE(allowVolumeBoost),
                             path.c_str());
    GetPrivateProfileStringW(L"Audio", L"VolumeHudPosition", L"TopLeft",
                             volumeHudPosition,
                             ARRAYSIZE(volumeHudPosition), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"MuteWhenBackground", L"0",
                             muteWhenBackground,
                             ARRAYSIZE(muteWhenBackground), path.c_str());
    GetPrivateProfileStringW(L"Audio", L"OutputDeviceId", L"",
                             audioOutputDeviceId,
                             ARRAYSIZE(audioOutputDeviceId), path.c_str());
    GetPrivateProfileStringW(L"Video", L"Resolution", L"1920x1080", resolution,
                             ARRAYSIZE(resolution), path.c_str());
    GetPrivateProfileStringW(L"Video", L"CaptureDeviceId", L"",
                             captureDeviceId, ARRAYSIZE(captureDeviceId),
                             path.c_str());
    GetPrivateProfileStringW(L"Video", L"CaptureAudioDeviceId", L"",
                             captureAudioDeviceId,
                             ARRAYSIZE(captureAudioDeviceId), path.c_str());
    GetPrivateProfileStringW(L"Video", L"PixelFormat", L"Auto",
                             pixelFormat, ARRAYSIZE(pixelFormat), path.c_str());
    GetPrivateProfileStringW(L"Video", L"FrameRate", L"0", frameRate,
                             ARRAYSIZE(frameRate), path.c_str());
    GetPrivateProfileStringW(L"Video", L"Presentation", L"AllowTearing",
                             presentation, ARRAYSIZE(presentation), path.c_str());
    wchar_t scaling[32]{};
    GetPrivateProfileStringW(L"Video", L"Scaling", L"Smooth",
                             scaling, ARRAYSIZE(scaling), path.c_str());
    GetPrivateProfileStringW(L"Video", L"PixelPerfect", L"1", pixelPerfect,
                             ARRAYSIZE(pixelPerfect), path.c_str());
    GetPrivateProfileStringW(L"Video", L"RelativeWindowSize", L"0",
                             relativeWindowSize,
                             ARRAYSIZE(relativeWindowSize), path.c_str());
    GetPrivateProfileStringW(L"Video", L"RelativeWindowScalePpm", L"0",
                             relativeWindowScale,
                             ARRAYSIZE(relativeWindowScale), path.c_str());
    GetPrivateProfileStringW(L"Video", L"RelativeWindowScaleVersion", L"0",
                             relativeWindowScaleVersion,
                             ARRAYSIZE(relativeWindowScaleVersion),
                             path.c_str());
    GetPrivateProfileStringW(L"Video", L"BorderlessWindow", L"0", borderlessWindow,
                             ARRAYSIZE(borderlessWindow), path.c_str());
    GetPrivateProfileStringW(L"Window", L"Snap", L"1", windowSnap,
                             ARRAYSIZE(windowSnap), path.c_str());
    GetPrivateProfileStringW(L"Window", L"X", L"", windowX,
                             ARRAYSIZE(windowX), path.c_str());
    GetPrivateProfileStringW(L"Window", L"Y", L"", windowY,
                             ARRAYSIZE(windowY), path.c_str());
    GetPrivateProfileStringW(L"Window", L"Monitor", L"", monitorDevice,
                             ARRAYSIZE(monitorDevice), path.c_str());
    GetPrivateProfileStringW(L"Diagnostics", L"SaveLog", L"0", saveLog,
                             ARRAYSIZE(saveLog), path.c_str());
    GetPrivateProfileStringW(L"Diagnostics", L"ShowConsole", L"0",
                             showDiagnosticConsole,
                             ARRAYSIZE(showDiagnosticConsole), path.c_str());

    if (_wcsicmp(language, L"English") == 0) {
        g_settings.uiLanguage = UiLanguage::English;
    } else if (_wcsicmp(language, L"Korean") == 0) {
        g_settings.uiLanguage = UiLanguage::Korean;
    } else {
        g_settings.uiLanguage = UiLanguage::Auto;
    }

    g_settings.audioMode = (_wcsicmp(audio, L"Exclusive") == 0)
                               ? AudioMode::WasapiExclusive
                               : AudioMode::WasapiShared;
    const int requestedBufferMs = static_cast<int>(wcstol(bufferMs, nullptr, 10));
    g_settings.wasapiBufferMs = kRecommendedWasapiBufferMs;
    for (const int option : kWasapiBufferOptionsMs) {
        if (requestedBufferMs == option) {
            g_settings.wasapiBufferMs = option;
            break;
        }
    }
    g_settings.wasapiSharedPeriodFrames =
        static_cast<UINT32>(wcstoul(sharedPeriodFrames, nullptr, 10));
    g_settings.driftCorrection =
        _wcsicmp(driftCorrection, L"Resample") == 0
            ? DriftCorrectionMode::Resample
            : DriftCorrectionMode::Off;
    // v0.13 and older implicitly used 20 ms with resampling and the first
    // available 10 ms packet without it. Preserve that behavior when the new
    // independent key is absent.
    g_settings.pcmQueueTargetMs =
        g_settings.driftCorrection == DriftCorrectionMode::Resample
            ? 20 : kLowestPcmQueueMs;
    if (pcmQueueTargetMs[0]) {
        const int requestedQueueMs = static_cast<int>(
            wcstol(pcmQueueTargetMs, nullptr, 10));
        for (const int option : kPcmQueueOptionsMs) {
            if (requestedQueueMs == option) {
                g_settings.pcmQueueTargetMs = option;
                break;
            }
        }
    }
    g_settings.allowVolumeBoost =
        wcstol(allowVolumeBoost, nullptr, 10) != 0;
    const int loadedVolume = std::clamp(
        static_cast<int>(wcstol(volume, nullptr, 10)), 0,
        g_settings.allowVolumeBoost ? kMaximumVolumePercent : 100);
    g_settings.volumePercent =
        std::clamp(((loadedVolume + 2) / 5) * 5, 0,
                   g_settings.allowVolumeBoost ? kMaximumVolumePercent : 100);
    g_settings.leftVolumePercent = std::clamp(
        ((static_cast<int>(wcstol(leftVolume, nullptr, 10)) + 2) / 5) * 5,
        0, 100);
    g_settings.rightVolumePercent = std::clamp(
        ((static_cast<int>(wcstol(rightVolume, nullptr, 10)) + 2) / 5) * 5,
        0, 100);
    if (_wcsicmp(volumeHudPosition, L"TopRight") == 0) {
        g_settings.volumeHudPosition = VolumeHudPosition::TopRight;
    } else if (_wcsicmp(volumeHudPosition, L"BottomLeft") == 0) {
        g_settings.volumeHudPosition = VolumeHudPosition::BottomLeft;
    } else if (_wcsicmp(volumeHudPosition, L"BottomRight") == 0) {
        g_settings.volumeHudPosition = VolumeHudPosition::BottomRight;
    } else {
        g_settings.volumeHudPosition = VolumeHudPosition::TopLeft;
    }
    g_settings.muteWhenBackground =
        wcstol(muteWhenBackground, nullptr, 10) != 0;
    g_volumePercent.store(g_settings.volumePercent,
                          std::memory_order_release);
    g_leftVolumePercent.store(g_settings.leftVolumePercent,
                              std::memory_order_release);
    g_rightVolumePercent.store(g_settings.rightVolumePercent,
                               std::memory_order_release);
    g_settings.presentationMode = (_wcsicmp(presentation, L"VSync") == 0)
                                      ? PresentationMode::VSync
                                      : PresentationMode::AllowTearing;
    g_settings.scalingMode = (_wcsicmp(scaling, L"Sharp") == 0)
        ? ScalingMode::Sharp : ScalingMode::Smooth;
    g_settings.audioOutputDeviceId = audioOutputDeviceId;
    g_settings.captureDeviceId = captureDeviceId;
    g_settings.captureAudioDeviceId = captureAudioDeviceId;
    if (_wcsicmp(pixelFormat, L"NV12") == 0) {
        g_settings.pixelFormat = VideoPixelFormat::Nv12;
    } else if (_wcsicmp(pixelFormat, L"YUY2") == 0) {
        g_settings.pixelFormat = VideoPixelFormat::Yuy2;
    } else if (_wcsicmp(pixelFormat, L"MJPEG") == 0) {
        g_settings.pixelFormat = VideoPixelFormat::Mjpeg;
    } else if (_wcsicmp(pixelFormat, L"P010") == 0) {
        g_settings.pixelFormat = VideoPixelFormat::P010;
    } else {
        // Older beta settings may contain H.264 or MPEG-4. Those modes are
        // no longer accepted by this low-latency viewer, so use Auto instead.
        g_settings.pixelFormat = VideoPixelFormat::Auto;
    }
    const int savedFrameRate = static_cast<int>(
        wcstol(frameRate, nullptr, 10));
    g_settings.videoFrameRate = savedFrameRate > 0 ? savedFrameRate : 0;
    g_settings.saveLog = wcstol(saveLog, nullptr, 10) != 0;
    g_settings.showDiagnosticConsole =
        wcstol(showDiagnosticConsole, nullptr, 10) != 0;
    g_settings.skipStartupSettings =
        wcstol(skipStartupSettings, nullptr, 10) != 0;
    g_settings.audioOnly = wcstol(audioOnly, nullptr, 10) != 0;
    g_settings.forceHdr10 = wcstol(forceHdr10, nullptr, 10) != 0;

    if (_wcsicmp(resolution, L"1920x1080") == 0) {
        g_settings.videoPreset = VideoPreset::R1920x1080;
    } else if (_wcsicmp(resolution, L"3840x2160") == 0) {
        g_settings.videoPreset = VideoPreset::R3840x2160;
    } else {
        g_settings.videoPreset = VideoPreset::R2560x1440;
    }
    g_settings.pixelPerfect = (wcstol(pixelPerfect, nullptr, 10) != 0);
    g_settings.relativeWindowSize =
        (wcstol(relativeWindowSize, nullptr, 10) != 0);
    g_settings.relativeWindowScalePpm = std::clamp(
        static_cast<int>(wcstol(relativeWindowScale, nullptr, 10)),
        0, kRelativeScaleUnit);
    g_settings.borderlessWindow = (wcstol(borderlessWindow, nullptr, 10) != 0);
    g_settings.windowSnap = (wcstol(windowSnap, nullptr, 10) != 0);
    if (windowX[0] && windowY[0]) {
        g_settings.hasWindowPosition = true;
        g_settings.windowX = static_cast<int>(wcstol(windowX, nullptr, 10));
        g_settings.windowY = static_cast<int>(wcstol(windowY, nullptr, 10));
        g_settings.monitorDevice = monitorDevice;
    }

    // Older builds used the smaller normalized dimension on mixed-aspect
    // displays. Correct only values that exactly match that legacy formula;
    // do not recompute every saved scale from the last monitor. Recomputing
    // would destroy a legitimate 50%/66.67% ratio after the user moved the
    // viewer to another monitor and closed it there.
    const int relativeScaleVersion = static_cast<int>(
        wcstol(relativeWindowScaleVersion, nullptr, 10));
    if (relativeScaleVersion < kRelativeScaleSettingsVersion &&
        g_settings.relativeWindowSize && g_settings.pixelPerfect &&
        g_settings.hasWindowPosition) {
        const HMONITOR savedViewerMonitor = SavedViewerMonitor();
        const int legacyScale = LegacyRelativeScaleForMonitor(
            savedViewerMonitor);
        const int correctedScale = RelativeScaleForMonitor(
            savedViewerMonitor);
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
    const std::wstring path = SettingsPath();
    const wchar_t* language = L"Auto";
    if (g_settings.uiLanguage == UiLanguage::Korean) language = L"Korean";
    if (g_settings.uiLanguage == UiLanguage::English) language = L"English";
    WritePrivateProfileStringW(L"General", L"Language", language,
                               path.c_str());
    WritePrivateProfileStringW(L"General", L"SkipStartupSettings",
                               g_settings.skipStartupSettings ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"General", L"AudioOnly",
                               g_settings.audioOnly ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"Video", L"ForceHdr10",
                               g_settings.forceHdr10 ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(
        L"Audio", L"Mode",
        g_settings.audioMode == AudioMode::WasapiExclusive ? L"Exclusive" : L"Shared",
        path.c_str());
    wchar_t bufferMs[16]{};
    swprintf_s(bufferMs, L"%d", g_settings.wasapiBufferMs);
    WritePrivateProfileStringW(L"Audio", L"BufferMs", bufferMs, path.c_str());
    wchar_t sharedPeriodFrames[16]{};
    swprintf_s(sharedPeriodFrames, L"%u", g_settings.wasapiSharedPeriodFrames);
    WritePrivateProfileStringW(L"Audio", L"SharedPeriodFrames",
                               sharedPeriodFrames, path.c_str());
    WritePrivateProfileStringW(
        L"Audio", L"DriftCorrection",
        g_settings.driftCorrection == DriftCorrectionMode::Resample
            ? L"Resample" : L"Off",
        path.c_str());
    wchar_t pcmQueueTargetMs[16]{};
    swprintf_s(pcmQueueTargetMs, L"%d", g_settings.pcmQueueTargetMs);
    WritePrivateProfileStringW(L"Audio", L"PcmQueueTargetMs",
                               pcmQueueTargetMs, path.c_str());
    wchar_t volume[16]{};
    swprintf_s(volume, L"%d", g_settings.volumePercent);
    WritePrivateProfileStringW(L"Audio", L"Volume", volume, path.c_str());
    wchar_t leftVolume[16]{};
    swprintf_s(leftVolume, L"%d", g_settings.leftVolumePercent);
    WritePrivateProfileStringW(L"Audio", L"LeftVolume", leftVolume,
                               path.c_str());
    wchar_t rightVolume[16]{};
    swprintf_s(rightVolume, L"%d", g_settings.rightVolumePercent);
    WritePrivateProfileStringW(L"Audio", L"RightVolume", rightVolume,
                               path.c_str());
    WritePrivateProfileStringW(L"Audio", L"AllowVolumeBoost",
                               g_settings.allowVolumeBoost ? L"1" : L"0",
                               path.c_str());
    const wchar_t* hudPosition = L"TopLeft";
    switch (g_settings.volumeHudPosition) {
    case VolumeHudPosition::TopRight: hudPosition = L"TopRight"; break;
    case VolumeHudPosition::BottomLeft: hudPosition = L"BottomLeft"; break;
    case VolumeHudPosition::BottomRight: hudPosition = L"BottomRight"; break;
    default: break;
    }
    WritePrivateProfileStringW(L"Audio", L"VolumeHudPosition", hudPosition,
                               path.c_str());
    WritePrivateProfileStringW(L"Audio", L"MuteWhenBackground",
                               g_settings.muteWhenBackground ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"Audio", L"OutputDeviceId",
                               g_settings.audioOutputDeviceId.c_str(),
                               path.c_str());

    const auto& video = CurrentVideoPreset();
    wchar_t resolution[32]{};
    swprintf_s(resolution, L"%dx%d", video.width, video.height);
    WritePrivateProfileStringW(L"Video", L"Resolution", resolution, path.c_str());
    WritePrivateProfileStringW(L"Video", L"CaptureDeviceId",
                               g_settings.captureDeviceId.c_str(), path.c_str());
    WritePrivateProfileStringW(L"Video", L"CaptureAudioDeviceId",
                               g_settings.captureAudioDeviceId.c_str(),
                               path.c_str());
    WritePrivateProfileStringW(L"Video", L"PixelFormat",
                               PixelFormatName(g_settings.pixelFormat),
                               path.c_str());
    wchar_t frameRate[16]{};
    swprintf_s(frameRate, L"%d", g_settings.videoFrameRate);
    WritePrivateProfileStringW(L"Video", L"FrameRate", frameRate,
                               path.c_str());
    WritePrivateProfileStringW(
        L"Video", L"Presentation",
        g_settings.presentationMode == PresentationMode::VSync
            ? L"VSync" : L"AllowTearing",
        path.c_str());
    WritePrivateProfileStringW(
        L"Video", L"Scaling",
        g_settings.scalingMode == ScalingMode::Sharp ? L"Sharp" : L"Smooth",
        path.c_str());
    WritePrivateProfileStringW(L"Video", L"PixelPerfect",
                               g_settings.pixelPerfect ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Video", L"RelativeWindowSize",
                               g_settings.relativeWindowSize ? L"1" : L"0",
                               path.c_str());
    wchar_t relativeScale[16]{};
    swprintf_s(relativeScale, L"%d", g_settings.relativeWindowScalePpm);
    WritePrivateProfileStringW(L"Video", L"RelativeWindowScalePpm",
                               relativeScale, path.c_str());
    wchar_t relativeScaleVersion[16]{};
    swprintf_s(relativeScaleVersion, L"%d", kRelativeScaleSettingsVersion);
    WritePrivateProfileStringW(L"Video", L"RelativeWindowScaleVersion",
                               relativeScaleVersion, path.c_str());
    WritePrivateProfileStringW(L"Video", L"BorderlessWindow",
                               g_settings.borderlessWindow ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Window", L"Snap",
                               g_settings.windowSnap ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"Diagnostics", L"SaveLog",
                               g_settings.saveLog ? L"1" : L"0",
                               path.c_str());
    WritePrivateProfileStringW(L"Diagnostics", L"ShowConsole",
                               g_settings.showDiagnosticConsole ? L"1" : L"0",
                               path.c_str());
}

// -----------------------------------------------------------------------------
// Lock-protected PCM ring buffer.
// The callback copies only the delivered PCM packet and never blocks capture.
// -----------------------------------------------------------------------------

class PcmRing {
public:
    PcmRing() : data_(kRingFrames * kChannels) {}

    void push(const int16_t* samples, size_t frames) {
        std::lock_guard<std::mutex> lock(mu_);

        if (frames >= kRingFrames) {
            samples += (frames - kRingFrames) * kChannels;
            frames = kRingFrames;
            readFrame_ = 0;
            writeFrame_ = 0;
            available_ = 0;
        }

        while (available_ + frames > kRingFrames) {
            // Drop the oldest frame(s), never block capture.
            const size_t drop = std::min(available_ + frames - kRingFrames, available_);
            readFrame_ = (readFrame_ + drop) % kRingFrames;
            available_ -= drop;
            if (AudioTrackingActive()) {
                const uint64_t nowMs = GetTickCount64();
                overruns_++;
                g_audioOverrunFrames.fetch_add(drop,
                                               std::memory_order_relaxed);
                g_audioLastOverrunMs.store(nowMs,
                                           std::memory_order_release);
                RecordAudioErrorEvent(
                    nowMs, static_cast<uint32_t>((std::min)(
                            drop, static_cast<size_t>(UINT32_MAX))),
                    AudioErrorKind::Overrun, AudioErrorCause::Overrun);
            }
        }

        for (size_t i = 0; i < frames; ++i) {
            const size_t dst = ((writeFrame_ + i) % kRingFrames) * kChannels;
            const size_t src = i * kChannels;
            data_[dst] = samples[src];
            data_[dst + 1] = samples[src + 1];
        }

        writeFrame_ = (writeFrame_ + frames) % kRingFrames;
        available_ += frames;
        g_audioRingFrames.store(static_cast<UINT32>(available_),
                                std::memory_order_release);
    }

    void pushConverted(const BYTE* source, size_t frames,
                       const llcv::capture_audio::Format& format) {
        if (!source || frames == 0 || format.blockAlign == 0) return;
        std::lock_guard<std::mutex> lock(mu_);

        if (frames >= kRingFrames) {
            source += (frames - kRingFrames) * format.blockAlign;
            frames = kRingFrames;
            readFrame_ = 0;
            writeFrame_ = 0;
            available_ = 0;
        }

        while (available_ + frames > kRingFrames) {
            const size_t drop = std::min(available_ + frames - kRingFrames,
                                         available_);
            readFrame_ = (readFrame_ + drop) % kRingFrames;
            available_ -= drop;
            if (AudioTrackingActive()) {
                const uint64_t nowMs = GetTickCount64();
                overruns_++;
                g_audioOverrunFrames.fetch_add(drop,
                                               std::memory_order_relaxed);
                g_audioLastOverrunMs.store(nowMs,
                                           std::memory_order_release);
                RecordAudioErrorEvent(
                    nowMs, static_cast<uint32_t>((std::min)(
                            drop, static_cast<size_t>(UINT32_MAX))),
                    AudioErrorKind::Overrun, AudioErrorCause::Overrun);
            }
        }

        for (size_t i = 0; i < frames; ++i) {
            int16_t left = 0;
            int16_t right = 0;
            llcv::capture_audio::ConvertFrame(
                source + i * format.blockAlign, format, left, right);
            const size_t dst = ((writeFrame_ + i) % kRingFrames) * kChannels;
            data_[dst] = left;
            data_[dst + 1] = right;
        }
        writeFrame_ = (writeFrame_ + frames) % kRingFrames;
        available_ += frames;
        g_audioRingFrames.store(static_cast<UINT32>(available_),
                                std::memory_order_release);
    }

    size_t pop(int16_t* out, size_t frames) {
        std::lock_guard<std::mutex> lock(mu_);
        const size_t n = std::min(frames, available_);

        for (size_t i = 0; i < n; ++i) {
            const size_t src = ((readFrame_ + i) % kRingFrames) * kChannels;
            const size_t dst = i * kChannels;
            out[dst] = data_[src];
            out[dst + 1] = data_[src + 1];
        }

        readFrame_ = (readFrame_ + n) % kRingFrames;
        available_ -= n;
        g_audioRingFrames.store(static_cast<UINT32>(available_),
                                std::memory_order_release);
        return n;
    }

    size_t availableFrames() const {
        std::lock_guard<std::mutex> lock(mu_);
        return available_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        readFrame_ = 0;
        writeFrame_ = 0;
        available_ = 0;
        g_audioRingFrames.store(0, std::memory_order_release);
    }

    uint64_t overruns() const { return overruns_.load(); }

private:
    std::vector<int16_t> data_;
    mutable std::mutex mu_;
    size_t readFrame_ = 0;
    size_t writeFrame_ = 0;
    size_t available_ = 0;
    std::atomic<uint64_t> overruns_{0};
};

static PcmRing g_ring;
static std::atomic<uint64_t> g_underruns{0};

// Optional clock-drift correction. A short windowed-sinc interpolator changes
// the consumption rate by at most +/-1000 ppm. With correction disabled this
// class is bypassed completely, preserving the original integer PCM samples.
class SincDriftResampler {
public:
    size_t render(int16_t* output, size_t outputFrames, double ratio) {
        if (!output || outputFrames == 0) return 0;
        ratio = std::clamp(ratio, 0.999, 1.001);

        if (!primed_) {
            const size_t wanted = outputFrames + kHalfTaps * 2 + 2;
            const size_t pulled = appendFromRing(wanted);
            if (pulled == 0) return 0;
            const int16_t firstLeft = source_[0];
            const int16_t firstRight = source_[1];
            source_.insert(source_.begin(), kHistoryFrames * kChannels, 0);
            for (int i = 0; i < kHistoryFrames; ++i) {
                source_[static_cast<size_t>(i) * kChannels] = firstLeft;
                source_[static_cast<size_t>(i) * kChannels + 1] = firstRight;
            }
            position_ = static_cast<double>(kHistoryFrames);
            primed_ = true;
        }

        const double lastPosition =
            position_ + ratio * static_cast<double>(outputFrames - 1);
        const size_t requiredFrames =
            static_cast<size_t>(std::floor(lastPosition)) + 1;
        const size_t currentFrames = source_.size() / kChannels;
        if (requiredFrames > currentFrames) {
            appendFromRing(requiredFrames - currentFrames);
        }

        size_t produced = 0;
        const size_t sourceFrames = source_.size() / kChannels;
        while (produced < outputFrames) {
            const double samplePosition =
                position_ - static_cast<double>(kHalfTaps);
            const size_t center =
                static_cast<size_t>(std::floor(samplePosition));
            if (center < static_cast<size_t>(kHalfTaps - 1) ||
                center + kHalfTaps >= sourceFrames) {
                break;
            }
            const double fraction =
                samplePosition - static_cast<double>(center);
            for (int channel = 0; channel < kChannels; ++channel) {
                double sum = 0.0;
                double normalization = 0.0;
                for (int tap = -kHalfTaps + 1; tap <= kHalfTaps; ++tap) {
                    const double distance =
                        static_cast<double>(tap) - fraction;
                    const double weight = WindowedSinc(distance);
                    const size_t index =
                        (center + static_cast<size_t>(tap + kHalfTaps - 1) -
                         static_cast<size_t>(kHalfTaps - 1)) * kChannels +
                        static_cast<size_t>(channel);
                    sum += static_cast<double>(source_[index]) * weight;
                    normalization += weight;
                }
                if (std::abs(normalization) > 1.0e-12) {
                    sum /= normalization;
                }
                const long sample = std::lround(std::clamp(
                    sum, static_cast<double>(INT16_MIN),
                    static_cast<double>(INT16_MAX)));
                output[produced * kChannels + static_cast<size_t>(channel)] =
                    static_cast<int16_t>(sample);
            }
            ++produced;
            position_ += ratio;
        }

        compactHistory();
        g_audioResamplerFrames.store(
            static_cast<UINT32>((std::min)(bufferedFrames(),
                                           static_cast<size_t>(UINT32_MAX))),
            std::memory_order_release);
        return produced;
    }

    size_t bufferedFrames() const {
        const size_t frames = source_.size() / kChannels;
        const size_t consumed = static_cast<size_t>(std::floor(position_));
        return frames > consumed ? frames - consumed : 0;
    }

private:
    static constexpr int kHalfTaps = 8;
    static constexpr int kHistoryFrames = kHalfTaps * 2;
    static constexpr double kPi = 3.14159265358979323846;

    static double Sinc(double value) {
        if (std::abs(value) < 1.0e-12) return 1.0;
        const double radians = kPi * value;
        return std::sin(radians) / radians;
    }

    static double WindowedSinc(double distance) {
        if (std::abs(distance) >= static_cast<double>(kHalfTaps)) return 0.0;
        return Sinc(distance) *
               Sinc(distance / static_cast<double>(kHalfTaps));
    }

    size_t appendFromRing(size_t wantedFrames) {
        const size_t available = g_ring.availableFrames();
        const size_t frames = (std::min)(wantedFrames, available);
        if (frames == 0) return 0;
        transfer_.resize(frames * kChannels);
        const size_t pulled = g_ring.pop(transfer_.data(), frames);
        source_.insert(source_.end(), transfer_.begin(),
                       transfer_.begin() +
                           static_cast<ptrdiff_t>(pulled * kChannels));
        return pulled;
    }

    void compactHistory() {
        const size_t integerPosition =
            static_cast<size_t>(std::floor(position_));
        if (integerPosition <= static_cast<size_t>(kHistoryFrames)) return;
        const size_t dropFrames =
            integerPosition - static_cast<size_t>(kHistoryFrames);
        source_.erase(source_.begin(),
                      source_.begin() +
                          static_cast<ptrdiff_t>(dropFrames * kChannels));
        position_ -= static_cast<double>(dropFrames);
    }

    std::vector<int16_t> source_;
    std::vector<int16_t> transfer_;
    double position_ = 0.0;
    bool primed_ = false;
};

// -----------------------------------------------------------------------------
// Sample Grabber callback
// -----------------------------------------------------------------------------

class SampleGrabberCB final : public ISampleGrabberCB {
public:
    explicit SampleGrabberCB(llcv::capture_audio::Format format)
        : format_(format) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ISampleGrabberCB)) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++ref_;
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP SampleCB(double, IMediaSample* pSample) override {
        if (!pSample) return E_POINTER;

        BYTE* p = nullptr;
        HRESULT hr = pSample->GetPointer(&p);
        if (FAILED(hr) || !p) return hr;

        const long bytes = pSample->GetActualDataLength();
        if (bytes <= 0 || format_.blockAlign == 0 ||
            bytes % format_.blockAlign != 0) return S_OK;

        const size_t frames = static_cast<size_t>(bytes / format_.blockAlign);
        const uint64_t nowMs = GetTickCount64();
        const bool track = AudioTrackingActive();
        if (track) {
            uint64_t unsetStart = 0;
            g_audioMonitorStartMs.compare_exchange_strong(
                unsetStart, nowMs, std::memory_order_release,
                std::memory_order_relaxed);
            g_audioCapturePacketFrames.store(static_cast<UINT32>(frames),
                                             std::memory_order_release);
        }
        const auto now = std::chrono::steady_clock::now();
        if (hasPreviousCallback_) {
            const int64_t intervalUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now - previousCallback_).count();
            if (track) {
                g_audioCaptureIntervalUs.store(intervalUs,
                                               std::memory_order_release);
                g_audioCaptureIntervalTotalUs.fetch_add(
                    static_cast<uint64_t>((std::max)(int64_t{0}, intervalUs)),
                    std::memory_order_relaxed);
            }
        }
        previousCallback_ = now;
        hasPreviousCallback_ = true;
        g_audioLastCaptureCallbackMs.store(nowMs,
                                           std::memory_order_release);
        if (track) {
            g_audioCaptureCallbacks.fetch_add(1, std::memory_order_relaxed);
            g_audioCaptureFrames.fetch_add(frames, std::memory_order_relaxed);
        }
        if (format_.path == llcv::capture_audio::Path::Direct16BitStereo) {
            g_ring.push(reinterpret_cast<const int16_t*>(p), frames);
        } else {
            g_ring.pushConverted(p, frames, format_);
        }
        return S_OK;
    }

    STDMETHODIMP BufferCB(double, BYTE*, long) override {
        return E_NOTIMPL;
    }

private:
    std::atomic<ULONG> ref_{1};
    llcv::capture_audio::Format format_{};
    std::chrono::steady_clock::time_point previousCallback_{};
    bool hasPreviousCallback_ = false;
};

// -----------------------------------------------------------------------------
// DirectShow helpers
// -----------------------------------------------------------------------------

static HRESULT FindCaptureFilter(const std::wstring& selectedId,
                                 IBaseFilter** out,
                                 std::wstring* selectedName = nullptr) {
    if (!out) return E_POINTER;
    *out = nullptr;

    ICreateDevEnum* devEnum = nullptr;
    IEnumMoniker* enumMon = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&devEnum));
    if (FAILED(hr)) return hr;

    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMon, 0);
    if (hr != S_OK) {
        SafeRelease(devEnum);
        return E_FAIL;
    }

    IMoniker* mon = nullptr;
    IMoniker* selectedMoniker = nullptr;
    std::wstring chosenName;
    int chosenRank = 0;
    while (enumMon->Next(1, &mon, nullptr) == S_OK) {
        const std::wstring name = MonikerFriendlyName(mon);
        const std::wstring id = MonikerDisplayName(mon);
        if (!name.empty()) {
            fwprintf(stderr, L"[capture] video device: %s\n", name.c_str());
        }
        int rank = 0;
        if (!selectedId.empty()) {
            if (id == selectedId) rank = 100;
        } else {
            std::wstring lowerName(name);
            std::transform(lowerName.begin(), lowerName.end(),
                           lowerName.begin(), [](wchar_t value) {
                               return static_cast<wchar_t>(std::towlower(value));
                           });
            if (_wcsicmp(name.c_str(), kCaptureName) == 0) {
                rank = 30;
            } else if (lowerName.find(L"gc573") != std::wstring::npos ||
                       lowerName.find(L"live gamer 4k") !=
                           std::wstring::npos) {
                rank = 20;
            } else {
                rank = 10;
            }
        }
        if (rank > chosenRank) {
            SafeRelease(selectedMoniker);
            selectedMoniker = mon;
            selectedMoniker->AddRef();
            chosenName = name;
            chosenRank = rank;
        }
        SafeRelease(mon);
    }

    if (selectedMoniker) {
        hr = selectedMoniker->BindToObject(nullptr, nullptr,
                                           IID_PPV_ARGS(out));
        if (SUCCEEDED(hr)) {
            fwprintf(stderr,
                     L"[capture] selected device: %s%s\n",
                     chosenName.c_str(),
                     selectedId.empty() ? L" (auto)" : L"");
            if (selectedName) *selectedName = chosenName;
        }
    } else {
        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    SafeRelease(selectedMoniker);
    SafeRelease(enumMon);
    SafeRelease(devEnum);
    return hr;
}

static std::wstring NormalizedDeviceName(const std::wstring& value) {
    std::wstring normalized;
    bool previousWasSpace = true;
    for (const wchar_t ch : value) {
        if (std::iswalnum(ch)) {
            normalized.push_back(static_cast<wchar_t>(std::towlower(ch)));
            previousWasSpace = false;
        } else if (!previousWasSpace) {
            normalized.push_back(L' ');
            previousWasSpace = true;
        }
    }
    while (!normalized.empty() && normalized.back() == L' ') normalized.pop_back();
    return normalized;
}

static int RelatedCaptureAudioScore(const std::wstring& videoName,
                                    const std::wstring& audioName) {
    const std::wstring video = NormalizedDeviceName(videoName);
    const std::wstring audio = NormalizedDeviceName(audioName);
    if (video.empty() || audio.empty()) return 0;
    if (video == audio) return 1000;
    if (video.find(audio) != std::wstring::npos ||
        audio.find(video) != std::wstring::npos) return 800;

    int score = 0;
    size_t start = 0;
    while (start < video.size()) {
        const size_t end = video.find(L' ', start);
        const std::wstring token = video.substr(
            start, end == std::wstring::npos ? std::wstring::npos : end - start);
        // Ignore vendor/generic words. Model identifiers such as GC313Pro and
        // HD60 X remain useful identifiers across video/audio filter names.
        if (token.size() >= 3 && token != L"avermedia" && token != L"elgato" &&
            token != L"capture" && token != L"video" && token != L"audio" &&
            audio.find(token) != std::wstring::npos) {
            score += 100;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return score;
}

static HRESULT FindCaptureAudioFilter(const std::wstring& selectedId,
                                      const std::wstring& videoName,
                                      IBaseFilter** out,
                                      std::wstring* selectedName = nullptr) {
    if (!out) return E_POINTER;
    *out = nullptr;

    ICreateDevEnum* devEnum = nullptr;
    IEnumMoniker* enumMon = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&devEnum));
    if (FAILED(hr)) return hr;
    hr = devEnum->CreateClassEnumerator(CLSID_AudioInputDeviceCategory,
                                        &enumMon, 0);
    if (hr != S_OK) {
        SafeRelease(devEnum);
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    IMoniker* mon = nullptr;
    IMoniker* chosen = nullptr;
    std::wstring chosenName;
    int chosenScore = 0;
    while (enumMon->Next(1, &mon, nullptr) == S_OK) {
        const std::wstring name = MonikerFriendlyName(mon);
        const std::wstring id = MonikerDisplayName(mon);
        if (!name.empty()) fwprintf(stderr, L"[capture] audio input: %s\n", name.c_str());
        const int score = !selectedId.empty()
            ? (id == selectedId ? 10000 : 0)
            : RelatedCaptureAudioScore(videoName, name);
        if (score > chosenScore) {
            SafeRelease(chosen);
            chosen = mon;
            chosen->AddRef();
            chosenName = name;
            chosenScore = score;
        }
        SafeRelease(mon);
    }

    if (chosen) {
        hr = chosen->BindToObject(nullptr, nullptr, IID_PPV_ARGS(out));
        if (SUCCEEDED(hr)) {
            fwprintf(stderr, L"[capture] selected audio input: %s%s\n",
                     chosenName.c_str(), selectedId.empty() ? L" (auto match)" : L"");
            if (selectedName) *selectedName = chosenName;
        }
    } else {
        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    SafeRelease(chosen);
    SafeRelease(enumMon);
    SafeRelease(devEnum);
    return hr;
}

static HRESULT FindOutputPinByName(IBaseFilter* filter, const wchar_t* name, IPin** out) {
    if (!filter || !out) return E_POINTER;
    *out = nullptr;

    IEnumPins* e = nullptr;
    HRESULT hr = filter->EnumPins(&e);
    if (FAILED(hr)) return hr;

    IPin* pin = nullptr;
    while (e->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION dir{};
        PIN_INFO info{};
        if (SUCCEEDED(pin->QueryDirection(&dir)) && dir == PINDIR_OUTPUT &&
            SUCCEEDED(pin->QueryPinInfo(&info))) {
            if (info.pFilter) info.pFilter->Release();
            if (_wcsicmp(info.achName, name) == 0) {
                *out = pin;
                e->Release();
                return S_OK;
            }
        }
        pin->Release();
    }

    e->Release();
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static HRESULT GetFirstPin(IBaseFilter* filter, PIN_DIRECTION wanted, IPin** out) {
    if (!filter || !out) return E_POINTER;
    *out = nullptr;

    IEnumPins* e = nullptr;
    HRESULT hr = filter->EnumPins(&e);
    if (FAILED(hr)) return hr;

    IPin* pin = nullptr;
    while (e->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION dir{};
        if (SUCCEEDED(pin->QueryDirection(&dir)) && dir == wanted) {
            *out = pin;
            e->Release();
            return S_OK;
        }
        pin->Release();
    }

    e->Release();
    return E_FAIL;
}

static void DeleteMediaType(AM_MEDIA_TYPE* mediaType);

static HRESULT FindOutputPinByMajorType(IBaseFilter* filter,
                                        const GUID& majorType, IPin** out) {
    if (!filter || !out) return E_POINTER;
    *out = nullptr;
    IEnumPins* pins = nullptr;
    HRESULT hr = filter->EnumPins(&pins);
    if (FAILED(hr)) return hr;

    IPin* pin = nullptr;
    while (pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_DIRECTION direction{};
        if (SUCCEEDED(pin->QueryDirection(&direction)) &&
            direction == PINDIR_OUTPUT) {
            IEnumMediaTypes* types = nullptr;
            if (SUCCEEDED(pin->EnumMediaTypes(&types))) {
                AM_MEDIA_TYPE* type = nullptr;
                while (types->Next(1, &type, nullptr) == S_OK) {
                    const bool matches = type && type->majortype == majorType;
                    DeleteMediaType(type);
                    if (matches) {
                        SafeRelease(types);
                        *out = pin;
                        SafeRelease(pins);
                        return S_OK;
                    }
                }
                SafeRelease(types);
            }
        }
        SafeRelease(pin);
    }
    SafeRelease(pins);
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static void LogFilterPins(IBaseFilter* filter, const wchar_t* label) {
    if (!filter || !label) return;
    IEnumPins* pins = nullptr;
    if (FAILED(filter->EnumPins(&pins))) return;
    fwprintf(stderr, L"[capture] %s pin diagnostics:\n", label);
    IPin* pin = nullptr;
    while (pins->Next(1, &pin, nullptr) == S_OK) {
        PIN_INFO info{};
        PIN_DIRECTION direction{};
        pin->QueryDirection(&direction);
        const bool hasInfo = SUCCEEDED(pin->QueryPinInfo(&info));
        fwprintf(stderr, L"[capture]   %s: %s\n",
                 hasInfo ? info.achName : L"(unnamed pin)",
                 direction == PINDIR_OUTPUT ? L"output" : L"input");
        if (hasInfo && info.pFilter) info.pFilter->Release();
        IEnumMediaTypes* types = nullptr;
        if (SUCCEEDED(pin->EnumMediaTypes(&types))) {
            int index = 0;
            AM_MEDIA_TYPE* type = nullptr;
            while (index < 12 && types->Next(1, &type, nullptr) == S_OK) {
                wchar_t major[48]{};
                wchar_t subtype[48]{};
                StringFromGUID2(type->majortype, major, ARRAYSIZE(major));
                StringFromGUID2(type->subtype, subtype, ARRAYSIZE(subtype));
                fwprintf(stderr, L"[capture]     type %d: %s / %s\n",
                         index + 1, major, subtype);
                DeleteMediaType(type);
                ++index;
            }
            SafeRelease(types);
        }
        SafeRelease(pin);
    }
    SafeRelease(pins);
}

// Prefer the existing direct 48 kHz / 16-bit stereo route when a device
// advertises it. Otherwise keep the first supported 24/32-bit or float route
// and convert at the callback without inserting a DirectShow transform.
static AM_MEDIA_TYPE* SelectSupportedCaptureAudioType(
    IPin* audioPin, llcv::capture_audio::Format& selectedFormat,
    llcv::capture_audio::Rejection* rejection = nullptr) {
    if (rejection) *rejection = llcv::capture_audio::Rejection::Malformed;
    if (!audioPin) return nullptr;
    IEnumMediaTypes* types = nullptr;
    if (FAILED(audioPin->EnumMediaTypes(&types)) || !types) return nullptr;

    AM_MEDIA_TYPE* fallback = nullptr;
    llcv::capture_audio::Format fallbackFormat{};
    llcv::capture_audio::Rejection firstRejection =
        llcv::capture_audio::Rejection::Malformed;
    AM_MEDIA_TYPE* type = nullptr;
    while (types->Next(1, &type, nullptr) == S_OK) {
        const auto classification = llcv::capture_audio::Classify(*type);
        if (classification.supported) {
            if (classification.format.path ==
                llcv::capture_audio::Path::Direct16BitStereo) {
                DeleteMediaType(fallback);
                selectedFormat = classification.format;
                types->Release();
                return type;
            }
            if (!fallback) {
                fallback = type;
                fallbackFormat = classification.format;
                type = nullptr;
            }
        } else if (firstRejection ==
                       llcv::capture_audio::Rejection::Malformed ||
                   classification.rejection ==
                       llcv::capture_audio::Rejection::SampleRate) {
            firstRejection = classification.rejection;
        }
        DeleteMediaType(type);
        type = nullptr;
    }
    types->Release();
    if (fallback) {
        selectedFormat = fallbackFormat;
    } else if (rejection) {
        *rejection = firstRejection;
    }
    return fallback;
}

static void SuggestCaptureBuffer(IPin* audioPin, WORD blockAlign) {
    if (blockAlign == 0) return;
    IAMBufferNegotiation* neg = nullptr;
    if (FAILED(audioPin->QueryInterface(IID_PPV_ARGS(&neg)))) {
        fwprintf(stderr, L"[audio] IAMBufferNegotiation unavailable; driver controls capture buffer.\n");
        return;
    }

    ALLOCATOR_PROPERTIES props{};
    props.cBuffers = 4;
    props.cbBuffer = (kSampleRate * blockAlign *
                      kRecommendedCaptureBufferMs) / 1000;
    props.cbAlign = 1;
    props.cbPrefix = 0;

    HRESULT hr = neg->SuggestAllocatorProperties(&props);
    if (FAILED(hr)) LogHr(L"IAMBufferNegotiation::SuggestAllocatorProperties", hr);
    else fwprintf(stderr, L"[audio] requested DirectShow capture buffer: %d ms (%ld bytes)\n",
                  kRecommendedCaptureBufferMs, props.cbBuffer);

    neg->Release();
}

static void ReportConnectedAudioAllocator(IPin* inputPin, WORD blockAlign) {
    IMemInputPin* memoryInput = nullptr;
    IMemAllocator* allocator = nullptr;
    ALLOCATOR_PROPERTIES properties{};
    HRESULT hr = inputPin
                     ? inputPin->QueryInterface(IID_PPV_ARGS(&memoryInput))
                     : E_POINTER;
    if (SUCCEEDED(hr)) hr = memoryInput->GetAllocator(&allocator);
    if (SUCCEEDED(hr)) hr = allocator->GetProperties(&properties);
    if (SUCCEEDED(hr)) {
        const LONG frames = blockAlign > 0
                                ? properties.cbBuffer / blockAlign : 0;
        g_audioCaptureAllocatorFrames.store(frames,
                                            std::memory_order_release);
        g_audioCaptureAllocatorBuffers.store(properties.cBuffers,
                                             std::memory_order_release);
        fwprintf(stderr,
                 L"[audio] actual DirectShow allocator: %ld buffers x "
                 L"%ld bytes (%ld frames / %.2f ms each)\n",
                 properties.cBuffers, properties.cbBuffer, frames,
                 1000.0 * frames / kSampleRate);
    } else {
        LogHr(L"DirectShow audio allocator query", hr);
    }
    SafeRelease(allocator);
    SafeRelease(memoryInput);
}

// -----------------------------------------------------------------------------
// WASAPI render thread (user-selectable Shared or Exclusive mode)
// -----------------------------------------------------------------------------

class DefaultAudioEndpointNotification final : public IMMNotificationClient {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                         LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            g_defaultAudioEndpointGeneration.fetch_add(
                1, std::memory_order_acq_rel);
        }
        return S_OK;
    }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
};

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

static bool AudioRenderThreadWasapi(AudioMode mode,
                                    bool reinitializingEndpoint) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHr(L"CoInitializeEx(audio render)", hr);
        return false;
    }

    const bool exclusive = mode == AudioMode::WasapiExclusive;
    const bool followDefault = g_settings.audioOutputDeviceId.empty();
    uint64_t watchedDefaultGeneration =
        g_defaultAudioEndpointGeneration.load(std::memory_order_acquire);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioClient3* client3 = nullptr;
    IAudioRenderClient* render = nullptr;
    DefaultAudioEndpointNotification* endpointNotification = nullptr;
    bool notificationRegistered = false;
    bool restartForDefaultChange = false;
    HANDLE eventHandle = nullptr;

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&en));
        if (FAILED(hr)) { LogHr(L"CoCreateInstance(MMDeviceEnumerator)", hr); break; }

        if (followDefault) {
            endpointNotification = new DefaultAudioEndpointNotification();
            hr = en->RegisterEndpointNotificationCallback(
                endpointNotification);
            if (SUCCEEDED(hr)) {
                notificationRegistered = true;
                watchedDefaultGeneration =
                    g_defaultAudioEndpointGeneration.load(
                        std::memory_order_acquire);
            } else {
                LogHr(L"RegisterEndpointNotificationCallback", hr);
                endpointNotification->Release();
                endpointNotification = nullptr;
            }
        }

        hr = GetConfiguredAudioEndpoint(
            en, g_settings.audioOutputDeviceId, &dev);
        if (FAILED(hr)) { LogHr(L"GetConfiguredAudioEndpoint", hr); break; }
        std::wstring outputName = AudioEndpointFriendlyName(dev);
        if (outputName.empty()) {
            outputName = g_settings.audioOutputDeviceId.empty()
                ? L"Windows 기본 장치" : L"선택한 출력 장치";
        }
        fwprintf(stderr, L"[audio] output endpoint: %s%s\n",
                 outputName.c_str(),
                 followDefault ? L" (following Windows default)" : L"");
        SetActiveAudioOutputName(
            outputName + (followDefault ? L" (기본 추적)" : L""));

        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                           reinterpret_cast<void**>(&client));
        if (FAILED(hr)) { LogHr(L"Activate(IAudioClient)", hr); break; }

        WAVEFORMATEX wf = PcmOutputFormat();

        WAVEFORMATEX* closest = nullptr;
        const AUDCLNT_SHAREMODE shareMode = exclusive
                                                 ? AUDCLNT_SHAREMODE_EXCLUSIVE
                                                 : AUDCLNT_SHAREMODE_SHARED;
        hr = client->IsFormatSupported(shareMode, &wf, &closest);
        if (exclusive) {
            if (closest) CoTaskMemFree(closest);
            if (hr != S_OK) {
                LogHr(L"IAudioClient::IsFormatSupported(exclusive)", hr);
                break;
            }
        } else if (FAILED(hr)) {
            // Shared mode can still accept the requested PCM format through
            // the Windows Audio Engine's built-in converter.
            if (closest) CoTaskMemFree(closest);
            LogHr(L"IAudioClient::IsFormatSupported(shared)", hr);
            break;
        } else if (closest) {
            CoTaskMemFree(closest);
        }

        bool usingAudioClient3 = false;
        UINT32 selectedSharedPeriodFrames = 0;
        if (!exclusive) {
            AudioClient3Support support{};
            hr = client->QueryInterface(IID_PPV_ARGS(&client3));
            if (SUCCEEDED(hr)) {
                AudioClientProperties properties{};
                properties.cbSize = sizeof(properties);
                properties.eCategory = AudioCategory_Media;
                hr = client3->SetClientProperties(&properties);
            }
            if (SUCCEEDED(hr)) {
                hr = client3->GetSharedModeEnginePeriod(
                    &wf, &support.defaultFrames, &support.fundamentalFrames,
                    &support.minimumFrames, &support.maximumFrames);
                support.supported = SUCCEEDED(hr);
            }
            if (support.supported) {
                UINT32 requestedFrames = g_settings.wasapiSharedPeriodFrames;
                if (requestedFrames == 0) {
                    requestedFrames = static_cast<UINT32>(
                        g_settings.wasapiBufferMs * kSampleRate / 1000);
                }
                selectedSharedPeriodFrames =
                    ClosestSupportedSharedPeriod(requestedFrames, support);
                hr = client3->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                    selectedSharedPeriodFrames, &wf, nullptr);
                if (SUCCEEDED(hr)) {
                    usingAudioClient3 = true;
                    fwprintf(stderr,
                             L"[audio] IAudioClient3 shared period: %u frames "
                             L"(%.2f ms)\n",
                             selectedSharedPeriodFrames,
                             1000.0 * selectedSharedPeriodFrames / kSampleRate);
                } else {
                    LogHr(L"IAudioClient3::InitializeSharedAudioStream", hr);
                }
            } else {
                fwprintf(stderr,
                         L"[audio] IAudioClient3 unavailable for this format; "
                         L"using classic Shared mode.\n");
            }

            if (!usingAudioClient3) {
                // A failed Initialize call can leave an audio client unusable.
                // Reactivate it before falling back to classic Shared mode.
                SafeRelease(client3);
                SafeRelease(client);
                hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                                   nullptr,
                                   reinterpret_cast<void**>(&client));
                if (FAILED(hr)) {
                    LogHr(L"Activate(IAudioClient fallback)", hr);
                    break;
                }
                const REFERENCE_TIME hns =
                    static_cast<REFERENCE_TIME>(g_settings.wasapiBufferMs) *
                    10'000;
                hr = client->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    hns, 0, &wf, nullptr);
                if (FAILED(hr)) {
                    LogHr(L"IAudioClient::Initialize(shared fallback/event)",
                          hr);
                    break;
                }
                fwprintf(stderr,
                         L"[audio] classic WASAPI Shared fallback active.\n");
            }
        } else {
            const REFERENCE_TIME hns =
                static_cast<REFERENCE_TIME>(g_settings.wasapiBufferMs) * 10'000;
            hr = client->Initialize(
                AUDCLNT_SHAREMODE_EXCLUSIVE,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                hns, hns, &wf, nullptr);
            if (FAILED(hr)) {
                LogHr(L"IAudioClient::Initialize(exclusive/event)", hr);
                break;
            }
        }

        eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            fwprintf(stderr, L"CreateEvent failed.\n");
            break;
        }

        hr = client->SetEventHandle(eventHandle);
        if (FAILED(hr)) { LogHr(L"SetEventHandle", hr); break; }

        hr = client->GetService(IID_PPV_ARGS(&render));
        if (FAILED(hr)) { LogHr(L"GetService(IAudioRenderClient)", hr); break; }

        UINT32 bufferFrames = 0;
        hr = client->GetBufferSize(&bufferFrames);
        if (FAILED(hr)) { LogHr(L"GetBufferSize", hr); break; }
        g_audioActualBufferFrames.store(bufferFrames,
                                        std::memory_order_release);

        fwprintf(stderr, L"[audio] WASAPI %s buffer: %u frames (%.2f ms)\n",
                  exclusive ? L"exclusive" : L"shared", bufferFrames,
                  1000.0 * bufferFrames / kSampleRate);
        if (exclusive || !usingAudioClient3) {
            fwprintf(stderr, L"[audio] requested WASAPI buffer: %d ms\n",
                     g_settings.wasapiBufferMs);
        }

        BYTE* p = nullptr;
        hr = render->GetBuffer(bufferFrames, &p);
        if (FAILED(hr)) { LogHr(L"GetBuffer(prime)", hr); break; }
        memset(p, 0, static_cast<size_t>(bufferFrames) * wf.nBlockAlign);
        render->ReleaseBuffer(bufferFrames, 0);

        if (reinitializingEndpoint) {
            // Capture remains live while a new WASAPI client is constructed.
            // Drop everything accumulated during that construction immediately
            // before Start, otherwise every switch permanently adds that setup
            // time to the live audio delay.
            g_ring.clear();
            g_audioResamplerFrames.store(0, std::memory_order_release);
            g_audioMinimumPreRenderFrames.store(UINT32_MAX,
                                                std::memory_order_release);
            fwprintf(stderr,
                     L"[audio] endpoint switch: discarded setup backlog "
                     L"immediately before Start.\n");
        }

        hr = client->Start();
        if (FAILED(hr)) { LogHr(L"IAudioClient::Start", hr); break; }

        fwprintf(stderr, L"[audio] WASAPI %s render running.\n",
                  exclusive ? L"exclusive" : L"shared");
        fwprintf(stderr, L"[audio] clock-drift correction: %s\n",
                 g_settings.driftCorrection ==
                         DriftCorrectionMode::Resample
                     ? L"16-tap windowed-sinc resampling (+/-1000 ppm)"
                     : L"off (unaltered PCM samples)");

        std::vector<int16_t> temp(static_cast<size_t>(bufferFrames) * kChannels);
        SincDriftResampler driftResampler;
        double filteredQueuedFrames = -1.0;
        double correctionPpm = 0.0;
        const double initialVolumeGain = TargetAudioVolumeGain();
        llcv::audio::StereoGain currentMix{
            initialVolumeGain * TargetAudioChannelGain(0),
            initialVolumeGain * TargetAudioChannelGain(1)};
        bool audioStarted = false;
        const UINT32 queueTargetFrames = static_cast<UINT32>(
            g_settings.pcmQueueTargetMs * kSampleRate / 1000);
        g_audioQueueTargetFrames.store(queueTargetFrames,
                                       std::memory_order_release);
        while (g_running.load()) {
            if (followDefault && notificationRegistered &&
                g_defaultAudioEndpointGeneration.load(
                    std::memory_order_acquire) !=
                    watchedDefaultGeneration) {
                restartForDefaultChange = true;
                fwprintf(stderr,
                         L"[audio] Windows default output changed; "
                         L"reinitializing WASAPI only.\n");
                break;
            }
            const DWORD waitResult = WaitForSingleObject(eventHandle, 100);
            if (waitResult != WAIT_OBJECT_0) continue;

            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) continue;
            g_audioWasapiPaddingFrames.store(padding,
                                             std::memory_order_release);

            const UINT32 writable = bufferFrames > padding ? bufferFrames - padding : 0;
            if (!writable) continue;

            BYTE* out = nullptr;
            hr = render->GetBuffer(writable, &out);
            if (FAILED(hr)) continue;

            size_t got = 0;
            const size_t availableBeforeRender =
                g_ring.availableFrames() + driftResampler.bufferedFrames();
            if (audioStarted && AudioTrackingActive()) {
                UINT32 observed = static_cast<UINT32>((std::min)(
                    availableBeforeRender,
                    static_cast<size_t>(UINT32_MAX)));
                UINT32 previousMinimum = g_audioMinimumPreRenderFrames.load(
                    std::memory_order_relaxed);
                while (observed < previousMinimum &&
                       !g_audioMinimumPreRenderFrames.compare_exchange_weak(
                           previousMinimum, observed,
                           std::memory_order_release,
                           std::memory_order_relaxed)) {}
            }
            if (g_settings.driftCorrection ==
                DriftCorrectionMode::Resample) {
                const double targetFrames =
                    static_cast<double>(queueTargetFrames);
                const double queuedFrames = static_cast<double>(
                    g_audioRingFrames.load(std::memory_order_acquire) +
                    static_cast<UINT32>((std::min)(
                        driftResampler.bufferedFrames(),
                        static_cast<size_t>(UINT32_MAX))));
                if (!audioStarted && queuedFrames >= targetFrames) {
                    audioStarted = true;
                }
                if (filteredQueuedFrames < 0.0) {
                    filteredQueuedFrames = queuedFrames;
                } else {
                    filteredQueuedFrames +=
                        (queuedFrames - filteredQueuedFrames) * 0.02;
                }
                const double requestedPpm = std::clamp(
                    (filteredQueuedFrames - targetFrames) * 2.0,
                    -1000.0, 1000.0);
                correctionPpm += (requestedPpm - correctionPpm) * 0.02;
                const double ratio = 1.0 + correctionPpm / 1'000'000.0;
                if (audioStarted) {
                    got = driftResampler.render(temp.data(), writable, ratio);
                }
                g_audioResamplePpm.store(
                    static_cast<int>(std::lround(correctionPpm)),
                    std::memory_order_release);
                g_audioResampledOutputFrames.fetch_add(
                    got, std::memory_order_relaxed);
            } else {
                if (!audioStarted &&
                    g_ring.availableFrames() >= queueTargetFrames) {
                    audioStarted = true;
                }
                if (audioStarted) {
                    got = g_ring.pop(temp.data(), writable);
                }
                g_audioResamplePpm.store(0, std::memory_order_release);
                g_audioResamplerFrames.store(0, std::memory_order_release);
            }
            const double targetVolumeGain = TargetAudioVolumeGain();
            // Meters are deliberately dormant when the audio OSD is hidden.
            // The default 100% path remains a direct ring-buffer-to-WASAPI
            // copy, matching the previous low-overhead implementation.
            const bool measurePeaks =
                g_audioOsdVisible.load(std::memory_order_acquire);
            const llcv::audio::MixMetrics mix =
                llcv::audio::ProcessStereoPcm(
                    temp.data(), got, currentMix,
                    {targetVolumeGain * TargetAudioChannelGain(0),
                     targetVolumeGain * TargetAudioChannelGain(1)},
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
            if (got) memcpy(out, temp.data(), got * wf.nBlockAlign);
            if (got < writable) {
                memset(out + got * wf.nBlockAlign, 0,
                       (writable - static_cast<UINT32>(got)) * wf.nBlockAlign);
                if (audioStarted && AudioTrackingActive()) {
                    const uint64_t nowMs = GetTickCount64();
                    const UINT32 missingFrames =
                        writable - static_cast<UINT32>(got);
                    AudioErrorCause cause = AudioErrorCause::PcmDepletion;
                    g_underruns++;
                    g_audioUnderrunFrames.fetch_add(
                        missingFrames,
                        std::memory_order_relaxed);
                    g_audioLastUnderrunMs.store(nowMs,
                                                std::memory_order_release);
                    if (g_settings.driftCorrection ==
                            DriftCorrectionMode::Resample &&
                        availableBeforeRender >= writable) {
                        // PCM existed for this render request, but the sinc
                        // filter could not produce every output frame (usually
                        // insufficient look-ahead at the lowest queue target).
                        cause = AudioErrorCause::Resampler;
                        g_audioResamplerUnderruns.fetch_add(
                            1, std::memory_order_relaxed);
                    } else {
                        const uint64_t callbackMs =
                            g_audioLastCaptureCallbackMs.load(
                                std::memory_order_acquire);
                        const UINT32 packetFrames =
                            g_audioCapturePacketFrames.load(
                                std::memory_order_acquire);
                        const uint64_t lateThresholdMs = packetFrames
                            ? 5 + (1000ull * packetFrames / kSampleRate)
                            : 15;
                        const uint64_t callbackNowMs = GetTickCount64();
                        if (callbackMs &&
                            callbackNowMs > callbackMs + lateThresholdMs) {
                            cause = AudioErrorCause::InputLate;
                            g_audioLatePacketUnderruns.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                    RecordAudioErrorEvent(nowMs, missingFrames,
                                          AudioErrorKind::Underrun, cause);
                }
            }
            render->ReleaseBuffer(writable, 0);
        }

        client->Stop();
    } while (false);

    if (eventHandle) CloseHandle(eventHandle);
    SafeRelease(render);
    SafeRelease(client3);
    SafeRelease(client);
    SafeRelease(dev);
    if (en && notificationRegistered && endpointNotification) {
        en->UnregisterEndpointNotificationCallback(endpointNotification);
    }
    if (endpointNotification) endpointNotification->Release();
    SafeRelease(en);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
    if (followDefault && notificationRegistered &&
        g_defaultAudioEndpointGeneration.load(std::memory_order_acquire) !=
            watchedDefaultGeneration) {
        restartForDefaultChange = true;
    }
    return restartForDefaultChange;
}

static void AudioRenderThread() {
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

class LatestNv12Sample {
public:
    LatestNv12Sample(size_t expectedBytes, HANDLE readyEvent)
        : expectedBytes_(expectedBytes), readyEvent_(readyEvent) {}

    ~LatestNv12Sample() {
        if (latest_) latest_->Release();
    }

    void push(IMediaSample* sample) {
        if (!sample || sample->GetActualDataLength() <= 0 ||
            (expectedBytes_ != 0 && sample->GetActualDataLength() <
                                      static_cast<long>(expectedBytes_))) {
            return;
        }
        const int64_t arrivalUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        sample->AddRef();
        IMediaSample* replaced = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            replaced = latest_;
            latest_ = sample;
            latestArrivalUs_ = arrivalUs;
        }
        if (replaced) {
            replaced->Release();
            if (OsdTrackingActive()) {
                g_videoReplacedFrames.fetch_add(1,
                                                std::memory_order_relaxed);
            }
        }
        if (OsdTrackingActive()) {
            g_videoCapturedFrames.fetch_add(1, std::memory_order_relaxed);
        }
        SetEvent(readyEvent_);
    }

    IMediaSample* takeLatest(int64_t& arrivalUs) {
        std::lock_guard<std::mutex> lock(mu_);
        IMediaSample* sample = latest_;
        latest_ = nullptr;
        if (!sample) return nullptr;
        arrivalUs = latestArrivalUs_;
        return sample;
    }

private:
    std::mutex mu_;
    IMediaSample* latest_ = nullptr;
    size_t expectedBytes_ = 0;
    HANDLE readyEvent_ = nullptr;
    int64_t latestArrivalUs_ = 0;
};

class VideoSampleGrabberCB final : public ISampleGrabberCB {
public:
    explicit VideoSampleGrabberCB(LatestNv12Sample* sampleSlot)
        : sampleSlot_(sampleSlot) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ISampleGrabberCB)) {
            *ppv = static_cast<ISampleGrabberCB*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG value = --ref_;
        if (!value) delete this;
        return value;
    }
    STDMETHODIMP SampleCB(double, IMediaSample* sample) override {
        if (!surfaceCapabilityProbed_.exchange(true,
                                                std::memory_order_acq_rel)) {
            IMediaSample2Config* surfaceConfig = nullptr;
            IUnknown* surface = nullptr;
            const HRESULT configHr = sample->QueryInterface(
                IID_PPV_ARGS(&surfaceConfig));
            const HRESULT surfaceHr = SUCCEEDED(configHr)
                ? surfaceConfig->GetSurface(&surface) : configHr;
            fwprintf(stderr,
                     L"[video] DirectShow VRAM sample surface: %s "
                     L"(interface 0x%08X, surface 0x%08X)\n",
                     SUCCEEDED(surfaceHr) && surface ? L"available"
                                                     : L"not available",
                     static_cast<unsigned>(configHr),
                     static_cast<unsigned>(surfaceHr));
            SafeRelease(surface);
            SafeRelease(surfaceConfig);
        }
        if (sampleSlot_) sampleSlot_->push(sample);
        return S_OK;
    }
    STDMETHODIMP BufferCB(double, BYTE*, long) override { return E_NOTIMPL; }

private:
    std::atomic<ULONG> ref_{1};
    std::atomic<bool> surfaceCapabilityProbed_{false};
    LatestNv12Sample* sampleSlot_ = nullptr;
};

static void DeleteMediaType(AM_MEDIA_TYPE* mediaType) {
    if (!mediaType) return;
    FreeMediaType(*mediaType);
    CoTaskMemFree(mediaType);
}

// Experimental compressed compatibility path. DirectShow still owns device
// capture and supplies the newest compressed access unit; a synchronous Media
// Foundation decoder expands it to NV12 for the existing D3D11 renderer.
// Keeping only the newest sample before decode prevents application-side
// queues from accumulating when a decoder cannot keep up.
class MediaFoundationCompressedDecoder {
public:
    ~MediaFoundationCompressedDecoder() { reset(); }

    HRESULT initialize(VideoPixelFormat inputFormat, int width, int height,
                       int fps, const AM_MEDIA_TYPE* captureType) {
        reset();
        if (!IsCompressedVideoFormat(inputFormat) || width <= 0 ||
            height <= 0 || fps <= 0) {
            return E_INVALIDARG;
        }

        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) return hr;
        mfStarted_ = true;

        IMFMediaType* inputType = nullptr;
        hr = MFCreateMediaType(&inputType);
        if (SUCCEEDED(hr)) hr = inputType->SetGUID(MF_MT_MAJOR_TYPE,
                                                    MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = inputType->SetGUID(MF_MT_SUBTYPE,
                                                    PixelFormatSubtype(inputFormat));
        if (SUCCEEDED(hr)) hr = MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE,
                                                    width, height);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(inputType, MF_MT_FRAME_RATE,
                                                     fps, 1);
        if (SUCCEEDED(hr)) hr = inputType->SetUINT32(
            MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr)) hr = inputType->SetUINT32(
            MF_MT_ALL_SAMPLES_INDEPENDENT,
            inputFormat == VideoPixelFormat::Mjpeg ? TRUE : FALSE);
        if (SUCCEEDED(hr)) {
            CopyMpegSequenceHeader(captureType, inputType);
        }
        if (FAILED(hr)) {
            SafeRelease(inputType);
            reset();
            return hr;
        }

        MFT_REGISTER_TYPE_INFO inputInfo{};
        inputInfo.guidMajorType = MFMediaType_Video;
        inputInfo.guidSubtype = PixelFormatSubtype(inputFormat);
        MFT_REGISTER_TYPE_INFO outputInfo{};
        outputInfo.guidMajorType = MFMediaType_Video;
        outputInfo.guidSubtype = MFVideoFormat_NV12;
        IMFActivate** activations = nullptr;
        UINT32 activationCount = 0;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                       MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                           MFT_ENUM_FLAG_SORTANDFILTER,
                       &inputInfo, &outputInfo, &activations, &activationCount);
        if (FAILED(hr) || activationCount == 0) {
            if (SUCCEEDED(hr)) hr = MF_E_TOPO_CODEC_NOT_FOUND;
            SafeRelease(inputType);
            if (activations) CoTaskMemFree(activations);
            reset();
            return hr;
        }

        HRESULT finalHr = MF_E_TOPO_CODEC_NOT_FOUND;
        for (UINT32 i = 0; i < activationCount; ++i) {
            IMFTransform* candidate = nullptr;
            const HRESULT activateHr = activations[i]->ActivateObject(
                IID_PPV_ARGS(&candidate));
            if (SUCCEEDED(activateHr)) {
                IMFAttributes* attributes = nullptr;
                if (SUCCEEDED(candidate->QueryInterface(IID_PPV_ARGS(&attributes)))) {
                    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
                    SafeRelease(attributes);
                }
                HRESULT candidateHr = candidate->SetInputType(0, inputType, 0);
                if (SUCCEEDED(candidateHr)) {
                    candidateHr = SetNv12OutputType(candidate);
                }
                if (SUCCEEDED(candidateHr)) {
                    candidateHr = candidate->GetOutputStreamInfo(0, &outputInfo_);
                }
                if (SUCCEEDED(candidateHr)) {
                    transform_ = candidate;
                    candidate = nullptr;
                    width_ = width;
                    height_ = height;
                    UINT32 defaultStride = 0;
                    if (outputType_) {
                        outputType_->GetUINT32(MF_MT_DEFAULT_STRIDE,
                                                &defaultStride);
                    }
                    stride_ = defaultStride
                        ? static_cast<LONG>(defaultStride)
                        : static_cast<LONG>(width_);
                    bufferBytes_ = (std::max)(outputInfo_.cbSize,
                        static_cast<DWORD>(width_) * static_cast<DWORD>(height_) *
                            3u / 2u);
                    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
                    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
                    fwprintf(stderr,
                             L"[video] Media Foundation compressed decoder: %s -> NV12, "
                             L"synchronous low-latency mode, output stride %ld.\n",
                             PixelFormatName(inputFormat), stride_);
                    finalHr = S_OK;
                } else {
                    finalHr = candidateHr;
                }
            } else {
                finalHr = activateHr;
            }
            SafeRelease(candidate);
            activations[i]->Release();
        }
        CoTaskMemFree(activations);
        SafeRelease(inputType);
        if (FAILED(finalHr)) reset();
        return finalHr;
    }

    // Returns S_OK without an output frame while the decoder is waiting for
    // enough input. When several frames become available, only the newest is
    // returned to the caller.
    HRESULT decode(IMediaSample* directShowSample, IMFMediaBuffer** output) {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!transform_ || !directShowSample) return MF_E_NOT_INITIALIZED;

        BYTE* source = nullptr;
        const long sourceLength = directShowSample->GetActualDataLength();
        HRESULT hr = directShowSample->GetPointer(&source);
        if (FAILED(hr) || !source || sourceLength <= 0) {
            return FAILED(hr) ? hr : E_FAIL;
        }

        IMFSample* inputSample = nullptr;
        IMFMediaBuffer* inputBuffer = nullptr;
        hr = MFCreateSample(&inputSample);
        if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(
            static_cast<DWORD>(sourceLength), &inputBuffer);
        BYTE* destination = nullptr;
        if (SUCCEEDED(hr)) hr = inputBuffer->Lock(&destination, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            memcpy(destination, source, static_cast<size_t>(sourceLength));
            inputBuffer->Unlock();
            destination = nullptr;
            hr = inputBuffer->SetCurrentLength(static_cast<DWORD>(sourceLength));
        }
        if (SUCCEEDED(hr)) hr = inputSample->AddBuffer(inputBuffer);
        REFERENCE_TIME start = 0;
        REFERENCE_TIME stop = 0;
        if (SUCCEEDED(directShowSample->GetTime(&start, &stop))) {
            inputSample->SetSampleTime(start);
            if (stop > start) inputSample->SetSampleDuration(stop - start);
        }
        if (FAILED(hr)) {
            if (destination) inputBuffer->Unlock();
            SafeRelease(inputBuffer);
            SafeRelease(inputSample);
            return hr;
        }

        IMFMediaBuffer* newest = nullptr;
        for (;;) {
            hr = transform_->ProcessInput(0, inputSample, 0);
            if (hr != MF_E_NOTACCEPTING) break;
            HRESULT drainHr = PullOutput(&newest);
            if (drainHr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                hr = MF_E_NOTACCEPTING;
                break;
            }
            if (FAILED(drainHr)) {
                hr = drainHr;
                break;
            }
        }
        SafeRelease(inputBuffer);
        SafeRelease(inputSample);
        if (FAILED(hr)) {
            SafeRelease(newest);
            return hr;
        }

        for (;;) {
            HRESULT outputHr = PullOutput(&newest);
            if (outputHr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            if (FAILED(outputHr)) {
                SafeRelease(newest);
                return outputHr;
            }
        }
        *output = newest;
        return S_OK;
    }

    LONG stride() const { return stride_; }

    void reset() {
        if (transform_) {
            transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        SafeRelease(outputType_);
        SafeRelease(transform_);
        outputInfo_ = {};
        bufferBytes_ = 0;
        stride_ = 0;
        width_ = 0;
        height_ = 0;
        if (mfStarted_) {
            MFShutdown();
            mfStarted_ = false;
        }
    }

private:
    static void CopyMpegSequenceHeader(const AM_MEDIA_TYPE* captureType,
                                       IMFMediaType* destination) {
        if (!captureType || !destination ||
            captureType->formattype != FORMAT_MPEG2Video ||
            captureType->cbFormat < FIELD_OFFSET(MPEG2VIDEOINFO,
                                                  dwSequenceHeader)) {
            return;
        }
        const auto* info = reinterpret_cast<const MPEG2VIDEOINFO*>(
            captureType->pbFormat);
        const size_t available = captureType->cbFormat -
            FIELD_OFFSET(MPEG2VIDEOINFO, dwSequenceHeader);
        if (!info->cbSequenceHeader || info->cbSequenceHeader > available) return;
        destination->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                             reinterpret_cast<const UINT8*>(
                                 info->dwSequenceHeader),
                             info->cbSequenceHeader);
    }

    HRESULT SetNv12OutputType(IMFTransform* transform) {
        SafeRelease(outputType_);
        for (DWORD index = 0;; ++index) {
            IMFMediaType* candidate = nullptr;
            HRESULT hr = transform->GetOutputAvailableType(0, index, &candidate);
            if (FAILED(hr)) return hr;
            GUID subtype{};
            const HRESULT subtypeHr = candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (SUCCEEDED(subtypeHr) && subtype == MFVideoFormat_NV12) {
                hr = transform->SetOutputType(0, candidate, 0);
                if (SUCCEEDED(hr)) {
                    outputType_ = candidate;
                    return S_OK;
                }
            }
            SafeRelease(candidate);
        }
    }

    HRESULT PullOutput(IMFMediaBuffer** newest) {
        if (!newest) return E_POINTER;
        IMFSample* suppliedSample = nullptr;
        MFT_OUTPUT_DATA_BUFFER output{};
        if ((outputInfo_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            HRESULT hr = MFCreateSample(&suppliedSample);
            if (SUCCEEDED(hr)) {
                IMFMediaBuffer* suppliedBuffer = nullptr;
                hr = MFCreateMemoryBuffer(bufferBytes_, &suppliedBuffer);
                if (SUCCEEDED(hr)) hr = suppliedSample->AddBuffer(suppliedBuffer);
                SafeRelease(suppliedBuffer);
            }
            if (FAILED(hr)) {
                SafeRelease(suppliedSample);
                return hr;
            }
            output.pSample = suppliedSample;
        }
        DWORD status = 0;
        HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);
        SafeRelease(output.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            SafeRelease(output.pSample);
            return SetNv12OutputType(transform_);
        }
        if (SUCCEEDED(hr) && output.pSample) {
            IMFMediaBuffer* buffer = nullptr;
            hr = output.pSample->ConvertToContiguousBuffer(&buffer);
            if (SUCCEEDED(hr)) {
                SafeRelease(*newest);
                *newest = buffer;
            }
        }
        SafeRelease(output.pSample);
        return hr;
    }

    IMFTransform* transform_ = nullptr;
    IMFMediaType* outputType_ = nullptr;
    MFT_OUTPUT_STREAM_INFO outputInfo_{};
    DWORD bufferBytes_ = 0;
    LONG stride_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool mfStarted_ = false;
};

static bool VideoFormatDetails(const AM_MEDIA_TYPE* mediaType, int& width,
                               int& height, REFERENCE_TIME& frameDuration,
                               DWORD& imageBytes,
                               VideoPixelFormat* pixelFormat = nullptr) {
    if (!mediaType || mediaType->majortype != MEDIATYPE_Video) {
        return false;
    }
    const VideoPixelFormat detected =
        VideoPixelFormatFromSubtype(mediaType->subtype);
    if (detected == VideoPixelFormat::Auto) return false;
    if (pixelFormat) {
        *pixelFormat = detected;
    }
    const BITMAPINFOHEADER* bitmap = nullptr;
    frameDuration = 0;
    if (mediaType->formattype == FORMAT_VideoInfo2 &&
        mediaType->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        const auto* info =
            reinterpret_cast<const VIDEOINFOHEADER2*>(mediaType->pbFormat);
        bitmap = &info->bmiHeader;
        frameDuration = info->AvgTimePerFrame;
    } else if (mediaType->formattype == FORMAT_VideoInfo &&
               mediaType->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        const auto* info =
            reinterpret_cast<const VIDEOINFOHEADER*>(mediaType->pbFormat);
        bitmap = &info->bmiHeader;
        frameDuration = info->AvgTimePerFrame;
    } else if (mediaType->formattype == FORMAT_MPEG2Video &&
               mediaType->cbFormat >=
                   FIELD_OFFSET(MPEG2VIDEOINFO, dwSequenceHeader)) {
        const auto* info =
            reinterpret_cast<const MPEG2VIDEOINFO*>(mediaType->pbFormat);
        bitmap = &info->hdr.bmiHeader;
        frameDuration = info->hdr.AvgTimePerFrame;
    }
    if (!bitmap) return false;
    width = static_cast<int>(bitmap->biWidth);
    height = std::abs(static_cast<int>(bitmap->biHeight));
    imageBytes = bitmap->biSizeImage;
    return width > 0 && height > 0;
}

// DirectShow carries extended color information in the upper 24 bits of
// VIDEOINFOHEADER2::dwControlFlags.  The low eight bits are reserved for the
// AMCONTROL_* flags.  Some capture drivers return a plain VIDEOINFOHEADER from
// IAMStreamConfig::GetFormat even when the matching stream-capability entry
// contains VIDEOINFOHEADER2, so this parser is intentionally independent of
// the active-format query.
struct DirectShowColorMetadata {
    bool present = false;
    DWORD controlFlags = 0;
    UINT chromaSubsampling = 0;
    UINT nominalRange = 0;
    UINT transferMatrix = 0;
    UINT lighting = 0;
    UINT primaries = 0;
    UINT transferFunction = 0;

    bool hdr10() const {
        // MF's canonical values are BT.2020 primaries (9), ST.2084/PQ (15),
        // and a BT.2020 YUV matrix (4 or 5).  The DirectShow bitfield uses
        // the same semantic values even though older dxva.h headers do not
        // name the HDR-era enum members.
        return present && primaries == 9 && transferFunction == 15 &&
               (transferMatrix == 4 || transferMatrix == 5);
    }
};

static void DeleteMediaType(AM_MEDIA_TYPE* mediaType);

static bool ExtractDirectShowColorMetadata(
    const AM_MEDIA_TYPE* mediaType, DirectShowColorMetadata& metadata) {
    metadata = {};
    if (!mediaType || !mediaType->pbFormat) return false;

    const VIDEOINFOHEADER2* info = nullptr;
    if (mediaType->formattype == FORMAT_VideoInfo2 &&
        mediaType->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        info = reinterpret_cast<const VIDEOINFOHEADER2*>(mediaType->pbFormat);
    } else if (mediaType->formattype == FORMAT_MPEG2Video &&
               mediaType->cbFormat >= sizeof(MPEG2VIDEOINFO)) {
        info = &reinterpret_cast<const MPEG2VIDEOINFO*>(
                    mediaType->pbFormat)->hdr;
    }
    if (!info || (info->dwControlFlags & AMCONTROL_COLORINFO_PRESENT) == 0) {
        return false;
    }

    const DWORD flags = info->dwControlFlags;
    metadata.present = true;
    metadata.controlFlags = flags;
    metadata.chromaSubsampling = DXVA_ExtractExtColorData(
        flags, DXVA_VideoChromaSubsamplingMask,
        DXVA_VideoChromaSubsamplingShift);
    metadata.nominalRange = DXVA_ExtractExtColorData(
        flags, DXVA_NominalRangeMask, DXVA_NominalRangeShift);
    metadata.transferMatrix = DXVA_ExtractExtColorData(
        flags, DXVA_VideoTransferMatrixMask, DXVA_VideoTransferMatrixShift);
    metadata.lighting = DXVA_ExtractExtColorData(
        flags, DXVA_VideoLightingMask, DXVA_VideoLightingShift);
    metadata.primaries = DXVA_ExtractExtColorData(
        flags, DXVA_VideoPrimariesMask, DXVA_VideoPrimariesShift);
    metadata.transferFunction = DXVA_ExtractExtColorData(
        flags, DXVA_VideoTransFuncMask, DXVA_VideoTransFuncShift);
    return true;
}

static const wchar_t* DirectShowTransferName(UINT value) {
    switch (value) {
    case 5: return L"BT.709";
    case 12: return L"BT.2020 constant";
    case 13: return L"BT.2020";
    case 15: return L"PQ/ST.2084";
    case 16: return L"HLG";
    default: return value == 0 ? L"unknown" : L"unrecognized";
    }
}

static const wchar_t* DirectShowPrimariesName(UINT value) {
    switch (value) {
    case 2: return L"BT.709";
    case 9: return L"BT.2020";
    case 11: return L"DCI-P3";
    default: return value == 0 ? L"unknown" : L"unrecognized";
    }
}

static void LogDirectShowColorMetadata(const wchar_t* source,
                                       const DirectShowColorMetadata& metadata) {
    if (!metadata.present) {
        fwprintf(stderr, L"[hdr] DirectShow color info (%s): unavailable.\n",
                 source ? source : L"unknown");
        return;
    }
    fwprintf(stderr,
             L"[hdr] DirectShow color info (%s): flags=0x%08X, primaries=%u (%s), "
             L"transfer=%u (%s), matrix=%u, range=%u%s.\n",
             source ? source : L"unknown",
             static_cast<unsigned>(metadata.controlFlags), metadata.primaries,
             DirectShowPrimariesName(metadata.primaries),
             metadata.transferFunction,
             DirectShowTransferName(metadata.transferFunction),
             metadata.transferMatrix, metadata.nominalRange,
             metadata.hdr10() ? L" [HDR10 candidate]" : L"");
}

static bool FindMatchingDirectShowColorMetadata(
    IPin* videoPin, VideoPixelFormat wantedFormat, int wantedWidth,
    int wantedHeight, int wantedFps, DirectShowColorMetadata& metadata) {
    metadata = {};
    if (!videoPin) return false;
    IAMStreamConfig* config = nullptr;
    HRESULT hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    if (FAILED(hr)) return false;

    int count = 0;
    int capBytes = 0;
    hr = config->GetNumberOfCapabilities(&count, &capBytes);
    if (FAILED(hr) || capBytes < static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS))) {
        SafeRelease(config);
        return false;
    }
    std::vector<BYTE> caps(static_cast<size_t>(capBytes));
    bool found = false;
    for (int i = 0; i < count && !found; ++i) {
        AM_MEDIA_TYPE* candidate = nullptr;
        if (FAILED(config->GetStreamCaps(i, &candidate, caps.data())) ||
            !candidate) {
            continue;
        }
        int width = 0;
        int height = 0;
        REFERENCE_TIME duration = 0;
        DWORD bytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Auto;
        const bool details = VideoFormatDetails(candidate, width, height,
                                                duration, bytes, &format);
        const int fps = duration > 0
            ? static_cast<int>((10'000'000 + duration / 2) / duration) : 0;
        DirectShowColorMetadata candidateMetadata{};
        const bool hasColor = ExtractDirectShowColorMetadata(
            candidate, candidateMetadata);
        if (details && format == wantedFormat && width == wantedWidth &&
            height == wantedHeight && fps == wantedFps && hasColor) {
            metadata = candidateMetadata;
            found = true;
        }
        DeleteMediaType(candidate);
    }
    SafeRelease(config);
    return found;
}

static bool SetVideoFrameDuration(AM_MEDIA_TYPE* mediaType,
                                  REFERENCE_TIME frameDuration) {
    if (!mediaType || frameDuration <= 0) return false;
    if (mediaType->formattype == FORMAT_VideoInfo2 &&
        mediaType->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        auto* info = reinterpret_cast<VIDEOINFOHEADER2*>(mediaType->pbFormat);
        info->AvgTimePerFrame = frameDuration;
        return true;
    }
    if (mediaType->formattype == FORMAT_VideoInfo &&
        mediaType->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        auto* info = reinterpret_cast<VIDEOINFOHEADER*>(mediaType->pbFormat);
        info->AvgTimePerFrame = frameDuration;
        return true;
    }
    if (mediaType->formattype == FORMAT_MPEG2Video &&
        mediaType->cbFormat >= FIELD_OFFSET(MPEG2VIDEOINFO, dwSequenceHeader)) {
        auto* info = reinterpret_cast<MPEG2VIDEOINFO*>(mediaType->pbFormat);
        info->hdr.AvgTimePerFrame = frameDuration;
        return true;
    }
    return false;
}

static REFERENCE_TIME FrameDurationForRate(int fps) {
    return fps > 0 ? (10'000'000 + fps / 2) / fps : 0;
}

static bool FrameRateAllowedByCaps(const VIDEO_STREAM_CONFIG_CAPS& caps,
                                   int fps) {
    const REFERENCE_TIME duration = FrameDurationForRate(fps);
    if (duration <= 0 || caps.MinFrameInterval <= 0 ||
        caps.MaxFrameInterval <= 0) {
        return false;
    }
    const REFERENCE_TIME minimum =
        std::min(caps.MinFrameInterval, caps.MaxFrameInterval);
    const REFERENCE_TIME maximum =
        std::max(caps.MinFrameInterval, caps.MaxFrameInterval);
    return duration >= minimum && duration <= maximum;
}

static HRESULT ConfigureVideoPin(IPin* videoPin, int wantedWidth,
                                 int wantedHeight, int wantedFps,
                                 VideoPixelFormat wantedFormat,
                                 DWORD& imageBytes, UINT32& stride,
                                 int& configuredFps,
                                 VideoPixelFormat& configuredFormat) {
    IAMStreamConfig* config = nullptr;
    HRESULT hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    if (FAILED(hr)) return hr;

    int count = 0;
    int capBytes = 0;
    hr = config->GetNumberOfCapabilities(&count, &capBytes);
    if (FAILED(hr) || capBytes < static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS))) {
        SafeRelease(config);
        return FAILED(hr) ? hr : E_FAIL;
    }

    struct FormatCandidate {
        AM_MEDIA_TYPE* mediaType = nullptr;
        int fps = 0;
        DWORD imageBytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Nv12;
    };
    std::vector<BYTE> caps(static_cast<size_t>(capBytes));
    std::vector<FormatCandidate> candidates;
    for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* candidate = nullptr;
        hr = config->GetStreamCaps(i, &candidate, caps.data());
        if (FAILED(hr) || !candidate) continue;

        VIDEO_STREAM_CONFIG_CAPS streamCaps{};
        std::memcpy(&streamCaps, caps.data(), sizeof(streamCaps));

        int width = 0;
        int height = 0;
        REFERENCE_TIME duration = 0;
        DWORD bytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Nv12;
        const bool details = VideoFormatDetails(candidate, width, height,
                                                duration, bytes, &format);
        int fps = duration > 0
                      ? static_cast<int>((10'000'000 + duration / 2) /
                                         duration)
                      : 0;
        // Compressed capture is opt-in. Auto preserves the original raw
        // NV12/YUY2 path even when the device advertises alternatives.
        const bool formatMatches = wantedFormat == VideoPixelFormat::Auto
            ? format == VideoPixelFormat::Nv12 ||
              format == VideoPixelFormat::Yuy2
            : format == wantedFormat;
        if (details && formatMatches && width == wantedWidth &&
            height == wantedHeight && fps > 0) {
            // Some UVC drivers advertise only their maximum-rate media type
            // and expose slower supported rates through the capability range.
            // Materialize the requested rate so 30 fps configures the capture
            // pin itself instead of receiving a faster stream and dropping it.
            if (wantedFps == 30 && fps != wantedFps &&
                FrameRateAllowedByCaps(streamCaps, wantedFps) &&
                SetVideoFrameDuration(candidate,
                                      FrameDurationForRate(wantedFps))) {
                fps = wantedFps;
            }
            candidates.push_back({candidate, fps, bytes, format});
            continue;
        }
        DeleteMediaType(candidate);
    }

    if (candidates.empty()) {
        SafeRelease(config);
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }

    // Prefer the requested rate. Otherwise keep the exact pixel dimensions
    // and select the highest supported rate not exceeding the request. If the
    // driver exposes only higher rates, select the lowest of those.
    std::stable_sort(
        candidates.begin(), candidates.end(),
        [wantedFps, wantedFormat](const FormatCandidate& a,
                                 const FormatCandidate& b) {
            if (wantedFormat == VideoPixelFormat::Auto &&
                a.format != b.format) {
                const auto rank = [](VideoPixelFormat format) {
                    switch (format) {
                    case VideoPixelFormat::Nv12: return 0;
                    case VideoPixelFormat::Yuy2: return 1;
                    case VideoPixelFormat::P010: return 2;
                    case VideoPixelFormat::Mjpeg: return 3;
                    default: return 3;
                    }
                };
                return rank(a.format) < rank(b.format);
            }
            if (a.fps == wantedFps || b.fps == wantedFps) {
                return a.fps == wantedFps && b.fps != wantedFps;
            }
            const bool aWithin = a.fps < wantedFps;
            const bool bWithin = b.fps < wantedFps;
            if (aWithin != bWithin) return aWithin;
            return aWithin ? a.fps > b.fps : a.fps < b.fps;
        });

    hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    FormatCandidate* selected = nullptr;
    for (auto& candidate : candidates) {
        hr = config->SetFormat(candidate.mediaType);
        if (SUCCEEDED(hr)) {
            selected = &candidate;
            break;
        }
    }

    if (selected) {
        imageBytes = selected->imageBytes;
        configuredFps = selected->fps;
        configuredFormat = selected->format;
        if (!imageBytes && !IsCompressedVideoFormat(configuredFormat)) {
            imageBytes = static_cast<DWORD>(
                configuredFormat == VideoPixelFormat::Yuy2
                    ? wantedWidth * wantedHeight * 2
                    : configuredFormat == VideoPixelFormat::P010
                        ? wantedWidth * wantedHeight * 3
                        : wantedWidth * wantedHeight * 3 / 2);
        }
        const uint64_t derivedStride = configuredFormat ==
                                                VideoPixelFormat::Yuy2
            ? static_cast<uint64_t>(imageBytes) / wantedHeight
            : configuredFormat == VideoPixelFormat::P010
                ? static_cast<uint64_t>(imageBytes) * 2 /
                      (static_cast<uint64_t>(wantedHeight) * 3)
            : (static_cast<uint64_t>(imageBytes) * 2) /
                  (static_cast<uint64_t>(wantedHeight) * 3);
        const UINT32 minimumStride = configuredFormat ==
                                             VideoPixelFormat::Yuy2
            ? static_cast<UINT32>(wantedWidth * 2)
            : configuredFormat == VideoPixelFormat::P010
                ? static_cast<UINT32>(wantedWidth * 2)
                : static_cast<UINT32>(wantedWidth);
        stride = IsCompressedVideoFormat(configuredFormat)
            ? 0
            : static_cast<UINT32>(derivedStride >= minimumStride
                                      ? derivedStride : minimumStride);
        if (configuredFps != wantedFps) {
            fwprintf(stderr,
                     L"[video] requested %s %dx%d @ %d unavailable; "
                     L"using %d fps at the exact resolution.\n",
                     PixelFormatName(configuredFormat), wantedWidth,
                     wantedHeight, wantedFps, configuredFps);
        }
    }
    for (auto& candidate : candidates) {
        DeleteMediaType(candidate.mediaType);
    }
    SafeRelease(config);
    return hr;
}

static HRESULT GetActiveVideoPinFormat(IPin* videoPin,
                                       AM_MEDIA_TYPE** mediaType) {
    if (!videoPin || !mediaType) return E_POINTER;
    *mediaType = nullptr;
    IAMStreamConfig* config = nullptr;
    HRESULT hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    if (SUCCEEDED(hr)) hr = config->GetFormat(mediaType);
    SafeRelease(config);
    return hr;
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
    IAMStreamConfig* config = nullptr;
    HRESULT hr = FindCaptureFilter(captureDeviceId, &capture);
    if (SUCCEEDED(hr)) {
        hr = FindOutputPinByMajorType(capture, MEDIATYPE_Video, &videoPin);
    }
    if (SUCCEEDED(hr)) hr = videoPin->QueryInterface(IID_PPV_ARGS(&config));
    int count = 0;
    int capBytes = 0;
    if (SUCCEEDED(hr)) hr = config->GetNumberOfCapabilities(&count, &capBytes);
    std::vector<BYTE> caps(capBytes > 0 ? static_cast<size_t>(capBytes) : 1);
    for (int i = 0; SUCCEEDED(hr) && i < count; ++i) {
        AM_MEDIA_TYPE* mediaType = nullptr;
        if (FAILED(config->GetStreamCaps(i, &mediaType, caps.data())) ||
            !mediaType) continue;
        VIDEO_STREAM_CONFIG_CAPS streamCaps{};
        if (capBytes >= static_cast<int>(sizeof(streamCaps))) {
            std::memcpy(&streamCaps, caps.data(), sizeof(streamCaps));
        }
        int candidateWidth = 0;
        int candidateHeight = 0;
        REFERENCE_TIME duration = 0;
        DWORD bytes = 0;
        VideoPixelFormat format = VideoPixelFormat::Auto;
        if (VideoFormatDetails(mediaType, candidateWidth, candidateHeight,
                               duration, bytes, &format) &&
            format != VideoPixelFormat::Auto &&
            candidateWidth == width && candidateHeight == height &&
            duration > 0) {
            const int fps = static_cast<int>(
                (10'000'000 + duration / 2) / duration);
            const bool duplicate = std::any_of(
                result.begin(), result.end(),
                [format, fps](const PixelFormatSupport& support) {
                    return support.format == format &&
                           support.selectedFps == fps;
                });
            if (!duplicate) {
                result.push_back({format, fps});
            }

            // A number of UVC drivers list only the fastest default media
            // type, while VIDEO_STREAM_CONFIG_CAPS still declares 30 fps as
            // valid. Surface that standard rate in the selector as well.
            constexpr int kAdditionalFrameRate = 30;
            const bool thirtyFpsDuplicate = std::any_of(
                result.begin(), result.end(),
                [format](const PixelFormatSupport& support) {
                    return support.format == format &&
                           support.selectedFps == 30;
                });
            if (!thirtyFpsDuplicate &&
                FrameRateAllowedByCaps(streamCaps, kAdditionalFrameRate)) {
                result.push_back({format, kAdditionalFrameRate});
            }
        }
        DeleteMediaType(mediaType);
    }
    std::sort(result.begin(), result.end(),
              [](const PixelFormatSupport& left,
                 const PixelFormatSupport& right) {
                  if (left.format != right.format) {
                      const auto rank = [](VideoPixelFormat format) {
                          switch (format) {
                          case VideoPixelFormat::Nv12: return 0;
                          case VideoPixelFormat::Yuy2: return 1;
                          case VideoPixelFormat::P010: return 2;
                          case VideoPixelFormat::Mjpeg: return 3;
                          default: return 3;
                          }
                      };
                      return rank(left.format) < rank(right.format);
                  }
                  return left.selectedFps > right.selectedFps;
              });
    SafeRelease(config);
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
            ? L"WASAPI Exclusive" : L"WASAPI Shared";
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

static std::wstring BuildRuntimeOsdText(int outputWidth, int outputHeight,
                                         size_t outputNameLimit = 64,
                                         size_t captureNameLimit = 42);

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
    uint64_t nextOcclusionTestMs = 0;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_NV12;
    bool hdrOutput = false;

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
        nextOcclusionTestMs = 0;
        inputFormat = DXGI_FORMAT_NV12;
        hdrOutput = false;
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
                       bool hdrInputMetadataAvailable = false) {
        reset();
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
            inputColor.YCbCr_Matrix = 1; // BT.709
            inputColor.Nominal_Range = 1; // studio 16-235
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

        // Keep the diagnostics panel at a stable size, but adapt long device
        // names to the actual rendered line width. A wrapped line must not
        // push the final audio diagnostics below the panel's bottom edge.
        std::wstring osdText;
        HRESULT hr = E_FAIL;
        constexpr UINT32 kExpectedOsdLineCount = 19;
        constexpr size_t kMinimumOutputNameLimit = 20;
        constexpr size_t kMinimumCaptureNameLimit = 20;
        // Start with a more generous name budget now that the diagnostics
        // layout has dedicated lines for the device/buffer information. The
        // line-count check below still backs these values down on narrower
        // fonts or unusually long endpoint names, so the OSD cannot clip.
        size_t outputNameLimit = 64;
        size_t captureNameLimit = 42;
        for (;;) {
            osdText = BuildRuntimeOsdText(
                static_cast<int>(outputWidth), static_cast<int>(outputHeight),
                outputNameLimit, captureNameLimit);
            IDWriteTextLayout* candidate = nullptr;
            hr = dwriteFactory->CreateTextLayout(
                osdText.c_str(), static_cast<UINT32>(osdText.size()),
                osdTextFormat, kOsdTextWidth, kOsdTextHeight, &candidate);
            if (FAILED(hr)) return hr;
            UINT32 lineCount = 0;
            candidate->GetLineMetrics(nullptr, 0, &lineCount);
            if (lineCount <= kExpectedOsdLineCount ||
                (outputNameLimit <= kMinimumOutputNameLimit &&
                 captureNameLimit <= kMinimumCaptureNameLimit)) {
                osdTextLayout = candidate;
                break;
            }
            candidate->Release();
            if (outputNameLimit > kMinimumOutputNameLimit + 1) {
                outputNameLimit -= 2;
            } else if (captureNameLimit > kMinimumCaptureNameLimit + 1) {
                captureNameLimit -= 2;
            } else {
                outputNameLimit = kMinimumOutputNameLimit;
                captureNameLimit = kMinimumCaptureNameLimit;
            }
        }

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

    IGraphBuilder* graph = nullptr;
    IMediaControl* control = nullptr;
    IMediaFilter* mediaFilter = nullptr;
    IBaseFilter* capture = nullptr;
    IBaseFilter* audioCapture = nullptr;
    IPin* audioPin = nullptr;
    IBaseFilter* audioGrabberFilter = nullptr;
    ISampleGrabber* audioGrabber = nullptr;
    IBaseFilter* audioNullRenderer = nullptr;
    IPin* audioGrabberIn = nullptr;
    IPin* audioGrabberOut = nullptr;
    IPin* audioNullIn = nullptr;
    SampleGrabberCB* audioCallback = nullptr;
    AM_MEDIA_TYPE* selectedAudioType = nullptr;
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
        selectedAudioType = SelectSupportedCaptureAudioType(
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
        hr = CoCreateInstance(CLSID_SampleGrabber, nullptr,
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
        audioCallback = new SampleGrabberCB(selectedAudioFormat);
        hr = audioGrabber->SetCallback(audioCallback, 0);
        if (FAILED(hr)) break;

        hr = CoCreateInstance(CLSID_NullRenderer, nullptr,
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
    if (audioGrabber) audioGrabber->SetCallback(nullptr, 0);
    if (audioCallback) audioCallback->Release();
    SafeRelease(audioNullIn);
    SafeRelease(audioGrabberOut);
    SafeRelease(audioGrabberIn);
    SafeRelease(audioNullRenderer);
    SafeRelease(audioGrabber);
    SafeRelease(audioGrabberFilter);
    DeleteMediaType(selectedAudioType);
    SafeRelease(audioPin);
    SafeRelease(audioCapture);
    SafeRelease(capture);
    SafeRelease(mediaFilter);
    SafeRelease(control);
    SafeRelease(graph);
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

    IGraphBuilder* graph = nullptr;
    IMediaControl* control = nullptr;
    IMediaFilter* mediaFilter = nullptr;
    IBaseFilter* capture = nullptr;
    IBaseFilter* audioCapture = nullptr;
    IPin* videoPin = nullptr;
    IPin* audioPin = nullptr;
    IBaseFilter* grabberFilter = nullptr;
    ISampleGrabber* grabber = nullptr;
    IBaseFilter* nullRenderer = nullptr;
    IPin* grabberIn = nullptr;
    IPin* grabberOut = nullptr;
    IPin* nullIn = nullptr;
    VideoSampleGrabberCB* callback = nullptr;
    IBaseFilter* audioGrabberFilter = nullptr;
    ISampleGrabber* audioGrabber = nullptr;
    IBaseFilter* audioNullRenderer = nullptr;
    IPin* audioGrabberIn = nullptr;
    IPin* audioGrabberOut = nullptr;
    IPin* audioNullIn = nullptr;
    SampleGrabberCB* audioCallback = nullptr;
    AM_MEDIA_TYPE* activeVideoType = nullptr;
    AM_MEDIA_TYPE* selectedAudioType = nullptr;
    llcv::capture_audio::Format selectedAudioFormat{};
    HANDLE frameEvent = nullptr;
    bool initialized = false;
    const wchar_t* initializationStage = L"create DirectShow graph";
    DirectD3D11Renderer renderer;
    MediaFoundationCompressedDecoder compressedDecoder;

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
        bool hdrInputMetadataDetected =
            configuredFormat == VideoPixelFormat::P010 &&
            ExtractDirectShowColorMetadata(activeVideoType,
                                           directShowColorInfo) &&
            directShowColorInfo.hdr10();
        if (configuredFormat == VideoPixelFormat::P010 &&
            !hdrInputMetadataDetected) {
            // A few capture drivers return a plain VIDEOINFOHEADER from
            // GetFormat but retain the extended color information on the
            // matching stream-capability entry.  Check that entry before
            // falling back to SDR.
            DirectShowColorMetadata capabilityColorInfo{};
            if (FindMatchingDirectShowColorMetadata(
                    videoPin, configuredFormat, preset.width, preset.height,
                    configuredFps, capabilityColorInfo)) {
                directShowColorInfo = capabilityColorInfo;
                hdrInputMetadataDetected = directShowColorInfo.hdr10();
            }
        }
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

        g_videoConfiguredFps.store(configuredFps,
                                   std::memory_order_release);
        initializationStage = L"initialize D3D11 video renderer";
        hr = renderer.initialize(host, preset.width, preset.height,
                                 configuredFps, rendererInputFormat,
                                 hdrInputMetadataAvailable);
        if (FAILED(hr)) {
            LogHr(L"DirectD3D11Renderer::initialize", hr);
            break;
        }
        if (compressedVideo) {
            initializationStage = L"initialize Media Foundation compressed decoder";
            hr = compressedDecoder.initialize(configuredFormat, preset.width,
                                              preset.height, configuredFps,
                                              activeVideoType);
            if (FAILED(hr)) {
                LogHr(L"Media Foundation compressed decoder", hr);
                break;
            }
        }
        UpdateConfiguredVideoTitle(host, configuredFps);

        frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!frameEvent) { hr = HRESULT_FROM_WIN32(GetLastError()); break; }
        LatestNv12Sample latest(compressedVideo ? 0 : imageBytes, frameEvent);

        initializationStage = L"build video sample path";
        hr = CoCreateInstance(CLSID_SampleGrabber, nullptr,
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
        callback = new VideoSampleGrabberCB(&latest);
        hr = grabber->SetCallback(callback, 0);
        if (FAILED(hr)) break;

        hr = CoCreateInstance(CLSID_NullRenderer, nullptr,
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
        // connection type, not on IAMStreamConfig::GetFormat.  Inspect the
        // Sample Grabber's connected type before starting the graph and, if
        // it contains a complete HDR10 description, rebuild only the D3D11
        // output resources with the HDR color space enabled.
        if (configuredFormat == VideoPixelFormat::P010) {
            AM_MEDIA_TYPE connectedVideoType{};
            const HRESULT connectedTypeHr =
                grabber->GetConnectedMediaType(&connectedVideoType);
            if (SUCCEEDED(connectedTypeHr)) {
                DirectShowColorMetadata connectedColorInfo{};
                if (ExtractDirectShowColorMetadata(&connectedVideoType,
                                                   connectedColorInfo)) {
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
                            break;
                        }
                    }
                }
            }
            FreeMediaType(connectedVideoType);
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
        selectedAudioType = SelectSupportedCaptureAudioType(
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
        hr = CoCreateInstance(CLSID_SampleGrabber, nullptr,
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
        audioCallback = new SampleGrabberCB(selectedAudioFormat);
        hr = audioGrabber->SetCallback(audioCallback, 0);
        if (FAILED(hr)) break;

        hr = CoCreateInstance(CLSID_NullRenderer, nullptr,
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
                    rendererInputFormat, hdrInputMetadataAvailable);
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
            IMediaSample* videoSample = latest.takeLatest(arrivalUs);
            if (!videoSample) continue;
            if (renderer.outputConfigurationChanged()) {
                hr = renderer.initialize(host, preset.width, preset.height,
                                         configuredFps,
                                         rendererInputFormat,
                                         hdrInputMetadataAvailable);
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
    if (audioGrabber) audioGrabber->SetCallback(nullptr, 0);
    if (audioCallback) audioCallback->Release();
    if (grabber) grabber->SetCallback(nullptr, 0);
    if (callback) callback->Release();
    if (frameEvent) CloseHandle(frameEvent);
    SafeRelease(nullIn);
    SafeRelease(audioNullIn);
    SafeRelease(audioGrabberOut);
    SafeRelease(audioGrabberIn);
    SafeRelease(audioNullRenderer);
    SafeRelease(audioGrabber);
    SafeRelease(audioGrabberFilter);
    SafeRelease(grabberOut);
    SafeRelease(grabberIn);
    SafeRelease(nullRenderer);
    SafeRelease(grabber);
    SafeRelease(grabberFilter);
    DeleteMediaType(activeVideoType);
    DeleteMediaType(selectedAudioType);
    SafeRelease(videoPin);
    SafeRelease(audioPin);
    SafeRelease(audioCapture);
    SafeRelease(capture);
    SafeRelease(mediaFilter);
    SafeRelease(control);
    SafeRelease(graph);
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
constexpr int IDC_SETTINGS_ADVANCED_TOGGLE = 2031;
constexpr int IDC_SETTINGS_AUDIO_ONLY = 2032;
constexpr int IDC_SETTINGS_FORCE_HDR10 = 2033;
constexpr int IDC_SETTINGS_FORCE_HDR10_HELP = 2034;
constexpr UINT WM_AUDIOCLIENT3_PROBE_COMPLETE = WM_APP + 73;
constexpr UINT WM_SETTINGS_TOOLTIP_SHOW = WM_APP + 74;
constexpr UINT WM_SETTINGS_TOOLTIP_HIDE = WM_APP + 75;
constexpr UINT WM_CAPTURE_AUDIO_PROBE_COMPLETE = WM_APP + 76;

struct SettingsDialogState {
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
    HWND driftCombo = nullptr;
    HWND pcmQueueCombo = nullptr;
    HWND audioStatus = nullptr;
    HWND presentationCombo = nullptr;
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
    HWND versionWatermark = nullptr;
    HWND advancedToggle = nullptr;
    HWND startButton = nullptr;
    HWND cancelButton = nullptr;
    HWND tooltipWindow = nullptr;
    HWND activeTooltipTarget = nullptr;
    std::vector<HFONT> uiFonts;
    std::thread probeThread;
    std::thread captureAudioProbeThread;
    std::atomic<bool> probeReady{false};
    std::atomic<bool> captureAudioProbeReady{false};
    AudioClient3Support probe{};
    InternalCaptureAudioProbe captureAudioProbe{};
    std::vector<UINT32> sharedPeriodChoices;
    std::vector<CaptureDeviceInfo> captureDevices;
    std::vector<CaptureDeviceInfo> captureAudioDevices;
    std::vector<AudioEndpointInfo> audioEndpoints;
    std::vector<PixelFormatSupport> pixelFormats;
    VideoPreset initialVideoPreset = VideoPreset::R1920x1080;
    HMONITOR viewerMonitor = nullptr;
    UINT32 selectedSharedPeriodFrames = 0;
    int selectedBufferMs = kRecommendedWasapiBufferMs;
    bool bufferItemsAreSharedFrames = false;
    bool showAdvanced = false;
    bool accepted = false;
};

static int SettingsPixels(int dips, UINT dpi) {
    return MulDiv(dips, dpi ? dpi : USER_DEFAULT_SCREEN_DPI,
                  USER_DEFAULT_SCREEN_DPI);
}

static constexpr int kSettingsClientWidthDip = 950;
static constexpr int kSettingsBasicClientHeightDip = 510;
static constexpr int kSettingsAdvancedClientHeightDip = 690;

static int SettingsClientHeightDip(const SettingsDialogState* state) {
    return state && state->showAdvanced
        ? kSettingsAdvancedClientHeightDip : kSettingsBasicClientHeightDip;
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
}

static void LayoutSettingsControls(SettingsDialogState* state, UINT dpi) {
    if (!state) return;
    if (!state->showAdvanced) {
        const bool pixelPerfect = state->pixelCheck &&
            SendMessageW(state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool relativeSize = state->relativeSizeCheck &&
            SendMessageW(state->relativeSizeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const int relativeY = pixelPerfect ? 346 : 382;
        const bool showRelativeWarning = pixelPerfect && relativeSize;
        const int borderlessY = showRelativeWarning
            ? relativeY + 64 : relativeY + 34;

        PlaceSettingsControl(state->audioLabel, 24, 24, 160, 24, dpi);
        PlaceSettingsControl(state->audioCombo, 195, 20, 280, 120, dpi);
        PlaceSettingsControl(state->audioOutputLabel, 24, 68, 160, 24, dpi);
        PlaceSettingsControl(state->audioOutputCombo, 195, 64, 280, 220, dpi);
        // Keep background mute beside the audio choices. Starting immediately
        // on the next launch is an application-behavior preference, so it
        // stays in the lower settings section with its explanatory text.
        PlaceSettingsControl(state->muteBackgroundCheck, 24, 112, 451, 28, dpi);
        PlaceSettingsControl(state->audioOnlyCheck, 24, 146, 451, 28, dpi);
        // Leave a deliberate gap before application behavior preferences so
        // language and quick start read as a separate category from audio.
        PlaceSettingsControl(state->languageLabel, 24, 180, 160, 24, dpi);
        PlaceSettingsControl(state->languageCombo, 195, 176, 280, 120, dpi);
        PlaceSettingsControl(state->skipStartupCheck, 24, 224, 451, 28, dpi);
        PlaceSettingsControl(state->skipStartupHint, 44, 252, 431, 42, dpi);
        PlaceSettingsControl(state->advancedToggle, 24, 316, 190, 28, dpi);
        PlaceSettingsControl(state->versionWatermark, 24, 466, 260, 20, dpi);

        PlaceSettingsControl(state->presentationLabel, 505, 24, 95, 24, dpi);
        PlaceSettingsControl(state->presentationHelp, 604, 20, 24, 24, dpi);
        PlaceSettingsControl(state->presentationCombo, 630, 20, 295, 120, dpi);
        PlaceSettingsControl(state->captureDeviceLabel, 505, 68, 120, 24, dpi);
        PlaceSettingsControl(state->captureDeviceCombo, 630, 64, 295, 220, dpi);
        PlaceSettingsControl(state->captureAudioDeviceLabel, 505, 112, 120, 24, dpi);
        PlaceSettingsControl(state->captureAudioDeviceCombo, 630, 108, 295, 220, dpi);
        PlaceSettingsControl(state->captureAudioStatus, 630, 112, 295, 24, dpi);
        PlaceSettingsControl(state->videoLabel, 505, 156, 120, 24, dpi);
        PlaceSettingsControl(state->videoCombo, 630, 152, 295, 120, dpi);
        PlaceSettingsControl(state->pixelFormatLabel, 505, 200, 120, 24, dpi);
        PlaceSettingsControl(state->pixelFormatCombo, 630, 196, 295, 160, dpi);
        PlaceSettingsControl(state->frameRateLabel, 505, 244, 120, 24, dpi);
        PlaceSettingsControl(state->frameRateCombo, 630, 240, 295, 200, dpi);
        PlaceSettingsControl(state->videoCapabilityStatus, 505, 278, 420, 24, dpi);
        PlaceSettingsControl(state->pixelCheck, 505, 312, 420, 28, dpi);
        PlaceSettingsControl(state->scalingLabel, 505, 346, 120, 24, dpi);
        PlaceSettingsControl(state->scalingCombo, 630, 342, 295, 120, dpi);
        PlaceSettingsControl(state->relativeSizeCheck, 505, relativeY, 420, 28, dpi);
        PlaceSettingsControl(state->relativeSizeWarning, 525, relativeY + 28,
                             400, 28, dpi);
        PlaceSettingsControl(state->borderlessCheck, 505, borderlessY, 420, 28, dpi);
        PlaceSettingsControl(state->startButton, 745, 454, 80, 30, dpi);
        PlaceSettingsControl(state->cancelButton, 835, 454, 80, 30, dpi);
        return;
    }

    PlaceSettingsControl(state->audioLabel, 24, 24, 160, 24, dpi);
    PlaceSettingsControl(state->audioCombo, 195, 20, 280, 120, dpi);
    PlaceSettingsControl(state->audioOutputLabel, 24, 68, 160, 24, dpi);
    PlaceSettingsControl(state->audioOutputCombo, 195, 64, 280, 220, dpi);
    PlaceSettingsControl(state->bufferLabel, 24, 112, 160, 24, dpi);
    PlaceSettingsControl(state->bufferCombo, 195, 108, 280, 180, dpi);
    PlaceSettingsControl(state->audioStatus, 24, 148, 451, 28, dpi);
    PlaceSettingsControl(state->volumeHudLabel, 24, 190, 160, 24, dpi);
    PlaceSettingsControl(state->volumeHudCombo, 195, 186, 280, 160, dpi);
    PlaceSettingsControl(state->volumeBoostCheck, 24, 230, 400, 28, dpi);
    PlaceSettingsControl(state->volumeBoostHelp, 438, 226, 24, 24, dpi);
    PlaceSettingsControl(state->muteBackgroundCheck, 24, 274, 230, 28, dpi);
    PlaceSettingsControl(state->audioOnlyCheck, 260, 274, 215, 28, dpi);
    PlaceSettingsControl(state->driftLabel, 24, 318, 140, 24, dpi);
    PlaceSettingsControl(state->driftHelp, 168, 314, 24, 24, dpi);
    PlaceSettingsControl(state->driftCombo, 195, 314, 280, 120, dpi);
    PlaceSettingsControl(state->pcmQueueLabel, 24, 362, 160, 24, dpi);
    PlaceSettingsControl(state->pcmQueueHelp, 168, 358, 24, 24, dpi);
    PlaceSettingsControl(state->pcmQueueCombo, 195, 358, 280, 140, dpi);
    // Keep application behavior visually separate from the audio controls.
    PlaceSettingsControl(state->languageLabel, 24, 412, 160, 24, dpi);
    PlaceSettingsControl(state->languageCombo, 195, 408, 280, 120, dpi);
    PlaceSettingsControl(state->skipStartupCheck, 24, 456, 451, 28, dpi);
    PlaceSettingsControl(state->skipStartupHint, 44, 484, 431, 42, dpi);
    PlaceSettingsControl(state->advancedToggle, 24, 540, 190, 28, dpi);
    PlaceSettingsControl(state->versionWatermark, 24, 596, 260, 20, dpi);

    const bool pixelPerfect = state->pixelCheck &&
        SendMessageW(state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool relativeSize = state->relativeSizeCheck &&
        SendMessageW(state->relativeSizeCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool showRelativeWarning = pixelPerfect && relativeSize;
    const int borderlessY = showRelativeWarning ? 482 : 446;
    const int snapY = borderlessY + 34;

    PlaceSettingsControl(state->presentationLabel, 505, 24, 95, 24, dpi);
    PlaceSettingsControl(state->presentationHelp, 604, 20, 24, 24, dpi);
    PlaceSettingsControl(state->presentationCombo, 630, 20, 295, 120, dpi);
    PlaceSettingsControl(state->captureDeviceLabel, 505, 68, 120, 24, dpi);
    PlaceSettingsControl(state->captureDeviceCombo, 630, 64, 295, 220, dpi);
    PlaceSettingsControl(state->captureAudioDeviceLabel, 505, 112, 120, 24, dpi);
    PlaceSettingsControl(state->captureAudioDeviceCombo, 630, 108, 295, 220, dpi);
    PlaceSettingsControl(state->captureAudioStatus, 630, 112, 295, 24, dpi);
    PlaceSettingsControl(state->videoLabel, 505, 156, 120, 24, dpi);
    PlaceSettingsControl(state->videoCombo, 630, 152, 295, 120, dpi);
    PlaceSettingsControl(state->pixelFormatLabel, 505, 200, 120, 24, dpi);
    PlaceSettingsControl(state->pixelFormatCombo, 630, 196, 295, 160, dpi);
    PlaceSettingsControl(state->frameRateLabel, 505, 244, 120, 24, dpi);
    PlaceSettingsControl(state->frameRateCombo, 630, 240, 295, 200, dpi);
    PlaceSettingsControl(state->videoCapabilityStatus, 505, 278, 420, 24, dpi);
    // Match the compact layout: choose pixel-perfect first, then its
    // applicable scaling method immediately underneath.
    PlaceSettingsControl(state->pixelCheck, 505, 310, 420, 28, dpi);
    PlaceSettingsControl(state->forceHdr10Check, 505, 378, 390, 28, dpi);
    PlaceSettingsControl(state->forceHdr10Help, 900, 374, 24, 24, dpi);
    PlaceSettingsControl(state->scalingLabel, 505, 344, 120, 24, dpi);
    PlaceSettingsControl(state->scalingCombo, 630, 340, 295, 120, dpi);
    PlaceSettingsControl(state->relativeSizeCheck, 505, 412, 420, 28, dpi);
    PlaceSettingsControl(state->relativeSizeWarning, 525, 440, 400, 36, dpi);
    // Window behavior stays together; diagnostics are a separate group below.
    PlaceSettingsControl(state->borderlessCheck, 505, borderlessY, 420, 28, dpi);
    PlaceSettingsControl(state->windowSnapCheck, 505, snapY, 420, 28, dpi);
    PlaceSettingsControl(state->saveLogCheck, 505, 550, 420, 28, dpi);
    PlaceSettingsControl(state->showConsoleCheck, 505, 584, 420, 28, dpi);
    PlaceSettingsControl(state->startButton, 745, 624, 80, 30, dpi);
    PlaceSettingsControl(state->cancelButton, 835, 624, 80, 30, dpi);
}

static void SetSettingsControlVisible(HWND control, bool visible) {
    if (!control) return;
    ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    EnableWindow(control, visible ? TRUE : FALSE);
}

static void UpdateScalingControlVisibility(SettingsDialogState* state) {
    if (!state) return;
    const bool pixelPerfect = state->pixelCheck &&
        SendMessageW(state->pixelCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool visible = state->showAdvanced || !pixelPerfect;
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
    SetSettingsControlVisible(state->relativeSizeCheck, true);
    SetSettingsControlVisible(state->borderlessCheck, true);
    SetSettingsControlVisible(state->relativeSizeWarning,
                              pixelPerfect && relativeSize);
}

static void UpdateAdvancedControlVisibility(SettingsDialogState* state) {
    if (!state) return;
    const bool visible = state->showAdvanced;
    for (HWND control : {
             state->bufferLabel, state->bufferCombo, state->audioStatus,
             state->volumeHudLabel, state->volumeHudCombo,
             state->volumeBoostCheck, state->volumeBoostHelp,
             state->driftLabel, state->driftHelp, state->driftCombo,
             state->pcmQueueLabel, state->pcmQueueHelp, state->pcmQueueCombo,
             state->forceHdr10Check, state->forceHdr10Help,
             state->windowSnapCheck, state->saveLogCheck,
             state->showConsoleCheck}) {
        SetSettingsControlVisible(control, visible);
    }
    UpdateScalingControlVisibility(state);
    UpdateWindowBehaviorVisibility(state);
    SetWindowTextW(state->advancedToggle,
                   UI_TEXT(visible ? L"⌄ 고급 설정 숨기기"
                                   : L"▸ 고급 설정"));
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
                     target == state->forceHdr10Help);
}

enum class SettingsHelpTopic {
    Drift,
    PcmQueue,
    Presentation,
    VolumeBoost,
    ForceHdr10,
};

static const wchar_t* SettingsHelpText(SettingsHelpTopic topic) {
    if (IsEnglishUi()) {
        switch (topic) {
        case SettingsHelpTopic::Drift:
            return L"Preventing audio tearing · deciding whether correction is needed\n\n"
                   L"The capture and output device clocks can run at slightly different rates. "
                   L"Automatic correction follows that difference with resampling to reduce "
                   L"dropouts or crackling during long sessions. Check the Tab OSD for 10–30 minutes.\n\n"
                   L"'Stable · correction unnecessary' or 'Rare errors · Off can be kept' means "
                   L"you can leave it Off when the audio is clean. If 'Repeated imbalance · "
                   L"correction recommended' continues, enable automatic resampling. Do not judge "
                   L"from errors immediately after startup.\n\n"
                   L"The resampler and PCM safety buffer are independent. 'Resampler correction "
                   L"limit approaching' indicates clock difference; 'Possible PCM buffer shortage' "
                   L"indicates a momentary lack of queued audio; 'Capture packet delay detected' "
                   L"indicates a late input callback. If the resampler is healthy but underruns "
                   L"continue, raise the PCM buffer target first.\n\n"
                   L"The imbalance ppm shown in the OSD is an estimate from accumulated underrun/"
                   L"overrun frames, not a direct hardware-clock measurement. Automatic correction "
                   L"adds a small amount of audio buffering and changes PCM samples.";
        case SettingsHelpTopic::PcmQueue:
            return L"PCM buffer target\n\n"
                   L"The amount of captured audio kept inside the application before playback.\n"
                   L"10 ms is minimum latency, 15 ms is the low-latency target, 20 ms is the stable "
                   L"recommendation, and 30 ms prioritizes stability.\n\n"
                   L"Higher values absorb more scheduling jitter but add the same amount of audio "
                   L"latency. This is independent of the WASAPI output buffer and clock-drift correction.";
        case SettingsHelpTopic::Presentation:
            return L"Presentation mode\n\n"
                   L"Immediate presents the newest frame without waiting for VSync. This minimizes "
                   L"display latency but can show tearing.\n\n"
                   L"VSync follows the monitor refresh and reduces tearing, but may wait for the next "
                   L"refresh interval and can have different pacing on mixed-refresh monitors.";
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
        }
    }
    switch (topic) {
    case SettingsHelpTopic::Drift:
        return L"소리 찢어짐 방지 · 보정 필요 확인\n\n"
               L"캡처 장치와 출력 장치의 클록 차이를 자동 리샘플링으로 보정합니다. "
               L"Tab OSD를 10~30분 확인하세요.\n\n"
               L"'안정 · 보정 불필요' 또는 '드문 오류 · 끔 유지 가능'이면 소리에 문제가 "
               L"없는 한 끔을 유지해도 됩니다. '반복 불균형 · 보정 권장'이 계속 보이면 "
               L"자동 리샘플링을 권장합니다. 시작 직후 오류만으로 판단하지 마세요.\n\n"
               L"리샘플러와 PCM 안전 대기량은 서로 독립입니다. '리샘플러 보정 한계 접근'은 "
               L"클록 차이, 'PCM 버퍼 부족 가능'은 순간 버퍼 여유 부족, '캡처 패킷 지연 감지'는 "
               L"입력 콜백 지연을 뜻합니다. 리샘플러가 정상인데 underrun이 나면 PCM 버퍼 "
               L"목표를 먼저 높이세요.\n\n"
               L"OSD의 불균형 ppm은 누적 underrun/overrun으로 계산한 참고값이며 실제 하드웨어 "
               L"클록을 직접 측정한 값은 아닙니다. 자동 보정은 작은 오디오 대기량을 추가하고 "
               L"PCM 샘플을 변경합니다.";
    case SettingsHelpTopic::PcmQueue:
        return L"PCM 버퍼 목표 안내\n\n"
               L"캡처 오디오를 재생 전에 확보하는 프로그램 내부 대기량입니다.\n"
               L"10ms는 최저 지연, 15ms는 저지연 목표, 20ms는 안정 권장, 30ms는 안정성 우선 "
               L"설정입니다.\n\n"
               L"값을 높이면 순간적인 입력 지연을 흡수할 여유가 커지지만, 그만큼 오디오 지연이 "
               L"늘어납니다. WASAPI 출력 버퍼와 클록 드리프트 보정과는 독립적으로 조정됩니다.";
    case SettingsHelpTopic::Presentation:
        return L"화면 표시 방식 안내\n\n"
               L"저지연: VSync 대기 없이 최신 프레임을 즉시 표시합니다. 표시 지연을 줄이는 대신 "
               L"화면 경계가 맞지 않을 때 찢어짐이 보일 수 있습니다.\n\n"
               L"VSync: 모니터 주기에 맞춰 표시해 찢어짐을 줄입니다. 대신 다음 표시 주기까지 "
               L"기다릴 수 있어 지연이 늘어날 수 있고, 주사율이 다른 모니터에서는 프레임 페이싱이 "
               L"달라질 수 있습니다.";
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
    }
    return L"";
}

static bool SettingsUsesSharedMode(const SettingsDialogState* state) {
    return state && SendMessageW(state->audioCombo, CB_GETCURSEL, 0, 0) == 0;
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

    size_t selected = 0;
    for (size_t i = 0; i < ARRAYSIZE(kWasapiBufferOptionsMs); ++i) {
        wchar_t label[64]{};
        if (kWasapiBufferOptionsMs[i] == kRecommendedWasapiBufferMs) {
            swprintf_s(label, UI_TEXT(L"%d ms (권장)"), kWasapiBufferOptionsMs[i]);
        } else {
            swprintf_s(label, L"%d ms", kWasapiBufferOptionsMs[i]);
        }
        const LRESULT index = SendMessageW(
            state->bufferCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label));
        SendMessageW(state->bufferCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(index), kWasapiBufferOptionsMs[i]);
        if (kWasapiBufferOptionsMs[i] == state->selectedBufferMs) selected = i;
    }
    SendMessageW(state->bufferCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selected), 0);
}

static void UpdateAudioClient3Status(SettingsDialogState* state) {
    if (!state || !state->audioStatus) return;
    if (!state->probeReady.load(std::memory_order_acquire)) {
        SetWindowTextW(state->audioStatus, UI_TEXT(L"Shared 저지연 지원 확인 중…"));
        return;
    }

    wchar_t status[200]{};
    if (state->probe.supported) {
        swprintf_s(status,
                   UI_TEXT(L"IAudioClient3 지원됨 · Shared %.2f–%.2f ms · 검사 %.1f ms"),
                   1000.0 * state->probe.minimumFrames / kSampleRate,
                   1000.0 * state->probe.maximumFrames / kSampleRate,
                   state->probe.probeMilliseconds);
    } else {
        swprintf_s(status,
                   UI_TEXT(L"IAudioClient3 사용 불가 · 기본 Shared로 자동 전환 (0x%08X)"),
                   static_cast<unsigned>(state->probe.result));
    }
    SetWindowTextW(state->audioStatus, status);
}

static std::wstring SelectedAudioEndpointId(
    const SettingsDialogState* state) {
    if (!state || !state->audioOutputCombo) return {};
    const LRESULT index = SendMessageW(
        state->audioOutputCombo, CB_GETCURSEL, 0, 0);
    if (index <= 0 || static_cast<size_t>(index - 1) >=
                          state->audioEndpoints.size()) return {};
    return state->audioEndpoints[static_cast<size_t>(index - 1)].id;
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
    const InternalCaptureAudioState probeState =
        state->captureAudioProbeReady.load(std::memory_order_acquire)
            ? state->captureAudioProbe.state
            : InternalCaptureAudioState::Checking;

    SetWindowTextW(state->captureAudioDeviceLabel,
                   UI_TEXT(L"캡처 오디오 장치"));
    if (explicitSeparateDevice ||
        probeState == InternalCaptureAudioState::SeparateDeviceNeeded ||
        probeState == InternalCaptureAudioState::Unknown) {
        SetSettingsControlVisible(state->captureAudioDeviceCombo, true);
        SetSettingsControlVisible(state->captureAudioStatus, false);
        return;
    }

    SetSettingsControlVisible(state->captureAudioDeviceCombo, false);
    SetSettingsControlVisible(state->captureAudioStatus, true);
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

static bool HasRealtimeRawVideoFormat(
    const std::vector<PixelFormatSupport>& formats) {
    return std::any_of(formats.begin(), formats.end(),
                       [](const PixelFormatSupport& support) {
                           const bool raw =
                               support.format == VideoPixelFormat::Nv12 ||
                               support.format == VideoPixelFormat::Yuy2;
                           return raw && support.selectedFps >= 30;
                       });
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
        message = UI_TEXT(L"자동 인식: ");
        // Do not hide MJPEG merely because a device advertises an unusably
        // slow raw mode. A raw mode must reach at least 30 fps before it takes
        // exclusive priority in the normal selector.
        const bool realtimeRawAvailable = HasRealtimeRawVideoFormat(
            state->pixelFormats);
        bool firstFormat = true;
        for (const auto format : {VideoPixelFormat::Nv12,
                                  VideoPixelFormat::Yuy2,
                                  VideoPixelFormat::P010,
                                  VideoPixelFormat::Mjpeg}) {
            if (realtimeRawAvailable && IsCompressedVideoFormat(format)) {
                continue;
            }
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
            if (!firstFormat) message += L"  ·  ";
            message += PixelFormatName(format);
            message += L" ";
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
        if (selectedFormat != VideoPixelFormat::Auto &&
            support.format != selectedFormat) continue;
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
    const bool realtimeRawAvailable = HasRealtimeRawVideoFormat(
        state->pixelFormats);
    for (const auto format : {VideoPixelFormat::Nv12,
                              VideoPixelFormat::Yuy2,
                              VideoPixelFormat::P010,
                              VideoPixelFormat::Mjpeg}) {
        if (realtimeRawAvailable && IsCompressedVideoFormat(format)) continue;
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
        const LRESULT volumeHudIndex = SendMessageW(
            state->volumeHudCombo, CB_GETCURSEL, 0, 0);
        const LRESULT driftIndex = SendMessageW(
            state->driftCombo, CB_GETCURSEL, 0, 0);
        const LRESULT pcmQueueIndex = SendMessageW(
            state->pcmQueueCombo, CB_GETCURSEL, 0, 0);
        const LRESULT pixelFormatIndex = SendMessageW(
            state->pixelFormatCombo, CB_GETCURSEL, 0, 0);
        const LRESULT frameRateIndex = SendMessageW(
            state->frameRateCombo, CB_GETCURSEL, 0, 0);
        const LRESULT languageIndex = SendMessageW(
            state->languageCombo, CB_GETCURSEL, 0, 0);
        if (languageIndex >= 0 && languageIndex <= 2) {
            g_settings.uiLanguage = static_cast<UiLanguage>(languageIndex);
        }
        if (audioIndex == 1) {
            g_settings.audioMode = AudioMode::WasapiExclusive;
        } else {
            g_settings.audioMode = AudioMode::WasapiShared;
        }
        if (videoIndex >= 0 && videoIndex < static_cast<LRESULT>(ARRAYSIZE(kVideoPresets))) {
            g_settings.videoPreset = kVideoPresets[videoIndex].preset;
        }
        g_settings.presentationMode = presentationIndex == 1
                                          ? PresentationMode::VSync
                                          : PresentationMode::AllowTearing;
        g_settings.scalingMode = scalingIndex == 1
            ? ScalingMode::Sharp : ScalingMode::Smooth;
        g_settings.wasapiBufferMs = state->selectedBufferMs;
        if (volumeHudIndex >= 0 && volumeHudIndex <= 3) {
            g_settings.volumeHudPosition =
                static_cast<VolumeHudPosition>(volumeHudIndex);
        }
        g_settings.driftCorrection = driftIndex == 1
                                         ? DriftCorrectionMode::Resample
                                         : DriftCorrectionMode::Off;
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
        g_settings.saveLog = SendMessageW(
            state->saveLogCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.showDiagnosticConsole = SendMessageW(
            state->showConsoleCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.skipStartupSettings = SendMessageW(
            state->skipStartupCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.muteWhenBackground = SendMessageW(
            state->muteBackgroundCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.audioOnly = SendMessageW(
            state->audioOnlyCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.forceHdr10 = SendMessageW(
            state->forceHdr10Check, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
        SaveSettings();
    }

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
    case WM_CREATE: {
        const HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;
        auto makeLabel = [&](const wchar_t* text, int x, int y) {
            return CreateWindowExW(0, L"STATIC", text,
                                   WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                   x, y, 160, 24, hwnd, nullptr, instance, nullptr);
        };

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
                     reinterpret_cast<LPARAM>(
                         UI_TEXT(L"WASAPI Exclusive (지연 최소화 · 장치 독점)")));
        SendMessageW(state->audioCombo, CB_SETCURSEL,
                     g_settings.audioMode == AudioMode::WasapiExclusive ? 1 : 0, 0);

        state->audioOutputLabel = makeLabel(UI_TEXT(L"WASAPI 출력 장치"), 24, 68);
        state->audioOutputCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            150, 64, 250, 220, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_AUDIO_OUTPUT)),
            instance, nullptr);
        SendMessageW(state->audioOutputCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         UI_TEXT(L"Windows 기본 출력 장치 따라가기 (권장)")));
        LRESULT selectedAudioEndpoint = 0;
        for (size_t i = 0; i < state->audioEndpoints.size(); ++i) {
            std::wstring label = state->audioEndpoints[i].name;
            if (state->audioEndpoints[i].isDefault) label += UI_TEXT(L" (현재 기본)");
            SendMessageW(state->audioOutputCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(label.c_str()));
            if (state->audioEndpoints[i].id ==
                g_settings.audioOutputDeviceId) {
                selectedAudioEndpoint = static_cast<LRESULT>(i + 1);
            }
        }
        SendMessageW(state->audioOutputCombo, CB_SETCURSEL,
                     selectedAudioEndpoint, 0);

        state->selectedBufferMs = g_settings.wasapiBufferMs;
        state->selectedSharedPeriodFrames =
            g_settings.wasapiSharedPeriodFrames;

        state->bufferLabel = makeLabel(UI_TEXT(L"WASAPI 출력 버퍼"), 24, 68);
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
                BS_PUSHBUTTON,
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
                         UI_TEXT(L"자동 리샘플링 (장시간 안정성 권장)")));
        SendMessageW(
            state->driftCombo, CB_SETCURSEL,
            g_settings.driftCorrection == DriftCorrectionMode::Resample
                ? 1 : 0,
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
        state->advancedToggle = CreateWindowExW(
            0, L"BUTTON", UI_TEXT(L"▸ 고급 설정"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            24, 520, 190, 28, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_ADVANCED_TOGGLE)),
            instance, nullptr);
        state->versionWatermark = CreateWindowExW(
            0, L"STATIC", kAppVersionLabel,
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            24, 596, 260, 20, hwnd, nullptr, instance, nullptr);
        EnableWindow(state->versionWatermark, FALSE);

        state->presentationLabel = makeLabel(UI_TEXT(L"화면 표시 방식"), 24, 274);
        state->presentationCombo = CreateWindowExW(
            0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            180, 270, 210, 120, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(IDC_SETTINGS_PRESENTATION)),
            instance, nullptr);
        SendMessageW(state->presentationCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(UI_TEXT(L"저지연")));
        SendMessageW(state->presentationCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"VSync"));
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
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            430, 234, 365, 24, hwnd, nullptr, instance, nullptr);
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
        UpdateAdvancedControlVisibility(state);
        RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                         RDW_UPDATENOW);

        const std::wstring endpointId = SelectedAudioEndpointId(state);
        state->probeThread = std::thread([state, hwnd, endpointId]() {
            state->probe = ProbeAudioClient3Support(endpointId);
            state->probeReady.store(true, std::memory_order_release);
            PostMessageW(hwnd, WM_AUDIOCLIENT3_PROBE_COMPLETE, 0, 0);
        });
        StartCaptureAudioProbe(state, hwnd);
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
        UpdateAudioClient3Status(state);
        if (SettingsUsesSharedMode(state)) {
            RememberCurrentBufferChoice(state);
            PopulateSettingsBufferCombo(state);
        }
        return 0;

    case WM_CAPTURE_AUDIO_PROBE_COMPLETE:
        if (state) {
            EnableWindow(state->captureDeviceCombo, TRUE);
            UpdateCaptureAudioSelectionUi(state);
        }
        return 0;

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

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SETTINGS_AUDIO &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            RememberCurrentBufferChoice(state);
            PopulateSettingsBufferCombo(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_AUDIO_OUTPUT &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            if (state->probeThread.joinable()) state->probeThread.join();
            state->probeReady.store(false, std::memory_order_release);
            UpdateAudioClient3Status(state);
            const std::wstring endpointId = SelectedAudioEndpointId(state);
            state->probeThread = std::thread([state, hwnd, endpointId]() {
                state->probe = ProbeAudioClient3Support(endpointId);
                state->probeReady.store(true, std::memory_order_release);
                PostMessageW(hwnd, WM_AUDIOCLIENT3_PROBE_COMPLETE, 0, 0);
            });
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_CAPTURE_DEVICE &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            PopulatePixelFormatCombo(state);
            StartCaptureAudioProbe(state, hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_VIDEO &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            PopulatePixelFormatCombo(state);
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
        if (LOWORD(wParam) == IDC_SETTINGS_ADVANCED_TOGGLE &&
            HIWORD(wParam) == BN_CLICKED) {
            state->showAdvanced = !state->showAdvanced;
            const UINT dpi = GetDpiForWindow(hwnd);
            LayoutSettingsControls(state, dpi);
            UpdateAdvancedControlVisibility(state);
            const SIZE outer = SettingsDialogOuterSize(hwnd, dpi, state);
            SetWindowPos(hwnd, nullptr, 0, 0, outer.cx, outer.cy,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateCaptureAudioSelectionUi(state);
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                             RDW_UPDATENOW);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PIXEL_FORMAT &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            PopulateFrameRateCombo(state);
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
    state.audioEndpoints = EnumerateAudioEndpoints();
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
    const int frameWidth = static_cast<int>(frame.right - frame.left);
    const int frameHeight = static_cast<int>(frame.bottom - frame.top);
    constexpr int kMinimumClientWidth = 320;
    constexpr int kMinimumClientHeight = 180;
    const int minimumOuterWidth = frameWidth + kMinimumClientWidth;
    const int minimumOuterHeight = frameHeight + kMinimumClientHeight;
    const int proposedWidth = (std::max)(
        minimumOuterWidth,
        static_cast<int>(sizingRect.right - sizingRect.left));
    const int proposedHeight = (std::max)(
        minimumOuterHeight,
        static_cast<int>(sizingRect.bottom - sizingRect.top));

    auto heightFromWidth = [&](int outerWidth) {
        const int clientWidth =
            (std::max)(kMinimumClientWidth, outerWidth - frameWidth);
        return frameHeight + MulDiv(clientWidth, video.height, video.width);
    };
    auto widthFromHeight = [&](int outerHeight) {
        const int clientHeight =
            (std::max)(kMinimumClientHeight, outerHeight - frameHeight);
        return frameWidth + MulDiv(clientHeight, video.width, video.height);
    };

    const bool verticalEdge =
        sizingEdge == WMSZ_TOP || sizingEdge == WMSZ_BOTTOM;
    const bool horizontalEdge =
        sizingEdge == WMSZ_LEFT || sizingEdge == WMSZ_RIGHT;
    int outerWidth = proposedWidth;
    int outerHeight = proposedHeight;
    if (verticalEdge) {
        outerWidth = widthFromHeight(proposedHeight);
    } else if (horizontalEdge) {
        outerHeight = heightFromWidth(proposedWidth);
    } else {
        const int widthDrivenHeight = heightFromWidth(proposedWidth);
        const int heightDrivenWidth = widthFromHeight(proposedHeight);
        if (std::abs(widthDrivenHeight - proposedHeight) <=
            std::abs(heightDrivenWidth - proposedWidth)) {
            outerHeight = widthDrivenHeight;
        } else {
            outerWidth = heightDrivenWidth;
        }
    }

    const LONG left = sizingRect.left;
    const LONG top = sizingRect.top;
    const LONG right = sizingRect.right;
    const LONG bottom = sizingRect.bottom;
    switch (sizingEdge) {
    case WMSZ_LEFT:
        sizingRect.left = right - outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    case WMSZ_RIGHT:
        sizingRect.right = left + outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    case WMSZ_TOP:
        sizingRect.top = bottom - outerHeight;
        sizingRect.right = left + outerWidth;
        break;
    case WMSZ_BOTTOM:
        sizingRect.bottom = top + outerHeight;
        sizingRect.right = left + outerWidth;
        break;
    case WMSZ_TOPLEFT:
        sizingRect.left = right - outerWidth;
        sizingRect.top = bottom - outerHeight;
        break;
    case WMSZ_TOPRIGHT:
        sizingRect.right = left + outerWidth;
        sizingRect.top = bottom - outerHeight;
        break;
    case WMSZ_BOTTOMLEFT:
        sizingRect.left = right - outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    case WMSZ_BOTTOMRIGHT:
        sizingRect.right = left + outerWidth;
        sizingRect.bottom = top + outerHeight;
        break;
    default:
        break;
    }
}

static LRESULT BorderlessHitTest(HWND hwnd, LPARAM lParam) {
    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return HTCLIENT;
    const POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (!g_settings.pixelPerfect) {
        const UINT dpi = GetDpiForWindow(hwnd);
        const int grip = (std::max)(6, MulDiv(8, dpi, 96));
        const bool left = cursor.x >= windowRect.left &&
                          cursor.x < windowRect.left + grip;
        const bool right = cursor.x < windowRect.right &&
                           cursor.x >= windowRect.right - grip;
        const bool top = cursor.y >= windowRect.top &&
                         cursor.y < windowRect.top + grip;
        const bool bottom = cursor.y < windowRect.bottom &&
                            cursor.y >= windowRect.bottom - grip;
        if (left && top) return HTTOPLEFT;
        if (right && top) return HTTOPRIGHT;
        if (left && bottom) return HTBOTTOMLEFT;
        if (right && bottom) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    return HTCAPTION;
}

static LRESULT CALLBACK VideoHostSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR) {
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
        ShowCursor(FALSE);
        g_autoFullscreen = automaticStartup;
    } else {
        g_fullscreen.store(false, std::memory_order_release);
        SetWindowLongPtrW(hwnd, GWL_STYLE, g_prevStyle);
        SetWindowPlacement(hwnd, &g_prevPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
        ShowCursor(TRUE);
        g_autoFullscreen = false;
    }
    if (updateOutput) {
        g_outputConfigurationGeneration.fetch_add(
            1, std::memory_order_acq_rel);
    }
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
        g_outputConfigurationGeneration.fetch_add(
            1, std::memory_order_acq_rel);
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

struct AudioPatternStats {
    size_t recentEvents = 0;
    size_t recentUnderruns = 0;
    size_t maxConsecutiveUnderruns = 0;
    uint64_t lastEventMs = 0;
};

static AudioPatternStats GetAudioPatternStats(uint64_t nowMs,
                                              UINT32 packetFrames) {
    constexpr uint64_t kPatternWindowMs = 5 * 60 * 1000;
    const uint64_t windowStartMs = nowMs > kPatternWindowMs
        ? nowMs - kPatternWindowMs : 0;
    const uint64_t packetPeriodMs = packetFrames
        ? (1000ull * packetFrames + kSampleRate - 1) / kSampleRate : 10;
    // Two packet periods apart still belong to the same short burst. This is
    // only used for display classification, not for underrun accounting.
    const uint64_t burstGapMs = (std::max)(20ull, packetPeriodMs * 2);

    AudioPatternStats stats;
    size_t currentBurst = 0;
    uint64_t previousUnderrunMs = 0;
    std::lock_guard<std::mutex> lock(g_audioErrorHistoryMutex);
    for (const AudioErrorEvent& event : g_audioErrorHistory) {
        if (event.timestampMs < windowStartMs) continue;
        ++stats.recentEvents;
        stats.lastEventMs = (std::max)(stats.lastEventMs, event.timestampMs);
        if (event.kind != AudioErrorKind::Underrun) {
            currentBurst = 0;
            previousUnderrunMs = 0;
            continue;
        }
        ++stats.recentUnderruns;
        if (previousUnderrunMs != 0 &&
            event.timestampMs >= previousUnderrunMs &&
            event.timestampMs - previousUnderrunMs <= burstGapMs) {
            ++currentBurst;
        } else {
            currentBurst = 1;
        }
        stats.maxConsecutiveUnderruns =
            (std::max)(stats.maxConsecutiveUnderruns, currentBurst);
        previousUnderrunMs = event.timestampMs;
    }
    return stats;
}

static std::wstring BuildRuntimeOsdText(int outputWidth, int outputHeight,
                                        size_t outputNameLimit,
                                        size_t captureNameLimit) {
    const auto& preset = CurrentVideoPreset();
    auto compactName = [](const std::wstring& value, size_t maximum) {
        if (value.size() <= maximum) return value;
        return value.substr(0, maximum > 1 ? maximum - 1 : 0) + L"…";
    };
    auto compactEndpointName = [](const std::wstring& value, size_t maximum) {
        if (value.size() <= maximum) return value;
        if (maximum == 0) return std::wstring{};
        if (maximum <= 2) return value.substr(0, maximum - 1) + L"…";
        // Keep both the device prefix and the endpoint/status suffix visible.
        const size_t available = maximum - 1; // one slot for the ellipsis
        const size_t prefix = (available + 1) / 2;
        const size_t suffix = available - prefix;
        return value.substr(0, prefix) + L"…" +
               value.substr(value.size() - suffix);
    };
    const std::wstring captureName = compactName(g_activeCaptureDeviceName,
                                                 captureNameLimit);
    const std::wstring outputName = compactEndpointName(
        ActiveAudioOutputName(), outputNameLimit);
    const VideoPixelFormat activeFormat = static_cast<VideoPixelFormat>(
        g_activePixelFormat.load(std::memory_order_acquire));
    const bool compressedVideo = IsCompressedVideoFormat(activeFormat);
    const bool p010Video = activeFormat == VideoPixelFormat::P010;
    const bool hdrVideo = p010Video &&
        g_hdrOutputActive.load(std::memory_order_acquire);
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
    const wchar_t* qualityText = hdrVideo
        ? L"BT.2020 · PQ · HDR10 prototype"
        : p010Video
        ? L"P010 · HDR output unavailable"
        : compressedVideo
        ? (IsEnglishUi()
            ? L"Compressed input · Media Foundation decode · NV12 output"
            : L"실험적 압축 입력 · Media Foundation 디코드 · NV12 D3D11 출력")
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
    const uint64_t overrunEvents = g_ring.overruns();
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
    const AudioPatternStats patternStats =
        GetAudioPatternStats(nowMs, capturePacketFrames);
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
        if (g_settings.driftCorrection ==
            DriftCorrectionMode::Resample) {
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

    const wchar_t* scaleText =
        g_settings.pixelPerfect && g_settings.relativeWindowSize
            ? UI_TEXT(L"Pixel-perfect 시작 · Monitor-relative 이동")
            : g_settings.pixelPerfect
                  ? UI_TEXT(L"Pixel-perfect (고정 크기)")
                  : g_settings.relativeWindowSize
                        ? L"Scaled · Monitor-relative" : UI_TEXT(L"Scaled (비율 고정)");
    const wchar_t* osdFormat = IsEnglishUi()
        ? L"Capture diagnostics                              [Tab close]\n"
          L"Path          %s · %s\n"
          L"Input         %d x %d @ %d fps · %s %s %s\n"
          L"Video quality %s\n"
          L"Display       %d x %d · %s · Flip-discard · %s\n"
          L"Actual FPS    Input %.1f · Present %.1f\n"
          L"App latency   %s  (not total HDMI latency)\n"
          L"Frames        Input %llu · Output %llu · Replaced %llu\n"
          L"Audio output  WASAPI %s\n"
          L"Output device %s\n"
          L"Device buffer %.2f ms · queued %.2f ms · input packet %.2f ms · period %.2f ms\n"
          L"Clock drift   %s · applied %+d ppm · %s\n"
          L"App PCM queue current %.2f ms · target %.2f ms · observed min %.2f ms\n"
          L"Buffer diagnosis %s\n"
          L"Volume        %d%% · %s\n"
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
          L"오디오 출력   WASAPI %s\n"
          L"출력 장치     %s\n"
          L"출력 버퍼(장치) %.2f ms · 현재 대기 %.2f ms · 입력 패킷 %.2f ms · 주기 %.2f ms\n"
          L"클록 보정     %s · 적용 %+d ppm · %s\n"
          L"앱 PCM 버퍼(대기) 현재 %.2f ms · 목표 %.2f ms · 관측 최저 %.2f ms\n"
          L"버퍼 진단     %s\n"
          L"음량          %d%% · %s\n"
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
            ? L"Exclusive" : L"Shared",
        outputName.c_str(),
        1000.0 * audioFrames / kSampleRate,
        1000.0 * paddingFrames / kSampleRate,
        1000.0 * capturePacketFrames / kSampleRate,
        captureIntervalUs / 1000.0,
        g_settings.driftCorrection == DriftCorrectionMode::Resample
            ? UI_TEXT(L"자동 리샘플링") : UI_TEXT(L"끔 (원본 PCM)"),
        activeCorrectionPpm, clockDiagnosis,
        1000.0 * queuedFrames / kSampleRate,
        1000.0 * queueTargetFrames / kSampleRate,
        1000.0 * minimumPreRenderFrames / kSampleRate,
        queueDiagnosis,
        logicalVolume, volumeProcessing,
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
            if (g_manualResizeInProgress) {
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
                if (g_fullscreen) ShowCursor(TRUE);
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        break;

    case WM_MOUSEWHEEL:
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

    case WM_CLOSE:
        PersistWindowPosition(hwnd);
        g_windowPositionPersisted = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (!g_windowPositionPersisted) PersistWindowPosition(hwnd);
        g_settings.volumePercent =
            g_volumePercent.load(std::memory_order_acquire);
        g_settings.leftVolumePercent =
            g_leftVolumePercent.load(std::memory_order_acquire);
        g_settings.rightVolumePercent =
            g_rightVolumePercent.load(std::memory_order_acquire);
        if (!g_suppressSettingsSave) SaveSettings();
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

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR commandLine, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX commonControls{
        sizeof(commonControls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&commonControls);

    LoadSettings();
    const bool audioOnlySmokeTest = commandLine &&
        wcsstr(commandLine, L"--smoke-test-audio-only") != nullptr;
    if (audioOnlySmokeTest) {
        g_settings.audioOnly = true;
        g_settings.skipStartupSettings = true;
    }
    const bool smokeTest = commandLine &&
        (wcsstr(commandLine, L"--smoke-test") != nullptr ||
         audioOnlySmokeTest);
    g_suppressSettingsSave = smokeTest;
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
    const bool showStartupSettings = !smokeTest &&
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
    if (!smokeTest) OpenSavedLog();
    SetActiveAudioOutputName(ConfiguredAudioEndpointName(
        g_settings.audioOutputDeviceId));

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
            ? L"WASAPI Exclusive" : L"WASAPI Shared";
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

    MSG m{};
    while (g_running.load() && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_MOUSEWHEEL &&
            AdjustVolumeFromWheel(hwnd, m.wParam, m.lParam)) {
            continue;
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

    if (renderThread.joinable()) renderThread.join();
    if (unifiedCaptureThread.joinable()) unifiedCaptureThread.join();
    if (smokeTestStopper.joinable()) smokeTestStopper.join();
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
                g_ring.overruns()),
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
