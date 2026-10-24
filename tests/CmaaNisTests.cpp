// Production renderer and embedded shaders only. No capture-device or profile access.
#include "../src/main.cpp"
#undef fwprintf
#include "../src/audio/AsioOutput.cpp"
#include <filesystem>
#include <fstream>
#include <numeric>
#include <cmath>
#include <limits>
#include <mfreadwrite.h>
#include "../third_party/upscaling/nis/NIS_Config.h"
static void Require(bool value,const char* label) {
    if(!value) { std::fprintf(stderr,"FAIL: %s\n",label);std::exit(1); }
}
static void Check(HRESULT hr,const char* label) {
    if(FAILED(hr)) {std::fprintf(stderr,"FAIL: %s 0x%08X\n",label,unsigned(hr));std::exit(1);}
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
static HWND Window(UINT w,UINT h) {
    HWND hwnd=CreateWindowW(L"LLCV_SPATIAL_TEST",L"LLCV spatial comparison",WS_POPUP,0,0,int(w),int(h),nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Require(hwnd!=nullptr,"hidden window");return hwnd;
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

namespace llcv::upscaling {
// Inspect the exact embedded NIS stage without changing production state/layout.
struct PipelineTestAccess {
    static void DisableSharpen(Pipeline& p,ID3D11Device* d) {
        NISConfig config{};
        Require(NVScalerUpdateConfig(config,.15f,0,0,p.width_,p.height_,p.width_,p.height_,
            0,0,p.scaledWidth_,p.scaledHeight_,p.scaledWidth_,p.scaledHeight_,NISHDRMode::None),"NIS diagnostic config");
        config.kSharpStrengthMin=0;config.kSharpStrengthScale=0;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(config);desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{&config,0,0};
        Check(d->CreateBuffer(&desc,&initial,p.nisConstants_.ReleaseAndGetAddressOf()),"NIS zero-sharpen diagnostic");
    }
    static ID3D11ShaderResourceView* NativeInput(Pipeline& p){return p.inputView_.Get();}
    static ID3D11ShaderResourceView* CmaaOnly(Pipeline& p,ID3D11DeviceContext* c){return p.cmaa_.Draw(c,p.inputView_.Get());}
    static void NisOnly(Pipeline& p,ID3D11DeviceContext* c,ID3D11RenderTargetView* target) {
        ID3D11ShaderResourceView* inputs[]={p.inputView_.Get(),p.coefficients_[0].Get(),p.coefficients_[1].Get()};
        c->CSSetShader(p.compute_.Get(),nullptr,0);c->CSSetShaderResources(0,3,inputs);
        c->CSSetSamplers(0,1,p.sampler_.GetAddressOf());c->CSSetConstantBuffers(0,1,p.nisConstants_.GetAddressOf());
        c->CSSetUnorderedAccessViews(0,1,p.intermediateUav_.GetAddressOf(),nullptr);
        c->Dispatch((p.scaledWidth_+31)/32,(p.scaledHeight_+23)/24,1);
        ID3D11ShaderResourceView* empty[3]{};ID3D11UnorderedAccessView* uav=nullptr;
        c->CSSetShaderResources(0,3,empty);c->CSSetUnorderedAccessViews(0,1,&uav,nullptr);c->CSSetShader(nullptr,nullptr,0);
        c->RSSetState(p.rasterizer_.Get());
        D3D11_VIEWPORT viewport{float(p.destination_.left),float(p.destination_.top),float(p.scaledWidth_),float(p.scaledHeight_),0,1};
        c->RSSetViewports(1,&viewport);c->OMSetRenderTargets(1,&target,nullptr);
        c->VSSetShader(p.cmaa_.vertex_.Get(),nullptr,0);c->PSSetShader(p.pixel_.Get(),nullptr,0);
        c->PSSetConstantBuffers(0,1,p.constants_.GetAddressOf());c->PSSetShaderResources(0,1,p.intermediateView_.GetAddressOf());c->Draw(3,0);
        c->PSSetShaderResources(0,1,empty);c->OMSetRenderTargets(0,nullptr,nullptr);c->RSSetState(nullptr);
    }
};
}
static std::vector<BYTE> ReadSdrView(DirectD3D11Renderer& r,ID3D11ShaderResourceView* view) {
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;view->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;Check(resource.As(&texture),"SDR stage texture");
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
    Require(d.Width==1920 && d.Height==1080,"SDR native stage dimensions");
    const auto format=d.Format;Require(format==DXGI_FORMAT_B8G8R8A8_UNORM || format==DXGI_FORMAT_R16G16B16A16_FLOAT,"native stage format");
    d.BindFlags=d.MiscFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;Check(r.device->CreateTexture2D(&d,nullptr,&staging),"SDR stage staging");
    r.context->CopyResource(staging.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE map{};
    Check(r.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"SDR stage readback");
    std::vector<BYTE> out(size_t(d.Width)*d.Height*4);
    for(UINT y=0;y<d.Height;y++)for(UINT x=0;x<d.Width;x++) {
        const auto* row=static_cast<const BYTE*>(map.pData)+size_t(y)*map.RowPitch;
        auto* pixel=out.data()+(size_t(y)*d.Width+x)*4;
        if(format==DXGI_FORMAT_B8G8R8A8_UNORM)std::memcpy(pixel,row+x*4,4);
        else {
            for(int c=0;c<3;c++) {
                uint16_t half=0;std::memcpy(&half,row+x*8+c*2,2);
                const unsigned exponent=(half>>10)&31,mantissa=half&1023;
                Require(!(half&0x8000) && exponent!=31,"finite nonnegative encoded SDR stage");
                const double value=exponent?std::ldexp(1.0+double(mantissa)/1024,int(exponent)-15):std::ldexp(double(mantissa),-24);
                pixel[2-c]=BYTE(std::lround(std::clamp(value,0.0,1.0)*255));
            }
            pixel[3]=255;
        }
    }
    r.context->Unmap(staging.Get(),0);return out;
}
static void SdrTextComparison(const std::filesystem::path& inputPath,const std::filesystem::path& output) {
    Require(llcv::test_profile::Enabled(),"SDR diagnosis must use isolated profile");
    Require(!std::filesystem::exists(output),"new SDR comparison output");std::filesystem::create_directories(output);
    std::ifstream source(inputPath,std::ios::binary);Require(bool(source),"saved SDR source opens");
    const std::vector<BYTE> input{std::istreambuf_iterator<char>(source),std::istreambuf_iterator<char>()};
    Require(input.size()==size_t(1920)*1080*3/2,"FHD NV12 snapshot size");
    auto save=[&](const std::string& name,const std::vector<BYTE>& pixels,bool native=false) {
        const UINT width=native?1920:2560,height=native?1080:1440;
        SaveBmp(output/(name+".bmp"),width,height,pixels);
        const int scale=native?4:3,cropWidth=native?165:220,cropHeight=native?68:90;
        const int left=native?885:1180,top=native?248:330;
        std::vector<BYTE> roi(size_t(cropWidth)*cropHeight*4);
        for(int y=0;y<cropHeight;y++)
            std::memcpy(roi.data()+size_t(y)*cropWidth*4,pixels.data()+(size_t(top+y)*width+left)*4,size_t(cropWidth)*4);
        SaveBmp(output/(name+"-roi.bmp"),cropWidth,cropHeight,roi);
        std::vector<BYTE> crop(size_t(cropWidth)*cropHeight*scale*scale*4);
        for(int y=0;y<cropHeight*scale;y++)for(int x=0;x<cropWidth*scale;x++)
            std::memcpy(crop.data()+(size_t(y)*cropWidth*scale+x)*4,pixels.data()+(size_t(top+y/scale)*width+left+x/scale)*4,4);
        SaveBmp(output/(name+"-text.bmp"),cropWidth*scale,cropHeight*scale,crop);
    };
    std::ofstream csv(output/"roi-differences.csv");
    csv<<"comparison,max_channel_difference,mean_absolute_channel_difference,pixels_delta_over_16,bright_to_dark,dark_to_bright\n";
    auto compare=[&](const char* label,const std::vector<BYTE>& a,const std::vector<BYTE>& b,bool native=false) {
        const int w=native?1920:2560,left=native?885:1180,top=native?248:330;
        const int cw=native?165:220,ch=native?68:90;
        unsigned maximum=0,changed=0,retreat=0,expand=0;double sum=0;
        for(int y=top;y<top+ch;y++)for(int x=left;x<left+cw;x++) {
            const auto i=(size_t(y)*w+x)*4;unsigned delta=0;
            for(int c=0;c<3;c++){const auto d=unsigned(std::abs(int(a[i+c])-int(b[i+c])));maximum=std::max(maximum,d);delta=std::max(delta,d);sum+=d;}
            changed+=delta>16;retreat+=a[i]>=230 && b[i]<128;expand+=a[i]<128 && b[i]>=230;
        }
        csv<<label<<','<<maximum<<','<<sum/(cw*ch*3)<<','<<changed<<','<<retreat<<','<<expand<<'\n';
    };
    HWND hwnd=Window(2560,1440);g_cmaaNisRequest=-1;DirectD3D11Renderer r;
    Check(r.initialize(hwnd,1920,1080,60,VideoPixelFormat::Nv12),"SDR text renderer");
    Render(r,input,1920);const auto off=Read(r);save("off",off);
    g_cmaaNisRequest=15;Check(r.configureVideoOutput(1920,1080),"SDR text filter enable");
    Require(r.cmaaNisPipeline.Enabled(),"production filter enabled");
    Render(r,input,1920);const auto both=Read(r);save("cmaa-nis15",both);
    using Access=llcv::upscaling::PipelineTestAccess;
    const auto native=ReadSdrView(r,Access::NativeInput(r.cmaaNisPipeline));save("native",native,true);
    Access::NisOnly(r.cmaaNisPipeline,r.context,r.backBufferRenderTarget);const auto nis=Read(r);save("nis15",nis);
    const auto cmaa=ReadSdrView(r,Access::CmaaOnly(r.cmaaNisPipeline,r.context));save("cmaa-native",cmaa,true);
    for(int repeat=0;repeat<8;repeat++) {
        Render(r,input,1920);const auto repeated=Read(r);unsigned repeatError=0,overOne=0;
        for(size_t i=0;i<repeated.size();i++){const auto delta=unsigned(std::abs(int(repeated[i])-int(both[i])));repeatError=std::max(repeatError,delta);overOne+=delta>1;}
        std::printf("SDR repeat=%d maximum8bitError=%u channelsOverOne=%u\n",repeat,repeatError,overOne);
        if(repeatError>1)save("repeat-failure",repeated);
        Require(repeatError<=1,"SDR production text output repeat stability within one quantization step");
        Access::NisOnly(r.cmaaNisPipeline,r.context,r.backBufferRenderTarget);Require(Read(r)==nis,"SDR NIS-only output repeat stability");
    }
    Access::DisableSharpen(r.cmaaNisPipeline,r.device);
    Render(r,input,1920);const auto noSharpBoth=Read(r);save("cmaa-nis-no-sharpen",noSharpBoth);
    Access::NisOnly(r.cmaaNisPipeline,r.context,r.backBufferRenderTarget);const auto noSharpNis=Read(r);save("nis-no-sharpen",noSharpNis);
    compare("off_to_nis15",off,nis);compare("nis15_to_cmaa_nis15",nis,both);compare("off_to_cmaa_nis15",off,both);
    compare("native_to_cmaa_native",native,cmaa,true);compare("nis15_to_no_sharpen",nis,noSharpNis);
    compare("off_to_nis_no_sharpen",off,noSharpNis);compare("cmaa_nis15_to_no_sharpen",both,noSharpBoth);
    g_cmaaNisRequest=-1;Check(r.configureVideoOutput(1920,1080),"SDR text disable");Render(r,input,1920);
    Require(Read(r)==off,"SDR OFF restoration exact");
    r.reset();DestroyWindow(hwnd);
    std::puts("PASS: same saved SDR frame, OFF/NIS/CMAA+NIS, native pre/post CMAA, zero-USM controls, repeat stability and exact OFF restoration. Pixel differences are not quality scores.");
}
static int Difference(const std::vector<BYTE>& a,const std::vector<BYTE>& b,bool hdr=false) {
    Require(a.size()==b.size(),"pixel sizes");
    int result=0;
    for(size_t i=0;i<a.size();i+=4) {
        if(hdr) {
            uint32_t x=0,y=0;std::memcpy(&x,a.data()+i,4);std::memcpy(&y,b.data()+i,4);
            for(int c=0;c<3;c++)result=std::max(result,std::abs(int((x>>(10*c))&1023)-int((y>>(10*c))&1023)));
            Require((x>>30)==3 && (y>>30)==3,"HDR alpha remains opaque");
        } else for(int c=0;c<4;c++)result=std::max(result,std::abs(int(a[i+c])-int(b[i+c])));
    }
    return result;
}
static void Regression() {
    using llcv::upscaling::Eligible;
    Require(!Eligible(false,640,360,{0,0,960,540}),"off bypass");
    Require(!Eligible(true,640,360,{0,0,640,360}),"native bypass");
    Require(!Eligible(true,640,360,{0,0,320,180}),"downscale bypass");
    Require(!Eligible(true,640,360,{0,0,1920,1080}),">2x bypass");
    Require(Eligible(true,640,360,{8,6,968,546}),"offset 1.5x");
    const UINT w=320,h=180;
    HWND hwnd=Window(480,270);
    for(auto mode:{PresentationMode::VSync,PresentationMode::Compatibility,PresentationMode::AllowTearing}) {
    g_settings.presentationMode=mode;
    for(auto format:{VideoPixelFormat::Nv12,VideoPixelFormat::Yuy2})
    for(auto range:{llcv::video_color::Range::Limited,llcv::video_color::Range::Full})
    for(auto matrix:{llcv::video_color::Matrix::Bt709,llcv::video_color::Matrix::Bt601}) {
        llcv::video_color::Configuration color{};color.range=range;color.matrix=matrix;
        std::vector<BYTE> input(size_t(w)*h*(format==VideoPixelFormat::Nv12?3:4)/2,126);
        if(format==VideoPixelFormat::Nv12)
            for(size_t i=size_t(w)*h;i<input.size();i+=2){input[i]=96;input[i+1]=158;}
        else for(size_t i=0;i<input.size();i+=4){input[i+1]=96;input[i+3]=158;}
        const auto original=input;
        const UINT stride=format==VideoPixelFormat::Nv12?w:w*2;
        g_cmaaNisRequest=-1;DirectD3D11Renderer r;
        Check(r.initialize(hwnd,w,h,60,format,false,color),"SDR initialize");
        Render(r,input,stride);const auto plain=Read(r);
        for(int sharp:{0,15,100}) {
            g_cmaaNisRequest=sharp;Check(r.configureVideoOutput(w,h),"enable production");
            Require(r.cmaaNisPipeline.Enabled(),"production pipeline active, no silent fallback");
            Render(r,input,stride);const auto processed=Read(r);
            Require(Difference(processed,plain)<=2,"SDR flat color/matrix/range preserved");
            for(int repeat=0;repeat<4;repeat++){Render(r,input,stride);Require(Difference(Read(r),processed)<=1,"stable current-frame output");}
            Check(r.cmaaNisPipeline.Configure(r.device,r.videoDevice,r.enumerator,true,false,w,h,
                RECT{0,0,480,270},0x1002,float(sharp)/100.0f),"generic 256-thread NIS shader");
            Render(r,input,stride);Require(Difference(Read(r),processed)<=1,"both vendor shader variants preserve output");
            g_audioOsdVisible=true;Check(r.drawOverlayQuads(),"audio OSD after upscaling");
            Require(Read(r)!=processed,"OSD visible");g_audioOsdVisible=false;
            Render(r,input,stride);Require(Difference(Read(r),processed)<=1,"no stale OSD");
            Require(input==original,"recording/screenshot source unchanged");
            g_cmaaNisRequest=-1;Check(r.configureVideoOutput(w,h),"disable");
            Require(!r.cmaaNisPipeline.Enabled(),"disabled releases resources");
            Render(r,input,stride);Require(Read(r)==plain,"OFF restores original exact pixels");
        }
    }
    }
    // Patterned frames prove AA/scaling is actually executing, not only reporting ON.
    g_cmaaNisRequest=-1;DirectD3D11Renderer r;
    Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::Nv12),"pattern initialize");
    const auto pattern=Pattern(w,h);Render(r,pattern,w);const auto plain=Read(r);
    g_cmaaNisRequest=15;Check(r.configureVideoOutput(w,h),"pattern enable");Render(r,pattern,w);
    const auto enabled=Read(r);Require(Difference(enabled,plain)>5,"upscaler changes edges");
    for(int i=0;i<8;i++){Render(r,pattern,w);Require(Difference(Read(r),enabled)<=1,"dense edge lists stable");}
    Require(r.cmaaNisPipeline.Configure(nullptr,nullptr,nullptr,true,true,w,h,
        RECT{0,0,480,270},r.adapterVendor,.15f)==S_FALSE,"HDR bypass releases an already active SDR pipeline");
    Require(!r.cmaaNisPipeline.Enabled() &&
        llcv::upscaling::PipelineTestAccess::NativeInput(r.cmaaNisPipeline)==nullptr,
        "HDR bypass retains no prior SDR input texture");
    Check(r.configureVideoOutput(w,h),"SDR resumes after resource bypass");Render(r,pattern,w);
    Require(Difference(Read(r),enabled)<=1,"SDR output is preserved after HDR bypass");
    g_vsrGpuVendor=r.adapterVendor;
    if(llcv::vsr::NvidiaAdapter(r.adapterVendor)) {
        g_settings.cmaaNisEnabled=true;g_settings.vsrEnabled=false;g_vsrMode=llcv::vsr::Mode::Disabled;
        Require(HandleVsrTestKey(VK_F6,0,0),"existing VSR shortcut");
        Require(!g_settings.cmaaNisEnabled && g_cmaaNisRequest==-1 && g_settings.vsrEnabled,"VSR shortcut excludes CMAA/NIS");
        Check(r.presentUploaded(),"VSR transition");
        Require(!r.cmaaNisPipeline.Enabled(),"F6 removes CMAA resources on render owner");
        HandleVsrTestKey(VK_F6,0,0);g_vsrMode=llcv::vsr::Mode::Disabled;
    }
    g_cmaaNisRequest=15;g_vsrMode=llcv::vsr::Mode::Off;
    Microsoft::WRL::ComPtr<ID3D11Device> sameDevice=r.device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sameUpload=r.nv12Textures[0];
    Microsoft::WRL::ComPtr<IDXGISwapChain1> sameChain=r.swapChain;
    for(const SIZE size:{SIZE{640,360},SIZE{320,180},SIZE{160,90},SIZE{960,540},SIZE{480,270}}) {
        Require(SetWindowPos(hwnd,nullptr,0,0,size.cx,size.cy,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE),"resize comparison window");
        Check(r.resizeOutput(hwnd),"resize active upscaler");
        Require(r.cmaaNisPipeline.Enabled()==Eligible(true,w,h,{0,0,size.cx,size.cy}),"resize enters/bypasses supported scale");
        Require(r.device==sameDevice.Get() && r.nv12Textures[0]==sameUpload.Get() && r.swapChain==sameChain.Get(),"resize preserves capture upload/device/swapchain");
        Render(r,pattern,w);const auto resized=Read(r);
        Render(r,pattern,w);Require(Difference(Read(r),resized)<=1,"resized output remains stable");
    }
    r.reset();DestroyWindow(hwnd);
    // Odd output and an offset viewport must never leave stale borders.
    hwnd=Window(490,278);g_cmaaNisRequest=15;
    Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::Nv12),"placement initialize");
    Check(r.cmaaNisPipeline.Configure(r.device,r.videoDevice,r.enumerator,true,false,w,h,
        RECT{5,4,485,274},r.adapterVendor,.15f),"offset configure");
    const float black[4]{0,0,0,1};r.context->ClearRenderTargetView(r.backBufferRenderTarget,black);
    Render(r,pattern,w);const auto placed=Read(r);
    Require(placed[0]==0 && placed[(size_t(277)*490+489)*4]==0,"black borders retained");
    Require(r.cmaaNisPipeline.Configure(r.device,r.videoDevice,r.enumerator,true,false,w,h,
        RECT{0,0,480,270},r.adapterVendor,std::numeric_limits<float>::quiet_NaN())==E_INVALIDARG &&
        !r.cmaaNisPipeline.Enabled(),"invalid strength fails closed");
    r.reset();DestroyWindow(hwnd);g_cmaaNisRequest=-1;
    std::puts("PASS: production SDR formats/ranges/matrices, sharpness, OFF restoration, OSD, source preservation, F6, borders.");
}
static void HdrBypassRegression() {
    const UINT w=320,h=180;HWND hwnd=Window(480,270);
    for(auto chroma:{DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020}) {
        g_cmaaNisRequest=-1;DirectD3D11Renderer r;
        const auto hr=r.initialize(hwnd,w,h,60,VideoPixelFormat::P010,true,{},chroma);
        Check(hr,"P010 PQ initialize");
        Require(r.hdrOutput && r.backBufferFormat==DXGI_FORMAT_R10G10B10A2_UNORM,"native 10-bit HDR output retained");
        for(auto uv:{std::pair{512u,512u},std::pair{430u,580u},std::pair{580u,430u}})
        for(unsigned level:{64u,80u,200u,400u,700u,900u,940u}) {
            std::vector<BYTE> p(size_t(w)*h*3);
            auto* values=reinterpret_cast<uint16_t*>(p.data());
            std::fill(values,values+size_t(w)*h,uint16_t(level<<6));
            std::fill(values+size_t(w)*h,values+size_t(w)*h*3/2,uint16_t(512<<6));
            for(size_t i=size_t(w)*h;i<size_t(w)*h*3/2;i+=2) {
                values[i]=uint16_t(uv.first<<6);values[i+1]=uint16_t(uv.second<<6);
            }
            const auto original=p;
            g_cmaaNisRequest=-1;Check(r.configureVideoOutput(w,h),"HDR baseline");Render(r,p,w*2);const auto plain=Read(r);
            for(int sharp:{0,15,100}) {
                g_cmaaNisRequest=sharp;Check(r.configureVideoOutput(w,h),"HDR enable");
                Require(!r.cmaaNisPipeline.Enabled() && r.cmaaNisStatus==S_FALSE,"HDR bypasses SDR-only filter");
                Require(g_cmaaNisRequest==sharp,"HDR bypass preserves SDR sharpness request");
                Render(r,p,w*2);const auto scaled=Read(r);
                const auto error=Difference(plain,scaled,true);
                std::printf("HDR chroma=%d Y=%u sharpness=%d maximum10bitError=%d\n",int(chroma),level,sharp,error);
                Require(error==0,"HDR bypass is pixel-identical to the original output");
                Require(p==original,"P010 source preserved");
                Require(r.cmaaNisPipeline.Configure(nullptr,nullptr,nullptr,true,true,w,h,
                    RECT{0,0,480,270},0x8086,float(sharp)/100.0f)==S_FALSE,
                    "HDR bypass needs no GPU filter resources on any vendor");
                Render(r,p,w*2);Require(Difference(Read(r),scaled,true)==0,"HDR bypass agrees across vendor selections");
                g_audioOsdVisible=true;Check(r.drawOverlayQuads(),"HDR OSD composition");
                Require(Read(r)!=scaled,"HDR OSD visible");g_audioOsdVisible=false;
                Render(r,p,w*2);Require(Difference(Read(r),scaled,true)<=1,"HDR OSD removed next frame");
            }
        }
    }
    // HDR bypass must preserve patterned edges as well as flat colors.
    const auto sdrPattern=Pattern(w,h);
    std::vector<BYTE> hdrPattern(size_t(w)*h*3);
    auto* words=reinterpret_cast<uint16_t*>(hdrPattern.data());
    std::fill(words,words+size_t(w)*h*3/2,uint16_t(512<<6));
    for(size_t i=0;i<size_t(w)*h;i++) words[i]=uint16_t((64+4*(unsigned(sdrPattern[i])-16))<<6);
    DirectD3D11Renderer r;g_cmaaNisRequest=-1;
    Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::P010,true),"HDR pattern initialize");
    Render(r,hdrPattern,w*2);const auto plain=Read(r);
    g_cmaaNisRequest=15;Check(r.configureVideoOutput(w,h),"HDR pattern enable");
    Render(r,hdrPattern,w*2);const auto filtered=Read(r);
    Require(Difference(plain,filtered,true)==0,"HDR edges are unchanged by an SDR filter request");
    for(int repeat=0;repeat<8;repeat++) {
        Render(r,hdrPattern,w*2);Require(Difference(Read(r),filtered,true)<=2,"HDR dense edge output stable");
    }
    // Bright, already-antialiased HUD text is not covered by flat fields. Keep
    // solid strokes and counters intact at different HDR luminances and scales.
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-LONG(h);
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* maskPixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&maskPixels,nullptr,0);
    Require(dc && bitmap && maskPixels,"HDR text mask");
    const auto previousBitmap=SelectObject(dc,bitmap);
    HFONT font=CreateFontW(-44,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Malgun Gothic");
    const auto previousFont=SelectObject(dc,font);
    PatBlt(dc,0,0,w,h,BLACKNESS);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
    RECT textRect{12,34,310,150};DrawTextW(dc,L"그래픽 HDR\n123 ABC",-1,&textRect,DT_LEFT|DT_TOP|DT_NOPREFIX);
    GdiFlush();const std::vector<BYTE> mask(static_cast<BYTE*>(maskPixels),static_cast<BYTE*>(maskPixels)+size_t(w)*h*4);
    SelectObject(dc,previousFont);DeleteObject(font);SelectObject(dc,previousBitmap);DeleteObject(bitmap);DeleteDC(dc);
    auto pq=[](double nits){const auto p=std::pow(nits/10000,2610.0/16384);return std::pow((3424.0/4096+2413.0/128*p)/(1+2392.0/128*p),2523.0/32);};
    for(double nits:{100.0,400.0,1000.0}) {
        const auto peak=pq(nits);std::vector<BYTE> text(size_t(w)*h*3);
        auto* values=reinterpret_cast<uint16_t*>(text.data());
        std::fill(values,values+size_t(w)*h*3/2,uint16_t(512<<6));
        for(size_t i=0;i<size_t(w)*h;i++)values[i]=uint16_t(std::lround(64+876*peak*mask[i*4]/255.0)<<6);
        for(const SIZE size:{SIZE{480,270},SIZE{640,360}}) {
            Require(SetWindowPos(hwnd,nullptr,0,0,size.cx,size.cy,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE),"HDR text resize");
            Check(r.resizeOutput(hwnd),"HDR text output resize");Render(r,text,w*2);const auto result=Read(r);
            unsigned cores=0;
            for(unsigned y=3;y<h-3;y++)for(unsigned x=3;x<w-3;x++) {
                bool core=true;
                for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++)core&=mask[((y+dy)*w+x+dx)*4]>=250;
                if(!core)continue;++cores;
                const auto ox=unsigned((x+.5)*size.cx/w),oy=unsigned((y+.5)*size.cy/h);
                uint32_t value=0;std::memcpy(&value,result.data()+(size_t(oy)*size.cx+ox)*4,4);
                for(int c=0;c<3;c++)Require(int((value>>(c*10))&1023)>=int(peak*1023)-16,"HDR solid glyph strokes cannot become holes");
            }
            Require(cores>100,"HDR fixture has substantial solid text strokes");
            for(int repeat=0;repeat<4;repeat++){Render(r,text,w*2);Require(Difference(Read(r),result,true)<=2,"HDR text is deterministic");}
        }
    }
    r.reset();
    g_settings.cmaaNisEnabled=true;g_settings.nisSharpnessPercent=15;g_cmaaNisRequest=15;
    for(int pass=0;pass<3;++pass) {
        const bool hdr=pass==1;
        Check(r.initialize(hwnd,w,h,60,hdr?VideoPixelFormat::P010:VideoPixelFormat::Nv12,hdr),"SDR HDR SDR transition");
        Require(r.cmaaNisPipeline.Enabled()==!hdr,"SDR filter resumes after HDR bypass");
        Require(g_settings.cmaaNisEnabled && g_settings.nisSharpnessPercent==15 && g_cmaaNisRequest==15,
            "HDR does not overwrite the selected SDR filter or sharpness");
        if(hdr)Require(r.vsrOsdLine().find(L"SDR")!=std::wstring::npos,"HDR OSD explains SDR-only bypass");
        r.reset();
    }
    DestroyWindow(hwnd);g_cmaaNisRequest=-1;std::puts("PASS: HDR bypass, exact output, chroma, PQ, OSD, source and SDR preference preservation.");
}
static void ApprovedReference(const std::filesystem::path& inputPath,const std::filesystem::path& referencePath) {
    std::ifstream source(inputPath,std::ios::binary);
    Require(bool(source),"approved NV12 source opens");
    const std::vector<BYTE> input{std::istreambuf_iterator<char>(source),std::istreambuf_iterator<char>()};
    Require(input.size()==size_t(1920)*1080*3/2,"approved FHD source length");
    std::ifstream expected(referencePath,std::ios::binary);
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
    expected.read(reinterpret_cast<char*>(&file),sizeof(file));expected.read(reinterpret_cast<char*>(&info),sizeof(info));
    Require(bool(expected) && file.bfType==0x4d42 && info.biWidth==2560 && info.biHeight==-1440 &&
        info.biBitCount==32 && info.biCompression==BI_RGB,"approved top-down BGRA reference");
    expected.seekg(file.bfOffBits);std::vector<BYTE> reference(size_t(2560)*1440*4);
    expected.read(reinterpret_cast<char*>(reference.data()),std::streamsize(reference.size()));
    Require(bool(expected),"approved pixels read");
    HWND hwnd=Window(2560,1440);g_cmaaNisRequest=15;
    DirectD3D11Renderer r;Check(r.initialize(hwnd,1920,1080,60,VideoPixelFormat::Nv12),"approved source renderer");
    Require(r.cmaaNisPipeline.Enabled(),"approved source uses production scaler");
    Render(r,input,1920);const auto difference=Difference(Read(r),reference);
    std::printf("Approved CMAA2 then NIS 0.15: maximum channel difference=%d/255\n",difference);
    Require(difference<=1,"production preserves selected prototype quality");
    r.reset();DestroyWindow(hwnd);g_cmaaNisRequest=-1;
}
static void BenchmarkProduction(const std::filesystem::path& output) {
    Require(!std::filesystem::exists(output),"preserve earlier benchmark");
    std::filesystem::create_directories(output);
    std::ofstream csv(output/"timings.csv");csv<<"round,method,gpu_mean_ms,gpu_p95_ms\n";
    HWND hwnd=Window(2560,1440);const auto input=Pattern(1920,1080);
    for(int round=0;round<3;round++)for(int j=0;j<2;j++) {
        const bool enabled=(j+round)%2!=0;g_cmaaNisRequest=enabled?15:-1;
        DirectD3D11Renderer r;Check(r.initialize(hwnd,1920,1080,60,VideoPixelFormat::Nv12),"benchmark initialize");
        Require(r.cmaaNisPipeline.Enabled()==enabled,"benchmark production mode");
        for(int i=0;i<25;i++)Render(r,input,1920);Read(r);
        Timer timer(r.device);std::vector<double> times;
        for(int i=0;i<120;i++) {
            const auto now=std::chrono::steady_clock::now();r.upload(input.data(),1920);
            D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=r.inputViews[r.activeUploadSurface];
            timer.Begin(r.context);Check(r.processVideo(stream),"benchmark process");times.push_back(timer.End(r.context,r.backBuffer));
            std::this_thread::sleep_until(now+std::chrono::microseconds(16667));
        }
        const auto mean=std::accumulate(times.begin(),times.end(),0.0)/times.size();
        csv<<round<<','<<(enabled?"cmaa2-nis15":"off")<<','<<mean<<','<<Percentile(times,.95)<<'\n';csv.flush();
        std::printf("round=%d enabled=%d GPU=%.4f p95=%.4f ms\n",round,enabled,mean,Percentile(times,.95));
    }
    DestroyWindow(hwnd);g_cmaaNisRequest=-1;
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
    for(int sharpness:{-1,0,15,100}) {
        g_cmaaNisRequest=sharpness;DirectD3D11Renderer r;Check(r.initialize(window,w,h,60,VideoPixelFormat::Nv12),"recording renderer");
        Require(r.cmaaNisPipeline.Enabled()==(sharpness>=0),"recording uses production upscaler");
        const auto path=output/(std::to_string(sharpness)+".mp4");
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
        reader.Reset();
        Check(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader),"audio recording opens");
        const DWORD audioStream=static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        Check(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE),"deselect for audio");
        Check(reader->SetStreamSelection(audioStream,TRUE),"select AAC");
        type.Reset();Check(MFCreateMediaType(&type),"audio decode type");
        Check(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio),"audio major");
        Check(type->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_PCM),"PCM decode");
        Check(reader->SetCurrentMediaType(audioStream,nullptr,type.Get()),"set PCM");
        uint64_t samples=0;double energy=0;LONGLONG audioEnd=0;
        for(;;) {
            DWORD flags=0;LONGLONG time=0;Microsoft::WRL::ComPtr<IMFSample> sample;
            Check(reader->ReadSample(audioStream,0,nullptr,&flags,&time,&sample),"read recorded sound");
            if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
            if(!sample)continue;
            LONGLONG duration=0;sample->GetSampleDuration(&duration);audioEnd=time+duration;
            Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;Check(sample->ConvertToContiguousBuffer(&buffer),"audio buffer");
            BYTE* data=nullptr;DWORD bytes=0;Check(buffer->Lock(&data,nullptr,&bytes),"audio lock");
            for(DWORD i=0;i+1<bytes;i+=2){short value=0;std::memcpy(&value,data+i,2);energy+=double(value)*value;++samples;}
            buffer->Unlock();
        }
        Require(samples>=96000 && samples<=102400 && std::sqrt(energy/samples)>4000,"recorded stereo tone retained");
        Require(audioEnd>=9800000 && audioEnd<=10700000,"recorded audio/video end within AAC padding");
        std::printf("RECORDING %s frames=%u path=%ls\n",std::to_string(sharpness).c_str(),frames,path.c_str());
    }
    DestroyWindow(window);g_cmaaNisRequest=-1;Check(MFShutdown(),"MF shutdown");
}
#endif

int main(int argc,char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
    g_suppressSettingsSave=true;g_settings.pixelPerfect=false;g_settings.displayMatchesCapture=false;
    g_settings.videoPreset=VideoPreset::R1920x1080;g_settings.scalingMode=ScalingMode::Smooth;
    g_settings.presentationMode=PresentationMode::AllowTearing;g_vsrMode=llcv::vsr::Mode::Disabled;
    g_osdVisible=false;g_audioOsdVisible=false;g_volumeHudUntilMs=0;
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;wc.lpszClassName=L"LLCV_SPATIAL_TEST";
    Require(RegisterClassW(&wc)!=0,"register class");
    if(argc>=4 && std::string(argv[1])=="--sdr-text")SdrTextComparison(argv[2],argv[3]);
    else if(argc>=4 && std::string(argv[1])=="--reference")ApprovedReference(argv[2],argv[3]);
    else if(argc>=3 && std::string(argv[1])=="--benchmark")BenchmarkProduction(argv[2]);
    else if(argc>=2 && std::string(argv[1])=="--hdr")HdrBypassRegression();
#ifdef LLCV_RECORDING
    else if(argc>=3 && std::string(argv[1])=="--recording")RecordingIntegration(argv[2]);
#endif
    else if(argc==1) Regression();
    else { std::fprintf(stderr,"Unknown or retired upscaling test option\n"); return 2; }
    UnregisterClassW(wc.lpszClassName,wc.hInstance);CoUninitialize();return 0;
}
