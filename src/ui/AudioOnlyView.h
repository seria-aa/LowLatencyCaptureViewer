#pragma once

#include <windows.h>

#include "ui/AudioOsdLayout.h"

namespace llcv::audio_only_view {

inline constexpr int kBaseClientWidth = 380;
inline constexpr int kBaseClientHeight = 230;

using Rect = audio_osd::Rect;

struct Size {
    int width = 0;
    int height = 0;
};

struct TextSizes {
    int master = 32;
    int channel = 25;
    int body = 14;
    int secondary = 12;
};

Size MinimumClientSize(UINT dpi) noexcept;
TextSizes TextSizesForContent(int contentHeight) noexcept;

struct State {
    bool english = false;
    bool allowBoost = false;
    const wchar_t* outputLabel = L"WASAPI Shared";
    bool clipping = false;
    int masterPercent = 100;
    int leftPercent = 100;
    int rightPercent = 100;
    int hoveredTarget = 0;
    double leftPeakDb = -96.0;
    double rightPeakDb = -96.0;
};

Rect ContentRect(int clientWidth, int clientHeight) noexcept;
audio_osd::HitTarget HitTest(int clientWidth, int clientHeight,
                             int clientX, int clientY) noexcept;
Size FitClientSize(int desiredWidth, int maximumWidth,
                   int maximumHeight) noexcept;
void Paint(HDC dc, const Rect& content, const State& state);

}  // namespace llcv::audio_only_view
