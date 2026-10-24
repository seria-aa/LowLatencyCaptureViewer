#pragma once
// SDR display-only, current-frame CMAA2 -> NIS. No capture queues or CPU readback.
#include <d3d11.h>
#include <wrl/client.h>
namespace llcv::upscaling {
// One atomic request carries selection, enable state and retained sharpness.
// -1: VSR selected; 0..100: CMAA/NIS on; -2..-102: CMAA/NIS off.
inline int Request(bool enabled, int sharpness) {
    sharpness = sharpness < 0 ? 0 : sharpness > 100 ? 100 : sharpness;
    return enabled ? sharpness : -sharpness - 2;
}
inline int RequestSharpness(int request) { return request >= 0 ? request : -request - 2; }
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
public:
    HRESULT Configure(ID3D11Device*,UINT width,UINT height);
    ID3D11ShaderResourceView* Draw(ID3D11DeviceContext*,ID3D11ShaderResourceView*);
};

class Pipeline {
    friend struct PipelineTestAccess;
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    bool enabled_ = false;
    CmaaPipeline cmaa_;
    UINT width_ = 0, height_ = 0, scaledWidth_ = 0, scaledHeight_ = 0;
    RECT destination_{};
    Ptr<ID3D11Texture2D> input_, intermediate_;
    Ptr<ID3D11VideoProcessorOutputView> inputOutput_;
    Ptr<ID3D11ShaderResourceView> inputView_, intermediateView_, coefficients_[2];
    Ptr<ID3D11UnorderedAccessView> intermediateUav_;
    Ptr<ID3D11PixelShader> pixel_;
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
                      float nisSharpness = 0.15f);
    HRESULT Draw(ID3D11DeviceContext*, ID3D11VideoContext*, ID3D11VideoProcessor*,
                 D3D11_VIDEO_PROCESSOR_STREAM&, ID3D11RenderTargetView*, UINT outputWidth, UINT outputHeight);
};
}
