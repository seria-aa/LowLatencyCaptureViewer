// Exercise the actual decoder selection/copy path with process-local fake COM
// objects. No capture device, registered codec or system registry is changed.
#include "video/MjpegDecoder.h"
#include <mferror.h>
#include <cstdlib>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>
static HRESULT WINAPI TestMFTEnum(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
    const MFT_REGISTER_TYPE_INFO*, IMFActivate***, UINT32*);
#define MFTEnumEx TestMFTEnum
#include "../src/video/MjpegDecoder.cpp"
#undef MFTEnumEx

static void Check(bool ok, const char* text) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", text); std::abort(); }
}
struct Layout {
    UINT32 width = 4, height = 2, stride = 8;
    DWORD bytes = 24, flags = 0;
    bool hasStride = true, hasSize = true, nv12 = true;
    DWORD alignment = 64;
    UINT32 matrix = 0, range = 0;
};
struct Candidate {
    HRESULT activate = S_OK, input = S_OK, info = S_OK;
    unsigned activated = 0, created = 0, destroyed = 0, inputs = 0;
    HRESULT attributes = S_OK, changedInfo = S_OK;
    bool nullAttributes = false, change = false, changed = false;
    bool emitBefore = false, emitAfter = true;
    unsigned attributeCalls = 0, infoCalls = 0, outputStep = 0, framesProduced = 0;
    UINT32 lowLatency = 0;
    HRESULT timeResult=E_FAIL,durationResult=E_FAIL;
    LONGLONG time=0,duration=0;
    Layout before{}, after{4, 2, 16, 48};
};
static std::vector<Candidate> candidates;
static unsigned liveActivations = 0;
static bool useSystemDecoder = false;
#define STUB(name, ...) STDMETHODIMP name(__VA_ARGS__) override { return E_NOTIMPL; }

class Transform final : public IMFTransform {
public:
    explicit Transform(Candidate& candidate) : c(candidate) {
        ++c.created;
        Check(SUCCEEDED(MFCreateAttributes(&attributes, 1)), "fake attributes created");
    }
    ~Transform() { attributes->Release(); }
    STDMETHODIMP QueryInterface(REFIID iid, void** p) override {
        *p = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IMFTransform)) return E_NOINTERFACE;
        *p = static_cast<IMFTransform*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto n = --refs; if (!n) { ++c.destroyed; delete this; } return n;
    }
    STUB(GetStreamLimits, DWORD*, DWORD*, DWORD*, DWORD*)
    STUB(GetStreamCount, DWORD*, DWORD*)
    STUB(GetStreamIDs, DWORD, DWORD*, DWORD, DWORD*)
    STDMETHODIMP GetInputStreamInfo(DWORD, MFT_INPUT_STREAM_INFO* info) override {
        *info = {}; info->cbSize = 64; info->cbAlignment = c.changed ? 128 : 32;
        return S_OK;
    }
    STDMETHODIMP GetOutputStreamInfo(DWORD, MFT_OUTPUT_STREAM_INFO* info) override {
        ++c.infoCalls;
        const auto& layout = c.changed ? c.after : c.before;
        *info = {}; info->cbSize = layout.bytes; info->dwFlags = layout.flags;
        info->cbAlignment = layout.alignment;
        return c.changed ? c.changedInfo : c.info;
    }
    STDMETHODIMP GetAttributes(IMFAttributes** p) override {
        ++c.attributeCalls; *p = nullptr;
        if (FAILED(c.attributes)) return c.attributes;
        if (!c.nullAttributes) { *p = attributes; attributes->AddRef(); }
        return S_OK;
    }
    STUB(GetInputStreamAttributes, DWORD, IMFAttributes**)
    STUB(GetOutputStreamAttributes, DWORD, IMFAttributes**)
    STUB(DeleteInputStream, DWORD)
    STUB(AddInputStreams, DWORD, DWORD*)
    STUB(GetInputAvailableType, DWORD, DWORD, IMFMediaType**)
    STDMETHODIMP GetOutputAvailableType(DWORD, DWORD index, IMFMediaType** p) override {
        *p = nullptr;
        if (index) return MF_E_NO_MORE_TYPES;
        HRESULT hr = MFCreateMediaType(p);
        if (SUCCEEDED(hr)) {
            (*p)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            const auto& layout = c.changed ? c.after : c.before;
            (*p)->SetGUID(MF_MT_SUBTYPE, layout.nv12 ? MFVideoFormat_NV12 : MFVideoFormat_YUY2);
            if (layout.hasSize) MFSetAttributeSize(*p, MF_MT_FRAME_SIZE, layout.width, layout.height);
            if (layout.hasStride) (*p)->SetUINT32(MF_MT_DEFAULT_STRIDE, layout.stride);
            if (layout.matrix) (*p)->SetUINT32(MF_MT_YUV_MATRIX, layout.matrix);
            if (layout.range) (*p)->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, layout.range);
        }
        return hr;
    }
    STDMETHODIMP SetInputType(DWORD, IMFMediaType*, DWORD) override {
        attributes->GetUINT32(MF_LOW_LATENCY, &c.lowLatency); return c.input;
    }
    STDMETHODIMP SetOutputType(DWORD, IMFMediaType*, DWORD) override { return S_OK; }
    STUB(GetInputCurrentType, DWORD, IMFMediaType**)
    STDMETHODIMP GetOutputCurrentType(DWORD, IMFMediaType** p) override {
        return GetOutputAvailableType(0, 0, p);
    }
    STUB(GetInputStatus, DWORD, DWORD*)
    STUB(GetOutputStatus, DWORD*)
    STUB(SetOutputBounds, LONGLONG, LONGLONG)
    STUB(ProcessEvent, DWORD, IMFMediaEvent*)
    STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE, ULONG_PTR) override { return S_OK; }
    STDMETHODIMP ProcessInput(DWORD, IMFSample* sample, DWORD) override {
        c.timeResult=sample->GetSampleTime(&c.time);
        c.durationResult=sample->GetSampleDuration(&c.duration);
        IMFMediaBuffer* buffer = nullptr;
        Check(SUCCEEDED(sample->GetBufferByIndex(0, &buffer)), "input buffer available");
        BYTE* data = nullptr; DWORD capacity = 0;
        Check(SUCCEEDED(buffer->Lock(&data, &capacity, nullptr)), "input buffer locked");
        Check(capacity >= 64 && reinterpret_cast<ULONG_PTR>(data) % (c.changed ? 128 : 32) == 0,
              "input minimum capacity and alignment requirements honored");
        buffer->Unlock(); buffer->Release();
        ++c.inputs; return S_OK;
    }
    STDMETHODIMP ProcessOutput(DWORD, DWORD, MFT_OUTPUT_DATA_BUFFER* output, DWORD*) override {
        if (!c.change) return MF_E_TRANSFORM_NEED_MORE_INPUT;
        if (c.outputStep == 0) {
            ++c.outputStep;
            if (c.emitBefore) return Produce(output);
        }
        if (c.outputStep == 1) {
            ++c.outputStep; c.changed = true; return MF_E_TRANSFORM_STREAM_CHANGE;
        }
        if (c.outputStep == 2) {
            ++c.outputStep;
            if (c.emitAfter) return Produce(output);
        }
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }
private:
    HRESULT Produce(MFT_OUTPUT_DATA_BUFFER* output) {
        const auto& layout = c.changed ? c.after : c.before;
        const DWORD bytes = (layout.hasStride ? layout.stride : layout.width) *
            (layout.height + layout.height / 2);
        const DWORD capacity = (std::max)(layout.bytes, bytes);
        if (layout.flags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) {
            Check(output->pSample == nullptr, "sample ownership refreshed after stream change");
            Check(SUCCEEDED(MFCreateSample(&output->pSample)), "provided sample created");
            IMFMediaBuffer* buffer = nullptr;
            Check(SUCCEEDED(MFCreateMemoryBuffer(capacity, &buffer)), "provided buffer created");
            Check(SUCCEEDED(output->pSample->AddBuffer(buffer)), "provided buffer attached");
            buffer->Release();
        }
        Check(output->pSample != nullptr, "caller-supplied sample required by output flags");
        IMFMediaBuffer* buffer = nullptr;
        Check(SUCCEEDED(output->pSample->GetBufferByIndex(0, &buffer)), "output has buffer");
        DWORD available = 0; buffer->GetMaxLength(&available);
        Check(available >= capacity, "output allocation refreshed including padding and cbSize");
        BYTE* data = nullptr;
        Check(SUCCEEDED(buffer->Lock(&data, nullptr, nullptr)), "output locked");
        if (!(layout.flags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES))
            Check(!layout.alignment || reinterpret_cast<ULONG_PTR>(data) % layout.alignment == 0,
                  "caller-supplied output honors required memory alignment");
        std::memset(data, c.changed ? 22 : 11, bytes);
        buffer->Unlock(); buffer->SetCurrentLength(bytes); buffer->Release();
        ++c.framesProduced;
        return S_OK;
    }
    Candidate& c; ULONG refs = 1;
    IMFAttributes* attributes = nullptr;
};
class Activation final : public IMFActivate {
public:
    explicit Activation(Candidate& candidate) : c(candidate) { ++liveActivations; }
    STDMETHODIMP QueryInterface(REFIID iid, void** p) override {
        *p = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IMFActivate)) return E_NOINTERFACE;
        *p = static_cast<IMFActivate*>(this); AddRef(); return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto n = --refs; if (!n) { --liveActivations; delete this; } return n;
    }
    STDMETHODIMP ActivateObject(REFIID iid, void** p) override {
        *p = nullptr; ++c.activated;
        Check(iid == __uuidof(IMFTransform), "decoder requests an IMFTransform");
        if (FAILED(c.activate)) return c.activate;
        *p = static_cast<IMFTransform*>(new Transform(c)); return S_OK;
    }
    STUB(ShutdownObject)
    STUB(DetachObject)
    STUB(GetItem, REFGUID, PROPVARIANT*)
    STUB(GetItemType, REFGUID, MF_ATTRIBUTE_TYPE*)
    STUB(CompareItem, REFGUID, REFPROPVARIANT, BOOL*)
    STUB(Compare, IMFAttributes*, MF_ATTRIBUTES_MATCH_TYPE, BOOL*)
    STUB(GetUINT32, REFGUID, UINT32*)
    STUB(GetUINT64, REFGUID, UINT64*)
    STUB(GetDouble, REFGUID, double*)
    STUB(GetGUID, REFGUID, GUID*)
    STUB(GetStringLength, REFGUID, UINT32*)
    STUB(GetString, REFGUID, LPWSTR, UINT32, UINT32*)
    STUB(GetAllocatedString, REFGUID, LPWSTR*, UINT32*)
    STUB(GetBlobSize, REFGUID, UINT32*)
    STUB(GetBlob, REFGUID, UINT8*, UINT32, UINT32*)
    STUB(GetAllocatedBlob, REFGUID, UINT8**, UINT32*)
    STUB(GetUnknown, REFGUID, REFIID, LPVOID*)
    STUB(SetItem, REFGUID, REFPROPVARIANT)
    STUB(DeleteItem, REFGUID)
    STUB(DeleteAllItems)
    STUB(SetUINT32, REFGUID, UINT32)
    STUB(SetUINT64, REFGUID, UINT64)
    STUB(SetDouble, REFGUID, double)
    STUB(SetGUID, REFGUID, REFGUID)
    STUB(SetString, REFGUID, LPCWSTR)
    STUB(SetBlob, REFGUID, const UINT8*, UINT32)
    STUB(SetUnknown, REFGUID, IUnknown*)
    STUB(LockStore)
    STUB(UnlockStore)
    STUB(GetCount, UINT32*)
    STUB(GetItemByIndex, UINT32, GUID*, PROPVARIANT*)
    STUB(CopyAllItems, IMFAttributes*)
private:
    Candidate& c; ULONG refs = 1;
};
static HRESULT WINAPI TestMFTEnum(GUID category, UINT32 flags, const MFT_REGISTER_TYPE_INFO* input,
    const MFT_REGISTER_TYPE_INFO* output, IMFActivate*** objects, UINT32* count) {
    if (useSystemDecoder) return MFTEnumEx(category, flags, input, output, objects, count);
    *count = static_cast<UINT32>(candidates.size());
    *objects = static_cast<IMFActivate**>(CoTaskMemAlloc(sizeof(IMFActivate*) * *count));
    if (!*objects) return E_OUTOFMEMORY;
    for (UINT32 i = 0; i < *count; ++i) (*objects)[i] = new Activation(candidates[i]);
    return S_OK;
}
class Sample final : public IMediaSample {
public:
    long bytes = 4, capacity = 4;
    bool timed=false;
    REFERENCE_TIME start=0,stop=0;
    unsigned pointerCalls = 0;
    std::vector<BYTE> data = std::vector<BYTE>(4);
    STDMETHODIMP QueryInterface(REFIID, void** p) override { *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP GetPointer(BYTE** p) override { ++pointerCalls; *p = data.data(); return S_OK; }
    STDMETHODIMP_(long) GetSize() override { return capacity; }
    STDMETHODIMP GetTime(REFERENCE_TIME* from,REFERENCE_TIME* to) override {
        if(!timed)return E_NOTIMPL;*from=start;*to=stop;return S_OK;
    }
    STUB(SetTime, REFERENCE_TIME*, REFERENCE_TIME*)
    STUB(IsSyncPoint)
    STUB(SetSyncPoint, BOOL)
    STUB(IsPreroll)
    STUB(SetPreroll, BOOL)
    STDMETHODIMP_(long) GetActualDataLength() override { return bytes; }
    STUB(SetActualDataLength, long)
    STUB(GetMediaType, AM_MEDIA_TYPE**)
    STUB(SetMediaType, AM_MEDIA_TYPE*)
    STUB(IsDiscontinuity)
    STUB(SetDiscontinuity, BOOL)
    STUB(GetMediaTime, LONGLONG*, LONGLONG*)
    STUB(SetMediaTime, LONGLONG*, LONGLONG*)
};
#undef STUB

static void TestInputTiming() {
    candidates.assign(1,{});
    llcv::video::MjpegDecoder decoder;
    Check(SUCCEEDED(decoder.initialize(4,2,60,nullptr,nullptr,
        llcv::video_color::MatrixOverride::Auto,nullptr)),"timing decoder initialization");
    Sample sample;Microsoft::WRL::ComPtr<IMFMediaBuffer> output;
    Check(SUCCEEDED(decoder.decode(&sample,&output)) && FAILED(candidates[0].timeResult),
        "untimed playback input remains untimed");
    sample.timed=true;sample.start=-166667;sample.stop=0;
    Check(SUCCEEDED(decoder.decode(&sample,&output)) && SUCCEEDED(candidates[0].timeResult) &&
        candidates[0].time==sample.start && candidates[0].duration==166667,
        "negative playback timestamps are preserved");
    Check(SUCCEEDED(decoder.decode(sample.data.data(),sample.bytes,10000000,166667,&output)) &&
        candidates[0].time==10000000 && candidates[0].duration==166667,
        "recording input timestamps are preserved independently of decode completion");
}
static void TestRecordingFramePolicy() {
    for(int scenario=0;scenario<3;++scenario){
        candidates.assign(1,{});
        auto& c=candidates.front();c.change=scenario!=0;c.emitBefore=scenario==2;
        llcv::video::MjpegDecoder decoder;
        Check(SUCCEEDED(decoder.initialize(4,2,60,nullptr,nullptr,
            llcv::video_color::MatrixOverride::Auto,nullptr)),"recording decoder initialization");
        Sample sample;Microsoft::WRL::ComPtr<IMFMediaBuffer> output;
        const auto hr=decoder.decode(sample.data.data(),static_cast<DWORD>(sample.data.size()),
            10000000,166667,&output,true);
        Check(scenario==1?SUCCEEDED(hr)&&output:hr==MF_E_INVALID_STREAM_DATA&&!output,
            "recording requires exactly one decoded frame instead of silently keeping the newest");
    }
}
static void TestColorSelection() {
    using namespace llcv;
    using video_color::MatrixOverride;
    using video_color::RangeOverride;
    for (auto matrix : {MatrixOverride::Auto, MatrixOverride::Bt709, MatrixOverride::Bt601})
        for (auto range : {RangeOverride::Auto, RangeOverride::Limited, RangeOverride::Full}) {
            candidates.assign(1, {});
            auto& c = candidates.front();
            c.change = true; c.emitBefore = true;
            c.before.matrix = 1; c.before.range = 1;
            c.after.matrix = 2; c.after.range = 2;
            video::CaptureColorMetadata directShow{};
            directShow.present = true; directShow.transferMatrix = 2; directShow.nominalRange = 2;
            video::MjpegDecoder decoder;
            Check(decoder.initialize(4, 2, 60, nullptr, &directShow, matrix, nullptr, range) == S_OK,
                  "independent MJPEG color selectors initialize");
            Check(decoder.colorConfiguration() == video_color::Resolve(true, 4, 2, {1, 1}, {2, 2}, matrix, range),
                  "decoder applies independent matrix/range over MF output metadata");
            Sample sample;
            Microsoft::WRL::ComPtr<IMFMediaBuffer> output;
            Check(SUCCEEDED(decoder.decode(&sample, &output)) && output,
                  "color test stream change returns newest frame");
            Check(decoder.colorConfiguration() == video_color::Resolve(true, 4, 2, {2, 2}, {2, 2}, matrix, range),
                  "manual selectors survive decoder stream changes; Auto follows updated metadata");
            decoder.reset();
            Check(decoder.colorConfiguration() == video_color::Configuration{},
                  "decoder reset clears selected color and its sources");
            c = {};
            Check(decoder.initialize(4, 2, 60, nullptr, nullptr, MatrixOverride::Auto, nullptr) == S_OK,
                  "decoder can reopen in Auto without an explicit range argument");
            Check(decoder.colorConfiguration() == video_color::Resolve(true, 4, 2, {}, {}),
                  "previous manual range cannot leak into reopened Auto decoder");
        }
}
static void TestStreamChanges() {
    for (unsigned scenario = 0; scenario < 15; ++scenario) {
        candidates.assign(1, {});
        auto& c = candidates[0];
        c.change = true; c.emitBefore = true;
        if (scenario == 1) c.after.bytes = 128;
        if (scenario == 2) c.after.bytes = 1; // stride, not cbSize, determines the safe minimum
        if (scenario == 3) { c.after.flags = MFT_OUTPUT_STREAM_PROVIDES_SAMPLES; c.after.bytes = 0; }
        if (scenario == 4) c.before.flags = MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
        if (scenario == 5) c.emitAfter = false;
        if (scenario == 6) c.after.hasStride = false;
        if (scenario == 7) c.after.width = 8;
        if (scenario == 8) c.after.height = 4;
        if (scenario == 9) c.after.stride = static_cast<UINT32>(-8);
        if (scenario == 10) c.after.stride = 2;
        if (scenario == 11) c.after.stride = 0;
        if (scenario == 12) c.after.stride = 7;
        if (scenario == 13) c.after.stride = LONG_MAX - 1u;
        if (scenario == 14) c.changedInfo = E_FAIL;
        llcv::video::MjpegDecoder decoder;
        Check(decoder.initialize(4, 2, 60, nullptr, nullptr,
            llcv::video_color::MatrixOverride::Auto, nullptr) == S_OK, "initial layout accepted");
        Check(c.attributeCalls == 1 && c.lowLatency == TRUE,
              "GetAttributes delivers low-latency request before input type setup");
        Sample sample; IMFMediaBuffer* output = nullptr;
        const HRESULT hr = decoder.decode(&sample, &output);
        if (scenario >= 7) {
            Check(FAILED(hr) && output == nullptr && decoder.stride() == 8,
                  "invalid changed layout rejected without committing partial state or stale frame");
        } else {
            Check(hr == S_OK && c.infoCalls == 2, "stream info refreshed after output change");
            Check(decoder.stride() == (scenario == 6 ? 4 : 16), "changed stride applied");
            if (scenario == 5) Check(output == nullptr, "old pending frame dropped even with no new frame");
            else {
                Check(output != nullptr, "new format frame returned");
                BYTE* data = nullptr; DWORD capacity = 0, length = 0;
                Check(SUCCEEDED(output->Lock(&data, &capacity, &length)), "result locked");
                Check(data[0] == 22 && decoder.validOutputBuffer(capacity, length),
                      "only new frame returned with valid layout");
                output->Unlock(); output->Release();
            }
        }
        decoder.reset();
        Check(c.created == c.destroyed && !liveActivations, "stream-change lifetime clean");
    }
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        candidates.assign(1, {}); auto& c = candidates[0];
        if (scenario == 0) c.attributes = E_NOTIMPL;
        if (scenario == 1) c.nullAttributes = true;
        if (scenario == 2) c.before.hasSize = false;
        if (scenario == 3) c.before.hasStride = false;
        llcv::video::MjpegDecoder decoder;
        const HRESULT hr = decoder.initialize(4, 2, 60, nullptr, nullptr,
            llcv::video_color::MatrixOverride::Auto, nullptr);
        Check(scenario == 2 ? FAILED(hr) : hr == S_OK,
              "optional attributes/stride accepted, missing required dimensions rejected");
        if (scenario < 2) Check(c.attributeCalls == 1 && c.lowLatency == 0,
            "unsupported low-latency store does not prevent decoding");
        if (scenario == 3) Check(decoder.stride() == 4, "packed NV12 stride fallback");
        decoder.reset();
        Check(c.created == c.destroyed && !liveActivations, "optional attribute lifetime clean");
    }
}

static void TestNativeDecoder() {
    // WIC builds an in-memory JPEG fixture; the actual registered synchronous
    // MJPEG MFT is exercised with no capture hardware or persistent files.
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");
    useSystemDecoder = true;
    for (UINT32 width : {640u, 1280u, 1920u}) {
        const UINT32 height = width * 9 / 16;
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        Check(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)), "JPEG memory stream");
        Check(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)), "JPEG encoder");
        Check(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)), "JPEG encoder initialized");
        Check(SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)), "JPEG frame");
        Check(SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height)), "JPEG dimensions");
        WICPixelFormatGUID pixel = GUID_WICPixelFormat24bppBGR;
        Check(SUCCEEDED(frame->SetPixelFormat(&pixel)) && pixel == GUID_WICPixelFormat24bppBGR, "JPEG BGR fixture");
        std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 3);
        for (UINT32 y = 0; y < height; ++y) {
            for (UINT32 x = 0; x < width; ++x) {
                const size_t offset = (static_cast<size_t>(y) * width + x) * 3;
                pixels[offset] = static_cast<BYTE>(x * 255 / width);
                pixels[offset + 1] = static_cast<BYTE>(y * 255 / height);
                pixels[offset + 2] = 127;
            }
        }
        Check(SUCCEEDED(frame->WritePixels(height, width * 3,
            static_cast<UINT>(pixels.size()), pixels.data())), "JPEG pixels encoded");
        Check(SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()), "JPEG committed");
        STATSTG stat{};
        Check(SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart <= LONG_MAX, "JPEG length");
        Check(SUCCEEDED(stream->Seek({}, STREAM_SEEK_SET, nullptr)), "JPEG stream rewind");
        Sample sample;
        sample.bytes = sample.capacity = static_cast<long>(stat.cbSize.QuadPart);
        sample.data.resize(sample.bytes);
        ULONG read = 0;
        Check(SUCCEEDED(stream->Read(sample.data.data(), sample.bytes, &read)) &&
            read == static_cast<ULONG>(sample.bytes), "JPEG fixture read");
        std::vector<BYTE> automaticPixels;
        llcv::video_color::Configuration automaticColor;
        uint64_t automaticInputAllocations = 0, automaticOutputAllocations = 0;
        for (auto range : {llcv::video_color::RangeOverride::Auto,
                           llcv::video_color::RangeOverride::Limited,
                           llcv::video_color::RangeOverride::Full}) {
            llcv::video::MjpegDecoder decoder;
            Check(SUCCEEDED(decoder.initialize(width, height, 60, nullptr, nullptr,
                llcv::video_color::MatrixOverride::Auto, nullptr, range)), "system MJPEG decoder initialized");
            unsigned decoded = 0;
            for (unsigned index = 0; index < 8; ++index) {
                ComPtr<IMFMediaBuffer> output;
                Check(SUCCEEDED(index%2?decoder.decode(sample.data.data(),
                    static_cast<DWORD>(sample.bytes),index*10000000LL/60,10000000/60,&output,true):
                    decoder.decode(&sample,&output)), "system MJPEG display/recording decode succeeds");
                if (!output) continue;
                BYTE* data = nullptr; DWORD capacity = 0, length = 0;
                Check(SUCCEEDED(output->Lock(&data, &capacity, &length)), "native NV12 buffer locked");
                Check(data && decoder.validOutputBuffer(capacity, length), "native NV12 output layout valid");
                Check(data[0] != data[(height - 1) * decoder.stride() + width - 1], "decoded gradient preserved");
                if (index == 0) {
                    std::vector<BYTE> packed;
                    packed.reserve(static_cast<size_t>(width) * height * 3 / 2);
                    for (UINT32 row = 0; row < height * 3 / 2; ++row)
                        packed.insert(packed.end(), data + row * decoder.stride(), data + row * decoder.stride() + width);
                    if (range == llcv::video_color::RangeOverride::Auto) {
                        automaticPixels = packed;
                        automaticColor = decoder.colorConfiguration();
                    } else {
                        Check(packed == automaticPixels, "manual SDR range leaves native decoded pixels byte-identical");
                        auto expected = automaticColor;
                        expected.range = range == llcv::video_color::RangeOverride::Full
                            ? llcv::video_color::Range::Full : llcv::video_color::Range::Limited;
                        expected.rangeSource = llcv::video_color::Source::UserOverride;
                        Check(decoder.colorConfiguration() == expected,
                              "native manual range changes only interpretation, preserving detected matrix");
                    }
                }
                output->Unlock(); ++decoded;
            }
            Check(decoded == 8, "native decoder outputs each frame without accumulating a queue");
            const auto inputStats = decoder.inputAllocations(), outputStats = decoder.outputAllocations();
            Check(inputStats.reuses > 0 && (!outputStats.allocations || outputStats.reuses > 0),
                  "native decoder reuses memory buffers after their consumers release them");
            if (range == llcv::video_color::RangeOverride::Auto) {
                automaticInputAllocations = inputStats.allocations;
                automaticOutputAllocations = outputStats.allocations;
            } else {
                Check(inputStats.allocations == automaticInputAllocations &&
                      outputStats.allocations == automaticOutputAllocations,
                      "manual range adds no native decoder buffer allocations");
            }
            std::printf("Native MJPEG: %u x %u, range %u, stride %ld, %u/8 frames passed.\n",
                width, height, static_cast<unsigned>(range), decoder.stride(), decoded);
            std::printf("MJPEG buffer allocations/reuses: input %u/%u, output %u/%u.\n",
                inputStats.allocations, inputStats.reuses, outputStats.allocations, outputStats.reuses);
        }
    }
    useSystemDecoder = false;
}

static void TestBufferReuse() {
    using Microsoft::WRL::ComPtr;
    llcv::video::MediaBufferPool pool;
    std::array<ComPtr<IMFMediaBuffer>, 5> held;
    for (unsigned i = 0; i < held.size(); ++i) {
        Check(pool.Acquire(256, 128, &held[i]) == S_OK, "retained buffers can exceed bounded cache safely");
        BYTE* data = nullptr; held[i]->Lock(&data, nullptr, nullptr);
        Check(reinterpret_cast<ULONG_PTR>(data) % 128 == 0, "aligned buffer address");
        std::memset(data, static_cast<int>(i), 256); held[i]->Unlock(); held[i]->SetCurrentLength(256);
    }
    for (unsigned i = 0; i < held.size(); ++i) {
        BYTE* data = nullptr; held[i]->Lock(&data, nullptr, nullptr);
        Check(data[0] == i && data[255] == i, "externally held buffers are never overwritten by pool reuse");
        held[i]->Unlock();
    }
    auto* releasedIdentity = held[1].Get(); held[1].Reset();
    Check(pool.Acquire(128, 64, &held[1]) == S_OK && held[1].Get() == releasedIdentity,
          "only released cached storage is reused for compatible requirements");
    DWORD length = 1; held[1]->GetCurrentLength(&length);
    Check(length == 0 && pool.Stats().allocations == 5 && pool.Stats().reuses == 1,
          "reused buffers reset length and allocator counts remain bounded");
    pool.Reset();
    BYTE* survivor = nullptr;
    Check(SUCCEEDED(held[0]->Lock(&survivor, nullptr, nullptr)) && survivor[255] == 0,
          "consumer buffers survive decoder reset/destruction");
    held[0]->Unlock();
    for (auto& buffer : held) buffer.Reset();
    Check(pool.Acquire(256, 3, &held[0]) == E_INVALIDARG, "invalid alignment is rejected, not silently ignored");
    Check(pool.Acquire(256, 0, &held[0]) == S_OK, "unspecified alignment remains supported");
    held[0].Reset();
    Check(pool.Acquire(512, 256, &held[0]) == S_OK,
          "free cached buffers are resized for changed stream requirements");
}

int main(int argc, char** argv) {
    Check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM initialized");
    if (argc == 2 && std::strcmp(argv[1], "--native") == 0) {
        TestNativeDecoder(); CoUninitialize(); return 0;
    }
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        candidates.assign(2, {});
        if (scenario == 0) candidates[1].activate = E_FAIL;
        if (scenario == 2) candidates[0].input = E_FAIL;
        if (scenario == 3) candidates[0].input = candidates[1].input = E_FAIL;
        if (scenario == 4) candidates[0].activate = E_FAIL;
        if (scenario == 5) candidates[0].info = E_FAIL;
        llcv::video::MjpegDecoder decoder;
        HRESULT hr = decoder.initialize(4, 2, 60, nullptr, nullptr,
                                        llcv::video_color::MatrixOverride::Auto, nullptr);
        Check((scenario == 3 ? FAILED(hr) : hr == S_OK), "first successful codec determines result");
        Check(liveActivations == 0, "every activation released including unused candidates");
        const unsigned chosen = scenario < 2 ? 0 : 1;
        if (scenario < 2)
            Check(candidates[1].activated == 0, "successful decoder stops further activation");
        if (SUCCEEDED(hr)) {
            Check(decoder.validOutputBuffer(24, 24) && !decoder.validOutputBuffer(24, 23) &&
                  !decoder.validOutputBuffer(23, 24) && !decoder.validOutputBuffer(0, 0),
                  "NV12 bounds include padded luma and chroma rows");
            Sample sample;
            IMFMediaBuffer* output = nullptr;
            for (long invalid : {-1L, 0L, 5L, LONG_MAX}) {
                sample.bytes = invalid;
                Check(decoder.decode(&sample, &output) == E_INVALIDARG && output == nullptr &&
                      sample.pointerCalls == 0 && candidates[chosen].inputs == 0,
                      "invalid compressed lengths rejected before pointer access or copy");
            }
            sample.bytes = 4;
            Check(decoder.decode(&sample, &output) == S_OK && output == nullptr &&
                  candidates[chosen].inputs == 1, "valid compressed packet reaches chosen transform");
        }
        decoder.reset(); decoder.reset();
        for (const auto& c : candidates)
            Check(c.created == c.destroyed, "failed and selected decoder references released exactly once");
        Check(!decoder.validOutputBuffer(24, 24), "reset decoder has no usable output layout");
    }
    TestStreamChanges();
    TestRecordingFramePolicy();
    TestColorSelection();
    TestInputTiming();
    TestBufferReuse();
    CoUninitialize();
    std::puts("MJPEG: 6 selection cases, packet bounds, 15 stream changes and 4 attribute/layout cases passed.");
}
