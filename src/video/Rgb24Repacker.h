#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace llcv::video {

inline constexpr HRESULT kRgb24ConversionUnsupported =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x303);

// Expands one uploaded raw BGR24 frame into a native R8G8B8A8_UNORM surface
// without changing the CPU sample. Unlike UyvyRepacker, neither side is a
// reinterpreted view of another format's texture: the raw upload target is
// a plain R8_UINT byte buffer (width = 3 * pixelWidth) and the output is a
// genuinely native RGBA8 texture, so no video-FOURCC view-aliasing exception
// is required (none exists for RGB formats). The renderer owns the textures/
// views and dispatches after each new upload; no frame queue, CPU pixel
// conversion/readback, or GPU-completion wait is used.
class Rgb24Repacker {
public:
    HRESULT Initialize(ID3D11Device* device) noexcept;
    void Reset() noexcept;
    // Raw upload target: R8_UINT, width = 3 * pixelWidth (one byte/texel).
    // Rejects a pixelWidth whose raw byte width would exceed the device's
    // maximum 2D texture dimension instead of letting CreateTexture2D fail
    // with a less specific error.
    HRESULT CreateRawTexture(UINT pixelWidth, UINT height,
                             ID3D11Texture2D** texture,
                             ID3D11ShaderResourceView** view) const noexcept;
    // Output: a real (non-aliased) R8G8B8A8_UNORM texture; the caller creates
    // it (e.g. as the shared VP-input texture) and passes it here only to
    // obtain a plain UAV on its own native format.
    HRESULT CreateOutputView(ID3D11Texture2D* texture,
                             ID3D11UnorderedAccessView** view) const noexcept;
    // pixelWidth is the final image width, not the raw 3x byte-texture width.
    HRESULT Dispatch(ID3D11DeviceContext* context,
                     ID3D11ShaderResourceView* input,
                     ID3D11UnorderedAccessView* output,
                     UINT pixelWidth, UINT height) const noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
};

} // namespace llcv::video
