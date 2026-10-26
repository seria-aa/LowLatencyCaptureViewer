#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>

namespace llcv::recording {
// Playback and source screenshots support packed UYVY/HDYC, but recording
// currently accepts only NV12/YUY2/MJPEG. Keep this separate from malformed
// media types so the UI can explain the supported alternative before startup.
inline constexpr HRESULT UnsupportedUyvyRecording =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x401);
enum class Phase { Idle, Starting, Recording, Finalizing, Saved, Failed };
enum class Failure {
    None, DiskFull, WriteFailed, EncoderOverload, EncoderFailed,
    InvalidInput, StorageUnavailable, FinalizeFailed
};

// Published by the recording worker. UI reads must never query a disk or wait
// for an encoder; an unknown value is distinct from a measured zero.
struct Snapshot {
    Phase phase=Phase::Idle;
    Failure failure=Failure::None;
    HRESULT result=S_OK, finalizeResult=S_OK;
    bool busy=false, playable=false,paused=false;
    uint64_t elapsedMs=0;
    // Worker-side low-space safety check; not a recording-time estimate.
    uint64_t freeBytes=0;
    bool freeBytesKnown=false;
    std::wstring path;
};
}
