/*
 * LLCV
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
#include "audio/RecoveryPolicy.h"
#include "audio/QueueDriftController.h"
#include "audio/RealtimeLogQueue.h"
#include "audio/CaptureAudioFormat.h"
#include "audio/PcmPipeline.h"
#include "audio/WasapiOutput.h"
#include "capture/DirectShowDevices.h"
#include "capture/AudioSampleGrabber.h"
#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
#include "capture/AudioInputRecovery.h"
#endif
#include "capture/DirectShowGraphResources.h"
#include "capture/LatestVideoSample.h"
#include "diagnostics/CaptureFrameRateMonitor.h"
#ifdef LLCV_RECORDING
#include "recording/Recorder.h"
#include "recording/RecordingGpuIndicator.h"
#include "recording/EncoderProbeProcess.h"
#endif
#include "capture/HardwareToneMapping.h"
#include "diagnostics/Logger.h"
#include "diagnostics/AudioErrorHistory.h"
#include "settings/AppSettings.h"
#include "settings/SmokeVideoOptions.h"
#include "power/PlaybackPowerRequest.h"
#include "power/PlaybackPowerEvents.h"
#include "power/SuspendResumeNotification.h"
#include "settings/SettingsStore.h"
#include "screenshot/ScreenshotService.h"
#include "ui/ViewerHelpWindow.h"
#include "ui/AudioOsdLayout.h"
#include "ui/OverlayStyle.h"
#include "ui/AudioOnlyView.h"
#include "ui/SpeakerIcon.h"
#include "ui/PresentationModeUi.h"
#include "video/VideoClearPolicy.h"
#include "video/UyvyRepacker.h"
#include "video/Rgb24Repacker.h"
#include "video/VideoPlacement.h"
#include "video/ExclusivePresentationState.h"
#include "ui/SettingsView.h"
#include "ui/SettingsTheme.h"
#include <dwmapi.h>
#include "ui/SettingsModeCache.h"
#ifdef LLCV_RECORDING
#include "recording/RecordingSettingsView.h"
#endif
#include "ui/SettingsDialogControls.h"
#include "ui/UiText.h"
#include "video/OutputTransitionState.h"
#include "video/PresentationPolicy.h"
#include "ui/WindowGeometry.h"
#include "update/UpdateChecker.h"
#include "update/UpdateCheckTask.h"
#include "video/CaptureColorMetadata.h"
#include "video/DirectShowVideoFormat.h"
#include "video/MjpegDecoder.h"
#include "video/VideoColor.h"
#include "video/HdrPolicy.h"
#include "video/HdrOverlay.h"
#include "video/HdrDisplay.h"

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
#include <filesystem>
#include "ui/WindowCorners.h"

static llcv::screenshot::Service g_screenshots;
static llcv::viewer_help::Window g_viewerHelp; // Modeless, sectioned help; no capture-thread ownership.

#ifdef LLCV_HDR_FRAME_AUDIT
#include "diagnostics/HdrFrameAudit.h"
static std::atomic<bool> g_hdrFrameAuditRequested{false};
static std::wstring g_hdrFrameAuditDirectory;
#endif
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
#include "video/ScrgbPrototype.h"
static bool g_useScrgbPrototype = false;
#endif
#ifdef LLCV_VSR_FEATURE
#include "video/VsrExperiment.h"
#include "video/CmaaNisUpscaler.h"
#ifdef LLCV_SPATIAL_PROTOTYPE
#include "video/SpatialUpscaler.h"
static llcv::spatial::LiveTrace g_spatialTrace;
static bool g_spatialInteractivePreview = false;
#endif
// UI publishes intent only; the capture/render thread owns all D3D calls.
static std::atomic<llcv::vsr::Mode> g_vsrMode{llcv::vsr::Mode::Disabled};
// Single request value keeps enable/sharpness coherent across the UI/render owners.
static std::atomic<int> g_cmaaNisRequest{-1}; // See upscaling::Request; -1 selects VSR.
static std::atomic<UINT> g_vsrGpuVendor{0};
#endif

// -----------------------------------------------------------------------------
// User-tested settings.
// -----------------------------------------------------------------------------

constexpr wchar_t kCaptureName[] = L"AVerMedia HD Capture GC573 1";
constexpr wchar_t kAudioPinName[] = L"Audio";
constexpr wchar_t kVideoPinName[] = L"Video";

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBitsPerSample = 16;
#if defined(LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC)
constexpr wchar_t kAppVersionLabel[] = L"v2.1.9-audio-recovery-diagnostic";
#elif defined(LLCV_VSR_EXPERIMENT)
constexpr wchar_t kAppVersionLabel[] = L"v2.1.9-vsr-test";
#else
constexpr wchar_t kAppVersionLabel[] = L"v2.1.9";
#endif

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

using llcv::settings::VideoPresetInfo;

static constexpr VideoPresetInfo kDefaultVideoPreset =
    {VideoPreset::R1920x1080, 1920, 1080, 120, L"1920 x 1080"};
static constexpr VideoPresetInfo kVideoPresets[] = {
    {VideoPreset::R1280x720, 1280, 720, 60, L"1280 x 720"},
    kDefaultVideoPreset,
    {VideoPreset::R2560x1440, 2560, 1440, 120, L"2560 x 1440"},
    {VideoPreset::R3840x2160, 3840, 2160, 60, L"3840 x 2160"},
};

static AppSettings g_settings{};
namespace keys = llcv::shortcuts;
// User preferences remain UI-owned. A failed backend may change this session's
// renderer without racing a settings save or erasing the selected ASIO driver.
static std::atomic<AudioMode> g_activeAudioMode{AudioMode::WasapiShared};
static AudioMode ActiveAudioMode() {
    return g_activeAudioMode.load(std::memory_order_acquire);
}
static bool g_exclusiveStartupFallback = false;
static AudioMode g_exclusiveStartupRequestedMode = AudioMode::WasapiShared;
static std::atomic<bool> g_fullscreen{false};
// F5 may temporarily request source-size fullscreen without changing the
// saved display resolution. Read only when configuring output geometry.
static std::atomic<bool> g_fullscreenSourceSize{false};
static std::atomic<uint64_t> g_outputConfigurationGeneration{0};
// UI publishes intent. Only the render owner calls DXGI. Never wait for a
// render acknowledgement while dispatching window messages: DXGI can send
// synchronous messages back to this UI thread.
static std::atomic<uint64_t> g_presentationRevision{1};
static std::atomic<bool> g_presentationActive{false};
static std::atomic<bool> g_presentationMinimized{false};
static std::atomic<bool> g_presentationBlocked{false};
static std::atomic<bool> g_presentationClosing{false};
static std::atomic<bool> g_videoOutputThreadActive{false};
static std::atomic<bool> g_running{true};
enum class DeferredPresentationUi { None, Help, RecordingSettings, ClosePrompt };
static constexpr UINT WM_PRESENTATION_UI_READY = WM_APP + 97;
static std::atomic<DeferredPresentationUi> g_deferredPresentationUi{DeferredPresentationUi::None};
static std::atomic<UINT_PTR> g_deferredPresentationSerial{0};
static std::mutex g_deferredPresentationMutex;
static UINT_PTR g_presentationUiSerial = 0;
static bool g_presentationUiPending = false; // UI owner only
static MSG g_deferredHelpMessage{};
static unsigned g_deferredHelpModifiers = 0;
#ifdef LLCV_RECORDING
// UI-thread-owned; the window remains a blocker even after a nested prompt
// consumes the pending presentation request. Cleared at final HWND teardown.
static HWND g_recordingSettingsWindow = nullptr;
#endif
static bool ExclusiveOutputSelected() {
    return !g_settings.audioOnly && llcv::presentation::IsExclusive(g_settings.presentationMode);
}
static void NotifyPresentationChange() {
    if (ExclusiveOutputSelected()) g_presentationRevision.fetch_add(1,std::memory_order_release);
}
static HWND PresentationWindow(HWND host) {
    return ExclusiveOutputSelected() ? GetAncestor(host,GA_ROOT) : host;
}

static bool DeferPresentationUi(DeferredPresentationUi action, const MSG* help = nullptr,
                                unsigned modifiers = 0) {
    if (!ExclusiveOutputSelected() || !g_videoOutputThreadActive.load(std::memory_order_acquire))
        return false;
    if (!g_running.load(std::memory_order_acquire) || g_presentationClosing.load(std::memory_order_acquire))
        return true;
    if (g_presentationUiPending && action != DeferredPresentationUi::ClosePrompt) return true;
    g_presentationUiPending = true;
    if (help) { g_deferredHelpMessage = *help; g_deferredHelpModifiers = modifiers; }
    g_presentationBlocked.store(true, std::memory_order_release);
    {
        // Publish the ticket and action as one request. No DXGI/window call is
        // made under this lock, and normal frame submission never acquires it.
        std::lock_guard<std::mutex> lock(g_deferredPresentationMutex);
        g_deferredPresentationSerial.store(++g_presentationUiSerial, std::memory_order_release);
        g_deferredPresentationUi.store(action, std::memory_order_release);
    }
    NotifyPresentationChange();
    return true;
}

static DeferredPresentationUi TakeDeferredPresentationUi(UINT_PTR& serial) {
    std::lock_guard<std::mutex> lock(g_deferredPresentationMutex);
    serial = g_deferredPresentationSerial.load(std::memory_order_acquire);
    return g_deferredPresentationUi.exchange(DeferredPresentationUi::None, std::memory_order_acq_rel);
}

static void RefreshPresentationUiBlock() {
    if (!ExclusiveOutputSelected()) return;
    const bool blocked = g_presentationUiPending || g_viewerHelp.Handle() != nullptr
#ifdef LLCV_RECORDING
        || g_recordingSettingsWindow != nullptr
#endif
        ;
    if (g_presentationBlocked.exchange(blocked, std::memory_order_acq_rel) != blocked)
        NotifyPresentationChange();
}

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
    return llcv::ui_text::Translate(korean, IsEnglishUi());
}

#define UI_TEXT(text) UiText(text)

enum class ExclusiveEndpointState {
    Unknown,
    Testing,
    Supported,
    Unsupported,
    RetryRequired,
};

struct ExclusiveEndpointVerification {
    ExclusiveEndpointState state = ExclusiveEndpointState::Unknown;
    int recommendedBufferMs = 0;
    std::wstring summary;
};

struct ExclusiveEndpointProbeResult {
    size_t endpointIndex = 0;
    ExclusiveCompatibilityProbe probe;
};

// 500 ms ring capacity. The program deliberately does NOT wait for this much
// data; it is only headroom. Old data is dropped if the producer overruns.
constexpr size_t kRingFrames = 48000 / 2;

static HWND g_videoHost = nullptr;
static bool g_suppressSettingsSave = false;
static llcv::update::UpdateCheckTask g_updateCheckTask;
static std::atomic<bool> g_restartToSettings{false};
static std::atomic<uint64_t> g_videoCapturedFrames{0};
static std::atomic<uint64_t> g_videoPresentedFrames{0};
static std::atomic<uint64_t> g_videoReplacedFrames{0};
static std::atomic<int64_t> g_videoAppLatencyUs{-1};
static std::atomic<int> g_videoConfiguredFps{0};
static std::atomic<bool> g_videoTearing{false};
static std::atomic<bool> g_videoExclusiveActive{false};
static std::atomic<bool> g_directVideoActive{false};
static std::atomic<HRESULT> g_captureFailureHr{S_OK};
static std::atomic<bool> g_captureGraphStopped{false};
static std::atomic<bool> g_captureGraphCompleted{false};
static llcv::power::PlaybackPowerEvents g_playbackPowerEvents;
static llcv::power::SuspendResumeNotification g_suspendResumeNotification;
static std::atomic<bool> g_captureVideoRejected{false};
static std::atomic<UINT32> g_audioActualBufferFrames{0};
static std::atomic<UINT32> g_audioWasapiPaddingFrames{0};
static std::atomic<bool> g_asioAudioStarted{false};
static std::atomic<UINT32> g_audioCapturePacketFrames{0};
static std::atomic<int64_t> g_audioCaptureIntervalUs{0};
static std::atomic<UINT32> g_audioRingFrames{0};
static std::atomic<UINT32> g_audioResamplerFrames{0};
static std::atomic<int> g_audioResamplePpm{0};
static std::atomic<bool> g_audioResamplerActive{false};
static std::atomic<uint64_t> g_audioCaptureCallbacks{0};
static std::atomic<uint64_t> g_audioCaptureFrames{0};
static std::atomic<uint64_t> g_audioCaptureIntervalTotalUs{0};
static std::atomic<uint64_t> g_audioUnderrunFrames{0};
static std::atomic<uint64_t> g_sharedDeadlineSuspicions{0};
static std::atomic<uint64_t> g_sharedLastDeadlineMs{0};
static std::atomic<uint64_t> g_sharedLastOverdueUs{0};
static std::atomic<bool> g_sharedLastDeadlineDuringFill{false};
static std::atomic<uint64_t> g_sharedRebuffers{0};
static std::atomic<uint64_t> g_sharedRebufferSilenceFrames{0};
static std::atomic<bool> g_sharedRebuffering{false};
static std::atomic<bool> g_sharedCorrectionNotice{false};
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
// Session-only playback control. Capture samples and recording gain never use it.
static std::atomic<bool> g_playbackMuted{false};
static std::atomic<uint64_t> g_volumeHudUntilMs{0};
enum class TransientHudContent { Volume, OneToOne, OneToOneUnavailable,
    ScreenshotPending, ScreenshotBusy, ScreenshotSaved, ScreenshotCopied,
    ScreenshotClipboardFailed, ScreenshotFailed, ScreenshotTimeout
#ifdef LLCV_VSR_FEATURE
    , VsrPending, VsrOn, VsrOff, VsrUnavailable, VsrRejected, VsrFailed
    , CmaaNisPending, CmaaNisOn, CmaaNisOff, CmaaNisHdrBypass, CmaaNisScaleBypass, CmaaNisFailed
#endif
};
static void ShowTransientHud(TransientHudContent content);
static bool HandleViewerShortcut(HWND hwnd,WPARAM key,LPARAM flags,unsigned modifiers);
#ifdef LLCV_RECORDING
// UI-thread only. Startup/finalization remain asynchronous on the writer.
static bool g_recordingClosePending=false;
static bool g_recordingDrainPending=false;
static bool g_recordingClosePrompt=false;
static bool g_recordingShutdownBlocked=false;
static uint64_t g_recordingCloseWarningAt=0;
static constexpr UINT WM_RECORDING_CAPTURE_ENDED=WM_APP+94;
static std::optional<llcv::recording::Capabilities> g_recordingCapabilities;
static int g_requestedSettingsTab=-1;
static void UpdateViewerRecordingIndicator(const llcv::recording::Snapshot& snapshot) {
    if (ExclusiveOutputSelected()) {
        llcv::recording::PublishGpuIndicator(snapshot, IsEnglishUi(),
            g_settings.settingsLightTheme, GetDpiForWindow(PresentationWindow(g_videoHost)));
    } else {
        llcv::recording::UpdateRecordingIndicator(g_videoHost, snapshot);
    }
}

static bool HandleGpuRecordingMouse(HWND hwnd, UINT message, LPARAM parameter) {
    if (!ExclusiveOutputSelected()) return false;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return false;
    const UINT width = static_cast<UINT>((std::max)(0L, client.right));
    const UINT height = static_cast<UINT>((std::max)(0L, client.bottom));
    if (message == WM_LBUTTONUP)
        return llcv::recording::HandleGpuIndicatorClick(hwnd, width, height,
            GET_X_LPARAM(parameter), GET_Y_LPARAM(parameter));
    if (message == WM_SETCURSOR && LOWORD(parameter) == HTCLIENT) {
        POINT point{};
        if (GetCursorPos(&point) && ScreenToClient(hwnd, &point) &&
            llcv::recording::GpuIndicatorHitTest(width, height, point.x, point.y)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return true;
        }
    }
    return false;
}
#endif
#ifdef LLCV_VSR_FEATURE
static bool HandleVsrTestKey(WPARAM key, LPARAM flags, unsigned modifiers=keys::Modifiers()) {
    if (!keys::Match(g_settings.shortcuts,keys::Action::Vsr,static_cast<UINT>(key),modifiers)) return false;
    if ((flags & (LPARAM{1} << 30)) != 0 || g_settings.audioOnly) return true;
    g_settings.preferredUpscaler = llcv::settings::SelectedUpscaler(g_settings);
    if (g_settings.preferredUpscaler == llcv::settings::UpscalingMethod::CmaaNis) {
        g_settings.cmaaNisEnabled = !g_settings.cmaaNisEnabled;
        g_settings.vsrEnabled = false;
        ShowTransientHud(TransientHudContent::CmaaNisPending);
        g_cmaaNisRequest.store(llcv::upscaling::Request(g_settings.cmaaNisEnabled,
            g_settings.nisSharpnessPercent), std::memory_order_release);
        return true;
    }
    if (!llcv::vsr::NvidiaAdapter(g_vsrGpuVendor.load(std::memory_order_acquire))) {
        ShowTransientHud(TransientHudContent::VsrUnavailable);
        return true;
    }
    const auto current = g_vsrMode.load(std::memory_order_acquire);
    g_settings.vsrEnabled = current != llcv::vsr::Mode::On;
    g_settings.cmaaNisEnabled = false;
    g_cmaaNisRequest.store(-1,std::memory_order_release);
    // Publish pending before the request, so a fast render-thread acknowledgement
    // cannot be overwritten by a late UI "pending" status.
    ShowTransientHud(TransientHudContent::VsrPending);
    g_vsrMode.store(current == llcv::vsr::Mode::On ? llcv::vsr::Mode::Off
                                                 : llcv::vsr::Mode::On,
                    std::memory_order_release);
    return true;
}
#endif
static std::atomic<TransientHudContent> g_transientHudContent{
    TransientHudContent::Volume};
static std::atomic<uint64_t> g_overlayGeneration{1};
static std::atomic<uint64_t> g_overlayRenderedFrames{0};
static std::atomic<double> g_osdInputFps{0.0};
static std::atomic<double> g_osdPresentFps{0.0};
static std::atomic<bool> g_captureFrameRateOverdelivery{false};
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
static std::atomic<int> g_hdrDisplayState{-1};
static std::atomic<float> g_hdrUiWhiteNits{203.0f};
static std::atomic<const wchar_t*> g_hdrFailureDetail{nullptr};

static void RefreshHdrDisplayStatus(HWND hwnd);
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
static llcv::audio::RealtimeLogQueue g_audioDiagnosticMessages;

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

static void RefreshHdrDisplayStatus(HWND hwnd) {
    const auto state = llcv::hdr::QueryDisplay(hwnd);
    const int previous = g_hdrDisplayState.exchange(state.hdr);
    const float previousWhite = g_hdrUiWhiteNits.exchange(state.uiWhiteNits);
    if (previous != state.hdr || previousWhite != state.uiWhiteNits) {
        fwprintf(stderr, L"[hdr] display=%s state=%s bits=%u UI-white=%.1f nits (%s); "
                         L"PQ video luminance unchanged.\n",
                 state.name, state.hdr == 1 ? L"HDR" : state.hdr == 0 ? L"SDR" : L"unknown",
                 state.bits, state.uiWhiteNits, state.systemWhite ? L"Windows" : L"reference fallback");
        if (state.hdr != 1)
            fwprintf(stderr, L"[hdr] HDR display not confirmed. Enable Windows HDR on the viewing monitor; "
                             L"PQ swapchain support alone does not confirm HDR display or passthrough brightness.\n");
    }
}

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
    case VideoPixelFormat::Nv12: return DXGI_FORMAT_NV12;
    case VideoPixelFormat::Yuy2: return DXGI_FORMAT_YUY2;
    case VideoPixelFormat::Uyvy: return DXGI_FORMAT_YUY2;
    case VideoPixelFormat::P010: return DXGI_FORMAT_P010;
    // X channel is never treated as alpha; the renderer never blends the
    // capture texture directly (D2D overlays render to a separate surface).
    case VideoPixelFormat::Rgb32: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case VideoPixelFormat::Rgb24: return DXGI_FORMAT_R8G8B8A8_UNORM;
    // No silent NV12 fallback: an unrecognized format must fail explicitly
    // in initialize() rather than reinterpret its bytes as planar YUV.
    default: return DXGI_FORMAT_UNKNOWN;
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

#include "settings/TestProfile.h"
// User-writable data belongs in LocalAppData so an installation under
// Program Files does not require elevation and portable copies do not mix
// machine-specific settings into the application directory.
#ifdef LLCV_SETTINGS_MODE_TEST
// Hidden-window lifecycle tests persist only in their unique temporary folder.
static std::wstring g_testUserDataDirectory;
#endif
static std::wstring UserDataDirectory() {
#ifdef LLCV_SETTINGS_MODE_TEST
    if (!g_testUserDataDirectory.empty()) return g_testUserDataDirectory;
#endif
    if (llcv::test_profile::Enabled()) return llcv::test_profile::Directory();
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
    if (llcv::test_profile::Enabled()) return; // Do not import a user's legacy profile.
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
        return cached->supported && !cached->inconclusive &&
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

static bool g_resolutionPlanLatched = false;
static bool g_splitResolutionPlan = false;
static VideoPreset g_capturePreset = VideoPreset::R1920x1080;

static const VideoPresetInfo& VideoPresetDetails(VideoPreset preset) {
    for (const auto& info : kVideoPresets) {
        if (info.preset == preset) return info;
    }
    return kDefaultVideoPreset;
}

static const VideoPresetInfo& CurrentVideoPreset() {
    return VideoPresetDetails(llcv::settings::EffectiveDisplayPreset(g_settings));
}

static const VideoPresetInfo& CurrentCapturePreset() {
    return VideoPresetDetails(g_resolutionPlanLatched ? g_capturePreset :
        g_settings.capturePreset);
}

static void LatchVideoResolutionPlan() {
    g_fullscreenSourceSize.store(false, std::memory_order_release);
    g_capturePreset = g_settings.capturePreset;
    g_splitResolutionPlan = !g_settings.audioOnly &&
        VideoPresetDetails(g_capturePreset).preset != CurrentVideoPreset().preset;
    g_resolutionPlanLatched = true;
}

static bool SourcePixelPerfect() {
    return g_settings.pixelPerfect &&
        CurrentCapturePreset().preset == CurrentVideoPreset().preset;
}

static int RequestedVideoFrameRate() {
    if (g_settings.capture40Fps) return 40;
    return g_settings.videoFrameRate > 0
               ? g_settings.videoFrameRate
               : CurrentCapturePreset().framerate;
}

static bool RequestsExactCaptureRate() {
    return g_settings.capture40Fps || g_settings.videoFrameRate > 0;
}
static bool AllowsUnlistedCaptureRate() {
    return g_settings.capture40Fps ||
        (g_settings.allowUnlistedFps && g_settings.videoFrameRate == 30);
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

static int RelativeScaleForMonitor(HMONITOR monitor, bool capture = false) {
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0;
    const int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
    const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
    if (monitorWidth <= 0 || monitorHeight <= 0) return 0;
    const auto& video = capture ? CurrentCapturePreset() : CurrentVideoPreset();
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
    if (!g_suppressSettingsSave) MigrateLegacySettings();
    if (!g_suppressSettingsSave &&
        !llcv::settings::MigrateLegacyPcmQueueTarget(SettingsPath())) {
        fwprintf(stderr, L"[settings] PCM default migration could not be saved; using 25 ms in memory.\n");
    }
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

static void SaveSettings(bool notifyFailure = false,
                         const llcv::settings::AppSettings* snapshot = nullptr) {
    if (g_suppressSettingsSave) return;
#ifdef LLCV_HDR_FRAME_AUDIT
    return; // Includes settings-dialog acceptance and window-position updates.
#endif
    EnsureUserDataDirectory();
    unsigned long error = 0;
    if (llcv::settings::SaveToIni(SettingsPath(), snapshot ? *snapshot : g_settings, &error))
        return;
    fwprintf(stderr, L"[settings] Could not save profile (Win32=%lu); previous settings preserved.\n", error);
    if (notifyFailure) {
        wchar_t message[512]{};
        swprintf_s(message, IsEnglishUi()
            ? L"Your settings could not be saved (error %lu).\n\nThe previous saved settings have been kept. Changes apply to this session only. Check the settings folder's permissions and available disk space."
            : L"설정을 저장하지 못했습니다. (오류 %lu)\n\n이전에 저장한 설정은 유지됩니다. 변경 사항은 이번 실행에만 적용됩니다. 설정 폴더의 쓰기 권한과 디스크 여유 공간을 확인해 주세요.", error);
        MessageBoxW(nullptr, message, L"LLCV", MB_OK | MB_ICONWARNING);
    }
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
static bool Surround51Active() { return g_ring.Channels() == 6; }
static std::atomic<bool> g_captureAudioRejected{false};
#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
static std::atomic<bool> g_audioInputRecoveryExhausted{false};
#endif
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
        &g_captureAudioRejected,
    };
    return new llcv::capture::AudioSampleGrabberCallback(
        format, g_ring, telemetry, CaptureAudioTrackingActive, nullptr);
}

// -----------------------------------------------------------------------------
// DirectShow helpers
// -----------------------------------------------------------------------------

static HRESULT FindCaptureFilter(
    const std::wstring& selectedId, IBaseFilter** output,
    std::wstring* selectedName = nullptr, std::wstring* selectedDevicePath = nullptr) {
    return llcv::capture::FindVideoCaptureFilter(
        selectedId, kCaptureName, output, selectedName,
        LogModuleMessage, selectedDevicePath);
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
    if (g_playbackMuted.load(std::memory_order_relaxed) ||
        g_backgroundAudioMuted.load(std::memory_order_acquire)) return 0.0;
    return g_volumePercent.load(std::memory_order_acquire) / 100.0;
}

static double TargetAudioChannelGain(int channel) {
    const int percent = channel == 0
        ? g_leftVolumePercent.load(std::memory_order_acquire)
        : g_rightVolumePercent.load(std::memory_order_acquire);
    return percent / 100.0;
}

static llcv::audio::StereoGain TargetStereoGain() {
    const double master = TargetAudioVolumeGain();
    return {master * TargetAudioChannelGain(0),
            master * TargetAudioChannelGain(1)};
}

static void QueueAudioDiagnostic(const wchar_t* message) {
    g_audioDiagnosticMessages.Push(message);
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
    // Session-owned, not driver-thread-local: even the first nonempty callback
    // must start at the saved volume/mute, and survive callback thread changes.
    llcv::audio::StereoGain currentMix = TargetStereoGain();
};

static void PrepareAsioMix(void* context) {
    if (context) static_cast<AsioRenderState*>(context)->currentMix = TargetStereoGain();
}

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
                    QueueAudioDiagnostic(
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

    const bool measurePeaks =
        g_audioOsdVisible.load(std::memory_order_acquire);
    const llcv::audio::MixMetrics mix = llcv::audio::ProcessStereoPcm(
        out, got, state->currentMix, TargetStereoGain(),
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
    // The minimum is sampled before rendering, after startup/warmup, just
    // like WASAPI. Post-render depletion is not a second minimum sample.
    return got;
}

struct WasapiRenderState {
    SincDriftResampler driftResampler{g_ring, &g_audioResamplerFrames};
    llcv::audio::QueueDriftController queueController;
    bool shared = true;
    bool rebuffering = false;
    size_t consecutiveMissingFrames = 0;
    double filteredQueuedFrames = -1.0;
    double correctionPpm = 0.0;
    uint64_t autoCandidateSinceMs = 0;
    bool audioStarted = false;
    bool autoCorrectionActive = false;
    UINT32 queueTargetFrames = 0;
    llcv::audio::StereoGain currentMix{};
    std::array<double, 6> surroundMix{1, 1, 1, 1, 1, 1};
};

static llcv::wasapi::FillResult FillWasapiPcm(
    void* user, int16_t* output, size_t frames) {
    llcv::wasapi::FillResult result{};
    auto* state = static_cast<WasapiRenderState*>(user);
    if (!state || !output || frames == 0) return result;

    result.availableBeforeRender =
        g_ring.AvailableFrames() + state->driftResampler.BufferedFrames();
    if (state->shared && state->rebuffering) {
        // Refill the user's reserve once after substantial starvation. Keep
        // resampler history and the learned clock rate; never grow the target.
        const size_t restartFrames = (std::max)(
            static_cast<size_t>(state->queueTargetFrames), frames + 16);
        if (result.availableBeforeRender < restartFrames) {
            g_sharedRebufferSilenceFrames.fetch_add(frames, std::memory_order_relaxed);
            result.queuedFrames = static_cast<UINT32>(result.availableBeforeRender);
            result.queueTargetFrames = state->queueTargetFrames;
            result.trackingActive = AudioTrackingActive();
            return result;
        }
        state->rebuffering = false;
        state->filteredQueuedFrames = static_cast<double>(result.availableBeforeRender);
        g_sharedRebuffering.store(false, std::memory_order_release);
    }
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
            const double seconds = (std::min)(frames / 48000.0, 0.05);
            state->filteredQueuedFrames +=
                (queuedFrames - state->filteredQueuedFrames) *
                (state->shared ? seconds / (0.5 + seconds) : 0.02);
        }

        if (g_settings.driftCorrection == DriftCorrectionMode::Auto &&
            !state->autoCorrectionActive && state->audioStarted &&
            AudioTrackingActive()) {
            const double deviation =
                std::abs(state->filteredQueuedFrames - targetFrames);
            const uint64_t nowMs = GetTickCount64();
            // Do not spend another five seconds observing once the next
            // block is all that remains. Auto still latches on, never toggles.
            const bool reserveAtRisk = state->shared &&
                queuedFrames <= static_cast<double>(frames + 16) &&
                targetFrames > static_cast<double>(frames + 16);
            if (reserveAtRisk || deviation >= kAutoCorrectionEngageDeviationFrames) {
                if (!state->autoCandidateSinceMs) {
                    state->autoCandidateSinceMs = nowMs;
                }
                if ((reserveAtRisk || (
                    nowMs >= state->autoCandidateSinceMs &&
                    nowMs - state->autoCandidateSinceMs >=
                        kAutoCorrectionEngageHoldMs)) &&
                    queuedFrames >= static_cast<double>(frames)) {
                    state->autoCorrectionActive = true;
                    state->driftResampler.Reset();
                    state->correctionPpm = 0.0;
                    if (reserveAtRisk) {
                        state->filteredQueuedFrames = queuedFrames;
                        state->queueController.BeginWithLowReserve();
                    }
                    if (state->shared) {
                        g_sharedCorrectionNotice.store(true, std::memory_order_release);
                    } else QueueAudioDiagnostic(
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
            if (state->shared && state->audioStarted) {
                state->correctionPpm = state->queueController.Update(
                    state->filteredQueuedFrames, targetFrames, frames);
            } else {
                state->correctionPpm +=
                    (requestedPpm - state->correctionPpm) * 0.02;
            }
            const double ratio =
                1.0 + state->correctionPpm / 1'000'000.0;
            if (state->audioStarted) {
                got = state->driftResampler.Render(output, frames, ratio);
            }
            g_audioResamplePpm.store(
                static_cast<int>(std::lround(state->correctionPpm)),
                std::memory_order_release);
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
    const llcv::audio::MixMetrics mix = Surround51Active()
        ? llcv::audio::ProcessSurroundPcm(output, got, state->surroundMix,
            targetVolumeGain, {TargetAudioChannelGain(0), TargetAudioChannelGain(1)}, measurePeaks)
        : llcv::audio::ProcessStereoPcm(output, got, state->currentMix,
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
        // A tiny isolated shortfall must not turn into a whole extra silent
        // output block. Re-prime only after a full block is missing, either
        // at once or cumulatively across consecutive short writes.
        state->consecutiveMissingFrames += missingFrames;
        if (state->shared && state->consecutiveMissingFrames >= frames) {
            state->rebuffering = true;
            state->consecutiveMissingFrames = 0;
            g_sharedRebuffering.store(true, std::memory_order_release);
            g_sharedRebuffers.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        state->consecutiveMissingFrames = 0;
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

static void OnWasapiBufferChanged(void* context, UINT32 frames) {
    if (context) {
        static_cast<WasapiRenderState*>(context)->driftResampler.Prepare(frames);
    }
    g_audioActualBufferFrames.store(frames, std::memory_order_release);
}

static void OnWasapiPaddingChanged(void*, UINT32 frames) {
    g_audioWasapiPaddingFrames.store(frames, std::memory_order_release);
}

static void OnSharedDeadlineSuspected(void* context, double overdueSeconds, bool duringFill) {
    const auto* state = static_cast<WasapiRenderState*>(context);
    if (!state || !state->audioStarted || !AudioTrackingActive()) return;
    g_sharedLastOverdueUs.store(static_cast<uint64_t>(overdueSeconds * 1'000'000),
                               std::memory_order_relaxed);
    g_sharedLastDeadlineDuringFill.store(duringFill, std::memory_order_relaxed);
    g_sharedDeadlineSuspicions.fetch_add(1, std::memory_order_relaxed);
    g_sharedLastDeadlineMs.store(GetTickCount64(), std::memory_order_release);
}

// Called by the UI timer, never between audio GetBuffer/ReleaseBuffer.
// Shared health counters are coalesced; other backends use a fixed, nonblocking
// message queue. Diagnostic publication never allocates storage or writes files.
static void FlushAudioDiagnostics(bool force = false) {
    // Drain all backend messages on the UI thread, including Exclusive/ASIO.
    // Keep a bounded batch even if diagnostic output itself is temporarily slow.
    llcv::audio::RealtimeLogQueue::Message message;
    for (size_t index = 0; index < llcv::audio::RealtimeLogQueue::kCapacity &&
         g_audioDiagnosticMessages.Pop(message); ++index) {
        fwprintf(stderr, L"%s", message.data());
    }
    const auto droppedMessages = g_audioDiagnosticMessages.TakeDropped();
    if (droppedMessages) fwprintf(stderr,
        L"[audio] %llu diagnostic messages skipped while the UI was busy; audio was not blocked.\n",
        static_cast<unsigned long long>(droppedMessages));
    static uint64_t lastLogMs = 0, previousDeadline = 0, previousRebuffer = 0;
    static uint64_t previousUnderrun = 0;
    static uint64_t previousOverrun = 0;
    static uint64_t previousSilence = 0;
    static bool previousRefilling = false;
    const uint64_t now = GetTickCount64();
    if (!force && now - lastLogMs < 1000) return;
    lastLogMs = now;
    if (g_sharedCorrectionNotice.exchange(false, std::memory_order_acq_rel)) {
        fwprintf(stderr, L"[audio][shared] auto clock correction engaged (deferred notice).\n");
    }
    const uint64_t deadline = g_sharedDeadlineSuspicions.load();
    const uint64_t rebuffer = g_sharedRebuffers.load();
    const uint64_t underrun = g_underruns.load();
    const uint64_t overrun = g_ring.Overruns();
    const uint64_t silence = g_sharedRebufferSilenceFrames.load();
    const bool refilling = g_sharedRebuffering.load();
    if (ActiveAudioMode() != AudioMode::WasapiShared) return;
    if (deadline == previousDeadline && rebuffer == previousRebuffer &&
        underrun == previousUnderrun && overrun == previousOverrun && silence == previousSilence &&
        refilling == previousRefilling) return;
    fwprintf(stderr,
        L"[audio][shared] health: output-deadline-suspected=%llu (+%llu; not measured audio loss), "
        L"PCM-underrun=%llu (+%llu), PCM-missing=%llu frames, PCM-overrun=%llu (+%llu), "
        L"reprime=%llu (+%llu), "
        L"reprime-silence=%llu frames, refilling=%s, queue=%u+%u frames (target %u), "
        L"capture-packet=%u frames, output-padding=%u frames, correction=%+d ppm, "
        L"last-output-overdue=%.3f ms (%s; scheduling estimate only)\n",
        deadline, deadline - previousDeadline, underrun, underrun - previousUnderrun,
        g_audioUnderrunFrames.load(), overrun, overrun - previousOverrun,
        rebuffer, rebuffer - previousRebuffer,
        silence, refilling ? L"yes" : L"no",
        g_audioRingFrames.load(), g_audioResamplerFrames.load(), g_audioQueueTargetFrames.load(),
        g_audioCapturePacketFrames.load(), g_audioWasapiPaddingFrames.load(), g_audioResamplePpm.load(),
        g_sharedLastOverdueUs.load() / 1000.0,
        g_sharedLastDeadlineDuringFill.load() ? L"fill/release" : L"wake");
    previousDeadline = deadline;
    previousRebuffer = rebuffer;
    previousUnderrun = underrun;
    previousOverrun = overrun;
    previousSilence = silence;
    previousRefilling = refilling;
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

static llcv::wasapi::RunResult AudioRenderThreadWasapi(
    AudioMode mode, bool reinitializingEndpoint,
    uint64_t* successfulRuntimeMilliseconds = nullptr) {
    WasapiRenderState state{};
    state.shared = mode == AudioMode::WasapiShared;
    g_sharedRebuffering.store(false, std::memory_order_release);
    state.autoCorrectionActive =
        g_settings.driftCorrection == DriftCorrectionMode::Resample;
    state.queueTargetFrames = static_cast<UINT32>(
        g_settings.pcmQueueTargetMs * kSampleRate / 1000);
    const double initialVolumeGain = TargetAudioVolumeGain();
    state.currentMix = {
        initialVolumeGain * TargetAudioChannelGain(0),
        initialVolumeGain * TargetAudioChannelGain(1)};
    state.surroundMix = {state.currentMix.left, state.currentMix.right,
        initialVolumeGain, initialVolumeGain, state.currentMix.left, state.currentMix.right};
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
    configuration.surround51 = Surround51Active();
    configuration.mode = mode == AudioMode::WasapiExclusive
        ? llcv::wasapi::Mode::Exclusive : llcv::wasapi::Mode::Shared;
    configuration.endpointId = g_settings.audioOutputDeviceId;
    configuration.bufferMilliseconds = g_settings.wasapiBufferMs;
    configuration.sharedPeriodFrames =
        g_settings.wasapiSharedPeriodFrames;
    configuration.reinitializingEndpoint = reinitializingEndpoint;
    configuration.detailedDiagnostics = g_settings.saveLog || g_settings.showDiagnosticConsole;
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
    host.log = &QueueAudioDiagnostic;
    host.outputDeadlineSuspected = &OnSharedDeadlineSuspected;
    const auto restart = llcv::wasapi::Run(
        configuration, host, successfulRuntimeMilliseconds);

    g_sharedRebuffering.store(false, std::memory_order_release);
    g_audioActualBufferFrames.store(0, std::memory_order_release);
    g_audioWasapiPaddingFrames.store(0, std::memory_order_release);
    g_audioResamplerActive.store(false, std::memory_order_release);
    g_audioResamplePpm.store(0, std::memory_order_release);
    g_audioResamplerFrames.store(0, std::memory_order_release);
    return restart;
}


static bool WaitForAudioRetry(unsigned milliseconds) {
    const uint64_t until = GetTickCount64() + milliseconds;
    while (g_running.load(std::memory_order_acquire) && GetTickCount64() < until) {
        Sleep(25);
    }
    return g_running.load(std::memory_order_acquire);
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
    if (!output.Start(&PrepareAsioMix)) {
        fwprintf(stderr, L"[audio] ASIO start failed: %S\n",
                 output.Error().c_str());
        return false;
    }
    uint64_t sessionStartMs = GetTickCount64();
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
    llcv::audio::RecoveryPolicy recovery;
    bool restartFailed = false;
    while (g_running.load(std::memory_order_acquire)) {
        if (!output.RestartRequested()) {
            Sleep(50);
            continue;
        }
        recovery.ObserveSuccessfulRuntime(GetTickCount64() - sessionStartMs);
        fwprintf(stderr, L"[audio] ASIO driver requested reset/resync; reopening output.\n");
        output.Stop();
        g_asioAudioStarted.store(false, std::memory_order_release);
        g_audioActualBufferFrames.store(0, std::memory_order_release);
        bool restarted = false;
        while (g_running.load(std::memory_order_acquire)) {
            const unsigned delay = recovery.NextDelay();
            if (!delay) {
                fwprintf(stderr, L"[audio] ASIO recovery limit reached.\n");
                restartFailed = true;
                break;
            }
            if (!WaitForAudioRetry(delay)) break;
            if (output.Start([](void* context) {
                    auto& state = *static_cast<AsioRenderState*>(context);
                    BeforeWasapiEndpointRestart(nullptr);
                    state.driftResampler.Reset();
                    state.filteredQueuedFrames = -1.0;
                    state.correctionPpm = 0.0;
                    state.autoCandidateSinceMs = 0;
                    state.audioStarted = false;
                    state.autoCorrectionActive =
                        g_settings.driftCorrection == DriftCorrectionMode::Resample;
                    PrepareAsioMix(context);
                    g_audioResamplePpm.store(0, std::memory_order_release);
                    g_audioResamplerActive.store(
                        state.autoCorrectionActive, std::memory_order_release);
                })) {
                sessionStartMs = GetTickCount64();
                g_audioActualBufferFrames.store(
                    static_cast<UINT32>(output.BufferFrames()), std::memory_order_release);
                fwprintf(stderr, L"[audio] ASIO recovered: %ld frames, %.0f Hz.\n",
                         output.BufferFrames(), output.SampleRate());
                restarted = true;
                break;
            }
            fwprintf(stderr, L"[audio] ASIO recovery failed: %S\n", output.Error().c_str());
        }
        if (!restarted) break;
    }
    output.Stop();
    g_asioAudioStarted.store(false, std::memory_order_release);
    g_audioActualBufferFrames.store(0, std::memory_order_release);
    g_audioResamplerActive.store(false, std::memory_order_release);
    g_audioResamplePpm.store(0, std::memory_order_release);
    g_audioResamplerFrames.store(0, std::memory_order_release);
    return !restartFailed;
}

static void AudioRenderThread() {
    bool reinitializingEndpoint = false;
    AudioMode mode = ActiveAudioMode();
    if (mode == AudioMode::Asio) {
        if (AudioRenderThreadAsio()) return;
        // A broken/unavailable ASIO driver must not leave the viewer silent.
        // Fall back to the unchanged WASAPI Shared path for this session.
        fwprintf(stderr,
                 L"[audio] ASIO unavailable; falling back to WASAPI Shared.\n");
        // Publish only the effective backend. Preserve the user's selected
        // mode/driver for the next attempt and never mutate UI-owned settings.
        mode = AudioMode::WasapiShared;
        g_activeAudioMode.store(mode, std::memory_order_release);
        SetActiveAudioOutputName(ConfiguredAudioEndpointName(
            g_settings.audioOutputDeviceId));
        reinitializingEndpoint = true;
    }
    llcv::audio::RecoveryPolicy recovery;
    while (g_running.load(std::memory_order_acquire)) {
        uint64_t successfulRuntimeMilliseconds = 0;
        const auto result = AudioRenderThreadWasapi(
            mode, reinitializingEndpoint,
            &successfulRuntimeMilliseconds);
        recovery.ObserveSuccessfulRuntime(successfulRuntimeMilliseconds);
        if (!g_running.load(std::memory_order_acquire) ||
            result == llcv::wasapi::RunResult::Stopped) break;
        if (result == llcv::wasapi::RunResult::Failed) {
            SetActiveAudioOutputName(keys::Expand(UI_TEXT(L"WASAPI: 출력 사용 불가 · F2로 설정 확인"),g_settings.shortcuts));
            g_audioActualBufferFrames.store(0, std::memory_order_release);
            break;
        }
        if (result == llcv::wasapi::RunResult::EndpointChanged) {
            // A user/default-device change is not a driver failure.
            reinitializingEndpoint = true;
            continue;
        }
        const unsigned delay = recovery.NextDelay();
        if (!delay) {
            fwprintf(stderr, L"[audio] WASAPI recovery limit reached; reopen settings with F2.\n");
            SetActiveAudioOutputName(keys::Expand(UI_TEXT(L"WASAPI: 출력 복구 실패 · F2로 설정 확인"),g_settings.shortcuts));
            g_audioActualBufferFrames.store(0, std::memory_order_release);
            break;
        }
        fwprintf(stderr, L"[audio] WASAPI restarting output after failure in %u ms.\n", delay);
        g_audioActualBufferFrames.store(0, std::memory_order_release);
        g_audioWasapiPaddingFrames.store(0, std::memory_order_release);
        if (!WaitForAudioRetry(delay)) break;
        reinitializingEndpoint = true;
    }
}

// -----------------------------------------------------------------------------
// Direct video path: DirectShow raw video (NV12/YUY2/P010) -> latest frame
// -> D3D11 video processor -> DXGI flip-discard swapchain. Raw input needs no
// decoder or external player. P010 preserves HDR10 through the HDR output path.
// -----------------------------------------------------------------------------

// Compressed compatibility path. DirectShow still owns device
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

static llcv::video_color::RangeOverride SelectedSdrRange(VideoPixelFormat format) {
    // HDR uses its own PQ/BT.2020 input policy. SDR preferences must never
    // reinterpret P010 or relax the HDR range validation.
    return format == VideoPixelFormat::P010
        ? llcv::video_color::RangeOverride::Auto
        : llcv::settings::EffectiveSdrRange(g_settings, format == VideoPixelFormat::Mjpeg);
}

static llcv::video_color::Configuration ResolveRawSdrVideoColor(
    VideoPixelFormat format, int width, int height,
    const DirectShowColorMetadata& metadata) {
    if (format == VideoPixelFormat::Rgb32 || format == VideoPixelFormat::Rgb24) {
        // RGB has no chroma matrix (Configuration::matrix is left at its
        // unused default); only range is meaningful. Unlike YUV's Limited
        // default, RGB defaults to Full when neither a user override nor a
        // valid DirectShow nominalRange hint is present.
        llcv::video_color::Configuration result{};
        result.range = llcv::video_color::Range::Full;
        if (metadata.present && (metadata.nominalRange == 1 || metadata.nominalRange == 2)) {
            result.range = metadata.nominalRange == 1
                ? llcv::video_color::Range::Full : llcv::video_color::Range::Limited;
            result.rangeSource = llcv::video_color::Source::DirectShow;
        }
        const auto rangeOverride = SelectedSdrRange(format);
        if (rangeOverride == llcv::video_color::RangeOverride::Full ||
            rangeOverride == llcv::video_color::RangeOverride::Limited) {
            result.range = rangeOverride == llcv::video_color::RangeOverride::Full
                ? llcv::video_color::Range::Full : llcv::video_color::Range::Limited;
            result.rangeSource = llcv::video_color::Source::UserOverride;
        }
        return result;
    }
    if (format != VideoPixelFormat::Nv12 && format != VideoPixelFormat::Yuy2 &&
        format != VideoPixelFormat::Uyvy) return {};
    // The raw-SDR fallback is BT.709, including HDYC. Do not synthesize a
    // DirectShow matrix: it would hide real metadata retained from negotiation.
    return llcv::video_color::Resolve(false, width, height, {},
        {metadata.transferMatrix, metadata.nominalRange},
        llcv::video_color::MatrixOverride::Auto, SelectedSdrRange(format));
}

static bool ReadConnectedVideoSampleFormat(
    const AM_MEDIA_TYPE* type, int fps, DirectShowColorMetadata& effectiveColor,
    llcv::video::VideoSampleFormat& sampleFormat) {
    if (!llcv::video::ReadVideoSampleFormat(type, fps, sampleFormat)) return false;
    if (sampleFormat.format == VideoPixelFormat::P010)
        effectiveColor = llcv::hdr::ConnectedMetadata(effectiveColor, sampleFormat.color);
    else
        MergeVideoColorMetadata(effectiveColor, sampleFormat.color);
    // Playback, dynamic-type validation and the recording decoder must share
    // the final connected hints, including fields omitted by the driver.
    sampleFormat.color = effectiveColor;
    return true;
}

static void LogDirectShowColorMetadata(
    const wchar_t* source, const DirectShowColorMetadata& metadata) {
    llcv::video::LogDirectShowColorMetadata(
        source, metadata, LogModuleMessage);
}

static bool ResolveInitialVideoColorMetadata(
    IPin* videoPin, const AM_MEDIA_TYPE* activeType,
    VideoPixelFormat wantedFormat, int wantedWidth, int wantedHeight,
    int wantedFps, const DirectShowColorMetadata& acceptedCandidateColor,
    DirectShowColorMetadata& metadata, const GUID* acceptedCandidateSubtype = nullptr) {
    // Active metadata remains authoritative. An unlisted 30/40fps request can
    // come from a different-rate capability; preserve that accepted hint if
    // the driver strips color flags from GetFormat/connected media types.
    // UYVY and HDYC share bytes but not necessarily declared color hints.
    // A driver can replace the alias between the two GetFormat calls.
    const bool sameCandidateSubtype = wantedFormat != VideoPixelFormat::Uyvy ||
        (activeType && acceptedCandidateSubtype &&
         activeType->subtype == *acceptedCandidateSubtype);
    if (wantedFormat == VideoPixelFormat::Uyvy) {
        metadata = {};
        if (acceptedCandidateColor.present && sameCandidateSubtype)
            metadata = acceptedCandidateColor;
        else
            llcv::video::FindMatchingDirectShowColorMetadata(videoPin, wantedFormat,
                wantedWidth, wantedHeight, wantedFps, metadata,
                activeType ? &activeType->subtype : nullptr);
        DirectShowColorMetadata activeColor{};
        ExtractVideoColorMetadata(activeType, activeColor);
        // Active declarations win field by field. Missing matrix/range must
        // not erase a known same-subtype declaration from negotiation.
        MergeVideoColorMetadata(metadata, activeColor);
        return metadata.present;
    }
    if (ExtractVideoColorMetadata(activeType, metadata)) return true;
    if (acceptedCandidateColor.present && sameCandidateSubtype) {
        metadata = acceptedCandidateColor;
        return true;
    }
    // Preserve the existing fallback for drivers exposing color only in caps.
    return llcv::video::FindMatchingDirectShowColorMetadata(
        videoPin, wantedFormat, wantedWidth, wantedHeight, wantedFps,
        metadata, activeType ? &activeType->subtype : nullptr);
}

static HRESULT ConfigureVideoPin(
    IPin* videoPin, int wantedWidth, int wantedHeight, int wantedFps,
    VideoPixelFormat wantedFormat, DWORD& imageBytes, UINT32& stride,
    int& configuredFps, VideoPixelFormat& configuredFormat,
    DirectShowColorMetadata& acceptedCandidateColor, GUID* acceptedCandidateSubtype = nullptr) {
    acceptedCandidateColor = {};
    return llcv::video::ConfigureVideoPin(
        videoPin, wantedWidth, wantedHeight, wantedFps, wantedFormat,
        imageBytes, stride, configuredFps, configuredFormat,
        LogModuleMessage, RequestsExactCaptureRate(),
        RequestsExactCaptureRate() ? &acceptedCandidateColor : nullptr,
        AllowsUnlistedCaptureRate(), acceptedCandidateSubtype);
}

static HRESULT GetActiveVideoPinFormat(
    IPin* videoPin, AM_MEDIA_TYPE** mediaType) {
    return llcv::video::GetActiveVideoPinFormat(videoPin, mediaType);
}

#if defined(LLCV_GPU_DIAGNOSTICS) || defined(LLCV_SETTINGS_MODE_TEST)
// Hardware-free settings integration tests; absent from production builds.
static std::vector<PixelFormatSupport> (*g_testVideoCapabilityProbe)(
    const std::wstring&, int, int, HRESULT*) = nullptr;
#endif
static std::mutex g_settingsCaptureProbeMutex;
static std::vector<PixelFormatSupport> ProbePixelFormats(
    const std::wstring& captureDeviceId, int width, int height, HRESULT* queryStatus = nullptr,
    llcv::video::VideoCapabilityDiagnostics* diagnostics = nullptr) {
    std::lock_guard captureLock(g_settingsCaptureProbeMutex);
    if (queryStatus) *queryStatus = S_OK;
    if (diagnostics) *diagnostics = {};
#if defined(LLCV_GPU_DIAGNOSTICS) || defined(LLCV_SETTINGS_MODE_TEST)
    if (g_testVideoCapabilityProbe)
        return g_testVideoCapabilityProbe(captureDeviceId, width, height, queryStatus);
#endif
    std::vector<PixelFormatSupport> result;
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initHr);
    if (initHr == RPC_E_CHANGED_MODE) initHr = S_OK;
    if (FAILED(initHr)) { if (queryStatus) *queryStatus = initHr; return result; }

    IBaseFilter* capture = nullptr;
    IPin* videoPin = nullptr;
    HRESULT hr = FindCaptureFilter(captureDeviceId, &capture);
    if (SUCCEEDED(hr)) {
        hr = FindOutputPinByMajorType(capture, MEDIATYPE_Video, &videoPin);
    }
    if (SUCCEEDED(hr)) {
        result = llcv::video::ProbePixelFormats(videoPin, width, height, queryStatus, LogModuleMessage, diagnostics);
    } else {
        if (queryStatus) *queryStatus = hr;
        LogHr(L"Video capability device/pin query", hr);
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
    std::lock_guard captureLock(g_settingsCaptureProbeMutex);
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

static std::wstring BuildRuntimeOsdText(int outputWidth, int outputHeight);

// Source rectangles are specified in texture coordinates before VP mirroring.
static RECT VideoSourceRectForStorage(RECT imageRect, int height, bool bottomUp) {
    if (bottomUp) {
        const LONG top = imageRect.top;
        imageRect.top = height - imageRect.bottom;
        imageRect.bottom = height - top;
    }
    return imageRect;
}

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
    llcv::video::UyvyRepacker uyvyRepacker;
    ID3D11UnorderedAccessView* uyvyViews[kUploadSurfaceCount]{};
    ID3D11Texture2D* uyvyUploadTextures[kUploadSurfaceCount]{};
    ID3D11ShaderResourceView* uyvyUploadViews[kUploadSurfaceCount]{};
    bool uyvyInput = false;
    llcv::video::Rgb24Repacker rgb24Repacker;
    ID3D11UnorderedAccessView* rgb24Views[kUploadSurfaceCount]{};
    ID3D11Texture2D* rgb24UploadTextures[kUploadSurfaceCount]{};
    ID3D11ShaderResourceView* rgb24UploadViews[kUploadSurfaceCount]{};
    bool rgb24Input = false;
    DXGI_COLOR_SPACE_TYPE rgbInputColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool rgbInput = false;
    bool rgbFullRange = true;
    // DIB row direction at last initialize(); orientation-only state, kept
    // separate from rgbInput so a reconnect with an unchanged format but a
    // flipped orientation can be detected and trigger a reinitialize.
    bool lastTopDown = true;
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
    ID2D1SolidColorBrush* osdCacheEdgeBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheTextBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheBoostBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheBarBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* volumeCacheBarBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheTextBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheBarBackgroundBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheBarBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheHighlightBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheClipBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheBoostBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheCardBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheEdgeBrush = nullptr;
    ID2D1SolidColorBrush* audioCacheSecondaryBrush = nullptr;
    ID3D11Texture2D* osdOverlayTexture = nullptr;
    ID3D11Texture2D* volumeOverlayTexture = nullptr;
    ID3D11Texture2D* audioOverlayTexture = nullptr;
    ID3D11ShaderResourceView* osdOverlayShaderView = nullptr;
    ID3D11ShaderResourceView* volumeOverlayShaderView = nullptr;
    ID3D11ShaderResourceView* audioOverlayShaderView = nullptr;
    ID3D11Texture2D* muteIndicatorTexture = nullptr;
    ID3D11ShaderResourceView* muteIndicatorShaderView = nullptr;
    bool muteIndicatorUnavailable = false;
    ID3D11VertexShader* overlayVertexShader = nullptr;
    ID3D11PixelShader* overlayPixelShader = nullptr;
    ID3D11Buffer* overlayRectBuffer = nullptr;
    ID3D11SamplerState* overlaySampler = nullptr;
    ID3D11BlendState* overlayBlendState = nullptr;
    ID3D11Texture2D* hdrOverlayBackground = nullptr;
    ID3D11ShaderResourceView* hdrOverlayBackgroundView = nullptr;
    ID3D11Buffer* hdrOverlayConstants = nullptr;
    IDWriteFactory* dwriteFactory = nullptr;
    llcv::overlay_ui::Fonts overlayFonts;
#ifdef LLCV_RECORDING
    llcv::recording::GpuIndicator recordingIndicator;
#endif
    IDWriteTextFormat* osdTextFormat = nullptr;
    IDWriteTextFormat* volumeTextFormat = nullptr;
    IDWriteTextFormat* audioTextFormat = nullptr;
    IDWriteTextFormat* audioValueFormat = nullptr;
    IDWriteTextFormat* audioSmallFormat = nullptr;
    IDWriteTextLayout* osdTextLayout = nullptr;
    IDWriteTextLayout* volumeTextLayout = nullptr;
    float volumePanelHeight = 82.0f;
    float volumePanelTop = 0.0f;
    UINT outputWidth = 0;
    UINT outputHeight = 0;
    bool fullscreenDisplaySize = false;
    bool pixelPerfectBorders = false;
    bool skipRedundantBorderClear = false;
    bool exclusiveActive = false;
    uint64_t presentationRevision = 0;
    llcv::exclusive_presentation::AttemptGate exclusiveAttempts;
    uint64_t outputConfigurationGeneration = 0;
    uint64_t cachedOverlayGeneration = 0;
    unsigned cachedOverlayMask = 0;
    UINT nextUploadSurface = 0;
    UINT activeUploadSurface = 0;
    bool allowTearing = false;
    bool sharpScalingActive = false;
    bool discardUpdateAvailable = false;
    bool occluded = false;
    bool occlusionLogged = false;
    uint64_t nextOcclusionTestMs = 0;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_NV12;
    DXGI_FORMAT backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    UINT adapterVendor = 0;
    int captureWidth = 0, captureHeight = 0, captureFps = 0;
    bool hdrOutput = false;
    DXGI_COLOR_SPACE_TYPE hdrInputColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;
#ifdef LLCV_GPU_DIAGNOSTICS
    double diagnosticVideoUs = 0;
    double diagnosticOverlayUs = 0;
    double diagnosticPresentUs = 0;
#endif
    llcv::video_color::Configuration sdrColor{};
    llcv::upscaling::Pipeline cmaaNisPipeline;
    int cmaaNisAppliedRequest = -1;
    HRESULT cmaaNisStatus = S_FALSE;
#ifdef LLCV_SPATIAL_PROTOTYPE
    llcv::spatial::Pipeline spatialPipeline;
    int64_t spatialPresentQpc=0;
#endif
#ifdef LLCV_VSR_FEATURE
    llcv::vsr::State vsrState = llcv::vsr::State::Untouched;
    llcv::vsr::Mode vsrAppliedMode = llcv::vsr::Mode::Disabled;
    bool vsrEligible = false;
    UINT vsrInputWidth = 0, vsrInputHeight = 0;
    UINT vsrDisplayWidth = 0, vsrDisplayHeight = 0;
    std::wstring vsrOsdLine() const {
        if (cmaaNisAppliedRequest != -1) {
            wchar_t line[192]{};
            swprintf_s(line,L"CMAA2 → NIS   %s · %.2f · %s\n",
                cmaaNisAppliedRequest < 0 ? L"OFF" : cmaaNisPipeline.Enabled()?L"ON":FAILED(cmaaNisStatus)
                    ? (IsEnglishUi()?L"Unavailable / OFF":L"사용 불가 / OFF")
                    : hdrOutput ? (IsEnglishUi()?L"Bypass (SDR only)":L"적용 제외 (SDR 전용)")
                    : (IsEnglishUi()?L"Bypass (scale limit)":L"적용 제외 (배율 범위)"),
                double(llcv::upscaling::RequestSharpness(cmaaNisAppliedRequest))/100.0,hdrOutput?L"HDR10":L"SDR");
            return line;
        }
        const bool english = IsEnglishUi();
        const wchar_t* status = L"OFF";
        switch (vsrState) {
        case llcv::vsr::State::Requested:
            status = L"ON"; break;
        case llcv::vsr::State::Bypassed:
            status = english ? L"Unavailable" : L"적용 불가"; break;
        case llcv::vsr::State::Rejected:
            status = english ? L"Rejected / OFF" : L"요청 거부 / OFF"; break;
        case llcv::vsr::State::Unknown:
            status = english ? L"Request failed / state unknown" : L"요청 실패 / 상태 미확인"; break;
        default: break;
        }
        wchar_t line[192]{};
        swprintf_s(line, L"VSR           %s · %u×%u → %u×%u\n", status,
            vsrInputWidth, vsrInputHeight, vsrDisplayWidth, vsrDisplayHeight);
        return line;
    }
#ifdef LLCV_VSR_EXPERIMENT
    // Private measurements must not reserve histograms in the release renderer.
    llcv::vsr::Timing vsrTiming;
    unsigned vsrWarmupFrames = 0;
    void reportVsrTiming() {
        if (vsrState == llcv::vsr::State::Untouched) return;
        const auto data = vsrTiming.Snapshot();
        fwprintf(stderr, L"[vsr-test] summary state=%s; CPU metrics only, not GPU completion/display latency\n",
            llcv::vsr::StateName(vsrState));
        auto print = [](const wchar_t* name, const llcv::vsr::Distribution& d) {
            fwprintf(stderr, L"[vsr-test] %s n=%llu mean=%.3f p95<=%.3f p99<=%.3f max=%.3f ms\n",
                name, d.count, d.Mean(), d.Percentile(.95), d.Percentile(.99), d.maximum);
        };
        print(L"CPU VideoProcessorBlt call", data.cpuBlt);
        print(L"capture callback -> Present return (not HDMI latency)", data.captureToPresent);
        print(L"CPU upload", data.upload);
        print(L"CPU Present", data.present);
        print(L"Present return intervals", data.intervals);
    }
#endif
    HRESULT applyVsrMode(bool initializing = false) {
        auto requested =
#ifdef LLCV_SPATIAL_PROTOTYPE
            spatialPipeline.Enabled() ? llcv::vsr::Mode::Off :
#endif
            g_vsrMode.load(std::memory_order_acquire);
        if(cmaaNisAppliedRequest>=0 && requested==llcv::vsr::Mode::On)
            requested=llcv::vsr::Mode::Off;
        if (!initializing && requested == vsrAppliedMode) return S_OK;
        cachedOverlayGeneration = 0; // invalidate once on the render owner, not every frame
#ifdef LLCV_VSR_EXPERIMENT
        reportVsrTiming();
        vsrTiming.Reset();
#endif
        vsrAppliedMode = requested;
        if (requested == llcv::vsr::Mode::Disabled) {
            vsrState = llcv::vsr::State::Untouched;
            // Default launch must keep UI-free frames (especially HDR) free
            // of overlay scratch copies. Show status only after an opt-in.
            return S_OK;
        }
        if (!vsrEligible) {
            vsrState = llcv::vsr::State::Bypassed;
            ShowTransientHud(TransientHudContent::VsrUnavailable);
        } else {
            const bool enable = requested == llcv::vsr::Mode::On;
            const HRESULT requestHr = llcv::vsr::SetRequest(videoContext, processor, enable);
            vsrState = enable ? llcv::vsr::State::Requested : llcv::vsr::State::Off;
            if (requestHr != S_OK) {
                fwprintf(stderr, L"[vsr-test] request rejected: 0x%08X\n", static_cast<unsigned>(requestHr));
                const HRESULT offHr = llcv::vsr::SetRequest(videoContext, processor, false);
                if (offHr != S_OK) {
                    // Unknown state must not be presented as a successful OFF.
                    vsrState = llcv::vsr::State::Unknown;
                    ShowTransientHud(TransientHudContent::VsrFailed);
                    return FAILED(offHr) ? offHr : E_FAIL;
                }
                vsrState = llcv::vsr::State::Rejected;
            }
#ifdef LLCV_VSR_EXPERIMENT
            vsrTiming.Initialize(device, vsrWarmupFrames);
#endif
            ShowTransientHud(vsrState == llcv::vsr::State::Requested ? TransientHudContent::VsrOn
                : vsrState == llcv::vsr::State::Off ? TransientHudContent::VsrOff
                : TransientHudContent::VsrRejected);
        }
        fwprintf(stderr, L"[vsr-test] F6: %s; no capture restart/output rebuild; "
            L"quality/activation depend on NVIDIA settings.\n",
            llcv::vsr::StateName(vsrState));
        return S_OK;
    }
#endif
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
    bool scrgbOutput = false;
    llcv::scrgb::Pipeline scrgbPipeline;
#endif
#ifdef LLCV_HDR_FRAME_AUDIT
    llcv::video::CaptureColorMetadata auditMetadata{};

    void saveRequestedFrameAudit() {
        if (!g_hdrFrameAuditRequested.exchange(false)) return;
        HRESULT result = E_FAIL;
        std::wstring path;
        try {
            GUID id{};
            result = CoCreateGuid(&id);
            wchar_t suffix[40]{};
            if (SUCCEEDED(result) && StringFromGUID2(id, suffix, 40)) {
                path = g_hdrFrameAuditDirectory + L"\\hdr-frame-" + suffix;
                llcv::hdr_audit::Interpretation info{};
                info.metadata = auditMetadata;
                info.forceHdr = g_settings.forceHdr10;
                info.hdrChromaSelection = static_cast<unsigned>(g_settings.hdrChromaLocation);
                info.hdrOutput = hdrOutput;
                info.inputColorSpace = static_cast<unsigned>(hdrInputColorSpace);
                DXGI_COLOR_SPACE_TYPE outputSpace{};
                if (videoContext1 && processor)
                    videoContext1->VideoProcessorGetOutputColorSpace1(processor, &outputSpace);
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
                if (scrgbOutput) outputSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
#endif
                info.outputColorSpace = static_cast<unsigned>(outputSpace);
                info.sdrMatrix = static_cast<unsigned>(sdrColor.matrix);
                info.sdrRange = static_cast<unsigned>(sdrColor.range);
                info.displayHdr = g_hdrDisplayState.load();
                info.uiWhiteNits = g_hdrUiWhiteNits.load();
                result = llcv::hdr_audit::SavePair(context,
                    nv12Textures[activeUploadSurface], backBuffer, path, info);
            }
        } catch (const std::bad_alloc&) {
            result = E_OUTOFMEMORY;
        }
        fwprintf(stderr, L"[hdr-audit] %s result=0x%08lX directory=%s\n",
                 SUCCEEDED(result) ? L"saved matching frame pair" : L"save failed (P010 required)",
                 static_cast<unsigned long>(result), path.c_str());
    }
#endif

    void reset() {
        if (swapChain && llcv::presentation::IsExclusive(g_settings.presentationMode)) {
            const HRESULT leave = swapChain->SetFullscreenState(FALSE, nullptr);
            if (FAILED(leave)) LogHr(L"Leave exclusive output during renderer reset",leave);
        }
        exclusiveActive=false;presentationRevision=0;exclusiveAttempts.Reset();
        g_videoExclusiveActive.store(false, std::memory_order_release);
        skipRedundantBorderClear=false;
        volumePanelHeight = 82.0f;
        volumePanelTop = 0.0f;
#ifdef LLCV_VSR_FEATURE
#ifdef LLCV_VSR_EXPERIMENT
        reportVsrTiming();
        vsrTiming.Reset();
        vsrWarmupFrames = 0;
#endif
        vsrState = llcv::vsr::State::Untouched;
        vsrAppliedMode = llcv::vsr::Mode::Disabled;
        vsrEligible = false;
        vsrInputWidth = vsrInputHeight = vsrDisplayWidth = vsrDisplayHeight = 0;
#endif
#ifdef LLCV_SPATIAL_PROTOTYPE
        spatialPipeline.Reset();
#endif
        cmaaNisPipeline.Reset();
        cmaaNisAppliedRequest=-1;
        cmaaNisStatus=S_FALSE;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        scrgbPipeline.Reset();
        scrgbOutput = false;
#endif
        if (context) {
            context->ClearState();
            context->Flush();
        }
        SafeRelease(overlayBlendState);
        SafeRelease(hdrOverlayBackgroundView);
        SafeRelease(hdrOverlayBackground);
        SafeRelease(hdrOverlayConstants);
        SafeRelease(overlaySampler);
        SafeRelease(overlayRectBuffer);
        SafeRelease(overlayPixelShader);
        SafeRelease(overlayVertexShader);
        SafeRelease(audioOverlayShaderView);
        SafeRelease(muteIndicatorShaderView);
        SafeRelease(muteIndicatorTexture);
        SafeRelease(volumeOverlayShaderView);
        SafeRelease(osdOverlayShaderView);
        SafeRelease(audioOverlayTexture);
        SafeRelease(volumeOverlayTexture);
        SafeRelease(osdOverlayTexture);
        SafeRelease(audioCacheClipBrush);
        SafeRelease(audioCacheBoostBrush);
        SafeRelease(audioCacheCardBrush);
        SafeRelease(audioCacheEdgeBrush);
        SafeRelease(audioCacheSecondaryBrush);
        SafeRelease(audioCacheHighlightBrush);
        SafeRelease(audioCacheTextBrush);
        SafeRelease(audioCacheBackgroundBrush);
        SafeRelease(audioCacheBarBrush);
        SafeRelease(audioCacheBarBackgroundBrush);
        SafeRelease(volumeCacheBarBrush);
        SafeRelease(volumeCacheBarBackgroundBrush);
        SafeRelease(volumeCacheTextBrush);
        SafeRelease(volumeCacheBoostBrush);
        SafeRelease(volumeCacheBackgroundBrush);
        SafeRelease(osdCacheTextBrush);
        SafeRelease(osdCacheEdgeBrush);
        SafeRelease(osdCacheBackgroundBrush);
        SafeRelease(audioCacheTarget);
        SafeRelease(volumeCacheTarget);
        SafeRelease(osdCacheTarget);
        SafeRelease(volumeTextLayout);
        SafeRelease(osdTextLayout);
        SafeRelease(volumeTextFormat);
        SafeRelease(audioTextFormat);
        SafeRelease(audioValueFormat);
        SafeRelease(audioSmallFormat);
        SafeRelease(osdTextFormat);
#ifdef LLCV_RECORDING
        recordingIndicator.Reset();
#endif
        overlayFonts.Reset(); // Release layouts/formats before unregistering fonts.
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
            SafeRelease(rgb24Views[i]);
            SafeRelease(rgb24UploadViews[i]);
            SafeRelease(rgb24UploadTextures[i]);
            SafeRelease(uyvyViews[i]);
            SafeRelease(uyvyUploadViews[i]);
            SafeRelease(uyvyUploadTextures[i]);
            SafeRelease(nv12Textures[i]);
        }
        uyvyRepacker.Reset();
        rgb24Repacker.Reset();
        rgb24Input = false;
        uyvyInput = false;
        rgbInput = false;
        rgbFullRange = true;
        lastTopDown = true;
        SafeRelease(swapChain);
        SafeRelease(videoContext);
        SafeRelease(videoContext1);
        SafeRelease(videoDevice);
        SafeRelease(context1);
        SafeRelease(context);
        SafeRelease(device);
        outputWidth = 0;
        outputHeight = 0;
        fullscreenDisplaySize = false;
        pixelPerfectBorders = false;
        outputConfigurationGeneration = 0;
        cachedOverlayGeneration = 0;
        cachedOverlayMask = 0;
        muteIndicatorUnavailable = false;
        nextUploadSurface = 0;
        activeUploadSurface = 0;
        allowTearing = false;
        sharpScalingActive = false;
        discardUpdateAvailable = false;
        occluded = false;
        occlusionLogged = false;
        nextOcclusionTestMs = 0;
        inputFormat = DXGI_FORMAT_NV12;
        backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        captureWidth = captureHeight = captureFps = 0;
        adapterVendor = 0;
        hdrOutput = false;
        sdrColor = {};
        g_hdrOutputActive.store(false, std::memory_order_release);
        g_hdrDisplayState.store(-1, std::memory_order_release);
    }

    ~DirectD3D11Renderer() {
        reset();
    }

    HRESULT deviceRemovedReason() const {
        return device ? device->GetDeviceRemovedReason() : E_POINTER;
    }

    bool outputConfigurationChanged() const {
        return outputConfigurationGeneration !=
            g_outputConfigurationGeneration.load(std::memory_order_acquire) ||
            (ExclusiveOutputSelected() && presentationRevision !=
                g_presentationRevision.load(std::memory_order_acquire));
    }

    HRESULT initialize(HWND hwnd, int width, int height, int fps,
                       VideoPixelFormat pixelFormat,
                       bool hdrInputMetadataAvailable = false,
                       llcv::video_color::Configuration color = {},
                       DXGI_COLOR_SPACE_TYPE hdrColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020,
                       bool topDown = true,
                       DirectShowColorMetadata rgbMetadata = {}) {
        reset();
        captureWidth = width; captureHeight = height; captureFps = fps;
        if (pixelFormat == VideoPixelFormat::P010 && !hdrInputMetadataAvailable) {
            fwprintf(stderr, L"[hdr] P010 is HDR-only; select NV12/YUY2 for SDR input.\n");
            return DXGI_ERROR_UNSUPPORTED;
        }
        hdrInputColorSpace = hdrColorSpace;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        scrgbOutput = g_useScrgbPrototype && pixelFormat == VideoPixelFormat::P010 && hdrInputMetadataAvailable;
#endif
        if (llcv::presentation::IsCompatibility(g_settings.presentationMode) &&
            pixelFormat == VideoPixelFormat::P010 && hdrInputMetadataAvailable) {
            fwprintf(stderr, L"[video] HDR10 is not supported by Blt compatibility output; "
                             L"select Immediate or VSync (Flip).\n");
            return DXGI_ERROR_UNSUPPORTED;
        }
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
        fullscreenDisplaySize =
            g_fullscreen.load(std::memory_order_acquire);
        outputConfigurationGeneration = configurationGeneration;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef LLCV_GPU_DIAGNOSTICS
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
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
        const bool hdr10Requested = pixelFormat == VideoPixelFormat::P010 &&
                                    hdrInputMetadataAvailable;
        rgb24Input = pixelFormat == VideoPixelFormat::Rgb24;
        rgbInput = rgb24Input || pixelFormat == VideoPixelFormat::Rgb32;
        if (rgbInput) {
            const UINT transfer = rgbMetadata.present ? rgbMetadata.transferFunction : 0;
            const UINT primaries = rgbMetadata.present ? rgbMetadata.primaries : 0;
            if ((transfer != 0 && transfer != 4 && transfer != 5 && transfer != 7) ||
                (primaries != 0 && primaries != 2)) {
                fwprintf(stderr, L"[video] unsupported RGB transfer/primaries (%u/%u); SDR RGB cannot represent this tuple.\n", transfer, primaries);
                return DXGI_ERROR_UNSUPPORTED;
            }
            rgbInputColorSpace = color.range == llcv::video_color::Range::Full
                ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 : DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709;
        }
        // RGB bottom-up orientation needs VideoProcessorSetStreamMirror
        // (ID3D11VideoContext1), independent of HDR10's own reason for it.
        if (hdr10Requested || rgbInput) {
            hr = context->QueryInterface(IID_PPV_ARGS(&videoContext1));
            if (FAILED(hr)) {
                fwprintf(stderr, hdr10Requested
                    ? L"[hdr] ID3D11VideoContext1 unavailable; HDR10 path rejected.\n"
                    : L"[video] ID3D11VideoContext1 unavailable; RGB input rejected.\n");
                return hr;
            }
            if (hdr10Requested) hdrOutput = true;
        }
        rgbFullRange = color.range == llcv::video_color::Range::Full;
        lastTopDown = topDown;

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

        if (llcv::presentation::IsCompatibility(g_settings.presentationMode)) {
            allowTearing = false;
        }
        auto swapDesc = llcv::presentation::Description(
            g_settings.presentationMode, outputWidth, outputHeight,
            hdrOutput, allowTearing);
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        if (scrgbOutput) swapDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
#endif
        const HWND outputWindow=PresentationWindow(hwnd);
        hr = factory->CreateSwapChainForHwnd(device, outputWindow, &swapDesc, nullptr,
                                             nullptr, &swapChain);
        backBufferFormat = swapDesc.Format;
        factory->MakeWindowAssociation(outputWindow, DXGI_MWA_NO_ALT_ENTER);
        DXGI_ADAPTER_DESC adapterDesc{};
        adapter->GetDesc(&adapterDesc);
        adapterVendor = adapterDesc.VendorId;
#ifdef LLCV_VSR_FEATURE
        // Actual renderer is authoritative if the startup adapter has changed.
        g_vsrGpuVendor.store(adapterDesc.VendorId, std::memory_order_release);
#endif
        fwprintf(stderr,
                 L"[video-output] create: app=%s path=%s size=%ux%u "
                 L"fullscreen=%d generation=%llu flags=0x%X result=0x%08X "
                 L"gpu=%s uptime=%llu ms\n",
                 kAppVersionLabel,
                 llcv::presentation::PathName(g_settings.presentationMode),
                 outputWidth, outputHeight, g_fullscreen.load() ? 1 : 0,
                 static_cast<unsigned long long>(configurationGeneration),
                 swapDesc.Flags, static_cast<unsigned>(hr), adapterDesc.Description,
                 static_cast<unsigned long long>(GetTickCount64()));
        SafeRelease(factory);
        SafeRelease(adapter);
        SafeRelease(dxgiDevice);
        if (FAILED(hr)) return hr;

        if (hdrOutput) {
            auto outputColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
            if (scrgbOutput) outputColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
#endif
            IDXGISwapChain3* swapChain3 = nullptr;
            hr = swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3));
            if (SUCCEEDED(hr)) {
                UINT support = 0;
                hr = swapChain3->CheckColorSpaceSupport(
                    outputColorSpace, &support);
                if (SUCCEEDED(hr) && !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
                    hr = DXGI_ERROR_UNSUPPORTED;
            }
            if (SUCCEEDED(hr)) {
                hr = swapChain3->SetColorSpace1(
                    outputColorSpace);
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
                // DirectShow extended color flags do not contain mastering
                // luminance/primaries or MaxCLL/MaxFALL. Do not invent them.
                const HRESULT metadataHr = swapChain4->SetHDRMetaData(
                    DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
                fwprintf(stderr, L"[hdr] original mastering metadata unavailable; no synthetic metadata sent (0x%08X).\n",
                         static_cast<unsigned>(metadataHr));
                SafeRelease(swapChain4);
            }
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
            if (scrgbOutput) fwprintf(stderr,
                L"[hdr] DIRECT P010 shader -> FP16 scRGB (linear BT.709, 1=80 nits); one draw, no tone mapping.\n");
            else
#endif
            fwprintf(stderr,
                     L"[hdr] HDR10 swapchain configured: P010 -> "
                     L"BT.2020 PQ 10-bit swap chain; no frame queue.\n");
            RefreshHdrDisplayStatus(hwnd);
        }

        IDXGISwapChain2* swapChain2 = nullptr;
        if ((swapDesc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) &&
            SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain2)))) {
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
        if (inputFormat == DXGI_FORMAT_UNKNOWN) {
            fwprintf(stderr, L"[video] unsupported capture pixel format; refusing to "
                              L"default the GPU input texture to NV12.\n");
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        uyvyInput = pixelFormat == VideoPixelFormat::Uyvy;
        if (uyvyInput) {
            hr = uyvyRepacker.Initialize(device);
            if (FAILED(hr)) {
                LogHr(L"UYVY GPU conversion support", hr);
                return llcv::video::kUyvyConversionUnsupported;
            }
        }
        if (rgb24Input) {
            hr = rgb24Repacker.Initialize(device);
            if (FAILED(hr)) return llcv::video::kRgb24ConversionUnsupported;
        }
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = static_cast<UINT>(width);
        textureDesc.Height = static_cast<UINT>(height);
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = inputFormat;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        if (uyvyInput || rgb24Input) textureDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        if (scrgbOutput) textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
#endif
        for (UINT i = 0; i < kUploadSurfaceCount; ++i) {
            hr = device->CreateTexture2D(&textureDesc, nullptr,
                                         &nv12Textures[i]);
            if (FAILED(hr)) {
                if (!uyvyInput) return hr;
                LogHr(L"UYVY upload texture", hr);
                return llcv::video::kUyvyConversionUnsupported;
            }
            if (rgb24Input) {
                hr = rgb24Repacker.CreateOutputView(nv12Textures[i], &rgb24Views[i]);
                if (SUCCEEDED(hr)) hr = rgb24Repacker.CreateRawTexture(
                    static_cast<UINT>(width), static_cast<UINT>(height),
                    &rgb24UploadTextures[i], &rgb24UploadViews[i]);
                if (FAILED(hr)) return llcv::video::kRgb24ConversionUnsupported;
            }
            if (uyvyInput) {
                hr = uyvyRepacker.CreateView(nv12Textures[i], &uyvyViews[i]);
                if (SUCCEEDED(hr)) hr = uyvyRepacker.CreateInputTexture(
                    static_cast<UINT>(width), static_cast<UINT>(height),
                    &uyvyUploadTextures[i], &uyvyUploadViews[i]);
                if (FAILED(hr)) {
                    LogHr(L"UYVY packed input and YUY2 output views", hr);
                    return llcv::video::kUyvyConversionUnsupported;
                }
            }
        }

        hr = createVideoOutputResources(width, height, fps);
        if (uyvyInput && (hr == DXGI_ERROR_UNSUPPORTED || hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED))) {
            LogHr(L"UYVY/YUY2 video processor support", hr);
            return llcv::video::kUyvyConversionUnsupported;
        }
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
            const char* pixelSource = hdrOutput ? llcv::hdr::kOverlayShader : overlayPixelSource;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
            if (scrgbOutput) pixelSource = llcv::scrgb::kOverlayShader;
#endif
            hr = D3DCompile(pixelSource,
                            strlen(pixelSource), nullptr, nullptr,
                            nullptr, "main", "ps_4_0", 0, 0, &pixelBlob,
                            &shaderErrors);
        }
        if (FAILED(hr) && shaderErrors)
            fwprintf(stderr, L"[video] overlay shader compile failed: %hs\n",
                     static_cast<const char*>(shaderErrors->GetBufferPointer()));
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
        if (hdrOutput) {
            hr = device->CreateBuffer(&constantBufferDesc, nullptr, &hdrOverlayConstants);
            if (FAILED(hr)) return hr;
        }
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&samplerDesc, &overlaySampler);
        if (FAILED(hr)) return hr;
        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = hdrOutput ? FALSE : TRUE;
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
        // The private GDI fonts used by settings need a DirectWrite collection
        // here. Fallback fonts preserve rendering on older DirectWrite versions.
        (void)overlayFonts.Initialize(dwriteFactory);
#ifdef LLCV_RECORDING
        if (ExclusiveOutputSelected()) {
            hr = recordingIndicator.Initialize(device, d2dFactory, dwriteFactory,
                overlayFonts, IsEnglishUi());
            if (FAILED(hr)) return hr;
        }
#endif
        hr = overlayFonts.CreateFormat(dwriteFactory, DWRITE_FONT_WEIGHT_MEDIUM,
            15.0f, IsEnglishUi(), &osdTextFormat);
        if (SUCCEEDED(hr)) hr = overlayFonts.CreateFormat(dwriteFactory,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, 22.0f, IsEnglishUi(), &volumeTextFormat);
        if (SUCCEEDED(hr)) hr = overlayFonts.CreateFormat(dwriteFactory,
            DWRITE_FONT_WEIGHT_MEDIUM, 16.0f, IsEnglishUi(), &audioTextFormat);
        if (SUCCEEDED(hr)) hr = overlayFonts.CreateFormat(dwriteFactory,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, 22.0f, IsEnglishUi(), &audioValueFormat);
        if (SUCCEEDED(hr)) hr = overlayFonts.CreateFormat(dwriteFactory,
            DWRITE_FONT_WEIGHT_MEDIUM, 13.0f, IsEnglishUi(), &audioSmallFormat);
        if (FAILED(hr)) return hr;
        audioTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        audioValueFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        audioSmallFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        osdTextFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 20.0f, 15.0f);
        // Grayscale avoids colored fringes on transparent overlay textures.
        for (auto* target : {osdCacheTarget, volumeCacheTarget, audioCacheTarget})
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        const auto& theme = llcv::overlay_ui::kPalette;
        const auto overlayColor = [](COLORREF value) { return llcv::overlay_ui::Color(value); };
        const auto brush = [&](ID2D1RenderTarget* target, D2D1_COLOR_F value,
                               ID2D1SolidColorBrush** output) {
            if (SUCCEEDED(hr)) hr = target->CreateSolidColorBrush(value, output);
        };
        brush(osdCacheTarget, llcv::overlay_ui::Background(hdrOutput), &osdCacheBackgroundBrush);
        brush(osdCacheTarget, overlayColor(theme.kText), &osdCacheTextBrush);
        brush(osdCacheTarget, overlayColor(theme.kEdge), &osdCacheEdgeBrush);
        brush(volumeCacheTarget, llcv::overlay_ui::Background(hdrOutput), &volumeCacheBackgroundBrush);
        brush(volumeCacheTarget, overlayColor(theme.kText), &volumeCacheTextBrush);
        brush(volumeCacheTarget, overlayColor(theme.kWarning), &volumeCacheBoostBrush);
        brush(volumeCacheTarget, overlayColor(theme.kEdge), &volumeCacheBarBackgroundBrush);
        brush(volumeCacheTarget, overlayColor(theme.kAccent), &volumeCacheBarBrush);
        brush(audioCacheTarget, llcv::overlay_ui::Background(hdrOutput, 0.92f), &audioCacheBackgroundBrush);
        brush(audioCacheTarget, overlayColor(theme.kText), &audioCacheTextBrush);
        brush(audioCacheTarget, overlayColor(theme.kEdge), &audioCacheBarBackgroundBrush);
        brush(audioCacheTarget, overlayColor(theme.kAccent), &audioCacheBarBrush);
        brush(audioCacheTarget, overlayColor(theme.kHover), &audioCacheHighlightBrush);
        brush(audioCacheTarget, overlayColor(theme.kDanger), &audioCacheClipBrush);
        brush(audioCacheTarget, overlayColor(theme.kWarning), &audioCacheBoostBrush);
        brush(audioCacheTarget, overlayColor(theme.kControl), &audioCacheCardBrush);
        brush(audioCacheTarget, overlayColor(theme.kEdge), &audioCacheEdgeBrush);
        brush(audioCacheTarget, overlayColor(theme.kSecondary), &audioCacheSecondaryBrush);
        if (FAILED(hr)) return hr;

        return configureVideoOutput(width, height);
    }

    // A window/output geometry change does not invalidate the D3D device,
    // upload ring, shaders, fonts, or fixed-size overlay caches. Keep those
    // resources alive; only device-loss recovery calls initialize again.
    HRESULT resizeOutput(HWND hwnd) {
        if (!device || !context || !swapChain || captureWidth <= 0 || captureHeight <= 0)
            return E_UNEXPECTED;
        const uint64_t generation = g_outputConfigurationGeneration.load(std::memory_order_acquire);
        const uint64_t revision = g_presentationRevision.load(std::memory_order_acquire);
        const HWND outputWindow=PresentationWindow(hwnd);
        RECT client{};
        if (!GetClientRect(outputWindow, &client)) {
            const DWORD error = GetLastError();
            return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
        }
        const bool minimized = ExclusiveOutputSelected() &&
            g_presentationMinimized.load(std::memory_order_acquire);
        const UINT width = minimized ? outputWidth
            : static_cast<UINT>((std::max)(1L, client.right - client.left));
        const UINT height = minimized ? outputHeight
            : static_cast<UINT>((std::max)(1L, client.bottom - client.top));
        DXGI_SWAP_CHAIN_DESC1 description{};
        HRESULT hr = swapChain->GetDesc1(&description);
        if (FAILED(hr)) return hr;

        context->ClearState(); // release indirect bindings to the back buffer
        SafeRelease(outputView);
        SafeRelease(backBufferRenderTarget);
        SafeRelease(backBuffer);
        SafeRelease(hdrOverlayBackgroundView);
        SafeRelease(hdrOverlayBackground);
        for (auto& view : inputViews) SafeRelease(view);
        SafeRelease(processor);
        SafeRelease(enumerator);
        bool transitioned=false;
        if (ExclusiveOutputSelected()) {
            BOOL actual=FALSE;
            hr=swapChain->GetFullscreenState(&actual,nullptr);
            if(FAILED(hr))return hr;
            // DXGI can leave fullscreen itself on focus loss. That still
            // requires a flip-chain ResizeBuffers even if client size agrees.
            transitioned=exclusiveActive!=(actual!=FALSE);
            const bool wanted=llcv::exclusive_presentation::Wanted({true,
                g_fullscreen.load(std::memory_order_acquire),
                g_presentationActive.load(std::memory_order_acquire),
                g_presentationMinimized.load(std::memory_order_acquire),
                g_presentationBlocked.load(std::memory_order_acquire),
                g_presentationClosing.load(std::memory_order_acquire)});
            if(exclusiveAttempts.Begin(revision,wanted,actual!=FALSE)) {
                const auto change=swapChain->SetFullscreenState(wanted?TRUE:FALSE,nullptr);
                BOOL current=FALSE;
                const auto query=swapChain->GetFullscreenState(&current,nullptr);
                transitioned=transitioned || (SUCCEEDED(query) && actual!=current);
                actual=current;
                fwprintf(stderr,L"[video-exclusive] requested=%d actual=%d change=0x%08X query=0x%08X revision=%llu\n",
                    wanted,actual,static_cast<unsigned>(change),static_cast<unsigned>(query),revision);
                if(change==DXGI_ERROR_DEVICE_REMOVED || change==DXGI_ERROR_DEVICE_RESET)return change;
                if(FAILED(query))return query;
                if(!wanted && actual)
                    return FAILED(change) ? change : DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
                // An unavailable exclusive mode remains usable as normal flip
                // output. Retry only after new user/focus/display intent.
            }
            exclusiveActive=actual!=FALSE;
            g_videoExclusiveActive.store(exclusiveActive, std::memory_order_release);
            g_videoTearing.store(llcv::presentation::PresentFlags(
                g_settings.presentationMode, exclusiveActive, allowTearing) != 0,
                std::memory_order_release);
        }
        if (transitioned || width != outputWidth || height != outputHeight) {
            hr = swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, description.Flags);
            if (FAILED(hr)) return hr;
        }
        outputWidth = width; outputHeight = height;
        fullscreenDisplaySize = g_fullscreen.load(std::memory_order_acquire);
        sharpScalingActive = false;
        cachedOverlayGeneration = 0; cachedOverlayMask = 0;
        occluded = false; nextOcclusionTestMs = 0;
        if (hdrOutput) {
            auto colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
            if (scrgbOutput) colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
#endif
            IDXGISwapChain3* chain3 = nullptr;
            hr = swapChain->QueryInterface(IID_PPV_ARGS(&chain3));
            UINT support = 0;
            if (SUCCEEDED(hr)) hr = chain3->CheckColorSpaceSupport(colorSpace, &support);
            if (SUCCEEDED(hr) && !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
                hr = DXGI_ERROR_UNSUPPORTED;
            if (SUCCEEDED(hr)) hr = chain3->SetColorSpace1(colorSpace);
            SafeRelease(chain3);
            if (FAILED(hr)) return hr;
            RefreshHdrDisplayStatus(hwnd);
        }
        hr = createVideoOutputResources(captureWidth, captureHeight, captureFps);
        if (SUCCEEDED(hr)) hr = configureVideoOutput(captureWidth, captureHeight);
        if (SUCCEEDED(hr)) {
            outputConfigurationGeneration = generation;
            presentationRevision=revision;
            // Open owned UI only after the output owner has completed the
            // windowed transition, never concurrently with SetFullscreenState.
            if (ExclusiveOutputSelected() && !exclusiveActive &&
                revision == g_presentationRevision.load(std::memory_order_acquire) &&
                g_presentationBlocked.load(std::memory_order_acquire)) {
                UINT_PTR serial = 0;
                const auto action = TakeDeferredPresentationUi(serial);
                if (action != DeferredPresentationUi::None)
                    PostMessageW(outputWindow, WM_PRESENTATION_UI_READY, static_cast<WPARAM>(action),
                        static_cast<LPARAM>(serial));
            }
        }
        return hr;
    }

    HRESULT createVideoOutputResources(int width, int height, int fps) {
        HRESULT hr = S_OK;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        if (!scrgbOutput) {
#endif
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
        if (hdrOutput) {
            ID3D11VideoProcessorEnumerator1* enumerator1 = nullptr;
            hr = enumerator->QueryInterface(IID_PPV_ARGS(&enumerator1));
            BOOL supported = FALSE;
            if (SUCCEEDED(hr)) hr = enumerator1->CheckVideoProcessorFormatConversion(
                inputFormat, hdrInputColorSpace, backBufferFormat,
                DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &supported);
            SafeRelease(enumerator1);
            if (FAILED(hr) || !supported) {
                fwprintf(stderr, L"[hdr] exact P010/PQ -> RGB10/PQ conversion unsupported (input color space %u, 0x%08X).\n",
                         static_cast<unsigned>(hdrInputColorSpace), static_cast<unsigned>(hr));
                return FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED;
            }
        }
        if (rgbInput) {
            // RGB as a Video Processor input is less commonly supported than
            // YUV; confirm the exact input/output format+colorspace pair the
            // SDR branch below will configure, instead of discovering a Blt
            // failure per frame later.
            ID3D11VideoProcessorEnumerator1* enumerator1 = nullptr;
            hr = enumerator->QueryInterface(IID_PPV_ARGS(&enumerator1));
            BOOL supported = FALSE;
            if (SUCCEEDED(hr)) hr = enumerator1->CheckVideoProcessorFormatConversion(
                inputFormat, rgbInputColorSpace, backBufferFormat,
                DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &supported);
            SafeRelease(enumerator1);
            if (FAILED(hr) || !supported) {
                fwprintf(stderr, L"[video] exact RGB -> RGB8 conversion unsupported (input color space %u, 0x%08X).\n",
                         static_cast<unsigned>(rgbInputColorSpace), static_cast<unsigned>(hr));
                return FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED;
            }
            const bool bottomUp = !lastTopDown;
            D3D11_VIDEO_PROCESSOR_CAPS vpCaps{};
            hr = enumerator->GetVideoProcessorCaps(&vpCaps);
            if (FAILED(hr)) return hr;
            if (bottomUp && (vpCaps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_MIRROR) == 0) {
                fwprintf(stderr, L"[video] bottom-up RGB input needs Video Processor mirror "
                                  L"support, which this driver/GPU does not advertise; refusing "
                                  L"to render the frame upside down.\n");
                return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
            }
            if (!videoContext1) return E_NOINTERFACE;
            // Source clipping is supplied in the underlying texture coordinates.
            videoContext1->VideoProcessorSetStreamMirror(
                processor, 0, bottomUp, FALSE, bottomUp);
        }
        // Keep driver-selected image enhancements out of both SDR and HDR.
        // Only the user's explicit scaling filter may alter edge sharpness.
        videoContext->VideoProcessorSetStreamAutoProcessingMode(processor, 0, FALSE);

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
        inputDesc.FourCC = 0;
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        for (UINT i = 0; i < kUploadSurfaceCount; ++i) {
            hr = videoDevice->CreateVideoProcessorInputView(
                nv12Textures[i], enumerator, &inputDesc, &inputViews[i]);
            if (FAILED(hr)) return hr;
        }
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        } else {
            hr = scrgbPipeline.Initialize(device, nv12Textures);
            if (FAILED(hr)) {
                LogHr(L"Direct P010/scRGB shader initialization", hr);
                return hr;
            }
            fwprintf(stderr, L"[hdr] scRGB prototype uses bilinear scaling; VP sharp filter is not applied.\n");
        }
#endif
        hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) return hr;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        if (!scrgbOutput) {
#endif
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = videoDevice->CreateVideoProcessorOutputView(
            backBuffer, enumerator, &outputDesc, &outputView);
        if (FAILED(hr)) return hr;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        }
#endif
        hr = device->CreateRenderTargetView(
            backBuffer, nullptr, &backBufferRenderTarget);
        if (FAILED(hr)) return hr;
        return S_OK;
    }

    HRESULT configureVideoOutput(int width, int height) {
        skipRedundantBorderClear=false;
        HRESULT hr = S_OK;
        RECT sourceRect{0, 0, width, height};
        RECT outputRect{0, 0, static_cast<LONG>(outputWidth),
                        static_cast<LONG>(outputHeight)};
        const auto& display = CurrentVideoPreset();
        const bool sourceSize = fullscreenDisplaySize &&
            g_fullscreenSourceSize.load(std::memory_order_acquire);
        const RECT videoRect = llcv::video_placement::Destination(width, height,
            static_cast<int>(outputWidth), static_cast<int>(outputHeight),
            fullscreenDisplaySize, sourceSize ? width : display.width,
            sourceSize ? height : display.height, g_settings.fullscreenVideoPosition);
        pixelPerfectBorders = !EqualRect(&videoRect, &outputRect);
        cmaaNisAppliedRequest=g_cmaaNisRequest.load(std::memory_order_acquire);
        bool cmaaNisRequested=cmaaNisAppliedRequest>=0;
#ifdef LLCV_SPATIAL_PROTOTYPE
        // A private comparison explicitly owns its selected filter.
        if(llcv::spatial::EnvironmentMode()!=llcv::spatial::Mode::Disabled) cmaaNisRequested=false;
#endif
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        if(scrgbOutput) cmaaNisRequested=false;
#endif
        cmaaNisStatus=cmaaNisPipeline.Configure(device,videoDevice,enumerator,cmaaNisRequested,
            hdrOutput,static_cast<UINT>(width),static_cast<UINT>(height),videoRect,adapterVendor,
            float(std::max(0,cmaaNisAppliedRequest))/100.0f);
        if(FAILED(cmaaNisStatus)) {
            cmaaNisPipeline.Reset();
            fwprintf(stderr,L"[upscaling] CMAA2/NIS unavailable 0x%08X; using Video Processor\n",unsigned(cmaaNisStatus));
        }
        if(cmaaNisRequested)
            fwprintf(stderr,L"[upscaling] CMAA2 -> NIS active=%d HDR=%d sharpness=%.2f\n",
                cmaaNisPipeline.Enabled(),hdrOutput,double(cmaaNisAppliedRequest)/100.0);
        cachedOverlayGeneration=0;
        if(cmaaNisPipeline.Enabled() || sharpScalingActive) {
            videoContext->VideoProcessorSetStreamFilter(processor,0,
                D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT,FALSE,0);
            sharpScalingActive=false;
        }
#ifdef LLCV_SPATIAL_PROTOTYPE
        const auto spatialMode = llcv::spatial::EnvironmentMode();
        const HRESULT spatialHr = spatialPipeline.Configure(device, videoDevice, enumerator,
            spatialMode, hdrOutput, static_cast<UINT>(width), static_cast<UINT>(height),
            videoRect, adapterVendor,llcv::spatial::EnvironmentNisSharpness(),llcv::spatial::EnvironmentAaMode());
        if (FAILED(spatialHr)) {
            spatialPipeline.Reset();
            fwprintf(stderr,L"[spatial] setup failed 0x%08X; using original Video Processor\n",unsigned(spatialHr));
        }
        if (spatialPipeline.Enabled()) {
            videoContext->VideoProcessorSetStreamFilter(processor,0,
                D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT,FALSE,0);
            sharpScalingActive=false;
        }
        fwprintf(stderr,L"[spatial] requested=%hs active=%hs (private SDR experiment)\n",
            llcv::spatial::Name(spatialMode),llcv::spatial::Name(spatialPipeline.ActiveMode()));
        fwprintf(stderr,L"[spatial] aa=%d sharpness=%.2f\n",
            int(spatialPipeline.ActiveAaMode()),double(llcv::spatial::EnvironmentNisSharpness()));
#endif
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        if (scrgbOutput) {
            scrgbPipeline.Configure(context, static_cast<UINT>(width), static_cast<UINT>(height),
                outputWidth, outputHeight, sourceRect, videoRect,
                hdrInputColorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020);
        } else {
#endif
        const RECT storageSourceRect = VideoSourceRectForStorage(sourceRect, height, rgbInput && !lastTopDown);
        videoContext->VideoProcessorSetStreamSourceRect(processor, 0, TRUE,
                                                        &storageSourceRect);
        const bool scaled = videoRect.right - videoRect.left != width ||
                            videoRect.bottom - videoRect.top != height;
#ifdef LLCV_VSR_FEATURE
        vsrInputWidth = static_cast<UINT>(width);
        vsrInputHeight = static_cast<UINT>(height);
        vsrDisplayWidth = static_cast<UINT>(videoRect.right - videoRect.left);
        vsrDisplayHeight = static_cast<UINT>(videoRect.bottom - videoRect.top);
        vsrEligible = llcv::vsr::Eligible(adapterVendor, inputFormat,
            hdrOutput, static_cast<UINT>(width), static_cast<UINT>(height),
            static_cast<UINT>(videoRect.right - videoRect.left),
            static_cast<UINT>(videoRect.bottom - videoRect.top));
#ifdef LLCV_VSR_EXPERIMENT
        vsrWarmupFrames = static_cast<unsigned>(captureFps) * 5u;
#endif
        hr = applyVsrMode(true);
        if (FAILED(hr)) return hr;
#endif
        if (scaled && !SourcePixelPerfect() && !cmaaNisPipeline.Enabled() &&
#ifdef LLCV_SPATIAL_PROTOTYPE
            !spatialPipeline.Enabled() &&
#endif
            g_settings.scalingMode == ScalingMode::Sharp) {
            D3D11_VIDEO_PROCESSOR_FILTER_RANGE sharpness{};
            if (SUCCEEDED(enumerator->GetVideoProcessorFilterRange(
                    D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, &sharpness)) &&
                sharpness.Maximum > sharpness.Minimum) {
                const int value = std::clamp(sharpness.Default +
                    (sharpness.Maximum - sharpness.Default) / 2,
                    sharpness.Minimum, sharpness.Maximum);
                videoContext->VideoProcessorSetStreamFilter(processor, 0,
                    D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, TRUE, value);
                sharpScalingActive = true;
                fwprintf(stderr, L"[video] scaling: sharp (Video Processor, value %d)\n", value);
            } else {
                fwprintf(stderr, L"[video] sharp scaling unavailable; using smooth scaling.\n");
            }
        }
        videoContext->VideoProcessorSetStreamDestRect(processor, 0, TRUE,
                                                      &videoRect);
        videoContext->VideoProcessorSetOutputTargetRect(processor, TRUE,
                                                        &outputRect);
        llcv::video_clear::State clear{};
        clear.outputWidth=outputWidth;clear.outputHeight=outputHeight;
        clear.outputFormat=backBufferFormat;clear.hdrOutput=hdrOutput;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        clear.scrgbOutput=scrgbOutput;
#endif
        clear.videoProcessorPath=!clear.scrgbOutput;
        clear.outputTargetEnabled=true;clear.outputTarget=outputRect;
        clear.sourceWidth=width;clear.sourceHeight=height;
        clear.source=sourceRect;clear.destination=videoRect;
        clear.singleOpaqueVideoStream=true;clear.opaqueOutput=true;
        BOOL backgroundYuv=TRUE;D3D11_VIDEO_COLOR background{};
        videoContext->VideoProcessorGetOutputBackgroundColor(processor,&backgroundYuv,&background);
        clear.backgroundKnown=true;clear.backgroundIsYuv=backgroundYuv!=FALSE;
        clear.backgroundR=background.RGBA.R;clear.backgroundG=background.RGBA.G;
        clear.backgroundB=background.RGBA.B;clear.backgroundA=background.RGBA.A;
        skipRedundantBorderClear=llcv::video_clear::CanSkipBorderClear(clear);
        if(cmaaNisPipeline.Enabled()) skipRedundantBorderClear=false;
#ifdef LLCV_SPATIAL_PROTOTYPE
        // Shader draws only the video rectangle; black margins must be cleared.
        if (spatialPipeline.Enabled()) skipRedundantBorderClear=false;
#endif
        if (hdrOutput) {
            // Preserve PQ video values; use the validated range/chroma tuple
            // instead of the legacy SDR color-space bitfield.
            videoContext1->VideoProcessorSetStreamColorSpace1(
                processor, 0, hdrInputColorSpace);
            videoContext1->VideoProcessorSetOutputColorSpace1(
                processor, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        } else if (rgbInput) {
            videoContext1->VideoProcessorSetStreamColorSpace1(processor, 0, rgbInputColorSpace);
            videoContext1->VideoProcessorSetOutputColorSpace1(processor, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
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
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{};
            outputColor.RGB_Range = 0; // Full-range RGB back buffer, not studio RGB.
            videoContext->VideoProcessorSetOutputColorSpace(processor, &outputColor);
        }
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        }
#endif
        g_hdrOutputActive.store(hdrOutput, std::memory_order_release);
        return S_OK;
    }


    void upload(const BYTE* pixels, UINT32 stride) {
#ifdef LLCV_VSR_EXPERIMENT
        const auto uploadStart = llcv::vsr::Timing::Clock::now();
#endif
        // Rotate upload targets to reduce CPU/GPU contention without an
        // application frame queue. Under GPU overload the driver may still
        // synchronize reuse; a three-surface ring is not a completion fence.
        activeUploadSurface = nextUploadSurface;
        nextUploadSurface = (nextUploadSurface + 1) % kUploadSurfaceCount;
        ID3D11Texture2D* target = uyvyInput ? uyvyUploadTextures[activeUploadSurface]
            : rgb24Input ? rgb24UploadTextures[activeUploadSurface] : nv12Textures[activeUploadSurface];
        if (context1) {
            context1->UpdateSubresource1(target, 0, nullptr, pixels, stride, 0,
                                         D3D11_COPY_DISCARD);
        } else {
            context->UpdateSubresource(target, 0, nullptr, pixels, stride, 0);
        }
        // UYVY and RGB24 each use one format-specific GPU conversion. The existing ring size,
        // raw capture sample, and all other formats remain unchanged.
        if (uyvyInput)
            uyvyRepacker.Dispatch(context, uyvyUploadViews[activeUploadSurface], uyvyViews[activeUploadSurface],
                static_cast<UINT>(captureWidth), static_cast<UINT>(captureHeight));
        if (rgb24Input)
            rgb24Repacker.Dispatch(context, rgb24UploadViews[activeUploadSurface], rgb24Views[activeUploadSurface],
                static_cast<UINT>(captureWidth), static_cast<UINT>(captureHeight));
#ifdef LLCV_VSR_EXPERIMENT
        vsrTiming.AddUpload(std::chrono::duration<double,std::milli>(
            llcv::vsr::Timing::Clock::now()-uploadStart).count());
#endif
    }

    // Each generation may update visible panels at different times. In
    // particular, F3/audio-meter updates must not rebuild hidden Tab/HUD text.
    HRESULT refreshOverlayLayouts(bool diagnostics = true, bool notification = true,
                                  bool audio = true) {
        const unsigned visibleMask = (diagnostics ? 1u : 0u) |
            (notification ? 2u : 0u) | (audio ? 4u : 0u);
        if (!visibleMask) return S_OK;
        const uint64_t generation = g_overlayGeneration.load(std::memory_order_acquire);
        const unsigned refreshMask = visibleMask &
            ~(cachedOverlayGeneration == generation ? cachedOverlayMask : 0u);
        if (!refreshMask) return S_OK;
        HRESULT hr = S_OK;
        const int overlayMaster = g_volumePercent.load(std::memory_order_acquire);
        const bool playbackMuted = g_playbackMuted.load(std::memory_order_relaxed);
        const bool masterBoosted = g_settings.allowVolumeBoost && overlayMaster > 100;

        if (refreshMask & 1u) {
            SafeRelease(osdTextLayout);
            // Keep the diagnostics panel at a stable size. Device names are kept
            // verbatim; the output device has its own line so long names do not
            // need an ellipsis just to share a line with the audio mode.
            std::wstring osdText;
            osdText = BuildRuntimeOsdText(
                static_cast<int>(outputWidth), static_cast<int>(outputHeight));
#ifdef LLCV_VSR_FEATURE
            // Keep VSR beside Input/Display. These are applied renderer state and
            // actual video rectangles, not UI intent or a claim of driver activation.
            size_t videoEnd = 0;
            for (int line = 0; line < 5; ++line) {
                const auto end = osdText.find(L'\n', videoEnd);
                if (end == std::wstring::npos) break;
                videoEnd = end + 1;
            }
            osdText.insert(videoEnd, vsrOsdLine());
#endif
            hr = dwriteFactory->CreateTextLayout(
                osdText.c_str(), static_cast<UINT32>(osdText.size()),
                osdTextFormat, kOsdTextWidth, kOsdTextHeight, &osdTextLayout);
            if (FAILED(hr)) return hr;
            const auto headingEnd = osdText.find(L'\n');
            if (headingEnd != std::wstring::npos) {
                osdTextLayout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD,
                    DWRITE_TEXT_RANGE{0, static_cast<UINT32>(headingEnd)});
            }
            osdCacheTarget->BeginDraw();
            osdCacheTarget->Clear(D2D1::ColorF(0, 0.0f));
            osdCacheTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(
                                      0.0f, 0.0f,
                                      static_cast<float>(kOsdOverlayWidth),
                                      static_cast<float>(kOsdOverlayHeight)),
                                  12.0f, 12.0f),
                osdCacheBackgroundBrush);
            osdCacheTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f,
                    kOsdOverlayWidth - 0.5f, kOsdOverlayHeight - 0.5f), 12, 12),
                osdCacheEdgeBrush, 1.0f);
            osdCacheTarget->DrawTextLayout(
                D2D1::Point2F(16.0f, 12.0f), osdTextLayout,
                osdCacheTextBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            hr = osdCacheTarget->EndDraw();
            if (FAILED(hr)) return hr;
        }
        if (refreshMask & 2u) {
            SafeRelease(volumeTextLayout);
            const TransientHudContent hudContent =
                g_transientHudContent.load(std::memory_order_acquire);
            wchar_t volumeText[96]{};
#ifdef LLCV_VSR_FEATURE
            if (hudContent >= TransientHudContent::VsrPending) {
                const wchar_t* ko = L"VSR 비교: F6\n현재: 기본 출력";
                const wchar_t* en = L"VSR compare: F6\nOriginal output";
                switch (hudContent) {
                case TransientHudContent::VsrPending: ko=L"VSR 전환 대기"; en=L"VSR switch pending"; break;
                case TransientHudContent::VsrOn: ko=L"VSR ON\nF6: 끄기"; en=L"VSR ON\nF6: OFF"; break;
                case TransientHudContent::VsrOff: ko=L"VSR OFF\nF6: 켜기"; en=L"VSR OFF\nF6: ON"; break;
                case TransientHudContent::VsrUnavailable: ko=L"VSR 적용 불가\n입력·표시 조건 확인"; en=L"VSR unavailable\nCheck format / size"; break;
                case TransientHudContent::VsrRejected: ko=L"VSR 요청 거부\nOFF로 복귀"; en=L"VSR rejected\nBack to OFF"; break;
                case TransientHudContent::VsrFailed: ko=L"VSR 전환 실패\n진단 로그 확인"; en=L"VSR switch failed\nSee diagnostic log"; break;
                case TransientHudContent::CmaaNisPending: ko=L"CMAA2 → NIS\n전환 대기"; en=L"CMAA2 → NIS\nSwitch pending"; break;
                case TransientHudContent::CmaaNisOn: ko=L"CMAA2 → NIS ON\nF6: 끄기"; en=L"CMAA2 → NIS ON\nF6: OFF"; break;
                case TransientHudContent::CmaaNisOff: ko=L"CMAA2 → NIS OFF\nF6: 켜기"; en=L"CMAA2 → NIS OFF\nF6: ON"; break;
                case TransientHudContent::CmaaNisHdrBypass: ko=L"CMAA2 → NIS 적용 제외\nSDR 전용"; en=L"CMAA2 → NIS bypass\nSDR only"; break;
                case TransientHudContent::CmaaNisScaleBypass: ko=L"CMAA2 → NIS 적용 제외\n확대 배율 확인"; en=L"CMAA2 → NIS bypass\nCheck scale"; break;
                case TransientHudContent::CmaaNisFailed: ko=L"CMAA2 → NIS 사용 불가\n기본 출력 유지"; en=L"CMAA2 → NIS unavailable\nUsing original output"; break;
                default: break;
                }
                wcscpy_s(volumeText, keys::Expand(IsEnglishUi() ? en : ko,g_settings.shortcuts).c_str());
            } else
#endif
            if (hudContent >= TransientHudContent::ScreenshotPending) {
                const wchar_t* ko = L"스크린샷 실패\n진단 로그 확인";
                const wchar_t* en = L"Screenshot failed\nCheck diagnostic log";
                switch (hudContent) {
                case TransientHudContent::ScreenshotPending: ko=L"스크린샷 처리 중"; en=L"Saving screenshot"; break;
                case TransientHudContent::ScreenshotBusy: ko=L"촬영 대기 중\n잠시 후 다시 시도"; en=L"Not ready / busy\nPlease try again"; break;
                case TransientHudContent::ScreenshotSaved: ko=L"PNG 저장 완료"; en=L"PNG saved"; break;
                case TransientHudContent::ScreenshotCopied: ko=L"PNG 저장 완료\n클립보드 복사 완료"; en=L"PNG saved\nCopied to clipboard"; break;
                case TransientHudContent::ScreenshotClipboardFailed: ko=L"PNG 저장 완료\n클립보드 복사 실패"; en=L"PNG saved\nClipboard failed"; break;
                case TransientHudContent::ScreenshotTimeout: ko=L"촬영 시간 초과\n입력 영상 확인"; en=L"Screenshot timed out\nCheck video input"; break;
                default: break;
                }
                wcscpy_s(volumeText, IsEnglishUi() ? en : ko);
            } else if (hudContent == TransientHudContent::OneToOne) {
                const auto& video = CurrentCapturePreset();
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
                swprintf_s(volumeText, playbackMuted
                    ? (IsEnglishUi() ? L"Muted  %d%%" : L"음소거  %d%%") : UI_TEXT(L"음량  %d%%"),
                           overlayMaster);
            }
            hr = dwriteFactory->CreateTextLayout(
                volumeText, static_cast<UINT32>(wcslen(volumeText)),
                volumeTextFormat, 228.0f, 62.0f, &volumeTextLayout);
            if (FAILED(hr)) return hr;
            if (hudContent != TransientHudContent::Volume)
                volumeTextLayout->SetFontSize(20.0f,
                    DWRITE_TEXT_RANGE{0, static_cast<UINT32>(wcslen(volumeText))});
#ifdef LLCV_VSR_FEATURE
            if (hudContent >= TransientHudContent::CmaaNisPending)
                volumeTextLayout->SetFontSize(18.0f,
                    DWRITE_TEXT_RANGE{0,static_cast<UINT32>(wcslen(volumeText))});
            if ((hudContent==TransientHudContent::VsrOn || hudContent==TransientHudContent::VsrOff ||
                 hudContent==TransientHudContent::CmaaNisOn || hudContent==TransientHudContent::CmaaNisOff) &&
                keys::Label(g_settings.shortcuts[static_cast<size_t>(keys::Action::Vsr)]).size()>12)
                volumeTextLayout->SetFontSize(16.0f,
                    DWRITE_TEXT_RANGE{0,static_cast<UINT32>(wcslen(volumeText))});
#endif
            // Keep the existing GPU texture/quad; only the painted panel follows
            // the cached text height. Volume retains space for its gain bar.
            DWRITE_TEXT_METRICS hudMetrics{};
            hr = volumeTextLayout->GetMetrics(&hudMetrics);
            if (FAILED(hr)) return hr;
            volumePanelHeight = hudContent == TransientHudContent::Volume ? 82.0f
                : std::clamp(std::ceil(hudMetrics.height) + 20.0f, 40.0f, 82.0f);
            const bool bottomHud = g_settings.volumeHudPosition == VolumeHudPosition::BottomLeft ||
                g_settings.volumeHudPosition == VolumeHudPosition::BottomRight;
            // Bottom notifications keep the same screen-edge margin even when
            // shorter. Unused texture rows are transparent, never stretched.
            volumePanelTop = bottomHud ? 82.0f - volumePanelHeight : 0.0f;
            volumeCacheTarget->BeginDraw();
            volumeCacheTarget->Clear(D2D1::ColorF(0, 0.0f));
            volumeCacheTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(0.0f, volumePanelTop, 260.0f, volumePanelTop + volumePanelHeight),
                                  12.0f, 12.0f),
                volumeCacheBackgroundBrush);
            volumeCacheTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(0.5f, volumePanelTop + 0.5f,
                    259.5f, volumePanelTop + volumePanelHeight - 0.5f), 12, 12),
                volumeCacheBarBackgroundBrush, 1.0f);
            volumeCacheTarget->DrawTextLayout(
                D2D1::Point2F(16.0f, volumePanelTop +
                    (hudContent == TransientHudContent::Volume ? 6.0f : 10.0f)), volumeTextLayout,
                hudContent == TransientHudContent::Volume && masterBoosted
                    ? volumeCacheBoostBrush : volumeCacheTextBrush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (hudContent == TransientHudContent::Volume) {
                const D2D1_RECT_F volumeBarBackground =
                    D2D1::RectF(16.0f, 57.0f, 244.0f, 66.0f);
                volumeCacheTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(volumeBarBackground, 4, 4), volumeCacheBarBackgroundBrush);
                D2D1_RECT_F volumeBar = volumeBarBackground;
                const int volumeBarMaximum = g_settings.allowVolumeBoost
                    ? kMaximumVolumePercent : 100;
                volumeBar.right = volumeBar.left +
                    (volumeBarBackground.right - volumeBarBackground.left) *
                        (playbackMuted ? 0 : overlayMaster) /
                        static_cast<float>(volumeBarMaximum);
                if (volumeBar.right > volumeBar.left) {
                    volumeCacheTarget->FillRoundedRectangle(D2D1::RoundedRect(volumeBar, 4, 4),
                                                           volumeCacheBarBrush);
                }
            }
            hr = volumeCacheTarget->EndDraw();
            if (FAILED(hr)) return hr;
        }
        if (refreshMask & 4u) {
            const int maximum = g_settings.allowVolumeBoost
                ? kMaximumVolumePercent : 100;
            constexpr int channelMaximum = 100;
            const int master = overlayMaster;
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
            const auto panel = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f,
                kAudioOsdWidth - 0.5f, kAudioOsdHeight - 0.5f), 12, 12);
            audioCacheTarget->FillRoundedRectangle(panel, audioCacheBackgroundBrush);
            audioCacheTarget->DrawRoundedRectangle(panel, audioCacheEdgeBrush, 1);
            const auto text = [&](const wchar_t* value, IDWriteTextFormat* format,
                                  D2D1_RECT_F rect, ID2D1Brush* brush) {
                audioCacheTarget->DrawTextW(value, static_cast<UINT32>(wcslen(value)),
                    format, rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            };
            const auto bar = [&](D2D1_RECT_F rect, int percent, int max) {
                audioCacheTarget->FillRoundedRectangle(D2D1::RoundedRect(rect, 3, 3),
                    audioCacheBarBackgroundBrush);
                rect.right = rect.left + (rect.right - rect.left) *
                    std::clamp(percent, 0, max) / static_cast<float>(max);
                if (rect.right > rect.left)
                    audioCacheTarget->FillRoundedRectangle(D2D1::RoundedRect(rect, 3, 3),
                        audioCacheBarBrush);
            };
            text(IsEnglishUi() ? L"Audio" : L"오디오", audioTextFormat,
                D2D1::RectF(16, 4, 80, 30), audioCacheTextBrush);
            // Setting-dependent limit is visible without consuming another row.
            audioSmallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            const auto audioShortcut=keys::Expand(maximum > 100 ? (IsEnglishUi() ? L"MAX 200% · F3" : L"최대 200% · F3") : L"F3",g_settings.shortcuts);
            text(audioShortcut.c_str(),
                audioSmallFormat, D2D1::RectF(86, 4, 320, 30), audioCacheSecondaryBrush);
            audioSmallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            const auto masterCard = D2D1::RoundedRect(D2D1::RectF(16, 32, 320, 72), 8, 8);
            audioCacheTarget->FillRoundedRectangle(masterCard,
                hovered == 3 ? audioCacheHighlightBrush : audioCacheCardBrush);
            const auto muteBounds = llcv::audio_osd::kMuteButton;
            audioCacheTarget->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(
                static_cast<float>(muteBounds.left), static_cast<float>(muteBounds.top),
                static_cast<float>(muteBounds.right), static_cast<float>(muteBounds.bottom)), 5, 5),
                hovered == static_cast<int>(llcv::audio_osd::HitTarget::Mute)
                    ? audioCacheHighlightBrush : audioCacheCardBrush);
            audioCacheTarget->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(
                static_cast<float>(muteBounds.left), static_cast<float>(muteBounds.top),
                static_cast<float>(muteBounds.right), static_cast<float>(muteBounds.bottom)), 5, 5),
                audioCacheEdgeBrush, 1);
            constexpr float speakerSize = 20.0f;
            constexpr float speakerScale = speakerSize / llcv::ui::kSpeakerIconSize;
            const float speakerLeft = (muteBounds.left + muteBounds.right - speakerSize) * 0.5f;
            const float speakerTop = (muteBounds.top + muteBounds.bottom - speakerSize) * 0.5f;
            llcv::ui::DrawSpeakerIcon(playbackMuted, [&](int x1,int y1,int x2,int y2) {
                audioCacheTarget->DrawLine(D2D1::Point2F(speakerLeft+x1*speakerScale,speakerTop+y1*speakerScale),
                    D2D1::Point2F(speakerLeft+x2*speakerScale,speakerTop+y2*speakerScale),audioCacheTextBrush,1.5f);
            });
            text(playbackMuted ? (IsEnglishUi() ? L"Muted" : L"음소거")
                               : (IsEnglishUi() ? L"Master" : L"마스터 음량"), audioTextFormat,
                D2D1::RectF(64, 33, 214, 62), audioCacheTextBrush);
            wchar_t masterText[32]{};
            swprintf_s(masterText, L"%d%%", master);
            audioValueFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            text(masterText, audioValueFormat, D2D1::RectF(220, 33, 308, 62),
                masterBoosted ? audioCacheBoostBrush : audioCacheTextBrush);
            bar(D2D1::RectF(28, 64, 308, 68), playbackMuted ? 0 : master, maximum);

            const auto drawChannel = [&](int channel, const wchar_t* label,
                                         int percent, double peakDb, float x0, float x1) {
                const auto card = D2D1::RoundedRect(D2D1::RectF(x0, 84, x1, 168), 8, 8);
                audioCacheTarget->FillRoundedRectangle(card,
                    hovered == channel ? audioCacheHighlightBrush : audioCacheCardBrush);
                audioCacheTarget->DrawRoundedRectangle(card, audioCacheEdgeBrush, 1);
                text(label, audioTextFormat, D2D1::RectF(x0 + 12, 90, x0 + 42, 123),
                    audioCacheTextBrush);
                wchar_t percentText[24]{}, levelText[32]{};
                swprintf_s(percentText, L"%d%%", percent);
                swprintf_s(levelText, L"%.1f dBFS", peakDb);
                text(percentText, audioValueFormat, D2D1::RectF(x0 + 44, 90, x1 - 12, 123),
                    audioCacheTextBrush);
                text(levelText, audioSmallFormat, D2D1::RectF(x0 + 12, 127, x1 - 12, 149),
                    audioCacheSecondaryBrush);
                // Preserve the existing gain bar semantics and hit regions.
                bar(D2D1::RectF(x0 + 12, 156, x1 - 12, 162), percent, channelMaximum);
            };
            drawChannel(1, L"L", left, leftDb, 16, 160);
            drawChannel(2, L"R", right, rightDb, 176, 320);
            audioValueFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            text(clipping ? (IsEnglishUi() ? L"CLIP" : L"클리핑")
                          : (IsEnglishUi() ? L"No clipping" : L"클리핑 없음"),
                audioSmallFormat, D2D1::RectF(16, 171, 111, 193),
                clipping ? audioCacheClipBrush : audioCacheSecondaryBrush);
            audioSmallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            text(IsEnglishUi() ? L"Wheel · Double-click reset" : L"휠 조절 · 두 번 클릭 초기화",
                audioSmallFormat, D2D1::RectF(112, 171, 320, 193), audioCacheSecondaryBrush);
            audioSmallFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            hr = audioCacheTarget->EndDraw();
        }
        if (FAILED(hr)) return hr;
        // Publish cache validity only after every requested draw succeeded.
        // A failed refresh must be retried, including in the same generation.
        if (cachedOverlayGeneration != generation) cachedOverlayMask = 0;
        cachedOverlayMask |= refreshMask;
        cachedOverlayGeneration = generation;
        return S_OK;
    }

    HRESULT prepareHdrOverlay(LONG left, LONG top, LONG right, LONG bottom) {
        const LONG x = (std::max)(0L, left), y = (std::max)(0L, top);
        const LONG r = (std::min)(static_cast<LONG>(outputWidth), right);
        const LONG b = (std::min)(static_cast<LONG>(outputHeight), bottom);
        if (r <= x || b <= y) return S_FALSE;
        if (r - x > static_cast<LONG>(kOsdOverlayWidth) ||
            b - y > static_cast<LONG>(kOsdOverlayHeight)) return E_INVALIDARG;
        if (!hdrOverlayBackground) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = kOsdOverlayWidth;
            desc.Height = kOsdOverlayHeight;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
            if (scrgbOutput) desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
#endif
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            HRESULT hr = device->CreateTexture2D(&desc, nullptr, &hdrOverlayBackground);
            if (FAILED(hr)) return hr;
        }
        if (!hdrOverlayBackgroundView) {
            const HRESULT hr = device->CreateShaderResourceView(
                hdrOverlayBackground, nullptr, &hdrOverlayBackgroundView);
            if (FAILED(hr)) return hr;
        }
        // Copy just this panel, including any earlier overlapping overlay.
        // Unbind before copying to avoid RTV/SRV hazards. No full-frame copy.
        ID3D11ShaderResourceView* empty = nullptr;
        context->PSSetShaderResources(1, 1, &empty);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        const D3D11_BOX box{static_cast<UINT>(x), static_cast<UINT>(y), 0,
                            static_cast<UINT>(r), static_cast<UINT>(b), 1};
        context->CopySubresourceRegion(hdrOverlayBackground, 0, 0, 0, 0, backBuffer, 0, &box);
        context->OMSetRenderTargets(1, &backBufferRenderTarget, nullptr);
        const float values[]{static_cast<float>(x), static_cast<float>(y),
            g_hdrUiWhiteNits.load(std::memory_order_relaxed), 0.0f};
        context->UpdateSubresource(hdrOverlayConstants, 0, nullptr, values, 0, 0);
        context->PSSetConstantBuffers(0, 1, &hdrOverlayConstants);
        context->PSSetShaderResources(1, 1, &hdrOverlayBackgroundView);
        return S_OK;
    }

    HRESULT ensureMuteIndicator() {
        if (muteIndicatorShaderView) return S_OK;
        // Immutable, tiny display-only texture. Allocate once on first use,
        // not on normal playback or every audio-meter/overlay generation.
        constexpr UINT size = llcv::audio_osd::kMuteIndicatorSize;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = size;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        IDXGISurface* surface = nullptr;
        ID2D1RenderTarget* target = nullptr;
        ID2D1SolidColorBrush* background = nullptr;
        ID2D1SolidColorBrush* foreground = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
        if (SUCCEEDED(hr)) hr = texture->QueryInterface(IID_PPV_ARGS(&surface));
        if (SUCCEEDED(hr)) hr = d2dFactory->CreateDxgiSurfaceRenderTarget(surface,
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &target);
        if (SUCCEEDED(hr)) hr = target->CreateSolidColorBrush(D2D1::ColorF(0, 0.38f), &background);
        if (SUCCEEDED(hr)) hr = target->CreateSolidColorBrush(D2D1::ColorF(0xffffff, 0.82f), &foreground);
        if (SUCCEEDED(hr)) {
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0, 0.0f));
            target->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(0, 0, size, size), 9, 9), background);
            constexpr float inset = (size - llcv::ui::kSpeakerIconSize) * 0.5f;
            llcv::ui::DrawSpeakerIcon(true, [&](int x1, int y1, int x2, int y2) {
                target->DrawLine(D2D1::Point2F(inset+x1, inset+y1),
                    D2D1::Point2F(inset+x2, inset+y2), foreground, 1.8f);
            });
            hr = target->EndDraw();
        }
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(texture, nullptr, &view);
        SafeRelease(foreground); SafeRelease(background);
        SafeRelease(target); SafeRelease(surface);
        if (SUCCEEDED(hr)) {
            muteIndicatorTexture = texture;
            muteIndicatorShaderView = view;
        } else {
            SafeRelease(view); SafeRelease(texture);
        }
        return hr;
    }

    HRESULT drawOverlayQuads() {
        HRESULT hr = S_OK;
        bool recordingVisible = false;
#ifdef LLCV_RECORDING
        if (ExclusiveOutputSelected()) {
            hr = recordingIndicator.Refresh();
            if (FAILED(hr)) return hr;
            recordingVisible = recordingIndicator.Visible();
        }
#endif
        const bool osdVisible =
            g_osdVisible.load(std::memory_order_acquire);
        const bool volumeVisible = GetTickCount64() <
            g_volumeHudUntilMs.load(std::memory_order_acquire);
        const bool audioVisible =
            g_audioOsdVisible.load(std::memory_order_acquire);
        bool muteVisible = !audioVisible && !muteIndicatorUnavailable &&
            g_playbackMuted.load(std::memory_order_relaxed);
        const auto muteRect = llcv::audio_osd::MuteIndicatorRect(
            static_cast<int>(outputWidth), static_cast<int>(outputHeight));
        if (muteVisible) {
            hr = ensureMuteIndicator();
            if (FAILED(hr)) {
                // A cosmetic indicator must not interrupt playback or retry
                // allocating resources on every frame after device failure.
                fwprintf(stderr, L"[audio] mute indicator unavailable: 0x%08X\n", static_cast<unsigned>(hr));
                muteIndicatorUnavailable = true;
                muteVisible = false;
                hr = S_OK;
            }
        }
        if (!osdVisible && !volumeVisible && !audioVisible && !recordingVisible && !muteVisible) return S_OK;
        hr = refreshOverlayLayouts(osdVisible, volumeVisible, audioVisible);
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
            if (FAILED(hr)) return;
            if (hdrOutput) {
                hr = prepareHdrOverlay(left, top, right, bottom);
                if (hr != S_OK) return;
            }
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
            if (muteVisible && right && !bottom) {
                const LONG reserved = muteRect.right - muteRect.left + 8;
                if (x >= hudMargin + reserved) x -= reserved;
                else y = muteRect.bottom + 8;
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
        if (muteVisible) {
            draw(muteIndicatorShaderView, muteRect.left, muteRect.top,
                 muteRect.right, muteRect.bottom);
        }
#ifdef LLCV_RECORDING
        if (recordingVisible) {
            const RECT rect = recordingIndicator.QuadRect(outputWidth, outputHeight);
            draw(recordingIndicator.ShaderView(), rect.left, rect.top, rect.right, rect.bottom);
        }
#endif
        ID3D11ShaderResourceView* noTextures[2]{};
        context->PSSetShaderResources(0, hdrOutput ? 2 : 1, noTextures);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        if (FAILED(hr)) return hr;
        g_overlayRenderedFrames.fetch_add(1,
                                          std::memory_order_relaxed);
        return S_OK;
    }

    HRESULT processVideo(D3D11_VIDEO_PROCESSOR_STREAM& stream) {
        if(cmaaNisPipeline.Enabled()) {
            const HRESULT hr=cmaaNisPipeline.Draw(context,videoContext,processor,stream,
                backBufferRenderTarget,outputWidth,outputHeight);
            if(SUCCEEDED(hr))return hr;
            cmaaNisStatus=hr;
            cmaaNisPipeline.Reset();
            cachedOverlayGeneration=0;
            ShowTransientHud(TransientHudContent::CmaaNisFailed);
            fwprintf(stderr,L"[upscaling] draw failed 0x%08X; using Video Processor\n",unsigned(hr));
            const HRESULT removed=device->GetDeviceRemovedReason();
            if(FAILED(removed))return removed;
        }
#ifdef LLCV_SPATIAL_PROTOTYPE
        if (spatialPipeline.Enabled()) {
            const HRESULT spatialHr = spatialPipeline.Draw(context, videoContext, processor,
                stream, backBufferRenderTarget, outputWidth, outputHeight);
            if (SUCCEEDED(spatialHr)) return spatialHr;
            fwprintf(stderr,L"[spatial] draw failed 0x%08X; using original Video Processor\n",unsigned(spatialHr));
            spatialPipeline.Reset();
        }
#endif
        auto blit = [&] {
            return videoContext->VideoProcessorBlt(processor, outputView, 0, 1, &stream);
        };
#ifdef LLCV_VSR_FEATURE
        const auto previous = vsrState;
        const HRESULT hr = llcv::vsr::ProcessWithFallback(vsrState, blit, [&] {
            return llcv::vsr::SetRequest(videoContext, processor, false);
        });
        if (previous != vsrState) {
            cachedOverlayGeneration = 0;
            ShowTransientHud(vsrState == llcv::vsr::State::Rejected
                ? TransientHudContent::VsrRejected : TransientHudContent::VsrFailed);
            fwprintf(stderr, L"[vsr] processor fallback: %s; result=0x%08X; HDR=%d\n",
                llcv::vsr::StateName(vsrState), static_cast<unsigned>(hr), hdrOutput ? 1 : 0);
        }
        return hr;
#else
        return blit();
#endif
    }

    HRESULT presentUploaded() {
        const bool cmaaRequestChanged=cmaaNisAppliedRequest!=g_cmaaNisRequest.load(std::memory_order_acquire);
        if(cmaaRequestChanged) {
            const HRESULT hr=configureVideoOutput(captureWidth,captureHeight);
            if(FAILED(hr))return hr;
        }
#ifdef LLCV_VSR_FEATURE
        // Apply on the render owner immediately before processing the next
        // frame. Never call D3D from the UI thread or rebuild for an F6 press.
        const HRESULT vsrHr = applyVsrMode();
        if (FAILED(vsrHr)) return vsrHr;
        // Acknowledge on the render owner, including rapid toggles back to the
        // already-applied state. Do not claim ON for HDR/native/downscale bypass.
        if (cmaaNisAppliedRequest != -1 && (cmaaRequestChanged ||
            g_transientHudContent.load(std::memory_order_acquire)==TransientHudContent::CmaaNisPending)) {
            ShowTransientHud(cmaaNisAppliedRequest < 0 ? TransientHudContent::CmaaNisOff
                : FAILED(cmaaNisStatus) ? TransientHudContent::CmaaNisFailed
                : cmaaNisPipeline.Enabled() ? TransientHudContent::CmaaNisOn
                : hdrOutput ? TransientHudContent::CmaaNisHdrBypass
                : TransientHudContent::CmaaNisScaleBypass);
        }
#endif
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
        if (pixelPerfectBorders && !skipRedundantBorderClear) {
            const float black[4]{0.0f, 0.0f, 0.0f, 1.0f};
            context->ClearRenderTargetView(backBufferRenderTarget, black);
        }
#ifdef LLCV_GPU_DIAGNOSTICS
        const auto diagnosticStart = std::chrono::steady_clock::now();
#endif
        HRESULT hr;
#ifdef LLCV_VSR_EXPERIMENT
        vsrTiming.Begin(context);
#endif
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
        if (scrgbOutput) hr = scrgbPipeline.Draw(context, backBufferRenderTarget,
                                               outputWidth, outputHeight, activeUploadSurface);
        else
#endif
        hr = processVideo(stream);
#ifdef LLCV_VSR_EXPERIMENT
        vsrTiming.End(context, SUCCEEDED(hr));
#endif
#ifdef LLCV_GPU_DIAGNOSTICS
        const auto diagnosticVideoEnd = std::chrono::steady_clock::now();
        diagnosticVideoUs = std::chrono::duration<double, std::micro>(
            diagnosticVideoEnd - diagnosticStart).count();
#endif
        if (FAILED(hr)) return hr;
#ifdef LLCV_HDR_FRAME_AUDIT
        saveRequestedFrameAudit();
#endif
#ifdef LLCV_SPATIAL_PROTOTYPE
        g_spatialTrace.SnapshotOutput(device,context,backBuffer);
#endif
        hr = drawOverlayQuads();
#ifdef LLCV_GPU_DIAGNOSTICS
        const auto diagnosticOverlayEnd = std::chrono::steady_clock::now();
        diagnosticOverlayUs = std::chrono::duration<double, std::micro>(
            diagnosticOverlayEnd - diagnosticVideoEnd).count();
#endif
        if (FAILED(hr)) return hr;
        const bool vsync =
            llcv::presentation::UsesVSync(g_settings.presentationMode);
        const UINT syncInterval = vsync ? 1u : 0u;
        const UINT flags = llcv::presentation::PresentFlags(
            g_settings.presentationMode, exclusiveActive, allowTearing);
#ifdef LLCV_VSR_EXPERIMENT
        const auto presentStart = llcv::vsr::Timing::Clock::now();
#endif
#ifdef LLCV_SPATIAL_PROTOTYPE
        LARGE_INTEGER spatialPresentTime{};QueryPerformanceCounter(&spatialPresentTime);
        spatialPresentQpc=spatialPresentTime.QuadPart;
#endif
        hr = swapChain->Present(syncInterval, flags);
#ifdef LLCV_VSR_EXPERIMENT
        if (hr == S_OK) vsrTiming.AddPresent(std::chrono::duration<double,std::milli>(
            llcv::vsr::Timing::Clock::now()-presentStart).count());
#endif
#ifdef LLCV_GPU_DIAGNOSTICS
        diagnosticPresentUs = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - diagnosticOverlayEnd).count();
#endif
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
#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
static llcv::capture::AudioInputAttempt AudioOnlyCaptureAttempt() {
#else
static bool AudioOnlyCaptureLoop() {
#endif
    g_captureFailureHr.store(S_OK, std::memory_order_release);
#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
    g_captureAudioRejected.store(false, std::memory_order_release);
#endif
    g_captureGraphStopped.store(false, std::memory_order_release);
    g_captureGraphCompleted.store(false, std::memory_order_release);
    g_captureAudioAvailable.store(false, std::memory_order_release);
    g_directVideoActive.store(false, std::memory_order_release);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LogHr(L"CoInitializeEx(audio-only capture)", hr);
        g_captureFailureHr.store(hr, std::memory_order_release);
#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
        return llcv::capture::AudioInputAttempt::Failed;
#else
        return false;
#endif
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
#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
    bool inputTimedOut = false;
#endif
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
            hr = Surround51Active()
                ? llcv::capture_audio::FindSurroundPin(capture, &audioPin)
                : FindOutputPinByName(capture, kAudioPinName, &audioPin);
            if (FAILED(hr) && !Surround51Active()) {
                hr = FindOutputPinByMajorType(capture, MEDIATYPE_Audio,
                                              &audioPin);
            }
            if (Surround51Active()) g_captureAudioRejected.store(FAILED(hr));
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
            hr = Surround51Active()
                ? llcv::capture_audio::FindSurroundPin(audioCapture, &audioPin)
                : FindOutputPinByMajorType(audioCapture, MEDIATYPE_Audio, &audioPin);
            if (Surround51Active()) g_captureAudioRejected.store(FAILED(hr));
        }
        if (FAILED(hr)) break;
        g_captureAudioAvailable.store(true, std::memory_order_release);

        initializationStage = L"negotiate supported capture audio format";
        llcv::capture_audio::Rejection rejection =
            llcv::capture_audio::Rejection::Malformed;
        selectedAudioType = llcv::capture_audio::SelectSupportedType(
            audioPin, selectedAudioFormat, &rejection, Surround51Active());
        if (!selectedAudioType) {
            if (Surround51Active()) g_captureAudioRejected.store(true);
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
        if (Surround51Active()) g_captureAudioRejected.store(true);
        if (FAILED(hr = graph->ConnectDirect(audioPin, audioGrabberIn,
                                             selectedAudioType))) break;
        if (Surround51Active()) {
            hr = llcv::capture_audio::VerifySurroundConnection(audioGrabberIn, selectedAudioFormat);
            if (FAILED(hr)) break;
            g_captureAudioRejected.store(false);
        }
        ReportConnectedAudioAllocator(audioGrabberIn,
                                      selectedAudioFormat.blockAlign);
        if (FAILED(hr = graph->Connect(audioGrabberOut, audioNullIn))) break;

        initializationStage = L"start audio-only capture graph";
        hr = resources.events.Initialize(graph);
        if (FAILED(hr)) break;
#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
        if (!g_running.load(std::memory_order_acquire)) { initialized = true; break; }
        // Previous attempt has released its graph/callback before we get here.
        // Reset BEFORE Run: a driver may publish its first packet inside Run.
        g_audioLastCaptureCallbackMs.store(0, std::memory_order_release);
        const llcv::capture::AudioInputWatchdog inputWatchdog(GetTickCount64());
#endif
        hr = control->Run();
        if (FAILED(hr)) break;
        initialized = true;
        llcv::power::PlaybackPowerRequest powerRequest(g_settings.preventSleep, true, IsEnglishUi(), true);
        uint64_t powerRevision = 0;
        if (g_running.load(std::memory_order_acquire))
            g_playbackPowerEvents.StartRequest(powerRequest, powerRevision);
        if (powerRequest.Error()) LogHr(L"Playback power request", HRESULT_FROM_WIN32(powerRequest.Error()));
        fwprintf(stderr,
                 L"[capture] audio-only graph running: %s · %s\n",
                 g_activeCaptureAudioDeviceName.c_str(),
                 llcv::capture_audio::Describe(selectedAudioFormat).c_str());
        while (g_running.load(std::memory_order_acquire)) {
            if (g_captureAudioRejected.load(std::memory_order_acquire)) {
                initializationStage = L"capture audio format changed; restart required";
                hr = VFW_E_TYPE_NOT_ACCEPTED;
                initialized = false;
                break;
            }
            hr = resources.events.Wait(nullptr, 100, g_playbackPowerEvents.Event());
            if (FAILED(hr)) {
                g_captureGraphStopped.store(true, std::memory_order_release);
                initializationStage = L"capture device disconnected or graph aborted; restart required";
                initialized = false;
                break;
            }
            if (resources.events.Completed()) {
                g_captureGraphCompleted.store(true, std::memory_order_release);
                break;
            }
            if (resources.events.LastWaitReason() == llcv::capture::CaptureGraphEvents::WaitReason::Wake) {
                if (!g_running.load(std::memory_order_acquire)) break;
                if (g_playbackPowerEvents.ApplyPending(powerRequest, powerRevision) && powerRequest.Error())
                    LogHr(L"Playback power request after power broadcast", HRESULT_FROM_WIN32(powerRequest.Error()));
            }
#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
            if (!g_running.load(std::memory_order_acquire)) break;
            const auto timeout = inputWatchdog.Check(GetTickCount64(),
                g_audioLastCaptureCallbackMs.load(std::memory_order_acquire));
            if (timeout != llcv::capture::AudioInputWatchdog::Timeout::None) {
                fwprintf(stderr, L"[audio-input] %s; rebuilding audio-only capture path.\n",
                    timeout == llcv::capture::AudioInputWatchdog::Timeout::Startup
                        ? L"no valid input packet within 10 seconds"
                        : L"no valid input packet for 5 seconds");
                inputTimedOut = true;
                initialized = false;
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                break;
            }
#endif
        }
        powerRequest.Reset();
        control->Stop();
    } while (false);

#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
    if (inputTimedOut) {
        g_captureFailureHr.store(HRESULT_FROM_WIN32(ERROR_TIMEOUT), std::memory_order_release);
    } else
#endif
    if (!initialized) {
        const HRESULT failure = FAILED(hr) ? hr : E_FAIL;
        g_captureFailureHr.store(failure, std::memory_order_release);
        fwprintf(stderr, L"[capture] initialization stage: %s\n",
                 initializationStage);
        LogHr(L"Audio-only capture graph initialization", failure);
        if (!g_captureGraphStopped.load(std::memory_order_acquire)) {
            LogFilterPins(capture, L"audio-only capture filter");
            if (audioCapture && audioCapture != capture)
                LogFilterPins(audioCapture, L"separate capture audio filter");
        }
    }
    resources.Reset();
#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
    g_captureAudioAvailable.store(false, std::memory_order_release);
#endif
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
    return inputTimedOut ? llcv::capture::AudioInputAttempt::InputTimeout
        : initialized ? llcv::capture::AudioInputAttempt::Complete
                      : llcv::capture::AudioInputAttempt::Failed;
#else
    return initialized;
#endif
}

#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
static bool AudioOnlyCaptureLoop() {
    g_audioInputRecoveryExhausted.store(false, std::memory_order_release);
    const auto result = llcv::capture::RunAudioInputRecovery(
        AudioOnlyCaptureAttempt,
        [] { return g_running.load(std::memory_order_acquire); },
        [](unsigned delayMs) {
            // Leave output-device ownership, volume, mute and recording alone.
            // Short cancellable backoff; never sleep through viewer shutdown.
            for (unsigned waited = 0; waited < delayMs; waited += 25) {
                if (!g_running.load(std::memory_order_acquire)) return false;
                Sleep(25);
            }
            return g_running.load(std::memory_order_acquire);
        },
        [](unsigned retry) {
            fwprintf(stderr, L"[audio-input] capture restart %u/%u (output device kept open).\n",
                retry, llcv::capture::AudioInputRetryLimit);
        });
    if (result == llcv::capture::AudioInputAttempt::InputTimeout) {
        g_audioInputRecoveryExhausted.store(true, std::memory_order_release);
        fwprintf(stderr, L"[audio-input] restart limit reached; returning to settings.\n");
    }
    return result == llcv::capture::AudioInputAttempt::Complete;
}
#endif

// Input arrival is independent of successful presentation (e.g. an occluded window).
static bool StartupInputWaitExpired(
    bool receivedAnyFrame, std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point deadline) {
    return !receivedAnyFrame && now >= deadline;
}

static HRESULT ValidateCaptureLayout(const wchar_t* stage, const AM_MEDIA_TYPE* media,
    int width, int height, VideoPixelFormat format, DWORD& bytes, UINT32& stride, int& fps) {
    int actualWidth = 0, actualHeight = 0;
    REFERENCE_TIME duration = 0;
    DWORD actualBytes = 0;
    VideoPixelFormat actualFormat = VideoPixelFormat::Auto;
    const bool parsed = llcv::video::VideoFormatDetails(media, actualWidth, actualHeight,
        duration, actualBytes, &actualFormat);
    fwprintf(stderr, L"[video-layout] %s parsed=%d actual=%s %dx%d duration=%lld bytes=%lu expected=%s %dx%d\n",
        stage, parsed ? 1 : 0, PixelFormatName(actualFormat), actualWidth, actualHeight,
        duration, actualBytes, PixelFormatName(format), width, height);
    return llcv::video::ValidateVideoLayout(media, width, height, format, bytes, stride, fps, LogModuleMessage);
}

static bool UnifiedCaptureRenderLoop(HWND host, bool smokeVideoOverride = false) {
    const auto& preset = CurrentCapturePreset();
    g_hdrFailureDetail.store(nullptr, std::memory_order_release);
    g_captureFailureHr.store(S_OK, std::memory_order_release);
    g_captureGraphStopped.store(false, std::memory_order_release);
    g_captureGraphCompleted.store(false, std::memory_order_release);
    g_captureVideoRejected.store(false, std::memory_order_release);
    g_captureAudioAvailable.store(false, std::memory_order_release);
    if (!llcv::presentation::SupportsCapture(g_settings.presentationMode, g_settings.pixelFormat)) {
        g_hdrFailureDetail.store(L"P010 HDR10 requires Immediate or VSync (Flip), not compatibility output",
                                 std::memory_order_release);
        g_captureFailureHr.store(DXGI_ERROR_UNSUPPORTED, std::memory_order_release);
        return false; // Also guard non-UI startup before opening a capture device.
    }
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
    std::wstring selectedVideoDevicePath;

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
                               &g_activeCaptureDeviceName, &selectedVideoDevicePath);
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
        DirectShowColorMetadata acceptedCandidateColor{};
        GUID acceptedCandidateSubtype{};
        initializationStage = L"negotiate video resolution/FPS/pixel format";
        hr = ConfigureVideoPin(videoPin, preset.width, preset.height,
                               RequestedVideoFrameRate(),
                               g_settings.pixelFormat,
                               imageBytes, stride, configuredFps,
                               configuredFormat, acceptedCandidateColor, &acceptedCandidateSubtype);
        if (FAILED(hr)) {
            LogHr(L"ConfigureVideoPin(exact format)", hr);
            break;
        }
        initializationStage = L"read active video capture format";
        hr = GetActiveVideoPinFormat(videoPin, &activeVideoType);
        if (FAILED(hr) || !activeVideoType) break;
        initializationStage = L"validate active video layout";
        hr = ValidateCaptureLayout(L"active", activeVideoType, preset.width, preset.height,
            configuredFormat, imageBytes, stride, configuredFps);
        if (SUCCEEDED(hr) && RequestsExactCaptureRate() &&
            !llcv::video::IsRequestedFrameRate(activeVideoType, RequestedVideoFrameRate()))
            hr = llcv::video::kRequestedFrameRateMismatch;
        if (FAILED(hr)) break;
        const bool compressedVideo = IsCompressedVideoFormat(configuredFormat);
        const VideoPixelFormat rendererInputFormat = compressedVideo
            ? VideoPixelFormat::Nv12 : configuredFormat;
        g_activePixelFormat.store(static_cast<int>(configuredFormat),
                                  std::memory_order_release);

        DirectShowColorMetadata directShowColorInfo{};
        const bool colorMetadataRelevant =
            configuredFormat == VideoPixelFormat::P010 || compressedVideo ||
            configuredFormat == VideoPixelFormat::Nv12 || configuredFormat == VideoPixelFormat::Yuy2 ||
            configuredFormat == VideoPixelFormat::Uyvy || configuredFormat == VideoPixelFormat::Rgb32 ||
            configuredFormat == VideoPixelFormat::Rgb24;
        bool directShowColorMetadataDetected = colorMetadataRelevant &&
            ResolveInitialVideoColorMetadata(videoPin, activeVideoType,
                configuredFormat, preset.width, preset.height, configuredFps,
                acceptedCandidateColor, directShowColorInfo, &acceptedCandidateSubtype);
        bool hdrInputMetadataAvailable = false;
        DXGI_COLOR_SPACE_TYPE hdrColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;
        if (configuredFormat == VideoPixelFormat::P010) {
            LogDirectShowColorMetadata(L"P010 selected format",
                                       directShowColorInfo);
        }
        if (compressedVideo) {
            LogDirectShowColorMetadata(L"MJPEG selected format",
                                       directShowColorInfo);
        }

        g_videoConfiguredFps.store(configuredFps,
                                   std::memory_order_release);
        llcv::video_color::Configuration sdrColor{};
        const bool rawSdr = configuredFormat == VideoPixelFormat::Nv12 ||
                            configuredFormat == VideoPixelFormat::Yuy2 ||
                            configuredFormat == VideoPixelFormat::Uyvy ||
                            configuredFormat == VideoPixelFormat::Rgb32 ||
                            configuredFormat == VideoPixelFormat::Rgb24;
        if (rawSdr) {
            sdrColor = ResolveRawSdrVideoColor(configuredFormat, preset.width,
                preset.height, directShowColorInfo);
        }
        if (compressedVideo) {
            initializationStage = L"initialize Media Foundation compressed decoder";
            hr = compressedDecoder.initialize(
                preset.width, preset.height, configuredFps, activeVideoType,
                directShowColorMetadataDetected ? &directShowColorInfo : nullptr,
                g_settings.mjpegMatrixOverride, LogModuleMessage, SelectedSdrRange(configuredFormat));
            if (FAILED(hr)) {
                LogHr(L"Media Foundation compressed decoder", hr);
                break;
            }
            sdrColor = compressedDecoder.colorConfiguration();
        }
        initializationStage = L"initialize D3D11 video renderer";
        // P010 color is decided from the final connected type below. Do not
        // create a speculative HDR/SDR swapchain before negotiation completes.
        if (configuredFormat != VideoPixelFormat::P010 && configuredFormat != VideoPixelFormat::Rgb24 &&
            configuredFormat != VideoPixelFormat::Rgb32)
            hr = renderer.initialize(host, preset.width, preset.height,
                                     configuredFps, rendererInputFormat,
                                     hdrInputMetadataAvailable, sdrColor, hdrColorSpace);
        if (FAILED(hr)) {
            LogHr(L"DirectD3D11Renderer::initialize", hr);
            break;
        }
        frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!frameEvent) { hr = HRESULT_FROM_WIN32(GetLastError()); break; }

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
        // Configure only this selected video device, once per graph start.
        // Vendor controls are capability-checked. USB control requires the
        // selected physical device container, never a name/VID-only match.
        // Optional vendor control must not make unsupported devices fail to
        // start. Do not restore at shutdown: that could overwrite another
        // application's setting. SDR graph starts explicitly request SDR.
        if (!llcv::settings::RunHardwareToneMapping(smokeVideoOverride, [&] {
            llcv::capture::ConfigureHardwareToneMapping(
                capture, g_activeCaptureDeviceName, configuredFormat, LogModuleMessage, selectedVideoDevicePath);
        }))
            fwprintf(stderr, L"[capture-hdr] skipped hardware tone-mapping control for isolated smoke video overrides; no device protocol call.\n");
        if (FAILED(hr = graph->ConnectDirect(videoPin, grabberIn,
                                             nullptr))) break;
        // Preserve connection-only hints even if a driver omits them after
        // the downstream pin connects. The final connected type below still
        // takes precedence, and initializes the MJPEG decoder exactly once.
        if (colorMetadataRelevant) {
            AM_MEDIA_TYPE connectedType{};
            if (SUCCEEDED(grabber->GetConnectedMediaType(&connectedType))) {
                DirectShowColorMetadata connectedColor{};
                if (ExtractVideoColorMetadata(&connectedType, connectedColor)) {
                    if (configuredFormat == VideoPixelFormat::P010)
                        directShowColorInfo = llcv::hdr::ConnectedMetadata(directShowColorInfo, connectedColor);
                    else
                        MergeVideoColorMetadata(directShowColorInfo, connectedColor);
                }
            }
            FreeMediaType(connectedType);
        }
        if (FAILED(hr = graph->ConnectDirect(grabberOut, nullIn,
                                             nullptr))) break;
        // The connected type, not the advertised type, defines the raw upload.
        initializationStage = L"validate connected video layout";
        AM_MEDIA_TYPE connectedLayout{};
        llcv::video::VideoSampleFormat sampleFormat{};
        hr = grabber->GetConnectedMediaType(&connectedLayout);
        const int previousFps = configuredFps;
        if (SUCCEEDED(hr)) hr = ValidateCaptureLayout(L"connected", &connectedLayout,
            preset.width, preset.height, configuredFormat, imageBytes, stride, configuredFps);
        if (SUCCEEDED(hr) && RequestsExactCaptureRate() &&
            !llcv::video::IsRequestedFrameRate(&connectedLayout, RequestedVideoFrameRate()))
            hr = llcv::video::kRequestedFrameRateMismatch;
        if (SUCCEEDED(hr) && !ReadConnectedVideoSampleFormat(
                &connectedLayout, configuredFps, directShowColorInfo, sampleFormat))
            hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if (SUCCEEDED(hr) && rawSdr) {
            const bool rgb = configuredFormat == VideoPixelFormat::Rgb24 || configuredFormat == VideoPixelFormat::Rgb32;
            if (!rgb) LogDirectShowColorMetadata(L"raw SDR effective metadata", directShowColorInfo);
            sdrColor = ResolveRawSdrVideoColor(configuredFormat, preset.width,
                preset.height, directShowColorInfo);
            if (rgb)
                fwprintf(stderr, L"[video-color] RGB metadata: present=%u primaries=%u transfer=%u nominal-range=%u; selected range=%s (source: %s); YUV matrix/chroma not applicable\n",
                    directShowColorInfo.present ? 1u : 0u, directShowColorInfo.primaries, directShowColorInfo.transferFunction,
                    directShowColorInfo.nominalRange, llcv::video_color::RangeName(sdrColor.range), llcv::video_color::SourceName(sdrColor.rangeSource));
            else
                fwprintf(stderr, L"[video-color] raw SDR: %s / %s (matrix: %s; range: %s)\n",
                    llcv::video_color::MatrixName(sdrColor.matrix), llcv::video_color::RangeName(sdrColor.range),
                    llcv::video_color::SourceName(sdrColor.matrixSource), llcv::video_color::SourceName(sdrColor.rangeSource));
        }
        if (SUCCEEDED(hr) && compressedVideo) {
            // Drivers may publish color only after connection. Rebuild from
            // the same final type and hints used by the guard and recorder.
            LogDirectShowColorMetadata(L"MJPEG effective metadata", directShowColorInfo);
            initializationStage = L"confirm Media Foundation MJPEG decoder color";
            hr = compressedDecoder.initialize(
                preset.width, preset.height, configuredFps, &connectedLayout,
                directShowColorInfo.present ? &directShowColorInfo : nullptr,
                g_settings.mjpegMatrixOverride, LogModuleMessage, SelectedSdrRange(configuredFormat));
            if (SUCCEEDED(hr)) sdrColor = compressedDecoder.colorConfiguration();
            else LogHr(L"MJPEG connected-type color initialization", hr);
        }
        FreeMediaType(connectedLayout);
        if (FAILED(hr)) break;
        if (configuredFormat == VideoPixelFormat::P010) {
            initializationStage = L"validate connected P010 color / HDR10 conversion";
            const auto input = llcv::hdr::ResolveInput(directShowColorInfo, g_settings.forceHdr10,
                g_settings.hdrChromaLocation);
            LogDirectShowColorMetadata(L"P010 effective metadata", directShowColorInfo);
            fwprintf(stderr, L"[hdr] input decision: %s\n", input.reason);
            if (input.kind == llcv::hdr::InputKind::Hdr10)
                fwprintf(stderr, L"[hdr] chroma placement: reported=%u selection=%s effective=%s overridden=%u\n",
                    directShowColorInfo.present ? directShowColorInfo.chromaSubsampling : 0,
                    llcv::hdr::ChromaLocationName(g_settings.hdrChromaLocation),
                    input.colorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 ? L"Left" : L"TopLeft",
                    input.chromaOverridden ? 1u : 0u);
            if (input.kind != llcv::hdr::InputKind::Hdr10) {
                g_hdrFailureDetail.store(input.reason, std::memory_order_release);
                hr = DXGI_ERROR_UNSUPPORTED;
                break;
            }
            hdrInputMetadataAvailable = input.kind == llcv::hdr::InputKind::Hdr10;
            hdrColorSpace = input.colorSpace;
        }
        if (configuredFps != previousFps || configuredFormat == VideoPixelFormat::P010 ||
            ((rawSdr || compressedVideo) && !(sdrColor == renderer.sdrColor)) ||
            configuredFormat == VideoPixelFormat::Rgb32 || configuredFormat == VideoPixelFormat::Rgb24) {
            hr = renderer.initialize(host, preset.width, preset.height, configuredFps,
                                     rendererInputFormat, hdrInputMetadataAvailable, sdrColor, hdrColorSpace,
                                     sampleFormat.topDown, directShowColorInfo);
            if (FAILED(hr)) break;
        }
        g_videoConfiguredFps.store(configuredFps, std::memory_order_release);
#ifdef LLCV_HDR_FRAME_AUDIT
        renderer.auditMetadata = directShowColorInfo;
#endif
        fwprintf(stderr, L"[video] connected layout verified: %s %dx%d @ %d stride=%u bytes=%lu\n",
                 PixelFormatName(configuredFormat), preset.width, preset.height,
                 configuredFps, stride, imageBytes);
        resources.latestVideoSample = std::make_unique<llcv::capture::LatestVideoSample>(
            compressedVideo ? 0 : imageBytes, frameEvent,
            llcv::capture::VideoSampleTelemetry{&g_osdTrackingStartMs, &g_videoCapturedFrames,
              &g_videoReplacedFrames});
        auto& latest = *resources.latestVideoSample;
        llcv::screenshot::Format screenshotFormat = llcv::screenshot::Format::Unknown;
        switch (rendererInputFormat) {
        case VideoPixelFormat::Nv12: screenshotFormat = llcv::screenshot::Format::Nv12; break;
        case VideoPixelFormat::Yuy2: screenshotFormat = llcv::screenshot::Format::Yuy2; break;
        case VideoPixelFormat::Uyvy: screenshotFormat = llcv::screenshot::Format::Uyvy; break;
        case VideoPixelFormat::P010: screenshotFormat = llcv::screenshot::Format::P010; break;
        case VideoPixelFormat::Rgb24: screenshotFormat = llcv::screenshot::Format::Rgb24; break;
        case VideoPixelFormat::Rgb32: screenshotFormat = llcv::screenshot::Format::Rgb32; break;
        default: break;
        }
        const bool screenshotTopDown = rendererInputFormat == VideoPixelFormat::Rgb24 ||
            rendererInputFormat == VideoPixelFormat::Rgb32 ? sampleFormat.topDown : true;
        if (!g_screenshots.Start(preset.width, preset.height, screenshotFormat,
                                llcv::screenshot::DefaultDirectory(), screenshotTopDown))
            fwprintf(stderr, L"[screenshot] unavailable: unsupported format or buffer/worker/Pictures folder initialization failed.\n");
        callback = new llcv::capture::VideoSampleGrabberCallback(&latest, nullptr, sampleFormat);
        hr = grabber->SetCallback(callback, 0);
        if (FAILED(hr)) break;
        // Prefer an audio pin on the selected video filter. Many USB UVC
        // capture devices instead expose their capture audio as a separate
        // DirectShow audio-input filter, which is added to this same graph.
        IBaseFilter* audioSource = capture;
        g_activeCaptureAudioDeviceName = g_activeCaptureDeviceName;
        initializationStage = L"find audio output pin on video capture filter";
        hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        if (g_settings.captureAudioDeviceId.empty()) {
            hr = Surround51Active()
                ? llcv::capture_audio::FindSurroundPin(capture, &audioPin)
                : FindOutputPinByName(capture, kAudioPinName, &audioPin);
            if (FAILED(hr) && !Surround51Active()) {
                hr = FindOutputPinByMajorType(capture, MEDIATYPE_Audio, &audioPin);
            }
            if (Surround51Active()) g_captureAudioRejected.store(FAILED(hr));
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
            hr = Surround51Active()
                ? llcv::capture_audio::FindSurroundPin(audioSource, &audioPin)
                : FindOutputPinByMajorType(audioSource, MEDIATYPE_Audio, &audioPin);
            if (Surround51Active()) g_captureAudioRejected.store(FAILED(hr));
        }
        if (FAILED(hr)) break;
        g_captureAudioAvailable.store(true, std::memory_order_release);

        initializationStage = L"negotiate supported capture audio format";
        llcv::capture_audio::Rejection audioFormatRejection =
            llcv::capture_audio::Rejection::Malformed;
        selectedAudioType = llcv::capture_audio::SelectSupportedType(
            audioPin, selectedAudioFormat, &audioFormatRejection, Surround51Active());
        if (!selectedAudioType) {
            if (Surround51Active()) g_captureAudioRejected.store(true);
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
        if (Surround51Active()) g_captureAudioRejected.store(true);
        if (FAILED(hr = graph->ConnectDirect(audioPin, audioGrabberIn,
                                             selectedAudioType))) break;
        if (Surround51Active()) {
            hr = llcv::capture_audio::VerifySurroundConnection(audioGrabberIn, selectedAudioFormat);
            g_captureAudioRejected.store(FAILED(hr));
            if (FAILED(hr)) break;
        }
        ReportConnectedAudioAllocator(audioGrabberIn,
                                      selectedAudioFormat.blockAlign);
        if (FAILED(hr = graph->Connect(audioGrabberOut, audioNullIn))) break;

        initializationStage = L"connect and start capture graph";
#ifdef LLCV_RECORDING
        auto recordingFormat=sampleFormat;
        AM_MEDIA_TYPE recordingType{};
        if(compressedVideo){
            // Keep the final JPEG initialization data and actual packet bound.
            // A recorder-only setup error must not stop viewing.
            const HRESULT recordingTypeHr=grabber->GetConnectedMediaType(&recordingType);
            if(FAILED(recordingTypeHr)){
                recordingFormat.valid=false;
                LogHr(L"read MJPEG recording input type",recordingTypeHr);
            }
            const auto allocation=llcv::capture_audio::QueryConnectedAllocator(grabberIn,1);
            if(SUCCEEDED(allocation.result) && allocation.bufferBytes>0)
                recordingFormat.bytes=static_cast<DWORD>(allocation.bufferBytes);
            else recordingFormat.bytes=(std::max)(recordingFormat.bytes,
                static_cast<DWORD>(recordingFormat.width*recordingFormat.height*3));
        }
        // Capture setup runs off the UI thread. Recording preferences are
        // copied by Start on the UI thread, including changes made during setup.
        llcv::recording::recorder.Configure(recordingFormat, selectedAudioFormat, sdrColor,
            true, llcv::recording::Options{},compressedVideo?&recordingType:nullptr,
            g_settings.mjpegMatrixOverride, SelectedSdrRange(configuredFormat));
        FreeMediaType(recordingType);
#endif
        hr = resources.events.Initialize(graph);
        if (FAILED(hr)) break;
        hr = control->Run();
        if (FAILED(hr)) break;
        initialized = true;
        llcv::power::PlaybackPowerRequest powerRequest(g_settings.preventSleep, false, IsEnglishUi(), true);
        uint64_t powerRevision = 0;
        if (g_running.load(std::memory_order_acquire))
            g_playbackPowerEvents.StartRequest(powerRequest, powerRevision);
        if (powerRequest.Error()) LogHr(L"Playback power request", HRESULT_FROM_WIN32(powerRequest.Error()));
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
                 tearingActive ? L"Tearing"
                     : llcv::presentation::ModeName(g_settings.presentationMode));
        fwprintf(stderr,
                 L"[video] upload ring: %u %s GPU surfaces; update: %s\n",
                 DirectD3D11Renderer::kUploadSurfaceCount,
                 PixelFormatName(rendererInputFormat),
                 renderer.discardUpdateAvailable
                     ? L"D3D11.1 discard" : L"D3D11 fallback");
        if (compressedVideo) {
            fwprintf(stderr,
                     L"[video] compressed compatibility path: %s capture -> "
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
                    rendererInputFormat, hdrInputMetadataAvailable, sdrColor, hdrColorSpace,
                    sampleFormat.topDown, directShowColorInfo);
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
        bool receivedAnyFrame = false;
        bool presentedAnyFrame = false;
        const auto firstFrameStart = std::chrono::steady_clock::now();
        const auto firstFrameDeadline = firstFrameStart + std::chrono::seconds(10);
        initializationStage = L"wait for first valid capture frame";
        while (g_running.load()) {
            if (ExclusiveOutputSelected() && g_presentationClosing.load(std::memory_order_acquire))
                break;
            // Focus and modal-window transitions must be serviced even when
            // capture has no new sample. Only this thread owns DXGI state.
            if (ExclusiveOutputSelected() && renderer.outputConfigurationChanged()) {
                hr = renderer.resizeOutput(host);
                if (FAILED(hr) && !recoverRenderer(L"D3D11 fullscreen transition", hr)) {
                    initialized = false;
                    break;
                }
            }
            if (g_captureAudioRejected.load(std::memory_order_acquire)) {
                initializationStage = L"capture audio format changed; restart required";
                hr = VFW_E_TYPE_NOT_ACCEPTED;
                initialized = false;
                break;
            }
            hr = resources.events.Wait(frameEvent, 100, g_playbackPowerEvents.Event());
            if (ExclusiveOutputSelected() && g_presentationClosing.load(std::memory_order_acquire))
                break;
            if (FAILED(hr)) {
                g_captureGraphStopped.store(true, std::memory_order_release);
                initializationStage = L"capture device disconnected or graph aborted; restart required";
                initialized = false;
                break;
            }
            if (resources.events.Completed()) {
                g_captureGraphCompleted.store(true, std::memory_order_release);
                break;
            }
            if (resources.events.LastWaitReason() == llcv::capture::CaptureGraphEvents::WaitReason::Wake) {
                if (!g_running.load(std::memory_order_acquire)) break;
                if (g_playbackPowerEvents.ApplyPending(powerRequest, powerRevision) && powerRequest.Error())
                    LogHr(L"Playback power request after power broadcast", HRESULT_FROM_WIN32(powerRequest.Error()));
                continue;
            }
            // Drain once even on timeout: publication can race the wait result.
            IMediaSample* videoSample = latest.TakeLatest(arrivalUs);
            if (FAILED(latest.FormatFailure())) {
                SafeRelease(videoSample);
                initializationStage = L"capture video format changed or could not be verified; restart required";
                hr = latest.FormatFailure();
                initialized = false;
                break;
            }
            if (!videoSample) {
                if (StartupInputWaitExpired(receivedAnyFrame,
                        std::chrono::steady_clock::now(), firstFrameDeadline)) {
                    fwprintf(stderr,
                             L"[video] no valid %s input within 10 seconds; rejected samples=%llu expected bytes=%lu.\n",
                             PixelFormatName(configuredFormat),
                             static_cast<unsigned long long>(latest.RejectedSamples()), imageBytes);
                    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                    initialized = false;
                    break;
                }
                continue;
            }
            if (!receivedAnyFrame) {
                receivedAnyFrame = true;
                initializationStage = L"process and present capture frames";
                fwprintf(stderr, L"[video] first valid input after %lld ms; rejected samples=%llu\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - firstFrameStart).count()),
                    static_cast<unsigned long long>(latest.RejectedSamples()));
            }
            if (renderer.outputConfigurationChanged()) {
                hr = renderer.resizeOutput(host);
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
                // Output/device rebuild can outlast a capture interval. Upload
                // the freshest available input, not the frame taken before it.
                latest.RefreshAfterOutputReset(videoSample, arrivalUs);
                if (FAILED(latest.FormatFailure())) {
                    SafeRelease(videoSample);
                    hr = latest.FormatFailure();
                    initialized = false;
                    break;
                }
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
                            sdrColor, hdrColorSpace);
                    }
                }
                if (SUCCEEDED(hr) && decodedBuffer) {
                    DWORD maximum = 0;
                    DWORD current = 0;
                    hr = decodedBuffer->Lock(&sampleData, &maximum, &current);
                    if (SUCCEEDED(hr) && sampleData &&
                        compressedDecoder.validOutputBuffer(maximum, current)) {
                        renderer.upload(sampleData,
                                        static_cast<UINT32>(
                                            compressedDecoder.stride()));
                        g_screenshots.Submit({static_cast<unsigned>(preset.width),
                            static_cast<unsigned>(preset.height), screenshotFormat, sdrColor, false},
                            sampleData, current, static_cast<unsigned>(compressedDecoder.stride()));
                    } else if (SUCCEEDED(hr)) {
                        hr = E_FAIL;
                    }
                    if (sampleData) decodedBuffer->Unlock();
                }
            } else {
                hr = videoSample->GetPointer(&sampleData);
                if (SUCCEEDED(hr) && sampleData) {
#ifdef LLCV_SPATIAL_PROTOTYPE
                    if ((rendererInputFormat==VideoPixelFormat::Nv12 && stride==1920) ||
                        (rendererInputFormat==VideoPixelFormat::P010 && stride==3840))
                        g_spatialTrace.Snapshot(sampleData,static_cast<size_t>(std::max(0L,videoSample->GetActualDataLength())),
                            rendererInputFormat==VideoPixelFormat::P010);
#endif
                    renderer.upload(sampleData, stride);
                    g_screenshots.Submit({static_cast<unsigned>(preset.width),
                        static_cast<unsigned>(preset.height), screenshotFormat, sdrColor,
                        screenshotFormat == llcv::screenshot::Format::P010 &&
                        hdrColorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020,
                        screenshotTopDown},
                        sampleData, static_cast<std::size_t>(std::max(0L, videoSample->GetActualDataLength())), stride);
                } else if (SUCCEEDED(hr)) {
                    hr = E_POINTER;
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
#ifdef LLCV_SPATIAL_PROTOTYPE
            g_spatialTrace.Add(arrivalUs,renderer.spatialPresentQpc,presentedUs);
#endif
#ifdef LLCV_VSR_EXPERIMENT
            renderer.vsrTiming.AddCaptureToPresent(static_cast<double>(presentedUs - arrivalUs) / 1000.0);
#endif
            if (OsdTrackingActive()) {
                g_videoAppLatencyUs.store(presentedUs - arrivalUs,
                                          std::memory_order_release);
                g_videoPresentedFrames.fetch_add(1,
                                                 std::memory_order_relaxed);
            }
            g_directVideoActive.store(true, std::memory_order_release);
            if (!presentedAnyFrame) {
                presentedAnyFrame = true;
                fwprintf(stderr, L"[video] first successful presentation after %lld ms\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - firstFrameStart).count()));
            }
        }
        powerRequest.Reset();
        control->Stop();
    } while (false);

    if (resources.latestVideoSample && FAILED(resources.latestVideoSample->FormatFailure())) {
        g_captureVideoRejected.store(true, std::memory_order_release);
        hr = resources.latestVideoSample->FormatFailure();
        initialized = false;
        initializationStage = L"capture video format changed or could not be verified; restart required";
    }
    if (!initialized) {
        const HRESULT failure = FAILED(hr) ? hr : E_FAIL;
        g_captureFailureHr.store(failure, std::memory_order_release);
        fwprintf(stderr, L"[capture] initialization stage: %s\n",
                 initializationStage);
        LogHr(L"Single capture graph initialization", failure);
        // A disconnected/aborted filter is no longer a useful capability
        // source. Preserve its HRESULT without probing the failed driver again.
        if (!g_captureGraphStopped.load(std::memory_order_acquire)) {
            LogFilterPins(capture, L"video capture filter");
            if (audioCapture && audioCapture != capture)
                LogFilterPins(audioCapture, L"separate capture audio filter");
        }
    }
    g_screenshots.Stop();
    resources.Reset();
    // Release exclusive ownership while the UI is still pumping, before a
    // recording writer is allowed to take time finalizing its file.
    if (ExclusiveOutputSelected()) renderer.reset();
#ifdef LLCV_RECORDING
    // Notify before joining: a stalled writer must still have a responsive
    // close/timeout UI when capture ends without a user WM_CLOSE.
    if (llcv::recording::recorder.Busy())
        PostMessageW(GetAncestor(host,GA_ROOT),WM_RECORDING_CAPTURE_ENDED,0,0);
    llcv::recording::recorder.Shutdown();
#endif
    renderer.reset();
    compressedDecoder.reset();
    if (videoMmcss) AvRevertMmThreadCharacteristics(videoMmcss);
    CoUninitialize();
    return initialized;
}

// -----------------------------------------------------------------------------
// Startup settings dialog
// -----------------------------------------------------------------------------

#include "ui/SettingsAudioSelection.h"

using namespace llcv::settings_ui::control_id;
constexpr UINT WM_AUDIOCLIENT3_PROBE_COMPLETE = WM_APP + 73;
constexpr UINT WM_SETTINGS_TOOLTIP_SHOW = WM_APP + 74;
constexpr UINT WM_SETTINGS_TOOLTIP_HIDE = WM_APP + 75;
constexpr UINT WM_CAPTURE_AUDIO_PROBE_COMPLETE = WM_APP + 76;
constexpr UINT WM_UPDATE_CHECK_COMPLETE = WM_APP + 77;
constexpr UINT WM_SETTINGS_UPDATE_CHECK_COMPLETE = WM_APP + 78;
constexpr UINT WM_EXCLUSIVE_ENDPOINT_PROBE_COMPLETE = WM_APP + 79;
constexpr UINT WM_EXCLUSIVE_SCAN_COMPLETE = WM_APP + 80;
constexpr UINT WM_SETTINGS_VIDEO_MODES_READY = WM_APP + 81;
constexpr UINT WM_SETTINGS_INVENTORY_READY = WM_APP + 82;
#ifdef LLCV_RECORDING
constexpr UINT WM_RECORD_ENCODERS_READY = WM_APP + 83;
#endif

using llcv::settings_ui::SettingsDialogOuterSize;
using llcv::settings_ui::ApplySettingsFont;
using llcv::settings_ui::LayoutSettingsControls;
using llcv::settings_ui::SetSettingsControlVisible;
using llcv::settings_ui::SetSettingsText;
using llcv::settings_ui::UpdateScalingControlVisibility;
using llcv::settings_ui::UpdateWindowBehaviorVisibility;
using llcv::settings_ui::TrackSettingsTooltip;
using llcv::settings_ui::IsSettingsHelpControl;
using llcv::settings_ui::SettingsTab;
using llcv::settings_ui::SettingsHelpTopic;

struct SettingsVideoModes {
    std::vector<PixelFormatSupport> formats;
    HRESULT status = S_OK;
    llcv::video::VideoCapabilityDiagnostics diagnostics;
};
struct SettingsDeviceInventory {
    std::vector<CaptureDeviceInfo> videoInputs;
    std::vector<CaptureDeviceInfo> audioInputs;
    std::vector<AudioEndpointInfo> audioOutputs;
};
struct SettingsDialogState : llcv::settings_ui::SettingsControls {
    std::unique_ptr<llcv::settings_ui::SettingsModeCache<SettingsVideoModes>> videoModeCache;
#ifdef LLCV_RECORDING
    std::atomic<bool> recordingProbeStop{false};
    std::unique_ptr<llcv::settings_ui::SettingsModeCache<llcv::recording::EncoderProbeResult>> recordingProbe;
#endif
    std::optional<llcv::settings_ui::ModeQueryKey> appliedVideoModeKey;
    bool videoModesPending = false;
    llcv::settings_ui::SettingsTheme theme;
    std::vector<llcv::display::MonitorChoice> displayMonitors;
    llcv::settings_ui::AudioOutputSelections audioSelections;
    std::unique_ptr<llcv::settings_ui::SettingsModeCache<AudioClient3Support>> sharedProbeCache;
    std::optional<std::wstring> appliedSharedEndpoint;
    llcv::update::UpdateCheckTask updateCheckTask;
    std::thread exclusiveProbeThread;
    std::unique_ptr<llcv::settings_ui::SettingsModeCache<InternalCaptureAudioProbe>> captureAudioProbeCache;
    std::optional<std::wstring> appliedCaptureAudioDevice;
    std::unique_ptr<llcv::settings_ui::SettingsModeCache<SettingsDeviceInventory>> inventoryCache;
    bool inventoryPending = false;
    bool probeReady = false; // Published cache results are applied on the UI thread.
    std::atomic<bool> exclusiveProbeStop{false};
    std::atomic<bool> exclusiveScanRunning{false};
    std::mutex exclusiveProbeResultsMutex;
    std::vector<ExclusiveEndpointProbeResult> pendingExclusiveProbeResults;
    bool captureAudioProbeReady = false;
    AudioClient3Support probe{};
    InternalCaptureAudioProbe captureAudioProbe{};
    std::vector<UINT32> sharedPeriodChoices;
    std::vector<CaptureDeviceInfo> captureDevices;
    std::vector<CaptureDeviceInfo> captureAudioDevices;
    std::vector<AudioEndpointInfo> audioEndpoints;
    std::vector<ExclusiveEndpointVerification> exclusiveEndpointResults;
    std::vector<llcv::asio::DriverInfo> asioDrivers;
    std::vector<PixelFormatSupport> pixelFormats;
    HRESULT videoCapabilityQueryStatus = S_OK;
    llcv::video::VideoCapabilityDiagnostics videoCapabilityDiagnostics;
    VideoPreset initialVideoPreset = VideoPreset::R1920x1080;
    HMONITOR viewerMonitor = nullptr;
    UINT32 selectedSharedPeriodFrames = 0;
    int selectedBufferMs = kRecommendedWasapiBufferMs;
    std::wstring exclusiveVerifiedEndpointId;
    size_t exclusiveScanCompleted = 0;
    int exclusiveVerifiedBufferMs = 0;
    bool bufferItemsAreSharedFrames = false;
    bool asioAvailable = false;
    bool accepted = false;
};

static VideoPixelFormat SelectedPixelFormat(
    const SettingsDialogState* state);
static bool SettingsUsesExclusiveMode(
    const SettingsDialogState* state);
static bool SettingsUsesAsioMode(const SettingsDialogState* state);
static bool SettingsCanStart(const SettingsDialogState* state);
static std::wstring SelectedAudioEndpointId(const SettingsDialogState* state);
static void RequestSettingsSharedProbe(SettingsDialogState* state, HWND hwnd);

// The view consumes selections, not device/probe state.
static void UpdateAdvancedControlVisibility(SettingsDialogState* state) {
    if (!state) return;
#ifdef LLCV_RECORDING
    if(state->recordingLive){
        llcv::settings_ui::UpdateAdvancedControlVisibility(state,false,VideoPixelFormat::Nv12);
        SetSettingsText(state->startButton,state->english?L"Apply / close":L"적용 후 닫기");
        EnableWindow(state->startButton,TRUE);
        return;
    }
#endif
    llcv::settings_ui::UpdateAdvancedControlVisibility(
        state, SettingsUsesExclusiveMode(state), SelectedPixelFormat(state),
        {SettingsCanStart(state),
         !state->videoModesPending && !state->pixelFormats.empty(),
         !state->captureAudioProbeCache ||
             state->captureAudioProbeReady,
         !state->exclusiveScanRunning.load(std::memory_order_acquire),
         !SettingsUsesAsioMode(state)});
    EnableWindow(state->videoRefreshButton, !state->inventoryPending &&
        !state->exclusiveScanRunning.load(std::memory_order_acquire));
}

static void SetSettingsUpdateStatus(SettingsDialogState* state,
                                    const std::wstring& text) {
    if (!state || !state->updateStatus) return;
    SetWindowTextW(state->updateStatus, text.c_str());
}

static void StartSettingsUpdateCheck(SettingsDialogState* state, HWND hwnd) {
    if (!state || !hwnd || state->updateCheckTask.IsRunning()) return;
    if (!state->updateCheckTask.Start(kAppVersionLabel, [hwnd]() {
            PostMessageW(hwnd, WM_SETTINGS_UPDATE_CHECK_COMPLETE, 0, 0);
        })) {
        SetSettingsUpdateStatus(state, UI_TEXT(L"업데이트를 확인하지 못했습니다. 인터넷 연결을 확인한 뒤 다시 시도하세요."));
        return;
    }
    SetSettingsUpdateStatus(state, UI_TEXT(L"최신 버전 확인 중…"));
    EnableWindow(state->updateNowButton, FALSE);
}

static std::wstring SettingsHelpText(SettingsHelpTopic topic, const SettingsDialogState* state) {
    return keys::Expand(llcv::settings_ui::SettingsHelpText(topic, IsEnglishUi()),state->shortcutBindings);
}

static bool SettingsUsesSharedMode(const SettingsDialogState* state) {
    return state && state->audioCombo && SendMessageW(state->audioCombo, CB_GETCURSEL, 0, 0) == 0;
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

static void RememberAudioOutputChoice(SettingsDialogState* state) {
    if (!state || !state->audioOutputCombo) return;
    state->audioSelections.Initialize(g_settings.audioOutputDeviceId,
                                     g_settings.asioDriverName);
    const LRESULT previousSelection = SendMessageW(
        state->audioOutputCombo, CB_GETCURSEL, 0, 0);
    if (previousSelection >= 0) {
        if (state->audioSelections.displayedAsio) {
            if (static_cast<size_t>(previousSelection) < state->asioDrivers.size())
                state->audioSelections.Remember(AsioDriverNameWide(
                    state->asioDrivers[previousSelection].name));
        } else if (previousSelection == 0) {
            state->audioSelections.Remember({});
        } else if (static_cast<size_t>(previousSelection - 1) < state->audioEndpoints.size()) {
            state->audioSelections.Remember(state->audioEndpoints[previousSelection - 1].id);
        }
    }
}

static void PopulateAudioOutputCombo(SettingsDialogState* state) {
    if (!state || !state->audioOutputCombo) return;
    RememberAudioOutputChoice(state);
    state->audioSelections.displayedAsio = SettingsUsesAsioMode(state);
    SendMessageW(state->audioOutputCombo, CB_RESETCONTENT, 0, 0);
    if (SettingsUsesAsioMode(state)) {
        SetSettingsText(state->audioOutputLabel, UI_TEXT(L"ASIO 출력 드라이버"));
        LRESULT selected = 0;
        for (size_t i = 0; i < state->asioDrivers.size(); ++i) {
            const std::wstring name = AsioDriverNameWide(
                state->asioDrivers[i].name);
            const LRESULT index = SendMessageW(
                state->audioOutputCombo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(name.c_str()));
            if (name == state->audioSelections.asioDriver) selected = index;
        }
        SendMessageW(state->audioOutputCombo, CB_SETCURSEL, selected, 0);
        return;
    }

    SetSettingsText(state->audioOutputLabel, UI_TEXT(L"오디오 출력 장치"));
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
                } else if (result.state == ExclusiveEndpointState::RetryRequired) {
                    defaultLabel += UI_TEXT(L" · 확인 보류");
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
            } else if (result.state == ExclusiveEndpointState::RetryRequired) {
                label += UI_TEXT(L" (확인 보류)");
            }
        }
        const LRESULT index = SendMessageW(
            state->audioOutputCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (state->audioEndpoints[i].id == state->audioSelections.wasapiEndpoint) {
            selected = index;
        }
    }
    SendMessageW(state->audioOutputCombo, CB_SETCURSEL, selected, 0);
}

static void RememberCurrentBufferChoice(SettingsDialogState* state) {
    if (!state || !state->bufferCombo) return;
    const LRESULT index = SendMessageW(state->bufferCombo, CB_GETCURSEL, 0, 0);
    if (index < 0) return;
    const LRESULT value = SendMessageW(state->bufferCombo, CB_GETITEMDATA,
                                       static_cast<WPARAM>(index), 0);
    // Only positive millisecond/frame values are choices. ASIO's explanatory
    // row has zero item data, even when the mode combo has already changed.
    if (value <= 0) return;
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
                                 state->probeReady &&
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
        SetSettingsText(state->audioStatus, UI_TEXT(
            L"ASIO 출력 · 드라이버 기본 버퍼 사용 · 앱 클록 보정 가능"));
        return;
    }
    if (SettingsUsesExclusiveMode(state)) {
        // Exclusive verification owns this row; never flash a generic status
        // between the previous mode and the current scan/verdict.
        return;
    }
    if (state->surround51Check && SendMessageW(state->surround51Check,
            BM_GETCHECK, 0, 0) == BST_CHECKED) {
        SetSettingsText(state->audioStatus, keys::Expand(IsEnglishUi()
            ? L"Console 5.1: output period is negotiated on start; actual value in Tab diagnostics."
            : L"콘솔 5.1: 출력 주기는 시작 시 협상 · 실제 값은 Tab 진단에서 확인",state->shortcutBindings).c_str());
        return;
    }
    if (!state->probeReady) {
        SetSettingsText(state->audioStatus, UI_TEXT(L"Shared 저지연 지원 확인 중…"));
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
    SetSettingsText(state->audioStatus, status);
}

static bool ProcessSettingsShortcutEdit(SettingsDialogState* state, const MSG& message,
                                        unsigned modifiers=keys::Modifiers()) {
    const auto previous=state->shortcutBindings;
    const bool consumed=llcv::settings_ui::ProcessShortcutEdit(state,message,modifiers);
    if (previous!=state->shortcutBindings) UpdateAudioClient3Status(state);
    return consumed;
}

static void RequestSettingsSharedProbe(SettingsDialogState* state, HWND hwnd) {
    if (!state || !hwnd || !SettingsUsesSharedMode(state)) return;
    const std::wstring configured = SelectedAudioEndpointId(state);
    // Cache against the actual endpoint, not the mutable "Windows default"
    // choice. Notifications for obsolete requests never apply to a new choice.
    std::wstring endpoint = configured;
    if (endpoint.empty()) {
        const auto found = std::find_if(state->audioEndpoints.begin(), state->audioEndpoints.end(),
            [](const AudioEndpointInfo& item) { return item.isDefault; });
        if (found != state->audioEndpoints.end()) endpoint = found->id;
    }
    if (state->appliedSharedEndpoint == endpoint && state->probeReady) return;
    if (!state->sharedProbeCache) {
        state->sharedProbeCache = std::make_unique<llcv::settings_ui::SettingsModeCache<AudioClient3Support>>(
            [](const llcv::settings_ui::ModeQueryKey& request) {
                return llcv::audio_device::ProbeSharedModeSupport(request.device);
            }, [hwnd] { PostMessageW(hwnd, WM_AUDIOCLIENT3_PROBE_COMPLETE, 0, 0); });
    }
    const auto result = state->sharedProbeCache->Request({endpoint, 0, 0});
    state->probeReady = result.has_value();
    if (result) {
        state->probe = *result;
        state->appliedSharedEndpoint = endpoint;
    }
    UpdateAudioClient3Status(state);
    RememberCurrentBufferChoice(state);
    PopulateSettingsBufferCombo(state);
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
    SetSettingsControlVisible(state->exclusiveTestButton, visible, !running && !state->inventoryPending);
    if (!visible) return;
    SetSettingsText(state->exclusiveTestButton,
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

static void EnsureExclusiveBufferForSelection(SettingsDialogState* state) {
    if (!SettingsUsesExclusiveMode(state)) return;
    const int minimumMs = ExclusiveVerifiedBufferForSelection(state);
    // A recommendation is a verified minimum, not a command to discard a
    // larger buffer chosen by the user when another endpoint finishes.
    if (IsExclusiveLowLatencyBuffer(minimumMs) &&
        (!IsExclusiveLowLatencyBuffer(state->selectedBufferMs) ||
         state->selectedBufferMs < minimumMs)) {
        state->selectedBufferMs = minimumMs;
        PopulateSettingsBufferCombo(state);
    }
}

static void PersistCompletedExclusiveEndpointResults(
    const SettingsDialogState* state) {
    if (!state) return;
    bool changed = false;
    for (size_t i = 0; i < state->audioEndpoints.size() &&
                       i < state->exclusiveEndpointResults.size(); ++i) {
        const auto& result = state->exclusiveEndpointResults[i];
        if (result.state != ExclusiveEndpointState::Supported &&
            result.state != ExclusiveEndpointState::Unsupported &&
            result.state != ExclusiveEndpointState::RetryRequired) {
            continue;
        }
        const bool supported = result.state == ExclusiveEndpointState::Supported;
        const bool inconclusive = result.state == ExclusiveEndpointState::RetryRequired;
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
                state->audioEndpoints[i].id, supported, recommendedBufferMs, inconclusive});
            changed = true;
        } else if (it->supported != supported ||
                   it->recommendedBufferMs != recommendedBufferMs ||
                   it->inconclusive != inconclusive) {
            it->supported = supported;
            it->recommendedBufferMs = recommendedBufferMs;
            it->inconclusive = inconclusive;
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

static int SelectedCaptureFrameRate(const SettingsDialogState* state) {
    if (!state || !state->frameRateCombo) return 0;
    const LRESULT index = SendMessageW(state->frameRateCombo, CB_GETCURSEL, 0, 0);
    const LRESULT value = index >= 0
        ? SendMessageW(state->frameRateCombo, CB_GETITEMDATA, index, 0) : CB_ERR;
    return value == CB_ERR ? 0 : static_cast<int>(value);
}

static bool SettingsVideoSelectionAvailable(const SettingsDialogState* state) {
    if (!state || state->videoModesPending) return false;
    const auto format = SelectedPixelFormat(state);
    const int fps = SelectedCaptureFrameRate(state);
    const bool directRequest =
        SendMessageW(state->capture40Check, BM_GETCHECK, 0, 0) == BST_CHECKED ||
        (fps == 30 && SendMessageW(state->unlistedFpsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    return std::any_of(state->pixelFormats.begin(), state->pixelFormats.end(),
        [=](const PixelFormatSupport& mode) {
            const bool matchesFormat = format == VideoPixelFormat::Auto
                ? IsAutoSelectableVideoFormat(mode.format) : mode.format == format;
            return matchesFormat && mode.selectedFps > 0 &&
                (directRequest || fps == 0 || mode.selectedFps == fps);
        });
}

static bool SettingsCanStart(const SettingsDialogState* state) {
    if (!state) return false;
    const bool audioOnly = state->audioOnlyCheck &&
        SendMessageW(state->audioOnlyCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return (audioOnly || SettingsVideoSelectionAvailable(state)) &&
        HasExclusiveVerificationForSelection(state);
}

static void UpdateExclusiveVerificationUi(SettingsDialogState* state) {
    if (!state || !state->startButton) return;
    const bool running = state->exclusiveScanRunning.load(
        std::memory_order_acquire);
    if (!SettingsUsesExclusiveMode(state)) {
        // Shared/ASIO must not be blocked by a diagnostic scan that is only
        // relevant to Exclusive. Its results can arrive after a mode change.
        EnableWindow(state->startButton, SettingsCanStart(state));
        return;
    }
    const bool verified = HasExclusiveVerificationForSelection(state);
    EnableWindow(state->startButton, SettingsCanStart(state));
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
            SetSettingsText(state->audioStatus, status);
        } else if (verified) {
            wchar_t status[160]{};
            swprintf_s(status, UI_TEXT(
                L"Exclusive 사용 가능 · 현재 출력 장치 · %d ms 이상"),
                ExclusiveVerifiedBufferForSelection(state));
            SetSettingsText(state->audioStatus, status);
        } else if (selectedResult &&
                   selectedResult->state == ExclusiveEndpointState::Supported) {
            wchar_t status[160]{};
            swprintf_s(status, UI_TEXT(
                L"Exclusive 사용 가능 · %d ms 이상 선택 필요"),
                selectedResult->recommendedBufferMs);
            SetSettingsText(state->audioStatus, status);
        } else if (selectedResult &&
                   selectedResult->state == ExclusiveEndpointState::RetryRequired) {
            SetSettingsText(state->audioStatus, UI_TEXT(
                L"Exclusive 확인 보류 · 장치 상태 확인 후 다시 검사해 주세요"));
        } else if (selectedResult &&
                   selectedResult->state == ExclusiveEndpointState::Unsupported) {
            // Other endpoints may still be running, but this selected one has
            // a conclusive result already and should say so immediately.
            SetSettingsText(state->audioStatus, UI_TEXT(
                L"Exclusive 사용 불가 · 현재 출력 장치"));
        } else {
            SetSettingsText(state->audioStatus, UI_TEXT(
                L"Exclusive 검사 필요 · 현재 출력 장치"));
        }
    }
}

static void QueueExclusiveEndpointProbeResult(
    SettingsDialogState* state, ExclusiveEndpointProbeResult result) {
    if (!state) return;
    std::lock_guard<std::mutex> lock(state->exclusiveProbeResultsMutex);
    state->pendingExclusiveProbeResults.push_back(std::move(result));
}

static void ConsumeExclusiveEndpointProbeResults(SettingsDialogState* state) {
    if (!state) return;
    std::vector<ExclusiveEndpointProbeResult> pending;
    {
        std::lock_guard<std::mutex> lock(state->exclusiveProbeResultsMutex);
        pending.swap(state->pendingExclusiveProbeResults);
    }
    for (const auto& message : pending) {
        if (message.endpointIndex >= state->exclusiveEndpointResults.size() ||
            message.endpointIndex >= state->audioEndpoints.size()) {
            continue;
        }
        auto& result = state->exclusiveEndpointResults[message.endpointIndex];
        const bool wasPending = result.state == ExclusiveEndpointState::Testing ||
                                result.state == ExclusiveEndpointState::Unknown;
        result.state = message.probe.compatible
            ? ExclusiveEndpointState::Supported
            : message.probe.inconclusive ? ExclusiveEndpointState::RetryRequired
                                         : ExclusiveEndpointState::Unsupported;
        result.recommendedBufferMs = message.probe.compatible
            ? static_cast<int>((message.probe.requestedFrames * 1000 +
                                kSampleRate / 2) / kSampleRate)
            : 0;
        result.summary = message.probe.summary;
        if (wasPending) ++state->exclusiveScanCompleted;
        fwprintf(stderr,
                 L"[audio][exclusive-scan] %s: %s | requested=%u frames "
                 L"actual=%u frames\n",
                 state->audioEndpoints[message.endpointIndex].name.c_str(),
                 result.summary.c_str(), message.probe.requestedFrames,
                 message.probe.actualBufferFrames);
    }
    // Save completed diagnostics even while other endpoints are still running.
    // The user's unaccepted settings controls are never copied into g_settings.
    if (!pending.empty()) PersistCompletedExclusiveEndpointResults(state);
}

static void CompleteExclusiveEndpointScan(SettingsDialogState* state) {
    if (!state) return;
    if (state->exclusiveProbeThread.joinable()) {
        state->exclusiveProbeThread.join();
    }
    // The completion notification owns the transition to idle. A worker that
    // has finished but whose notifications are still queued must not allow a
    // new scan to replace its thread or mix old verdicts into the new scan.
    ConsumeExclusiveEndpointProbeResults(state);
    if (state->exclusiveProbeStop.load(std::memory_order_acquire)) {
        for (auto& result : state->exclusiveEndpointResults) {
            if (result.state == ExclusiveEndpointState::Testing) {
                result = {};
            }
        }
    }
    state->exclusiveScanRunning.store(false, std::memory_order_release);
}

static void RefreshExclusiveScanUi(SettingsDialogState* state, HWND hwnd) {
    EnableWindow(state->videoRefreshButton, !state->inventoryPending &&
        !state->exclusiveScanRunning.load(std::memory_order_acquire));
    // Late results belong in the cache, but must not rebuild Shared/ASIO UI.
    if (!SettingsUsesExclusiveMode(state)) return;
    llcv::settings_ui::SettingsVisualUpdate visualUpdate(hwnd);
    PopulateAudioOutputCombo(state);
    EnsureExclusiveBufferForSelection(state);
    UpdateExclusiveProbeControl(state);
    UpdateExclusiveVerificationUi(state);
}

static void StartExclusiveEndpointScan(SettingsDialogState* state, HWND hwnd,
                                       bool forceRestart = false) {
    if (!state || state->inventoryPending || state->exclusiveScanRunning.load(std::memory_order_acquire)) {
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
            result.state == ExclusiveEndpointState::Unsupported ||
            result.state == ExclusiveEndpointState::RetryRequired) {
            ++completed;
        } else {
            result.state = ExclusiveEndpointState::Testing;
        }
    }
    state->exclusiveScanCompleted = completed;
    state->exclusiveProbeStop.store(false, std::memory_order_release);
    state->exclusiveScanRunning.store(true, std::memory_order_release);
    RefreshExclusiveScanUi(state, hwnd);

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
                ExclusiveEndpointProbeResult message{};
                message.endpointIndex = i;
                message.probe = ProbeExclusiveBufferRecommendation(
                    endpoints[i].id, &state->exclusiveProbeStop);
                if (state->exclusiveProbeStop.load(std::memory_order_acquire)) break;
                // Results belong to the dialog state, not its message queue.
                // Closing the HWND can discard notifications safely: the
                // state outlives the joined worker and persists unread results.
                QueueExclusiveEndpointProbeResult(state, std::move(message));
                if (!PostMessageW(hwnd, WM_EXCLUSIVE_ENDPOINT_PROBE_COMPLETE,
                                  0, 0)) {
                    break;
                }
            }
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
        state->captureAudioProbeReady
            ? state->captureAudioProbe.state
            : InternalCaptureAudioState::Checking;

    llcv::settings_ui::SetSettingsText(state->captureAudioDeviceLabel,
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
        llcv::settings_ui::SetSettingsText(state->captureAudioStatus,
                       UI_TEXT(L"영상 장치 내부 오디오 감지됨 · 자동 사용"));
    } else {
        llcv::settings_ui::SetSettingsText(state->captureAudioStatus,
                       UI_TEXT(L"내부 오디오 확인 중…"));
    }
}

static void StartCaptureAudioProbe(SettingsDialogState* state, HWND hwnd) {
    if (!state || !hwnd) return;
    const std::wstring captureDeviceId = SelectedCaptureDeviceId(state);
    if (state->appliedCaptureAudioDevice == captureDeviceId && state->captureAudioProbeReady) {
        UpdateCaptureAudioSelectionUi(state);
        return;
    }
    if (!state->captureAudioProbeCache)
        state->captureAudioProbeCache = std::make_unique<llcv::settings_ui::SettingsModeCache<InternalCaptureAudioProbe>>(
            [](const llcv::settings_ui::ModeQueryKey& request) { return ProbeInternalCaptureAudio(request.device); },
            [hwnd] { PostMessageW(hwnd, WM_CAPTURE_AUDIO_PROBE_COMPLETE, 0, 0); });
    const auto result = state->captureAudioProbeCache->Request({captureDeviceId, 0, 0});
    state->captureAudioProbeReady = result.has_value();
    if (result) {
        state->captureAudioProbe = *result;
        state->appliedCaptureAudioDevice = captureDeviceId;
    }
    EnableWindow(state->captureDeviceCombo, result.has_value());
    UpdateCaptureAudioSelectionUi(state);
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

    const bool supported = !state->videoModesPending && !state->pixelFormats.empty();
    const bool audioOnly = state->audioOnlyCheck &&
        SendMessageW(state->audioOnlyCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (state->pixelFormatCombo) {
        EnableWindow(state->pixelFormatCombo, supported ? TRUE : FALSE);
    }
    if (state->frameRateCombo) {
        EnableWindow(state->frameRateCombo, supported &&
            SendMessageW(state->capture40Check, BM_GETCHECK, 0, 0) != BST_CHECKED);
    }
    if (state->capture40Check) EnableWindow(state->capture40Check, supported);
    if (state->unlistedFpsCheck) EnableWindow(state->unlistedFpsCheck, supported &&
        SendMessageW(state->capture40Check, BM_GETCHECK, 0, 0) != BST_CHECKED);
    if (state->startButton) {
        EnableWindow(state->startButton, SettingsCanStart(state));
    }
    if (!state->videoCapabilityStatus) return;
    const auto selectedFormat = SelectedPixelFormat(state);
    bool hasSelectedMode = false;
    bool hasListed30 = false;
    for (const auto& mode : state->pixelFormats) {
        if (selectedFormat == VideoPixelFormat::Auto
            ? !IsAutoSelectableVideoFormat(mode.format) : mode.format != selectedFormat)
            continue;
        hasSelectedMode = true;
        hasListed30 |= mode.selectedFps == 30;
    }
    const bool showDirectRequest = supported && !audioOnly && hasSelectedMode && !hasListed30 &&
        SendMessageW(state->unlistedFpsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED &&
        SendMessageW(state->capture40Check, BM_GETCHECK, 0, 0) != BST_CHECKED;
    llcv::settings_ui::SetSettingsText(state->unlistedFpsStatus, showDirectRequest
        ? (IsEnglishUi() ? L"Direct request: 30 fps" : L"직접 요청: 30 fps") : L"");
    if (supported && !audioOnly && !SettingsVideoSelectionAvailable(state)) {
        llcv::settings_ui::SetSettingsText(state->unlistedFpsStatus, hasSelectedMode
            ? (IsEnglishUi() ? L"Selected FPS unavailable: select another rate"
                             : L"선택한 FPS 미지원 · 다른 FPS 선택")
            : (IsEnglishUi() ? L"Selected format unavailable: select another format"
                             : L"선택한 포맷 미지원 · 다른 포맷 선택"));
    }

    std::wstring message;
    if (audioOnly) {
        message = UI_TEXT(L"오디오 only: 영상 형식 확인 안 함");
    } else if (state->videoModesPending) {
        message = IsEnglishUi() ? L"Checking this capture resolution…" : L"선택한 캡처 해상도 확인 중…";
    } else if (!supported && FAILED(state->videoCapabilityQueryStatus)) {
        wchar_t failure[256]{};
        swprintf_s(failure, IsEnglishUi()
            ? L"Device mode query failed (0x%08X). Use Refresh to retry."
            : L"장치 모드 조회 실패 (0x%08X). 새로고침을 눌러 다시 확인하세요.",
            static_cast<unsigned>(state->videoCapabilityQueryStatus));
        message = failure;
    } else if (!supported && state->videoCapabilityDiagnostics.UnsupportedFormatsOnly()) {
        std::wstring names;
        const auto& formats = state->videoCapabilityDiagnostics.unsupportedFormatNames;
        for (size_t i = 0; i < formats.size() && i < 3; ++i) {
            if (!names.empty()) names += L", ";
            names += formats[i];
        }
        if (formats.size() > 3) names += L", …";
        message = IsEnglishUi()
            ? L"Only unsupported pixel formats at this resolution:\r\n" + names
            : L"선택한 해상도에서 미지원 픽셀 포맷만 제공됩니다:\r\n" + names;
    } else if (!supported) {
        message = UI_TEXT(L"지원 모드 없음: 다른 장치 또는 해상도를 선택하세요.");
    } else {
        message = FAILED(state->videoCapabilityQueryStatus)
            ? (IsEnglishUi() ? L"Partial query failure; some modes may be missing:\r\n"
                            : L"일부 조회 실패 · 모드가 누락될 수 있음:\r\n")
            : (IsEnglishUi() ? L"Detected:\r\n" : L"자동 인식:\r\n");
        bool firstFormat = true;
        for (const auto format : {VideoPixelFormat::Nv12,
                                  VideoPixelFormat::Yuy2,
                                  VideoPixelFormat::Uyvy,
                                  VideoPixelFormat::Rgb24,
                                  VideoPixelFormat::Rgb32,
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
    llcv::settings_ui::SetSettingsText(state->videoCapabilityStatus, message.c_str());
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
            reinterpret_cast<LPARAM>(FAILED(state->videoCapabilityQueryStatus)
                ? (IsEnglishUi() ? L"Query failed" : L"조회 실패") : UI_TEXT(L"지원 프레임 없음")));
        SendMessageW(state->frameRateCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(noModeIndex), desiredFrameRate);
        SendMessageW(state->frameRateCombo, CB_SETCURSEL,
                     static_cast<WPARAM>(noModeIndex), 0);
        return;
    }
    const LRESULT autoIndex = SendMessageW(
        state->frameRateCombo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(IsEnglishUi() ? L"Auto (recommended)" : L"자동 (권장)"));
    SendMessageW(state->frameRateCombo, CB_SETITEMDATA,
                 static_cast<WPARAM>(autoIndex), 0);
    LRESULT selectedIndex = desiredFrameRate == 0 ? autoIndex : CB_ERR;
    const VideoPixelFormat selectedFormat = SelectedPixelFormat(state);
    std::vector<int> frameRates;
    for (const auto& support : state->pixelFormats) {
        if (selectedFormat == VideoPixelFormat::Auto) {
            if (!IsAutoSelectableVideoFormat(support.format)) continue;
        } else if (support.format != selectedFormat) {
            continue;
        }
        if (support.selectedFps > 0) frameRates.push_back(support.selectedFps);
    }
    // Explicit request only. Keep detected capabilities truthful and leave
    // automatic selection untouched, even when the driver omits 30 fps.
    if (!frameRates.empty() &&
        SendMessageW(state->unlistedFpsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED)
        frameRates.push_back(30);
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
    // A refresh or mode change must never turn an explicit FPS into Auto.
    // Preserve the choice visibly; Start stays disabled until it is available
    // again or the user deliberately selects another rate.
    if (selectedIndex == CB_ERR) {
        wchar_t label[96]{};
        swprintf_s(label, IsEnglishUi() ? L"%d fps (unavailable)" : L"%d fps (지원 안 됨)", desiredFrameRate);
        selectedIndex = SendMessageW(state->frameRateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(state->frameRateCombo, CB_SETITEMDATA, selectedIndex, desiredFrameRate);
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
    const llcv::settings_ui::ModeQueryKey key{SelectedCaptureDeviceId(state), preset.width, preset.height};
    // Display-only changes, repeated notifications and completed stale queries
    // must not reset the lists or re-open a capture device.
    if (state->appliedVideoModeKey == key && !state->videoModesPending) return;
    if (!state->videoModeCache) {
        const HWND owner = GetParent(state->videoCombo);
        state->videoModeCache = std::make_unique<llcv::settings_ui::SettingsModeCache<SettingsVideoModes>>(
            [](const llcv::settings_ui::ModeQueryKey& request) {
                SettingsVideoModes result;
                result.formats = ProbePixelFormats(request.device, request.width, request.height, &result.status, &result.diagnostics);
                return result;
            }, [owner] { PostMessageW(owner, WM_SETTINGS_VIDEO_MODES_READY, 0, 0); });
    }
    const auto modes = state->videoModeCache->Request(key);
    if (!modes) {
        state->videoModesPending = true;
        UpdateVideoCapabilityStatus(state);
        return;
    }
    state->videoModesPending = false;
    state->appliedVideoModeKey = key;
    VideoPixelFormat desiredFormat = g_settings.pixelFormat;
    if (SendMessageW(state->pixelFormatCombo, CB_GETCOUNT, 0, 0) > 0) {
        desiredFormat = SelectedPixelFormat(state);
    }
    state->pixelFormats = modes->formats;
    state->videoCapabilityQueryStatus = modes->status;
    state->videoCapabilityDiagnostics = modes->diagnostics;
    SendMessageW(state->pixelFormatCombo, CB_RESETCONTENT, 0, 0);
    if (state->pixelFormats.empty()) {
        const LRESULT noModeIndex = SendMessageW(
            state->pixelFormatCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(FAILED(state->videoCapabilityQueryStatus)
                ? (IsEnglishUi() ? L"Query failed" : L"조회 실패") : UI_TEXT(L"지원 포맷 없음")));
        SendMessageW(state->pixelFormatCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(noModeIndex),
                     static_cast<LPARAM>(desiredFormat));
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
    LRESULT selectedIndex = desiredFormat == VideoPixelFormat::Auto ? autoIndex : CB_ERR;
    for (const auto format : {VideoPixelFormat::Nv12,
                              VideoPixelFormat::Yuy2,
                              VideoPixelFormat::Uyvy,
                              VideoPixelFormat::Rgb24,
                              VideoPixelFormat::Rgb32,
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
                : format == VideoPixelFormat::Uyvy
                    ? L"UYVY / HDYC 8-bit 4:2:2"
                : format == VideoPixelFormat::Rgb24
                    ? L"RGB24 8-bit BGR"
                : format == VideoPixelFormat::Rgb32
                    ? L"XRGB (RGB32) 8-bit BGRX"
                : format == VideoPixelFormat::P010
                    ? UI_TEXT(L"P010 10-bit HDR10")
                    : UI_TEXT(L"MJPEG (압축 호환)");
        const LRESULT index = SendMessageW(
            state->pixelFormatCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(label));
        SendMessageW(state->pixelFormatCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(index),
                     static_cast<LPARAM>(format));
        if (format == desiredFormat) selectedIndex = index;
    }
    if (selectedIndex == CB_ERR) {
        const std::wstring label = std::wstring(PixelFormatName(desiredFormat)) +
            (IsEnglishUi() ? L" (unavailable)" : L" (지원 안 됨)");
        selectedIndex = SendMessageW(state->pixelFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(state->pixelFormatCombo, CB_SETITEMDATA, selectedIndex, static_cast<LPARAM>(desiredFormat));
    }
    SendMessageW(state->pixelFormatCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selectedIndex), 0);
    PopulateFrameRateCombo(state);
    UpdateVideoCapabilityStatus(state);
}

// Refresh inventory only on demand. Keep selections and completed Exclusive
// verdicts by persistent identity, never by the enumeration order.
static void ApplySettingsInventory(SettingsDialogState* state, SettingsDeviceInventory inventory) {
    const auto captureId = SelectedCaptureDeviceId(state);
    const auto captureAudioId = SelectedCaptureAudioDeviceId(state);
    RememberAudioOutputChoice(state);
    const auto retainMissing = [](auto& fresh, const auto& old, const std::wstring& selected) {
        if (selected.empty() || std::any_of(fresh.begin(), fresh.end(),
            [&](const auto& device) { return device.id == selected; })) return;
        const auto found = std::find_if(old.begin(), old.end(),
            [&](const auto& device) { return device.id == selected; });
        if (found != old.end()) fresh.push_back(*found);
    };
    retainMissing(inventory.videoInputs, state->captureDevices, captureId);
    retainMissing(inventory.audioInputs, state->captureAudioDevices, captureAudioId);
    retainMissing(inventory.audioOutputs, state->audioEndpoints, state->audioSelections.wasapiEndpoint);
    std::vector<ExclusiveEndpointVerification> verified(inventory.audioOutputs.size());
    size_t completed = 0;
    for (size_t i = 0; i < inventory.audioOutputs.size(); ++i) {
        for (size_t old = 0; old < state->audioEndpoints.size() && old < state->exclusiveEndpointResults.size(); ++old)
            if (inventory.audioOutputs[i].id == state->audioEndpoints[old].id) {
                verified[i] = state->exclusiveEndpointResults[old];
                break;
            }
        if (verified[i].state == ExclusiveEndpointState::Supported ||
            verified[i].state == ExclusiveEndpointState::Unsupported ||
            verified[i].state == ExclusiveEndpointState::RetryRequired) ++completed;
    }
    // Clear old indices before replacing their backing arrays.
    SendMessageW(state->audioOutputCombo, CB_RESETCONTENT, 0, 0);
    state->captureDevices = std::move(inventory.videoInputs);
    state->captureAudioDevices = std::move(inventory.audioInputs);
    state->audioEndpoints = std::move(inventory.audioOutputs);
    state->exclusiveEndpointResults = std::move(verified);
    state->exclusiveScanCompleted = completed;
    llcv::settings_ui::PopulateCaptureDevicePicker(state->captureDeviceCombo,
        state->captureDevices, captureId, false, state->english);
    llcv::settings_ui::PopulateCaptureDevicePicker(state->captureAudioDeviceCombo,
        state->captureAudioDevices, captureAudioId, true, state->english);
    PopulateAudioOutputCombo(state);
}

static void RequestSettingsInventory(SettingsDialogState* state, HWND hwnd, bool refresh) {
    if (!state || state->exclusiveScanRunning.load(std::memory_order_acquire)) return;
    if (refresh && state->inventoryPending) return;
    if (!state->inventoryCache)
        state->inventoryCache = std::make_unique<llcv::settings_ui::SettingsModeCache<SettingsDeviceInventory>>(
            [](const llcv::settings_ui::ModeQueryKey&) {
                return SettingsDeviceInventory{EnumerateCaptureDevices(), EnumerateCaptureAudioDevices(),
                    llcv::audio_device::EnumerateRenderEndpoints()};
            }, [hwnd] { PostMessageW(hwnd, WM_SETTINGS_INVENTORY_READY, 0, 0); });
    if (refresh) state->inventoryCache->Invalidate();
    const auto inventory = state->inventoryCache->Request({});
    state->inventoryPending = !inventory.has_value();
    if (inventory) {
        llcv::settings_ui::SettingsVisualUpdate visualUpdate(hwnd);
        ApplySettingsInventory(state, *inventory);
        const auto captureId = SelectedCaptureDeviceId(state);
        if (state->videoModeCache) state->videoModeCache->InvalidateDevice(captureId);
        if (state->captureAudioProbeCache) state->captureAudioProbeCache->InvalidateDevice(captureId);
        state->appliedVideoModeKey.reset();
        state->appliedCaptureAudioDevice.reset();
        // ASIO displays driver names, but Shared must refresh the retained
        // WASAPI choice. Automatic selection resolves against the new inventory.
        const auto endpoint = SettingsUsesAsioMode(state) && !state->audioSelections.wasapiEndpoint.empty()
            ? state->audioSelections.wasapiEndpoint : EffectiveSelectedAudioEndpointId(state);
        if (state->sharedProbeCache) state->sharedProbeCache->InvalidateDevice(endpoint);
        state->appliedSharedEndpoint.reset();
        PopulatePixelFormatCombo(state);
        StartCaptureAudioProbe(state, hwnd);
        RequestSettingsSharedProbe(state, hwnd);
        // Inventory refresh does not retest already known Exclusive endpoints.
        if (SettingsUsesExclusiveMode(state)) StartExclusiveEndpointScan(state, hwnd);
    }
    SetSettingsText(state->videoRefreshButton, state->inventoryPending
        ? (state->english ? L"Refreshing…" : L"새로고침 중…") : UI_TEXT(L"새로고침"));
    UpdateAdvancedControlVisibility(state);
}

static void FinishSettingsDialog(HWND hwnd, SettingsDialogState* state, bool accepted) {
    if (!state) return;
#ifdef LLCV_RECORDING
    if(state->recordingLive){
        if(accepted && state->recordingOptions!=llcv::recording::sessionOptions){
            if(!llcv::recording::SaveOptions(state->recordingOptions)){
                MessageBoxW(hwnd,state->english?L"Could not save recording settings.":L"녹화 설정을 저장하지 못했습니다.",L"LLCV",MB_OK|MB_ICONERROR);return;
            }
            llcv::recording::sessionOptions=state->recordingOptions;
        }
        DestroyWindow(hwnd);return;
    }
#endif
    // Enter/IDOK and direct commands must obey the same guard as mouse clicks.
    if (accepted && (!state->startButton || !IsWindowEnabled(state->startButton) ||
                     !SettingsCanStart(state))) return;

    if (accepted) {
        const VideoPreset previousVideoPreset = llcv::settings::EffectiveDisplayPreset(g_settings);
        const bool previouslyRelative = g_settings.relativeWindowSize;
        RememberCurrentBufferChoice(state);
        const LRESULT audioIndex = SendMessageW(
            state->audioCombo, CB_GETCURSEL, 0, 0);
        const LRESULT presentationIndex = SendMessageW(
            state->presentationCombo, CB_GETCURSEL, 0, 0);
        if (!llcv::presentation::SupportsCapture(
                llcv::presentation_ui::ModeFromSelection(static_cast<int>(presentationIndex)),
                SelectedPixelFormat(state),
                SendMessageW(state->audioOnlyCheck, BM_GETCHECK, 0, 0) == BST_CHECKED)) {
            MessageBoxW(hwnd, IsEnglishUi()
                ? L"HDR10 cannot use compatibility output. Select Immediate or VSync, "
                  L"or use an SDR capture format for this comparison."
                : L"HDR10은 호환성 출력과 함께 사용할 수 없습니다. 저지연 또는 VSync를 "
                  L"선택하거나, SDR 캡처 포맷으로 비교해 주세요.",
                L"LLCV", MB_OK | MB_ICONINFORMATION);
            return;
        }
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
#ifdef LLCV_RECORDING
        if(state->recordingOptions!=llcv::recording::sessionOptions &&
           !llcv::recording::SaveOptions(state->recordingOptions)){
            MessageBoxW(hwnd,IsEnglishUi()?L"Could not save recording settings.":L"녹화 설정을 저장하지 못했습니다.",L"LLCV",MB_OK|MB_ICONERROR);
            return;
        }
        llcv::recording::sessionOptions=state->recordingOptions;
#endif
        g_settings.shortcuts=state->shortcutBindings;
        if (languageIndex >= 0 && languageIndex <= 2) {
            g_settings.uiLanguage = static_cast<UiLanguage>(languageIndex);
        }
        g_settings.settingsLightTheme =
            SendMessageW(state->themeCombo, CB_GETCURSEL, 0, 0) == 1;
        if (audioIndex == 1) {
            g_settings.audioMode = AudioMode::WasapiExclusive;
        } else if (audioIndex == 2 && state->asioAvailable) {
            g_settings.audioMode = AudioMode::Asio;
        } else {
            g_settings.audioMode = AudioMode::WasapiShared;
        }
        if (g_settings.audioMode == AudioMode::Asio) {
            g_settings.asioDriverName = SelectedAsioDriverName(state);
            if (g_settings.asioDriverName.empty()) {
                g_settings.audioMode = AudioMode::WasapiShared;
            }
        } else {
            g_settings.asioDriverName = state->audioSelections.asioDriver;
        }
        llcv::settings_ui::ApplyResolutionSelections(*state, kVideoPresets, g_settings);
        const LRESULT monitorIndex = SendMessageW(state->displayMonitorCombo, CB_GETCURSEL, 0, 0);
        if (monitorIndex == 0) g_settings.preferredDisplayMonitor.clear();
        else if (monitorIndex > 0 && static_cast<size_t>(monitorIndex) <= state->displayMonitors.size())
            g_settings.preferredDisplayMonitor = state->displayMonitors[monitorIndex - 1].id;
        // The disconnected entry preserves its saved identity until reconnected.
        g_settings.presentationMode = llcv::presentation_ui::ModeFromSelection(
            static_cast<int>(presentationIndex));
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
        if (audioIndex == 0 && state->probeReady &&
            state->probe.supported) {
            g_settings.wasapiSharedPeriodFrames =
                state->selectedSharedPeriodFrames;
        }
        g_settings.audioOutputDeviceId = SettingsUsesAsioMode(state)
            ? state->audioSelections.wasapiEndpoint : SelectedAudioEndpointId(state);
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
                g_settings.mjpegMatrixOverride =
                    static_cast<llcv::video_color::MatrixOverride>(value);
            }
        }
        g_settings.sdrRangeOverride = state->sdrRangeOverride;
        g_settings.legacyMjpegRangeOverride = state->legacyMjpegRangeOverride;
        g_settings.saveLog = SendMessageW(
            state->saveLogCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.screenshotClipboard = SendMessageW(
            state->screenshotClipboardCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.capture40Fps = SendMessageW(
            state->capture40Check, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.allowUnlistedFps = SendMessageW(
            state->unlistedFpsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const LRESULT upscaler=SendMessageW(state->upscalerCombo,CB_GETCURSEL,0,0);
        g_settings.vsrEnabled = !state->vsrGpuUnavailable && upscaler==2;
        g_settings.cmaaNisEnabled = upscaler==1;
        if (upscaler==1 || upscaler==2)
            g_settings.preferredUpscaler = upscaler==1 ? llcv::settings::UpscalingMethod::CmaaNis
                : llcv::settings::UpscalingMethod::NvidiaVsr;
        const LRESULT nisSharpness=SendMessageW(state->nisSharpnessCombo,CB_GETCURSEL,0,0);
        g_settings.nisSharpnessPercent = nisSharpness>=0 && nisSharpness<=100 ? int(nisSharpness) : 15;
        g_settings.showDiagnosticConsole = SendMessageW(
            state->showConsoleCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.skipStartupSettings = SendMessageW(
            state->skipStartupCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.checkForUpdates = SendMessageW(
            state->checkForUpdatesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.preventSleep = SendMessageW(
            state->preventSleepCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.muteWhenBackground = SendMessageW(
            state->muteBackgroundCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.audioOnly = SendMessageW(
            state->audioOnlyCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.consoleSurround51 = SendMessageW(
            state->surround51Check, BM_GETCHECK, 0, 0) == BST_CHECKED;
        // The override only has meaning on an explicit P010 selection. Clear
        // any older saved value when switching back to a normal SDR format.
        g_settings.forceHdr10 =
            g_settings.pixelFormat == VideoPixelFormat::P010 &&
            SendMessageW(state->forceHdr10Check, BM_GETCHECK, 0, 0) ==
                BST_CHECKED;
        g_settings.hdrChromaLocation = llcv::hdr::ChromaLocation::Auto;
        if (g_settings.pixelFormat == VideoPixelFormat::P010) {
            const LRESULT selection = SendMessageW(state->hdrChromaCombo, CB_GETCURSEL, 0, 0);
            const LRESULT value = selection == CB_ERR ? CB_ERR :
                SendMessageW(state->hdrChromaCombo, CB_GETITEMDATA, selection, 0);
            if (value == static_cast<LRESULT>(llcv::hdr::ChromaLocation::TopLeft) ||
                value == static_cast<LRESULT>(llcv::hdr::ChromaLocation::Left))
                g_settings.hdrChromaLocation = static_cast<llcv::hdr::ChromaLocation>(value);
        }
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
                previousVideoPreset != llcv::settings::EffectiveDisplayPreset(g_settings) ||
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
        g_settings.roundedCorners = SendMessageW(
            state->roundedCornersCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        g_settings.windowSnap = SendMessageW(
            state->windowSnapCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        // The user has explicitly accepted a new settings profile, so do not
        // preserve a stale immediate-start fallback from the previous run.
        g_exclusiveStartupFallback = false;
        SaveSettings(true);
    }

    // Stop the background per-endpoint scan only when the dialog is actually
    // closing. A disabled Start button must leave the scan running so its
    // result can eventually enable a compatible output.
    state->exclusiveProbeStop.store(true, std::memory_order_release);
    state->accepted = accepted;
    DestroyWindow(hwnd);
}

#ifdef LLCV_RECORDING
static void RequestRecordingEncoders(SettingsDialogState* state,HWND hwnd){
    if(state->activeTab!=SettingsTab::Recording || state->recordingProbeStop.load() ||
        state->recordingProbe || !state->recordingProbePending || llcv::recording::recorder.Busy())return;
    state->recordingProbe=std::make_unique<llcv::settings_ui::SettingsModeCache<llcv::recording::EncoderProbeResult>>(
        [state](const auto&){return llcv::recording::ProbeEncodersIsolated(&state->recordingProbeStop);},
        [hwnd]{PostMessageW(hwnd,WM_RECORD_ENCODERS_READY,0,0);});
    state->recordingProbe->Request({});
}
#endif
static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<SettingsDialogState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<SettingsDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    const bool frameOptionsWereOpen = state && state->frameOptionsOpen;
    const bool frameOptionsHandled = llcv::settings_ui::HandleFrameOptionsMessage(state, hwnd, msg, wParam, lParam);
    if (state && frameOptionsWereOpen != state->frameOptionsOpen) {
        // The flyout reuses controls over the format/color rows. Hide the old
        // fields and enable/show the new ones as one redraw transaction so a
        // help button cannot briefly paint disabled or behind an old field.
        llcv::settings_ui::SettingsVisualUpdate visualUpdate(hwnd);
        UpdateAdvancedControlVisibility(state);
    }
    if (frameOptionsHandled) return 0;
    LRESULT themedResult = 0;
    if (state && state->theme.HandleMessage(msg, wParam, lParam, themedResult)) {
        return themedResult;
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
        const UINT vendor = g_vsrGpuVendor.load(std::memory_order_acquire);
        state->vsrGpuUnavailable = !llcv::vsr::NvidiaAdapter(vendor);
        state->vsrGpuUnknown = vendor == 0;
        const llcv::settings_ui::SettingsControlInitialValues initial{
            g_settings, IsEnglishUi(), state->asioAvailable, kAppVersionLabel,
            state->initialVideoPreset, state->captureDevices,
            state->captureAudioDevices, kVideoPresets, kPcmQueueOptionsMs,
            state->displayMonitors};
        const llcv::settings_ui::SettingsControlPopulation population{
            state,
            [](void* context) {
                auto* dialog = static_cast<SettingsDialogState*>(context);
#ifdef LLCV_RECORDING
                if(dialog->recordingLive)return;
#endif
                PopulateAudioOutputCombo(dialog);
                dialog->selectedBufferMs = g_settings.wasapiBufferMs;
                dialog->selectedSharedPeriodFrames = g_settings.wasapiSharedPeriodFrames;
            },
            [](void* context) {
#ifdef LLCV_RECORDING
                if(static_cast<SettingsDialogState*>(context)->recordingLive)return;
#endif
                PopulateSettingsBufferCombo(static_cast<SettingsDialogState*>(context));
            },
            [](void* context) {
#ifdef LLCV_RECORDING
                if(static_cast<SettingsDialogState*>(context)->recordingLive)return;
#endif
                PopulatePixelFormatCombo(static_cast<SettingsDialogState*>(context));
            }};
        llcv::settings_ui::CreateSettingsDialogControls(
            state, hwnd, instance, initial, population);
#ifdef LLCV_RECORDING
        state->recordingOptions=llcv::recording::sessionOptions;
        llcv::settings_ui::CreateRecordingSettings(state,hwnd);
        state->recordingSnapshot=llcv::recording::recorder.GetSnapshot();
        if(state->recordingLive && g_recordingCapabilities){
            state->recordingCapabilities=*g_recordingCapabilities;state->recordingProbePending=false;
            llcv::settings_ui::RefreshRecordingSettings(state);
        }else RequestRecordingEncoders(state,hwnd);
        SetTimer(hwnd,2071,500,nullptr);
        if(state->recordingLive){
            const UINT dpi=GetDpiForWindow(hwnd);
            ApplySettingsFont(state,hwnd,dpi);LayoutSettingsControls(state,dpi);
            UpdateAdvancedControlVisibility(state);
            state->theme.Attach(hwnd,state);
            return 0;
        }
#endif
        UpdateVideoCapabilityStatus(state);

        const UINT initialDpi = GetDpiForWindow(hwnd);
        ApplySettingsFont(state, hwnd, initialDpi);
        LayoutSettingsControls(state, initialDpi);
        UpdateCaptureAudioSelectionUi(state);
        UpdateAdvancedControlVisibility(state);
        UpdateAsioControlVisibility(state);
        UpdateExclusiveProbeControl(state);
        UpdateExclusiveVerificationUi(state);
        state->theme.Attach(hwnd, state);
        RedrawWindow(hwnd, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                         RDW_UPDATENOW);

        RequestSettingsSharedProbe(state, hwnd);
        StartCaptureAudioProbe(state, hwnd);
        if (SettingsUsesExclusiveMode(state)) {
            StartExclusiveEndpointScan(state, hwnd);
        }
        return 0;
    }

    case WM_DPICHANGED: {
        llcv::settings_ui::SettingsVisualUpdate visualUpdate(hwnd);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT fitted = llcv::settings_ui::FitSettingsDialogToWorkArea(state, monitor.rcWork,
            HIWORD(wParam), static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
            static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
        const int width = fitted.right - fitted.left, height = fitted.bottom - fitted.top;
        SetWindowPos(hwnd, nullptr,
                     std::clamp(suggested->left, monitor.rcWork.left, monitor.rcWork.right-width),
                     std::clamp(suggested->top, monitor.rcWork.top, monitor.rcWork.bottom-height),
                     width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ApplySettingsFont(state, hwnd, HIWORD(wParam));
        LayoutSettingsControls(state, HIWORD(wParam));
        UpdateAdvancedControlVisibility(state);
        UpdateCaptureAudioSelectionUi(state);
        state->theme.RefreshControls();
        return 0;
    }

    case WM_EXITSIZEMOVE:
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        // Equal-DPI monitors can have different work areas, and the taskbar
        // can change that area without a DPI notification. Reuse the same
        // fitting path when the move ends, without rescanning any devices.
        if (state && (msg != WM_SETTINGCHANGE || wParam == SPI_SETWORKAREA)) {
            RECT suggested{}; GetWindowRect(hwnd, &suggested);
            const UINT dpi = GetDpiForWindow(hwnd);
            SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(dpi,dpi), reinterpret_cast<LPARAM>(&suggested));
            return 0;
        }
        break;

    case WM_GETDPISCALEDSIZE:
        if (lParam) {
            *reinterpret_cast<SIZE*>(lParam) =
                SettingsDialogOuterSize(hwnd, static_cast<UINT>(wParam), state);
            return TRUE;
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam)==WA_INACTIVE && state) llcv::settings_ui::CancelShortcutEdit(state);
        if (LOWORD(wParam) != WA_INACTIVE) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case WM_AUDIOCLIENT3_PROBE_COMPLETE: {
        // A late Shared result has no visible effect in Exclusive/ASIO.
        if (!SettingsUsesSharedMode(state)) return 0;
        llcv::settings_ui::SettingsVisualUpdate visualUpdate(hwnd);
        RequestSettingsSharedProbe(state, hwnd);
        return 0;
    }

    case WM_EXCLUSIVE_ENDPOINT_PROBE_COMPLETE: {
        if (!state) return 0;
        ConsumeExclusiveEndpointProbeResults(state);
        RefreshExclusiveScanUi(state, hwnd);
        return 0;
    }

    case WM_EXCLUSIVE_SCAN_COMPLETE:
        if (state) {
            CompleteExclusiveEndpointScan(state);
            RefreshExclusiveScanUi(state, hwnd);
        }
        return 0;

    case WM_CAPTURE_AUDIO_PROBE_COMPLETE:
        if (state) {
            StartCaptureAudioProbe(state, hwnd);
            UpdateAdvancedControlVisibility(state);
        }
        return 0;

    case WM_SETTINGS_VIDEO_MODES_READY:
        if (state) {
            PopulatePixelFormatCombo(state);
            UpdateAdvancedControlVisibility(state);
        }
        return 0;

    case WM_SETTINGS_INVENTORY_READY:
        if (state && state->inventoryPending) RequestSettingsInventory(state, hwnd, false);
        return 0;

    case WM_SETTINGS_UPDATE_CHECK_COMPLETE: {
        if (!state) return 0;
        auto result = state->updateCheckTask.TakeResult();
        if (!result) return 0;
        EnableWindow(state->updateNowButton, TRUE);
        if (!result || !result->success) {
            SetSettingsUpdateStatus(
                state, UI_TEXT(L"업데이트를 확인하지 못했습니다. 인터넷 연결을 확인한 뒤 다시 시도하세요."));
            return 0;
        }
        if (result->newer && result->installerUrl.empty()) {
            SetSettingsUpdateStatus(state, IsEnglishUi()
                ? L"A newer version is available, but its installer is not available yet. Try again later."
                : L"새 버전이 있지만 설치 파일이 아직 없습니다. 잠시 후 다시 확인하세요.");
            return 0;
        }
        if (!result->newer) {
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

#ifdef LLCV_RECORDING
    case WM_TIMER:
        if(state && wParam==2071 && state->activeTab==SettingsTab::Recording){
            state->recordingSnapshot=llcv::recording::recorder.GetSnapshot();
            state->recordingClosing=llcv::recording::ControlsClosing();
            llcv::settings_ui::ShowRecordingSettings(state);
            RequestRecordingEncoders(state,hwnd);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd,2071);
        if(state)state->recordingProbeStop.store(true);
        break;
    case WM_NCDESTROY:
        if(g_recordingSettingsWindow==hwnd){
            g_recordingSettingsWindow=nullptr;
            RefreshPresentationUiBlock();
        }
        break;
    case WM_RECORD_ENCODERS_READY:
        if(state&&state->recordingProbe){
            if(auto result=state->recordingProbe->Request({})){
                state->recordingCapabilities=std::move(result->capabilities);state->recordingProbePending=false;
                state->recordingProbeFailed=!result->completed;
                if(result->completed)g_recordingCapabilities=state->recordingCapabilities;
                llcv::settings_ui::RefreshRecordingSettings(state);
            }
        }
        return 0;
#endif
    case WM_COMMAND:
#ifdef LLCV_RECORDING
        if(state)state->recordingSnapshot=llcv::recording::recorder.GetSnapshot();
        if(state && HIWORD(wParam)==BN_CLICKED && LOWORD(wParam)==llcv::settings_ui::kRecordingPause){
            if(state->recordingLive&&!llcv::recording::ControlsClosing())
                llcv::recording::recorder.SetPaused(!state->recordingSnapshot.paused);
            state->recordingSnapshot=llcv::recording::recorder.GetSnapshot();
            llcv::settings_ui::ShowRecordingSettings(state);
            return 0;
        }
        if(state && HIWORD(wParam)==BN_CLICKED && LOWORD(wParam)==llcv::settings_ui::kRecordingToggleCommand){
            state->recordingClosing=llcv::recording::ControlsClosing();
            if(!llcv::settings_ui::CanToggleRecordingSettings(state))return 0;
            if(!state->recordingSnapshot.busy){
                if(!llcv::recording::SaveOptions(state->recordingOptions)){
                    MessageBoxW(hwnd,state->english?L"Could not save recording settings.":L"녹화 설정을 저장하지 못했습니다.",L"LLCV",MB_OK|MB_ICONERROR);return 0;
                }
                llcv::recording::sessionOptions=state->recordingOptions;
            }
            llcv::recording::ToggleRecording();
            state->recordingSnapshot=llcv::recording::recorder.GetSnapshot();
            llcv::settings_ui::ShowRecordingSettings(state);
            return 0;
        }
        if(state&&llcv::settings_ui::RecordingSettingsCommand(state,hwnd,LOWORD(wParam),HIWORD(wParam)))return 0;
#endif
        if (state && HIWORD(wParam)==BN_CLICKED && LOWORD(wParam)>=IDC_SETTINGS_SHORTCUT_FIRST &&
            LOWORD(wParam)<IDC_SETTINGS_SHORTCUT_FIRST+keys::Count) {
            llcv::settings_ui::BeginShortcutEdit(state,LOWORD(wParam)-IDC_SETTINGS_SHORTCUT_FIRST);
            return 0;
        }
        if (state && LOWORD(wParam)==IDC_SETTINGS_SHORTCUT_RESET && HIWORD(wParam)==BN_CLICKED) {
            llcv::settings_ui::CancelShortcutEdit(state);
            state->shortcutBindings=keys::Defaults;
            llcv::settings_ui::RefreshShortcutCaptions(state);
            UpdateAudioClient3Status(state);
            return 0;
        }
        if (state && LOWORD(wParam) == IDC_SETTINGS_SECTION && HIWORD(wParam) == CBN_SELCHANGE) {
            const LRESULT page = SendMessageW(state->sectionNavigation, CB_GETCURSEL, 0, 0);
            if (page != CB_ERR) {
                llcv::settings_ui::SettingsVisualUpdate visualUpdate(hwnd);
                state->compactPages[llcv::settings_ui::SettingsNavigationIndex(state->activeTab)] = static_cast<int>(page);
                UpdateAdvancedControlVisibility(state);
                UpdateCaptureAudioSelectionUi(state);
            }
            return 0;
        }
        if (state && LOWORD(wParam) == IDOK) {
            // This is a normal HWND rather than a dialog-template window.
            // IsDialogMessage can use IDOK even when a non-default push button
            // owns focus. Preserve Enter on Cancel/help/action buttons.
            const HWND focused = GetFocus();
            wchar_t className[32]{};
            GetClassNameW(focused, className, ARRAYSIZE(className));
            const LONG_PTR type = GetWindowLongPtrW(focused, GWL_STYLE) & BS_TYPEMASK;
            if (GetParent(focused) == hwnd && IsWindowEnabled(focused) &&
                _wcsicmp(className, L"BUTTON") == 0 &&
                (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) &&
                GetDlgCtrlID(focused) != IDOK) {
                SendMessageW(hwnd, WM_COMMAND,
                    MAKEWPARAM(GetDlgCtrlID(focused), BN_CLICKED),
                    reinterpret_cast<LPARAM>(focused));
                return 0;
            }
        }
        if (state && LOWORD(wParam) == IDC_SETTINGS_TAB && HIWORD(wParam) == LBN_SELCHANGE) {
            const int index = static_cast<int>(SendMessageW(state->tabControl, LB_GETCURSEL, 0, 0));
            if (index != LB_ERR) {
                const auto nextTab = llcv::settings_ui::SettingsTabFromNavigationIndex(index);
#ifdef LLCV_RECORDING
                if(state->recordingLive && nextTab!=SettingsTab::Recording){
                    const HWND viewer=GetWindow(hwnd,GW_OWNER);
                    FinishSettingsDialog(hwnd,state,true);
                    if(!IsWindow(hwnd)){g_requestedSettingsTab=static_cast<int>(nextTab);PostMessageW(viewer,WM_APP+92,1,0);}
                    return 0;
                }
#endif
                if (nextTab == state->activeTab) {
                    // No page repaint, but keep capability-dependent gates
                    // correct even if a completion changed the current state.
                    UpdateAdvancedControlVisibility(state);
                    return 0;
                }
                llcv::settings_ui::SettingsVisualUpdate visualUpdate(hwnd);
                state->activeTab = nextTab;
                UpdateAdvancedControlVisibility(state);
                UpdateCaptureAudioSelectionUi(state);
#ifdef LLCV_RECORDING
                RequestRecordingEncoders(state,hwnd);
#endif
            }
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_VSR_GUIDE && HIWORD(wParam) == BN_CLICKED && state) {
            // This is guidance, not a hardware capability/activation check.
            // Request acceptance alone cannot establish NVIDIA's active state.
            const std::wstring guide = IsEnglishUi()
                ? L"CMAA2 → NIS reduces jagged edges before upscaling. Default sharpness: 0.15.\n"
                  L"SDR only, up to 2× per axis. HDR, native size and downscaling use the normal path.\n"
                  L"F6 toggles the selected method without changing it or its sharpness.\n"
                  L"Display only: recordings/screenshots keep the capture source; OSD is drawn afterward.\n\n"
                : L"CMAA2로 계단 현상을 보정한 뒤 NIS로 확대합니다. 기본 선명도는 0.15입니다.\n"
                  L"SDR에서만 가로·세로 최대 2배 확대를 지원합니다. HDR·원본 크기·축소는 기존 경로를 사용합니다.\n"
                  L"F6은 선택한 방식과 선명도를 유지하면서 켜고 끕니다.\n"
                  L"화면 표시에만 적용되며 녹화·스크린샷 원본은 유지합니다. OSD는 처리 후 표시합니다.\n\n";
            MessageBoxW(hwnd, (guide+keys::Expand(llcv::ui_text::VsrSetupGuide(IsEnglishUi()),state->shortcutBindings)).c_str(),
                IsEnglishUi() ? L"Upscaling — Setup guide" : L"업스케일링 — 설정 안내",
                MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_SURROUND51 && HIWORD(wParam) == BN_CLICKED) {
            UpdateAudioClient3Status(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_AUDIO &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            llcv::settings_ui::SettingsVisualUpdate visualUpdate(hwnd);
            RememberCurrentBufferChoice(state);
            PopulateAudioOutputCombo(state);
            PopulateSettingsBufferCombo(state);
            RequestSettingsSharedProbe(state, hwnd);
            if (SettingsUsesExclusiveMode(state)) {
                StartExclusiveEndpointScan(state, hwnd);
            }
            UpdateAsioControlVisibility(state);
            UpdateAdvancedControlVisibility(state);
            UpdateAudioClient3Status(state);
            UpdateExclusiveProbeControl(state);
            UpdateExclusiveVerificationUi(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_AUDIO_OUTPUT &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            llcv::settings_ui::SettingsVisualUpdate visualUpdate(hwnd);
            RequestSettingsSharedProbe(state, hwnd);
            EnsureExclusiveBufferForSelection(state);
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
        if (LOWORD(wParam) == IDC_SETTINGS_SCREENSHOT_FOLDER &&
            HIWORD(wParam) == BN_CLICKED) {
            const auto directory = llcv::screenshot::DefaultDirectory();
            std::error_code error;
            if (!directory.empty()) std::filesystem::create_directories(directory, error);
            if (directory.empty() || error || reinterpret_cast<INT_PTR>(ShellExecuteW(
                    hwnd, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                MessageBoxW(hwnd, IsEnglishUi() ? L"Could not open the screenshot folder."
                    : L"스크린샷 폴더를 열지 못했습니다.", L"Screenshot", MB_OK | MB_ICONWARNING);
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
        if (LOWORD(wParam) == IDC_SETTINGS_VIDEO_REFRESH && HIWORD(wParam) == BN_CLICKED && state) {
            RequestSettingsInventory(state, hwnd, true);
            return 0;
        }
        if (((LOWORD(wParam) == IDC_SETTINGS_VIDEO || LOWORD(wParam) == IDC_SETTINGS_DISPLAY_RESOLUTION) &&
             HIWORD(wParam) == CBN_SELCHANGE) ||
            (LOWORD(wParam) == IDC_SETTINGS_UPSCALER && HIWORD(wParam) == CBN_SELCHANGE)) {
            if (LOWORD(wParam) == IDC_SETTINGS_VIDEO) PopulatePixelFormatCombo(state);
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
            UpdateAdvancedControlVisibility(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PIXEL &&
            HIWORD(wParam) == BN_CLICKED) {
            UpdateScalingControlVisibility(state);
            UpdateWindowBehaviorVisibility(state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_RELATIVE_SIZE &&
            HIWORD(wParam) == BN_CLICKED) {
            UpdateWindowBehaviorVisibility(state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PIXEL_FORMAT &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            PopulateFrameRateCombo(state);
            UpdateVideoCapabilityStatus(state);
            UpdateAdvancedControlVisibility(state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_SDR_RANGE && HIWORD(wParam) == CBN_SELCHANGE) {
            llcv::settings_ui::ApplySdrRangeSelection(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PRESENTATION_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::Presentation,state).c_str(),
                UI_TEXT(L"화면 표시 방식"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_UNLISTED_FPS && HIWORD(wParam) == BN_CLICKED) {
            PopulateFrameRateCombo(state);
            UpdateVideoCapabilityStatus(state);
            UpdateAdvancedControlVisibility(state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_FRAME_RATE && HIWORD(wParam) == CBN_SELCHANGE) {
            UpdateVideoCapabilityStatus(state);
            UpdateAdvancedControlVisibility(state);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_CAPTURE_40 && HIWORD(wParam) == BN_CLICKED) {
            UpdateVideoCapabilityStatus(state);
            UpdateAdvancedControlVisibility(state);
            return 0;
        }
        if ((LOWORD(wParam) == IDC_SETTINGS_CAPTURE_40_HELP ||
             LOWORD(wParam) == IDC_SETTINGS_LS_GUIDE) && HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(hwnd, SettingsHelpText(SettingsHelpTopic::Capture40,state).c_str(),
                IsEnglishUi() ? L"40 fps capture guide" : L"40fps 캡처 안내", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_VOLUME_BOOST_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::VolumeBoost,state).c_str(),
                UI_TEXT(L"100% 이상 볼륨 증폭"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_FORCE_HDR10_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::ForceHdr10,state).c_str(),
                UI_TEXT(L"HDR10 강제 출력"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_MJPEG_COLOR_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::MjpegColor,state).c_str(),
                IsEnglishUi() ? L"MJPEG color matrix" : L"MJPEG 색 공간", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_SDR_RANGE_HELP && HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(hwnd, SettingsHelpText(SettingsHelpTopic::SdrRange,state).c_str(),
                IsEnglishUi() ? L"SDR color range" : L"SDR 색 범위", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_HDR_CHROMA_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(hwnd, SettingsHelpText(SettingsHelpTopic::HdrChroma,state).c_str(),
                UI_TEXT(L"HDR 색차 배치"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_PCM_QUEUE_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::PcmQueue,state).c_str(),
                UI_TEXT(L"PCM 버퍼 목표"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_DRIFT_HELP &&
            HIWORD(wParam) == BN_CLICKED) {
            MessageBoxW(
                hwnd,
                SettingsHelpText(SettingsHelpTopic::Drift,state).c_str(),
                UI_TEXT(L"클록 드리프트 보정"), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_START || LOWORD(wParam) == IDOK) {
            FinishSettingsDialog(hwnd, state, true);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SETTINGS_CANCEL || LOWORD(wParam) == IDCANCEL) {
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

#ifdef LLCV_RECORDING
static bool ProcessSettingsRecordingShortcut(HWND dialog, const MSG& message, unsigned modifiers) {
    if (!keys::KeyDown(message.message) ||
        !keys::AllowedInSettings(static_cast<UINT>(message.wParam)) ||
        !(message.hwnd == dialog || IsChild(dialog, message.hwnd)) ||
        !keys::Match(g_settings.shortcuts, keys::Action::Recording,
                     static_cast<UINT>(message.wParam), modifiers)) return false;
    if (!(message.lParam & (LPARAM{1} << 30)))
        SendMessageW(dialog, WM_COMMAND,
            MAKEWPARAM(llcv::settings_ui::kRecordingToggleCommand, BN_CLICKED), 0);
    return true;
}
#endif

static bool ShowSettingsDialog(HINSTANCE hInst,
                               bool preferSavedViewerMonitor,HWND liveViewer=nullptr,
                               bool reviewStartupVersion=false) {
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
    state.activeTab = g_settings.audioOnly ? SettingsTab::Audio : SettingsTab::VideoWindow;
#ifdef LLCV_RECORDING
    state.recordingLive=liveViewer!=nullptr;
    if(state.recordingLive)state.activeTab=SettingsTab::Recording;
    else if(g_requestedSettingsTab>=0 && g_requestedSettingsTab<llcv::settings_ui::kSettingsNavigationCount)
        state.activeTab=static_cast<SettingsTab>(g_requestedSettingsTab);
    if(!state.recordingLive){
#endif
    state.displayMonitors = llcv::display::EnumerateMonitors(IsEnglishUi());
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
            state.exclusiveEndpointResults[i].state = cached->inconclusive
                ? ExclusiveEndpointState::RetryRequired : cached->supported
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
#ifdef LLCV_RECORDING
    }
#endif
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR savedViewerMonitor = SavedViewerMonitor();
    HMONITOR settingsMonitor = liveViewer?MonitorFromWindow(liveViewer,MONITOR_DEFAULTTONEAREST):nullptr;
    if (!settingsMonitor && preferSavedViewerMonitor && savedViewerMonitor) {
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
    // Parent repaint must not temporarily erase native child fields.
    const DWORD settingsStyle =
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_CLIPCHILDREN;
    const DWORD settingsExStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    MONITORINFO settingsMonitorInfo{sizeof(settingsMonitorInfo)};
    GetMonitorInfoW(settingsMonitor, &settingsMonitorInfo);
    state.initialVideoPreset = g_settings.videoPreset;
    const RECT work = settingsMonitorInfo.rcWork;
    const RECT settingsRect = llcv::settings_ui::FitSettingsDialogToWorkArea(
        &state, work, settingsDpi, settingsStyle, settingsExStyle);
    HWND hwnd = CreateWindowExW(
        settingsExStyle,
        kSettingsClass, UI_TEXT(L"LLCV 설정"),
        settingsStyle,
        settingsRect.left, settingsRect.top, settingsRect.right-settingsRect.left,
        settingsRect.bottom-settingsRect.top,
        liveViewer, nullptr, hInst, &state);
    if (!hwnd) return false;
#ifdef LLCV_RECORDING
    if(state.recordingLive)g_recordingSettingsWindow=hwnd;
#endif

    // F2 reopens settings in a new process after the capture threads have
    // stopped. Briefly promote the dialog while activating it, then return it
    // to the normal z-order so it cannot remain hidden behind the previously
    // foreground application and is never permanently topmost.
    ShowWindow(hwnd, SW_SHOWNORMAL);
    if(!liveViewer)SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    if(!liveViewer)SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    UpdateWindow(hwnd);
    // Count only a successfully displayed startup dialog, including Cancel.
    // Store the marker alone so this never commits unaccepted settings.
    if (reviewStartupVersion && !g_suppressSettingsSave) {
        EnsureUserDataDirectory();
        unsigned long error=0;
        if (!llcv::settings::MarkStartupSettingsShown(SettingsPath(),kAppVersionLabel,&error))
            fwprintf(stderr,L"[settings] Startup review marker not saved (Win32=%lu); retry next launch.\n",error);
    }
    MSG msg{};
    while (IsWindow(hwnd)) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            if(liveViewer){
                if(IsWindow(hwnd))DestroyWindow(hwnd);
                // A nested settings loop must not consume the viewer's quit.
                if(result==0)PostQuitMessage(static_cast<int>(msg.wParam));
            }
            break;
        }
#ifdef LLCV_RECORDING
        if(state.recordingLive && keys::KeyDown(msg.message)){
            const bool inDialog=msg.hwnd==hwnd || IsChild(hwnd,msg.hwnd);
            if(ProcessSettingsRecordingShortcut(hwnd,msg,keys::Modifiers()))continue;
            if(!inDialog && HandleViewerShortcut(liveViewer,msg.wParam,msg.lParam,keys::Modifiers()))continue;
            if(!inDialog && keys::Match(g_settings.shortcuts,keys::Action::Screenshot,msg)){
                if(llcv::viewer_help::IsScreenshotRequest(msg,g_settings.audioOnly,g_settings.shortcuts))
                    ShowTransientHud(g_screenshots.Request(g_settings.screenshotClipboard)?
                        TransientHudContent::ScreenshotPending:TransientHudContent::ScreenshotBusy);
                continue;
            }
        }
#endif
        if (ProcessSettingsShortcutEdit(&state,msg)) continue;
        g_viewerHelp.SetBindings(state.shortcutBindings);
        if (g_viewerHelp.ProcessMessage(msg, hwnd, IsEnglishUi(), kAppVersionLabel,
                SendMessageW(state.themeCombo, CB_GETCURSEL, 0, 0) == 1,
                llcv::viewer_help::MessageContext::Settings)) continue;
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
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    state.sharedProbeCache.reset(); // Driver waits only after the HWND closes.
#ifdef LLCV_RECORDING
    state.recordingProbeStop.store(true);
    state.recordingProbe.reset();
#endif
    state.updateCheckTask.CancelAndWait();
    state.exclusiveProbeStop.store(true, std::memory_order_release);
    // The HWND may have discarded the worker's notifications. Join, drain and
    // save its completed results through the same path as normal completion.
    CompleteExclusiveEndpointScan(&state);
    state.captureAudioProbeCache.reset();
    state.inventoryCache.reset();
    state.videoModeCache.reset(); // join once, after the dialog closes, never on an option click
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

enum class HorizontalSnapEdge { None, Left, Right };
enum class VerticalSnapEdge { None, Top, Bottom };

struct WindowSnapState {
    HorizontalSnapEdge horizontal = HorizontalSnapEdge::None;
    VerticalSnapEdge vertical = VerticalSnapEdge::None;
    int horizontalCursorAnchor = 0;
    int verticalCursorAnchor = 0;
    int horizontalEdgeOffset = 0;
    int verticalEdgeOffset = 0;
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
    // Explicit display selection wins at startup; F6 never changes this plan.
    if (g_splitResolutionPlan) return SIZE{video.width, video.height};
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

static void NormalizeWindowSize(HWND hwnd, bool clampToWorkArea,
                                HMONITOR startupMonitor = nullptr) {
    // Only strict pixel-perfect without monitor-relative behavior is fixed.
    // When both options are enabled, relative sizing is allowed to change the
    // size programmatically as the window crosses monitors.
    if (!hwnd || g_settings.audioOnly || !g_settings.pixelPerfect ||
        g_settings.relativeWindowSize || g_fullscreen) return;
    RECT current{};
    if (!GetWindowRect(hwnd, &current)) return;
    // An oversized initial window may overlap another display more than the
    // selected one. Honor the startup target for this one normalization only.
    MONITORINFO startupInfo{sizeof(startupInfo)};
    const HMONITOR currentMonitor = startupMonitor && GetMonitorInfoW(startupMonitor, &startupInfo)
        ? startupMonitor : MonitorFromRect(&current, MONITOR_DEFAULTTONEAREST);
    const UINT dpi = EffectiveMonitorDpi(currentMonitor, hwnd);
    const SIZE desired = DesiredWindowOuterSize(
        hwnd, currentMonitor, dpi);
    int x = current.left;
    int y = current.top;
    if (clampToWorkArea) {
        MONITORINFO info{sizeof(info)};
        const HMONITOR monitor = currentMonitor;
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
static bool g_interactiveWindowMove = false;
static bool g_audioOnlyResizePending = false;
static bool g_audioOnlyDragPending = false;
static bool g_muteButtonPressed = false;
static int AudioOsdHitTarget(HWND root, POINT screenPoint);
static POINT g_audioOnlyDragStart{};
static llcv::video::OutputTransitionState g_outputTransition;

static void BeginOutputTransition() {
    g_outputTransition.Begin();
}

static void EndOutputTransition(bool requestOutputUpdate) {
    if (g_outputTransition.End(requestOutputUpdate)) {
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
    if (!g_settings.preferredDisplayMonitor.empty()) {
        const auto monitors = llcv::display::EnumerateMonitors(IsEnglishUi());
        if (const HMONITOR selected = llcv::display::FindMonitor(monitors, g_settings.preferredDisplayMonitor))
            return selected;
        return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
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
    if (!g_settings.hasWindowPosition && g_settings.preferredDisplayMonitor.empty()) return false;
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
    if (!g_settings.preferredDisplayMonitor.empty() &&
        (!g_settings.hasWindowPosition ||
         _wcsicmp(g_settings.monitorDevice.c_str(), monitorInfo.szDevice) != 0)) {
        origin.x = work.left + (std::max)(0L, (work.right - work.left - outerSize.cx) / 2);
        origin.y = work.top + (std::max)(0L, (work.bottom - work.top - outerSize.cy) / 2);
        return true;
    }
    const int maximumX = (std::max)(work.left, work.right - outerSize.cx);
    const int maximumY = (std::max)(work.top, work.bottom - outerSize.cy);
    origin.x = std::clamp(g_settings.windowX,
                          static_cast<int>(work.left), maximumX);
    origin.y = std::clamp(g_settings.windowY,
                          static_cast<int>(work.top), maximumY);
    return true;
}

static RECT NormalPlacementScreenRect(RECT normal, DWORD exStyle,
                                      const MONITORINFO& monitor) {
    // WINDOWPLACEMENT is in workspace coordinates for ordinary top-level
    // windows, but already in screen coordinates for WS_EX_TOOLWINDOW.
    if (!(exStyle & WS_EX_TOOLWINDOW))
        OffsetRect(&normal, monitor.rcWork.left - monitor.rcMonitor.left,
                           monitor.rcWork.top - monitor.rcMonitor.top);
    return normal;
}

static bool ReadPersistedWindowRect(HWND hwnd, RECT& rect) {
    if (g_fullscreen && g_haveLastWindowedRect) {
        rect = g_lastWindowedRect;
        return true;
    }
    if (!IsIconic(hwnd)) return GetWindowRect(hwnd, &rect) != FALSE;
    WINDOWPLACEMENT placement{sizeof(placement)};
    MONITORINFO monitor{sizeof(monitor)};
    // MonitorFromWindow uses the pre-minimize rectangle for an iconic HWND.
    if (!GetWindowPlacement(hwnd, &placement) ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
        return false;
    rect = NormalPlacementScreenRect(placement.rcNormalPosition,
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), monitor);
    return true;
}

static void PersistWindowPosition(HWND hwnd) {
    // Smoke tests and diagnostic probes must not alter the user's placement.
    if (g_suppressSettingsSave) return;
    RECT rect{};
    if (!ReadPersistedWindowRect(hwnd, rect)) return;
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
    const int width = movingRect.right - movingRect.left;
    const int height = movingRect.bottom - movingRect.top;
    const RECT work = monitorInfo.rcWork;
    // A latch belongs to an actual work-area edge, not the previous monitor.
    if ((g_windowSnapState.horizontal == HorizontalSnapEdge::Left &&
         g_windowSnapState.horizontalSnapCoordinate != work.left) ||
        (g_windowSnapState.horizontal == HorizontalSnapEdge::Right &&
         g_windowSnapState.horizontalSnapCoordinate != work.right)) {
        g_windowSnapState.horizontal = HorizontalSnapEdge::None;
        g_windowSnapState.suppressHorizontal = false;
    }
    if ((g_windowSnapState.vertical == VerticalSnapEdge::Top &&
         g_windowSnapState.verticalSnapCoordinate != work.top) ||
        (g_windowSnapState.vertical == VerticalSnapEdge::Bottom &&
         g_windowSnapState.verticalSnapCoordinate != work.bottom)) {
        g_windowSnapState.vertical = VerticalSnapEdge::None;
        g_windowSnapState.suppressVertical = false;
    }
    // Preserve the unsnapped edge offset when entering the attraction zone.
    // Using only travel since latching adds a second dead zone (up to twice
    // the snap distance). Both entry and exit now use the same edge distance.
    bool horizontalReleased = false;
    if (g_windowSnapState.horizontal == HorizontalSnapEdge::Left) {
        const int cursorDelta =
            cursor.x - g_windowSnapState.horizontalCursorAnchor;
        const int edgeOffset = g_windowSnapState.horizontalEdgeOffset + cursorDelta;
        const bool release = std::abs(edgeOffset) > snapDistance;
        if (release) {
            movingRect.left = g_windowSnapState.horizontalSnapCoordinate + edgeOffset;
            movingRect.right = movingRect.left + width;
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
        const int edgeOffset = g_windowSnapState.horizontalEdgeOffset + cursorDelta;
        const bool release = std::abs(edgeOffset) > snapDistance;
        if (release) {
            movingRect.right = g_windowSnapState.horizontalSnapCoordinate + edgeOffset;
            movingRect.left = movingRect.right - width;
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
                g_windowSnapState.horizontalEdgeOffset = movingRect.left - work.left;
                movingRect.left = work.left;
                movingRect.right = movingRect.left + width;
                g_windowSnapState.horizontal = HorizontalSnapEdge::Left;
                g_windowSnapState.horizontalCursorAnchor = cursor.x;
                g_windowSnapState.horizontalSnapCoordinate = work.left;
            } else if (std::abs(movingRect.right - work.right) <= snapDistance) {
                g_windowSnapState.horizontalEdgeOffset = movingRect.right - work.right;
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
        const int edgeOffset = g_windowSnapState.verticalEdgeOffset + cursorDelta;
        const bool release = std::abs(edgeOffset) > snapDistance;
        if (release) {
            movingRect.top = g_windowSnapState.verticalSnapCoordinate + edgeOffset;
            movingRect.bottom = movingRect.top + height;
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
        const int edgeOffset = g_windowSnapState.verticalEdgeOffset + cursorDelta;
        const bool release = std::abs(edgeOffset) > snapDistance;
        if (release) {
            movingRect.bottom = g_windowSnapState.verticalSnapCoordinate + edgeOffset;
            movingRect.top = movingRect.bottom - height;
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
                g_windowSnapState.verticalEdgeOffset = movingRect.top - work.top;
                movingRect.top = work.top;
                movingRect.bottom = movingRect.top + height;
                g_windowSnapState.vertical = VerticalSnapEdge::Top;
                g_windowSnapState.verticalCursorAnchor = cursor.y;
                g_windowSnapState.verticalSnapCoordinate = work.top;
            } else if (std::abs(movingRect.bottom - work.bottom) <= snapDistance) {
                g_windowSnapState.verticalEdgeOffset = movingRect.bottom - work.bottom;
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

static void ConstrainWindowRectToAudioAspect(HWND hwnd, RECT& sizingRect,
                                             UINT sizingEdge, UINT dpi) {
    if (!hwnd) return;
    const DWORD style =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle =
        static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    RECT frame{0, 0, 0, 0};
    if (!AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, dpi)) {
        AdjustWindowRectEx(&frame, style, FALSE, exStyle);
    }
    const auto minimum = llcv::audio_only_view::MinimumClientSize(dpi);
    llcv::window_geometry::ConstrainToAspect(
        sizingRect, sizingEdge,
        {static_cast<int>(frame.right - frame.left),
         static_cast<int>(frame.bottom - frame.top),
         llcv::audio_only_view::kBaseClientWidth,
         llcv::audio_only_view::kBaseClientHeight,
         minimum.width, minimum.height});
}

static LRESULT BorderlessHitTest(HWND hwnd, LPARAM lParam) {
    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return HTCLIENT;
    const POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    const UINT dpi = GetDpiForWindow(hwnd);
    const int grip = (std::max)(6, MulDiv(8, dpi, 96));
    return llcv::window_geometry::BorderlessHitTest(
        windowRect, cursor, grip,
        g_settings.audioOnly || !g_settings.pixelPerfect);
}

static llcv::window_corners::State g_viewerCorners;
static void ApplyViewerCorners(HWND hwnd) {
    (void)g_viewerCorners.Apply(hwnd, g_settings.roundedCorners,
        g_fullscreen.load(std::memory_order_acquire), IsZoomed(hwnd) != FALSE,
        g_settings.borderlessWindow || g_fullscreen.load(std::memory_order_acquire));
}

static DWORD ViewerWindowStyle(const AppSettings& settings) {
    if (settings.borderlessWindow) return WS_POPUP | WS_VISIBLE;
    const DWORD fixed =
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    if (settings.audioOnly) return fixed | WS_THICKFRAME;
    return settings.pixelPerfect ? fixed : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
}

constexpr UINT_PTR kFullscreenCursorTimerId = 2;
constexpr UINT kFullscreenCursorTimerPeriodMs = 250;
constexpr uint64_t kFullscreenCursorHideDelayMs = 2000;
static bool g_fullscreenCursorHidden = false;
static uint64_t g_lastFullscreenCursorActivityMs = 0;

static bool FullscreenCursorAutoHideActive() {
    return g_fullscreen.load(std::memory_order_acquire) &&
        !g_viewerHelp.Handle() &&
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
        if (parent && AudioOsdHitTarget(parent, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}) ==
                static_cast<int>(llcv::audio_osd::HitTarget::Mute)) return HTTRANSPARENT;
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
    if (g_settings.audioOnly) return;
    BeginOutputTransition();
    g_fullscreenSourceSize.store(false, std::memory_order_release);
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
    ApplyViewerCorners(hwnd);
    NotifyPresentationChange();
    EndOutputTransition(updateOutput);
}

static bool SelectedResolutionMatchesMonitor(HMONITOR monitor, bool capture = false) {
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return false;

    const auto& video = capture ? CurrentCapturePreset() : CurrentVideoPreset();
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
    const auto& video = CurrentCapturePreset();
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
    if (SelectedResolutionMatchesMonitor(monitor, true)) {
        BeginOutputTransition();
        if (g_settings.relativeWindowSize) {
            g_settings.relativeWindowScalePpm =
                RelativeScaleForMonitor(monitor, true);
        }
        if (!fullscreen) ToggleFullscreen(hwnd, false, false);
        g_fullscreenSourceSize.store(true, std::memory_order_release);
        EndOutputTransition(true);
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
                RelativeScaleForMonitor(monitor, true);
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
    if (!hwnd || !g_settings.checkForUpdates || g_updateCheckTask.IsRunning()) return;
    g_updateCheckTask.Start(kAppVersionLabel, [hwnd]() {
        PostMessageW(hwnd, WM_UPDATE_CHECK_COMPLETE, 0, 0);
    }, std::chrono::seconds(2));
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
    const auto& preset = CurrentCapturePreset();
    const std::wstring& captureName = g_activeCaptureDeviceName;
    const std::wstring outputName = ActiveAudioOutputName();
    const VideoPixelFormat activeFormat = static_cast<VideoPixelFormat>(
        g_activePixelFormat.load(std::memory_order_acquire));
    const bool compressedVideo = IsCompressedVideoFormat(activeFormat);
    const bool p010Video = activeFormat == VideoPixelFormat::P010;
    const bool rgbVideo = activeFormat == VideoPixelFormat::Rgb24 || activeFormat == VideoPixelFormat::Rgb32;
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
    const wchar_t* chromaText = rgbVideo ? (IsEnglishUi() ? L"RGB (no YUV chroma)" : L"RGB (YUV 색차 없음)") : compressedVideo
                                    ? L"decode→4:2:0"
                                    : (activeFormat == VideoPixelFormat::Yuy2 || activeFormat == VideoPixelFormat::Uyvy)
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
    const int hdrDisplay = g_hdrDisplayState.load(std::memory_order_acquire);
    wchar_t rgbQualityText[128]{};
    if (rgbVideo) swprintf_s(rgbQualityText, IsEnglishUi() ? L"BT.709/sRGB SDR · %s · D3D11 VP" : L"BT.709/sRGB SDR · %s · D3D11 VP",
        llcv::video_color::RangeName(activeColorRange));
    const wchar_t* qualityText = rgbVideo ? rgbQualityText : hdrVideo
        ? (hdrDisplay == 1 ? L"BT.2020 · PQ · HDR10 · Display HDR"
           : hdrDisplay == 0 ? (IsEnglishUi() ? L"BT.2020 · PQ · Display SDR (enable Windows HDR)"
                                             : L"BT.2020 · PQ · 화면 SDR (Windows HDR 켜기)")
                             : (IsEnglishUi() ? L"BT.2020 · PQ · Display HDR unconfirmed"
                                              : L"BT.2020 · PQ · 화면 HDR 확인 불가"))
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
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
    if (hdrVideo && g_useScrgbPrototype) videoPath = L"DirectShow P010 → Shader → scRGB (test)";
#endif
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
        g_videoExclusiveActive.load(std::memory_order_acquire)
            ? (IsEnglishUi() ? L"Exclusive fullscreen" : L"독점 전체화면")
        : llcv::presentation::UsesVSync(g_settings.presentationMode)
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
        if (overrunEvents > 0 && lastErrorAgeMs <= 10 * 60 * 1000) {
            queueDiagnosis = IsEnglishUi() ? L"PCM queue overflow · check output timing"
                                          : L"PCM 버퍼 넘침 · 출력 지연 확인";
        } else if (underrunEvents == 0) {
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

    if (trackingActive && ActiveAudioMode() == AudioMode::WasapiShared) {
        if (g_sharedRebuffering.load(std::memory_order_acquire)) {
            queueDiagnosis = IsEnglishUi() ? L"Refilling PCM reserve" : L"PCM 버퍼 다시 채우는 중";
        } else if (const uint64_t late = g_sharedLastDeadlineMs.load();
                   late && nowMs >= late && nowMs - late < 60000) {
            queueDiagnosis = IsEnglishUi() ? L"Output delay suspected · check log"
                                          : L"출력 지연 의심 · 로그 확인";
        }
    }

    const int logicalVolume =
        g_volumePercent.load(std::memory_order_acquire);
    const bool backgroundMuted =
        g_backgroundAudioMuted.load(std::memory_order_acquire);
    const wchar_t* volumeProcessing = g_playbackMuted.load(std::memory_order_relaxed)
        ? UI_TEXT(L"음소거") : backgroundMuted
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

    const wchar_t* scaleText = g_splitResolutionPlan
        ? (IsEnglishUi() ? L"Display size" : L"표시 크기") :
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
    // Keep the fixed diagnostics panel's line count: the warning belongs next
    // to actual input FPS, not on a new row below the audio diagnostics.
    wchar_t captureRateWarning[64]{};
    if (!g_settings.audioOnly && RequestsExactCaptureRate() &&
        g_captureFrameRateOverdelivery.load(std::memory_order_relaxed)) {
        swprintf_s(captureRateWarning, IsEnglishUi()
            ? L" · Above requested %d" : L" · 요청 %d 초과", RequestedVideoFrameRate());
    }
    const wchar_t* osdFormat = IsEnglishUi()
        ? L"Capture diagnostics                              [Tab close]\n"
          L"Path          %s · %s\n"
          L"Input         %d x %d @ %d fps · %s %s %s\n"
          L"Video quality %s\n"
          L"Display       %d x %d · %s · %s · %s\n"
          L"Actual FPS    Input %.1f · Present %.1f%s\n"
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
          L"표시          %d x %d · %s · %s · %s\n"
          L"실제 FPS      입력 %.1f · Present %.1f%s\n"
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
        text, keys::Expand(osdFormat,g_settings.shortcuts).c_str(),
        captureName.c_str(), videoPath, preset.width, preset.height, configuredFps,
        PixelFormatName(activeFormat), bitDepthText, chromaText, qualityText,
        outputWidth,
        outputHeight,
        scaleText,
        llcv::presentation::PathName(g_settings.presentationMode),
        presentationText,
        g_osdInputFps.load(std::memory_order_acquire),
        g_osdPresentFps.load(std::memory_order_acquire), captureRateWarning, latencyText,
        static_cast<unsigned long long>(
            g_videoCapturedFrames.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_videoPresentedFrames.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_videoReplacedFrames.load(std::memory_order_relaxed)),
         ActiveAudioMode() == AudioMode::WasapiExclusive
             ? L"WASAPI Exclusive"
             : ActiveAudioMode() == AudioMode::Asio ? L"ASIO"
                                                        : Surround51Active() ? L"WASAPI Shared · PCM 5.1" : L"WASAPI Shared",
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
    static llcv::diagnostics::CaptureFrameRateMonitor rateMonitor;
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
        rateMonitor.Reset();
        g_captureFrameRateOverdelivery.store(false, std::memory_order_relaxed);
        trackingStarted = false;
        previousCaptured = captured;
        previousPresented = presented;
        previousTime = now;
        g_osdInputFps.store(0.0, std::memory_order_release);
        g_osdPresentFps.store(0.0, std::memory_order_release);
        return;
    }
    const auto observed = rateMonitor.Observe(GetTickCount64(), captured,
        !g_settings.audioOnly && RequestsExactCaptureRate() ? RequestedVideoFrameRate() : 0);
    g_captureFrameRateOverdelivery.store(observed.overdelivery, std::memory_order_relaxed);
    if (observed.event != llcv::diagnostics::CaptureFrameRateMonitor::Event::None) {
        fwprintf(stderr, L"[video-rate] %s requested=%d measured-input=%.2f fps; input callbacks, not external frame generation.\n",
            observed.overdelivery ? L"sustained-overdelivery" : L"delivery-recovered",
            RequestedVideoFrameRate(), observed.measuredFps);
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
    if (g_settings.audioOnly) return;
    const bool visible = !g_audioOsdVisible.load(std::memory_order_acquire);
    g_audioOsdVisible.store(visible, std::memory_order_release);
    g_audioOsdHoverTarget.store(0, std::memory_order_release);
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
}

// Returns: 0 outside the OSD, 1 left card, 2 right card, 3 master row,
// 4 another part of the OSD. Cards deliberately have large hit areas; the
// small numbers themselves are not the interaction target.
static int AudioOsdHitTarget(HWND root, POINT screenPoint) {
    if (!root || (!g_settings.audioOnly &&
                  !g_audioOsdVisible.load(std::memory_order_acquire))) return 0;
    POINT client = screenPoint;
    if (!ScreenToClient(root, &client)) return 0;
    RECT bounds{};
    if (!GetClientRect(root, &bounds)) return 0;
    return static_cast<int>(g_settings.audioOnly
        ? llcv::audio_only_view::HitTest(
              bounds.right, bounds.bottom, client.x, client.y)
        : llcv::audio_osd::HitTest(
              bounds.right, bounds.bottom, client.x, client.y));
}

static void SetPlaybackMuted(HWND hwnd, bool muted, bool showHud = true) {
    if (g_playbackMuted.exchange(muted, std::memory_order_relaxed) == muted) return;
    if (showHud && !g_settings.audioOnly && !g_audioOsdVisible.load(std::memory_order_acquire)) {
        g_transientHudContent.store(TransientHudContent::Volume, std::memory_order_release);
        g_volumeHudUntilMs.store(GetTickCount64()+1200, std::memory_order_release);
    }
    g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
    if (hwnd && g_settings.audioOnly) InvalidateRect(hwnd, nullptr, FALSE);
}

static bool BeginMuteButtonPress(HWND hwnd, LPARAM position) {
    POINT point{GET_X_LPARAM(position),GET_Y_LPARAM(position)};
    if (!ClientToScreen(hwnd,&point) || AudioOsdHitTarget(hwnd,point) !=
            static_cast<int>(llcv::audio_osd::HitTarget::Mute)) return false;
    NoteFullscreenCursorActivity();
    SetCapture(hwnd);
    g_muteButtonPressed = GetCapture()==hwnd;
    return true;
}

static bool AdjustVolumeFromWheel(HWND root, WPARAM wParam, LPARAM lParam) {
    const POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    HWND hovered = WindowFromPoint(screenPoint);
    if (!hovered || GetAncestor(hovered, GA_ROOT) != root) return false;

    static int wheelRemainder = 0;
    const int hit = AudioOsdHitTarget(root, screenPoint);
    const int target = hit == static_cast<int>(llcv::audio_osd::HitTarget::Mute) ? 3 : hit;
    if (g_settings.audioOnly && target != 1 && target != 2 && target != 3) {
        // Header, footer and gutters are window-drag areas, not volume controls.
        wheelRemainder = 0;
        return true;
    }
    wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
    const int steps = wheelRemainder / WHEEL_DELTA;
    wheelRemainder %= WHEEL_DELTA;
    if (steps == 0) return true;

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
    SetPlaybackMuted(root, false, false);
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

// Audio-only owns its presentation; it shares only the underlying audio state
// and volume controls with the video overlay.
static bool g_audioOnlyHighContrast = false;
static void RefreshAudioOnlyTheme(HWND hwnd) {
    g_audioOnlyHighContrast = llcv::ui::HighContrastEnabled();
    const auto palette = llcv::ui::ResolvePalette(g_settings.settingsLightTheme, g_audioOnlyHighContrast);
    const BOOL dark = !g_audioOnlyHighContrast && !g_settings.settingsLightTheme;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = g_audioOnlyHighContrast ? DWMWA_COLOR_DEFAULT : palette.kBackground;
    const COLORREF text = g_audioOnlyHighContrast ? DWMWA_COLOR_DEFAULT : palette.kText;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
}
static void PaintAudioOnlyView(HDC dc,
                               const llcv::audio_only_view::Rect& content) {
    llcv::audio_only_view::State state{};
    state.english = IsEnglishUi();
    state.lightTheme = g_settings.settingsLightTheme;
    state.highContrast = g_audioOnlyHighContrast;
    state.allowBoost = g_settings.allowVolumeBoost;
    state.muted = g_playbackMuted.load(std::memory_order_relaxed);
    state.hoveredTarget = g_audioOsdHoverTarget.load(std::memory_order_acquire);
    state.outputLabel = ActiveAudioMode() == AudioMode::WasapiExclusive
        ? L"WASAPI Exclusive"
        : ActiveAudioMode() == AudioMode::Asio ? L"ASIO" : L"WASAPI Shared";
    state.clipping = GetTickCount64() < g_audioClipUntilMs.load(
        std::memory_order_acquire);
    state.masterPercent = g_volumePercent.load(std::memory_order_acquire);
    state.leftPercent = g_leftVolumePercent.load(std::memory_order_acquire);
    state.rightPercent = g_rightVolumePercent.load(std::memory_order_acquire);
    state.leftPeakDb = llcv::audio::PeakToDbfs(
        g_audioPeakLeft.load(std::memory_order_acquire));
    state.rightPeakDb = llcv::audio::PeakToDbfs(
        g_audioPeakRight.load(std::memory_order_acquire));
    llcv::audio_only_view::Paint(dc, content, state);
}

struct AudioOnlyPaintBuffer {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    int width = 0;
    int height = 0;

    void Release() {
        if (dc && previousBitmap) SelectObject(dc, previousBitmap);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc = nullptr;
        bitmap = nullptr;
        previousBitmap = nullptr;
        width = height = 0;
    }

    bool Ensure(HDC target, int requestedWidth, int requestedHeight) {
        if (dc && width == requestedWidth && height == requestedHeight)
            return true;
        Release();
        if (requestedWidth <= 0 || requestedHeight <= 0) return false;
        dc = CreateCompatibleDC(target);
        if (dc) bitmap = CreateCompatibleBitmap(
            target, requestedWidth, requestedHeight);
        if (!bitmap) {
            Release();
            return false;
        }
        previousBitmap = SelectObject(dc, bitmap);
        width = requestedWidth;
        height = requestedHeight;
        return true;
    }
};

static AudioOnlyPaintBuffer g_audioOnlyPaintBuffer;

static void HandleViewerExit(HWND hwnd) {
    if (g_fullscreen && !g_autoFullscreen) {
        ToggleFullscreen(hwnd);
    } else {
        if (g_fullscreen) EndFullscreenCursorTracking(hwnd);
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
}

#ifdef LLCV_RECORDING
// Closing never joins the writer on the UI thread. Keep pumping messages while
// Finalize/validation runs, and offer an explicit escape if a driver stalls.
static bool RecordingCloseInteractive(const wchar_t* commandLine=GetCommandLineW()) {
    // Only diagnostic runs skip interactive close confirmation.
    return !commandLine || wcsstr(commandLine,L"--smoke-test")==nullptr;
}
static bool BeginRecordingClose(HWND hwnd,bool ask) {
    if (g_recordingClosePrompt) return false;
    if (!llcv::recording::recorder.Busy()) return true;
    if (!g_recordingClosePending) {
        if (ask) {
            g_recordingClosePrompt=true;
            const int answer=MessageBoxW(hwnd,IsEnglishUi()
                ? L"Finish the recording and close? The app will stay open until saving completes."
                : L"녹화를 마무리하고 닫을까요?\n파일 저장이 끝날 때까지 앱을 유지합니다.",
                L"LLCV",MB_YESNO|MB_ICONQUESTION);
            g_recordingClosePrompt=false;
            if (answer!=IDYES) { g_restartToSettings=false;return false; }
        }
        g_recordingClosePending=true;
        g_recordingCloseWarningAt=GetTickCount64()+15000;
        llcv::recording::SetControlsClosing(true);
        llcv::recording::recorder.Stop();
        UpdateViewerRecordingIndicator(llcv::recording::recorder.GetSnapshot());
    }
    return false;
}
static bool PollRecordingClose(HWND hwnd) {
    if (!llcv::recording::recorder.Busy()) {
        if (g_recordingShutdownBlocked) {
            ShutdownBlockReasonDestroy(hwnd);g_recordingShutdownBlocked=false;
        }
        if (g_recordingClosePending && !g_recordingClosePrompt &&
            RecordingCloseInteractive()) {
            const auto saved=llcv::recording::recorder.GetSnapshot();
            if (saved.failure!=llcv::recording::Failure::None || !saved.playable) {
                g_recordingClosePrompt=true;
                std::wstring message=IsEnglishUi()
                    ? L"There was a problem saving the recording.\n"
                    : L"녹화 저장 중 문제가 있었습니다.\n";
                message+=saved.playable ? (IsEnglishUi()
                    ? L"A saved portion can be opened; check its final section.\n\n"
                    : L"저장된 일부 영상은 열 수 있습니다. 마지막 구간을 확인하세요.\n\n")
                    : (IsEnglishUi()?L"The remaining file is not verified.\n\n":L"남은 파일의 정상 저장은 확인되지 않았습니다.\n\n");
                message+=saved.path;
                MessageBoxW(hwnd,message.c_str(),L"LLCV",MB_OK|MB_ICONWARNING);
                g_recordingClosePrompt=false;
            }
        }
        return true;
    }
    if ((g_recordingClosePending || g_recordingDrainPending) && !g_recordingClosePrompt &&
        GetTickCount64()>=g_recordingCloseWarningAt) {
        g_recordingClosePrompt=true;
        const int answer=MessageBoxW(hwnd,IsEnglishUi()
            ? L"Saving is taking longer than expected. Keep waiting?\n\nYes: continue saving.\nNo: force close LLCV. The recording may be incomplete or unreadable."
            : L"녹화 저장이 지연되고 있습니다. 계속 기다릴까요?\n\n예: 저장을 계속 기다립니다.\n아니요: LLCV를 강제로 닫습니다. 녹화 일부가 유실되거나 파일을 읽지 못할 수 있습니다.",
            L"LLCV",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON1);
        g_recordingClosePrompt=false;
        // Never terminate an individual thread or abandon live COM objects.
        // Only the user's explicit force-close choice ends the whole process.
        if (answer==IDNO && llcv::recording::recorder.Busy())
            TerminateProcess(GetCurrentProcess(),ERROR_TIMEOUT);
        g_recordingCloseWarningAt=GetTickCount64()+30000;
    }
    return !llcv::recording::recorder.Busy();
}
static void DrainRecordingBeforeDestroy(HWND hwnd) {
    if (!llcv::recording::recorder.Busy()) return;
    BeginRecordingClose(hwnd,false);
    while (!PollRecordingClose(hwnd)) {
        MsgWaitForMultipleObjects(0,nullptr,FALSE,100,QS_ALLINPUT);
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if (message.message==WM_QUIT) continue;
            TranslateMessage(&message);DispatchMessageW(&message);
        }
    }
}
#endif

// Shared by the viewer window and child-focus message loop. Resolve modifiers
// once per key event; no allocations or work on the capture/render threads.
static bool HandleViewerShortcut(HWND hwnd, WPARAM key, LPARAM flags, unsigned modifiers) {
    if (key>=256) return false;
    const auto found=std::find(g_settings.shortcuts.begin(),g_settings.shortcuts.end(),
        static_cast<unsigned>(key)|modifiers);
    if (found==g_settings.shortcuts.end()) return false;
    const auto action=static_cast<keys::Action>(found-g_settings.shortcuts.begin());
    // These two have their own modeless-help / source-frame routing below.
    if (action==keys::Action::Help || action==keys::Action::Screenshot) return false;
    if (flags & (LPARAM{1}<<30)) return true;
#ifdef LLCV_RECORDING
    if ((g_recordingClosePending || g_recordingDrainPending) && action==keys::Action::Settings) return true;
#endif
    switch (action) {
    case keys::Action::Settings: SendMessageW(hwnd,WM_OPEN_SETTINGS,0,0); break;
    case keys::Action::AudioOsd: ToggleAudioOsd(); break;
    case keys::Action::Mute: SetPlaybackMuted(hwnd,!g_playbackMuted.load(std::memory_order_relaxed)); break;
    case keys::Action::PixelPerfect: SendMessageW(hwnd,WM_RESTORE_ONE_TO_ONE,0,0); break;
#ifdef LLCV_VSR_FEATURE
    case keys::Action::Vsr: return HandleVsrTestKey(key,flags,modifiers);
#endif
    case keys::Action::Fullscreen: ToggleFullscreen(hwnd); break;
    case keys::Action::Diagnostics: ToggleRuntimeOsd(); break;
    case keys::Action::Exit: HandleViewerExit(hwnd); break;
#ifdef LLCV_RECORDING
    case keys::Action::Recording:
        if (!g_settings.audioOnly) {
            llcv::recording::ToggleRecording();
            UpdateViewerRecordingIndicator(llcv::recording::recorder.GetSnapshot());
        }
        break;
#endif
    default: return false;
    }
    return true;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_POWERBROADCAST:
        g_playbackPowerEvents.Notify(wParam);
        return TRUE;

    case WM_CREATE:
        g_presentationActive.store(false, std::memory_order_release);
        g_presentationMinimized.store(false, std::memory_order_release);
        g_presentationBlocked.store(false, std::memory_order_release);
        g_presentationClosing.store(false, std::memory_order_release);
        g_presentationUiPending = false;
        g_deferredPresentationUi.store(DeferredPresentationUi::None, std::memory_order_release);
        g_presentationRevision.fetch_add(1, std::memory_order_release);
#ifdef LLCV_RECORDING
        g_recordingClosePending=false;g_recordingDrainPending=false;g_recordingClosePrompt=false;
        g_recordingShutdownBlocked=false;g_recordingCloseWarningAt=0;
        llcv::recording::SetControlsClosing(false);
#endif
        // Connected/Modern Standby broadcasts are opt-in. Subscribe once for
        // this viewer, never on the capture/render path or while in Settings.
        if (const DWORD error = g_suspendResumeNotification.Initialize(
                hwnd, g_settings.preventSleep && g_playbackPowerEvents.Event()))
            LogHr(L"Register playback suspend/resume notification", HRESULT_FROM_WIN32(error));
        // A viewer destroyed during a modal move must not leave a deferred
        // transition behind when settings restart it in this process.
        g_outputTransition = {};
        g_interactiveWindowMove = false;
        g_audioOnlyResizePending = false;
        g_audioOnlyDragPending = false;
        g_audioOnlyPaintBuffer.Release();
        g_viewerCorners.Reset();
        ApplyViewerCorners(hwnd);
        if (g_settings.audioOnly) RefreshAudioOnlyTheme(hwnd);
        g_relativeMoveMonitor = nullptr;
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
                if (ExclusiveOutputSelected()) ShowWindow(g_videoHost, SW_HIDE);
            }
        }
        SetTimer(hwnd, 1, g_settings.audioOnly ? 33 : 500, nullptr);
#ifdef LLCV_RECORDING
        if (!g_settings.audioOnly) {
            if (!ExclusiveOutputSelected())
                llcv::recording::AttachRecordingIndicator(g_videoHost,hwnd,IsEnglishUi(),g_settings.settingsLightTheme);
            UpdateViewerRecordingIndicator(llcv::recording::recorder.GetSnapshot());
        }
#endif
        return 0;

    case WM_SIZE:
        if (g_presentationMinimized.exchange(wParam == SIZE_MINIMIZED,
                std::memory_order_acq_rel) != (wParam == SIZE_MINIMIZED))
            NotifyPresentationChange();
        ApplyViewerCorners(hwnd);
        if (g_videoHost) {
            MoveWindow(g_videoHost, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        // Coalesce F11/F5 and interactive-drag notifications before rebuilding.
        if (!g_settings.audioOnly && wParam != SIZE_MINIMIZED &&
            g_outputTransition.OnClientSize(LOWORD(lParam), HIWORD(lParam))) {
            g_outputConfigurationGeneration.fetch_add(
                1, std::memory_order_acq_rel);
        }
        if (g_settings.audioOnly) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_PAINT:
        if (g_settings.audioOnly) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client{};
            GetClientRect(hwnd, &client);
            const auto panel = llcv::audio_only_view::ContentRect(
                client.right, client.bottom);
            const RECT panelRect{panel.left, panel.top, panel.right, panel.bottom};
            RECT panelDirty{};
            const bool paintPanel =
                IntersectRect(&panelDirty, &paint.rcPaint, &panelRect);
            const int panelWidth = panel.right - panel.left;
            const int panelHeight = panel.bottom - panel.top;
            const bool buffered = paintPanel && g_audioOnlyPaintBuffer.Ensure(
                dc, panelWidth, panelHeight);
            const auto palette = llcv::ui::ResolvePalette(g_settings.settingsLightTheme, g_audioOnlyHighContrast);
            const HBRUSH background = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
            SetDCBrushColor(dc, palette.kBackground);
            if (buffered) {
                HDC backDc = g_audioOnlyPaintBuffer.dc;
                const RECT localPanel{0, 0, panelWidth, panelHeight};
                SetDCBrushColor(backDc, palette.kBackground);
                FillRect(backDc, &localPanel, background);
                SetViewportOrgEx(backDc, -panel.left, -panel.top, nullptr);
                PaintAudioOnlyView(backDc, panel);
                SetViewportOrgEx(backDc, 0, 0, nullptr);

                // The changing panel is double-buffered. Only newly exposed
                // background is cleared, so a large window is not repainted
                // in full at the audio meter's 30 Hz update rate.
                const int savedDc = SaveDC(dc);
                if (savedDc != 0) {
                    ExcludeClipRect(dc, panel.left, panel.top,
                                    panel.right, panel.bottom);
                    FillRect(dc, &paint.rcPaint, background);
                    RestoreDC(dc, savedDc);
                } else {
                    FillRect(dc, &paint.rcPaint, background);
                }
                BitBlt(dc, panel.left, panel.top,
                       panelWidth, panelHeight,
                       backDc, 0, 0, SRCCOPY);
            } else {
                FillRect(dc, &paint.rcPaint, background);
                if (paintPanel) PaintAudioOnlyView(dc, panel);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        if (g_settings.audioOnly) return 1;
        break;

    case WM_DWMCOMPOSITIONCHANGED:
    case WM_SETTINGCHANGE: case WM_THEMECHANGED: case WM_SYSCOLORCHANGE:
        g_viewerCorners.Reset();
        ApplyViewerCorners(hwnd);
        if (g_settings.audioOnly) {
            RefreshAudioOnlyTheme(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case WM_SIZING:
        if (g_settings.audioOnly) {
            g_audioOnlyResizePending = true;
            if (lParam) {
                ConstrainWindowRectToAudioAspect(
                    hwnd, *reinterpret_cast<RECT*>(lParam),
                    static_cast<UINT>(wParam), GetDpiForWindow(hwnd));
                return TRUE;
            }
            break;
        }
        if (lParam && !g_settings.audioOnly && !g_fullscreen &&
            !g_settings.pixelPerfect) {
            g_outputTransition.SetManualResize(true);
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
            llcv::screenshot::Result screenshot;
            if (g_screenshots.TakeResult(screenshot)) {
                fwprintf(stderr, L"[screenshot] PNG=0x%08X clipboard-requested=%d clipboard=0x%08X "
                    L"HDR-to-SDR=%d copy=%.3f ms path=%s\n",
                    static_cast<unsigned>(screenshot.fileResult), screenshot.clipboardRequested,
                    static_cast<unsigned>(screenshot.clipboardResult), screenshot.toneMapped,
                    screenshot.copyMs, screenshot.path.c_str());
                ShowTransientHud(FAILED(screenshot.fileResult) ? TransientHudContent::ScreenshotFailed
                    : !screenshot.clipboardRequested ? TransientHudContent::ScreenshotSaved
                    : screenshot.clipboardResult == S_OK ? TransientHudContent::ScreenshotCopied
                    : TransientHudContent::ScreenshotClipboardFailed);
            } else if (g_screenshots.ExpireRequest()) {
                fwprintf(stderr, L"[screenshot] no valid frame within 3 seconds.\n");
                ShowTransientHud(TransientHudContent::ScreenshotTimeout);
            }
            // All display enumeration stays off the capture/render thread.
#ifdef LLCV_RECORDING
            UpdateViewerRecordingIndicator(llcv::recording::recorder.GetSnapshot());
            if (PollRecordingClose(hwnd) && g_recordingClosePending && !g_recordingClosePrompt) {
                g_recordingClosePending=false;
                PostMessageW(hwnd,WM_CLOSE,0,0);
            }
#endif
            // Also catches HDR/SDR white changes for which no resize is sent.
            static ULONGLONG nextHdrDisplayCheck = 0;
            const ULONGLONG now = GetTickCount64();
            if (g_hdrOutputActive.load(std::memory_order_acquire) && now >= nextHdrDisplayCheck) {
                nextHdrDisplayCheck = now + 2000;
                RefreshHdrDisplayStatus(hwnd);
            }
            FlushAudioDiagnostics();
            UpdateOsdRates();
            g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
            if (g_settings.audioOnly) {
                RECT client{};
                if (GetClientRect(hwnd, &client)) {
                    const auto panel = llcv::audio_only_view::ContentRect(
                        client.right, client.bottom);
                    const RECT panelRect{panel.left, panel.top,
                                         panel.right, panel.bottom};
                    InvalidateRect(hwnd, &panelRect, FALSE);
                }
            }
            return 0;
        }
        break;

    case WM_ACTIVATE:
        if (g_presentationActive.exchange(LOWORD(wParam) != WA_INACTIVE,
                std::memory_order_acq_rel) != (LOWORD(wParam) != WA_INACTIVE))
            NotifyPresentationChange();
        break;

    case WM_ACTIVATEAPP:
        UpdateBackgroundAudioMute(wParam != FALSE);
        return 0;

    case WM_MOUSEMOVE:
        NoteFullscreenCursorActivity();
        if (!g_settings.audioOnly && g_audioOsdVisible.load(std::memory_order_acquire)) {
            POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd,&point);
            const int hovered=AudioOsdHitTarget(hwnd,point);
            if (g_audioOsdHoverTarget.exchange(hovered,std::memory_order_relaxed)!=hovered)
                g_overlayGeneration.fetch_add(1,std::memory_order_relaxed);
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,hwnd,0};
            TrackMouseEvent(&tracking);
        }
        if (g_settings.audioOnly) {
            RECT client{};
            int hovered = 0;
            if (GetClientRect(hwnd, &client)) {
                const auto target = llcv::audio_only_view::HitTest(
                    client.right, client.bottom,
                    GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                if (target == llcv::audio_osd::HitTarget::Master ||
                    target == llcv::audio_osd::HitTarget::Mute ||
                    target == llcv::audio_osd::HitTarget::Left ||
                    target == llcv::audio_osd::HitTarget::Right)
                    hovered = static_cast<int>(target);
            }
            if (g_audioOsdHoverTarget.exchange(hovered, std::memory_order_acq_rel) != hovered)
                InvalidateRect(hwnd, nullptr, FALSE);
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
        }
        if (g_settings.audioOnly && g_audioOnlyDragPending &&
            GetCapture() == hwnd) {
            if ((wParam & MK_LBUTTON) == 0) {
                g_audioOnlyDragPending = false;
                ReleaseCapture();
                return 0;
            }
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (!ClientToScreen(hwnd, &point)) return 0;
            const UINT dpi = GetDpiForWindow(hwnd);
            if (std::abs(point.x - g_audioOnlyDragStart.x) >=
                    (std::max)(1, GetSystemMetricsForDpi(SM_CXDRAG, dpi)) ||
                std::abs(point.y - g_audioOnlyDragStart.y) >=
                    (std::max)(1, GetSystemMetricsForDpi(SM_CYDRAG, dpi))) {
                g_audioOnlyDragPending = false;
                ReleaseCapture();
                // Enter the native move loop only after an actual drag.
                // Ordinary clicks remain client clicks for double-click reset.
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                             MAKELPARAM(point.x, point.y));
            }
            return 0;
        }
        break;

    case WM_MOUSELEAVE:
    case WM_NCMOUSEMOVE:
        if (!g_settings.audioOnly && g_audioOsdHoverTarget.exchange(0,std::memory_order_relaxed)!=0)
            g_overlayGeneration.fetch_add(1,std::memory_order_relaxed);
        if (g_settings.audioOnly &&
            g_audioOsdHoverTarget.exchange(0, std::memory_order_acq_rel) != 0)
            InvalidateRect(hwnd, nullptr, FALSE);
        break;

    case WM_LBUTTONDOWN:
        if (BeginMuteButtonPress(hwnd,lParam)) return 0;
        if (g_settings.audioOnly) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (ClientToScreen(hwnd, &point)) {
                g_audioOnlyDragStart = point;
                SetCapture(hwnd);
                g_audioOnlyDragPending = GetCapture() == hwnd;
            }
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (g_muteButtonPressed) {
            g_muteButtonPressed=false;
            const bool captured=GetCapture()==hwnd;
            if (captured) ReleaseCapture();
            POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            if (captured && ClientToScreen(hwnd,&point) && AudioOsdHitTarget(hwnd,point)==
                    static_cast<int>(llcv::audio_osd::HitTarget::Mute))
                SetPlaybackMuted(hwnd,!g_playbackMuted.load(std::memory_order_relaxed));
            return 0;
        }
#ifdef LLCV_RECORDING
        if (HandleGpuRecordingMouse(hwnd, msg, lParam)) return 0;
#endif
        [[fallthrough]];
    case WM_CANCELMODE:
        if (g_muteButtonPressed && GetCapture()==hwnd) ReleaseCapture();
        g_muteButtonPressed=false;
        if (g_settings.audioOnly) {
            g_audioOnlyDragPending = false;
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        break;

    case WM_CAPTURECHANGED:
        g_muteButtonPressed=false;
        g_audioOnlyDragPending = false;
        break;

    case WM_SETCURSOR:
        if (FullscreenCursorAutoHideActive() && g_fullscreenCursorHidden) {
            SetCursor(nullptr);
            return TRUE;
        }
        if (LOWORD(lParam)==HTCLIENT) {
            POINT point{};
            if (GetCursorPos(&point) && AudioOsdHitTarget(hwnd,point)==
                    static_cast<int>(llcv::audio_osd::HitTarget::Mute)) {
                SetCursor(LoadCursorW(nullptr,IDC_HAND));
                return TRUE;
            }
        }
#ifdef LLCV_RECORDING
        if (HandleGpuRecordingMouse(hwnd, msg, lParam)) return TRUE;
#endif
        break;

    case WM_DISPLAYCHANGE:
        if (g_settings.audioOnly) g_audioOnlyPaintBuffer.Release();
        NotifyPresentationChange();
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

    case WM_GETMINMAXINFO:
        if (g_settings.audioOnly && lParam) {
            const UINT dpi = GetDpiForWindow(hwnd);
            const auto clientMinimum = llcv::audio_only_view::MinimumClientSize(dpi);
            const SIZE minimum = OuterSizeForClientPixels(
                clientMinimum.width, clientMinimum.height,
                static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
                dpi);
            auto& limits = *reinterpret_cast<MINMAXINFO*>(lParam);
            limits.ptMinTrackSize = POINT{minimum.cx, minimum.cy};
            return 0;
        }
        break;

    case WM_GETDPISCALEDSIZE:
        if (g_settings.audioOnly) return FALSE;
        // During an interactive resize the incoming pending size can differ
        // from GetClientRect. Let Windows scale that user-controlled size;
        // the saved monitor-relative ratio is only a policy for moving.
        if (g_outputTransition.ManualResize() && !g_settings.pixelPerfect)
            return FALSE;
        if ((g_settings.pixelPerfect || g_settings.relativeWindowSize) &&
            !g_fullscreen && lParam) {
            const UINT pendingDpi = static_cast<UINT>(wParam);
            const HMONITOR targetMonitor =
                g_interactiveWindowMove && g_relativeMoveMonitor
                    ? g_relativeMoveMonitor
                    : MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
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
        if (g_settings.audioOnly) {
            g_audioOnlyPaintBuffer.Release();
            if (lParam) {
                RECT suggested = *reinterpret_cast<const RECT*>(lParam);
                ConstrainWindowRectToAudioAspect(
                    hwnd, suggested, WMSZ_BOTTOMRIGHT, LOWORD(wParam));
                SetWindowPos(hwnd, nullptr, suggested.left, suggested.top,
                             suggested.right - suggested.left,
                             suggested.bottom - suggested.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        if ((g_settings.pixelPerfect || g_settings.relativeWindowSize) &&
            !g_fullscreen) {
            if (!lParam) return 0;
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            const HMONITOR targetMonitor = MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST);
            const bool manualResize = g_outputTransition.ManualResize() && !g_settings.pixelPerfect;
            const SIZE desired = manualResize
                ? SIZE{suggested->right - suggested->left, suggested->bottom - suggested->top}
                : DesiredWindowOuterSize(hwnd, targetMonitor, LOWORD(wParam));
            if (desired.cx <= 0 || desired.cy <= 0) return 0;
            fwprintf(stderr,
                     L"[video] DPI changed: %u dpi, suggested outer %ld x %ld\n",
                     LOWORD(wParam), suggested->right - suggested->left,
                     suggested->bottom - suggested->top);
            BeginOutputTransition();
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         desired.cx, desired.cy,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_interactiveWindowMove) g_relativeMoveMonitor = targetMonitor;
            EndOutputTransition(false);
            return 0;
        }
        break;

    case WM_ENTERSIZEMOVE:
        ResetWindowSnapState();
        if (g_settings.audioOnly) {
            g_audioOnlyResizePending = false;
            return 0;
        }
        // Moving can resize a monitor-relative window, just like WM_SIZING.
        // Keep one transition open until final geometry has been reconciled.
        if (!g_interactiveWindowMove) {
            g_interactiveWindowMove = true;
            BeginOutputTransition();
        }
        g_outputTransition.SetManualResize(false);
        g_relativeMoveMonitor =
            MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        return 0;

    case WM_MOVING:
        if (lParam) {
            if (!g_settings.audioOnly) {
                POINT cursor{};
                if (GetCursorPos(&cursor)) {
                    const HMONITOR monitor = MonitorFromPoint(
                        cursor, MONITOR_DEFAULTTONEAREST);
                    ApplyRelativeSizeForMonitor(
                        hwnd, monitor, cursor,
                        *reinterpret_cast<RECT*>(lParam));
                }
            }
            ApplyWindowEdgeSnap(hwnd, *reinterpret_cast<RECT*>(lParam));
            return TRUE;
        }
        break;

    case WM_EXITSIZEMOVE:
        ResetWindowSnapState();
        if (g_settings.audioOnly) {
            // A move may cross display adapters without a DPI change.
            g_audioOnlyPaintBuffer.Release();
            if (g_audioOnlyResizePending) {
                RECT client{};
                if (GetClientRect(hwnd, &client) &&
                    client.right > client.left && client.bottom > client.top) {
                    g_settings.audioOnlyWidth = client.right - client.left;
                    g_settings.audioOnlyHeight = client.bottom - client.top;
                }
            }
            g_audioOnlyResizePending = false;
            return 0;
        }
        g_relativeMoveMonitor = nullptr;
        if (g_outputTransition.ManualResize()) {
            RememberRelativeScaleFromWindow(hwnd);
        }
        // WM_MOVING / WM_DPICHANGED already chose the final relative size.
        // Reclassifying its monitor by overlap here can select the opposite
        // display solely because that size changed, making each exit oscillate.
        g_outputTransition.SetManualResize(false);
        NormalizeWindowSize(hwnd, true);
        if (g_interactiveWindowMove) {
            g_interactiveWindowMove = false;
            EndOutputTransition(false);
        }
        // An enclosing output transition still owns the deferred rebuild.
        if (g_outputTransition.TakePendingUpdate()) {
            g_outputConfigurationGeneration.fetch_add(
                1, std::memory_order_acq_rel);
        }
        return 0;

    case WM_NCHITTEST:
        if (g_settings.borderlessWindow && !g_fullscreen) {
            const LRESULT hit = BorderlessHitTest(hwnd, lParam);
            if (hit==HTCAPTION && AudioOsdHitTarget(hwnd,POINT{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)})==
                    static_cast<int>(llcv::audio_osd::HitTarget::Mute)) return HTCLIENT;
            if (!g_settings.audioOnly || hit != HTCAPTION) return hit;

            // Every interior region supports drag-to-move and double-click.
            // Keep native edge resize hit codes; start moving after a threshold.
            return HTCLIENT;
        }
        break;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (HandleViewerShortcut(hwnd,wParam,lParam,keys::Modifiers())) return 0;
        break;

    case WM_MOUSEWHEEL:
        NoteFullscreenCursorActivity();
        if (AdjustVolumeFromWheel(hwnd, wParam, lParam)) return 0;
        break;

    case WM_LBUTTONDBLCLK: {
        if (BeginMuteButtonPress(hwnd,lParam)) return 0;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(hwnd, &point);
        const int target = AudioOsdHitTarget(hwnd, point);
        if (target == 1 || target == 2 || target == 3) {
            SetPlaybackMuted(hwnd,false,false);
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
            g_audioOsdHoverTarget.store(target == 3 && !g_settings.audioOnly ? 0 : target,
                                        std::memory_order_release);
            g_overlayGeneration.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        break;
    }

    case WM_TOGGLE_RUNTIME_OSD:
        ToggleRuntimeOsd();
        return 0;

#ifdef LLCV_RECORDING
    case llcv::recording::kOpenRecordingSettings:
        if (g_recordingClosePending || g_recordingDrainPending) return 0;
        if(!g_settings.audioOnly){
            if(g_recordingSettingsWindow){SetForegroundWindow(g_recordingSettingsWindow);return 0;}
            if (DeferPresentationUi(DeferredPresentationUi::RecordingSettings)) return 0;
            g_presentationBlocked.store(true, std::memory_order_release);
            NotifyPresentationChange();
            ShowSettingsDialog(GetModuleHandleW(nullptr),true,hwnd);
            if (ExclusiveOutputSelected()) RefreshPresentationUiBlock();
            else g_presentationBlocked.store(false, std::memory_order_release);
            return 0;
        }
        return 0;
#endif

    case WM_OPEN_SETTINGS:
#ifdef LLCV_RECORDING
        if (g_recordingClosePending || g_recordingDrainPending) return 0;
        // A settings shortcut always opens the normal default page. Only an
        // explicit handoff from a live recording tab preserves its target.
        if (!wParam) g_requestedSettingsTab = -1;
#endif
        // The capture graph and WASAPI renderer are rebuilt only after this
        // window has closed and their threads have joined.
        g_restartToSettings.store(true, std::memory_order_release);
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case WM_PRESENTATION_UI_READY: {
        if (!g_presentationUiPending || static_cast<UINT_PTR>(lParam) !=
                g_deferredPresentationSerial.load(std::memory_order_acquire)) return 0;
        if (!g_presentationClosing.load(std::memory_order_acquire) && g_running.load()) {
            // Some drivers minimize the old fullscreen HWND on focus changes.
            // The user explicitly asked to open UI, so restore its owner now.
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            const auto action = static_cast<DeferredPresentationUi>(wParam);
            if (action == DeferredPresentationUi::Help)
                g_viewerHelp.ProcessMessage(g_deferredHelpMessage, hwnd, IsEnglishUi(),
                    kAppVersionLabel, g_settings.settingsLightTheme, g_deferredHelpModifiers);
#ifdef LLCV_RECORDING
            else if (action == DeferredPresentationUi::RecordingSettings)
                ShowSettingsDialog(GetModuleHandleW(nullptr), true, hwnd);
            else if (action == DeferredPresentationUi::ClosePrompt &&
                BeginRecordingClose(hwnd, g_running.load() && RecordingCloseInteractive()))
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
#endif
        }
        // A nested modal loop may have published a newer request while this
        // acknowledgement was on the stack. Only retire our own ticket.
        if (static_cast<UINT_PTR>(lParam) ==
                g_deferredPresentationSerial.load(std::memory_order_acquire))
            g_presentationUiPending = false;
        RefreshPresentationUiBlock();
        return 0;
    }

    case WM_RESTORE_ONE_TO_ONE:
        if (!g_settings.audioOnly) RestoreOneToOneWindow(hwnd);
        return 0;

    case WM_UPDATE_CHECK_COMPLETE: {
        auto result = g_updateCheckTask.TakeResult();
        if (!result || !result->success || !result->newer || result->latestTag.empty() ||
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

#ifdef LLCV_RECORDING
    case WM_RECORDING_CAPTURE_ENDED:
        // Capture's caller still chooses exit vs. return to Settings. Arm the
        // writer timeout now without racing that disposition by destroying UI.
        if (!g_recordingDrainPending) {
            g_recordingDrainPending=true;
            g_recordingCloseWarningAt=GetTickCount64()+15000;
            llcv::recording::SetControlsClosing(true);
            llcv::recording::recorder.Stop();
            UpdateViewerRecordingIndicator(llcv::recording::recorder.GetSnapshot());
        }
        return 0;
#endif

    case WM_QUERYENDSESSION:
#ifdef LLCV_RECORDING
        if (llcv::recording::recorder.Busy()) {
            llcv::recording::recorder.Stop();
            if (!(lParam & ENDSESSION_CRITICAL)) {
                g_recordingShutdownBlocked=ShutdownBlockReasonCreate(hwnd,IsEnglishUi()
                    ? L"Finishing the recording file" : L"녹화 파일 저장을 마무리하고 있습니다")!=FALSE;
                return FALSE;
            }
        }
#endif
        return TRUE;

    case WM_ENDSESSION:
#ifdef LLCV_RECORDING
        if (wParam) {
            llcv::recording::SetControlsClosing(true);
            llcv::recording::recorder.Stop();
            g_running=false;
            PostMessageW(hwnd,WM_CLOSE,0,0);
        }
#endif
        return 0;

    case WM_CLOSE:
#ifdef LLCV_RECORDING
        if (llcv::recording::recorder.Busy() && !g_recordingClosePending && !g_recordingClosePrompt &&
            DeferPresentationUi(DeferredPresentationUi::ClosePrompt)) return 0;
        if (!BeginRecordingClose(hwnd,g_running.load() && RecordingCloseInteractive())) return 0;
#endif
        g_deferredPresentationUi.store(DeferredPresentationUi::None, std::memory_order_release);
        g_presentationUiPending = false;
        // DXGI may send synchronous window messages while leaving fullscreen.
        // Keep this UI thread pumping until the output owner has released it.
        if (ExclusiveOutputSelected() && g_videoOutputThreadActive.load(std::memory_order_acquire)) {
            // Stop audio consumption with capture, rather than draining an
            // empty ring while DXGI or a recording writer finishes teardown.
            g_running.store(false, std::memory_order_release);
            if (!g_presentationClosing.exchange(true, std::memory_order_acq_rel))
                NotifyPresentationChange();
            return 0;
        }
        PersistWindowPosition(hwnd);
        g_windowPositionPersisted = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_deferredPresentationUi.store(DeferredPresentationUi::None, std::memory_order_release);
        g_presentationUiPending = false;
#ifdef LLCV_RECORDING
        if (g_recordingShutdownBlocked) {
            ShutdownBlockReasonDestroy(hwnd);g_recordingShutdownBlocked=false;
        }
#endif
        if (const DWORD error = g_suspendResumeNotification.Reset())
            LogHr(L"Unregister playback suspend/resume notification", HRESULT_FROM_WIN32(error));
        g_audioOnlyDragPending = false;
        g_audioOnlyPaintBuffer.Release();
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
            auto savedSettings = g_settings;
            if (g_exclusiveStartupFallback) {
                savedSettings.audioMode = g_exclusiveStartupRequestedMode;
            }
            SaveSettings(true, &savedSettings);
        }
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void RunViewerMessageLoop(HWND hwnd) {
    g_viewerHelp.SetBindings(g_settings.shortcuts);
    MSG m{};
    while (g_running.load() || (ExclusiveOutputSelected() &&
                g_videoOutputThreadActive.load(std::memory_order_acquire))) {
        const int result = GetMessageW(&m, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0 && ExclusiveOutputSelected() &&
                g_videoOutputThreadActive.load(std::memory_order_acquire)) {
                g_running.store(false, std::memory_order_release);
                g_presentationClosing.store(true, std::memory_order_release);
                NotifyPresentationChange();
                continue;
            }
            break;
        }
        if (!g_viewerHelp.Handle() && keys::Match(g_settings.shortcuts, keys::Action::Help, m) &&
            !(m.lParam & (LPARAM{1} << 30)) &&
            DeferPresentationUi(DeferredPresentationUi::Help, &m, keys::Modifiers())) continue;
        if (g_viewerHelp.ProcessMessage(m, hwnd, IsEnglishUi(), kAppVersionLabel,
                g_settings.settingsLightTheme)) {
            RefreshPresentationUiBlock();
            // Help shares the UI thread's cursor, but must not inherit the
            // fullscreen idle-hide state. Restart the grace period on close.
            g_lastFullscreenCursorActivityMs = GetTickCount64();
            if (g_fullscreenCursorHidden) SetFullscreenCursorVisible(true);
            continue;
        }
        if (m.message == WM_MOUSEWHEEL) {
            // The message-loop fast path handles wheel volume before normal
            // dispatch, so it must also count as cursor activity.
            NoteFullscreenCursorActivity();
            if (AdjustVolumeFromWheel(hwnd, m.wParam, m.lParam)) {
                continue;
            }
        }
#ifdef LLCV_SPATIAL_PROTOTYPE
        // Private, non-persisting visual comparison. No production shortcut changes.
        if (g_spatialInteractivePreview && m.message == WM_KEYDOWN &&
            m.wParam == VK_F9 && keys::Modifiers() == 0) {
            if (!(m.lParam & (1LL << 30))) {
                g_settings.cmaaNisEnabled = g_cmaaNisRequest.load() < 0;
                g_settings.vsrEnabled = false;
                g_vsrMode.store(llcv::vsr::Mode::Off);
                g_cmaaNisRequest.store(g_settings.cmaaNisEnabled ? 15 : -1);
                SetWindowTextW(hwnd, g_settings.cmaaNisEnabled
                    ? L"LLCV · CMAA2 → NIS 0.15 · ON | F9: 비교 전환"
                    : L"LLCV · 기본 출력 · OFF | F9: 비교 전환");
            }
            continue;
        }
#endif
        // Keyboard focus can belong to the video-host child. Key messages do
        // not bubble to its parent, so route viewer shortcuts at the thread
        // message-loop level.
        if (keys::KeyDown(m.message) && HandleViewerShortcut(hwnd,m.wParam,m.lParam,keys::Modifiers())) continue;
#ifdef LLCV_HDR_FRAME_AUDIT
        if (m.message == WM_KEYDOWN && m.wParam == VK_F8) {
            if (!(m.lParam & (1LL << 30))) {
                g_hdrFrameAuditRequested.store(true);
                fwprintf(stderr, L"[hdr-audit] One frame requested; waiting for a video frame.\n");
            }
            continue;
        }
#endif
        if (keys::Match(g_settings.shortcuts,keys::Action::Screenshot,m)) {
            if (llcv::viewer_help::IsScreenshotRequest(m, g_settings.audioOnly,g_settings.shortcuts))
                ShowTransientHud(g_screenshots.Request(g_settings.screenshotClipboard)
                    ? TransientHudContent::ScreenshotPending : TransientHudContent::ScreenshotBusy);
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
        RefreshPresentationUiBlock();
    }

    // Capture can stop while an earlier UI message is still being handled.
    // The running guard then exits before the queued WM_CLOSE. Finish on the
    // owning UI thread so WM_DESTROY persists volume/placement exactly once.
    // A normal WM_CLOSE has already destroyed the window, so this is a no-op.
#ifdef LLCV_RECORDING
    DrainRecordingBeforeDestroy(hwnd);
#endif
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
}

static void RelaunchWithSettings() {
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable,
                                             ARRAYSIZE(executable));
    if (!length || length >= ARRAYSIZE(executable)) return;

    std::wstring commandLine = L"\"";
    commandLine += executable;
    commandLine += L"\" --force-settings";
#ifdef LLCV_RECORDING
    if(g_requestedSettingsTab>=0)commandLine+=L" --settings-tab "+std::to_wstring(g_requestedSettingsTab);
#endif
#ifdef LLCV_VSR_EXPERIMENT
    if (g_vsrMode == llcv::vsr::Mode::On) commandLine += L" --vsr-test on";
    else if (g_vsrMode == llcv::vsr::Mode::Off) commandLine += L" --vsr-test off";
#endif
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
    commandLine += g_useScrgbPrototype ? L" --hdr-output scrgb" : L" --hdr-output hdr10";
#endif
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

#if defined(LLCV_VSR_EXPERIMENT) || defined(LLCV_GPU_DIAGNOSTICS)
static void ApplyVsrLiveTestSettings(AppSettings& settings) {
    // Session-only overrides; saved optional capture rates must not change
    // the fixed 1080p120 -> 1440p A/B measurement conditions.
    settings.videoPreset = VideoPreset::R2560x1440;
    settings.capturePreset = VideoPreset::R1920x1080;
    settings.displayMatchesCapture = false;
    settings.vsrEnabled = true;
    settings.videoFrameRate = 120;
    settings.capture40Fps = false;
    settings.pixelFormat = VideoPixelFormat::Nv12;
    settings.presentationMode = PresentationMode::AllowTearing;
    settings.scalingMode = ScalingMode::Smooth;
    settings.pixelPerfect = true;
    settings.relativeWindowSize = false;
    settings.skipStartupSettings = true;
}
#endif

static bool SuppressSettingsWritesForRun(const wchar_t* commandLine) {
#if defined(LLCV_HDR_FRAME_AUDIT) || defined(LLCV_VSR_EXPERIMENT) || defined(LLCV_SPATIAL_PROTOTYPE)
    (void)commandLine;
    return true;
#else
    return commandLine &&
        (wcsstr(commandLine, L"--smoke-test") != nullptr ||
         wcsstr(commandLine, L"--exclusive-probe") != nullptr);
#endif
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR commandLine, int show) {
#ifdef LLCV_RECORDING
    const int probeExit = llcv::recording::RunEncoderProbeChild(commandLine,
        [] { return llcv::recording::ProbeEncoders(); });
    if (probeExit >= 0) return probeExit;
#endif
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX commonControls{
        sizeof(commonControls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&commonControls);

    // Diagnostic runs must suppress migrations before loading settings, not
    // only suppress later saves after a real settings file was already changed.
    g_suppressSettingsSave = SuppressSettingsWritesForRun(commandLine);
#ifdef LLCV_VSR_EXPERIMENT
    const auto vsrOption = CommandLineOptionValue(L"--vsr-test");
    if (vsrOption == L"on") g_vsrMode = llcv::vsr::Mode::On;
    else if (vsrOption == L"off") g_vsrMode = llcv::vsr::Mode::Off;
    else if (!vsrOption.empty()) {
        MessageBoxW(nullptr, L"Use --vsr-test on or --vsr-test off.", L"VSR experiment", MB_OK | MB_ICONERROR);
        return 2;
    }
#endif
    // Validate before loading a profile: override-only invocations must not
    // accidentally become smoke runs through substring matching.
    int smokeArgc = 0;
    LPWSTR* smokeArgv = CommandLineToArgvW(GetCommandLineW(), &smokeArgc);
    if (!smokeArgv) return 2;
    std::vector<std::wstring> smokeArgs;
    for (int i=1; i<smokeArgc; ++i) smokeArgs.emplace_back(smokeArgv[i]);
    LocalFree(smokeArgv);
    llcv::settings::AppSettings smokeValidation{};
    if (!llcv::settings::ApplySmokeVideoOptions(smokeArgs, smokeValidation)) return 2;
    LoadSettings();
    bool smokeVideoOverride = false;
    if (!llcv::settings::ApplySmokeVideoOptions(smokeArgs, g_settings, &smokeVideoOverride)) return 2;
#ifdef LLCV_UPSCALING_TELEMETRY
    // Only the separately named diagnostic viewer defines this flag. Its
    // TestProfile boundary prevents reading/migrating/writing real settings.
    g_settings.checkForUpdates=false;g_settings.skipStartupSettings=false;
    if(CommandLineOptionValue(L"--overflow-live")==L"yes") {
        g_settings.capturePreset=VideoPreset::R1920x1080;
        g_settings.videoPreset=VideoPreset::R2560x1440;
        g_settings.displayMatchesCapture=false;g_settings.videoFrameRate=60;
        g_settings.capture40Fps=false;g_settings.pixelFormat=VideoPixelFormat::Nv12;
        g_settings.pixelPerfect=false;g_settings.relativeWindowSize=false;
        g_settings.presentationMode=PresentationMode::AllowTearing;
        g_settings.vsrEnabled=false;g_settings.cmaaNisEnabled=true;
        g_settings.preferredUpscaler=llcv::settings::UpscalingMethod::CmaaNis;
        g_settings.nisSharpnessPercent=15;
    }
#endif
#ifdef LLCV_SPATIAL_PROTOTYPE
    // Private fixed-condition live comparison. Overrides are never persisted.
    g_spatialInteractivePreview = CommandLineOptionValue(L"--spatial-interactive") == L"yes";
    if (CommandLineOptionValue(L"--spatial-live-test")==L"yes") {
        g_settings.capturePreset=VideoPreset::R1920x1080;
        g_settings.videoPreset=VideoPreset::R2560x1440;
        g_settings.displayMatchesCapture=false;g_settings.videoFrameRate=60;
        g_settings.capture40Fps=false;g_settings.pixelFormat=VideoPixelFormat::Nv12;
        g_settings.presentationMode=PresentationMode::AllowTearing;
        g_settings.scalingMode=ScalingMode::Smooth;g_settings.pixelPerfect=false;
        g_settings.relativeWindowSize=false;g_settings.skipStartupSettings=true;
        const auto reference=CommandLineOptionValue(L"--spatial-reference");
        g_settings.vsrEnabled=reference==L"vsr";
        g_settings.cmaaNisEnabled=reference==L"production-cmaa";
        g_settings.nisSharpnessPercent=15;
        if(CommandLineOptionValue(L"--spatial-hdr")==L"yes")g_settings.pixelFormat=VideoPixelFormat::P010;
        g_settings.checkForUpdates=false;
    }
#endif
#ifdef LLCV_RECORDING
    llcv::recording::sessionOptions=llcv::recording::LoadOptions();
    const auto requestedTab=CommandLineOptionValue(L"--settings-tab");
    if(requestedTab.size()==1 && requestedTab[0]>=L'0' && requestedTab[0]<L'0'+llcv::settings_ui::kSettingsNavigationCount)
        g_requestedSettingsTab=requestedTab[0]-L'0';
#endif
#ifdef LLCV_VSR_EXPERIMENT
    // Reproducible, session-only live-capture experiment. Same input/output
    // plan for ON and OFF, no migration/profile writes and no capture restart.
    const auto vsrLiveTest = CommandLineOptionValue(L"--vsr-live-test");
    if (!vsrLiveTest.empty()) {
        if (vsrLiveTest != L"on" && vsrLiveTest != L"off") return 2;
        ApplyVsrLiveTestSettings(g_settings);
    }
    if (!vsrOption.empty()) g_settings.vsrEnabled = vsrOption == L"on";
#endif
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
    g_useScrgbPrototype = CommandLineOptionValue(L"--hdr-output") != L"hdr10";
#endif
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
    g_suppressSettingsSave = SuppressSettingsWritesForRun(commandLine);
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
    // Identify the default renderer once, before settings or immediate start.
    // No extension requests, capture probes or per-frame checks are added.
    const auto vsrAdapter = llcv::vsr::ProbeDefaultAdapter();
    g_vsrGpuVendor.store(vsrAdapter.vendor, std::memory_order_release);
    if (!llcv::vsr::NvidiaAdapter(vsrAdapter.vendor)) g_settings.vsrEnabled = false;
    const bool reviewStartupVersion = !g_suppressSettingsSave &&
        llcv::settings::StartupReviewRequired(SettingsPath(),kAppVersionLabel);
    const bool showStartupSettings = !smokeTest && !exclusiveProbe &&
        (forceSettings || shiftLaunch || reviewStartupVersion || !g_settings.skipStartupSettings ||
         !llcv::presentation::SupportsCapture(g_settings.presentationMode,
                                               g_settings.pixelFormat, g_settings.audioOnly));
    if (showStartupSettings &&
        !ShowSettingsDialog(hInst, forceSettings,nullptr,reviewStartupVersion)) return 0;
    g_activeAudioMode.store(g_settings.audioMode, std::memory_order_release);
    if (g_settings.audioOnly) {
        // Audio-only is an audio meter window, so the existing audio OSD is
        // visible from the first paint instead of a placeholder status label.
        g_audioOsdVisible.store(true, std::memory_order_release);
    }

    LatchVideoResolutionPlan();
    if (g_splitResolutionPlan) {
        const auto& capture = CurrentCapturePreset();
        const auto& display = CurrentVideoPreset();
        fwprintf(stderr, L"[video] resolution plan: capture %d x %d -> display %d x %d; VSR toggles preserve both.\n",
            capture.width, capture.height, display.width, display.height);
    }
    g_cmaaNisRequest.store(llcv::settings::SelectedUpscaler(g_settings)==llcv::settings::UpscalingMethod::CmaaNis
        ? llcv::upscaling::Request(g_settings.cmaaNisEnabled,g_settings.nisSharpnessPercent) : -1,
        std::memory_order_release);
    g_vsrMode.store(g_settings.vsrEnabled && !g_settings.cmaaNisEnabled ? llcv::vsr::Mode::On
                                         : llcv::vsr::Mode::Disabled,
                    std::memory_order_release);
#ifdef LLCV_VSR_EXPERIMENT
    if (vsrLiveTest == L"off") g_vsrMode = llcv::vsr::Mode::Off;
#endif
    // Allocate a console for prototype diagnostics.
#ifdef LLCV_VSR_EXPERIMENT
    g_settings.checkForUpdates = false;
    g_settings.saveLog = true; // F6 can opt in after a default launch.
#endif
#ifdef LLCV_HDR_FRAME_AUDIT
    // Diagnostic settings are session-only; never migrate/save the user's profile.
    g_settings.saveLog = true;
    g_settings.showDiagnosticConsole = true;
    g_settings.checkForUpdates = false;
    EnsureUserDataDirectory();
    g_hdrFrameAuditDirectory = LogDirectory();
    CreateDirectoryW(g_hdrFrameAuditDirectory.c_str(), nullptr);
#endif
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
#ifdef LLCV_HDR_SCRGB_PROTOTYPE
    fwprintf(stderr, L"[hdr-compare] Selected path: %s. P010 and HDR input/Force HDR10 are still required.\n",
             g_useScrgbPrototype ? L"direct shader/scRGB" : L"original VP/HDR10");
#endif
#ifdef LLCV_HDR_FRAME_AUDIT
    fwprintf(stderr, L"[hdr-audit] Private diagnostic build. With P010 selected, press F8 once "
             L"in the viewer to save a matching input/output frame pair. Readback may briefly "
             L"stall video. Local files only; settings are not saved. Directory: %s\n",
             g_hdrFrameAuditDirectory.c_str());
#endif
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

    const DWORD powerEventError = g_playbackPowerEvents.Initialize(g_settings.preventSleep);
    if (powerEventError) LogHr(L"Playback power event", HRESULT_FROM_WIN32(powerEventError));

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

    const DWORD windowStyle = ViewerWindowStyle(g_settings);
    constexpr DWORD windowExStyle = 0;
    HMONITOR initialMonitor = SavedViewerMonitor();
    if (!initialMonitor) {
        initialMonitor = MonitorFromPoint(
            POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    SIZE initialClient = g_settings.audioOnly
        ? SIZE{g_settings.audioOnlyWidth, g_settings.audioOnlyHeight}
        : InitialClientPixelsForMonitor(initialMonitor);
    const UINT initialDpi = EffectiveMonitorDpi(initialMonitor, nullptr);
    if (g_settings.audioOnly) {
        // Older settings stored width and height independently. Keep the
        // chosen width, but restore a proportional client area on launch.
        const SIZE frame = OuterSizeForClientPixels(
            0, 0, windowStyle, windowExStyle, initialDpi);
        int maximumClientWidth = 16384;
        int maximumClientHeight = 16384;
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfoW(initialMonitor, &info)) {
            maximumClientWidth = (std::max)(1,
                static_cast<int>(info.rcWork.right - info.rcWork.left - frame.cx));
            maximumClientHeight = (std::max)(1,
                static_cast<int>(info.rcWork.bottom - info.rcWork.top - frame.cy));
        }
        const auto minimum = llcv::audio_only_view::MinimumClientSize(initialDpi);
        const auto fitted = llcv::audio_only_view::FitClientSize(
            (std::max)(initialClient.cx, static_cast<LONG>(minimum.width)),
            maximumClientWidth, maximumClientHeight);
        initialClient = SIZE{fitted.width, fitted.height};
        g_settings.audioOnlyWidth = initialClient.cx;
        g_settings.audioOnlyHeight = initialClient.cy;
    }
    SIZE outerSize = OuterSizeForClientPixels(
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
        windowExStyle, wc.lpszClassName,
#ifdef LLCV_SPATIAL_PROTOTYPE
        g_spatialInteractivePreview ? L"LLCV · CMAA2 → NIS 0.15 · ON | F9: 비교 전환"
                                   : L"LLCV · Spatial Upscaling Prototype",
#else
        L"LLCV",
#endif
        windowStyle,
        restoreOrigin ? restoredOrigin.x : CW_USEDEFAULT,
        restoreOrigin ? restoredOrigin.y : CW_USEDEFAULT,
        outerSize.cx, outerSize.cy,
        nullptr, nullptr, hInst, nullptr);

    if (!hwnd) {
        CloseSavedLog();
        return 1;
    }
    if (!g_settings.audioOnly) NormalizeWindowSize(hwnd, true,
        g_settings.preferredDisplayMonitor.empty() ? nullptr : initialMonitor);
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
                 g_splitResolutionPlan ? L" (display size)" : g_settings.pixelPerfect
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
    if (!g_settings.audioOnly && (ExclusiveOutputSelected() ||
        (!preserveSmallerRelativeWindow && SelectedResolutionMatchesMonitor(initialMonitor)))) {
        fwprintf(stderr,
                 L"[video] entering fullscreen output.\n");
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
    // Freeze queue width before either audio thread (or its resampler) exists.
    g_ring.ConfigureChannels(g_settings.consoleSurround51 &&
        g_settings.audioMode == AudioMode::WasapiShared ? 6 : 2);
    g_captureAudioRejected.store(false, std::memory_order_release);
    std::thread renderThread(AudioRenderThread);
    std::atomic<bool> smokeCaptureFailed{false};
    g_videoOutputThreadActive.store(!g_settings.audioOnly, std::memory_order_release);
    std::thread unifiedCaptureThread([hwnd, smokeTest, smokeVideoOverride, &smokeCaptureFailed]() {
        struct OutputCompletion {
            HWND hwnd;
            ~OutputCompletion() {
                g_videoOutputThreadActive.store(false, std::memory_order_release);
                if (g_presentationClosing.load(std::memory_order_acquire))
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
        } completion{hwnd};
        const bool initialized = g_settings.audioOnly
            ? AudioOnlyCaptureLoop()
            : UnifiedCaptureRenderLoop(g_videoHost, smokeVideoOverride);
        if (initialized && g_captureGraphCompleted.load(std::memory_order_acquire) && g_running.load()) {
            // EOS is a normal end, not a disconnected device or startup error.
            // Both loops have already released their power request and graph.
            fwprintf(stderr, L"[capture] all input streams completed.\n");
            if (!smokeTest) g_restartToSettings.store(true, std::memory_order_release);
            g_running.store(false);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return;
        }
        if (!initialized) {
            smokeCaptureFailed.store(true, std::memory_order_release);
        }
        if (!initialized && g_running.load()) {
            fwprintf(stderr, g_settings.audioOnly
                         ? L"[capture] audio-only graph stopped.\n"
                         : L"[capture] single capture graph stopped.\n");
            if (!smokeTest) {
                if (g_captureGraphStopped.load(std::memory_order_acquire)) {
                    MessageBoxW(hwnd, IsEnglishUi()
                        ? L"The capture device was disconnected or its input path stopped.\n\nReconnect the device, close other capture applications if necessary, then start again from Settings. LLCV will not repeatedly rescan the device. If this repeats, attach the diagnostic log."
                        : L"캡처 장치 연결이 끊어졌거나 입력 경로가 중단되었습니다.\n\n장치를 다시 연결하고 필요한 경우 다른 캡처 프로그램을 종료한 뒤 설정에서 다시 시작해 주세요. 장치를 반복해서 재검색하지 않습니다. 반복되면 진단 로그를 첨부해 주세요.",
                        L"LLCV", MB_OK | MB_ICONWARNING);
                    g_restartToSettings.store(true, std::memory_order_release);
                } else if (g_captureVideoRejected.load(std::memory_order_acquire)) {
                    MessageBoxW(hwnd, IsEnglishUi()
                        ? L"The capture video format changed or could not be verified. Playback was stopped to avoid interpreting frames with the wrong layout or colors.\n\nCheck the console HDR and capture format settings, then start capture again. Attach the diagnostic log if this repeats."
                        : L"캡처 영상 형식이 변경되었거나 확인할 수 없어 잘못된 화면·색상 출력을 막기 위해 재생을 중단했습니다.\n\n콘솔의 HDR과 캡처 형식 설정을 확인한 뒤 다시 시작해 주세요. 반복되면 진단 로그를 첨부해 주세요.",
                        L"LLCV", MB_OK | MB_ICONERROR);
                    g_restartToSettings.store(true, std::memory_order_release);
                } else if (Surround51Active() && g_captureAudioRejected.load(std::memory_order_acquire)) {
                    MessageBoxW(hwnd, IsEnglishUi()
                        ? L"The capture device did not provide a compatible 48 kHz 5.1/7.1 PCM input.\n\nSet the console to 5.1 LPCM and use a multichannel capture input. If unsupported, turn off Console LPCM 5.1 and select stereo on the console. Dolby/DTS bitstreams are not supported."
                        : L"캡처 장치에서 호환되는 48 kHz 5.1/7.1 PCM 입력을 받지 못했습니다.\n\n콘솔을 5.1 LPCM으로 설정하고 다채널 캡처 입력을 선택하세요. 지원하지 않는 장치라면 콘솔 LPCM 5.1 옵션을 끄고 콘솔도 스테레오로 바꿔 주세요. Dolby/DTS 비트스트림은 지원하지 않습니다.",
                        L"LLCV", MB_OK | MB_ICONERROR);
                    g_restartToSettings.store(true, std::memory_order_release);
                } else if (g_captureAudioRejected.load(std::memory_order_acquire)) {
                    MessageBoxW(hwnd, IsEnglishUi()
                        ? L"The capture audio format changed during playback. Playback was stopped to prevent incorrect audio.\n\nCheck the console and capture device audio settings, then start capture again. Attach the diagnostic log if this repeats."
                        : L"재생 중 캡처 오디오 형식이 변경되어 잘못된 음성 출력을 막기 위해 재생을 중단했습니다.\n\n콘솔과 캡처 장치의 오디오 설정을 확인한 뒤 다시 시작해 주세요. 반복되면 진단 로그를 첨부해 주세요.",
                        L"LLCV", MB_OK | MB_ICONERROR);
                    g_restartToSettings.store(true, std::memory_order_release);
                } else if (g_settings.audioOnly) {
                    const HRESULT failure =
                        g_captureFailureHr.load(std::memory_order_acquire);
                    wchar_t message[512]{};
                    swprintf_s(message,
#ifdef LLCV_AUDIO_INPUT_RECOVERY_DIAGNOSTIC
                               g_audioInputRecoveryExhausted.load(std::memory_order_acquire)
                                   ? (IsEnglishUi()
                                       ? L"No audio input packets were received. Capture was restarted up to 3 times without lasting recovery.\n\nError: 0x%08X  %s\n\nCheck the HDMI signal and capture audio device, close other capture apps, then start again. If this repeats, attach the diagnostic log."
                                       : L"오디오 입력 데이터가 수신되지 않습니다. 캡처 경로를 최대 3회 다시 시작했지만 입력 끊김이 계속됩니다.\n\n오류: 0x%08X  %s\n\nHDMI 신호와 캡처 오디오 장치를 확인하고 다른 캡처 앱을 종료한 뒤 다시 시작해 주세요. 반복되면 진단 로그를 첨부해 주세요.")
                                   :
#endif
                               IsEnglishUi()
                                   ? L"Audio-only capture initialization failed.\n\nError: 0x%08X  %s\n\nSelect a compatible capture audio device or close other capture applications."
                                   : L"오디오 only 캡처 초기화에 실패했습니다.\n\n오류: 0x%08X  %s\n\n호환되는 캡처 오디오 장치를 선택하거나 다른 캡처 프로그램을 종료해 주세요.",
                               static_cast<unsigned int>(failure),
                               HrText(failure).c_str());
                    MessageBoxW(hwnd, message, L"LLCV",
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
                        ? L"Compressed capture initialization failed.\n\n"
                          L"Error: 0x%08X  %s\n\n"
                          L"The selected device exposes a compressed %s stream, but "
                          L"Windows could not provide a compatible Media Foundation "
                          L"decoder or the device's compressed stream was not accepted.\n"
                          L"Close other capture applications, try the device's other "
                          L"compressed format or frame rate, and attach the diagnostic "
                          L"log when reporting the result. Raw NV12/YUY2 remains the "
                          L"recommended lowest-latency path."
                        : L"압축 캡처 초기화에 실패했습니다.\n\n"
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
                if (failure == llcv::video::kUyvyConversionUnsupported) {
                    wcscpy_s(message, IsEnglishUi()
                        ? L"This GPU or driver does not support UYVY/HDYC conversion.\n\nSelect NV12 or YUY2 if the capture device offers it, or update the graphics driver. UYVY requires integer texture reads, a YUY2 write view, and YUY2 video processing."
                        : L"현재 GPU 또는 드라이버에서 UYVY/HDYC 변환을 지원하지 않습니다.\n\n캡처 장치가 제공한다면 NV12 또는 YUY2를 선택하거나 그래픽 드라이버를 업데이트해 주세요. UYVY에는 정수 텍스처 읽기, YUY2 쓰기 뷰와 비디오 처리 지원이 필요합니다.");
                } else if ((failure == DXGI_ERROR_UNSUPPORTED || failure == E_NOINTERFACE ||
                            failure == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) || failure == llcv::video::kRgb24ConversionUnsupported) &&
                           (static_cast<VideoPixelFormat>(g_activePixelFormat.load()) == VideoPixelFormat::Rgb24 ||
                            static_cast<VideoPixelFormat>(g_activePixelFormat.load()) == VideoPixelFormat::Rgb32)) {
                    wcscpy_s(message, IsEnglishUi()
                        ? L"RGB input is unsupported by this GPU/driver or color metadata. RGB24 and XRGB (RGB32) require SDR BT.709/sRGB (or unspecified) primaries and transfer; PQ, HLG and BT.2020 are unsupported. Check the diagnostic log for the rejected capability or metadata."
                        : L"이 GPU/드라이버 또는 색상 메타데이터는 RGB 입력을 지원하지 않습니다. RGB24와 XRGB (RGB32)는 SDR BT.709/sRGB (또는 미지정) 색역과 전달 함수를 사용합니다. PQ, HLG, BT.2020은 지원하지 않습니다. 거부된 기능 또는 메타데이터는 진단 로그를 확인하세요.");
                } else if (failure == DXGI_ERROR_UNSUPPORTED &&
                    static_cast<VideoPixelFormat>(g_activePixelFormat.load()) == VideoPixelFormat::P010) {
                    const wchar_t* detail = g_hdrFailureDetail.load(std::memory_order_acquire);
                    swprintf_s(message, IsEnglishUi()
                        ? L"P010/HDR10 output is not supported by the current input or output path.\n\n%s\n\nP010 is HDR-only; select NV12/YUY2 for SDR input. HDR10 requires PQ / BT.2020 / Limited input and a supported Flip output conversion. HLG, Full-range HDR and Blt HDR are not supported. Enable Windows HDR on the viewing monitor. Attach the diagnostic log to report this issue."
                        : L"현재 입력 형식 또는 출력 경로에서 P010/HDR10을 지원하지 않습니다.\n\n%s\n\nP010은 HDR 전용이며 SDR 입력은 NV12/YUY2를 선택하세요. HDR10은 PQ / BT.2020 / Limited 입력과 변환을 지원하는 Flip 출력이 필요합니다. HLG, Full-range HDR, Blt HDR은 지원하지 않습니다. 표시할 모니터의 Windows HDR을 켜고, 문제가 계속되면 진단 로그를 첨부해 주세요.",
                        detail ? detail : (IsEnglishUi() ? L"Check the HDR conversion stage in the diagnostic log."
                                                        : L"진단 로그의 HDR 변환 단계에서 상세 원인을 확인할 수 있습니다."));
                } else if (failure == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
                    swprintf_s(message, IsEnglishUi()
                        ? L"Capture startup timed out.\n\nError: 0x%08X\n\nCheck that the HDMI source is on and close other capture applications, then retry. The diagnostic log identifies the failed stage and rejected input samples. This does not by itself mean the selected display mode is unsupported."
                        : L"캡처 시작 대기 시간이 초과되었습니다.\n\n오류: 0x%08X\n\nHDMI 입력 기기가 켜져 있는지 확인하고 다른 캡처 앱을 종료한 뒤 다시 시도하세요. 진단 로그에서 실패 단계와 거부된 입력 샘플 수를 확인할 수 있습니다. 이 오류만으로 화면 출력 방식이 미지원이라는 뜻은 아닙니다.",
                        static_cast<unsigned>(failure));
                } else if (failure == HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) {
                    swprintf_s(message, IsEnglishUi()
                        ? L"The capture device returned an invalid or unexpected data format.\n\nError: 0x%08X\n\nReselect the capture mode and retry. Attach the diagnostic log so the selected and connected video layouts can be compared."
                        : L"캡처 장치가 잘못되었거나 예상과 다른 데이터 형식을 반환했습니다.\n\n오류: 0x%08X\n\n캡처 모드를 다시 선택해 시도하세요. 선택 형식과 실제 연결 형식을 비교할 수 있도록 진단 로그를 첨부해 주세요.",
                        static_cast<unsigned>(failure));
                }
                std::wstring captureMessage = message;
                if (failure == llcv::video::kRequestedFrameRateMismatch) {
                    captureMessage = IsEnglishUi()
                        ? L"The driver did not apply the requested capture frame rate. Capture was stopped instead of using a different rate."
                        : L"드라이버가 요청한 캡처 FPS를 적용하지 않았습니다. 다른 FPS로 대신 시작하지 않고 중단했습니다.";
                }
                if (!g_settings.capture40Fps && RequestsExactCaptureRate()) {
                    captureMessage += (IsEnglishUi() ? L"\n\nRequested capture rate: " : L"\n\n요청한 캡처 FPS: ") +
                        std::to_wstring(RequestedVideoFrameRate()) + L" fps.";
                    captureMessage += IsEnglishUi()
                        ? L" A different rate is not used automatically. Select Auto or another supported rate to retry. See the log for the failed startup stage."
                        : L" 다른 FPS로 자동 변경하지 않습니다. 자동 또는 지원되는 다른 FPS로 다시 시도하세요. 실패 단계는 진단 로그에서 확인할 수 있습니다.";
                }
                if (g_settings.capture40Fps) captureMessage += IsEnglishUi()
                    ? L"\n\n40 fps capture is enabled. This device/format may reject 40 fps or report a different rate. Uncheck '40 fps capture' to retry with the normal frame-rate selection. See the diagnostic log to distinguish a format rejection from other startup errors."
                    : L"\n\n40fps 캡처가 켜져 있습니다. 이 장치/형식이 40fps를 거부하거나 다른 FPS를 보고할 수 있습니다. '40fps 캡처'를 해제하면 기존 프레임 선택으로 다시 시도할 수 있습니다. 형식 거부인지 다른 시작 오류인지는 진단 로그에서 확인하세요.";
                MessageBoxW(hwnd, captureMessage.c_str(), L"LLCV",
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

    RunViewerMessageLoop(hwnd);

    g_running.store(false);
    g_screenshots.CancelPending();
    g_viewerHelp.Close();
    g_updateCheckTask.CancelAndWait();

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
    FlushAudioDiagnostics(true);
    CloseSavedLog();
    if (restartToSettings) RelaunchWithSettings();
    if (smokeTest && smokeCaptureFailed.load(std::memory_order_acquire))
        return 1;
    return 0;
}
