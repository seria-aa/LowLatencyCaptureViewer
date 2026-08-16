#include "ui/AudioOsdLayout.h"

#include <algorithm>

namespace llcv::audio_osd {

Rect RectForClient(int clientWidth) noexcept {
    const int left = (std::max)(kMargin, clientWidth - kWidth - kMargin);
    return {left, kMargin, left + kWidth, kMargin + kHeight};
}

HitTarget HitTest(int clientWidth, int clientHeight, int clientX,
                  int clientY) noexcept {
    if (clientWidth <= 0 || clientHeight <= 0) return HitTarget::Outside;
    const Rect rect = RectForClient(clientWidth);
    if (clientX < rect.left || clientX >= rect.right || clientY < rect.top ||
        clientY >= rect.bottom) return HitTarget::Outside;
    const int x = clientX - rect.left;
    const int y = clientY - rect.top;
    if (y >= 84 && y < 168) {
        if (x >= 16 && x < 160) return HitTarget::Left;
        if (x >= 176 && x < 320) return HitTarget::Right;
    }
    if (y >= 32 && y < 72) return HitTarget::Master;
    return HitTarget::Panel;
}

}  // namespace llcv::audio_osd
