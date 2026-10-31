#include "ui/SettingsTheme.h"
#include "ui/SettingsView.h"
#include "ui/SettingsDialogControls.h"
#include "ui/UiText.h"
#include "capture/DirectShowDevices.h"
#ifdef LLCV_RECORDING
#include "recording/RecordingSettingsView.h"
#endif
#include <commctrl.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <string>
#include <tlhelp32.h>
#include <vector>

using namespace llcv::settings_ui;
namespace {
bool traceCycle = false;
bool traceResources = false;
struct WindowDiagnostics {
    std::map<std::wstring, unsigned> classes;
    unsigned windows = 0;
};
BOOL CALLBACK CountDiagnosticWindow(HWND hwnd, LPARAM parameter) {
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process != GetCurrentProcessId()) return TRUE;
    auto& diagnostic = *reinterpret_cast<WindowDiagnostics*>(parameter);
    wchar_t className[256]{};
    GetClassNameW(hwnd, className, ARRAYSIZE(className));
    ++diagnostic.classes[className];
    ++diagnostic.windows;
    return TRUE;
}
BOOL CALLBACK CountDiagnosticTopWindow(HWND hwnd, LPARAM parameter) {
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process == GetCurrentProcessId()) {
        CountDiagnosticWindow(hwnd, parameter);
        EnumChildWindows(hwnd, CountDiagnosticWindow, parameter);
    }
    return TRUE;
}
void PrintResourceDiagnostics(const char* phase, int cycle) {
    // Opt-in only: enumerating native subsystem windows can change the timing
    // of their deferred initialization. Do not alter the normal test baseline.
    if (!traceResources) return;
    WindowDiagnostics diagnostic;
    EnumWindows(CountDiagnosticTopWindow, reinterpret_cast<LPARAM>(&diagnostic));
    for (HWND hwnd = FindWindowExW(HWND_MESSAGE, nullptr, nullptr, nullptr); hwnd;
         hwnd = FindWindowExW(HWND_MESSAGE, hwnd, nullptr, nullptr)) {
        CountDiagnosticWindow(hwnd, reinterpret_cast<LPARAM>(&diagnostic));
    }
    unsigned threads = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{sizeof(entry)};
        if (Thread32First(snapshot, &entry)) do {
            if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++threads;
        } while (Thread32Next(snapshot, &entry));
        CloseHandle(snapshot);
    }
    std::printf("Resource diagnostic %s %d: GDI=%lu USER=%lu windows=%u threads=%u",
        phase, cycle, GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
        GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS), diagnostic.windows, threads);
    for (const auto& item : diagnostic.classes)
        std::printf(" [%ls=%u]", item.first.c_str(), item.second);
    std::putchar('\n');
}
void DrainNativeMessages() {
    // Hidden-window fixtures still produce native focus/accessibility and
    // mouse-tracking work. Service queued work between fixtures as a real UI
    // loop would, without sleeping or waiting for new messages indefinitely.
    MSG message{};
    for (int i = 0; i < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
        if (message.message == WM_QUIT) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
void Check(bool value, const char* text) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", text); std::exit(1); }
}
struct TestWindow {
    SettingsControls controls;
    SettingsTheme theme;
    int clicks = 0;
    bool preview = false;
};
void RefreshPreview(TestWindow& state) {
    auto& c = state.controls;
    SettingsVisualUpdate visualUpdate(GetParent(c.tabControl));
    const LRESULT selected = SendMessageW(c.pixelFormatCombo, CB_GETCURSEL, 0, 0);
    const auto format = static_cast<llcv::settings::VideoPixelFormat>(SendMessageW(c.pixelFormatCombo, CB_GETITEMDATA, selected, 0));
    UpdateAdvancedControlVisibility(&c, SendMessageW(c.audioCombo, CB_GETCURSEL, 0, 0) == 1, format);
    SetSettingsControlVisible(c.captureAudioDeviceCombo, c.activeTab == SettingsTab::VideoWindow);
    SetSettingsControlVisible(c.captureAudioStatus, false);
    // Match production: repaint once after all visibility changes, without
    // rebuilding the theme/subclasses for ordinary selections.
    RedrawWindow(GetParent(c.tabControl), nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<TestWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<TestWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state) {
        const bool wasOpen=state->controls.frameOptionsOpen;
        const bool handled=HandleFrameOptionsMessage(&state->controls,hwnd,message,wParam,lParam);
        if (wasOpen!=state->controls.frameOptionsOpen) RefreshPreview(*state);
        if (handled) return 0;
        LRESULT result = 0;
        if (state->theme.HandleMessage(message, wParam, lParam, result)) return result;
        if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED) ++state->clicks;
        if (state->preview) {
            auto& c = state->controls;
            if (message == WM_COMMAND) {
                const HWND control = reinterpret_cast<HWND>(lParam);
#ifdef LLCV_RECORDING
                if (RecordingSettingsCommand(&c, hwnd, LOWORD(wParam), HIWORD(wParam))) {
                    RefreshPreview(*state); return 0;
                }
#endif
                if (control == c.tabControl && HIWORD(wParam) == LBN_SELCHANGE) {
                    c.activeTab = SettingsTabFromNavigationIndex(static_cast<int>(SendMessageW(c.tabControl, LB_GETCURSEL, 0, 0)));
                    RefreshPreview(*state); return 0;
                }
                if (control && (HIWORD(wParam) == BN_CLICKED || HIWORD(wParam) == CBN_SELCHANGE)) {
                    if (control == c.startButton || control == c.cancelButton) { DestroyWindow(hwnd); return 0; }
                    if (control == c.vsrGuideButton && HIWORD(wParam) == BN_CLICKED) {
                        MessageBoxW(hwnd, llcv::ui_text::VsrSetupGuide(c.english),
                            c.english ? L"NVIDIA VSR — Setup guide" : L"NVIDIA VSR — 설정 안내",
                            MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    RefreshPreview(*state); return 0;
                }
                if (LOWORD(wParam) == IDCANCEL && !control) { DestroyWindow(hwnd); return 0; }
            }
            if (message == WM_DPICHANGED) {
                SettingsVisualUpdate visualUpdate(hwnd);
                const UINT dpi = HIWORD(wParam);
                ApplySettingsFont(&c, hwnd, dpi); LayoutSettingsControls(&c, dpi); state->theme.RefreshControls(dpi);
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left, suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
            if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
HWND Child(HWND owner, const wchar_t* kind, const wchar_t* text, DWORD style,
           int x, int y, int width, int height, int id) {
    HWND child = CreateWindowW(kind, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, owner, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    Check(child != nullptr, "create native child");
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
    return child;
}
void SaveSnapshot(HWND hwnd, const wchar_t* path, HWND comboRows = nullptr) {
    RECT client{}; GetClientRect(hwnd, &client);
    const int rowHeight = comboRows ? static_cast<int>(SendMessageW(comboRows, CB_GETITEMHEIGHT, 0, 0)) : 0;
    const int rows = comboRows ? std::min(6, static_cast<int>(SendMessageW(comboRows, CB_GETCOUNT, 0, 0))) : 0;
    if (comboRows) { client.right = MulDiv(rowHeight, 560, 24); client.bottom = rows * rowHeight; }
    const int width = client.right, height = client.bottom;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    Check(bitmap != nullptr && pixels, "snapshot bitmap");
    HGDIOBJ old = SelectObject(dc, bitmap);
    if (comboRows) {
        for (int i = 0; i < rows; ++i) {
            DRAWITEMSTRUCT item{ODT_COMBOBOX, static_cast<UINT>(GetDlgCtrlID(comboRows)),
                static_cast<UINT>(i), ODA_DRAWENTIRE, i == 0 ? ODS_SELECTED : 0u, comboRows, dc,
                {0, i * rowHeight, width, (i + 1) * rowHeight}, 0};
            SendMessageW(hwnd, WM_DRAWITEM, item.CtlID, reinterpret_cast<LPARAM>(&item));
        }
    } else {
      SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
      for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT r{}; GetWindowRect(child, &r);
        MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<POINT*>(&r), 2);
        int saved = SaveDC(dc);
        SetViewportOrgEx(dc, r.left, r.top, nullptr);
        IntersectClipRect(dc, 0, 0, r.right - r.left, r.bottom - r.top);
        SendMessageW(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        RestoreDC(dc, saved);
      }
    }
    // GDI may batch writes to a DIB section; finish them before the CPU/file
    // reader consumes pixels, otherwise a screenshot can show stale glyphs.
    GdiFlush();
    if (path) {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        const DWORD bytes = width * height * 4;
        header.bfSize = header.bfOffBits + bytes;
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(file != INVALID_HANDLE_VALUE, "open snapshot");
        DWORD written = 0;
        Check(WriteFile(file, &header, sizeof(header), &written, nullptr) && written == sizeof(header), "snapshot header");
        Check(WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr) && written == sizeof(info.bmiHeader), "snapshot info");
        Check(WriteFile(file, pixels, bytes, &written, nullptr) && written == bytes, "snapshot pixels");
        CloseHandle(file);
    }
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
}
void CheckPaintContracts(TestWindow& state) {
    HDC dc = CreateCompatibleDC(nullptr);
    Check(dc != nullptr, "create paint-contract DC");
    const auto backgroundFor = [&](HWND child) {
        LRESULT brush = 0;
        Check(state.theme.HandleMessage(WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc),
            reinterpret_cast<LPARAM>(child), brush), "theme supplies static colors");
        LOGBRUSH description{};
        Check(GetObjectW(reinterpret_cast<HBRUSH>(brush), sizeof(description), &description) == static_cast<int>(sizeof(description)),
            "static paint uses a valid reusable brush");
        Check(GetBkColor(dc) == description.lbColor, "static brush and DC background agree");
        return description.lbColor;
    };
    const COLORREF page = backgroundFor(state.controls.pageTitle);
    const COLORREF group = backgroundFor(state.controls.vsrStatus);
    Check(backgroundFor(state.controls.screenshotClipboardCheck) == page,
        "screenshot checkbox blends into the uncluttered video page");
    if (!state.theme.HighContrast()) {
        Check(page == group, "upscaler details share the existing field background without an extra card");
        backgroundFor(state.controls.screenshotClipboardCheck);
        const COLORREF enabledText = GetTextColor(dc);
        backgroundFor(state.controls.pixelCheck);
        Check(GetTextColor(dc) != enabledText, "disabled text is visually distinct");
        const bool detectedEnabled = IsWindowEnabled(state.controls.videoCapabilityStatus) != FALSE;
        const bool requestEnabled = IsWindowEnabled(state.controls.unlistedFpsStatus) != FALSE;
        EnableWindow(state.controls.videoCapabilityStatus, TRUE);
        EnableWindow(state.controls.unlistedFpsStatus, TRUE);
        backgroundFor(state.controls.videoCapabilityStatus);
        const COLORREF detectedText = GetTextColor(dc);
        backgroundFor(state.controls.unlistedFpsStatus);
        Check(GetTextColor(dc) != detectedText,
            "direct-request hint uses a distinct accent from detected capabilities");
        EnableWindow(state.controls.videoCapabilityStatus, detectedEnabled);
        EnableWindow(state.controls.unlistedFpsStatus, requestEnabled);
    }
    Check((SendMessageW(state.controls.screenshotClipboardCheck, WM_GETDLGCODE, VK_SPACE, 0) & DLGC_BUTTON) != 0,
        "checkbox keeps native dialog keyboard semantics");
    Check((SendMessageW(state.controls.videoCombo, WM_GETDLGCODE, VK_DOWN, 0) & DLGC_WANTARROWS) != 0,
        "combo keeps native arrow-key navigation");
    LRESULT result = 0;
    Check(!state.theme.HandleMessage(WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
        reinterpret_cast<LPARAM>(state.controls.startButton), result), "theme never consumes command notifications");
    Check(!state.theme.HandleMessage(WM_KEYDOWN, VK_TAB, 0, result), "theme never consumes dialog navigation");
    DeleteDC(dc);
}
void OneCycle(const wchar_t* snapshot = nullptr) {
    static int diagnosticCycle = 0;
    ++diagnosticCycle;
    ULONGLONG previous = GetTickCount64();
    const auto trace = [&](const char* stage) {
        const ULONGLONG now = GetTickCount64();
        if (traceCycle) std::printf("Theme cycle %-25s %llu ms\n", stage, now - previous);
        previous = now;
    };
    TestWindow state;
    HWND hwnd = CreateWindowW(L"LLCV.SettingsTheme.Tests", L"Settings theme regression",
        WS_OVERLAPPED | WS_CLIPCHILDREN, 0, 0, 950, 650, nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    Check(hwnd != nullptr, "create hidden test window");
    auto& c = state.controls;
    c.brandLabel = Child(hwnd, L"STATIC", L"LLCV", SS_LEFT, 16, 25, 130, 28, 10);
    c.pageTitle = Child(hwnd, L"STATIC", L"Video settings", SS_LEFT, 184, 25, 500, 28, 11);
    c.pageSubtitle = Child(hwnd, L"STATIC", L"Capture and display, with a clean native interface.", SS_LEFT, 184, 60, 640, 22, 12);
    c.tabControl = Child(hwnd, L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | WS_TABSTOP,
        12, 100, 132, 240, 13);
    for (auto label : { L"Video", L"Audio", L"Window", L"Shortcuts", L"App" })
        SendMessageW(c.tabControl, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    SendMessageW(c.tabControl, LB_SETCURSEL, 0, 0);
    c.videoCaptureSection = Child(hwnd, L"STATIC", L"Capture", SS_LEFT, 184, 110, 310, 24, 14);
    c.videoCombo = Child(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 184, 146, 342, 180, 15);
    for (auto label : { L"1920 x 1080", L"2560 x 1440", L"3840 x 2160" })
        SendMessageW(c.videoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    c.screenshotClipboardCheck = Child(hwnd, L"BUTTON", L"NVIDIA RTX Video Super Resolution", BS_AUTOCHECKBOX | WS_TABSTOP,
        184, 207, 500, 30, 16);
    c.pixelCheck = Child(hwnd, L"BUTTON", L"Disabled option", BS_AUTOCHECKBOX | WS_TABSTOP,
        184, 252, 400, 30, 17);
    c.vsrStatus = Child(hwnd, L"STATIC", L"Experimental. Actual NVIDIA enhancement cannot be verified here.", SS_LEFT,
        184, 310, 650, 28, 18);
    c.videoCapabilityStatus = Child(hwnd, L"STATIC", L"Detected: 60 fps", SS_LEFT,
        184, 350, 376, 20, 19);
    c.unlistedFpsStatus = Child(hwnd, L"STATIC", L"Direct request: 30 fps", SS_LEFT,
        184, 374, 376, 20, 20);
    c.startButton = Child(hwnd, L"BUTTON", L"Start", BS_DEFPUSHBUTTON | WS_TABSTOP, 698, 608, 110, 32, IDOK);
    c.cancelButton = Child(hwnd, L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 820, 608, 106, 32, IDCANCEL);
    c.themeCombo = Child(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
        600, 164, 300, 120, 2053);
    for (auto label : {L"Dark", L"Light"})
        SendMessageW(c.themeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    SendMessageW(c.themeCombo, CB_SETCURSEL, diagnosticCycle % 2, 0);
    trace("create controls");
    Check(state.theme.Attach(hwnd, &c), "attach theme");
    state.theme.RefreshControls(); // repeated attach must not duplicate subclass state
    trace("attach + refresh");
    SendMessageW(c.videoCombo, CB_SETCURSEL, 1, 0);
    Check(SendMessageW(c.videoCombo, CB_GETCURSEL, 0, 0) == 1, "combo retains native selection");
    wchar_t selected[64]{}; GetWindowTextW(c.videoCombo, selected, 64);
    Check(std::wcscmp(selected, L"2560 x 1440") == 0, "combo retains native accessibility text");
    Check(SendMessageW(c.screenshotClipboardCheck, BM_GETCHECK, 0, 0) == BST_UNCHECKED, "native checkbox starts unchecked");
    trace("combo select/read + check");
    SendMessageW(c.screenshotClipboardCheck, BM_CLICK, 0, 0);
    Check(SendMessageW(c.screenshotClipboardCheck, BM_GETCHECK, 0, 0) == BST_CHECKED, "click toggles native checkbox");
    Check(state.clicks == 1, "native click notification delivered once");
    trace("selection + first click");
    SendMessageW(c.screenshotClipboardCheck, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageW(c.screenshotClipboardCheck, WM_KEYUP, VK_SPACE, 0);
    Check(SendMessageW(c.screenshotClipboardCheck, BM_GETCHECK, 0, 0) == BST_UNCHECKED, "space toggles native checkbox");
    SendMessageW(c.screenshotClipboardCheck, BM_SETCHECK, BST_CHECKED, 0);
    EnableWindow(c.pixelCheck, FALSE);
    Check(!IsWindowEnabled(c.pixelCheck), "disabled behavior retained");
    CheckPaintContracts(state);
    for (int selection : {1, 0, 1, 0}) {
        SendMessageW(c.themeCombo, CB_SETCURSEL, selection, 0);
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(2053, CBN_SELCHANGE),
            reinterpret_cast<LPARAM>(c.themeCombo));
        CheckPaintContracts(state);
        Check(SendMessageW(c.videoCombo, CB_GETCURSEL, 0, 0) == 1,
            "switching theme preserves other selections");
        if (!state.theme.HighContrast()) {
            HDC dc = CreateCompatibleDC(nullptr);
            LRESULT brush = 0;
            state.theme.HandleMessage(WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc),
                reinterpret_cast<LPARAM>(c.pageTitle), brush);
            Check(selection ? GetBkColor(dc) == RGB(247,247,247) : GetRValue(GetBkColor(dc)) < 50,
                "theme choice changes the actual paint palette");
            Check(selection ? GetTextColor(dc) == RGB(49,64,82) : GetRValue(GetTextColor(dc)) > 200,
                "theme choice retains contrasting title text");
            DeleteDC(dc);
        }
    }
    SendMessageW(c.startButton, BM_CLICK, 0, 0);
    Check(state.clicks == 3, "push button notification delivered");
    trace("space + push click");
    SendMessageW(c.screenshotClipboardCheck, WM_MOUSEMOVE, 0, MAKELPARAM(4, 4));
    SendMessageW(c.screenshotClipboardCheck, WM_MOUSELEAVE, 0, 0);
    SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
    SendMessageW(hwnd, WM_SYSCOLORCHANGE, 0, 0);
    trace("hover + theme updates");
    Check(SendMessageW(c.videoCombo, CB_GETCURSEL, 0, 0) == 1, "theme refresh preserves selected format");
    SaveSnapshot(hwnd, snapshot);
    trace("render snapshot");
    // Explicit detach while controls still exist and reattach are both supported.
    state.theme.Detach();
    Check(state.theme.Attach(hwnd, &c), "reattach theme");
    trace("detach + reattach");
    DestroyWindow(c.pixelCheck); c.pixelCheck = nullptr;
    state.theme.RefreshControls();
    DestroyWindow(hwnd); // parent NCDESTROY detaches after children clean subclasses
    PrintResourceDiagnostics("before-drain", diagnosticCycle);
    DrainNativeMessages();
    PrintResourceDiagnostics("after-drain", diagnosticCycle);
    trace("destroy");
}
void AddComboItem(HWND combo, const wchar_t* value) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}
void CreateActualContents(TestWindow& state, HWND hwnd, bool english, UINT dpi, bool asioAvailable = false) {
    using namespace llcv::settings;
    const VideoPresetInfo presets[] = {
        {VideoPreset::R1280x720, 1280, 720, 60, L"1280 x 720"},
        {VideoPreset::R1920x1080, 1920, 1080, 120, L"1920 x 1080"},
        {VideoPreset::R2560x1440, 2560, 1440, 120, L"2560 x 1440"},
        {VideoPreset::R3840x2160, 3840, 2160, 60, L"3840 x 2160"}};
    const llcv::capture::DeviceInfo devices[] = {{L"sample-capture", L"AVerMedia HD Capture GC573 1"}};
    const llcv::display::MonitorChoice monitors[] = {{nullptr, L"sample-display", L"DISPLAY1 · 3840 x 2160"}};
    const int pcm[] = {10, 15, 20, 25, 30};
        AppSettings settings;
        settings.pixelPerfect = false;
        settings.vsrEnabled = true;
        settings.allowVolumeBoost = true;
        settings.consoleSurround51 = true;
        settings.captureDeviceId = L"sample-capture";
        const SettingsControlInitialValues initial{settings, english, asioAvailable, L"UI Preview",
            VideoPreset::R1920x1080, devices, devices, presets, pcm, monitors};
        SettingsControlPopulation population{
            &state.controls,
            [](void* pointer) { AddComboItem(static_cast<SettingsControls*>(pointer)->audioOutputCombo, L"Speakers (USB Audio Device)"); },
            [](void* pointer) { AddComboItem(static_cast<SettingsControls*>(pointer)->bufferCombo, L"20 ms"); },
            [](void* pointer) {
                auto& c = *static_cast<SettingsControls*>(pointer);
                AddComboItem(c.pixelFormatCombo, L"NV12 (8-bit · 4:2:0)");
                SendMessageW(c.pixelFormatCombo, CB_SETITEMDATA, 0, static_cast<LPARAM>(VideoPixelFormat::Nv12));
                SendMessageW(c.pixelFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"P010 10-bit HDR10"));
                SendMessageW(c.pixelFormatCombo, CB_SETITEMDATA, 1, static_cast<LPARAM>(VideoPixelFormat::P010));
                SendMessageW(c.pixelFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MJPEG"));
                SendMessageW(c.pixelFormatCombo, CB_SETITEMDATA, 2, static_cast<LPARAM>(VideoPixelFormat::Mjpeg));
                AddComboItem(c.frameRateCombo, L"60 fps");
            }};
        CreateSettingsDialogControls(&state.controls, hwnd, GetModuleHandleW(nullptr), initial, population);
        auto& c = state.controls;
#ifdef LLCV_RECORDING
        // Synthetic capabilities only: visual QA must not probe capture/encode hardware.
        c.recordingOptions.folder = L"C:\\Videos\\LLCV";
        CreateRecordingSettings(&c, hwnd);
        c.recordingProbePending = false;
        c.recordingCapabilities[0] = {true,true,{{L"Hardware encoder (preview)",L"fixture"}},{true,true},{true,true}};
        c.recordingCapabilities[1] = {true,false,{{L"HEVC encoder (preview)",L"fixture"}},{true,true},{}};
        RefreshRecordingSettings(&c);
#endif
        SetWindowTextW(c.videoCapabilityStatus, english ? L"Detected formats\nNV12  144 / 120 / 60 fps\nYUY2  60 fps\nP010  60 fps" :
            L"자동 인식\nNV12  144 / 120 / 60 fps\nYUY2  60 fps\nP010  60 fps");
        // VSR help is the production factory copy, not a fabricated successful
        // hardware probe. The preview caption identifies the fixture globally.
        SetWindowTextW(c.audioStatus, english ? L"WASAPI Shared · 48 kHz / stereo" : L"WASAPI Shared · 48 kHz / 스테레오");
        ApplySettingsFont(&c, hwnd, dpi);
        LayoutSettingsControls(&c, dpi);
        Check(state.theme.Attach(hwnd, &c), "attach actual theme");
        state.theme.RefreshControls(dpi);
}
void CheckActualTabOrder() {
    using llcv::settings::VideoPixelFormat;
    unsigned profiles = 0;
    for (bool english : {false, true}) for (UINT dpi : {96u, 120u, 144u, 192u}) {
        TestWindow state;
        HWND hwnd = CreateWindowW(L"LLCV.SettingsTheme.Tests", L"Native tab-order regression",
            WS_POPUP | WS_CLIPCHILDREN, 0, 0, SettingsPixels(kSettingsClientWidthDip, dpi), SettingsPixels(650, dpi),
            nullptr, nullptr, GetModuleHandleW(nullptr), &state);
        Check(hwnd != nullptr, "create native tab-order fixture");
        CreateActualContents(state, hwnd, english, dpi);
        auto& c = state.controls;
        const auto verify = [&](SettingsTab tab, bool exclusive, VideoPixelFormat format,
                                bool pixelPerfect, bool externalCaptureAudio) {
            c.activeTab = tab;
            SendMessageW(c.tabControl, LB_SETCURSEL, SettingsNavigationIndex(tab), 0);
            SendMessageW(c.audioCombo, CB_SETCURSEL, exclusive ? 1 : 0, 0);
            SendMessageW(c.pixelCheck, BM_SETCHECK, pixelPerfect ? BST_CHECKED : BST_UNCHECKED, 0);
            // A later page reflow/DPI application must preserve factory order.
            LayoutSettingsControls(&c, dpi);
            UpdateAdvancedControlVisibility(&c, exclusive, format);
            SetSettingsControlVisible(c.captureAudioDeviceCombo,
                tab == SettingsTab::VideoWindow && externalCaptureAudio);
            SetSettingsControlVisible(c.captureAudioStatus,
                tab == SettingsTab::VideoWindow && !externalCaptureAudio);

            std::vector<HWND> expected{c.tabControl};
            switch (tab) {
            case SettingsTab::VideoWindow:
                expected.push_back(c.videoRefreshButton);
                expected.push_back(c.captureDeviceCombo);
                if (externalCaptureAudio) expected.push_back(c.captureAudioDeviceCombo);
                expected.insert(expected.end(), {c.videoCombo, c.frameRateCombo, c.frameOptionsButton, c.pixelFormatCombo});
                if (format == VideoPixelFormat::P010)
                    expected.insert(expected.end(), {c.forceHdr10Check, c.forceHdr10Help,
                        c.hdrChromaCombo, c.hdrChromaHelp});
                if (format != VideoPixelFormat::P010)
                    expected.insert(expected.end(), {c.sdrRangeCombo, c.sdrRangeHelp});
                if (format == VideoPixelFormat::Mjpeg)
                    expected.insert(expected.end(), {c.mjpegColorCombo, c.mjpegColorHelp});
                expected.insert(expected.end(), {c.presentationCombo, c.presentationHelp,
                    c.displayMonitorCombo, c.pixelCheck});
                if (!pixelPerfect) expected.push_back(c.scalingCombo); // fixture uses equal capture/display sizes
                expected.insert(expected.end(), {c.displayResolutionCombo, c.upscalerCombo, c.vsrGuideButton,
                    c.screenshotClipboardCheck, c.screenshotFolderButton});
                break;
            case SettingsTab::Audio:
                expected.insert(expected.end(), {c.audioCombo, c.audioOutputCombo, c.bufferCombo});
                if (exclusive) expected.push_back(c.exclusiveTestButton);
                expected.insert(expected.end(), {c.volumeHudCombo, c.volumeBoostCheck,
                    c.volumeBoostHelp, c.muteBackgroundCheck, c.audioOnlyCheck});
                if (!exclusive) expected.push_back(c.surround51Check);
                expected.insert(expected.end(), {c.driftCombo, c.driftHelp, c.pcmQueueCombo, c.pcmQueueHelp});
                break;
            case SettingsTab::Window:
                expected.insert(expected.end(), {c.relativeSizeCheck, c.borderlessCheck,
                    c.roundedCornersCheck, c.windowSnapCheck, c.fullscreenCursorCombo, c.fullscreenPositionCombo});
                break;
            case SettingsTab::GuideDiagnostics:
                expected.push_back(c.shortcutResetButton);
                expected.insert(expected.end(), c.guideKeys.begin(), c.guideKeys.end());
                expected.insert(expected.end(), {c.saveLogCheck, c.showConsoleCheck, c.guideLogFolderButton, c.guideReportButton, c.guideLsButton});
                break;
            case SettingsTab::Updates:
                expected.insert(expected.end(), {c.languageCombo, c.themeCombo, c.skipStartupCheck,
                    c.preventSleepCheck, c.checkForUpdatesCheck, c.updateNowButton});
                break;
            }
            expected.insert(expected.end(), {c.startButton, c.cancelButton});
            Check(GetNextDlgTabItem(hwnd, nullptr, FALSE) == expected.front(),
                "native traversal starts at sidebar");
            for (size_t i = 0; i < expected.size(); ++i) {
                const HWND current = expected[i];
                Check((GetWindowLongPtrW(current, GWL_STYLE) & (WS_VISIBLE | WS_TABSTOP)) ==
                    (WS_VISIBLE | WS_TABSTOP) && IsWindowEnabled(current),
                    "expected tab stop is visible and enabled");
                const HWND next = GetNextDlgTabItem(hwnd, current, FALSE);
                const HWND previous = GetNextDlgTabItem(hwnd, current, TRUE);
                if (next != expected[(i + 1) % expected.size()] ||
                    previous != expected[(i + expected.size() - 1) % expected.size()]) {
                    std::fprintf(stderr, "Native tab-order mismatch: page=%d dpi=%u english=%d stop=%d next=%d previous=%d\n",
                        static_cast<int>(tab), dpi, english, GetDlgCtrlID(current),
                        GetDlgCtrlID(next), GetDlgCtrlID(previous));
                    Check(false, "native forward/reverse traversal matches visual groups and wraps");
                }
            }
            Check(GetWindow(c.captureDeviceLabel, GW_HWNDNEXT) == c.captureDeviceCombo &&
                GetWindow(c.frameRateLabel, GW_HWNDNEXT) == c.frameRateCombo,
                "native labels stay adjacent to their fields");
            const HWND labelledFields[][3] = {
                {c.presentationLabel, c.presentationCombo, c.presentationHelp},
                {c.driftLabel, c.driftCombo, c.driftHelp},
                {c.pcmQueueLabel, c.pcmQueueCombo, c.pcmQueueHelp},
                {c.hdrChromaLabel, c.hdrChromaCombo, c.hdrChromaHelp},
                {c.mjpegColorLabel, c.mjpegColorCombo, c.mjpegColorHelp},
                {c.sdrRangeLabel, c.sdrRangeCombo, c.sdrRangeHelp},
            };
            for (const auto& group : labelledFields) {
                Check(GetWindow(group[1], GW_HWNDPREV) == group[0] &&
                    GetWindow(group[1], GW_HWNDNEXT) == group[2],
                    "native accessible label directly precedes its combo; help follows");
            }
            Check(GetWindow(c.fullscreenCursorLabel, GW_HWNDNEXT) == c.fullscreenCursorCombo,
                "cursor label stays adjacent after the hint is brought forward");
            bool sawHint = false;
            for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                if (child == c.fullscreenCursorHint) sawHint = true;
                if (child == c.fullscreenCursorCombo) {
                    Check(sawHint, "F11 hint stays above the cursor combo in Z order");
                    break;
                }
            }
            ++profiles;
        };
        for (auto format : {VideoPixelFormat::Nv12, VideoPixelFormat::P010, VideoPixelFormat::Mjpeg})
            for (bool pixelPerfect : {false, true}) for (bool externalAudio : {false, true})
                verify(SettingsTab::VideoWindow, false, format, pixelPerfect, externalAudio);
        for (bool exclusive : {false, true})
            verify(SettingsTab::Audio, exclusive, VideoPixelFormat::Nv12, false, false);
        for (auto tab : {SettingsTab::Window, SettingsTab::GuideDiagnostics, SettingsTab::Updates})
            verify(tab, false, VideoPixelFormat::Nv12, false, false);
        DestroyWindow(hwnd);
        for (HFONT font : c.uiFonts) DeleteObject(font);
        DrainNativeMessages();
    }
    std::printf("Native tab order: %u bilingual/DPI/page profiles, forward/reverse/wrap and label groups passed.\n", profiles);
}
LRESULT CALLBACK CountPaints(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                             UINT_PTR, DWORD_PTR count) {
    if (message == WM_PAINT) ++*reinterpret_cast<unsigned*>(count);
    return DefSubclassProc(hwnd, message, wParam, lParam);
}
void CheckDeferredTransitions() {
    TestWindow state;
    HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE, L"LLCV.SettingsTheme.Tests",
        L"Offscreen transition regression", WS_POPUP | WS_CLIPCHILDREN,
        -20000, -20000, 1000, 650, nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    Check(hwnd != nullptr, "create transition window");
    CreateActualContents(state, hwnd, false, 96);
    auto& c = state.controls;
    c.activeTab = SettingsTab::VideoWindow;
    RefreshPreview(state);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    unsigned childPaints = 0;
    Check(SetWindowSubclass(c.screenshotClipboardCheck, CountPaints, 72,
          reinterpret_cast<DWORD_PTR>(&childPaints)) != FALSE, "track button painting");
    for (int i = 0; i < 50; ++i)
        SendMessageW(c.screenshotClipboardCheck, BM_SETCHECK, i % 2 ? BST_CHECKED : BST_UNCHECKED, 0);
    Check(childPaints == 0, "nested state notifications never force intermediate paints");
    UpdateWindow(c.screenshotClipboardCheck);
    Check(childPaints == 1, "repeated state changes coalesce into one buffered paint");
    for (int i = 0; i < 50; ++i) {
        {
            SettingsVisualUpdate outer(hwnd);
            Check(!IsWindowVisible(hwnd), "page update suppresses intermediate redraw");
            {
                SettingsVisualUpdate nested(hwnd);
                c.activeTab = i % 3 == 0 ? SettingsTab::GuideDiagnostics :
                    i % 3 == 1 ? SettingsTab::VideoWindow : SettingsTab::Audio;
                RefreshPreview(state);
                for (size_t row = 0; row < c.guideKeys.size(); ++row) {
                    for (HWND control : {c.guideKeys[row], c.guideDescriptions[row]}) {
                        const bool guideVisible = c.activeTab == SettingsTab::GuideDiagnostics;
                        Check(((GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0) == guideVisible,
                              "shortcut rows are visible only on the guide page");
                        Check((IsWindowEnabled(control) != FALSE) == guideVisible,
                              "all shortcut rows follow page visibility");
                    }
                }
            }
            Check(!IsWindowVisible(hwnd), "nested update does not prematurely restore redraw");
        }
        Check(IsWindowVisible(hwnd), "completed page update restores the visible owner");
        Check(GetUpdateRect(hwnd, nullptr, FALSE), "completed page update queues its final paint");
        Check(SendMessageW(c.screenshotClipboardCheck, BM_GETCHECK, 0, 0) == BST_CHECKED,
              "page transitions preserve selection");
        UpdateWindow(hwnd);
    }
    ShowWindow(hwnd, SW_HIDE);
    { SettingsVisualUpdate hidden(hwnd); RefreshPreview(state); }
    Check(!IsWindowVisible(hwnd), "updating a hidden dialog never reveals it");
    RemoveWindowSubclass(c.screenshotClipboardCheck, CountPaints, 72);
    DestroyWindow(hwnd);
    for (HFONT font : c.uiFonts) DeleteObject(font);
    DrainNativeMessages();
    std::puts("Transitions: 50 page changes, nested/hidden redraw scopes and 50-to-1 queued button paints passed.");
}
void CheckBrandPixels(HWND label, UINT dpi) {
    wchar_t text[32]{}; GetWindowTextW(label, text, ARRAYSIZE(text));
    Check(std::wcscmp(text, L"LLCV") == 0, "logo retains accessible native name");
    Check(!(GetWindowLongPtrW(label, GWL_STYLE) & WS_TABSTOP), "logo is not an interactive control");
    RECT rect{}; GetClientRect(label, &rect);
    Check(rect.right == SettingsPixels(132, dpi) && rect.bottom == SettingsPixels(40, dpi),
        "logo geometry scales with DPI");
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right; info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(nullptr); void* bytes = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bytes, nullptr, 0);
    Check(dc && bitmap && bytes, "allocate logo test surface");
    const auto old = SelectObject(dc, bitmap);
    SendMessageW(label, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    const DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int paint = 0; paint < 20; ++paint)
        SendMessageW(label, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    GdiFlush();
    Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == gdi, "logo repeated painting releases pens bitmap DC and font");
    const auto* pixels = static_cast<const DWORD*>(bytes);
    unsigned coral = 0, wordmark = 0;
    int leftInk = rect.right, rightInk = -1;
    const DWORD background = pixels[0] & 0xffffff;
    for (int y = 0; y < rect.bottom; ++y) for (int x = 0; x < rect.right; ++x) {
        const DWORD pixel = pixels[y * rect.right + x] & 0xffffff;
        const unsigned red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
        if (red > 220 && green > 70 && green < 160 && blue < 130) ++coral;
        if (pixel != background) {
            leftInk = std::min(leftInk, x); rightInk = std::max(rightInk, x);
            if (x >= SettingsPixels(64, dpi)) ++wordmark;
        }
    }
    Check(coral > 0 && wordmark > 0, "logo contains icon signal and visible wordmark");
    Check(std::abs(leftInk - (rect.right - 1 - rightInk)) <= SettingsPixels(3, dpi),
        "visible logo group is centered, not just its control rectangle");
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
}

void CheckDisabledLabelPixels() {
    unsigned testedLabels = 0;
    for (bool light : {false, true}) for (bool english : {false, true}) for (UINT dpi : {96u, 120u, 144u, 192u}) {
        TestWindow state;
        HWND hwnd = CreateWindowW(L"LLCV.SettingsTheme.Tests", L"Disabled text regression",
            WS_POPUP | WS_CLIPCHILDREN, 0, 0, SettingsPixels(1000, dpi), SettingsPixels(650, dpi),
            nullptr, nullptr, GetModuleHandleW(nullptr), &state);
        Check(hwnd != nullptr, "create disabled text fixture");
        CreateActualContents(state, hwnd, english, dpi, true);
        auto& c = state.controls;
        SendMessageW(c.themeCombo, CB_SETCURSEL, light ? 1 : 0, 0);
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(2053, CBN_SELCHANGE), reinterpret_cast<LPARAM>(c.themeCombo));
        SendMessageW(c.pixelCheck, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(c.upscalerCombo, CB_SETCURSEL, 0, 0);
        c.activeTab = SettingsTab::VideoWindow;
        RefreshPreview(state);
        Check(!IsWindowEnabled(c.scalingLabel) && !IsWindowEnabled(c.scalingCombo),
            "pixel-perfect still disables scaling");
        Check(IsWindowEnabled(c.displayResolutionLabel) && IsWindowEnabled(c.displayResolutionCombo),
            "VSR off keeps display selection available");
        if (!state.theme.HighContrast()) CheckBrandPixels(c.brandLabel, dpi);
        // Audit every actual STATIC, not just currently visible disabled
        // labels. This catches missing text styles on other pages/modes.
        std::vector<HWND> labels;
        for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t className[32]{}; GetClassNameW(child, className, ARRAYSIZE(className));
            if (std::wcscmp(className, L"Static") != 0 && _wcsicmp(className, L"STATIC") != 0) continue;
            labels.push_back(child);
        }
        Check(labels.size() >= 40, "audit covers every settings page's text controls");
        for (HWND label : labels) {
            const bool wasEnabled = IsWindowEnabled(label) != FALSE;
            EnableWindow(label, FALSE);
            const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(label, GWL_STYLE));
            const DWORD type = style & SS_TYPEMASK;
            Check(type == SS_LEFT || type == SS_LEFTNOWORDWRAP || type == SS_CENTER || type == SS_RIGHT,
                "new settings static types need explicit paint regression coverage");
            RECT rect{}; GetClientRect(label, &rect);
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = rect.right; info.bmiHeader.biHeight = -rect.bottom;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            HDC dc = CreateCompatibleDC(nullptr); void* bytes = nullptr;
            HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bytes, nullptr, 0);
            Check(bitmap && bytes, "allocate disabled text bitmap");
            HGDIOBJ old = SelectObject(dc, bitmap);
            LRESULT brush = 0;
            state.theme.HandleMessage(WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc),
                reinterpret_cast<LPARAM>(label), brush);
            const COLORREF background = GetBkColor(dc);
            // Compare against one ordinary antialiased text draw, not a numeric
            // color envelope: ClearType filtering can overshoot that envelope.
            FillRect(dc, &rect, reinterpret_cast<HBRUSH>(brush));
            const auto previousFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(label, WM_GETFONT, 0, 0)));
            std::wstring caption(static_cast<size_t>(GetWindowTextLengthW(label)) + 1, L'\0');
            GetWindowTextW(label, caption.data(), static_cast<int>(caption.size()));
            RECT textRect = rect;
            UINT referenceFlags = DT_NOPREFIX | DT_EXPANDTABS;
            if (style & SS_CENTERIMAGE) referenceFlags |= DT_SINGLELINE | DT_VCENTER;
            else if (type != SS_LEFTNOWORDWRAP) referenceFlags |= DT_WORDBREAK;
            if (type == SS_CENTER) referenceFlags |= DT_CENTER;
            if (type == SS_RIGHT) referenceFlags |= DT_RIGHT;
            if (label == c.surround51Hint) {
                RECT measured = rect;
                TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
                DrawTextW(dc, caption.c_str(), -1, &measured, referenceFlags | DT_CALCRECT);
                Check(measured.bottom - measured.top >= 4 * metrics.tmHeight,
                    "surround explanation retains at least four lines");
                Check(measured.bottom <= rect.bottom, "surround explanation fits at each language/DPI");
            }
            if (std::find(c.guideDescriptions.begin(),c.guideDescriptions.end(),label)!=c.guideDescriptions.end()) {
                RECT measured=textRect;
                DrawTextW(dc,caption.c_str(),-1,&measured,referenceFlags|DT_CALCRECT);
                textRect.top+=std::max(0L,((textRect.bottom-textRect.top)-(measured.bottom-measured.top))/2);
            }
            DrawTextW(dc, caption.c_str(), -1, &textRect, referenceFlags);
            GdiFlush();
            const auto* reference = static_cast<const DWORD*>(bytes);
            const std::vector<DWORD> expected(reference, reference + rect.right * rect.bottom);
            SelectObject(dc, previousFont);
            if ((label == c.scalingLabel || label == c.surround51Hint) && !state.theme.HighContrast()) {
                // Unsubclassed control reproduces the original OS rendering on
                // this host, independently of the corrected theme paint path.
                HWND native = CreateWindowW(L"STATIC", caption.c_str(),
                    WS_CHILD | WS_DISABLED | (style & 0xffff),
                    0, 0, rect.right, rect.bottom, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                Check(native != nullptr, "create native disabled comparison");
                SendMessageW(native, WM_SETFONT, SendMessageW(label, WM_GETFONT, 0, 0), FALSE);
                FillRect(dc, &rect, reinterpret_cast<HBRUSH>(brush));
                SendMessageW(native, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
                GdiFlush();
                unsigned nativeDifferences = 0;
                const auto* nativePixels = static_cast<const DWORD*>(bytes);
                for (size_t i = 0; i < expected.size(); ++i)
                    if ((nativePixels[i] & 0xffffff) != (expected[i] & 0xffffff)) ++nativeDifferences;
                std::printf("Native disabled comparison light=%d en=%d dpi=%u type=%lu differences=%u\n",
                    light, english, dpi, type, nativeDifferences);
                DestroyWindow(native);
            }
            for (int pass = 0; pass < 2; ++pass) {
                if (pass == 0) FillRect(dc, &rect, reinterpret_cast<HBRUSH>(brush));
                SendMessageW(label, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
                GdiFlush();
                unsigned differences = 0, ink = 0;
                const auto* pixels = static_cast<const DWORD*>(bytes);
                for (int i = 0; i < rect.right * rect.bottom; ++i) {
                    const DWORD pixel = pixels[i];
                    const int channels[] = {int((pixel >> 16) & 255), int((pixel >> 8) & 255), int(pixel & 255)};
                    const int bg[] = {GetRValue(background), GetGValue(background), GetBValue(background)};
                    if (channels[0] != bg[0] || channels[1] != bg[1] || channels[2] != bg[2]) ++ink;
                    if ((pixel & 0xffffff) != (expected[i] & 0xffffff)) ++differences;
                }
                if (!state.theme.HighContrast()) {
                    std::printf("Disabled label light=%d en=%d dpi=%u type=%lu pass=%d ink=%u differences=%u\n",
                        light, english, dpi, type, pass, ink, differences);
                    Check((ink > 0 || caption[0] == L'\0') && differences == 0,
                        "all disabled descriptions/labels match flat rendering without embossing");
                }
            }
            SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
            EnableWindow(label, wasEnabled);
            ++testedLabels;
        }
        c.activeTab = SettingsTab::Audio;
        Check(SendMessageW(c.audioCombo, CB_GETCOUNT, 0, 0) == 3, "fixture includes ASIO without loading a driver");
        for (int mode : {2, 1, 0, 2, 0}) {
            SendMessageW(c.audioCombo, CB_SETCURSEL, mode, 0);
            RefreshPreview(state);
            Check(IsWindowEnabled(c.surround51Hint) == (mode == 0) &&
                  IsWindowEnabled(c.surround51Check) == (mode == 0),
                "ASIO/Exclusive/Shared transitions preserve actual surround availability");
            Check(GetWindowLongPtrW(c.surround51Hint, GWL_STYLE) & WS_VISIBLE,
                "unavailable surround explanation stays visible");
        }
        c.activeTab = SettingsTab::VideoWindow;
        SendMessageW(c.pixelCheck, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(c.upscalerCombo, CB_SETCURSEL, 2, 0);
        RefreshPreview(state);
        Check(IsWindowEnabled(c.scalingLabel) && IsWindowEnabled(c.displayResolutionLabel),
            "labels restore native enabled state");
        DestroyWindow(hwnd);
        for (HFONT font : c.uiFonts) DeleteObject(font);
        DrainNativeMessages();
    }
    std::printf("Disabled text audit: %u labels, two consecutive paints each, both languages/themes and four DPIs.\n", testedLabels);
}
void CheckComboRows() {
    for (bool light : {false, true}) for (bool english : {false, true}) for (UINT dpi : {96u, 120u, 144u, 192u}) {
        TestWindow state;
        HWND hwnd = CreateWindowW(L"LLCV.SettingsTheme.Tests", L"Dropdown paint test", WS_POPUP,
            0, 0, SettingsPixels(1000, dpi), SettingsPixels(650, dpi), nullptr, nullptr, GetModuleHandleW(nullptr), &state);
        Check(hwnd != nullptr, "create dropdown fixture");
        CreateActualContents(state, hwnd, english, dpi);
        auto& c = state.controls;
        SendMessageW(c.themeCombo, CB_SETCURSEL, light ? 1 : 0, 0);
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(2053, CBN_SELCHANGE), reinterpret_cast<LPARAM>(c.themeCombo));
        HWND combo = c.captureDeviceCombo;
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        const wchar_t* caption = english ? L"Automatic (GC573 preferred) & Camera" : L"자동 선택 (GC573 우선 · 권장) & 카메라";
        AddComboItem(combo, caption);
        SendMessageW(combo, CB_SETITEMDATA, 0, 12345);
        const int rowHeight = static_cast<int>(SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0));
        const int width = SettingsPixels(480, dpi), height = rowHeight + 6;
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        Check(dc && bitmap && bits, "allocate dropdown paint test surface");
        const auto previous = SelectObject(dc, bitmap);
        const auto previousFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0)));
        TEXTMETRICW metrics{};
        Check(GetTextMetricsW(dc, &metrics), "dropdown uses actual UI font metrics");
        for (UINT flags : {0u, UINT(ODS_SELECTED), UINT(ODS_DISABLED), UINT(ODS_COMBOBOXEDIT | ODS_SELECTED)}) {
            RECT row{0, 3, width, rowHeight + 3};
            DRAWITEMSTRUCT item{ODT_COMBOBOX, static_cast<UINT>(GetDlgCtrlID(combo)), 0, ODA_DRAWENTIRE,
                flags, combo, dc, row, 0};
            Check(SendMessageW(hwnd, WM_DRAWITEM, item.CtlID, reinterpret_cast<LPARAM>(&item)) == TRUE,
                "parent handles native dropdown draw requests");
            GdiFlush();
            const auto* pixels = static_cast<const DWORD*>(bits);
            const std::vector<DWORD> actual(pixels, pixels + width * height);
            const COLORREF background = GetPixel(dc, 0, row.top);
            // Query theme text color directly rather than relying on ClearType
            // edge colors, which can overshoot the source color.
            LRESULT brush = 0;
            EnableWindow(combo, (flags & ODS_DISABLED) == 0);
            state.theme.HandleMessage(WM_CTLCOLORLISTBOX, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(combo), brush);
            COLORREF foreground = GetTextColor(dc);
            const bool selected = (flags & ODS_SELECTED) && (!(flags & ODS_COMBOBOXEDIT) || state.theme.HighContrast());
            if (selected) foreground = GetSysColor(COLOR_HIGHLIGHTTEXT);
            EnableWindow(combo, TRUE);
            SetDCBrushColor(dc, background);
            FillRect(dc, &row, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            SetTextColor(dc, foreground); SetBkMode(dc, TRANSPARENT);
            RECT reference = row;
            reference.left += SettingsPixels(11, dpi); reference.right -= SettingsPixels(8, dpi);
            reference.top += (rowHeight - metrics.tmHeight) / 2;
            DrawTextW(dc, caption, -1, &reference, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            GdiFlush();
            size_t differences = 0, ink = 0;
            const DWORD bg = RGB(GetBValue(background), GetGValue(background), GetRValue(background));
            for (int y = row.top; y < row.bottom; ++y) for (int x = 0; x < width; ++x) {
                const int offset = y * width + x;
                if ((actual[offset] & 0xffffff) != (pixels[offset] & 0xffffff)) ++differences;
                if ((actual[offset] & 0xffffff) != bg) ++ink;
            }
            std::printf("Dropdown center light=%d en=%d dpi=%u state=%u ink=%zu differences=%zu\n",
                light, english, dpi, flags, ink, differences);
            Check(ink > 0 && differences == 0, "dropdown text matches independently centered font-height reference");
        }
        Check(SendMessageW(combo, CB_GETITEMDATA, 0, 0) == 12345, "painting preserves separate native item data");
        Check(SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(caption)) == 0,
            "native string lookup remains available");
        AddComboItem(combo, L"Second camera");
        SendMessageW(combo, WM_KEYDOWN, VK_DOWN, 0);
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1, "native down-arrow selection retained");
        Check(SendMessageW(combo, CB_GETITEMDATA, 0, 0) == 12345, "native navigation preserves item data");
        SelectObject(dc, previousFont); SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc);
        DestroyWindow(hwnd);
        for (HFONT font : c.uiFonts) DeleteObject(font);
        DrainNativeMessages();
    }
}
void CheckCompactNavigationTheme() {
    for (UINT dpi : {192u, 288u}) for (bool english : {false, true}) {
        TestWindow state;
        auto& c = state.controls;
        FitSettingsClientArea(&c, {SettingsPixels(627,dpi),SettingsPixels(276,dpi)}, dpi);
        HWND hwnd = CreateWindowW(L"LLCV.SettingsTheme.Tests", L"Offscreen short-sidebar test",
            WS_POPUP | WS_CLIPCHILDREN, -32000, -32000,
            SettingsPixels(c.clientWidthDip,dpi), SettingsPixels(c.clientHeightDip,dpi),
            nullptr,nullptr,GetModuleHandleW(nullptr),&state);
        Check(hwnd != nullptr, "create short-sidebar theme fixture");
        CreateActualContents(state,hwnd,english,dpi,true);
        const auto font = SendMessageW(c.tabControl,WM_GETFONT,0,0);
        for (bool light : {false,true}) {
            SendMessageW(c.themeCombo,CB_SETCURSEL,light ? 1 : 0,0);
            SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(2053,CBN_SELCHANGE),reinterpret_cast<LPARAM>(c.themeCombo));
            state.theme.RefreshControls(dpi);
            LRESULT result = 0;
            state.theme.HandleMessage(WM_DPICHANGED,MAKEWPARAM(dpi,dpi),0,result);
            RECT bounds{}; GetClientRect(c.tabControl,&bounds);
            for (int row=0;row<kSettingsNavigationCount;++row) {
                RECT item{};
                Check(SendMessageW(c.tabControl,LB_GETITEMRECT,row,reinterpret_cast<LPARAM>(&item)) != LB_ERR &&
                      item.top >= 0 && item.bottom <= bounds.bottom,
                      "theme refresh and DPI handling keep every compact navigation row visible");
            }
            RECT navigation{}, watermark{}, overlap{};
            GetWindowRect(c.tabControl,&navigation); GetWindowRect(c.versionWatermark,&watermark);
            Check(!IntersectRect(&overlap,&navigation,&watermark), "compact navigation stays above version");
            Check(SendMessageW(c.tabControl,WM_GETFONT,0,0) == font, "compact theme keeps original font");
            Check(!(GetWindowLongPtrW(c.tabControl,GWL_STYLE) & (WS_VSCROLL|WS_HSCROLL)),
                  "compact navigation does not add scrollbars");
        }
        FitSettingsClientArea(&c,{2000,1400},96);
        LayoutSettingsControls(&c,96); state.theme.RefreshControls(96);
        Check(SendMessageW(c.tabControl,LB_GETITEMHEIGHT,0,0) == 44,
              "normal navigation spacing is restored after compact mode");
        DestroyWindow(hwnd);
        for (HFONT item : c.uiFonts) DeleteObject(item);
        DrainNativeMessages();
    }
}
void CompactSnapshots(const wchar_t* directory, bool shortWorkArea = false) {
    using namespace llcv::settings;
    const UINT dpi=shortWorkArea ? 192 : 144;
    for(bool light:{false,true}) for(bool english:{false,true}) {
        TestWindow state;
        FitSettingsClientArea(&state.controls,shortWorkArea ? SIZE{1254,552} : SIZE{1280,680},dpi);
        HWND hwnd=CreateWindowW(L"LLCV.SettingsTheme.Tests",L"Offscreen compact settings",
            WS_POPUP|WS_CLIPCHILDREN,-32000,-32000,
            SettingsPixels(state.controls.clientWidthDip,dpi),SettingsPixels(state.controls.clientHeightDip,dpi),
            nullptr,nullptr,GetModuleHandleW(nullptr),&state);
        Check(hwnd!=nullptr,"create compact snapshot fixture");
        CreateActualContents(state,hwnd,english,dpi,true);
        auto& c=state.controls;
        SendMessageW(c.themeCombo,CB_SETCURSEL,light?1:0,0);
        SendMessageW(hwnd,WM_COMMAND,MAKEWPARAM(2053,CBN_SELCHANGE),reinterpret_cast<LPARAM>(c.themeCombo));
        state.theme.RefreshControls(dpi);
        for(int tab=0;tab<kSettingsNavigationCount;++tab) {
            c.activeTab=SettingsTabFromNavigationIndex(tab);
            SendMessageW(c.tabControl,LB_SETCURSEL,tab,0);
            UpdateAdvancedControlVisibility(&c,false,VideoPixelFormat::Nv12);
            const int pages=static_cast<int>(c.compactPageNames.size());
            for(int page=0;page<pages;++page) {
                c.compactPages[tab]=page;
                UpdateAdvancedControlVisibility(&c,false,VideoPixelFormat::Nv12);
                SetSettingsControlVisible(c.captureAudioDeviceCombo,tab==0);
                SetSettingsControlVisible(c.captureAudioStatus,false);
                wchar_t name[100]{};
                swprintf_s(name,L"compact-%s-%s-tab%d-page%d.bmp",light?L"light":L"dark",english?L"en":L"ko",tab,page);
                SaveSnapshot(hwnd,(std::wstring(directory)+L"/"+name).c_str());
            }
        }
        DestroyWindow(hwnd);
        for(HFONT font:c.uiFonts) DeleteObject(font);
    }
}

void ActualSnapshots(const wchar_t* directory, bool videoOnly = false, bool windowOnly = false) {
    using namespace llcv::settings;
    for (bool light : {false, true}) for (bool english : {false, true}) for (UINT dpi : {96u, 120u, 144u, 192u}) {
        if ((videoOnly || windowOnly) && dpi != 144) continue;
        TestWindow state;
        HWND hwnd = CreateWindowW(L"LLCV.SettingsTheme.Tests", L"Offscreen actual settings",
            WS_POPUP | WS_CLIPCHILDREN, 0, 0, SettingsPixels(kSettingsClientWidthDip, dpi), SettingsPixels(650, dpi),
            nullptr, nullptr, GetModuleHandleW(nullptr), &state);
        Check(hwnd != nullptr, "create actual settings snapshot window");
        CreateActualContents(state, hwnd, english, dpi, true);
        auto& c = state.controls;
        SendMessageW(c.themeCombo, CB_SETCURSEL, light ? 1 : 0, 0);
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(2053, CBN_SELCHANGE),
            reinterpret_cast<LPARAM>(c.themeCombo));
        for (int index = 0; index < kSettingsNavigationCount; ++index) {
            if (videoOnly && SettingsTabFromNavigationIndex(index) != SettingsTab::VideoWindow) continue;
            if (windowOnly && SettingsTabFromNavigationIndex(index) != SettingsTab::Window) continue;
            if (!windowOnly && dpi != 96 &&
                SettingsTabFromNavigationIndex(index) == SettingsTab::Updates) continue;
            c.activeTab = SettingsTabFromNavigationIndex(index);
            SendMessageW(c.tabControl, LB_SETCURSEL, index, 0);
            UpdateAdvancedControlVisibility(&c, false, VideoPixelFormat::Nv12);
#ifdef LLCV_RECORDING
            // Page entry refreshes storage; replace machine-specific data only
            // after that refresh when exporting documentation screenshots.
            if(c.activeTab==SettingsTab::Recording)
                SetWindowTextW(c.recSpace, english ? L"Free space is checked for the selected folder." :
                    L"선택한 폴더의 남은 공간을 확인합니다.");
#endif
            if (index == 0) {
                SetSettingsControlVisible(c.captureAudioDeviceCombo, true);
                SetSettingsControlVisible(c.captureAudioStatus, false);
            }
            wchar_t name[100]{};
            swprintf_s(name, L"settings-%s-%s-tab%d-%udpi.bmp", light ? L"light" : L"dark",
                english ? L"en" : L"ko", index, dpi);
            SaveSnapshot(hwnd, (std::wstring(directory) + L"/" + name).c_str());
            if (windowOnly) continue;
            if (videoOnly) {
                SendMessageW(c.upscalerCombo, CB_SETCURSEL, 0, 0);
                SendMessageW(c.videoCombo, CB_SETCURSEL, 1, 0);
                SendMessageW(c.displayResolutionCombo, CB_SETCURSEL, 2, 0);
                UpdateAdvancedControlVisibility(&c, false, VideoPixelFormat::Nv12);
                swprintf_s(name, L"settings-%s-%s-vsr-off-%udpi.bmp", light ? L"light" : L"dark",
                    english ? L"en" : L"ko", dpi);
                SaveSnapshot(hwnd, (std::wstring(directory) + L"/" + name).c_str());
                SendMessageW(c.upscalerCombo,CB_SETCURSEL,1,0);
                UpdateAdvancedControlVisibility(&c,false,VideoPixelFormat::Nv12);
                swprintf_s(name,L"settings-%s-%s-cmaa-nis-%udpi.bmp",light?L"light":L"dark",english?L"en":L"ko",dpi);
                SaveSnapshot(hwnd,(std::wstring(directory)+L"/"+name).c_str());
                continue;
            }
            if (c.activeTab==SettingsTab::GuideDiagnostics) {
                c.shortcutBindings[0]=llcv::shortcuts::Ctrl|VK_F1;
                c.shortcutBindings[static_cast<size_t>(llcv::shortcuts::Action::Exit)]=llcv::shortcuts::Ctrl|'Q';
                for (bool longKey:{false,true}) {
                    if (longKey) c.shortcutBindings[0]=llcv::shortcuts::Ctrl|llcv::shortcuts::Alt|llcv::shortcuts::Shift|VK_F12;
                    RefreshShortcutCaptions(&c);
                    swprintf_s(name,L"shortcuts-%s-%s-%s-%udpi.bmp",longKey ? L"expanded" : L"compact",
                        light ? L"light" : L"dark",english ? L"en" : L"ko",dpi);
                    SaveSnapshot(hwnd,(std::wstring(directory)+L"/"+name).c_str());
                }
                c.shortcutBindings=llcv::shortcuts::Defaults;
                RefreshShortcutCaptions(&c);
            }
            if (index == 1) {
                for (int mode : {1, 2}) {
                    SendMessageW(c.audioCombo, CB_SETCURSEL, mode, 0);
                    UpdateAdvancedControlVisibility(&c, mode == 1, VideoPixelFormat::Nv12);
                    if (mode == 2) {
                        SetWindowTextW(c.audioOutputLabel, english ? L"ASIO output driver" : L"ASIO 출력 드라이버");
                        SendMessageW(c.audioOutputCombo, CB_RESETCONTENT, 0, 0);
                        AddComboItem(c.audioOutputCombo, L"ASIO driver (UI fixture)");
                        SendMessageW(c.bufferCombo, CB_RESETCONTENT, 0, 0);
                        AddComboItem(c.bufferCombo, english ? L"ASIO driver preferred buffer" : L"ASIO 드라이버 선호 버퍼 (드라이버 설정 사용)");
                        SetWindowTextW(c.audioStatus, english ? L"ASIO output · driver buffer · app clock correction available" :
                            L"ASIO 출력 · 드라이버 기본 버퍼 사용 · 앱 클록 보정 가능");
                    }
                    swprintf_s(name, L"audio-%s-%s-%s-%udpi.bmp", mode == 2 ? L"asio" : L"exclusive",
                        light ? L"light" : L"dark", english ? L"en" : L"ko", dpi);
                    SaveSnapshot(hwnd, (std::wstring(directory) + L"/" + name).c_str());
                }
                SendMessageW(c.audioCombo, CB_SETCURSEL, 0, 0);
            }
            if (index == 0) {
                const LRESULT pixel = SendMessageW(c.pixelCheck, BM_GETCHECK, 0, 0);
                const LRESULT vsr = SendMessageW(c.upscalerCombo, CB_GETCURSEL, 0, 0);
                SendMessageW(c.pixelCheck, BM_SETCHECK, BST_CHECKED, 0);
                SendMessageW(c.upscalerCombo, CB_SETCURSEL, 0, 0);
                UpdateAdvancedControlVisibility(&c, false, VideoPixelFormat::Nv12);
                swprintf_s(name, L"settings-%s-%s-pixel-perfect-%udpi.bmp", light ? L"light" : L"dark",
                    english ? L"en" : L"ko", dpi);
                SaveSnapshot(hwnd, (std::wstring(directory) + L"/" + name).c_str());
                SendMessageW(c.pixelCheck, BM_SETCHECK, pixel, 0);
                SendMessageW(c.upscalerCombo, CB_SETCURSEL, vsr, 0);
                UpdateAdvancedControlVisibility(&c, false, VideoPixelFormat::Nv12);
            }
        }
        if (videoOnly) {
            c.activeTab = SettingsTab::VideoWindow;
            SendMessageW(c.pixelFormatCombo, CB_RESETCONTENT, 0, 0);
            AddComboItem(c.pixelFormatCombo, L"MJPEG");
            UpdateAdvancedControlVisibility(&c, false, VideoPixelFormat::Mjpeg);
            SetSettingsControlVisible(c.captureAudioDeviceCombo, true);
            SetSettingsControlVisible(c.captureAudioStatus, false);
            SaveSnapshot(hwnd, (std::wstring(directory) + (light ? L"/light" : L"/dark") +
                (english ? L"-en-sdr-mjpeg-144dpi.bmp" : L"-ko-sdr-mjpeg-144dpi.bmp")).c_str());
            const auto savedDisplay = SendMessageW(c.displayResolutionCombo, CB_GETCURSEL, 0, 0);
            const auto matchCapture = SendMessageW(c.displayResolutionCombo, CB_GETCOUNT, 0, 0) - 1;
            SendMessageW(c.displayResolutionCombo, CB_SETCURSEL, matchCapture, 0);
            UpdateAdvancedControlVisibility(&c,false,VideoPixelFormat::Mjpeg);
            SaveSnapshot(hwnd, (std::wstring(directory) + (light ? L"/light" : L"/dark") +
                (english ? L"-en-match-capture.bmp" : L"-ko-match-capture.bmp")).c_str());
            c.frameOptionsOpen=true;
            UpdateAdvancedControlVisibility(&c,false,VideoPixelFormat::Mjpeg);
            SaveSnapshot(hwnd, (std::wstring(directory) + (light ? L"/light" : L"/dark") +
                (english ? L"-en-frame-options.bmp" : L"-ko-frame-options.bmp")).c_str());
            c.frameOptionsOpen=false;
            SendMessageW(c.displayResolutionCombo, CB_SETCURSEL, savedDisplay, 0);
            UpdateAdvancedControlVisibility(&c,false,VideoPixelFormat::Mjpeg);
        }
        if (!videoOnly && !windowOnly && dpi == 96) {
            c.activeTab = SettingsTab::VideoWindow;
            SendMessageW(c.tabControl, LB_SETCURSEL, 0, 0);
            SendMessageW(c.pixelFormatCombo, CB_RESETCONTENT, 0, 0);
            AddComboItem(c.pixelFormatCombo, L"P010 10-bit HDR10");
            UpdateAdvancedControlVisibility(&c, false, VideoPixelFormat::P010);
            SetSettingsControlVisible(c.captureAudioDeviceCombo, true);
            SetSettingsControlVisible(c.captureAudioStatus, false);
            SaveSnapshot(hwnd, (std::wstring(directory) + (light ? L"/light" : L"/dark") +
                (english ? L"-en-hdr-96dpi.bmp" : L"-ko-hdr-96dpi.bmp")).c_str());
        }
        for (const wchar_t* label : {L"S22U (Windows 가상 카메라) (실험적)", L"IP Camera Bridge Plus (실험적)", L"OBS Virtual Camera (실험적)"})
            SendMessageW(c.captureDeviceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        wchar_t dropdownName[100]{};
        swprintf_s(dropdownName, L"dropdown-%s-%s-%udpi.bmp", light ? L"light" : L"dark", english ? L"en" : L"ko", dpi);
        if (!videoOnly && !windowOnly)
            SaveSnapshot(hwnd, (std::wstring(directory) + L"/" + dropdownName).c_str(), c.captureDeviceCombo);
        DestroyWindow(hwnd);
        for (HFONT font : c.uiFonts) DeleteObject(font);
    }
}
}
#ifdef LLCV_SETTINGS_PREVIEW
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
#else
int wmain(int argc, wchar_t** argv) {
#endif
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LLCV.SettingsTheme.Tests";
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    Check(RegisterClassW(&wc) != 0, "register hidden test class");
#ifdef LLCV_SETTINGS_PREVIEW
    TestWindow state;
    state.preview = true;
    const UINT dpi = GetDpiForSystem();
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rect{0, 0, SettingsPixels(kSettingsClientWidthDip, dpi), SettingsPixels(650, dpi)};
    AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_CONTROLPARENT, dpi);
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName,
        L"LLCV · UI Preview (no hardware)", style, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, &state);
    Check(hwnd != nullptr, "create interactive design preview");
    CreateActualContents(state, hwnd, false, GetDpiForWindow(hwnd));
#ifdef LLCV_RECORDING
    state.controls.activeTab = SettingsTab::Recording;
    state.controls.recordingLive=true;ShowRecordingSettings(&state.controls);
#else
    state.controls.activeTab = SettingsTab::VideoWindow;
#endif
    SendMessageW(state.controls.tabControl, LB_SETCURSEL, SettingsNavigationIndex(state.controls.activeTab), 0);
    RefreshPreview(state);
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd); SetFocus(state.controls.tabControl);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    for (HFONT font : state.controls.uiFonts) DeleteObject(font);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
#else
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2 && (std::wcscmp(argv[1], L"--trace-cycle") == 0 || std::wcscmp(argv[1], L"--trace-two") == 0)) {
        traceCycle = true; OneCycle();
        if (std::wcscmp(argv[1], L"--trace-two") == 0) OneCycle();
        UnregisterClassW(wc.lpszClassName, wc.hInstance); return 0;
    }
    if (argc == 2 && std::wcscmp(argv[1], L"--trace-all") == 0) traceCycle = true;
    if (argc == 2 && std::wcscmp(argv[1], L"--trace-resources") == 0) traceResources = true;
    if (argc == 2 && std::wcscmp(argv[1], L"--disabled-text") == 0) {
        CheckDisabledLabelPixels();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }
    if (argc == 2 && std::wcscmp(argv[1], L"--combo-rows") == 0) {
        CheckComboRows();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }
    // A fast design-only path; the normal invocation and --screenshots still
    // exercise every native input/state assertion and all 32 lifecycle cycles.
    if (argc == 3 && (std::wcscmp(argv[1], L"--compact-screenshots-only") == 0 ||
                     std::wcscmp(argv[1], L"--compact-edge-screenshots-only") == 0)) {
        CompactSnapshots(argv[2],std::wcscmp(argv[1], L"--compact-edge-screenshots-only") == 0);
        UnregisterClassW(wc.lpszClassName,wc.hInstance);
        std::puts("Compact native offscreen snapshots saved.");
        return 0;
    }
    if (argc == 3 && std::wcscmp(argv[1], L"--window-screenshots-only") == 0) {
        ActualSnapshots(argv[2], false, true);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }
    if (argc == 3 && std::wcscmp(argv[1], L"--screenshots-only") == 0) {
        ActualSnapshots(argv[2]);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        std::puts("Settings snapshots saved (visual QA only; lifecycle regression not run).");
        return 0;
    }
    if (argc == 3 && std::wcscmp(argv[1], L"--video-screenshots-only") == 0) {
        ActualSnapshots(argv[2], true);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }
    CheckActualTabOrder();
    CheckDeferredTransitions();
    CheckDisabledLabelPixels();
    CheckComboRows();
    CheckCompactNavigationTheme();
    PrintResourceDiagnostics("after-tab-order", 0);
    if (argc == 2 && std::wcscmp(argv[1], L"--tab-order") == 0) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 0;
    }
    for (int i = 0; i < 4; ++i) {
        OneCycle();
        PrintResourceDiagnostics("warmup", i + 1);
    }
    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int i = 0; i < 32; ++i) {
        OneCycle(i == 31 && argc == 2 && !traceCycle && !traceResources ? argv[1] : nullptr);
        PrintResourceDiagnostics("measured", i + 1);
        if (i % 8 == 7) std::printf("Theme lifecycle %d/32 complete\n", i + 1);
    }
    const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userAfter = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    std::printf("GDI %lu -> %lu; USER %lu -> %lu\n", gdiBefore, gdiAfter, userBefore, userAfter);
    Check(gdiAfter <= gdiBefore, "no GDI resource growth after repeated attach/destroy");
    Check(userAfter <= userBefore, "no USER resource growth after repeated attach/destroy");
    if (argc == 3 && std::wcscmp(argv[1], L"--screenshots") == 0) ActualSnapshots(argv[2]);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::puts("SettingsThemeTests passed: native state, notifications, refresh, painting and lifecycle.");
    return 0;
#endif
}
