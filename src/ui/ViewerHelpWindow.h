#pragma once
#include "ViewerShortcuts.h"
#include "AppPalette.h"
#include <array>
#include <string>
#include <vector>

namespace llcv::viewer_help {
enum class MessageContext { Viewer, Settings };
// UI-thread-only modeless owned window. No renderer/capture/settings ownership.
class Window {
public:
    ~Window();
    Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    void Toggle(HWND owner, bool english, const wchar_t* version, int show = SW_SHOWNORMAL,
                bool lightTheme = false);
    // Call before viewer shortcut/wheel routing. Dismissal consumes key repeat.
    bool ProcessMessage(MSG& message, HWND owner, bool english, const wchar_t* version,
                        bool lightTheme = false, MessageContext context = MessageContext::Viewer) {
        return ProcessMessage(message, owner, english, version, lightTheme,
            shortcuts::KeyDown(message.message) ? shortcuts::Modifiers() : 0, context);
    }
    bool ProcessMessage(MSG& message, HWND owner, bool english, const wchar_t* version,
                        bool lightTheme, unsigned modifiers, MessageContext context = MessageContext::Viewer);
    void SetLightTheme(bool lightTheme);
    void SetBindings(const shortcuts::Bindings& bindings);
    HWND Handle() const { return window_; }
    void Close();
private:
    friend struct WindowTestAccess;
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK BodyProc(HWND, UINT, WPARAM, LPARAM);
    void Layout(UINT windowDpi = 0);
    void ApplyFont(UINT dpi);
    void FitToMonitor(const RECT* suggested = nullptr);
    void FitToWorkArea(const RECT& work, const RECT* suggested = nullptr);
    SIZE MinimumTrackSize(const RECT& work) const;
    bool CreateContent();
    bool UpdateTheme();
    void Paint(HDC dc, bool body);
    enum Font { Normal, Strong, Title, Small, FontCount };
    enum Surface { Background, Card, Key, SurfaceCount };
    struct Field { HWND hwnd; Font font; Surface surface; bool muted; RECT rect{}; };
    HWND window_ = nullptr, owner_ = nullptr, body_ = nullptr, close_ = nullptr, footer_ = nullptr;
    std::vector<Field> fields_;
    std::array<HFONT, FontCount> fonts_{};
    std::array<HBRUSH, SurfaceCount> brushes_{};
    std::array<RECT, 3> cards_{};
    std::vector<RECT> keys_;
    std::wstring version_;
    shortcuts::Bindings bindings_=shortcuts::Defaults;
    bool layingOut_ = false, highContrast_ = false;
    bool english_ = false;
    UINT dismissKey_ = 0;
    bool lightTheme_ = false;
    bool fitting_ = false;
    RECT fittingWork_{};
    UINT layoutDpi_ = 96, fontDpi_ = 0;
    ui::Palette palette_;
};
} // namespace llcv::viewer_help
