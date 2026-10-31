// Manual diagnostic only. Uses the shipping renderer/NIS and shader source;
// does not enumerate capture hardware, load settings or modify release filters.
#include <gdiplus.h>
#include <iomanip>

namespace text_comparison {
constexpr UINT W=1920,H=1080,SS=4;
using Pixels=std::vector<BYTE>;
using Access=llcv::upscaling::PipelineTestAccess;
struct Roi {const char* name;int x,y,w,h;};
constexpr Roi Regions[]={
    {"text-dark",80,148,800,300},{"text-light",1024,148,800,300},
    {"diagonal",80,600,520,260},{"curves",704,600,520,260},
    {"texture",1328,600,500,260},{"low-contrast",80,928,1740,100}
};
constexpr Roi Focus{"focus",72,148,528,152};

static CLSID PngEncoder() {
    UINT count=0,bytes=0;
    Require(Gdiplus::GetImageEncodersSize(&count,&bytes)==Gdiplus::Ok,"PNG encoders");
    std::vector<BYTE> buffer(bytes);
    auto* codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Require(Gdiplus::GetImageEncoders(count,bytes,codecs)==Gdiplus::Ok,"PNG codec list");
    for(UINT i=0;i<count;++i)if(!wcscmp(codecs[i].MimeType,L"image/png"))return codecs[i].Clsid;
    Require(false,"PNG encoder exists");return {};
}
static void SavePng(const std::filesystem::path& path,UINT w,UINT h,const Pixels& pixels) {
    Require(pixels.size()==size_t(w)*h*4,"PNG dimensions");
    Gdiplus::Bitmap image(INT(w),INT(h),INT(w*4),PixelFormat32bppARGB,const_cast<BYTE*>(pixels.data()));
    const auto codec=PngEncoder();Require(image.Save(path.c_str(),&codec,nullptr)==Gdiplus::Ok,"PNG save");
}
static Pixels Master() {
    Pixels pixels(size_t(W)*H*SS*SS*4,255);
    Gdiplus::Bitmap image(INT(W*SS),INT(H*SS),INT(W*SS*4),PixelFormat32bppARGB,pixels.data());
    Gdiplus::Graphics g(&image);g.ScaleTransform(float(SS),float(SS));
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.Clear(Gdiplus::Color(255,48,48,48));
    Gdiplus::FontFamily family(L"Malgun Gothic");
    Require(family.GetLastStatus()==Gdiplus::Ok,"Korean fixture font available");
    auto rect=[&](int x,int y,int w,int h,BYTE shade) {
        Gdiplus::SolidBrush b(Gdiplus::Color(255,shade,shade,shade));g.FillRectangle(&b,x,y,w,h);
    };
    auto text=[&](const wchar_t* s,float x,float y,float size,BYTE shade,int style=Gdiplus::FontStyleRegular) {
        Gdiplus::GraphicsPath path;
        Require(path.AddString(s,-1,&family,style,size,Gdiplus::PointF(x,y),
                    Gdiplus::StringFormat::GenericTypographic())==Gdiplus::Ok,"fixture glyph paths");
        Gdiplus::SolidBrush b(Gdiplus::Color(255,shade,shade,shade));g.FillPath(&b,&path);
    };
    text(L"문자 획 · 얇은 선 · 곡선 / FHD 원본",48,30,36,235);
    for(int side=0;side<2;++side) {
        const int x=48+944*side;const BYTE bg=side?232:16,fg=side?24:232;
        rect(x,128,880,360,bg);
        text(L"팔러 왔어요 · 그래픽 설정",float(x+32),158,34,fg,Gdiplus::FontStyleBold);
        text(L"저장하기 / 취소 / 게임을 시작합니다",float(x+32),218,24,fg);
        text(L"1234567890  AVMW  한글 획 ㅁㅍㅎ",float(x+32),270,18,fg);
        text(L"일시정지 · 다시 시작 · 음량 100%",float(x+32),312,28,fg);
        text(L"작은 글씨: 가갸거겨고교구규그기",float(x+32),365,14,fg);
        text(L"RGB / SDR · 1080p → QHD / 4K",float(x+32),405,20,fg);
        text(L"팔러 왔어요",float(x+620),220,26,fg,Gdiplus::FontStyleItalic);
        text(L"그래픽",float(x+620),274,34,fg,Gdiplus::FontStyleBold);
    }
    for(int i=0;i<3;++i)rect(48+624*i,544,592,344,24);
    text(L"사선 / 1·2·3 px",80,554,22,225);
    for(int i=0;i<8;++i) {
        Gdiplus::Pen pen(Gdiplus::Color(255,225,225,225),float(1+i%3));
        const float y=610.f+i*25;
        g.DrawLine(&pen,88.f,y,575.f,y+float(6+i*4));
    }
    text(L"곡선 / 원 / 둥근 모서리",704,554,22,225);
    for(int i=0;i<4;++i) {
        Gdiplus::Pen pen(Gdiplus::Color(255,220,220,220),float(1+i%3));
        g.DrawEllipse(&pen,720.f+i*108,615.f+i*6,94.f,180.f-i*22);
    }
    text(L"반복 무늬 / 세부 패턴",1328,554,22,225);
    for(int y=610;y<850;y+=8)for(int x=1328;x<1820;x+=8)
        rect(x,y,8,8,BYTE(((x/8+y/8)&1)?190:48));
    rect(48,912,1824,120,105);
    text(L"낮은 대비: 팔러 왔어요 · 그래픽 설정 · 작은 글씨의 획과 간격",80,932,28,154);
    text(L"같은 4배 벡터 원본에서 입력·참조를 생성한 합성 자료",80,977,18,154);
    g.Flush(Gdiplus::FlushIntentionSync);return pixels;
}
static Pixels Sample(const Pixels& master,UINT w,UINT h,bool point=false) {
    const UINT scale=W*SS/w;
    Require(w*scale==W*SS && h*scale==H*SS,"integer reference reduction");
    Pixels output(size_t(w)*h*4,255);
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x) {
        unsigned sum=0;
        if(point)sum=master[(size_t(y*scale+scale/2)*W*SS+x*scale+scale/2)*4];
        else {
            for(UINT sy=0;sy<scale;++sy)for(UINT sx=0;sx<scale;++sx)
                sum+=master[(size_t(y*scale+sy)*W*SS+x*scale+sx)*4];
            sum=(sum+scale*scale/2)/(scale*scale);
        }
        for(int c=0;c<3;++c)output[(size_t(y)*w+x)*4+c]=BYTE(sum);
    }
    return output;
}
static Pixels Nv12(const Pixels& pixels) {
    Pixels output(size_t(W)*H*3/2,128);
    for(size_t i=0;i<size_t(W)*H;++i)output[i]=BYTE(16+(219u*pixels[i*4]+127)/255);
    return output;
}
static void Crop(const std::filesystem::path& path,UINT w,UINT h,const Pixels& image,const Roi& roi) {
    const int x=int(uint64_t(roi.x)*w/W),y=int(uint64_t(roi.y)*h/H);
    const UINT cw=UINT(uint64_t(roi.w)*w/W),ch=UINT(uint64_t(roi.h)*h/H);
    Require(x>=0 && y>=0 && x+cw<=w && y+ch<=h,"crop bounds");
    Pixels pixels(size_t(cw)*ch*4);
    for(UINT row=0;row<ch;++row)
        std::memcpy(pixels.data()+size_t(row)*cw*4,image.data()+(size_t(y+row)*w+x)*4,size_t(cw)*4);
    SavePng(path,cw,ch,pixels);
}
static Access::CmaaPrograms Compile(ID3D11Device* device,const std::filesystem::path& file,bool preserve) {
    const D3D_SHADER_MACRO macros[]={
        {"CMAA2_STATIC_QUALITY_PRESET","2"},{"CMAA2_EXTRA_SHARPNESS",preserve?"1":"0"},
        {"CMAA2_EDGE_DETECTION_LUMA_PATH","0"},{"CMAA2_USE_HALF_FLOAT_PRECISION","0"},
        {"CMAA2_COLLECT_EXPAND_BLEND_ITEMS","0"},{"CMAA2_UAV_STORE_TYPED","1"},
        {"CMAA2_UAV_STORE_TYPED_UNORM_FLOAT","0"},{"CMAA2_UAV_STORE_CONVERT_TO_SRGB","0"},
        {"CMAA2_SUPPORT_HDR_COLOR_RANGE","0"},{nullptr,nullptr}
    };
    const char* entries[]={"EdgesColor2x2CS","ComputeDispatchArgsCS","ProcessCandidatesCS","DeferredColorApply2x2CS"};
    Access::CmaaPrograms programs;
    for(unsigned i=0;i<4;++i) {
        Microsoft::WRL::ComPtr<ID3DBlob> code,error;
        const HRESULT hr=D3DCompileFromFile(file.c_str(),macros,D3D_COMPILE_STANDARD_FILE_INCLUDE,
                entries[i],"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
        if(FAILED(hr) && error)std::fprintf(stderr,"%s\n",static_cast<const char*>(error->GetBufferPointer()));
        Check(hr,"compile diagnostic CMAA variant");
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&programs[i]),"diagnostic shader");
    }
    return programs;
}
static void NisFrame(DirectD3D11Renderer& r,const Pixels& input,UINT ow,UINT oh) {
    r.upload(input.data(),W);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=r.inputViews[r.activeUploadSurface];
    Access::NativeForNis(r.cmaaNisPipeline,r.context,r.videoContext,r.processor,stream,ow,oh);
    Access::NisOnly(r.cmaaNisPipeline,r.context,r.backBufferRenderTarget);
}
static void Metrics(std::ofstream& csv,const char* input,UINT w,UINT h,const char* mode,
                    const Pixels& image,const Pixels& reference,const Pixels& nis) {
    for(const auto& roi:Regions) {
        const UINT left=UINT(uint64_t(roi.x)*w/W),top=UINT(uint64_t(roi.y)*h/H);
        const UINT rw=UINT(uint64_t(roi.w)*w/W),rh=UINT(uint64_t(roi.h)*h/H);
        double absolute=0,squared=0,nisAbsolute=0;unsigned maximum=0,changed=0;
        for(UINT y=top;y<top+rh;++y)for(UINT x=left;x<left+rw;++x) {
            const size_t at=(size_t(y)*w+x)*4;unsigned pixelDelta=0;
            for(int c=0;c<3;++c) {
                const unsigned delta=unsigned(std::abs(int(image[at+c])-int(reference[at+c])));
                absolute+=delta;squared+=double(delta)*delta;maximum=std::max(maximum,delta);
                const unsigned fromNis=unsigned(std::abs(int(image[at+c])-int(nis[at+c])));
                nisAbsolute+=fromNis;pixelDelta=std::max(pixelDelta,fromNis);
            }
            changed+=pixelDelta>16;
        }
        const double count=double(rw)*rh*3,mse=squared/count;
        csv<<input<<','<<w<<','<<h<<','<<mode<<','<<roi.name<<','<<absolute/count<<','
           <<(mse?10*std::log10(255.0*255/mse):100)<<','<<maximum<<','<<nisAbsolute/count<<','<<changed<<'\n';
    }
}
static void Run(const std::filesystem::path& shader,const std::filesystem::path& output) {
    Require(llcv::test_profile::Enabled(),"synthetic comparison uses isolated profile");
    Require(!std::filesystem::exists(output),"fresh comparison directory");std::filesystem::create_directories(output);
    Gdiplus::GdiplusStartupInput startup;ULONG_PTR token=0;
    Require(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"fixture graphics startup");
    const auto master=Master();
    std::ofstream metrics(output/"metrics.csv"),timings(output/"gpu-times.csv");
    metrics<<"input,width,height,mode,region,mae_to_vector_reference,psnr_to_vector_reference_db,max_reference_delta,mae_to_nis,pixels_delta_from_nis_over16\n";
    timings<<"input,width,height,mode,samples,mean_gpu_ms,p95_gpu_ms\n";
    metrics<<std::setprecision(9);timings<<std::setprecision(9);
    std::ofstream metadata(output/"method.txt");
    metadata<<"FHD grayscale NV12, BT.709 Limited, NIS 0.15. Font: Malgun Gothic regular/bold/italic glyph outlines.\n"
            <<"One 7680x4320 vector master; AA input = 4x4 box reduction, aliased input = center point sampling.\n"
            <<"QHD reference = 3x3 box reduction; 4K reference = 2x2 box reduction of the SAME master.\n"
            <<"Reference is synthetic, not a high-resolution capture of a real game. Metrics do not rank all game quality.\n"
            <<"GPU timestamps include upload/processing/output dependency, not physical display latency. No Present or capture device.\n";
    for(const bool aliased:{false,true}) {
        const char* scene=aliased?"aliased":"antialiased";
        const auto source=Sample(master,W,H,aliased);const auto input=Nv12(source),unchanged=input;
        SavePng(output/(std::string(scene)+"-source.png"),W,H,source);
        for(const SIZE size:{SIZE{2560,1440},SIZE{3840,2160}}) {
            const UINT w=UINT(size.cx),h=UINT(size.cy);const std::string prefix=std::string(scene)+"-"+std::to_string(w);
            const auto reference=Sample(master,w,h);
            SavePng(output/(prefix+"-reference.png"),w,h,reference);
            Crop(output/(prefix+"-reference-text.png"),w,h,reference,Focus);
            HWND hwnd=Window(w,h);g_cmaaNisRequest=-1;DirectD3D11Renderer r;
            llcv::video_color::Configuration color{};color.range=llcv::video_color::Range::Limited;color.matrix=llcv::video_color::Matrix::Bt709;
            Check(r.initialize(hwnd,W,H,60,VideoPixelFormat::Nv12,false,color),"synthetic renderer");
            D3D11_TEXTURE2D_DESC desc{};r.backBuffer->GetDesc(&desc);Require(desc.Width==w && desc.Height==h,"exact output size");
            Render(r,input,W);const auto off=Read(r);
            g_cmaaNisRequest=15;Check(r.configureVideoOutput(W,H),"synthetic filter enable");
            Require(r.cmaaNisPipeline.Enabled(),"synthetic production filter active");
            const auto original=Access::Programs(r.cmaaNisPipeline);
            const auto compiled=Compile(r.device,shader,false),preserving=Compile(r.device,shader,true);
            Render(r,input,W);const auto current=Read(r);
            Require(Access::Overflow(r.cmaaNisPipeline,r.device,r.context)==0,"fixture never uses overflow AA bypass");
            if(w==2560) {
                const auto native=ReadSdrView(r,Access::NativeInput(r.cmaaNisPipeline));
                const auto aa=ReadSdrView(r,Access::CmaaOnly(r.cmaaNisPipeline,r.context));
                SavePng(output/(std::string(scene)+"-native-vp.png"),W,H,native);
                SavePng(output/(std::string(scene)+"-native-cmaa-current.png"),W,H,aa);
                Crop(output/(std::string(scene)+"-native-vp-text.png"),W,H,native,Focus);
                Crop(output/(std::string(scene)+"-native-cmaa-current-text.png"),W,H,aa,Focus);
            }
            Access::NisOnly(r.cmaaNisPipeline,r.context,r.backBufferRenderTarget);const auto nis=Read(r);
            NisFrame(r,input,w,h);Require(Read(r)==nis,"true NIS-only agrees with isolated NIS stage");
            Access::Programs(r.cmaaNisPipeline,compiled);Render(r,input,W);
            Require(Difference(Read(r),current)<=1,"runtime compiler matches shipping CMAA macros");
            Access::Programs(r.cmaaNisPipeline,preserving);Render(r,input,W);const auto preserve=Read(r);
            Require(Access::Overflow(r.cmaaNisPipeline,r.device,r.context)==0,"preserving variant never bypasses AA");
            if(w==2560) {
                const auto aa=ReadSdrView(r,Access::CmaaOnly(r.cmaaNisPipeline,r.context));
                SavePng(output/(std::string(scene)+"-native-cmaa-preserve.png"),W,H,aa);
                Crop(output/(std::string(scene)+"-native-cmaa-preserve-text.png"),W,H,aa,Focus);
            }
            const char* modes[]={"off","nis","cmaa-current","cmaa-preserve"};
            const Pixels* images[]={&off,&nis,&current,&preserve};
            for(int mode=0;mode<4;++mode) {
                SavePng(output/(prefix+"-"+modes[mode]+".png"),w,h,*images[mode]);
                Crop(output/(prefix+"-"+modes[mode]+"-text.png"),w,h,*images[mode],Focus);
                Metrics(metrics,scene,w,h,modes[mode],*images[mode],reference,nis);
            }
            auto select=[&](int mode) {
                if(mode==0) {g_cmaaNisRequest=-1;Check(r.configureVideoOutput(W,H),"benchmark OFF");}
                else {
                    g_cmaaNisRequest=15;Check(r.configureVideoOutput(W,H),"benchmark ON");
                    Access::Programs(r.cmaaNisPipeline,mode==3?preserving:original);
                }
            };
            auto draw=[&](int mode) {if(mode==1)NisFrame(r,input,w,h);else Render(r,input,W);};
            std::vector<double> costs[4];Timer timer(r.device);
            for(int round=0;round<3;++round)for(int at=0;at<4;++at) {
                const int mode=(round&1)?3-at:at;select(mode);
                for(int i=0;i<4;++i)draw(mode);
                Require(Difference(Read(r),*images[mode])<=1,"variant output stable after switching");
                for(int i=0;i<16;++i) {
                    timer.Begin(r.context);draw(mode);costs[mode].push_back(timer.End(r.context,r.backBuffer));
                }
            }
            for(int mode=0;mode<4;++mode)timings<<scene<<','<<w<<','<<h<<','<<modes[mode]<<','<<costs[mode].size()<<','
                <<std::accumulate(costs[mode].begin(),costs[mode].end(),0.0)/costs[mode].size()<<','<<Percentile(costs[mode],.95)<<'\n';
            Require(input==unchanged,"source recording/screenshot data untouched");
            g_cmaaNisRequest=-1;Check(r.configureVideoOutput(W,H),"final OFF restoration");Render(r,input,W);
            Require(Read(r)==off,"OFF restores exact original output");
            metadata<<prefix<<" adapter_vendor="<<r.adapterVendor<<"; parity, no-overflow, switching, source/OFF restoration passed\n";
            r.reset();DestroyWindow(hwnd);
            std::printf("PASS: %s four modes, compiler parity, no overflow, source/OFF restored\n",prefix.c_str());
        }
    }
    Require(bool(metrics)&&bool(timings)&&bool(metadata),"comparison reports written");
    Gdiplus::GdiplusShutdown(token);
}
} // namespace text_comparison
static void SyntheticTextComparison(const std::filesystem::path& shader,const std::filesystem::path& output) {
    text_comparison::Run(shader,output);
}
