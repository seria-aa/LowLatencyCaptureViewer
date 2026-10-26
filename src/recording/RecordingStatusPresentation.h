#pragma once
#include "recording/RecordingStatus.h"
#include <cstdio>

namespace llcv::recording::presentation {
inline std::wstring Elapsed(uint64_t milliseconds) {
    const auto seconds=milliseconds/1000;
    wchar_t value[64]{};
    swprintf_s(value,L"%02llu:%02llu:%02llu",
        seconds/3600,seconds/60%60,seconds%60);
    return value;
}
inline const wchar_t* FailureText(Failure failure,bool english) {
    switch(failure) {
    case Failure::DiskFull:return english?L"Recording stopped: not enough disk space.":L"저장 공간이 부족해 녹화를 중단했습니다.";
    case Failure::WriteFailed:return english?L"Recording stopped: the file could not be written.":L"파일 쓰기 실패로 녹화를 중단했습니다.";
    case Failure::EncoderOverload:return english?L"Encoding or writing could not keep up.":L"인코딩·파일 저장 처리가 밀려 녹화를 중단했습니다.";
    case Failure::EncoderFailed:return english?L"Recording stopped: the encoder reported an error.":L"인코더 오류로 녹화를 중단했습니다.";
    case Failure::InvalidInput:return english?L"This input format cannot be recorded.":L"녹화할 수 없는 입력 형식입니다.";
    case Failure::StorageUnavailable:return english?L"The recording folder is unavailable.":L"녹화 저장 폴더를 사용할 수 없습니다.";
    case Failure::FinalizeFailed:return english?L"The recording file could not be finalized.":L"녹화 파일 마무리에 실패했습니다.";
    default:return english?L"Recording could not be completed.":L"녹화를 완료하지 못했습니다.";
    }
}
struct View {
    std::wstring title,description;
    bool canOpenVideo=false,canOpenFolder=false;
};
inline View Build(const Snapshot& snapshot,bool english) {
    View result;
    switch(snapshot.phase) {
    case Phase::Starting:
        result.title=english?L"Preparing recording…":L"녹화 시작 중…";
        break;
    case Phase::Recording:
        result.title=snapshot.paused?(english?L"Ⅱ Recording paused":L"Ⅱ 녹화 일시정지"):(english?L"● Recording":L"● 녹화 중");
        break;
    case Phase::Finalizing:
        result.title=english?L"Finalizing recording…":L"마무리 중…";
        break;
    case Phase::Saved:
        result.title=english?L"Recording saved":L"저장 완료";
        break;
    case Phase::Failed:
        result.title=english?L"Recording stopped":L"녹화 중단";
        break;
    default:
        result.title=english?L"Ready to record":L"녹화 대기";
        break;
    }
    result.canOpenVideo=!snapshot.busy && snapshot.playable && !snapshot.path.empty();
    result.canOpenFolder=!snapshot.busy && !snapshot.path.empty();
    if(snapshot.phase==Phase::Failed && snapshot.failure==Failure::InvalidInput &&
       snapshot.result==UnsupportedUyvyRecording && !snapshot.busy && !snapshot.playable) {
        result.title=english?L"Recording unavailable":L"녹화할 수 없음";
        result.description=english?
            L"UYVY/HDYC supports playback and screenshots, but not recording.\r\nSelect NV12, YUY2 or MJPEG input to record.":
            L"UYVY/HDYC 입력은 재생·스크린샷을 지원하지만 녹화는 지원하지 않습니다.\r\n녹화하려면 NV12·YUY2·MJPEG 입력을 선택하세요.";
        // No file or recording worker was created; do not imply a damaged
        // remaining file, or offer an output path that was never opened.
        result.canOpenVideo=false;result.canOpenFolder=false;
        return result;
    }
    if(snapshot.failure!=Failure::None || snapshot.phase==Phase::Failed) {
        if(snapshot.phase==Phase::Saved && snapshot.playable && !snapshot.busy)
            result.title=english?L"Recording stopped · file saved":L"녹화 중단 · 파일 저장됨";
        result.description=FailureText(snapshot.failure,english);
        if(snapshot.busy)result.description+=english?L"\r\nFinishing the recoverable part. Playback continues.":L"\r\n남은 파일을 마무리하고 있습니다. 재생은 계속됩니다.";
        else if(snapshot.playable && FAILED(snapshot.finalizeResult))result.description+=english?
            L"\r\nPart of the video can be opened. Check the final section.":
            L"\r\n일부 영상은 열 수 있습니다. 마지막 구간은 확인이 필요합니다.";
        else if(snapshot.playable)result.description+=english?L"\r\nThe saved portion is ready to open. Playback continues.":L"\r\n중단 전까지의 파일은 저장됐습니다. 재생은 계속됩니다.";
        else result.description+=english?L"\r\nThe remaining file is not verified. Playback continues.":L"\r\n남은 파일의 정상 저장은 확인되지 않았습니다. 재생은 계속됩니다.";
    }
    // A stale/unverified terminal status must never offer a misleading success.
    if(snapshot.phase==Phase::Saved && (snapshot.busy || !snapshot.playable)) {
        result.title=english?L"Checking recording…":L"녹화 파일 확인 중…";
        result.description=english?L"Open will be available after the file is checked.":L"파일 확인을 마치면 영상을 열 수 있습니다.";
    }
    return result;
}
}
