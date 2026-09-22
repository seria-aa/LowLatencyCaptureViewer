#include "SettingsDialogControls.h"
#include "PresentationModeUi.h"
#include "UiText.h"
#include "capture/DirectShowDevices.h"

#include <commctrl.h>
#include <algorithm>
#include <cwctype>
#include <string>

namespace llcv::settings_ui {
using namespace control_id;
using settings::AudioMode;
using settings::DriftCorrectionMode;
using settings::FullscreenCursorMode;
using settings::PresentationMode;
using settings::ScalingMode;

void CreateSettingsDialogControls(SettingsControls* state, HWND hwnd,
                                  HINSTANCE instance,
                                  const SettingsControlInitialValues& initial,
                                  const SettingsControlPopulation& population) {
    const auto text = [&initial](const wchar_t* korean) {
        return ui_text::Translate(korean, initial.english);
    };
    auto makeLabel = [&](const wchar_t* text, int x, int y) {
        return CreateWindowExW(0, L"STATIC", text,
                               WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                               x, y, 160, 24, hwnd, nullptr, instance, nullptr);
    };

    state->tabControl = CreateWindowExW(
        0, WC_TABCONTROLW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FIXEDWIDTH,
        24, 16, 901, 31, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_TAB)),
        instance, nullptr);
    if (state->tabControl) {
        const wchar_t* labels[] = {
            text(L"오디오"), text(L"영상 · 창"),
            text(L"단축키 · 진단"), text(L"업데이트")};
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(labels)); ++i) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<LPWSTR>(labels[i]);
            TabCtrl_InsertItem(state->tabControl, i, &item);
        }
        TabCtrl_SetCurSel(state->tabControl,
                          static_cast<int>(state->activeTab));
    }

    state->audioOutputSection = makeLabel(text(L"출력"), 34, 62);
    state->audioPlaybackSection = makeLabel(text(L"재생 · 편의"), 34, 226);
    state->audioStabilitySection = makeLabel(text(L"동기화 · 안정성"), 34, 392);
    state->videoCaptureSection = makeLabel(text(L"캡처"), 34, 62);
    state->videoDisplaySection = makeLabel(text(L"영상"), 505, 62);
    state->videoWindowSection = makeLabel(text(L"창"), 505, 184);
    state->audioLabel = makeLabel(text(L"오디오 출력 모드"), 24, 24);
    state->audioCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        180, 20, 210, 120, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_AUDIO)), instance, nullptr);
    SendMessageW(state->audioCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     text(L"WASAPI Shared (호환성 우선 · 권장)")));
    SendMessageW(state->audioCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(text(
                     L"WASAPI Exclusive (이벤트 진단 · 장치 독점)")));
    if (initial.asioAvailable) {
        SendMessageW(state->audioCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(text(
                         L"ASIO (지연 최소화 · 드라이버 필요 · 실험적)")));
    }
    const LRESULT audioSelection =
        initial.settings.audioMode == AudioMode::Asio && initial.asioAvailable
            ? 2
            : initial.settings.audioMode == AudioMode::WasapiExclusive ? 1 : 0;
    SendMessageW(state->audioCombo, CB_SETCURSEL,
                  audioSelection, 0);

    state->audioOutputLabel = makeLabel(text(L"오디오 출력 장치"), 24, 68);
    state->audioOutputCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        150, 64, 250, 220, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_AUDIO_OUTPUT)),
        instance, nullptr);
    population.audioOutput(population.context);

    state->bufferLabel = makeLabel(text(L"오디오 출력 버퍼"), 24, 68);
    state->bufferCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
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

    state->volumeHudLabel = makeLabel(text(L"볼륨 HUD 위치"), 24, 142);
    state->volumeHudCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
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
        0, L"BUTTON", text(L"100% 이상 볼륨 증폭 허용 (최대 200%)"),
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
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
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
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
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
        0, L"BUTTON", text(L"오디오 only 모드"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 146, 451, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_AUDIO_ONLY)),
        instance, nullptr);
    SendMessageW(state->audioOnlyCheck, BM_SETCHECK,
                 initial.settings.audioOnly ? BST_CHECKED : BST_UNCHECKED, 0);

    state->surround51Check = CreateWindowExW(
        0, L"BUTTON", text(L"콘솔 LPCM 5.1 (실험적 · Shared 전용)"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        580, 244, 320, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_SURROUND51)),
        instance, nullptr);
    SendMessageW(state->surround51Check, BM_SETCHECK,
        initial.settings.consoleSurround51 ? BST_CHECKED : BST_UNCHECKED, 0);
    state->surround51Hint = CreateWindowExW(
        0, L"STATIC", text(L"콘솔: 5.1 LPCM · 캡처: 6/8채널 PCM 필요\r\n"
        L"Windows 출력 장치도 5.1로 설정하세요.\r\n"
        L"스테레오 출력에서는 Windows가 다운믹스합니다.\r\n"
        L"Dolby/DTS 및 가상 서라운드는 지원하지 않습니다."),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        580, 280, 320, 100, hwnd, nullptr, instance, nullptr);

    state->languageLabel = makeLabel(
        text(L"언어 / Language"), 24, 392);
    state->languageCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
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
        0, L"STATIC", text(
            L"저장된 설정으로 바로 실행 · Shift 실행 또는 F2로 설정 열기"),
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        44, 424, 431, 42, hwnd, nullptr, instance, nullptr);
    state->checkForUpdatesCheck = CreateWindowExW(
        0, L"BUTTON", text(L"업데이트 자동 확인 (시작 후 백그라운드)"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 466, 451, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_UPDATE_CHECK)),
        instance, nullptr);
    SendMessageW(state->checkForUpdatesCheck, BM_SETCHECK,
                 initial.settings.checkForUpdates
                     ? BST_CHECKED : BST_UNCHECKED, 0);

    state->guideShortcutsTitle = makeLabel(
        text(L"단축키"), 34, 62);
    state->guideText = CreateWindowExW(
        0, L"STATIC", text(
            L"F2  설정 다시 열기\r\nF3  오디오 OSD (영상 모드)\r\n"
            L"F5  Pixel-perfect 크기로 맞추기\r\n"
            L"F11  보더리스 전체화면 켜기/끄기\r\n"
            L"Tab  실시간 진단 표시\r\nEsc  전체화면 해제 또는 종료"),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        34, 84, 400, 220, hwnd, nullptr, instance, nullptr);
    state->guideDiagnosticsTitle = makeLabel(
        text(L"진단 · 문제 해결"), 505, 62);
    state->guideDiagnosticsText = CreateWindowExW(
        0, L"STATIC", text(
            L"문제가 생길 때만 로그 저장을 켜고 같은 문제를 재현하세요.\r\n"
            L"로그는 사용자 폴더의 logs에 저장됩니다."),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        505, 84, 360, 70, hwnd, nullptr, instance, nullptr);
    state->guideLogFolderButton = CreateWindowExW(
        0, L"BUTTON", text(L"로그 폴더 열기"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        505, 248, 165, 26, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_OPEN_LOG_FOLDER)),
        instance, nullptr);
    std::wstring versionLabel = text(L"현재 버전");
    versionLabel += L"  ";
    versionLabel += initial.versionLabel;
    state->updateTitle = CreateWindowExW(
        0, L"STATIC", versionLabel.c_str(),
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        34, 76, 400, 24, hwnd, nullptr, instance, nullptr);
    state->updateText = CreateWindowExW(
        0, L"STATIC", text(
            L"자동 확인은 시작 후 백그라운드에서 최신 릴리스를 확인합니다. "
            L"새 버전이 있으면 공식 설치 파일 다운로드를 안내합니다."),
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
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        24, 596, 260, 20, hwnd, nullptr, instance, nullptr);

    state->presentationLabel = makeLabel(text(L"화면 표시 방식"), 24, 274);
    state->presentationCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
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
    SendMessageW(state->presentationCombo, CB_SETCURSEL,
                 initial.settings.presentationMode == PresentationMode::VSync
                     ? 1 : 0, 0);
    SendMessageW(state->presentationCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     llcv::presentation_ui::CompatibilityLabel(initial.english)));
    if (initial.settings.presentationMode == PresentationMode::Compatibility) {
        SendMessageW(state->presentationCombo, CB_SETCURSEL, 2, 0);
    }
    state->displayMonitorLabel = makeLabel(
        initial.english ? L"Display monitor" : L"표시 모니터", 505, 124);
    state->displayMonitorCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
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
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        550, 64, 245, 220, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_CAPTURE_DEVICE)),
        instance, nullptr);
    SendMessageW(state->captureDeviceCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(
                     text(L"자동 선택 (GC573 우선 · 권장)")));
    LRESULT selectedCaptureDevice = 0;
    for (size_t i = 0; i < initial.captureDevices.size(); ++i) {
        std::wstring label = initial.captureDevices[i].name;
        std::wstring lowered = label;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       ::towlower);
        if (lowered.find(L"gc573") == std::wstring::npos &&
            lowered.find(L"live gamer 4k") == std::wstring::npos) {
            label += text(L" (실험적)");
        }
        SendMessageW(state->captureDeviceCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
        if (initial.captureDevices[i].id == initial.settings.captureDeviceId) {
            selectedCaptureDevice = static_cast<LRESULT>(i + 1);
        }
    }
    SendMessageW(state->captureDeviceCombo, CB_SETCURSEL,
                 selectedCaptureDevice, 0);

    state->captureAudioDeviceLabel = makeLabel(
        text(L"캡처 오디오 장치"), 430, 112);
    state->captureAudioDeviceCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        550, 108, 245, 220, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_CAPTURE_AUDIO_DEVICE)),
        instance, nullptr);
    SendMessageW(state->captureAudioDeviceCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(text(
                     L"자동 선택 (영상 장치 오디오 우선 · 권장)")));
    LRESULT selectedCaptureAudioDevice = 0;
    for (size_t i = 0; i < initial.captureAudioDevices.size(); ++i) {
        const std::wstring& label = initial.captureAudioDevices[i].name;
        SendMessageW(state->captureAudioDeviceCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
        if (initial.captureAudioDevices[i].id ==
            initial.settings.captureAudioDeviceId) {
            selectedCaptureAudioDevice = static_cast<LRESULT>(i + 1);
        }
    }
    SendMessageW(state->captureAudioDeviceCombo, CB_SETCURSEL,
                 selectedCaptureAudioDevice, 0);
    state->captureAudioStatus = CreateWindowExW(
        0, L"STATIC", text(L"내부 오디오 확인 중…"),
        WS_CHILD | SS_LEFTNOWORDWRAP,
        550, 108, 245, 24, hwnd, nullptr, instance, nullptr);

    state->videoLabel = makeLabel(text(L"캡처 해상도"), 430, 156);
    state->videoCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        550, 152, 245, 120, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_VIDEO)), instance, nullptr);
    for (const auto& info : initial.videoPresets) {
        SendMessageW(state->videoCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(info.label));
    }
    size_t selectedVideo = 0;
    for (size_t i = 0; i < initial.videoPresets.size(); ++i) {
        if (initial.videoPresets[i].preset == initial.initialVideoPreset) {
            selectedVideo = i;
            break;
        }
    }
    SendMessageW(state->videoCombo, CB_SETCURSEL,
                 static_cast<WPARAM>(selectedVideo), 0);

    state->pixelFormatLabel = makeLabel(text(L"픽셀 포맷"), 430, 156);
    state->pixelFormatCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        550, 152, 245, 160, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_PIXEL_FORMAT)),
        instance, nullptr);
    state->frameRateLabel = makeLabel(text(L"프레임"), 430, 200);
    state->frameRateCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        550, 196, 245, 200, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_FRAME_RATE)),
        instance, nullptr);
    state->videoCapabilityStatus = CreateWindowExW(
        0, L"STATIC", text(L"지원 모드 확인 중..."),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        430, 234, 365, 90, hwnd, nullptr, instance, nullptr);
    population.pixelFormat(population.context);

    state->scalingLabel = makeLabel(text(L"화면 확대 방식"), 430, 274);
    state->scalingCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
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
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
            CBS_DROPDOWNLIST | WS_TABSTOP,
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
        text(L"F11  보더리스 전체화면 켜기/끄기"),
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        630, 364, 255, 24, hwnd, nullptr, instance, nullptr);

    state->forceHdr10Check = CreateWindowExW(
        0, L"BUTTON", text(
            L"P010 HDR10 강제 (메타데이터 없을 때 · 실험적)"),
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
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
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

    state->mjpegColorLabel = makeLabel(
        text(L"MJPEG 색상 해석"), 24, 376);
    state->mjpegColorCombo = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        190, 372, 240, 150, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_MJPEG_COLOR)),
        instance, nullptr);
    const struct {
        const wchar_t* label;
        llcv::video_color::Override value;
    } mjpegColorChoices[] = {
        {text(L"자동 (권장)"), llcv::video_color::Override::Auto},
        {L"BT.709 · Full range", llcv::video_color::Override::Bt709Full},
        {L"BT.709 · Limited range", llcv::video_color::Override::Bt709Limited},
        {L"BT.601 · Full range", llcv::video_color::Override::Bt601Full},
        {L"BT.601 · Limited range", llcv::video_color::Override::Bt601Limited},
    };
    LRESULT selectedMjpegColor = 0;
    for (const auto& choice : mjpegColorChoices) {
        const LRESULT index = SendMessageW(
            state->mjpegColorCombo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(choice.label));
        SendMessageW(state->mjpegColorCombo, CB_SETITEMDATA,
                     static_cast<WPARAM>(index),
                     static_cast<LPARAM>(choice.value));
        if (choice.value == initial.settings.mjpegColorOverride) {
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
        0, L"BUTTON", text(L"Pixel-perfect (1:1 · 창 크기 고정)"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 362, 250, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_PIXEL)), instance, nullptr);
    SendMessageW(state->pixelCheck, BM_SETCHECK,
                 initial.settings.pixelPerfect ? BST_CHECKED : BST_UNCHECKED, 0);

    state->relativeSizeCheck = CreateWindowExW(
        0, L"BUTTON", text(L"모니터 이동 시 상대적 창 크기 유지 (독립 옵션)"),
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
        0, L"BUTTON", text(L"제목 표시줄 숨기기 (borderless 창)"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 430, 300, 28, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_BORDERLESS)),
        instance, nullptr);
    SendMessageW(state->borderlessCheck, BM_SETCHECK,
                 initial.settings.borderlessWindow ? BST_CHECKED : BST_UNCHECKED, 0);

    state->windowSnapCheck = CreateWindowExW(
        0, L"BUTTON", text(L"창을 모니터 가장자리에 스냅 (권장)"),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        24, 464, 330, 28, hwnd,
        reinterpret_cast<HMENU>(
            static_cast<INT_PTR>(IDC_SETTINGS_WINDOW_SNAP)),
        instance, nullptr);
    SendMessageW(state->windowSnapCheck, BM_SETCHECK,
                 initial.settings.windowSnap ? BST_CHECKED : BST_UNCHECKED, 0);

    state->saveLogCheck = CreateWindowExW(
        0, L"BUTTON", text(L"진단 로그 파일 저장 (사용자 폴더)"),
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
}

} // namespace llcv::settings_ui
