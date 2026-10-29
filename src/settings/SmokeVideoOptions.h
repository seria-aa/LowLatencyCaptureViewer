#pragma once
#include "AppSettings.h"
#include <string>
#include <vector>

namespace llcv::settings {
// Diagnostic overrides require the explicit bounded smoke-test switch and are
// applied only to the isolated in-memory profile.
inline bool ApplySmokeVideoOptions(const std::vector<std::wstring>& args, AppSettings& settings,
                                   bool* videoOverride = nullptr) {
    if (videoOverride) *videoOverride=false;
    bool smoke=false, any=false, seenFormat=false, seenResolution=false, seenFps=false;
    for (const auto& arg : args) if (arg==L"--smoke-test") smoke=true;
    auto next=settings;
    for (size_t i=0;i<args.size();++i) {
        const auto& arg=args[i];
        if (arg!=L"--smoke-test-video-format" && arg!=L"--smoke-test-resolution" && arg!=L"--smoke-test-fps") continue;
        any=true;
        if (++i>=args.size()) return false;
        const auto& value=args[i];
        if (arg==L"--smoke-test-video-format") {
            if (seenFormat) return false; seenFormat=true;
            if (value==L"rgb24") next.pixelFormat=VideoPixelFormat::Rgb24;
            else if (value==L"rgb32") next.pixelFormat=VideoPixelFormat::Rgb32;
            else if (value==L"nv12") next.pixelFormat=VideoPixelFormat::Nv12;
            else return false;
        } else if (arg==L"--smoke-test-resolution") {
            if (seenResolution) return false; seenResolution=true;
            if (value==L"1280x720") next.capturePreset=VideoPreset::R1280x720;
            else if (value==L"1920x1080") next.capturePreset=VideoPreset::R1920x1080;
            else if (value==L"2560x1440") next.capturePreset=VideoPreset::R2560x1440;
            else if (value==L"3840x2160") next.capturePreset=VideoPreset::R3840x2160;
            else return false;
        } else {
            if (seenFps) return false; seenFps=true;
            if (value.empty() || value.size()>3) return false;
            int fps=0; for (wchar_t ch : value) { if (ch<L'0' || ch>L'9') return false; fps=fps*10+int(ch-L'0'); }
            if (fps<1 || fps>240) return false;
            next.videoFrameRate=fps; next.capture40Fps=false;
        }
    }
    if (any && !smoke) return false;
    if (any) settings=next;
    if (videoOverride) *videoOverride=any;
    return true;
}

// This gate wraps the actual vendor call, so the diagnostic path does not
// query or write the capture device (including an OFF command).
template<class Configure>
inline bool RunHardwareToneMapping(bool smokeVideoOverride, Configure&& configure) {
    if (smokeVideoOverride) return false;
    configure();
    return true;
}
}
