#include "ui/AudioOnlyView.h"
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
int failures = 0;
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace llcv::audio_only_view;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 800;
    info.bmiHeader.biHeight = -754;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return 1;
    }
    const auto oldBitmap = SelectObject(dc, bitmap);
    const RECT all{0, 0, 800, 754};
    FillRect(dc, &all, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    const auto originalFont = GetCurrentObject(dc, OBJ_FONT);
    const auto originalBrush = GetCurrentObject(dc, OBJ_BRUSH);
    const int originalBk = GetBkMode(dc);
    // Measure the actual native fonts: ellipsis in a screenshot can conceal
    // a layout regression. Test both localizations and fractional DPI scales.
    for (const UINT dpi : {96u, 120u, 144u, 168u, 192u, 240u, 288u}) {
        const auto minimum = MinimumClientSize(dpi);
        Check(minimum.width == MulDiv(380, dpi, 96) &&
              minimum.height == MulDiv(230, dpi, 96), "DPI-aware minimum");
        const auto content = ContentRect(minimum.width, minimum.height);
        const int height = content.bottom - content.top;
        const int width = content.right - content.left;
        const auto sizes = TextSizesForContent(height);
        Check(sizes.body >= MulDiv(14, dpi, 96) - 1 &&
              sizes.secondary >= MulDiv(12, dpi, 96) - 1,
              "minimum window preserves readable logical font size");
        const auto fits = [&](const wchar_t* text, int fontHeight, int weight,
                              int slotWidth, int slotHeight) {
            LOGFONTW desc{};
            desc.lfHeight = -fontHeight;
            desc.lfWeight = weight;
            wcscpy_s(desc.lfFaceName, L"Segoe UI");
            HFONT font = CreateFontIndirectW(&desc);
            const auto previous = SelectObject(dc, font);
            SIZE extent{};
            const BOOL measured = GetTextExtentPoint32W(
                dc, text, static_cast<int>(wcslen(text)), &extent);
            const bool fit = measured &&
                extent.cx <= MulDiv(slotWidth, width, 380) &&
                extent.cy <= MulDiv(slotHeight, height, 230);
            if (!fit) std::fprintf(stderr,
                "Text fit: dpi=%u font=%d measured=%ld,%ld slot=%d,%d\n",
                dpi, fontHeight, extent.cx, extent.cy,
                MulDiv(slotWidth, width, 380), MulDiv(slotHeight, height, 230));
            Check(fit, "native text fits its slot without ellipsis or clipping");
            SelectObject(dc, previous);
            DeleteObject(font);
        };
        for (const auto text : {L"Audio", L"오디오"})
            fits(text, sizes.body, FW_NORMAL, 136, 22);
        for (const auto text : {L"Master volume", L"마스터 음량"})
            fits(text, sizes.body, FW_NORMAL, 190, 23);
        for (const auto text : {L"Output level", L"출력 레벨"})
            fits(text, sizes.secondary, FW_NORMAL, 70, 18);
        fits(L"-96.0 dBFS", sizes.secondary, FW_NORMAL, 80, 18);
        for (const auto text : {L"No clipping", L"클리핑 없음"})
            fits(text, sizes.secondary, FW_NORMAL, 94, 17);
        for (const auto text : {L"Wheel: volume · Double-click: reset",
                               L"휠 조절 · 두 번 클릭 초기화"})
            fits(text, sizes.secondary, FW_NORMAL, 247, 17);
        fits(L"WASAPI Exclusive", sizes.secondary, FW_NORMAL, 214, 18);
        fits(L"200", sizes.master, FW_MEDIUM, 121, 46);
        fits(L"100", sizes.channel, FW_MEDIUM, 103, 40);
        fits(L"%", sizes.secondary, FW_NORMAL, 12, 18);
    }
    Check(TextSizesForContent(115).secondary == 6,
          "work-area constrained window keeps text proportional to cards");
    State state{};
    state.leftPeakDb = -9.4;
    state.rightPeakDb = -11.1;
    Paint(dc, {0, 0, 380, 230}, state);
    Check(GetPixel(dc, 3, 3) == RGB(12, 15, 19), "dark charcoal background");
    Check(GetPixel(dc, 0, 115) == RGB(59, 70, 80) &&
          GetPixel(dc, 379, 115) == RGB(59, 70, 80), "thin outer frame");
    Check(GetPixel(dc, 100, 38) == RGB(48, 58, 67) &&
          GetPixel(dc, 100, 107) == RGB(48, 58, 67), "subtle card outlines");
    Check(GetPixel(dc, 190, 94) == RGB(30, 36, 44), "master tile");
    Check(GetPixel(dc, 30, 153) == RGB(23, 28, 34), "channel tile");
    Check(GetPixel(dc, 30, 184) == RGB(129, 206, 186), "real level fills meter");
    Check(GetPixel(dc, 170, 184) == RGB(58, 72, 79), "meter unfilled remainder");
    Check(GetCurrentObject(dc, OBJ_FONT) == originalFont &&
          GetCurrentObject(dc, OBJ_BRUSH) == originalBrush &&
          GetBkMode(dc) == originalBk, "paint restores caller GDI state");

    const auto hasColor = [&](RECT area, COLORREF color) {
        for (int y = area.top; y < area.bottom; ++y)
            for (int x = area.left; x < area.right; ++x)
                if (GetPixel(dc, x, y) == color) return true;
        return false;
    };
    const RECT hintArea{26, 70, 216, 89};
    const RECT masterNumber{218, 48, 339, 90};
    for (const bool english : {false, true}) {
        state.english = english;
        state.allowBoost = false;
        state.masterPercent = 100;
        Paint(dc, {0, 0, 380, 230}, state);
        Check(!hasColor(hintArea, RGB(162, 178, 188)), "boost-off hides capacity hint");
        state.allowBoost = true;
        Paint(dc, {0, 0, 380, 230}, state);
        Check(hasColor(hintArea, RGB(162, 178, 188)), "boost-on shows localized capacity hint");
        Check(!hasColor(masterNumber, RGB(226, 176, 117)), "100 percent stays neutral");
        for (const int volume : {105, 150, 200}) {
            state.masterPercent = volume;
            Paint(dc, {0, 0, 380, 230}, state);
            Check(hasColor(masterNumber, RGB(226, 176, 117)), "boosted master number is amber");
            Check(!hasColor({24, 115, 356, 151}, RGB(226, 176, 117)), "channel numbers remain neutral");
        }
        state.masterPercent = 100;
        Paint(dc, {0, 0, 380, 230}, state);
        Check(!hasColor(masterNumber, RGB(226, 176, 117)), "reset clears boost highlight");
        state.allowBoost = false;
        Paint(dc, {0, 0, 380, 230}, state);
        Check(!hasColor(hintArea, RGB(162, 178, 188)), "disabling boost clears old hint");
    }

    for (int target : {1, 2, 3, 0}) {
        state.hoveredTarget = target;
        Paint(dc, {0, 0, 380, 230}, state);
        Check(GetPixel(dc, 100, 107) == (target == 1 ? RGB(109, 155, 145) : RGB(48, 58, 67)) &&
              GetPixel(dc, 250, 107) == (target == 2 ? RGB(109, 155, 145) : RGB(48, 58, 67)) &&
              GetPixel(dc, 100, 38) == (target == 3 ? RGB(109, 155, 145) : RGB(48, 58, 67)),
              "hover outlines follow only the active card");
        Check(GetPixel(dc, 30, 153) == (target == 1 ? RGB(39, 49, 59) : RGB(23, 28, 34)),
              "left hover restores independently");
        Check(GetPixel(dc, 210, 153) == (target == 2 ? RGB(39, 49, 59) : RGB(23, 28, 34)),
              "right hover restores independently");
        Check(GetPixel(dc, 190, 94) == (target == 3 ? RGB(39, 49, 59) : RGB(30, 36, 44)),
              "master hover restores independently");
    }
    state.clipping = true;
    state.leftPeakDb = std::numeric_limits<double>::quiet_NaN();
    state.rightPeakDb = -96;
    Paint(dc, {0, 0, 380, 230}, state);
    Check(GetPixel(dc, 30, 153) == RGB(23, 28, 34), "unhovered channel");
    Check(GetPixel(dc, 30, 184) == RGB(58, 72, 79), "invalid level paints silent track");
    bool clipVisible = false;
    for (int y = 209; y < 213; ++y)
        for (int x = 14; x < 18; ++x)
            clipVisible |= GetPixel(dc, x, y) == RGB(237, 98, 84);
    Check(clipVisible, "clipping warning");
    // Warm caches before checking that fonts/brushes are released every frame.
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 1000; ++i) {
        state.english = (i % 2) != 0;
        state.masterPercent = i % 201;
        state.allowBoost = (i % 3) != 0;
        state.outputLabel = state.english ? L"WASAPI Exclusive" : L"ASIO";
        Paint(dc, ContentRect(i % 2 ? 380 : 760, i % 2 ? 230 : 460), state);
    }
    Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before,
          "repeated paint does not leak GDI resources");
    Paint(nullptr, {0, 0, 380, 230}, state);
    Paint(dc, {}, state);

    if (argc == 2) {
        FillRect(dc, &all, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        state = State{};
        state.allowBoost = true;
        state.leftPeakDb = -9.4; state.rightPeakDb = -11.1;
        Paint(dc, {10, 10, 390, 240}, state);
        state.english = true;
        state.masterPercent = 200;
        state.outputLabel = L"WASAPI Exclusive";
        Paint(dc, {410, 10, 790, 240}, state);
        state.english = false;
        state.masterPercent = 150;
        state.outputLabel = L"WASAPI Shared";
        Paint(dc, {20, 270, 780, 730}, state);
        GdiFlush();
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        const auto startup = Gdiplus::GdiplusStartup(&token, &input, nullptr);
        Check(startup == Gdiplus::Ok, "preview encoder startup");
        if (startup == Gdiplus::Ok) {
            {
                UINT count = 0, size = 0;
                Gdiplus::GetImageEncodersSize(&count, &size);
                std::vector<unsigned char> bytes(size);
                auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(bytes.data());
                bool saved = false;
                if (Gdiplus::GetImageEncoders(count, size, encoders) == Gdiplus::Ok) {
                    Gdiplus::Bitmap image(bitmap, nullptr);
                    for (UINT i = 0; i < count; ++i) {
                        if (wcscmp(encoders[i].MimeType, L"image/png") == 0)
                            saved = image.Save(argv[1], &encoders[i].Clsid) == Gdiplus::Ok;
                    }
                }
                Check(saved, "native preview saved");
            }
            Gdiplus::GdiplusShutdown(token);
        }
    }
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    if (!failures) std::puts("PASS audio-only paint, metering and GDI lifetime");
    return failures ? 1 : 0;
}
