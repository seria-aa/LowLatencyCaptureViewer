// Private GPU A/B timing. Uses no capture device, Present, or persisted settings.
static void PreserveTiming(const std::filesystem::path& shader,const std::filesystem::path& saved,
                           const std::filesystem::path& output) {
    using namespace text_comparison;
    Require(llcv::test_profile::Enabled(),"GPU preserve timing requires isolated profile");
    Require(!std::filesystem::exists(output),"new preserve GPU timing directory");std::filesystem::create_directories(output);
    std::ifstream file(saved,std::ios::binary);Require(bool(file),"saved game for GPU timing");
    const Pixels game{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    Require(game.size()==size_t(W)*H*3/2,"GPU input FHD NV12 size");
    std::ofstream csv(output/"gpu-rounds.csv"),raw(output/"gpu-frames.csv"),info(output/"method.txt");
    csv<<"scene,width,height,round,mode,samples,mean_ms,p50_ms,p95_ms\n";
    raw<<"scene,width,height,round,mode,index,gpu_ms\n";csv<<std::setprecision(9);raw<<std::setprecision(9);
    info<<"RTX adapter recorded below. FHD NV12 BT.709 Limited -> QHD/4K. NIS=0.15.\n"
        <<"Both variants compiled from the same source/options, differing only ExtraSharpness 0/1.\n"
        <<"Six paired rounds; alternate AB/BA; >=500ms sustained warmup then 120 timestamped frames per method per round.\n"
        <<"Continuous GPU load (no sleep); GPU interval includes upload, VP, CMAA2, NIS and output dependency.\n"
        <<"No Present/capture hardware; these are GPU work times, not capture-to-display or optical latency.\n";
    for(bool savedGame:{false,true})for(SIZE size:{SIZE{2560,1440},SIZE{3840,2160}}) {
        const char* scene=savedGame?"saved-game":"synthetic";
        const auto input=savedGame?game:Pattern(W,H);const auto unchanged=input;
        HWND window=Window(UINT(size.cx),UINT(size.cy));g_cmaaNisRequest=15;DirectD3D11Renderer r;
        llcv::video_color::Configuration color{};color.matrix=llcv::video_color::Matrix::Bt709;color.range=llcv::video_color::Range::Limited;
        Check(r.initialize(window,W,H,60,VideoPixelFormat::Nv12,false,color),"preserve GPU renderer");
        Require(r.cmaaNisPipeline.Enabled(),"preserve GPU filter active");
        Render(r,input,W);const auto shipping=Read(r);
        const auto high=Compile(r.device,shader,false),preserve=Compile(r.device,shader,true);
        Access::Programs(r.cmaaNisPipeline,high);Render(r,input,W);
        Require(Difference(Read(r),shipping)<=1,"GPU high matches shipping path");
        Timer timer(r.device);
        for(int round=0;round<6;++round)for(int at=0;at<2;++at) {
            const bool keep=(round&1)?at==0:at==1;const char* mode=keep?"preserve":"high";
            Access::Programs(r.cmaaNisPipeline,keep?preserve:high);
            const ULONGLONG warmUntil=GetTickCount64()+500;unsigned warmFrames=0;
            do {Render(r,input,W);if(++warmFrames%32==0)Read(r);}while(GetTickCount64()<warmUntil);
            Read(r);
            Require(Access::Overflow(r.cmaaNisPipeline,r.device,r.context)==0,"GPU timing does not bypass AA");
            std::vector<double> times;
            for(int i=0;i<120;++i) {
                timer.Begin(r.context);Render(r,input,W);const double ms=timer.End(r.context,r.backBuffer);
                times.push_back(ms);raw<<scene<<','<<size.cx<<','<<size.cy<<','<<round<<','<<mode<<','<<i<<','<<ms<<'\n';
            }
            const double mean=std::accumulate(times.begin(),times.end(),0.0)/times.size();
            csv<<scene<<','<<size.cx<<','<<size.cy<<','<<round<<','<<mode<<','<<times.size()<<','<<mean<<','<<Percentile(times,.5)<<','<<Percentile(times,.95)<<'\n';csv.flush();
            std::printf("GPU %s %ld round=%d %s mean=%.4f p95=%.4f ms\n",scene,size.cx,round,mode,mean,Percentile(times,.95));
        }
        Require(input==unchanged,"GPU timing input preserved");
        info<<scene<<' '<<size.cx<<" adapter_vendor="<<r.adapterVendor<<"; high parity, no bypass and input preservation passed\n";
        r.reset();DestroyWindow(window);
    }
    Require(bool(csv)&&bool(raw)&&bool(info),"GPU timing reports saved");g_cmaaNisRequest=-1;
}
