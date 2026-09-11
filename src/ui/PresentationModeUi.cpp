#include "ui/PresentationModeUi.h"

namespace llcv::presentation_ui {

const wchar_t* ImmediateLabel(bool english) {
    return english ? L"Immediate (minimum latency)"
                   : L"저지연 (최소 지연)";
}

const wchar_t* VSyncLabel(bool english) {
    return english ? L"VSync (reduced tearing)"
                   : L"VSync (찢어짐 완화)";
}

const wchar_t* HelpText(bool english) {
    if (english) {
        return L"Presentation mode\n\n"
               L"Immediate presents the newest frame without waiting for VSync. This minimizes "
               L"display latency but can show tearing. On some graphics-driver and display "
               L"combinations it can also cause an intermittent black screen or signal loss.\n\n"
               L"VSync follows the monitor refresh to reduce tearing. It uses the same Flip "
               L"presentation path as Immediate, but waiting for a refresh can add latency. "
               L"Try it if Immediate shows tearing or intermittent black screens.\n\n"
               L"Compatibility output (Blt + VSync) changes the presentation path from Flip "
               L"to Blt and also enables VSync. Try it if problems persist with both Immediate "
               L"and VSync. It can increase latency and GPU load, and does not guarantee a fix. "
               L"HDR10 output is not supported in this mode.\n\n"
               L"These modes do not change capture or audio processing. "
               L"The borderless-window option is separate.";
    }
    return L"화면 표시 방식 안내\n\n"
           L"저지연: VSync 대기 없이 최신 프레임을 즉시 표시합니다. 표시 지연을 줄이는 대신 "
           L"화면 경계가 맞지 않을 때 찢어짐이 보일 수 있고, 일부 그래픽 드라이버·모니터 "
           L"조합에서는 간헐적인 검은 화면이나 신호 끊김이 발생할 수 있습니다.\n\n"
           L"VSync: 모니터 주기에 맞춰 표시해 찢어짐을 줄입니다. 저지연과 같은 Flip 출력 "
           L"경로를 사용하지만, 표시 주기를 기다리는 만큼 지연이 늘어날 수 있습니다. "
           L"저지연에서 찢어짐이나 간헐적인 검은 화면이 나타날 때 비교해 보세요.\n\n"
           L"호환성 출력(Blt · VSync): 출력 경로를 Flip에서 Blt로 바꾸며, VSync도 함께 "
           L"적용합니다. 저지연과 VSync 모두에서 문제가 계속될 때 비교해 보세요. "
           L"지연과 GPU 부하가 늘 수 있으며 해결을 보장하지는 않습니다. "
           L"이 모드에서는 HDR10 출력을 지원하지 않습니다.\n\n"
           L"화면 표시 방식은 캡처와 오디오 처리를 바꾸지 않습니다. "
           L"보더리스 창 옵션은 별도 설정입니다.";
}

const wchar_t* CompatibilityLabel(bool english) {
    return english ? L"Compatibility (Blt + VSync)"
                   : L"호환성 출력 (Blt · VSync)";
}

}  // namespace llcv::presentation_ui
