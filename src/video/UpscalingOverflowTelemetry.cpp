#include "UpscalingOverflowTelemetry.h"
#include <atomic>
#include <cstring>
#include <sstream>

namespace llcv::upscaling {
namespace {
#define OVERFLOW_FIELDS(X) \
    X(frames) X(collected) X(overflow) X(candidates) X(items) X(locations) \
    X(skipped) X(abandoned) X(queryErrors) X(mapErrors) X(initErrors) X(disabled) \
    X(transitions) X(adjacentPairs) X(ageSum) X(maxAge) X(streams) X(lastWidth) X(lastHeight)
struct Counters {
#define FIELD(name) std::atomic<uint64_t> name{0};
    OVERFLOW_FIELDS(FIELD)
#undef FIELD
} counters;
std::atomic<bool> enabled{true};
void Add(std::atomic<uint64_t>& value,uint64_t amount=1) noexcept {value.fetch_add(amount,std::memory_order_relaxed);}
}
OverflowCounts ReadOverflowCounts() noexcept {
    OverflowCounts result;
#define READ(name) result.name=counters.name.load(std::memory_order_relaxed);
    OVERFLOW_FIELDS(READ)
#undef READ
    return result;
}
void ResetOverflowCounts() noexcept {
#define ZERO(name) counters.name.store(0,std::memory_order_relaxed);
    OVERFLOW_FIELDS(ZERO)
#undef ZERO
}
#undef OVERFLOW_FIELDS
void SetOverflowSamplingEnabled(bool value) noexcept {enabled.store(value,std::memory_order_relaxed);}
bool OverflowSamplingEnabled() noexcept {return enabled.load(std::memory_order_relaxed);}
std::string FormatOverflowReport(const OverflowCounts& s) {
    const uint64_t unknown=s.frames>=s.collected?s.frames-s.collected:0;
    std::ostringstream out;
    out<<"LLCV private CMAA2 overflow diagnostic\n"
       <<"Counts describe CMAA2 render calls, NOT distinct capture frames or all app frames.\n"
       <<"HDR, disabled filter and non-upscaled output are not sampled.\n"
       <<"frames="<<s.frames<<" collected="<<s.collected<<" overflow="<<s.overflow<<" unknown="<<unknown<<'\n'
       <<"candidate_overflow="<<s.candidates<<" item_overflow="<<s.items<<" location_overflow="<<s.locations<<'\n'
       <<"ring_full_skipped="<<s.skipped<<" abandoned_on_reset="<<s.abandoned<<" disabled_frames="<<s.disabled<<'\n'
       <<"query_errors="<<s.queryErrors<<" map_errors="<<s.mapErrors<<" init_errors="<<s.initErrors<<'\n'
       <<"adjacent_sample_pairs="<<s.adjacentPairs<<" observed_transitions="<<s.transitions<<'\n'
       <<"streams="<<s.streams<<" last_input="<<s.lastWidth<<'x'<<s.lastHeight<<'\n';
    if(s.frames)out<<"coverage_percent="<<100.0*s.collected/s.frames
        <<" overflow_lower_bound_percent="<<100.0*s.overflow/s.frames
        <<" overflow_upper_bound_percent="<<100.0*(s.overflow+unknown)/s.frames<<'\n';
    if(s.collected)out<<"observed_overflow_percent="<<100.0*s.overflow/s.collected
        <<" mean_read_age="<<double(s.ageSum)/s.collected<<" max_read_age="<<s.maxAge<<'\n';
    else out<<"No samples collected. This does NOT establish zero overflows.\n";
    out<<"Unknown samples are never counted as clean. Reason counters can overlap.\n"
       <<"No GPU drain on shutdown; pending samples remain unknown.\n";
    return out.str();
}

OverflowTelemetry::OverflowTelemetry(ID3D11Device* device,UINT width,UINT height) noexcept {
    Add(counters.streams);counters.lastWidth=width;counters.lastHeight=height;
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(UINT);desc.Usage=D3D11_USAGE_STAGING;
    desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};
    if(!device) {Add(counters.initErrors);return;}
    for(auto& slot:slots_) {
        if(FAILED(device->CreateBuffer(&desc,nullptr,&slot.buffer)) ||
           FAILED(device->CreateQuery(&query,&slot.event))) {Add(counters.initErrors);return;}
    }
    ready_=true;
}
OverflowTelemetry::~OverflowTelemetry() {Add(counters.abandoned,pending_);}
void OverflowTelemetry::Poll(ID3D11DeviceContext* context) noexcept {
    // Queries are submitted in order. A busy oldest slot ends this poll; never
    // flush or spin to make a query complete. At most eight ready slots per call.
    for(unsigned n=0;n<kSlots&&pending_;++n) {
        auto& slot=slots_[read_];
        if(frame_<slot.frame || frame_-slot.frame<kDelay)break;
        BOOL done=FALSE;
        const HRESULT result=context->GetData(slot.event.Get(),&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(result==S_FALSE || (result==S_OK&&!done))break;
        if(FAILED(result))Add(counters.queryErrors);
        else {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT map=context->Map(slot.buffer.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if(map==DXGI_ERROR_WAS_STILL_DRAWING)break;
            if(FAILED(map))Add(counters.mapErrors);
            else {
                UINT flags=0;std::memcpy(&flags,mapped.pData,sizeof(flags));context->Unmap(slot.buffer.Get(),0);
                Add(counters.collected);const bool overflow=flags!=0;
                if(overflow)Add(counters.overflow);
                if(flags&1)Add(counters.candidates);
                if(flags&2)Add(counters.items);
                if(flags&4)Add(counters.locations);
                if(previousFrame_&&slot.frame==previousFrame_+1) {
                    Add(counters.adjacentPairs);if(overflow!=previousOverflow_)Add(counters.transitions);
                }
                previousFrame_=slot.frame;previousOverflow_=overflow;
                const uint64_t age=frame_-slot.frame;Add(counters.ageSum,age);
                // The rendering thread is the only writer; no unbounded CAS loop.
                if(age>counters.maxAge.load(std::memory_order_relaxed))counters.maxAge.store(age,std::memory_order_relaxed);
            }
        }
        slot.busy=false;--pending_;read_=(read_+1)%kSlots;
    }
}
void OverflowTelemetry::Observe(ID3D11DeviceContext* context,ID3D11Buffer* control) noexcept {
    ++frame_;Add(counters.frames);
    if(!ready_||!context||!control) {Add(counters.disabled);return;}
    Poll(context);
    if(pending_==kSlots) {Add(counters.skipped);return;}
    auto& slot=slots_[write_];
    const D3D11_BOX word{0,0,0,sizeof(UINT),1,1};
    context->CopySubresourceRegion(slot.buffer.Get(),0,0,0,0,control,0,&word);
    context->End(slot.event.Get());slot.frame=frame_;slot.busy=true;
    ++pending_;write_=(write_+1)%kSlots;
}
}
