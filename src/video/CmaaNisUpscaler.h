#pragma once
// Display-only, current-frame CMAA2 -> NIS (SDR), NIS + contour guard (HDR10).
// The release path has no capture
// queues or CPU readback; only the separately linked diagnostic variant samples.
#include <d3d11.h>
#include <wrl/client.h>
#ifdef LLCV_UPSCALING_TELEMETRY
#include <memory>
#include "UpscalingOverflowTelemetry.h"
#endif
namespace llcv::upscaling {
// One atomic request carries selection, enable state, quality and sharpness.
// Bit 8 retains preserve mode. Legacy HIGH values remain unchanged; -1 selects VSR.
inline int Request(bool enabled, int sharpness, bool preserve = false) {
    sharpness = sharpness < 0 ? 0 : sharpness > 100 ? 100 : sharpness;
    const int value = sharpness | (preserve ? 256 : 0);
    return enabled ? value : -value - 2;
}
inline int RequestSharpness(int request) { return request == -1 ? -1 : (request >= 0 ? request : -request - 2) & 255; }
inline bool RequestPreserve(int request) { return request != -1 && ((request >= 0 ? request : -request - 2) & 256) != 0; }
bool Eligible(bool enabled, unsigned width, unsigned height, const RECT& destination);
class CmaaPipeline {
    friend struct PipelineTestAccess;
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    UINT width_=0,height_=0;
    Ptr<ID3D11RenderTargetView> linearTarget_,encodedTarget_;
    Ptr<ID3D11ShaderResourceView> linearView_,encodedView_;
    Ptr<ID3D11UnorderedAccessView> uavs_[8];
    Ptr<ID3D11Buffer> indirect_;
    Ptr<ID3D11ComputeShader> compute_[4];
    Ptr<ID3D11VertexShader> vertex_;
    Ptr<ID3D11PixelShader> decode_,encode_;
#ifdef LLCV_UPSCALING_TELEMETRY
    std::unique_ptr<OverflowTelemetry> overflowTelemetry_;
    Ptr<ID3D11Buffer> diagnosticControl_;
#endif
public:
    HRESULT Configure(ID3D11Device*,UINT width,UINT height,bool preserve = false);
    ID3D11ShaderResourceView* Draw(ID3D11DeviceContext*,ID3D11ShaderResourceView*);
};

class Pipeline {
    friend struct PipelineTestAccess;
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    bool enabled_ = false;
    bool hdr_ = false;
    CmaaPipeline cmaa_;
    UINT width_ = 0, height_ = 0, scaledWidth_ = 0, scaledHeight_ = 0;
    RECT destination_{};
    Ptr<ID3D11Texture2D> input_, intermediate_;
    Ptr<ID3D11VideoProcessorOutputView> inputOutput_;
    Ptr<ID3D11ShaderResourceView> inputView_, intermediateView_, coefficients_[2];
    Ptr<ID3D11UnorderedAccessView> intermediateUav_;
    Ptr<ID3D11PixelShader> pixel_;
    Ptr<ID3D11VertexShader> hdrVertex_;
    Ptr<ID3D11ComputeShader> compute_;
    Ptr<ID3D11Buffer> constants_, nisConstants_;
    Ptr<ID3D11SamplerState> sampler_;
    Ptr<ID3D11RasterizerState> rasterizer_;
    UINT nisBlockHeight_ = 24, nisBlockWidth_ = 32;
public:
    void Reset();
    bool Enabled() const { return enabled_; }
    HRESULT Configure(ID3D11Device*, ID3D11VideoDevice*, ID3D11VideoProcessorEnumerator*,
                      bool enabled, bool hdr, UINT width, UINT height, const RECT& destination, UINT vendor,
                      float nisSharpness = 0.15f, bool preserve = false);
    HRESULT Draw(ID3D11DeviceContext*, ID3D11VideoContext*, ID3D11VideoProcessor*,
                 D3D11_VIDEO_PROCESSOR_STREAM&, ID3D11RenderTargetView*, UINT outputWidth, UINT outputHeight);
};
}
