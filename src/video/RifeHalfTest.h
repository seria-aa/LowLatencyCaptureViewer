#pragma once
// Private, profile-isolated test build only. No persisted settings keys.
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "video/RifeHistoryPolicy.h"
#include "video/VfgGpuProbe.h"
namespace llcv::rife_test {
inline constexpr int CheckCommand=2087;
inline constexpr int ScaleCommand=2088;
inline constexpr int QualityCommand=2089;
inline constexpr int AutoUiCommand=2090;
constexpr bool ValidAutoUiWidth(int width){return width==0||width==16||width==32;}
constexpr bool ResetAutoUiHistory(int previous,int next,bool explicitReset){
    return explicitReset||previous<0||((previous==0)!=(next==0));
}
inline std::atomic<int> autoUiWidth{0};
inline bool SelectAutoUiWidth(int width){if(!ValidAutoUiWidth(width))return false;autoUiWidth.store(width,std::memory_order_release);return true;}
#ifdef LLCV_RIFE_TRAJECTORY
inline void NextAutoUiWidth(){SelectAutoUiWidth(autoUiWidth.load()?0:16);}
#else
inline void NextAutoUiWidth(){const int width=autoUiWidth.load();SelectAutoUiWidth(width==0?16:width==16?32:0);}
#endif
enum class Scale : int { High=70, Fast=40, ThreeQuarter=75, Half=50, Quarter=25, VfgLow=101, VfgMedium=102 };
constexpr bool IsVfg(Scale s){return s==Scale::VfgLow||s==Scale::VfgMedium;}
// NVIDIA Windows x64 VFG hardware policy: Ada or Blackwell, not Ampere/Hopper.
constexpr bool OfficialVfgArchitecture(int major,int minor){return vfg::OfficialArchitecture(major,minor);}
constexpr bool ConfiguredVfgArchitecture(int major,int minor){
    return vfg::SupportForArchitecture(major,minor)!=vfg::GpuSupport::Unavailable;
}
#if defined(LLCV_RIFE_TRAJECTORY) && defined(LLCV_RIFE_UHD_PROBE)
inline constexpr std::array<Scale,1> Scales{Scale::Half};
constexpr bool ValidScale(Scale s){return s==Scale::Half;}
#elif defined(LLCV_RIFE_OPTIONAL)
inline constexpr std::array<Scale,5> Scales{Scale::Fast,Scale::Half,Scale::High,Scale::VfgLow,Scale::VfgMedium};
constexpr bool ValidScale(Scale s){for(auto preset:Scales)if(preset==s)return true;return false;}
#elif defined(LLCV_RIFE_TRAJECTORY)
inline constexpr std::array<Scale,3> Scales{Scale::Fast,Scale::Half,Scale::High};
constexpr bool ValidScale(Scale s){return s==Scale::Fast||s==Scale::Half||s==Scale::High;}
#else
inline constexpr std::array<Scale,3> Scales{Scale::ThreeQuarter,Scale::Half,Scale::Quarter};
constexpr bool ValidScale(Scale s){return s==Scale::ThreeQuarter||s==Scale::Half||s==Scale::Quarter;}
#endif
constexpr bool SupportedSize(int width,int height){return (width==1920&&height==1080)
#ifdef LLCV_RIFE_UHD_PROBE
    ||(width==3840&&height==2160)
#endif
#ifndef LLCV_RIFE_TRAJECTORY
    ||(width==2560&&height==1440)
#endif
    ;}
constexpr bool SupportsInput(Scale mode,int width,int height){
    if(!ValidScale(mode))return false;
    if(IsVfg(mode))return (width==1920&&height==1080)||(width==2560&&height==1440)||(width==3840&&height==2160);
    return SupportedSize(width,height);
}
constexpr int64_t CompletionDeadlineUs(int64_t arrival,int64_t submitted,int fps,bool quality){return quality?submitted+200000:fps>0?arrival+500000/fps:arrival;}
constexpr size_t ScaleIndex(Scale s){for(size_t i=0;i<Scales.size();++i)if(Scales[i]==s)return i;return 0;}
#ifdef LLCV_RIFE_TRAJECTORY
#ifdef LLCV_RIFE_UHD_PROBE
constexpr const wchar_t* ScaleLabel(Scale){return L"Half (960×540)";}
#elif defined(LLCV_RIFE_OPTIONAL)
constexpr const wchar_t* PresetLabel(Scale s,bool en){switch(s){
case Scale::Fast:return en?L"RIFE · Performance (40%)":L"RIFE · 성능 (40%)";
case Scale::High:return en?L"RIFE · Quality (70%)":L"RIFE · 화질 (70%)";
case Scale::VfgLow:return en?L"NVIDIA VFG · Low":L"NVIDIA VFG · 낮음";
case Scale::VfgMedium:return en?L"NVIDIA VFG · Medium":L"NVIDIA VFG · 중간";
default:return en?L"RIFE · Balanced (50%)":L"RIFE · 균형 (50%)";}}
constexpr const wchar_t* ScaleLabel(Scale s){return PresetLabel(s,true);}
#else
constexpr const wchar_t* ScaleLabel(Scale s){return s==Scale::Fast?L"Fast (40%)":s==Scale::High?L"High (70%)":L"Half (50%)";}
#endif
constexpr const wchar_t* ScaleFile(Scale s,int){return s==Scale::Fast?L"rife-fast-fused.engine":s==Scale::High?L"rife-high-fused.engine":L"rife-half-fused.engine";}
#else
constexpr const wchar_t* ScaleLabel(Scale s){return s==Scale::ThreeQuarter?L"75%":s==Scale::Quarter?L"Quarter (25%)":L"Half (960×540)";}
constexpr const wchar_t* ScaleFile(Scale s,int width){return s==Scale::ThreeQuarter?L"rife-75-fused.engine":s==Scale::Quarter?L"rife-quarter-fused.engine":width==2560?L"rife-half-960x540-fused.engine":L"rife-half-fused.engine";}
#endif
enum class Status { Off, Pending, Active, Late, Unsupported, Failed };
// Per-frame availability is not the user's ON/OFF selection. A dropped input,
// missed deadline or renderer rebuild must not repeatedly announce ON/OFF.
class NotificationTracker {
    bool enabled_=false,ready_=false,pending_=false;
    unsigned misses_=0, recovered_=0;
    Scale scale_=Scale::Half;
    std::optional<Status> problem_;
public:
    std::optional<Status> Observe(bool enabled,Scale scale,Status frame){
        if(!enabled){const bool was=enabled_;*this={};return was?std::optional{Status::Off}:std::nullopt;}
        if(!enabled_||scale_!=scale){*this={};enabled_=true;scale_=scale;}
        if(frame==Status::Active){
            misses_=0;
            if(problem_ && ++recovered_<3) return std::nullopt;
            if(!ready_||problem_){problem_.reset();ready_=true;return Status::Active;}
            return std::nullopt;
        }
        recovered_=0;
        if(frame==Status::Failed||frame==Status::Unsupported){
            misses_=0;
            if(problem_!=frame){problem_=frame;return frame;}
            return std::nullopt;
        }
        if(frame==Status::Late){
            // A missed pair is counted in Tab immediately. Only interrupt playback
            // with a warning after three misses without a generated frame.
            if(misses_<3)++misses_;
            if(misses_==3 && problem_!=frame){problem_=frame;return frame;}
            return std::nullopt;
        }
        if(!ready_&&!pending_&&!problem_){pending_=true;return Status::Pending;}
        return std::nullopt;
    }
};
constexpr const wchar_t* StateLabel(bool enabled,Status state,bool english=true){
    if(!english){
        if(!enabled)return L"꺼짐";
        return state==Status::Active?L"켜짐":state==Status::Late?L"기한 초과 · 원본":
            state==Status::Failed?L"오류 · 원본":state==Status::Unsupported?L"사용 불가":L"준비 중";
    }
    if(!enabled)return L"OFF";
    return state==Status::Active?L"ON":state==Status::Late?L"ON / late, original":
        state==Status::Failed?L"ON / error, original":state==Status::Unsupported?L"ON / unavailable":L"ON / preparing";
}
constexpr const wchar_t* FallbackLabel(const wchar_t* detail, bool english) {
    if (english) return detail;
    const std::wstring_view reason(detail);
    if (reason==L"NVIDIA driver") return L"NVIDIA 드라이버 필요";
    if (reason==L"Unsupported NVIDIA VFG GPU architecture") return L"VFG 미지원 GPU";
    if (reason==L"GPU mismatch") return L"재생 GPU 불일치";
    if (reason==L"GPU / engine mismatch") return L"GPU와 보간 모델 불일치";
    if (reason==L"runtime error") return L"구성품 실행 오류";
    return detail; // File names and identifiers do not need translation.
}
constexpr bool AllowAutomaticChanges(bool smoke,bool explicitAutomation){return smoke&&explicitAutomation;}
inline std::atomic<bool> requested{false};
#ifdef LLCV_RIFE_OPTIONAL
inline std::atomic<bool> componentsEnabled{false};
#endif
inline std::atomic<bool> qualityComparison{false};
#ifdef LLCV_RIFE_OPTIONAL
inline constexpr Scale DefaultScale=Scale::VfgMedium;
#else
inline constexpr Scale DefaultScale=Scale::Half;
#endif
inline std::atomic<Scale> selectedScale{DefaultScale},activeScale{DefaultScale};
inline int HudWidthFor(Scale mode){
#ifdef LLCV_RIFE_OPTIONAL
    if(IsVfg(mode))return 0;
#endif
    return autoUiWidth.load();
}
inline int SelectedHudWidth(){return HudWidthFor(selectedScale.load());}
inline void SetSelectedHudWidth(int width){
#ifdef LLCV_RIFE_OPTIONAL
    if(IsVfg(selectedScale.load()))return;
#endif
    SelectAutoUiWidth(width);
}
inline std::atomic<Status> status{Status::Off};
#if defined(LLCV_FLYHIGH_TEST) || defined(LLCV_RIFE_OPTIONAL)
inline constexpr int ProviderCommand=2091;
#endif
#ifdef LLCV_FLYHIGH_TEST
enum class Provider:int { Rife=0, FlyHigh=1 };
inline std::atomic<Provider> selectedProvider{Provider::Rife};
inline std::atomic<uint64_t> providerEpoch{0};
inline std::atomic<bool> flyHighHud{true};
inline std::atomic<uint64_t> flyHighGenerated{0},flyHighLate{0},flyHighBusy{0};
inline bool SelectProvider(Provider p){
    if(p!=Provider::Rife&&p!=Provider::FlyHigh)return false;
    if(selectedProvider.exchange(p)!=p)++providerEpoch;
    status=requested.load()?Status::Pending:Status::Off;return true;
}
inline void NextProvider(){SelectProvider(selectedProvider.load()==Provider::Rife?Provider::FlyHigh:Provider::Rife);}
inline bool IsFlyHigh(){return selectedProvider.load()==Provider::FlyHigh;}
#endif
inline const wchar_t* MethodLabel(){
#ifdef LLCV_FLYHIGH_TEST
    if(IsFlyHigh())return L"FlyHighFG";
#endif
    return selectedScale.load()==Scale::ThreeQuarter?L"RIFE 75%":selectedScale.load()==Scale::Quarter?L"RIFE Quarter":L"RIFE Half";
}
inline std::atomic<uint64_t> generated{0},late{0},busy{0};
inline std::atomic<uint64_t> intentChanges{0};
inline std::atomic<uint64_t> hudSeedInputs{0},hudPairInputs{0},hudExplicitResets{0};
inline std::atomic<uint64_t> jobs{0},gpuUs{0},maxGpuUs{0};
#ifdef LLCV_RIFE_OPTIONAL
// Labels point to string literals, published only on initialization failure.
inline std::atomic<const wchar_t*> fallbackDetail{L""};
#endif
inline std::array<std::atomic<uint64_t>,Scales.size()> modeGenerated{},modeLate{},modeCancelled{},modeJobs{},modeGpuUs{};
// Session-only automated live verification; never read by the release target.
inline unsigned toggleEvery=0;
inline bool cycleScales=false;
// Explicit diagnostic capture only. Default playback never reads back pixels.
inline std::wstring pairDirectory;
inline std::wstring sequenceDirectory;
inline std::atomic<unsigned> sequencesSaved{0};
void CaptureSequence(const uint8_t* nv12,int width,int height,unsigned stride,int64_t arrivalUs);
inline unsigned pairAfterJobs=0;
inline std::atomic<bool> pairRequested{false};
inline std::atomic<unsigned> pairsSaved{0},pairSaveFailures{0};
inline void Request(bool enabled){
#ifdef LLCV_RIFE_OPTIONAL
    enabled=enabled&&componentsEnabled.load();
#endif
    if(requested.exchange(enabled,std::memory_order_acq_rel)!=enabled)++intentChanges;
    status.store(enabled?Status::Pending:Status::Off,std::memory_order_release);
}
#ifdef LLCV_RIFE_OPTIONAL
inline void EnableComponents(bool enabled){componentsEnabled.store(enabled);Request(enabled);}
#endif
#ifdef LLCV_RIFE_OPTIONAL
inline std::atomic<Scale> lastRife{Scale::Half},lastVfg{Scale::VfgMedium};
#endif
inline bool SelectScale(Scale scale){
    if(!ValidScale(scale))return false;
#ifdef LLCV_RIFE_OPTIONAL
    (IsVfg(scale)?lastVfg:lastRife).store(scale);
#endif
    selectedScale.store(scale,std::memory_order_release);
    status.store(requested.load()?Status::Pending:Status::Off,std::memory_order_release);return true;
}
inline void NextScale(){SelectScale(Scales[(ScaleIndex(selectedScale.load())+1)%Scales.size()]);}
inline void SetQualityComparison(bool enabled){qualityComparison.store(enabled,std::memory_order_release);status.store(requested.load()?Status::Pending:Status::Off);}
class Half {
    struct Impl;
    std::unique_ptr<Impl> impl_;
#ifdef LLCV_FLYHIGH_TEST
    struct FlyHigh;
    std::unique_ptr<FlyHigh> flyHigh_;
    bool flyHighFailed_=false;
#endif
    bool failed_=false;
    Scale failedScale_=Scale::Half;
#ifdef LLCV_INTERPOLATION_RELEASE
    bool failedVfgWithoutVerifiedRuntime_=false;
#endif
public:
    Half();~Half();
    void Reset();
    void Invalidate();
    // Borrowed view valid until Reset/device loss. The owner must consume it
    // on this immediate context before the next Submit (same graphics queue).
    ID3D11VideoProcessorInputView* Submit(ID3D11Device*,ID3D11DeviceContext*,
        ID3D11VideoDevice*,ID3D11VideoContext*,ID3D11VideoProcessor*,
        ID3D11VideoProcessorEnumerator*,ID3D11Texture2D*,int width,int height,int fps,int64_t arrivalUs);
    void ScheduleOriginalSlot(int fps);
    void WaitOriginalSlot(int fps);
};
}
