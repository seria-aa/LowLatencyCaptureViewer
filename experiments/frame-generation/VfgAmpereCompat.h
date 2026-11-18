// SPDX-License-Identifier: GPL-3.0-only
// Native diagnostic port, 2026-11-17, of Veyra-NRVideo VfgAmpereCompat.cpp
// and the LLCV probe. See third_party/frame-generation/VFG-RTX30-NOTICE.txt.
// Exact-runtime-only, process-local VFG imports. Never patches a file/driver.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <span>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace vfg_compat_detail {
inline bool SafeCopy(void* dst,const void* src,size_t bytes)noexcept {
    __try {std::memcpy(dst,src,bytes);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
inline bool Fits(size_t p,size_t n,size_t size){return p<=size&&n<=size-p;}
template<class T> T Get(std::span<const std::byte> data,size_t p) {
    if(!Fits(p,sizeof(T),data.size()))throw std::runtime_error("VFG image bounds");
    T value{};std::memcpy(&value,data.data()+p,sizeof(T));return value;
}
template<class T> void Put(std::vector<std::byte>& data,size_t p,T value){std::memcpy(data.data()+p,&value,sizeof(T));}
inline std::string String(std::span<const std::byte> data,size_t p) {
    if(!Fits(p,1,data.size()))throw std::runtime_error("VFG import name bounds");
    const char* start=reinterpret_cast<const char*>(data.data()+p);
    const char* end=static_cast<const char*>(std::memchr(start,0,(std::min)(size_t(256),data.size()-p)));
    if(!end)throw std::runtime_error("VFG unterminated import");return {start,end};
}
inline std::string Hash(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    std::ifstream file(path,std::ios::binary);if(!file)throw std::runtime_error("VFG runtime identity read");
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("VFG SHA256 provider");
    try {
        if(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0)throw std::runtime_error("VFG SHA256 create");
        std::array<unsigned char,65536> bytes{};
        while(file){file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());const auto count=file.gcount();if(count&&BCryptHashData(hash,bytes.data(),ULONG(count),0)<0)throw std::runtime_error("VFG SHA256 update");}
        if(!file.eof())throw std::runtime_error("VFG SHA256 short read");
        std::array<unsigned char,32> digest{};if(BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0)<0)throw std::runtime_error("VFG SHA256 finish");
        std::string result;for(auto b:digest){char pair[3]{};sprintf_s(pair,"%02X",b);result+=pair;}
        BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);return result;
    }catch(...){if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);throw;}
}
}

class VfgAmpereCompat {
    using Attribute=int(__stdcall*)(int*,int,int);
    using Load=int(__stdcall*)(void**,const void*);
    using ContextDevice=int(__stdcall*)(int*);
    inline static std::atomic<VfgAmpereCompat*> active_=nullptr;
    Attribute attribute_=nullptr;Load load_=nullptr;ContextDevice context_=nullptr;
    int device_=-1;
    struct Slot {void** pointer=nullptr;void* original=nullptr;void* hook=nullptr;bool changed=false;};
    std::array<Slot,2> slots_{};
    std::atomic<unsigned> loads_=0,spoofs_=0,rejected_=0;
    static int __stdcall AttributeHook(int* value,int attribute,int device) {
        auto* self=active_.load();if(!self)return 3;
        const int status=self->attribute_(value,attribute,device);
        if(!status&&value&&attribute==76&&device==self->device_&&*value==6){*value=9;++self->spoofs_;}return status;
    }
    static int __stdcall LoadHook(void** module,const void* image) {
        using namespace vfg_compat_detail;
        auto* self=active_.load();if(!self)return 3;
        int device=-1;if(self->context_(&device)||device!=self->device_||!image)return self->load_(module,image);
        uint32_t magic=0;if(!SafeCopy(&magic,image,4)){++self->rejected_;return 200;}
        if(magic!=0xBA55ED50)return self->load_(module,image);
        try {
            std::array<std::byte,16> header{};if(!SafeCopy(header.data(),image,header.size()))throw std::runtime_error("header unreadable");
            const auto payload=Get<uint64_t>(header,8);if(payload>32u*1024u*1024u-16)throw std::runtime_error("payload bound");
            std::vector<std::byte> original(size_t(payload)+16);if(!SafeCopy(original.data(),image,original.size()))throw std::runtime_error("image unreadable");
            unsigned changed=0;auto copy=Retarget(original,changed);
            if(!changed)return self->load_(module,image);
            const int status=self->load_(module,copy.data());++self->loads_;return status;
        }catch(...){++self->rejected_;return 200;}
    }
    static bool Change(Slot& slot,bool restore)noexcept {
        DWORD protection=0;if(!VirtualProtect(slot.pointer,sizeof(void*),PAGE_READWRITE,&protection))return false;
        void* expected=restore?slot.hook:slot.original;void* desired=restore?slot.original:slot.hook;
        const bool changed=InterlockedCompareExchangePointer(slot.pointer,desired,expected)==expected;
        if(changed)slot.changed=!restore;
        DWORD ignored=0;const bool protectedAgain=VirtualProtect(slot.pointer,sizeof(void*),protection,&ignored)!=FALSE;
        return changed&&protectedAgain&&*slot.pointer==desired;
    }
public:
    static std::vector<std::byte> Retarget(std::span<const std::byte> input,unsigned& changed) {
        using namespace vfg_compat_detail;changed=0;
        if(input.size()<16||input.size()>32u*1024u*1024u||Get<uint32_t>(input,0)!=0xBA55ED50||Get<uint16_t>(input,4)!=1||Get<uint16_t>(input,6)!=16||Get<uint64_t>(input,8)!=input.size()-16)throw std::runtime_error("VFG unknown fatbin");
        std::vector<std::byte> copy(input.begin(),input.end());size_t p=16;unsigned entries=0;
        while(p<input.size()) {
            if(++entries>4096||!Fits(p,64,input.size()))throw std::runtime_error("VFG truncated entry");
            const auto kind=Get<uint16_t>(input,p),version=Get<uint16_t>(input,p+2);const auto header=Get<uint32_t>(input,p+4);const auto bytes=Get<uint64_t>(input,p+8);
            if((kind!=1&&kind!=2)||version!=0x101||header<64||header>4096||header%8||!Fits(p,header,input.size())||!bytes||bytes>input.size()-p-header)throw std::runtime_error("VFG invalid entry");
            const size_t payload=p+header;
            if(Get<uint32_t>(input,p+28)==89) {
                if(kind!=2||header!=64||bytes<64||Get<uint32_t>(input,payload)!=0x464c457f||Get<uint8_t>(input,payload+4)!=2||Get<uint8_t>(input,payload+5)!=1||Get<uint16_t>(input,payload+18)!=190||Get<uint32_t>(input,payload+48)!=0x00590559)throw std::runtime_error("VFG unrecognized CUDA ELF");
                Put<uint32_t>(copy,p+28,86);Put<uint32_t>(copy,payload+48,0x00560556);++changed;
            }
            p=payload+size_t(bytes);
        }
        if(!entries)throw std::runtime_error("VFG empty fatbin");return copy;
    }
    ~VfgAmpereCompat(){Reset();}
    void Install(HMODULE module,int device,const std::filesystem::path& path) {
        using namespace vfg_compat_detail;
        if(std::filesystem::file_size(path)!=207321712||Hash(path)!="270CF4FFF9329908F9770D306CB51B910EE04323AF5520CD02A98A2E22ABA795")throw std::runtime_error("VFG SM86 runtime identity mismatch");
        auto driver=GetModuleHandleW(L"nvcuda.dll");if(!driver)throw std::runtime_error("VFG CUDA module missing");
        attribute_=reinterpret_cast<Attribute>(GetProcAddress(driver,"cuDeviceGetAttribute"));load_=reinterpret_cast<Load>(GetProcAddress(driver,"cuModuleLoadData"));context_=reinterpret_cast<ContextDevice>(GetProcAddress(driver,"cuCtxGetDevice"));
        if(!attribute_||!load_||!context_)throw std::runtime_error("VFG CUDA exports missing");
        int major=0,minor=0;if(attribute_(&major,75,device)||attribute_(&minor,76,device)||major!=8||minor!=6)throw std::runtime_error("VFG compatibility is SM86 only");device_=device;
        auto* base=reinterpret_cast<const std::byte*>(module);IMAGE_DOS_HEADER dos{};if(!SafeCopy(&dos,base,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||dos.e_lfanew>1024*1024)throw std::runtime_error("VFG invalid DOS header");
        IMAGE_NT_HEADERS64 nt{};if(!SafeCopy(&nt,base+dos.e_lfanew,sizeof(nt))||nt.Signature!=IMAGE_NT_SIGNATURE||nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC||nt.OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_IMPORT)throw std::runtime_error("VFG invalid NT header");
        std::span<const std::byte> data(base,nt.OptionalHeader.SizeOfImage);const auto dir=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if(dir.Size<sizeof(IMAGE_IMPORT_DESCRIPTOR)||!Fits(dir.VirtualAddress,dir.Size,data.size()))throw std::runtime_error("VFG import directory bounds");
        std::array<size_t,2> imports{};bool terminated=false;
        for(size_t p=0;Fits(p,sizeof(IMAGE_IMPORT_DESCRIPTOR),dir.Size);p+=sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            const auto entry=Get<IMAGE_IMPORT_DESCRIPTOR>(data,dir.VirtualAddress+p);
            if(!entry.Name&&!entry.FirstThunk&&!entry.OriginalFirstThunk){terminated=true;break;}
            const auto name=String(data,entry.Name);if(_stricmp(name.c_str(),"nvcuda.dll"))continue;
            if(!entry.OriginalFirstThunk||!entry.FirstThunk||entry.FirstThunk%8)throw std::runtime_error("VFG named imports absent");
            bool end=false;
            for(size_t i=0;i<65536;++i) {
                const auto value=Get<uint64_t>(data,size_t(entry.OriginalFirstThunk)+i*8);if(!value){end=true;break;}if(value&IMAGE_ORDINAL_FLAG64)continue;
                if(value>UINT32_MAX)throw std::runtime_error("VFG import name RVA");const auto function=String(data,size_t(value)+2);
                const int index=function=="cuDeviceGetAttribute"?0:function=="cuModuleLoadData"?1:-1;if(index<0)continue;
                const size_t at=size_t(entry.FirstThunk)+i*8;if(imports[index]||!Fits(at,8,data.size()))throw std::runtime_error("VFG ambiguous import");imports[index]=at;
            }
            if(!end)throw std::runtime_error("VFG unterminated thunks");
        }
        if(!terminated||!imports[0]||!imports[1])throw std::runtime_error("VFG CUDA imports unavailable");
        const std::array<void*,2> original{reinterpret_cast<void*>(attribute_),reinterpret_cast<void*>(load_)};
        const std::array<void*,2> hooks{reinterpret_cast<void*>(AttributeHook),reinterpret_cast<void*>(LoadHook)};
        for(size_t i=0;i<2;++i){slots_[i]={reinterpret_cast<void**>(const_cast<std::byte*>(base)+imports[i]),original[i],hooks[i],false};if(*slots_[i].pointer!=original[i])throw std::runtime_error("VFG imports already redirected");}
        VfgAmpereCompat* expected=nullptr;if(!active_.compare_exchange_strong(expected,this))throw std::runtime_error("VFG compatibility already active");
        for(auto& slot:slots_)if(!Change(slot,false)){Reset();throw std::runtime_error("VFG import installation failed");}
        puts("VFG SM86 experimental compatibility: process-local imports/private kernel copies only.");
    }
    void Reset()noexcept {
        if(active_.load()!=this)return;
        for(auto it=slots_.rbegin();it!=slots_.rend();++it)if(it->changed&&!Change(*it,true)) {
            // Do not let a callback outlive this object on uncertain restoration.
            fputs("VFG import restoration failed; terminating isolated preview.\n",stderr);TerminateProcess(GetCurrentProcess(),3);
        }
        active_=nullptr;printf("vfg_compat_restored=1 private_loads=%u attribute_overrides=%u rejected_loads=%u\n",loads_.load(),spoofs_.load(),rejected_.load());
    }
};
