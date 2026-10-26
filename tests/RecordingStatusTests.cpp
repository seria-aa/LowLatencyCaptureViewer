#include "recording/RecordingStatusPresentation.h"
#include <cstdio>
#include <cstdlib>
#include <set>

using namespace llcv::recording;
static void Check(bool ok,const char* message) {
    if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::abort();}
}
int main() {
    Check(presentation::Elapsed(0)==L"00:00:00","initial elapsed time");
    Check(presentation::Elapsed(3661999)==L"01:01:01","elapsed formatted without rounding up");
    Check(presentation::Elapsed(100ULL*3600000)==L"100:00:00","long recording does not wrap daily");
    for(bool english:{false,true}) {
        std::set<std::wstring> phaseLabels;
        for(auto phase:{Phase::Idle,Phase::Starting,Phase::Recording,Phase::Finalizing,Phase::Saved,Phase::Failed}) {
            Snapshot snapshot;snapshot.phase=phase;snapshot.path=L"C:\\recordings\\recording.mp4";
            snapshot.busy=phase==Phase::Starting || phase==Phase::Recording || phase==Phase::Finalizing;
            snapshot.playable=phase==Phase::Saved;
            const auto view=presentation::Build(snapshot,english);
            Check(!view.title.empty(),"every state has a distinct notification title");
            phaseLabels.insert(view.title);
            Check(!view.canOpenVideo || (phase==Phase::Saved && !snapshot.busy),"open only after verified completion");
            Check(view.canOpenFolder==!snapshot.busy,"folder unavailable during write/finalize");
        }
        Check(phaseLabels.size()==6,"start recording finalize saved failure are distinct");
        Snapshot snapshot;snapshot.phase=Phase::Recording;snapshot.busy=true;
        snapshot.path=L"C:\\recordings\\recording.mp4";
        auto view=presentation::Build(snapshot,english);
        Check(view.description.empty(),"recording has no hidden details-only copy");
        snapshot.phase=Phase::Finalizing;snapshot.playable=true;
        view=presentation::Build(snapshot,english);
        Check(!view.canOpenVideo && !view.canOpenFolder,"stale playable flag cannot bypass finalization");
        snapshot.phase=Phase::Failed;snapshot.busy=false;snapshot.failure=Failure::DiskFull;
        view=presentation::Build(snapshot,english);
        Check(view.canOpenVideo && view.description.find(english?L"saved portion":L"중단 전까지")!=std::wstring::npos,"partial verified recording can open after failure");
        snapshot.phase=Phase::Saved;
        view=presentation::Build(snapshot,english);
        Check(view.canOpenVideo && view.title.find(english?L"stopped":L"중단")!=std::wstring::npos,
            "low disk graceful finalize is clearly stopped rather than normal success");
        snapshot.phase=Phase::Failed;
        snapshot.finalizeResult=E_FAIL;
        view=presentation::Build(snapshot,english);
        Check(view.canOpenVideo && view.description.find(english?L"Check the final section":L"마지막 구간")!=std::wstring::npos,"recoverable prefix not complete finalization promise");
        snapshot.playable=false;
        view=presentation::Build(snapshot,english);
        Check(!view.canOpenVideo && view.canOpenFolder && view.description.find(english?L"not verified":L"확인되지")!=std::wstring::npos,"unverified residue clearly reported without open video");
        snapshot.phase=Phase::Saved;
        Check(presentation::Build(snapshot,english).title.find(english?L"saved":L"저장 완료")==std::wstring::npos,"unverified file never called saved");
        std::set<std::wstring> failureLabels;
        for(auto failure:{Failure::DiskFull,Failure::WriteFailed,Failure::EncoderOverload,
            Failure::EncoderFailed,Failure::InvalidInput,Failure::StorageUnavailable,Failure::FinalizeFailed})
            failureLabels.insert(presentation::FailureText(failure,english));
        Check(failureLabels.size()==7,"failure classes stay distinct");
        snapshot={};snapshot.phase=Phase::Failed;snapshot.failure=Failure::InvalidInput;
        snapshot.result=UnsupportedUyvyRecording;snapshot.path=L"C:\\recordings\\not-created.mp4";
        view=presentation::Build(snapshot,english);
        Check(view.description.find(L"UYVY/HDYC")!=std::wstring::npos &&
            view.description.find(english?L"playback and screenshots":L"재생·스크린샷")!=std::wstring::npos &&
            view.description.find(english?L"not recording":L"녹화는 지원하지")!=std::wstring::npos,
            "UYVY/HDYC recording limitation is explicit and translated");
        Check(!view.canOpenVideo && !view.canOpenFolder &&
            view.description.find(english?L"remaining file":L"남은 파일")==std::wstring::npos,
            "rejected UYVY recording does not imply damaged file residue");
        snapshot.result=E_INVALIDARG;
        Check(presentation::Build(snapshot,english).description.find(L"UYVY/HDYC")==std::wstring::npos,
            "other invalid input retains the existing generic message");
    }
    std::puts("Recording status presentation tests passed");
}
