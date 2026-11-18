#include "ui/PresentationModeUi.h"

namespace llcv::presentation_ui {

const wchar_t* ImmediateLabel(bool english) {
    return english ? L"Low latency (standard)"
                   : L"저지연 (일반 출력)";
}

const wchar_t* VSyncLabel(bool english) {
    return english ? L"VSync (reduced tearing)"
                   : L"VSync (찢어짐 완화)";
}

const wchar_t* ExclusiveFullscreenLabel(bool english) {
    return english ? L"Low latency (exclusive fullscreen)"
                   : L"저지연 (독점 전체화면)";
}

settings::PresentationMode ModeFromSelection(int selection) {
    using settings::PresentationMode;
    switch (selection) {
    case 1: return PresentationMode::VSync;
    case 2: return PresentationMode::Compatibility;
    case 3: return PresentationMode::ExclusiveFullscreen;
    default: return PresentationMode::AllowTearing;
    }
}

int SelectionFromMode(settings::PresentationMode mode) {
    using settings::PresentationMode;
    switch (mode) {
    case PresentationMode::VSync: return 1;
    case PresentationMode::Compatibility: return 2;
    case PresentationMode::ExclusiveFullscreen: return 3;
    default: return 0;
    }
}

const wchar_t* HelpText(bool english) {
    return english
        ? L"Low latency (standard): use this for windowed or borderless playback without waiting for the monitor to refresh.\n\n"
          L"VSync: try this if the picture tears or briefly goes black. Waiting for a refresh can add latency.\n\n"
          L"Compatibility: try this if both modes above still cause problems. It can add latency and does not support HDR10.\n\n"
          L"Low latency (exclusive fullscreen): gives playback the whole display. Alt+Tab or opening settings temporarily leaves exclusive mode. Use the fullscreen shortcut to return to a window."
        : L"저지연 (일반 출력)\n창이나 보더리스 전체화면에서 표시 지연을 줄일 때 선택합니다.\n\n"
          L"VSync\n화면이 가로로 찢어져 보이거나 잠깐씩 검게 변할 때 사용합니다.\n"
          L"모니터 주사율에 맞춰 표시하므로 지연이 조금 늘 수 있습니다.\n\n"
          L"호환성 출력\n위 두 방식에서 모두 문제가 생길 때 사용합니다. 지연이 늘 수 있고 HDR10은 지원하지 않습니다.\n\n"
          L"저지연 (독점 전체화면)\n재생 화면이 모니터를 단독으로 사용합니다.\n"
          L"Alt+Tab을 누르거나 설정을 열면 독점 모드가 잠시 풀립니다. 전체화면 단축키로 창 모드로 전환합니다.";
}

const wchar_t* CompatibilityLabel(bool english) {
    return english ? L"Compatibility (Blt + VSync)"
                   : L"호환성 출력 (Blt · VSync)";
}

}  // namespace llcv::presentation_ui
