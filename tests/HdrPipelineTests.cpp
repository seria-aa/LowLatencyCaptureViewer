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
        for (unsigned matrix : {1u, 2u}) for (unsigned reportedRange : {1u, 2u})
        for (auto rangeOverride : {llcv::video_color::RangeOverride::Auto,
                                  llcv::video_color::RangeOverride::Limited,
                                  llcv::video_color::RangeOverride::Full}) {
            g_settings.presentationMode = mode;
            // Exercise the device metadata parser and connected-type override,
            // rather than handing the renderer a pre-decoded color tuple.
            VIDEOINFOHEADER2 info{};
            info.dwControlFlags = AMCONTROL_COLORINFO_PRESENT |
                (matrix << DXVA_VideoTransferMatrixShift) |
                (reportedRange << DXVA_NominalRangeShift);
            AM_MEDIA_TYPE mt{};
            mt.formattype = FORMAT_VideoInfo2;
            mt.pbFormat = reinterpret_cast<BYTE*>(&info); mt.cbFormat = sizeof(info);
            DirectShowColorMetadata connected{};
            Require(ExtractVideoColorMetadata(&mt, connected), "raw connected metadata parsed");
            DirectShowColorMetadata selected{};
            selected.present = true; selected.transferMatrix = 3 - matrix;
            selected.nominalRange = 3 - reportedRange;
            MergeVideoColorMetadata(selected, connected);
            g_settings.sdrRangeOverride = rangeOverride;
            g_settings.mjpegMatrixOverride = matrix == 1
                ? llcv::video_color::MatrixOverride::Bt601 : llcv::video_color::MatrixOverride::Bt709;
            const auto color = ResolveRawSdrVideoColor(format, width, height, selected);
            const unsigned range = rangeOverride == llcv::video_color::RangeOverride::Auto
                ? reportedRange : rangeOverride == llcv::video_color::RangeOverride::Full ? 1u : 2u;
            Require(color.rangeSource == (rangeOverride == llcv::video_color::RangeOverride::Auto
                    ? llcv::video_color::Source::DirectShow : llcv::video_color::Source::UserOverride),
                "Auto preserves metadata provenance; explicit SDR range overrides metadata");
            Require(color.matrixSource == llcv::video_color::Source::DirectShow,
                "SDR range override does not replace the device matrix");
            Check(r.initialize(hwnd, width, height, 60, format, false, color), "4K SDR initialize");
            Require(r.outputWidth == width && r.outputHeight == height && !r.sharpScalingActive,
                "4K 1:1 output has no sharp enhancement even when Sharp selected");
            BOOL automatic = TRUE;
            r.videoContext->VideoProcessorGetStreamAutoProcessingMode(r.processor, 0, &automatic);
            Require(!automatic, "SDR driver auto processing is explicitly disabled");
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{};
            r.videoContext->VideoProcessorGetOutputColorSpace(r.processor, &outputColor);
            Require(outputColor.RGB_Range == 0, "SDR output RGB is full range");
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};
            r.videoContext->VideoProcessorGetStreamColorSpace(r.processor, 0, &inputColor);
            Require(inputColor.Nominal_Range == static_cast<unsigned>(range == 1
                    ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                    : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235),
                "chosen SDR range reaches the actual GPU video processor");
            const UINT pitch = width * (format == VideoPixelFormat::Nv12 ? 1 : 2) + 64;
            const UINT rows = format == VideoPixelFormat::Nv12 ? height * 3 / 2 : height;
            std::vector<BYTE> pixels(static_cast<size_t>(pitch) * rows, 0xee);
            auto fill = [&](bool checker) {
                for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
                    constexpr BYTE endpoints[]{0, 16, 235, 255};
                    const bool endpoint = checker && y >= 100 && y < 104 && x >= 120 && x < 136;
                    const BYTE luma = endpoint ? endpoints[(x - 120) / 4]
                        : checker ? ((x + y) % 2 ? 192 : 64) : 100;
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
            const llcv::screenshot::Description shot{width, height,
                format == VideoPixelFormat::Nv12 ? llcv::screenshot::Format::Nv12
                    : llcv::screenshot::Format::Yuy2, color};
            std::vector<BYTE> packed(llcv::screenshot::PackedSize(shot));
            std::vector<BYTE> screenshotRow(width * 4);
            Require(llcv::screenshot::CopyFrame(shot, pixels.data(), pixels.size(), pitch, packed) &&
                    llcv::screenshot::ConvertRows(shot, packed, 102, 1, screenshotRow),
                "screenshot converts SDR endpoint samples");
            for (unsigned i = 0; i < 4; ++i) {
                constexpr double endpoints[]{0, 16, 235, 255};
                const double expected = std::clamp(range == 1 ? endpoints[i]
                    : (endpoints[i] - 16) * 255 / 219, 0.0, 255.0);
                const unsigned x = 122 + 4 * i;
                const auto endpoint = Read(r, x, 102);
                for (size_t c = 0; c < 3; ++c) {
                    Near(endpoint[c], expected, 3, "SDR black/white clipping follows chosen range");
                    Near(screenshotRow[x*4+2-c], endpoint[c], 3,
                        "screenshot and GPU black/white endpoints agree");
                }
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
#ifdef LLCV_RECORDING
            // UI is a child surface, never a modification of source samples,
            // renderer back buffer, or the encoder's raw video input.
            const auto cornerBefore=Read(r,width-40,height-24);
            llcv::recording::AttachRecordingIndicator(hwnd,hwnd,false,false);
            llcv::recording::Snapshot recordUi;recordUi.phase=llcv::recording::Phase::Recording;
            recordUi.busy=true;recordUi.elapsedMs=123000;
            llcv::recording::UpdateRecordingIndicator(hwnd,recordUi);
            UpdateWindow(hwnd);
            Require(Read(r,width-40,height-24)==cornerBefore,"recording UI does not modify video pixels");
#endif
            const double expected[]{rr, gg, bb};
            for (size_t c = 0; c < 3; ++c)
                Near(actual[c], std::clamp(expected[c], 0.0, 255.0), 3, "SDR matrix/range RGB reference");
            // Screenshot conversion must agree with the real renderer's
            // current source interpretation, not a hard-coded 709/limited path.
            Require(llcv::screenshot::CopyFrame(shot, pixels.data(), pixels.size(), pitch, packed),
                "screenshot copies padded live-renderer layout");
            Require(llcv::screenshot::ConvertRows(shot, packed, 100, 1, screenshotRow),
                "screenshot converts SDR row");
            for (size_t c = 0; c < 3; ++c)
                Near(screenshotRow[100*4+2-c], actual[c], 3, "screenshot and GPU matrix/range agree");
            std::printf("4K SDR mode=%u format=%u matrix=%u reported=%u override=%u applied=%u: "
                "pixel, color and screenshot reference passed\n",
                static_cast<unsigned>(mode), static_cast<unsigned>(format), matrix, reportedRange,
                static_cast<unsigned>(rangeOverride), range);
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

// RGB32/BGRX GPU fidelity: asymmetric per-channel pattern (not just
// luma), top-down vs bottom-up orientation (VideoProcessorSetStreamMirror),
// Full vs Limited RGB range, the garbage X byte never leaking into the
// output, and a resize + repeated draw. Skips cleanly (77) if the driver
// does not advertise RGB32 Video Processor input or mirror support, since
// this is a synthetic GPU/policy test independent of any real capture
// device's advertised formats (GC573 does not advertise RGB32 at all).
static int TestRawSdrRgb32(bool rgb24 = false) {
    const auto format = rgb24 ? VideoPixelFormat::Rgb24 : VideoPixelFormat::Rgb32;
    const UINT bytesPerPixel = rgb24 ? 3u : 4u;
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "RGB COM");
    constexpr UINT width = 255, height = 128;
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"Hidden RGB32 fidelity test", WS_POPUP,
        0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(hwnd != nullptr, "RGB hidden window");
    g_settings.pixelPerfect = false;
    g_settings.scalingMode = ScalingMode::Sharp;
    g_settings.presentationMode = PresentationMode::VSync;
    g_osdVisible = false; g_audioOsdVisible = false; g_volumeHudUntilMs = 0;
    bool anySkipped = false, anyRun = false;
    {
        DirectShowColorMetadata metadata{};
        metadata.present = true; metadata.nominalRange = 2;
        g_settings.sdrRangeOverride = llcv::video_color::RangeOverride::Auto;
        Require(ResolveRawSdrVideoColor(format, width, height, {}).range == llcv::video_color::Range::Full, "RGB default full");
        Require(ResolveRawSdrVideoColor(format, width, height, metadata).range == llcv::video_color::Range::Limited, "RGB metadata range");
        g_settings.sdrRangeOverride = llcv::video_color::RangeOverride::Full;
        Require(ResolveRawSdrVideoColor(format, width, height, metadata).range == llcv::video_color::Range::Full, "RGB override range");
        g_settings.sdrRangeOverride = llcv::video_color::RangeOverride::Auto;
        DirectD3D11Renderer reject;
        for (UINT transfer : {15u, 16u}) {
            metadata.transferFunction = transfer;
            Require(reject.initialize(hwnd, width, height, 60, format, false, {},
                DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020, true, metadata) == DXGI_ERROR_UNSUPPORTED,
                "RGB PQ/HLG explicitly rejected without YUV matrix");
        }
        metadata.transferFunction = 4; metadata.primaries = 9;
        Require(reject.initialize(hwnd, width, height, 60, format, false, {},
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020, true, metadata) == DXGI_ERROR_UNSUPPORTED,
            "RGB BT2020 primaries rejected even for SDR transfer");
    }
    {
        DirectD3D11Renderer r;
        for (bool fullRange : {true, false})
        for (bool topDown : {true, false}) {
            llcv::video_color::Configuration color{};
            color.range = fullRange ? llcv::video_color::Range::Full : llcv::video_color::Range::Limited;
            const HRESULT hr = r.initialize(hwnd, width, height, 60, format,
                false, color, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020, topDown);
            if (hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) || hr == DXGI_ERROR_UNSUPPORTED ||
                hr == E_NOINTERFACE) {
                std::printf("SKIP: GPU RGB Video Processor input/mirror unsupported (full=%d topDown=%d) 0x%08X\n",
                    fullRange, topDown, static_cast<unsigned>(hr));
                anySkipped = true;
                continue;
            }
            Check(hr, "RGB initialize");
            anyRun = true;
            Require(r.rgbInput && r.lastTopDown == topDown, "RGB renderer state matches request");
            DXGI_COLOR_SPACE_TYPE streamSpace{}, outputSpace{};
            r.videoContext1->VideoProcessorGetStreamColorSpace1(r.processor, 0, &streamSpace);
            r.videoContext1->VideoProcessorGetOutputColorSpace1(r.processor, &outputSpace);
            Require(streamSpace == (fullRange ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 : DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709), "RGB queried tuple applied to stream");
            Require(outputSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, "RGB queried tuple applied to output");

            // Pattern is defined in buffer-row space: blue ramps with column,
            // green ramps with buffer row, red alternates, and the X byte is
            // deliberately non-constant garbage that must never be sampled.
            const UINT pitch = (width * bytesPerPixel + 3) & ~3u;
            std::vector<BYTE> pixels(static_cast<size_t>(pitch) * height);
            for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
                BYTE* p = &pixels[y * pitch + x * bytesPerPixel];
                p[0] = static_cast<BYTE>((x * 255) / (width - 1));   // B
                p[1] = static_cast<BYTE>((y * 255) / (height - 1));  // G
                p[2] = static_cast<BYTE>(((x + y) % 2) ? 200 : 40);  // R
                if (!rgb24) p[3] = static_cast<BYTE>((x * 37 + y) & 0xff);       // garbage X
            }
            r.upload(pixels.data(), pitch);
            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE; stream.pInputSurface = r.inputViews[r.activeUploadSurface];
            Check(r.videoContext->VideoProcessorBlt(r.processor, r.outputView, 0, 1, &stream), "RGB blit");

            const auto expand = [&](double value) {
                return fullRange ? value : std::clamp((value - 16.0) * 255 / 219, 0.0, 255.0);
            };
            llcv::screenshot::Description screenshot{width, height,
                rgb24 ? llcv::screenshot::Format::Rgb24 : llcv::screenshot::Format::Rgb32};
            screenshot.color = color; screenshot.topDown = topDown;
            std::vector<BYTE> packed(llcv::screenshot::PackedSize(screenshot));
            std::vector<BYTE> screenshotPixels(size_t(width)*height*4);
            Require(llcv::screenshot::CopyFrame(screenshot, pixels.data(), pixels.size(), pitch, packed), "RGB screenshot copy");
            Require(llcv::screenshot::ConvertRows(screenshot, packed, 0, height, screenshotPixels), "RGB screenshot conversion");
            for (UINT bufferRow : {0u, height - 1}) for (UINT x : {0u, width / 2, width - 1}) {
                // A bottom-up DIB's buffer row 0 is the image's bottom; the
                // renderer's mirror corrects this on the GPU without any CPU
                // reordering, so buffer row and screen row diverge exactly
                // when topDown is false.
                const UINT screenY = topDown ? bufferRow : (height - 1 - bufferRow);
                const double expectedBlue = expand((x * 255.0) / (width - 1));
                const double expectedGreen = expand((bufferRow * 255.0) / (height - 1));
                const double expectedRed = expand(((x + bufferRow) % 2) ? 200.0 : 40.0);
                const auto actual = Read(r, x, screenY);
                const BYTE* screenshotPixel = screenshotPixels.data() + (size_t(screenY)*width+x)*4;
                Near(actual[0], screenshotPixel[2], 3, "GPU matches screenshot red");
                Near(actual[1], screenshotPixel[1], 3, "GPU matches screenshot green");
                Near(actual[2], screenshotPixel[0], 3, "GPU matches screenshot blue");
                Require(screenshotPixel[3] == 255, "screenshot alpha opaque");
                Near(actual[0], expectedRed, 3, "RGB red channel (orientation/range aware)");
                Near(actual[1], expectedGreen, 3, "RGB green channel (orientation/range aware)");
                Near(actual[2], expectedBlue, 3, "RGB blue channel (orientation/range aware)");
            }
            RequireCleanGpu(r);

            Check(r.resizeOutput(hwnd), "RGB output resize");
            Require(r.lastTopDown == topDown, "RGB orientation survives output resize");
            // Crop in post-mirror image coordinates, using an asymmetric image.
            for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
                BYTE* p = &pixels[y * pitch + x * bytesPerPixel];
                const UINT screenY = topDown ? y : height - 1 - y;
                p[0] = screenY < height / 2 ? 50 : 180;
                p[1] = x < width / 2 ? 80 : 200;
                p[2] = screenY < height / 2 ? 120 : 220;
                if (!rgb24) p[3] = 0;
            }
            r.upload(pixels.data(), pitch);
            RECT crop{16, 8, LONG(width / 2 - 16), LONG(height / 2 - 8)};
            const RECT storageCrop = VideoSourceRectForStorage(crop, height, !topDown);
            r.videoContext->VideoProcessorSetStreamSourceRect(r.processor, 0, TRUE, &storageCrop);
            stream.pInputSurface = r.inputViews[r.activeUploadSurface];
            for (int draw = 0; draw < 2; ++draw)
                Check(r.videoContext->VideoProcessorBlt(r.processor, r.outputView, 0, 1, &stream), "RGB crop repeated blit");
            const auto cropped = Read(r, width / 2, height / 2);
            Near(cropped[0], expand(120), 3, "RGB crop red");
            Near(cropped[1], expand(80), 3, "RGB crop green");
            Near(cropped[2], expand(50), 3, "RGB crop blue");
            RequireCleanGpu(r);

            // Resize + repeated draw: reinitialize smaller with the same
            // orientation/range and confirm the pipeline still produces the
            // correct sample without leaking GPU warnings.
            Check(r.initialize(hwnd, width / 2, height / 2, 60, format,
                false, color, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020, topDown),
                "RGB resize");
            const UINT smallPitch = ((width / 2) * bytesPerPixel + 3) & ~3u;
            std::vector<BYTE> smallPixels(static_cast<size_t>(smallPitch) * (height / 2));
            for (auto& byteValue : smallPixels) byteValue = 0;
            for (UINT y = 0; y < height / 2; ++y) for (UINT x = 0; x < width / 2; ++x) {
                BYTE* p = &smallPixels[y * smallPitch + x * bytesPerPixel];
                p[0] = 30; p[1] = 20; p[2] = 10; if (!rgb24) p[3] = 0xAA;
            }
            r.upload(smallPixels.data(), smallPitch);
            D3D11_VIDEO_PROCESSOR_STREAM smallStream{};
            smallStream.Enable = TRUE; smallStream.pInputSurface = r.inputViews[r.activeUploadSurface];
            Check(r.videoContext->VideoProcessorBlt(r.processor, r.outputView, 0, 1, &smallStream),
                "RGB resized blit");
            const UINT cy = topDown ? 0u : (height / 2 - 1);
            const auto resizedActual = Read(r, 0, cy);
            Near(resizedActual[0], expand(10), 3, "RGB resize preserves red");
            Near(resizedActual[1], expand(20), 3, "RGB resize preserves green");
            Near(resizedActual[2], expand(30), 3, "RGB resize preserves blue");
            RequireCleanGpu(r);
            std::printf("RGB fullRange=%d topDown=%d: pixel/orientation/resize fidelity passed\n",
                fullRange, topDown);
        }
    }
    DestroyWindow(hwnd); CoUninitialize();
    if (!anyRun) { std::puts("SKIP: GPU RGB Video Processor path unsupported in this environment"); return 77; }
    std::printf("RGB fidelity tests %s; no capture device used.\n",
        anySkipped ? "passed for supported combinations (some skipped)" : "passed for all combinations");
    return 0;
}

static void SaveOverlayPreview(DirectD3D11Renderer& renderer, ID3D11Texture2D* texture,
                               const std::filesystem::path& path) {
    if (path.empty()) return;
    llcv::hdr_audit::Image image;
    Check(llcv::hdr_audit::Read(renderer.context, texture, image), "overlay preview readback");
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
    info.biSize = sizeof(info); info.biWidth = desc.Width;
    info.biHeight = -static_cast<LONG>(desc.Height); info.biPlanes = 1;
    info.biBitCount = 32; info.biCompression = BI_RGB;
    info.biSizeImage = desc.Width * desc.Height * 4;
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    Require(image.bytes.size() == info.biSizeImage, "tightly packed BGRA preview");
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(image.bytes.data()), image.bytes.size());
    Require(output.good(), "preview written");
}

static void TestOverlayRefreshIsolation(DirectD3D11Renderer& r) {
    const bool savedOsd = g_osdVisible.load();
    const bool savedAudio = g_audioOsdVisible.load();
    const auto savedUntil = g_volumeHudUntilMs.load();
    const auto savedContent = g_transientHudContent.load();
    const int savedVolume = g_volumePercent.load();
    g_osdVisible = false; g_audioOsdVisible = false; g_volumeHudUntilMs = 0;
    g_overlayGeneration.fetch_add(1);
    Check(r.drawOverlayQuads(), "hidden panels skip refresh");
    Require(!r.osdTextLayout && !r.volumeTextLayout && !r.cachedOverlayMask,
        "hidden panels allocate no layouts");

    g_audioOsdVisible = true;
    Check(r.drawOverlayQuads(), "audio-only visible refresh");
    Require(!r.osdTextLayout && !r.volumeTextLayout && r.cachedOverlayMask == 4,
        "audio updates do not create hidden diagnostic or notification layouts");
    g_audioOsdVisible = false;
    g_transientHudContent = TransientHudContent::Volume;
    g_volumeHudUntilMs = GetTickCount64() + 60000;
    Check(r.drawOverlayQuads(), "notification revealed in same generation");
    Require(!r.osdTextLayout && r.volumeTextLayout && r.cachedOverlayMask == 6,
        "newly visible notification is not skipped by shared generation");
    g_volumeHudUntilMs = 0; g_osdVisible = true;
    Check(r.drawOverlayQuads(), "diagnostics revealed in same generation");
    Require(r.osdTextLayout && r.cachedOverlayMask == 7,
        "newly visible diagnostics are refreshed");

    auto* savedDiagnosticLayout = r.osdTextLayout;
    auto* savedNotificationLayout = r.volumeTextLayout;
    savedDiagnosticLayout->AddRef(); savedNotificationLayout->AddRef();
    const auto pixels = [&](ID3D11Texture2D* texture) {
        llcv::hdr_audit::Image image;
        Check(llcv::hdr_audit::Read(r.context, texture, image), "isolation readback");
        return image.bytes;
    };
    const auto diagnosticPixels = pixels(r.osdOverlayTexture);
    const auto notificationPixels = pixels(r.volumeOverlayTexture);
    const auto audioPixels = pixels(r.audioOverlayTexture);
    g_osdVisible = false; g_audioOsdVisible = true;
    for (int update = 0; update < 32; ++update) {
        g_volumePercent = 20 + update;
        g_overlayGeneration.fetch_add(1);
        Check(r.drawOverlayQuads(), "isolated audio meter update");
        Require(r.osdTextLayout == savedDiagnosticLayout &&
            r.volumeTextLayout == savedNotificationLayout && r.cachedOverlayMask == 4,
            "repeated audio updates preserve hidden layouts");
    }
    Require(pixels(r.osdOverlayTexture) == diagnosticPixels &&
        pixels(r.volumeOverlayTexture) == notificationPixels,
        "hidden panel textures are unchanged");
    Require(pixels(r.audioOverlayTexture) != audioPixels, "visible audio panel still updates");
    Check(r.refreshOverlayLayouts(true, true, true), "reveal stale panels");
    Require(r.osdTextLayout != savedDiagnosticLayout &&
        r.volumeTextLayout != savedNotificationLayout && r.cachedOverlayMask == 7,
        "stale hidden layouts refresh when revealed");
    savedDiagnosticLayout->Release(); savedNotificationLayout->Release();
    const auto selectiveNotification = pixels(r.volumeOverlayTexture);
    const auto selectiveAudio = pixels(r.audioOverlayTexture);
    g_overlayGeneration.fetch_add(1);
    Check(r.refreshOverlayLayouts(), "full refresh reference");
    Require(pixels(r.volumeOverlayTexture) == selectiveNotification &&
        pixels(r.audioOverlayTexture) == selectiveAudio,
        "selective refresh preserves output pixels");

    const auto validGeneration = r.cachedOverlayGeneration;
    g_overlayGeneration.fetch_add(1);
    r.osdCacheTarget->BeginDraw(); // A second BeginDraw causes a recoverable D2D error.
    Require(FAILED(r.refreshOverlayLayouts(true, false, false)), "failed draw injected");
    Require(r.cachedOverlayGeneration == validGeneration, "failed draw never publishes cache");
    Check(r.refreshOverlayLayouts(true, false, false), "failed generation is retried");
    Require(r.cachedOverlayGeneration == g_overlayGeneration.load() &&
        r.cachedOverlayMask == 1, "retry publishes only successful panel");

    g_osdVisible = savedOsd; g_audioOsdVisible = savedAudio;
    g_volumeHudUntilMs = savedUntil; g_transientHudContent = savedContent;
    g_volumePercent = savedVolume;
    g_overlayGeneration.fetch_add(1);
}

static void TestMuteIndicator(DirectD3D11Renderer& r, const std::filesystem::path& preview = {}) {
    const bool savedOsd=g_osdVisible.load(), savedAudio=g_audioOsdVisible.load();
    const bool savedMute=g_playbackMuted.load();
    const auto savedHud=g_volumeHudUntilMs.load();
    g_osdVisible=false; g_audioOsdVisible=false; g_volumeHudUntilMs=0; g_playbackMuted=false;
    Require(!r.muteIndicatorTexture && !r.muteIndicatorShaderView,"mute badge has no eager GPU allocation");
    const float background[]{0.08f,0.12f,0.18f,1.0f};
    const auto clear=[&] { r.context->ClearRenderTargetView(r.backBufferRenderTarget,background); };
    clear();
    auto frames=g_overlayRenderedFrames.load();
    Check(r.drawOverlayQuads(),"unmuted UI-free output");
    Require(g_overlayRenderedFrames.load()==frames && !r.muteIndicatorTexture,
            "normal hidden-OSD playback keeps its zero-overlay path");
    g_playbackMuted=true; g_audioOsdVisible=true;
    Check(r.drawOverlayQuads(),"mute shown in existing audio OSD");
    Require(!r.muteIndicatorTexture,"audio OSD suppresses redundant corner badge");
    g_audioOsdVisible=false;
    clear();
    llcv::hdr_audit::Image before,after,icon,restored;
    Check(llcv::hdr_audit::Read(r.context,r.backBuffer,before),"mute badge background");
    const auto cachedGeneration=r.cachedOverlayGeneration;
    const auto cachedMask=r.cachedOverlayMask;
    Check(r.drawOverlayQuads(),"persistent mute with no other overlay");
    Check(llcv::hdr_audit::Read(r.context,r.backBuffer,after),"mute badge output");
    Require(before.bytes!=after.bytes,"mute remains visible after transient HUD expires");
    const auto rect=llcv::audio_osd::MuteIndicatorRect(r.outputWidth,r.outputHeight);
    const UINT bpp=before.rowBytes/before.desc.Width;
    bool outsideUnchanged=true;
    for (UINT y=0;y<before.desc.Height;++y) for (UINT x=0;x<before.desc.Width;++x) {
        if (x>=static_cast<UINT>(rect.left) && x<static_cast<UINT>(rect.right) &&
            y>=static_cast<UINT>(rect.top) && y<static_cast<UINT>(rect.bottom)) continue;
        const auto offset=y*before.rowBytes+x*bpp;
        outsideUnchanged &= memcmp(before.bytes.data()+offset,after.bytes.data()+offset,bpp)==0;
    }
    Require(outsideUnchanged,"mute badge changes only its small top-right rectangle");
    Check(llcv::hdr_audit::Read(r.context,r.muteIndicatorTexture,icon),"mute badge alpha");
    bool transparent=false,translucent=false,opaque=false;
    for(size_t i=3;i<icon.bytes.size();i+=4) {
        transparent |= icon.bytes[i]==0;
        translucent |= icon.bytes[i]>0 && icon.bytes[i]<255;
        opaque |= icon.bytes[i]==255;
    }
    Require(transparent && translucent && !opaque,"badge and speaker strokes remain translucent");
    const auto* texture=r.muteIndicatorTexture;
    for(int i=0;i<3;++i) { clear(); Check(r.drawOverlayQuads(),"cached mute indicator"); }
    Require(texture==r.muteIndicatorTexture && cachedGeneration==r.cachedOverlayGeneration &&
            cachedMask==r.cachedOverlayMask,"persistent mute reuses its texture and leaves hidden text caches untouched");
    if(!preview.empty()) SaveOverlayPreview(r,r.backBuffer,preview);
    const auto savedPosition=g_settings.volumeHudPosition;
    const auto savedContent=g_transientHudContent.load();
    g_settings.volumeHudPosition=VolumeHudPosition::TopRight;
    g_transientHudContent=TransientHudContent::Volume;
    g_volumeHudUntilMs=GetTickCount64()+60000; g_overlayGeneration.fetch_add(1);
    clear(); Check(r.drawOverlayQuads(),"top-right HUD beside persistent mute");
    llcv::hdr_audit::Image withHud;
    Check(llcv::hdr_audit::Read(r.context,r.backBuffer,withHud),"HUD and mute coexistence");
    bool badgeUnchanged=true;
    for(int y=rect.top;y<rect.bottom;++y) for(int x=rect.left;x<rect.right;++x) {
        const auto offset=y*before.rowBytes+x*bpp;
        badgeUnchanged &= memcmp(after.bytes.data()+offset,withHud.bytes.data()+offset,bpp)==0;
    }
    Require(badgeUnchanged,"top-right volume notification never overlaps the mute badge");
    g_settings.volumeHudPosition=savedPosition; g_transientHudContent=savedContent;
    g_volumeHudUntilMs=0;
    g_playbackMuted=false; clear();
    Check(r.drawOverlayQuads(),"unmute hides persistent badge");
    Check(llcv::hdr_audit::Read(r.context,r.backBuffer,restored),"unmuted background");
    Require(before.bytes==restored.bytes,"unmuting leaves no icon pixels");
    r.muteIndicatorUnavailable=true; g_playbackMuted=true;
    frames=g_overlayRenderedFrames.load();
    Check(r.drawOverlayQuads(),"unavailable cosmetic indicator does not stop playback");
    Require(g_overlayRenderedFrames.load()==frames,"unavailable badge is not retried every frame");
    r.muteIndicatorUnavailable=false;
    RequireCleanGpu(r);
    g_playbackMuted=savedMute; g_osdVisible=savedOsd; g_audioOsdVisible=savedAudio;
    g_volumeHudUntilMs=savedHud; g_overlayGeneration.fetch_add(1);
}

static int TestOverlayUi(const char* directory) {
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "overlay COM");
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"Hidden OSD UI test", WS_POPUP,
        0, 0, 1280, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(hwnd != nullptr, "overlay hidden window");
    const std::filesystem::path root = directory ? directory : "";
    if (!root.empty()) std::filesystem::create_directories(root);
    g_settings.pixelPerfect = false;
    g_settings.presentationMode = PresentationMode::VSync;
    g_osdVisible = true; g_audioOsdVisible = true;
    for (bool english : {false, true}) {
        g_settings.uiLanguage = english ? UiLanguage::English : UiLanguage::Korean;
        DirectD3D11Renderer r;
        Check(r.initialize(hwnd, 64, 64, 60, VideoPixelFormat::Nv12), "overlay initialize");
        TestOverlayRefreshIsolation(r);
        TestMuteIndicator(r, root.empty() ? std::filesystem::path{} :
            root/(english ? "persistent-mute-en.bmp" : "persistent-mute-ko.bmp"));
        Require(r.overlayFonts.Available(), "embedded DirectWrite fonts loaded");
        for (auto* format : {r.osdTextFormat, r.volumeTextFormat, r.audioTextFormat,
                            r.audioValueFormat, r.audioSmallFormat}) {
            wchar_t family[128]{};
            Check(format->GetFontFamilyName(family, 128), "private family name");
            Require(wcsstr(family, L"Pretendard") != nullptr, "OSD uses application font family");
            IDWriteFontCollection* collection = nullptr;
            Check(format->GetFontCollection(&collection), "private collection");
            Require(collection != nullptr, "not silent system font substitution");
            UINT32 index = 0; BOOL exists = FALSE;
            Check(collection->FindFamilyName(family, &index, &exists), "family present");
            Require(exists, "family exists in embedded collection");
            IDWriteFontFamily* fontFamily = nullptr;
            Check(collection->GetFontFamily(index, &fontFamily), "private font family");
            IDWriteFont* font = nullptr;
            Check(fontFamily->GetFirstMatchingFont(format->GetFontWeight(),
                DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font), "actual font weight");
            Require(font->GetWeight() == format->GetFontWeight() &&
                font->GetSimulations() == DWRITE_FONT_SIMULATIONS_NONE, "real font weight, not synthetic bold");
            font->Release(); fontFamily->Release(); collection->Release();
        }
        const auto fits = [&](const wchar_t* text, IDWriteTextFormat* format, float width, float height) {
            IDWriteTextLayout* layout = nullptr;
            Check(r.dwriteFactory->CreateTextLayout(text, static_cast<UINT32>(wcslen(text)),
                format, width, height, &layout), "measure overlay caption");
            DWRITE_TEXT_METRICS metrics{}; Check(layout->GetMetrics(&metrics), "caption metrics");
            if (metrics.width > width || metrics.height > height)
                std::wprintf(L"Clipped caption: %s, %.1f x %.1f / %.1f x %.1f\n",
                    text, metrics.width, metrics.height, width, height);
            Require(metrics.width <= width && metrics.height <= height, "overlay text fits");
            layout->Release();
        };
#ifdef LLCV_VSR_FEATURE
        const auto savedVsrState = r.vsrState;
        const UINT savedInputWidth = r.vsrInputWidth, savedInputHeight = r.vsrInputHeight;
        const UINT savedDisplayWidth = r.vsrDisplayWidth, savedDisplayHeight = r.vsrDisplayHeight;
        r.vsrInputWidth = 1920; r.vsrInputHeight = 1080;
        r.vsrDisplayWidth = 3840; r.vsrDisplayHeight = 2160;
        for (const auto state : {llcv::vsr::State::Untouched, llcv::vsr::State::Off,
                llcv::vsr::State::Requested, llcv::vsr::State::Bypassed,
                llcv::vsr::State::Rejected, llcv::vsr::State::Unknown}) {
            r.vsrState = state;
            const auto line = r.vsrOsdLine();
            Require(line.find(L"1920×1080 → 3840×2160") != std::wstring::npos,
                "VSR diagnostics use actual input and video rectangle sizes");
            Require(line.find(L"ms") == std::wstring::npos, "no unverified VSR latency number");
            if (state == llcv::vsr::State::Requested)
                Require(line.find(L"ON") != std::wstring::npos &&
                    line.find(L"requested") == std::wstring::npos &&
                    line.find(L"요청") == std::wstring::npos,
                    "ON UI is concise; activation caveat remains in setup guide");
            if (state == llcv::vsr::State::Unknown)
                Require(line.find(L"OFF") == std::wstring::npos, "unknown state never claims OFF");
            fits(line.substr(0, line.size() - 1).c_str(), r.osdTextFormat, r.kOsdTextWidth, 20);
            g_overlayGeneration.fetch_add(1);
            Check(r.refreshOverlayLayouts(), "VSR diagnostics refresh");
            DWRITE_TEXT_METRICS metrics{};
            Check(r.osdTextLayout->GetMetrics(&metrics), "VSR diagnostics fit");
            Require(metrics.lineCount == 20 && metrics.height <= r.kOsdTextHeight,
                "VSR row retains all diagnostics without wrapping or clipping");
        }
        r.vsrState = savedVsrState;
        r.vsrInputWidth = savedInputWidth; r.vsrInputHeight = savedInputHeight;
        r.vsrDisplayWidth = savedDisplayWidth; r.vsrDisplayHeight = savedDisplayHeight;
        g_overlayGeneration.fetch_add(1);
#endif
        fits(L"100%", r.audioValueFormat, 88, 33);
        fits(L"200%", r.audioValueFormat, 88, 29);
        fits(L"-96.0 dBFS", r.audioSmallFormat, 120, 22);
        for (unsigned key : {unsigned(VK_SPACE),unsigned(VK_SEPARATOR),unsigned(VK_F24),unsigned(VK_RIGHT)}) {
            auto custom=keys::Defaults;
            custom[2]=keys::Ctrl|keys::Alt|keys::Shift|key;
            const auto hint=keys::Expand(english ? L"MAX 200% · F3" : L"최대 200% · F3",custom);
            fits(hint.c_str(),r.audioSmallFormat,234,26);
        }
        fits(english ? L"Wheel · Double-click reset" : L"휠 조절 · 두 번 클릭 초기화",
            r.audioSmallFormat, 208, 22);
        const auto hasColor = [&](ID3D11Texture2D* texture, RECT rect, COLORREF expected) {
            llcv::hdr_audit::Image pixels;
            Check(llcv::hdr_audit::Read(r.context, texture, pixels), "OSD color readback");
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            for (LONG y = rect.top; y < rect.bottom; ++y)
                for (LONG x = rect.left; x < rect.right; ++x) {
                    const BYTE* pixel = pixels.bytes.data() +
                        (static_cast<size_t>(y) * desc.Width + x) * 4;
                    if (std::abs(int(pixel[2]) - GetRValue(expected)) <= 1 &&
                        std::abs(int(pixel[1]) - GetGValue(expected)) <= 1 &&
                        std::abs(int(pixel[0]) - GetBValue(expected)) <= 1) return true;
                }
            return false;
        };
        const auto verifyBoost = [&](bool expected) {
            const COLORREF orange = llcv::overlay_ui::kPalette.kWarning;
            Require(hasColor(r.audioOverlayTexture, {220, 33, 308, 62}, orange) == expected,
                "master percentage is orange only above unity gain with boost enabled");
            Require(hasColor(r.volumeOverlayTexture, {16, 6, 244, 50}, orange) == expected,
                "transient volume indication follows the same boost threshold");
            Require(!hasColor(r.audioOverlayTexture, {16, 84, 320, 168}, orange),
                "master boost never recolors L/R channel values");
        };
        for (bool boost : {false, true}) for (bool clip : {false, true}) {
            g_settings.allowVolumeBoost = boost;
            g_volumePercent = boost ? 200 : 100;
            g_leftVolumePercent = 100; g_rightVolumePercent = clip ? 75 : 100;
            g_audioPeakLeft = clip ? 32768 : 1638; g_audioPeakRight = 492;
            g_audioClipUntilMs = clip ? GetTickCount64() + 5000 : 0;
            g_audioOsdHoverTarget = clip ? 1 : 0;
            g_transientHudContent = TransientHudContent::Volume;
            g_overlayGeneration.fetch_add(1);
            Check(r.refreshOverlayLayouts(), "production OSD render");
            verifyBoost(boost);
            Require(hasColor(r.audioOverlayTexture, {16, 171, 111, 193},
                llcv::overlay_ui::kPalette.kDanger) == clip, "red clipping status remains independent");
            DWRITE_TEXT_METRICS metrics{};
            Check(r.osdTextLayout->GetMetrics(&metrics), "diagnostics metrics");
            Require(metrics.height <= r.kOsdTextHeight, "diagnostics retained without clipping");
            auto* cached = r.osdTextLayout;
            for (int frame = 0; frame < 1000; ++frame)
                Check(r.refreshOverlayLayouts(), "unchanged overlay cache");
            Require(cached == r.osdTextLayout, "unchanged frames do not recreate layout");
            if (!root.empty()) {
                const std::string suffix = std::string(english ? "en" : "ko") +
                    (boost ? "-200" : "-100") + (clip ? "-clip" : "-normal") + ".bmp";
                SaveOverlayPreview(r, r.audioOverlayTexture, root / ("audio-" + suffix));
                SaveOverlayPreview(r, r.volumeOverlayTexture, root / ("volume-" + suffix));
                SaveOverlayPreview(r, r.osdOverlayTexture, root / ("diagnostics-" + suffix));
            }
        }
        for (bool enabled : {false, true}) {
            g_settings.allowVolumeBoost = enabled;
            for (int percent : {0, 99, 100, 101, 150, 200, 100, 99}) {
                g_volumePercent = percent;
                g_overlayGeneration.fetch_add(1);
                Check(r.refreshOverlayLayouts(), "boost boundary transition");
                verifyBoost(enabled && percent > 100);
            }
        }
        g_volumePercent=50;g_audioClipUntilMs=0;g_audioOsdHoverTarget=0;
        g_transientHudContent=TransientHudContent::Volume;
        g_overlayGeneration.fetch_add(1);
        Check(r.refreshOverlayLayouts(),"unmuted audio overlay reference");
        llcv::hdr_audit::Image audible,muted,restored;
        Check(llcv::hdr_audit::Read(r.context,r.audioOverlayTexture,audible),"audible overlay readback");
        SetPlaybackMuted(nullptr,true);
        Check(r.refreshOverlayLayouts(),"muted audio overlay");
        Check(llcv::hdr_audit::Read(r.context,r.audioOverlayTexture,muted),"muted overlay readback");
        Require(audible.bytes!=muted.bytes && g_volumePercent==50,
            "mute changes speaker/text/bar pixels but preserves original volume");
        DWRITE_TEXT_METRICS muteMetrics{};
        Check(r.volumeTextLayout->GetMetrics(&muteMetrics),"mute notification metrics");
        Require(muteMetrics.height<=62 && muteMetrics.width<=228,"KO/EN muted notification fits");
        if(!root.empty()) SaveOverlayPreview(r,r.audioOverlayTexture,root/(english?"audio-muted-en.bmp":"audio-muted-ko.bmp"));
        SetPlaybackMuted(nullptr,false);
        Check(r.refreshOverlayLayouts(),"unmuted audio overlay restored");
        Check(llcv::hdr_audit::Read(r.context,r.audioOverlayTexture,restored),"restored overlay readback");
        Require(audible.bytes==restored.bytes,"unmute restores exactly the cached audio panel");
        g_settings.allowVolumeBoost = true; g_volumePercent = 200;
        for (bool custom:{false,true}) {
        g_settings.shortcuts=keys::Defaults;
        if (custom) {
            g_settings.shortcuts[4]=keys::Ctrl|keys::Alt|keys::Shift|VK_SPACE;
            g_settings.shortcuts[7]=keys::Ctrl|'D';
            const auto diagnostic=BuildRuntimeOsdText(1280,720);
            Require(diagnostic.find(L"[Ctrl+D")!=std::wstring::npos &&
                diagnostic.find(L"[Tab")==std::wstring::npos,"diagnostic OSD shows configured close shortcut");
        }
        for (const auto position : {VolumeHudPosition::TopLeft, VolumeHudPosition::TopRight,
                VolumeHudPosition::BottomLeft, VolumeHudPosition::BottomRight}) {
        g_settings.volumeHudPosition = position;
        for (int message = 0; message <= static_cast<int>(TransientHudContent::VsrFailed); ++message) {
            g_transientHudContent = static_cast<TransientHudContent>(message);
            g_overlayGeneration.fetch_add(1);
            Check(r.refreshOverlayLayouts(), "every transient HUD");
            DWRITE_TEXT_METRICS metrics{}; Check(r.volumeTextLayout->GetMetrics(&metrics), "HUD metrics");
            if (metrics.height > 62 || metrics.width > 228)
                std::printf("HUD clipped: language=%s message=%d metrics=%.1fx%.1f\n",
                    english ? "en" : "ko", message, metrics.width, metrics.height);
            Require(metrics.height <= 62 && metrics.width <= 228, "transient message fits");
            const bool volume = message == static_cast<int>(TransientHudContent::Volume);
            const bool bottom = position == VolumeHudPosition::BottomLeft || position == VolumeHudPosition::BottomRight;
            const float expectedHeight = volume ? 82.0f : std::clamp(std::ceil(metrics.height) + 20.0f, 40.0f, 82.0f);
            Require(r.volumePanelHeight == expectedHeight &&
                r.volumePanelTop == (bottom ? 82.0f - expectedHeight : 0.0f),
                "notification hugs text and preserves top/bottom screen margin");
            llcv::hdr_audit::Image notification;
            Check(llcv::hdr_audit::Read(r.context, r.volumeOverlayTexture, notification), "notification padding pixels");
            for (int y = 0; y < 82; ++y) if (y < r.volumePanelTop || y >= r.volumePanelTop + r.volumePanelHeight)
                for (int x = 0; x < 260; ++x)
                    Require(notification.bytes[(y * 260 + x) * 4 + 3] == 0,
                        "no stale panel pixels outside fitted notification");
            if (!custom && !root.empty() && position == VolumeHudPosition::TopLeft &&
                (message == static_cast<int>(TransientHudContent::VsrOn) ||
                 message == static_cast<int>(TransientHudContent::ScreenshotSaved)))
                SaveOverlayPreview(r, r.volumeOverlayTexture, root / (std::string(english ? "en-" : "ko-") +
                    (message == static_cast<int>(TransientHudContent::VsrOn) ? "vsr-fit.bmp" : "saved-fit.bmp")));
            if (message != static_cast<int>(TransientHudContent::Volume))
                Require(!hasColor(r.volumeOverlayTexture, {0, 0, 260, 82},
                    llcv::overlay_ui::kPalette.kWarning), "boost color does not leak to screenshot/VSR messages");
        }
        }
        }
        g_settings.shortcuts=keys::Defaults;
        g_settings.volumeHudPosition = VolumeHudPosition::TopLeft;
        r.reset();
        Require(!r.overlayFonts.Available() && !r.audioValueFormat && !r.audioSmallFormat &&
            !r.muteIndicatorTexture && !r.muteIndicatorShaderView && !r.muteIndicatorUnavailable &&
            !r.audioCacheCardBrush && !r.osdCacheEdgeBrush &&
            !r.audioCacheBoostBrush && !r.volumeCacheBoostBrush, "OSD resources released on reset");
        Require(r.cachedOverlayGeneration == 0 && r.cachedOverlayMask == 0,
            "reset invalidates all panel caches");
    }
    DestroyWindow(hwnd); CoUninitialize();
    std::puts("OSD UI: embedded font weights, bilingual text fit, gain/clip states, all notifications, cache reuse and reset passed.");
    return 0;
}

// Native HDR VSR uses the production processor, not an SDR intermediate.
static int TestHdrVsr(bool paced = false) {
    using namespace llcv::vsr;
    // This fixture exercises VSR explicitly; a fresh profile now selects CMAA/NIS for F6.
    g_settings.preferredUpscaler=llcv::settings::UpscalingMethod::NvidiaVsr;
    g_settings.cmaaNisEnabled=false;
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "VSR HDR COM");
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"Hidden HDR VSR test", WS_POPUP,
        0, 0, 1280, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(hwnd != nullptr, "VSR HDR hidden window");
    g_settings.pixelPerfect = false;
    g_settings.audioOnly = false;
    g_settings.presentationMode = PresentationMode::VSync;
    g_osdVisible = false; g_audioOsdVisible = false; g_volumeHudUntilMs = 0;
    unsigned requestedFrames = 0, fallbackFrames = 0, bypassFrames = 0, changedPatterns = 0, patterns = 0;
    for (SIZE inputSize : {SIZE{1280,720}, SIZE{1920,1080}, SIZE{2560,1440}}) {
    if (paced && inputSize.cx != 1280) continue;
    const UINT width = static_cast<UINT>(inputSize.cx), height = static_cast<UINT>(inputSize.cy);
    const size_t pixels = size_t{width} * height;
    std::vector<unsigned short> frame(pixels * 3 / 2, 512 << 6);
    for (auto chroma : {DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,
                       DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020}) {
        if (paced && chroma != DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020) continue;
        for (SIZE size : {SIZE{1280,720}, SIZE{1920,1080}, SIZE{2560,1440}, SIZE{3840,2160}}) {
            if (paced && size.cx != 2560) continue;
            Require(SetWindowPos(hwnd, nullptr, 0, 0, size.cx, size.cy,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "HDR VSR output size");
            // Include startup-ON as well as F6 enabling from a disabled start.
            g_vsrMode.store(size.cx == 1280 ? Mode::Disabled : Mode::On);
            DirectD3D11Renderer r;
            const HRESULT init = r.initialize(hwnd, width, height, 60,
                VideoPixelFormat::P010, true, {}, chroma);
            if (init == DXGI_ERROR_UNSUPPORTED || init == DXGI_ERROR_SDK_COMPONENT_MISSING) {
                std::puts("SKIP: HDR GPU conversion/debug layer unavailable");
                DestroyWindow(hwnd); CoUninitialize(); return 77;
            }
            Check(init, "HDR VSR initialize");
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            Check(r.device->QueryInterface(IID_PPV_ARGS(&dxgi)), "HDR VSR adapter device");
            Check(dxgi->GetAdapter(&adapter), "HDR VSR adapter");
            DXGI_ADAPTER_DESC adapterDesc{};
            Check(adapter->GetDesc(&adapterDesc), "HDR VSR adapter description");
            Require(r.vsrEligible == Eligible(adapterDesc.VendorId, DXGI_FORMAT_P010, true,
                width, height, static_cast<UINT>(size.cx), static_cast<UINT>(size.cy)),
                "HDR processor eligibility follows real adapter and scaling policy");
            Require(r.hdrOutput && !r.scrgbOutput, "native HDR10 route retained");
            Require(r.vsrInputWidth == width && r.vsrInputHeight == height &&
                r.vsrDisplayWidth == static_cast<UINT>(size.cx) &&
                r.vsrDisplayHeight == static_cast<UINT>(size.cy), "HDR VSR input/display dimensions");
            auto* const processor = r.processor;
            auto* const swapChain = r.swapChain;
            auto* const upload = r.nv12Textures[0];
            auto verifyRoute = [&] {
                Require(r.processor == processor && r.swapChain == swapChain &&
                    r.nv12Textures[0] == upload, "F6 never rebuilds HDR processor/swapchain/upload ring");
                D3D11_TEXTURE2D_DESC input{}, output{};
                r.nv12Textures[0]->GetDesc(&input); r.backBuffer->GetDesc(&output);
                Require(input.Format == DXGI_FORMAT_P010 && output.Format == DXGI_FORMAT_R10G10B10A2_UNORM,
                    "HDR stays ten-bit end to end");
                DXGI_COLOR_SPACE_TYPE in{}, out{};
                r.videoContext1->VideoProcessorGetStreamColorSpace1(r.processor, 0, &in);
                r.videoContext1->VideoProcessorGetOutputColorSpace1(r.processor, &out);
                Require(in == chroma && out == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
                    "F6 preserves PQ/BT2020/range/chroma interpretation");
                Require(r.hdrOverlayBackground == nullptr, "VSR adds no HDR UI scratch surface");
            };
            for (const std::array<double,3>& rgb : {
                std::array<double,3>{0,0,0}, {Pq(1),Pq(1),Pq(1)}, {Pq(100),Pq(100),Pq(100)},
                {Pq(203),Pq(203),Pq(203)}, {Pq(1000),Pq(1000),Pq(1000)},
                {Pq(4000),Pq(4000),Pq(4000)}, {0.7,0.3,0.1}, {0.1,0.3,0.7}}) {
                const double y = .2627*rgb[0] + .6780*rgb[1] + .0593*rgb[2];
                const int yc = static_cast<int>(std::lround(64+876*y));
                const int uc = static_cast<int>(std::lround(512+896*(rgb[2]-y)/1.8814));
                const int vc = static_cast<int>(std::lround(512+896*(rgb[0]-y)/1.4746));
                std::fill_n(frame.begin(), pixels, static_cast<unsigned short>(yc << 6));
                for (size_t i=pixels; i<frame.size(); i+=2) {
                    frame[i]=static_cast<unsigned short>(uc<<6); frame[i+1]=static_cast<unsigned short>(vc<<6);
                }
                std::array<unsigned,3> baseline{};
                for (int stage=0; stage<3; ++stage) {
                    if (stage == 0) g_vsrMode.store(Mode::Off);
                    else Require(HandleVsrTestKey(VK_F6, 0, 0), "HDR F6 handled");
                    Check(r.applyVsrMode(), "HDR F6 apply");
                    r.upload(reinterpret_cast<const BYTE*>(frame.data()), width*2);
                    D3D11_VIDEO_PROCESSOR_STREAM stream{};
                    stream.Enable=TRUE; stream.pInputSurface=r.inputViews[r.activeUploadSurface];
                    Check(r.processVideo(stream), "HDR VSR production blit with fallback");
                    verifyRoute();
                    const auto actual = Read(r, static_cast<UINT>(size.cx/2), static_cast<UINT>(size.cy/2));
                    for (size_t c=0; c<3; ++c) Near(actual[c], rgb[c]*1023, 4, "VSR HDR uniform patch luminance/color");
                    if (stage == 0) baseline = actual;
                    if (stage == 2) Require(actual == baseline, "F6 OFF restores unfiltered HDR exactly");
                    if (stage == 1) {
                        if (r.vsrState == State::Requested) ++requestedFrames;
                        else if (r.vsrState == State::Rejected) ++fallbackFrames;
                        else { Require(r.vsrState == State::Bypassed, "HDR ON state truthful"); ++bypassFrames; }
                    }
                }
            }
            // Fine structured detail: report actual ON/OFF pixel differences,
            // without failing hosts where NVIDIA's global enhancement is OFF.
            for (UINT y=0; y<height; ++y) for (UINT x=0; x<width; ++x) {
                const unsigned pattern = 250 + ((x/3 + y/3) % 2)*240 + ((x*13+y*7)%29);
                frame[size_t{y}*width+x] = static_cast<unsigned short>(pattern << 6);
            }
            std::fill(frame.begin()+pixels, frame.end(), static_cast<unsigned short>(512 << 6));
            auto renderPattern = [&](Mode mode) {
                g_vsrMode.store(mode);
                Check(r.applyVsrMode(), "HDR pattern request");
                auto next = std::chrono::steady_clock::now();
                for (int warm=0; warm<(paced ? 360 : 12); ++warm) {
                    if (paced) for (UINT y=0; y<height; ++y) for (UINT x=0; x<width; ++x)
                        frame[size_t{y}*width+x] = static_cast<unsigned short>((64 + 4 *
                            (((x+warm*3)/3 + y/5 + (((x+warm*3)/40+y/40)%2)*90)%220)) << 6);
                    r.upload(reinterpret_cast<const BYTE*>(frame.data()), width*2);
                    D3D11_VIDEO_PROCESSOR_STREAM stream{};
                    stream.Enable=TRUE; stream.pInputSurface=r.inputViews[r.activeUploadSurface];
                    Check(r.processVideo(stream), "HDR pattern blit");
                    if (paced) {
                        // Test-only completion/pacing lets asynchronous driver
                        // model initialization settle. NEVER used by the viewer.
                        r.context->Flush();
                        (void)Read(r, 0, 0);
                        next += std::chrono::microseconds(16667);
                        std::this_thread::sleep_until(next);
                    }
                }
                llcv::hdr_audit::Image image;
                Check(llcv::hdr_audit::Read(r.context, r.backBuffer, image), "HDR pattern test-only readback");
                return image.bytes;
            };
            const auto baseline = renderPattern(Mode::Off);
            const auto enabled = renderPattern(Mode::On);
            Require(enabled.size() == baseline.size(), "HDR VSR pattern size unchanged");
            const bool changed = enabled != baseline;
            changedPatterns += changed ? 1 : 0;
            ++patterns;
            std::printf("HDR pattern %ux%u -> %ldx%ld chroma=%d ON differs=%d\n",
                width, height, size.cx, size.cy, static_cast<int>(chroma), changed ? 1 : 0);
            Require(renderPattern(Mode::Off) == baseline, "HDR pattern OFF restores exact image");
            verifyRoute();
            RequireCleanGpu(r);
        }
    }
    }
    g_vsrMode.store(Mode::Disabled);
    DestroyWindow(hwnd); CoUninitialize();
    std::printf("HDR VSR: 720p/1080p/1440p input, 720p/1080p/1440p/4K output, both chroma placements; "
        "ON frames requested=%u fallback=%u bypass=%u, changed patterns=%u/%u. "
        "Request success is NOT proof of NVIDIA enhancement activation.\n",
        requestedFrames, fallbackFrames, bypassFrames, changedPatterns, patterns);
    return 0;
}

#include "RendererResizeTests.inl"

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--resize") return TestRendererResize();
    if (argc == 2 && std::string(argv[1]) == "--vsr-hdr-paced") return TestHdrVsr(true);
    if (argc == 2 && std::string(argv[1]) == "--vsr-hdr") return TestHdrVsr();
    if (argc >= 2 && std::string(argv[1]) == "--osd-ui") return TestOverlayUi(argc >= 3 ? argv[2] : nullptr);
    if (argc == 2 && std::string(argv[1]) == "--sdr-only") return TestRawSdr();
    if (argc == 2 && std::string(argv[1]) == "--rgb32") return TestRawSdrRgb32();
    if (argc == 2 && std::string(argv[1]) == "--rgb24") return TestRawSdrRgb32(true);
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
        // Exercise the same policy -> renderer handoff as production, not just
        // a test-only hardcoded HDR flag. Untagged P010 must now enter HDR.
        const auto automaticHdr = llcv::hdr::ResolveInput({}, false);
        Require(automaticHdr.kind == llcv::hdr::InputKind::Hdr10 && automaticHdr.assumed,
            "untagged P010 auto policy selects assumed HDR");
        Check(r.initialize(hwnd, 64, 64, 30, VideoPixelFormat::P010,
            automaticHdr.kind == llcv::hdr::InputKind::Hdr10, {}, automaticHdr.colorSpace),
            "automatic P010 HDR policy reaches real renderer");
        Require(r.hdrOutput, "automatic P010 HDR does not fall through to SDR rendering");
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
        TestMuteIndicator(r);
        blit();
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
        const auto expectedSdr = llcv::overlay_ui::Background(false);
        Require(sdrPanelColor.r == expectedSdr.r && sdrPanelColor.g == expectedSdr.g && sdrPanelColor.b == expectedSdr.b,
            "HDR-to-SDR reinitialization restores neutral application palette");
        ClearUi(r, 1, 1, 1, 1); DrawUi(r); Require(Read(r)[0] == 255, "SDR white unchanged");
        ClearUi(r, 0, 0, 0, 0.5f); Benchmark(r); RequireCleanGpu(r);
        // Explicit SDR is still identified, but P010 has no SDR output path.
        llcv::video::CaptureColorMetadata explicitSdr{};
        explicitSdr.present = true; explicitSdr.transferFunction = 5;
        const auto sdrInput = llcv::hdr::ResolveInput(explicitSdr, false);
        Require(sdrInput.kind == llcv::hdr::InputKind::Sdr, "explicit SDR policy preserved");
        Require(r.initialize(hwnd, 64, 64, 30, VideoPixelFormat::P010,
            sdrInput.kind == llcv::hdr::InputKind::Hdr10) == DXGI_ERROR_UNSUPPORTED,
            "SDR P010 rejected rather than displayed with wrong transfer");
        Require(!r.device && !r.swapChain && !r.hdrOutput,
            "rejected SDR P010 creates no GPU output resources");
        Check(r.initialize(hwnd, 64, 64, 30, VideoPixelFormat::P010,
            automaticHdr.kind == llcv::hdr::InputKind::Hdr10, {}, automaticHdr.colorSpace),
            "rejected SDR P010 to untagged P010 restores automatic HDR");
        encodePatch({Pq(203), Pq(203), Pq(203)}); blit();
        const auto automaticWhite = Read(r, 400, 250);
        for (unsigned c : automaticWhite) Near(c, Pq(203)*1023, 2, "automatic HDR keeps absolute PQ white");
        // Even when SDR correction and legacy MJPEG preferences are present,
        // the production handoff must not apply them to P010 playback/export.
        const auto savedSettings = g_settings;
        encodePatch({Pq(203), Pq(40), Pq(10)}); blit();
        const auto hdrColorBeforeSdrChoices = Read(r, 400, 250);
        std::vector<BYTE> hdrScreenshot;
        for (auto rangeOverride : {llcv::video_color::RangeOverride::Auto,
                                  llcv::video_color::RangeOverride::Limited,
                                  llcv::video_color::RangeOverride::Full}) {
            g_settings.sdrRangeOverride = rangeOverride;
            g_settings.legacyMjpegRangeOverride = llcv::video_color::RangeOverride::Full;
            g_settings.mjpegMatrixOverride = llcv::video_color::MatrixOverride::Bt601;
            Require(SelectedSdrRange(VideoPixelFormat::P010) == llcv::video_color::RangeOverride::Auto,
                "P010 rejects current and legacy SDR range overrides at the production boundary");
            const auto ignoredSdr = ResolveRawSdrVideoColor(VideoPixelFormat::P010, 64, 64, {});
            Require(ignoredSdr == llcv::video_color::Configuration{},
                "P010 has no SDR matrix/range interpretation");
            Check(r.initialize(hwnd, 64, 64, 30, VideoPixelFormat::P010, true,
                ignoredSdr, automaticHdr.colorSpace), "HDR initialize with SDR preference present");
            blit();
            Require(Read(r, 400, 250) == hdrColorBeforeSdrChoices,
                "SDR range choices leave actual HDR GPU pixels identical");
            // Poison the unused SDR description as an independent export guard.
            const auto poison = llcv::video_color::Resolve(true, 64, 64, {}, {},
                llcv::video_color::MatrixOverride::Bt601, rangeOverride);
            const llcv::screenshot::Description shot{64, 64, llcv::screenshot::Format::P010, poison};
            std::vector<BYTE> hdrScreenshotPacked(llcv::screenshot::PackedSize(shot)), bgra(64 * 4);
            Require(llcv::screenshot::CopyFrame(shot, reinterpret_cast<const BYTE*>(p.data()),
                    p.size() * sizeof(unsigned short), 128, hdrScreenshotPacked) &&
                    llcv::screenshot::ConvertRows(shot, hdrScreenshotPacked, 32, 1, bgra),
                "HDR screenshot conversion accepts P010 independently of SDR preferences");
            if (hdrScreenshot.empty()) hdrScreenshot = bgra;
            else Require(bgra == hdrScreenshot, "SDR range override leaves HDR screenshot pixels identical");
            RequireCleanGpu(r);
        }
        g_settings = savedSettings;
        Check(r.initialize(hwnd, 64, 64, 30, VideoPixelFormat::P010, true), "forced HDR comparison");
        encodePatch({Pq(203), Pq(203), Pq(203)}); blit();
        Require(Read(r, 400, 250) == automaticWhite, "automatic and forced HDR produce identical GPU pixels");
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
