#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <windows.h>

namespace llcv::asio {

struct DriverInfo {
    int index = -1;
    std::string name;
};

std::vector<DriverInfo> EnumerateDrivers();

// Minimal ASIO output host used by the optional ASIO mode. The audio callback
// is called from the driver's realtime buffer-switch thread and must not
// allocate or wait on UI work.
class Output {
public:
    using RenderCallback = std::size_t (*)(void* user, int16_t* interleaved,
                                           std::size_t frames);

    Output(const std::string& driverName, HWND hostWindow,
           RenderCallback callback, void* user);
    ~Output();

    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    bool Start();
    void Stop();
    bool Running() const { return running_; }
    long BufferFrames() const { return bufferFrames_; }
    double SampleRate() const { return sampleRate_; }
    const std::string& Error() const { return error_; }

    // Called only by the ASIO callback trampoline.
    void ProcessBuffer(long index);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool running_ = false;
    long bufferFrames_ = 0;
    double sampleRate_ = 0.0;
    std::string error_;
};

}  // namespace llcv::asio
