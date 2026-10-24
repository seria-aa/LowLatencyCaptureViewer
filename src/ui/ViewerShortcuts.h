#pragma once
#include <windows.h>
#include <array>
#include <string>
#include "input/Shortcuts.h"

namespace llcv::viewer_help {
inline constexpr UINT kHelpKey = VK_F1;
inline constexpr UINT kScreenshotKey = VK_F12;
inline bool IsScreenshotRequest(const MSG& message, bool audioOnly,
                                const shortcuts::Bindings& bindings, unsigned modifiers) {
    return shortcuts::KeyDown(message.message) &&
        shortcuts::Match(bindings,shortcuts::Action::Screenshot,static_cast<UINT>(message.wParam),modifiers) &&
        !(message.lParam & (LPARAM{1}<<30)) && !audioOnly;
}
inline bool IsScreenshotRequest(const MSG& message, bool audioOnly,
                                const shortcuts::Bindings& bindings=shortcuts::Defaults) {
    return IsScreenshotRequest(message, audioOnly, bindings,
        shortcuts::KeyDown(message.message) ? shortcuts::Modifiers() : 0);
}
struct Shortcut { const wchar_t* key; const wchar_t* korean; const wchar_t* english; bool videoOnly; };
inline constexpr std::array<Shortcut,shortcuts::Count> kShortcuts{{
    {L"F1",L"앱 정보 · 단축키",L"App information & shortcuts",false},
    {L"F2",L"설정 열기",L"Open settings",false},
    {L"F3",L"오디오 OSD",L"Audio OSD",true},
    {L"F5",L"Pixel-perfect 크기 복원",L"Restore Pixel-perfect size",true},
    {L"F6",L"업스케일링 켜기/끄기",L"Toggle selected upscaling",true},
    {L"F11",L"전체화면",L"Fullscreen",false},
    {L"F12",L"스크린샷 저장",L"Save screenshot",true},
    {L"Tab",L"실시간 진단",L"Live diagnostics",false},
    {L"Esc",L"전체화면 해제 / 종료",L"Leave fullscreen / exit",false}
#ifdef LLCV_RECORDING
    ,{L"F9",L"녹화 시작 / 종료",L"Start / stop recording",true}
#endif
    ,{L"M",L"재생 음소거 켜기/끄기",L"Toggle playback mute",false}
}};
inline const wchar_t* VideoOnlyHint(bool english) {
#ifdef LLCV_RECORDING
    return english ? L"F3, F5, F6, F9 and F12 are available in video mode."
                   : L"F3 · F5 · F6 · F9 · F12는 영상 모드에서 사용합니다.";
#else
    return english ? L"F3, F5, F6 and F12 are available in video mode."
                   : L"F3 · F5 · F6 · F12는 영상 모드에서 사용합니다.";
#endif
}
inline const wchar_t* Shortcuts(bool english, bool alignedColumns = false) {
    static const auto text=[] {
        std::array<std::wstring,4> result;
        for (int aligned=0;aligned<2;++aligned) for (int language=0;language<2;++language) for (const auto& shortcut:kShortcuts) {
            auto& value=result[aligned*2+language];
            if (!value.empty()) value+=L"\r\n";
            value+=shortcut.key; value+=aligned ? L"\t" : L"  "; value+=language ? shortcut.english : shortcut.korean;
            if (shortcut.videoOnly) value+=language ? L" (video mode)" : L" (영상 모드)";
        }
        return result;
    }();
    return text[(alignedColumns ? 2 : 0)+(english ? 1 : 0)].c_str();
}
inline std::wstring KeyLabel(size_t index,const shortcuts::Bindings& bindings) {
    return index<shortcuts::Count ? shortcuts::Label(bindings[index]) : L"Esc";
}
} // namespace llcv::viewer_help
