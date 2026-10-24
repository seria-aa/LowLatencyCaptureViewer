#pragma once
#include <windows.h>
#include <array>
#include <string>
#include <cwctype>
#include <cwchar>

namespace llcv::shortcuts {
// Keep the legacy Vsr action/key name so existing remapped F6 bindings survive.
// Its user-facing action now toggles the selected upscaling method.
enum class Action : size_t { Help, Settings, AudioOsd, PixelPerfect, Vsr, Fullscreen, Screenshot, Diagnostics, Exit,
#ifdef LLCV_RECORDING
    Recording,
#endif
    Mute, Count };
inline constexpr size_t Count = static_cast<size_t>(Action::Count);
inline constexpr unsigned Ctrl = 0x100, Alt = 0x200, Shift = 0x400;
using Bindings = std::array<unsigned, Count>;
inline constexpr Bindings Defaults{VK_F1,VK_F2,VK_F3,VK_F5,VK_F6,VK_F11,VK_F12,VK_TAB,VK_ESCAPE
#ifdef LLCV_RECORDING
    ,VK_F9
#endif
    ,'M'
};
inline constexpr std::array<const wchar_t*, Count> Names{L"Help",L"Settings",L"AudioOsd",L"PixelPerfect",L"Vsr",L"Fullscreen",L"Screenshot",L"Diagnostics",L"Exit"
#ifdef LLCV_RECORDING
    ,L"Recording"
#endif
    ,L"Mute"
};
inline constexpr std::array<const wchar_t*, Count> DefaultLabels{L"F1",L"F2",L"F3",L"F5",L"F6",L"F11",L"F12",L"Tab",L"Esc"
#ifdef LLCV_RECORDING
    ,L"F9"
#endif
    ,L"M"
};
#ifdef LLCV_RECORDING
// Preserve legacy custom bindings when the new action has no saved key yet.
inline void AddRecordingDefault(Bindings& bindings) {
    constexpr auto index=static_cast<size_t>(Action::Recording);
    for (unsigned key=VK_F9;key<=VK_F24;++key) {
        bool used=false;
        for (size_t i=0;i<Count;++i) if (i!=index && bindings[i]==key) used=true;
        if (!used) { bindings[index]=key; return; }
    }
}
#endif
inline void AddMuteDefault(Bindings& bindings) {
    constexpr auto index=static_cast<size_t>(Action::Mute);
    const auto available=[&](unsigned key) {
        for (size_t i=0;i<Count;++i) if (i!=index && bindings[i]==key) return false;
        return true;
    };
    if (available('M')) { bindings[index]='M'; return; }
    for (unsigned key=VK_F1;key<=VK_F24;++key)
        if (key!=VK_F4 && available(key)) { bindings[index]=key; return; }
}
inline bool Modifier(UINT key) {
    return key==VK_CONTROL || key==VK_LCONTROL || key==VK_RCONTROL || key==VK_MENU ||
        key==VK_LMENU || key==VK_RMENU || key==VK_SHIFT || key==VK_LSHIFT || key==VK_RSHIFT;
}
inline bool Valid(unsigned binding) {
    if (binding & ~0x7ffu) return false;
    const unsigned key=binding & 0xff, mods=binding & 0x700;
    const bool keyAllowed=(key>='A' && key<='Z') || (key>='0' && key<='9') ||
        (key>=VK_F1 && key<=VK_F24) || key==VK_TAB || key==VK_SPACE || key==VK_ESCAPE ||
        key==VK_RETURN || key==VK_BACK || (key>=VK_PRIOR && key<=VK_DOWN) ||
        key==VK_INSERT || key==VK_DELETE || (key>=VK_NUMPAD0 && key<=VK_DIVIDE);
    if (!keyAllowed) return false; // Empty, Windows and modifier-only bindings stay reserved.
    if ((mods & Alt) && (key==VK_F4 || key==VK_SPACE || key==VK_TAB || key==VK_RETURN || key==VK_ESCAPE)) return false;
    if ((mods & Ctrl) && key==VK_ESCAPE) return false; // Windows Start menu.
    if ((mods & (Ctrl|Alt))==(Ctrl|Alt) && key==VK_DELETE) return false;
    return true;
}
inline bool Valid(const Bindings& bindings) {
    for (size_t i=0;i<Count;++i) {
        if (!Valid(bindings[i])) return false;
        for (size_t j=0;j<i;++j) if (bindings[i]==bindings[j]) return false;
    }
    return true;
}
inline unsigned Modifiers() {
    if (GetKeyState(VK_LWIN)<0 || GetKeyState(VK_RWIN)<0) return 0x800; // never intercept OS shortcuts
    return (GetKeyState(VK_CONTROL)<0 ? Ctrl : 0) | (GetKeyState(VK_MENU)<0 ? Alt : 0) |
        (GetKeyState(VK_SHIFT)<0 ? Shift : 0);
}
inline bool KeyDown(UINT message) { return message==WM_KEYDOWN || message==WM_SYSKEYDOWN; }
// Settings fields own navigation, text/editing chords and the native F4
// dropdown. Viewer actions may use their function-key bindings there; the
// viewer itself continues to accept every valid customized binding.
inline bool AllowedInSettings(UINT key) {
    return key>=VK_F1 && key<=VK_F24 && key!=VK_F4;
}
inline bool Match(const Bindings& bindings,Action action,UINT key,unsigned modifiers) {
    return key<256 && bindings[static_cast<size_t>(action)]==(key|modifiers);
}
inline bool Match(const Bindings& bindings,Action action,const MSG& message) {
    return KeyDown(message.message) && Match(bindings,action,static_cast<UINT>(message.wParam),Modifiers());
}
inline std::wstring Label(unsigned binding) {
    std::wstring label;
    if (binding & Ctrl) label+=L"Ctrl+";
    if (binding & Alt) label+=L"Alt+";
    if (binding & Shift) label+=L"Shift+";
    const unsigned key=binding & 0xff;
    if (key>=VK_F1 && key<=VK_F24) return label+L"F"+std::to_wstring(key-VK_F1+1);
    if ((key>='A' && key<='Z') || (key>='0' && key<='9')) return label+wchar_t(key);
    switch (key) {
    case VK_TAB: return label+L"Tab"; case VK_SPACE: return label+L"Space"; case VK_ESCAPE: return label+L"Esc";
    case VK_RETURN: return label+L"Enter"; case VK_BACK: return label+L"Bksp";
    case VK_PRIOR: return label+L"PgUp"; case VK_NEXT: return label+L"PgDn";
    case VK_HOME: return label+L"Home"; case VK_END: return label+L"End";
    case VK_LEFT: return label+L"Left"; case VK_RIGHT: return label+L"Right";
    case VK_UP: return label+L"Up"; case VK_DOWN: return label+L"Down";
    case VK_INSERT: return label+L"Ins"; case VK_DELETE: return label+L"Del";
    default: break;
    }
    if (key>=VK_NUMPAD0 && key<=VK_NUMPAD9) return label+L"Num"+std::to_wstring(key-VK_NUMPAD0);
    const wchar_t* names[]{L"Num*",L"Num+",L"Num,",L"Num-",L"Num.",L"Num/"};
    if (key>=VK_MULTIPLY && key<=VK_DIVIDE) return label+names[key-VK_MULTIPLY];
    return label+L"?";
}
// Expand only tokens from the source, never rescan replacements (bindings can be swapped).
inline std::wstring Expand(const wchar_t* source,const Bindings& bindings) {
    std::wstring result;
    for (size_t pos=0;source[pos];) {
        bool replaced=false;
        for (size_t i=0;i<Count;++i) {
            const size_t n=wcslen(DefaultLabels[i]);
            if ((!pos || !iswalnum(source[pos-1])) && wcsncmp(source+pos,DefaultLabels[i],n)==0 &&
                !(source[pos+n]>=L'0' && source[pos+n]<=L'9') &&
                !(source[pos+n]>=L'A' && source[pos+n]<=L'Z') && !(source[pos+n]>=L'a' && source[pos+n]<=L'z')) {
                result+=Label(bindings[i]); pos+=n; replaced=true; break;
            }
        }
        if (!replaced) result+=source[pos++];
    }
    return result;
}
} // namespace llcv::shortcuts
