#pragma once

#include "recording/RecordingStatus.h"
#include <d2d1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace llcv::overlay_ui { class Fonts; }

namespace llcv::recording {

// The UI publishes the same snapshot used by the native recording control.
// The render owner never reads Recorder or touches the recording worker mutex.
void PublishGpuIndicator(const Snapshot& snapshot, bool english, bool light,
                         UINT dpi = 96);
uint64_t GpuIndicatorRevision() noexcept;
bool GpuIndicatorHitTest(UINT width, UINT height, int x, int y);
bool HandleGpuIndicatorClick(HWND viewer, UINT width, UINT height, int x, int y);

// Fixed-size, transparent SDR UI texture. Composite through the renderer's
// existing SDR/HDR overlay shader; raw recording samples never include it.
class GpuIndicator {
public:
    HRESULT Initialize(ID3D11Device* device, ID2D1Factory* d2d,
                       IDWriteFactory* dwrite,
                       const overlay_ui::Fonts& fonts, bool english);
    void Reset() noexcept;
    HRESULT Refresh();
    bool Visible() const noexcept { return visible_; }
    ID3D11ShaderResourceView* ShaderView() const noexcept { return view_.Get(); }
    RECT QuadRect(UINT width, UINT height) const noexcept;
    RECT CardRect(UINT width, UINT height) const noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background_, edge_, text_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
    uint64_t revision_ = UINT64_MAX;
    UINT dpi_ = 96;
    RECT card_{};
    bool visible_ = false;
};

} // namespace llcv::recording
