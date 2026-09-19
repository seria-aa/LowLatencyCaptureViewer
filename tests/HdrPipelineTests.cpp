// Actual app renderer + synthetic P010 + GPU readback. No capture/audio device,
// HDR display, display-mode changes, settings writes or visible test window.
#define LLCV_GPU_DIAGNOSTICS
#ifndef LLCV_HDR_FRAME_AUDIT
#define LLCV_HDR_FRAME_AUDIT
#endif
#ifndef LLCV_HDR_SCRGB_PROTOTYPE
#define LLCV_HDR_SCRGB_PROTOTYPE
#endif
#include "../src/main.cpp"
#undef fwprintf
#include "../src/audio/AsioOutput.cpp"
#include <d3d11sdklayers.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <DirectXPackedVector.h>

static void Require(bool ok, const char* name) {
    if (!ok) { std::printf("FAIL: %s\n", name); std::exit(1); }
}
static void Check(HRESULT hr, const char* name) {
    if (FAILED(hr)) { std::printf("FAIL %s: %08lX\n", name, static_cast<unsigned long>(hr)); std::exit(1); }
}

static void VerifyFrameAudit(DirectD3D11Renderer& r, const std::vector<unsigned short>& p) {
    namespace fs = std::filesystem;
    GUID id{}; Check(CoCreateGuid(&id), "audit test GUID");
    wchar_t suffix[40]{}, temp[MAX_PATH]{};
    Require(StringFromGUID2(id, suffix, 40) != 0 && GetTempPathW(MAX_PATH, temp) != 0, "audit temp path");
    const fs::path root = fs::path(temp) / (std::wstring(L"llcv-hdr-audit-test-") + suffix);
    Require(CreateDirectoryW(root.c_str(), nullptr) != 0, "new private test directory");
    const auto pair = root / L"pair";
    Check(llcv::hdr_audit::SavePair(r.context, r.nv12Textures[r.activeUploadSurface],
        r.backBuffer, pair.wstring(), {}), "save matching raw/GPU pair");
    auto bytes = [](const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::vector<unsigned char>(std::istreambuf_iterator<char>(file), {});
    };
    const auto raw = bytes(pair / L"input.p010");
    Require(raw.size() == p.size()*sizeof(unsigned short) &&
        memcmp(raw.data(), p.data(), raw.size()) == 0, "saved P010 equals uploaded frame byte-for-byte");
    llcv::hdr_audit::Image output;
    Check(llcv::hdr_audit::Read(r.context, r.backBuffer, output), "reference output readback");
    Require(bytes(pair / L"output.raw") == output.bytes, "saved output equals same GPU frame");
    const auto manifest = bytes(pair / L"metadata.json");
    const std::string json(manifest.begin(), manifest.end());
    Require(json.find("\"complete\": true") != std::string::npos, "completion marker written last");
    Require(json.find("\"hdr_chroma_selection\": 0") != std::string::npos,
        "audit records requested chroma interpretation separately from reported metadata");
    Require(FAILED(llcv::hdr_audit::SavePair(r.context, r.nv12Textures[r.activeUploadSurface],
        r.backBuffer, pair.wstring(), {})), "existing pair cannot be overwritten");
    Require(bytes(pair / L"input.p010") == raw, "refusal preserves existing data");
    Require(FAILED(llcv::hdr_audit::SavePair(r.context, r.nv12Textures[r.activeUploadSurface],
        r.backBuffer, (root / L"missing-parent" / L"pair").wstring(), {})), "missing destination handled");
    auto count = [&] { return std::distance(fs::directory_iterator(root), fs::directory_iterator()); };
    g_hdrFrameAuditDirectory = root.wstring();
    g_hdrFrameAuditRequested = false;
    r.saveRequestedFrameAudit(); Require(count() == 1, "no files without explicit request");
    g_hdrFrameAuditRequested = true;
    r.saveRequestedFrameAudit(); Require(count() == 2 && !g_hdrFrameAuditRequested, "one request one pair");
    r.saveRequestedFrameAudit(); Require(count() == 2, "no repeated capture after request consumed");
    // Only remove the three files created in this unique test-owned directory.
    for (const auto& entry : fs::directory_iterator(root)) {
        for (const wchar_t* name : {L"input.p010", L"output.raw", L"metadata.json"})
            Require(DeleteFileW((entry.path() / name).c_str()) != 0, "remove test artifact");
        Require(RemoveDirectoryW(entry.path().c_str()) != 0, "remove empty pair directory");
    }
    Require(RemoveDirectoryW(root.c_str()) != 0, "remove empty test directory");
    g_hdrFrameAuditDirectory.clear();
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
static std::array<double, 3> ReadFloat(DirectD3D11Renderer& r, UINT x=400, UINT y=250) {
    D3D11_TEXTURE2D_DESC d{}; r.backBuffer->GetDesc(&d);
    Require(d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT,"FP16 scRGB output");
    d.Width=d.Height=1; d.Usage=D3D11_USAGE_STAGING;
    d.BindFlags=d.MiscFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* copy=nullptr; Check(r.device->CreateTexture2D(&d,nullptr,&copy),"FP16 readback texture");
    r.context->OMSetRenderTargets(0,nullptr,nullptr);
    D3D11_BOX box{x,y,0,x+1,y+1,1};
    r.context->CopySubresourceRegion(copy,0,0,0,0,r.backBuffer,0,&box);
    D3D11_MAPPED_SUBRESOURCE map{}; Check(r.context->Map(copy,0,D3D11_MAP_READ,0,&map),"FP16 map");
    const auto* half=static_cast<const unsigned short*>(map.pData);
    std::array<double,3> value{};
    for(int i=0;i<3;++i) value[i]=DirectX::PackedVector::XMConvertHalfToFloat(half[i]);
    r.context->Unmap(copy,0); copy->Release(); return value;
}
static unsigned ReadOsdCachePixel(DirectD3D11Renderer& r, UINT x, UINT y) {
    D3D11_TEXTURE2D_DESC d{}; r.osdOverlayTexture->GetDesc(&d);
    Require(d.Format == DXGI_FORMAT_B8G8R8A8_UNORM, "BGRA8 OSD cache");
    d.Width = d.Height = 1; d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = d.MiscFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* copy = nullptr;
    Check(r.device->CreateTexture2D(&d, nullptr, &copy), "OSD cache readback texture");
    const D3D11_BOX box{x, y, 0, x+1, y+1, 1};
    r.context->CopySubresourceRegion(copy, 0, 0, 0, 0, r.osdOverlayTexture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE map{};
    Check(r.context->Map(copy, 0, D3D11_MAP_READ, 0, &map), "OSD cache readback");
    const unsigned pixel = *static_cast<const unsigned*>(map.pData);
    r.context->Unmap(copy, 0); copy->Release();
    return pixel;
}
static std::array<double,3> ScRgbReference(std::array<double,3> pq) {
    const double r=Nits(std::clamp(pq[0],0.0,1.0));
    const double g=Nits(std::clamp(pq[1],0.0,1.0));
    const double b=Nits(std::clamp(pq[2],0.0,1.0));
    return {(1.660491*r-.587641*g-.072850*b)/80,
        (-.124550*r+1.132900*g-.008350*b)/80,
        (-.018151*r-.100579*g+1.118730*b)/80};
}
static void NearFloat(std::array<double,3> actual, std::array<double,3> expected, const char* name) {
    for(size_t i=0;i<3;++i) {
        const double tolerance=.0025+.0015*std::abs(expected[i]);
        if(!std::isfinite(actual[i]) || std::abs(actual[i]-expected[i])>tolerance) {
            std::printf("%s channel=%zu actual=%.8f expected=%.8f\n",name,i,actual[i],expected[i]);
            Require(false,name);
        }
    }
}
static double BenchmarkVideo(DirectD3D11Renderer& r) {
    ID3D11Query *disjoint=nullptr,*start=nullptr,*end=nullptr;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
    Check(r.device->CreateQuery(&desc,&disjoint),"video benchmark disjoint");
    desc.Query=D3D11_QUERY_TIMESTAMP;
    Check(r.device->CreateQuery(&desc,&start),"video benchmark start");
    Check(r.device->CreateQuery(&desc,&end),"video benchmark end");
    auto draw=[&] {
        if(r.scrgbOutput) Check(r.scrgbPipeline.Draw(r.context,r.backBufferRenderTarget,
            r.outputWidth,r.outputHeight,r.activeUploadSurface),"benchmark direct shader");
        else {
            D3D11_VIDEO_PROCESSOR_STREAM s{}; s.Enable=TRUE; s.pInputSurface=r.inputViews[r.activeUploadSurface];
            Check(r.videoContext->VideoProcessorBlt(r.processor,r.outputView,0,1,&s),"benchmark VP");
        }
    };
    for(int i=0;i<20;++i) draw();
    r.context->Begin(disjoint); r.context->End(start);
    for(int i=0;i<100;++i) draw();
    r.context->End(end); r.context->End(disjoint); r.context->Flush();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
    const auto deadline=GetTickCount64()+5000;
    HRESULT hr;
    while((hr=r.context->GetData(disjoint,&data,sizeof(data),0))==S_FALSE && GetTickCount64()<deadline) Sleep(1);
    Require(hr==S_OK && !data.Disjoint && data.Frequency,"video benchmark completes");
    UINT64 a=0,b=0;
    Require(r.context->GetData(start,&a,sizeof(a),0)==S_OK &&
            r.context->GetData(end,&b,sizeof(b),0)==S_OK,"video benchmark timestamps");
    end->Release(); start->Release(); disjoint->Release();
    return double(b-a)/double(data.Frequency)*1e6/100;
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
static int TestRawSdr() {
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "SDR COM");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    constexpr UINT width = 3840, height = 2160;
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"Hidden SDR fidelity test", WS_POPUP,
        0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(hwnd != nullptr, "SDR hidden window");
    g_settings.pixelPerfect = false;
    g_settings.scalingMode = ScalingMode::Sharp; // must not sharpen 1:1
    g_settings.presentationMode = PresentationMode::VSync;
    g_osdVisible = false; g_audioOsdVisible = false; g_volumeHudUntilMs = 0;
    {
        DirectD3D11Renderer r;
        for (auto mode : {PresentationMode::AllowTearing, PresentationMode::VSync,
                          PresentationMode::Compatibility})
        for (auto format : {VideoPixelFormat::Nv12, VideoPixelFormat::Yuy2})
        for (unsigned matrix : {1u, 2u}) for (unsigned range : {1u, 2u}) {
            g_settings.presentationMode = mode;
            // Exercise the device metadata parser and connected-type override,
            // rather than handing the renderer a pre-decoded color tuple.
            VIDEOINFOHEADER2 info{};
            info.dwControlFlags = AMCONTROL_COLORINFO_PRESENT |
                (matrix << DXVA_VideoTransferMatrixShift) |
                (range << DXVA_NominalRangeShift);
            AM_MEDIA_TYPE mt{};
            mt.formattype = FORMAT_VideoInfo2;
            mt.pbFormat = reinterpret_cast<BYTE*>(&info); mt.cbFormat = sizeof(info);
            DirectShowColorMetadata connected{};
            Require(ExtractVideoColorMetadata(&mt, connected), "raw connected metadata parsed");
            DirectShowColorMetadata selected{};
            selected.present = true; selected.transferMatrix = 3 - matrix;
            selected.nominalRange = 3 - range;
            MergeVideoColorMetadata(selected, connected);
            const auto color = llcv::video_color::Resolve(false, width, height, {},
                {selected.transferMatrix, selected.nominalRange});
            Check(r.initialize(hwnd, width, height, 60, format, false, color), "4K SDR initialize");
            Require(r.outputWidth == width && r.outputHeight == height && !r.sharpScalingActive,
                "4K 1:1 output has no sharp enhancement even when Sharp selected");
            BOOL automatic = TRUE;
            r.videoContext->VideoProcessorGetStreamAutoProcessingMode(r.processor, 0, &automatic);
            Require(!automatic, "SDR driver auto processing is explicitly disabled");
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{};
            r.videoContext->VideoProcessorGetOutputColorSpace(r.processor, &outputColor);
            Require(outputColor.RGB_Range == 0, "SDR output RGB is full range");
            const UINT pitch = width * (format == VideoPixelFormat::Nv12 ? 1 : 2) + 64;
            const UINT rows = format == VideoPixelFormat::Nv12 ? height * 3 / 2 : height;
            std::vector<BYTE> pixels(static_cast<size_t>(pitch) * rows, 0xee);
            auto fill = [&](bool checker) {
                for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
                    const BYTE luma = checker ? ((x + y) % 2 ? 192 : 64) : 100;
                    if (format == VideoPixelFormat::Nv12) pixels[y * pitch + x] = luma;
                    else {
                        pixels[y * pitch + x * 2] = luma;
                        pixels[y * pitch + x * 2 + 1] = checker ? 128 : (x % 2 ? 180 : 160);
                    }
                }
                if (format == VideoPixelFormat::Nv12)
                    for (UINT y = height; y < rows; ++y) for (UINT x = 0; x < width; ++x)
                        pixels[y * pitch + x] = checker ? 128 : (x % 2 ? 180 : 160);
                r.upload(pixels.data(), pitch);
                D3D11_VIDEO_PROCESSOR_STREAM stream{};
                stream.Enable = TRUE; stream.pInputSurface = r.inputViews[r.activeUploadSurface];
                Check(r.videoContext->VideoProcessorBlt(r.processor, r.outputView, 0, 1, &stream), "SDR blit");
            };
            // Alternate individual luma pixels; padding must not enter the image.
            fill(true);
            for (UINT y : {0u, 1u, 1079u, 2159u}) for (UINT x : {0u, 1u, 1919u, 3839u}) {
                const double luma = (x + y) % 2 ? 192 : 64;
                const double expected = range == 1 ? luma : (luma - 16) * 255 / 219;
                for (unsigned c : Read(r, x, y)) Near(c, expected, 3, "4K single-pixel luma preserved");
            }
            // Independent YCbCr reference; compare flat color away from boundaries.
            fill(false);
            const double yy = range == 1 ? 100.0 : (100.0 - 16) * 255 / 219;
            const double uu = (160.0 - 128) * (range == 1 ? 1.0 : 255.0 / 224);
            const double vv = (180.0 - 128) * (range == 1 ? 1.0 : 255.0 / 224);
            const double kr = matrix == 1 ? 0.2126 : 0.299;
            const double kb = matrix == 1 ? 0.0722 : 0.114;
            const double rr = yy + 2 * (1 - kr) * vv;
            const double bb = yy + 2 * (1 - kb) * uu;
            const double gg = (yy - kr * rr - kb * bb) / (1 - kr - kb);
            const auto actual = Read(r, 100, 100);
            const double expected[]{rr, gg, bb};
            for (size_t c = 0; c < 3; ++c)
                Near(actual[c], std::clamp(expected[c], 0.0, 255.0), 3, "SDR matrix/range RGB reference");
            std::printf("4K SDR mode=%u format=%u matrix=%u range=%u: pixel and color reference passed\n",
                static_cast<unsigned>(mode), static_cast<unsigned>(format), matrix, range);
            RequireCleanGpu(r);
        }
        // A real scale still honors Sharp if the GPU exposes that filter.
        Check(r.initialize(hwnd, 1920, 1080, 60, VideoPixelFormat::Nv12), "scaled SDR initialize");
        D3D11_VIDEO_PROCESSOR_FILTER_RANGE filter{};
        const bool supported = SUCCEEDED(r.enumerator->GetVideoProcessorFilterRange(
            D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, &filter)) && filter.Maximum > filter.Minimum;
        Require(r.sharpScalingActive == supported, "actual upscale keeps supported Sharp filter");
        Check(r.initialize(hwnd, width, height, 60, VideoPixelFormat::Nv12), "return to 1:1");
        Require(!r.sharpScalingActive, "sharp state cleared on return to 1:1");
        if (supported) {
            BOOL enabled = TRUE; int level = 0;
            r.videoContext->VideoProcessorGetStreamFilter(r.processor, 0,
                D3D11_VIDEO_PROCESSOR_FILTER_EDGE_ENHANCEMENT, &enabled, &level);
            Require(!enabled, "GPU sharp filter not retained after reinitialize");
        }
        g_settings.scalingMode = ScalingMode::Smooth;
        Check(r.initialize(hwnd, 1920, 1080, 60, VideoPixelFormat::Nv12), "smooth upscale");
        Require(!r.sharpScalingActive, "smooth upscale does not enable Sharp");
        RequireCleanGpu(r);
    }
    DestroyWindow(hwnd); CoUninitialize();
    std::puts("SDR 4K fidelity tests passed; no capture device or visible window used.");
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--sdr-only") return TestRawSdr();
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
        blit(); VerifyFrameAudit(r, p); RequireCleanGpu(r);
        llcv::video::CaptureColorMetadata reportedChroma6{};
        reportedChroma6.present = true;
        reportedChroma6.chromaSubsampling = 6;
        reportedChroma6.nominalRange = 2;
        reportedChroma6.primaries = 2;
        reportedChroma6.transferMatrix = 1;
        for (auto placement : {llcv::hdr::ChromaLocation::TopLeft, llcv::hdr::ChromaLocation::Left}) {
            const auto resolved = llcv::hdr::ResolveInput(reportedChroma6, true, placement);
            Require(resolved.kind == llcv::hdr::InputKind::Hdr10 && resolved.chromaOverridden,
                "explicit chroma selection resolves reported tuple");
            Check(r.initialize(hwnd,64,64,30,VideoPixelFormat::P010,true,{},resolved.colorSpace),
                "manual placement enters native HDR10 conversion");
            DXGI_COLOR_SPACE_TYPE actual{};
            r.videoContext1->VideoProcessorGetStreamColorSpace1(r.processor,0,&actual);
            Require(actual == resolved.colorSpace, "chosen placement reaches GPU stream state");
            blit();
            for (unsigned channel : Read(r,400,250))
                Near(channel,(512.-64)/876.*1023,3,"placement override preserves neutral PQ luminance");
            RequireCleanGpu(r);
        }
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
        // Audit high-saturation colors as well as neutral ramps. Generate valid
        // BT.2020 NCL P010 from known nonlinear RGB, then compare GPU output to
        // an independent inverse of the quantized input. No tone mapping here.
        DXGI_COLOR_SPACE_TYPE outputSpace{};
        r.videoContext1->VideoProcessorGetOutputColorSpace1(r.processor, &outputSpace);
        Require(outputSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
                "video processor output remains full-range BT2020 PQ");
        auto encodePatch = [&](const std::array<double, 3>& rgb) {
            const double y = 0.2627*rgb[0] + 0.6780*rgb[1] + 0.0593*rgb[2];
            const int yc = static_cast<int>(std::lround(64 + 876*y));
            const int uc = static_cast<int>(std::lround(512 + 896*(rgb[2]-y)/1.8814));
            const int vc = static_cast<int>(std::lround(512 + 896*(rgb[0]-y)/1.4746));
            std::fill(p.begin(), p.begin()+4096, static_cast<unsigned short>(yc << 6));
            for (size_t i = 4096; i < p.size(); i += 2) {
                p[i] = static_cast<unsigned short>(uc << 6);
                p[i+1] = static_cast<unsigned short>(vc << 6);
            }
            const double luma = (yc-64.0)/876, cb = (uc-512.0)/896, cr = (vc-512.0)/896;
            const double red = luma + 1.4746*cr, blue = luma + 1.8814*cb;
            return std::array<double, 3>{red, (luma-0.2627*red-0.0593*blue)/0.6780, blue};
        };
        unsigned patchCount = 0;
        double maxCodeError = 0;
        for (double red : {0.0, 0.02, 0.10, 0.25, 0.50, 0.58, 0.75, 0.90, 1.0})
            for (double green : {0.0, 0.02, 0.10, 0.25, 0.50, 0.58, 0.75, 0.90, 1.0})
                for (double blue : {0.0, 0.02, 0.10, 0.25, 0.50, 0.58, 0.75, 0.90, 1.0}) {
                    const auto expected = encodePatch({red, green, blue});
                    blit();
                    const auto actual = Read(r, 400, 250);
                    for (size_t c = 0; c < 3; ++c) {
                        const double code = std::clamp(expected[c], 0.0, 1.0)*1023;
                        maxCodeError = (std::max)(maxCodeError, std::abs(actual[c]-code));
                        Near(actual[c], code, 4, "HDR saturated RGB cube");
                    }
                    ++patchCount;
                }
        std::printf("HDR color cube: %u patches, max error %.3f/1023 codes\n",
                    patchCount, maxCodeError);
        for (double nits : {0.1, 1.0, 10.0, 80.0, 100.0, 203.0, 280.0, 1000.0, 4000.0, 10000.0}) {
            const auto expected = encodePatch({Pq(nits), Pq(nits), Pq(nits)});
            blit();
            const auto beforeUiWhite = Read(r, 400, 250);
            for (unsigned c : beforeUiWhite)
                Near(c, expected[0]*1023, 3, "HDR absolute luminance encoding");
            for (float white : {80.0f, 280.0f, 1000.0f}) {
                g_hdrUiWhiteNits = white;
                Check(r.drawOverlayQuads(), "hidden UI with different Windows white");
                Require(Read(r, 400, 250) == beforeUiWhite, "UI white cannot alter video");
            }
            std::printf("Known HDR white %.1f nit -> RGB10 %u (decoded %.2f nit)\n",
                        nits, beforeUiWhite[0], Nits(beforeUiWhite[0]/1023.0));
        }
        // Real capture rows can have padding. Poison it and exercise every ring
        // surface with a two-color image to catch stride/UV-plane offset errors.
        const UINT paddedStride = 160;
        std::vector<unsigned short> padded(paddedStride/2*96, 0xffff);
        const auto top = encodePatch({Pq(203), Pq(40), Pq(10)});
        const auto firstPatch = p;
        const auto bottom = encodePatch({Pq(10), Pq(40), Pq(203)});
        for (size_t y = 0; y < 64; ++y)
            std::copy_n((y < 32 ? firstPatch : p).data()+y*64, 64,
                        padded.data()+y*(paddedStride/2));
        for (size_t y = 0; y < 32; ++y)
            std::copy_n((y < 16 ? firstPatch : p).data()+4096+y*64, 64,
                        padded.data()+(64+y)*(paddedStride/2));
        for (unsigned frame = 0; frame < 9; ++frame) {
            r.upload(reinterpret_cast<const BYTE*>(padded.data()), paddedStride);
            stream.pInputSurface = r.inputViews[r.activeUploadSurface];
            Check(r.videoContext->VideoProcessorBlt(r.processor, r.outputView, 0, 1, &stream), "padded HDR upload");
            const auto a = Read(r, 400, 100), b = Read(r, 400, 400);
            for (size_t c = 0; c < 3; ++c) {
                Near(a[c], std::clamp(top[c], 0.0, 1.0)*1023, 4, "padded HDR top");
                Near(b[c], std::clamp(bottom[c], 0.0, 1.0)*1023, 4, "padded HDR bottom");
            }
        }
        std::puts("Padded P010 two-color upload: 9 frames / 3 ring surfaces passed");
        std::vector<unsigned short> packed(64*96);
        for (size_t row = 0; row < 96; ++row)
            std::copy_n(padded.data()+row*(paddedStride/2), 64, packed.data()+row*64);
        VerifyFrameAudit(r, packed); RequireCleanGpu(r);
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
        const auto hdrPanelColor = r.osdCacheBackgroundBrush->GetColor();
        Require(hdrPanelColor.r == 0 && hdrPanelColor.g == 0 && hdrPanelColor.b == 0,
            "HDR Tab panel uses neutral black, not tinted UI light");
        Require(r.osdCacheTextBrush->GetColor().a == 1.0f, "HDR text opacity unchanged");
        r.osdCacheTarget->BeginDraw();
        r.osdCacheTarget->Clear(D2D1::ColorF(0, 0.0f));
        r.osdCacheTarget->FillRectangle(D2D1::RectF(0, 0, 700, 440), r.osdCacheBackgroundBrush);
        Check(r.osdCacheTarget->EndDraw(), "actual dark HDR panel");
        for (double sceneNits : {0.0, 0.1, 10.0, 100.0, 1000.0, 4000.0, 10000.0}) {
            ClearVideo(r, sceneNits); DrawUi(r);
            const auto panel = Read(r);
            const double storedScene = Nits(std::round(Pq(sceneNits)*1023)/1023.0);
            const double storedAlpha = (ReadOsdCachePixel(r, 8, 100) >> 24)/255.0;
            for (unsigned channel : panel)
                Near(channel, Pq(storedScene*(1-storedAlpha))*1023, 2,
                    "HDR black panel retains quantized 10% scene light");
            Near(Read(r, 799, 499)[0], Pq(sceneNits)*1023, 2, "panel leaves outside video unchanged");
        }
        // Exercise the real Tab path, including D2D rounded panel/text cache,
        // cache refresh and hide/show. Always start from a fresh video frame:
        // reopening Tab must not repeatedly darken the previous UI frame.
        g_osdVisible = true;
        for (unsigned repeat = 0; repeat < 12; ++repeat) {
            g_hdrUiWhiteNits = repeat % 2 ? 80.0f : 1000.0f;
            if (repeat % 3 == 0) g_overlayGeneration.fetch_add(1);
            ClearVideo(r, 1000);
            Check(r.drawOverlayQuads(), "production HDR Tab overlay");
            const unsigned cache = ReadOsdCachePixel(r, 8, 100); // left padding, no text
            const unsigned alpha = cache >> 24;
            Require((cache & 0x00ffffffu) == 0 && (alpha == 229 || alpha == 230),
                "actual D2D Tab cache is black with 90% opacity within BGRA8 precision");
            const double background = Nits(std::round(Pq(1000)*1023)/1023.0);
            const auto panel = Read(r, 24, 116); // production panel starts at (16,16)
            for (unsigned channel : panel)
                Near(channel, Pq(background*(1-alpha/255.0))*1023, 2,
                    "Tab opacity independent of UI white/cache refresh");
            Require(panel[0] == panel[1] && panel[1] == panel[2], "neutral scene has no panel color cast");
            Near(Read(r, 799, 499)[0], Pq(1000)*1023, 2, "real Tab does not alter outside video");
            g_osdVisible = false;
            ClearVideo(r, 1000); const auto untouched = Read(r, 24, 116);
            Check(r.drawOverlayQuads(), "hidden Tab has no blend");
            Require(Read(r, 24, 116) == untouched, "hiding Tab restores unmodified video");
            g_osdVisible = true;
        }
        g_osdVisible = false; g_hdrUiWhiteNits = 203.0f;
        Benchmark(r); RequireCleanGpu(r);
        r.reset(); Require(!r.hdrOverlayBackground && !r.hdrOverlayConstants && !g_hdrOutputActive.load(), "HDR reset");
        Check(r.initialize(hwnd, 64, 64, 30, VideoPixelFormat::Nv12), "SDR reinitialize");
        Require(!r.hdrOutput && !r.hdrOverlayConstants, "SDR no HDR resources");
        Require(std::abs(r.osdCacheBackgroundBrush->GetColor().a - 0.90f) < 0.0001f &&
                std::abs(r.volumeCacheBackgroundBrush->GetColor().a - 0.90f) < 0.0001f &&
                std::abs(r.audioCacheBackgroundBrush->GetColor().a - 0.92f) < 0.0001f,
                "SDR panel opacity unchanged");
        const auto sdrPanelColor = r.osdCacheBackgroundBrush->GetColor();
        Require(sdrPanelColor.r == 0.055f && sdrPanelColor.g == 0.063f && sdrPanelColor.b == 0.078f,
            "HDR-to-SDR reinitialization restores original SDR panel tint");
        ClearUi(r, 1, 1, 1, 1); DrawUi(r); Require(Read(r)[0] == 255, "SDR white unchanged");
        ClearUi(r, 0, 0, 0, 0.5f); Benchmark(r); RequireCleanGpu(r);
        // Force off does not tone-map unknown P010. Feed a known PQ white to
        // the actual SDR path and record the encoded SDR result; a darker SDR
        // preview alone therefore cannot establish the source transfer function.
        Check(r.initialize(hwnd, 64, 64, 30, VideoPixelFormat::P010, false), "P010 SDR comparison");
        encodePatch({Pq(203), Pq(203), Pq(203)});
        blit();
        const auto sdrPqWhite = Read(r, 400, 250);
        VerifyFrameAudit(r, p);
        for (unsigned c : sdrPqWhite)
            Near(c, Pq(203)*255, 3, "unknown P010 SDR path does not apply HDR tone mapping");
        std::printf("Same known 203-nit PQ white with force off -> SDR RGB8 %u; not an HDR-to-SDR tone map\n",
                    sdrPqWhite[0]);
        RequireCleanGpu(r);
        Check(r.initialize(hwnd, 64, 64, 30, VideoPixelFormat::P010, true, {},
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020), "left-chroma HDR");
        DXGI_COLOR_SPACE_TYPE input{};
        r.videoContext1->VideoProcessorGetStreamColorSpace1(r.processor, 0, &input);
        Require(input == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020, "left metadata reaches actual VP");
        RequireCleanGpu(r);
        g_useScrgbPrototype=true;
        Check(r.initialize(hwnd,64,64,30,VideoPixelFormat::P010,true),"direct scRGB initialize");
        Require(r.scrgbOutput && r.processor==nullptr && r.outputView==nullptr,"direct path has no VP/intermediate");
        auto direct=[&] {
            r.upload(reinterpret_cast<const BYTE*>(p.data()),128);
            Check(r.scrgbPipeline.Draw(r.context,r.backBufferRenderTarget,r.outputWidth,r.outputHeight,
                                      r.activeUploadSurface),"direct scRGB draw");
        };
        for(double nits : {.0,.1,1.,10.,80.,203.,280.,1000.,4000.,10000.}) {
            const auto ref=encodePatch({Pq(nits),Pq(nits),Pq(nits)}); direct();
            NearFloat(ReadFloat(r),ScRgbReference(ref),"scRGB absolute luminance");
        }
        for(auto color : {std::array<double,3>{.75,.1,.1},{.1,.75,.1},{.1,.1,.75},
                          {.4,.7,.2},{.7,.4,.8},{.02,.9,.5}}) {
            const auto ref=encodePatch(color); direct();
            NearFloat(ReadFloat(r),ScRgbReference(ref),"scRGB wide gamut");
        }
        encodePatch({.75,.1,.1}); direct();
        Require(ReadFloat(r)[1]<0 && ReadFloat(r)[0]>1,"negative and HDR channels are not clipped");
        VerifyFrameAudit(r,p);
        const auto beforeUi=ReadFloat(r);
        ClearUi(r,0,0,0,128.0f/255); DrawUi(r);
        auto halfExpected=beforeUi;
        for(auto& c:halfExpected) c*=1-128.0/255;
        NearFloat(ReadFloat(r),halfExpected,"scRGB UI dark panel linear blend");
        g_hdrUiWhiteNits=280;
        ClearUi(r,1,1,1,1); DrawUi(r);
        NearFloat(ReadFloat(r),{3.5,3.5,3.5},"scRGB UI uses Windows white divided by 80");
        ClearUi(r,0,0,0,0); direct(); DrawUi(r);
        NearFloat(ReadFloat(r),beforeUi,"transparent scRGB UI preserves video");
        RequireCleanGpu(r);
        g_osdVisible=true;
        Check(r.presentUploaded(),"production scRGB render/overlay/Present path");
        g_osdVisible=false;
        RequireCleanGpu(r);
        Require(SetWindowPos(hwnd,nullptr,0,0,64,64,SWP_NOZORDER|SWP_NOACTIVATE)!=0,"chroma test 1:1 size");
        std::fill(p.begin(),p.begin()+4096,static_cast<unsigned short>(512<<6));
        for(unsigned cy=0;cy<32;++cy) for(unsigned cx=0;cx<32;++cx) {
            p[4096+(cy*32+cx)*2]=static_cast<unsigned short>((480+2*cx)<<6);
            p[4097+(cy*32+cx)*2]=static_cast<unsigned short>((400+12*cy)<<6);
        }
        for(bool topLeft : {true,false}) {
            llcv::video::CaptureColorMetadata reported{};
            reported.present=true; reported.chromaSubsampling=6; reported.nominalRange=2;
            const auto resolved=llcv::hdr::ResolveInput(reported,true,topLeft ?
                llcv::hdr::ChromaLocation::TopLeft : llcv::hdr::ChromaLocation::Left);
            Check(r.initialize(hwnd,64,64,30,VideoPixelFormat::P010,true,{},resolved.colorSpace),
                "scRGB chroma override initialize");
            direct();
            const double y=(512.-64)/876,cb=(500.-512)/896,cr=((topLeft?520.:517.)-512)/896;
            const double red=y+1.4746*cr,blue=y+1.8814*cb,green=(y-.2627*red-.0593*blue)/.678;
            NearFloat(ReadFloat(r,20,20),ScRgbReference({red,green,blue}),"left/top-left chroma interpolation");
            RequireCleanGpu(r);
        }
        // Measure GPU video work only, excluding capture/upload, Present and readback.
        Require(SetWindowPos(hwnd,nullptr,0,0,2560,1440,SWP_NOZORDER|SWP_NOACTIVATE)!=0,"benchmark window size");
        std::vector<unsigned short> frame(2560*1440*3/2,512<<6);
        for(bool directPath : {false,true,false,true}) {
            g_useScrgbPrototype=directPath;
            Check(r.initialize(hwnd,2560,1440,60,VideoPixelFormat::P010,true),"benchmark path initialize");
            r.upload(reinterpret_cast<const BYTE*>(frame.data()),5120);
            const double timeUs=BenchmarkVideo(r);
            if(!directPath && timeUs<.01)
                std::puts("Video GPU VP HDR10: timestamp result unusable; NOT zero-cost and not comparable");
            else std::printf("Video GPU 2560x1440 %s: %.2f us/frame (100 draws, no Present)\n",
                             directPath?"direct scRGB":"VP HDR10",timeUs);
            RequireCleanGpu(r);
        }
        g_useScrgbPrototype=false;
        Check(r.initialize(hwnd,64,64,30,VideoPixelFormat::Nv12),"scRGB -> SDR transition");
        Require(!r.scrgbOutput && !r.scrgbPipeline.ps,"scRGB resources released on SDR transition");
    }
    DestroyWindow(hwnd); CoUninitialize(); std::puts("HDR pixel/overlay/transition tests passed");
}
