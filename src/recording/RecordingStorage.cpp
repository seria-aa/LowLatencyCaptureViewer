#include "recording/RecordingStorage.h"
#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace llcv::recording {
namespace {
using Microsoft::WRL::ComPtr;
struct __declspec(uuid("548579DA-08A5-419C-A626-5F0D607D48CE")) IStorage : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RequestFlush()=0;
    virtual HRESULT STDMETHODCALLTYPE Result()=0;
    virtual HRESULT STDMETHODCALLTYPE Retire()=0;
    virtual HRESULT STDMETHODCALLTYPE Sibling(IMFByteStream*,IMFByteStream**)=0;
    virtual HRESULT STDMETHODCALLTYPE RequestSpace(const wchar_t*)=0;
    virtual StorageSpace STDMETHODCALLTYPE Space()=0;
};
constexpr GUID kTicketBytes{0x5f0d607d,0x48ce,0x419c,{0xa6,0x26,0x54,0x85,0x79,0xda,0x08,0xa5}};
constexpr GUID kTicketKind{0x5f0d607e,0x48ce,0x419c,{0xa6,0x26,0x54,0x85,0x79,0xda,0x08,0xa5}};
struct File {
    ComPtr<IMFByteStream> bytes;
    std::atomic<bool> flushPending{false};
};
#ifdef LLCV_RECORDING_STORAGE_SPACE_TEST
std::atomic<RecordingSpaceQuery> spaceQueryForTest{nullptr};
#endif
enum class Kind { Write,Read,Resize,Flush,Close,Space };
struct Job {
    Kind kind=Kind::Write;
    std::shared_ptr<File> file;
    std::vector<BYTE> data;
    QWORD position=0,sequence=0;
    BYTE* read=nullptr;
    ULONG count=0,*transferred=nullptr;
    bool coalescedFlush=false;
    std::wstring folder;
};
class Queue {
    StorageLimits limits_;
    std::mutex mutex_;
    std::condition_variable wake_,completed_;
    std::deque<Job> jobs_;
    size_t bytes_=0,operations_=0;
    uint64_t issued_=0,done_=0;
    bool stopping_=false;
    bool spacePending_=false;
    StorageSpace space_;
    std::atomic<HRESULT> error_{S_OK};
    std::thread worker_;
    void Fail(HRESULT hr) {
        if(FAILED(hr)){HRESULT ok=S_OK;error_.compare_exchange_strong(ok,hr);}
    }
    HRESULT PutLocked(Job&& job,uint64_t* sequence,bool finalControl) {
        if(stopping_)return MF_E_SHUTDOWN;
        if(!finalControl && FAILED(Result()))return Result();
        if(!finalControl && (operations_>=limits_.operations || job.data.size()>limits_.bytes-bytes_))
            return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
        job.sequence=issued_+1;
        jobs_.push_back(std::move(job));
        ++issued_;++operations_;bytes_+=jobs_.back().data.size();
        if(sequence)*sequence=issued_;
        wake_.notify_one();return S_OK;
    }
    void Run() {
        const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);Fail(com);
        for(;;){
            Job job;
            {
                std::unique_lock lock(mutex_);wake_.wait(lock,[&]{return stopping_||!jobs_.empty();});
                if(jobs_.empty())break;
                job=std::move(jobs_.front());jobs_.pop_front();
            }
            HRESULT hr=Result();ULONG count=0;
            StorageSpace queried;
#ifdef LLCV_RECORDING_STORAGE_DELAY_TEST
            // Dedicated regression binary only; never enabled in the app.
            if(job.kind==Kind::Flush||job.kind==Kind::Close)Sleep(500);
#endif
            // Even after a write failure, flush/close every owned file so a
            // usable prefix can survive and no split handle is leaked.
            if(job.kind==Kind::Space){
#ifdef LLCV_RECORDING_SPACE_DELAY_TEST
                // Dedicated regression build: expose the old encoder-worker
                // stall without delaying capture or changing normal builds.
                // Longer than even the 16-packet ring at 50 fps: passing must
                // rely on asynchronous queries, not just the delivery margin.
                Sleep(500);
#endif
                ULARGE_INTEGER free{};
#ifdef LLCV_RECORDING_STORAGE_SPACE_TEST
                const auto query=spaceQueryForTest.load();
                queried.known=query?query(job.folder.c_str(),&free)!=FALSE:
                    GetDiskFreeSpaceExW(job.folder.c_str(),&free,nullptr,nullptr)!=FALSE;
#else
                queried.known=GetDiskFreeSpaceExW(job.folder.c_str(),&free,nullptr,nullptr)!=FALSE;
#endif
                queried.completed=true;queried.freeBytes=queried.known?free.QuadPart:0;
                // A failed informational query must not poison file writes.
                hr=S_OK;
            }else if(job.kind==Kind::Flush)hr=job.file->bytes->Flush();
            else if(job.kind==Kind::Close){
                hr=job.file->bytes->Flush();
                const auto close=job.file->bytes->Close();if(SUCCEEDED(hr))hr=close;
            }else if(SUCCEEDED(hr)){
                if(job.kind==Kind::Resize)hr=job.file->bytes->SetLength(job.position);
                else{
                    hr=job.file->bytes->SetCurrentPosition(job.position);
                    if(SUCCEEDED(hr)){
                        if(job.kind==Kind::Read)hr=job.file->bytes->Read(job.read,job.count,&count);
                        else{
                            hr=job.file->bytes->Write(job.data.data(),static_cast<ULONG>(job.data.size()),&count);
                            if(SUCCEEDED(hr)&&count!=job.data.size())hr=HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
                        }
                    }
                }
            }
            Fail(hr);
            if(job.transferred)*job.transferred=count;
            if(job.coalescedFlush)job.file->flushPending=false;
            {
                std::lock_guard lock(mutex_);bytes_-=job.data.size();--operations_;done_=job.sequence;
                if(job.kind==Kind::Space){space_=queried;spacePending_=false;}
            }
            completed_.notify_all();
        }
        if(SUCCEEDED(com))CoUninitialize();
    }
public:
    explicit Queue(StorageLimits limits):limits_(limits),worker_([this]{Run();}){}
    ~Queue(){
        {std::lock_guard lock(mutex_);stopping_=true;}wake_.notify_one();
        if(worker_.joinable())worker_.join();
    }
    HRESULT Result() const {return error_.load();}
    HRESULT CanAccept(size_t bytes){
        std::lock_guard lock(mutex_);
        if(FAILED(Result()))return Result();
        return operations_>=limits_.operations || bytes>limits_.bytes-bytes_ ? HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW):S_OK;
    }
    HRESULT Put(Job job,uint64_t* sequence=nullptr,bool finalControl=false) noexcept {
        try{
            std::lock_guard lock(mutex_);
            return PutLocked(std::move(job),sequence,finalControl);
        }catch(...){return E_OUTOFMEMORY;}
    }
    HRESULT RequestSpace(const wchar_t* folder) noexcept {
        if(!folder || !*folder)return E_INVALIDARG;
        try{
            std::lock_guard lock(mutex_);
            if(spacePending_)return Result();
            Job job;job.kind=Kind::Space;job.folder=folder;
            const auto hr=PutLocked(std::move(job),nullptr,false);
            if(SUCCEEDED(hr))spacePending_=true;
            return hr;
        }catch(...){return E_OUTOFMEMORY;}
    }
    StorageSpace Space() {std::lock_guard lock(mutex_);return space_;}
    HRESULT Wait(uint64_t sequence){
        std::unique_lock lock(mutex_);completed_.wait(lock,[&]{return done_>=sequence;});return Result();
    }
};
class Stream final : public IMFByteStream,public IStorage {
    std::atomic<ULONG> references_{1};
    std::shared_ptr<Queue> queue_;
    std::shared_ptr<File> file_;
    std::mutex mutex_;
    QWORD position_=0,length_=0;
    DWORD capabilities_=0;
    bool closed_=false;
    uint64_t sealedSequence_=0;
    HRESULT Seal(bool wait){
        uint64_t sequence=0;
        {
            std::lock_guard lock(mutex_);
            if(closed_)sequence=sealedSequence_;
            else{
                Job job;job.kind=Kind::Close;job.file=file_;
                const auto hr=queue_->Put(std::move(job),&sequence,true);if(FAILED(hr))return hr;
                closed_=true;sealedSequence_=sequence;
            }
        }
        return wait?queue_->Wait(sequence):queue_->Result();
    }
    static HRESULT Notify(IMFAsyncCallback* callback,IUnknown* state,ULONG count,UINT32 kind){
        if(!callback)return E_POINTER;
        ComPtr<IMFAttributes> ticket;HRESULT hr=MFCreateAttributes(&ticket,2);
        if(FAILED(hr)||FAILED(hr=ticket->SetUINT32(kTicketBytes,count))||FAILED(hr=ticket->SetUINT32(kTicketKind,kind)))return hr;
        ComPtr<IMFAsyncResult> result;hr=MFCreateAsyncResult(ticket.Get(),callback,state,&result);
        if(FAILED(hr))return hr;
        result->SetStatus(S_OK);return MFPutWorkItemEx(MFASYNC_CALLBACK_QUEUE_STANDARD,result.Get());
    }
    static HRESULT End(IMFAsyncResult* result,ULONG* count,UINT32 kind){
        if(!result||!count)return E_POINTER;*count=0;
        HRESULT hr=result->GetStatus();if(FAILED(hr))return hr;
        ComPtr<IUnknown> object;ComPtr<IMFAttributes> ticket;UINT32 value=0;
        if(FAILED(hr=result->GetObject(&object))||FAILED(hr=object.As(&ticket))||FAILED(hr=ticket->GetUINT32(kTicketKind,&value)))return hr;
        if(value!=kind)return E_INVALIDARG;
        hr=ticket->GetUINT32(kTicketBytes,&value);if(SUCCEEDED(hr))*count=value;return hr;
    }
public:
    Stream(IMFByteStream* bytes,std::shared_ptr<Queue> queue):queue_(std::move(queue)),file_(std::make_shared<File>()){
        file_->bytes=bytes;
        HRESULT hr=bytes->GetCapabilities(&capabilities_);
        if(SUCCEEDED(hr))hr=bytes->GetCurrentPosition(&position_);
        if(SUCCEEDED(hr))hr=bytes->GetLength(&length_);
        if(FAILED(hr))throw hr;
    }
    STDMETHODIMP QueryInterface(REFIID iid,void** object) override {
        if(!object)return E_POINTER;*object=nullptr;
        if(iid==IID_IUnknown||iid==__uuidof(IMFByteStream))*object=static_cast<IMFByteStream*>(this);
        else if(iid==__uuidof(IStorage))*object=static_cast<IStorage*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override {return ++references_;}
    STDMETHODIMP_(ULONG) Release() override {const auto left=--references_;if(!left)delete this;return left;}
    STDMETHODIMP GetCapabilities(DWORD* value) override {if(!value)return E_POINTER;*value=capabilities_;return S_OK;}
    STDMETHODIMP GetLength(QWORD* value) override {if(!value)return E_POINTER;std::lock_guard lock(mutex_);*value=length_;return queue_->Result();}
    STDMETHODIMP GetCurrentPosition(QWORD* value) override {if(!value)return E_POINTER;std::lock_guard lock(mutex_);*value=position_;return queue_->Result();}
    STDMETHODIMP SetLength(QWORD value) override {
        std::lock_guard lock(mutex_);if(closed_)return MF_E_SHUTDOWN;
        Job job;job.kind=Kind::Resize;job.file=file_;job.position=value;
        const auto hr=queue_->Put(std::move(job));if(SUCCEEDED(hr))length_=value;return hr;
    }
    STDMETHODIMP SetCurrentPosition(QWORD value) override {return Seek(msoBegin,static_cast<LONGLONG>(value),0,nullptr);}
    STDMETHODIMP IsEndOfStream(BOOL* end) override {if(!end)return E_POINTER;std::lock_guard lock(mutex_);*end=position_>=length_;return queue_->Result();}
    STDMETHODIMP Read(BYTE* data,ULONG count,ULONG* read) override {
        if(!read||(!data&&count))return E_POINTER;*read=0;
        std::lock_guard lock(mutex_);if(closed_)return MF_E_SHUTDOWN;
        Job job;job.kind=Kind::Read;job.file=file_;job.position=position_;job.read=data;job.count=count;job.transferred=read;
        uint64_t sequence=0;auto hr=queue_->Put(std::move(job),&sequence,true);
        if(SUCCEEDED(hr))hr=queue_->Wait(sequence);if(SUCCEEDED(hr))position_+=*read;return hr;
    }
    STDMETHODIMP BeginRead(BYTE* data,ULONG count,IMFAsyncCallback* cb,IUnknown* state) override {
        if(!cb)return E_POINTER;ULONG read=0;const auto hr=Read(data,count,&read);return FAILED(hr)?hr:Notify(cb,state,read,0);
    }
    STDMETHODIMP EndRead(IMFAsyncResult* result,ULONG* count) override {return End(result,count,0);}
    STDMETHODIMP Write(const BYTE* data,ULONG count,ULONG* written) override {
        if(!written||(!data&&count))return E_POINTER;*written=0;
        try{
            std::lock_guard lock(mutex_);if(closed_)return MF_E_SHUTDOWN;
            if(count>std::numeric_limits<QWORD>::max()-position_)return E_INVALIDARG;
            const auto available=queue_->CanAccept(count);if(FAILED(available)||!count)return available;
            Job job;job.kind=Kind::Write;job.file=file_;job.position=position_;
            if(count)job.data.assign(data,data+count);
            const auto hr=queue_->Put(std::move(job));
            if(SUCCEEDED(hr)){position_+=count;length_=(std::max)(length_,position_);*written=count;}return hr;
        }catch(...){return E_OUTOFMEMORY;}
    }
    STDMETHODIMP BeginWrite(const BYTE* data,ULONG count,IMFAsyncCallback* cb,IUnknown* state) override {
        if(!cb)return E_POINTER;ULONG written=0;const auto hr=Write(data,count,&written);return FAILED(hr)?hr:Notify(cb,state,written,1);
    }
    STDMETHODIMP EndWrite(IMFAsyncResult* result,ULONG* count) override {return End(result,count,1);}
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin,LONGLONG offset,DWORD flags,QWORD* position) override {
        std::lock_guard lock(mutex_);if(closed_)return MF_E_SHUTDOWN;
        if((origin!=msoBegin&&origin!=msoCurrent)||(flags&~MFBYTESTREAM_SEEK_FLAG_CANCEL_PENDING_IO))return E_INVALIDARG;
        const QWORD base=origin==msoBegin?0:position_;
        if(base>INT64_MAX || offset < -static_cast<LONGLONG>(base) || offset>INT64_MAX-static_cast<LONGLONG>(base))return E_INVALIDARG;
        const auto hr=queue_->Result();if(FAILED(hr))return hr;
        position_=static_cast<QWORD>(static_cast<LONGLONG>(base)+offset);if(position)*position=position_;return S_OK;
    }
    STDMETHODIMP Flush() override {
        uint64_t sequence=0;HRESULT hr;
        {std::lock_guard lock(mutex_);if(closed_)return MF_E_SHUTDOWN;
            Job job;job.kind=Kind::Flush;job.file=file_;hr=queue_->Put(std::move(job),&sequence,true);}
        return FAILED(hr)?hr:queue_->Wait(sequence);
    }
    STDMETHODIMP Close() override {return Seal(true);}
    STDMETHODIMP RequestFlush() override {
        std::lock_guard lock(mutex_);if(closed_)return MF_E_SHUTDOWN;
        if(file_->flushPending.exchange(true))return queue_->Result();
        Job job;job.kind=Kind::Flush;job.file=file_;job.coalescedFlush=true;
        const auto hr=queue_->Put(std::move(job));if(FAILED(hr))file_->flushPending=false;return hr;
    }
    STDMETHODIMP Result() override {return queue_->Result();}
    STDMETHODIMP RequestSpace(const wchar_t* folder) override {return queue_->RequestSpace(folder);}
    StorageSpace STDMETHODCALLTYPE Space() override {return queue_->Space();}
    STDMETHODIMP Retire() override {return Seal(false);}
    STDMETHODIMP Sibling(IMFByteStream* file,IMFByteStream** result) override {
        try{*result=new Stream(file,queue_);return S_OK;}catch(HRESULT hr){return hr;}catch(...){return E_OUTOFMEMORY;}
    }
};
}
HRESULT BufferRecordingStorage(IMFByteStream* file,IMFByteStream* sibling,IMFByteStream** result,StorageLimits limits){
    if(!result)return E_POINTER;*result=nullptr;
    if(!file||!limits.bytes||!limits.operations)return E_INVALIDARG;
    ComPtr<IStorage> storage;
    if(sibling&&SUCCEEDED(sibling->QueryInterface(IID_PPV_ARGS(&storage))))return storage->Sibling(file,result);
    try{*result=new Stream(file,std::make_shared<Queue>(limits));return S_OK;}
    catch(HRESULT hr){return hr;}catch(...){return E_OUTOFMEMORY;}
}
HRESULT RequestRecordingFlush(IMFByteStream* stream){
    if(!stream)return E_POINTER;ComPtr<IStorage> storage;
    return SUCCEEDED(stream->QueryInterface(IID_PPV_ARGS(&storage)))?storage->RequestFlush():stream->Flush();
}
HRESULT RecordingStorageResult(IMFByteStream* stream){
    if(!stream)return E_POINTER;ComPtr<IStorage> storage;
    return SUCCEEDED(stream->QueryInterface(IID_PPV_ARGS(&storage)))?storage->Result():S_OK;
}
HRESULT RequestRecordingSpace(IMFByteStream* stream,const wchar_t* folder){
    if(!stream)return E_POINTER;
    ComPtr<IStorage> storage;
    const auto hr=stream->QueryInterface(IID_PPV_ARGS(&storage));
    return FAILED(hr)?hr:storage->RequestSpace(folder);
}
StorageSpace RecordingStorageSpace(IMFByteStream* stream){
    if(!stream)return {};
    ComPtr<IStorage> storage;
    return SUCCEEDED(stream->QueryInterface(IID_PPV_ARGS(&storage)))?storage->Space():StorageSpace{};
}
#ifdef LLCV_RECORDING_STORAGE_SPACE_TEST
void SetRecordingSpaceQueryForTest(RecordingSpaceQuery query){spaceQueryForTest=query;}
#endif
HRESULT RetireRecordingStorage(IMFByteStream* stream){
    if(!stream)return E_POINTER;ComPtr<IStorage> storage;
    if(SUCCEEDED(stream->QueryInterface(IID_PPV_ARGS(&storage))))return storage->Retire();
    const auto hr=stream->Flush();const auto close=stream->Close();return FAILED(hr)?hr:close;
}
}
