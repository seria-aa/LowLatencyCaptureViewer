#include "AsioOutput.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unknwn.h>

#include "asiodrivers.h"
#ifndef interface
#define interface struct
#endif
#include "iasiodrv.h"

namespace llcv::asio {
namespace {

Output* g_activeOutput = nullptr;
std::mutex g_outputMutex;

long AsioMessage(long selector, long value, void*, double*) {
    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioResetRequest || value == kAsioResyncRequest ||
                       value == kAsioLatenciesChanged ||
                       value == kAsioEngineVersion ||
                       value == kAsioSupportsTimeInfo
                   ? 1L
                   : 0L;
    case kAsioEngineVersion:
        return 2L;
    case kAsioResetRequest:
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:
        return 1L;
    default:
        return 0L;
    }
}

void AsioSampleRateChanged(ASIOSampleRate) {}

void AsioBufferSwitch(long index, ASIOBool) {
    Output* output = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        output = g_activeOutput;
    }
    if (output) {
        // The implementation calls the instance through its private callback
        // entry point. The mutex is only held for pointer handoff; no audio
        // copy is performed while it is held.
        output->ProcessBuffer(index);
    }
}

}  // namespace

struct Output::Impl {
    AsioDrivers drivers;
    IASIO* driver = nullptr;
    int driverIndex = -1;
    HWND hostWindow = nullptr;
    RenderCallback callback = nullptr;
    void* user = nullptr;
    ASIOBufferInfo buffers[2]{};
    ASIOChannelInfo channels[2]{};
    ASIOCallbacks callbacks{};
    long bufferFrames = 0;
    double sampleRate = 0.0;
    ASIOSampleType sampleType = ASIOSTInt16LSB;
    bool buffersCreated = false;
    bool started = false;

    void Switch(long index) {
        if (!callback || !buffersCreated || index < 0 || index > 1) return;
        int16_t* scratch = scratchBuffer.data();
        const std::size_t frames = static_cast<std::size_t>(bufferFrames);
        if (scratchBuffer.size() < frames * 2) scratchBuffer.resize(frames * 2);
        const std::size_t got = callback(user, scratch, frames);
        if (got < frames) {
            std::memset(scratch + got * 2, 0,
                        (frames - got) * 2 * sizeof(int16_t));
        }

        if (sampleType == ASIOSTInt16LSB) {
            auto* left = static_cast<int16_t*>(buffers[0].buffers[index]);
            auto* right = static_cast<int16_t*>(buffers[1].buffers[index]);
            for (std::size_t i = 0; i < frames; ++i) {
                left[i] = scratch[i * 2];
                right[i] = scratch[i * 2 + 1];
            }
        } else if (sampleType == ASIOSTFloat32LSB) {
            auto* left = static_cast<float*>(buffers[0].buffers[index]);
            auto* right = static_cast<float*>(buffers[1].buffers[index]);
            constexpr float scale = 1.0f / 32768.0f;
            for (std::size_t i = 0; i < frames; ++i) {
                left[i] = std::clamp(static_cast<float>(scratch[i * 2]) * scale,
                                     -1.0f, 1.0f);
                right[i] = std::clamp(
                    static_cast<float>(scratch[i * 2 + 1]) * scale, -1.0f,
                    1.0f);
            }
        } else if (sampleType == ASIOSTInt32LSB ||
                   sampleType == ASIOSTInt32LSB16) {
            auto* left = static_cast<int32_t*>(buffers[0].buffers[index]);
            auto* right = static_cast<int32_t*>(buffers[1].buffers[index]);
            for (std::size_t i = 0; i < frames; ++i) {
                const int32_t valueLeft = static_cast<int32_t>(
                    static_cast<int64_t>(scratch[i * 2]) << 16);
                const int32_t valueRight = static_cast<int32_t>(
                    static_cast<int64_t>(scratch[i * 2 + 1]) << 16);
                left[i] = valueLeft;
                right[i] = valueRight;
            }
        } else {
            std::memset(buffers[0].buffers[index], 0,
                        static_cast<std::size_t>(bufferFrames) * 4);
            std::memset(buffers[1].buffers[index], 0,
                        static_cast<std::size_t>(bufferFrames) * 4);
        }
        if (driver) driver->outputReady();
    }

    std::vector<int16_t> scratchBuffer;
};

std::vector<DriverInfo> EnumerateDrivers() {
    std::vector<DriverInfo> result;
    AsioDrivers drivers;
    const long count = drivers.asioGetNumDev();
    for (long i = 0; i < count; ++i) {
        char name[128]{};
        if (drivers.asioGetDriverName(static_cast<int>(i), name,
                                      static_cast<int>(sizeof(name))) == 0) {
            result.push_back(DriverInfo{static_cast<int>(i), name});
        }
    }
    return result;
}

Output::Output(const std::string& driverName, HWND hostWindow,
               RenderCallback callback, void* user)
    : impl_(new Impl()) {
    impl_->hostWindow = hostWindow;
    impl_->callback = callback;
    impl_->user = user;
    const long count = impl_->drivers.asioGetNumDev();
    for (long i = 0; i < count; ++i) {
        char name[128]{};
        if (impl_->drivers.asioGetDriverName(static_cast<int>(i), name,
                                             static_cast<int>(sizeof(name))) ==
                0 &&
            driverName == name) {
            impl_->driverIndex = static_cast<int>(i);
            break;
        }
    }
    if (impl_->driverIndex < 0) error_ = "ASIO driver not found";
}

Output::~Output() {
    Stop();
    delete impl_;
    impl_ = nullptr;
}

void Output::ProcessBuffer(long index) {
    if (impl_) impl_->Switch(index);
}

bool Output::Start() {
    if (!impl_ || impl_->driverIndex < 0) return false;
    if (impl_->drivers.asioOpenDriver(
            impl_->driverIndex, reinterpret_cast<void**>(&impl_->driver)) !=
        0) {
        error_ = "ASIO driver could not be opened";
        return false;
    }
    if (!impl_->driver->init(impl_->hostWindow)) {
        error_ = "ASIO driver initialization failed";
        impl_->drivers.asioCloseDriver(impl_->driverIndex);
        impl_->driver = nullptr;
        return false;
    }

    long inputs = 0, outputs = 0;
    if (impl_->driver->getChannels(&inputs, &outputs) != ASE_OK ||
        outputs < 2) {
        error_ = "ASIO driver has fewer than two output channels";
        impl_->drivers.asioCloseDriver(impl_->driverIndex);
        impl_->driver = nullptr;
        return false;
    }
    // The capture path supplies 48 kHz PCM and this backend intentionally does
    // not resample on the ASIO callback thread. Refuse drivers that cannot run
    // at the capture rate rather than silently playing at the wrong speed.
    if (impl_->driver->canSampleRate(48000.0) != ASE_OK ||
        impl_->driver->setSampleRate(48000.0) != ASE_OK) {
        error_ = "ASIO driver does not support 48 kHz";
        impl_->drivers.asioCloseDriver(impl_->driverIndex);
        impl_->driver = nullptr;
        return false;
    }
    ASIOSampleRate rate = 0.0;
    if (impl_->driver->getSampleRate(&rate) != ASE_OK || rate <= 0.0) {
        error_ = "ASIO sample rate query failed";
        impl_->drivers.asioCloseDriver(impl_->driverIndex);
        impl_->driver = nullptr;
        return false;
    }
    if (std::abs(rate - 48000.0) > 0.5) {
        error_ = "ASIO driver did not accept 48 kHz";
        impl_->drivers.asioCloseDriver(impl_->driverIndex);
        impl_->driver = nullptr;
        return false;
    }
    impl_->sampleRate = rate;
    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    if (impl_->driver->getBufferSize(&minSize, &maxSize, &preferred,
                                     &granularity) != ASE_OK ||
        preferred <= 0) {
        error_ = "ASIO buffer size query failed";
        impl_->drivers.asioCloseDriver(impl_->driverIndex);
        impl_->driver = nullptr;
        return false;
    }
    impl_->bufferFrames = preferred;
    for (int i = 0; i < 2; ++i) {
        impl_->buffers[i].isInput = ASIOFalse;
        impl_->buffers[i].channelNum = i;
        impl_->buffers[i].buffers[0] = nullptr;
        impl_->buffers[i].buffers[1] = nullptr;
        impl_->channels[i].channel = i;
        impl_->channels[i].isInput = ASIOFalse;
    }
    impl_->callbacks.bufferSwitch = &AsioBufferSwitch;
    impl_->callbacks.sampleRateDidChange = &AsioSampleRateChanged;
    impl_->callbacks.asioMessage = &AsioMessage;
    impl_->callbacks.bufferSwitchTimeInfo = nullptr;
    if (impl_->driver->createBuffers(impl_->buffers, 2, preferred,
                                     &impl_->callbacks) != ASE_OK) {
        error_ = "ASIO buffer creation failed";
        impl_->drivers.asioCloseDriver(impl_->driverIndex);
        impl_->driver = nullptr;
        return false;
    }
    impl_->buffersCreated = true;
    for (int i = 0; i < 2; ++i) {
        if (impl_->driver->getChannelInfo(&impl_->channels[i]) != ASE_OK) {
            error_ = "ASIO channel query failed";
            Stop();
            return false;
        }
    }
    if (impl_->channels[0].type != ASIOSTInt16LSB &&
        impl_->channels[0].type != ASIOSTFloat32LSB &&
        impl_->channels[0].type != ASIOSTInt32LSB &&
        impl_->channels[0].type != ASIOSTInt32LSB16) {
        error_ = "ASIO output format is not supported (16-bit, 32-bit LSB, or float32 required)";
        Stop();
        return false;
    }
    if (impl_->channels[1].type != impl_->channels[0].type) {
        error_ = "ASIO stereo output formats differ";
        Stop();
        return false;
    }
    impl_->sampleType = impl_->channels[0].type;
    impl_->scratchBuffer.resize(static_cast<std::size_t>(preferred) * 2);
    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        g_activeOutput = this;
    }
    if (impl_->driver->start() != ASE_OK) {
        error_ = "ASIO start failed";
        Stop();
        return false;
    }
    impl_->started = true;
    running_ = true;
    bufferFrames_ = preferred;
    sampleRate_ = rate;
    return true;
}

void Output::Stop() {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(g_outputMutex);
        if (g_activeOutput == this) g_activeOutput = nullptr;
    }
    if (impl_->driver) {
        if (impl_->started) impl_->driver->stop();
        if (impl_->buffersCreated) impl_->driver->disposeBuffers();
        impl_->started = false;
        impl_->buffersCreated = false;
        impl_->drivers.asioCloseDriver(impl_->driverIndex);
        impl_->driver = nullptr;
    }
    running_ = false;
}

}  // namespace llcv::asio
