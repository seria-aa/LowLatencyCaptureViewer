#include "Rgb24Repacker.h"
#include "rgb24Repack.h"

namespace llcv::video {
void Rgb24Repacker::Reset() noexcept { shader_.Reset(); device_.Reset(); }
HRESULT Rgb24Repacker::Initialize(ID3D11Device* device) noexcept {
    Reset();
    if (!device) return E_INVALIDARG;
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return DXGI_ERROR_UNSUPPORTED;
    UINT in = 0, out = 0;
    HRESULT hr = device->CheckFormatSupport(DXGI_FORMAT_R8_UINT, &in);
    if (FAILED(hr)) return hr;
    hr = device->CheckFormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM, &out);
    if (FAILED(hr)) return hr;
    constexpr UINT inputFlags = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD;
    constexpr UINT outputFlags = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    if ((in & inputFlags) != inputFlags || (out & outputFlags) != outputFlags) return DXGI_ERROR_UNSUPPORTED;
    hr = device->CreateComputeShader(rgb24Repack, sizeof(rgb24Repack), nullptr, shader_.GetAddressOf());
    if (SUCCEEDED(hr)) device_ = device;
    return hr;
}
HRESULT Rgb24Repacker::CreateRawTexture(UINT w, UINT h, ID3D11Texture2D** texture, ID3D11ShaderResourceView** view) const noexcept {
    if (texture) *texture = nullptr;
    if (view) *view = nullptr;
    if (!texture || !view) return E_POINTER;
    if (!device_ || !shader_) return E_UNEXPECTED;
    if (!w || w > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / 3 || !h || h > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC d{}; d.Width = 3*w; d.Height = h; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8_UINT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> t;
    HRESULT hr = device_->CreateTexture2D(&d, nullptr, &t);
    if (FAILED(hr)) return hr;
    hr = device_->CreateShaderResourceView(t.Get(), nullptr, view);
    if (SUCCEEDED(hr)) *texture = t.Detach();
    return hr;
}
HRESULT Rgb24Repacker::CreateOutputView(ID3D11Texture2D* texture, ID3D11UnorderedAccessView** view) const noexcept {
    if (!view) return E_POINTER;
    *view = nullptr;
    if (!device_ || !shader_) return E_UNEXPECTED;
    if (!texture) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D11Device> owner; texture->GetDevice(owner.GetAddressOf());
    D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d);
    if (owner.Get() != device_.Get() || d.Format != DXGI_FORMAT_R8G8B8A8_UNORM || !d.Width || !d.Height || d.MipLevels != 1 || d.ArraySize != 1 || d.SampleDesc.Count != 1 || d.Usage != D3D11_USAGE_DEFAULT || !(d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) || d.CPUAccessFlags) return E_INVALIDARG;
    return device_->CreateUnorderedAccessView(texture, nullptr, view);
}
HRESULT Rgb24Repacker::Dispatch(ID3D11DeviceContext* context, ID3D11ShaderResourceView* input, ID3D11UnorderedAccessView* output, UINT w, UINT h) const noexcept {
    if (!shader_) return E_UNEXPECTED;
    if (!context || !input || !output || !w || w > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / 3 || !h || h > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return E_INVALIDARG;
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &input);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch((w + 15)/16, (h + 15)/16, 1);
    ID3D11UnorderedAccessView* noOutput = nullptr; context->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
    ID3D11ShaderResourceView* noInput = nullptr; context->CSSetShaderResources(0, 1, &noInput);
    context->CSSetShader(nullptr, nullptr, 0);
    return S_OK;
}
}
