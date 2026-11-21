#include "video/RifeHalfTest.h"
#ifdef LLCV_INTERPOLATION_RELEASE
#include "video/VfgRuntimeInstall.h"
#endif
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cuda_runtime.h>
#include <NvInferRuntime.h>
#include "CudaContextScope.h"
#if defined(LLCV_RIFE_OPTIONAL) && (!defined(LLCV_INTERPOLATION_RELEASE) || defined(LLCV_VFG_AMPERE_COMPAT))
#include "../../experiments/frame-generation/VfgAmpereCompat.h"
#endif
#ifdef LLCV_FLYHIGH_TEST
#include "video/FlyHighGpu.h"
#endif
#ifdef LLCV_RIFE_VULKAN
#include "video/RifeVulkanFast.h"
#include <numeric>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace llcv::rife_test {
namespace {
template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
int64_t Now(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT result,const char* message){if(FAILED(result)){char text[200];sprintf_s(text,"%s: 0x%08X",message,unsigned(result));throw std::runtime_error(text);}}
void Cu(cudaError_t result,const char* message){if(result!=cudaSuccess)throw std::runtime_error(std::string(message)+": "+cudaGetErrorString(result));}
template<class T>T Bind(HMODULE module,const char* name){auto fn=reinterpret_cast<T>(GetProcAddress(module,name));Check(fn!=nullptr,name);return fn;}
std::filesystem::path RuntimeFolder(){
    wchar_t exe[32768]{};auto n=GetModuleFileNameW(nullptr,exe,32768);
    Check(n&&n<32768,"RIFE executable path");
    auto root=std::filesystem::path(exe).parent_path();
#ifdef LLCV_RIFE_OPTIONAL
    root/=L"nvidia-rife";
#endif
    return root;
}
#ifdef LLCV_RIFE_OPTIONAL
std::filesystem::path VfgDirectory(){
#ifdef LLCV_INTERPOLATION_RELEASE
    if(auto installed=llcv::vfg_runtime::VerifiedDirectory(); !installed.empty())return installed;
#endif
    return RuntimeFolder().parent_path()/L"vfg";
}
void PrepareOptionalRuntime(Scale scale,int width){
    auto root=RuntimeFolder();
    auto require=[](bool ok,const wchar_t* detail){
        if(!ok){fallbackDetail.store(detail);throw std::runtime_error("Frame interpolation component unavailable (see Tab)");}
    };
    if(!IsVfg(scale)){
    for(const auto* f:{L"cudart64_12.dll",L"nvinfer_10.dll",L"LLCV_RifeGridSample.dll",L"LLCV_RifeTrajectory.dll",L"LLCV_RifeLiveTransfer.dll"})
        require(std::filesystem::is_regular_file(root/f),f);
    require(std::filesystem::is_regular_file(root/L"models"/L"fhd"/ScaleFile(scale,width)),ScaleFile(scale,width));
    }
    auto runtimeDirectory=root;
    if(IsVfg(scale)){
        const auto vfg=VfgDirectory();
        for(const auto* f:{L"NVVideoEffects.dll",L"nvVFXVideoFrameGeneration.dll",L"NVCVImage.dll",L"nvngxruntime.dll"})
            require(std::filesystem::is_regular_file(vfg/f),f);
        if(std::filesystem::is_regular_file(vfg/L"cudart64_12.dll"))runtimeDirectory=vfg;
    }
    // Keep these two modules loaded for the lifetime of the delay-import IAT.
    static HMODULE driver=nullptr,cudart=nullptr;
    if(!driver)driver=LoadLibraryExW(L"nvcuda.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(driver!=nullptr,L"NVIDIA driver");
    if(!cudart)cudart=LoadLibraryExW((runtimeDirectory/L"cudart64_12.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(cudart!=nullptr,L"cudart64_12.dll");
}
#endif
}
void CaptureSequence(const uint8_t* nv12,int width,int height,unsigned stride,int64_t arrivalUs){
    if(sequenceDirectory.empty())return;
    // Explicit diagnostic only: half-size luma stream, never a playback queue.
    struct Sequence {
        std::filesystem::path folder;std::ofstream pixels;std::vector<int64_t> arrivals;
        std::vector<uint8_t> half;int width=0,height=0;bool finished=false;
    };
    static Sequence capture;
    if(capture.finished)return;
    try{
        Check(nv12&&SupportedSize(width,height)&&stride>=unsigned(width)&&arrivalUs>0,"Invalid sequence input");
        if(capture.arrivals.empty()){
            capture.width=width;capture.height=height;
            capture.folder=std::filesystem::path(sequenceDirectory)/(L"sequence-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
            std::filesystem::create_directories(capture.folder.parent_path());
            Check(std::filesystem::create_directory(capture.folder),"Sequence folder already exists");
            capture.pixels.open(capture.folder/L"luma.gray8",std::ios::binary);
            capture.pixels.exceptions(std::ios::badbit|std::ios::failbit);
            capture.half.resize(size_t(width/2)*(height/2));capture.arrivals.reserve(180);
        }
        Check(width==capture.width&&height==capture.height,"Sequence resolution changed");
        if(!capture.arrivals.empty())Check(arrivalUs>capture.arrivals.back(),"Sequence clock discontinuity");
        for(int y=0;y<height/2;++y)for(int x=0;x<width/2;++x){
            const auto* row=nv12+size_t(2*y)*stride+2*x;
            capture.half[size_t(y)*(width/2)+x]=uint8_t((unsigned(row[0])+row[1]+row[stride]+row[stride+1]+2)/4);
        }
        capture.pixels.write(reinterpret_cast<const char*>(capture.half.data()),std::streamsize(capture.half.size()));
        capture.arrivals.push_back(arrivalUs);
        if(capture.arrivals.size()==180){
            capture.pixels.close();std::ofstream meta(capture.folder/L"sequence.json");meta.exceptions(std::ios::badbit|std::ios::failbit);
            meta<<"{\"width\":"<<width/2<<",\"height\":"<<height/2<<",\"source_width\":"<<width<<",\"source_height\":"<<height
                <<",\"format\":\"NV12 Y plane / 2x2 box / gray8\",\"arrivals_us\":[";
            for(size_t i=0;i<capture.arrivals.size();++i){if(i)meta<<',';meta<<capture.arrivals[i];}
            meta<<"]}\n";meta.close();capture.finished=true;++sequencesSaved;
            pairRequested=!pairDirectory.empty();
            fwprintf(stderr,L"[rife-sequence] saved 180 ordered inputs: %s\n",capture.folder.c_str());
        }
    }catch(const std::exception& e){capture.finished=true;capture.pixels.exceptions(std::ios::goodbit);if(capture.pixels.is_open())capture.pixels.close();fprintf(stderr,"[rife-sequence] capture failed: %s\n",e.what());}
}
#ifdef LLCV_RIFE_TRAJECTORY
#include "RifeDeadlineTrace.inl"
#endif
#ifdef LLCV_RIFE_OPTIONAL
#include "VfgLive.inl"
#endif
struct Half::Impl {
#ifdef LLCV_RIFE_OPTIONAL
    std::unique_ptr<VfgLive> vfg;
#endif
#ifdef LLCV_RIFE_TRAJECTORY
    DeadlineTrace trace;
#endif
    UINT W=1920,H=1080;
    Ptr<ID3D11Device5> device;Ptr<ID3D11DeviceContext4> context;Ptr<ID3D11VideoDevice> vd;Ptr<ID3D11VideoContext> vc;Ptr<ID3D11VideoContext1> vc1;
    Ptr<ID3D11VideoProcessorEnumerator> nativeEnum,ownerEnum;Ptr<ID3D11VideoProcessor> native;
    Ptr<ID3D11VideoProcessorInputView> generatedView;
    std::array<Ptr<ID3D11Texture2D>,3> cachedSources;
    std::array<Ptr<ID3D11VideoProcessorInputView>,3> cachedInputs;
    struct Image {
        Ptr<ID3D11Texture2D> texture;Ptr<ID3D11VideoProcessorOutputView> output;
        cudaExternalMemory_t memory=nullptr;cudaMipmappedArray_t mip=nullptr;cudaSurfaceObject_t surface=0;
    } images[3];
    Ptr<ID3D11Fence> fence;HANDLE complete=nullptr,timer=nullptr;cudaExternalSemaphore_t semaphore=nullptr;cudaStream_t stream=nullptr;CUcontext cudaContext=nullptr;
    UINT64 serial=0,ticket=0;UINT parity=0;bool history=false;int64_t previousArrival=0,originalAt=0;
    cudaEvent_t gpuBegin=nullptr,gpuEnd=nullptr;bool timingPending=false;
    Scale scale=Scale::Half;
    unsigned completedPairs=0;
    void SavePair(int fps,int64_t previous,int64_t current,bool quality){
        try{
            Check(!pairDirectory.empty(),"RIFE pair directory missing");
            const auto folder=std::filesystem::path(pairDirectory)/(L"pair-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
            std::filesystem::create_directories(folder.parent_path());
            Check(std::filesystem::create_directory(folder),"RIFE pair folder already exists");
            std::vector<float> pixels(size_t(W)*H*7);
            Cu(cudaMemcpy(pixels.data(),input,pixels.size()*sizeof(float),cudaMemcpyDeviceToHost),"RIFE pair input readback");
            auto write=[&](const wchar_t* name,const float* data,size_t count){
                std::ofstream file(folder/name,std::ios::binary);file.exceptions(std::ios::badbit|std::ios::failbit);
                file.write(reinterpret_cast<const char*>(data),std::streamsize(count*sizeof(float)));file.close();
            };
            write(L"input.fp32",pixels.data(),pixels.size());
            pixels.resize(size_t(W)*H*3);
            Cu(cudaMemcpy(pixels.data(),output,pixels.size()*sizeof(float),cudaMemcpyDeviceToHost),"RIFE pair output readback");
            write(L"generated.fp32",pixels.data(),pixels.size());
#ifdef LLCV_RIFE_AUTO_UI_TEST
            // An explicitly requested pair also records the applied weights.
            // No readback or file access occurs during ordinary playback.
            struct DeviceWeight{float* value=nullptr;~DeviceWeight(){if(value)cudaFree(value);}} weight;
            const size_t count=size_t(W/2)*(H/2);
            Cu(cudaMalloc(&weight.value,count*sizeof(float)),"RIFE UI diagnostic allocation");
            using ReadUi=cudaError_t(*)(void*,float*,unsigned char*,cudaStream_t);
            Cu(Bind<ReadUi>(fusedDll,"ReadRifeAutoUiMask")(ui,weight.value,nullptr,stream),"RIFE UI diagnostic read");
            Cu(cudaStreamSynchronize(stream),"RIFE UI diagnostic completion");
            pixels.resize(count);Cu(cudaMemcpy(pixels.data(),weight.value,count*sizeof(float),cudaMemcpyDeviceToHost),"RIFE UI weight readback");
            write(L"hud-weight.fp32",pixels.data(),count);
#endif
            // Written last: consumers reject partial captures without metadata.
            std::ofstream manifest(folder/L"pair.json");manifest.exceptions(std::ios::badbit|std::ios::failbit);
            const int ew=scale==Scale::Half?960:W*int(scale)/100,eh=scale==Scale::Half?540:H*int(scale)/100;
            manifest<<"{\"width\":"<<W<<",\"height\":"<<H<<",\"fps\":"<<fps<<",\"mode_percent\":"<<int(scale)
                <<",\"scale_percent\":"<<(100.0*ew/W)<<",\"estimation_width\":"<<ew<<",\"estimation_height\":"<<eh
                <<",\"previous_arrival_us\":"<<previous<<",\"current_arrival_us\":"<<current
                <<",\"quality_wait\":"<<(quality?"true":"false")
#ifdef LLCV_RIFE_AUTO_UI_TEST
                <<",\"auto_ui_source_pixels\":"<<uiWidth
                <<",\"hud_seed_inputs\":"<<hudSeedInputs.load()<<",\"hud_pair_inputs\":"<<hudPairInputs.load()
#endif
                <<",\"layout\":\"NCHW FP32 RGB0 RGB1 timestep\"}\n";
            manifest.close();++pairsSaved;
            fwprintf(stderr,L"[rife-pair] saved actual inference input and output: %s\n",folder.c_str());
        }catch(const std::exception& e){++pairSaveFailures;fprintf(stderr,"[rife-pair] save failed: %s\n",e.what());}
    }
    bool FenceReady() const {
        const auto progress=SharedFenceProgress(fence->GetCompletedValue(),ticket);
        if(progress==SharedFenceState::DeviceRemoved){
            const auto reason=device->GetDeviceRemovedReason();
            Hr(FAILED(reason)?reason:DXGI_ERROR_DEVICE_REMOVED,"RIFE shared D3D11 fence");
        }
        return progress==SharedFenceState::Ready;
    }
    void HarvestTiming(){
        if(!timingPending||!FenceReady())return;
#ifdef LLCV_RIFE_TRAJECTORY
        trace.harvest(gpuBegin,gpuEnd);
#endif
        float ms=0;if(cudaEventElapsedTime(&ms,gpuBegin,gpuEnd)==cudaSuccess){
            const auto us=uint64_t(ms*1000);++jobs;gpuUs.fetch_add(us);
            ++modeJobs[ScaleIndex(scale)];modeGpuUs[ScaleIndex(scale)].fetch_add(us);
            auto old=maxGpuUs.load();while(old<us&&!maxGpuUs.compare_exchange_weak(old,us)){}
        }timingPending=false;
    }
    HMODULE trtDll=nullptr,warpDll=nullptr,fusedDll=nullptr,transferDll=nullptr;bool warpRegistered=false,fusedRegistered=false;
    using Pack=cudaError_t(*)(cudaSurfaceObject_t,cudaSurfaceObject_t,float*,int,int,cudaStream_t);
    using Unpack=cudaError_t(*)(const float*,cudaSurfaceObject_t,int,int,cudaStream_t);
    Pack pack=nullptr;Unpack unpack=nullptr;
#ifdef LLCV_RIFE_TRAJECTORY
    void* path=nullptr;
    using PathStep=cudaError_t(*)(void*,const float*,float*,int64_t,int64_t,int,cudaStream_t);
    PathStep pathStep=nullptr;void(*pathDestroy)(void*)=nullptr;bool resetPath=true;
#endif
#ifdef LLCV_RIFE_AUTO_UI_TEST
    using UiCreate=cudaError_t(*)(int,int,void**);
    using UiDisable=cudaError_t(*)(void*,cudaStream_t);
    using UiUpdate=cudaError_t(*)(void*,const float*,int64_t,int,cudaStream_t);
    using UiSurface=cudaError_t(*)(void*,cudaSurfaceObject_t,int64_t,int,cudaStream_t);
    using UiDestroy=void(*)(void*);
    void* ui=nullptr;UiDisable uiDisable=nullptr;UiUpdate uiUpdate=nullptr;UiSurface uiSurface=nullptr;UiDestroy uiDestroy=nullptr;
    int uiWidth=-1;bool uiReset=true;
#endif
    struct Logger:nvinfer1::ILogger{void log(Severity s,const char* text)noexcept override{if(s<=Severity::kWARNING)fprintf(stderr,"[rife-trt] %s\n",text);}}logger;
    std::unique_ptr<nvinfer1::IRuntime> runtime;std::unique_ptr<nvinfer1::ICudaEngine> engine;std::unique_ptr<nvinfer1::IExecutionContext> inference;
    float *input=nullptr,*output=nullptr;cudaGraphExec_t graph=nullptr;
    bool Idle()const{return !ticket||FenceReady();}
    void ResetModel(){
        if(graph){cudaGraphExecDestroy(graph);graph=nullptr;}
        inference.reset();engine.reset();
    }
    void LoadModel(Scale requestedScale){
        Check(ValidScale(requestedScale),"RIFE invalid scale");
        Check(Idle(),"RIFE model switch with unfinished work");
        HarvestTiming();ResetModel();history=false;previousArrival=0;
#ifdef LLCV_RIFE_AUTO_UI_TEST
        Cu(uiDisable(ui,stream),"RIFE UI model reset");uiReset=true;uiWidth=-1;
#endif
        const auto root=RuntimeFolder();
        std::ifstream file(root/L"models"/(W==3840?L"uhd":W==2560?L"qhd":L"fhd")/ScaleFile(requestedScale,W),std::ios::binary|std::ios::ate);
        Check(bool(file),"RIFE selected model missing");auto bytes=file.tellg();
        Check(bytes>0&&bytes<std::streamoff(1<<28),"RIFE model size");
        std::vector<char> data(static_cast<size_t>(bytes));file.seekg(0);file.read(data.data(),bytes);
        Check(bool(file),"RIFE model read");engine.reset(runtime->deserializeCudaEngine(data.data(),data.size()));
        Check(bool(engine),"RIFE model not built for this GPU/runtime");inference.reset(engine->createExecutionContext());
        Check(bool(inference)&&engine->getNbIOTensors()==2,"RIFE I/O contract");
        int inputs=0,outputs=0;
        for(int i=0;i<2;++i){
            const auto* name=engine->getIOTensorName(i);
            Check(engine->getTensorDataType(name)==nvinfer1::DataType::kFLOAT,"RIFE FP32 contract");
            const bool in=engine->getTensorIOMode(name)==nvinfer1::TensorIOMode::kINPUT;
            if(in){++inputs;Check(inference->setInputShape(name,nvinfer1::Dims4{1,7,int(H),int(W)}),"RIFE model shape");}else ++outputs;
            const auto shape=inference->getTensorShape(name);
            Check(shape.nbDims==4&&shape.d[0]==1&&shape.d[1]==(in?7:3)&&shape.d[2]==int(H)&&shape.d[3]==int(W),"RIFE tensor extent");
            Check(inference->setTensorAddress(name,in?input:output),"RIFE model address");
        }
        Check(inputs==1&&outputs==1,"RIFE input/output count");
        Cu(cudaMemsetAsync(input,0,size_t(W)*H*7*4,stream),"RIFE warm input");
        Check(inference->enqueueV3(stream),"RIFE warmup");Cu(cudaStreamSynchronize(stream),"RIFE warmup completion");
        Cu(cudaStreamBeginCapture(stream,cudaStreamCaptureModeThreadLocal),"RIFE graph begin");
        const bool enqueued=inference->enqueueV3(stream);cudaGraph_t captured=nullptr;
        const auto ended=cudaStreamEndCapture(stream,&captured);
        if(!enqueued||ended!=cudaSuccess){if(captured)cudaGraphDestroy(captured);Check(enqueued,"RIFE graph enqueue");Cu(ended,"RIFE graph end");}
        const auto result=cudaGraphInstantiateWithFlags(&graph,captured,0);cudaGraphDestroy(captured);
        Cu(result,"RIFE graph instantiate");scale=requestedScale;activeScale.store(scale);
        status=Status::Pending;
        fwprintf(stderr,L"[rife-live] %s fused ready; %ux%u NV12 SDR, DX11 external fence; no CPU image expansion.\n",ScaleLabel(scale),W,H);
    }
    void ImageInit(Image& image) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=W;desc.Height=H;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
        Hr(device->CreateTexture2D(&desc,nullptr,&image.texture),"RIFE shared image");Ptr<IDXGIResource1> resource;Hr(image.texture.As(&resource),"RIFE shared resource");HANDLE handle=nullptr;
        Hr(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle),"RIFE image handle");
        cudaExternalMemoryHandleDesc external{};external.type=cudaExternalMemoryHandleTypeD3D11Resource;external.handle.win32.handle=handle;external.size=UINT64(W)*H*4;external.flags=cudaExternalMemoryDedicated;
        const auto imported=cudaImportExternalMemory(&image.memory,&external);CloseHandle(handle);Cu(imported,"RIFE image import");
        cudaExternalMemoryMipmappedArrayDesc mapped{};mapped.formatDesc=cudaCreateChannelDesc<uchar4>();mapped.extent=make_cudaExtent(W,H,0);mapped.numLevels=1;mapped.flags=cudaArraySurfaceLoadStore;
        Cu(cudaExternalMemoryGetMappedMipmappedArray(&image.mip,image.memory,&mapped),"RIFE image map");cudaArray_t array=nullptr;Cu(cudaGetMipmappedArrayLevel(&array,image.mip,0),"RIFE array");
        cudaResourceDesc surface{};surface.resType=cudaResourceTypeArray;surface.res.array.array=array;Cu(cudaCreateSurfaceObject(&image.surface,&surface),"RIFE surface");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};view.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;Hr(vd->CreateVideoProcessorOutputView(image.texture.Get(),nativeEnum.Get(),&view,&image.output),"RIFE native output");
    }
    void Initialize(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11VideoDevice* videoDevice,ID3D11VideoContext* videoContext,int width,int height,int fps,Scale initialScale) {
        Check(SupportsInput(initialScale,width,height),"Unsupported interpolation input size");W=UINT(width);H=UINT(height);
        Hr(d->QueryInterface(IID_PPV_ARGS(&device)),"RIFE D3D11.4 device");Hr(c->QueryInterface(IID_PPV_ARGS(&context)),"RIFE D3D11.4 context");vd=videoDevice;vc=videoContext;Hr(vc.As(&vc1),"RIFE video context1");
        Ptr<IDXGIDevice> dxgi;Ptr<IDXGIAdapter> adapter;Hr(device.As(&dxgi),"RIFE DXGI device");Hr(dxgi->GetAdapter(&adapter),"RIFE adapter");DXGI_ADAPTER_DESC identity{};Hr(adapter->GetDesc(&identity),"RIFE adapter identity");Check(identity.VendorId==0x10de,"RIFE test needs NVIDIA GPU");
        Check(cuInit(0)==CUDA_SUCCESS,"RIFE CUDA init");int count=0,selected=-1;Cu(cudaGetDeviceCount(&count),"CUDA count");for(int i=0;i<count;++i){cudaDeviceProp prop{};Cu(cudaGetDeviceProperties(&prop,i),"CUDA identity");if(!memcmp(prop.luid,&identity.AdapterLuid,8))selected=i;}Check(selected>=0,"RIFE graphics/CUDA mismatch");Cu(cudaSetDevice(selected),"RIFE CUDA device");Check(cuCtxGetCurrent(&cudaContext)==CUDA_SUCCESS&&cudaContext,"RIFE CUDA context");
        Cu(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking),"Interpolation stream");
        if(!IsVfg(initialScale)){Cu(cudaMalloc(&input,size_t(W)*H*7*4),"RIFE input");Cu(cudaMalloc(&output,size_t(W)*H*3*4),"RIFE output");}
        Cu(cudaEventCreate(&gpuBegin),"RIFE timing begin");Cu(cudaEventCreate(&gpuEnd),"RIFE timing end");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};content.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;content.InputFrameRate={UINT(fps),1};content.OutputFrameRate=content.InputFrameRate;content.InputWidth=content.OutputWidth=W;content.InputHeight=content.OutputHeight=H;content.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        Hr(vd->CreateVideoProcessorEnumerator(&content,&nativeEnum),"RIFE native enumerator");Hr(vd->CreateVideoProcessor(nativeEnum.Get(),0,&native),"RIFE native processor");
        RECT rect{0,0,LONG(W),LONG(H)};vc->VideoProcessorSetStreamAutoProcessingMode(native.Get(),0,FALSE);vc->VideoProcessorSetStreamFrameFormat(native.Get(),0,D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);vc->VideoProcessorSetStreamSourceRect(native.Get(),0,TRUE,&rect);vc->VideoProcessorSetStreamDestRect(native.Get(),0,TRUE,&rect);vc->VideoProcessorSetOutputTargetRect(native.Get(),TRUE,&rect);
        vc1->VideoProcessorSetOutputColorSpace1(native.Get(),DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        for(auto& image:images)ImageInit(image);
        Hr(device->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"RIFE fence");HANDLE handle=nullptr;Hr(fence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle),"RIFE fence handle");cudaExternalSemaphoreHandleDesc sem{};sem.type=cudaExternalSemaphoreHandleTypeD3D11Fence;sem.handle.win32.handle=handle;const auto imported=cudaImportExternalSemaphore(&semaphore,&sem);CloseHandle(handle);Cu(imported,"RIFE fence import");
        complete=CreateEventW(nullptr,FALSE,FALSE,nullptr);timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);Check(complete&&timer,"RIFE timer/event");
        auto root=RuntimeFolder();
#ifdef LLCV_RIFE_OPTIONAL
        if(IsVfg(initialScale)){vfg=std::make_unique<VfgLive>();vfg->Init(VfgDirectory(),selected,W,H,initialScale==Scale::VfgMedium,stream);scale=initialScale;activeScale.store(scale);return;}
#endif
        auto load=[&](const wchar_t* name){auto value=LoadLibraryExW((root/name).c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);Check(value!=nullptr,"RIFE runtime file missing");return value;};
        trtDll=load(L"nvinfer_10.dll");using Factory=void*(*)(void*,int32_t)noexcept;runtime.reset(static_cast<nvinfer1::IRuntime*>(Bind<Factory>(trtDll,"createInferRuntime_INTERNAL")(&logger,NV_TENSORRT_VERSION)));Check(bool(runtime),"RIFE runtime version");
        warpDll=load(L"LLCV_RifeGridSample.dll");warpRegistered=Bind<bool(*)()>(warpDll,"RegisterRifeGridSample")();Check(warpRegistered,"RIFE warp registration");
#ifdef LLCV_RIFE_AUTO_UI_TEST
#ifdef LLCV_RIFE_TRAJECTORY
        fusedDll=load(L"LLCV_RifeTrajectory.dll");
        pathStep=Bind<PathStep>(fusedDll,"StepRifeTrajectory");
        pathDestroy=Bind<void(*)(void*)>(fusedDll,"DestroyRifeTrajectory");
        Cu(Bind<cudaError_t(*)(int,int,void**)>(fusedDll,"CreateRifeTrajectory")(W,H,&path),"RIFE trajectory allocation");
#else
        fusedDll=load(L"LLCV_RifeFusedSynthesis_AutoUi.dll");
#endif
        uiDisable=Bind<UiDisable>(fusedDll,"DisableRifeAutoUi");
        uiUpdate=Bind<UiUpdate>(fusedDll,"UpdateRifeAutoUiPlanar");
        uiSurface=Bind<UiSurface>(fusedDll,"UpdateRifeAutoUiSurface");
        uiDestroy=Bind<UiDestroy>(fusedDll,"DestroyRifeAutoUi");
        // Allocate before engine warmup/graph capture. Its stable address is
        // captured by synthesis; mask updates run on the same stream.
        Cu(Bind<UiCreate>(fusedDll,"CreateRifeAutoUi")(W/2,H/2,&ui),"RIFE UI context");
#else
        fusedDll=load(W==2560?L"LLCV_RifeFusedSynthesis_Scales.dll":L"LLCV_RifeFusedSynthesis.dll");
#endif
        fusedRegistered=Bind<bool(*)()>(fusedDll,"RegisterRifeFusedSynthesis")();Check(fusedRegistered,"RIFE synthesis registration");
        transferDll=load(L"LLCV_RifeLiveTransfer.dll");pack=Bind<Pack>(transferDll,"LiveRifePack");unpack=Bind<Unpack>(transferDll,"LiveRifeUnpack");
        LoadModel(initialScale);
#ifdef LLCV_RIFE_TRAJECTORY
        trace.init(stream);
#endif
    }
    ID3D11VideoProcessorInputView* Submit(ID3D11Texture2D* source,ID3D11VideoProcessor* original,ID3D11VideoProcessorEnumerator* originalEnum,int fps,int64_t arrival) {
#ifdef LLCV_FLYHIGH_TEST
        const auto providerTicket=providerEpoch.load(std::memory_order_acquire);
        if(IsFlyHigh()){history=false;status=Status::Pending;return nullptr;}
#endif
        if(ticket&&!FenceReady()){history=false;++busy;status=Status::Late;return nullptr;}
        HarvestTiming();
#ifdef LLCV_RIFE_AUTO_UI_TEST
        const int protect=HudWidthFor(scale);
        Check(ValidAutoUiWidth(protect),"RIFE invalid UI protection width");
#ifdef LLCV_RIFE_TRAJECTORY
        if(uiReset||uiWidth!=protect||!history)resetPath=true;
#endif
        if(!IsVfg(scale)&&ResetAutoUiHistory(uiWidth,protect,uiReset)){Cu(uiDisable(ui,stream),"RIFE UI history reset");++hudExplicitResets;}
        uiWidth=protect;uiReset=false;
#endif
        const auto pairPreviousArrival=previousArrival;
        // Adjacent interpolation frames and persistent screen-space evidence
        // have different lifetimes. The GPU detector applies its own 100 ms
        // source-clock/cut policy; a 33 ms input gap only reseeds the RIFE pair.
        const auto step=PlanInputHistory(previousArrival,arrival,fps,history);
        history=step.pairReady;
        previousArrival=arrival;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};desc.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
        size_t slot=0;while(slot<cachedSources.size()&&cachedSources[slot].Get()!=source&&cachedSources[slot])++slot;
        Check(slot<cachedSources.size(),"RIFE upload surface count changed");
        if(!cachedInputs[slot]){cachedSources[slot]=source;Hr(vd->CreateVideoProcessorInputView(source,nativeEnum.Get(),&desc,&cachedInputs[slot]),"RIFE native input");}
        D3D11_TEXTURE2D_DESC srcDesc{};source->GetDesc(&srcDesc);
        if(srcDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||srcDesc.Format==DXGI_FORMAT_B8G8R8X8_UNORM){
            DXGI_COLOR_SPACE_TYPE space{};vc1->VideoProcessorGetStreamColorSpace1(original,0,&space);vc1->VideoProcessorSetStreamColorSpace1(native.Get(),0,space);
        }else{D3D11_VIDEO_PROCESSOR_COLOR_SPACE color{};vc->VideoProcessorGetStreamColorSpace(original,0,&color);vc->VideoProcessorSetStreamColorSpace(native.Get(),0,&color);}
        D3D11_VIDEO_PROCESSOR_STREAM s{};s.Enable=TRUE;s.pInputSurface=cachedInputs[slot].Get();Hr(vc->VideoProcessorBlt(native.Get(),images[parity].output.Get(),0,1,&s),"RIFE NV12 RGB conversion");
        if(!history){
#ifdef LLCV_RIFE_TRAJECTORY
            resetPath=true;
#endif
#ifdef LLCV_RIFE_AUTO_UI_TEST
            if(protect&&!IsVfg(scale)){
                // Observe the current frame even while an adjacent RIFE pair
                // is unavailable. Read just this converted surface, never an
                // uninitialized/stale second input. Same one-ticket ownership
                // as inference, with no CPU wait or added frame queue.
                const auto available=++serial;ticket=++serial;
                Hr(context->Signal(fence.Get(),available),"RIFE UI seed signal");context->Flush();
                cudaExternalSemaphoreWaitParams wait{};wait.params.fence.value=available;
                Cu(cudaWaitExternalSemaphoresAsync(&semaphore,&wait,1,stream),"RIFE UI seed input wait");
                Cu(uiSurface(ui,images[parity].surface,arrival,protect,stream),"RIFE UI seed observation");
                cudaExternalSemaphoreSignalParams signal{};signal.params.fence.value=ticket;
                Cu(cudaSignalExternalSemaphoresAsync(&semaphore,&signal,1,stream),"RIFE UI seed completion");
                ++hudSeedInputs;
            }
#endif
            history=true;parity=1-parity;status=Status::Pending;return nullptr;
        }
        const bool quality=qualityComparison.load(std::memory_order_acquire);
#ifdef LLCV_RIFE_TRAJECTORY
        const auto submitUs=trace.anchor?Now():0;
#endif
        const UINT current=parity;parity=1-parity;const auto available=++serial;ticket=++serial;Hr(context->Signal(fence.Get(),available),"RIFE input signal");context->Flush();
        cudaExternalSemaphoreWaitParams wait{};wait.params.fence.value=available;Cu(cudaWaitExternalSemaphoresAsync(&semaphore,&wait,1,stream),"RIFE input wait");Cu(cudaEventRecord(gpuBegin,stream),"Interpolation timing start");
#ifdef LLCV_RIFE_OPTIONAL
        if(vfg){cudaArray_t a[3]{};for(int i=0;i<3;++i)Cu(cudaGetMipmappedArrayLevel(&a[i],images[i].mip,0),"VFG shared image");vfg->Process(a[1-current],a[current],a[2],stream);}else
#endif
        {Cu(pack(images[1-current].surface,images[current].surface,input,W,H,stream),"RIFE input pack");
#ifdef LLCV_RIFE_AUTO_UI_TEST
        if(protect){Cu(uiUpdate(ui,input,arrival,protect,stream),"RIFE UI detection");++hudPairInputs;}
#endif
#ifdef LLCV_RIFE_TRAJECTORY
        if(trace.anchor)Cu(cudaEventRecord(trace.pre,stream),"trace inference start");
#endif
        Cu(cudaGraphLaunch(graph,stream),"RIFE inference");
#ifdef LLCV_RIFE_TRAJECTORY
        if(trace.anchor)Cu(cudaEventRecord(trace.post,stream),"trace inference end");
        if(protect){Cu(pathStep(path,input,output,pairPreviousArrival,arrival,resetPath,stream),"RIFE trajectory correction");resetPath=false;}
#endif
        Cu(unpack(output,images[2].surface,W,H,stream),"RIFE output unpack");}Cu(cudaEventRecord(gpuEnd,stream),"RIFE timing finish");timingPending=true;cudaExternalSemaphoreSignalParams signal{};signal.params.fence.value=ticket;Cu(cudaSignalExternalSemaphoresAsync(&semaphore,&signal,1,stream),"RIFE output signal");
        // Quality comparison explicitly trades responsiveness for seeing the
        // selected model. It still has one job and a bounded, cancellable wait.
        const auto deadline=CompletionDeadlineUs(arrival,Now(),fps,quality);const auto remaining=deadline-Now();
        if(remaining>0&&!FenceReady()){
            ResetEvent(complete);Hr(fence->SetEventOnCompletion(ticket,complete),"RIFE completion event");
            // Millisecond rounding of an 8.33ms budget can wake *after* the
            // deadline even when CUDA completed in time. Sleep for the coarse
            // portion, then use a bounded pause loop for the final sub-ms gap.
            if(quality){
                while(!FenceReady()&&Now()<deadline&&requested.load()&&qualityComparison.load()&&selectedScale.load()==scale
#ifdef LLCV_FLYHIGH_TEST
                    &&!IsFlyHigh()&&providerEpoch.load()==providerTicket
#endif
#ifdef LLCV_RIFE_AUTO_UI_TEST
                    &&HudWidthFor(scale)==protect
#endif
                    )
                    WaitForSingleObject(complete,2);
            }else{
                const auto sleepMs=remaining>1500?DWORD((remaining-1500)/1000):0;
                if(sleepMs)WaitForSingleObject(complete,sleepMs);
                while(!FenceReady()&&Now()<deadline&&requested.load(std::memory_order_relaxed))YieldProcessor();
            }
        }
        const bool completed=FenceReady();
        const auto decision=Now();
        const bool ready=completed&&decision<=deadline;
#ifdef LLCV_RIFE_TRAJECTORY
        if(trace.anchor&&trace.rows.size()<8192){
            trace.active=int(trace.rows.size());
            trace.rows.push_back({arrival,submitUs,decision,deadline,fps,protect,ready?1:0});
        }
#endif
        HarvestTiming();
        if(!requested.load(std::memory_order_acquire)||selectedScale.load()!=scale||qualityComparison.load()!=quality
#ifdef LLCV_FLYHIGH_TEST
            ||IsFlyHigh()||providerEpoch.load()!=providerTicket
#endif
#ifdef LLCV_RIFE_AUTO_UI_TEST
            ||HudWidthFor(scale)!=protect
#endif
            ){
            history=false;++modeCancelled[ScaleIndex(scale)];status=requested.load()?Status::Pending:Status::Off;return nullptr;
        }
        if(!ready){++late;++modeLate[ScaleIndex(scale)];status=Status::Late;return nullptr;}
        if(pairAfterJobs&&++completedPairs==pairAfterJobs)pairRequested=true;
        if(pairRequested.exchange(false)&&!IsVfg(scale))SavePair(fps,pairPreviousArrival,arrival,quality);
        Hr(context->Wait(fence.Get(),ticket),"RIFE output acquire");
        if(ownerEnum.Get()!=originalEnum){generatedView.Reset();ownerEnum=originalEnum;Hr(vd->CreateVideoProcessorInputView(images[2].texture.Get(),ownerEnum.Get(),&desc,&generatedView),"RIFE generated input view");}
        status=Status::Active;return generatedView.Get();
    }
    ~Impl(){
        CudaContextScope current(cudaContext);
        if(stream)cudaStreamSynchronize(stream);
#ifdef LLCV_RIFE_OPTIONAL
        vfg.reset();
#endif
#ifdef LLCV_RIFE_TRAJECTORY
        if(trace.anchor){trace.harvest(gpuBegin,gpuEnd);trace.save(stream);}
#endif
        ResetModel();runtime.reset();
#ifdef LLCV_RIFE_TRAJECTORY
        if(path&&pathDestroy)pathDestroy(path);
#endif
#ifdef LLCV_RIFE_AUTO_UI_TEST
        if(ui&&uiDestroy)uiDestroy(ui);
#endif
        if(input)cudaFree(input);if(output)cudaFree(output);
        for(auto& image:images){if(image.surface)cudaDestroySurfaceObject(image.surface);if(image.mip)cudaFreeMipmappedArray(image.mip);if(image.memory)cudaDestroyExternalMemory(image.memory);}
        if(semaphore)cudaDestroyExternalSemaphore(semaphore);if(stream)cudaStreamDestroy(stream);
        if(gpuBegin)cudaEventDestroy(gpuBegin);if(gpuEnd)cudaEventDestroy(gpuEnd);
        if(complete)CloseHandle(complete);if(timer)CloseHandle(timer);
        auto unload=[](HMODULE module,bool registered,const char* name){if(!module)return;auto fn=reinterpret_cast<bool(*)()>(GetProcAddress(module,name));if(!registered||(fn&&fn()))FreeLibrary(module);else fprintf(stderr,"[rife-live] plugin retained until process exit\n");};
        unload(fusedDll,fusedRegistered,"UnregisterRifeFusedSynthesis");
        unload(warpDll,warpRegistered,"UnregisterRifeGridSample");
        if(transferDll)FreeLibrary(transferDll);if(trtDll)FreeLibrary(trtDll);
    }
};
#ifdef LLCV_FLYHIGH_TEST
#include "video/FlyHighLive.inl"
#endif
#ifdef LLCV_RIFE_VULKAN
// Fast and Half presets on ncnn Vulkan. Same playback contract as Impl: one job at a
// time, never a queue, and the original is shown when the midpoint is late.
struct Half::VulkanFast {
    UINT W=rife_vulkan::kWidth,H=rife_vulkan::kHeight;
    const Scale scale=vulkanScale;
    Ptr<ID3D11Device5> device;Ptr<ID3D11DeviceContext4> context;Ptr<ID3D11VideoDevice> vd;Ptr<ID3D11VideoContext> vc;Ptr<ID3D11VideoContext1> vc1;
    Ptr<ID3D11VideoProcessorEnumerator> nativeEnum,ownerEnum;Ptr<ID3D11VideoProcessor> native;
    std::array<Ptr<ID3D11Texture2D>,3> cachedSources;
    std::array<Ptr<ID3D11VideoProcessorInputView>,3> cachedInputs;
    Ptr<ID3D11Texture2D> input,output;Ptr<ID3D11VideoProcessorOutputView> inputTarget;
    Ptr<ID3D11VideoProcessorInputView> generatedView;
    Ptr<ID3D11Fence> fence;HANDLE complete=nullptr,timer=nullptr;
    std::unique_ptr<rife_vulkan::FastBackend> backend; // Released before the shared textures.
    // Unlike a std::async future, abandoning this state does not wait for the loader.
    std::future<std::unique_ptr<rife_vulkan::FastBackend>> loading;
    std::stop_source startupStop;
    int64_t startupAt=0;
    UINT64 serial=0,ticket=0;bool history=false;int64_t previousArrival=0,originalAt=0;
    // Session statistics, written to the log when playback stops.
    struct Row{
        int64_t arrival,submitted,decided,deadline;
        int outcome;
        int64_t generatedPresentEnd=0,originalPresentBegin=0,originalPresentEnd=0;
        uint64_t ticket=0;
        rife_vulkan::SubmissionTiming vulkan{};
    };
    std::vector<Row> rows;std::vector<double> gpuMs;
    bool traceSubmission=false;
    size_t lastTicketRow=0;
    uint64_t pairs=0,seeds=0,onTime=0,lateCount=0,busyCount=0,cancelled=0;
    uint64_t inputAttempts=0,generatedPresented=0;
    uint64_t rgbCopies=0,rgbConversions=0;
    void Texture(Ptr<ID3D11Texture2D>& texture,const char* what){
        D3D11_TEXTURE2D_DESC desc{};desc.Width=W;desc.Height=H;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
        Hr(device->CreateTexture2D(&desc,nullptr,&texture),what);
    }
    void Initialize(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11VideoDevice* videoDevice,ID3D11VideoContext* videoContext,int width,int height,int fps){
        Check(width==int(W)&&height==int(H),"Vulkan RIFE needs FHD input");
        Hr(d->QueryInterface(IID_PPV_ARGS(&device)),"Vulkan D3D11.4 device");Hr(c->QueryInterface(IID_PPV_ARGS(&context)),"Vulkan D3D11.4 context");
        vd=videoDevice;vc=videoContext;Hr(vc.As(&vc1),"Vulkan video context1");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};content.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;content.InputFrameRate={UINT(fps>0?fps:60),1};content.OutputFrameRate=content.InputFrameRate;content.InputWidth=content.OutputWidth=W;content.InputHeight=content.OutputHeight=H;content.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        Hr(vd->CreateVideoProcessorEnumerator(&content,&nativeEnum),"Vulkan native enumerator");Hr(vd->CreateVideoProcessor(nativeEnum.Get(),0,&native),"Vulkan native processor");
        RECT rect{0,0,LONG(W),LONG(H)};vc->VideoProcessorSetStreamAutoProcessingMode(native.Get(),0,FALSE);vc->VideoProcessorSetStreamFrameFormat(native.Get(),0,D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);vc->VideoProcessorSetStreamSourceRect(native.Get(),0,TRUE,&rect);vc->VideoProcessorSetStreamDestRect(native.Get(),0,TRUE,&rect);vc->VideoProcessorSetOutputTargetRect(native.Get(),TRUE,&rect);
        vc1->VideoProcessorSetOutputColorSpace1(native.Get(),DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        Texture(input,"Vulkan shared input");Texture(output,"Vulkan shared output");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};view.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
        Hr(vd->CreateVideoProcessorOutputView(input.Get(),nativeEnum.Get(),&view,&inputTarget),"Vulkan input conversion target");
        Hr(device->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"Vulkan shared fence");
        complete=CreateEventW(nullptr,FALSE,FALSE,nullptr);timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);Check(complete&&timer,"Vulkan timer/event");
        traceSubmission=!vulkanTraceFile.empty();
        if(traceSubmission){rows.reserve(65536);gpuMs.reserve(65536);}
    }
    bool PollBackend(){
        if(backend)return true;
        if(!startupAt)startupAt=Now();
        if(!loading.valid()){
            const auto preset=scale==Scale::Half?rife_vulkan::Preset::Half:rife_vulkan::Preset::Fast;
            loading=rife_vulkan::FastBackend::TryInitializeAsync(device.Get(),input.Get(),output.Get(),fence.Get(),
                std::filesystem::path(vulkanModelDirectory),preset,vulkanHud,vulkanHudChroma,traceSubmission,startupStop);
        }
        if(!loading.valid()||loading.wait_for(std::chrono::seconds(0))!=std::future_status::ready){
            if(VulkanStartupExpired(startupAt,Now())){
                startupStop.request_stop();
                throw std::runtime_error("Vulkan RIFE initialization timed out; continuing original playback");
            }
            return false;
        }
        try{backend=loading.get();}
        catch(const rife_vulkan::BackendRetiring&){return false;}
        activeScale.store(scale);status=Status::Pending;
        fwprintf(stderr,L"[rife-vulkan] %s ready on %s; %ux%u, D3D11 shared textures and fence; HUD protection %s.\n",
                 scale==Scale::Half?L"Half":L"Fast",backend->DeviceName().c_str(),W,H,
                 vulkanHudChroma?L"on (edge history + chroma, 16 px)":vulkanHud?L"on (edge history, 16 px)":L"off");
        return true;
    }
    void Harvest(){
        if(backend&&traceSubmission){
            rife_vulkan::SubmissionTiming timing;
            if(backend->TakeSubmissionTiming(timing)&&lastTicketRow<rows.size()&&rows[lastTicketRow].ticket==timing.ticket)
                rows[lastTicketRow].vulkan=timing;
        }
        double ms=0;
        if(backend&&backend->TakeGpuMs(ms)&&ms>=0){
            if(gpuMs.size()<gpuMs.capacity())gpuMs.push_back(ms);
            const auto us=uint64_t(ms*1000);++jobs;gpuUs.fetch_add(us);
            ++modeJobs[ScaleIndex(scale)];modeGpuUs[ScaleIndex(scale)].fetch_add(us);
            auto old=maxGpuUs.load();while(old<us&&!maxGpuUs.compare_exchange_weak(old,us)){}
        }
    }
    void Record(int64_t arrival,int64_t submitted,int64_t decided,int64_t deadline,int outcome,uint64_t jobTicket=0){
        if(rows.size()>=rows.capacity())return;
        if(jobTicket)lastTicketRow=rows.size();
        rows.push_back({arrival,submitted,decided,deadline,outcome,0,0,0,jobTicket});
    }
    bool FenceReady(SharedFenceState state) const {
        if(state==SharedFenceState::DeviceRemoved) {
            const auto reason=device->GetDeviceRemovedReason();
            Hr(FAILED(reason)?reason:DXGI_ERROR_DEVICE_REMOVED,"Vulkan shared D3D11 fence");
        }
        return state==SharedFenceState::Ready;
    }
    ID3D11VideoProcessorInputView* Submit(ID3D11Texture2D* source,ID3D11VideoProcessor* original,ID3D11VideoProcessorEnumerator* originalEnum,int fps,int64_t arrival){
        ++inputAttempts;
        const auto fenceValue=[&]{return fence->GetCompletedValue();};
        if(ticket&&!FenceReady(CheckPreviousVulkanJob(ticket,[&]{return backend->Free();},fenceValue))){
            history=false;++busy;++busyCount;Record(arrival,0,Now(),0,3);status=Status::Late;return nullptr;
        }
        Harvest();
        D3D11_TEXTURE2D_DESC srcDesc{};source->GetDesc(&srcDesc);
        if(!SupportsVulkanSdrTexture(srcDesc.Format)){
            history=false;Record(arrival,0,Now(),0,5);status=Status::Unsupported;return nullptr;
        }
        const auto step=PlanInputHistory(previousArrival,arrival,fps,history);
        previousArrival=arrival;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};desc.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
        const bool rgb=srcDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||srcDesc.Format==DXGI_FORMAT_B8G8R8X8_UNORM;
        DXGI_COLOR_SPACE_TYPE color{};
        BOOL mirror=FALSE,flipX=FALSE,flipY=FALSE;
        if(rgb){
            vc1->VideoProcessorGetStreamColorSpace1(original,0,&color);
            vc1->VideoProcessorGetStreamMirror(original,0,&mirror,&flipX,&flipY);
        }
        const bool rgbCopy=vulkanTryRgbCopy&&srcDesc.Format==DXGI_FORMAT_R8G8B8A8_UNORM&&
            color==DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709&&(!mirror||!flipX)&&
            srcDesc.Width==W&&srcDesc.Height==H&&srcDesc.MipLevels==1&&srcDesc.ArraySize==1&&
            srcDesc.SampleDesc.Count==1&&srcDesc.SampleDesc.Quality==0;
        if(rgbCopy){
            // RGB24 has already been expanded into RGBA8 on the GPU.
            // Vulkan folds the optional Y flip into its existing input loads.
            context->CopyResource(input.Get(),source);
            ++rgbCopies;
        }else{
            size_t slot=0;
            while(slot<cachedSources.size()&&cachedSources[slot].Get()!=source&&cachedSources[slot])++slot;
            Check(slot<cachedSources.size(),"Vulkan upload surface count changed");
            if(!cachedInputs[slot]){
                cachedSources[slot]=source;
                Hr(vd->CreateVideoProcessorInputView(source,nativeEnum.Get(),&desc,&cachedInputs[slot]),"Vulkan native input");
            }
            if(rgb){
                vc1->VideoProcessorSetStreamColorSpace1(native.Get(),0,color);
                vc1->VideoProcessorSetStreamMirror(native.Get(),0,mirror,flipX,flipY);
                ++rgbConversions;
            }else{
                D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
                vc->VideoProcessorGetStreamColorSpace(original,0,&yuv);
                vc->VideoProcessorSetStreamColorSpace(native.Get(),0,&yuv);
            }
            D3D11_VIDEO_PROCESSOR_STREAM s{};s.Enable=TRUE;s.pInputSurface=cachedInputs[slot].Get();
            Hr(vc->VideoProcessorBlt(native.Get(),inputTarget.Get(),0,1,&s),"Vulkan SDR to RGB conversion");
        }
        // Every accepted input advances the GPU-resident history, including
        // the first frame after a gap; only a valid pair is shown.
        const auto submitted=Now();
        const auto available=++serial;ticket=++serial;
        Hr(context->Signal(fence.Get(),available),"Vulkan input signal");context->Flush();
        const auto deadline=CompletionDeadlineUs(arrival,submitted,fps,false);
        const auto admitted=Now();
        const auto kind=CanGeneratePair(step.pairReady,admitted,deadline)
            ?rife_vulkan::JobKind::Generate:rife_vulkan::JobKind::HistoryOnly;
        backend->Enqueue(available,ticket,arrival,rgbCopy&&mirror&&flipY,kind,deadline);
        if(!step.pairReady){history=true;++seeds;Record(arrival,submitted,submitted,0,4,ticket);status=Status::Pending;return nullptr;}
        history=true;++pairs;
        const auto miss=[&](int64_t decided){
            ++late;++lateCount;++modeLate[ScaleIndex(scale)];
            Record(arrival,submitted,decided,deadline,1,ticket);status=Status::Late;
        };
        if(kind==rife_vulkan::JobKind::HistoryOnly){miss(admitted);return nullptr;}
        const auto remaining=deadline-Now();
        if(remaining>0&&!FenceReady(SharedFenceProgress(fenceValue(),ticket))){
            ResetEvent(complete);Hr(fence->SetEventOnCompletion(ticket,complete),"Vulkan completion event");
            const auto sleepMs=remaining>1500?DWORD((remaining-1500)/1000):0;
            if(sleepMs)WaitForSingleObject(complete,sleepMs);
            while(!FenceReady(SharedFenceProgress(fenceValue(),ticket))&&Now()<deadline&&!backend->HasFailed()&&
                requested.load(std::memory_order_relaxed))YieldProcessor();
        }
        if(backend->HasFailed())backend->Free(); // Rethrows even without a fence signal.
        const bool completed=FenceReady(SharedFenceProgress(fenceValue(),ticket));
        const auto decision=Now();
        const bool ready=completed&&backend->GeneratesOutputFor(ticket)&&decision<=deadline;
        if(!requested.load(std::memory_order_acquire)||!VulkanSelected()){
            history=false;++cancelled;++modeCancelled[ScaleIndex(scale)];Record(arrival,submitted,decision,deadline,2,ticket);
            status=requested.load()?Status::Pending:Status::Off;return nullptr;
        }
        if(!ready){miss(decision);return nullptr;}
        ++onTime;Record(arrival,submitted,decision,deadline,0,ticket);
        Hr(context->Wait(fence.Get(),ticket),"Vulkan output acquire");
        if(ownerEnum.Get()!=originalEnum){generatedView.Reset();ownerEnum=originalEnum;Hr(vd->CreateVideoProcessorInputView(output.Get(),ownerEnum.Get(),&desc,&generatedView),"Vulkan generated input view");}
        status=Status::Active;return generatedView.Get();
    }
    void WaitOriginalSlot(int fps){
        if(fps<=0||!requested.load())return;
        context->Flush();const auto remaining=originalAt-Now();
        if(remaining<=0)return;
        LARGE_INTEGER due{};due.QuadPart=-LONGLONG(remaining*10);
        if(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,DWORD((remaining+999)/1000+10));
    }
    static void ReportDistribution(const wchar_t* label,std::vector<double>& values){
        if(values.empty()){
            fwprintf(stderr,L"[rife-vulkan] %ls: not measured (n=0)\n",label);
            return;
        }
        std::sort(values.begin(),values.end());
        const auto percentile=[&](double p){return values[size_t(p*double(values.size()-1)+0.5)];};
        fwprintf(stderr,L"[rife-vulkan] %ls p50=%.3f p95=%.3f p99=%.3f (n=%zu)\n",
            label,percentile(.5),percentile(.95),percentile(.99),values.size());
    }
    void Report(){
        Harvest();
        std::vector<double> ready, inputWait, workerPrep, enqueueToSubmit;
        uint64_t measuredGenerate=0,generateSubmittedAfterDeadline=0,expiredBeforeGraph=0;
        for(const auto& r:rows){
            if(r.outcome==0)ready.push_back(double(r.decided-r.arrival)/1000.0);
            const auto& t=r.vulkan;
            if(t.expiredBeforeGraph)++expiredBeforeGraph;
            if(t.inputReadyUs)inputWait.push_back(double(t.inputReadyUs-t.inputWaitBeginUs)/1000.0);
            if(t.inputWaitBeginUs)workerPrep.push_back(double(t.inputWaitBeginUs-t.enqueuedUs)/1000.0);
            if(t.submitBeginUs){
                enqueueToSubmit.push_back(double(t.submitBeginUs-t.enqueuedUs)/1000.0);
                if(t.submitEndUs&&t.submitResult==0&&t.kind==rife_vulkan::JobKind::Generate&&r.deadline){
                    ++measuredGenerate;
                    if(t.submitBeginUs>=r.deadline)++generateSubmittedAfterDeadline;
                }
            }
        }
        const auto shown=pairs?double(onTime)*100.0/double(pairs):0.0;
        fwprintf(stderr,L"[rife-vulkan] pid=%lu\n",GetCurrentProcessId());
        if(rgbCopies||rgbConversions)
            fwprintf(stderr,L"[rife-vulkan] RGB input copies=%llu processor=%llu\n",
                static_cast<unsigned long long>(rgbCopies),static_cast<unsigned long long>(rgbConversions));
        fwprintf(stderr,L"[rife-vulkan] pairs=%llu on_time=%llu (%.2f%%) late=%llu busy=%llu seeds=%llu cancelled=%llu\n",
            static_cast<unsigned long long>(pairs),static_cast<unsigned long long>(onTime),shown,static_cast<unsigned long long>(lateCount),
            static_cast<unsigned long long>(busyCount),static_cast<unsigned long long>(seeds),static_cast<unsigned long long>(cancelled));
        fwprintf(stderr,L"[rife-vulkan] generated Present accepted=%llu / interpolation inputs=%llu (%.2f%%; includes busy/seed inputs)\n",
            static_cast<unsigned long long>(generatedPresented),static_cast<unsigned long long>(inputAttempts),
            inputAttempts?double(generatedPresented)*100.0/double(inputAttempts):0.0);
        ReportDistribution(L"arrival->ready ms (on-time only)",ready);
        ReportDistribution(L"GPU ms",gpuMs);
        if(traceSubmission){
            ReportDistribution(L"enqueue->input check ms",workerPrep);
            ReportDistribution(L"D3D11 input wait ms (completed only)",inputWait);
            ReportDistribution(L"enqueue->Vulkan submit ms",enqueueToSubmit);
            fwprintf(stderr,L"[rife-vulkan] generated jobs submitted at/after deadline=%llu / %llu traced\n",
                static_cast<unsigned long long>(generateSubmittedAfterDeadline),static_cast<unsigned long long>(measuredGenerate));
            fwprintf(stderr,L"[rife-vulkan] expired before graph; kept only history=%llu\n",
                static_cast<unsigned long long>(expiredBeforeGraph));
        }
        uint64_t hudJobs=0,hudActive=0;double hudCore=0;
        uint64_t chromaJobs=0,chromaCapped=0;
        double chromaCore=0;
        if(backend&&backend->HudSummary(hudJobs,hudActive,hudCore,chromaJobs,chromaCore,chromaCapped)){
            fwprintf(stderr,L"[rife-vulkan] HUD detector jobs=%llu with_core=%llu mean_core=%.4f%%\n",
                static_cast<unsigned long long>(hudJobs),static_cast<unsigned long long>(hudActive),hudCore*100.0);
            if(vulkanHudChroma)
                fwprintf(stderr,L"[rife-vulkan] HUD chroma extra_core_jobs=%llu mean_extra_core=%.4f%% luma_fallback_jobs=%llu\n",
                    static_cast<unsigned long long>(chromaJobs),chromaCore*100.0,static_cast<unsigned long long>(chromaCapped));
        }
        if(vulkanTraceFile.empty()||rows.empty())return;
        try{
            constexpr auto header="pid,arrival_us,submitted_us,decided_us,deadline_us,outcome,"
                "generated_present_end_us,original_present_begin_us,original_present_end_us,ticket,"
                "worker_enqueued_us,d3d_input_wait_begin_us,d3d_input_ready_us,vk_submit_begin_us,vk_submit_end_us,vk_job_kind,vk_submit_result,expired_before_graph";
            const std::filesystem::path tracePath(vulkanTraceFile);
            {
                std::ifstream existing(tracePath);
                if(std::string line;std::getline(existing,line)&&line!=header)
                    throw std::runtime_error("incompatible trace format; use a new CSV file");
            }
            std::ofstream csv(tracePath,std::ios::app);
            csv.exceptions(std::ios::badbit|std::ios::failbit);
            // The process id ties each row to the same run's log lines.
            if(csv.tellp()==0)csv<<header<<'\n';
            static constexpr const char* names[]{"ready","late","cancelled","busy","seed","unsupported"};
            const auto pid=GetCurrentProcessId();
            for(const auto& r:rows){
                const auto& t=r.vulkan;
                csv<<pid<<','<<r.arrival<<','<<r.submitted<<','<<r.decided<<','<<r.deadline<<','<<names[r.outcome]
                    <<','<<r.generatedPresentEnd<<','<<r.originalPresentBegin<<','<<r.originalPresentEnd<<','<<r.ticket
                    <<','<<t.enqueuedUs<<','<<t.inputWaitBeginUs<<','<<t.inputReadyUs<<','<<t.submitBeginUs<<','<<t.submitEndUs
                    <<','<<(t.ticket?(t.kind==rife_vulkan::JobKind::Generate?"generate":"history"):"")<<',';
                if(t.submitEndUs)csv<<t.submitResult;
                csv<<','<<int(t.expiredBeforeGraph)<<'\n';
            }
        }catch(const std::exception& e){fprintf(stderr,"[rife-vulkan] trace not written: %s\n",e.what());}
    }
    ~VulkanFast(){
        startupStop.request_stop();
        // The backend keeps ownership of the shared textures until retirement.
        if(backend)Report();
        backend.reset();
        if(complete)CloseHandle(complete);if(timer)CloseHandle(timer);
    }
};
#endif
Half::Half()=default;Half::~Half()=default;
void Half::Reset(){
#ifdef LLCV_FLYHIGH_TEST
    flyHigh_.reset();
    flyHighFailed_=false;
#endif
#ifdef LLCV_RIFE_VULKAN
    vulkanFast_.reset();vulkanFailedIntent_.reset();
#endif
    impl_.reset();failed_=false;status=Status::Off;}
void Half::Invalidate(){
#ifdef LLCV_RIFE_VULKAN
    if(vulkanFast_)vulkanFast_->history=false;
    vulkanFailedIntent_.reset();
#endif
    if(impl_){impl_->history=false;
#ifdef LLCV_RIFE_AUTO_UI_TEST
    impl_->uiReset=true;
#endif
}
#ifdef LLCV_FLYHIGH_TEST
    if(flyHigh_)flyHigh_->Invalidate();
    flyHighFailed_=false;
#endif
failed_=false;}
ID3D11VideoProcessorInputView* Half::Submit(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11VideoDevice* vd,ID3D11VideoContext* vc,ID3D11VideoProcessor* processor,ID3D11VideoProcessorEnumerator* owner,ID3D11Texture2D* source,int width,int height,int fps,int64_t arrival){
#ifdef LLCV_FLYHIGH_TEST
    if(IsFlyHigh()){
        impl_.reset();failed_=false;
        if(flyHighFailed_){status=Status::Failed;return nullptr;}
        try{
            if(flyHigh_&&(flyHigh_->W!=width||flyHigh_->H!=height))flyHigh_.reset();
            if(!flyHigh_){flyHigh_=std::make_unique<FlyHigh>();flyHigh_->Initialize(d,c,vd,vc,width,height,fps);status=Status::Pending;return nullptr;}
            return flyHigh_->Submit(source,processor,owner,fps,arrival);
        }catch(const std::exception& e){fprintf(stderr,"[flyhigh] original fallback: %s\n",e.what());flyHigh_.reset();flyHighFailed_=true;status=Status::Failed;return nullptr;}
    }
    flyHigh_.reset();flyHighFailed_=false;
#endif
#ifdef LLCV_RIFE_VULKAN
    if(VulkanSelected()){
        impl_.reset();failed_=false;
        const auto intent=intentChanges.load(std::memory_order_acquire);
        if(vulkanFailedIntent_==intent){status=Status::Failed;return nullptr;}
        // F7 can be toggled twice before another capture frame arrives.
        // Retry once for the new intent even if the render path never saw Off.
        vulkanFailedIntent_.reset();
        try{
            if(vulkanFast_&&(vulkanFast_->W!=UINT(width)||vulkanFast_->H!=UINT(height)))vulkanFast_.reset();
            if(!vulkanFast_){
#ifdef LLCV_RIFE_OPTIONAL
                fallbackDetail.store(L"");
#endif
                vulkanFast_=std::make_unique<VulkanFast>();vulkanFast_->Initialize(d,c,vd,vc,width,height,fps);
            }
            if(!vulkanFast_->PollBackend()){status=Status::Pending;return nullptr;}
            return vulkanFast_->Submit(source,processor,owner,fps,arrival);
        }catch(const std::exception& e){
#ifdef LLCV_RIFE_OPTIONAL
            fallbackDetail.store(L"Vulkan runtime error");
#endif
            fprintf(stderr,"[rife-vulkan] original fallback: %s\n",e.what());
            vulkanFast_.reset();vulkanFailedIntent_=intent;status=Status::Failed;return nullptr;
        }
    }
    vulkanFast_.reset();vulkanFailedIntent_.reset();
#endif
    const auto selected=selectedScale.load(std::memory_order_acquire);
#ifdef LLCV_INTERPOLATION_RELEASE
    namespace runtime = llcv::vfg_runtime;
    const bool isVfg = IsVfg(selected);
    const auto runtimeStage = isVfg && (!impl_ || impl_->scale != selected || failed_)
        ? runtime::GetStatus().stage : runtime::Stage::Idle;
    const bool initializingWithoutVerifiedRuntime = isVfg && (!impl_ || impl_->scale != selected) &&
        runtimeStage != runtime::Stage::Ready;
    if (failed_ && failedScale_ == selected &&
        !(isVfg && runtime::CanRetryWithInstalledRuntime(failedVfgWithoutVerifiedRuntime_, runtimeStage))) {
        status = isVfg && failedVfgWithoutVerifiedRuntime_ && runtime::InProgress(runtimeStage)
            ? Status::Pending : Status::Failed;
        return nullptr;
    }
#else
    if(failed_&&failedScale_==selected){status=Status::Failed;return nullptr;}
#endif
    failed_=false;
    try{
#ifdef LLCV_RIFE_OPTIONAL
        if(!impl_||impl_->scale!=selected){
            fallbackDetail.store(L"");PrepareOptionalRuntime(selected,width);
        }
#endif
        // A live owner already initialized the driver. Still preserve the caller's context per submission.
        if(!impl_)Check(cuInit(0)==CUDA_SUCCESS,"RIFE CUDA initialization");
        CudaContextScope caller;Check(bool(caller),"RIFE caller context");
        if(impl_&&(impl_->W!=UINT(width)||impl_->H!=UINT(height)))impl_.reset();
#ifdef LLCV_RIFE_OPTIONAL
        if(impl_&&impl_->scale!=selected&&(IsVfg(impl_->scale)||IsVfg(selected)))impl_.reset();
#endif
        if(!impl_){impl_=std::make_unique<Impl>();impl_->Initialize(d,c,vd,vc,width,height,fps,selected);return nullptr;}
        CudaContextScope current(impl_->cudaContext);Check(bool(current),"RIFE context activation");
        if(impl_->scale!=selected){
            impl_->history=false;status=Status::Pending;
            if(impl_->Idle())impl_->LoadModel(selected);
            return nullptr;
        }
        return impl_->Submit(source,processor,owner,fps,arrival);
    }catch(const std::exception& e){
#ifdef LLCV_RIFE_OPTIONAL
        if(!*fallbackDetail.load()){
            if(strstr(e.what(),"mismatch")||strstr(e.what(),"needs NVIDIA"))fallbackDetail.store(L"GPU mismatch");
            else if(strstr(e.what(),"not built for"))fallbackDetail.store(L"GPU / engine mismatch");
            else fallbackDetail.store(L"runtime error");
        }
#endif
        fprintf(stderr,"[rife-live] fallback to original: %s\n",e.what());impl_.reset();failed_=true;failedScale_=selected;
#ifdef LLCV_INTERPOLATION_RELEASE
        failedVfgWithoutVerifiedRuntime_=initializingWithoutVerifiedRuntime;
        status=failedVfgWithoutVerifiedRuntime_ && runtime::InProgress(runtimeStage) ? Status::Pending : Status::Failed;
#else
        status=Status::Failed;
#endif
        return nullptr;}
}
void Half::ScheduleOriginalSlot(int fps,[[maybe_unused]] int64_t arrival){
#ifdef LLCV_FLYHIGH_TEST
    if(flyHigh_&&fps>0)flyHigh_->originalAt=Now()+500000/fps;
#endif
#ifdef LLCV_RIFE_VULKAN
    if(vulkanFast_&&fps>0){
        const auto now=Now();
        vulkanFast_->originalAt=vulkanTryCapturePacing
            ?CaptureBoundedOriginalSlot(arrival,now,fps):now+500000/fps;
    }
#endif
    if(impl_&&fps>0)impl_->originalAt=Now()+500000/fps;}
#ifdef LLCV_RIFE_VULKAN
void Half::RecordPresent(int64_t arrival,bool interpolated,int64_t begin,int64_t end){
    if(!vulkanFast_)return;
    if(interpolated)++vulkanFast_->generatedPresented;
    if(!begin||vulkanFast_->rows.empty())return;
    auto& row=vulkanFast_->rows.back();
    if(row.arrival!=arrival)return;
    if(interpolated)row.generatedPresentEnd=end;
    else {row.originalPresentBegin=begin;row.originalPresentEnd=end;}
}
#endif
void Half::WaitOriginalSlot(int fps){
#ifdef LLCV_FLYHIGH_TEST
    if(flyHigh_){flyHigh_->WaitOriginalSlot(fps);return;}
#endif
#ifdef LLCV_RIFE_VULKAN
    if(vulkanFast_){vulkanFast_->WaitOriginalSlot(fps);return;}
#endif
    if(!impl_||fps<=0||!requested.load())return;
    // Original-frame scaling/OSD has already been queued. Let it run while
    // waiting for its display slot, instead of adding it after a full delay.
    impl_->context->Flush();const auto remaining=impl_->originalAt-Now();
    if(remaining<=0)return;
    LARGE_INTEGER due{};due.QuadPart=-LONGLONG(remaining*10);
    if(SetWaitableTimer(impl_->timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(impl_->timer,DWORD((remaining+999)/1000+10));
}
}
