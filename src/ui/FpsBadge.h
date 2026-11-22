#pragma once

#include "ui/OverlayStyle.h"
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace llcv::overlay_ui {
class FpsBadge {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11ShaderResourceView> view_;
    Ptr<ID2D1RenderTarget> target_;
    Ptr<ID2D1SolidColorBrush> background_, text_;
    Ptr<IDWriteTextFormat> format_;
    int drawn_=-1;
    bool unavailable_=false;

    HRESULT Initialize(ID3D11Device* device,ID2D1Factory* d2d,IDWriteFactory* dwrite,
                       const Fonts& fonts,bool hdr,bool english) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=Width; desc.Height=Height;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        Ptr<ID3D11Texture2D> texture;
        Ptr<IDXGISurface> surface;
        HRESULT hr=device->CreateTexture2D(&desc,nullptr,&texture);
        if(SUCCEEDED(hr))hr=texture.As(&surface);
        if(SUCCEEDED(hr))hr=device->CreateShaderResourceView(texture.Get(),nullptr,&view_);
        if(SUCCEEDED(hr))hr=d2d->CreateDxgiSurfaceRenderTarget(surface.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_HARDWARE,
                D2D1::PixelFormat(desc.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96.0f,96.0f),
            &target_);
        if(SUCCEEDED(hr))hr=target_->CreateSolidColorBrush(Background(hdr,0.72f),&background_);
        if(SUCCEEDED(hr))hr=target_->CreateSolidColorBrush(Color(kPalette.kText),&text_);
        if(SUCCEEDED(hr))hr=fonts.CreateFormat(dwrite,DWRITE_FONT_WEIGHT_SEMI_BOLD,16.0f,
                                               english,&format_);
        if(FAILED(hr))return hr;
        format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        return S_OK;
    }
public:
    static constexpr LONG Width=104, Height=34;

    static int DisplayRate(double fps) {
        return std::isfinite(fps)&&fps>0.0
            ? static_cast<int>(std::lround((std::min)(fps,9999.0))) : 0;
    }
    void Reset(){*this=FpsBadge{};}

    ID3D11ShaderResourceView* Refresh(ID3D11Device* device,ID2D1Factory* d2d,
            IDWriteFactory* dwrite,const Fonts& fonts,bool hdr,bool english,double fps) {
        if(unavailable_)return nullptr;
        const int rate=DisplayRate(fps);
        HRESULT hr=S_OK;
        if(!target_)hr=Initialize(device,d2d,dwrite,fonts,hdr,english);
        if(SUCCEEDED(hr)&&drawn_!=rate){
            wchar_t label[16]{};
            if(rate)swprintf_s(label,L"%d FPS",rate);
            else wcscpy_s(label,L"— FPS");
            const auto bounds=D2D1::RectF(0,0,Width,Height);
            target_->BeginDraw();
            target_->Clear(D2D1::ColorF(0,0.0f));
            target_->FillRoundedRectangle(D2D1::RoundedRect(bounds,8.0f,8.0f),background_.Get());
            target_->DrawText(label,static_cast<UINT32>(wcslen(label)),format_.Get(),bounds,text_.Get());
            hr=target_->EndDraw();
            if(SUCCEEDED(hr))drawn_=rate;
        }
        if(FAILED(hr)){
            // A cosmetic badge must not interrupt normal playback.
            Reset();unavailable_=true;return nullptr;
        }
        return view_.Get();
    }
};
}
