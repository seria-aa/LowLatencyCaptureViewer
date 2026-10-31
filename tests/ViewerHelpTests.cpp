#include "ui/ViewerHelpWindow.h"
#include "ui/ViewerHelpGeometry.h"
#include "ui/AppPalette.h"
#include "ui/SettingsFonts.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <string>
#include <objidl.h>
#include <gdiplus.h>
#include <filesystem>
#include <vector>
#include <imm.h>
namespace llcv::viewer_help {
struct WindowTestAccess {
    static void LayoutAtDpi(Window& window,UINT dpi) { window.Layout(dpi); }
    static void Fit(Window& window,const RECT& work,const RECT* preferred=nullptr) { window.FitToWorkArea(work,preferred); }
    static SIZE Minimum(const Window& window,const RECT& work) { return window.MinimumTrackSize(work); }
    static UINT Dpi(HWND body) {
        const auto* window=reinterpret_cast<Window*>(GetWindowLongPtrW(body,GWLP_USERDATA));
        return window ? window->layoutDpi_ : GetDpiForWindow(body);
    }
};
}
using namespace llcv::viewer_help;
namespace {
void Check(bool ok,const char* what) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
// Synthetic messages carry their own modifiers: real desktop key events can
// update GetKeyState even after SetKeyboardState during native window tests.
struct TestMessage : MSG { unsigned modifiers=0; };
TestMessage Key(HWND target,UINT key,bool repeat=false,unsigned modifiers=0) {
    TestMessage message{}; message.modifiers=modifiers;
    message.hwnd=target; message.message=WM_KEYDOWN;
    message.wParam=key; message.lParam=repeat ? (LPARAM{1}<<30) : 0; return message;
}
class TestWindow : public Window {
public:
    using Window::ProcessMessage;
    bool ProcessMessage(TestMessage& message,HWND owner,bool english,const wchar_t* version,bool light=false,
                        MessageContext context=MessageContext::Viewer) {
        return Window::ProcessMessage(message,owner,english,version,light,message.modifiers,context);
    }
};
bool IsScreenshotRequest(const TestMessage& message,bool audioOnly,
                         const llcv::shortcuts::Bindings& bindings=llcv::shortcuts::Defaults) {
    return llcv::viewer_help::IsScreenshotRequest(message,audioOnly,bindings,message.modifiers);
}
std::wstring Text(HWND window) {
    wchar_t text[4096]{}; GetWindowTextW(window,text,4096); return text;
}
void DrainNativeMessages() {
    // The real viewer continuously pumps native UI/IME cleanup messages.
    // Lifecycle tests must do likewise before sampling process-wide resources.
    MSG pending{};
    for (unsigned count=0;count<256 && PeekMessageW(&pending,nullptr,0,0,PM_REMOVE);++count) {
        Check(pending.message!=WM_QUIT,"help never posts quit while draining native cleanup");
        TranslateMessage(&pending);
        DispatchMessageW(&pending);
    }
}
BOOL CALLBACK FindRemainingHelpWindow(HWND hwnd,LPARAM found) {
    wchar_t name[64]{}; GetClassNameW(hwnd,name,64);
    if (!wcscmp(name,L"LLCV.ViewerHelp") || !wcscmp(name,L"LLCV.ViewerHelp.Content")) {
        *reinterpret_cast<bool*>(found)=true;
        return FALSE;
    }
    return TRUE;
}
struct OwnedHelpResources {
    std::vector<HWND> windows;
    std::vector<HFONT> fonts;
    std::vector<HBRUSH> brushes;
    bool Released() const {
        // Check exact handles, not process totals: even one surviving help
        // child fails independently of unrelated Windows accessibility hooks.
        for (auto window:windows) if (IsWindow(window)) return false;
        LOGFONTW fontInfo{};
        for (auto font:fonts) if (GetObjectW(font,sizeof(fontInfo),&fontInfo)) return false;
        LOGBRUSH brushInfo{};
        for (auto brush:brushes) if (GetObjectW(brush,sizeof(brushInfo),&brushInfo)) return false;
        return true;
    }
};
BOOL CALLBACK CaptureHelpChild(HWND window,LPARAM context) {
    auto& resources=*reinterpret_cast<OwnedHelpResources*>(context);
    resources.windows.push_back(window);
    const auto font=reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0));
    if (font && std::find(resources.fonts.begin(),resources.fonts.end(),font)==resources.fonts.end())
        resources.fonts.push_back(font);
    return TRUE;
}
OwnedHelpResources CaptureHelpResources(HWND window) {
    OwnedHelpResources resources;
    resources.windows.push_back(window);
    EnumChildWindows(window,CaptureHelpChild,reinterpret_cast<LPARAM>(&resources));
    Check(resources.fonts.size()==4,"four distinct role fonts are owned");
    const HWND body=GetDlgItem(window,100);
    HDC dc=GetDC(body);
    for (int id:{201,204,205}) resources.brushes.push_back(reinterpret_cast<HBRUSH>(
        SendMessageW(body,WM_CTLCOLORSTATIC,reinterpret_cast<WPARAM>(dc),
            reinterpret_cast<LPARAM>(GetDlgItem(body,id)))));
    ReleaseDC(body,dc);
    Check(!resources.Released(),"owned-resource leak check detects a live help window");
    return resources;
}
void CheckHelpResourcesReleased(const OwnedHelpResources& resources) {
    Check(resources.Released(),"every owned help window, font and brush is released after each close");
}
BOOL CALLBACK TraceWindow(HWND hwnd,LPARAM) {
    wchar_t name[128]{}; GetClassNameW(hwnd,name,128);
    std::printf("NATIVE HWND=%p owner=%p class=",hwnd,GetWindow(hwnd,GW_OWNER));
    for (const wchar_t* p=name;*p;++p) std::putchar(*p<128 ? char(*p) : '?');
    std::puts("");
    return TRUE;
}
void CheckPalette(HWND body,bool light) {
    HIGHCONTRASTW hc{sizeof(hc)};
    const bool highContrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0) && (hc.dwFlags&HCF_HIGHCONTRASTON);
    struct Sample { int id; COLORREF surface,ink; };
    const auto p=llcv::ui::ResolvePalette(light,highContrast);
    const Sample samples[]{{201,p.kBackground,p.kText},{203,p.kBackground,p.kSecondary},
        {204,p.kCard,p.kText},{205,p.kControl,p.kAccent},{206,p.kCard,p.kText}};
    HDC dc=GetDC(body);
    for (const auto& sample:samples) {
        HBRUSH brush=reinterpret_cast<HBRUSH>(SendMessageW(body,WM_CTLCOLORSTATIC,
            reinterpret_cast<WPARAM>(dc),reinterpret_cast<LPARAM>(GetDlgItem(body,sample.id))));
        LOGBRUSH value{}; Check(GetObjectW(brush,sizeof(value),&value)!=0,"help surface brush");
        const COLORREF expectedBackground=highContrast ? GetSysColor(COLOR_WINDOW) : sample.surface;
        Check(value.lbColor==expectedBackground && GetBkColor(dc)==expectedBackground,"help uses shared app surfaces");
        Check(GetTextColor(dc)==sample.ink,"help uses shared app text/accent");
    }
    ReleaseDC(body,dc);
}
void CheckFields(HWND body) {
    RECT bounds{}; GetClientRect(body,&bounds);
    Check(!(GetWindowLongPtrW(body,GWL_STYLE)&(WS_VSCROLL|WS_HSCROLL)),"guide has no scrollbars");
    std::vector<RECT> fields;
    HDC dc=GetDC(body);
    for (HWND child=GetWindow(body,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        RECT r{}; GetWindowRect(child,&r); MapWindowPoints(nullptr,body,reinterpret_cast<POINT*>(&r),2);
        Check(r.left>=0 && r.right<=bounds.right,"text stays within body width");
        if (r.top<0 || r.bottom>bounds.bottom-MulDiv(16,WindowTestAccess::Dpi(body),96))
            std::fwprintf(stderr,L"Offscreen help field: %ls at %ld..%ld, body %ld\n",Text(child).c_str(),r.top,r.bottom,bounds.bottom);
        Check(r.top>=0 && r.bottom<=bounds.bottom-MulDiv(16,WindowTestAccess::Dpi(body),96),
              "all guide text is visible at once with bottom margin");
        for (const auto& other:fields) {
            RECT overlap{};
            Check(!IntersectRect(&overlap,&r,&other),"guide text controls never overlap");
        }
        fields.push_back(r);
        const auto font=reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0));
        HGDIOBJ old=SelectObject(dc,font);
        RECT needed{0,0,r.right-r.left,0};
        const auto text=Text(child);
        DrawTextW(dc,text.c_str(),-1,&needed,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        if(needed.right>r.right-r.left)
            std::fwprintf(stderr,L"Clipped help field: %ls needs %ld px, has %ld px\n",text.c_str(),needed.right,r.right-r.left);
        Check(needed.right<=r.right-r.left,"all help text fits control width");
        Check(needed.bottom<=r.bottom-r.top,"all wrapped text fits control height");
        SelectObject(dc,old);
    }
    ReleaseDC(body,dc);
    // Each keyboard description is beside its key, not a prose paragraph.
    for (int i=0;i<static_cast<int>(llcv::viewer_help::kShortcuts.size());++i) {
        RECT key{},action{};
        GetWindowRect(GetDlgItem(body,205+2*i),&key);
        GetWindowRect(GetDlgItem(body,206+2*i),&action);
        Check(key.right<action.left && abs((key.top+key.bottom)-(action.top+action.bottom))<=1,
              "key/action text is vertically centered on the same row");
    }
}
void ResizeClient(HWND window,int width,int height) {
    const UINT dpi=GetDpiForWindow(window);
    RECT r{0,0,MulDiv(width,dpi,96),MulDiv(height,dpi,96)};
    Check(AdjustWindowRectExForDpi(&r,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,
          static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),dpi)!=FALSE,"client-size test frame");
    SetWindowPos(window,nullptr,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
}
void Snapshot(HWND window,const std::filesystem::path& path) {
    RECT r{}; GetWindowRect(window,&r); HDC dc=GetDC(window),memory=CreateCompatibleDC(dc);
    HBITMAP bitmap=CreateCompatibleBitmap(dc,r.right-r.left,r.bottom-r.top);
    HGDIOBJ old=SelectObject(memory,bitmap);
    SendMessageW(window,WM_PRINT,reinterpret_cast<WPARAM>(memory),PRF_NONCLIENT|PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
    SelectObject(memory,old);
    const int width=r.right-r.left,height=r.bottom-r.top;
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    std::vector<DWORD> pixels(size_t(width)*height);
    Check(GetDIBits(dc,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS)==height,"help preview pixels readable");
    const HWND body=GetDlgItem(window,100);
    HDC textDc=GetDC(body);
    for(HWND child=GetWindow(body,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        RECT field{}; GetWindowRect(child,&field); OffsetRect(&field,-r.left,-r.top);
        SendMessageW(body,WM_CTLCOLORSTATIC,reinterpret_cast<WPARAM>(textDc),reinterpret_cast<LPARAM>(child));
        const COLORREF ink=GetTextColor(textDc);
        bool found=false;
        for(int y=std::max(0L,field.top);y<std::min(LONG(height),field.bottom) && !found;++y)
            for(int x=std::max(0L,field.left);x<std::min(LONG(width),field.right);++x) {
                const DWORD pixel=pixels[size_t(y)*width+x];
                if(abs(int((pixel>>16)&255)-GetRValue(ink))<16 &&
                   abs(int((pixel>>8)&255)-GetGValue(ink))<16 && abs(int(pixel&255)-GetBValue(ink))<16) {
                    found=true; break;
                }
            }
        if(!found) std::fwprintf(stderr,L"Missing help preview text: %ls\n",Text(child).c_str());
        Check(found,"every guide field has visible text in rendered preview");
    }
    ReleaseDC(body,textDc);
    UINT count=0,bytes=0; Gdiplus::GetImageEncodersSize(&count,&bytes);
    std::vector<BYTE> codecs(bytes); auto* list=reinterpret_cast<Gdiplus::ImageCodecInfo*>(codecs.data());
    Gdiplus::GetImageEncoders(count,bytes,list); bool saved=false;
    {
        Gdiplus::Bitmap image(bitmap,nullptr);
        for (UINT i=0;i<count;++i) if (!wcscmp(list[i].MimeType,L"image/png")) {
            saved=image.Save(path.c_str(),&list[i].Clsid)==Gdiplus::Ok; break;
        }
    }
    Check(saved,"rendered help preview saved");
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window,dc);
}
void CheckFooter(HWND window) {
    HWND footer=nullptr;
    for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        wchar_t name[32]{}; GetClassNameW(child,name,32);
        if(!_wcsicmp(name,L"STATIC")) footer=child;
    }
    Check(footer!=nullptr,"help footer exists");
    RECT bounds{},root{},body{},button{};
    GetClientRect(footer,&bounds); GetClientRect(window,&root);
    HDC dc=GetDC(footer);
    const auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(footer,WM_GETFONT,0,0)));
    RECT needed{0,0,bounds.right,0}; const auto text=Text(footer);
    DrawTextW(dc,text.c_str(),-1,&needed,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    SelectObject(dc,old); ReleaseDC(footer,dc);
    Check(needed.right<=bounds.right && needed.bottom<=bounds.bottom,"entire footer fits after remapping/resizing");
    GetWindowRect(footer,&bounds); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);
    GetWindowRect(GetDlgItem(window,100),&body); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&body),2);
    GetWindowRect(GetDlgItem(window,IDCANCEL),&button); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&button),2);
    Check(bounds.top>=body.bottom && bounds.bottom<=root.bottom && bounds.right<button.left &&
          button.top>=body.bottom && button.bottom<=root.bottom,"footer, close button and body never overlap");
}
void TestSettingsKeyRouting(TestWindow& help,HWND owner) {
    namespace keys=llcv::shortcuts;
    HWND combo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST,0,0,100,100,owner,nullptr,nullptr,nullptr);
    HWND button=CreateWindowW(L"BUTTON",L"",WS_CHILD|BS_PUSHBUTTON,0,0,100,30,owner,nullptr,nullptr,nullptr);
    Check(combo && button,"native settings input fixtures");
    for(unsigned binding:{unsigned(VK_TAB),keys::Shift|VK_TAB,unsigned(VK_RETURN),unsigned(VK_SPACE),
            unsigned(VK_ESCAPE),unsigned(VK_LEFT),unsigned(VK_HOME),unsigned(VK_BACK),unsigned(VK_DELETE),
            unsigned('A'),keys::Ctrl|'A',keys::Ctrl|'V',keys::Alt|'A',unsigned(VK_NUMPAD2),unsigned(VK_F4)}) {
        auto custom=keys::Defaults;
        for(size_t i=1;i<custom.size();++i) if(custom[i]==binding) custom[i]=VK_F24;
        custom[0]=binding;
        Check(keys::Valid(custom),"routing case uses assignable unique bindings");
        help.SetBindings(custom);
        for(HWND target:{combo,button,owner}) {
            auto key=Key(target,binding&0xff,false,binding&0x700);
            if(binding&keys::Alt) key.message=WM_SYSKEYDOWN;
            Check(!help.ProcessMessage(key,owner,false,L"v-test",false,MessageContext::Settings) && !help.Handle(),
                  "settings retains native navigation, editing and combo input");
        }
        help.Toggle(owner,false,L"v-test",SW_HIDE);
        auto key=Key(GetDlgItem(help.Handle(),100),binding&0xff,false,binding&0x700);
        Check(help.ProcessMessage(key,owner,false,L"v-test",false,MessageContext::Settings) && !help.Handle(),
              "configured key still closes focused guide from settings");
        help.Toggle(owner,false,L"v-test",SW_HIDE);
        key=Key(owner,binding&0xff,false,binding&0x700);
        Check(help.ProcessMessage(key,owner,false,L"v-test") && !help.Handle(),"viewer retains every custom help binding");
    }
    for(unsigned binding:{unsigned(VK_F1),keys::Ctrl|VK_F9}) {
        auto custom=keys::Defaults; custom[0]=binding; help.SetBindings(custom);
        help.Toggle(owner,false,L"v-test",SW_HIDE);
        auto key=Key(combo,binding&0xff,false,binding&0x700);
        Check(help.ProcessMessage(key,owner,false,L"v-test",false,MessageContext::Settings) && !help.Handle(),
              "function-key help remains available in settings");
    }
    DestroyWindow(combo); DestroyWindow(button); help.SetBindings(keys::Defaults);
}
void CheckInside(const RECT& rect,const RECT& work) {
    if(rect.left<work.left || rect.top<work.top || rect.right>work.right || rect.bottom>work.bottom)
        std::fprintf(stderr,"Outside work area: frame %ld,%ld..%ld,%ld work %ld,%ld..%ld,%ld\n",
            rect.left,rect.top,rect.right,rect.bottom,work.left,work.top,work.right,work.bottom);
    Check(rect.left>=work.left && rect.top>=work.top && rect.right<=work.right && rect.bottom<=work.bottom,
          "guide frame stays inside monitor work area");
    Check(rect.right>rect.left && rect.bottom>rect.top,"fitted guide has a positive size");
}
void TestDisplayLayouts(TestWindow& help,HWND owner,const char* snapshots) {
    namespace keys=llcv::shortcuts;
    unsigned cases=0;
    const SIZE displays[]{{800,600},{1280,720},{1366,768},{1920,1080},{2560,1440},{3840,2160},{1080,1920}};
    for(bool english:{false,true}) for(bool light:{false,true}) for(int bindings:{0,1,2}) {
        auto custom=keys::Defaults;
        if(bindings) {
            if(bindings==2) for(size_t i=0;i<custom.size();++i)
                custom[i]=keys::Ctrl|keys::Alt|keys::Shift|(VK_F13+unsigned(i));
            custom[0]=keys::Ctrl|keys::Alt|keys::Shift|VK_RIGHT;
            custom[8]=keys::Ctrl|keys::Alt|keys::Shift|VK_HOME;
        }
        Check(keys::Valid(custom),"display cases use valid bindings");
        help.SetBindings(custom); help.Toggle(owner,english,L"v-test",SW_HIDE,light);
        const HWND dialog=help.Handle(),body=GetDlgItem(dialog,100);
        for(const auto display:displays) for(UINT dpi:{96u,120u,144u,168u,192u,240u,288u}) for(bool negative:{false,true}) {
            const LONG x=negative ? -display.cx : 40, y=negative ? -120 : 24;
            // Model a taskbar at either the top or bottom, including a monitor
            // left of/above the primary. No OS display/DPI settings are changed.
            const RECT work{x,y+32,x+display.cx,y+display.cy-32};
            RECT preferred{work.right-100,work.bottom-100,work.right-100+MulDiv(820,dpi,96),
                work.bottom-100+MulDiv(680,dpi,96)};
            const RECT fitted=detail::FitToWorkArea(preferred,work);
            CheckInside(fitted,work);
            const auto again=detail::FitToWorkArea(fitted,work);
            Check(EqualRect(&fitted,&again)!=FALSE,"work-area fitting is idempotent");
            WindowTestAccess::Fit(help,work,&preferred);
            WindowTestAccess::LayoutAtDpi(help,dpi);
            RECT actual{},client{}; GetWindowRect(dialog,&actual); GetClientRect(dialog,&client);
            CheckInside(actual,work);
            const UINT effective=WindowTestAccess::Dpi(body);
            Check(effective<=dpi && effective==detail::ContentDpi(dpi,client.right,client.bottom),
                  "guide content never exceeds window DPI and fits small clients");
            LOGFONTW font{};
            Check(GetObjectW(reinterpret_cast<HFONT>(SendMessageW(body,WM_GETFONT,0,0)),sizeof(font),&font)!=0 &&
                  font.lfHeight==-std::max(1,MulDiv(10,effective,72)),
                  "font pixels follow the fitted guide DPI, not a stale monitor DPI");
            Check(MulDiv(detail::kMinimumClientWidth,effective,96)<=client.right &&
                  MulDiv(detail::kMinimumClientHeight,effective,96)<=client.bottom,
                  "fitted logical minimum cannot exceed the physical client");
            const SIZE minimum=WindowTestAccess::Minimum(help,work);
            Check(minimum.cx<=work.right-work.left && minimum.cy<=work.bottom-work.top,
                  "minimum tracking size never exceeds available work area");
            CheckFields(body); CheckFooter(dialog); CheckPalette(body,light);
            // Return to normal DPI and fit a different monitor. This also
            // checks font replacement when both scaling up and scaling down.
            const RECT normal{100,80,100+1920,80+1040};
            WindowTestAccess::Fit(help,normal,&preferred);
            WindowTestAccess::LayoutAtDpi(help,96);
            CheckFields(body); CheckFooter(dialog);
            ++cases;
            if(snapshots && !negative && !light && bindings!=1 && display.cx==1920 && dpi==288) {
                WindowTestAccess::Fit(help,work,&preferred);
                WindowTestAccess::LayoutAtDpi(help,dpi);
                Snapshot(dialog,std::filesystem::path(snapshots)/(std::string("help-1080p-300-")+
                    (english ? "en" : "ko")+(bindings ? "-long.png" : ".png")));
            }
        }
        // Exercise the actual notification handlers on only this hidden test
        // window. The real monitor layout and viewer window are not touched.
        for(UINT message:{UINT(WM_DISPLAYCHANGE),UINT(WM_SETTINGCHANGE),UINT(WM_EXITSIZEMOVE),UINT(WM_DPICHANGED)}) {
            RECT offscreen{-30000,-30000,-28000,-28000};
            SetWindowPos(dialog,nullptr,offscreen.left,offscreen.top,900,700,SWP_NOZORDER|SWP_NOACTIVATE);
            SendMessageW(dialog,message,message==WM_DPICHANGED ? MAKELONG(192,192) :
                message==WM_SETTINGCHANGE ? SPI_SETWORKAREA : 0,
                message==WM_DPICHANGED ? reinterpret_cast<LPARAM>(&offscreen) : 0);
            MONITORINFO monitor{sizeof(monitor)};
            Check(GetMonitorInfoW(MonitorFromWindow(dialog,MONITOR_DEFAULTTONEAREST),&monitor)!=FALSE,"notification monitor");
            RECT actual{}; GetWindowRect(dialog,&actual); CheckInside(actual,monitor.rcWork);
            Check(help.Handle()==dialog && IsWindowEnabled(owner),"display changes preserve guide and viewer owner");
            CheckFields(body); CheckFooter(dialog);
        }
        help.Close();
    }
    std::printf("%u monitor work-area / DPI / language / theme / binding layouts and live notification paths passed.\n",cases);
}
}
int main(int argc,char** argv) {
    // Diagnostic mode records a complete resource trajectory without changing
    // the normal regression assertion. It is not a passing leak certification.
    const bool resourceDiagnostics=argc>1 && std::strcmp(argv[1],"--resource-diagnostics")==0;
    const bool displayLayouts=argc>1 && std::strcmp(argv[1],"--display-layout")==0;
    const bool nativeIme=resourceDiagnostics || (argc>1 && std::strcmp(argv[1],"--with-ime")==0);
    const bool saveSnapshots=displayLayouts ? argc>2 : argc>1 && !nativeIme;
    // Isolate the strict process-wide GDI assertion from Windows' IME
    // service, which lazily creates persistent UI after focus/key messages.
    // This affects only this test thread, never the app or user IME settings.
    // A separate --with-ime run keeps the real input path and verifies every
    // app-owned font/brush/window is released, without asserting on OS caches.
    if (!nativeIme) Check(ImmDisableIME(GetCurrentThreadId())!=FALSE,"isolate test-thread IME resources");
    const auto traceResources=[resourceDiagnostics](const char* stage,bool english,unsigned trial) {
        if (!resourceDiagnostics) return;
        std::printf("RESOURCE language=%s trial=%u stage=%s gdi=%lu user=%lu\n",
            english ? "en" : "ko",trial,stage,
            GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS),
            GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS));
        std::fflush(stdout);
    };
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput startup; ULONG_PTR token=0;
    // The encoder owns background resources unrelated to help-window lifetime.
    // Do not start it in lifecycle tests that never save a preview image.
    if (saveSnapshots)
        Check(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"preview encoder");
    TestWindow help;
    HWND owner=CreateWindowW(L"STATIC",L"Hidden help-test owner",WS_POPUP,
        0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(owner!=nullptr,"owner");
    if(displayLayouts) {
        TestDisplayLayouts(help,owner,saveSnapshots ? argv[2] : nullptr);
        DestroyWindow(owner);
        if(token) Gdiplus::GdiplusShutdown(token);
        return 0;
    }
    Check(kHelpKey==VK_F1 && kScreenshotKey==VK_F12,"public shortcut bindings");
    DWORD stableGdi=0,stableUser=0,lastUserDiagnostic=0;
    for (bool light:{false,true}) for (bool english:{false,true}) {
        Check(std::wcsstr(Shortcuts(english),L"F1 ") && std::wcsstr(Shortcuts(english),L"F12 ") &&
            !std::wcsstr(Shortcuts(english),L"F8 "),"one shared up-to-date shortcut list");
        for (unsigned trial=0;trial<20;++trial) {
            traceResources("before-open",english,trial);
            help.Toggle(owner,english,saveSnapshots ? L"UI preview" : L"v-test",SW_HIDE,light);
            if (trial<3) traceResources("after-open",english,trial);
            HWND dialog=help.Handle(); Check(IsWindow(dialog)!=FALSE,"modeless creation");
            Check(GetWindow(dialog,GW_OWNER)==owner && IsWindowEnabled(owner),"owned, nonblocking, owner enabled");
            HWND edit=GetDlgItem(dialog,100);
            Check(IsWindow(edit) && !FindWindowExW(dialog,nullptr,L"EDIT",nullptr),"sectioned content replaces text box");
            std::wstring body;
            for (HWND field=GetWindow(edit,GW_CHILD);field;field=GetWindow(field,GW_HWNDNEXT)) body+=Text(field)+L"\n";
            Check(body.find(L"F12")!=std::wstring::npos && body.find(L"HDR")!=std::wstring::npos &&
                body.find(L"LowLatencyCaptureViewer")!=std::wstring::npos,"help includes screenshot details");
            Check(Text(GetDlgItem(edit,204))==(english ? L"Keyboard shortcuts" : L"키보드 단축키"),"keyboard section heading");
            Check(body.find(english ? L"Mouse controls" : L"마우스 조작")!=std::wstring::npos &&
                body.find(english ? L"Screenshots" : L"스크린샷")!=std::wstring::npos,"separate mouse and screenshot sections");
            Check(body.find(english ? L"Double-click" : L"두 번 클릭")==std::wstring::npos &&
                body.find(english ? L"Reset a volume control" : L"음량 영역을 100%로 복원")==std::wstring::npos,
                "common F1 guide omits audio-only double-click instructions");
            const HFONT font=reinterpret_cast<HFONT>(SendMessageW(edit,WM_GETFONT,0,0));
            LOGFONTW fontInfo{};
            Check(font && GetObjectW(font,sizeof(fontInfo),&fontInfo),"owned help font");
            Check(!wcscmp(fontInfo.lfFaceName,llcv::settings_ui::SettingsFontFamily(FW_MEDIUM,english)) &&
                fontInfo.lfWeight==FW_MEDIUM && fontInfo.lfQuality==CLEARTYPE_QUALITY,
                "help matches settings font family, weight and raster quality");
            Check(Text(GetDlgItem(edit,202))==(saveSnapshots ? L"UI preview" : L"v-test"),"runtime version badge");
            CheckFields(edit);
            CheckPalette(edit,light);
            help.SetLightTheme(!light); CheckPalette(edit,!light);
            help.SetLightTheme(light); CheckPalette(edit,light);
            const auto stem=std::string(light ? "help-light-" : "help-dark-")+(english ? "en" : "ko");
            if (saveSnapshots && !trial) Snapshot(dialog,std::filesystem::path(argv[1])/(stem+".png"));
            RECT initial{}; GetWindowRect(dialog,&initial);
            ResizeClient(dialog,760,640);
            CheckFields(edit);
            RECT beforeWheel{}; GetWindowRect(GetDlgItem(edit,204),&beforeWheel);
            SendMessageW(edit,WM_VSCROLL,SB_BOTTOM,0);
            SendMessageW(edit,WM_MOUSEWHEEL,MAKEWPARAM(0,-WHEEL_DELTA),0);
            RECT afterWheel{}; GetWindowRect(GetDlgItem(edit,204),&afterWheel);
            Check(EqualRect(&beforeWheel,&afterWheel)!=FALSE,"wheel/scroll messages cannot shift guide content");
            CheckFields(edit);
            if (saveSnapshots && !trial) Snapshot(dialog,std::filesystem::path(argv[1])/(stem+"-narrow.png"));
            SetWindowPos(dialog,nullptr,0,0,initial.right-initial.left,initial.bottom-initial.top,SWP_NOMOVE|SWP_NOACTIVATE|SWP_NOZORDER);
            CheckFields(edit);
            auto shot=Key(edit,VK_F12);
            const bool consumed=help.ProcessMessage(shot,owner,english,L"v-test",light);
            const bool requested=IsScreenshotRequest(shot,false);
            Check(!consumed && requested,"F12 reaches source screenshot handler");
            Check(!IsScreenshotRequest(shot,true),"audio-only screenshot ignored");
            shot=Key(edit,VK_F12,true); Check(!IsScreenshotRequest(shot,false),"held F12 ignored");
            shot=Key(edit,VK_F8); Check(!IsScreenshotRequest(shot,false),"old public F8 no longer captures");
            MSG wheel{}; wheel.hwnd=edit; wheel.message=WM_MOUSEWHEEL; wheel.wParam=MAKEWPARAM(0,WHEEL_DELTA);
            Check(help.ProcessMessage(wheel,owner,english,L"v-test",light),"help wheel never reaches volume fast path");
            auto tab=Key(edit,VK_TAB);
            Check(help.ProcessMessage(tab,owner,english,L"v-test",light),"help Tab never reaches diagnostic toggle");
            auto f1=Key(edit,VK_F1,true);
            Check(help.ProcessMessage(f1,owner,english,L"v-test",light) && help.Handle()==dialog,"F1 repeat does not toggle");
            auto owned=CaptureHelpResources(dialog);
            auto escape=Key(edit,VK_ESCAPE);
            Check(help.ProcessMessage(escape,owner,english,L"v-test",light) && !help.Handle() && IsWindow(owner),"Esc closes only help");
            if (trial<3) traceResources("after-first-close",english,trial);
            CheckHelpResourcesReleased(owned);
            escape=Key(owner,VK_ESCAPE,true);
            Check(help.ProcessMessage(escape,owner,english,L"v-test"),"Esc repeat cannot exit viewer after dismiss");
            escape.message=WM_KEYUP;
            Check(help.ProcessMessage(escape,owner,english,L"v-test"),"dismiss key-up consumed");
            escape=Key(owner,VK_ESCAPE);
            Check(!help.ProcessMessage(escape,owner,english,L"v-test"),"next deliberate Esc retains viewer behavior");
            help.Toggle(owner,english,L"v-test",SW_HIDE,light);
            owned=CaptureHelpResources(help.Handle());
            f1=Key(owner,VK_F1);
            Check(help.ProcessMessage(f1,owner,english,L"v-test") && !help.Handle(),"F1 closes existing help");
            CheckHelpResourcesReleased(owned);
            help.Toggle(owner,english,L"v-test",SW_HIDE,light);
            owned=CaptureHelpResources(help.Handle());
            SendMessageW(help.Handle(),WM_COMMAND,IDCANCEL,0);
            Check(!help.Handle() && IsWindow(owner),"close button preserves viewer");
            CheckHelpResourcesReleased(owned);
            help.Toggle(owner,english,L"v-test",SW_HIDE,light);
            owned=CaptureHelpResources(help.Handle());
            SendMessageW(help.Handle(),WM_CLOSE,0,0);
            Check(!help.Handle(),"title-bar close");
            CheckHelpResourcesReleased(owned);
            DrainNativeMessages();
            bool remainingHelp=false;
            EnumThreadWindows(GetCurrentThreadId(),FindRemainingHelpWindow,
                reinterpret_cast<LPARAM>(&remainingHelp));
            Check(!remainingHelp,"all owned help windows were destroyed before resource sampling");
            const DWORD gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
            const DWORD user=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
            traceResources("after-four-lifecycles",english,trial);
            if (resourceDiagnostics && (trial==0 || trial==1 || trial==19))
                EnumThreadWindows(GetCurrentThreadId(),TraceWindow,0);
            // The isolated native renderer realizes one extra cached GDI
            // object during its first few focus/paint cycles. Warm 16 complete
            // lifecycles, then require a flat process-wide GDI plateau; all owned
            // fonts, brushes and windows are checked even during warmup.
            if (!stableGdi || (!nativeIme && !light && !english && trial<4)) {
                stableGdi=gdi; stableUser=user;
            }
            else {
                // UIAutomationCore installs/removes WH_CALLWNDPROC hooks on
                // another thread even with IME disabled. Debugger traces showed
                // USER 1->2->1 while only the hidden owner HWND remained. A raw
                // process-wide USER sample therefore measures OS activity too,
                // not help ownership. Keep it diagnostic; exact owned handles
                // above still fail for any leaked help HWND/font/brush per close.
                if (user>stableUser && user!=lastUserDiagnostic)
                    std::fprintf(stderr,"RESOURCE USER diagnostic (includes asynchronous OS hooks): language=%s trial=%u user=%lu/%lu\n",
                        english ? "en" : "ko",trial,user,stableUser);
                if (gdi>stableGdi && (!nativeIme || resourceDiagnostics)) {
                    std::fprintf(stderr,"RESOURCE GDI above initial baseline: language=%s trial=%u gdi=%lu/%lu\n",
                        english ? "en" : "ko",trial,gdi,stableGdi);
                    if (!nativeIme)
                        Check(false,"repeated help lifecycle does not leak GDI objects");
                }
            }
            lastUserDiagnostic=user;
        }
    }
    for (bool english:{false,true}) {
        auto custom=llcv::shortcuts::Defaults;
        custom[0]=VK_F9;
#ifdef LLCV_RECORDING
        custom[static_cast<size_t>(llcv::shortcuts::Action::Recording)]=llcv::shortcuts::Ctrl|'R';
#endif
        custom[4]=llcv::shortcuts::Ctrl|llcv::shortcuts::Alt|llcv::shortcuts::Shift|VK_SPACE;
        custom[6]=VK_F10;
        custom[static_cast<size_t>(llcv::shortcuts::Action::Exit)]=llcv::shortcuts::Ctrl|'Q';
        help.SetBindings(custom);
        help.Toggle(owner,english,L"v-test",SW_HIDE);
        HWND dialog=help.Handle(), body=GetDlgItem(dialog,100);
        Check(Text(GetDlgItem(body,205))==L"F9","help shows reassigned help key");
        std::wstring content;
        for (HWND field=GetWindow(body,GW_CHILD);field;field=GetWindow(field,GW_HWNDNEXT))
            content+=Text(field)+L"\n";
        Check(content.find(L"F12")==std::wstring::npos && content.find(L"F10")!=std::wstring::npos &&
              content.find(L"Ctrl+Alt+Shift+Space")!=std::wstring::npos && content.find(L"Ctrl+Q")!=std::wstring::npos,
              "help rows, video-only hint and screenshot instructions use current bindings");
        CheckFields(body);
#ifdef LLCV_RECORDING
        Check(content.find(L"Ctrl+R")!=std::wstring::npos &&
            content.find(english?L"Start / stop recording":L"녹화 시작 / 종료")!=std::wstring::npos,
            "help includes custom recording key and action");
#endif
        auto old=Key(owner,VK_F1);
        Check(!help.ProcessMessage(old,owner,english,L"v-test") && help.Handle()==dialog,
              "old help binding no longer toggles");
        auto shot=Key(body,VK_F10);
        Check(!help.ProcessMessage(shot,owner,english,L"v-test") &&
              IsScreenshotRequest(shot,false,custom) && !IsScreenshotRequest(shot,true,custom),
              "custom screenshot key passes through help and preserves audio-only gate");
        auto toggle=Key(owner,VK_F9,true);
        Check(help.ProcessMessage(toggle,owner,english,L"v-test") && help.Handle()==dialog,
              "custom help key repeat does not toggle");
        auto escape=Key(body,VK_ESCAPE);
        Check(help.ProcessMessage(escape,owner,english,L"v-test") && help.Handle()==dialog,
              "old Esc does not bypass a remapped exit through native dialog handling");
        escape=Key(body,'Q',false,llcv::shortcuts::Ctrl);
        Check(help.ProcessMessage(escape,owner,english,L"v-test") && !help.Handle(),
              "configured exit closes only the guide");
        escape=Key(owner,'Q',true,llcv::shortcuts::Ctrl);
        Check(help.ProcessMessage(escape,owner,english,L"v-test"),"custom exit repeat cannot also close the viewer");
        escape.message=WM_KEYUP; help.ProcessMessage(escape,owner,english,L"v-test");
        escape=Key(owner,'Q',false,llcv::shortcuts::Ctrl);
        Check(!help.ProcessMessage(escape,owner,english,L"v-test"),"next custom exit press reaches viewer routing");
        help.Toggle(owner,english,L"v-test",SW_HIDE);
        toggle=Key(owner,VK_F9);
        Check(help.ProcessMessage(toggle,owner,english,L"v-test") && !help.Handle(),
              "custom help key closes help");
    }
    auto altHelp=llcv::shortcuts::Defaults;
    altHelp[0]=llcv::shortcuts::Alt|VK_F9;
    help.SetBindings(altHelp); help.Toggle(owner,false,L"v-test",SW_HIDE);
    auto altKey=Key(owner,VK_F9,false,llcv::shortcuts::Alt); altKey.message=WM_SYSKEYDOWN;
    Check(help.ProcessMessage(altKey,owner,false,L"v-test") && !help.Handle(),
          "Alt combination reaches custom help through WM_SYSKEYDOWN");
    help.SetBindings(llcv::shortcuts::Defaults);
    TestSettingsKeyRouting(help,owner);
    for(bool english:{false,true}) for(bool light:{false,true}) for(int remapped:{0,1,2}) {
        auto custom=llcv::shortcuts::Defaults;
        if(remapped) {
            if(remapped==2) for(size_t i=0;i<custom.size();++i)
                custom[i]=llcv::shortcuts::Ctrl|llcv::shortcuts::Alt|llcv::shortcuts::Shift|(VK_F13+unsigned(i));
            custom[0]=llcv::shortcuts::Ctrl|llcv::shortcuts::Alt|llcv::shortcuts::Shift|VK_RIGHT;
            custom[8]=llcv::shortcuts::Ctrl|llcv::shortcuts::Alt|llcv::shortcuts::Shift|VK_HOME;
        }
        Check(llcv::shortcuts::Valid(custom),"footer uses valid longest bindings");
        help.SetBindings(custom); help.Toggle(owner,english,L"v-test",SW_HIDE,light);
        const UINT dpi=GetDpiForWindow(help.Handle());
        MINMAXINFO minimum{}; SendMessageW(help.Handle(),WM_GETMINMAXINFO,0,reinterpret_cast<LPARAM>(&minimum));
        RECT frame{0,0,MulDiv(760,dpi,96),MulDiv(640,dpi,96)};
        AdjustWindowRectExForDpi(&frame,static_cast<DWORD>(GetWindowLongPtrW(help.Handle(),GWL_STYLE)),FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(help.Handle(),GWL_EXSTYLE)),dpi);
        MONITORINFO monitor{sizeof(monitor)};
        Check(GetMonitorInfoW(MonitorFromWindow(help.Handle(),MONITOR_DEFAULTTONEAREST),&monitor)!=FALSE,"minimum-size monitor");
        Check(minimum.ptMinTrackSize.x==std::min(frame.right-frame.left,monitor.rcWork.right-monitor.rcWork.left) &&
              minimum.ptMinTrackSize.y==std::min(frame.bottom-frame.top,monitor.rcWork.bottom-monitor.rcWork.top),
              "native minimum keeps two-column guide readable without scrolling");
        for(int height:{640,680,768}) for(int width:{760,820,1024,820}) {
            ResizeClient(help.Handle(),width,height);
            CheckFooter(help.Handle()); CheckFields(GetDlgItem(help.Handle(),100));
            if(saveSnapshots && remapped && width==760 && height==640)
                Snapshot(help.Handle(),std::filesystem::path(argv[1])/(std::string("help-long-")+
                    (english?"en":"ko")+(light?"-light.png":"-dark.png")));
        }
        help.Close();
    }
    help.SetBindings(llcv::shortcuts::Defaults);
    help.Toggle(owner,false,L"v-test",SW_HIDE);
    const auto owned=CaptureHelpResources(help.Handle());
    HWND child=help.Handle(); DestroyWindow(owner);
    Check(!IsWindow(child) && !help.Handle(),"owner destruction clears help handle");
    CheckHelpResourcesReleased(owned);
    help.Close();
    MSG quit{}; Check(!PeekMessageW(&quit,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE),"help never posts quit");
    std::puts("F1 help lifecycle, Escape repeats, F12, focus routing and bilingual text passed.");
    if (token) Gdiplus::GdiplusShutdown(token);
    traceResources("after-shutdown",false,0);
}
