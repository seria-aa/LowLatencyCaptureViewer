// Same production renderer and overlays; synthetic input, no capture/audio devices,
// no user-settings reads/writes. GPU timestamps are NOT input-to-display latency.
#include "../src/main.cpp"
#undef fwprintf
#include "../src/audio/AsioOutput.cpp"
#include <filesystem>
#include <fstream>
#include <numeric>
#include <cmath>
#include <limits>
#include <mfreadwrite.h>

using llcv::spatial::Mode;
static void Require(bool value,const char* label) {
    if(!value) { std::fprintf(stderr,"FAIL: %s\n",label);std::exit(1); }
}
static void Check(HRESULT hr,const char* label) {
    if(FAILED(hr)) {std::fprintf(stderr,"FAIL: %s 0x%08X\n",label,unsigned(hr));std::exit(1);}
}
static void Select(Mode mode) {
    std::string name=llcv::spatial::Name(mode);std::wstring w(name.begin(),name.end());
    SetEnvironmentVariableW(L"LLCV_SPATIAL_UPSCALER",w.c_str());
}
static std::vector<BYTE> Read(DirectD3D11Renderer& r) {
    D3D11_TEXTURE2D_DESC d{};r.backBuffer->GetDesc(&d);
    d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    Check(r.device->CreateTexture2D(&d,nullptr,&staging),"readback allocation");
    r.context->CopyResource(staging.Get(),r.backBuffer);
    D3D11_MAPPED_SUBRESOURCE map{};Check(r.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"readback");
    std::vector<BYTE> p(size_t(d.Width)*d.Height*4);
    for(UINT y=0;y<d.Height;y++) std::memcpy(p.data()+size_t(y)*d.Width*4,static_cast<BYTE*>(map.pData)+size_t(y)*map.RowPitch,size_t(d.Width)*4);
    r.context->Unmap(staging.Get(),0);return p;
}
static void SaveBmp(const std::filesystem::path& path,UINT w,UINT h,const std::vector<BYTE>& p) {
    BITMAPFILEHEADER f{};BITMAPINFOHEADER b{};f.bfType=0x4d42;f.bfOffBits=sizeof(f)+sizeof(b);f.bfSize=f.bfOffBits+DWORD(p.size());
    b.biSize=sizeof(b);b.biWidth=LONG(w);b.biHeight=-LONG(h);b.biPlanes=1;b.biBitCount=32;b.biCompression=BI_RGB;
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(&f),sizeof(f));out.write(reinterpret_cast<char*>(&b),sizeof(b));
    out.write(reinterpret_cast<const char*>(p.data()),std::streamsize(p.size()));Require(bool(out),"save bitmap");
}
static double Scene(double x,double y) {
    if(y<.22) return .04+.92*x; // Smooth ramp, including near black/white.
    if(y<.48) return (x>.16+.34*(y-.22) && x<.8) ? .85 : .12; // Slanted edges.
    if(y<.74) return .5+.36*std::sin(6.283185307*x*(8+120*x)); // Frequency sweep.
    const double dx=(x-.52)*1.7,dy=(y-.87)*3;
    if(dx*dx+dy*dy<.065) return .9;
    return ((int(x*120)^int(y*120))&1) ? .2 : .7;
}
static std::vector<BYTE> Pattern(UINT w,UINT h) {
    std::vector<BYTE> p(size_t(w)*h*3/2,128);
    for(UINT y=0;y<h;y++) for(UINT x=0;x<w;x++) {
        double value=0;for(int sy=0;sy<2;sy++) for(int sx=0;sx<2;sx++)
            value+=Scene((x+(sx+.5)/2)/w,(y+(sy+.5)/2)/h)*.25;
        p[size_t(y)*w+x]=BYTE(std::lround(16+219*value));
    }
    return p;
}
static std::vector<BYTE> Reference(UINT w,UINT h) {
    std::vector<BYTE> p(size_t(w)*h*4,255);
    for(UINT y=0;y<h;y++) for(UINT x=0;x<w;x++) {
        const BYTE v=BYTE(std::lround(255*Scene((x+.5)/w,(y+.5)/h)));
        for(int c=0;c<3;c++) p[(size_t(y)*w+x)*4+c]=v;
    }return p;
}
static void Render(DirectD3D11Renderer& r,const std::vector<BYTE>& p,UINT stride) {
    r.upload(p.data(),stride);
    if(r.pixelPerfectBorders && !r.skipRedundantBorderClear) {const float black[4]{0,0,0,1};r.context->ClearRenderTargetView(r.backBufferRenderTarget,black);}
    D3D11_VIDEO_PROCESSOR_STREAM s{};s.Enable=TRUE;s.pInputSurface=r.inputViews[r.activeUploadSurface];
    Check(r.processVideo(s),"production processVideo");
}
static const std::array<Mode,10> modes{Mode::Disabled,Mode::Bicubic,Mode::Cas,Mode::Fsr1,Mode::Nis,Mode::Sgsr,
    Mode::Ravu,Mode::Fsrcnnx,Mode::Jinc,Mode::Lanczos};
static HWND Window(UINT w,UINT h) {
    HWND hwnd=CreateWindowW(L"LLCV_SPATIAL_TEST",L"LLCV spatial comparison",WS_POPUP,0,0,int(w),int(h),nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Require(hwnd!=nullptr,"hidden window");return hwnd;
}
static void SmaaRegression() {
    using llcv::spatial::AaMode;
    const UINT w=640,h=360;HWND window=Window(960,540);
    Select(Mode::Nis);
    const auto patterned=Pattern(w,h);
    for(auto range:{llcv::video_color::Range::Limited,llcv::video_color::Range::Full})
    for(auto matrix:{llcv::video_color::Matrix::Bt709,llcv::video_color::Matrix::Bt601}) {
        llcv::video_color::Configuration color{};color.range=range;color.matrix=matrix;
        DirectD3D11Renderer r;Check(r.initialize(window,w,h,60,VideoPixelFormat::Nv12,false,color),"SMAA color init");
        std::vector<BYTE> flat(size_t(w)*h*3/2,126);
        for(size_t i=size_t(w)*h;i<flat.size();i+=2){flat[i]=96;flat[i+1]=158;}
        auto configure=[&](AaMode order) {
            Check(r.spatialPipeline.Configure(r.device,r.videoDevice,r.enumerator,Mode::Nis,false,w,h,
                RECT{0,0,960,540},r.adapterVendor,.15f,order),"SMAA regression configure");
            Require(r.spatialPipeline.ActiveAaMode()==order,"SMAA regression order active");
        };
        configure(AaMode::Disabled);Render(r,flat,w);const auto reference=Read(r);
        Render(r,patterned,w);const auto rawEdges=Read(r);
        for(auto order:{AaMode::SmaaBeforeNis,AaMode::SmaaAfterNis,AaMode::SmaaUltraAfterNis,AaMode::Cmaa2AfterNis,AaMode::SmaaUltraBeforeNis,AaMode::Cmaa2BeforeNis}) {
              std::fprintf(stderr,"AA check mode=%d matrix=%d range=%d\n",int(order),int(matrix),int(range));
            configure(order);Render(r,patterned,w);const auto antialiased=Read(r);
            Require(antialiased!=rawEdges,"SMAA affects synthetic edges");
              Render(r,patterned,w);const auto repeated=Read(r);
              if(repeated!=antialiased) {
                  int maximum=0;size_t changed=0;
                  for(size_t i=0;i<repeated.size();++i) {const int d=std::abs(int(repeated[i])-int(antialiased[i]));maximum=std::max(maximum,d);changed+=d!=0;}
                  std::fprintf(stderr,"AA repeat delta mode=%d maximum=%d changed=%zu\n",int(order),maximum,changed);
                  // CMAA2 uses atomic linked-list order; floating-point addition
                  // can round a blended channel one 8-bit step differently.
                  Require(llcv::spatial::AaIsCmaa2(order) && maximum<=1,"AA repeat differs beyond floating-point rounding");
              }
            Render(r,flat,w);const auto pixels=Read(r);
            for(size_t i=0;i<pixels.size();i++) {
                if(i%4==3)Require(pixels[i]==255,"SMAA alpha opaque");
                else if(std::abs(int(pixels[i])-int(reference[i]))>1) {
                    std::fprintf(stderr,"AA flat mismatch mode=%d matrix=%d range=%d byte=%zu actual=%u reference=%u\n",
                        int(order),int(matrix),int(range),i,unsigned(pixels[i]),unsigned(reference[i]));
                    Require(false,"AA flat color unchanged and no stale edges");
                }
            }
            g_audioOsdVisible=true;Check(r.drawOverlayQuads(),"SMAA OSD still draws after processing");
            Require(Read(r)!=pixels,"SMAA OSD visible");g_audioOsdVisible=false;
            Render(r,flat,w);Require(Read(r)==pixels,"SMAA next frame removes OSD");
            Require(flat[0]==126 && flat[size_t(w)*h]==96,"SMAA original input unchanged");
            Require(r.spatialPipeline.Configure(r.device,r.videoDevice,r.enumerator,Mode::Nis,true,w,h,
                RECT{0,0,960,540},r.adapterVendor,.15f,order)==S_FALSE,"SMAA HDR bypass");
            Require(!r.spatialPipeline.Enabled(),"SMAA HDR bypass releases pipeline");
        }
    }
    DestroyWindow(window);
    // AA operates on the video rectangle, not OSD or surrounding black margins.
    window=Window(976,552);DirectD3D11Renderer r;
    Check(r.initialize(window,w,h,60,VideoPixelFormat::Nv12),"SMAA placement init");
    for(auto order:{AaMode::SmaaBeforeNis,AaMode::SmaaAfterNis,AaMode::SmaaUltraAfterNis,AaMode::Cmaa2AfterNis,AaMode::SmaaUltraBeforeNis,AaMode::Cmaa2BeforeNis}) {
        Check(r.spatialPipeline.Configure(r.device,r.videoDevice,r.enumerator,Mode::Nis,false,w,h,
            RECT{8,6,968,546},r.adapterVendor,.15f,order),"SMAA offset configure");
        const float zero[4]{0,0,0,1};r.context->ClearRenderTargetView(r.backBufferRenderTarget,zero);
        Render(r,patterned,w);const auto pixels=Read(r);
        Require(pixels[0]==0 && pixels[(size_t(551)*976+975)*4]==0,"SMAA preserves black margins");
        Require(pixels[(size_t(80)*976+500)*4]>20,"SMAA offset video present");
    }
    r.reset();DestroyWindow(window);Select(Mode::Disabled);
    std::puts("PASS: SMAA High/Ultra and CMAA2 both orders, range/matrix, synthetic edges, flat/no-stale output, OSD, HDR bypass and placement.");
}
static void Regression() {
    using namespace llcv::spatial;
    for(auto m:modes) {
        Require(!Eligible(m,true,640,360,{0,0,1280,720}),"HDR bypass");
        Require(!Eligible(m,false,640,360,{0,0,640,360}),"native bypass");
        Require(!Eligible(m,false,640,360,{0,0,320,180}),"downscale bypass");
        Require(!Eligible(m,false,640,360,{0,0,1920,1080}),"outside common 2x limit bypass");
    }
    HWND hwnd=Window(960,540);
    const UINT w=640,h=360;
    // Odd destination dimensions exercise edge dispatch/viewport clipping too.
    std::vector<BYTE> p(size_t(w)*h*3/2,128);std::fill(p.begin(),p.begin()+size_t(w)*h,126);
    for(auto range:{llcv::video_color::Range::Limited,llcv::video_color::Range::Full})
    for(auto matrix:{llcv::video_color::Matrix::Bt709,llcv::video_color::Matrix::Bt601}) {
        // Non-neutral chroma detects an accidental matrix/range override.
        for(size_t i=size_t(w)*h;i<p.size();i+=2){p[i]=96;p[i+1]=158;}
        std::vector<BYTE> baseline;
        for(auto m:modes) {
            Select(m);DirectD3D11Renderer r;llcv::video_color::Configuration color{};color.range=range;color.matrix=matrix;
            Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::Nv12,false,color),"SDR renderer initialize");
            Require(r.spatialPipeline.ActiveMode()==m,"requested kernel compiled/active, no silent fallback");
            Render(r,p,w);auto pixels=Read(r);
            if(m==Mode::Disabled) baseline=pixels;
            int maxError=0;for(size_t i=0;i<pixels.size();i++) if(i%4!=3)
                maxError=std::max(maxError,std::abs(int(pixels[i])-int(baseline[i])));
            Require(maxError<=2,"flat color/range preservation within 2 code values");
            for(size_t i=3;i<pixels.size();i+=4)Require(pixels[i]==255,"opaque video alpha");
            g_audioOsdVisible=true;Check(r.drawOverlayQuads(),"unchanged production OSD");
            Require(Read(r)!=pixels,"OSD composited after scaling");g_audioOsdVisible=false;
            Render(r,p,w);Require(Read(r)==pixels,"next video frame removes old OSD");
            // Settings values and original uploaded NV12 memory are not modified by a draw.
            Require(p[0]==126 && p[size_t(w)*h]==96,"original recording/screenshot source untouched");
        }
    }
    // YUY2 uses the same production color conversion, not NIS's fixed NV12 matrix.
    std::vector<BYTE> yuy(size_t(w)*h*2,128);
    for(size_t i=0;i<yuy.size();i+=2) yuy[i]=126;
    for(auto m:modes) {
        Select(m);DirectD3D11Renderer r;Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::Yuy2),"YUY2 initialize");
        Require(r.spatialPipeline.ActiveMode()==m,"YUY2 candidate active");Render(r,yuy,w*2);
        auto pixels=Read(r);Require(pixels[pixels.size()/2]>110 && pixels[pixels.size()/2]<145,"YUY2 level preserved");
        // Native-size recreation must release all experiment textures and preserve exact baseline.
        SetWindowPos(hwnd,nullptr,0,0,w,h,SWP_NOACTIVATE|SWP_NOZORDER);
        Check(r.resizeOutput(hwnd),"native resize");
        Require(!r.spatialPipeline.Enabled(),"native size bypass after resize");Render(r,yuy,w*2);
        SetWindowPos(hwnd,nullptr,0,0,960,540,SWP_NOACTIVATE|SWP_NOZORDER);
    }
    DestroyWindow(hwnd);Select(Mode::Disabled);
    // Fullscreen placement is exercised without changing any desktop/display mode.
    hwnd=Window(1600,900);g_settings.videoPreset=VideoPreset::R1280x720;
    for(auto m:modes) {
        Select(m);DirectD3D11Renderer r;Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::Nv12),"placement init");
        r.fullscreenDisplaySize=true;
        Check(r.configureVideoOutput(w,h),"centered fullscreen configure");
        Require(r.spatialPipeline.ActiveMode()==m,"centered fullscreen active mode");
        Render(r,p,w);auto pixels=Read(r);
        const size_t middle=(size_t(450)*1600+800)*4;
        Require(pixels[0]==0 && pixels[1]==0 && pixels[2]==0 && pixels[middle]>20,"centered image with black margins");
        g_fullscreenSourceSize=true;Check(r.configureVideoOutput(w,h),"F5 source-size override");
        Require(!r.spatialPipeline.Enabled(),"F5 bypasses scaler");
        g_fullscreenSourceSize=false;Check(r.configureVideoOutput(w,h),"restore scaled placement");
        Require(r.spatialPipeline.ActiveMode()==m,"restore spatial scaler");
        Render(r,p,w);Require(Read(r)==pixels,"placement restore exact pixels");
    }
    DestroyWindow(hwnd);
    hwnd=Window(961,541);g_settings.videoPreset=VideoPreset::R1920x1080;
    for(auto m:modes) {
        Select(m);DirectD3D11Renderer r;Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::Nv12),"odd viewport init");
        Require(r.spatialPipeline.ActiveMode()==m,"odd viewport active");Render(r,p,w);auto pixels=Read(r);
        Require(pixels[(size_t(270)*961+480)*4]>20,"odd viewport dispatch covers center");
        std::vector<BYTE> black=p;std::fill(black.begin(),black.begin()+size_t(w)*h,16);
        std::fill(black.begin()+size_t(w)*h,black.end(),128);Render(r,black,w);pixels=Read(r);
        for(size_t i=0;i<pixels.size();i++)if(i%4!=3)Require(pixels[i]<=1,"black input no stale/unwritten edge pixels");
    }
    DestroyWindow(hwnd);
    // Symmetric data catches missing/duplicated taps and phase shifts in the
    // new analytic filters. This is not a quality assertion for learned models.
    hwnd=Window(960,540);
    std::vector<BYTE> symmetric(size_t(w)*h*3/2,128);
    for(UINT y=0;y<h;y++)for(UINT x=0;x<w;x++) {
        const UINT dx=std::min(x,w-1-x),dy=std::min(y,h-1-y);
        symmetric[size_t(y)*w+x]=BYTE(16+((dx/7+dy/11)%2)*170);
    }
    for(auto m:{Mode::Jinc,Mode::Lanczos}) {
        Select(m);DirectD3D11Renderer r;Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::Nv12),"analytic symmetry initialize");
        Render(r,symmetric,w);auto pixels=Read(r);int difference=0;
        for(UINT y=0;y<540;y++)for(UINT x=0;x<960;x++) {
            const auto value=pixels[(size_t(y)*960+x)*4];
            difference=std::max(difference,std::abs(int(value)-int(pixels[(size_t(y)*960+959-x)*4])));
            difference=std::max(difference,std::abs(int(value)-int(pixels[(size_t(539-y)*960+x)*4])));
        }
        Require(difference<=1,"windowed-sinc reflection symmetry within rounding");
    }
    DestroyWindow(hwnd);
    // Input dimensions not divisible by compute blocks exercise partial source
    // tiles in RAVU and all six FSRCNNX passes, not merely odd output viewports.
    // Exact 1.5x aspect ratio avoids testing intentional letterbox pixels.
    hwnd=Window(963,543);
    const UINT partialW=642,partialH=362;
    std::vector<BYTE> partial(size_t(partialW)*partialH*3/2,128);
    std::fill(partial.begin(),partial.begin()+size_t(partialW)*partialH,126);
    for(auto m:{Mode::Ravu,Mode::Fsrcnnx}) {
        Select(m);DirectD3D11Renderer r;Check(r.initialize(hwnd,partialW,partialH,60,VideoPixelFormat::Nv12),"partial source tiles initialize");
        Require(r.spatialPipeline.ActiveMode()==m,"partial tile mode active");
        Render(r,partial,partialW);auto pixels=Read(r);
        for(size_t i=0;i<pixels.size();i+=4)Require(pixels[i]>=125 && pixels[i]<=131,"partial source tile output has no holes");
    }
    DestroyWindow(hwnd);Select(Mode::Disabled);
    std::puts("PASS: all kernels, NV12/YUY2, BT.601/709 Limited/Full, OSD redraw, centered/odd viewport/F5/native/downscale/HDR policy, analytic symmetry and partial compute tiles.");
    SmaaRegression();
}
struct Timer {
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint,start,stop;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> completionPixel;
    explicit Timer(ID3D11Device* d) {
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT,0};Check(d->CreateQuery(&q,&disjoint),"disjoint query");
        q.Query=D3D11_QUERY_TIMESTAMP;Check(d->CreateQuery(&q,&start),"start timestamp");Check(d->CreateQuery(&q,&stop),"stop timestamp");
        D3D11_TEXTURE2D_DESC pixel{};pixel.Width=pixel.Height=pixel.MipLevels=pixel.ArraySize=pixel.SampleDesc.Count=1;
        pixel.Format=DXGI_FORMAT_B8G8R8A8_UNORM;pixel.Usage=D3D11_USAGE_STAGING;pixel.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Check(d->CreateTexture2D(&pixel,nullptr,&completionPixel),"output-dependent completion pixel");
    }
    void Begin(ID3D11DeviceContext* c){c->Begin(disjoint.Get());c->End(start.Get());}
    double End(ID3D11DeviceContext* c,ID3D11Texture2D* output) {
        // Bare VP timestamps can omit work on the video engine. Force an output
        // dependency, and independently verify completion with one-pixel Map.
        const D3D11_BOX box{0,0,0,1,1,1};
        c->CopySubresourceRegion(completionPixel.Get(),0,0,0,0,output,0,&box);
        c->End(stop.Get());c->End(disjoint.Get());c->Flush();
        const ULONGLONG deadline=GetTickCount64()+5000;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};HRESULT hr;
        while((hr=c->GetData(disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH))==S_FALSE) {
            Require(GetTickCount64()<deadline,"bounded GPU wait");SwitchToThread();
        }
        Check(hr,"timestamp disjoint read");UINT64 a=0,b=0;
        Require(c->GetData(start.Get(),&a,sizeof(a),0)==S_OK && c->GetData(stop.Get(),&b,sizeof(b),0)==S_OK,"timestamps ready");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(c->Map(completionPixel.Get(),0,D3D11_MAP_READ,0,&mapped),"actual output ready");c->Unmap(completionPixel.Get(),0);
        Require(!d.Disjoint && d.Frequency && b>=a,"valid GPU clock");return double(b-a)*1000/d.Frequency;
    }
};
static double Percentile(std::vector<double> v,double p){std::sort(v.begin(),v.end());return v[size_t((v.size()-1)*p)];}
static void Benchmark(const std::filesystem::path& output,const std::string& filter={}) {
    std::filesystem::create_directories(output);
    std::ofstream csv(output/"timings.csv");csv<<"width,height,round,method,samples,gpu_dependency_mean_ms,gpu_dependency_p95_ms,cpu_submit_mean_ms,completion_mean_ms,completion_p95_ms,synthetic_psnr_db\n";
    const UINT w=1920,h=1080;const auto input=Pattern(w,h);
    for(auto size:{SIZE{2560,1440},SIZE{3840,2160}}) {
        HWND hwnd=Window(size.cx,size.cy);const auto reference=Reference(size.cx,size.cy);
        SaveBmp(output/(std::to_string(size.cx)+"-reference.bmp"),size.cx,size.cy,reference);
        // Rotate order across runs. Warm shader compilation/allocation outside all timings.
        for(int round=0;round<3;round++) for(size_t j=0;j<modes.size()+2;j++) {
            const size_t index=(j+size_t(round)*2)%(modes.size()+2);
            const bool sharp=index==modes.size(),vsr=index==modes.size()+1;const Mode m=index<modes.size()?modes[index]:Mode::Disabled;
            const char* name=vsr?"vsr-requested":sharp?"vp-sharp":llcv::spatial::Name(m);
            if(!filter.empty() && filter!=name)continue;
            Select(m);g_settings.scalingMode=sharp?ScalingMode::Sharp:ScalingMode::Smooth;
            g_vsrMode=vsr?llcv::vsr::Mode::On:llcv::vsr::Mode::Disabled;
            DirectD3D11Renderer r;Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::Nv12),"benchmark init");
            Require(r.spatialPipeline.ActiveMode()==m,"benchmark mode active");
            if(vsr && r.vsrState!=llcv::vsr::State::Requested) {std::puts("SKIP: VSR request unsupported");continue;}
            for(int warm=0;warm<30;warm++){Render(r,input,w);r.context->Flush();}
            Read(r);Timer timer(r.device);std::vector<double> gpu,cpu,completion;
            for(int f=0;f<100;f++) {
                // Upload is common and excluded from GPU filter timings, included in completion.
                const auto begin=std::chrono::steady_clock::now();r.upload(input.data(),w);
                D3D11_VIDEO_PROCESSOR_STREAM s{};s.Enable=TRUE;s.pInputSurface=r.inputViews[r.activeUploadSurface];
                timer.Begin(r.context);const auto submit=std::chrono::steady_clock::now();
                Check(r.processVideo(s),"timed render");const auto submitted=std::chrono::steady_clock::now();
                gpu.push_back(timer.End(r.context,r.backBuffer));const auto end=std::chrono::steady_clock::now();
                cpu.push_back(std::chrono::duration<double,std::milli>(submitted-submit).count());
                completion.push_back(std::chrono::duration<double,std::milli>(end-begin).count());
                std::this_thread::sleep_until(begin+std::chrono::microseconds(16667));
            }
            auto pixels=Read(r);double mse=0;for(size_t i=0;i<pixels.size();i++)if(i%4!=3){double e=int(pixels[i])-int(reference[i]);mse+=e*e;}
            mse/=double(pixels.size()/4*3);const double psnr=10*std::log10(255*255/std::max(mse,1e-12));
            if(round==0)SaveBmp(output/(std::to_string(size.cx)+"-"+name+".bmp"),size.cx,size.cy,pixels);
            auto mean=[](const auto& v){return std::accumulate(v.begin(),v.end(),0.0)/v.size();};
            csv<<size.cx<<','<<size.cy<<','<<round<<','<<name<<','<<gpu.size()<<','<<mean(gpu)<<','<<Percentile(gpu,.95)<<','<<mean(cpu)<<','<<mean(completion)<<','<<Percentile(completion,.95)<<','<<psnr<<'\n';csv.flush();
            std::printf("%dx%d round=%d %-13s GPU mean=%.4f p95=%.4f ms CPU=%.4f completion=%.4f ms PSNR=%.2f dB\n",
                size.cx,size.cy,round,name,mean(gpu),Percentile(gpu,.95),mean(cpu),mean(completion),psnr);
        }DestroyWindow(hwnd);
    }
    std::puts("Synthetic scene only; completion is serialized GPU work, NOT display latency. VSR request does not certify neural activation.");
}
static void AaBenchmark(const std::filesystem::path& file,const std::filesystem::path& output) {
    const UINT w=1920,h=1080,ow=2560,oh=1440;std::vector<BYTE> input(size_t(w)*h*3/2);
    std::ifstream in(file,std::ios::binary);in.read(reinterpret_cast<char*>(input.data()),std::streamsize(input.size()));
    Require(in.gcount()==std::streamsize(input.size()) && in.peek()==std::char_traits<char>::eof(),"AA benchmark exact stored NV12 input");
    Require(!std::filesystem::exists(output),"AA benchmark preserves earlier runs");
    std::filesystem::create_directories(output);
    std::ofstream csv(output/"timings.csv");
    csv<<"round,method,samples,gpu_mean_ms,gpu_median_ms,gpu_p95_ms,cpu_submit_mean_ms,upload_to_completion_mean_ms,completion_p95_ms\n";
    const wchar_t* presets[]={L"baseline",L"nis15",L"cmaa2-nis15"};
    const char* names[]={"baseline","nis15","cmaa2-nis15"};
    const auto expectedAa=llcv::spatial::AaMode::Cmaa2BeforeNis;
    for(unsigned round=0;round<3;++round)for(unsigned slot=0;slot<3;++slot) {
        const unsigned index=(round+slot)%3;
        SetEnvironmentVariableW(L"LLCV_SPATIAL_UPSCALER",presets[index]);
        HWND window=Window(ow,oh);{
            DirectD3D11Renderer r;Check(r.initialize(window,w,h,60,VideoPixelFormat::Nv12),"AA benchmark init");
            Require(r.spatialPipeline.ActiveMode()==(index?Mode::Nis:Mode::Disabled),"AA benchmark scaler activated");
            Require(r.spatialPipeline.ActiveAaMode()==(index==2?expectedAa:llcv::spatial::AaMode::Disabled),"AA benchmark AA activated");
            if(index)Require(llcv::spatial::EnvironmentNisSharpness()==.15f,"AA benchmark NIS 0.15");
            for(unsigned warm=0;warm<30;++warm){Render(r,input,w);r.context->Flush();}
            Read(r);Timer timer(r.device);std::vector<double> gpu,cpu,completion;
            for(unsigned frame=0;frame<180;++frame) {
                const auto begin=std::chrono::steady_clock::now();r.upload(input.data(),w);
                D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=r.inputViews[r.activeUploadSurface];
                timer.Begin(r.context);const auto submit=std::chrono::steady_clock::now();
                Check(r.processVideo(stream),"AA timed render");const auto submitted=std::chrono::steady_clock::now();
                gpu.push_back(timer.End(r.context,r.backBuffer));const auto end=std::chrono::steady_clock::now();
                cpu.push_back(std::chrono::duration<double,std::milli>(submitted-submit).count());
                completion.push_back(std::chrono::duration<double,std::milli>(end-begin).count());
                std::this_thread::sleep_until(begin+std::chrono::microseconds(16667));
            }
            auto mean=[](const auto& values){return std::accumulate(values.begin(),values.end(),0.0)/values.size();};
            csv<<round<<','<<names[index]<<','<<gpu.size()<<','<<mean(gpu)<<','<<Percentile(gpu,.5)<<','<<Percentile(gpu,.95)<<','<<mean(cpu)<<','<<mean(completion)<<','<<Percentile(completion,.95)<<'\n';csv.flush();
            Require(bool(csv),"AA benchmark write");
            std::printf("AA GPU round=%u method=%s mean=%.4f median=%.4f p95=%.4f ms CPU=%.4f completion=%.4f\n",
                round,names[index],mean(gpu),Percentile(gpu,.5),Percentile(gpu,.95),mean(cpu),mean(completion));
            if(round==0)SaveBmp(output/(std::string(names[index])+".bmp"),ow,oh,Read(r));
        }DestroyWindow(window);
    }
    Select(Mode::Disabled);
    std::puts("Same stored frame, 1080p to QHD. GPU/output-dependent completion, NOT optical or HDMI latency.");
}
static void CapturedComparison(const std::filesystem::path& file,const std::filesystem::path& output,bool nisSweep=false,bool smaaSweep=false,bool extendedAA=false) {
    const UINT w=1920,h=1080;std::vector<BYTE> input(size_t(w)*h*3/2);
    std::ifstream in(file,std::ios::binary);in.read(reinterpret_cast<char*>(input.data()),std::streamsize(input.size()));
    Require(in.gcount()==std::streamsize(input.size()) && in.peek()==std::char_traits<char>::eof(),"exact 1080p NV12 capture fixture");
    std::filesystem::create_directories(output);
    for(const auto size:{SIZE{1920,1080},SIZE{2560,1440}}) {
        HWND window=Window(size.cx,size.cy);
        for(auto m:modes) {
            if(size.cx==1920 && m!=Mode::Disabled)continue;
            if((nisSweep || smaaSweep) && m!=Mode::Disabled && m!=Mode::Nis)continue;
            Select(m);DirectD3D11Renderer r;
            Check(r.initialize(window,w,h,60,VideoPixelFormat::Nv12),"captured fixture initialize");
            if(size.cx!=1920)Require(r.spatialPipeline.ActiveMode()==m,"captured comparison mode active");
            Render(r,input,w);
            if(smaaSweep && m==Mode::Nis) {
                using llcv::spatial::AaMode;
                std::vector<BYTE> baseline,before;
                const std::vector<AaMode> orders=extendedAA?
                    std::vector<AaMode>{AaMode::Disabled,AaMode::SmaaAfterNis,AaMode::SmaaUltraAfterNis,AaMode::Cmaa2AfterNis,AaMode::SmaaUltraBeforeNis,AaMode::Cmaa2BeforeNis}:
                    std::vector<AaMode>{AaMode::Disabled,AaMode::SmaaBeforeNis,AaMode::SmaaAfterNis};
                std::vector<std::vector<BYTE>> rendered;
                for(const auto order:orders) {
                    Check(r.spatialPipeline.Configure(r.device,r.videoDevice,r.enumerator,m,false,w,h,
                        RECT{0,0,size.cx,size.cy},r.adapterVendor,.15f,order),"SMAA comparison configure");
                    Require(r.spatialPipeline.ActiveMode()==Mode::Nis && r.spatialPipeline.ActiveAaMode()==order,"SMAA order active");
                    Render(r,input,w);const auto pixels=Read(r);
                    for(size_t i=3;i<pixels.size();i+=4)Require(pixels[i]==255,"SMAA comparison opaque output");
                    if(order==AaMode::Disabled)baseline=pixels;
                    else Require(pixels!=baseline,"SMAA actually changes captured edges");
                    if(order==AaMode::SmaaBeforeNis)before=pixels;
                    if(order==AaMode::SmaaAfterNis)Require(pixels!=before,"SMAA ordering gives distinct images");
                    for(const auto& previous:rendered)Require(pixels!=previous,"AA variants must be distinct");
                    rendered.push_back(pixels);
                    Render(r,input,w);const auto repeated=Read(r);
                    if(repeated!=pixels) {
                        int maximum=0;size_t changed=0;
                        for(size_t i=0;i<pixels.size();++i) {const int delta=std::abs(int(pixels[i])-int(repeated[i]));maximum=std::max(maximum,delta);changed+=delta!=0;}
                        std::fprintf(stderr,"AA capture repeat mode=%d maximum=%d changed=%zu\n",int(order),maximum,changed);
                        Require(llcv::spatial::AaIsCmaa2(order) && maximum<=1,"AA capture repeat differs beyond floating-point rounding");
                    }
                    const char* name=order==AaMode::Disabled?"nis15":order==AaMode::SmaaBeforeNis?"smaa-before":
                        order==AaMode::SmaaUltraAfterNis?"smaa-ultra":order==AaMode::Cmaa2AfterNis?"cmaa2":
                        order==AaMode::SmaaUltraBeforeNis?"smaa-ultra-before":order==AaMode::Cmaa2BeforeNis?"cmaa2-before":"smaa-after";
                    SaveBmp(output/(std::string("2560-")+name+".bmp"),size.cx,size.cy,pixels);
                }
                continue;
            }
            if(nisSweep && m==Mode::Nis) {
                const auto original=Read(r);
                std::vector<BYTE> previous;
                for(const auto strength:{0,15,30,50}) {
                    Check(r.spatialPipeline.Configure(r.device,r.videoDevice,r.enumerator,m,false,w,h,
                        RECT{0,0,size.cx,size.cy},r.adapterVendor,float(strength)/100),"NIS sharpness configure");
                    Require(r.spatialPipeline.ActiveMode()==Mode::Nis,"NIS sharpness mode active");
                    Render(r,input,w);const auto pixels=Read(r);
                    for(size_t i=3;i<pixels.size();i+=4)Require(pixels[i]==255,"NIS sharpness opaque output");
                    if(!previous.empty())Require(previous!=pixels,"NIS sharpness actually changes this fixture");
                    if(strength==50)Require(original==pixels,"NIS 0.5 retains original default output");
                    SaveBmp(output/("2560-nis-"+std::to_string(strength)+".bmp"),size.cx,size.cy,pixels);
                    previous=pixels;
                }
                for(float invalid:{-0.1f,1.1f,std::numeric_limits<float>::quiet_NaN()}) {
                    Require(r.spatialPipeline.Configure(r.device,r.videoDevice,r.enumerator,m,false,w,h,
                        RECT{0,0,size.cx,size.cy},r.adapterVendor,invalid)==E_INVALIDARG,"invalid NIS sharpness rejected");
                    Require(!r.spatialPipeline.Enabled(),"invalid NIS sharpness disables pipeline");
                }
                continue;
            }
            SaveBmp(output/(std::to_string(size.cx)+"-"+llcv::spatial::Name(m)+".bmp"),size.cx,size.cy,Read(r));
        }DestroyWindow(window);
    }
    std::puts(extendedAA?"PASS: NIS 0.15, SMAA High, SMAA Ultra color, CMAA2; distinct opaque repeatable outputs."
        :smaaSweep?"PASS: same captured SDR frame through NIS 0.15, SMAA before NIS and SMAA after NIS; distinct opaque outputs."
        :nisSweep?"PASS: NIS 0/0.15/0.3/0.5; distinct outputs, opaque alpha, unchanged default, invalid strengths rejected."
        :"PASS: same captured SDR frame rendered through baseline and all kernels; no quality score without ground truth.");
}
#ifdef LLCV_RECORDING
static void RecordingIntegration(std::filesystem::path output) {
    Check(MFStartup(MF_VERSION),"recording MF startup");
    output/=std::to_wstring(GetCurrentProcessId());std::filesystem::create_directories(output);
    const UINT w=640,h=360;const auto pixels=Pattern(w,h);HWND window=Window(960,540);
    llcv::video::VideoSampleFormat video{};video.valid=true;video.format=VideoPixelFormat::Nv12;
    video.width=w;video.height=h;video.stride=w;video.fps=60;video.bytes=DWORD(pixels.size());
    llcv::capture_audio::Format audio{};audio.encoding=llcv::capture_audio::Encoding::IntegerPcm;
    audio.path=llcv::capture_audio::Path::Direct16BitStereo;audio.containerBits=audio.validBits=16;
    audio.channels=2;audio.blockAlign=4;
    llcv::recording::Options options;options.encoder=L"cpu";options.rateControl=llcv::recording::RateControl::Bitrate;
    for(auto mode:modes) {
        Select(mode);DirectD3D11Renderer r;Check(r.initialize(window,w,h,60,VideoPixelFormat::Nv12),"recording renderer");
        Require(r.spatialPipeline.ActiveMode()==mode,"recording uses requested scaler");
        const auto path=output/(std::string(llcv::spatial::Name(mode))+".mp4");
        llcv::recording::Recorder recorder;recorder.Configure(video,audio,{},false,options);
        Require(recorder.Start(path.wstring()),"recording starts");
        const auto deadline=GetTickCount64()+15000;
        while(recorder.Busy() && !recorder.Recording() && GetTickCount64()<deadline)Sleep(10);
        Require(recorder.Recording(),"bounded recording startup");
        Microsoft::WRL::ComPtr<IMemAllocator> allocator;
        Check(CoCreateInstance(CLSID_MemoryAllocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&allocator)),"allocator");
        ALLOCATOR_PROPERTIES requested{2,LONG(pixels.size()),1,0},actual{};
        Check(allocator->SetProperties(&requested,&actual),"sample allocation");Check(allocator->Commit(),"sample commit");
        const auto start=std::chrono::steady_clock::now();
        for(int frame=0;frame<60;frame++) {
            std::this_thread::sleep_until(start+std::chrono::microseconds(frame*1000000LL/60));
            for(bool isVideo:{true,false}) {
                Microsoft::WRL::ComPtr<IMediaSample> sample;Check(allocator->GetBuffer(&sample,nullptr,nullptr,0),"get sample");
                const LONG bytes=isVideo?LONG(pixels.size()):800*4;
                Check(sample->SetActualDataLength(bytes),"sample length");
                LONGLONG begin=frame*10000000LL/60,end=(frame+1)*10000000LL/60;
                Check(sample->SetTime(&begin,&end),"sample timestamp");BYTE* data=nullptr;Check(sample->GetPointer(&data),"sample data");
                if(isVideo)std::memcpy(data,pixels.data(),pixels.size());
                else for(int i=0;i<800;i++) {
                    const auto tone=short(std::lround(8000*std::sin(6.283185307*440*(frame*800+i)/48000)));
                    std::memcpy(data+i*4,&tone,2);std::memcpy(data+i*4+2,&tone,2);
                }
                recorder.Push(isVideo,sample.Get());
            }
            Render(r,pixels,w);
        }
        recorder.Stop();recorder.Shutdown();Check(recorder.Result(),"recording finalizes");Check(allocator->Decommit(),"sample decommit");
        Microsoft::WRL::ComPtr<IMFSourceReader> reader;
        Check(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader),"recorded file opens");
        const DWORD stream=static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        Check(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE),"deselect streams");Check(reader->SetStreamSelection(stream,TRUE),"select video");
        Microsoft::WRL::ComPtr<IMFMediaType> type;Check(MFCreateMediaType(&type),"decoded type");
        Check(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video),"video type");Check(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12),"decoded NV12");
        Check(reader->SetCurrentMediaType(stream,nullptr,type.Get()),"set decoded type");
        unsigned frames=0;LONGLONG previous=-1;
        for(;;) {
            DWORD flags=0;LONGLONG time=0;Microsoft::WRL::ComPtr<IMFSample> sample;
            Check(reader->ReadSample(stream,0,nullptr,&flags,&time,&sample),"decode recording");
            if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
            if(!sample)continue;
            Require(time>previous,"recording timestamps strictly increase");previous=time;frames++;
        }
        Require(frames==60,"all original recording frames saved, no scaler frame loss");
        std::printf("RECORDING %s frames=%u path=%ls\n",llcv::spatial::Name(mode),frames,path.c_str());
    }
    DestroyWindow(window);Select(Mode::Disabled);Check(MFShutdown(),"MF shutdown");
}
#endif
int main(int argc,char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
    g_suppressSettingsSave=true;g_settings.pixelPerfect=false;g_settings.displayMatchesCapture=false;
    g_settings.videoPreset=VideoPreset::R1920x1080;g_settings.scalingMode=ScalingMode::Smooth;
    g_settings.presentationMode=PresentationMode::AllowTearing;g_vsrMode=llcv::vsr::Mode::Disabled;
    g_audioOsdVisible=false;g_volumeHudUntilMs=0;
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;wc.lpszClassName=L"LLCV_SPATIAL_TEST";
    Require(RegisterClassW(&wc)!=0,"register class");
    if(argc>=3 && std::string(argv[1])=="--benchmark") Benchmark(std::filesystem::path(argv[2]),argc>=4?argv[3]:"");
    else if(argc>=4 && std::string(argv[1])=="--aa-benchmark") AaBenchmark(argv[2],argv[3]);
    else if(argc>=4 && std::string(argv[1])=="--capture") CapturedComparison(argv[2],argv[3]);
    else if(argc>=4 && std::string(argv[1])=="--nis-capture") CapturedComparison(argv[2],argv[3],true);
    else if(argc>=4 && std::string(argv[1])=="--smaa-capture") CapturedComparison(argv[2],argv[3],false,true);
    else if(argc>=4 && std::string(argv[1])=="--aa-capture") CapturedComparison(argv[2],argv[3],false,true,true);
    else if(argc>=2 && std::string(argv[1])=="--aa-regression") SmaaRegression();
#ifdef LLCV_RECORDING
    else if(argc>=3 && std::string(argv[1])=="--recording") RecordingIntegration(argv[2]);
#endif
    else Regression();
    SetEnvironmentVariableW(L"LLCV_SPATIAL_UPSCALER",nullptr);UnregisterClassW(wc.lpszClassName,wc.hInstance);CoUninitialize();return 0;
}
