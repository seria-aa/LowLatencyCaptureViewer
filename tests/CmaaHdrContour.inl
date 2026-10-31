#include "HdrGlyphFixture.h"

static std::vector<BYTE> ReadHdrNative(DirectD3D11Renderer& r,UINT& w,UINT& h) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Resource> resource;
    llcv::upscaling::PipelineTestAccess::NativeInput(r.cmaaNisPipeline)->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;Check(resource.As(&texture),"HDR native texture");
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);w=desc.Width;h=desc.Height;
    Require(desc.Format==DXGI_FORMAT_R10G10B10A2_UNORM,"HDR scratch retains ten-bit PQ");
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.BindFlags=desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging;Check(r.device->CreateTexture2D(&desc,nullptr,&staging),"HDR native staging");
    r.context->CopyResource(staging.Get(),texture.Get());D3D11_MAPPED_SUBRESOURCE map{};
    Check(r.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map),"HDR native readback");
    std::vector<BYTE> result(size_t(w)*h*4);
    for(UINT y=0;y<h;y++)std::memcpy(result.data()+size_t(y)*w*4,static_cast<const BYTE*>(map.pData)+size_t(y)*map.RowPitch,size_t(w)*4);
    r.context->Unmap(staging.Get(),0);return result;
}
static unsigned HdrContourViolations(const std::vector<BYTE>& native,UINT w,UINT h,
    const std::vector<BYTE>& filtered,UINT ow,UINT oh,const RECT& rect) {
    Require(filtered.size()==size_t(ow)*oh*4 && native.size()==size_t(w)*h*4,"HDR bound input sizes");
    auto channel=[&](int x,int y,int c) {
        x=std::clamp(x,0,int(w)-1);y=std::clamp(y,0,int(h)-1);
        uint32_t pixel=0;std::memcpy(&pixel,native.data()+(size_t(y)*w+x)*4,4);
        return double((pixel>>(10*c))&1023);
    };
    unsigned violations=0;
    for(int y=rect.top;y<rect.bottom;y++)for(int x=rect.left;x<rect.right;x++) {
        const double sx=(x-rect.left+.5)*w/(rect.right-rect.left)-.5;
        const double sy=(y-rect.top+.5)*h/(rect.bottom-rect.top)-.5;
        const int ix=int(std::floor(sx)),iy=int(std::floor(sy));
        const double fx=sx-ix,fy=sy-iy;
        uint32_t pixel=0;std::memcpy(&pixel,filtered.data()+(size_t(y)*ow+x)*4,4);
        Require((pixel>>30)==3,"HDR output stays opaque");
        for(int c=0;c<3;c++) {
            const double reference=(1-fy)*((1-fx)*channel(ix,iy,c)+fx*channel(ix+1,iy,c))+
                fy*((1-fx)*channel(ix,iy+1,c)+fx*channel(ix+1,iy+1,c));
            // CPU bilinear reference, with one code for final quantization and
            // sampler interpolation precision. No use of the shader's result.
            violations+=std::abs(double((pixel>>(10*c))&1023)-reference)>std::max(reference*.02,.5)+1.0;
        }
    }
    return violations;
}
static double HdrPixelNits(const std::vector<BYTE>& image,UINT width,UINT x,UINT y) {
    uint32_t pixel=0;std::memcpy(&pixel,image.data()+(size_t(y)*width+x)*4,4);
    const double p=std::pow(double(pixel&1023)/1023,1.0/78.84375);
    return 10000*std::pow(std::max(p-.8359375,0.0)/(18.8515625-18.6875*p),1.0/.1593017578125);
}
static void HdrCapturedGlyphRegression() {
    constexpr UINT w=192,h=108,ow=256,oh=144;
    HWND hwnd=Window(ow,oh);DirectD3D11Renderer r;g_cmaaNisRequest=-1;
    std::vector<BYTE> input(size_t(w)*h*3);auto* words=reinterpret_cast<uint16_t*>(input.data());
    std::fill(words,words+size_t(w)*h,uint16_t(64<<6));
    std::fill(words+size_t(w)*h,words+size_t(w)*h*3/2,uint16_t(512<<6));
    for(UINT y=0;y<24;y++)for(UINT x=0;x<24;x++)words[(y+30)*w+x+72]=uint16_t(kHdrGlyphLuma[y*24+x]<<6);
    const auto source=input;
    Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::P010,true),"captured HDR glyph initialize");
    Render(r,input,w*2);const auto off=Read(r);
    unsigned mutationViolations=0;
    for(bool preserve:{false,true})for(int sharpness:{0,15,50,100}) {
        g_cmaaNisRequest=llcv::upscaling::Request(true,sharpness,preserve);
        Check(r.configureVideoOutput(w,h),"captured glyph filter configure");
        Require(r.cmaaNisPipeline.Enabled(),"captured glyph uses real HDR filter");
        Render(r,input,w*2);const auto filtered=Read(r);
        UINT nw=0,nh=0;const auto native=ReadHdrNative(r,nw,nh);
        Require(HdrContourViolations(native,nw,nh,filtered,ow,oh,{0,0,LONG(ow),LONG(oh)})==0,
            "all captured glyph pixels obey the original-image contour bound");
        const auto guard=llcv::upscaling::PipelineTestAccess::DisableHdrGuard(r.cmaaNisPipeline,r.device);
        Render(r,input,w*2);const auto unguarded=Read(r);
        mutationViolations+=HdrContourViolations(native,nw,nh,unguarded,ow,oh,{0,0,LONG(ow),LONG(oh)});
        std::printf("captured-glyph preserve=%d sharpness=%d problem-pixel nits OFF=%.5f guarded=%.5f unguarded=%.5f\n",
            preserve,sharpness,HdrPixelNits(off,ow,108,58),HdrPixelNits(filtered,ow,108,58),HdrPixelNits(unguarded,ow,108,58));
        llcv::upscaling::PipelineTestAccess::RestoreHdrGuard(r.cmaaNisPipeline,guard);
        Render(r,input,w*2);Require(Difference(Read(r),filtered,true)<=2,"restored HDR guard is deterministic");
        Require(input==source,"captured P010 source never changes");
        g_cmaaNisRequest=-1;Check(r.configureVideoOutput(w,h),"captured glyph OFF");Render(r,input,w*2);
        Require(Read(r)==off,"captured glyph OFF restores exact original output");
    }
    Require(mutationViolations>100,"disabling the guard demonstrably fails the captured glyph contour bound");
    g_cmaaNisRequest=15;Check(r.configureVideoOutput(w,h),"HDR offset preparation");
    const RECT offset{5,4,245,139};
    Check(r.cmaaNisPipeline.Configure(r.device,r.videoDevice,r.enumerator,true,true,w,h,offset,r.adapterVendor,.15f,true),"HDR offset configure");
    const float black[]{0,0,0,1};r.context->ClearRenderTargetView(r.backBufferRenderTarget,black);
    Render(r,input,w*2);const auto placed=Read(r);UINT nw=0,nh=0;const auto native=ReadHdrNative(r,nw,nh);
    Require(HdrContourViolations(native,nw,nh,placed,ow,oh,offset)==0,"offset HDR guard samples the matching original viewport");
    uint32_t corner=0;std::memcpy(&corner,placed.data(),4);
    Require(corner==0xc0000000,"HDR filter retains opaque black placement margins");
    for(const RECT rect:{RECT{0,0,192,108},RECT{0,0,96,54},RECT{0,0,576,324}})
        Require(r.cmaaNisPipeline.Configure(nullptr,nullptr,nullptr,true,true,w,h,rect,0x10de,.15f)==S_FALSE &&
            !r.cmaaNisPipeline.Enabled(),"HDR native/downscale/beyond-2x bypass uses no filter resources");
    r.reset();DestroyWindow(hwnd);g_cmaaNisRequest=-1;
    std::puts("PASS: reported glyph ROI, original-reference contour bound, mutation, both correction modes/all sharpness, exact OFF restoration.");
}
static void HdrSavedComparison(const std::filesystem::path& source,const std::filesystem::path& output) {
    Require(llcv::test_profile::Enabled() && !std::filesystem::exists(output),"isolated HDR comparison and new output path");
    std::ifstream in(source,std::ios::binary);Require(bool(in),"saved P010 opens");
    const std::vector<BYTE> pixels{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
    constexpr UINT w=1920,h=1080,ow=2560,oh=1440;
    Require(pixels.size()==size_t(w)*h*3,"saved P010 is FHD");
    std::filesystem::create_directories(output);
    HWND hwnd=Window(ow,oh);DirectD3D11Renderer r;g_cmaaNisRequest=-1;
    Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::P010,true),"saved HDR renderer");
    Render(r,pixels,w*2);const auto off=Read(r);
    std::ofstream csv(output/"gpu.csv");csv<<"preserve,round,guard,samples,mean_ms,p95_ms\n";
    std::ofstream evidence(output/"glyph.txt");
    evidence<<"Same saved 1920x1080 P010/PQ frame -> 2560x1440 RGB10/PQ. No capture or real profiles.\n"
        <<"GPU timestamps include upload/VP/AA/NIS/output dependency, not Present, scanout or optical delay.\n"
        <<"Alternating guard/no-guard on one renderer; continuous 500ms warmup before each block.\n";
    for(bool preserve:{false,true}) {
        g_cmaaNisRequest=llcv::upscaling::Request(true,15,preserve);Check(r.configureVideoOutput(w,h),"saved HDR filter");
        Render(r,pixels,w*2);const auto guarded=Read(r);UINT nw=0,nh=0;const auto native=ReadHdrNative(r,nw,nh);
        Require(HdrContourViolations(native,nw,nh,guarded,ow,oh,{0,0,LONG(ow),LONG(oh)})==0,"full saved HDR frame contour bound");
        const auto guard=llcv::upscaling::PipelineTestAccess::DisableHdrGuard(r.cmaaNisPipeline,r.device);
        Render(r,pixels,w*2);const auto unguarded=Read(r);
        const auto noGuard=llcv::upscaling::PipelineTestAccess::DisableHdrGuard(r.cmaaNisPipeline,r.device);
        evidence<<"preserve="<<preserve<<" problem pixel (1340,378) nits off="<<HdrPixelNits(off,ow,1340,378)
            <<" guarded="<<HdrPixelNits(guarded,ow,1340,378)<<" unguarded="<<HdrPixelNits(unguarded,ow,1340,378)<<'\n';
        Timer timer(r.device,r.backBufferFormat);
        for(int round=0;round<3;round++)for(int at=0;at<2;at++) {
            const bool enabled=(round&1)?at==0:at==1;
            llcv::upscaling::PipelineTestAccess::RestoreHdrGuard(r.cmaaNisPipeline,enabled?guard:noGuard);
            const auto until=GetTickCount64()+500;unsigned warm=0;
            do {Render(r,pixels,w*2);if(++warm%32==0)Read(r);}while(GetTickCount64()<until);
            Read(r);std::vector<double> times;
            for(int i=0;i<120;i++){timer.Begin(r.context);Render(r,pixels,w*2);times.push_back(timer.End(r.context,r.backBuffer));}
            csv<<preserve<<','<<round<<','<<enabled<<','<<times.size()<<','
                <<std::accumulate(times.begin(),times.end(),0.0)/times.size()<<','<<Percentile(times,.95)<<'\n';csv.flush();
        }
        llcv::upscaling::PipelineTestAccess::RestoreHdrGuard(r.cmaaNisPipeline,guard);
    }
    r.reset();DestroyWindow(hwnd);g_cmaaNisRequest=-1;
    std::puts("PASS: full saved HDR frame, independent contour bounds, guarded/unguarded glyph evidence and paired GPU timings.");
}
