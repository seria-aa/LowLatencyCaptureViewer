// Real OS MJPEG decoder + production capture mailbox/recorder + GPU renderer.
// No capture device, visible window, Present call or settings file is used.
#include "../src/main.cpp"
#undef fwprintf
#include "../src/audio/AsioOutput.cpp"

#include <array>
#include <filesystem>
#include <stdexcept>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Check(HRESULT result, const char* message) {
    if (FAILED(result)) {
        std::printf("HRESULT %08lX: %s\n", static_cast<unsigned long>(result), message);
        throw std::runtime_error(message);
    }
}

struct Window {
    HWND handle = CreateWindowExW(0, L"STATIC", L"Hidden MJPEG recording isolation test",
        WS_POPUP, 0, 0, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~Window() { if (handle) DestroyWindow(handle); }
};

struct Event {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Event() { if (handle) CloseHandle(handle); }
};

struct RecordingSession {
    ~RecordingSession() { llcv::recording::recorder.Shutdown(); }
};

struct TestDirectory {
    std::filesystem::path path;
    TestDirectory() {
        GUID id{};
        Check(CoCreateGuid(&id), "unique test directory ID");
        wchar_t name[40]{};
        Require(StringFromGUID2(id, name, ARRAYSIZE(name)) != 0, "directory ID text");
        path = std::filesystem::temp_directory_path() /
            (std::wstring(L"LLCV-MjpegPlayback-") + name);
        Require(CreateDirectoryW(path.c_str(), nullptr) != 0, "new private test directory");
    }
    ~TestDirectory() {
        // Only this newly created private directory contains test recordings.
        // Delete its own regular files without traversing any subdirectory.
        std::error_code error;
        for (std::filesystem::directory_iterator i(path, error), end;
             !error && i != end; i.increment(error)) {
            if (i->is_regular_file(error)) DeleteFileW(i->path().c_str());
        }
        RemoveDirectoryW(path.c_str());
    }
};

std::vector<BYTE> JpegFrame(bool bright) {
    constexpr UINT width = 640, height = 360;
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)), "WIC factory");
    ComPtr<IStream> stream;
    Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "JPEG stream");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder), "JPEG encoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "JPEG encoder initialization");
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(encoder->CreateNewFrame(&frame, nullptr), "JPEG frame");
    Check(frame->Initialize(nullptr), "JPEG frame initialization");
    Check(frame->SetSize(width, height), "JPEG dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    Check(frame->SetPixelFormat(&format), "JPEG pixel format");
    Require(format == GUID_WICPixelFormat24bppBGR, "BGR JPEG fixture supported");
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 3, bright ? 224 : 32);
    Check(frame->WritePixels(height, width * 3, static_cast<UINT>(pixels.size()),
        pixels.data()), "JPEG pixels");
    Check(frame->Commit(), "JPEG frame commit");
    Check(encoder->Commit(), "JPEG commit");
    STATSTG stat{};
    Check(stream->Stat(&stat, STATFLAG_NONAME), "JPEG size");
    Check(stream->Seek({}, STREAM_SEEK_SET, nullptr), "JPEG rewind");
    std::vector<BYTE> result(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    Check(stream->Read(result.data(), static_cast<ULONG>(result.size()), &read), "JPEG bytes");
    Require(read == result.size(), "complete JPEG fixture");
    return result;
}

ComPtr<IMediaSample> Sample(IMemAllocator* allocator, const std::vector<BYTE>& bytes,
                            unsigned frame) {
    ComPtr<IMediaSample> sample;
    Check(allocator->GetBuffer(&sample, nullptr, nullptr, 0), "capture sample lease");
    Check(sample->SetActualDataLength(static_cast<long>(bytes.size())), "capture length");
    BYTE* destination = nullptr;
    Check(sample->GetPointer(&destination), "capture data");
    std::memcpy(destination, bytes.data(), bytes.size());
    LONGLONG start = static_cast<LONGLONG>(frame) * 10000000 / 30;
    LONGLONG end = static_cast<LONGLONG>(frame + 1) * 10000000 / 30;
    Check(sample->SetTime(&start, &end), "capture timestamp");
    return sample;
}

std::array<unsigned, 3> ReadPixel(DirectD3D11Renderer& renderer) {
    D3D11_TEXTURE2D_DESC desc{};
    renderer.backBuffer->GetDesc(&desc);
    Require(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM, "SDR BGRA render target");
    desc.Width = desc.Height = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy;
    Check(renderer.device->CreateTexture2D(&desc, nullptr, &copy), "GPU readback texture");
    renderer.context->OMSetRenderTargets(0, nullptr, nullptr);
    const D3D11_BOX center{320, 180, 0, 321, 181, 1};
    renderer.context->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0,
        renderer.backBuffer, 0, &center);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(renderer.context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped), "GPU frame readback");
    const auto pixel = *static_cast<const UINT32*>(mapped.pData);
    renderer.context->Unmap(copy.Get(), 0);
    return {(pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255};
}

int Run() {
    using namespace llcv;
    g_settings.pixelPerfect = false;
    g_settings.audioOnly = false;
    g_settings.scalingMode = ScalingMode::Smooth;
    g_settings.presentationMode = PresentationMode::VSync;
    g_fullscreen = false;
    g_fullscreenSourceSize = false;
    g_osdVisible = false;
    g_audioOsdVisible = false;
    g_volumeHudUntilMs = 0;
    g_vsrMode = vsr::Mode::Disabled;

    const std::array<std::vector<BYTE>, 2> jpeg{JpegFrame(false), JpegFrame(true)};
    video::MjpegDecoder viewerDecoder;
    const HRESULT decoderResult = viewerDecoder.initialize(640, 360, 30, nullptr, nullptr,
        video_color::MatrixOverride::Auto, nullptr);
    if (decoderResult == MF_E_TOPO_CODEC_NOT_FOUND) {
        std::puts("SKIP: system synchronous MJPEG decoder unavailable");
        return 77;
    }
    Check(decoderResult, "real viewer MJPEG decoder");
    Window window;
    Require(window.handle != nullptr, "hidden render window");
    DirectD3D11Renderer renderer;
    const HRESULT rendererResult = renderer.initialize(window.handle, 640, 360, 30,
        settings::VideoPixelFormat::Nv12, false, viewerDecoder.colorConfiguration());
    if (rendererResult == DXGI_ERROR_UNSUPPORTED ||
        rendererResult == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        std::puts("SKIP: GPU NV12 video processor unavailable");
        return 77;
    }
    Check(rendererResult, "production SDR renderer");
    Event ready;
    Require(ready.handle != nullptr, "capture notification event");
    capture::LatestVideoSample latest(0, ready.handle);
    video::VideoSampleFormat videoFormat{};
    videoFormat.valid = true;
    videoFormat.width = 640;
    videoFormat.height = 360;
    videoFormat.fps = 30;
    videoFormat.format = settings::VideoPixelFormat::Mjpeg;
    videoFormat.bytes = static_cast<DWORD>((std::max)(jpeg[0].size(), jpeg[1].size()));
    capture::VideoSampleGrabberCallback callback(&latest, nullptr, videoFormat);
    ComPtr<IMemAllocator> allocator;
    Check(CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&allocator)), "DirectShow sample allocator");
    ALLOCATOR_PROPERTIES requested{4, (std::max)(static_cast<LONG>(videoFormat.bytes), 6400L), 1, 0};
    ALLOCATOR_PROPERTIES actual{};
    Check(allocator->SetProperties(&requested, &actual), "sample allocation properties");
    Require(actual.cbBuffer >= requested.cbBuffer && actual.cBuffers >= requested.cBuffers,
        "allocator satisfies fixture storage");
    Check(allocator->Commit(), "sample allocator commit");

    unsigned index = 0, displayed = 0;
    auto display = [&](bool bright) {
        auto source = Sample(allocator.Get(), jpeg[bright ? 1 : 0], index++);
        const auto original = jpeg[bright ? 1 : 0];
        Check(callback.SampleCB(0, source.Get()), "production capture callback");
        int64_t arrival = 0;
        ComPtr<IMediaSample> selected;
        selected.Attach(latest.TakeLatest(arrival));
        Require(selected.Get() == source.Get() && arrival > 0, "viewer receives latest capture sample");
        ComPtr<IMFMediaBuffer> decoded;
        Check(viewerDecoder.decode(selected.Get(), &decoded), "viewer continues actual MJPEG decode");
        Require(decoded.Get() != nullptr, "decoded viewer frame");
        const auto decodedColor = viewerDecoder.colorConfiguration();
        if (!(decodedColor == renderer.sdrColor))
            Check(renderer.initialize(window.handle, 640, 360, 30,
                settings::VideoPixelFormat::Nv12, false, decodedColor),
                "viewer follows decoded JPEG color metadata");
        BYTE* pixels = nullptr;
        DWORD capacity = 0, length = 0;
        Check(decoded->Lock(&pixels, &capacity, &length), "decoded viewer bytes");
        const bool valid = pixels && viewerDecoder.validOutputBuffer(capacity, length);
        if (valid) renderer.upload(pixels, static_cast<UINT32>(viewerDecoder.stride()));
        Check(decoded->Unlock(), "decoded viewer unlock");
        Require(valid, "complete decoded NV12 frame");
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = renderer.inputViews[renderer.activeUploadSurface];
        Check(renderer.processVideo(stream), "production video processor render");
        const auto pixel = ReadPixel(renderer);
        BYTE* unchanged = nullptr;
        Check(source->GetPointer(&unchanged), "source bytes after decode and render");
        Require(source->GetActualDataLength() == static_cast<long>(original.size()) &&
            std::memcmp(unchanged, original.data(), original.size()) == 0,
            "recording and viewing never mutate capture-owned JPEG bytes");
        Require(SUCCEEDED(latest.FormatFailure()) && latest.RejectedSamples() == 0,
            "recording state does not invalidate capture mailbox");
        ++displayed;
        return pixel;
    };
    std::array<std::array<unsigned, 3>, 2> reference{};
    for (unsigned frame = 0; frame < 8; ++frame) reference[frame % 2] = display(frame % 2 != 0);
    Require(reference[1][0] > reference[0][0] + 100 &&
        reference[1][1] > reference[0][1] + 100 && reference[1][2] > reference[0][2] + 100,
        "GPU readback follows changing JPEG images while recording is off");
    auto verifyDisplay = [&](bool bright) {
        const auto pixel = display(bright);
        for (size_t channel = 0; channel < pixel.size(); ++channel)
            Require(std::abs(static_cast<int>(pixel[channel]) -
                static_cast<int>(reference[bright ? 1 : 0][channel])) <= 2,
                "recording state preserves rendered MJPEG pixels");
    };

    TestDirectory directory;
    RecordingSession recordingSession;
    recording::Options options;
    options.encoder = L"cpu";
    options.rateControl = recording::RateControl::Bitrate;
    options.videoMbps = 4;
    capture_audio::Format audio{};
    audio.channels = 2;
    audio.containerBits = audio.validBits = 16;
    audio.blockAlign = 4;
    audio.path = capture_audio::Path::Direct16BitStereo;
    recording::recorder.Configure(videoFormat, audio, viewerDecoder.colorConfiguration(), false, options);
    Require(recording::recorder.Start((directory.path / L"isolation.mp4").wstring()), "recording start");
    const ULONGLONG readyDeadline = GetTickCount64() + 10000;
    while (recording::recorder.Busy() && !recording::recorder.Recording() &&
           GetTickCount64() < readyDeadline) Sleep(5);
    if (!recording::recorder.Recording() &&
        recording::recorder.Result() == MF_E_TOPO_CODEC_NOT_FOUND) {
        std::puts("SKIP: software H.264 recording encoder unavailable");
        return 77;
    }
    Require(recording::recorder.Recording(), "real recording worker ready");
    const std::vector<BYTE> silence(6400, 0);
    for (unsigned frame = 0; frame < 8; ++frame) {
        auto pcm = Sample(allocator.Get(), silence, index);
        recording::recorder.Push(false, pcm.Get());
        verifyDisplay(frame % 2 != 0);
        Sleep(34);
        Require(recording::recorder.Recording(), "normal MJPEG recording stays active");
    }

    // Send a damaged packet through the real callback, then publish the next
    // normal frame before the viewer takes the mailbox. The recorder receives
    // the damaged packet; the latest-frame viewer correctly decodes the newer
    // image instead of pretending a damaged JPEG itself is viewable.
    const std::vector<BYTE> malformed(videoFormat.bytes, 0);
    auto bad = Sample(allocator.Get(), malformed, index++);
    Check(callback.SampleCB(0, bad.Get()), "malformed recording input delivery");
    verifyDisplay(true);
    BYTE* badBytes = nullptr;
    Check(bad->GetPointer(&badBytes), "malformed source after delivery");
    Require(std::memcmp(badBytes, malformed.data(), malformed.size()) == 0,
        "failed recording does not mutate malformed source either");
    bad.Reset();
    const ULONGLONG failureDeadline = GetTickCount64() + 10000;
    while (recording::recorder.Busy() && GetTickCount64() < failureDeadline) Sleep(5);
    Require(!recording::recorder.Busy() && FAILED(recording::recorder.Result()) &&
        recording::recorder.GetSnapshot().failure == recording::Failure::InvalidInput,
        "invalid MJPEG stops recording with an input failure");
    Require(!recording::recorder.Recording(), "failed recorder stops accepting capture input");
    for (unsigned frame = 0; frame < 12; ++frame) verifyDisplay(frame % 2 != 0);
    recording::recorder.Shutdown();
    for (unsigned frame = 0; frame < 8; ++frame) verifyDisplay(frame % 2 != 0);
    Check(allocator->Decommit(), "sample allocator decommit");
    std::printf("MJPEG isolation: %u changing frames decoded/rendered, recording off/on/failed/shutdown; source bytes unchanged.\n",
        displayed);
    return 0;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 1;
    const HRESULT media = MFStartup(MF_VERSION);
    if (FAILED(media)) { CoUninitialize(); return 1; }
    int result = 1;
    try { result = Run(); }
    catch (const std::exception& error) { std::printf("FAIL: %s\n", error.what()); }
    catch (...) { std::puts("FAIL: unexpected test exception"); }
    llcv::recording::recorder.Shutdown();
    MFShutdown();
    CoUninitialize();
    return result;
}
