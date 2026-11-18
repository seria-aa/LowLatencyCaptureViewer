#include "video/ExclusivePresentationState.h"
#include "video/PresentationPolicy.h"
#include "video/PresentationReadiness.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace {
unsigned checks = 0;

void Check(bool value, const char* message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::abort();
    }
}
}

int main() {
    using llcv::presentation::Mode;
    {
        using namespace llcv::presentation;
        Check(VfgPresentationBlock(Mode::AllowTearing,60,0,false)==VfgBlock::None &&
              VfgPresentationBlock(Mode::ExclusiveFullscreen,60,0,false)==VfgBlock::None,
              "existing immediate and exclusive VFG paths need no waitable object");
        Check(VfgPresentationBlock(Mode::Compatibility,30,144,true)==VfgBlock::Compatibility,
              "legacy Blt output does not support VFG");
        Check(VfgPresentationBlock(Mode::VSync,60,120,false)==VfgBlock::SyncUnavailable,
              "VSync VFG needs frame admission");
        for(const auto hz:{0u,60u,118u})
            Check(VfgPresentationBlock(Mode::VSync,60,hz,true)==VfgBlock::RefreshRate,
                  "VSync with unknown or insufficient refresh keeps original playback");
        Check(VfgPresentationBlock(Mode::VSync,30,59,true)==VfgBlock::None &&
              VfgPresentationBlock(Mode::VSync,60,119,true)==VfgBlock::None &&
              VfgPresentationBlock(Mode::VSync,60,144,true)==VfgBlock::None,
              "VSync accepts full double-rate cadence and fractional Windows modes");
        Check(!UseVfgOriginalTimer(Mode::VSync,60,120) &&
               UseVfgOriginalTimer(Mode::VSync,30,144) &&
               UseVfgOriginalTimer(Mode::ExclusiveFullscreen,60,0) &&
               VfgOriginalWaitMs(48)<=50 && VfgOriginalWaitMs(240)>=8,
              "VSync lets the next refresh pace 2x output and bounds the second-slot wait");
        Readiness admission;
        HANDLE signal=CreateEventW(nullptr,FALSE,TRUE,nullptr);
        Check(signal!=nullptr,"create synthetic readiness signal");
        admission.Reset(signal);
        Check(admission.Poll()==S_OK,"admit first presentation");
        admission.Presented();
        Check(admission.Wait(1)==S_FALSE && !admission.Ready(),
              "generated presentation cannot reuse its admission credit for the original");
        Check(SetEvent(signal)!=FALSE && admission.Wait(10)==S_OK && admission.Ready(),
              "next signal independently admits the original");
        admission.Presented();
        VfgSecondSlotRecovery recovery;
        recovery.ObserveRequest(true, 1);
        Check(admission.Wait(0)==S_FALSE,"simulate a missing second slot");
        recovery.TimedOut(60);
        Check(recovery.OriginalOnly() && admission.Enabled() && admission.WaitHandle()==signal,
              "VFG timeout does not disable original-frame readiness");
        recovery.ObserveRequest(true, 1);
        Check(recovery.OriginalOnly(),"same intent with no admitted frame cannot consume the retry delay");
        for (int i=0; i<60; ++i) {
            Check(recovery.OriginalOnly() && !admission.Ready(),
                  "recovery continues to wait for independent original credits");
            Check(SetEvent(signal)!=FALSE && admission.Poll()==S_OK,"source-only slot is admitted");
            admission.Presented();
            recovery.OriginalPresented();
        }
        Check(!recovery.OriginalOnly() &&
                  VfgPresentationBlock(Mode::VSync,60,120,admission.Enabled())==VfgBlock::None,
              "VFG can automatically retry after stable original playback");
        recovery.TimedOut(30);
        recovery.ObserveRequest(true, 3);
        Check(!recovery.OriginalOnly(),"rapid F7 off/on permits a fresh attempt");
        recovery.TimedOut(30);
        recovery.ObserveRequest(false, 4);
        Check(!recovery.OriginalOnly(),"disabling VFG clears the recovery delay");
        admission.Bypass();
        Check(admission.Wait(1)==S_OK && !admission.Enabled(),
              "fallback admission does not block ordinary VSync playback");
    }
    for (Mode mode : {Mode::AllowTearing, Mode::VSync, Mode::Compatibility,
                      Mode::ExclusiveFullscreen})
    for (bool hdr : {false, true})
    for (bool tearing : {false, true}) {
        const auto desc = llcv::presentation::Description(mode, 2560, 1440, hdr, tearing);
        const bool compatibility = mode == Mode::Compatibility;
        const bool exclusiveMode = mode == Mode::ExclusiveFullscreen;
        Check(desc.Width == 2560 && desc.Height == 1440 && desc.BufferCount == 2 &&
              desc.SampleDesc.Count == 1 && desc.AlphaMode == DXGI_ALPHA_MODE_IGNORE,
              "presentation modes retain geometry, two buffers, opaque non-MSAA output");
        Check(desc.Format == (hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM),
              "exclusive keeps existing SDR/HDR output precision");
        Check(desc.SwapEffect == (compatibility ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD),
              "exclusive uses the existing flip-discard output");
        Check(bool(desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ==
                  (!compatibility && !exclusiveMode),
              "D3D11 exclusive excludes unsupported waitable-object flag without changing existing modes");
        Check(bool(desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) == (!compatibility && tearing),
              "exclusive retains supported windowed tearing for suspension and fallback");
        Check(llcv::presentation::UsesVSync(mode) ==
                  (mode == Mode::VSync || mode == Mode::Compatibility),
              "exclusive presents without a VSync interval");
        for (bool actualExclusive : {false, true}) {
            const UINT expected = !actualExclusive && tearing &&
                (mode == Mode::AllowTearing || mode == Mode::ExclusiveFullscreen)
                    ? DXGI_PRESENT_ALLOW_TEARING : 0u;
            Check(llcv::presentation::PresentFlags(mode, actualExclusive, tearing) == expected,
                  "actual exclusive never submits the windowed-only tearing flag");
        }
    }
    Check(llcv::presentation::SupportsCapture(Mode::ExclusiveFullscreen,
              llcv::settings::VideoPixelFormat::P010) &&
          !llcv::presentation::SupportsCapture(Mode::Compatibility,
              llcv::settings::VideoPixelFormat::P010),
          "exclusive preserves P010 support and compatibility retains its restriction");
    Check(std::wcscmp(llcv::presentation::ModeName(Mode::ExclusiveFullscreen),
              L"Exclusive fullscreen") == 0 &&
          std::wcscmp(llcv::presentation::PathName(Mode::ExclusiveFullscreen),
              L"Flip-discard") == 0,
          "exclusive mode diagnostics describe actual presentation policy");
    using namespace llcv::exclusive_presentation;
    for (unsigned mask = 0; mask != 64; ++mask) {
        const Inputs state{
            (mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0,
            (mask & 8) != 0, (mask & 16) != 0, (mask & 32) != 0};
        Check(Wanted(state) == (mask == 7),
              "exclusive is wanted only for an active, unblocked fullscreen viewer");
    }

    Inputs state{true, true, true, false, false, false};
    AttemptGate gate;
    Check(gate.Begin(0, true, false), "initial zero revision is valid");
    Check(!gate.Begin(0, true, false), "zero revision failures remain latched");
    gate.Reset();
    std::uint64_t revision = 1;
    bool exclusive = false;
    Check(gate.Begin(revision, Wanted(state), exclusive), "initial fullscreen entry");
    // Simulate an unsupported entry: playback must continue without trying again
    // for every captured frame while the same UI request remains in force.
    for (unsigned frame = 0; frame != 1000; ++frame)
        Check(!gate.Begin(revision, Wanted(state), exclusive),
              "failed entry is not retried in the frame loop");

    state.active = false;
    Check(!gate.Begin(++revision, Wanted(state), exclusive),
          "Alt+Tab needs no leave call after a failed entry");
    state.active = true;
    Check(gate.Begin(++revision, Wanted(state), exclusive),
          "focus return permits a fresh entry attempt");
    exclusive = true;
    Check(!gate.Begin(revision, Wanted(state), exclusive), "successful entry is stable");

    state.active = false;
    Check(gate.Begin(++revision, Wanted(state), exclusive), "Alt+Tab leaves exclusive");
    exclusive = false;
    Check(!gate.Begin(revision, Wanted(state), exclusive), "suspended window is stable");
    state.active = true;
    Check(gate.Begin(++revision, Wanted(state), exclusive), "Alt+Tab return restores exclusive");
    exclusive = true;

    state.blocked = true;
    Check(gate.Begin(++revision, Wanted(state), exclusive), "settings suspend exclusive");
    exclusive = false;
    state.blocked = false;
    Check(gate.Begin(++revision, Wanted(state), exclusive), "settings return restores exclusive");
    exclusive = true;

    state.fullscreen = false;
    Check(gate.Begin(++revision, Wanted(state), exclusive), "fullscreen shortcut leaves exclusive");
    exclusive = false;
    Check(!gate.Begin(++revision, Wanted(state), exclusive),
          "focus changes cannot reenter after user selects windowed playback");
    state.fullscreen = true;
    Check(gate.Begin(++revision, Wanted(state), exclusive), "fullscreen shortcut allows reentry");
    exclusive = true;

    state.minimized = true;
    Check(gate.Begin(++revision, Wanted(state), exclusive), "minimize suspends exclusive");
    exclusive = false;
    state.minimized = false;
    Check(gate.Begin(++revision, Wanted(state), exclusive), "restore permits exclusive");
    exclusive = true;

    state.selected = false;
    Check(gate.Begin(++revision, Wanted(state), exclusive), "mode change leaves exclusive");
    exclusive = false;
    state.selected = true;
    Check(gate.Begin(++revision, Wanted(state), exclusive), "selecting mode permits reentry");
    exclusive = true;

    state.closing = true;
    Check(gate.Begin(++revision, Wanted(state), exclusive), "shutdown leaves exclusive");
    Check(!gate.Begin(revision, Wanted(state), exclusive),
          "failed leave is not retried on every frame during shutdown");
    exclusive = false;
    Check(!gate.Begin(++revision, Wanted(state), exclusive),
          "closing never reenters exclusive on a later focus revision");

    gate.Reset();
    Check(gate.Begin(revision, true, false), "new swapchain gets a fresh attempt");
    Check(!gate.Begin(revision, true, false), "new swapchain failure stays latched");
    gate.Reset();
    Check(gate.Begin(revision, true, false), "device recreation retries with unchanged UI revision");

    std::printf("Exclusive presentation state: %u checks passed.\n", checks);
    return 0;
}
