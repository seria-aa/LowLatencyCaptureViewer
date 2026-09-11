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
#define GetWindowRect FakeGetWindowRect
#define GetClientRect FakeGetClientRect
#define GetWindowLongPtrW FakeGetWindowLongPtrW
#define SetWindowPos FakeSetWindowPos
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
#undef GetWindowRect
#undef GetClientRect
#undef GetWindowLongPtrW
#undef SetWindowPos

static void Require(bool value,const char* message) {
    if(!value) {std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}
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
