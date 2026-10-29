#include "ScreenshotService.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <shlobj.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace llcv::screenshot {
using Microsoft::WRL::ComPtr;
namespace {
constexpr HRESULT Cancelled = HRESULT_FROM_WIN32(ERROR_CANCELLED);
HRESULT Win32Failure() noexcept {
    const DWORD error=GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}
class Pacer {
public:
    explicit Pacer(unsigned ms) : ms_(ms) {
        if (ms_) timer_=CreateWaitableTimerExW(nullptr,nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
    ~Pacer() { if (timer_) CloseHandle(timer_); }
    void Pause() noexcept {
        if (!ms_) return;
        LARGE_INTEGER due{};
        due.QuadPart=-static_cast<LONGLONG>(ms_)*10000;
        if (timer_ && SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE) &&
            WaitForSingleObject(timer_,1000)==WAIT_OBJECT_0) return;
        // Older Windows releases can lack high-resolution timers. Stay
        // conservative there, without changing global timer resolution.
        Sleep(ms_);
    }
    Pacer(const Pacer&) = delete;
    Pacer& operator=(const Pacer&) = delete;
private:
    unsigned ms_;
    HANDLE timer_=nullptr;
};
bool ValidBgra(unsigned w, unsigned h, std::span<const std::uint8_t> data) {
    return w && h && w <= 3840 && h <= 2160 && data.size() >= std::size_t(w)*h*4;
}
std::wstring UniqueName() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};
    wchar_t id[40]{};
    if (!StringFromGUID2(guid, id, 40)) return {};
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stamp[64]{};
    swprintf_s(stamp, L"Screenshot_%04u%02u%02u_%02u%02u%02u_",
        time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond);
    return std::wstring(stamp) + id + L".png";
}
}

std::wstring DefaultDirectory() {
    PWSTR folder = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &folder);
    std::wstring result;
    if (SUCCEEDED(hr) && folder) result = std::wstring(folder) + L"\\LowLatencyCaptureViewer";
    CoTaskMemFree(folder);
    return result;
}

HRESULT WritePng(const std::wstring& path, unsigned width, unsigned height,
                 std::span<const std::uint8_t> bgra, const std::atomic<bool>& cancel,
                 unsigned pauseMs) {
    if (!ValidBgra(width,height,bgra) || path.empty()) return E_INVALIDARG;
    if (cancel.load()) return Cancelled;
    // Reserve a private sibling, then atomically rename the completed file.
    // Neither a failed encode nor a name collision overwrites an existing PNG.
    const std::wstring partial = path + L".partial";
    const HANDLE reservation = CreateFileW(partial.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reservation == INVALID_HANDLE_VALUE) return Win32Failure();
    CloseHandle(reservation);
    HRESULT hr = E_FAIL;
    {
        Pacer pacer(pauseMs);
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(partial.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
        if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(width,height);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        // Explicit SDR tagging also prevents a HDR source from being mistaken
        // for PQ-coded PNG by consumers. File and clipboard share this BGRA.
        if (SUCCEEDED(hr)) {
            ComPtr<IWICMetadataQueryWriter> metadata;
            hr = frame->GetMetadataQueryWriter(&metadata);
            PROPVARIANT intent{};
            intent.vt = VT_UI1;
            intent.bVal = 0;
            if (SUCCEEDED(hr)) hr = metadata->SetMetadataByName(L"/sRGB/RenderingIntent", &intent);
        }
        for (unsigned y=0; SUCCEEDED(hr) && y<height; y+=8) {
            if (cancel.load()) { hr=Cancelled; break; }
            const unsigned rows=std::min(8u,height-y);
            hr=frame->WritePixels(rows,width*4,rows*width*4,
                const_cast<BYTE*>(bgra.data()+std::size_t(y)*width*4));
            pacer.Pause();
        }
        if (SUCCEEDED(hr) && cancel.load()) hr=Cancelled;
        if (SUCCEEDED(hr)) hr=frame->Commit();
        if (SUCCEEDED(hr)) hr=encoder->Commit();
    }
    if (SUCCEEDED(hr) && cancel.load()) hr=Cancelled;
    if (SUCCEEDED(hr) && !MoveFileExW(partial.c_str(),path.c_str(),0))
        hr=Win32Failure();
    if (FAILED(hr)) DeleteFileW(partial.c_str());
    return hr;
}

HRESULT PublishClipboard(std::span<const std::uint8_t> bgra, unsigned width,
                         unsigned height, const std::atomic<bool>& cancel) {
    if (!ValidBgra(width,height,bgra)) return E_INVALIDARG;
    if (cancel.load()) return Cancelled;
    const auto size=std::size_t(width)*height*4;
    HGLOBAL dib=GlobalAlloc(GMEM_MOVEABLE,sizeof(BITMAPV5HEADER)+size);
    if (!dib) return E_OUTOFMEMORY;
    void* memory=GlobalLock(dib);
    if (!memory) { GlobalFree(dib); return E_OUTOFMEMORY; }
    auto* header=static_cast<BITMAPV5HEADER*>(memory);
    *header={};
    header->bV5Size=sizeof(*header);
    header->bV5Width=static_cast<LONG>(width);
    header->bV5Height=-static_cast<LONG>(height);
    header->bV5Planes=1; header->bV5BitCount=32; header->bV5Compression=BI_BITFIELDS;
    header->bV5SizeImage=static_cast<DWORD>(size);
    header->bV5RedMask=0x00ff0000; header->bV5GreenMask=0x0000ff00;
    header->bV5BlueMask=0x000000ff; header->bV5AlphaMask=0xff000000;
    header->bV5CSType=LCS_sRGB; header->bV5Intent=LCS_GM_IMAGES;
    std::memcpy(header+1,bgra.data(),size);
    GlobalUnlock(dib);
    // A real owner is necessary: OpenClipboard(nullptr)+EmptyClipboard can
    // leave no owner, causing SetClipboardData to fail. No delayed rendering.
    const HWND owner=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,
                                      nullptr,GetModuleHandleW(nullptr),nullptr);
    HRESULT hr=owner ? HRESULT_FROM_WIN32(ERROR_BUSY) : Win32Failure();
    const auto deadline=GetTickCount64()+150;
    if (owner) do {
        if (cancel.load()) { hr=Cancelled; break; }
        if (OpenClipboard(owner)) {
            if (cancel.load()) hr=Cancelled;
            else if (!EmptyClipboard()) hr=Win32Failure();
            else if (SetClipboardData(CF_DIBV5,dib)) { dib=nullptr; hr=S_OK; }
            else hr=Win32Failure();
            CloseClipboard();
            break;
        }
        Sleep(5);
    } while (GetTickCount64()<deadline);
    if (owner) DestroyWindow(owner);
    if (dib) GlobalFree(dib); // Only free when ownership was not transferred.
    return hr;
}

Service::~Service() { Stop(); }
bool Service::Start(unsigned width, unsigned height, Format format, std::wstring directory, bool topDown) noexcept {
    Stop();
    try {
        desc_={width,height,format};
        desc_.topDown=topDown;
        const auto size=PackedSize(desc_);
        if (!size || directory.empty()) return false;
        raw_.resize(size); // Pre-touch before streaming, not on a screenshot frame.
        directory_=std::move(directory);
        event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!event_) return false;
        cancel_.store(false);
        thread_=std::thread(&Service::Run,this);
        state_.store(State::Idle,std::memory_order_release);
        return true;
    } catch (...) { Stop(); return false; }
}
void Service::Stop() noexcept {
    state_.store(State::Unavailable,std::memory_order_release);
    cancel_.store(true);
    if (event_) SetEvent(event_);
    if (thread_.joinable()) thread_.join();
    if (event_) { CloseHandle(event_); event_=nullptr; }
    // Retain capacity across output-only resets; release on object destruction.
}
void Service::CancelPending() noexcept {
    cancel_.store(true,std::memory_order_release);
}
bool Service::Request(bool clipboard) noexcept {
    // Single UI producer. Idle guarantees neither the capture thread nor the
    // worker is using options. Publish options before the release transition.
    if (cancel_.load() || state_.load(std::memory_order_acquire)!=State::Idle) return false;
    clipboard_=clipboard;
    requestedAt_.store(GetTickCount64());
    State expected=State::Idle;
    return state_.compare_exchange_strong(expected,State::Requested,std::memory_order_release);
}
void Service::Submit(const Description& desc, const std::uint8_t* data,
                     std::size_t length, unsigned stride) noexcept {
    State expected=State::Requested;
    // No lock, allocation, COM, conversion, disk I/O or GPU readback when idle.
    if (state_.load(std::memory_order_relaxed)!=expected ||
        !state_.compare_exchange_strong(expected,State::Copying,std::memory_order_acquire)) return;
    const auto begin=std::chrono::steady_clock::now();
    if (desc.width!=desc_.width || desc.height!=desc_.height || desc.format!=desc_.format || desc.topDown!=desc_.topDown ||
        !CopyFrame(desc,data,length,stride,raw_)) {
        state_.store(State::Requested,std::memory_order_release);
        return;
    }
    desc_=desc;
    copyMs_=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    state_.store(State::Processing,std::memory_order_release);
    SetEvent(event_);
}
bool Service::TakeResult(Result& result) {
    if (state_.load(std::memory_order_acquire)!=State::Complete) return false;
    std::lock_guard lock(resultMutex_);
    result=std::move(result_);
    State expected=State::Complete;
    state_.compare_exchange_strong(expected,State::Idle,std::memory_order_release);
    return true;
}
bool Service::ExpireRequest() noexcept {
    if (GetTickCount64()-requestedAt_.load()<3000) return false;
    State expected=State::Requested;
    return state_.compare_exchange_strong(expected,State::Idle,std::memory_order_acq_rel);
}
void Service::Run() noexcept {
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    Pacer pacer(2);
    while (WaitForSingleObject(event_,INFINITE)==WAIT_OBJECT_0 && !cancel_.load()) {
        if (state_.load(std::memory_order_acquire)!=State::Processing) continue;
        Result result;
        result.clipboardRequested=clipboard_;
        result.toneMapped=desc_.format==Format::P010;
        result.copyMs=copyMs_;
        try {
            if (FAILED(com)) result.fileResult=com;
            else {
                std::vector<std::uint8_t> bgra(std::size_t(desc_.width)*desc_.height*4);
                bool valid=true;
                for (unsigned y=0; y<desc_.height && valid && !cancel_.load(); y+=8) {
                    const unsigned rows=std::min(8u,desc_.height-y);
                    valid=ConvertRows(desc_,raw_,y,rows,
                        std::span(bgra).subspan(std::size_t(y)*desc_.width*4,std::size_t(rows)*desc_.width*4));
                    pacer.Pause();
                }
                if (cancel_.load()) result.fileResult=Cancelled;
                else if (!valid) result.fileResult=E_INVALIDARG;
                else {
                    // Clipboard first, but its failure never suppresses the PNG.
                    if (clipboard_) result.clipboardResult=PublishClipboard(bgra,desc_.width,desc_.height,cancel_);
                    std::error_code error;
                    std::filesystem::create_directories(directory_,error);
                    if (error) result.fileResult=HRESULT_FROM_WIN32(error.value());
                    else {
                        const auto name=UniqueName();
                        if (name.empty()) result.fileResult=E_FAIL;
                        else {
                            result.path=directory_+L"\\"+name;
                            result.fileResult=WritePng(result.path,desc_.width,desc_.height,bgra,cancel_);
                        }
                    }
                }
            }
        } catch (const std::bad_alloc&) { result.fileResult=E_OUTOFMEMORY; }
          catch (...) { result.fileResult=E_FAIL; }
        if (cancel_.load()) break;
        {
            std::lock_guard lock(resultMutex_);
            result_=std::move(result);
        }
        State expected=State::Processing;
        state_.compare_exchange_strong(expected,State::Complete,std::memory_order_release);
    }
    if (SUCCEEDED(com)) CoUninitialize();
}
} // namespace llcv::screenshot
