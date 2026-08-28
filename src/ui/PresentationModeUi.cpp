#include "ui/PresentationModeUi.h"

namespace llcv::presentation_ui {

const wchar_t* ImmediateLabel(bool english) {
    return english ? L"Immediate (minimum latency)"
                   : L"저지연 (최소 지연)";
}

const wchar_t* VSyncLabel(bool english) {
    return english ? L"VSync (compatibility)"
                   : L"VSync (호환성 우선)";
}

const wchar_t* HelpText(bool english) {
    if (english) {
        return L"Presentation mode\n\n"
               L"Immediate presents the newest frame without waiting for VSync. This minimizes "
               L"display latency but can show tearing. On some graphics-driver and display "
               L"combinations it can also cause an intermittent black screen or signal loss.\n\n"
               L"VSync follows the monitor refresh and prioritizes compatibility. It reduces "
               L"tearing, but may wait for the next refresh interval and can have different pacing "
               L"on mixed-refresh monitors. If Immediate causes signal loss, choose VSync; the "
               L"borderless-window option can remain enabled.";
    }
    return L"화면 표시 방식 안내\n\n"
           L"저지연: VSync 대기 없이 최신 프레임을 즉시 표시합니다. 표시 지연을 줄이는 대신 "
           L"화면 경계가 맞지 않을 때 찢어짐이 보일 수 있고, 일부 그래픽 드라이버·모니터 "
           L"조합에서는 간헐적인 검은 화면이나 신호 끊김이 발생할 수 있습니다.\n\n"
           L"VSync: 모니터 주기에 맞춰 표시하는 호환성 우선 모드입니다. 찢어짐을 줄이는 대신 "
           L"다음 표시 주기까지 기다릴 수 있어 지연이 늘어날 수 있고, 주사율이 다른 모니터에서는 "
           L"프레임 페이싱이 달라질 수 있습니다. 저지연에서 신호가 끊기면 VSync를 선택하세요. "
           L"보더리스 창 옵션은 그대로 사용해도 됩니다.";
}

}  // namespace llcv::presentation_ui
