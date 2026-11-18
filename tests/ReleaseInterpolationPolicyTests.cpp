#include "video/RifeHalfTest.h"
#include "ui/SettingsView.h"
#include "ui/SettingsDialogControls.h"
#include <cstdio>
#ifdef LLCV_RECORDING
#include "recording/RecordingOptions.h"
#include "settings/SettingsStore.h"
#include "settings/TestProfile.h"
#endif

using namespace llcv::rife_test;
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
#endif

int main(){
    if (componentsEnabled.load() || requested.load()) return 1;
    Request(true);
    if (requested.load()) return 2;
    if (!SelectScale(Scale::VfgMedium) || requested.load()) return 3;
    EnableComponents(true);
    if (!requested.load() || selectedScale.load()!=Scale::VfgMedium) return 4;
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
#endif
    std::puts("VFG-only UI policy, GPU support, shortcuts and interpolation opt-in passed.");
}
