#include "UyvyRepacker.h"
#include "uyvyRepack.h"

namespace llcv::video {

void UyvyRepacker::Reset() noexcept {
    shader_.Reset();
    device_.Reset();
}

HRESULT UyvyRepacker::Initialize(ID3D11Device* device) noexcept {
    Reset();
    if (!device) return E_INVALIDARG;
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
        return DXGI_ERROR_UNSUPPORTED;
    UINT videoSupport = 0, packedSupport = 0;
    HRESULT hr = device->CheckFormatSupport(DXGI_FORMAT_YUY2, &videoSupport);
    if (FAILED(hr)) return hr;
    hr = device->CheckFormatSupport(DXGI_FORMAT_R8G8B8A8_UINT, &packedSupport);
    if (FAILED(hr)) return hr;
    constexpr UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D |
                              D3D11_FORMAT_SUPPORT_SHADER_LOAD |
                              D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if (!(videoSupport & D3D11_FORMAT_SUPPORT_TEXTURE2D) ||
        (packedSupport & required) != required)
        return DXGI_ERROR_UNSUPPORTED;
    // Read through a normal SRV, write only through the video UAV. This does
    // not require optional D3D11.3 R8G8B8A8_UINT typed UAV loads.
    hr = device->CreateComputeShader(uyvyRepack, sizeof(uyvyRepack), nullptr,
                                    shader_.GetAddressOf());
    if (SUCCEEDED(hr)) device_ = device;
    return hr;
}

HRESULT UyvyRepacker::CreateInputTexture(UINT pixelWidth, UINT height,
                                        ID3D11Texture2D** texture,
                                        ID3D11ShaderResourceView** view) const noexcept {
    if (texture) *texture = nullptr;
    if (view) *view = nullptr;
    if (!texture || !view) return E_POINTER;
    if (!device_ || !shader_) return E_UNEXPECTED;
    if (!pixelWidth || (pixelWidth & 1) || !height ||
        pixelWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = pixelWidth / 2; desc.Height = height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &source);
    if (FAILED(hr)) return hr;
    hr = device_->CreateShaderResourceView(source.Get(), nullptr, view);
    if (SUCCEEDED(hr)) *texture = source.Detach();
    return hr;
}

HRESULT UyvyRepacker::CreateView(ID3D11Texture2D* texture,
                                ID3D11UnorderedAccessView** view) const noexcept {
    if (!view) return E_POINTER;
    *view = nullptr;
    if (!device_ || !shader_) return E_UNEXPECTED;
    if (!texture) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D11Device> owner;
    texture->GetDevice(owner.GetAddressOf());
    if (owner.Get() != device_.Get()) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_YUY2 || !desc.Width || (desc.Width & 1) ||
        !desc.Height || desc.MipLevels != 1 || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 || desc.Usage != D3D11_USAGE_DEFAULT ||
        !(desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) || desc.CPUAccessFlags)
        return E_INVALIDARG;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    // Texture and view creation are the authoritative per-driver check for
    // this video-format alias; RGBA support alone does not guarantee success.
    return device_->CreateUnorderedAccessView(texture, &uav, view);
}

HRESULT UyvyRepacker::Dispatch(ID3D11DeviceContext* context,
                              ID3D11ShaderResourceView* input,
                              ID3D11UnorderedAccessView* view,
                              UINT pixelWidth, UINT height) const noexcept {
    if (!shader_) return E_UNEXPECTED;
    if (!context || !input || !view || !pixelWidth || (pixelWidth & 1) || !height ||
        pixelWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return E_INVALIDARG;
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &input);
    context->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
    context->Dispatch((pixelWidth / 2 + 15) / 16, (height + 15) / 16, 1);
    // Release the write binding before the same resource becomes VP input.
    ID3D11UnorderedAccessView* empty = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
    ID3D11ShaderResourceView* emptyInput = nullptr;
    context->CSSetShaderResources(0, 1, &emptyInput);
    context->CSSetShader(nullptr, nullptr, 0);
    return S_OK;
}

} // namespace llcv::video
