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
}
