#include "ViewerHelpWindow.h"
#include "SettingsFonts.h"
#include "ViewerHelpGeometry.h"
#include <algorithm>
#include <dwmapi.h>

namespace llcv::viewer_help {
namespace {
constexpr wchar_t kClass[]=L"LLCV.ViewerHelp", kBodyClass[]=L"LLCV.ViewerHelp.Content";
enum Index : unsigned {
    Tag,Name,Version,Subtitle,KeyboardTitle,FirstKey,VideoHint=FirstKey+2*kShortcuts.size(),
    MouseTitle,WheelLabel,WheelText,DragLabel,DragText,
    ShotTitle,ShotSummary,FolderLabel,FolderText,ShotOption,FieldCount
};
int Pixels(int dip,UINT dpi) { return MulDiv(dip,dpi ? dpi : 96,96); }
RECT MonitorWorkArea(HWND window,const RECT* suggested=nullptr) {
    MONITORINFO monitor{sizeof(monitor)};
    const HMONITOR target=suggested ? MonitorFromRect(suggested,MONITOR_DEFAULTTONEAREST)
        : MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(target,&monitor)) return monitor.rcWork;
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)) return work;
    return {0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
}
}
Window::~Window() { Close(); }
void Window::Close() {
    if (!window_) return;
    const HWND owner=owner_;
    const bool active=GetActiveWindow()==window_;
    DestroyWindow(window_);
    if (active && IsWindow(owner) && IsWindowEnabled(owner)) SetFocus(owner);
}
void Window::ApplyFont(UINT dpi) {
    std::array<HFONT,FontCount> next{};
    const int points[]{10,10,20,9};
    for (int i=0;i<FontCount;++i) {
        const int weight=i==Strong || i==Title ? FW_SEMIBOLD : i==Normal ? FW_MEDIUM : FW_NORMAL;
        next[i]=CreateFontW(-std::max(1,MulDiv(points[i],dpi,72)),0,0,0,weight,
            FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,DEFAULT_PITCH,settings_ui::SettingsFontFamily(weight,english_));
        if (!next[i]) { for (auto font:next) if (font) DeleteObject(font); return; }
    }
    for (const auto& field:fields_) SendMessageW(field.hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(next[field.font]),FALSE);
    for (HWND child:{footer_,close_}) SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(next[Small]),FALSE);
    for (auto font:fonts_) if (font) DeleteObject(font);
    fonts_=next; fontDpi_=dpi;
}
bool Window::UpdateTheme() {
    const bool highContrast=ui::HighContrastEnabled();
    const auto palette=ui::ResolvePalette(lightTheme_,highContrast);
    const COLORREF colors[]{palette.kBackground,palette.kCard,palette.kControl};
    std::array<HBRUSH,SurfaceCount> next{};
    for (int i=0;i<SurfaceCount;++i) {
        next[i]=CreateSolidBrush(colors[i]);
        if (!next[i]) { for (auto brush:next) if (brush) DeleteObject(brush); return false; }
    }
    for (auto brush:brushes_) if (brush) DeleteObject(brush);
    brushes_=next; palette_=palette; highContrast_=highContrast;
    const BOOL dark=!highContrast_ && !lightTheme_;
    DwmSetWindowAttribute(window_,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    const COLORREF caption=highContrast_ ? DWMWA_COLOR_DEFAULT : palette_.kBackground;
    const COLORREF text=highContrast_ ? DWMWA_COLOR_DEFAULT : palette_.kText;
    DwmSetWindowAttribute(window_,DWMWA_CAPTION_COLOR,&caption,sizeof(caption));
    DwmSetWindowAttribute(window_,DWMWA_TEXT_COLOR,&text,sizeof(text));
    return true;
}
void Window::SetLightTheme(bool lightTheme) {
    if (lightTheme_==lightTheme) return;
    const bool previous=lightTheme_;
    lightTheme_=lightTheme;
    if (window_) {
        if (!UpdateTheme()) { lightTheme_=previous; return; }
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_FRAME);
    }
}
bool Window::CreateContent() {
    if (!UpdateTheme()) return false;
    body_=CreateWindowExW(WS_EX_CONTROLPARENT,kBodyClass,english_ ? L"Quick guide" : L"빠른 사용 안내",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPCHILDREN,0,0,1,1,window_,
        reinterpret_cast<HMENU>(100),GetModuleHandleW(nullptr),this);
    if (!body_) return false;
    auto add=[&](const wchar_t* ko,const wchar_t* en,Font font,Surface surface,bool muted=false) {
        const auto caption=shortcuts::Expand(english_ ? en : ko,bindings_);
        HWND child=CreateWindowW(L"STATIC",caption.c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT|SS_NOPREFIX,
            0,0,1,1,body_,reinterpret_cast<HMENU>(INT_PTR(200+fields_.size())),nullptr,nullptr);
        fields_.push_back({child,font,surface,muted});
    };
    add(L"빠른 사용 안내",L"QUICK GUIDE",Small,Background,true);
    add(L"LLCV",L"LLCV",Title,Background);
    add(version_.c_str(),version_.c_str(),Small,Key);
    add(L"캡처 장치의 영상과 오디오를 저지연으로 재생합니다.",L"Low-latency video and audio playback from capture devices.",Normal,Background,true);
    add(L"키보드 단축키",L"Keyboard shortcuts",Strong,Card);
    for (const auto& shortcut:kShortcuts) {
        add(shortcut.key,shortcut.key,Strong,Key);
        add(shortcut.korean,shortcut.english,Normal,Card);
    }
    add(VideoOnlyHint(false),VideoOnlyHint(true),Small,Card,true);
    add(L"마우스 조작",L"Mouse controls",Strong,Card);
    add(L"휠",L"Scroll wheel",Strong,Card);
    add(L"음량 5%씩 조절\nL/R 카드 위에서는 개별 조절\n스피커 버튼: 재생만 음소거 (녹화 유지)",
        L"Adjust volume in 5% steps.\nOver L/R cards: adjust each channel.\nSpeaker button: mute playback, not recording.",Normal,Card,true);
    add(L"Shift + 드래그",L"Shift + drag",Strong,Card);
    add(L"스냅 없이 창 이동",L"Move the window without snapping.",Normal,Card,true);
    add(L"스크린샷",L"Screenshots",Strong,Card);
    add(L"F12로 입력 해상도 PNG 저장\nOSD·화면 필터 제외 / HDR은 SDR로 변환",L"F12 saves a source-resolution PNG.\nNo OSD or display filters; HDR to SDR.",Normal,Card,true);
    add(L"저장 위치",L"Save location",Small,Card,true);
    add(L"사진 / LowLatencyCaptureViewer",L"Pictures / LowLatencyCaptureViewer",Small,Card);
    add(L"클립보드 복사: 앱 설정의 스크린샷",L"Clipboard copy: Screenshots in App settings.",Small,Card,true);
    const auto footer=shortcuts::Expand(english_ ? L"Playback continues · F1 / Esc closes this guide" : L"재생은 계속됩니다 · F1 / Esc로 안내창 닫기",bindings_);
    footer_=CreateWindowW(L"STATIC",footer.c_str(),
        WS_CHILD|WS_VISIBLE|SS_NOPREFIX,0,0,1,1,window_,nullptr,nullptr,nullptr);
    close_=CreateWindowW(L"BUTTON",english_ ? L"Close" : L"닫기",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
        0,0,1,1,window_,reinterpret_cast<HMENU>(IDCANCEL),nullptr,nullptr);
    if (!footer_ || !close_ || fields_.size()!=FieldCount) return false;
    for (const auto& field:fields_) if (!field.hwnd) return false;
    ApplyFont(GetDpiForWindow(window_)); return true;
}
void Window::Layout(UINT windowDpi) {
    if (layingOut_ || !body_ || fields_.size()!=FieldCount) return;
    layingOut_=true;
    RECT root{}; GetClientRect(window_,&root);
    const UINT dpi=detail::ContentDpi(windowDpi ? windowDpi : GetDpiForWindow(window_),root.right,root.bottom);
    if (fontDpi_!=dpi) ApplyFont(dpi);
    layoutDpi_=dpi;
    auto px=[&](int value) { return Pixels(value,dpi); };
    const int margin=px(24), gap=px(16), padding=px(20);
    HDC dc=GetDC(body_);
    const auto footerFont=SelectObject(dc,fonts_[Small]);
    wchar_t footerText[512]{}; GetWindowTextW(footer_,footerText,512);
    RECT footerMeasure{0,0,std::max(1,int(root.right)-px(166)),0};
    DrawTextW(dc,footerText,-1,&footerMeasure,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    SelectObject(dc,footerFont);
    const int footerTextHeight=std::max(px(38),int(footerMeasure.bottom));
    const int footerHeight=footerTextHeight+px(26);
    MoveWindow(body_,0,0,root.right,std::max(1,int(root.bottom)-footerHeight),FALSE);
    MoveWindow(footer_,margin,std::max(0,int(root.bottom)-footerTextHeight-px(5)),
        std::max(1,int(root.right)-px(166)),footerTextHeight,TRUE);
    MoveWindow(close_,std::max(0,int(root.right)-px(124)),std::max(0,int(root.bottom)-px(48)),px(100),px(34),TRUE);
    {
        RECT client{}; GetClientRect(body_,&client);
        const int width=std::max(px(240),int(client.right)-2*margin);
        keys_.clear();
        auto field=[&](unsigned index,int x,int y,int w) {
            auto& value=fields_[index]; wchar_t text[512]{}; GetWindowTextW(value.hwnd,text,512);
            HGDIOBJ old=SelectObject(dc,fonts_[value.font]);
            RECT measure{0,0,std::max(1,w),0};
            DrawTextW(dc,text,-1,&measure,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
            SelectObject(dc,old);
            const int height=std::max(px(16),int(measure.bottom));
            value.rect={x,y,x+w,y+height}; return height;
        };
        field(Tag,margin,px(16),width);
        const int titleBottom=px(38)+field(Name,margin,px(38),width-px(106));
        const int versionY=px(42);
        const int versionX=margin+width-px(82);
        keys_.push_back({versionX,versionY,versionX+px(82),versionY+px(29)});
        field(Version,versionX+px(8),versionY+px(6),px(66));
        const int subtitleY=titleBottom+px(8);
        const int top=subtitleY+field(Subtitle,margin,subtitleY,width)+px(20);
        const int leftWidth=(width-gap)*54/100;
        const int rightX=margin+leftWidth+gap;
        const int rightWidth=width-leftWidth-gap;
        int y=top+padding;
        y+=field(KeyboardTitle,margin+padding,y,leftWidth-2*padding)+px(12);
        int keyWidth=px(80);
        const auto previousFont=SelectObject(dc,fonts_[Strong]);
        for (unsigned binding:bindings_) {
            const auto label=shortcuts::Label(binding);
            SIZE size{}; GetTextExtentPoint32W(dc,label.c_str(),static_cast<int>(label.size()),&size);
            keyWidth=std::max(keyWidth,static_cast<int>(size.cx)+px(18));
        }
        SelectObject(dc,previousFont);
        // At the minimum window width, a half-column cap cuts long chords.
        // Let the key keep its measured width while reserving a readable
        // wrapped description and the existing gap beside it.
        keyWidth=std::min(keyWidth,leftWidth-2*padding-px(96+12));
        std::array<int,kShortcuts.size()> keyHeights{},actionHeights{};
        int textHeight=0;
        for (unsigned i=0;i<kShortcuts.size();++i) {
            keyHeights[i]=field(FirstKey+2*i,0,0,keyWidth-px(16));
            actionHeights[i]=field(FirstKey+2*i+1,0,0,leftWidth-2*padding-keyWidth-px(12));
            textHeight+=std::max(keyHeights[i],actionHeights[i]);
        }
        const int hintHeight=field(VideoHint,0,0,leftWidth-2*padding);
        const int available=int(client.bottom)-margin-padding-px(8)-hintHeight-y;
        // Keep the whole shortcut list visible, including wrapped custom chords.
        // Use comfortable 32-DIP rows when possible, then reduce only row spacing.
        int rowPadding=std::clamp((available-textHeight)/int(kShortcuts.size()),px(4),px(12));
        int minimumRow=px(32);
        auto rowsHeight=[&] {
            int height=0;
            for (unsigned i=0;i<kShortcuts.size();++i)
                height+=std::max(minimumRow,std::max(keyHeights[i],actionHeights[i])+rowPadding);
            return height;
        };
        if (rowsHeight()>available) {
            minimumRow=px(24);
            while (rowPadding>px(4) && rowsHeight()>available) --rowPadding;
        }
        for (unsigned i=0;i<kShortcuts.size();++i) {
            const int rowHeight=std::max(minimumRow,std::max(keyHeights[i],actionHeights[i])+rowPadding);
            auto place=[&](unsigned index,int x,int h) {
                const int w=index==FirstKey+2*i ? keyWidth-px(16) : leftWidth-2*padding-keyWidth-px(12);
                const int top=y+(rowHeight-h)/2;
                fields_[index].rect={x,top,x+w,top+h};
            };
            place(FirstKey+2*i,margin+padding+px(8),keyHeights[i]);
            place(FirstKey+2*i+1,margin+padding+keyWidth+px(12),actionHeights[i]);
            keys_.push_back({margin+padding,y+px(2),margin+padding+keyWidth,y+rowHeight-px(2)});
            y+=rowHeight;
        }
        y+=px(8); y+=field(VideoHint,margin+padding,y,leftWidth-2*padding)+padding;
        cards_[0]={margin,top,margin+leftWidth,y};
        const int mouseTop=top;
        y=mouseTop+padding;
        y+=field(MouseTitle,rightX+padding,y,rightWidth-2*padding)+px(16);
        for (unsigned i:{WheelLabel,DragLabel}) {
            y+=field(i,rightX+padding,y,rightWidth-2*padding)+px(4);
            y+=field(i+1,rightX+padding,y,rightWidth-2*padding)+px(14);
        }
        y+=padding-px(14); cards_[1]={rightX,mouseTop,rightX+rightWidth,y};
        const int shotTop=y+gap; y=shotTop+padding;
        y+=field(ShotTitle,rightX+padding,y,rightWidth-2*padding)+px(12);
        y+=field(ShotSummary,rightX+padding,y,rightWidth-2*padding)+px(16);
        y+=field(FolderLabel,rightX+padding,y,rightWidth-2*padding)+px(6);
        y+=field(FolderText,rightX+padding,y,rightWidth-2*padding)+px(12);
        y+=field(ShotOption,rightX+padding,y,rightWidth-2*padding)+padding;
        cards_[2]={rightX,shotTop,rightX+rightWidth,y};
    }
    ReleaseDC(body_,dc);
    for (const auto& field:fields_) {
        const RECT& r=field.rect;
        SetWindowPos(field.hwnd,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW|SWP_NOCOPYBITS);
        InvalidateRect(field.hwnd,nullptr,TRUE);
    }
    // Child moves must not copy/repaint intermediate overlapping layouts.
    RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    layingOut_=false;
}
SIZE Window::MinimumTrackSize(const RECT& work) const {
    const UINT dpi=GetDpiForWindow(window_);
    RECT minimum{0,0,Pixels(detail::kMinimumClientWidth,dpi),Pixels(detail::kMinimumClientHeight,dpi)};
    if (!AdjustWindowRectExForDpi(&minimum,static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_STYLE)),FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_EXSTYLE)),dpi))
        AdjustWindowRectEx(&minimum,static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_STYLE)),FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(window_,GWL_EXSTYLE)));
    return {std::min(minimum.right-minimum.left,std::max(1L,work.right-work.left)),
        std::min(minimum.bottom-minimum.top,std::max(1L,work.bottom-work.top))};
}
void Window::FitToWorkArea(const RECT& work,const RECT* suggested) {
    if (fitting_ || !window_) return;
    RECT current{}; GetWindowRect(window_,&current);
    RECT requested=suggested ? *suggested : current;
    // SetWindowPos can enforce WM_GETMINMAXINFO while moving. Include that
    // size before clamping the origin, not after the OS expands the frame.
    const SIZE minimum=MinimumTrackSize(work);
    requested.right=requested.left+std::max(requested.right-requested.left,minimum.cx);
    requested.bottom=requested.top+std::max(requested.bottom-requested.top,minimum.cy);
    const RECT fit=detail::FitToWorkArea(requested,work);
    fittingWork_=work; fitting_=true;
    if (!EqualRect(&current,&fit))
        SetWindowPos(window_,nullptr,fit.left,fit.top,fit.right-fit.left,fit.bottom-fit.top,
            SWP_NOZORDER|SWP_NOACTIVATE);
    fitting_=false;
    Layout();
}
void Window::FitToMonitor(const RECT* suggested) {
    if (fitting_ || !window_) return;
    FitToWorkArea(MonitorWorkArea(window_,suggested),suggested);
}
void Window::Paint(HDC dc,bool body) {
    RECT r{}; GetClientRect(body ? body_ : window_,&r); FillRect(dc,&r,brushes_[Background]);
    if (!body) {
        RECT bodyRect{}; GetClientRect(body_,&bodyRect);
        RECT line{0,bodyRect.bottom,r.right,bodyRect.bottom+1};
        SetDCBrushColor(dc,palette_.kCardEdge);
        FillRect(dc,&line,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        return;
    }
    HPEN pen=CreatePen(PS_SOLID,1,palette_.kCardEdge);
    HGDIOBJ oldPen=SelectObject(dc,pen),oldBrush=SelectObject(dc,brushes_[Card]);
    const int radius=Pixels(14,layoutDpi_);
    for (const auto& card:cards_) RoundRect(dc,card.left,card.top,card.right,card.bottom,radius,radius);
    SelectObject(dc,brushes_[Key]);
    for (const auto& key:keys_) RoundRect(dc,key.left,key.top,key.right,key.bottom,radius/2,radius/2);
    SelectObject(dc,oldBrush); SelectObject(dc,oldPen); DeleteObject(pen);
}
void Window::Toggle(HWND owner,bool english,const wchar_t* version,int show,bool lightTheme) {
    if (window_) { Close(); return; }
    if (!IsWindow(owner)) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=Proc; wc.hInstance=GetModuleHandleW(nullptr); wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.lpszClassName=kClass;
    if (!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return;
    wc.lpfnWndProc=BodyProc; wc.lpszClassName=kBodyClass;
    if (!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return;
    owner_=owner; english_=english; version_=version ? version : L"";
    lightTheme_=lightTheme;
    const UINT dpi=GetDpiForWindow(owner);
    RECT rect{0,0,Pixels(820,dpi),Pixels(680,dpi)};
    constexpr DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_CLIPCHILDREN;
    if (!AdjustWindowRectExForDpi(&rect,style,FALSE,WS_EX_CONTROLPARENT,dpi)) AdjustWindowRectEx(&rect,style,FALSE,WS_EX_CONTROLPARENT);
    const RECT work=MonitorWorkArea(owner);
    const int width=std::min(int(rect.right-rect.left),int(work.right-work.left));
    const int height=std::min(int(rect.bottom-rect.top),int(work.bottom-work.top));
    window_=CreateWindowExW(WS_EX_CONTROLPARENT,kClass,english ? L"App information & shortcuts" : L"앱 정보 · 단축키",style,
        work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,owner,nullptr,GetModuleHandleW(nullptr),this);
    if (!window_) return;
    FitToMonitor(); ShowWindow(window_,show); if (show!=SW_HIDE) SetFocus(close_);
}
void Window::SetBindings(const shortcuts::Bindings& bindings) {
    if (bindings_==bindings) return;
    Close(); // Existing content belongs to the previous settings snapshot.
    bindings_=bindings;
}
bool Window::ProcessMessage(MSG& m,HWND owner,bool english,const wchar_t* version,bool lightTheme,unsigned modifiers,MessageContext context) {
    if (window_) SetLightTheme(lightTheme);
    if ((m.message==WM_KEYUP || m.message==WM_SYSKEYUP) && m.wParam==dismissKey_ && dismissKey_) { dismissKey_=0; return true; }
    if (shortcuts::KeyDown(m.message) && m.wParam==dismissKey_ && dismissKey_) {
        if (m.lParam & (LPARAM{1}<<30)) return true;
        dismissKey_=0;
    }
    const bool inHelp=window_ && (m.hwnd==window_ || IsChild(window_,m.hwnd));
    // Viewer bindings must not steal dialog navigation, text entry, editing
    // chords or the native F4 combo dropdown. Function-key help still works
    // from settings; all configured bindings work inside the guide/viewer.
    if (context==MessageContext::Settings && !inHelp && shortcuts::KeyDown(m.message) &&
        !shortcuts::AllowedInSettings(static_cast<UINT>(m.wParam))) return false;
    if (shortcuts::KeyDown(m.message) && shortcuts::Match(bindings_,shortcuts::Action::Help,static_cast<UINT>(m.wParam),modifiers)) {
        if (!(m.lParam & (LPARAM{1}<<30))) Toggle(owner,english,version,SW_SHOWNORMAL,lightTheme);
        return true;
    }
    if (window_ && shortcuts::KeyDown(m.message) &&
        shortcuts::Match(bindings_,shortcuts::Action::Exit,static_cast<UINT>(m.wParam),modifiers)) {
        dismissKey_=static_cast<UINT>(m.wParam); Close(); return true;
    }
    if (inHelp) {
        if (shortcuts::KeyDown(m.message) && shortcuts::Match(bindings_,shortcuts::Action::Screenshot,static_cast<UINT>(m.wParam),modifiers)) return false;
        // Do not let the native dialog manager reintroduce a remapped Esc.
        if (shortcuts::KeyDown(m.message) && m.wParam==VK_ESCAPE) return true;
        if (m.message==WM_MOUSEWHEEL) { SendMessageW(body_,m.message,m.wParam,m.lParam); return true; }
        if (!IsDialogMessageW(window_,&m)) { TranslateMessage(&m); DispatchMessageW(&m); }
        return true;
    }
    return false;
}
LRESULT CALLBACK Window::BodyProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* self=reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        self=static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); self->body_=hwnd;
    }
    if (!self) return DefWindowProcW(hwnd,message,wParam,lParam);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; HDC dc=BeginPaint(hwnd,&ps); self->Paint(dc,true); EndPaint(hwnd,&ps); return 0; }
    case WM_PRINTCLIENT: self->Paint(reinterpret_cast<HDC>(wParam),true); return 0;
    case WM_SIZE: self->Layout(); return 0;
    case WM_GETFONT: return reinterpret_cast<LRESULT>(self->fonts_[Normal]);
    case WM_LBUTTONDOWN: SetFocus(hwnd); return 0;
    case WM_MOUSEWHEEL: case WM_VSCROLL: return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc=reinterpret_cast<HDC>(wParam);
        for (const auto& field:self->fields_) if (field.hwnd==reinterpret_cast<HWND>(lParam)) {
            const auto& p=self->palette_;
            const COLORREF bg[]{p.kBackground,p.kCard,p.kControl};
            SetBkColor(dc,bg[field.surface]);
            SetTextColor(dc,field.surface==Key ? p.kAccent : (field.muted ? p.kSecondary : p.kText));
            return reinterpret_cast<LRESULT>(self->brushes_[field.surface]);
        } break;
    }
    }
    return DefWindowProcW(hwnd,message,wParam,lParam);
}
LRESULT CALLBACK Window::Proc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* self=reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        self=static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); self->window_=hwnd;
    }
    if (!self) return DefWindowProcW(hwnd,message,wParam,lParam);
    switch (message) {
    case WM_CREATE: return self->CreateContent() ? 0 : -1;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; HDC dc=BeginPaint(hwnd,&ps); self->Paint(dc,false); EndPaint(hwnd,&ps); return 0; }
    case WM_PRINTCLIENT: self->Paint(reinterpret_cast<HDC>(wParam),false); return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wParam),self->palette_.kBackground);
        SetTextColor(reinterpret_cast<HDC>(wParam),self->palette_.kSecondary);
        return reinterpret_cast<LRESULT>(self->brushes_[Background]);
    case WM_DRAWITEM: {
        const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item->CtlID!=IDCANCEL) break;
        const int saved=SaveDC(item->hDC);
        const auto& p=self->palette_;
        FillRect(item->hDC,&item->rcItem,self->brushes_[Background]);
        SelectObject(item->hDC,GetStockObject(DC_BRUSH));
        SelectObject(item->hDC,GetStockObject(NULL_PEN));
        SetDCBrushColor(item->hDC,item->itemState&ODS_SELECTED ? p.kPrimaryPressed : p.kAccent);
        const int radius=Pixels(14,self->layoutDpi_);
        RoundRect(item->hDC,item->rcItem.left,item->rcItem.top,item->rcItem.right,item->rcItem.bottom,radius,radius);
        SetBkMode(item->hDC,TRANSPARENT); SetTextColor(item->hDC,p.kOnAccent);
        HGDIOBJ old=SelectObject(item->hDC,self->fonts_[Strong]);
        RECT text=item->rcItem; if (item->itemState&ODS_SELECTED) OffsetRect(&text,1,1);
        DrawTextW(item->hDC,self->english_ ? L"Close" : L"닫기",-1,&text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(item->hDC,old);
        if (item->itemState&ODS_FOCUS) { RECT focus=item->rcItem; InflateRect(&focus,-3,-3); DrawFocusRect(item->hDC,&focus); }
        if (saved) RestoreDC(item->hDC,saved);
        return TRUE;
    }
    case WM_SETTINGCHANGE: case WM_THEMECHANGED: case WM_SYSCOLORCHANGE:
        if (message==WM_SETTINGCHANGE) self->FitToMonitor();
        if (self->UpdateTheme())
            RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_FRAME);
        return 0;
    case WM_SIZE: self->Layout(); return 0;
    case WM_DPICHANGED: {
        self->FitToMonitor(reinterpret_cast<const RECT*>(lParam));
        self->Layout(LOWORD(wParam)); return 0;
    }
    case WM_DISPLAYCHANGE: case WM_EXITSIZEMOVE:
        self->FitToMonitor(); return 0;
    case WM_GETMINMAXINFO: {
        auto* info=reinterpret_cast<MINMAXINFO*>(lParam);
        const SIZE minimum=self->MinimumTrackSize(self->fitting_ ? self->fittingWork_ : MonitorWorkArea(hwnd));
        info->ptMinTrackSize={minimum.cx,minimum.cy}; return 0;
    }
    case WM_COMMAND: if (LOWORD(wParam)==IDCANCEL || LOWORD(wParam)==IDOK) { self->Close(); return 0; } break;
    case WM_CLOSE: self->Close(); return 0;
    case WM_NCDESTROY:
        self->window_=self->body_=self->footer_=self->close_=nullptr; self->fields_.clear();
        for (auto& font:self->fonts_) { if (font) DeleteObject(font); font=nullptr; }
        for (auto& brush:self->brushes_) { if (brush) DeleteObject(brush); brush=nullptr; }
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,0); break;
    }
    return DefWindowProcW(hwnd,message,wParam,lParam);
}
} // namespace llcv::viewer_help
