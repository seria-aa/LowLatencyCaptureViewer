#include "SettingsDialogControls.h"
#include "ViewerShortcuts.h"
#include "PresentationModeUi.h"
#include "UiText.h"
#include "capture/DirectShowDevices.h"

#include <commctrl.h>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <string>
#include <vector>

namespace llcv::settings_ui {
using namespace control_id;
using settings::AudioMode;
using settings::DriftCorrectionMode;
using settings::FullscreenCursorMode;
using settings::ScalingMode;

void ApplyResolutionSelections(const SettingsControls& state,
                              std::span<const settings::VideoPresetInfo> presets,
                              settings::AppSettings& settings) {
    const LRESULT capture = SendMessageW(state.videoCombo, CB_GETCURSEL, 0, 0);
    const LRESULT display = SendMessageW(state.displayResolutionCombo, CB_GETCURSEL, 0, 0);
    if (capture >= 0 && static_cast<size_t>(capture) < presets.size()) {
        settings.capturePreset = presets[capture].preset;
    }
    if (DisplayResolutionMatchesCapture(state.displayResolutionCombo)) {
        settings.displayMatchesCapture = true;
    } else if (display >= 0 && static_cast<size_t>(display) < presets.size()) {
        settings.displayMatchesCapture = false;
        settings.videoPreset = presets[display].preset;
    }
    if (state.fullscreenPositionCombo) {
        const LRESULT position = SendMessageW(state.fullscreenPositionCombo, CB_GETCURSEL, 0, 0);
        settings.fullscreenVideoPosition = settings::FullscreenPositionFromIndex(static_cast<int>(position));
    }
}

void PopulateCaptureDevicePicker(HWND picker, std::span<const capture::DeviceInfo> devices,
                                 const std::wstring& selected, bool audio, bool english) {
    SendMessageW(picker, CB_RESETCONTENT, 0, 0);
    const wchar_t* automatic = ui_text::Translate(audio
        ? L"자동 선택 (영상 장치 오디오 우선 · 권장)" : L"자동 선택 (권장)", english);
    SendMessageW(picker, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(automatic));
    LRESULT choice = 0;
    for (size_t i=0; i<devices.size(); ++i) {
        const LRESULT row = SendMessageW(picker, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(devices[i].name.c_str()));
        if (devices[i].id == selected) choice = row;
    }
    SendMessageW(picker, CB_SETCURSEL, choice, 0);
}

void PopulateNisSharpnessPicker(HWND picker, int selectedPercent) {
    const int selected = std::clamp(selectedPercent, 0, 100);
    std::vector<int> choices{0, 10, 15, 20, 30, 50, 100};
    const auto position = std::lower_bound(choices.begin(), choices.end(), selected);
    // Preserve an exact saved value without restoring the old 101-item list.
    if (position == choices.end() || *position != selected) choices.insert(position, selected);
    SendMessageW(picker, CB_RESETCONTENT, 0, 0);
    for (int percent : choices) {
        wchar_t label[16]{};
        swprintf_s(label, L"%.2f", double(percent) / 100.0);
        const LRESULT row = SendMessageW(picker, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        if (row < 0) continue;
        SendMessageW(picker, CB_SETITEMDATA, row, percent);
        if (percent == selected) SendMessageW(picker, CB_SETCURSEL, row, 0);
    }
}

int SelectedNisSharpness(HWND picker) {
    if (!IsWindow(picker)) return 15;
    const LRESULT row = SendMessageW(picker, CB_GETCURSEL, 0, 0);
    const LRESULT value = row == CB_ERR ? CB_ERR : SendMessageW(picker, CB_GETITEMDATA, row, 0);
    return value >= 0 && value <= 100 ? int(value) : 15;
}

void CreateSettingsDialogControls(SettingsControls* state, HWND hwnd,
                                  HINSTANCE instance,
                                  const SettingsControlInitialValues& initial,
                                  const SettingsControlPopulation& population) {
    const auto text = [&initial](const wchar_t* korean) {
        return ui_text::Translate(korean, initial.english);
    };
    auto makeLabel = [&](const wchar_t* text, int x, int y) {
        return CreateWindowExW(0, L"STATIC", text,
                               WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_NOPREFIX | SS_CENTERIMAGE,
                               x, y, 160, 24, hwnd, nullptr, instance, nullptr);
    };

    state->english = initial.english;
    state->sdrRangeOverride = initial.settings.sdrRangeOverride;
    state->legacyMjpegRangeOverride = initial.settings.legacyMjpegRangeOverride;
    state->brandLabel = makeLabel(L"LLCV", 12, 28);
    state->pageTitle = makeLabel(L"", 184, 24);
    state->pageSubtitle = makeLabel(L"", 184, 60);
    state->sectionNavigationLabel = makeLabel(initial.english ? L"Section" : L"설정 항목", 184, 90);
    state->sectionNavigation = CreateWindowExW(0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle, 270, 90, 290, 180, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_SECTION)), instance, nullptr);
    state->appPreferencesSection = makeLabel(initial.english ? L"General" : L"일반", 184, 110);
    state->tabControl = CreateWindowExW(
        0, L"LISTBOX", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
        12, 100, 132, 220, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_TAB)),
        instance, nullptr);
    if (state->tabControl) {
        const wchar_t* labels[] = {
            initial.english ? L"Video" : L"영상", text(L"오디오"),
#ifdef LLCV_RECORDING
            initial.english ? L"Recording" : L"녹화",
#endif
            initial.english ? L"Window" : L"창",
            initial.english ? L"Guide & logs" : L"도움말 · 진단",
            initial.english ? L"App" : L"앱 설정"};
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(labels)); ++i) {
            SendMessageW(state->tabControl, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(labels[i]));
        }
        SendMessageW(state->tabControl, LB_SETCURSEL,
                     SettingsNavigationIndex(state->activeTab), 0);
    }
    RefreshSettingsPageHeader(state);

    state->audioOutputSection = makeLabel(text(L"출력"), 34, 62);
    state->audioPlaybackSection = makeLabel(text(L"재생 · 편의"), 34, 226);
    state->audioStabilitySection = makeLabel(text(L"동기화 · 안정성"), 34, 392);
    state->videoCaptureSection = makeLabel(text(L"캡처"), 34, 62);
    state->videoDisplaySection = makeLabel(initial.english ? L"Display" : L"화면 표시", 505, 62);
    state->videoWindowSection = makeLabel(initial.english ? L"Window behavior" : L"창 동작", 505, 184);
    state->audioLabel = makeLabel(text(L"오디오 출력 모드"), 24, 24);
    state->audioCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        180, 20, 210, 120, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_AUDIO)), instance, nullptr);
    SendMessageW(state->audioCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     initial.english ? L"WASAPI Shared (recommended)" : L"WASAPI Shared (권장)"));
    SendMessageW(state->audioCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(initial.english
                     ? L"WASAPI Exclusive (exclusive access)" : L"WASAPI Exclusive (장치 독점)"));
    if (initial.asioAvailable) {
        SendMessageW(state->audioCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(initial.english
                         ? L"ASIO (driver required)" : L"ASIO (드라이버 필요)"));
    }
    const LRESULT audioSelection =
        initial.settings.audioMode == AudioMode::Asio && initial.asioAvailable
            ? 2
            : initial.settings.audioMode == AudioMode::WasapiExclusive ? 1 : 0;
    SendMessageW(state->audioCombo, CB_SETCURSEL,
                  audioSelection, 0);
    AddSettingsTooltip(state, hwnd, state->audioCombo, initial.english
        ? L"Shared works alongside other apps. Exclusive reserves the output device and requires a successful buffer test. ASIO requires an installed driver."
        : L"Shared는 다른 앱과 함께 사용할 수 있습니다. Exclusive는 출력 장치를 독점하며 버퍼 검사가 필요합니다. ASIO는 전용 드라이버가 필요합니다.");

    state->audioOutputLabel = makeLabel(text(L"오디오 출력 장치"), 24, 68);
    state->audioOutputCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        150, 64, 250, 220, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_AUDIO_OUTPUT)),
        instance, nullptr);
    population.audioOutput(population.context);

    state->bufferLabel = makeLabel(text(L"오디오 출력 버퍼"), 24, 68);
    state->bufferCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        180, 64, 210, 180, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_BUFFER)),
        instance, nullptr);
    population.buffer(population.context);

    state->audioStatus = CreateWindowExW(
        0, L"STATIC", text(L"Shared 저지연 지원 확인 중…"),
        WS_CHILD | WS_VISIBLE,
        24, 104, 370, 22, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_AUDIO_STATUS)),
        instance, nullptr);
    state->exclusiveTestButton = CreateWindowExW(
        0, L"BUTTON", text(L"독점 버퍼 검사"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        350, 104, 125, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_EXCLUSIVE_TEST)),
        instance, nullptr);

    state->volumeHudLabel = makeLabel(initial.english ? L"Volume overlay position" : L"음량 표시 위치", 24, 142);
    state->volumeHudCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        180, 138, 210, 160, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_VOLUME_HUD)),
        instance, nullptr);
    for (const wchar_t* label : {text(L"좌측 상단 (기본)"), text(L"우측 상단"),
                                 text(L"좌측 하단"), text(L"우측 하단")}) {
        SendMessageW(state->volumeHudCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label));
    }
    SendMessageW(
        state->volumeHudCombo, CB_SETCURSEL,
        static_cast<WPARAM>(initial.settings.volumeHudPosition), 0);

    state->volumeBoostCheck = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Allow volume up to 200%" : L"최대 200% 음량 증폭 허용",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 230, 400, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_VOLUME_BOOST)),
        instance, nullptr);
    SendMessageW(state->volumeBoostCheck, BM_SETCHECK,
                 initial.settings.allowVolumeBoost
                     ? BST_CHECKED : BST_UNCHECKED, 0);
    state->volumeBoostHelp = CreateWindowExW(
        0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_PUSHBUTTON | BS_NOTIFY,
        438, 226, 24, 24, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_VOLUME_BOOST_HELP)),
        instance, nullptr);
    AddSettingsTooltip(
        state, hwnd, state->volumeBoostHelp,
        SettingsHelpText(SettingsHelpTopic::VolumeBoost, initial.english));

    state->driftLabel = makeLabel(text(L"클록 드리프트 보정"), 24, 226);
    state->driftHelp = CreateWindowExW(
        0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_PUSHBUTTON,
        162, 222, 24, 24, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_DRIFT_HELP)),
        instance, nullptr);
    AddSettingsTooltip(
        state, hwnd, state->driftHelp,
        SettingsHelpText(SettingsHelpTopic::Drift, initial.english));
    state->driftCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        192, 222, 228, 120, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_DRIFT)),
        instance, nullptr);
    SendMessageW(state->driftCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     text(L"끔 (원본 PCM · 음질 우선)")));
    SendMessageW(state->driftCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     text(L"자동 (권장 · 필요 시 보정)")));
    SendMessageW(state->driftCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     text(L"켬 (항상 리샘플링)")));
    SendMessageW(
        state->driftCombo, CB_SETCURSEL,
        initial.settings.driftCorrection == DriftCorrectionMode::Resample
            ? 2
            : initial.settings.driftCorrection == DriftCorrectionMode::Auto
                  ? 1
                  : 0,
        0);

    state->pcmQueueLabel = makeLabel(text(L"PCM 버퍼 목표"), 24, 274);
    state->pcmQueueHelp = CreateWindowExW(
        0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_PUSHBUTTON,
        162, 270, 24, 24, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_PCM_QUEUE_HELP)),
        instance, nullptr);
    AddSettingsTooltip(
        state, hwnd, state->pcmQueueHelp,
        SettingsHelpText(SettingsHelpTopic::PcmQueue, initial.english));
    state->pcmQueueCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        180, 270, 210, 140, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_PCM_QUEUE)),
        instance, nullptr);
    const wchar_t* queueLabels[] = {
        text(L"10 ms (최저 지연)"),
        text(L"15 ms (저지연 목표)"),
        text(L"20 ms (안정 목표)"),
        text(L"25 ms (권장 · 기본)"),
        text(L"30 ms (안정성 우선)" )};
    size_t selectedQueue = 0;
    for (size_t i = 0; i < initial.pcmQueueOptionsMs.size(); ++i) {
        const LRESULT index = SendMessageW(
            state->pcmQueueCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(queueLabels[i]));
        SendMessageW(state->pcmQueueCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(index),
                     initial.pcmQueueOptionsMs[i]);
        if (initial.settings.pcmQueueTargetMs == initial.pcmQueueOptionsMs[i]) {
            selectedQueue = i;
        }
    }
    SendMessageW(state->pcmQueueCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selectedQueue), 0);

    state->muteBackgroundCheck = CreateWindowExW(
        0, L"BUTTON", text(L"백그라운드에서 자동 음소거"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 358, 390, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_MUTE_BACKGROUND)),
        instance, nullptr);
    SendMessageW(state->muteBackgroundCheck, BM_SETCHECK,
                 initial.settings.muteWhenBackground
                     ? BST_CHECKED : BST_UNCHECKED, 0);

    state->audioOnlyCheck = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Audio-only mode" : L"오디오 전용 모드",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 146, 451, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_AUDIO_ONLY)),
        instance, nullptr);
    SendMessageW(state->audioOnlyCheck, BM_SETCHECK,
                 initial.settings.audioOnly ? BST_CHECKED : BST_UNCHECKED, 0);

    state->surround51Check = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Console LPCM 5.1" : L"콘솔 LPCM 5.1",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        580, 244, 320, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_SURROUND51)),
        instance, nullptr);
    SendMessageW(state->surround51Check, BM_SETCHECK,
        initial.settings.consoleSurround51 ? BST_CHECKED : BST_UNCHECKED, 0);
    state->surround51Hint = CreateWindowExW(
        0, L"STATIC", initial.english
        ? L"Shared only · needs 6/8-channel PCM capture.\r\n"
          L"Set console and Windows output to 5.1 LPCM.\r\n"
          L"Windows downmixes on stereo outputs.\r\n"
          L"No Dolby/DTS or virtual surround."
        : L"Shared 전용 · 6/8채널 PCM 캡처가 필요합니다.\r\n"
          L"콘솔과 Windows 출력을 5.1 LPCM으로 설정하세요.\r\n"
          L"스테레오 출력은 Windows가 다운믹스합니다.\r\n"
          L"Dolby/DTS와 가상 서라운드는 지원하지 않습니다.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        580, 280, 320, 100, hwnd, nullptr, instance, nullptr);

    state->languageLabel = makeLabel(
        text(L"언어 / Language"), 24, 392);
    state->languageCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        180, 388, 210, 120, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_LANGUAGE)),
        instance, nullptr);
    SendMessageW(state->languageCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Auto (Windows language)"));
    SendMessageW(state->languageCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"한국어"));
    SendMessageW(state->languageCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"English"));
    SendMessageW(state->languageCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(initial.settings.uiLanguage), 0);

    state->themeLabel = makeLabel(initial.english ? L"App theme" : L"앱 테마", 600, 140);
    state->themeCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr, kSettingsDropdownStyle,
        600, 164, 376, 120, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_THEME)), instance, nullptr);
    SendMessageW(state->themeCombo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(initial.english ? L"Dark" : L"다크"));
    SendMessageW(state->themeCombo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(initial.english ? L"Light" : L"라이트"));
    SendMessageW(state->themeCombo, CB_SETCURSEL, initial.settings.settingsLightTheme ? 1 : 0, 0);

    state->skipStartupCheck = CreateWindowExW(
        0, L"BUTTON", text(L"다음 실행부터 바로 시작"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 436, 451, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_SKIP_STARTUP)),
        instance, nullptr);
    SendMessageW(state->skipStartupCheck, BM_SETCHECK,
                 initial.settings.skipStartupSettings
                     ? BST_CHECKED : BST_UNCHECKED, 0);
    state->skipStartupHint = CreateWindowExW(
        0, L"STATIC", initial.english
            ? L"Uses saved settings. Hold Shift at launch or press F2 to return here."
            : L"저장된 설정으로 실행합니다. Shift를 누른 채 실행하거나 F2로 설정을 여세요.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        44, 424, 431, 42, hwnd, nullptr, instance, nullptr);
    state->preventSleepCheck = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Prevent sleep during playback" : L"재생 중 자동 절전 방지",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        0, 0, 376, 32, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_PREVENT_SLEEP)), instance, nullptr);
    SendMessageW(state->preventSleepCheck, BM_SETCHECK,
                 initial.settings.preventSleep ? BST_CHECKED : BST_UNCHECKED, 0);
    state->preventSleepHint = CreateWindowExW(
        0, L"STATIC", initial.english
            ? L"Video keeps the screen on.\r\nAudio-only allows the screen to turn off."
            : L"영상 재생 중에는 화면도 켜둡니다.\r\n오디오 전용 모드에서는 화면 꺼짐을 허용합니다.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 348, 56, hwnd, nullptr, instance, nullptr);
    state->checkForUpdatesCheck = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Check for updates automatically" : L"업데이트 자동 확인",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 466, 451, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_UPDATE_CHECK)),
        instance, nullptr);
    SendMessageW(state->checkForUpdatesCheck, BM_SETCHECK,
                 initial.settings.checkForUpdates
                     ? BST_CHECKED : BST_UNCHECKED, 0);

    state->guideShortcutsTitle = makeLabel(
        initial.english ? L"Keyboard shortcuts" : L"키보드 단축키", 34, 62);
    state->shortcutBindings=initial.settings.shortcuts;
    state->shortcutResetButton=CreateWindowW(L"BUTTON",initial.english ? L"Reset defaults" : L"기본값 복원",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,1,1,hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_SHORTCUT_RESET)),instance,nullptr);
    for (size_t i = 0; i < viewer_help::kShortcuts.size(); ++i) {
        const auto& shortcut = viewer_help::kShortcuts[i];
        state->guideKeys[i] = CreateWindowExW(0, L"BUTTON", shortcut.key,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            0, 0, 148, 29, hwnd, reinterpret_cast<HMENU>(IDC_SETTINGS_SHORTCUT_FIRST+i), instance, nullptr);
        const std::wstring description=std::wstring(initial.english ? shortcut.english : shortcut.korean)+
            (shortcut.videoOnly ? L" *" : L"");
        state->guideDescriptions[i] = CreateWindowExW(0, L"STATIC",
            description.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 264, 29, hwnd, nullptr, instance, nullptr);
    }
    state->guideVideoHint = CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT|SS_NOPREFIX,
        0,0,336,32,hwnd,nullptr,instance,nullptr);
    state->guideDiagnosticsTitle = makeLabel(
        initial.english ? L"Diagnostics" : L"진단", 505, 62);
    state->screenshotTitle = makeLabel(text(L"스크린샷 (F12)"), 505, 470);
    state->capture40Check = CreateWindowExW(0, L"BUTTON",
        initial.english ? L"40 fps capture" : L"40fps 캡처",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 344, 32, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_CAPTURE_40)), instance, nullptr);
    SendMessageW(state->capture40Check, BM_SETCHECK,
        initial.settings.capture40Fps ? BST_CHECKED : BST_UNCHECKED, 0);
    state->unlistedFpsCheck = CreateWindowExW(0, L"BUTTON",
        initial.english ? L"Allow unlisted FPS" : L"목록에 없는 FPS 요청",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 240, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_UNLISTED_FPS)), instance, nullptr);
    SendMessageW(state->unlistedFpsCheck, BM_SETCHECK,
        initial.settings.allowUnlistedFps ? BST_CHECKED : BST_UNCHECKED, 0);
    state->capture40Help = CreateWindowExW(0, L"BUTTON", L"?",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 24, 24, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_CAPTURE_40_HELP)), instance, nullptr);
    state->frameOptionsButton = CreateWindowExW(0, L"BUTTON",
        initial.english ? L"More ▾" : L"추가 설정 ▾",
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 96, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_FRAME_OPTIONS)), instance, nullptr);
    state->frameOptionsTitle = makeLabel(initial.english ? L"Frame options" : L"프레임 추가 설정", 0, 0);
    state->frameOptionsClose = CreateWindowExW(0, L"BUTTON", L"×",
        WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 28, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_FRAME_OPTIONS_CLOSE)), instance, nullptr);
    state->frameOptionsHint = CreateWindowExW(0, L"STATIC",
        initial.english ? L"Unlisted FPS adds a 30 fps request.\n40 fps overrides the frame selection.\nDevice support is required."
                        : L"목록에 없는 30fps를 직접 요청합니다.\n40fps는 프레임 선택보다 우선합니다.\n장치 지원이 필요합니다.",
        WS_CHILD | SS_LEFT, 0, 0, 312, 56, hwnd, nullptr, instance, nullptr);
    state->guideLsButton = CreateWindowExW(0, L"BUTTON",
        initial.english ? L"40 fps capture guide" : L"40fps 캡처 안내",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 376, 32, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_LS_GUIDE)), instance, nullptr);
    state->videoRefreshButton = CreateWindowExW(0, L"BUTTON",
        initial.english ? L"Refresh" : L"새로고침",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        468, 104, 84, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_VIDEO_REFRESH)), instance, nullptr);
    state->upscalerLabel = makeLabel(initial.english ? L"Upscaling" : L"업스케일링", 600, 412);
    state->upscalerCombo = CreateWindowExW(0, L"COMBOBOX", nullptr, kSettingsDropdownStyle,
        724, 412, 252, 150, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_UPSCALER)), instance, nullptr);
    for (const auto* label : {initial.english ? L"Off" : L"끄기", L"CMAA2 → NIS", L"NVIDIA VSR"}) {
        SendMessageW(state->upscalerCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
    }
    SendMessageW(state->upscalerCombo, CB_SETCURSEL,
        initial.settings.cmaaNisEnabled ? 1 : initial.settings.vsrEnabled && !state->vsrGpuUnavailable ? 2 : 0, 0);
    state->nisSharpnessLabel = makeLabel(initial.english ? L"NIS sharpness" : L"NIS 선명도", 600, 448);
    state->nisSharpnessCombo = CreateWindowExW(0, L"COMBOBOX", nullptr, kSettingsDropdownStyle,
        724, 448, 136, 230, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_NIS_SHARPNESS)), instance, nullptr);
    PopulateNisSharpnessPicker(state->nisSharpnessCombo, initial.settings.nisSharpnessPercent);
    state->vsrGuideButton = CreateWindowExW(0, L"BUTTON",
        L"?",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        340, 444, 120, 26, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_VSR_GUIDE)), instance, nullptr);
    state->displayResolutionLabel = makeLabel(initial.english ? L"Display resolution" : L"표시 해상도", 576, 420);
    state->displayResolutionCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        748, 416, 178, 150, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_DISPLAY_RESOLUTION)), instance, nullptr);
    for (const auto& preset : initial.videoPresets) {
        const LRESULT index = SendMessageW(state->displayResolutionCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(preset.label));
        if (preset.preset == initial.initialVideoPreset)
            SendMessageW(state->displayResolutionCombo, CB_SETCURSEL, index, 0);
    }
    const LRESULT matchCapture = SendMessageW(state->displayResolutionCombo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(initial.english ? L"Same as capture" : L"캡처 해상도와 동일"));
    if (matchCapture >= 0) {
        SendMessageW(state->displayResolutionCombo, CB_SETITEMDATA, matchCapture, kDisplayMatchCaptureItem);
        if (initial.settings.displayMatchesCapture)
            SendMessageW(state->displayResolutionCombo, CB_SETCURSEL, matchCapture, 0);
    }
    state->vsrStatus = makeLabel(initial.english
        ? L"SDR / HDR10 · Toggle with F6"
        : L"SDR / HDR10 · F6으로 전환", 34, 474);
    state->screenshotClipboardCheck = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Also copy to clipboard" : L"클립보드에도 복사",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        505, 494, 400, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_SCREENSHOT_CLIPBOARD)),
        instance, nullptr);
    SendMessageW(state->screenshotClipboardCheck, BM_SETCHECK,
        initial.settings.screenshotClipboard ? BST_CHECKED : BST_UNCHECKED, 0);
    state->screenshotHelp = CreateWindowExW(
        0, L"STATIC", initial.english ? L"Source-size PNG · HDR saved as SDR" : L"입력 해상도 PNG · HDR은 SDR로 저장",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 705, 530, 200, 36,
        hwnd, nullptr, instance, nullptr);
    state->screenshotFolderButton = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Open folder" : L"저장 폴더 열기",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        505, 530, 185, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_SCREENSHOT_FOLDER)),
        instance, nullptr);
    state->guideDiagnosticsText = CreateWindowExW(
        0, L"STATIC", initial.english
            ? L"Enable log saving and reproduce the issue.\r\nCreate a report ZIP to review and attach manually."
            : L"로그 저장을 켠 뒤 증상을 다시 재현하세요.\r\n진단자료 ZIP을 확인한 뒤 직접 첨부해 주세요.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        505, 84, 360, 70, hwnd, nullptr, instance, nullptr);
    state->guideLogFolderButton = CreateWindowExW(
        0, L"BUTTON", text(L"로그 폴더 열기"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        505, 248, 165, 26, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_OPEN_LOG_FOLDER)),
        instance, nullptr);
    state->guideReportButton = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Create report ZIP" : L"진단자료 ZIP 만들기",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 700, 248, 175, 26, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_CREATE_REPORT)), instance, nullptr);
    std::wstring versionLabel = text(L"현재 버전");
    versionLabel += L"  ";
    versionLabel += initial.versionLabel;
    state->updateTitle = CreateWindowExW(
        0, L"STATIC", versionLabel.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        34, 76, 400, 24, hwnd, nullptr, instance, nullptr);
    state->updateText = CreateWindowExW(
        0, L"STATIC", initial.english
            ? L"Checks run in the background after startup.\r\nYou choose whether to download a new version."
            : L"실행 후 백그라운드에서 새 버전을 확인합니다.\r\n다운로드는 안내를 확인한 뒤 선택할 수 있습니다.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        34, 110, 760, 70, hwnd, nullptr, instance, nullptr);
    state->updateNowButton = CreateWindowExW(
        0, L"BUTTON", text(L"최신 버전 확인"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        34, 230, 185, 30, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_UPDATE_NOW)),
        instance, nullptr);
    state->updateStatus = CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        235, 234, 650, 24, hwnd, nullptr, instance, nullptr);
    state->versionWatermark = CreateWindowExW(
        0, L"STATIC", initial.versionLabel,
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
        24, 596, 260, 20, hwnd, nullptr, instance, nullptr);

    state->presentationLabel = makeLabel(text(L"화면 표시 방식"), 24, 274);
    state->presentationCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        180, 270, 210, 120, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_PRESENTATION)),
        instance, nullptr);
    SendMessageW(state->presentationCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     llcv::presentation_ui::ImmediateLabel(
                         initial.english)));
    SendMessageW(state->presentationCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     llcv::presentation_ui::VSyncLabel(
                         initial.english)));
    SendMessageW(state->presentationCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     llcv::presentation_ui::CompatibilityLabel(initial.english)));
    SendMessageW(state->presentationCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     llcv::presentation_ui::ExclusiveFullscreenLabel(initial.english)));
    SendMessageW(state->presentationCombo, CB_SETCURSEL,
                 llcv::presentation_ui::SelectionFromMode(
                     initial.settings.presentationMode), 0);
    state->displayMonitorLabel = makeLabel(
        initial.english ? L"Display monitor" : L"표시 모니터", 505, 124);
    state->displayMonitorCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr, kSettingsDropdownStyle,
        630, 120, 255, 180, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_DISPLAY_MONITOR)),
        instance, nullptr);
    SendMessageW(state->displayMonitorCombo, CB_SETDROPPEDWIDTH, 560, 0);
    SendMessageW(state->displayMonitorCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(
        initial.english ? L"Auto (restore last position)" : L"자동 (마지막 위치 복원)"));
    LRESULT selectedMonitor = 0;
    for (size_t i=0; i<initial.displayMonitors.size(); ++i) {
        const auto& monitor = initial.displayMonitors[i];
        const LRESULT index = SendMessageW(state->displayMonitorCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(monitor.label.c_str()));
        if (_wcsicmp(monitor.id.c_str(), initial.settings.preferredDisplayMonitor.c_str()) == 0)
            selectedMonitor = index;
    }
    if (!initial.settings.preferredDisplayMonitor.empty() && selectedMonitor == 0) {
        selectedMonitor = SendMessageW(state->displayMonitorCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(initial.english
                ? L"Saved monitor disconnected (use primary)"
                : L"저장된 모니터 연결 안 됨 (주 모니터 사용)"));
    }
    SendMessageW(state->displayMonitorCombo, CB_SETCURSEL, selectedMonitor, 0);
    AddSettingsTooltip(state, hwnd, state->displayMonitorCombo, initial.english
        ? L"Choose where the viewer starts. You can still move the window afterwards. If disconnected, the primary monitor is used."
        : L"뷰어를 시작할 모니터를 선택합니다. 실행 후에는 창을 이동할 수 있습니다. 연결되지 않았으면 주 모니터를 사용합니다.");

    state->presentationHelp = CreateWindowExW(
        0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_PUSHBUTTON,
        604, 20, 24, 24, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_PRESENTATION_HELP)),
        instance, nullptr);
    AddSettingsTooltip(
        state, hwnd, state->presentationHelp,
        SettingsHelpText(SettingsHelpTopic::Presentation, initial.english));

    state->captureDeviceLabel = makeLabel(text(L"캡처 장치"), 430, 68);
    state->captureDeviceCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        550, 64, 245, 220, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_CAPTURE_DEVICE)),
        instance, nullptr);
    PopulateCaptureDevicePicker(state->captureDeviceCombo, initial.captureDevices,
        initial.settings.captureDeviceId, false, initial.english);

    state->captureAudioDeviceLabel = makeLabel(
        text(L"캡처 오디오 장치"), 430, 112);
    state->captureAudioDeviceCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        550, 108, 245, 220, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_CAPTURE_AUDIO_DEVICE)),
        instance, nullptr);
    PopulateCaptureDevicePicker(state->captureAudioDeviceCombo, initial.captureAudioDevices,
        initial.settings.captureAudioDeviceId, true, initial.english);
    state->captureAudioStatus = CreateWindowExW(
        0, L"STATIC", text(L"내부 오디오 확인 중…"),
        WS_CHILD | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE,
        550, 108, 245, 24, hwnd, nullptr, instance, nullptr);

    state->videoLabel = makeLabel(text(L"캡처 해상도"), 430, 156);
    state->videoCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        550, 152, 245, 120, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_VIDEO)), instance, nullptr);
    for (const auto& info : initial.videoPresets) {
        SendMessageW(state->videoCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(info.label));
    }
    size_t selectedVideo = 0;
    const auto capturePreset = initial.settings.capturePreset;
    for (size_t i = 0; i < initial.videoPresets.size(); ++i) {
        if (initial.videoPresets[i].preset == capturePreset) {
            selectedVideo = i;
            break;
        }
    }
    SendMessageW(state->videoCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selectedVideo), 0);

    state->pixelFormatLabel = makeLabel(text(L"픽셀 포맷"), 430, 156);
    state->pixelFormatCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        550, 152, 245, 160, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_PIXEL_FORMAT)),
        instance, nullptr);
    state->frameRateLabel = makeLabel(text(L"프레임"), 430, 200);
    state->frameRateCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        550, 196, 245, 200, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_FRAME_RATE)),
        instance, nullptr);
    AddSettingsTooltip(state, hwnd, state->frameRateCombo, initial.english
        ? L"Enable 'Allow unlisted FPS' to add 30 fps when absent from detected modes. Startup stops if the driver rejects 30 fps or applies a different rate."
        : L"'목록에 없는 FPS 요청'을 켜면 자동 인식 목록에 없는 30fps를 추가합니다. 드라이버가 30fps를 거부하거나 다른 FPS로 적용하면 시작을 중단합니다.");
    state->videoCapabilityStatus = CreateWindowExW(
        0, L"STATIC", text(L"지원 모드 확인 중..."),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        430, 234, 365, 90, hwnd, nullptr, instance, nullptr);
    state->unlistedFpsStatus = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | SS_LEFT | SS_NOPREFIX, 0, 0, 376, 16, hwnd,
        nullptr, instance, nullptr);
    population.pixelFormat(population.context);

    state->scalingLabel = makeLabel(text(L"화면 확대 방식"), 430, 274);
    state->scalingCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        550, 270, 245, 120, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_SCALING)),
        instance, nullptr);
    SendMessageW(state->scalingCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(text(L"부드럽게")));
    SendMessageW(state->scalingCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(text(L"선명하게")));
    SendMessageW(state->scalingCombo, CB_SETCURSEL,
                 initial.settings.scalingMode == ScalingMode::Sharp ? 1 : 0, 0);

    state->fullscreenCursorLabel = makeLabel(
        text(L"전체화면 커서"), 505, 336);
    state->fullscreenCursorCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle | WS_CLIPSIBLINGS,
        630, 332, 255, 120, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(
            IDC_SETTINGS_FULLSCREEN_CURSOR)),
        instance, nullptr);
    SendMessageW(state->fullscreenCursorCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(text(L"자동 숨김 (권장)")));
    SendMessageW(state->fullscreenCursorCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(text(L"항상 표시")));
    SendMessageW(state->fullscreenCursorCombo, CB_SETCURSEL,
                 initial.settings.fullscreenCursorMode ==
                         FullscreenCursorMode::AlwaysVisible
                     ? 1 : 0,
                 0);
    state->fullscreenCursorHint = CreateWindowExW(
        0, L"STATIC",
        text(L"F11  전체화면 켜기/끄기"),
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        630, 364, 255, 24, hwnd, nullptr, instance, nullptr);

    state->fullscreenPositionLabel = makeLabel(
        initial.english ? L"Fullscreen video position" : L"전체화면 영상 위치", 600, 316);
    state->fullscreenPositionCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle, 600, 340, 376, 300, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_FULLSCREEN_POSITION)), instance, nullptr);
    const wchar_t* positionKo[] = {L"중앙", L"왼쪽 위", L"위", L"오른쪽 위", L"왼쪽",
        L"오른쪽", L"왼쪽 아래", L"아래", L"오른쪽 아래"};
    const wchar_t* positionEn[] = {L"Center", L"Top left", L"Top", L"Top right", L"Left",
        L"Right", L"Bottom left", L"Bottom", L"Bottom right"};
    static_assert(std::size(positionKo) == settings::kFullscreenVideoPositionCount);
    static_assert(std::size(positionEn) == settings::kFullscreenVideoPositionCount);
    for (int i = 0; i < settings::kFullscreenVideoPositionCount; ++i)
        SendMessageW(state->fullscreenPositionCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(initial.english ? positionEn[i] : positionKo[i]));
    SendMessageW(state->fullscreenPositionCombo, CB_SETCURSEL,
        static_cast<int>(settings::FullscreenPositionFromIndex(
            static_cast<int>(initial.settings.fullscreenVideoPosition))), 0);
    state->fullscreenPositionHint = CreateWindowExW(0, L"STATIC",
        initial.english ? L"Applies in exclusive / borderless fullscreen.\r\nOversized video is reduced to fit."
                        : L"독점·보더리스 전체화면에 적용합니다.\r\n화면보다 크면 잘리지 않도록 축소합니다.",
        WS_CHILD | WS_VISIBLE, 600, 380, 376, 40, hwnd, nullptr, instance, nullptr);

    state->forceHdr10Check = CreateWindowExW(
        0, L"BUTTON", text(
            L"P010 HDR10 강제 (색 정보가 틀릴 때)"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        505, 376, 390, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_FORCE_HDR10)),
        instance, nullptr);
    SendMessageW(state->forceHdr10Check, BM_SETCHECK,
                 initial.settings.forceHdr10 ? BST_CHECKED : BST_UNCHECKED, 0);
    state->forceHdr10Help = CreateWindowExW(
        0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_PUSHBUTTON,
        900, 372, 24, 24, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_FORCE_HDR10_HELP)),
        instance, nullptr);
    AddSettingsTooltip(
        state, hwnd, state->forceHdr10Help,
        SettingsHelpText(SettingsHelpTopic::ForceHdr10, initial.english));

    state->hdrChromaLabel = makeLabel(text(L"HDR 색차 배치"), 34, 414);
    state->hdrChromaCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        190, 410, 240, 150, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_HDR_CHROMA)),
        instance, nullptr);
    const struct { const wchar_t* label; llcv::hdr::ChromaLocation value; } chromaChoices[] = {
        {text(L"자동 (권장)"), llcv::hdr::ChromaLocation::Auto},
        {text(L"Top-left (호환성 해석)"), llcv::hdr::ChromaLocation::TopLeft},
        {text(L"Left (호환성 해석)"), llcv::hdr::ChromaLocation::Left},
    };
    LRESULT selectedChroma = 0;
    for (const auto& choice : chromaChoices) {
        const LRESULT index = SendMessageW(state->hdrChromaCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(choice.label));
        SendMessageW(state->hdrChromaCombo, CB_SETITEMDATA,
            static_cast<WPARAM>(index), static_cast<LPARAM>(choice.value));
        if (choice.value == initial.settings.hdrChromaLocation) selectedChroma = index;
    }
    SendMessageW(state->hdrChromaCombo, CB_SETCURSEL, selectedChroma, 0);
    state->hdrChromaHelp = CreateWindowExW(
        0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        438, 410, 24, 24, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_HDR_CHROMA_HELP)),
        instance, nullptr);
    AddSettingsTooltip(state, hwnd, state->hdrChromaHelp,
        SettingsHelpText(SettingsHelpTopic::HdrChroma, initial.english));

    state->sdrRangeLabel = makeLabel(initial.english ? L"SDR color range" : L"SDR 색 범위", 24, 376);
    state->sdrRangeCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle, 190, 372, 240, 150, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_SDR_RANGE)), instance, nullptr);
    const wchar_t* rangeChoices[] = {initial.english ? L"Auto" : L"자동", L"Limited", L"Full"};
    for (int i = 0; i < 3; ++i) {
        const LRESULT row = SendMessageW(state->sdrRangeCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(rangeChoices[i]));
        SendMessageW(state->sdrRangeCombo, CB_SETITEMDATA, row, i);
    }
    SendMessageW(state->sdrRangeCombo, CB_SETCURSEL,
        static_cast<WPARAM>(settings::EffectiveSdrRange(initial.settings,
            initial.settings.pixelFormat == settings::VideoPixelFormat::Mjpeg)), 0);
    state->sdrRangeHelp = CreateWindowExW(0, L"BUTTON", L"?",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 438, 372, 24, 24, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_SDR_RANGE_HELP)), instance, nullptr);
    AddSettingsTooltip(state, hwnd, state->sdrRangeHelp,
        SettingsHelpText(SettingsHelpTopic::SdrRange, initial.english));

    state->mjpegColorLabel = makeLabel(
        initial.english ? L"MJPEG matrix" : L"MJPEG 색 공간", 24, 376);
    state->mjpegColorCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        kSettingsDropdownStyle,
        190, 372, 240, 150, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_MJPEG_COLOR)),
        instance, nullptr);
    const struct {
        const wchar_t* label;
        llcv::video_color::MatrixOverride value;
    } mjpegColorChoices[] = {
        {initial.english ? L"Auto" : L"자동", llcv::video_color::MatrixOverride::Auto},
        {L"BT.709", llcv::video_color::MatrixOverride::Bt709},
        {L"BT.601", llcv::video_color::MatrixOverride::Bt601},
    };
    LRESULT selectedMjpegColor = 0;
    for (const auto& choice : mjpegColorChoices) {
        const LRESULT index = SendMessageW(
            state->mjpegColorCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(choice.label));
        SendMessageW(state->mjpegColorCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(index),
                     static_cast<LPARAM>(choice.value));
        if (choice.value == initial.settings.mjpegMatrixOverride) {
            selectedMjpegColor = index;
        }
    }
    SendMessageW(state->mjpegColorCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selectedMjpegColor), 0);
    state->mjpegColorHelp = CreateWindowExW(
        0, L"BUTTON", L"?", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_PUSHBUTTON,
        438, 372, 24, 24, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_MJPEG_COLOR_HELP)),
        instance, nullptr);
    AddSettingsTooltip(
        state, hwnd, state->mjpegColorHelp,
        SettingsHelpText(SettingsHelpTopic::MjpegColor, initial.english));

    state->pixelCheck = CreateWindowExW(
        0, L"BUTTON", L"Pixel-perfect (1:1)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 362, 250, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_PIXEL)), instance, nullptr);
    SendMessageW(state->pixelCheck, BM_SETCHECK,
                 initial.settings.pixelPerfect ? BST_CHECKED : BST_UNCHECKED, 0);
    AddSettingsTooltip(state, hwnd, state->pixelCheck, initial.english
        ? L"Locks the selected display size, with or without VSR. Matching capture and display sizes give 1:1 pixels. Relative-size behavior across monitors is separate."
        : L"VSR 사용 여부와 관계없이 선택한 표시 크기로 창을 고정합니다. 캡처와 표시 해상도가 같으면 1:1 크기입니다. 모니터 이동 시 상대적 창 크기 유지는 별도 옵션입니다.");

    state->relativeSizeCheck = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Keep window size proportional across monitors" : L"모니터에 맞춰 상대적 창 크기 유지",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 396, 390, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_RELATIVE_SIZE)),
        instance, nullptr);
    SendMessageW(state->relativeSizeCheck, BM_SETCHECK,
                 initial.settings.relativeWindowSize
                     ? BST_CHECKED : BST_UNCHECKED, 0);

    state->relativeSizeWarning = CreateWindowExW(
        0, L"STATIC",
        text(L"※ Pixel-perfect와 함께 켜면 모니터 이동 시 1:1이 깨질 수 있습니다."),
        WS_CHILD | WS_VISIBLE,
        44, 424, 411, 24, hwnd, nullptr, instance, nullptr);

    state->borderlessCheck = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Hide title bar (borderless)" : L"제목 표시줄 숨기기 (보더리스)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 430, 300, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_BORDERLESS)),
        instance, nullptr);
    SendMessageW(state->borderlessCheck, BM_SETCHECK,
                 initial.settings.borderlessWindow ? BST_CHECKED : BST_UNCHECKED, 0);

    state->roundedCornersCheck = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Rounded corners (Windows 11)" : L"둥근 모서리 (Windows 11)",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 464, 330, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_ROUNDED_CORNERS)),
        instance, nullptr);
    SendMessageW(state->roundedCornersCheck, BM_SETCHECK,
                 initial.settings.roundedCorners ? BST_CHECKED : BST_UNCHECKED, 0);

    state->windowSnapCheck = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Snap to monitor edges" : L"모니터 가장자리에 창 맞추기",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 464, 330, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_WINDOW_SNAP)),
        instance, nullptr);
    SendMessageW(state->windowSnapCheck, BM_SETCHECK,
                 initial.settings.windowSnap ? BST_CHECKED : BST_UNCHECKED, 0);

    state->saveLogCheck = CreateWindowExW(
        0, L"BUTTON", initial.english ? L"Save diagnostic log" : L"진단 로그 저장",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        430, 374, 365, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_SAVE_LOG)),
        instance, nullptr);
    SendMessageW(state->saveLogCheck, BM_SETCHECK,
                 initial.settings.saveLog ? BST_CHECKED : BST_UNCHECKED, 0);

    state->showConsoleCheck = CreateWindowExW(
        0, L"BUTTON", text(L"진단 콘솔 창 표시"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        430, 408, 365, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_SHOW_CONSOLE)),
        instance, nullptr);
    SendMessageW(state->showConsoleCheck, BM_SETCHECK,
                 initial.settings.showDiagnosticConsole
                     ? BST_CHECKED : BST_UNCHECKED, 0);

    state->startButton = CreateWindowExW(
        0, L"BUTTON", text(L"시작"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
        285, 520, 80, 30, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_START)), instance, nullptr);
    state->cancelButton = CreateWindowExW(
        0, L"BUTTON", text(L"취소"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        375, 520, 80, 30, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_CANCEL)), instance, nullptr);

    // Long localized options retain their complete accessible labels and wrap
    // naturally in the narrower two-column layout.
    for (HWND checkbox : {state->volumeBoostCheck, state->surround51Check,
                          state->forceHdr10Check, state->saveLogCheck}) {
        SetWindowLongPtrW(checkbox, GWL_STYLE,
            GetWindowLongPtrW(checkbox, GWL_STYLE) | BS_MULTILINE);
    }
    for (HWND status : {state->vsrStatus, state->updateStatus}) {
        SetWindowLongPtrW(status, GWL_STYLE,
            (GetWindowLongPtrW(status, GWL_STYLE) & ~SS_TYPEMASK) | SS_LEFT);
    }
    // These are descriptive labels, never access-key labels. Literal '&'
    // appears in English help and must not disappear as a mnemonic prefix.
    for (HWND child = GetWindow(hwnd, GW_CHILD); child;
         child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t className[32]{};
        GetClassNameW(child, className, ARRAYSIZE(className));
        if (_wcsicmp(className, L"STATIC") == 0)
            SetWindowLongPtrW(child, GWL_STYLE,
                GetWindowLongPtrW(child, GWL_STYLE) | SS_NOPREFIX);
    }

    // Native dialog navigation follows sibling Z order, not screen position.
    // Keep each label directly before its field: native accessibility discovers
    // combo names from that preceding static. Related help follows the field.
    // Whole groups retain the same reading order as the visual pages.
    // Hidden/disabled fields are skipped by Windows; no input is intercepted.
    const HWND readingOrder[] = {
        state->brandLabel, state->tabControl, state->pageTitle, state->pageSubtitle,
        state->sectionNavigationLabel, state->sectionNavigation,

        state->videoCaptureSection, state->videoRefreshButton,
        state->captureDeviceLabel, state->captureDeviceCombo,
        state->captureAudioDeviceLabel, state->captureAudioDeviceCombo, state->captureAudioStatus,
        state->videoLabel, state->videoCombo, state->frameRateLabel, state->frameRateCombo,
        state->frameOptionsButton, state->frameOptionsTitle,
        state->unlistedFpsCheck, state->capture40Check, state->capture40Help,
        state->frameOptionsHint, state->frameOptionsClose,
        state->pixelFormatLabel, state->pixelFormatCombo, state->videoCapabilityStatus,
        state->forceHdr10Check, state->forceHdr10Help,
        state->hdrChromaLabel, state->hdrChromaCombo, state->hdrChromaHelp,
        state->sdrRangeLabel, state->sdrRangeCombo, state->sdrRangeHelp,
        state->mjpegColorLabel, state->mjpegColorCombo, state->mjpegColorHelp,
        state->unlistedFpsStatus,
        state->videoDisplaySection,
        state->presentationLabel, state->presentationCombo, state->presentationHelp,
        state->displayMonitorLabel, state->displayMonitorCombo, state->pixelCheck,
        state->scalingLabel, state->scalingCombo,
        state->displayResolutionLabel, state->displayResolutionCombo, state->upscalerLabel, state->upscalerCombo,
        state->nisSharpnessLabel, state->nisSharpnessCombo, state->vsrGuideButton, state->vsrStatus,
        state->screenshotTitle, state->screenshotClipboardCheck,
        state->screenshotFolderButton, state->screenshotHelp,

        state->audioOutputSection,
        state->audioLabel, state->audioCombo, state->audioOutputLabel, state->audioOutputCombo,
        state->bufferLabel, state->bufferCombo, state->audioStatus, state->exclusiveTestButton,
        state->audioPlaybackSection,
        state->volumeHudLabel, state->volumeHudCombo,
        state->volumeBoostCheck, state->volumeBoostHelp, state->muteBackgroundCheck,
        state->audioOnlyCheck, state->surround51Check, state->surround51Hint,
        state->audioStabilitySection,
        state->driftLabel, state->driftCombo, state->driftHelp,
        state->pcmQueueLabel, state->pcmQueueCombo, state->pcmQueueHelp,

        state->videoWindowSection,
        state->relativeSizeCheck, state->relativeSizeWarning,
        state->borderlessCheck, state->roundedCornersCheck, state->windowSnapCheck,
        // Keep the hint above the combo in Z order, including before layout's
        // existing hint-to-front safeguard, and the actual label next to it.
        state->fullscreenCursorHint, state->fullscreenCursorLabel, state->fullscreenCursorCombo,
        state->fullscreenPositionLabel, state->fullscreenPositionCombo, state->fullscreenPositionHint,

        state->guideShortcutsTitle, state->shortcutResetButton, state->guideVideoHint,
        state->guideDiagnosticsTitle, state->guideDiagnosticsText,
        state->saveLogCheck, state->showConsoleCheck, state->guideLogFolderButton,
        state->guideReportButton, state->guideLsButton,

        state->appPreferencesSection, state->languageLabel, state->languageCombo,
        state->themeLabel, state->themeCombo,
        state->skipStartupCheck, state->skipStartupHint,
        state->preventSleepCheck, state->preventSleepHint,
        state->updateTitle, state->updateText, state->checkForUpdatesCheck,
        state->updateNowButton, state->updateStatus,

        state->versionWatermark, state->startButton, state->cancelButton,
    };
    for (HWND control : readingOrder) {
        if (control) SetWindowPos(control, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
        if (control == state->shortcutResetButton) {
            for (size_t i = 0; i < state->guideKeys.size(); ++i) {
                for (HWND row : {state->guideKeys[i], state->guideDescriptions[i]})
                    SetWindowPos(row, HWND_BOTTOM, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
            }
        }
    }
    for (HWND control : {state->screenshotTitle,state->skipStartupHint,state->fullscreenCursorHint}) {
        wchar_t caption[512]{}; GetWindowTextW(control,caption,512);
        state->shortcutCaptionTemplates.emplace_back(control,caption);
    }
    RefreshShortcutCaptions(state);
}

} // namespace llcv::settings_ui
