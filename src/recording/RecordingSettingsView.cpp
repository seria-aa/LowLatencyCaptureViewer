#include "recording/RecordingSettingsView.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstdio>
namespace llcv::settings_ui {
namespace {
const wchar_t* Text(SettingsControls* s,const wchar_t* ko,const wchar_t* en){return s->english?en:ko;}
void Item(HWND h,const wchar_t* text,int value,int selected){
    auto i=SendMessageW(h,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
    SendMessageW(h,CB_SETITEMDATA,i,value);
    if(value==selected)SendMessageW(h,CB_SETCURSEL,i,0);
}
int Value(HWND h){auto i=SendMessageW(h,CB_GETCURSEL,0,0);return i==CB_ERR?-1:static_cast<int>(SendMessageW(h,CB_GETITEMDATA,i,0));}
void SetTextIfChanged(HWND control,const wchar_t* text){
    const auto length=GetWindowTextLengthW(control);
    std::wstring current(static_cast<size_t>(length)+1,L'\0');
    current.resize(GetWindowTextW(control,current.data(),length+1));
    if(current!=text)SetWindowTextW(control,text);
}
void ShowStorageSpace(SettingsControls* s,bool known,uint64_t bytes){
    wchar_t line[256]{};
    if(known)
        swprintf_s(line,Text(s,L"남은 공간: %.1f GB\n공간이 부족해지면 녹화를 자동 마무리합니다.",L"Free space: %.1f GB\nRecording stops safely when space runs low."),bytes/1e9);
    else wcscpy_s(line,Text(s,L"저장 위치의 남은 공간을 확인할 수 없습니다.",L"Free space is unavailable for this folder."));
    SetTextIfChanged(s->recSpace,line);
}
void RefreshStorage(SettingsControls* s){
    std::filesystem::path path(s->recordingStoragePath);std::error_code error;
    while(!path.empty()&&!std::filesystem::exists(path,error)){auto parent=path.parent_path();if(parent==path)break;path=parent;}
    ULARGE_INTEGER free{};
    const bool known=!path.empty()&&GetDiskFreeSpaceExW(path.c_str(),&free,nullptr,nullptr);
    ShowStorageSpace(s,known,free.QuadPart);
}
bool SnapshotMatchesStorage(const SettingsControls* s){
    if(s->recordingSnapshot.path.empty()||s->recordingStoragePath.empty())return false;
    // Lexical comparison only: resolving symlinks or querying the filesystem
    // here would put storage latency back into the UI's polling timer.
    const auto normalized=[](const std::filesystem::path& path){
        auto value=path.lexically_normal();
        if(value.has_relative_path()&&!value.has_filename())value=value.parent_path();
        return value.make_preferred().wstring();
    };
    const auto actual=normalized(std::filesystem::path(s->recordingSnapshot.path).parent_path());
    const auto selected=normalized(s->recordingStoragePath);
    return CompareStringOrdinal(actual.c_str(),-1,selected.c_str(),-1,TRUE)==CSTR_EQUAL;
}
}
void CreateRecordingSettings(SettingsControls* s,HWND owner){
    const auto create=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int id=0){
        HWND h=CreateWindowExW(0,cls,text,WS_CHILD|style,0,0,1,1,owner,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        s->recordingControls.push_back(h);return h;
    };
    const auto label=[&](const wchar_t* ko,const wchar_t* en){return create(L"STATIC",Text(s,ko,en),SS_LEFT|SS_NOPREFIX);};
    const auto combo=[&](int id){return create(L"COMBOBOX",L"",kSettingsDropdownStyle,id);};
    s->recVideoTitle=label(L"녹화 설정",L"Recording settings");s->recSaveTitle=label(L"저장",L"Storage");
    s->recCodecLabel=label(L"영상 코덱",L"Video codec");
    const wchar_t* names[]={L"H.264",L"HEVC",L"AV1"};
    for(int i=0;i<3;++i)s->recordingCodecs[i]=create(L"BUTTON",names[i],BS_CHECKBOX|WS_TABSTOP,2061+i);
    s->recStatus=label(L"인코더 지원 확인 중…",L"Checking encoder support…");
    s->recEncoderLabel=label(L"인코더",L"Encoder");s->recEncoder=combo(2064);
    s->recModeLabel=label(L"녹화 방식",L"Rate control");s->recMode=combo(2069);
    s->recBitrateLabel=label(L"영상 비트레이트",L"Video bitrate");s->recBitrate=combo(2066);
    s->recResolutionLabel=label(L"녹화 해상도 상한",L"Resolution limit");s->recResolution=combo(kRecordingResolution);
    Item(s->recResolution,Text(s,L"입력 해상도 유지",L"Match capture"),0,s->recordingOptions.maxHeight);
    for(int height:{1440,1080,720})Item(s->recResolution,(std::to_wstring(height)+L"p").c_str(),height,s->recordingOptions.maxHeight);
    s->recFpsLabel=label(L"프레임 상한",L"Frame rate limit");s->recFps=combo(2065);
    Item(s->recFps,Text(s,L"입력 프레임 유지",L"Match capture"),0,s->recordingOptions.maxFps);
    Item(s->recFps,L"60 fps",60,s->recordingOptions.maxFps);Item(s->recFps,L"30 fps",30,s->recordingOptions.maxFps);
    s->recAudioLabel=label(L"오디오 (AAC)",L"Audio (AAC)");s->recAudio=combo(2067);
    Item(s->recAudio,L"128 kbps",128,s->recordingOptions.audioKbps);Item(s->recAudio,L"192 kbps",192,s->recordingOptions.audioKbps);
    s->recVolumeLabel=label(L"녹화 음량",L"Recording volume");
    s->recVolume=combo(kRecordingVolume);
    for(int percent=0;percent<=100;percent+=10)
        Item(s->recVolume,(std::to_wstring(percent)+(percent==0?Text(s,L"% (무음)",L"% (mute)"):L"%")).c_str(),percent,s->recordingOptions.audioVolumePercent);
    if(SendMessageW(s->recVolume,CB_GETCURSEL,0,0)==CB_ERR)
        Item(s->recVolume,(std::to_wstring(s->recordingOptions.audioVolumePercent)+L"%").c_str(),s->recordingOptions.audioVolumePercent,s->recordingOptions.audioVolumePercent);
    s->recFolderLabel=label(L"저장 폴더",L"Save folder");s->recFolder=create(L"STATIC",L"",SS_PATHELLIPSIS|SS_NOPREFIX);
    s->recBrowse=create(L"BUTTON",Text(s,L"폴더 변경",L"Choose folder"),BS_PUSHBUTTON|WS_TABSTOP,2068);
    s->recSpace=label(L"",L"");
    s->recOpenFolder=create(L"BUTTON",Text(s,L"폴더 열기",L"Open folder"),BS_PUSHBUTTON|WS_TABSTOP,kRecordingOpenFolder);
    s->recSplitLabel=label(L"파일 자동 분할",L"Automatic file splitting");s->recSplit=combo(kRecordingSplit);
    Item(s->recSplit,Text(s,L"분할 안 함",L"Off"),0,static_cast<int>(s->recordingOptions.splitMode));
    Item(s->recSplit,Text(s,L"시간 기준",L"By duration"),1,static_cast<int>(s->recordingOptions.splitMode));
    Item(s->recSplit,Text(s,L"용량 기준",L"By file size"),2,static_cast<int>(s->recordingOptions.splitMode));
    s->recSplitValueLabel=label(L"분할 기준",L"Split threshold");s->recSplitValue=combo(kRecordingSplitValue);
    s->recPause=create(L"BUTTON",Text(s,L"녹화 일시정지",L"Pause recording"),BS_PUSHBUTTON|WS_TABSTOP,kRecordingPause);
    s->recSplitHint=label(L"독립 재생 가능한 지점에서 분할합니다.",L"Splits wait for an independently decodable frame.");
    s->recNote=label(L"현재 NV12·YUY2·MJPEG·RGB SDR 영상과\n16비트 PCM 스테레오 오디오를 녹화할 수 있습니다.\nMP4 저장 · CQP 용량은 장면에 따라 달라집니다.\n\nVSR·OSD·앱 음량은 녹화에 반영되지 않습니다.\n녹화 프레임 설정은 재생에 영향을 주지 않습니다.",
        L"Recording supports NV12/YUY2/MJPEG/RGB SDR video\nand 16-bit PCM stereo audio.\nMP4 files · CQP size varies with the scene.\n\nVSR, OSD and app volume are not recorded.\nThe frame rate limit applies only to recording.");
    // New page fields precede the shared footer in keyboard traversal.
    for(auto h:s->recordingControls)SetWindowPos(h,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    for(auto h:{s->startButton,s->cancelButton})if(h)SetWindowPos(h,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    RefreshRecordingSettings(s);
}
bool CanToggleRecordingSettings(const SettingsControls* s){
    if(!s->recordingLive || s->recordingClosing || s->recordingSnapshot.phase==recording::Phase::Finalizing)return false;
    if(s->recordingSnapshot.busy)return true;
    const auto& support=s->recordingCapabilities[static_cast<int>(s->recordingOptions.codec)];
    const bool cpu=s->recordingOptions.encoder==L"cpu";
    return !s->recordingProbePending&&!s->recordingProbeFailed&&support.container&&(cpu?support.software:!support.hardware.empty())&&
        support.Rates(cpu).Supports(s->recordingOptions.rateControl);
}
void RefreshRecordingSettings(SettingsControls* s){
    const int codec=static_cast<int>(s->recordingOptions.codec);
    SendMessageW(s->recEncoder,CB_RESETCONTENT,0,0);
    const auto& support=s->recordingCapabilities[codec];
    // List only verified policies. An unavailable saved selection remains unset;
    // never silently fall back to CPU, a different codec, or a different mode.
    const bool cpu=s->recordingOptions.encoder==L"cpu";
    const bool hardwareAvailable=!s->recordingProbeFailed&&support.container&&!support.hardware.empty()&&support.hardwareRates.Available();
    const bool softwareAvailable=!s->recordingProbeFailed&&support.container&&support.software&&support.softwareRates.Available();
    if(hardwareAvailable)Item(s->recEncoder,Text(s,L"자동 · 하드웨어",L"Automatic · hardware"),0,cpu?1:0);
    if(softwareAvailable)Item(s->recEncoder,Text(s,L"CPU · 소프트웨어",L"CPU · software"),1,cpu?1:0);
    const bool available=cpu?softwareAvailable:hardwareAvailable;
    const auto& rates=support.Rates(cpu);
    SendMessageW(s->recMode,CB_RESETCONTENT,0,0);
    if(available&&rates.quality)Item(s->recMode,Text(s,L"품질 기준 (CQP)",L"Quality (CQP)"),0,static_cast<int>(s->recordingOptions.rateControl));
    if(available&&rates.bitrate)Item(s->recMode,Text(s,L"비트레이트 지정",L"Target bitrate"),1,static_cast<int>(s->recordingOptions.rateControl));
    SendMessageW(s->recBitrate,CB_RESETCONTENT,0,0);
    const bool quality=s->recordingOptions.rateControl==recording::RateControl::Quality;
    SetWindowTextW(s->recBitrateLabel,quality?Text(s,L"녹화 품질",L"Recording quality"):Text(s,L"영상 비트레이트",L"Video bitrate"));
    if(quality){
        const wchar_t* ko[]={L"용량 절약",L"균형",L"높은 화질"};
        const wchar_t* en[]={L"Smaller files",L"Balanced",L"Higher quality"};
        for(int i=0;i<3;++i){
            const auto text=std::wstring(Text(s,ko[i],en[i]))+L" · QP "+std::to_wstring(recording::QualityQp(static_cast<recording::Quality>(i)));
            Item(s->recBitrate,text.c_str(),i,static_cast<int>(s->recordingOptions.quality));
        }
    }else for(int rate=2;rate<=100;++rate){auto text=std::to_wstring(rate)+L" Mbps";Item(s->recBitrate,text.c_str(),rate,s->recordingOptions.videoMbps);}
    std::wstring status;
    if(s->recordingProbePending)status=Text(s,L"인코더 지원 확인 중…",L"Checking encoder support…");
    else if(s->recordingProbeFailed)status=Text(s,L"인코더 확인을 완료하지 못했습니다.\n설정을 다시 열어 재시도하세요.",L"Encoder check could not finish.\nReopen settings to retry.");
    else if(!available)status=Text(s,L"선택한 인코더를 사용할 수 없습니다.\n다른 코덱 또는 인코더를 선택하세요.",L"Selected encoder is unavailable.\nChoose another codec or encoder.");
    else if(!rates.Supports(s->recordingOptions.rateControl))status=Text(s,L"선택한 녹화 방식을 지원하지 않습니다.\n지원되는 녹화 방식을 선택하세요.",L"Selected rate control is unavailable.\nChoose a supported rate control mode.");
    else if(s->recordingOptions.encoder==L"cpu")status=Text(s,L"CPU 인코딩 · 높은 해상도/프레임에서는 부하가 큽니다.",L"CPU encoding may be demanding at high resolutions / frame rates.");
    else status=support.hardware.front().name;
    SetWindowTextW(s->recStatus,status.c_str());ShowRecordingSettings(s);
    SendMessageW(s->recSplitValue,CB_RESETCONTENT,0,0);
    if(s->recordingOptions.splitMode==recording::SplitMode::Size){
        for(int size:{1024,2048,4096,8192,16384})Item(s->recSplitValue,(std::to_wstring(size/1024)+L" GiB").c_str(),size,s->recordingOptions.splitMiB);
    }else{
        for(int minutes:{15,30,60,120})Item(s->recSplitValue,(std::to_wstring(minutes)+Text(s,L"분",L" min")).c_str(),minutes*60,s->recordingOptions.splitSeconds);
    }
    // Valid saved thresholds may come from earlier builds or a hand-edited
    // profile. Show their exact value instead of a blank or a different preset.
    if(SendMessageW(s->recSplitValue,CB_GETCURSEL,0,0)==CB_ERR){
        const bool size=s->recordingOptions.splitMode==recording::SplitMode::Size;
        const int value=size?s->recordingOptions.splitMiB:s->recordingOptions.splitSeconds;
        const auto text=std::to_wstring(value)+(size?L" MiB":Text(s,L"초",L" sec"));
        Item(s->recSplitValue,text.c_str(),value,value);
    }
}
void ShowRecordingSettings(SettingsControls* s){
    const bool visible=s->activeTab==SettingsTab::Recording;
    // The recording worker already measures storage. Reuse its latest result
    // without any disk I/O on busy UI ticks, including unavailable results.
    if(!visible)s->recordingStorageReady=false;
    else{
        const bool refresh=!s->recordingStorageReady || s->recordingStorageFolder!=s->recordingOptions.folder;
        if(refresh){
            s->recordingStorageFolder=s->recordingOptions.folder;
            s->recordingStoragePath=s->recordingOptions.folder.empty()?recording::DefaultFolder():s->recordingOptions.folder;
            SetTextIfChanged(s->recFolder,s->recordingStoragePath.c_str());
            s->recordingStorageReady=true;
        }
        if(s->recordingSnapshot.busy){
            ShowStorageSpace(s,s->recordingSnapshot.freeBytesKnown&&SnapshotMatchesStorage(s),s->recordingSnapshot.freeBytes);
        }else if(refresh||s->recordingStorageFromSnapshot){
            // Idle values refresh on page-open, folder change and recording
            // completion, not on unrelated option changes or timer ticks.
            RefreshStorage(s);
        }
        s->recordingStorageFromSnapshot=s->recordingSnapshot.busy;
    }
    const bool editable=!s->recordingSnapshot.busy&&!s->recordingClosing;
    for(auto h:s->recordingControls){
        if(h==s->recEncoder||h==s->recMode||h==s->recBitrate||h==s->recFps||h==s->recAudio||h==s->recBrowse||
            h==s->recVolume||h==s->recResolution||h==s->recSplit||h==s->recSplitValue||h==s->recPause||
            std::find(s->recordingCodecs.begin(),s->recordingCodecs.end(),h)!=s->recordingCodecs.end())continue;
        SetSettingsControlVisible(h,visible);
    }
    const bool probeReady=!s->recordingProbePending&&!s->recordingProbeFailed;
    for(int i=0;i<3;++i){SetSettingsControlVisible(s->recordingCodecs[i],visible,editable&&probeReady&&s->recordingCapabilities[i].Available());
        const auto checked=static_cast<int>(s->recordingOptions.codec)==i?BST_CHECKED:BST_UNCHECKED;
        if(SendMessageW(s->recordingCodecs[i],BM_GETCHECK,0,0)!=checked)SendMessageW(s->recordingCodecs[i],BM_SETCHECK,checked,0);}
    SetSettingsControlVisible(s->recEncoder,visible,editable&&probeReady&&s->recordingCapabilities[static_cast<int>(s->recordingOptions.codec)].Available());
    const auto& support=s->recordingCapabilities[static_cast<int>(s->recordingOptions.codec)];
    const bool cpu=s->recordingOptions.encoder==L"cpu";
    const auto& rates=support.Rates(cpu);
    const bool available=probeReady&&support.container&&(cpu?support.software:!support.hardware.empty())&&rates.Available();
    SetSettingsControlVisible(s->recMode,visible,editable&&available);
    SetSettingsControlVisible(s->recBitrate,visible,editable&&available&&rates.Supports(s->recordingOptions.rateControl));
    for(auto h:{s->recFps,s->recAudio,s->recVolume,s->recBrowse,s->recResolution,s->recSplit})SetSettingsControlVisible(h,visible,editable);
    SetSettingsControlVisible(s->recSplitValue,visible,editable&&s->recordingOptions.splitMode!=recording::SplitMode::None);
    const bool canPause=s->recordingLive&&!s->recordingClosing&&s->recordingSnapshot.busy&&s->recordingSnapshot.phase==recording::Phase::Recording;
    SetSettingsControlVisible(s->recPause,visible&&canPause,canPause);
    const auto pauseCaption=s->recordingSnapshot.paused?Text(s,L"녹화 재개",L"Resume recording"):Text(s,L"녹화 일시정지",L"Pause recording");
    wchar_t current[64]{};GetWindowTextW(s->recPause,current,64);
    if(wcscmp(current,pauseCaption)!=0)SetWindowTextW(s->recPause,pauseCaption);
}
void LayoutRecordingSettings(SettingsControls* s,UINT dpi){
    const auto p=[&](HWND h,int x,int y,int w,int height){PlaceSettingsControl(h,x,y,w,height,dpi);};
    p(s->recVideoTitle,184,108,376,20);p(s->recSaveTitle,600,108,376,20);p(s->recCodecLabel,184,136,376,20);
    for(int i=0;i<3;++i)p(s->recordingCodecs[i],184+i*128,164,120,32);
    const auto field=[&](HWND label,HWND control,int x,int y,int width){
        p(label,x,y,width,20);p(control,x,y+28,width,200);ApplySettingsComboMetrics(control,dpi);
    };
    field(s->recEncoderLabel,s->recEncoder,184,212,376);
    p(s->recStatus,184,280,376,48);
    field(s->recModeLabel,s->recMode,184,328,180);
    field(s->recBitrateLabel,s->recBitrate,380,328,180);
    field(s->recResolutionLabel,s->recResolution,184,392,180);
    field(s->recFpsLabel,s->recFps,380,392,180);
    field(s->recAudioLabel,s->recAudio,184,456,180);
    field(s->recVolumeLabel,s->recVolume,380,456,180);
    p(s->recFolderLabel,600,136,376,20);p(s->recFolder,600,164,376,24);p(s->recBrowse,600,200,180,32);
    p(s->recSpace,600,240,376,40);
    p(s->recOpenFolder,796,200,180,32);
    field(s->recSplitLabel,s->recSplit,600,328,180);
    field(s->recSplitValueLabel,s->recSplitValue,796,328,180);
    p(s->recSplitHint,600,392,376,32);
    p(s->recNote,600,432,376,104);
    p(s->recPause,600,544,180,30);
}
bool RecordingSettingsCommand(SettingsControls* s,HWND owner,int id,int notification){
    if(id==kRecordingOpenFolder){
        if(notification==BN_CLICKED){
            const auto folder=s->recordingOptions.folder.empty()?recording::DefaultFolder():s->recordingOptions.folder;
            std::error_code error;
            if(!folder.empty())std::filesystem::create_directories(folder,error);
            const bool directory=!folder.empty()&&!error&&std::filesystem::is_directory(folder,error)&&!error;
            if(!directory||reinterpret_cast<INT_PTR>(ShellExecuteW(owner,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)
                MessageBoxW(owner,Text(s,L"저장 폴더를 열 수 없습니다. 폴더가 있는지 확인해 주세요.",L"Could not open the save folder. Check that the folder exists."),L"LLCV",MB_OK|MB_ICONERROR);
        }
        return true;
    }
    if((id<kRecordingControlFirst||id>kRecordingControlLast)&&id!=kRecordingSplit&&id!=kRecordingSplitValue&&id!=kRecordingVolume)return false;
    if(s->recordingSnapshot.busy || s->recordingClosing)return true;
    // While a dropdown is open, arrows preview native selection only. Commit
    // on acceptance so Escape cannot restore the caption but leave new options.
    if(notification==CBN_SELCHANGE && SendMessageW(GetDlgItem(owner,id),CB_GETDROPPEDSTATE,0,0))return true;
    if(notification==CBN_SELENDOK)notification=CBN_SELCHANGE;
    if(id==kRecordingVolume){
        if(notification==CBN_SELCHANGE){
            const int value=Value(s->recVolume);
            if(value>=0&&value<=100)s->recordingOptions.audioVolumePercent=value;
        }
        return true;
    }
    if(id<=2063&&notification==BN_CLICKED){if(IsWindowEnabled(s->recordingCodecs[id-2061]))s->recordingOptions.codec=static_cast<recording::Codec>(id-2061);}
    else if(id==2064&&notification==CBN_SELCHANGE){
        const int selected=Value(s->recEncoder);const bool cpu=selected==1;
        const auto& c=s->recordingCapabilities[static_cast<int>(s->recordingOptions.codec)];
        if(!s->recordingProbePending&&!s->recordingProbeFailed&&(selected==0||selected==1)&&c.container&&c.Rates(cpu).Available()&&(cpu?c.software:!c.hardware.empty()))s->recordingOptions.encoder=cpu?L"cpu":L"";
    }else if(id==2065&&notification==CBN_SELCHANGE)s->recordingOptions.maxFps=Value(s->recFps);
    else if(id==2066&&notification==CBN_SELCHANGE){
        const int value=Value(s->recBitrate);
        if(IsWindowEnabled(s->recBitrate)){
            if(s->recordingOptions.rateControl==recording::RateControl::Quality){if(value>=0&&value<=2)s->recordingOptions.quality=static_cast<recording::Quality>(value);}
            else if(value>=2&&value<=100)s->recordingOptions.videoMbps=value;
        }
    }
    else if(id==2067&&notification==CBN_SELCHANGE)s->recordingOptions.audioKbps=Value(s->recAudio);
    else if(id==kRecordingResolution&&notification==CBN_SELCHANGE)s->recordingOptions.maxHeight=Value(s->recResolution);
    else if(id==kRecordingSplit&&notification==CBN_SELCHANGE)s->recordingOptions.splitMode=static_cast<recording::SplitMode>(Value(s->recSplit));
    else if(id==kRecordingSplitValue&&notification==CBN_SELCHANGE&&IsWindowEnabled(s->recSplitValue)){
        if(s->recordingOptions.splitMode==recording::SplitMode::Size)s->recordingOptions.splitMiB=Value(s->recSplitValue);
        else s->recordingOptions.splitSeconds=Value(s->recSplitValue);
    }
    else if(id==2069&&notification==CBN_SELCHANGE){
        const int value=Value(s->recMode);
        if(IsWindowEnabled(s->recMode)&&value>=0&&value<=1){
            const auto mode=static_cast<recording::RateControl>(value);
            if(s->recordingCapabilities[static_cast<int>(s->recordingOptions.codec)].Rates(s->recordingOptions.encoder==L"cpu").Supports(mode))s->recordingOptions.rateControl=mode;
        }
    }
    else if(id==2068&&notification==BN_CLICKED){
        struct Apartment {
            HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            ~Apartment(){if(SUCCEEDED(result))CoUninitialize();}
        } apartment;
        Microsoft::WRL::ComPtr<IFileOpenDialog> picker;
        if(SUCCEEDED(apartment.result)&&SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&picker)))){
            DWORD flags=0;picker->GetOptions(&flags);picker->SetOptions(flags|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
            if(SUCCEEDED(picker->Show(owner))){Microsoft::WRL::ComPtr<IShellItem> item;PWSTR path=nullptr;
                if(SUCCEEDED(picker->GetResult(&item))&&SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){s->recordingOptions.folder=path;CoTaskMemFree(path);}}
        }else MessageBoxW(owner,Text(s,L"폴더 선택 창을 열 수 없습니다.",L"Could not open the folder picker."),L"LLCV",MB_OK|MB_ICONERROR);
    }else return true;
    RefreshRecordingSettings(s);return true;
}
}
