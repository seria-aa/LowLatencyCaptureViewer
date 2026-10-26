#include "recording/Recorder.h"
#include "recording/RecordingStatusPresentation.h"
#include <mfapi.h>
#include <mferror.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace llcv::recording {
struct RecorderTestAccess {
    static bool HasWorker(const Recorder& subject) { return subject.worker_.joinable(); }
};
}

using llcv::recording::Failure;
using llcv::recording::Options;
using llcv::recording::Phase;
using llcv::recording::RateControl;
using llcv::recording::Recorder;
namespace presentation = llcv::recording::presentation;
using Microsoft::WRL::ComPtr;
namespace {
int failures = 0;
void Expect(bool passed, const char* message) {
    if (!passed) { ++failures; std::printf("FAIL: %s\n", message); }
}
void Check(HRESULT result) { if (FAILED(result)) throw result; }
template<class Predicate>
bool WaitFor(Predicate predicate, unsigned timeoutMs = 15000) {
    const auto begin = GetTickCount64();
    do {
        if (predicate()) return true;
        Sleep(10);
    } while (GetTickCount64() - begin < timeoutMs);
    return predicate();
}
void WaitIdle(Recorder& recorder) {
    if (!WaitFor([&] { return !recorder.Busy(); })) {
        Expect(false, "recording worker reaches a terminal state before deadline");
        throw HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
}
bool WaitRecording(Recorder& recorder) {
    const bool ready = WaitFor([&] { return recorder.Recording() || !recorder.Busy(); });
    Expect(ready && recorder.Recording(), "CPU recorder starts without hardware encoder dependencies");
    return ready && recorder.Recording();
}
struct TemporaryFiles {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        (L"LLCV-recording-state-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::vector<std::filesystem::path> files;
    TemporaryFiles() { std::filesystem::create_directory(directory); }
    std::filesystem::path Path(const wchar_t* name) {
        auto path = directory / name;
        files.push_back(path);
        return path;
    }
    ~TemporaryFiles() {
        // Only exact files created by this test, never a recursive directory removal.
        std::error_code error;
        for (const auto& file : files) std::filesystem::remove(file, error);
        std::filesystem::remove(directory, error);
    }
};
void Sentinel(const std::filesystem::path& path) {
    std::ofstream stream(path, std::ios::binary);
    stream << "LLCV-preserve-this-file";
    if (!stream) throw E_FAIL;
}
bool SentinelPreserved(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return contents == "LLCV-preserve-this-file";
}
ComPtr<IMediaSample> Sample(IMemAllocator* allocator, long bytes, LONGLONG time, LONGLONG duration) {
    ComPtr<IMediaSample> sample;
    Check(allocator->GetBuffer(&sample, nullptr, nullptr, 0));
    Check(sample->SetActualDataLength(bytes));
    LONGLONG end = time + duration;
    Check(sample->SetTime(&time, &end));
    return sample;
}
void Feed(Recorder& recorder, int frames, bool inspectLiveStatus, bool includeAudio = true,bool expectRunning=true) {
    constexpr long width = 640, height = 360, frameBytes = width * height * 3 / 2;
    ComPtr<IMemAllocator> allocator;
    Check(CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&allocator)));
    ALLOCATOR_PROPERTIES requested{ 2, frameBytes, 1, 0 }, actual{};
    Check(allocator->SetProperties(&requested, &actual));
    Check(allocator->Commit());
    const auto began = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frames && recorder.Recording(); ++frame) {
        std::this_thread::sleep_until(began + std::chrono::nanoseconds(frame * 1000000000LL / 30));
        const auto time = frame * 10000000LL / 30;
        auto video = Sample(allocator.Get(), frameBytes, time, 10000000 / 30);
        BYTE* pixels = nullptr;
        Check(video->GetPointer(&pixels));
        std::memset(pixels, 128, frameBytes);
        for (long y = 0; y < height; ++y) for (long x = 0; x < width; ++x)
            pixels[y * width + x] = static_cast<BYTE>(16 + ((x + frame * 3) % 220));
        recorder.Push(true, video.Get());
        if (includeAudio) {
            auto audio = Sample(allocator.Get(), 1600 * 4, time, 10000000 / 30);
            BYTE* bytes = nullptr;
            Check(audio->GetPointer(&bytes));
            auto* pcm = reinterpret_cast<short*>(bytes);
            for (int i = 0; i < 1600; ++i)
                pcm[i * 2] = pcm[i * 2 + 1] = static_cast<short>(6000 * std::sin(i * 6.283185307179586 / 48));
            recorder.Push(false, audio.Get());
        }
        if (inspectLiveStatus && frame == 45) {
            const auto snapshot = recorder.GetSnapshot();
            Expect(snapshot.phase == Phase::Recording && snapshot.busy,
                   "recording state stays distinct from preparing/finalizing");
            Expect(snapshot.elapsedMs >= 1000,
                   "live indicator reports elapsed recording time");
            Expect(!presentation::Build(snapshot, true).canOpenVideo,
                   "unfinalized output cannot be opened from the recording notification");
        }
    }
    if(expectRunning)Expect(recorder.Recording(), "paced CPU clip does not overflow bounded recording queues");
}
}

int main() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    const HRESULT mf = MFStartup(MF_VERSION);
    if (FAILED(mf)) { CoUninitialize(); return 1; }
    try {
        TemporaryFiles temporary;
        Recorder recorder;
        const auto idle = recorder.GetSnapshot();
        Expect(idle.phase == Phase::Idle && !idle.busy && !idle.playable,
               "new recorder is idle with no openable stale file");

        llcv::video::VideoSampleFormat video{};
        video.valid = true; video.width = 640; video.height = 360; video.fps = 30;
        video.stride = 640; video.format = llcv::settings::VideoPixelFormat::Nv12;
        llcv::capture_audio::Format audio{};
        audio.channels = 2; audio.containerBits = audio.validBits = 16; audio.blockAlign = 4;
        audio.path = llcv::capture_audio::Path::Direct16BitStereo;
        Options options; options.encoder = L"cpu"; options.rateControl = RateControl::Bitrate; options.videoMbps = 2;

        const auto invalidFile = temporary.Path(L"invalid-input.mp4");
        auto invalid = video; invalid.format = llcv::settings::VideoPixelFormat::P010;
        recorder.Configure(invalid, audio, {}, false, options);
        Expect(!recorder.Start(invalidFile.wstring()), "unsupported input fails before worker starts");
        auto state = recorder.GetSnapshot();
        Expect(state.phase == Phase::Failed && state.failure == Failure::InvalidInput && !state.busy && !state.playable,
               "invalid input has actionable terminal failure without an openable file");
        Expect(!std::filesystem::exists(invalidFile), "unsupported input does not create a misleading empty video");

        const auto uyvyFile=temporary.Path(L"unsupported-uyvy.mp4");
        auto uyvy=video;uyvy.format=llcv::settings::VideoPixelFormat::Uyvy;uyvy.stride=uyvy.width*2;
        recorder.Configure(uyvy,audio,{},false,options);
        Expect(!recorder.Start(uyvyFile.wstring()),"UYVY/HDYC recording rejected before startup");
        state=recorder.GetSnapshot();
        Expect(state.phase==Phase::Failed && state.failure==Failure::InvalidInput &&
            state.result==llcv::recording::UnsupportedUyvyRecording && !state.busy && !state.playable &&
            !llcv::recording::RecorderTestAccess::HasWorker(recorder),"UYVY rejection has dedicated reason and starts no worker");
        Expect(!std::filesystem::exists(uyvyFile),"UYVY rejection creates no recording file");
        for(bool english:{false,true}) {
            const auto view=presentation::Build(state,english);
            Expect(view.description.find(L"UYVY/HDYC")!=std::wstring::npos &&
                view.description.find(english?L"screenshots":L"스크린샷")!=std::wstring::npos &&
                view.description.find(english?L"not recording":L"녹화는 지원하지")!=std::wstring::npos &&
                !view.canOpenVideo && !view.canOpenFolder,"UYVY rejection clearly explains playback/screenshots only in both languages");
        }
        const auto uyvyExisting=temporary.Path(L"unsupported-uyvy-existing.mp4");Sentinel(uyvyExisting);
        Expect(!recorder.Start(uyvyExisting.wstring()) && SentinelPreserved(uyvyExisting),
            "UYVY rejection never touches an existing target");

        const auto occupiedFolder = temporary.Path(L"not-a-folder");
        Sentinel(occupiedFolder);
        recorder.Configure(video, audio, {}, false, options);
        Expect(recorder.Start((occupiedFolder / L"recording.mp4").wstring()), "invalid folder is checked by worker");
        WaitIdle(recorder);
        state = recorder.GetSnapshot();
        Expect(state.phase == Phase::Failed && state.failure == Failure::StorageUnavailable && !state.playable,
               "invalid folder is distinguished from encoder failure");
        Expect(SentinelPreserved(occupiedFolder), "invalid folder failure does not alter existing file");

        const auto existing = temporary.Path(L"existing.mp4");
        Sentinel(existing);
        Expect(recorder.Start(existing.wstring()), "existing target is checked asynchronously");
        WaitIdle(recorder);
        state = recorder.GetSnapshot();
        Expect(state.phase == Phase::Failed && !state.playable && FAILED(state.result),
               "existing target is not marked as a saved recording");
        Expect(SentinelPreserved(existing), "refusing an existing output preserves its exact bytes");

        const auto cancelled = temporary.Path(L"cancelled-start.mp4");
        Expect(recorder.Start(cancelled.wstring()), "recording can start after a failed attempt");
        recorder.Stop();
        WaitIdle(recorder);
        state = recorder.GetSnapshot();
        Expect(!state.busy && !recorder.Recording() && !state.playable,
               "stop during initialization terminates without enabling an empty file");

        const auto complete = temporary.Path(L"complete.mp4");
        Expect(recorder.Start(complete.wstring()), "recording can restart after startup cancellation");
        if (WaitRecording(recorder)) {
            const auto busyRejected = temporary.Path(L"busy-rejected.mp4");
            Expect(!recorder.Start(busyRejected.wstring()), "busy recorder refuses a second session");
            Expect(recorder.GetSnapshot().path == complete.wstring() && !std::filesystem::exists(busyRejected),
                   "rejected overlapping start does not replace active path or create an output");
            recorder.Configure(invalid, audio, {}, false, options);
            Feed(recorder, 60, true);
        }
        recorder.Stop();
        const auto stopped = recorder.GetSnapshot();
        Expect(!recorder.Recording() &&
               (stopped.phase == Phase::Finalizing || stopped.phase == Phase::Saved || stopped.phase == Phase::Failed),
               "stop ends sample acceptance before file completion");
        if (stopped.busy) Expect(!presentation::Build(stopped, false).canOpenVideo,
                                "finalizing file is not openable");
        // Repeated stop requests can race the worker's terminal publication.
        // They must never turn a completed Saved/Failed phase back to Finalizing.
        std::thread repeatedStop([&] {
            const auto began = GetTickCount64();
            while (recorder.Busy() && GetTickCount64() - began < 15000) {
                recorder.Stop(); std::this_thread::yield();
            }
            recorder.Stop();
        });
        const bool stopFinished = WaitFor([&] { return !recorder.Busy(); });
        repeatedStop.join();
        Expect(stopFinished, "repeated stop remains bounded");
        state = recorder.GetSnapshot();
        Expect(state.phase == Phase::Saved && SUCCEEDED(state.result) && SUCCEEDED(state.finalizeResult),
               "successful recording distinguishes saved from simply stopped");
        Expect(state.playable && presentation::Build(state, true).canOpenVideo,
               "only verified completed video enables the open action");
        Expect(std::filesystem::file_size(complete) > 0,
               "completed recording file is nonempty");
        Sleep(40);
        Expect(state.elapsedMs == stopped.elapsedMs && recorder.GetSnapshot().elapsedMs == state.elapsedMs,
               "elapsed recording time freezes at stop instead of including finalization/idle time");

        // A real invalid input callback exercises recoverable partial output,
        const auto pausedFile=temporary.Path(L"paused-stop.mp4");
        Expect(!recorder.SetPaused(true),"idle pause rejected");
        Expect(recorder.Start(pausedFile.wstring(),&options),"start stop-while-paused scenario");
        if(WaitRecording(recorder)){
            Feed(recorder,30,false);
            Expect(recorder.SetPaused(true)&&recorder.SetPaused(true),"pause is idempotent");
            auto frozen=recorder.GetSnapshot();
            Sleep(60);
            Expect(recorder.GetSnapshot().elapsedMs==frozen.elapsedMs,"paused elapsed time freezes");
            recorder.Stop();
            Expect(!recorder.SetPaused(false),"resume after stop rejected");
            WaitIdle(recorder);
            auto ended=recorder.GetSnapshot();
            Expect(ended.phase==Phase::Saved&&ended.playable&&!ended.paused,"stop paused saves existing media");
            Expect(ended.elapsedMs==frozen.elapsedMs,"stop paused excludes paused wall time");
        }

        // A real invalid input callback exercises recoverable partial output,
        // without adding production-only fault injection or depending on disk size.
        const auto partial = temporary.Path(L"partial-input-error.mp4");
#ifdef LLCV_RECORDING_MODERN_MP4
        const auto splitBase=temporary.Path(L"split-collision.mp4");
        const auto splitFirst=temporary.Path(L"split-collision-001.mp4");
        const auto splitOccupied=temporary.Path(L"split-collision-002.mp4");
        Sentinel(splitOccupied);
        auto splitOptions=options;splitOptions.splitMode=llcv::recording::SplitMode::Time;splitOptions.splitSeconds=1;
        Expect(recorder.Start(splitBase.wstring(),&splitOptions),"start split collision scenario");
        if(WaitRecording(recorder))Feed(recorder,120,false,true,false);
        recorder.Stop();WaitIdle(recorder);state=recorder.GetSnapshot();
        Expect(FAILED(state.result)&&state.playable&&state.path==splitFirst.wstring(),"split creation failure preserves playable current file");
        Expect(SentinelPreserved(splitOccupied),"split never overwrites next part");
#endif
        auto invalidOptions=options;invalidOptions.maxFps=41;
        Expect(!recorder.Start(partial.wstring(),&invalidOptions),"live start validates freshly supplied options");
        Expect(recorder.Start(partial.wstring(),&options), "next start accepts current tab options without reconfiguring capture");
        state = recorder.GetSnapshot();
        Expect(!state.playable && state.path == partial.wstring() && !presentation::Build(state, true).canOpenVideo,
               "new session clears the previous saved file's openability");
        if (WaitRecording(recorder)) {
            Feed(recorder, 36, false);
            recorder.Push(true, nullptr);
        }
        WaitIdle(recorder);
        state = recorder.GetSnapshot();
        Expect(state.phase == Phase::Failed && state.failure == Failure::InvalidInput && FAILED(state.result),
               "input callback failure remains distinct from file finalization result");
        Expect(SUCCEEDED(state.finalizeResult) && state.playable && presentation::Build(state, false).canOpenVideo,
               "usable partial recording remains openable after an input failure");
        recorder.Push(true, nullptr); recorder.Push(false, nullptr);
        Expect(recorder.GetSnapshot().result == state.result, "callbacks after stop are cheap no-ops preserving terminal result");

        const auto noAudio = temporary.Path(L"missing-audio.mp4");
        Expect(recorder.Start(noAudio.wstring()), "start incomplete-stream failure scenario");
        if (WaitRecording(recorder)) {
            Feed(recorder, 12, false, false);
            recorder.Push(false, nullptr);
        }
        WaitIdle(recorder);
        state = recorder.GetSnapshot();
        Expect(state.result == E_INVALIDARG && state.failure == Failure::InvalidInput,
               "first input failure survives a later missing-stream finalization failure");
        Expect(FAILED(state.finalizeResult) && !state.playable &&
               !presentation::Build(state, true).canOpenVideo,
               "an incomplete recording is never offered as a normally saved video");

        recorder.Configure(invalid, audio, {}, false, options);
        Expect(!recorder.Start(invalidFile.wstring()), "invalid request after saved partial is rejected");
        state = recorder.GetSnapshot();
        Expect(!state.playable && state.path == invalidFile.wstring() && !presentation::Build(state, true).canOpenVideo,
               "failed new request never opens a previous session's saved file");
        recorder.Shutdown();
    } catch (HRESULT result) {
        std::printf("Unexpected HRESULT: 0x%08lX\n", static_cast<unsigned long>(result)); ++failures;
    } catch (const std::exception& error) {
        std::printf("Unexpected exception: %s\n", error.what()); ++failures;
    }
    MFShutdown(); CoUninitialize();
    std::printf("Recording state integration: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
