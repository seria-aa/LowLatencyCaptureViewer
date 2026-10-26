#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace llcv::video {

inline constexpr HRESULT kUyvyConversionUnsupported =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x302);

// Converts one uploaded UYVY/HDYC frame to YUY2 without changing the CPU sample.
// A normal packed source texture avoids driver-dependent typed loads from a
// video-format alias; the single compute pass writes a separate YUY2 surface.
// The renderer owns the textures/views and dispatches after each new upload.
// No frame queue, CPU pixel conversion/readback, or GPU-completion wait is used.
class UyvyRepacker {
public:
    HRESULT Initialize(ID3D11Device* device) noexcept;
    void Reset() noexcept;
    HRESULT CreateInputTexture(UINT pixelWidth, UINT height,
                               ID3D11Texture2D** texture,
                               ID3D11ShaderResourceView** view) const noexcept;
    HRESULT CreateView(ID3D11Texture2D* texture,
                       ID3D11UnorderedAccessView** view) const noexcept;
    // pixelWidth is the full video width, not the width/2 packed-UAV width.
    // The view must have been created by CreateView for the same-size texture.
    HRESULT Dispatch(ID3D11DeviceContext* context,
                     ID3D11ShaderResourceView* input,
                     ID3D11UnorderedAccessView* view,
                     UINT pixelWidth, UINT height) const noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
};

} // namespace llcv::video
