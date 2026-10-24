#pragma once
// Private SDR spatial-scaling experiment. Not compiled into the regular viewer.
#include <d3d11.h>
#include "CmaaNisUpscaler.h"
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>

namespace llcv::spatial {
enum class Mode { Disabled, Bicubic, Cas, Fsr1, Nis, Sgsr, Ravu, Fsrcnnx, Jinc, Lanczos };
enum class AaMode { Disabled, SmaaBeforeNis, SmaaAfterNis, SmaaUltraAfterNis, Cmaa2AfterNis, SmaaUltraBeforeNis, Cmaa2BeforeNis };
inline bool AaBeforeNis(AaMode a) { return a==AaMode::SmaaBeforeNis || a==AaMode::SmaaUltraBeforeNis || a==AaMode::Cmaa2BeforeNis; }
inline bool AaIsCmaa2(AaMode a) { return a==AaMode::Cmaa2BeforeNis || a==AaMode::Cmaa2AfterNis; }
const char* Name(Mode mode);
Mode EnvironmentMode();
float EnvironmentNisSharpness();
AaMode EnvironmentAaMode();
bool Eligible(Mode mode, bool hdr, unsigned width, unsigned height, const RECT& destination);
// Private bounded capture trace. Buffer in memory; write only on normal shutdown.
class LiveTrace {
    struct Frame { int64_t arrivalUs, presentQpc, returnedUs; };
    std::wstring path_;
    std::vector<Frame> frames_;
    std::vector<unsigned char> input_;
    std::vector<unsigned char> output_;
    UINT outputWidth_=0,outputHeight_=0;
    bool inputHdr_=false;
    bool outputHdr_=false;
public:
    LiveTrace();
    ~LiveTrace();
    void Add(int64_t arrivalUs,int64_t presentQpc,int64_t returnedUs);
    void Snapshot(const unsigned char* pixels,size_t bytes,bool hdr=false);
    void SnapshotOutput(ID3D11Device*,ID3D11DeviceContext*,ID3D11Texture2D*);
};

// Private, current-frame SMAA 1x. No capture/UI integration or history buffers.
class SmaaPipeline {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    UINT width_=0,height_=0;
    Ptr<ID3D11RenderTargetView> targets_[3];
    Ptr<ID3D11ShaderResourceView> views_[3],area_,search_;
    Ptr<ID3D11VertexShader> vertex_[3];
    Ptr<ID3D11PixelShader> pixel_[3];
    Ptr<ID3D11Buffer> metrics_;
    Ptr<ID3D11SamplerState> samplers_[2];
public:
    HRESULT Configure(ID3D11Device*,UINT width,UINT height,bool ultraColor=false);
    ID3D11ShaderResourceView* Draw(ID3D11DeviceContext*,ID3D11ShaderResourceView* input);
};

using CmaaPipeline = llcv::upscaling::CmaaPipeline;
class Pipeline {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Mode mode_ = Mode::Disabled;
    AaMode aaMode_ = AaMode::Disabled;
    SmaaPipeline smaa_;
    CmaaPipeline cmaa_;
    UINT width_ = 0, height_ = 0, scaledWidth_ = 0, scaledHeight_ = 0;
    RECT destination_{};
    Ptr<ID3D11Texture2D> input_, intermediate_;
    Ptr<ID3D11VideoProcessorOutputView> inputOutput_;
    Ptr<ID3D11ShaderResourceView> inputView_, intermediateView_, coefficients_[2];
    Ptr<ID3D11RenderTargetView> intermediateTarget_;
    Ptr<ID3D11UnorderedAccessView> intermediateUav_;
    Ptr<ID3D11VertexShader> vertex_;
    Ptr<ID3D11PixelShader> pixel_, second_;
    Ptr<ID3D11ComputeShader> compute_;
    Ptr<ID3D11ComputeShader> network_[6];
    Ptr<ID3D11ShaderResourceView> featureViews_[6];
    Ptr<ID3D11UnorderedAccessView> featureUavs_[6];
    Ptr<ID3D11Buffer> learnedConstants_;
    Ptr<ID3D11SamplerState> pointSampler_;
    Ptr<ID3D11Buffer> constants_, nisConstants_;
    Ptr<ID3D11SamplerState> sampler_;
    Ptr<ID3D11RasterizerState> rasterizer_;
    UINT nisBlockHeight_ = 24, nisBlockWidth_ = 32;
public:
    void Reset();
    Mode ActiveMode() const { return mode_; }
    AaMode ActiveAaMode() const { return aaMode_; }
    bool Enabled() const { return mode_ != Mode::Disabled; }
    HRESULT Configure(ID3D11Device*, ID3D11VideoDevice*, ID3D11VideoProcessorEnumerator*,
                      Mode, bool hdr, UINT width, UINT height, const RECT& destination, UINT vendor,
                      float nisSharpness = 0.5f, AaMode smaa = AaMode::Disabled);
    HRESULT Draw(ID3D11DeviceContext*, ID3D11VideoContext*, ID3D11VideoProcessor*,
                 D3D11_VIDEO_PROCESSOR_STREAM&, ID3D11RenderTargetView*, UINT outputWidth, UINT outputHeight);
};
}
