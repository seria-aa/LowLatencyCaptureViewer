#pragma once
// Private diagnostic library only; never linked into llcv_upscaling/release.
#ifndef LLCV_UPSCALING_TELEMETRY
#error Upscaling overflow telemetry is diagnostic-only
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <string>

namespace llcv::upscaling {
struct OverflowCounts {
    uint64_t frames=0,collected=0,overflow=0,candidates=0,items=0,locations=0;
    uint64_t skipped=0,abandoned=0,queryErrors=0,mapErrors=0,initErrors=0,disabled=0;
    uint64_t transitions=0,adjacentPairs=0,ageSum=0,maxAge=0,streams=0;
    uint64_t lastWidth=0,lastHeight=0;
};
OverflowCounts ReadOverflowCounts() noexcept;
std::string FormatOverflowReport(const OverflowCounts&);
// Harness controls: only change between pipelines/runs, never concurrently.
void ResetOverflowCounts() noexcept;
void SetOverflowSamplingEnabled(bool enabled) noexcept;
bool OverflowSamplingEnabled() noexcept;

class OverflowTelemetry {
    friend struct OverflowTelemetryTestAccess;
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Slot {Ptr<ID3D11Buffer> buffer;Ptr<ID3D11Query> event;uint64_t frame=0;bool busy=false;};
    static constexpr unsigned kSlots=8;
    static constexpr uint64_t kDelay=3;
    std::array<Slot,kSlots> slots_{};
    unsigned read_=0,write_=0,pending_=0;
    uint64_t frame_=0,previousFrame_=0;
    bool previousOverflow_=false,ready_=false;
    void Poll(ID3D11DeviceContext*) noexcept;
public:
    OverflowTelemetry(ID3D11Device*,UINT width,UINT height) noexcept;
    ~OverflowTelemetry();
    OverflowTelemetry(const OverflowTelemetry&)=delete;
    OverflowTelemetry& operator=(const OverflowTelemetry&)=delete;
    // Bounded polling, no wait/Flush/retry loop, no file I/O or allocation.
    void Observe(ID3D11DeviceContext*,ID3D11Buffer* control) noexcept;
};
}
