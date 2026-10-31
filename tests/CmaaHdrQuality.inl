// Saved HDR frame: how much of the CMAA2/NIS change survives the HDR contour
// bound, compared with OFF, a CPU bilinear reference and NIS alone.
namespace hdr_quality {
struct Image { std::vector<BYTE> pixels; UINT width=0,height=0; };
static double Code(const Image& image,int x,int y,int c) {
    x=std::clamp(x,0,int(image.width)-1);y=std::clamp(y,0,int(image.height)-1);
    uint32_t pixel=0;std::memcpy(&pixel,image.pixels.data()+(size_t(y)*image.width+x)*4,4);
    return double((pixel>>(10*c))&1023);
}
// Same half-pixel mapping as the guard shader, in 10-bit PQ codes.
static std::vector<float> Bilinear(const Image& native,UINT ow,UINT oh) {
    std::vector<float> out(size_t(ow)*oh*3);
    for(UINT y=0;y<oh;y++)for(UINT x=0;x<ow;x++) {
        const double sx=(x+.5)*native.width/ow-.5,sy=(y+.5)*native.height/oh-.5;
        const int ix=int(std::floor(sx)),iy=int(std::floor(sy));const double fx=sx-ix,fy=sy-iy;
        for(int c=0;c<3;c++)out[(size_t(y)*ow+x)*3+c]=float(
            (1-fy)*((1-fx)*Code(native,ix,iy,c)+fx*Code(native,ix+1,iy,c))+
            fy*((1-fx)*Code(native,ix,iy+1,c)+fx*Code(native,ix+1,iy+1,c)));
    }
    return out;
}
// Diagnostic edge mask: output pixels whose 4x4 native neighborhood spans
// more than `threshold` codes in any channel.
static std::vector<bool> EdgeMask(const Image& native,UINT ow,UINT oh,double threshold) {
    std::vector<bool> mask(size_t(ow)*oh);
    for(UINT y=0;y<oh;y++)for(UINT x=0;x<ow;x++) {
        const int ix=int(std::floor((x+.5)*native.width/ow-.5)),iy=int(std::floor((y+.5)*native.height/oh-.5));
        bool edge=false;
        for(int c=0;c<3 && !edge;c++) {
            double low=1024,high=-1;
            for(int dy=-1;dy<=2;dy++)for(int dx=-1;dx<=2;dx++) {
                const double v=Code(native,ix+dx,iy+dy,c);low=std::min(low,v);high=std::max(high,v);
            }
            edge=high-low>threshold;
        }
        mask[size_t(y)*ow+x]=edge;
    }
    return mask;
}
struct Stats { double meanFromBilinear=0,gradient=0; size_t samples=0; };
static Stats Measure(const Image& v,const std::vector<float>& bilinear,const std::vector<bool>& mask) {
    Stats s;double gradient=0;size_t gradientSamples=0;
    for(UINT y=0;y+1<v.height;y++)for(UINT x=0;x+1<v.width;x++) {
        if(!mask[size_t(y)*v.width+x])continue;
        for(int c=0;c<3;c++) {
            const double value=Code(v,x,y,c);
            s.meanFromBilinear+=std::abs(value-bilinear[(size_t(y)*v.width+x)*3+c]);++s.samples;
            gradient+=std::abs(Code(v,x+1,y,c)-value)+std::abs(Code(v,x,y+1,c)-value);++gradientSamples;
        }
    }
    if(s.samples)s.meanFromBilinear/=double(s.samples);
    if(gradientSamples)s.gradient=gradient/double(gradientSamples);
    return s;
}
static Image FromBilinear(const std::vector<float>& bilinear,UINT ow,UINT oh) {
    Image image{std::vector<BYTE>(size_t(ow)*oh*4),ow,oh};
    for(size_t i=0;i<size_t(ow)*oh;i++) {
        uint32_t pixel=3u<<30;
        for(int c=0;c<3;c++)pixel|=uint32_t(std::clamp(std::lround(bilinear[i*3+c]),0L,1023L))<<(10*c);
        std::memcpy(image.pixels.data()+i*4,&pixel,4);
    }
    return image;
}
// Guard activation: the unguarded value lies outside the shader's bound.
static void GuardRates(const Image& unguarded,const std::vector<float>& bilinear,const std::vector<bool>& mask,
                       double& edgeRate,double& frameRate) {
    size_t edgeHits=0,edgeSamples=0,hits=0,samples=size_t(unguarded.width)*unguarded.height*3;
    for(UINT y=0;y<unguarded.height;y++)for(UINT x=0;x<unguarded.width;x++)for(int c=0;c<3;c++) {
        const double reference=bilinear[(size_t(y)*unguarded.width+x)*3+c];
        const bool hit=std::abs(Code(unguarded,x,y,c)-reference)>std::max(reference*.02,.5);
        hits+=hit;
        if(mask[size_t(y)*unguarded.width+x]) {edgeHits+=hit;++edgeSamples;}
    }
    edgeRate=edgeSamples?100.0*edgeHits/edgeSamples:0;frameRate=100.0*hits/samples;
}
// PQ codes shown directly as 8-bit gray/colour, 4x nearest, around the glyph.
static void SaveCrop(const std::filesystem::path& path,const Image& image,RECT crop) {
    constexpr UINT zoom=4;const UINT w=UINT(crop.right-crop.left)*zoom,h=UINT(crop.bottom-crop.top)*zoom;
    std::vector<BYTE> out(size_t(w)*h*4,255);
    for(UINT y=0;y<h;y++)for(UINT x=0;x<w;x++)for(int c=0;c<3;c++)
        out[(size_t(y)*w+x)*4+2-c]=BYTE(Code(image,crop.left+int(x/zoom),crop.top+int(y/zoom),c)/4);
    SaveBmp(path,w,h,out);
}
static void SavePreview(const std::filesystem::path& path,const Image& image) {
    // Fixed monotone view for inspecting scene content/contours, not HDR
    // appearance certification. All candidates use exactly the same curve.
    std::vector<BYTE> pixels(size_t(image.width)*image.height*4,255);
    for(UINT y=0;y<image.height;y++)for(UINT x=0;x<image.width;x++)for(int c=0;c<3;c++) {
        const double p=std::pow(Code(image,x,y,c)/1023,1.0/78.84375);
        const double n=10000*std::pow(std::max(p-.8359375,0.0)/(18.8515625-18.6875*p),1.0/.1593017578125);
        const double linear=n/(n+132.0),srgb=linear<=.0031308?12.92*linear:1.055*std::pow(linear,1/2.4)-.055;
        pixels[(size_t(y)*image.width+x)*4+2-c]=BYTE(std::lround(std::clamp(srgb,0.0,1.0)*255));
    }
    SaveBmp(path,image.width,image.height,pixels);
}
static void Run(const std::filesystem::path& source,const std::filesystem::path& output,bool timing=true) {
    using Access=llcv::upscaling::PipelineTestAccess;
    Require(!std::filesystem::exists(output),"new HDR quality output path");
    std::ifstream in(source,std::ios::binary);Require(bool(in),"saved P010 opens");
    const std::vector<BYTE> pixels{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
    constexpr UINT w=1920,h=1080,ow=2560,oh=1440;
    Require(pixels.size()==size_t(w)*h*3,"saved P010 is FHD");
    std::filesystem::create_directories(output);
    HWND hwnd=Window(ow,oh);DirectD3D11Renderer r;g_cmaaNisRequest=-1;
    Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::P010,true),"saved HDR renderer");
    Render(r,pixels,w*2);const Image off{Read(r),ow,oh};
    D3D11_SAMPLER_DESC sam{};sam.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sam.AddressU=sam.AddressV=sam.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sam.MaxLOD=D3D11_FLOAT32_MAX;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;Check(r.device->CreateSamplerState(&sam,&sampler),"guard sampler");
    struct Variant { std::string name; Image image; };
    std::vector<Variant> variants{{"off",off}};
    Image native,nisUnguarded;
    for(bool preserve:{false,true}) {
        g_cmaaNisRequest=llcv::upscaling::Request(true,15,preserve);Check(r.configureVideoOutput(w,h),"saved HDR filter");
        Require(r.cmaaNisPipeline.Enabled(),"saved HDR frame uses the real HDR filter");
        Access::ConfigureLegacyHdrCmaa(r.cmaaNisPipeline,r.device,preserve);
        RenderLegacyHdr(r,pixels,w*2);const Image guarded{Read(r),ow,oh};
        if(native.pixels.empty())native.pixels=ReadHdrNative(r,native.width,native.height);
        const auto guard=Access::DisableHdrGuard(r.cmaaNisPipeline,r.device);
        RenderLegacyHdr(r,pixels,w*2);const Image unguarded{Read(r),ow,oh};
        const std::string mode=preserve?"preserve":"high";
        if(!preserve) {
            // NIS alone on the same native PQ image, unguarded then guarded.
            D3D11_VIDEO_PROCESSOR_STREAM s{};s.Enable=TRUE;s.pInputSurface=r.inputViews[r.activeUploadSurface];
            Access::NativeForNis(r.cmaaNisPipeline,r.context,r.videoContext,r.processor,s,ow,oh);
            Access::NisOnly(r.cmaaNisPipeline,r.context,r.backBufferRenderTarget);nisUnguarded={Read(r),ow,oh};
            Access::RestoreHdrGuard(r.cmaaNisPipeline,guard);
            Access::NativeForNis(r.cmaaNisPipeline,r.context,r.videoContext,r.processor,s,ow,oh);
            ID3D11ShaderResourceView* original=Access::NativeInput(r.cmaaNisPipeline);
            r.context->PSSetShaderResources(1,1,&original);r.context->PSSetSamplers(0,1,sampler.GetAddressOf());
            Access::NisOnly(r.cmaaNisPipeline,r.context,r.backBufferRenderTarget);
            ID3D11ShaderResourceView* empty=nullptr;r.context->PSSetShaderResources(1,1,&empty);
            variants.push_back({"nis-only-unguarded",nisUnguarded});
            variants.push_back({"nis-only-guarded",{Read(r),ow,oh}});
        }
        Access::RestoreHdrGuard(r.cmaaNisPipeline,guard);
        variants.push_back({"cmaa-nis-"+mode+"-unguarded",unguarded});
        variants.push_back({"cmaa-nis-"+mode+"-guarded",guarded});
    }
    const auto bilinear=Bilinear(native,ow,oh);
    variants.insert(variants.begin(),{"bilinear-cpu",FromBilinear(bilinear,ow,oh)});
    const auto mask=EdgeMask(native,ow,oh,24);
    size_t edgePixels=0;for(bool e:mask)edgePixels+=e;
    std::ofstream report(output/"quality.csv");
    report<<"variant,edge_mean_abs_from_bilinear_codes,edge_gradient_codes,gradient_vs_bilinear,problem_pixel_nits\n";
    const auto base=Measure(variants[0].image,bilinear,mask);
    const RECT crop{1300,350,1400,410};
    for(const auto& v:variants) {
        const auto s=Measure(v.image,bilinear,mask);
        report<<v.name<<','<<s.meanFromBilinear<<','<<s.gradient<<','<<s.gradient/base.gradient<<','
              <<HdrPixelNits(v.image.pixels,ow,1340,378)<<'\n';
        std::printf("%-28s edge|d|=%7.3f grad=%7.3f (x%.3f) pixel=%.3f nit\n",v.name.c_str(),s.meanFromBilinear,
            s.gradient,s.gradient/base.gradient,HdrPixelNits(v.image.pixels,ow,1340,378));
        SaveCrop(output/(v.name+".bmp"),v.image,crop);
        if(v.name=="off" || v.name=="nis-only-guarded" || v.name=="cmaa-nis-high-guarded")
            SavePreview(output/(v.name+"-preview.bmp"),v.image);
    }
    std::ofstream guards(output/"guard.csv");guards<<"variant,edge_guard_percent,frame_guard_percent,edge_change_retained_percent\n";
    const auto find=[&](const std::string& name)->const Image&{
        for(const auto& v:variants)if(v.name==name)return v.image;
        Require(false,"variant exists");return variants[0].image;
    };
    for(const std::string name:{"nis-only","cmaa-nis-high","cmaa-nis-preserve"}) {
        double edgeRate=0,frameRate=0;GuardRates(find(name+"-unguarded"),bilinear,mask,edgeRate,frameRate);
        const double retained=100*Measure(find(name+"-guarded"),bilinear,mask).meanFromBilinear/
            Measure(find(name+"-unguarded"),bilinear,mask).meanFromBilinear;
        guards<<name<<','<<edgeRate<<','<<frameRate<<','<<retained<<'\n';
        std::printf("%-18s guard fires: edge %.2f%% frame %.2f%%, edge change retained %.1f%%\n",
            name.c_str(),edgeRate,frameRate,retained);
    }
    std::printf("edge pixels: %.2f%% of output\n",100.0*edgePixels/mask.size());
    // CMAA2's own share: the same NIS stage with and without CMAA2 in front.
    for(const std::string mode:{"high","preserve"}) {
        const Image& with=find("cmaa-nis-"+mode+"-unguarded");const Image& without=find("nis-only-unguarded");
        double sum=0,largest=0;size_t samples=0,changed=0;
        for(UINT y=0;y<oh;y++)for(UINT x=0;x<ow;x++)for(int c=0;c<3;c++) {
            const double d=std::abs(Code(with,x,y,c)-Code(without,x,y,c));
            sum+=d;largest=std::max(largest,d);changed+=d>2;++samples;
        }
        guards<<"cmaa-"<<mode<<"-vs-nis-only,mean="<<sum/samples<<",max="<<largest<<",over2codes_percent="<<100.0*changed/samples<<'\n';
        std::printf("CMAA2 %-8s vs NIS only: mean %.3f max %.0f codes, %.3f%% of samples differ by >2 codes\n",
            mode.c_str(),sum/samples,largest,100.0*changed/samples);
    }
    std::ofstream differences(output/"guarded-aa-difference.csv");
    differences<<"mode,all_channel_mean_abs_codes,edge_channel_mean_abs_codes,edge_over2_percent,max_code_difference\n";
    for(const std::string mode:{"high","preserve"}) {
        const auto& with=find("cmaa-nis-"+mode+"-guarded");const auto& without=find("nis-only-guarded");
        double sum=0,edgeSum=0,largest=0;size_t edgeCount=0,edgeChanged=0;
        for(UINT y=0;y<oh;y++)for(UINT x=0;x<ow;x++)for(int c=0;c<3;c++) {
            const double d=std::abs(Code(with,x,y,c)-Code(without,x,y,c));sum+=d;largest=std::max(largest,d);
            if(mask[size_t(y)*ow+x]){edgeSum+=d;++edgeCount;edgeChanged+=d>2;}
        }
        differences<<mode<<','<<sum/(size_t(ow)*oh*3)<<','<<edgeSum/std::max(size_t(1),edgeCount)<<','
            <<100.0*edgeChanged/std::max(size_t(1),edgeCount)<<','<<largest<<'\n';
    }
    std::ofstream regions(output/"regions.csv");
    regions<<"region,correction,all_channel_mean_abs_codes,edge_channel_mean_abs_codes,edge_over2_percent,max_code_difference\n";
    struct Region{const char* name;RECT rect;};
    for(const auto& region:{Region{"character",{736,640,984,896}},Region{"rocks-pot",{1056,640,1536,896}},
                            Region{"stone-wall",{0,280,256,1360}},Region{"reported-glyph",{1300,350,1400,410}}}) {
        for(const std::string mode:{"high","preserve"}) {
            const auto& with=find("cmaa-nis-"+mode+"-guarded");const auto& without=find("nis-only-guarded");
            double sum=0,edgeSum=0,maximum=0;size_t allCount=0,edgeCount=0,edgeChanged=0;
            for(int y=region.rect.top;y<region.rect.bottom;y++)for(int x=region.rect.left;x<region.rect.right;x++)for(int c=0;c<3;c++) {
                const auto delta=std::abs(Code(with,x,y,c)-Code(without,x,y,c));sum+=delta;++allCount;maximum=std::max(maximum,delta);
                if(mask[size_t(y)*ow+x]){edgeSum+=delta;++edgeCount;edgeChanged+=delta>2;}
            }
            regions<<region.name<<','<<mode<<','<<sum/allCount<<','<<edgeSum/std::max(size_t(1),edgeCount)<<','
                <<100.0*edgeChanged/std::max(size_t(1),edgeCount)<<','<<maximum<<'\n';
            SaveCrop(output/(std::string(region.name)+"-"+mode+".bmp"),with,region.rect);
        }
        SaveCrop(output/(std::string(region.name)+"-nis.bmp"),find("nis-only-guarded"),region.rect);
    }
    if(!timing){r.reset();DestroyWindow(hwnd);g_cmaaNisRequest=-1;std::puts("DONE: HDR region quality comparison; no repeated GPU benchmark.");return;}
    // GPU time of the whole frame, OFF against the current guarded HIGH path,
    // paired AB/BA rounds after continuous warmup.
    std::ofstream csv(output/"gpu.csv");csv<<"round,mode,samples,mean_ms,p95_ms\n";
    Timer timer(r.device,r.backBufferFormat);
    for(int round=0;round<4;round++)for(int at=0;at<2;at++) {
        const bool enabled=(round&1)?at==0:at==1;
        g_cmaaNisRequest=enabled?llcv::upscaling::Request(true,15,false):-1;Check(r.configureVideoOutput(w,h),"timing mode");
        if(enabled)Access::ConfigureLegacyHdrCmaa(r.cmaaNisPipeline,r.device,false);
        const auto frame=[&](){if(enabled)RenderLegacyHdr(r,pixels,w*2);else Render(r,pixels,w*2);};
        const auto until=GetTickCount64()+500;unsigned warm=0;
        do {frame();if(++warm%32==0)Read(r);}while(GetTickCount64()<until);
        Read(r);std::vector<double> times;
        for(int i=0;i<120;i++){timer.Begin(r.context);frame();times.push_back(timer.End(r.context,r.backBuffer));}
        const double mean=std::accumulate(times.begin(),times.end(),0.0)/times.size();
        csv<<round<<','<<(enabled?"cmaa-nis-high-guarded":"off")<<','<<times.size()<<','<<mean<<','<<Percentile(times,.95)<<'\n';
        std::printf("round=%d %-22s mean=%.4f p95=%.4f ms\n",round,enabled?"cmaa-nis-high-guarded":"off",mean,Percentile(times,.95));
    }
    r.reset();DestroyWindow(hwnd);g_cmaaNisRequest=-1;
    std::puts("DONE: saved HDR frame quality comparison.");
}
// GPU time for OFF, current production NIS + guard, legacy CMAA2 -> NIS,
// and an independently driven NIS + guard reference. Legacy AA allocation
// and shaders exist only in this test, never in the production HDR path.
static void Timing(const std::filesystem::path& source,const std::filesystem::path& output) {
    using Access=llcv::upscaling::PipelineTestAccess;
    Require(!std::filesystem::exists(output),"new HDR timing output path");
    std::ifstream in(source,std::ios::binary);Require(bool(in),"saved P010 opens");
    const std::vector<BYTE> pixels{std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
    constexpr UINT w=1920,h=1080,ow=2560,oh=1440;
    Require(pixels.size()==size_t(w)*h*3,"saved P010 is FHD");
    std::filesystem::create_directories(output);
    HWND hwnd=Window(ow,oh);DirectD3D11Renderer r;g_cmaaNisRequest=-1;
    Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::P010,true),"saved HDR renderer");
    D3D11_SAMPLER_DESC sam{};sam.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sam.AddressU=sam.AddressV=sam.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sam.MaxLOD=D3D11_FLOAT32_MAX;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;Check(r.device->CreateSamplerState(&sam,&sampler),"guard sampler");
    enum Mode { Off, Production, ManualCmaaNis, ManualNis, ModeCount };
    const char* names[]{"off","production-nis-guard","legacy-cmaa-nis","manual-nis-only"};
    const auto frame=[&](Mode mode) {
        if(mode==Off || mode==Production) {Render(r,pixels,w*2);return;}
        r.upload(pixels.data(),w*2);
        D3D11_VIDEO_PROCESSOR_STREAM s{};s.Enable=TRUE;s.pInputSurface=r.inputViews[r.activeUploadSurface];
        auto& p=r.cmaaNisPipeline;
        Access::NativeForNis(p,r.context,r.videoContext,r.processor,s,ow,oh);
        ID3D11ShaderResourceView* input=mode==ManualCmaaNis?Access::CmaaOnly(p,r.context):nullptr;
        ID3D11ShaderResourceView* original=Access::NativeInput(p);
        r.context->PSSetShaderResources(1,1,&original);r.context->PSSetSamplers(0,1,sampler.GetAddressOf());
        Access::NisOnly(p,r.context,r.backBufferRenderTarget,input);
        ID3D11ShaderResourceView* empty=nullptr;r.context->PSSetShaderResources(1,1,&empty);
    };
    const auto configure=[&](Mode mode) {
        g_cmaaNisRequest=mode==Off?-1:llcv::upscaling::Request(true,15,false);
        Check(r.configureVideoOutput(w,h),"timing mode");
        Require(r.cmaaNisPipeline.Enabled()==(mode!=Off),"timing mode filter state");
        if(mode==ManualCmaaNis)Access::ConfigureLegacyHdrCmaa(r.cmaaNisPipeline,r.device,false);
    };
    // The simplified production path must reproduce the measured NIS-only path.
    configure(Production);frame(Production);const auto production=Read(r);
    frame(ManualNis);const auto nisOnly=Read(r);
    Require(Difference(nisOnly,production,true)<=1,"manual NIS + guard matches production");
    UINT nw=0,nh=0;const auto native=ReadHdrNative(r,nw,nh);
    Require(HdrContourViolations(native,nw,nh,nisOnly,ow,oh,{0,0,LONG(ow),LONG(oh)})==0,"NIS + guard keeps the contour bound");
    std::ofstream validation(output/"validation.csv");
    validation<<"production_manual_max_codes,production_glyph_nits,contour_violations\n"
        <<Difference(nisOnly,production,true)<<','<<HdrPixelNits(production,ow,1340,378)<<",0\n";
    std::ofstream csv(output/"gpu.csv");csv<<"round,mode,samples,mean_ms,median_ms,p95_ms\n";
    Timer timer(r.device,r.backBufferFormat);std::vector<std::vector<double>> all(ModeCount);
    constexpr int rounds=8;
    for(int round=0;round<rounds;round++)for(int at=0;at<ModeCount;at++) {
        // Rotate the order every round so drift is spread over all modes.
        const Mode mode=Mode((at+round)%ModeCount);configure(mode);
        const auto until=GetTickCount64()+500;unsigned warm=0;
        do {frame(mode);if(++warm%32==0)Read(r);}while(GetTickCount64()<until);
        Read(r);std::vector<double> times;
        for(int i=0;i<120;i++){timer.Begin(r.context);frame(mode);times.push_back(timer.End(r.context,r.backBuffer));}
        const double mean=std::accumulate(times.begin(),times.end(),0.0)/times.size();
        csv<<round<<','<<names[mode]<<','<<times.size()<<','<<mean<<','<<Percentile(times,.5)<<','<<Percentile(times,.95)<<'\n';
        all[mode].insert(all[mode].end(),times.begin(),times.end());
    }
    for(int mode=0;mode<ModeCount;mode++) {
        const auto& t=all[mode];const double mean=std::accumulate(t.begin(),t.end(),0.0)/t.size();
        std::printf("%-22s n=%zu mean=%.4f median=%.4f p95=%.4f ms\n",names[mode],t.size(),mean,Percentile(t,.5),Percentile(t,.95));
    }
    r.reset();DestroyWindow(hwnd);g_cmaaNisRequest=-1;
    std::puts("DONE: saved HDR frame NIS-only timing.");
}
static void Root(const std::filesystem::path& source,const std::filesystem::path& output) {
    using Access=llcv::upscaling::PipelineTestAccess;
    Require(!std::filesystem::exists(output),"new root comparison output");
    std::ifstream file(source,std::ios::binary);Require(bool(file),"root P010 source");
    const std::vector<BYTE> input{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    constexpr UINT w=1920,h=1080,ow=2560,oh=1440;
    Require(input.size()==size_t(w)*h*3,"root FHD P010");std::filesystem::create_directories(output);
    HWND hwnd=Window(ow,oh);DirectD3D11Renderer r;g_cmaaNisRequest=15;
    Check(r.initialize(hwnd,w,h,60,VideoPixelFormat::P010,true),"root HDR renderer");
    const auto guard=Access::DisableHdrGuard(r.cmaaNisPipeline,r.device);
    std::ofstream csv(output/"root.csv");csv<<"mode,sharp_min,sharp_scale,problem_pixel_nits\n";
    for(int mode=0;mode<3;mode++) {
        const float strength=mode==0?0.0f:.15f;
        const auto config=Access::NisParameters(r.cmaaNisPipeline,r.device,strength,mode==2);
        if(mode==0)Require(config.kSharpStrengthMin==0 && std::abs(config.kSharpStrengthScale-.625f)<1e-6,"HDR slider zero still has nonzero USM strength");
        if(mode==2)Require(config.kSharpStrengthMin==0 && config.kSharpStrengthScale==0 && config.kSharpStartY==.35f,"true zero USM retains PQ settings");
        r.upload(input.data(),w*2);D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=r.inputViews[r.activeUploadSurface];
        Access::NativeForNis(r.cmaaNisPipeline,r.context,r.videoContext,r.processor,stream,ow,oh);
        Access::NisOnly(r.cmaaNisPipeline,r.context,r.backBufferRenderTarget);
        const Image result{Read(r),ow,oh};
        const char* name=mode==0?"slider-zero":mode==1?"slider-0.15":"true-zero-USM";
        csv<<name<<','<<config.kSharpStrengthMin<<','<<config.kSharpStrengthScale<<','<<HdrPixelNits(result.pixels,ow,1340,378)<<'\n';
        SaveCrop(output/(std::string(name)+".bmp"),result,{1300,350,1400,410});
        if(mode==2) {
            const double nits=HdrPixelNits(result.pixels,ow,1340,378);
            Require(nits>10 && nits<25,"reported pixel with genuine zero USM retains scaling-only darkening");
        }
    }
    Access::RestoreHdrGuard(r.cmaaNisPipeline,guard);
    // Fixed monotone display-referred preview. This is not an HDR screenshot.
    g_cmaaNisRequest=-1;Check(r.configureVideoOutput(w,h),"root unfiltered preview");Render(r,input,w*2);
    SavePreview(output/"source-preview.bmp",{Read(r),ow,oh});
    r.reset();DestroyWindow(hwnd);g_cmaaNisRequest=-1;
    std::puts("PASS: HDR slider zero vs true zero-USM with PQ config retained.");
}
}
