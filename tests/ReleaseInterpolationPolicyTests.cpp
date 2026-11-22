#include "video/RifeHalfTest.h"
#include "video/VfgPreferences.h"
#include "video/VfgInputCache.h"
#include "video/InterpolationMethodPolicy.h"
#include "video/RifeHistoryPolicy.h"
#include "video/PlaybackGpu.h"
#include "ui/SettingsView.h"
#include "ui/SettingsDialogControls.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#ifdef LLCV_RECORDING
#include "recording/RecordingOptions.h"
#include "settings/SettingsStore.h"
#include "settings/TestProfile.h"
#endif

using namespace llcv::rife_test;
static_assert(!CanGeneratePair(false, 100, 120) &&
              !CanGeneratePair(true, 120, 120) &&
              !CanGeneratePair(true, 121, 120) && CanGeneratePair(true, 119, 120));
static_assert(CaptureBoundedOriginalSlot(100000,104000,60)==112333);
static_assert(CaptureBoundedOriginalSlot(100000,109000,60)==115625);
static_assert(CaptureBoundedOriginalSlot(100000,117000,60)==117000);
static_assert(CaptureBoundedOriginalSlot(100000,108000,30)==124666);
static_assert(SharedFenceProgress(0,2)==SharedFenceState::Waiting);
static_assert(SharedFenceProgress(2,2)==SharedFenceState::Ready);
static_assert(SharedFenceProgress(UINT64_MAX,2)==SharedFenceState::DeviceRemoved);
static_assert(!VulkanStartupExpired(1,kVulkanStartupLimitUs));
static_assert(VulkanStartupExpired(1,kVulkanStartupLimitUs+1));
constexpr bool CheckVfgInputReuse() {
    VfgInputCache cache;
    if(!cache.NeedsCopy(0,1)||!cache.NeedsCopy(1,2))return false;
    cache.MarkCopied(0,1);
    cache.MarkCopied(1,2);
    if(cache.NeedsCopy(1,2)||!cache.NeedsCopy(0,3))return false;
    cache.MarkCopied(0,3);
    // Slot 1 was overwritten by a seed frame but was not submitted to CUDA.
    if(!cache.NeedsCopy(1,4)||!cache.NeedsCopy(0,5))return false;
    return VfgInputCache{}.NeedsCopy(0,3)&&cache.NeedsCopy(0,0);
}
static_assert(CheckVfgInputReuse());
static_assert(VfgInputReuseEnabled(false)&&!VfgInputReuseEnabled(true));
constexpr bool CheckInterpolationNotices(Scale scale) {
    NotificationTracker tracker;
    auto frame=[&](Status state){return tracker.Observe(true,scale,state,1);};
    if(frame(Status::Pending)!=Status::Pending||frame(Status::Pending)||
       frame(Status::Active)!=Status::Active||frame(Status::Active))return false;
    // A rebuild can go through Pending again without a new interpolation request.
    if(frame(Status::Pending)||frame(Status::Active))return false;
    for(int streak=0;streak<2;++streak){
        if(frame(Status::Late)||frame(Status::Late)||frame(Status::Late)!=Status::Late||
           frame(Status::Late))return false;
        for(int i=0;i<4;++i)if(frame(Status::Active))return false;
    }
    if(tracker.Observe(false,scale,Status::Off,2)!=Status::Off||
       tracker.Observe(false,scale,Status::Off,2))return false;
    if(tracker.Observe(true,scale,Status::Active,3)!=Status::Active)return false;
    // F7 can go off and on again before the next captured frame.
    return tracker.Observe(true,scale,Status::Active,5)==Status::Active&&
           !tracker.Observe(true,scale,Status::Active,5);
}
constexpr bool CheckInterpolationStartupRecovery(Scale scale,Status problem) {
    NotificationTracker tracker;
    auto frame=[&](Status state){return tracker.Observe(true,scale,state,1);};
    if(frame(Status::Pending)!=Status::Pending||frame(problem)!=problem||frame(problem))return false;
    for(int i=0;i<4;++i)if(frame(Status::Active))return false;
    return frame(problem)==problem;
}
static_assert(CheckInterpolationNotices(Scale::Fast)&&CheckInterpolationNotices(Scale::VfgMedium));
static_assert(CheckInterpolationStartupRecovery(Scale::Fast,Status::Failed)&&
              CheckInterpolationStartupRecovery(Scale::VfgMedium,Status::Unsupported));
using Method = llcv::settings::InterpolationMethod;
using Provider = llcv::interpolation::Provider;
static_assert(llcv::interpolation::ChooseProvider(Method::NvidiaVfg,true,true)==Provider::NvidiaVfg);
static_assert(llcv::interpolation::ChooseProvider(Method::NvidiaVfg,false,true)==Provider::Unavailable);
static_assert(llcv::interpolation::ChooseProvider(Method::RifeVulkan,true,true)==Provider::RifeVulkan);
static_assert(llcv::interpolation::ChooseProvider(Method::RifeVulkan,false,true)==Provider::RifeVulkan);
static_assert(llcv::interpolation::ChooseProvider(Method::RifeVulkan,true,false)==Provider::Unavailable);
using Pixel = llcv::settings::VideoPixelFormat;
static_assert(llcv::interpolation::UseRifeCaptureOverride(Provider::RifeVulkan,true,false,Pixel::Rgb24));
static_assert(!llcv::interpolation::UseRifeCaptureOverride(Provider::RifeVulkan,true,false,Pixel::P010));
static_assert(!llcv::interpolation::UseRifeCaptureOverride(Provider::RifeVulkan,false,false,Pixel::Nv12));
static_assert(!llcv::interpolation::UseRifeCaptureOverride(Provider::RifeVulkan,true,true,Pixel::Nv12));
static_assert(!llcv::interpolation::UseRifeCaptureOverride(Provider::NvidiaVfg,true,false,Pixel::Nv12));
static_assert(!llcv::interpolation::UseRifeCaptureOverride(Provider::Unavailable,true,false,Pixel::Nv12));
static_assert(llcv::interpolation::RifeCaptureRate(-1, 120, false) == 0);
static_assert(llcv::interpolation::RifeCaptureRate(-1, 30, false) == 30);
static_assert(llcv::interpolation::RifeCaptureRate(-1, 60, true) == 40);
static_assert(llcv::interpolation::RifeCaptureRate(0, 30, true) == 0);
static_assert(llcv::interpolation::RifeCaptureRate(24, 120, false) == 24);
static_assert(llcv::interpolation::RifeCaptureRate(60, 30, true) == 60);
static_assert(!llcv::interpolation::RifeCaptureRateSupported(23) &&
              !llcv::interpolation::RifeCaptureRateSupported(61));
#ifdef LLCV_RIFE_VULKAN
static_assert(SupportsVulkanSdrTexture(DXGI_FORMAT_NV12) && SupportsVulkanSdrTexture(DXGI_FORMAT_YUY2) &&
    SupportsVulkanSdrTexture(DXGI_FORMAT_B8G8R8X8_UNORM) && SupportsVulkanSdrTexture(DXGI_FORMAT_R8G8B8A8_UNORM) &&
    !SupportsVulkanSdrTexture(DXGI_FORMAT_P010));
#endif
static_assert(llcv::settings_ui::control_id::IDC_SETTINGS_VFG_DOWNLOAD != CheckCommand &&
              llcv::settings_ui::control_id::IDC_SETTINGS_VFG_DOWNLOAD != ScaleCommand &&
              llcv::settings_ui::control_id::IDC_SETTINGS_VFG_DOWNLOAD != QualityCommand &&
              llcv::settings_ui::control_id::IDC_SETTINGS_VFG_DOWNLOAD != AutoUiCommand);
static_assert(OfficialVfgArchitecture(8,9) && OfficialVfgArchitecture(12,0));
static_assert(!OfficialVfgArchitecture(8,6) && !OfficialVfgArchitecture(8,0) &&
              !OfficialVfgArchitecture(9,0) && !OfficialVfgArchitecture(7,5));
#ifdef LLCV_VFG_AMPERE_COMPAT
static_assert(ConfiguredVfgArchitecture(8,6));
#else
static_assert(!ConfiguredVfgArchitecture(8,6));
#endif
static_assert(!ConfiguredVfgArchitecture(8,0) && !ConfiguredVfgArchitecture(7,5));
static_assert(llcv::vfg::SupportForArchitecture(7,5)==llcv::vfg::GpuSupport::Unavailable);
static_assert(llcv::vfg::SupportForArchitecture(8,9)==llcv::vfg::GpuSupport::Official);
static_assert(std::wstring_view(StateLabel(true,Status::Late,false))==L"기한 초과 · 원본");
static_assert(std::wstring_view(FallbackLabel(L"runtime error",false))==L"구성품 실행 오류");
static_assert(SupportsInput(Scale::VfgMedium,3840,2160) &&
              SupportsInput(Scale::VfgMedium,2560,1440) &&
              SupportsInput(Scale::VfgMedium,1920,1080));
static_assert(SupportsInput(Scale::Half,1920,1080) && !SupportsInput(Scale::Half,2560,1440));

#ifdef LLCV_RECORDING
static bool CheckUpgradeProfile() {
    namespace keys = llcv::shortcuts;
    namespace settings = llcv::settings;
    namespace recording = llcv::recording;
    if (!llcv::test_profile::Enabled()) return false;
    const auto profile = llcv::test_profile::Directory() + L"\\settings.ini";
    const auto recorder = recording::ProfilePath();
    // Use only fields written by v2.2.1; in particular it never had a VFG key.
    // F7 was available to bind to a different action and must stay there.
    const auto put = [](const std::wstring& file, const wchar_t* section,
                        const wchar_t* key, const wchar_t* value) {
        return WritePrivateProfileStringW(section, key, value, file.c_str()) != FALSE;
    };
    if (!put(profile, L"General", L"StartupSettingsVersion", L"v2.2.1") ||
        !put(profile, L"General", L"SkipStartupSettings", L"1") ||
        !put(profile, L"Video", L"PixelFormat", L"NV12") ||
        !put(profile, L"Video", L"FrameRate", L"60") ||
        !put(profile, L"Video", L"VsrEnabled", L"1") ||
        !put(profile, L"Shortcuts", L"Recording", L"345") || // Ctrl+Y
        !put(profile, L"Shortcuts", L"Screenshot", L"118") || // F7
        !put(recorder, L"Recording", L"MaxFps", L"30") ||
        !put(recorder, L"Recording", L"MaxHeight", L"1080") ||
        !put(recorder, L"Recording", L"AudioVolumePercent", L"42")) return false;

    const auto matches = [&] {
        const auto loaded = settings::LoadFromIni(profile).settings;
        const auto rec = recording::LoadOptions(recorder);
        const auto shortcuts = [&](keys::Action action) {
            return loaded.shortcuts[static_cast<size_t>(action)];
        };
        return loaded.skipStartupSettings && loaded.vsrEnabled &&
               loaded.pixelFormat == settings::VideoPixelFormat::Nv12 &&
               loaded.videoFrameRate == 60 &&
               shortcuts(keys::Action::Recording) == (keys::Ctrl | 'Y') &&
               shortcuts(keys::Action::Screenshot) == VK_F7 &&
               shortcuts(keys::Action::FrameInterpolation) == (keys::Ctrl | VK_F13) &&
               rec.maxFps == 30 && rec.maxHeight == 1080 && rec.audioVolumePercent == 42;
    };
    return matches() && settings::StartupReviewRequired(profile, L"v2.3.0") &&
           settings::MarkStartupSettingsShown(profile, L"v2.3.0") &&
           !settings::StartupReviewRequired(profile, L"v2.3.0") && matches();
}

static bool CheckVfgPreferences() {
    using namespace llcv;
    if (!test_profile::Enabled()) return false;
    const auto file = test_profile::Directory() + L"\\vfg-preferences.ini";
    auto saved = settings::LoadFromIni(file).settings;
    if (saved.vfgEnabled || saved.vfgLowQuality || saved.vfgWaitForFrame ||
        saved.interpolationMethod!=Method::RifeVulkan || saved.rifeHudMode!=2) return false;
    const auto legacy=test_profile::Directory()+L"\\legacy-vfg.ini";
    if(!WritePrivateProfileStringW(L"Video",L"VfgEnabled",L"1",legacy.c_str())||
       !WritePrivateProfileStringW(L"Video",L"VfgPlaybackEnabled",L"0",legacy.c_str()))return false;
    auto legacySettings=settings::LoadFromIni(legacy).settings;
    if(legacySettings.interpolationMethod!=Method::NvidiaVfg||legacySettings.vfgPlaybackEnabled||
       legacySettings.rifeHudMode!=2)return false;
    if(!WritePrivateProfileStringW(L"Video",L"InterpolationMethod",L"0",legacy.c_str()))return false;
    const auto previousAuto=settings::LoadFromIni(legacy).settings;
    if(previousAuto.interpolationMethod!=Method::RifeVulkan||!previousAuto.vfgEnabled||
       previousAuto.vfgPlaybackEnabled)return false;
    legacySettings.interpolationMethod=Method::RifeVulkan;
    legacySettings.rifeHudMode=2;
    if(!settings::SaveToIni(legacy,legacySettings))return false;
    legacySettings=settings::LoadFromIni(legacy).settings;
    if(legacySettings.interpolationMethod!=Method::RifeVulkan||legacySettings.rifeHudMode!=2||
       legacySettings.vfgPlaybackEnabled)return false;
    for(const int mode:{0,1}) {
        legacySettings.rifeHudMode=mode;
        if(!settings::SaveToIni(legacy,legacySettings)||settings::LoadFromIni(legacy).settings.rifeHudMode!=mode)
            return false;
    }
    EnableComponents(true);
    SelectScale(Scale::VfgLow);
    SetQualityComparison(true);
    vfg::AcceptPreferences(saved, true, true);
    if (!settings::SaveToIni(file, saved)) return false;
    if (!WritePrivateProfileStringW(L"Unrelated", L"Keep", L"7", file.c_str())) return false;
    if (!settings::SaveVfgPlayback(file, false) ||
        GetPrivateProfileIntW(L"Unrelated", L"Keep", 0, file.c_str()) != 7) return false;
    saved = settings::LoadFromIni(file).settings;
    vfg::RestorePreferences(saved, true);
    if (!componentsEnabled.load() || requested.load()) return false;
    vfg::AcceptPreferences(saved, true, false); // F2, then Start without editing the checkbox.
    if (saved.vfgPlaybackEnabled || !settings::SaveToIni(file, saved)) return false;
    if (!settings::SaveVfgPlayback(file, true)) return false;
    saved = settings::LoadFromIni(file).settings;
    vfg::RestorePreferences(saved, false);
    if (componentsEnabled.load() || requested.load()) return false;
    vfg::AcceptPreferences(saved, false, false); // Unavailable GPU must not erase opt-in.
    vfg::RestorePreferences(saved, true);
    if (!componentsEnabled.load() || !requested.load() ||
        selectedScale.load() != Scale::VfgLow || !qualityComparison.load()) return false;
    EnableComponents(false);
    vfg::AcceptPreferences(saved, true, true);
    if (!settings::SaveToIni(file, saved)) return false;
    vfg::RestorePreferences(settings::LoadFromIni(file).settings, true);
    return !componentsEnabled.load() && !requested.load() &&
        selectedScale.load() == Scale::VfgLow && qualityComparison.load();
}
#endif

static int CheckPlaybackGpuHardware() {
    using namespace llcv::video_gpu;
    const auto inventory = Enumerate();
    for (const auto& candidate : inventory) {
        DXGI_ADAPTER_DESC1 desc{};
        candidate.device->GetDesc1(&desc);
        std::wprintf(L"Inventory: %ls (%ls); LUID=%08lx:%08lx; outputs=%d; selectable=%d\n",
            candidate.name.c_str(), candidate.id.c_str(), static_cast<unsigned long>(desc.AdapterLuid.HighPart),
            desc.AdapterLuid.LowPart,
            OwnsMonitor(candidate.device.Get(), MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY)),
            Find(inventory, candidate.id) != nullptr);
    }
    const auto adapter = Automatic();
    if (!adapter) return 77;
    if (inventory.empty()) return 4; // A hardware auto choice must not leave only Auto in the UI.
    for (const auto& entry : inventory)
        if (!Find(inventory, entry.id)) return 5;
    DXGI_ADAPTER_DESC1 selected{};
    if (FAILED(adapter->GetDesc1(&selected)) || (selected.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return 1;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);
    if (FAILED(hr)) return 77; // Hardware/remote session does not permit D3D11 video.
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> actual;
    DXGI_ADAPTER_DESC used{};
    if (FAILED(device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&actual)) || FAILED(actual->GetDesc(&used)) ||
        std::memcmp(&selected.AdapterLuid, &used.AdapterLuid, sizeof(LUID)) != 0) return 2;
    const auto primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    const auto exclusive = Automatic(primary);
    if (exclusive && !OwnsMonitor(exclusive.Get(), primary)) return 3;
    std::wprintf(L"Automatic adapter: %ls; D3D11 opened that adapter.\n", selected.Description);
    return 0;
}

int main(int argc, char** argv){
    if (argc == 2 && std::strcmp(argv[1], "--playback-gpu") == 0) return CheckPlaybackGpuHardware();
    std::vector<llcv::video_gpu::Adapter> adapters{{L"intel", L"Integrated", {}}, {L"rtx", L"Discrete", {}}};
    if (llcv::video_gpu::Find(adapters, L"rtx") != &adapters[1] ||
        llcv::video_gpu::Find(adapters, L"") || llcv::video_gpu::Find(adapters, L"removed")) return 13;
    adapters.push_back(adapters[1]);
    if (llcv::video_gpu::Find(adapters, L"rtx")) return 14;
    if (componentsEnabled.load() || requested.load()) return 1;
    Request(true);
    if (requested.load()) return 2;
    if (!SelectScale(Scale::VfgMedium) || requested.load()) return 3;
    EnableComponents(true);
    if (!requested.load() || selectedScale.load()!=Scale::VfgMedium) return 4;
    const auto onIntent=intentChanges.load();
    Request(false);
    Request(true);
    if (!requested.load() || intentChanges.load()!=onIntent+2) return 15;
    EnableComponents(false);
    if (requested.load()) return 5;
    llcv::settings_ui::SettingsControls controls;
    if (controls.InterpolationGpuAvailable()) return 6; // Unknown or unsupported architecture.
    controls.vfgGpuSupported=true;
    if (!controls.InterpolationGpuAvailable()) return 6;
    controls.vsrGpuUnavailable=true;
    if (controls.InterpolationGpuAvailable()) return 7;
    controls.vsrGpuUnavailable=false;
    controls.vfgRtx30=true;
    controls.vfgGpuSupported=ConfiguredVfgArchitecture(8,6);
    if (controls.InterpolationGpuAvailable()!=ConfiguredVfgArchitecture(8,6)) return 8;
    if (controls.ShowShortcut(static_cast<size_t>(llcv::shortcuts::Action::HudCorrection))) return 9;
    for(auto index:llcv::viewer_help::kDisplayOrder)
        if(index==static_cast<size_t>(llcv::shortcuts::Action::HudCorrection)) return 10;
#ifdef LLCV_RECORDING
    if (!CheckUpgradeProfile()) {
        std::fputs("v2.2.1 profile failed v2.3.0 startup/shortcut/recording compatibility.\n", stderr);
        return 11;
    }
    if (!CheckVfgPreferences()) return 12;
#endif
    std::puts("VFG-only UI policy, GPU support, shortcuts and interpolation opt-in passed.");
}
