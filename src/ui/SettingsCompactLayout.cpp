#include "SettingsView.h"
#include "settings/AppSettings.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace llcv::settings_ui {
namespace {
constexpr wchar_t kOtherPage[] = L"LLCV.Settings.OtherPage";
struct Item { HWND control; int x, y, width, height; };
struct Page { std::wstring name; std::vector<Item> items; int used = 0; };

// Keep complete options together whenever they fit. Oversized compound options
// can continue at an explicit semantic boundary, never inside a label/field,
// explanation or help-button pair. All dimensions remain in DIPs.
class Pages {
public:
    Pages(int width, int height) : width_(width), height_(height) {}
    void Group(const wchar_t* name) { name_ = name; continuation_ = 0; NewPage(); }
    void Block(int height, std::initializer_list<Item> items, int continuationY = 0) {
        if (height > height_ && continuationY > 0) {
            Append(continuationY, items, 0);
            Append(height - continuationY, items, continuationY);
        } else {
            Append(height, items, 0);
        }
    }
    void Field(HWND label, HWND field, HWND help = nullptr) {
        const int width = width_ - (help ? 34 : 0);
        Block(54, {{label,0,0,width,20}, {field,0,24,width,30},
                   {help,width+10,27,24,24}});
    }
    void Check(HWND control, HWND help = nullptr) {
        Block(32, {{control,0,0,width_-(help?34:0),32},
                   {help,width_-24,4,24,24}});
    }
    void Text(HWND control, int height) { Block(height, {{control,0,0,width_,height}}); }
    void Button(HWND control) { Block(32, {{control,0,0,std::min(width_,240),32}}); }
    std::vector<Page> Take() { return std::move(pages_); }
private:
    void Append(int height, std::initializer_list<Item> items, int offset) {
        if (pages_.back().used && pages_.back().used + height > height_) NewPage();
        auto& page = pages_.back();
        for (auto item : items) {
            if (!item.control || item.y < offset || item.y >= offset + height) continue;
            item.y += page.used - offset;
            page.items.push_back(item);
        }
        page.used += height + 8;
    }
    void NewPage() {
        ++continuation_;
        pages_.push_back({name_ + (continuation_ > 1 ? L" · " + std::to_wstring(continuation_) : L"")});
    }
    int width_, height_, continuation_ = 0;
    std::wstring name_;
    std::vector<Page> pages_;
};
}

bool SettingsCompactLayout(const SettingsControls* state) {
    return state && (state->clientWidthDip < kSettingsClientWidthDip || state->clientHeightDip < 650);
}

bool SettingsControlOnCurrentPage(HWND control) {
    return !control || !GetPropW(control, kOtherPage);
}

void FitSettingsClientArea(SettingsControls* state, SIZE available, UINT dpi) {
    if (!state) return;
    dpi = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    // Floor the conversion: rounding up can put the last physical pixel under
    // a taskbar at fractional scale factors. Leave normal-size layouts intact.
    state->clientWidthDip = std::min(kSettingsClientWidthDip,
        static_cast<int>(std::max(1L, available.cx) * 96 / dpi));
    state->clientHeightDip = std::min(650,
        static_cast<int>(std::max(1L, available.cy) * 96 / dpi));
    state->sidebarWidthDip = state->clientWidthDip < 740 ? 136 : kSettingsSidebarWidthDip;
    state->footerTopDip = state->clientHeightDip - 60;
}

RECT FitSettingsDialogToWorkArea(SettingsControls* state, RECT work, UINT dpi,
                                DWORD style, DWORD exStyle) {
    RECT frame{};
    if (!AdjustWindowRectExForDpi(&frame,style,FALSE,exStyle,dpi))
        AdjustWindowRectEx(&frame,style,FALSE,exStyle);
    const int frameWidth=frame.right-frame.left, frameHeight=frame.bottom-frame.top;
    FitSettingsClientArea(state,{work.right-work.left-frameWidth,work.bottom-work.top-frameHeight},dpi);
    const int width=SettingsPixels(state->clientWidthDip,dpi)+frameWidth;
    const int height=SettingsPixels(state->clientHeightDip,dpi)+frameHeight;
    const int x=work.left+(work.right-work.left-width)/2;
    const int y=work.top+(work.bottom-work.top-height)/2;
    return {x,y,x+width,y+height};
}

void LayoutCompactSettingsControls(SettingsControls* s, UINT dpi) {
    if (!s) return;
    const HWND owner = s->tabControl ? GetParent(s->tabControl) : nullptr;
    if (!owner) return;
    const bool compact = SettingsCompactLayout(s);
    for (const auto item : {std::pair{s->skipStartupHint, SS_LEFT},
                            std::pair{s->fullscreenCursorHint, SS_RIGHT}}) {
        if (!item.first) continue;
        const LONG_PTR style=GetWindowLongPtrW(item.first,GWL_STYLE);
        const LONG_PTR type=compact ? SS_LEFT : item.second;
        if ((style & SS_TYPEMASK)!=type)
            SetWindowLongPtrW(item.first,GWL_STYLE,(style & ~SS_TYPEMASK)|type);
    }
    if (!compact) {
        if (s->compactPropertiesApplied) {
            for (HWND child = GetWindow(owner, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                RemovePropW(child, kOtherPage);
            s->compactPropertiesApplied = false;
        }
        SetSettingsControlVisible(s->sectionNavigationLabel, false);
        SetSettingsControlVisible(s->sectionNavigation, false);
        return;
    }
    s->compactPropertiesApplied = true;
    const bool shortHeader = s->clientHeightDip < 470;
    const int left = s->sidebarWidthDip + 20;
    const int width = s->clientWidthDip - left - 24;
    const int top = shortHeader ? 104 : 132;
    const int pageHeight = s->footerTopDip - top - 16;
    const auto text = [s](const wchar_t* ko, const wchar_t* en) { return s->english ? en : ko; };
    Pages pages(width, pageHeight);
    switch (s->activeTab) {
#ifdef LLCV_RECORDING
    case SettingsTab::Recording:
        pages.Group(text(L"코덱",L"Codec"));
        pages.Text(s->recCodecLabel,20);
        for(auto codec:s->recordingCodecs)pages.Check(codec);
        pages.Field(s->recEncoderLabel,s->recEncoder);
        pages.Text(s->recStatus,52);
        pages.Group(text(L"화질",L"Quality"));
        pages.Field(s->recModeLabel,s->recMode);
        pages.Field(s->recBitrateLabel,s->recBitrate);
        {
            const int half=(width-16)/2;
            pages.Block(54,{{s->recResolutionLabel,0,0,half,20},{s->recResolution,0,24,half,30},
                {s->recFpsLabel,half+16,0,width-half-16,20},{s->recFps,half+16,24,width-half-16,30}});
        }
        pages.Field(s->recAudioLabel,s->recAudio);
        pages.Field(s->recVolumeLabel,s->recVolume);
        pages.Group(text(L"저장",L"Storage"));
        pages.Text(s->recFolderLabel,20);pages.Text(s->recFolder,44);pages.Button(s->recBrowse);pages.Button(s->recOpenFolder);
        pages.Text(s->recSpace,64);

        pages.Field(s->recSplitLabel,s->recSplit);
        pages.Field(s->recSplitValueLabel,s->recSplitValue);
        pages.Text(s->recSplitHint,40);
        // Keep a stable pause location without creating an empty idle page.
        pages.Block(152,{{s->recNote,0,0,width,112},
            {s->recPause,0,120,std::min(width,240),32}});
        break;
#endif
    case SettingsTab::VideoWindow:
        pages.Group(text(L"캡처 장치",L"Capture devices"));
        pages.Button(s->videoRefreshButton);
        pages.Field(s->captureDeviceLabel,s->captureDeviceCombo);
        pages.Block(54, {{s->captureAudioDeviceLabel,0,0,width,20},
            {s->captureAudioDeviceCombo,0,24,width,30},{s->captureAudioStatus,0,24,width,30}});
        pages.Field(s->videoLabel,s->videoCombo);
        pages.Field(s->frameRateLabel,s->frameRateCombo);
        pages.Check(s->unlistedFpsCheck);
        pages.Check(s->capture40Check,s->capture40Help);
        pages.Group(text(L"영상 형식",L"Video format"));
        pages.Field(s->pixelFormatLabel,s->pixelFormatCombo);
        pages.Text(s->videoCapabilityStatus,88);
        if (GetWindowTextLengthW(s->unlistedFpsStatus) > 0)
            pages.Text(s->unlistedFpsStatus,20);
        // Do not create blank continuation pages for formats without overrides.
        {
        const LRESULT selection=SendMessageW(s->pixelFormatCombo,CB_GETCURSEL,0,0);
        const LRESULT format=selection==CB_ERR?CB_ERR:SendMessageW(s->pixelFormatCombo,CB_GETITEMDATA,selection,0);
        if (format==static_cast<LRESULT>(settings::VideoPixelFormat::P010))
        pages.Block(100, {{s->forceHdr10Check,0,0,width-34,36},{s->forceHdr10Help,width-24,6,24,24},
            {s->hdrChromaLabel,0,44,width-34,20},{s->hdrChromaCombo,0,68,width-34,30},
            {s->hdrChromaHelp,width-24,71,24,24}},44);
        if (format!=static_cast<LRESULT>(settings::VideoPixelFormat::P010))
            pages.Field(s->sdrRangeLabel,s->sdrRangeCombo,s->sdrRangeHelp);
        if (format==static_cast<LRESULT>(settings::VideoPixelFormat::Mjpeg))
            pages.Field(s->mjpegColorLabel,s->mjpegColorCombo,s->mjpegColorHelp);
        }
        pages.Group(text(L"화면 표시",L"Display"));
        pages.Field(s->presentationLabel,s->presentationCombo,s->presentationHelp);
        pages.Field(s->displayMonitorLabel,s->displayMonitorCombo);
        pages.Field(s->scalingLabel,s->scalingCombo);
        pages.Check(s->pixelCheck);
        pages.Group(text(L"표시 해상도 · VSR",L"Display resolution & VSR"));
        pages.Field(s->displayResolutionLabel,s->displayResolutionCombo);
        pages.Block(32, {{s->vsrCheck,0,0,width-120,32},{s->vsrGuideButton,width-110,0,110,32}});
        pages.Text(s->vsrStatus,36);
        pages.Block(110, {{s->screenshotTitle,0,0,width,20},
            {s->screenshotClipboardCheck,0,28,width,28},
            {s->screenshotFolderButton,0,66,std::min(200,width/2),32},
            {s->screenshotHelp,std::min(200,width/2)+12,62,width-std::min(200,width/2)-12,44}},62);
        break;
    case SettingsTab::Audio:
        pages.Group(text(L"출력",L"Output"));
        pages.Field(s->audioLabel,s->audioCombo);
        pages.Field(s->audioOutputLabel,s->audioOutputCombo);
        pages.Field(s->bufferLabel,s->bufferCombo);
        pages.Text(s->audioStatus,48);
        if (SendMessageW(s->audioCombo,CB_GETCURSEL,0,0)==1) pages.Button(s->exclusiveTestButton);
        pages.Group(text(L"재생 · 편의",L"Playback"));
        pages.Field(s->volumeHudLabel,s->volumeHudCombo);
        pages.Check(s->volumeBoostCheck,s->volumeBoostHelp);
        pages.Check(s->muteBackgroundCheck);
        pages.Check(s->audioOnlyCheck);
        // The continuation name keeps the standalone explanation associated
        // with its option on the shortest supported work areas.
        if (pageHeight < 128) pages.Group(text(L"콘솔 LPCM 5.1",L"Console LPCM 5.1"));
        pages.Block(128, {{s->surround51Check,0,0,width,32},{s->surround51Hint,0,40,width,88}},40);
        pages.Group(text(L"동기화 · 안정성",L"Timing & stability"));
        pages.Field(s->driftLabel,s->driftCombo,s->driftHelp);
        pages.Field(s->pcmQueueLabel,s->pcmQueueCombo,s->pcmQueueHelp);
        break;
    case SettingsTab::Window:
        pages.Group(text(L"창 동작",L"Window behavior"));
        pages.Check(s->relativeSizeCheck);
        pages.Check(s->borderlessCheck);
        pages.Check(s->roundedCornersCheck);
        pages.Check(s->windowSnapCheck);
        pages.Text(s->relativeSizeWarning,56);
        pages.Group(text(L"전체화면 커서",L"Fullscreen cursor"));
        pages.Field(s->fullscreenCursorLabel,s->fullscreenCursorCombo);
        pages.Text(s->fullscreenCursorHint,40);
        pages.Group(text(L"전체화면 영상 위치",L"Fullscreen video position"));
        pages.Field(s->fullscreenPositionLabel,s->fullscreenPositionCombo);
        pages.Text(s->fullscreenPositionHint,40);
        break;
    case SettingsTab::GuideDiagnostics:
        pages.Group(text(L"키보드 단축키",L"Keyboard shortcuts"));
        pages.Button(s->shortcutResetButton);
        {
        const auto metrics=MeasureShortcutLayout(s,dpi,width);
        for (size_t i=0;i<s->guideKeys.size();++i) {
            const int height=std::max(29,metrics.descriptionHeights[i]);
            pages.Block(std::max(36,height+7),{{s->guideKeys[i],0,0,metrics.keyWidth,29},
                {s->guideDescriptions[i],metrics.keyWidth+12,0,width-metrics.keyWidth-12,height}});
        }
        pages.Text(s->guideVideoHint,metrics.hintHeight);
        }
        pages.Group(text(L"진단",L"Diagnostics"));
        pages.Text(s->guideDiagnosticsText,52);
        pages.Check(s->saveLogCheck);
        pages.Check(s->showConsoleCheck);
        pages.Button(s->guideLogFolderButton);
        pages.Button(s->guideLsButton);
        break;
    case SettingsTab::Updates:
        pages.Group(text(L"일반",L"General"));
        pages.Field(s->languageLabel,s->languageCombo);
        pages.Field(s->themeLabel,s->themeCombo);
        pages.Check(s->skipStartupCheck);
        pages.Text(s->skipStartupHint,44);
        pages.Check(s->preventSleepCheck);
        pages.Text(s->preventSleepHint,56);
        pages.Group(text(L"업데이트",L"Updates"));
        pages.Text(s->updateText,64);
        pages.Check(s->checkForUpdatesCheck);
        pages.Button(s->updateNowButton);
        pages.Text(s->updateStatus,56);
        break;
    }
    auto available = pages.Take();
    if (available.empty()) return;
    int& selected = s->compactPages[SettingsNavigationIndex(s->activeTab)];
    selected = std::clamp(selected,0,static_cast<int>(available.size())-1);
    std::vector<std::wstring> names;
    for (const auto& page : available) names.push_back(page.name);
    if (names != s->compactPageNames) {
        s->compactPageNames = std::move(names);
        SendMessageW(s->sectionNavigation,CB_RESETCONTENT,0,0);
        for (const auto& name : s->compactPageNames)
            SendMessageW(s->sectionNavigation,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
    }
    SendMessageW(s->sectionNavigation,CB_SETCURSEL,selected,0);
    // Establish the page mask before the controller reapplies its enable and
    // format predicates. Async status updates obey this mask too.
    for (HWND child=GetWindow(owner,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
        SetPropW(child,kOtherPage,reinterpret_cast<HANDLE>(1));
    const auto place = [dpi](HWND control,int x,int y,int w,int h) {
        if (!control) return;
        RemovePropW(control,kOtherPage);
        PlaceSettingsControl(control,x,y,w,h,dpi);
        wchar_t className[32]{};
        GetClassNameW(control,className,32);
        if (_wcsicmp(className,L"COMBOBOX")==0) ApplySettingsComboMetrics(control,dpi);
    };
    const int navWidth=s->sidebarWidthDip-20;
    const bool shortSidebar=s->clientHeightDip<380;
    const int navTop=shortSidebar?60:88;
    const int navRowHeight=std::max(1,std::min(44,(s->clientHeightDip-42-8-navTop)/kSettingsNavigationCount));
    place(s->brandLabel,10,shortSidebar?12:20,navWidth,shortSidebar?36:40);
    place(s->tabControl,10,navTop,navWidth,navRowHeight*kSettingsNavigationCount);
    // Divide physical pixels after scaling so fractional DPI cannot clip the
    // last navigation row through accumulated per-row rounding.
    RECT navClient{};
    GetClientRect(s->tabControl,&navClient);
    SendMessageW(s->tabControl,LB_SETITEMHEIGHT,0,std::max(1L,navClient.bottom/kSettingsNavigationCount));
    place(s->versionWatermark,10,s->clientHeightDip-42,navWidth,20);
    place(s->pageTitle,left,20,width,40);
    if (!shortHeader) place(s->pageSubtitle,left,64,width,24);
    place(s->sectionNavigationLabel,left,shortHeader?68:96,76,20);
    place(s->sectionNavigation,left+84,shortHeader?64:92,width-84,30);
    place(s->startButton,s->clientWidthDip-238,s->footerTopDip+12,102,34);
    place(s->cancelButton,s->clientWidthDip-124,s->footerTopDip+12,100,34);
    for (const auto& item:available[selected].items)
        place(item.control,left+item.x,top+item.y,item.width,item.height);
    for (HWND child=GetWindow(owner,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))
        if (!SettingsControlOnCurrentPage(child)) SetSettingsControlVisible(child,false);
}

} // namespace llcv::settings_ui
