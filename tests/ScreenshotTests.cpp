#include "screenshot/ScreenshotService.h"
#include "HdrScreenshotReference.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace llcv::screenshot;
using Microsoft::WRL::ComPtr;
namespace {
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); }
}
void PutWord(std::vector<std::uint8_t>& raw, std::size_t offset, unsigned value) {
    value <<= 6; raw[offset]=static_cast<std::uint8_t>(value); raw[offset+1]=static_cast<std::uint8_t>(value>>8);
}
std::vector<std::uint8_t> Neutral(Description d, unsigned level) {
    std::vector<std::uint8_t> raw(PackedSize(d));
    if (d.format==Format::P010) {
        for (std::size_t i=0;i<raw.size();i+=2)
            PutWord(raw,i,i<std::size_t(d.width)*d.height*2 ? level : 512);
    } else if (d.format==Format::Nv12) {
        std::fill(raw.begin(),raw.begin()+std::size_t(d.width)*d.height,static_cast<std::uint8_t>(level));
        std::fill(raw.begin()+std::size_t(d.width)*d.height,raw.end(),std::uint8_t{128});
    } else for (std::size_t i=0;i<raw.size();i+=2) {
        raw[i+(d.format==Format::Uyvy ? 1 : 0)]=static_cast<std::uint8_t>(level);
        raw[i+(d.format==Format::Uyvy ? 0 : 1)]=128;
    }
    return raw;
}
std::vector<std::uint8_t> Convert(Description d, const std::vector<std::uint8_t>& raw) {
    std::vector<std::uint8_t> out(std::size_t(d.width)*d.height*4);
    Check(ConvertRows(d,raw,0,d.height,out),"convert frame");
    return out;
}
class GuardedBuffer {
public:
    GuardedBuffer(std::size_t size,bool trailing) : size_(size) {
        SYSTEM_INFO info{}; GetSystemInfo(&info);
        const std::size_t page=info.dwPageSize;
        const std::size_t capacity=((size+page-1)/page)*page;
        base_=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,capacity+2*page,MEM_RESERVE,PAGE_NOACCESS));
        Check(base_!=nullptr,"reserve guarded buffer");
        auto* committed=static_cast<std::uint8_t*>(VirtualAlloc(base_+page,capacity,MEM_COMMIT,PAGE_READWRITE));
        Check(committed!=nullptr,"commit between inaccessible guard pages");
        data_=committed+(trailing ? capacity-size : 0);
    }
    ~GuardedBuffer() { VirtualFree(base_,0,MEM_RELEASE); }
    std::span<std::uint8_t> Bytes() { return {data_,size_}; }
private:
    std::uint8_t* base_=nullptr;
    std::uint8_t* data_=nullptr;
    std::size_t size_;
};
void GuardPages() {
    for (auto format:{Format::Nv12,Format::Yuy2,Format::P010,Format::Uyvy})
    for (bool trailing:{false,true}) for (unsigned width:{2u,32u}) {
        Description desc{width,2,format};
        const auto input=Neutral(desc,format==Format::P010 ? 550 : 100);
        GuardedBuffer source(input.size(),trailing),packed(input.size(),trailing),output(std::size_t(width)*2*4,trailing);
        std::memcpy(source.Bytes().data(),input.data(),input.size());
        Check(CopyFrame(desc,source.Bytes().data(),input.size(),width*(format==Format::Nv12 ? 1 : 2),packed.Bytes()),"guarded capture copy");
        for (bool topLeft:{false,true}) {
            desc.topLeftChroma=topLeft;
            Check(ConvertRows(desc,packed.Bytes(),0,2,output.Bytes()),"guarded first/last chroma rows");
            const auto expected=Convert(desc,input);
            Check(std::equal(expected.begin(),expected.end(),output.Bytes().begin()),"guarded conversion exact");
            Check(!ConvertRows(desc,packed.Bytes(),2,1,output.Bytes()),"guarded out-of-frame row rejected");
            Check(!CopyFrame(desc,source.Bytes().data(),input.size()-1,width*(format==Format::Nv12 ? 1 : 2),packed.Bytes()),"guarded truncated source rejected");
        }
    }
}
void Pixels() {
    Check(!PackedSize({0,1080}),"zero dimension");
    Check(!PackedSize({3842,2160}),"oversize rejected");
    Check(!PackedSize({3,4}),"odd width rejected");
    Check(!PackedSize({4,3}),"odd planar height rejected");
    Check(!PackedSize({4,4,static_cast<Format>(100)}),"unknown format rejected");
    for (auto format : {Format::Nv12,Format::Yuy2,Format::P010,Format::Uyvy}) {
        Description d{4,4,format};
        auto raw=Neutral(d,format==Format::P010 ? 64 : 16);
        const auto black=Convert(d,raw);
        for (std::size_t i=0;i<black.size();++i) Check(black[i]==(i%4==3 ? 255 : 0),"limited black opaque neutral");
        std::vector<std::uint8_t> packed(raw.size());
        const unsigned rowBytes=d.width*(format==Format::Nv12 ? 1 : 2);
        const unsigned rows=static_cast<unsigned>(raw.size()/rowBytes);
        const unsigned stride=rowBytes+7;
        std::vector<std::uint8_t> padded(stride*rows,0xee);
        for (unsigned y=0;y<rows;++y) std::memcpy(padded.data()+y*stride,raw.data()+y*rowBytes,rowBytes);
        Check(CopyFrame(d,padded.data(),padded.size(),stride,packed) && packed==raw,"padded rows copied without padding");
        Check(!CopyFrame(d,padded.data(),(rows-1)*stride+rowBytes-1,stride,packed),"short last row rejected");
        Check(!CopyFrame(d,padded.data(),padded.size(),rowBytes-1,packed),"short stride rejected");
        Check(!CopyFrame(d,nullptr,padded.size(),stride,packed),"null source rejected");
        std::vector<std::uint8_t> shortBuffer(raw.size()-1);
        Check(!CopyFrame(d,padded.data(),padded.size(),stride,shortBuffer),"small target rejected");
        std::vector<std::uint8_t> output(64);
        Check(!ConvertRows(d,shortBuffer,0,4,output),"short conversion input rejected");
        Check(!ConvertRows(d,raw,3,2,output),"rows outside frame rejected");
        Check(!ConvertRows(d,raw,0,4,std::span(output).first(63)),"short BGRA target rejected");
        raw=Neutral(d,format==Format::P010 ? 940 : 235);
        const auto white=Convert(d,raw);
        Check(white[0]>=252 && white[0]==white[1] && white[1]==white[2],"white mapped neutral without wrap");
    }
    for (auto format : {Format::Nv12,Format::Yuy2,Format::Uyvy}) {
        Description d{4,4,format};
        d.color.range=llcv::video_color::Range::Full;
        Check(Convert(d,Neutral(d,0))[0]==0 && Convert(d,Neutral(d,255))[0]==255,"full range black white");
    }
    Description d{4,4,Format::Nv12};
    auto raw=Neutral(d,81);
    for (std::size_t i=16;i<raw.size();i+=2) { raw[i]=90; raw[i+1]=240; }
    d.color.matrix=llcv::video_color::Matrix::Bt601;
    auto color601=Convert(d,raw);
    d.color.matrix=llcv::video_color::Matrix::Bt709;
    auto color709=Convert(d,raw);
    Check(color601[2]>=253 && color601[1]<=1 && color601[0]<=1,"601 limited red reference");
    Check(color601!=color709,"matrix honored");
    d={4,4,Format::P010}; raw=Neutral(d,400);
    // Distinct vertical chroma, enough to distinguish left from top-left.
    PutWord(raw,32,400); PutWord(raw,36,400); PutWord(raw,40,650); PutWord(raw,44,650);
    auto left=Convert(d,raw); d.topLeftChroma=true; auto top=Convert(d,raw);
    Check(left!=top,"HDR resolved chroma placement honored");
    std::vector<std::uint8_t> rows(top.size());
    for (unsigned y=0;y<d.height;++y)
        Check(ConvertRows(d,raw,y,1,std::span(rows).subspan(y*d.width*4,d.width*4)),"row conversion");
    Check(rows==top,"chunked conversion exactly equals whole frame");
    int previous=-1;
    for (unsigned code=64;code<=940;++code) {
        const auto out=Convert(d,Neutral(d,code));
        Check(out[0]>=previous && out[0]==out[1] && out[1]==out[2],"HDR gray ramp monotonic and neutral");
        previous=out[0];
    }
    // Independent double-precision PQ/reference-tone-map oracle (not LUT).
    for (double nits : {0.1,1.0,10.0,100.0,203.0,1000.0,4000.0,10000.0}) {
        const double p=std::pow(nits/10000.0,2610.0/16384.0);
        const double pq=std::pow((3424.0/4096.0+2413.0/128.0*p)/(1+2392.0/128.0*p),2523.0/32.0);
        const unsigned code=static_cast<unsigned>(std::lround(64+876*pq));
        const double a=std::pow((code-64)/876.0,32.0/2523.0);
        const double actual=10000*std::pow(std::max(a-3424.0/4096.0,0.0)/(2413.0/128.0-2392.0/128.0*a),16384.0/2610.0);
        const double mapped=actual<=100 ? actual/203 : 1-10609/(203*(actual+3));
        const int expected=static_cast<int>(std::lround(255*(mapped<=.0031308 ? 12.92*mapped : 1.055*std::pow(mapped,1/2.4)-.055)));
        Check(std::abs(int(Convert(d,Neutral(d,code))[0])-expected)<=1,"PQ LUT + tonemap within one 8-bit code of oracle");
    }
}
void RgbPixels() {
    for (auto format : {Format::Rgb24, Format::Rgb32})
    for (bool topDown : {true, false})
    for (auto range : {llcv::video_color::Range::Full, llcv::video_color::Range::Limited}) {
        Description d{3,3,format}; d.topDown=topDown; d.color.range=range;
        const unsigned bpp=format==Format::Rgb24 ? 3u : 4u, row=d.width*bpp, stride=(row+3)&~3u;
        const size_t required=size_t(stride)*(d.height-1)+row;
        GuardedBuffer source(required,true); std::fill(source.Bytes().begin(),source.Bytes().end(),0xed);
        for (unsigned y=0;y<d.height;++y) for (unsigned x=0;x<d.width;++x) {
            auto* px=source.Bytes().data()+size_t(y)*stride+x*bpp;
            px[0]=static_cast<std::uint8_t>(x*91+y*17); px[1]=static_cast<std::uint8_t>(16+x*50+y*30);
            px[2]=static_cast<std::uint8_t>(235-x*31-y*19); if (bpp==4) px[3]=static_cast<std::uint8_t>(x*63+y*29);
        }
        Check(PackedSize(d)==size_t(row)*d.height,"RGB odd dimensions packed size");
        std::vector<std::uint8_t> packed(PackedSize(d));
        Check(!CopyFrame(d,source.Bytes().data(),required-1,stride,packed),"RGB truncated final row rejected");
        Check(!CopyFrame(d,source.Bytes().data(),required,row-1,packed),"RGB short stride rejected");
        Check(CopyFrame(d,source.Bytes().data(),required,stride,packed),"RGB padded guarded copy");
        for (unsigned y=0;y<d.height;++y) Check(std::memcmp(packed.data()+size_t(y)*row,source.Bytes().data()+size_t(y)*stride,row)==0,"RGB copy preserves source row order");
        const auto bgra=Convert(d,packed);
        const auto expand=[&](unsigned c) { return range==llcv::video_color::Range::Full ? int(c) : std::clamp(int(std::lround((int(c)-16)*255.0/219.0)),0,255); };
        for (unsigned y=0;y<d.height;++y) for (unsigned x=0;x<d.width;++x) {
            const unsigned sy=topDown?y:d.height-1-y;
            const auto* px=source.Bytes().data()+size_t(sy)*stride+x*bpp;
            for (unsigned c=0;c<3;++c) Check(bgra[(size_t(y)*d.width+x)*4+c]==expand(px[c]),"RGB range and orientation");
            Check(bgra[(size_t(y)*d.width+x)*4+3]==255,"RGB opaque alpha ignores X");
        }
        std::vector<std::uint8_t> chunks(bgra.size());
        for (unsigned y=0;y<d.height;++y) Check(ConvertRows(d,packed,y,1,std::span(chunks).subspan(size_t(y)*d.width*4,d.width*4)),"RGB row conversion");
        Check(chunks==bgra,"RGB chunked orientation");
        d.color.matrix=llcv::video_color::Matrix::Bt601;
        Check(Convert(d,packed)==bgra,"RGB ignores YUV matrix");
    }
    Description single{1,1,Format::Rgb24};
    const std::uint8_t one[]{1,2,3}; std::vector<std::uint8_t> packed(3);
    Check(CopyFrame(single,one,3,3,packed),"single row copy avoids zero divisor");
    Check(!PackedSize({UINT_MAX,UINT_MAX,Format::Rgb32}),"RGB oversized dimensions rejected");
}
void UyvyPixels() {
    for (unsigned width : {2u,6u,32u}) for (unsigned height : {2u,6u,18u}) {
        Description yuy2{width,height,Format::Yuy2};
        Description uyvy=yuy2;uyvy.format=Format::Uyvy;
        std::vector<std::uint8_t> yuy2Bytes(PackedSize(yuy2)),uyvyBytes(PackedSize(uyvy));
        Check(uyvyBytes.size()==std::size_t(width)*height*2 && yuy2Bytes.size()==uyvyBytes.size(),
            "UYVY uses exactly one packed 4:2:2 plane");
        for(std::size_t i=0;i<yuy2Bytes.size();i+=4) {
            const auto pair=i/4;
            yuy2Bytes[i]=static_cast<std::uint8_t>((pair*41+16)%256);
            yuy2Bytes[i+1]=static_cast<std::uint8_t>((pair*29+90)%256);
            yuy2Bytes[i+2]=static_cast<std::uint8_t>((pair*67+235)%256);
            yuy2Bytes[i+3]=static_cast<std::uint8_t>((pair*53+240)%256);
            uyvyBytes[i]=yuy2Bytes[i+1];uyvyBytes[i+1]=yuy2Bytes[i];
            uyvyBytes[i+2]=yuy2Bytes[i+3];uyvyBytes[i+3]=yuy2Bytes[i+2];
        }
        for(auto matrix:{llcv::video_color::Matrix::Bt601,llcv::video_color::Matrix::Bt709})
        for(auto range:{llcv::video_color::Range::Limited,llcv::video_color::Range::Full}) {
            yuy2.color.matrix=uyvy.color.matrix=matrix;yuy2.color.range=uyvy.color.range=range;
            const auto expected=Convert(yuy2,yuy2Bytes);
            Check(Convert(uyvy,uyvyBytes)==expected,"UYVY BGRA bit-exact to matching YUY2 for every SDR matrix/range");
            std::vector<std::uint8_t> rows(expected.size());
            for(unsigned y=0;y<height;++y)
                Check(ConvertRows(uyvy,uyvyBytes,y,1,std::span(rows).subspan(std::size_t(y)*width*4,width*4)),
                    "UYVY conversion supports existing worker row chunks");
            Check(rows==expected,"UYVY row chunks preserve pixel-exact YUY2 equivalence");
        }
        const unsigned stride=width*2+11;
        const size_t required=std::size_t(stride)*(height-1)+width*2;
        GuardedBuffer source(required,true),packed(uyvyBytes.size(),true);
        for(unsigned y=0;y<height;++y)
            std::memcpy(source.Bytes().data()+std::size_t(y)*stride,uyvyBytes.data()+std::size_t(y)*width*2,width*2);
        Check(CopyFrame(uyvy,source.Bytes().data(),required,stride,packed.Bytes()),"UYVY padded last row guard exact copy");
        Check(std::equal(uyvyBytes.begin(),uyvyBytes.end(),packed.Bytes().begin()),"UYVY padded copy preserves source byte order");
        Check(!CopyFrame(uyvy,source.Bytes().data(),required-1,stride,packed.Bytes()),"UYVY truncated last row rejected without reading guard");
    }
}
std::vector<std::uint8_t> ReadPng(const std::wstring& path, unsigned w, unsigned h) {
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
    Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))),"WIC factory");
    Check(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)),"read saved PNG");
    Check(SUCCEEDED(decoder->GetFrame(0,&frame)),"read PNG frame");
    UINT width=0,height=0; Check(SUCCEEDED(frame->GetSize(&width,&height)) && width==w && height==h,"source resolution retained");
    ComPtr<IWICMetadataQueryReader> reader;
    Check(SUCCEEDED(frame->GetMetadataQueryReader(&reader)),"read PNG metadata");
    PROPVARIANT tag{};
    Check(SUCCEEDED(reader->GetMetadataByName(L"/sRGB/RenderingIntent",&tag)) && tag.vt==VT_UI1,"PNG explicitly sRGB");
    PropVariantClear(&tag);
    Check(SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)),"PNG BGRA decoder");
    std::vector<std::uint8_t> out(std::size_t(w)*h*4);
    Check(SUCCEEDED(converter->CopyPixels(nullptr,w*4,static_cast<UINT>(out.size()),out.data())),"PNG pixels");
    return out;
}
Result Await(Service& service) {
    Result result;
    const auto deadline=GetTickCount64()+15000;
    while (GetTickCount64()<deadline) {
        if (service.TakeResult(result)) return result;
        Sleep(5);
    }
    Check(false,"worker completion deadline"); return result;
}
void Native(const std::wstring& dir) {
    std::atomic<bool> cancel{false};
    Description desc{32,18,Format::Nv12}; auto raw=Neutral(desc,100); auto bgra=Convert(desc,raw);
    const auto file=dir+L"\\roundtrip.png";
    Check(SUCCEEDED(WritePng(file,32,18,bgra,cancel,0)),"native PNG encode");
    Check(ReadPng(file,32,18)==bgra,"PNG is pixel-exact BGRA");
    auto different=bgra; different[0]^=0x55;
    Check(FAILED(WritePng(file,32,18,different,cancel,0)) && ReadPng(file,32,18)==bgra,"never overwrite existing screenshot");
    Check(!std::filesystem::exists(file+L".partial"),"failed encode leaves no partial file");
    cancel.store(true);
    Check(WritePng(dir+L"\\cancel.png",32,18,bgra,cancel,0)==HRESULT_FROM_WIN32(ERROR_CANCELLED),"cancel before encode");
    Check(!std::filesystem::exists(dir+L"\\cancel.png"),"cancel leaves no PNG");
    cancel.store(false);
    Check(FAILED(WritePng(dir+L"\\missing\\output.png",32,18,bgra,cancel,0)),"missing directory failure handled");
    std::thread stopper([&] { Sleep(25); cancel.store(true); });
    const auto interrupted=dir+L"\\interrupted.png";
    const HRESULT interruptedResult=WritePng(interrupted,32,18,bgra,cancel,20);
    stopper.join();
    Check(interruptedResult==HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
        !std::filesystem::exists(interrupted) && !std::filesystem::exists(interrupted+L".partial"),
        "cancel during encoding removes only private partial file");
    cancel.store(false);
    Service service;
    Check(!service.Request(false),"not ready before start");
    for (bool topLeft : {false,true}) {
        Description colored{32,18,Format::P010}; colored.topLeftChroma=topLeft;
        auto input=HdrScreenshotPattern(colored); const auto expected=Convert(colored,input);
        Check(service.Start(32,18,Format::P010,dir) && service.Request(false),"colored HDR worker start");
        service.Submit(colored,input.data(),input.size(),64);
        std::fill(input.begin(),input.end(),std::uint8_t{0xdd});
        const auto result=Await(service);
        Check(result.fileResult==S_OK && result.toneMapped,"colored HDR worker completion");
        Check(ReadPng(result.path,32,18)==expected,"colored HDR PNG pixel-exact with resolved siting");
        service.Stop();
    }
    for (auto format : {Format::Rgb24,Format::Rgb32}) for (bool topDown : {true,false}) {
        Description rgb{3,3,format}; rgb.topDown=topDown; rgb.color.range=llcv::video_color::Range::Full;
        const unsigned bpp=format==Format::Rgb24?3u:4u, stride=(rgb.width*bpp+3)&~3u;
        std::vector<std::uint8_t> input(size_t(stride)*rgb.height,0xee), packed(PackedSize(rgb));
        for (unsigned y=0;y<rgb.height;++y) for (unsigned x=0;x<rgb.width;++x) {
            auto* px=input.data()+size_t(y)*stride+x*bpp; px[0]=BYTE(10+y*40); px[1]=BYTE(20+x*30); px[2]=BYTE(30+y*20); if(bpp==4) px[3]=BYTE(x+y);
        }
        Check(CopyFrame(rgb,input.data(),input.size(),stride,packed),"RGB worker expected copy");
        const auto expected=Convert(rgb,packed);
        Check(service.Start(rgb.width,rgb.height,format,dir,topDown) && service.Request(false),"RGB worker start");
        auto wrong=rgb; wrong.topDown=!topDown;
        service.Submit(wrong,input.data(),input.size(),stride);
        Check(!service.Request(false),"RGB mismatched orientation cannot consume request");
        service.Submit(rgb,input.data(),input.size(),stride);
        std::fill(input.begin(),input.end(),0xdd);
        const auto result=Await(service);
        Check(result.fileResult==S_OK && !result.toneMapped && ReadPng(result.path,rgb.width,rgb.height)==expected,"RGB worker PNG matches immutable snapshot");
        service.Stop();
    }
    for (auto format : {Format::Nv12,Format::Yuy2,Format::P010,Format::Uyvy}) {
        desc.format=format; raw=Neutral(desc,format==Format::P010 ? 550 : 100); bgra=Convert(desc,raw);
        Check(service.Start(32,18,format,dir),"start screenshot worker");
        Check(service.Request(false) && !service.Request(false),"single pending request");
        service.Submit(desc,raw.data(),1,32*(format==Format::Nv12 ? 1 : 2));
        Check(!service.Request(false),"short sample cannot consume request");
        service.Submit(desc,raw.data(),raw.size(),32*(format==Format::Nv12 ? 1 : 2));
        std::fill(raw.begin(),raw.end(),std::uint8_t{0xdd}); // Simulate immediate driver reuse.
        Check(!service.Request(false),"no backlog while encoding");
        auto result=Await(service);
        Check(result.fileResult==S_OK && !result.clipboardRequested,"worker saved without clipboard access");
        Check(result.toneMapped==(format==Format::P010),"HDR status explicit");
        Check(ReadPng(result.path,32,18)==bgra,"worker uses owned input after driver release");
        Check(service.Request(false),"next request accepted after result consumed");
        const auto start=GetTickCount64(); service.Stop();
        Check(GetTickCount64()-start<1000 && !service.Request(false),"stop pending request bounded");
    }
    Check(service.Start(32,18,Format::Nv12,dir) && service.Request(false),"timeout start");
    Check(!service.ExpireRequest(),"fresh request not timed out");
    Sleep(3020);
    Check(service.ExpireRequest() && service.Request(false),"no-video timeout reopens slot");
    service.Stop();
    Check(!service.Start(4,3,Format::Nv12,dir) && !service.Request(false),"invalid dimensions leave unavailable");
    desc={32,18,Format::Nv12}; raw=Neutral(desc,100);
    Check(service.Start(32,18,Format::Nv12,file) && service.Request(false),"worker I/O failure setup");
    service.Submit(desc,raw.data(),raw.size(),32);
    Check(FAILED(Await(service).fileResult) && service.Request(false),"failed save releases request slot");
    service.Stop();
    // Large HDR conversion cancellation: shutdown never waits for the entire
    // frame's tone mapping or all PNG rows to complete.
    desc={3840,2160,Format::P010}; raw=Neutral(desc,550);
    Check(service.Start(desc.width,desc.height,desc.format,dir) && service.Request(false),"4K HDR start");
    service.Submit(desc,raw.data(),raw.size(),desc.width*2);
    service.CancelPending();
    Check(!service.Request(false),"UI close immediately blocks new work without joining");
    const auto start=GetTickCount64(); service.Stop();
    Check(GetTickCount64()-start<1500,"cancel active 4K work promptly");
    for (auto& item : std::filesystem::directory_iterator(dir))
        Check(item.path().extension()!=L".partial","no abandoned partials");
}
void Clipboard() {
    std::atomic<bool> cancel{false};
    Description desc{32,18,Format::P010}; auto bgra=Convert(desc,HdrScreenshotPattern(desc));
    Check(PublishClipboard(bgra,32,18,cancel)==S_OK,"real clipboard publish");
    // Clipboard listeners can briefly open the newly published image. That
    // is not a publication failure; use the same bounded retry discipline
    // for this independent reader instead of racing them with one attempt.
    bool opened=false;
    const auto readDeadline=GetTickCount64()+500;
    do { opened=OpenClipboard(nullptr)!=FALSE; if (!opened) Sleep(5); }
    while (!opened && GetTickCount64()<readDeadline);
    Check(opened,"read own clipboard image");
    const auto data=GetClipboardData(CF_DIBV5); Check(data!=nullptr,"DIBV5 present");
    const auto* header=static_cast<const BITMAPV5HEADER*>(GlobalLock(data));
    Check(header && header->bV5Width==32 && header->bV5Height==-18 && header->bV5CSType==LCS_sRGB,"clipboard top-down SDR header");
    Check(std::memcmp(header+1,bgra.data(),bgra.size())==0,"clipboard pixels exactly match converted PNG input");
    GlobalUnlock(data);
    HRESULT blocked=S_OK;
    const auto started=GetTickCount64();
    std::thread worker([&] { blocked=PublishClipboard(bgra,32,18,cancel); });
    worker.join(); CloseClipboard();
    Check(FAILED(blocked) && GetTickCount64()-started<1000,"clipboard contention bounded without deleting held data");
    cancel.store(true);
    Check(PublishClipboard(bgra,32,18,cancel)==HRESULT_FROM_WIN32(ERROR_CANCELLED),"cancel clipboard without touching current image");
}
void ConcurrentLifecycle(const std::wstring& dir) {
    Service service;
    std::atomic<bool> finished{false};
    std::thread ui([&] {
        while (!finished.load()) {
            Result result; service.TakeResult(result);
            service.Request(false);
            Sleep(1);
        }
    });
    for (unsigned trial=0;trial<30;++trial) {
        const Format format=trial%2 ? Format::P010 : Format::Nv12;
        Description desc{32,18,format};
        auto raw=Neutral(desc,format==Format::P010 ? 550 : 100);
        Check(service.Start(desc.width,desc.height,format,dir),"concurrent lifecycle start");
        for (unsigned sample=0;sample<6;++sample) {
            service.Submit(desc,raw.data(),raw.size(),desc.width*(format==Format::Nv12 ? 1 : 2));
            Sleep(1);
        }
        service.CancelPending(); service.Stop();
    }
    finished.store(true); ui.join(); service.Stop();
    Check(!service.Request(false),"concurrent lifecycle stops unavailable");
    for (const auto& file:std::filesystem::directory_iterator(dir))
        Check(file.path().extension()!=L".partial","concurrent cancellation cleans temporary PNGs");
}
void Benchmark(const std::wstring& dir) {
    for (auto format : {Format::Nv12,Format::P010}) {
        Description desc{3840,2160,format};
        auto raw=Neutral(desc,format==Format::P010 ? 550 : 100);
        for (unsigned y=0;y<desc.height;++y) for (unsigned x=0;x<desc.width;++x) {
            const auto at=std::size_t(y)*desc.width+x;
            if (format==Format::P010) PutWord(raw,at*2,64+(x*876/desc.width+y)%877);
            else raw[at]=static_cast<std::uint8_t>(16+(x*219/desc.width+y)%220);
        }
        Service service;
        Check(service.Start(desc.width,desc.height,format,dir),"benchmark start");
        for (unsigned trial=0;trial<3;++trial) {
            Check(service.Request(false),"benchmark request");
            const auto begin=GetTickCount64();
            service.Submit(desc,raw.data(),raw.size(),desc.width*(format==Format::P010 ? 2 : 1));
            auto result=Await(service);
            Check(result.fileResult==S_OK,"benchmark PNG completed");
            std::printf("4K %s trial=%u copy=%.4f ms worker+save=%llu ms bytes=%llu\n",
                format==Format::P010 ? "HDR-to-SDR" : "NV12",trial+1,result.copyMs,
                static_cast<unsigned long long>(GetTickCount64()-begin),
                static_cast<unsigned long long>(std::filesystem::file_size(result.path)));
        }
        service.Stop();
    }
}
}

int wmain(int argc, wchar_t** argv) {
    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"COM initialization");
    wchar_t temp[MAX_PATH]{}; Check(GetTempPathW(MAX_PATH,temp)>0,"temp directory");
    GUID guid{}; Check(SUCCEEDED(CoCreateGuid(&guid)),"test UUID");
    wchar_t id[40]{}; StringFromGUID2(guid,id,40);
    const auto dir=std::wstring(temp)+L"llcv-screenshot-test-"+id;
    Check(std::filesystem::create_directory(dir),"isolated test folder");
    Pixels(); GuardPages(); RgbPixels(); UyvyPixels(); HdrScreenshotReferenceTests(); Native(dir); ConcurrentLifecycle(dir);
    if (argc>1 && std::wcscmp(argv[1],L"--clipboard")==0) Clipboard();
    if (argc>1 && std::wcscmp(argv[1],L"--benchmark")==0) Benchmark(dir);
    // Only exact files produced in this unique test folder are removed.
    for (auto& file : std::filesystem::directory_iterator(dir)) std::filesystem::remove(file.path());
    std::filesystem::remove(dir);
    CoUninitialize();
    std::puts("Screenshot conversion, PNG, ownership, cancellation and lifecycle tests passed.");
}
