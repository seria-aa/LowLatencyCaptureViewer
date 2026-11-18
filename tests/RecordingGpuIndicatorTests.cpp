// Snapshot-only status tests and offscreen WARP texture tests. This fixture
// never opens capture/audio devices, visible windows, or user settings.
#include "recording/RecordingGpuIndicator.h"
#include "recording/Recorder.h"
#include "recording/RecordingStatusPresentation.h"
#include "ui/OverlayStyle.h"
#include "ui/ViewerOverlayGeometry.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace llcv::recording;
namespace {
int failures = 0, openVideo = 0, openFolder = 0, openSettings = 0;
void Expect(bool condition, const char* label) {
    if (!condition) { ++failures; std::printf("FAIL: %s\n", label); }
}
void Check(HRESULT hr, const char* label) {
    if (FAILED(hr)) {
        std::printf("FAIL: %s 0x%08X\n", label, static_cast<unsigned>(hr));
        ++failures;
        throw hr;
    }
}
Snapshot Active(uint64_t elapsedMs = 1000) {
    Snapshot snapshot;
    snapshot.phase = Phase::Recording;
    snapshot.busy = true;
    snapshot.elapsedMs = elapsedMs;
    snapshot.path = L"C:\\LLCV-test\\fixture.mp4";
    return snapshot;
}
LRESULT CALLBACK MessageProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == kOpenRecordingSettings) { ++openSettings; return 0; }
    return DefWindowProcW(hwnd, message, w, l);
}
void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void TestPublicationAndClicks() {
    auto snapshot = Active();
    PublishGpuIndicator(snapshot, false, false, 96);
    const auto initial = GpuIndicatorRevision();
    PublishGpuIndicator(snapshot, false, false, 96);
    Expect(GpuIndicatorRevision() == initial, "same snapshot does not churn cache revision");
    snapshot.elapsedMs = 1499;
    PublishGpuIndicator(snapshot, false, false, 96);
    Expect(GpuIndicatorRevision() == initial, "subsecond elapsed updates do not redraw identical text");
    snapshot.elapsedMs = 2000;
    PublishGpuIndicator(snapshot, false, false, 96);
    Expect(GpuIndicatorRevision() > initial, "elapsed seconds update the indicator");
    auto revision = GpuIndicatorRevision();
    snapshot.paused = true;
    PublishGpuIndicator(snapshot, false, false, 96);
    Expect(GpuIndicatorRevision() > revision, "pause state updates the indicator");
    revision = GpuIndicatorRevision();
    PublishGpuIndicator(snapshot, true, false, 96);
    Expect(GpuIndicatorRevision() > revision, "language change updates indicator publication");
    revision = GpuIndicatorRevision();
    PublishGpuIndicator(snapshot, true, true, 96);
    Expect(llcv::ui::HighContrastEnabled() || GpuIndicatorRevision() > revision,
        "light theme updates resolved colors outside high contrast");
    revision = GpuIndicatorRevision();
    PublishGpuIndicator(snapshot, true, true, 144);
    Expect(GpuIndicatorRevision() > revision, "DPI change updates indicator placement");
    PublishGpuIndicator(snapshot, false, false, 96);
    Expect(GpuIndicatorHitTest(800, 600, 680, 570), "active card hit is recognized");
    Expect(!GpuIndicatorHitTest(800, 600, 630, 570), "space freed by the smaller REC badge is not clickable");
    Expect(!GpuIndicatorHitTest(800, 600, 420, 470), "transparent texture area is not clickable");
    Expect(!GpuIndicatorHitTest(800, 600, 0, 0), "outside card hit is rejected");
    Expect(!GpuIndicatorHitTest(0, 0, 0, 0), "zero client cannot receive a hit");
    Expect(!GpuIndicatorHitTest(1, 1, 2, 1), "tiny client cannot receive off-client hits");
    Expect(!GpuIndicatorHitTest(50, 20, 50, 19), "small client right boundary is excluded");
    Expect(!GpuIndicatorHitTest(50, 20, -1, 10), "negative client position is excluded");

    const wchar_t* className = L"LLCV.RecordingGpuIndicatorTests.Message";
    WNDCLASSW wc{};
    wc.lpfnWndProc = MessageProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    Expect(RegisterClassW(&wc) != 0, "message-only fixture class registers");
    HWND viewer = CreateWindowExW(0, className, nullptr, 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    Expect(viewer != nullptr, "message-only fixture creates without display");
    Expect(HandleGpuIndicatorClick(viewer, 800, 600, 680, 570), "active card click is handled");
    Pump();
    Expect(openSettings == 1 && openVideo == 0 && openFolder == 0,
        "active card opens settings rather than unfinished file");

    snapshot.phase = Phase::Finalizing;
    PublishGpuIndicator(snapshot, false, false, 96);
    Expect(HandleGpuIndicatorClick(viewer, 800, 600, 744, 570), "finalizing card click is handled");
    Expect(!GpuIndicatorHitTest(800, 600, 690, 570), "the finalizing card no longer occupies excess width");
    snapshot.phase = Phase::Starting;
    PublishGpuIndicator(snapshot, false, false, 96);
    Expect(GpuIndicatorHitTest(800, 600, 700, 570) &&
           !GpuIndicatorHitTest(800, 600, 675, 570), "starting card has its own tighter width");
    Pump();
    Expect(openVideo == 0 && openFolder == 0, "finalizing status never opens unfinished file");

    snapshot.phase = Phase::Saved;
    snapshot.busy = false;
    snapshot.playable = false;
    PublishGpuIndicator(snapshot, false, false, 96);
    Expect(HandleGpuIndicatorClick(viewer, 800, 600, 661, 570), "unverified saved card click is consumed");
    Expect(openVideo == 0 && openFolder == 0, "unverified saved file has no open action");
    snapshot.playable = true;
    PublishGpuIndicator(snapshot, false, false, 96);
    Expect(HandleGpuIndicatorClick(viewer, 800, 600, 661, 570), "saved video action is handled");
    Expect(HandleGpuIndicatorClick(viewer, 800, 600, 744, 570), "saved folder action is handled");
    Expect(openVideo == 1 && openFolder == 1, "verified terminal actions preserve video/folder distinction");

    snapshot.phase = Phase::Failed;
    snapshot.failure = Failure::WriteFailed;
    snapshot.playable = false;
    PublishGpuIndicator(snapshot, false, false, 96);
    HandleGpuIndicatorClick(viewer, 800, 600, 448, 570);
    Expect(openVideo == 1 && openFolder == 1, "unverified failure cannot open a video");
    snapshot.playable = true;
    PublishGpuIndicator(snapshot, false, false, 96);
    HandleGpuIndicatorClick(viewer, 800, 600, 448, 570);
    Expect(openVideo == 2, "verified recoverable partial file retains its open action");

    PublishGpuIndicator({}, false, false, 96);
    Expect(!GpuIndicatorHitTest(800, 600, 650, 570), "idle status removes click target");
    Expect(!HandleGpuIndicatorClick(viewer, 800, 600, 448, 570), "hidden status cannot trigger stale actions");
    if (viewer) DestroyWindow(viewer);
    UnregisterClassW(className, wc.hInstance);

    std::atomic<bool> finished{false}, monotonic{true};
    std::thread reader([&] {
        uint64_t previous = 0;
        while (!finished.load(std::memory_order_acquire)) {
            const auto value = GpuIndicatorRevision();
            if (value < previous) monotonic = false;
            previous = value;
            (void)GpuIndicatorHitTest(800, 600, 650, 570);
        }
    });
    for (uint64_t i = 0; i < 200; ++i)
        PublishGpuIndicator(Active(1000 * i), (i & 1) != 0, false, 96);
    finished.store(true, std::memory_order_release);
    reader.join();
    Expect(monotonic.load(), "concurrent reader sees monotonic publication revisions");
}

struct Pixels {
    uint64_t hash = 1469598103934665603ULL;
    size_t covered = 0, solidInterior = 0;
    std::array<BYTE, 4> corner{}, center{};
};
Pixels ReadPixels(ID3D11Device* device, ID3D11DeviceContext* context,
                  ID3D11ShaderResourceView* view) {
    Expect(view != nullptr, "GPU indicator owns an SRV");
    if (!view) throw E_POINTER;
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    Check(resource.As(&texture), "indicator resource is Texture2D");
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    Expect(desc.Width == 760 && desc.Height == 256 && desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM,
        "indicator raster is double its logical dimensions");
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&desc, nullptr, &staging), "create offscreen readback");
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map offscreen indicator");
    Pixels result;
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x) {
            const auto* pixel = row + x * 4;
            if (pixel[3]) ++result.covered;
            if (x>=500 && x<730 && y>=212 && y<242 && pixel[3]==255) ++result.solidInterior;
            for (UINT c = 0; c < 4; ++c) result.hash = (result.hash ^ pixel[c]) * 1099511628211ULL;
        }
    }
    std::memcpy(result.corner.data(), mapped.pData, 4);
    std::memcpy(result.center.data(), static_cast<const BYTE*>(mapped.pData) +
        220 * mapped.RowPitch + 738 * 4, 4);
    context->Unmap(staging.Get(), 0);
    return result;
}

void TestOffscreenTexture() {
    namespace overlay = llcv::overlay_ui;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, nullptr, &context), "create offscreen WARP device");
    ComPtr<ID2D1Factory> d2d;
    Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()), "create offscreen D2D factory");
    ComPtr<IDWriteFactory> dwrite;
    Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())), "create DirectWrite factory");
    llcv::overlay_ui::Fonts fonts; // Explicitly exercise installed-font fallback.
    for (bool english : {false, true}) {
        Snapshot rejected; rejected.phase = Phase::Failed;
        rejected.result = RgbRecordingTooSlow;
        rejected.rgbConversionMs = 31; rejected.rgbBudgetMs = 1000.0 / 60;
        const auto view = presentation::Build(rejected, english);
        const auto message = view.title + L"\n" + view.description;
        ComPtr<IDWriteTextFormat> format; ComPtr<IDWriteTextLayout> layout;
        Check(fonts.CreateFormat(dwrite.Get(), DWRITE_FONT_WEIGHT_MEDIUM, 40.0f / 3.0f,
            english, &format), "RGB rejection font");
        Check(dwrite->CreateTextLayout(message.c_str(), static_cast<UINT32>(message.size()),
            format.Get(), 356, 84, &layout), "RGB rejection text layout");
        DWRITE_TEXT_METRICS metrics{};
        Check(layout->GetMetrics(&metrics), "RGB rejection text metrics");
        Expect(metrics.height <= 84 && metrics.width <= 356,
            "RGB preflight guidance fits existing failure card in both languages");
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        const auto fits = [&](const wchar_t* text, float width) {
            ComPtr<IDWriteTextLayout> line;
            Check(dwrite->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)),
                format.Get(), width, 22, &line), "compact badge text layout");
            DWRITE_TEXT_METRICS size{};
            Check(line->GetMetrics(&size), "compact badge text metrics");
            if (size.width > width || size.height > 22)
                std::printf("Compact text %s needs %.1f x %.1f, available %.1f x 22\n",
                    english ? "EN" : "KO", size.width, size.height, width);
            return size.width <= width && size.height <= 22;
        };
        Expect(fits(L"● REC  88:88:88", 104), "REC and timer fit with room on both sides");
        Snapshot transition;
        transition.phase = Phase::Starting;
        Expect(fits(presentation::Build(transition, english).title.c_str(), english ? 140.f : 88.f),
            "starting caption fits its narrower card");
        transition.phase = Phase::Finalizing;
        Expect(fits(presentation::Build(transition, english).title.c_str(), english ? 136.f : 72.f),
            "finalizing caption fits its narrower card");
        Expect(fits(english ? L"Open video" : L"영상 열기", english ? 80.f : 68.f) &&
               fits(english ? L"Open folder" : L"폴더 열기", english ? 80.f : 68.f),
            "both saved actions fit their compact cells");
    }
    GpuIndicator indicator;
    Check(indicator.Initialize(device.Get(), d2d.Get(), dwrite.Get(), fonts, true),
        "initialize GPU indicator");
    PublishGpuIndicator(Active(), true, false, 96);
    Check(indicator.Refresh(), "refresh recording texture");
    Expect(indicator.Visible(), "active recording makes GPU indicator visible");
    const auto rect = indicator.QuadRect(800, 600);
    Expect(rect.right == 784 && rect.bottom == 584, "REC has equal 16 px outer margins at 96 DPI");
    const auto checkVolume = [&](SIZE viewport, bool requireOriginalSize, const char* label) {
        const auto badge = indicator.CardRect(static_cast<UINT>(viewport.cx), static_cast<UINT>(viewport.cy));
        const auto before = overlay::FitPanel(viewport, {260, 82}, 24, true, true);
        const auto after = overlay::ClearRecordingCard(before, badge, viewport, true, true);
        Expect(overlay::Fits(badge, viewport) && overlay::Fits(after, viewport) &&
               !overlay::Overlaps(after, badge), label);
        if (requireOriginalSize)
            Expect(overlay::Width(after) == 260 && overlay::Height(after) == 82,
                   "collision does not shrink the volume panel unnecessarily");
        Expect(GpuIndicatorHitTest(static_cast<UINT>(viewport.cx), static_cast<UINT>(viewport.cy),
                                  (badge.left + badge.right) / 2, (badge.top + badge.bottom) / 2),
               "recording card remains clickable at its painted position");
    };
    const auto activeCard = indicator.CardRect(800, 600);
    Expect(activeCard.left == 664 && activeCard.top == 556 &&
           activeCard.right == rect.right && activeCard.bottom == rect.bottom,
           "REC collision bounds exclude the transparent texture area");
    checkVolume({800, 600}, true, "bottom-right volume clears the REC badge");
    const auto leftVolume = overlay::FitPanel({800, 600}, {260, 82}, 24, false, true);
    const auto same = overlay::ClearRecordingCard(leftVolume, activeCard, {800, 600}, false, true);
    Expect(EqualRect(&same, &leftVolume) != FALSE, "noncolliding volume keeps the chosen corner");
    const auto narrow = indicator.QuadRect(320, 100);
    const auto narrowMargin = 320 - narrow.right;
    Expect(narrowMargin > 0 && narrowMargin == 100 - narrow.bottom &&
        narrow.left >= 0 && narrow.top >= 0, "shrunk layout keeps both margins and fits viewport");
    const auto recording = ReadPixels(device.Get(), context.Get(), indicator.ShaderView());
    Expect(recording.covered > 4000 && recording.covered <= 4 * 120 * 28 && recording.corner[3] == 0,
        "higher-resolution raster keeps the compact badge footprint and transparent margins");
    if (!llcv::ui::HighContrastEnabled())
        Expect(recording.center[3] >= 191 && recording.center[3] <= 196,
            "badge fill is translucent but the backdrop outside it is clear");
    Expect(recording.solidInterior > 30, "REC lettering itself is opaque");
    Check(indicator.Refresh(), "unchanged refresh succeeds");
    Expect(ReadPixels(device.Get(), context.Get(), indicator.ShaderView()).hash == recording.hash,
        "unchanged snapshot preserves cached pixels");

    auto snapshot = Active();
    snapshot.paused = true;
    PublishGpuIndicator(snapshot, true, false, 96);
    Check(indicator.Refresh(), "refresh paused texture");
    Expect(ReadPixels(device.Get(), context.Get(), indicator.ShaderView()).hash != recording.hash,
        "pause changes rendered status text");

    snapshot.phase = Phase::Failed;
    snapshot.busy = false;
    snapshot.failure = Failure::WriteFailed;
    PublishGpuIndicator(snapshot, true, false, 96);
    Check(indicator.Refresh(), "refresh failure texture");
    checkVolume({320, 180}, false, "small view fits volume beside or above a large failure card");
    const auto failed = ReadPixels(device.Get(), context.Get(), indicator.ShaderView());
    Expect(failed.covered > recording.covered && failed.center[3] == 255, "failure card is solid and expands without reallocating texture");
    PublishGpuIndicator(Active(), true, false, 96);
    Check(indicator.Refresh(), "refresh recording after failure");
    Expect(ReadPixels(device.Get(), context.Get(), indicator.ShaderView()).hash == recording.hash,
        "smaller resumed card clears old terminal panel pixels");

    snapshot = Active();
    snapshot.phase = Phase::Saved;
    snapshot.busy = false;
    snapshot.playable = true;
    for (const bool english : {true, false}) {
        PublishGpuIndicator(snapshot, english, false, 96);
        Check(indicator.Refresh(), "refresh saved texture");
        checkVolume({489, 275}, true, "small view keeps both saved actions and volume visible");
    }

    PublishGpuIndicator(Active(), true, false, 144);
    Check(indicator.Refresh(), "refresh 150 percent DPI placement");
    const auto scaled = indicator.QuadRect(1280, 720);
    Expect(scaled.right == 1256 && scaled.bottom == 696, "both margins scale with DPI");
    PublishGpuIndicator(Active(), true, false, 384);
    Check(indicator.Refresh(), "refresh high DPI placement");
    const auto large = indicator.QuadRect(3840, 2160);
    Expect(large.right - large.left <= 700 && large.bottom - large.top <= 440,
        "high DPI quad fits existing HDR overlay scratch");
    const auto smallRect = indicator.QuadRect(1, 1);
    Expect(smallRect.left >= 0 && smallRect.top >= 0 && smallRect.right <= 1 && smallRect.bottom <= 1,
        "tiny client quad remains inside client bounds");
    const auto empty = indicator.QuadRect(0, 0);
    Expect(IsRectEmpty(&empty), "zero client returns empty quad");

    PublishGpuIndicator({}, true, false, 96);
    Check(indicator.Refresh(), "hide indicator");
    Expect(!indicator.Visible(), "idle hides cached card");
    indicator.Reset();
    Expect(!indicator.Visible() && !indicator.ShaderView(), "reset releases GPU view and visibility");
    Check(indicator.Initialize(device.Get(), d2d.Get(), dwrite.Get(), fonts, true),
        "reinitialize after device resources reset");
    Check(indicator.Refresh(), "refresh idle after reinitialize");
    Expect(!indicator.Visible(), "reinitialize does not restore stale active card");
    PublishGpuIndicator(Active(), true, false, 96);
    Check(indicator.Refresh(), "refresh active after reinitialize");
    Expect(ReadPixels(device.Get(), context.Get(), indicator.ShaderView()).hash == recording.hash,
        "reinitialized active texture matches original recording card");
}
} // namespace

// Test-only replacement: verify intent without launching any file or app.
namespace llcv::recording {
void OpenRecording(HWND, bool video, bool) {
    if (video) ++openVideo;
    else ++openFolder;
}
}

int main(int argc, char** argv) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        std::printf("FAIL: initialize COM 0x%08X\n", static_cast<unsigned>(initialized));
        return 1;
    }
    try {
        if (argc > 1 && std::strcmp(argv[1], "--gpu") == 0) TestOffscreenTexture();
        else TestPublicationAndClicks();
    } catch (HRESULT) {
        // Check already reports the failing operation.
    }
    CoUninitialize();
    if (!failures) std::printf("Recording GPU indicator tests passed.\n");
    return failures ? 1 : 0;
}
