#pragma once
#include <cstdint>

namespace llcv::rife_test {
inline constexpr int64_t kHudHistoryMaxGapUs=100000;
constexpr bool HudHistoryDiscontinuity(int64_t previous,int64_t current){
    return previous<=0||current<=previous||current-previous>kHudHistoryMaxGapUs;
}
struct InputHistoryStep { bool pairReady=false; bool resetHud=false; };
constexpr InputHistoryStep PlanInputHistory(int64_t previous,int64_t current,int fps,bool pairHistory){
    const bool invalid=previous<=0||current<=previous;
    const int64_t gap=invalid?0:current-previous;
    return {pairHistory&&!invalid&&fps>0&&gap<=1500000/fps,HudHistoryDiscontinuity(previous,current)};
}
constexpr bool CanGeneratePair(bool pairReady,int64_t admitted,int64_t deadline) {
    return pairReady&&admitted<deadline;
}
inline constexpr int64_t kVulkanStartupLimitUs=10'000'000;
constexpr bool VulkanStartupExpired(int64_t started,int64_t now) {
    return started>0&&now>=started&&now-started>=kVulkanStartupLimitUs;
}

enum class SharedFenceState { Waiting, Ready, DeviceRemoved };
constexpr SharedFenceState SharedFenceProgress(uint64_t completed,uint64_t ticket) {
    if(completed==UINT64_MAX)return SharedFenceState::DeviceRemoved;
    return completed>=ticket?SharedFenceState::Ready:SharedFenceState::Waiting;
}
template<class WorkerFree,class ReadFence>
SharedFenceState CheckPreviousVulkanJob(uint64_t ticket,WorkerFree workerFree,ReadFence readFence) {
    // A failed worker may never signal this ticket. Read its error first.
    const bool idle=workerFree();
    const auto state=SharedFenceProgress(readFence(),ticket);
    return !idle&&state==SharedFenceState::Ready?SharedFenceState::Waiting:state;
}

// Comparison policy: keep normal spacing when it fits, but leave time to
// submit the original before the next predicted capture. This is a CPU wait
// target, not a guarantee about when the monitor scans the frame out.
constexpr int64_t CaptureBoundedOriginalSlot(int64_t arrival,int64_t afterPresent,int fps) {
    if(fps<=0||arrival<=0||afterPresent<arrival)return afterPresent;
    const int64_t half=500000/fps;
    const int64_t normal=afterPresent+half;
    const int64_t capped=arrival+2*half-half/8;
    return capped<afterPresent?afterPresent:capped<normal?capped:normal;
}
}
