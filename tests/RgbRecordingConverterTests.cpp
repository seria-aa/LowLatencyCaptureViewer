#include "recording/RgbRecordingConverter.h"
#include <cstdio>
#include <chrono>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr){if(FAILED(hr))throw hr;}
static void Require(bool ok){if(!ok)throw E_FAIL;}
int main(){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);MFStartup(MF_VERSION);
    try{
        for(int step:{3,4})for(bool topDown:{false,true})for(bool limited:{false,true}){
            constexpr int w=18,h=8;std::vector<BYTE> source(w*h*step,0xff);
            for(int y=0;y<h;++y)for(int x=0;x<w;++x){
                auto* p=source.data()+((topDown?y:h-1-y)*w+x)*step;
                for(int c=0;c<3;++c){const int n=(x*29+y*47+c*83)%256;p[c]=static_cast<BYTE>(limited?16+(n*219+127)/255:n);}
                if(step==4)p[3]=static_cast<BYTE>(x+y);
            }
            ComPtr<IMFMediaBuffer> input;Check(MFCreateMemoryBuffer(static_cast<DWORD>(source.size()),&input));
            BYTE* p=nullptr;Check(input->Lock(&p,nullptr,nullptr));memcpy(p,source.data(),source.size());Check(input->Unlock());
            Check(input->SetCurrentLength(static_cast<DWORD>(source.size())));
            llcv::recording::RgbRecordingConverter converter;converter.Configure(w,h,step,topDown,limited);
            for(int repeat=0;repeat<20;++repeat){
                ComPtr<IMFMediaBuffer> output;Check(converter.Apply(input.Get(),&output));
                DWORD length=0;Check(output->Lock(&p,nullptr,&length));Require(length==w*h*3/2);
                const auto rgb=[&](int x,int y,int c){const double n=source[((topDown?y:h-1-y)*w+x)*step+2-c];return limited?std::clamp((n-16)*255/219,0.0,255.0):n;};
                for(int y=0;y<h;y+=2)for(int x=0;x<w;x+=2){
                    double u=0,v=0;
                    for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){
                        const double rr=rgb(x+dx,y+dy,0),g=rgb(x+dx,y+dy,1),b=rgb(x+dx,y+dy,2);
                        const double lum=.2126*rr+.7152*g+.0722*b;
                        Require(std::abs(p[(y+dy)*w+x+dx]-(16+std::lround(lum*219/255)))<=1);
                        u+=(b-lum)/(2*(1-.0722))*224/255;
                        v+=(rr-lum)/(2*(1-.2126))*224/255;
                    }
                    const int uv=w*h+y/2*w+x;
                    Require(std::abs(p[uv]-(128+std::lround(u/4)))<=1);
                    Require(std::abs(p[uv+1]-(128+std::lround(v/4)))<=1);
                }
                Check(output->Unlock());
            }
            Check(input->SetCurrentLength(3));ComPtr<IMFMediaBuffer> bad;
            Require(FAILED(converter.Apply(input.Get(),&bad)) && !bad);
        }
        for(int step:{3,4})for(bool topDown:{false,true}){
            ComPtr<IMFMediaBuffer> input;Check(MFCreateMemoryBuffer(16*step,&input));
            BYTE* p=nullptr;Check(input->Lock(&p,nullptr,nullptr));
            for(int y=0;y<4;++y)for(int x=0;x<4;++x){
                auto* pixel=p+((topDown?y:3-y)*4+x)*step;
                pixel[0]=pixel[1]=pixel[2]=static_cast<BYTE>(y*60+x*8);if(step==4)pixel[3]=0;
            }
            Check(input->Unlock());Check(input->SetCurrentLength(16*step));
            llcv::recording::RgbRecordingConverter converter;converter.Configure(4,4,step,topDown,false,2,2);
            ComPtr<IMFMediaBuffer> output;Check(converter.Apply(input.Get(),&output));
            Check(output->Lock(&p,nullptr,nullptr));
            const int expected[]={34,50,154,170};
            for(int i=0;i<4;++i)Require(std::abs(p[i]-(16+std::lround(expected[i]*219.0/255)))<=1);
            Require(p[4]==128 && p[5]==128);Check(output->Unlock());
        }
        for(int step:{3,4})for(bool topDown:{false,true})for(bool limited:{false,true}){
            constexpr int w=36,h=16;ComPtr<IMFMediaBuffer> input;Check(MFCreateMemoryBuffer(w*h*step,&input));
            BYTE* p=nullptr;Check(input->Lock(&p,nullptr,nullptr));
            for(int i=0;i<w*h*step;++i)p[i]=static_cast<BYTE>(i*37+i/19);
            Check(input->Unlock());Check(input->SetCurrentLength(w*h*step));
            for(bool scale:{false,true}){
                llcv::recording::RgbRecordingConverter fast,scalar;
                fast.Configure(w,h,step,topDown,limited,scale?w/2:w,scale?h/2:h);
                scalar.Configure(w,h,step,topDown,limited,scale?w/2:w,scale?h/2:h,false);
                std::atomic<bool> stopped{false}; llcv::recording::RgbPreflight timing;
                Check(fast.Preflight(input.Get(),60,stopped,timing));
                Require(timing.samples==6 && timing.meanMs>=0 && timing.maximumMs>=timing.meanMs &&
                    std::abs(timing.budgetMs-1000.0/60)<1e-9);
                stopped=true;
                Require(fast.Preflight(input.Get(),60,stopped,timing)==HRESULT_FROM_WIN32(ERROR_CANCELLED));
                Require(fast.Preflight(input.Get(),0,stopped,timing)==E_INVALIDARG);
                // Preflight/cancellation must not alter pixels or retain output leases.
                std::vector<ComPtr<IMFMediaBuffer>> leases(16);
                for(auto& lease:leases)Check(fast.Apply(input.Get(),&lease));
                leases.clear();
                ComPtr<IMFMediaBuffer> a,b;Check(fast.Apply(input.Get(),&a));Check(scalar.Apply(input.Get(),&b));
                BYTE *ap=nullptr,*bp=nullptr;DWORD an=0,bn=0;Check(a->Lock(&ap,nullptr,&an));Check(b->Lock(&bp,nullptr,&bn));
                Require(an==bn);for(DWORD i=0;i<an;++i)Require(std::abs(int(ap[i])-bp[i])<=1);
                Check(a->Unlock());Check(b->Unlock());
            }
        }
        for(int w:{1920,3840}){
            const int h=w==1920?1080:2160;ComPtr<IMFMediaBuffer> input;
            Check(MFCreateMemoryBuffer(w*h*4,&input));BYTE* p=nullptr;Check(input->Lock(&p,nullptr,nullptr));
            memset(p,127,w*h*4);Check(input->Unlock());Check(input->SetCurrentLength(w*h*4));
            llcv::recording::RgbRecordingConverter converter;converter.Configure(w,h,4,true,false);
            const auto start=std::chrono::steady_clock::now();
            for(int i=0;i<20;++i){ComPtr<IMFMediaBuffer> out;Check(converter.Apply(input.Get(),&out));}
            printf("RGB32 %dx%d worker conversion mean %.3f ms\n",w,h,
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/20);
            if(w==3840){
                converter.Configure(w,h,4,true,false,1920,1080);
                const auto scaledStart=std::chrono::steady_clock::now();
                for(int i=0;i<20;++i){ComPtr<IMFMediaBuffer> out;Check(converter.Apply(input.Get(),&out));}
                printf("RGB32 4K -> 1080p fused conversion mean %.3f ms\n",
                    std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-scaledStart).count()/20);
            }
        }
        puts("PASS RGB recording conversion: independent BT.709 reference, orientation, range, X, short buffers, repeated leases.");
    }catch(HRESULT hr){printf("FAIL %08X\n",static_cast<unsigned>(hr));MFShutdown();CoUninitialize();return 1;}
    MFShutdown();CoUninitialize();return 0;
}
