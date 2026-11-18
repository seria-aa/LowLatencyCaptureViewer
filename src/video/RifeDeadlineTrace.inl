// Included only by the private trajectory viewer. File I/O is deferred until teardown.
struct DeadlineTrace {
    struct Row {int64_t arrival,submit,decision,deadline;int fps,protect,ready;
        double begin=-1,net=-1,end=-1;};
    cudaEvent_t anchor=nullptr,pre=nullptr,post=nullptr,finish=nullptr;
    int64_t lo=0,hi=0;std::vector<Row> rows;int active=-1;
    void init(cudaStream_t s){
        wchar_t flag[2]{};
        if(GetEnvironmentVariableW(L"LLCV_RIFE_DEADLINE_TRACE",flag,2)!=1||flag[0]!=L'1')return;
        rows.reserve(8192);
        for(auto p:{&anchor,&pre,&post,&finish})Cu(cudaEventCreate(p),"trace event");
        lo=Now();Cu(cudaEventRecord(anchor,s),"trace clock");Cu(cudaEventSynchronize(anchor),"trace sync");hi=Now();
    }
    double elapsed(cudaEvent_t a,cudaEvent_t b)const{
        float ms=0;return cudaEventElapsedTime(&ms,a,b)==cudaSuccess?ms*1000.:-1.;
    }
    void harvest(cudaEvent_t begin,cudaEvent_t end){
        if(active<0)return;
        auto& r=rows[active];r.begin=elapsed(anchor,begin);r.net=elapsed(pre,post);r.end=elapsed(anchor,end);active=-1;
    }
    void save(cudaStream_t s){
        if(!anchor||!finish)return;
        auto tailLo=Now();auto rc=cudaEventRecord(finish,s);
        if(rc==cudaSuccess)rc=cudaEventSynchronize(finish);
        auto tailHi=Now();
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
        std::ofstream out(std::filesystem::path(exe).parent_path()/
            (L"deadline-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(lo)+L".csv"));
        out.precision(16);
        out<<"# clock_us,"<<lo<<','<<hi<<','<<tailLo<<','<<tailHi<<','<<(rc==cudaSuccess?elapsed(anchor,finish):-1)<<"\n";
        out<<"arrival,submit,decision,deadline,fps,protect,ready,gpu_start_offset,net_us,gpu_end_offset\n";
        for(auto& r:rows)out<<r.arrival<<','<<r.submit<<','<<r.decision<<','<<r.deadline<<','<<r.fps<<','<<r.protect<<','<<r.ready<<','<<r.begin<<','<<r.net<<','<<r.end<<'\n';
    }
    ~DeadlineTrace(){for(auto e:{anchor,pre,post,finish})if(e)cudaEventDestroy(e);}
};
