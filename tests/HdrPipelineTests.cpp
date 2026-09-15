// Actual app renderer + synthetic P010 + GPU readback. No capture/audio device,
// HDR display, display-mode changes, settings writes or visible test window.
#define LLCV_GPU_DIAGNOSTICS
#include "../src/main.cpp"
#undef fwprintf
#include "../src/audio/AsioOutput.cpp"
#include <d3d11sdklayers.h>
#include <array>

static void Require(bool ok, const char* name) {
    if (!ok) { std::printf("FAIL: %s\n", name); std::exit(1); }
}
static void Check(HRESULT hr, const char* name) {
    if (FAILED(hr)) { std::printf("FAIL %s: %08lX\n", name, static_cast<unsigned long>(hr)); std::exit(1); }
}
static double Pq(double nits) {
    // Independent CPU reference from BT.2100-3 Table 4, not shader evaluation.
    const double p = std::pow(nits / 10000.0, 2610.0/16384.0);
    return std::pow((3424.0/4096.0 + 2413.0/128.0*p) / (1 + 2392.0/128.0*p), 2523.0/32.0);
}
static double Nits(double pq) {
    const double p = std::pow(pq, 32.0/2523.0);
    return 10000 * std::pow((std::max)(0.0, p-3424.0/4096.0) /
        (2413.0/128.0-2392.0/128.0*p), 16384.0/2610.0);
}
static double LinearSrgb(double s) {
    return s <= 0.04045 ? s / 12.92 : std::pow((s+0.055)/1.055, 2.4);
}
static std::array<unsigned, 3> Read(DirectD3D11Renderer& r, UINT x = 100, UINT y = 100) {
    D3D11_TEXTURE2D_DESC d{}; r.backBuffer->GetDesc(&d);
    d.Width = d.Height = 1; d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* copy = nullptr;
    Check(r.device->CreateTexture2D(&d, nullptr, &copy), "readback texture");
    r.context->OMSetRenderTargets(0, nullptr, nullptr);
    const D3D11_BOX box{x, y, 0, x+1, y+1, 1};
    r.context->CopySubresourceRegion(copy, 0, 0, 0, 0, r.backBuffer, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(r.context->Map(copy, 0, D3D11_MAP_READ, 0, &mapped), "readback");
    const unsigned v = *static_cast<unsigned*>(mapped.pData);
    r.context->Unmap(copy, 0); copy->Release();
    if (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
        return {v & 1023, (v >> 10) & 1023, (v >> 20) & 1023};
    return {(v >> 16) & 255, (v >> 8) & 255, v & 255};
}
static void Near(unsigned value, double expected, double tolerance, const char* name) {
    if (std::abs(value - expected) > tolerance) {
        std::printf("%s actual=%u expected=%.3f\n", name, value, expected); Require(false, name);
    }
}
static void ClearUi(DirectD3D11Renderer& r, float red, float green, float blue, float alpha) {
    ID3D11RenderTargetView* view = nullptr;
    Check(r.device->CreateRenderTargetView(r.osdOverlayTexture, nullptr, &view), "UI target");
    const float premultiplied[]{red*alpha, green*alpha, blue*alpha, alpha};
    r.context->ClearRenderTargetView(view, premultiplied); view->Release();
}
static void ClearVideo(DirectD3D11Renderer& r, double nits) {
    const float value = static_cast<float>(Pq(nits));
    const float color[]{value, value, value, 1};
    r.context->ClearRenderTargetView(r.backBufferRenderTarget, color);
}
static void DrawUi(DirectD3D11Renderer& r, LONG left = 0, LONG top = 0) {
    const LONG right = left+700, bottom = top+440;
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(r.outputWidth), static_cast<float>(r.outputHeight), 0, 1};
    r.context->RSSetViewports(1, &vp);
    r.context->OMSetRenderTargets(1, &r.backBufferRenderTarget, nullptr);
    r.context->OMSetBlendState(r.overlayBlendState, nullptr, 0xffffffff);
    r.context->IASetInputLayout(nullptr);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    r.context->VSSetShader(r.overlayVertexShader, nullptr, 0);
    const float rect[]{-1+2.0f*left/r.outputWidth, 1-2.0f*top/r.outputHeight,
                       -1+2.0f*right/r.outputWidth, 1-2.0f*bottom/r.outputHeight};
    r.context->UpdateSubresource(r.overlayRectBuffer, 0, nullptr, rect, 0, 0);
    r.context->VSSetConstantBuffers(0, 1, &r.overlayRectBuffer);
    r.context->PSSetShader(r.overlayPixelShader, nullptr, 0);
    r.context->PSSetSamplers(0, 1, &r.overlaySampler);
    if (r.hdrOutput) Check(r.prepareHdrOverlay(left, top, right, bottom), "HDR background");
    r.context->PSSetShaderResources(0, 1, &r.osdOverlayShaderView);
    r.context->Draw(4, 0);
    ID3D11ShaderResourceView* empty[2]{};
    r.context->PSSetShaderResources(0, 2, empty);
    r.context->OMSetRenderTargets(0, nullptr, nullptr);
    r.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
}
static void RequireCleanGpu(DirectD3D11Renderer& r) {
    ID3D11InfoQueue* q = nullptr;
    Check(r.device->QueryInterface(IID_PPV_ARGS(&q)), "debug messages");
    unsigned errors = 0;
    for (UINT64 i = 0; i < q->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0; q->GetMessage(i, nullptr, &size);
        std::vector<BYTE> bytes(size); auto* m = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
        q->GetMessage(i, m, &size);
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) { ++errors; std::printf("GPU: %s\n", m->pDescription); }
    }
    q->Release(); Require(errors == 0, "GPU warnings/errors");
}
static void Benchmark(DirectD3D11Renderer& r) {
    ID3D11Query *disjoint = nullptr, *start = nullptr, *end = nullptr;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    Check(r.device->CreateQuery(&desc, &disjoint), "timer disjoint");
    desc.Query = D3D11_QUERY_TIMESTAMP;
    Check(r.device->CreateQuery(&desc, &start), "timer start");
    Check(r.device->CreateQuery(&desc, &end), "timer end");
    r.context->Begin(disjoint); r.context->End(start);
    for (int i = 0; i < 200; ++i) DrawUi(r);
    r.context->End(end); r.context->End(disjoint); r.context->Flush();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
    HRESULT status = S_FALSE;
    for (int i = 0; i < 1000 && status == S_FALSE; ++i) {
        status = r.context->GetData(disjoint, &clock, sizeof(clock), 0);
        if (status == S_FALSE) Sleep(1);
    }
    UINT64 t0 = 0, t1 = 0;
    if (status == S_OK && !clock.Disjoint && clock.Frequency &&
        r.context->GetData(start, &t0, sizeof(t0), 0) == S_OK &&
        r.context->GetData(end, &t1, sizeof(t1), 0) == S_OK)
        std::printf("%s 700x440 UI GPU average: %.4f ms (200 draws, local GPU only)\n",
            r.hdrOutput ? "HDR linear composite + regional copy" : "SDR baseline composite",
            (t1-t0) * 1000.0 / clock.Frequency / 200.0);
    else std::puts("GPU timestamp unavailable; not a performance assertion");
    end->Release(); start->Release(); disjoint->Release();
}
int main() {
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM");
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"HDR pixel test", WS_POPUP,
        0, 0, 800, 500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(hwnd != nullptr, "hidden window");
    g_settings.pixelPerfect = false; g_settings.presentationMode = PresentationMode::VSync;
    g_osdVisible = false; g_audioOsdVisible = false; g_volumeHudUntilMs = 0;
    {
        DirectD3D11Renderer r;
        HRESULT hr = r.initialize(hwnd, 64, 64, 30, VideoPixelFormat::P010, true);
        if (hr == DXGI_ERROR_UNSUPPORTED || hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            std::puts("SKIP: GPU HDR conversion/debug layer unavailable"); DestroyWindow(hwnd); return 77;
        }
        Check(hr, "HDR initialize");
        const auto display = llcv::hdr::QueryDisplay(hwnd);
        Require(g_hdrDisplayState.load() == display.hdr, "physical display state separate from PQ signal");
        Require(r.hdrOverlayBackground == nullptr, "no HDR scratch allocation without UI");
        Check(r.drawOverlayQuads(), "hidden overlays");
        Require(r.hdrOverlayBackground == nullptr, "no scratch/copy on UI-free frame");
        std::vector<unsigned short> p(64*64*3/2, 512 << 6);
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE;
        auto blit = [&] {
            r.upload(reinterpret_cast<const BYTE*>(p.data()), 128);
            stream.pInputSurface = r.inputViews[r.activeUploadSurface];
            Check(r.videoContext->VideoProcessorBlt(r.processor, r.outputView, 0, 1, &stream), "HDR VP blit");
        };
        // Table 9: Y [64,940], neutral chroma 512, chroma excursion 896.
        // Exhaust the entire ten-bit container, including clipping boundaries.
        unsigned previous = 0;
        for (unsigned y = 0; y <= 1023; ++y) {
            std::fill(p.begin(), p.begin()+4096, static_cast<unsigned short>(y << 6)); blit();
            const auto rgb = Read(r, 400, 250);
            for (unsigned c : rgb) Near(c, std::clamp((y-64.0)/876.0, 0.0, 1.0)*1023, 3, "PQ grey ramp");
            Require(rgb[0] >= previous, "ten-bit luma conversion stays monotonic");
            previous = rgb[0];
        }
        // Non-neutral YCbCr patches catch matrix mistakes hidden by grey tests.
        for (auto uv : {std::array<int, 2>{560, 470}, {450, 570}, {620, 512}, {512, 600}}) {
            std::fill(p.begin(), p.begin()+4096, static_cast<unsigned short>(512 << 6));
            for (size_t i = 4096; i < p.size(); i += 2) { p[i] = static_cast<unsigned short>(uv[0]<<6); p[i+1] = static_cast<unsigned short>(uv[1]<<6); }
            blit(); const auto rgb = Read(r, 400, 250);
            const double y = (512.0-64)/876, cb = (uv[0]-512.0)/896, cr = (uv[1]-512.0)/896;
            const double red = y + 1.4746*cr, blue = y + 1.8814*cb;
            const double green = (y-0.2627*red-0.0593*blue)/0.6780;
            Near(rgb[0], red*1023, 4, "2020 red"); Near(rgb[1], green*1023, 4, "2020 green"); Near(rgb[2], blue*1023, 4, "2020 blue");
        }
        g_hdrUiWhiteNits = 203.0f;
        // Test every sRGB grey code across the piecewise transfer-function knee.
        // Byte-exact values avoid mistaking BGRA8 quantization for shader error.
        for (unsigned code = 0; code <= 255; ++code) {
            const float s = code / 255.0f;
            ClearUi(r, s, s, s, 1); DrawUi(r);
            for (auto c : Read(r))
                Near(c, Pq(203 * LinearSrgb(code/255.0))*1023, 2, "sRGB UI grey ramp to PQ");
        }
        // Exercise BOTH PQ directions, rather than a round trip which could
        // conceal a matching pair of wrong transfer functions. Reference uses
        // the quantized background and premultiplied UI values actually stored.
        for (double background : {0.0, 0.001, 0.01, 0.1, 1.0, 10.0, 100.0, 1000.0, 4000.0, 10000.0}) {
            for (unsigned a : {1u, 16u, 64u, 128u, 240u, 254u}) {
                const double alpha = a/255.0;
                const double uiCode = std::round(0.375*alpha*255)/255;
                const double uiLight = 203 * LinearSrgb(uiCode/alpha);
                ClearVideo(r, background);
                const double backgroundLight = Nits(Read(r)[0]/1023.0);
                ClearUi(r, 0.375f, 0.375f, 0.375f, static_cast<float>(alpha)); DrawUi(r);
                Near(Read(r)[0], Pq(uiLight*alpha+backgroundLight*(1-alpha))*1023, 3,
                    "absolute-light PQ alpha blend across 0..10000 nit");
            }
        }
        // The production cache is drawn by D2D, not ClearRenderTargetView.
        // Verify its actual premultiplication matches the shader's input model.
        ID2D1SolidColorBrush* brush = nullptr;
        Check(r.osdCacheTarget->CreateSolidColorBrush(
            D2D1::ColorF(0.375f, 0.5f, 0.75f, 0.5f), &brush), "D2D HDR test brush");
        r.osdCacheTarget->BeginDraw();
        r.osdCacheTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        r.osdCacheTarget->FillRectangle(D2D1::RectF(0, 0, 700, 440), brush);
        Check(r.osdCacheTarget->EndDraw(), "D2D premultiplied cache"); brush->Release();
        ClearVideo(r, 100);
        const double actualBackground = Nits(Read(r)[0]/1023.0);
        DrawUi(r); const auto d2d = Read(r);
        const double rr = LinearSrgb(48.0/128), gg = LinearSrgb(64.0/128), bb = LinearSrgb(96.0/128);
        const double linear2020[]{0.627404*rr+0.329283*gg+0.043313*bb,
            0.069097*rr+0.919540*gg+0.011362*bb, 0.016391*rr+0.088013*gg+0.895595*bb};
        for (size_t c = 0; c < 3; ++c)
            Near(d2d[c], Pq(203*linear2020[c]*(128.0/255)+actualBackground*(127.0/255))*1023, 3,
                "real D2D cache to linear BT.2020/PQ blend");
        ClearUi(r, 1, 1, 1, 1); ClearVideo(r, 100); DrawUi(r);
        for (auto c : Read(r)) Near(c, Pq(203)*1023, 2, "UI reference white 203 nit");
        g_hdrUiWhiteNits = 80.0f; DrawUi(r);
        Near(Read(r)[0], Pq(80)*1023, 2, "Windows UI white update 80 nit");
        g_hdrUiWhiteNits = 203.0f;
        ClearUi(r, 0, 0, 0, 0.5f); ClearVideo(r, 100); DrawUi(r);
        Near(Read(r)[0], Pq(100*(1-128.0/255))*1023, 2, "black half alpha is about 50 nit, not 5 nit");
        DrawUi(r);
        Near(Read(r)[0], Pq(100*std::pow(1-128.0/255, 2))*1023, 3, "overlapping overlays composite in order");
        ClearUi(r, 0, 0, 0, 0); ClearVideo(r, 100); const auto before = Read(r); DrawUi(r);
        Require(Read(r) == before, "transparent UI preserves PQ codes exactly");
        ClearUi(r, 1, 0, 0, 1); ClearVideo(r, 100); DrawUi(r);
        const auto redUi = Read(r);
        Near(redUi[0], Pq(203*0.627404)*1023, 2, "UI red gamut R");
        Near(redUi[1], Pq(203*0.069097)*1023, 2, "UI red gamut G");
        Near(redUi[2], Pq(203*0.016391)*1023, 2, "UI red gamut B");
        ClearUi(r, 0, 0, 0, 0.5f); ClearVideo(r, 100); DrawUi(r, -50, -20);
        Near(Read(r, 1, 1)[0], Pq(100*(1-128.0/255))*1023, 2, "clipped overlay copy origin");
        Near(Read(r, 799, 499)[0], Pq(100)*1023, 2, "outside UI unchanged");
        Require(r.prepareHdrOverlay(900, 0, 1000, 200) == S_FALSE, "off-screen overlay skipped");
        // Keep the requested 90% HDR panel opacity without changing text/composition.
        for (auto* panelBrush : {r.osdCacheBackgroundBrush, r.volumeCacheBackgroundBrush,
                            r.audioCacheBackgroundBrush})
            Require(std::abs(panelBrush->GetColor().a - 0.90f) < 0.0001f, "HDR panel opacity");
        Require(r.osdCacheTextBrush->GetColor().a == 1.0f, "HDR text opacity unchanged");
        r.osdCacheTarget->BeginDraw();
        r.osdCacheTarget->Clear(D2D1::ColorF(0, 0.0f));
        r.osdCacheTarget->FillRectangle(D2D1::RectF(0, 0, 700, 440), r.osdCacheBackgroundBrush);
        Check(r.osdCacheTarget->EndDraw(), "actual dark HDR panel");
        for (double sceneNits : {100.0, 1000.0}) {
            ClearVideo(r, sceneNits); DrawUi(r);
            const auto panel = Read(r);
            for (unsigned channel : panel)
                Require(Nits(channel/1023.0) > sceneNits*0.08 &&
                        Nits(channel/1023.0) < sceneNits*0.11 + 3.0,
                        "HDR panel retains about 10% scene light");
            Near(Read(r, 799, 499)[0], Pq(sceneNits)*1023, 2, "panel leaves outside video unchanged");
        }
        Benchmark(r); RequireCleanGpu(r);
        r.reset(); Require(!r.hdrOverlayBackground && !r.hdrOverlayConstants && !g_hdrOutputActive.load(), "HDR reset");
        Check(r.initialize(hwnd, 64, 64, 30, VideoPixelFormat::Nv12), "SDR reinitialize");
        Require(!r.hdrOutput && !r.hdrOverlayConstants, "SDR no HDR resources");
        Require(std::abs(r.osdCacheBackgroundBrush->GetColor().a - 0.90f) < 0.0001f &&
                std::abs(r.volumeCacheBackgroundBrush->GetColor().a - 0.90f) < 0.0001f &&
                std::abs(r.audioCacheBackgroundBrush->GetColor().a - 0.92f) < 0.0001f,
                "SDR panel opacity unchanged");
        ClearUi(r, 1, 1, 1, 1); DrawUi(r); Require(Read(r)[0] == 255, "SDR white unchanged");
        ClearUi(r, 0, 0, 0, 0.5f); Benchmark(r); RequireCleanGpu(r);
        Check(r.initialize(hwnd, 64, 64, 30, VideoPixelFormat::P010, true, {},
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020), "left-chroma HDR");
        DXGI_COLOR_SPACE_TYPE input{};
        r.videoContext1->VideoProcessorGetStreamColorSpace1(r.processor, 0, &input);
        Require(input == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020, "left metadata reaches actual VP");
        RequireCleanGpu(r);
    }
    DestroyWindow(hwnd); CoUninitialize(); std::puts("HDR pixel/overlay/transition tests passed");
}
