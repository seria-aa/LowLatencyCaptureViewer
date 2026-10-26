// Reuse the synthetic GPU helpers and the unchanged shader safety regressions.
#define main UpscalingSafetyMain
#include "UpscalingSafetyTests.cpp"
#undef main
#include <chrono>
#include <thread>
#include <numeric>

namespace llcv::upscaling {
struct OverflowTelemetryTestAccess {
    static void SaturateYoungSlots(OverflowTelemetry& t) {
        t.pending_=8;t.read_=t.write_=0;
        for(auto& slot:t.slots_) {slot.busy=true;slot.frame=1000;}
    }
};
}
static void DiagnosticRegression(Gpu& g) {
    using namespace llcv::upscaling;
    constexpr UINT w=97,h=73;
    ResetOverflowCounts();SetOverflowSamplingEnabled(false);
    Image baseline;
    {
        CmaaPipeline p;Check(p.Configure(g.d.Get(),w,h));baseline=Cmaa(g,p,Pattern(w,h,4),w,h);
        Require(ReadOverflowCounts().frames==0,"diagnostic disabled does not enqueue/read telemetry");
    }
    SetOverflowSamplingEnabled(true);ResetOverflowCounts();
    {
        CmaaPipeline p;Check(p.Configure(g.d.Get(),w,h));
        Require(Difference(baseline,Cmaa(g,p,Pattern(w,h,4),w,h),w,0,0,w,h)<=1.f/255,
            "telemetry does not change SDR pixels");
        Require(ReadOverflowCounts().collected==0,"first frame is never read synchronously");
    }
    Require(ReadOverflowCounts().abandoned==1,"shutdown does not drain GPU");
    ResetOverflowCounts();
    {
        CmaaPipeline p;Check(p.Configure(g.d.Get(),w,h));
        for(unsigned n=0;n<24;n++) {
            Cmaa(g,p,Pattern(w,h,n%2?5:0),w,h); // helper's image readback is test-only
            const auto s=ReadOverflowCounts();
            Require(s.frames==n+1&&s.collected==(n<3?0:n-2),"three-frame asynchronous read age");
        }
        const auto s=ReadOverflowCounts();
        Require(s.collected==21&&s.overflow==10&&s.items==10,"actual shader flags collected exactly once");
        Require(s.candidates==0&&s.locations==0,"overflow reasons remain separate");
        Require(s.adjacentPairs==20&&s.transitions==20,"adjacent frame transition counts");
        Require(s.ageSum==63&&s.maxAge==3&&s.skipped==0,"read delay and full coverage before shutdown tail");
    }
    auto s=ReadOverflowCounts();Require(s.abandoned==3&&s.frames-s.collected==3,"pending tail reported unknown");
    const auto report=FormatOverflowReport(s);
    Require(report.find("unknown=3")!=std::string::npos&&report.find("overflow_upper_bound_percent=")!=std::string::npos,
        "missing samples not counted as clean");
    ResetOverflowCounts();
    {
        OverflowTelemetry disabled(nullptr,w,h);disabled.Observe(g.c.Get(),nullptr);
        s=ReadOverflowCounts();Require(s.initErrors==1&&s.disabled==1&&s.frames==1,"allocation failure disables only measurement");
    }
    ResetOverflowCounts();
    {
        OverflowTelemetry full(g.d.Get(),w,h);OverflowTelemetryTestAccess::SaturateYoungSlots(full);
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=64;Ptr<ID3D11Buffer> control;Check(g.d->CreateBuffer(&desc,nullptr,&control));
        full.Observe(g.c.Get(),control.Get());s=ReadOverflowCounts();
        Require(s.frames==1&&s.skipped==1&&s.collected==0,"full ring skips immediately without overwriting pending data");
    }
    Require(FormatOverflowReport(ReadOverflowCounts()).find("No samples collected")!=std::string::npos,
        "zero samples cannot establish zero overflows");
    std::puts("PASS: telemetry pixels, delayed exact flags/reasons, transitions, shutdown unknown, init failure, ring-full skip, report coverage");
    ResetOverflowCounts();
}
static void DiagnosticBenchmark(Gpu& g) {
    using namespace llcv::upscaling;
    constexpr UINT w=1920,h=1080;
    const auto input=Pattern(w,h,4);
    auto texture=g.Texture(w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_BIND_SHADER_RESOURCE,input.data(),w*sizeof(Pixel));
    auto view=g.View(texture.Get());D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    Ptr<ID3D11RasterizerState> raster;Check(g.d->CreateRasterizerState(&rd,&raster));
    Ptr<ID3D11Query> disjoint,start,end;
    D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT,0};Check(g.d->CreateQuery(&q,&disjoint));
    q.Query=D3D11_QUERY_TIMESTAMP;Check(g.d->CreateQuery(&q,&start));Check(g.d->CreateQuery(&q,&end));
    std::puts("BENCH: synthetic FHD CMAA stage only; 60 Hz host pacing; GPU waits/readback OUTSIDE measured CPU Draw; no capture latency claim");
    for(unsigned round=0;round<3;round++)for(unsigned order=0;order<2;order++) {
        const bool enabled=(round+order)%2!=0;SetOverflowSamplingEnabled(enabled);ResetOverflowCounts();
        CmaaPipeline p;Check(p.Configure(g.d.Get(),w,h));std::vector<double> gpu,cpu;
        for(unsigned frame=0;frame<145;frame++) {
            const auto tick=std::chrono::steady_clock::now();
            g.c->ClearState();g.c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);g.c->RSSetState(raster.Get());
            g.c->Begin(disjoint.Get());g.c->End(start.Get());
            const auto before=std::chrono::steady_clock::now();p.Draw(g.c.Get(),view.Get());
            const auto after=std::chrono::steady_clock::now();g.c->End(end.Get());g.c->End(disjoint.Get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};UINT64 a=0,b=0;
            // Benchmark host only: drain timestamp queries outside CPU timing.
            // Do not copy/convert a full FHD image just to wait for completion.
            g.c->Flush();HRESULT ready=S_FALSE;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while((ready=g.c->GetData(disjoint.Get(),&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH))==S_FALSE) {
                Require(std::chrono::steady_clock::now()<deadline,"benchmark query deadline");Sleep(1);
            }
            Require(ready==S_OK&&!d.Disjoint,"benchmark timestamp ready");
            Require(g.c->GetData(start.Get(),&a,sizeof(a),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK&&
                    g.c->GetData(end.Get(),&b,sizeof(b),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK,"benchmark timestamp values");
            if(frame>=25) {gpu.push_back(double(b-a)*1000/d.Frequency);cpu.push_back(std::chrono::duration<double,std::milli>(after-before).count());}
            std::this_thread::sleep_until(tick+std::chrono::microseconds(16667));
        }
        auto mean=[](const auto& values) {return std::accumulate(values.begin(),values.end(),0.0)/values.size();};
        auto p95=[](auto values) {std::sort(values.begin(),values.end());return values[size_t(.95*(values.size()-1))];};
        const auto counts=ReadOverflowCounts();
        Require(!enabled||counts.collected==142,"benchmark telemetry actually collected");
        std::printf("BENCH: round=%u telemetry=%u gpu_mean_ms=%.6f gpu_p95_ms=%.6f cpu_mean_ms=%.6f cpu_p95_ms=%.6f frames=%llu collected=%llu skipped=%llu\n",
            round,unsigned(enabled),mean(gpu),p95(gpu),mean(cpu),p95(cpu),counts.frames,counts.collected,counts.skipped);
        std::fflush(stdout);
    }
    SetOverflowSamplingEnabled(true);
}
int main(int argc,char** argv) try {
    bool warp=false,benchmark=false;
    for(int i=1;i<argc;i++) {
        if(std::string(argv[i])=="--warp")warp=true;
        else if(std::string(argv[i])=="--benchmark")benchmark=true;
        else Require(false,"unknown telemetry option");
    }
    Gpu g(warp);if(benchmark)DiagnosticBenchmark(g);
    else {ListValidatorTests();DiagnosticRegression(g);Boundaries(g);Overflow(g);FlatTransitions(g);OverflowSequence(g);}
    std::printf("PASS: diagnostic telemetry (%s)\n",warp?"WARP":"hardware");return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
