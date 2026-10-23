#pragma once
#include "video/DirectShowVideoFormat.h"
#include "video/VideoColor.h"
#include "audio/CaptureAudioFormat.h"
#include "recording/RecordingOptions.h"
#include "recording/RecordingBufferPool.h"
#include "recording/RecordingStatus.h"
#include "recording/RecordingSafetyPolicy.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace llcv::recording {
// No driver samples are retained by the encoder.
class Recorder {
public:
    ~Recorder();
    void Configure(const video::VideoSampleFormat&, const capture_audio::Format&,
                   video_color::Configuration, bool arrivalClock = false,
                   const Options& options = {}, const AM_MEDIA_TYPE* captureType = nullptr,
                   video_color::MatrixOverride mjpegColorOverride = video_color::MatrixOverride::Auto,
                   video_color::RangeOverride sdrRangeOverride = video_color::RangeOverride::Auto);
    bool Start(const std::wstring& path, const Options* options=nullptr);
    void Stop() noexcept;
    bool SetPaused(bool paused);
    void Shutdown();
    void Push(bool video, IMediaSample*) noexcept;
    Snapshot GetSnapshot() const;
    bool Busy() const { return busy_.load(); }
    bool Recording() const { return phase_.load(std::memory_order_acquire)==Phase::Recording; }
    HRESULT Result() const { return result_.load(); }
private:
    friend struct RecorderTestAccess; // Regression-only access; no runtime hooks.
    struct Packet { Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer; LONGLONG time=0, duration=0; uint64_t pauseGeneration=0; bool video=false; };
    void Run(std::wstring path);
    void PrepareQueues(); // Worker only; shared by deterministic regression fixtures.
    void PushAudioChunks(const BYTE* data, long length, LONGLONG start, uint64_t pauseGeneration);
    void Fail(HRESULT, Operation operation=Operation::Encoder) noexcept;
    void UpdateStorage(IMFByteStream* stream);
    enum class WorkerActivity : unsigned { Starting, Waiting, Storage, Video, Audio, WriteVideo, WriteAudio, Finalizing };
    void TrackWorkerActivity(WorkerActivity activity) noexcept;
    void ReportQueueFailure(bool video, LONGLONG incomingTime, const wchar_t* reason) noexcept;
    std::atomic<uint64_t> workerActivity_{0}; // One packed stage/time snapshot, diagnostic only.
    std::mutex lifecycle_;
    mutable std::mutex statusMutex_;
    Snapshot snapshot_;
    // Auto-reset event retains a notification sent just before the worker waits.
    HANDLE wake_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    std::thread worker_;
    struct Ring {
        std::vector<Packet> packets;
        std::atomic<size_t> write{0}, read{0};
    };
    Ring videoQueue_, audioQueue_;
    RecordingBufferPool videoBuffers_,audioBuffers_;
    std::atomic<unsigned> producers_{0};
    std::atomic<uint64_t> session_{0};
    std::atomic<uint64_t> pauseGeneration_{0};
    uint64_t pausedSinceMs_=0,totalPausedMs_=0; // statusMutex_
    video::VideoSampleFormat video_{};
    GUID compressedFormatType_{};
    std::vector<BYTE> compressedFormat_;
    video_color::MatrixOverride mjpegColorOverride_=video_color::MatrixOverride::Auto;
    video_color::RangeOverride sdrRangeOverride_=video_color::RangeOverride::Auto;
    capture_audio::Format audio_{};
    video_color::Configuration color_{};
    bool arrivalClock_=false;
    Options options_{};
    FrameLimiter videoLimiter_{};
    LONGLONG audioAnchor_=-1;
    uint64_t audioFrames_=0;
    std::atomic<bool> busy_{false}, stop_{false};
    std::atomic<HRESULT> result_{S_OK};
    std::atomic<HRESULT> finalizeResult_{S_OK};
    std::atomic<Failure> failure_{Failure::None};
    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic<uint64_t> recordingStartedMs_{0},recordingStoppedMs_{0};
    std::atomic<uint64_t> videoCount_{0}, audioCount_{0};
};
extern Recorder recorder;
enum class ToggleResult { Starting, Stopping, Failed };
ToggleResult ToggleRecording();
void SetControlsClosing(bool closing);
bool ControlsClosing();
void OpenRecording(HWND owner,bool video,bool english);
// UI-thread-only child overlay; raw capture samples never include this HWND.
inline constexpr UINT kOpenRecordingSettings=WM_APP+95;
void AttachRecordingIndicator(HWND videoHost,HWND viewer,bool english,bool light);
void UpdateRecordingIndicator(HWND videoHost,const Snapshot& snapshot);
} // namespace llcv::recording
