#pragma once

namespace llcv::audio_osd {

inline constexpr int kWidth = 336;
inline constexpr int kHeight = 196;
inline constexpr int kMargin = 16;

struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

enum class HitTarget { Outside, Left, Right, Master, Panel };

Rect RectForClient(int clientWidth) noexcept;
HitTarget HitTest(int clientWidth, int clientHeight, int clientX,
                  int clientY) noexcept;

}  // namespace llcv::audio_osd
