// Synthetic resources only. Readback/waits below are test-only, not playback.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "../src/video/UyvyRepacker.h"

template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
using llcv::video::UyvyRepacker;
static void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void Check(HRESULT hr) {
    if (FAILED(hr)) {
        char text[48]{};
        sprintf_s(text, "D3D failure 0x%08X", unsigned(hr));
        throw std::runtime_error(text);
    }
}
struct Gpu {
    Ptr<ID3D11Device> d;
    Ptr<ID3D11DeviceContext> c;
    explicit Gpu(bool warp) {
        D3D_FEATURE_LEVEL level{};
        Check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &level, &c));
        Require(level >= D3D_FEATURE_LEVEL_11_0, "FL11 device required");
    }
    Ptr<ID3D11Texture2D> Texture(UINT w, UINT h, UINT bind,
                                DXGI_FORMAT format = DXGI_FORMAT_YUY2) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.Format = format;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.BindFlags = bind;
        Ptr<ID3D11Texture2D> texture;
        Check(d->CreateTexture2D(&desc, nullptr, &texture));
        return texture;
    }
    std::vector<BYTE> Read(ID3D11Texture2D* texture) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        Require(desc.Format == DXGI_FORMAT_YUY2 || desc.Format == DXGI_FORMAT_R8G8B8A8_UINT,
                "readback packed input or YUY2");
        const UINT row = desc.Width * (desc.Format == DXGI_FORMAT_YUY2 ? 2 : 4), height = desc.Height;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.BindFlags = desc.MiscFlags = 0;
        Ptr<ID3D11Texture2D> staging;
        Check(d->CreateTexture2D(&desc, nullptr, &staging));
        c->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE map{};
        Check(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
        std::vector<BYTE> result(size_t(row) * height);
        for (UINT y = 0; y < height; ++y)
            std::memcpy(result.data() + size_t(y) * row,
                static_cast<const BYTE*>(map.pData) + size_t(y) * map.RowPitch, row);
        c->Unmap(staging.Get(), 0);
        return result;
    }
};

static void InvalidInputs(Gpu& gpu) {
    UyvyRepacker repack;
    Ptr<ID3D11UnorderedAccessView> view;
    Require(repack.Initialize(nullptr) == E_INVALIDARG, "null initialization rejected");
    Require(repack.Dispatch(gpu.c.Get(), nullptr, nullptr, 2, 1) == E_UNEXPECTED,
            "uninitialized dispatch rejected");
    Require(repack.CreateView(nullptr, nullptr) == E_POINTER, "null output rejected");
    Require(repack.CreateView(nullptr, &view) == E_UNEXPECTED, "uninitialized view rejected");
    Check(repack.Initialize(gpu.d.Get()));
    Require(repack.CreateView(nullptr, &view) == E_INVALIDARG && !view,
            "null texture rejected and output cleared");
    auto wrongFormat = gpu.Texture(2, 2, D3D11_BIND_UNORDERED_ACCESS, DXGI_FORMAT_R32_UINT);
    Require(repack.CreateView(wrongFormat.Get(), &view) == E_INVALIDARG && !view,
            "non-video surface rejected");
    auto missingBind = gpu.Texture(2, 2, 0);
    Require(repack.CreateView(missingBind.Get(), &view) == E_INVALIDARG && !view,
            "surface without UAV bind rejected");
    auto valid = gpu.Texture(2, 2, D3D11_BIND_UNORDERED_ACCESS);
    Check(repack.CreateView(valid.Get(), &view));
    Ptr<ID3D11Texture2D> source;
    Ptr<ID3D11ShaderResourceView> input;
    Check(repack.CreateInputTexture(2, 2, &source, &input));
    Require(repack.Dispatch(nullptr, input.Get(), view.Get(), 2, 2) == E_INVALIDARG, "null context rejected");
    Require(repack.Dispatch(gpu.c.Get(), input.Get(), nullptr, 2, 2) == E_INVALIDARG, "null view rejected");
    Require(repack.Dispatch(gpu.c.Get(), nullptr, view.Get(), 2, 2) == E_INVALIDARG, "null input rejected");
    for (const auto dims : {std::array<UINT, 2>{0, 2}, {3, 2}, {2, 0}, {32768, 2}, {2, 32768}})
        Require(repack.Dispatch(gpu.c.Get(), input.Get(), view.Get(), dims[0], dims[1]) == E_INVALIDARG,
                "invalid dispatch dimensions rejected");
    Require(repack.Initialize(nullptr) == E_INVALIDARG, "failed reinitialize resets state");
    Require(repack.Dispatch(gpu.c.Get(), input.Get(), view.Get(), 2, 2) == E_UNEXPECTED,
            "failed reinitialize cannot reuse old shader");
    Check(repack.Initialize(gpu.d.Get()));
    repack.Reset();
    Require(repack.CreateView(valid.Get(), &view) == E_UNEXPECTED && !view,
            "reset invalidates view creation");

    Ptr<ID3D11Device> lowDevice;
    D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_10_0;
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &requested, 1,
                           D3D11_SDK_VERSION, &lowDevice, nullptr, nullptr));
    Require(repack.Initialize(lowDevice.Get()) == DXGI_ERROR_UNSUPPORTED,
            "shader-model-5 unsupported device fails before dispatch");
    std::puts("PASS: initialization, capability rejection, resource validation, reset");
}

static std::vector<BYTE> Input(UINT w, UINT h, UINT pitch, UINT frame) {
    std::vector<BYTE> bytes(size_t(pitch) * h, 0xcd);
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w / 2; ++x) {
        BYTE* p = bytes.data() + size_t(y) * pitch + x * 4;
        p[0] = BYTE(x * 43 + y * 13 + frame * 71);
        p[1] = BYTE(x * 23 + y * 47 + frame * 11);
        p[2] = BYTE(x * 61 + y * 37 + frame * 53);
        p[3] = BYTE(x * 19 + y * 29 + frame * 97);
    }
    return bytes;
}
static void ExactFrames(Gpu& gpu, UINT width, UINT height, UINT padding) {
    UyvyRepacker repack;
    Check(repack.Initialize(gpu.d.Get()));
    std::array<Ptr<ID3D11Texture2D>, 3> textures;
    std::array<Ptr<ID3D11Texture2D>, 3> sources;
    std::array<Ptr<ID3D11ShaderResourceView>, 3> inputs;
    std::array<Ptr<ID3D11UnorderedAccessView>, 3> views;
    for (size_t i = 0; i < textures.size(); ++i) {
        textures[i] = gpu.Texture(width, height, D3D11_BIND_UNORDERED_ACCESS);
        Check(repack.CreateView(textures[i].Get(), &views[i]));
        Check(repack.CreateInputTexture(width, height, &sources[i], &inputs[i]));
    }
    auto reference = gpu.Texture(width, height, 0);
    const UINT row = width * 2, pitch = row + padding;
    for (UINT frame = 0; frame < 9; ++frame) {
        auto input = Input(width, height, pitch, frame);
        const auto original = input;
        std::vector<BYTE> expected(size_t(row) * height);
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width / 2; ++x) {
            const BYTE* source = input.data() + size_t(y) * pitch + x * 4;
            BYTE* target = expected.data() + size_t(y) * row + x * 4;
            target[0] = source[1]; target[1] = source[0];
            target[2] = source[3]; target[3] = source[2];
        }
        const size_t slot = frame % textures.size();
        gpu.c->UpdateSubresource(sources[slot].Get(), 0, nullptr, input.data(), pitch, 0);
        const auto uploaded = gpu.Read(sources[slot].Get());
        for(UINT y=0;y<height;++y)Require(std::memcmp(uploaded.data()+size_t(y)*row,
            input.data()+size_t(y)*pitch,row)==0,"upload preserves all UYVY source rows before compute");
        gpu.c->UpdateSubresource(reference.Get(), 0, nullptr, expected.data(), row, 0);
        Check(repack.Dispatch(gpu.c.Get(), inputs[slot].Get(), views[slot].Get(), width, height));
        const auto actual = gpu.Read(textures[slot].Get());
        if(actual!=expected) {
            unsigned shown=0;
            for(size_t i=0;i<actual.size()&&shown<12;++i)if(actual[i]!=expected[i]) {
                std::printf("DIFF %ux%u frame=%u row=%zu byte=%zu got=%u expected=%u\n",width,height,frame,i/row,i%row,actual[i],expected[i]);++shown;
            }
        }
        Require(actual == expected && actual == gpu.Read(reference.Get()),
                "every output byte equals equivalent uploaded YUY2, including all edges");
        Require(input == original, "source and padded rows are not mutated");
        Require(gpu.Read(sources[slot].Get()) == uploaded, "GPU upload source remains unchanged");
        Ptr<ID3D11UnorderedAccessView> bound;
        gpu.c->CSGetUnorderedAccessViews(0, 1, &bound);
        Require(!bound, "UAV unbound before video processor input");
        Ptr<ID3D11ComputeShader> shader;
        gpu.c->CSGetShader(&shader, nullptr, nullptr);
        Require(!shader, "compute shader unbound before downstream filters");
        Ptr<ID3D11ShaderResourceView> boundInput;
        gpu.c->CSGetShaderResources(0, 1, &boundInput);
        Require(!boundInput, "source SRV unbound before downstream filters");
        Check(repack.Dispatch(gpu.c.Get(), inputs[slot].Get(), views[slot].Get(), width, height));
        Require(gpu.Read(textures[slot].Get()) == expected,
                "repeat conversion of immutable source is stable");
    }
    std::printf("PASS: %ux%u pitch=%u, nine frames/three upload surfaces, bit-exact YUY2\n",
                width, height, pitch);
}

static void Benchmark(Gpu& gpu) {
    constexpr UINT width = 1920, height = 1080, count = 120;
    UyvyRepacker repack;
    Check(repack.Initialize(gpu.d.Get()));
    auto texture = gpu.Texture(width, height, D3D11_BIND_UNORDERED_ACCESS);
    Ptr<ID3D11UnorderedAccessView> view;
    Check(repack.CreateView(texture.Get(), &view));
    Ptr<ID3D11Texture2D> source;
    Ptr<ID3D11ShaderResourceView> inputView;
    Check(repack.CreateInputTexture(width, height, &source, &inputView));
    auto input = Input(width, height, width * 2, 0);
    gpu.c->UpdateSubresource(source.Get(), 0, nullptr, input.data(), width * 2, 0);
    Ptr<ID3D11Query> disjoint, begin, end;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    Check(gpu.d->CreateQuery(&desc, &disjoint));
    desc.Query = D3D11_QUERY_TIMESTAMP;
    Check(gpu.d->CreateQuery(&desc, &begin));
    Check(gpu.d->CreateQuery(&desc, &end));
    for (unsigned round = 0; round < 5; ++round) {
        for (unsigned i = 0; i < 20; ++i)
            Check(repack.Dispatch(gpu.c.Get(), inputView.Get(), view.Get(), width, height));
        gpu.c->Begin(disjoint.Get()); gpu.c->End(begin.Get());
        const auto cpuStart = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < count; ++i)
            Check(repack.Dispatch(gpu.c.Get(), inputView.Get(), view.Get(), width, height));
        const double cpu = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpuStart).count() / count;
        gpu.c->End(end.Get()); gpu.c->End(disjoint.Get());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        auto wait = [&](ID3D11Query* query, void* data, UINT size) {
            HRESULT hr;
            while ((hr = gpu.c->GetData(query, data, size, 0)) == S_FALSE) {
                Require(std::chrono::steady_clock::now() < deadline, "benchmark query timeout");
                Sleep(1);
            }
            Check(hr);
        };
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
        UINT64 first = 0, last = 0;
        wait(disjoint.Get(), &frequency, sizeof(frequency));
        wait(begin.Get(), &first, sizeof(first)); wait(end.Get(), &last, sizeof(last));
        Require(!frequency.Disjoint && frequency.Frequency && last >= first, "valid GPU timestamps");
        std::printf("BENCH round=%u 1080p GPU-repack-ms=%.6f CPU-dispatch-ms=%.6f\n", round,
                    double(last - first) * 1000.0 / double(frequency.Frequency) / count, cpu);
    }
    std::puts("BENCH measures queued compute only; not capture/upload/present/end-to-end latency.");
}

int main(int argc, char** argv) {
    try {
        bool warp = false, benchmark = false;
        for (int i = 1; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--warp")) warp = true;
            else if (!std::strcmp(argv[i], "--benchmark")) benchmark = true;
            else throw std::runtime_error("unknown argument");
        }
        Gpu gpu(warp);
        InvalidInputs(gpu);
        for (const auto dims : {std::array<UINT, 3>{2, 1, 0}, {6, 3, 28},
                               {34, 19, 36}, {258, 73, 28}, {1920, 1080, 64},
                               {3840, 2160, 64}})
            ExactFrames(gpu, dims[0], dims[1], dims[2]);
        if (benchmark) Benchmark(gpu);
        std::puts("PASS: UYVY GPU repack regression");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
