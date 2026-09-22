// Diagnostic harness: real application handlers, simulated Win32 geometry.
// No GPU, DWM, capture device, or physical display link is simulated.
#include <windows.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
struct VirtualMonitor { RECT rect; UINT dpi; };
static VirtualMonitor monitors[2];
static int connectedMonitors = 2;
static bool reverseMonitorEnumeration = false;
static LONG_PTR simulatedStyle = WS_POPUP;
static RECT simulatedWindowRect;
static POINT cursorPoint;
static HWND testWindow = reinterpret_cast<HWND>(0x12345);
static HWND simulatedCapture = nullptr;
static int audioDragMoves = 0;
static HWND FakeSetCapture(HWND hwnd) {
    const HWND previous = simulatedCapture; simulatedCapture = hwnd; return previous;
}
static HWND FakeGetCapture() { return simulatedCapture; }
static BOOL FakeReleaseCapture() { simulatedCapture = nullptr; return TRUE; }
static LRESULT FakeSendMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static HMONITOR MonitorHandle(int i) { return reinterpret_cast<HMONITOR>(INT_PTR(i + 1)); }
static int MonitorIndex(HMONITOR h) { return h == MonitorHandle(1) ? 1 : 0; }
static HMONITOR FakeMonitorFromPoint(POINT p, DWORD) {
    for (int i=0;i<2;++i) if (PtInRect(&monitors[i].rect,p)) return MonitorHandle(i);
    return MonitorHandle(0);
}
static HMONITOR FakeMonitorFromRect(LPCRECT r, DWORD) {
    LONG best=-1; int chosen=0;
    for(int i=0;i<2;++i) {
        RECT overlap{}; IntersectRect(&overlap,r,&monitors[i].rect);
        LONG area=(overlap.right-overlap.left)*(overlap.bottom-overlap.top);
        if(area>best) { best=area; chosen=i; }
    }
    return MonitorHandle(chosen);
}
static HMONITOR FakeMonitorFromWindow(HWND,DWORD f) {return FakeMonitorFromRect(&simulatedWindowRect,f);}
static BOOL FakeGetMonitorInfoW(HMONITOR h,LPMONITORINFO info) {
    if ((h!=MonitorHandle(0) && h!=MonitorHandle(1)) || MonitorIndex(h)>=connectedMonitors) return FALSE;
    info->rcMonitor=info->rcWork=monitors[MonitorIndex(h)].rect;
    info->dwFlags=h==MonitorHandle(0)?MONITORINFOF_PRIMARY:0;
    if (info->cbSize >= sizeof(MONITORINFOEXW))
        swprintf_s(reinterpret_cast<MONITORINFOEXW*>(info)->szDevice,L"DISPLAY%d",MonitorIndex(h));
    return TRUE;
}
static BOOL FakeEnumDisplayDevicesW(LPCWSTR name,DWORD,PDISPLAY_DEVICEW device,DWORD) {
    swprintf_s(device->DeviceID,L"fake-%d",name && wcscmp(name,L"DISPLAY1")==0 ? 1 : 0);
    wcscpy_s(device->DeviceString,L"Simulated monitor");
    return TRUE;
}
static BOOL FakeEnumDisplayMonitors(HDC,LPCRECT,MONITORENUMPROC callback,LPARAM context) {
    for (int i=0;i<connectedMonitors;++i) {
        int index=reverseMonitorEnumeration && connectedMonitors==2 ? 1-i : i;
        if(!callback(MonitorHandle(index),nullptr,&monitors[index].rect,context)) break;
    }
    return TRUE;
}
static HRESULT FakeGetDpiForMonitor(HMONITOR h,MONITOR_DPI_TYPE,UINT* x,UINT* y) {
    *x=*y=monitors[MonitorIndex(h)].dpi; return S_OK;
}
static UINT FakeGetDpiForWindow(HWND) {return monitors[MonitorIndex(FakeMonitorFromWindow(nullptr,0))].dpi;}
static BOOL FakeGetCursorPos(LPPOINT p) {*p=cursorPoint;return TRUE;}
static BOOL FakeScreenToClient(HWND, LPPOINT p) {
    p->x -= simulatedWindowRect.left; p->y -= simulatedWindowRect.top; return TRUE;
}
static BOOL FakeClientToScreen(HWND, LPPOINT p) {
    p->x += simulatedWindowRect.left; p->y += simulatedWindowRect.top; return TRUE;
}
static HWND FakeWindowFromPoint(POINT p) {
    return PtInRect(&simulatedWindowRect, p) ? testWindow : nullptr;
}
static HWND FakeGetAncestor(HWND hwnd, UINT) { return hwnd; }
static BOOL FakeGetWindowRect(HWND,LPRECT r) {*r=simulatedWindowRect;return TRUE;}
static BOOL FakeGetClientRect(HWND,LPRECT r) {
    *r={0,0,simulatedWindowRect.right-simulatedWindowRect.left,simulatedWindowRect.bottom-simulatedWindowRect.top};return TRUE;
}
static LONG_PTR FakeGetWindowLongPtrW(HWND,int index) {return index==GWL_STYLE?simulatedStyle:0;}
static BOOL FakeSetWindowPos(HWND,HWND,int,int,int,int,UINT);
#define EnumDisplayDevicesW FakeEnumDisplayDevicesW
#define EnumDisplayMonitors FakeEnumDisplayMonitors
#define MonitorFromPoint FakeMonitorFromPoint
#define MonitorFromRect FakeMonitorFromRect
#define MonitorFromWindow FakeMonitorFromWindow
#define GetMonitorInfoW FakeGetMonitorInfoW
#define GetDpiForMonitor FakeGetDpiForMonitor
#define GetDpiForWindow FakeGetDpiForWindow
#define GetCursorPos FakeGetCursorPos
#define ScreenToClient FakeScreenToClient
#define ClientToScreen FakeClientToScreen
#define WindowFromPoint FakeWindowFromPoint
#define GetAncestor FakeGetAncestor
#define GetWindowRect FakeGetWindowRect
#define GetClientRect FakeGetClientRect
#define GetWindowLongPtrW FakeGetWindowLongPtrW
#define SetWindowPos FakeSetWindowPos
#define SetCapture FakeSetCapture
#define GetCapture FakeGetCapture
#define ReleaseCapture FakeReleaseCapture
#define SendMessageW FakeSendMessageW
#include "../src/main.cpp"
#undef fwprintf
#undef EnumDisplayDevicesW
#undef EnumDisplayMonitors
#undef MonitorFromPoint
#undef MonitorFromRect
#undef MonitorFromWindow
#undef GetMonitorInfoW
#undef GetDpiForMonitor
#undef GetDpiForWindow
#undef GetCursorPos
#undef ScreenToClient
#undef ClientToScreen
#undef WindowFromPoint
#undef GetAncestor
#undef GetWindowRect
#undef GetClientRect
#undef GetWindowLongPtrW
#undef SetWindowPos
#undef SetCapture
#undef GetCapture
#undef ReleaseCapture
#undef SendMessageW

static void Require(bool value,const char* message) {
    if(!value) {std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}
}
static LRESULT FakeSendMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCLBUTTONDOWN && wParam == HTCAPTION) {
        Require(hwnd == testWindow && simulatedCapture == nullptr &&
                !g_audioOnlyDragPending, "release capture before native move");
        ++audioDragMoves;
        return 0; // Do not enter the real OS modal move loop in this harness.
    }
    return WndProc(hwnd, msg, wParam, lParam);
}
static BOOL FakeSetWindowPos(HWND hwnd,HWND,int x,int y,int w,int h,UINT flags) {
    Require(hwnd==testWindow,"only simulated viewer may be positioned");
    const int oldW=simulatedWindowRect.right-simulatedWindowRect.left,oldH=simulatedWindowRect.bottom-simulatedWindowRect.top;
    if(flags&SWP_NOMOVE) {x=simulatedWindowRect.left;y=simulatedWindowRect.top;}
    if(flags&SWP_NOSIZE) {w=oldW;h=oldH;}
    simulatedWindowRect={x,y,x+w,y+h};
    if(oldW!=w||oldH!=h) WndProc(hwnd,WM_SIZE,SIZE_RESTORED,MAKELPARAM(w,h));
    return TRUE;
}
static void ApplyRect(RECT r) {
    FakeSetWindowPos(testWindow,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,0);
}
static void Reset(bool relative=true,bool perfect=true) {
    g_settings=AppSettings{};simulatedStyle=WS_POPUP;
    g_settings.relativeWindowSize=relative;
    g_settings.relativeWindowScalePpm=500000;
    g_settings.pixelPerfect=perfect;
    g_settings.windowSnap=false;
    g_settings.audioOnly=false;
    g_audioOnlyDragPending = false; simulatedCapture = nullptr; audioDragMoves = 0;
    g_fullscreen=false;g_videoHost=nullptr;
    g_outputTransition={};
    g_relativeMoveMonitor=nullptr;g_interactiveWindowMove=false;
    simulatedWindowRect={100,100,1060,640};
    cursorPoint={500,110};
    WndProc(testWindow,WM_SIZE,SIZE_RESTORED,MAKELPARAM(960,540));
    g_outputConfigurationGeneration=0;
}
static uint64_t Generation() {return g_outputConfigurationGeneration.load();}
static void Enter() {WndProc(testWindow,WM_ENTERSIZEMOVE,0,0);}
static void Leave() {WndProc(testWindow,WM_EXITSIZEMOVE,0,0);}
static void MoveTo(int monitor) {
    const RECT r=monitors[monitor].rect;
    cursorPoint={r.left+100,r.top+100};
    const LONG width=simulatedWindowRect.right-simulatedWindowRect.left;
    const LONG height=simulatedWindowRect.bottom-simulatedWindowRect.top;
    RECT moving{cursorPoint.x-100,cursorPoint.y-10,
                cursorPoint.x-100+width,cursorPoint.y-10+height};
    WndProc(testWindow,WM_MOVING,0,reinterpret_cast<LPARAM>(&moving));
    ApplyRect(moving); // Windows applies the RECT returned from WM_MOVING.
}
static void DpiTo(UINT dpi,int target) {
    SIZE size{};
    Require(WndProc(testWindow,WM_GETDPISCALEDSIZE,dpi,reinterpret_cast<LPARAM>(&size))==TRUE,
            "DPI preflight handled");
    const int x=monitors[target].rect.left+100,y=monitors[target].rect.top+100;
    RECT r{x,y,x+size.cx,y+size.cy};
    WndProc(testWindow,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&r));
}
int main(int argc, char** argv) {
    monitors[0]={{0,0,1920,1080},96};
    monitors[1]={{1920,0,4480,1440},144};
    // Placement regressions run in the regular suite, not just review mode.
    unsigned placementCases=0;
    for(int layout=0;layout<4;++layout)
    for(bool mixedDpi : {false,true})
    for(int target : {0,1})
    for(auto preset : {VideoPreset::R1920x1080,VideoPreset::R2560x1440,VideoPreset::R3840x2160})
    for(bool decorated : {false,true})
    for(bool relative : {false,true}) {
        monitors[1]={layout==0?RECT{1920,0,4480,1440}:
                     layout==1?RECT{-2560,0,0,1440}:
                     layout==2?RECT{0,-1440,2560,0}:RECT{0,1080,2560,2520},
                     mixedDpi?144u:96u};
        Reset(relative,true);
        simulatedStyle=decorated?WS_OVERLAPPEDWINDOW:WS_POPUP;
        g_settings.preferredDisplayMonitor=target?L"interface:fake-1":L"interface:fake-0";
        g_settings.videoPreset=preset;
        const auto chosen=SavedViewerMonitor();
        const auto client=InitialClientPixelsForMonitor(chosen);
        const auto outer=OuterSizeForClientPixels(client.cx,client.cy,
            static_cast<DWORD>(simulatedStyle),0,monitors[target].dpi);
        POINT origin{};
        Require(RestoredWindowOrigin(outer,origin),"explicit display restores origin");
        ApplyRect({origin.x,origin.y,origin.x+outer.cx,origin.y+outer.cy});
        NormalizeWindowSize(testWindow,true,chosen);
        Require(simulatedWindowRect.left==origin.x && simulatedWindowRect.top==origin.y,
                "initial normalization keeps selected-display origin even with oversized windows");
        Require(simulatedWindowRect.right-simulatedWindowRect.left==outer.cx &&
                simulatedWindowRect.bottom-simulatedWindowRect.top==outer.cy,
                "startup selection preserves pixel-perfect or relative-size policy");
        ++placementCases;
    }
    monitors[1]={{-2560,0,0,1440},144};
    Reset();ApplyRect({-560,100,400,640});
    for(int i=0;i<20;++i) {Enter();Leave();Leave();}
    Require(simulatedWindowRect.left==-560 && simulatedWindowRect.right==400 &&
            simulatedWindowRect.top==100 && simulatedWindowRect.bottom==640 && Generation()==0,
            "stationary boundary gestures and duplicate exits do not resize or rebuild");

    Reset();Enter();MoveTo(1);Leave();
    const RECT settled=simulatedWindowRect;
    const auto settledGeneration=Generation();
    for(int i=0;i<20;++i) {Enter();Leave();}
    Require(EqualRect(&settled,&simulatedWindowRect) && Generation()==settledGeneration,
            "actual cross-monitor move remains settled over subsequent empty gestures");

    Reset();
    const RECT beforeCancel=simulatedWindowRect;
    Enter();MoveTo(1);
    ApplyRect(beforeCancel); // Simulate Windows restoring geometry after Esc.
    Leave();Leave();
    Require(EqualRect(&beforeCancel,&simulatedWindowRect) && Generation()==1 &&
            g_outputTransition.Depth()==0,"cancel-restored geometry is not resized again at exit");

    monitors[1]={{1920,0,4480,1440},144};
    Reset(false,true);
    g_settings.preferredDisplayMonitor=L"interface:fake-0";
    ApplyRect({2100,100,3060,640});
    NormalizeWindowSize(testWindow,true); // Runtime call carries no startup target.
    Require(simulatedWindowRect.left>=1920,"startup preference does not pin subsequent movement");
    const RECT runtimeRect=simulatedWindowRect;
    NormalizeWindowSize(testWindow,true,reinterpret_cast<HMONITOR>(999));
    Require(EqualRect(&runtimeRect,&simulatedWindowRect),"invalid startup handle safely uses current monitor");
    g_fullscreen=true;
    NormalizeWindowSize(testWindow,true,MonitorHandle(0));
    Require(EqualRect(&runtimeRect,&simulatedWindowRect),"startup normalization does not override fullscreen geometry");
    g_fullscreen=false;

    std::printf("PASS %u startup placements + boundary/repeated-exit/cancel/free-move regressions\n",placementCases);
    if (argc == 2 && std::string(argv[1]) == "--review-placement") return 0;
    // Always run this regression, including normal CTest invocations.
    unsigned manualCases=0;
    for (UINT dpi : {96u,144u,192u})
    for (int target : {0,1})
    for (int width : {800,1200,1600})
    for (bool appliedBeforeDpi : {false,true}) {
        Reset(true,false);
        ApplyRect({1200,100,2160,640});
        Enter();
        RECT sizing{1200,100,1200+width,100+width*9/16};
        WndProc(testWindow,WM_SIZING,WMSZ_BOTTOMRIGHT,reinterpret_cast<LPARAM>(&sizing));
        if(appliedBeforeDpi) ApplyRect(sizing);
        SIZE pending{width,width*9/16};
        Require(WndProc(testWindow,WM_GETDPISCALEDSIZE,dpi,reinterpret_cast<LPARAM>(&pending))==FALSE,
                "manual preflight delegates pending-size DPI scaling to Windows");
        Require(pending.cx==width && pending.cy==width*9/16,
                "manual pending size is not overwritten by saved ratio");
        // Supply the OS-computed rectangle; the harness does not emulate its
        // linear scaling or non-client layout.
        const LONG x=monitors[target].rect.left+100;
        RECT suggested{x,100,x+width,100+width*9/16};
        WndProc(testWindow,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&suggested));
        Require(simulatedWindowRect.right-simulatedWindowRect.left==width &&
                simulatedWindowRect.bottom-simulatedWindowRect.top==width*9/16,
                "manual DPI preserves supplied user-controlled size");
        Require(Generation()==0,"manual DPI rebuild remains deferred");
        Leave();
        Require(simulatedWindowRect.right-simulatedWindowRect.left==width && Generation()==1 &&
                g_outputTransition.Depth()==0 && !g_outputTransition.Pending(),
                "manual exit retains size and flushes once");
        const auto& monitor=monitors[target].rect;
        Require(g_settings.relativeWindowScalePpm==
                    static_cast<int64_t>(width)*kRelativeScaleUnit/(monitor.right-monitor.left),
                "manual exit saves new relative scale");
        ++manualCases;
    }
    std::printf("PASS %u manual-resize/DPI regressions: pending size preserved, supplied size retained, one final rebuild\n",manualCases);
    if (argc == 2 && std::string(argv[1]) == "--review-manual-dpi") return 0;
    Reset();Enter();
    MoveTo(1);const auto oneCross=Generation();
    for(int i=0;i<1000;++i) MoveTo(1);
    Require(Generation()==oneCross,"stationary target does not repeatedly resize");
    Leave();
    Require(oneCross==0 && Generation()==1,"one crossing defers rebuild until exit");
    std::printf("OBSERVED single crossing: %llu early rebuild request; same-target repeats: 0\n",
                static_cast<unsigned long long>(oneCross));

    Reset();Enter();
    for(int i=0;i<100;++i) MoveTo((i+1)%2);
    const auto bouncing=Generation();Leave();
    Require(bouncing==0 && Generation()==1,"all automatic move sizes coalesce until exit");
    std::printf("OBSERVED 100 alternating crossings: %llu early rebuild requests, exit adds 1\n",
                static_cast<unsigned long long>(bouncing));

    Reset(true,false);Enter();
    RECT sizing=simulatedWindowRect;
    WndProc(testWindow,WM_SIZING,WMSZ_BOTTOMRIGHT,reinterpret_cast<LPARAM>(&sizing));
    for(int i=0;i<100;++i) ApplyRect({100,100,1060+i,640+i});
    Require(Generation()==0,"manual resize defers all intermediate updates");
    Leave();Require(Generation()==1,"manual resize emits one final update");
    std::printf("CONTROL 100 manual sizes: 0 early requests, 1 final request\n");

    // DPI target is monitor B while the pointer is still on A. The production
    // preflight stays on the current target; DPI notification resolves its RECT.
    Reset();Enter();
    cursorPoint={1800,100};
    SIZE preflight{};
    WndProc(testWindow,WM_GETDPISCALEDSIZE,144,reinterpret_cast<LPARAM>(&preflight));
    Require(preflight.cx==960,"DPI preflight uses current move target before crossing");
    DpiTo(144,1); MoveTo(1);
    Require(simulatedWindowRect.right-simulatedWindowRect.left==1280,"crossing subsequently selects B width");
    std::printf("OBSERVED DPI/cursor disagreement: preflight width %ld, settled width %ld\n",
                preflight.cx,simulatedWindowRect.right-simulatedWindowRect.left);
    Leave();

    // Regression: the pointer on A must not override the already-selected B.
    Reset();Enter();MoveTo(1);
    cursorPoint={1800,100};DpiTo(144,1);MoveTo(1);Leave();
    Require(simulatedWindowRect.right-simulatedWindowRect.left==1280 && Generation()==1,"late DPI cursor mismatch resolves B once");
    std::printf("OBSERVED supplied late-DPI order: B expected width 1280, retained width %ld; requests %llu\n",
                simulatedWindowRect.right-simulatedWindowRect.left,static_cast<unsigned long long>(Generation()));

    Reset(false,false);Enter();
    for(int i=0;i<100;++i) MoveTo((i+1)%2);
    Leave();Require(Generation()==0,"relative sizing disabled: moving alone requests no rebuild");
    std::puts("CONTROL relative sizing disabled: 100 crossings, 0 rebuild requests");

    // Nested transitions, duplicate enter/exit and null DPI payload.
    Reset();BeginOutputTransition();Enter();Enter();MoveTo(1);Leave();Leave();
    Require(Generation()==0 && g_outputTransition.Depth()==1,"outer transition owns nested move");
    EndOutputTransition(false);
    Require(Generation()==1,"outer transition flushes exactly once");
    WndProc(testWindow,WM_DPICHANGED,MAKELONG(144,144),0);
    Require(Generation()==1,"null DPI data ignored");

    Reset();
    g_settings.preferredDisplayMonitor=L"interface:fake-1";
    Require(SavedViewerMonitor()==MonitorHandle(1),"explicit monitor overrides unsaved position");
    POINT origin{};
    Require(RestoredWindowOrigin(SIZE{960,540},origin) && origin.x>=1920,"explicit startup placed on B");
    reverseMonitorEnumeration=true;
    Require(SavedViewerMonitor()==MonitorHandle(1),"identity survives enumeration reorder");
    reverseMonitorEnumeration=false;connectedMonitors=1;
    Require(SavedViewerMonitor()==MonitorHandle(0),"missing selected display falls back to primary");
    Require(g_settings.preferredDisplayMonitor==L"interface:fake-1","disconnect preserves preference");
    connectedMonitors=2;
    Require(SavedViewerMonitor()==MonitorHandle(1),"reconnected display restored");
    g_settings.preferredDisplayMonitor.clear();
    Require(SavedViewerMonitor()==nullptr,"automatic retains no-saved-position default");
    std::puts("PASS explicit startup display, enumeration reorder, disconnect/reconnect, automatic default");
    Reset();Enter();MoveTo(1);
    g_settings.audioOnly=true; // Avoid creating a child window in this virtual test.
    WndProc(testWindow,WM_CREATE,0,0);
    Require(g_outputTransition.Depth()==0 && !g_interactiveWindowMove && !g_outputTransition.Pending(),
            "new viewer resets abandoned modal move state");

    Reset(false, true);
    g_settings.audioOnly = true;
    g_settings.borderlessWindow = true;
    Require(ViewerWindowStyle(g_settings) == (WS_POPUP | WS_VISIBLE),
            "audio-only honors the borderless setting even with video pixel-perfect enabled");
    g_settings.borderlessWindow = false;
    Require((ViewerWindowStyle(g_settings) & (WS_CAPTION | WS_THICKFRAME)) ==
                (WS_CAPTION | WS_THICKFRAME),
            "audio-only keeps its resizable caption when borderless is disabled");
    g_settings.borderlessWindow = true;
    simulatedStyle = ViewerWindowStyle(g_settings);
    ApplyRect(RECT{100, 100, 480, 330});
    Require(WndProc(testWindow, WM_NCHITTEST, 0, MAKELPARAM(101, 101)) == HTTOPLEFT,
            "borderless audio-only has a resize corner");
    Require(WndProc(testWindow, WM_NCHITTEST, 0, MAKELPARAM(200, 130)) == HTCLIENT,
            "borderless header uses the same client drag gesture as controls");
    Require(WndProc(testWindow, WM_NCHITTEST, 0, MAKELPARAM(180, 190)) == HTCLIENT &&
                WndProc(testWindow, WM_NCHITTEST, 0, MAKELPARAM(180, 260)) == HTCLIENT &&
                WndProc(testWindow, WM_NCHITTEST, 0, MAKELPARAM(350, 260)) == HTCLIENT,
            "borderless audio-only volume controls retain client mouse interaction");

    g_volumePercent = 100;
    g_leftVolumePercent = 100;
    g_rightVolumePercent = 100;
    for (const auto point : {POINT{20, 45}, POINT{20, 112}, POINT{202, 112}}) {
        const WPARAM wheelDown = MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA));
        WndProc(testWindow, WM_MOUSEWHEEL, wheelDown,
                MAKELPARAM(point.x + 100, point.y + 100));
    }
    Require(g_volumePercent == 95 && g_leftVolumePercent == 95 &&
                g_rightVolumePercent == 95,
            "wheel works on expanded master and channel surfaces");
    for (const auto point : {POINT{30, 22}, POINT{30, 103},
                             POINT{190, 150}, POINT{30, 211}}) {
        WndProc(testWindow, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA),
                MAKELPARAM(point.x + 100, point.y + 100));
    }
    Require(g_volumePercent == 95 && g_leftVolumePercent == 95 &&
                g_rightVolumePercent == 95,
            "drag regions never adjust audio-only volume");
    for (const auto point : {POINT{20, 45}, POINT{20, 112}, POINT{202, 112}})
        WndProc(testWindow, WM_LBUTTONDBLCLK, 0, MAKELPARAM(point.x, point.y));
    Require(g_volumePercent == 100 && g_leftVolumePercent == 100 &&
                g_rightVolumePercent == 100,
            "double-click resets each modern audio control independently");

    for (const auto point : {POINT{30, 65}, POINT{30, 130}, POINT{220, 130}, POINT{190, 150}}) {
        const int target = point.y == 65 ? 3 : point.x == 30 ? 1 : point.x == 220 ? 2 : 0;
        WndProc(testWindow, WM_MOUSEMOVE, 0, MAKELPARAM(point.x, point.y));
        Require(g_audioOsdHoverTarget == target && !g_audioOnlyDragPending,
                "hover identifies controls without moving");
    }
    WndProc(testWindow, WM_MOUSEMOVE, 0, MAKELPARAM(30, 65));
    WndProc(testWindow, WM_MOUSELEAVE, 0, 0);
    Require(g_audioOsdHoverTarget == 0, "leaving client clears hover");
    WndProc(testWindow, WM_MOUSEMOVE, 0, MAKELPARAM(30, 65));
    WndProc(testWindow, WM_NCMOUSEMOVE, 0, 0);
    Require(g_audioOsdHoverTarget == 0, "resize edge clears hover");

    for (const bool borderless : {false, true}) {
        g_settings.borderlessWindow = borderless;
        simulatedStyle = ViewerWindowStyle(g_settings);
        for (const auto point : {POINT{30, 22}, POINT{30, 65}, POINT{30, 130},
                                 POINT{220, 130}, POINT{190, 150}, POINT{30, 211}}) {
            const LPARAM at = MAKELPARAM(point.x, point.y);
            const LPARAM moved = MAKELPARAM(point.x + 40, point.y + 30);
            const int beforeMove = audioDragMoves;
            WndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, at);
            Require(g_audioOnlyDragPending && simulatedCapture == testWindow,
                    "every audio-only region arms drag");
            WndProc(testWindow, WM_MOUSEMOVE, MK_LBUTTON, at);
            Require(audioDragMoves == beforeMove, "click alone never moves window");
            WndProc(testWindow, WM_LBUTTONUP, 0, at);
            Require(!g_audioOnlyDragPending && !simulatedCapture,
                    "click release cancels pending drag");
            WndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, at);
            WndProc(testWindow, WM_MOUSEMOVE, MK_LBUTTON, moved);
            Require(audioDragMoves == beforeMove + 1 &&
                    !g_audioOnlyDragPending && !simulatedCapture,
                    "dragging any audio-only region starts exactly one native move");
            WndProc(testWindow, WM_MOUSEMOVE, MK_LBUTTON, moved);
            Require(audioDragMoves == beforeMove + 1, "drag does not restart move loop");
            WndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, at);
            WndProc(testWindow, WM_CANCELMODE, 0, 0);
            WndProc(testWindow, WM_MOUSEMOVE, MK_LBUTTON, moved);
            Require(audioDragMoves == beforeMove + 1 && !simulatedCapture,
                    "cancelled drag never moves or retains capture");
            WndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, at);
            simulatedCapture = nullptr;
            WndProc(testWindow, WM_CAPTURECHANGED, 0, 0);
            WndProc(testWindow, WM_MOUSEMOVE, MK_LBUTTON, moved);
            Require(!g_audioOnlyDragPending && audioDragMoves == beforeMove + 1,
                    "lost capture cancels pending drag");
            WndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, at);
            WndProc(testWindow, WM_MOUSEMOVE, 0, moved);
            Require(!g_audioOnlyDragPending && !simulatedCapture &&
                    audioDragMoves == beforeMove + 1, "released button cancels drag");
        }
    }
    Require(g_volumePercent == 100 && g_leftVolumePercent == 100 &&
            g_rightVolumePercent == 100, "window drag never changes volume");
    for (const auto point : {POINT{30, 65}, POINT{30, 130}, POINT{220, 130}}) {
        g_volumePercent = 150; g_leftVolumePercent = 85; g_rightVolumePercent = 75;
        const LPARAM at = MAKELPARAM(point.x, point.y);
        const int beforeMove = audioDragMoves;
        WndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, at);
        WndProc(testWindow, WM_LBUTTONUP, 0, at);
        WndProc(testWindow, WM_LBUTTONDBLCLK, MK_LBUTTON, at);
        WndProc(testWindow, WM_LBUTTONUP, 0, at);
        Require(!g_audioOnlyDragPending && !simulatedCapture &&
                audioDragMoves == beforeMove &&
                g_volumePercent == (point.y == 65 ? 100 : 150) &&
                g_leftVolumePercent == (point.y == 130 && point.x == 30 ? 100 : 85) &&
                g_rightVolumePercent == (point.x == 220 ? 100 : 75),
                "complete double-click sequence resets only its control without dragging");
    }
    g_settings.audioOnly = false;
    WndProc(testWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 65));
    Require(!g_audioOnlyDragPending && !simulatedCapture, "video mode does not arm audio drag");
    g_settings.audioOnly = true;

    const UINT originalDpi = monitors[0].dpi;
    for (const UINT dpi : {96u, 120u, 144u, 168u, 192u, 240u, 288u}) {
        monitors[0].dpi = dpi;
        for (const bool borderless : {false, true}) {
            g_settings.borderlessWindow = borderless;
            simulatedStyle = ViewerWindowStyle(g_settings);
            MINMAXINFO limits{};
            WndProc(testWindow, WM_GETMINMAXINFO, 0,
                    reinterpret_cast<LPARAM>(&limits));
            const auto minimum = llcv::audio_only_view::MinimumClientSize(dpi);
            const SIZE outer = OuterSizeForClientPixels(
                minimum.width, minimum.height,
                static_cast<DWORD>(simulatedStyle), 0, dpi);
            Require(limits.ptMinTrackSize.x == outer.cx &&
                    limits.ptMinTrackSize.y == outer.cy,
                    "audio-only minimum follows DPI and window decoration");
            RECT tiny{100, 100, 150, 130};
            WndProc(testWindow, WM_SIZING, WMSZ_BOTTOMRIGHT,
                    reinterpret_cast<LPARAM>(&tiny));
            Require(tiny.right - tiny.left >= outer.cx &&
                    tiny.bottom - tiny.top >= outer.cy - 1,
                    "resize cannot shrink audio controls below DPI minimum");
        }
    }
    monitors[0].dpi = originalDpi;

    // Audio-only geometry must never borrow video sizing/fullscreen policies.
    for (bool relative : {false, true}) for (bool pixelPerfect : {false, true}) {
        Reset(relative, pixelPerfect);
        g_settings.audioOnly = true;
        g_settings.borderlessWindow = true;
        const int videoScale = g_settings.relativeWindowScalePpm;
        const auto generation = Generation();
        const int savedWidth = g_settings.audioOnlyWidth;
        const int savedHeight = g_settings.audioOnlyHeight;
        WndProc(testWindow, WM_SIZE, SIZE_RESTORED, MAKELPARAM(500, 280));
        Require(g_settings.audioOnlyWidth == savedWidth &&
                    g_settings.audioOnlyHeight == savedHeight,
                "automatic audio-only size changes do not overwrite preference");
        Enter();
        RECT rightEdge{100, 100, 700, 450};
        Require(WndProc(testWindow, WM_SIZING, WMSZ_RIGHT,
                        reinterpret_cast<LPARAM>(&rightEdge)) == TRUE &&
                    rightEdge.left == 100 && rightEdge.top == 100 &&
                    std::abs((rightEdge.right - rightEdge.left) * 230 -
                             (rightEdge.bottom - rightEdge.top) * 380) <= 190,
                "audio-only right-edge drag adjusts height proportionally");
        RECT topEdge{100, 100, 700, 500};
        Require(WndProc(testWindow, WM_SIZING, WMSZ_TOP,
                        reinterpret_cast<LPARAM>(&topEdge)) == TRUE &&
                    topEdge.bottom == 500 && topEdge.left == 100 &&
                    std::abs((topEdge.right - topEdge.left) * 230 -
                             (topEdge.bottom - topEdge.top) * 380) <= 190,
                "audio-only top-edge drag adjusts width proportionally");
        RECT audioSizing{100, 100, 740, 460};
        Require(WndProc(testWindow, WM_SIZING, WMSZ_BOTTOMRIGHT,
                        reinterpret_cast<LPARAM>(&audioSizing)) == TRUE &&
                    audioSizing.right - audioSizing.left == 640 &&
                    audioSizing.bottom - audioSizing.top == 387,
                "audio-only border drag locks the client aspect ratio");
        ApplyRect(audioSizing);
        Leave();
        Require(g_settings.audioOnlyWidth == 640 && g_settings.audioOnlyHeight == 387,
                "audio-only remembers normal client size");
        WndProc(testWindow, WM_SIZE, SIZE_MINIMIZED, 0);
        WndProc(testWindow, WM_SIZE, SIZE_MAXIMIZED, MAKELPARAM(1920, 1080));
        Require(g_settings.audioOnlyWidth == 640 && g_settings.audioOnlyHeight == 387,
                "audio-only ignores minimized/maximized sizes");
        const RECT before = simulatedWindowRect;
        WndProc(testWindow, WM_RESTORE_ONE_TO_ONE, 0, 0);
        ToggleFullscreen(testWindow);
        NormalizeWindowSize(testWindow, true);
        Require(EqualRect(&before, &simulatedWindowRect) && !g_fullscreen,
                "video geometry commands leave audio-only unchanged");
        SIZE pending{800, 500};
        Require(WndProc(testWindow, WM_GETDPISCALEDSIZE, 144,
                        reinterpret_cast<LPARAM>(&pending)) == FALSE,
                "audio-only DPI preflight does not apply video dimensions");
        RECT suggested{100, 100, 900, 600};
        WndProc(testWindow, WM_DPICHANGED, MAKELONG(144, 144),
                reinterpret_cast<LPARAM>(&suggested));
        Require(simulatedWindowRect.left == suggested.left &&
                    simulatedWindowRect.top == suggested.top &&
                    simulatedWindowRect.right - simulatedWindowRect.left == 800 &&
                    simulatedWindowRect.bottom - simulatedWindowRect.top == 484,
                "audio-only DPI move keeps the suggested origin and fixed aspect");
        Require(g_settings.audioOnlyWidth == 640 && g_settings.audioOnlyHeight == 387,
                "DPI resize does not replace user-chosen audio-only dimensions");
        Require(g_settings.relativeWindowScalePpm == videoScale &&
                    Generation() == generation && g_outputTransition.Depth() == 0,
                "audio-only geometry does not alter video scale or rebuild output");

        g_settings.windowSnap = true;
        cursorPoint = POINT{25, 110};
        RECT moving{10, 100, 650, 460};
        Enter();
        Require(WndProc(testWindow, WM_MOVING, 0,
                        reinterpret_cast<LPARAM>(&moving)) == TRUE &&
                    moving.left == monitors[0].rect.left &&
                    moving.right - moving.left == 640,
                "audio-only retains window edge snap without video resizing");
        Leave();
    Require(g_settings.relativeWindowScalePpm == videoScale &&
                    Generation() == generation,
                "audio-only snap leaves video settings and output untouched");
    }

    Reset(false, false);
    g_suppressSettingsSave = true;
    PersistWindowPosition(testWindow);
    Require(!g_settings.hasWindowPosition,
            "diagnostic run does not persist window placement");
    g_suppressSettingsSave = false;

    // Dedicated audio-only controls scale with the resized window.
    HDC screenDc = GetDC(nullptr);
    Require(screenDc != nullptr, "desktop DC for audio-only paint check");
    HDC panelDc = CreateCompatibleDC(screenDc);
    const RECT largeClient{0, 0, 1200, 730};
    const auto panel = llcv::audio_only_view::ContentRect(
        largeClient.right, largeClient.bottom);
    const int panelWidth = panel.right - panel.left;
    const int panelHeight = panel.bottom - panel.top;
    HBITMAP panelBitmap = CreateCompatibleBitmap(
        screenDc, panelWidth, panelHeight);
    Require(panelDc != nullptr && panelBitmap != nullptr,
            "scaled audio meter back buffer");
    HGDIOBJ previousBitmap = SelectObject(panelDc, panelBitmap);
    const RECT panelPixels{0, 0, panelWidth, panelHeight};
    FillRect(panelDc, &panelPixels,
             reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    g_audioOsdVisible.store(false, std::memory_order_release);
    SetViewportOrgEx(panelDc, -panel.left, -panel.top, nullptr);
    PaintAudioOnlyView(panelDc, panel);
    SetViewportOrgEx(panelDc, 0, 0, nullptr);
    Require(GetPixel(panelDc, MulDiv(365, panelWidth, 380),
                     MulDiv(200, panelHeight, 230)) == RGB(12, 15, 19),
            "audio-only view renders its full-size background");
    Require(GetPixel(panelDc, MulDiv(200, panelWidth, 380),
                     MulDiv(94, panelHeight, 230)) == RGB(30, 36, 44),
            "audio-only master control renders at its scaled size");
    g_settings.audioOnly = true;
    ToggleAudioOsd();
    Require(!g_audioOsdVisible.load(std::memory_order_acquire),
            "F3 cannot hide the dedicated audio-only screen");
    SelectObject(panelDc, previousBitmap);
    DeleteObject(panelBitmap);
    DeleteDC(panelDc);
    ReleaseDC(nullptr, screenDc);

    // Replay both DPI/move orderings with negative monitor coordinates and
    // equal/mixed DPI. These are supplied event orders, not a Windows emulator.
    unsigned scenarios=0;
    for(int layout=0;layout<3;++layout)
    for(int mixedDpi=0;mixedDpi<2;++mixedDpi)
    for(int relative=0;relative<2;++relative)
    for(int order=0;order<2;++order)
    for(int repeat=0;repeat<100;++repeat) {
        monitors[1]={layout==0?RECT{1920,0,4480,1440}:
                     layout==1?RECT{-2560,0,0,1440}:RECT{0,-1440,2560,0},
                     mixedDpi?144u:96u};
        Reset(relative!=0,true);Enter();
        cursorPoint={monitors[1].rect.left+100,monitors[1].rect.top+100};
        if(order==0) {DpiTo(monitors[1].dpi,1);MoveTo(1);}
        else {MoveTo(1);DpiTo(monitors[1].dpi,1);}
        Leave();
        const auto stable=Generation();
        for(int idle=0;idle<20;++idle)
            WndProc(testWindow,WM_SIZE,SIZE_RESTORED,
                MAKELPARAM(simulatedWindowRect.right-simulatedWindowRect.left,simulatedWindowRect.bottom-simulatedWindowRect.top));
        Require(Generation()==stable,"no duplicate-size rebuild loop after settling");
        Require(g_outputTransition.Depth()==0&&!g_outputTransition.Pending(),"no stranded transition");
        Require(simulatedWindowRect.right>simulatedWindowRect.left&&simulatedWindowRect.bottom>simulatedWindowRect.top,"valid geometry");
        ++scenarios;
    }
    std::printf("PASS %u topology/order replays: positive geometry, no stranded transition, no idle rebuild loop\n",scenarios);
    std::puts("LIMIT: generations are rebuild requests, not measured GPU recreations or display signal failures.");
    return 0;
}
