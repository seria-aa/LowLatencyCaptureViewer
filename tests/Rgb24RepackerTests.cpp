#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "../src/video/Rgb24Repacker.h"
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
static void Check(HRESULT hr) { if (FAILED(hr)) { char b[64]; sprintf_s(b,"HRESULT 0x%08X",unsigned(hr)); throw std::runtime_error(b); } }
static void Require(bool b, const char* s) { if (!b) throw std::runtime_error(s); }
int main(int argc, char**) {
 try {
    Ptr<ID3D11Device> d; Ptr<ID3D11DeviceContext> c;
    Check(D3D11CreateDevice(nullptr, argc > 1 ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c));
    llcv::video::Rgb24Repacker rep; Check(rep.Initialize(d.Get()));
    Ptr<ID3D11Texture2D> bad; Ptr<ID3D11ShaderResourceView> badView;
    Require(rep.CreateRawTexture(5462,1,&bad,&badView)==E_INVALIDARG,"oversize rejected");
    for (auto dims : {std::pair<UINT,UINT>{1,1},{3,5},{257,7},{3840,2160}}) {
      const UINT w=dims.first,h=dims.second,pitch=(3*w+3)&~3u;
      Ptr<ID3D11Texture2D> raw[3], out[3], staging; Ptr<ID3D11ShaderResourceView> srv[3]; Ptr<ID3D11UnorderedAccessView> uav[3];
      D3D11_TEXTURE2D_DESC desc{}; desc.Width=w; desc.Height=h; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
      for (int i=0;i<3;i++) { Check(rep.CreateRawTexture(w,h,&raw[i],&srv[i])); Check(d->CreateTexture2D(&desc,nullptr,&out[i])); Check(rep.CreateOutputView(out[i].Get(),&uav[i])); }
      desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; Check(d->CreateTexture2D(&desc,nullptr,&staging));
      for (UINT frame=0;frame<5;frame++) {
        std::vector<BYTE> input(size_t(pitch)*h,0xcd);
        for(UINT y=0;y<h;y++) for(UINT x=0;x<w;x++) { auto p=&input[size_t(y)*pitch+3*x]; p[0]=BYTE(x*3+y+frame); p[1]=BYTE(x+y*7+frame*2); p[2]=BYTE(x*5+y*11+frame*3); }
        const auto original=input; const UINT i=frame%3;
        c->UpdateSubresource(raw[i].Get(),0,nullptr,input.data(),pitch,0); Check(rep.Dispatch(c.Get(),srv[i].Get(),uav[i].Get(),w,h));
        Require(input==original,"source modified"); c->CopyResource(staging.Get(),out[i].Get()); D3D11_MAPPED_SUBRESOURCE m{}; Check(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&m));
        for(UINT y=0;y<h;y++) for(UINT x=0;x<w;x++) { const BYTE* src=&input[size_t(y)*pitch+3*x]; const BYTE* dst=static_cast<const BYTE*>(m.pData)+size_t(y)*m.RowPitch+4*x; if(dst[0]!=src[2]||dst[1]!=src[1]||dst[2]!=src[0]||dst[3]!=255) { c->Unmap(staging.Get(),0); throw std::runtime_error("pixel mismatch"); } }
        c->Unmap(staging.Get(),0);
      }
      std::printf("PASS %ux%u padded pitch %u, five ring frames\n",w,h,pitch);
    }
    rep.Reset(); Require(rep.Dispatch(c.Get(),nullptr,nullptr,1,1)==E_UNEXPECTED,"reset"); return 0;
 } catch(const std::exception& e) { std::fprintf(stderr,"FAIL %s\n",e.what()); return 1; }
}
