// Runs against the real sample callback/mailbox; no capture device is opened.
#include "video/PresentationPolicy.h"

static void TestVideoSampleChanges() {
    using namespace llcv::capture;
    using namespace llcv::video;
    using llcv::settings::PresentationMode;
    for (auto mode : {PresentationMode::AllowTearing, PresentationMode::VSync, PresentationMode::Compatibility})
    for (auto format : {VideoPixelFormat::Auto, VideoPixelFormat::Nv12, VideoPixelFormat::Yuy2,
                        VideoPixelFormat::P010, VideoPixelFormat::Mjpeg})
    for (bool audioOnly : {false, true}) {
        const bool allowed = audioOnly || mode != PresentationMode::Compatibility || format != VideoPixelFormat::P010;
        Check(llcv::presentation::SupportsCapture(mode, format, audioOnly) == allowed,
              "HDR-only output guard is independent of Force and preserves audio-only");
    }
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(ready != nullptr, "format event");
    if (!ready) return;
    VIDEOINFOHEADER2 info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = info.bmiHeader.biHeight = 4;
    info.bmiHeader.biSizeImage = 48;
    info.AvgTimePerFrame = 166667;
    AM_MEDIA_TYPE type{};
    type.majortype = MEDIATYPE_Video; type.subtype = MFVideoFormat_P010;
    type.formattype = FORMAT_VideoInfo2;
    type.cbFormat = sizeof(info); type.pbFormat = reinterpret_cast<BYTE*>(&info);
    VideoSampleFormat expected{};
    Check(ReadVideoSampleFormat(&type, 60, expected), "normalize connected P010 type");
    unsigned scenarios = 0;
    for (unsigned iteration=0; iteration<40; ++iteration)
    for (unsigned scenario=0; scenario<24; ++scenario) {
        LatestVideoSample slot(48, ready);
        auto* cb = new VideoSampleGrabberCallback(&slot, nullptr, expected);
        auto* normal = new Sample(48);
        Check(cb->SampleCB(0, normal) == S_OK, "initial valid frame");
        normal->Release();
        auto newInfo = info;
        auto newType = type; newType.pbFormat = reinterpret_cast<BYTE*>(&newInfo);
        auto* changed = new Sample(48);
        changed->dynamicType = &newType; changed->typeResult = S_OK;
        bool same = false;
        // Color declaration: PQ/BT.2020/limited/top-left, DXVA flags.
        const DWORD pq = 0x81u | (7u << 8) | (2u << 12) | (4u << 15) | (9u << 22) | (15u << 27);
        switch (scenario) {
        case 0: same=true; break; // identical reannouncement
        case 1: newInfo.bmiHeader.biSizeImage=0; same=true; break;
        case 2: newInfo.AvgTimePerFrame=0; same=true; break;
        case 3: newInfo.dwBitRate=100; newInfo.dwBitErrorRate=3; same=true; break;
        case 4: newInfo.dwPictAspectRatioX=16; newInfo.dwPictAspectRatioY=16; same=true; break;
        case 5: newInfo.rcSource={0,0,4,4}; newInfo.rcTarget=newInfo.rcSource; same=true; break;
        case 6: newInfo.dwControlFlags=pq; same=true; break;
        case 7: newInfo.dwControlFlags=pq | (8u << 8); same=true; break;
        case 8: newInfo.dwControlFlags=pq; newInfo.dwControlFlags ^= (15u ^ 5u) << 27; break;
        case 9: newInfo.dwControlFlags=pq; newInfo.dwControlFlags ^= (2u ^ 1u) << 12; break;
        case 10: newInfo.dwControlFlags=pq; newInfo.dwControlFlags ^= (7u ^ 5u) << 8; break;
        case 11: newInfo.bmiHeader.biSizeImage=96; break; // new stride
        case 12: newInfo.bmiHeader.biWidth=2; break;
        case 13: newInfo.bmiHeader.biHeight=8; break;
        case 14: newType.subtype=MEDIASUBTYPE_NV12; break;
        case 15: newInfo.AvgTimePerFrame=333333; break;
        case 16: newInfo.dwInterlaceFlags=1; break;
        case 17: newInfo.rcSource={1,0,4,4}; break;
        case 18: newInfo.dwPictAspectRatioX=16; newInfo.dwPictAspectRatioY=9; break;
        case 19: newType.cbFormat=1; break;
        case 20: newType.pbFormat=nullptr; break;
        case 21: changed->dynamicType=nullptr; break; // S_OK without a type
        case 22: changed->typeResult=E_OUTOFMEMORY; break; // must still free returned type
        case 23: changed->SetActualDataLength(0); newInfo.bmiHeader.biWidth=2; break;
        }
        auto* identity = new Sample(1);
        newType.pUnk = identity; // ensure returned type's reference is always released
        const auto result = cb->SampleCB(0, changed);
        Check(changed->typeQueries==1 && identity->References()==1, "type queried once; allocated COM type released");
        Check((result == S_OK) == same && (SUCCEEDED(slot.FormatFailure()) == same),
              "same semantic format accepted; changed/invalid type latched");
        identity->Release(); changed->Release();
        auto* after = new Sample(48);
        Check((cb->SampleCB(0, after)==S_OK)==same, "ordinary later frame cannot erase the type-change latch");
        int64_t arrival=0;
        auto* taken=slot.TakeLatest(arrival);
        Check(same ? taken==after : taken==nullptr, "rejected change clears retained old input and blocks later frames");
        if (taken) taken->Release();
        Check(same || WaitForSingleObject(ready,0)==WAIT_OBJECT_0,
              "type failure wakes owner even without valid input");
        after->Release(); cb->Release(); ++scenarios;
    }
    // A capture-rate change must never hide behind an unchanged resolution or
    // pixel format. Fractional NTSC equivalents keep their nominal FPS, while
    // an announced jump to 120 must clear retained input and wake the owner.
    for (const GUID subtype : {MEDIASUBTYPE_NV12, MEDIASUBTYPE_YUY2,
                               MFVideoFormat_P010, MEDIASUBTYPE_MJPG})
    for (int requested : {30, 40, 60}) {
        auto requestedInfo = info;
        requestedInfo.bmiHeader.biSizeImage = subtype == MFVideoFormat_P010 ? 48 :
            subtype == MEDIASUBTYPE_YUY2 ? 32 : 24;
        requestedInfo.AvgTimePerFrame = 10000000LL / requested;
        auto requestedType = type;
        requestedType.subtype = subtype;
        requestedType.pbFormat = reinterpret_cast<BYTE*>(&requestedInfo);
        VideoSampleFormat requestedFormat{};
        Check(ReadVideoSampleFormat(&requestedType, requested, requestedFormat),
              "selected rate format initializes across raw and compressed capture");
        LatestVideoSample slot(requestedInfo.bmiHeader.biSizeImage, ready);
        auto* callback = new VideoSampleGrabberCallback(&slot, nullptr, requestedFormat);
        auto* sample = new Sample(requestedInfo.bmiHeader.biSizeImage);
        Check(callback->SampleCB(0, sample) == S_OK,
              "unchanged input remains valid before a rate reannouncement");

        auto announcedInfo = requestedInfo;
        auto announcedType = requestedType;
        announcedType.pbFormat = reinterpret_cast<BYTE*>(&announcedInfo);
        sample->dynamicType = &announcedType;
        sample->typeResult = S_OK;
        if (requested == 30 || requested == 60) {
            announcedInfo.AvgTimePerFrame = 10010000LL / requested;
            Check(callback->SampleCB(0, sample) == S_OK,
                  "29.97/59.94 equivalents do not falsely reject the selected nominal FPS");
        }
        announcedInfo.AvgTimePerFrame = 10000000LL / 120;
        Check(callback->SampleCB(0, sample) == VFW_E_TYPE_NOT_ACCEPTED &&
                  FAILED(slot.FormatFailure()),
              "announced 30/40/60 to 120 change cannot silently increase input FPS");
        int64_t arrival = 0;
        auto* retained = slot.TakeLatest(arrival);
        Check(retained == nullptr, "rate change removes stale retained sample");
        if (retained) retained->Release();
        sample->dynamicType = nullptr;
        sample->typeResult = S_FALSE;
        Check(callback->SampleCB(0, sample) == VFW_E_TYPE_NOT_ACCEPTED,
              "later untagged sample cannot clear rate-change rejection");
        sample->Release();
        callback->Release();
    }
    // MJPEG's compressed allocation hint may change without a layout change.
    auto jpeg = type; jpeg.subtype = MEDIASUBTYPE_MJPG;
    VideoSampleFormat jpegExpected{};
    Check(ReadVideoSampleFormat(&jpeg,60,jpegExpected), "MJPEG signature");
    auto jpegInfo=info; jpegInfo.bmiHeader.biSizeImage=123456;
    jpeg.pbFormat=reinterpret_cast<BYTE*>(&jpegInfo);
    Check(MatchesVideoSampleFormat(&jpeg,jpegExpected), "variable compressed size is not a format change");
    {
        auto selectedHint=expected;
        selectedHint.color.present=true;
        selectedHint.color.transferFunction=15; selectedHint.color.primaries=9;
        selectedHint.color.transferMatrix=4; selectedHint.color.nominalRange=2;
        selectedHint.color.chromaSubsampling=5;
        Check(MatchesVideoSampleFormat(&type,selectedHint),
              "omitted sample metadata preserves selected-format hints used by renderer");
        auto newInfo=info; auto newType=type;
        newType.pbFormat=reinterpret_cast<BYTE*>(&newInfo);
        newInfo.dwControlFlags=0x81u | (7u<<8) | (2u<<12) | (4u<<15) | (9u<<22) | (15u<<27);
        Check(!MatchesVideoSampleFormat(&newType,selectedHint),
              "new top-left declaration cannot silently replace effective selected left hint");
        newInfo.dwControlFlags ^= (7u ^ 5u) << 8;
        Check(MatchesVideoSampleFormat(&newType,selectedHint), "same effective left HDR declaration accepted");
    }
    {
        std::atomic<uint64_t> tracking{0}, captured{0}, replaced{0};
        LatestVideoSample slot(48,ready,{&tracking,&captured,&replaced});
        auto* old=new Sample(48,1); slot.Push(old); old->Release();
        int64_t arrival=0;
        auto* current=slot.TakeLatest(arrival);
        const auto oldArrival=arrival;
        slot.RefreshAfterOutputReset(current,arrival);
        Check(current && static_cast<Sample*>(current)->Sequence()==1 && arrival==oldArrival,
              "output reset without new input does not discard usable frame or wait");
        for(unsigned sequence=2; sequence<=6; ++sequence) {
            auto* fresh=new Sample(48,sequence); slot.Push(fresh); fresh->Release();
        }
        slot.RefreshAfterOutputReset(current,arrival);
        Check(current && static_cast<Sample*>(current)->Sequence()==6 && arrival>=oldArrival && liveSamples==1,
              "after output reset upload the newest frame; no stale queued frames");
        Check(captured==6 && replaced==5,
              "freshness replacement accounts for the previously taken but never displayed frame");
        if(current) current->Release();
    }
    for(unsigned repeat=0; repeat<100; ++repeat) {
        HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        HANDLE resume=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        Check(entered && resume,"format race gates");
        if(!entered || !resume) {
            if(entered) CloseHandle(entered);
            if(resume) CloseHandle(resume);
            break;
        }
        LatestVideoSample slot(48,ready);
        auto* cb=new VideoSampleGrabberCallback(&slot,nullptr,expected);
        auto* sample=new Sample(48);
        sample->sizeEntered=entered; sample->sizeResume=resume;
        std::thread producer([&] { cb->SampleCB(0,sample); });
        Check(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0,"valid producer paused before publication");
        slot.RejectFormat(VFW_E_TYPE_NOT_ACCEPTED);
        SetEvent(resume); producer.join();
        int64_t arrival=0;
        auto* taken=slot.TakeLatest(arrival);
        Check(taken==nullptr && sample->References()==1,
              "concurrent valid publication cannot repopulate a format-rejected slot");
        if(taken) taken->Release();
        sample->Release(); cb->Release();
        CloseHandle(entered); CloseHandle(resume);
    }
    CloseHandle(ready);
    Check(liveSamples==0,"type changes and output refresh leave no sample references");
    std::printf("Dynamic video format: %u callback scenarios + 100 publication races; ownership, wakeup and reset freshness passed.\n",scenarios);
}

// Real VideoSampleGrabberCallback/LatestVideoSample integration (not just the
// DirectShowVideoFormat unit layer) for RGB32: a silent orientation flip on
// dynamic reconnect must be rejected, and a short/truncated RGB32 buffer must
// be dropped by the existing generic length guard rather than accepted as a
// partial frame.
static void TestRgb32CallbackOrientationAndShortBuffer() {
    using namespace llcv::capture;
    using namespace llcv::video;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(ready != nullptr, "rgb32 callback format event");
    if (!ready) return;

    VIDEOINFOHEADER2 info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biWidth = 4; info.bmiHeader.biHeight = -4; // top-down
    info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = 64; // 4 * 4 * 4
    info.AvgTimePerFrame = 166667;
    AM_MEDIA_TYPE type{};
    type.majortype = MEDIATYPE_Video; type.subtype = MEDIASUBTYPE_RGB32;
    type.formattype = FORMAT_VideoInfo2;
    type.cbFormat = sizeof(info); type.pbFormat = reinterpret_cast<BYTE*>(&info);
    VideoSampleFormat expected{};
    Check(ReadVideoSampleFormat(&type, 60, expected) && expected.topDown,
          "RGB32 top-down orientation captured on connect");

    LatestVideoSample slot(64, ready);
    auto* cb = new VideoSampleGrabberCallback(&slot, nullptr, expected);
    auto* normal = new Sample(64);
    Check(cb->SampleCB(0, normal) == S_OK, "rgb32 initial valid frame accepted");
    normal->Release();

    auto flippedInfo = info;
    flippedInfo.bmiHeader.biHeight = 4; // same dimensions, bottom-up now
    auto flippedType = type;
    flippedType.pbFormat = reinterpret_cast<BYTE*>(&flippedInfo);
    auto* changed = new Sample(64);
    changed->dynamicType = &flippedType; changed->typeResult = S_OK;
    Check(cb->SampleCB(0, changed) != S_OK,
          "silent RGB32 orientation flip with unchanged dimensions is rejected end to end");
    int64_t arrival = 0;
    auto* retainedAfterFlip = slot.TakeLatest(arrival);
    Check(retainedAfterFlip == nullptr, "rejected orientation flip clears the retained sample");
    if (retainedAfterFlip) retainedAfterFlip->Release();
    changed->Release();
    cb->Release();

    // A fresh callback/slot for the short-buffer case: an RGB32 frame
    // declaring fewer bytes than width*height*4 must not be retained by the
    // generic LatestVideoSample length guard, regardless of pixel format.
    LatestVideoSample shortSlot(64, ready);
    auto* shortCb = new VideoSampleGrabberCallback(&shortSlot, nullptr, expected);
    auto* shortSample = new Sample(64);
    shortSample->SetActualDataLength(32); // half the declared RGB32 frame
    shortCb->SampleCB(0, shortSample);
    int64_t shortArrival = 0;
    auto* retainedShort = shortSlot.TakeLatest(shortArrival);
    Check(retainedShort == nullptr, "truncated RGB32 buffer is not retained as a partial frame");
    if (retainedShort) retainedShort->Release();
    shortSample->Release();
    shortCb->Release();

    CloseHandle(ready);
}

// Same real callback/mailbox integration for RGB24 (GC573's actual advertised
// format, bottom-up by convention): orientation flip rejection, short-buffer
// drop, and a dynamic RGB24<->RGB32 subtype switch at an unchanged size must
// not be silently accepted as "still RGB".
static void TestRgb24CallbackOrientationAndShortBuffer() {
    using namespace llcv::capture;
    using namespace llcv::video;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(ready != nullptr, "rgb24 callback format event");
    if (!ready) return;

    VIDEOINFOHEADER info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biWidth = 4; info.bmiHeader.biHeight = 4; // bottom-up, matches GC573
    info.bmiHeader.biBitCount = 24; info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = 48; // stride align(4*3,4)=12 * height 4
    info.AvgTimePerFrame = 166667;
    AM_MEDIA_TYPE type{};
    type.majortype = MEDIATYPE_Video; type.subtype = MEDIASUBTYPE_RGB24;
    type.formattype = FORMAT_VideoInfo;
    type.cbFormat = sizeof(info); type.pbFormat = reinterpret_cast<BYTE*>(&info);
    VideoSampleFormat expected{};
    Check(ReadVideoSampleFormat(&type, 60, expected) && !expected.topDown,
          "RGB24 bottom-up orientation captured on connect");

    LatestVideoSample slot(48, ready);
    auto* cb = new VideoSampleGrabberCallback(&slot, nullptr, expected);
    auto* normal = new Sample(48);
    Check(cb->SampleCB(0, normal) == S_OK, "rgb24 initial valid frame accepted");
    normal->Release();

    auto flippedInfo = info;
    flippedInfo.bmiHeader.biHeight = -4; // same dimensions, top-down now
    auto flippedType = type;
    flippedType.pbFormat = reinterpret_cast<BYTE*>(&flippedInfo);
    auto* changed = new Sample(48);
    changed->dynamicType = &flippedType; changed->typeResult = S_OK;
    Check(cb->SampleCB(0, changed) != S_OK,
          "silent RGB24 orientation flip with unchanged dimensions is rejected end to end");
    int64_t arrival = 0;
    auto* retainedAfterFlip = slot.TakeLatest(arrival);
    Check(retainedAfterFlip == nullptr, "rejected orientation flip clears the retained sample");
    if (retainedAfterFlip) retainedAfterFlip->Release();
    changed->Release();

    // Dynamic RGB24 -> RGB32 subtype switch at the same pixel dimensions:
    // the format field itself must differ and be rejected, not aliased.
    VIDEOINFOHEADER2 rgb32Info{};
    rgb32Info.AvgTimePerFrame = 166667;
    rgb32Info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    rgb32Info.bmiHeader.biPlanes = 1;
    rgb32Info.bmiHeader.biWidth = 4; rgb32Info.bmiHeader.biHeight = 4;
    rgb32Info.bmiHeader.biBitCount = 32; rgb32Info.bmiHeader.biCompression = BI_RGB;
    AM_MEDIA_TYPE rgb32Type{};
    rgb32Type.majortype = MEDIATYPE_Video; rgb32Type.subtype = MEDIASUBTYPE_RGB32;
    rgb32Type.formattype = FORMAT_VideoInfo2; rgb32Type.cbFormat = sizeof(rgb32Info);
    rgb32Type.pbFormat = reinterpret_cast<BYTE*>(&rgb32Info);
    auto* switched = new Sample(64);
    switched->dynamicType = &rgb32Type; switched->typeResult = S_OK;
    Check(cb->SampleCB(0, switched) != S_OK,
          "silent RGB24 -> RGB32 subtype switch at the same size is rejected end to end");
    switched->Release();
    cb->Release();

    // A fresh callback/slot for the short-buffer case: an RGB24 frame
    // declaring fewer bytes than stride*height must not be retained by the
    // generic LatestVideoSample length guard.
    LatestVideoSample shortSlot(48, ready);
    auto* shortCb = new VideoSampleGrabberCallback(&shortSlot, nullptr, expected);
    auto* shortSample = new Sample(48);
    shortSample->SetActualDataLength(24); // half the declared RGB24 frame
    shortCb->SampleCB(0, shortSample);
    int64_t shortArrival = 0;
    auto* retainedShort = shortSlot.TakeLatest(shortArrival);
    Check(retainedShort == nullptr, "truncated RGB24 buffer is not retained as a partial frame");
    if (retainedShort) retainedShort->Release();
    shortSample->Release();
    shortCb->Release();

    CloseHandle(ready);
}

static int BenchmarkSampleGuard() {
    using namespace llcv::capture;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!event) return 1;
    constexpr unsigned count=300000;
    std::array<double,7> direct{}, guarded{};
    for(unsigned run=0; run<7; ++run) {
        LatestVideoSample slot(48,event);
        auto* cb=new VideoSampleGrabberCallback(&slot);
        auto* sample=new Sample(48);
        const auto measure=[&](bool check) {
            const auto start=std::chrono::steady_clock::now();
            for(unsigned i=0;i<count;++i) {
                if(check) cb->SampleCB(0,sample); else slot.Push(sample);
            }
            return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/count;
        };
        if(run&1) {guarded[run]=measure(true); direct[run]=measure(false);}
        else {direct[run]=measure(false); guarded[run]=measure(true);}
        cb->Release(); sample->Release();
    }
    CloseHandle(event);
    std::sort(direct.begin(),direct.end()); std::sort(guarded.begin(),guarded.end());
    std::printf("Synthetic callback cost, median of 7 x %u: direct mailbox %.3f us; checked callback %.3f us; difference %.3f us.\n",
        count,direct[3],guarded[3],guarded[3]-direct[3]);
    std::puts("No driver/GPU/HDMI latency claim: fake GetMediaType returns S_FALSE without allocation.");
    return liveSamples ? 1:0;
}
