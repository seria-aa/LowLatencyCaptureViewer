// Manual SDR image comparison only. No capture devices or user profiles.
namespace spatial_aa_comparison {
using namespace text_comparison;
using Microsoft::WRL::ComPtr;

struct Candidate {
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> prefilter,dlaa,fxaa,copy;
    ComPtr<ID3D11RenderTargetView> targets[2];
    ComPtr<ID3D11ShaderResourceView> views[2];
    ComPtr<ID3D11SamplerState> clamp,mirror;
    ComPtr<ID3D11Buffer> constants[6];
    explicit Candidate(ID3D11Device* device,const std::filesystem::path& shader) {
        auto code=[&](const char* entry,const char* profile,bool isDlaa) {
            const D3D_SHADER_MACRO macros[]={{"CANDIDATE_DLAA",isDlaa?"1":"0"},{nullptr,nullptr}};
            ComPtr<ID3DBlob> result,error;
            const HRESULT hr=D3DCompileFromFile(shader.c_str(),macros,D3D_COMPILE_STANDARD_FILE_INCLUDE,
                entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&result,&error);
            if(error)std::fprintf(stderr,"%s\n",static_cast<const char*>(error->GetBufferPointer()));
            Check(hr,"compile spatial AA candidate");return result;
        };
        auto vs=code("VS_Quad","vs_5_0",false);
        Check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex),"candidate vertex");
        auto pixel=[&](const char* entry,bool isDlaa,ComPtr<ID3D11PixelShader>& target) {
            auto ps=code(entry,"ps_5_0",isDlaa);
            Check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&target),"candidate pixel");
        };
        pixel("PS_Prefilter",true,prefilter);pixel("PS_Main",true,dlaa);
        pixel("PS_Main",false,fxaa);pixel("PS_Copy",false,copy);
        for(int i=0;i<2;++i) {
            D3D11_TEXTURE2D_DESC d{};d.Width=W;d.Height=H;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
            d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> texture;
            Check(device->CreateTexture2D(&d,nullptr,&texture),"candidate texture");
            Check(device->CreateRenderTargetView(texture.Get(),nullptr,&targets[i]),"candidate RTV");
            Check(device->CreateShaderResourceView(texture.Get(),nullptr,&views[i]),"candidate SRV");
        }
        D3D11_SAMPLER_DESC s{};s.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;s.MaxLOD=D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&s,&clamp),"candidate clamp sampler");
        s.AddressU=s.AddressV=s.AddressW=D3D11_TEXTURE_ADDRESS_MIRROR;
        Check(device->CreateSamplerState(&s,&mirror),"candidate mirror sampler");
        const float strengths[]={0,1,1,.5f,.25f,0};
        for(int i=0;i<6;++i) {
            const float values[4]={1.f/W,1.f/H,strengths[i],0};
            D3D11_BUFFER_DESC b{};b.ByteWidth=sizeof(values);b.Usage=D3D11_USAGE_IMMUTABLE;b.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA data{values,0,0};
            Check(device->CreateBuffer(&b,&data,&constants[i]),"candidate constants");
        }
    }
    ID3D11ShaderResourceView* Draw(DirectD3D11Renderer& r,int mode) {
        // NativeForNis establishes the same SDR VP input and rasterizer as NIS-only.
        auto* c=r.context;
        D3D11_VIEWPORT viewport{0,0,float(W),float(H),0,1};c->RSSetViewports(1,&viewport);
        Require(mode>=0 && mode<6,"candidate strength index");
        c->VSSetShader(vertex.Get(),nullptr,0);c->PSSetConstantBuffers(0,1,constants[mode].GetAddressOf());
        ID3D11SamplerState* samplers[]={clamp.Get(),mirror.Get()};c->PSSetSamplers(0,2,samplers);
        auto pass=[&](ID3D11PixelShader* ps,int target,bool withPrefilter) {
            ID3D11ShaderResourceView* inputs[]={Access::NativeInput(r.cmaaNisPipeline),withPrefilter?views[0].Get():nullptr};
            c->OMSetRenderTargets(1,targets[target].GetAddressOf(),nullptr);
            c->PSSetShaderResources(0,2,inputs);c->PSSetShader(ps,nullptr,0);c->Draw(3,0);
            ID3D11ShaderResourceView* empty[2]{};c->PSSetShaderResources(0,2,empty);c->OMSetRenderTargets(0,nullptr,nullptr);
        };
        if(mode!=0 && mode!=2) {pass(prefilter.Get(),0,false);pass(dlaa.Get(),1,true);}
        else pass(mode==2?fxaa.Get():copy.Get(),1,false);
        return views[1].Get();
    }
};

static void Run(const std::filesystem::path& shader,const std::filesystem::path& output,
                const std::filesystem::path& savedFrame={},bool soft=false) {
    Require(llcv::test_profile::Enabled(),"candidate images require isolated profile");
    Require(!std::filesystem::exists(output),"fresh candidate image directory");std::filesystem::create_directories(output);
    Gdiplus::GdiplusStartupInput startup;ULONG_PTR token=0;
    Require(Gdiplus::GdiplusStartup(&token,&startup,nullptr)==Gdiplus::Ok,"candidate image startup");
    Pixels snapshot;
    if(!savedFrame.empty()) {
        std::ifstream file(savedFrame,std::ios::binary);Require(bool(file),"saved game snapshot opens");
        snapshot.assign(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
        Require(snapshot.size()==size_t(W)*H*3/2,"saved snapshot is FHD NV12");
    }
    const auto master=snapshot.empty()?Master():Pixels{};
    std::ofstream metrics(output/"metrics.csv"),metadata(output/"method.txt");
    metrics<<"input,width,height,mode,region,mae_to_vector_reference,psnr_to_vector_reference_db,max_reference_delta,mae_to_nis,pixels_delta_from_nis_over16\n";
    metrics<<std::setprecision(9);
    metadata<<(snapshot.empty()?"Synthetic grayscale FHD NV12 SDR":"Saved color FHD NV12 SDR")
            <<", BT.709 Limited -> QHD/4K. NIS=0.15 throughout.\n"
            <<"cDLAA: defaults (short scale 3, short threshold 1/8, long threshold 1/6), Retain Fine Details=true.\n"
            <<(soft?"cDLAA correction strengths 1.0 / 0.5 / 0.25 before NIS; preserve CMAA2 uses ExtraSharpness=1.\n"
                    :"cFXAA: upstream default thresholds (1/16,1/6); subpixel strength=0.25 (very low), instead of default 0.75.\n")
            <<"CShade upstream pin 1e9cc77e1c2d30c22ff44a1b347747011c923954; D3D11 plumbing only; gamma-domain SDR.\n"
            <<"Native prefilter and candidate output RGBA8; source sampler clamp, intermediate sampler mirror.\n"
            <<(snapshot.empty()?"Same 4x master as prior text-AA study; box-reduced AA or point-sampled aliased input; reference from same master.\n"
                               :"Previously captured game still; no synthetic sampling or high-resolution reference.\n")
            <<"Manual quality comparison, not proof of performance/display latency or real-game temporal stability.\n";
    if(!snapshot.empty())metadata<<"Saved game frame: "<<savedFrame.generic_string()<<"; no high-resolution reference available; no reference metrics.\n";
    for(bool aliased:{false,true}) {
        if(!snapshot.empty() && aliased)continue;
        const char* scene=!snapshot.empty()?"game":aliased?"aliased":"antialiased";
        const auto source=snapshot.empty()?Sample(master,W,H,aliased):Pixels{};
        const auto input=snapshot.empty()?Nv12(source):snapshot,unchanged=input;
        if(snapshot.empty())SavePng(output/(std::string(scene)+"-source.png"),W,H,source);
        for(SIZE size:{SIZE{2560,1440},SIZE{3840,2160}}) {
            const UINT w=UINT(size.cx),h=UINT(size.cy);const std::string prefix=std::string(scene)+"-"+std::to_string(w);
            const auto reference=snapshot.empty()?Sample(master,w,h):Pixels{};
            HWND hwnd=Window(w,h);g_cmaaNisRequest=-1;DirectD3D11Renderer r;
            llcv::video_color::Configuration color{};color.range=llcv::video_color::Range::Limited;color.matrix=llcv::video_color::Matrix::Bt709;
            Check(r.initialize(hwnd,W,H,60,VideoPixelFormat::Nv12,false,color),"candidate renderer");
            D3D11_TEXTURE2D_DESC desc{};r.backBuffer->GetDesc(&desc);Require(desc.Width==w && desc.Height==h,"candidate output size");
            Render(r,input,W);const auto off=Read(r);
            g_cmaaNisRequest=15;Check(r.configureVideoOutput(W,H),"candidate NIS enable");
            Require(r.cmaaNisPipeline.Enabled(),"shipping NIS active for candidates");
            if(soft) {
                const auto cmaaShader=shader.parent_path().parent_path().parent_path()/"third_party/upscaling/cmaa2/CMAA2.hlsl";
                Access::Programs(r.cmaaNisPipeline,Compile(r.device,cmaaShader,true));
            }
            Render(r,input,W);const auto cmaa=Read(r);
            Require(Access::Overflow(r.cmaaNisPipeline,r.device,r.context)==0,"CMAA baseline not bypassed");
            NisFrame(r,input,w,h);const auto nis=Read(r);
            Candidate candidate(r.device,shader);
            auto draw=[&](const Pixels& frame,int mode) {
                r.upload(frame.data(),W);D3D11_VIDEO_PROCESSOR_STREAM stream{};
                stream.Enable=TRUE;stream.pInputSurface=r.inputViews[r.activeUploadSurface];
                Access::NativeForNis(r.cmaaNisPipeline,r.context,r.videoContext,r.processor,stream,w,h);
                auto* view=candidate.Draw(r,mode);Access::NisOnly(r.cmaaNisPipeline,r.context,r.backBufferRenderTarget,view);
            };
            draw(input,0);Require(Difference(Read(r),nis)<=1,"identity AA adapter matches true NIS-only");
            draw(input,5);Require(Difference(Read(r),nis)<=1,"zero-strength cDLAA matches NIS-only");
            const auto native=ReadSdrView(r,Access::NativeInput(r.cmaaNisPipeline));
            if(!snapshot.empty() && w==2560)SavePng(output/"game-source.png",W,H,native);
            std::vector<int> modes=soft?std::vector<int>{1,3,4}:std::vector<int>{1,2};
            std::vector<Pixels> candidates(modes.size());Pixels fullNative;
            for(size_t at=0;at<modes.size();++at) {
                const int mode=modes[at];draw(input,mode);candidates[at]=Read(r);
                Require(Difference(candidates[at],nis)>1,"candidate performs real AA, not a bypass");
                if(w==2560) {
                    const char* suffix=mode==1?"dlaa":mode==2?"fxaa":mode==3?"dlaa50":"dlaa25";
                    const std::string name=std::string(scene)+"-native-"+suffix;
                    const auto aa=ReadSdrView(r,candidate.views[1].Get());
                    SavePng(output/(name+".png"),W,H,aa);Crop(output/(name+"-text.png"),W,H,aa,Focus);
                    Require(Difference(aa,native)>1,"native candidate modifies edges before NIS");
                    if(soft) {
                        if(at==0)fullNative=aa;
                        else for(size_t p=0;p<aa.size();++p) {
                            const float strength=mode==3?.5f:.25f;
                            const int expected=int(std::lround(native[p]*(1-strength)+fullNative[p]*strength));
                            Require(std::abs(int(aa[p])-expected)<=1,"soft cDLAA matches bounded pre-NIS interpolation");
                        }
                    }
                }
            }
            // Flat -> complex -> flat and alternating modes catch accidental history/stale RTV usage.
            Pixels flat(size_t(W)*H*3/2,128);std::fill(flat.begin(),flat.begin()+size_t(W)*H,BYTE(105));
            NisFrame(r,flat,w,h);const auto flatBaseline=Read(r);
            for(int round=0;round<2;++round)for(size_t at=0;at<modes.size();++at) {
                const int mode=modes[at];
                draw(flat,mode);Require(Difference(Read(r),flatBaseline)<=1,"flat frame remains flat");
                draw(input,mode);Require(Difference(Read(r),candidates[at])<=1,"candidate switching is stable");
                draw(flat,mode);Require(Difference(Read(r),flatBaseline)<=1,"complex-to-flat frame has no residue");
            }
            std::vector<const char*> names={"off","nis",soft?"cmaa-preserve":"cmaa-current","dlaa",soft?"dlaa50":"fxaa"};
            std::vector<const Pixels*> images={&off,&nis,&cmaa,&candidates[0],&candidates[1]};
            if(soft){names.push_back("dlaa25");images.push_back(&candidates[2]);}
            const size_t rendered=images.size();
            if(snapshot.empty()){names.push_back("reference");images.push_back(&reference);}
            for(size_t i=0;i<images.size();++i) {
                SavePng(output/(prefix+"-"+names[i]+".png"),w,h,*images[i]);
                Crop(output/(prefix+"-"+names[i]+"-text.png"),w,h,*images[i],Focus);
                if(snapshot.empty() && i<rendered)Metrics(metrics,scene,w,h,names[i],*images[i],reference,nis);
                if(!snapshot.empty()) {
                    Crop(output/(prefix+"-"+names[i]+"-game-text.png"),w,h,*images[i],Roi{"game-text",728,500,480,236});
                    Crop(output/(prefix+"-"+names[i]+"-detail.png"),w,h,*images[i],Roi{"game-detail",552,108,528,300});
                }
            }
            Require(input==unchanged,"candidate recording/screenshot input unchanged");
            g_cmaaNisRequest=-1;Check(r.configureVideoOutput(W,H),"candidate OFF restore");Render(r,input,W);
            Require(Read(r)==off,"candidate OFF restores original output exactly");
            metadata<<prefix<<" adapter="<<r.adapterVendor<<" identity/flat/switching/source/OFF checks passed\n";
            std::printf("PASS: %s %s images + zero/identity/flat/switch/source/OFF\n",prefix.c_str(),soft?"cDLAA100/50/25/NIS/preserve-CMAA":"cDLAA/FXAA/NIS/CMAA");
            r.reset();DestroyWindow(hwnd);
        }
    }
    Require(bool(metrics)&&bool(metadata),"candidate report written");
    Gdiplus::GdiplusShutdown(token);
}
} // namespace spatial_aa_comparison
