// Runs on the renderer's CUDA stream. Packageable builds use the official API only.
struct VfgLive {
#if !defined(LLCV_INTERPOLATION_RELEASE) || defined(LLCV_VFG_AMPERE_COMPAT)
    VfgAmpereCompat compat;
#endif
    HMODULE libs[4]{};
    void* effect=nullptr;
    void* image[3]{}; void* rgba[3]{};
    VfgInputCache inputCache;
    bool reuseInputs=false;
    using FnSet=int(__cdecl*)(void*,const char*,unsigned);
    using FnImage=int(__cdecl*)(void*,const char*,void*);
    FnSet set=nullptr; FnImage bind=nullptr;
    int(__cdecl* run)(void*,int)=nullptr;
    void(__cdecl* destroy)(void*)=nullptr;
    void(__cdecl* freeImage)(void*)=nullptr;
    unsigned w=0,h=0;
    static void Verify(int code,const char* where){if(code)throw std::runtime_error(std::string(where)+": "+std::to_string(code));}
    void Init(const std::filesystem::path& dir,int adapter,unsigned width,unsigned height,bool medium,cudaStream_t stream){
        w=width;h=height;reuseInputs=VfgInputReuseEnabled(medium);
        cudaDeviceProp prop{};Cu(cudaGetDeviceProperties(&prop,adapter),"VFG device");
#ifdef LLCV_INTERPOLATION_RELEASE
        if(!ConfiguredVfgArchitecture(prop.major,prop.minor)){
            fallbackDetail.store(L"Unsupported NVIDIA VFG GPU architecture");
            throw std::runtime_error("GPU does not support official VFG");
        }
#endif
        const wchar_t* names[]={L"NVCVImage.dll",L"nvngxruntime.dll",L"NVVideoEffects.dll",L"nvVFXVideoFrameGeneration.dll"};
        for(int i=0;i<4;++i){libs[i]=LoadLibraryExW((dir/names[i]).c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);Check(libs[i]!=nullptr,"VFG components missing");}
#if !defined(LLCV_INTERPOLATION_RELEASE) || defined(LLCV_VFG_AMPERE_COMPAT)
        if(prop.major==8&&prop.minor==6)compat.Install(libs[3],adapter,dir/names[3]);
#endif
        auto core=libs[2],images=libs[0];
        destroy=Bind<decltype(destroy)>(core,"NvVFX_DestroyEffect");set=Bind<FnSet>(core,"NvVFX_SetU32");
        bind=Bind<FnImage>(core,"NvVFX_SetImage");run=Bind<decltype(run)>(core,"NvVFX_Run");
        auto createImage=Bind<int(__cdecl*)(unsigned,unsigned,int,int,unsigned,unsigned,unsigned,void**)>(images,"NvCVImage_Create");
        auto initImage=Bind<int(__cdecl*)(void*,unsigned,unsigned,int,void*,int,int,unsigned,unsigned)>(images,"NvCVImage_Init");
        auto dealloc=Bind<void(__cdecl*)(void*)>(images,"NvCVImage_Dealloc");
        freeImage=Bind<decltype(freeImage)>(images,"NvCVImage_Destroy");
        for(int i=0;i<3;++i){
            Cu(cudaMalloc(&rgba[i],size_t(w)*h*4),"VFG RGBA allocation");
            Verify(createImage(1,1,6,1,0,0,1,&image[i]),"VFG image create");dealloc(image[i]);
            Verify(initImage(image[i],w,h,int(w*4),rgba[i],6,1,0,1),"VFG RGBA descriptor");
        }
        Verify(Bind<int(__cdecl*)(const char*,void**)>(core,"NvVFX_CreateEffect")("VideoFrameGeneration",&effect),"VFG create");
        Verify(Bind<int(__cdecl*)(void*,const char*,cudaStream_t)>(core,"NvVFX_SetCudaStream")(effect,"CudaStream",stream),"VFG stream");
        Verify(set(effect,"InputWidth",w),"VFG width");Verify(set(effect,"InputHeight",h),"VFG height");
        Verify(set(effect,"Mode",medium?1:0),"VFG quality");Verify(set(effect,"AutomaticShotChangeDetectionEnabled",1),"VFG scene detection");
        Verify(bind(effect,"SrcImage0",image[0]),"VFG input");Verify(bind(effect,"SrcImage1",image[1]),"VFG input");
        Verify(Bind<int(__cdecl*)(void*)>(core,"NvVFX_Load")(effect),"VFG load");
        Verify(bind(effect,"DstImage0",image[2]),"VFG output");Verify(set(effect,"FrameMultiplier",2),"VFG multiplier");
    }
    void Process(const std::array<cudaArray_t,2>& input,const std::array<uint64_t,2>& generations,
                 unsigned current,cudaArray_t out,cudaStream_t stream){
        // Both linear buffers follow their shared texture slots. The render
        // fence retires the preceding job before either buffer can be reused.
        for(unsigned i=0;i<2;++i){
            const unsigned source=reuseInputs?i:(i?current:1-current);
            if(reuseInputs&&!inputCache.NeedsCopy(source,generations[source]))continue;
            Cu(cudaMemcpy2DFromArrayAsync(rgba[i],w*4,input[source],0,0,w*4,h,cudaMemcpyDeviceToDevice,stream),"VFG input transfer");
            if(reuseInputs)inputCache.MarkCopied(source,generations[source]);
        }
        Verify(bind(effect,"SrcImage0",image[reuseInputs?1-current:0]),"VFG previous");
        Verify(bind(effect,"SrcImage1",image[reuseInputs?current:1]),"VFG current");
        Verify(set(effect,"ShotChange",0),"VFG continuous pair");Verify(set(effect,"FrameIndex",1),"VFG midpoint");Verify(run(effect,1),"VFG run");
        Cu(cudaMemcpy2DToArrayAsync(out,0,0,rgba[2],w*4,w*4,h,cudaMemcpyDeviceToDevice,stream),"VFG output transfer");
    }
    ~VfgLive(){
        if(effect)destroy(effect);
        for(int i=0;i<3;++i){if(image[i])freeImage(image[i]);if(rgba[i])cudaFree(rgba[i]);}
#if !defined(LLCV_INTERPOLATION_RELEASE) || defined(LLCV_VFG_AMPERE_COMPAT)
        compat.Reset();
#endif
        for(int i=3;i>=0;--i)if(libs[i])FreeLibrary(libs[i]);
    }
};
