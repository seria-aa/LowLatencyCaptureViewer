#pragma once

// Included only by the opt-in diagnostic executable and GPU tests.
// Readback is intentionally synchronous: both textures belong to the same
// render-thread frame, before overlays/Present. Never use for continuous capture.
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../video/CaptureColorMetadata.h"

namespace llcv::hdr_audit {
struct Image {
    D3D11_TEXTURE2D_DESC desc{};
    UINT rowBytes = 0;
    std::vector<unsigned char> bytes;
};

inline HRESULT Read(ID3D11DeviceContext* context, ID3D11Texture2D* source,
                    Image& image) {
    if (!context || !source) return E_POINTER;
    source->GetDesc(&image.desc);
    auto d = image.desc;
    if (d.ArraySize != 1 || d.MipLevels != 1 || d.SampleDesc.Count != 1 ||
        !d.Width || !d.Height || d.Width > 16384 || d.Height > 16384)
        return E_INVALIDARG;
    UINT rows = d.Height;
    if (d.Format == DXGI_FORMAT_P010) {
        if ((d.Width | d.Height) & 1) return E_INVALIDARG;
        image.rowBytes = d.Width * 2;
        rows += d.Height / 2;
    } else if (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM ||
               d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
               d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        image.rowBytes = d.Width * 4;
    } else if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        image.rowBytes = d.Width * 8;
    } else return DXGI_ERROR_UNSUPPORTED;
    image.bytes.resize(static_cast<size_t>(image.rowBytes) * rows);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&d, nullptr, &staging);
    if (FAILED(hr)) return hr;
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;
    if (mapped.RowPitch < image.rowBytes) hr = E_UNEXPECTED;
    else for (UINT row = 0; row < rows; ++row)
        memcpy(image.bytes.data() + static_cast<size_t>(row) * image.rowBytes,
               static_cast<const unsigned char*>(mapped.pData) +
                   static_cast<size_t>(row) * mapped.RowPitch, image.rowBytes);
    context->Unmap(staging.Get(), 0);
    return hr;
}

inline HRESULT WriteNew(const std::wstring& path, const void* data, DWORD size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    DWORD written = 0;
    HRESULT hr = S_OK;
    if (!WriteFile(file, data, size, &written, nullptr))
        hr = HRESULT_FROM_WIN32(GetLastError());
    else if (written != size) hr = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    if (!CloseHandle(file) && SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32(GetLastError());
    return hr;
}

struct Interpretation {
    llcv::video::CaptureColorMetadata metadata{};
    bool forceHdr = false;
    bool hdrOutput = false;
    unsigned inputColorSpace = 0;
    unsigned hdrChromaSelection = 0; // 0=Auto, 1=Top-left, 2=Left (requested)
    unsigned outputColorSpace = 0;
    unsigned sdrMatrix = 0;
    unsigned sdrRange = 0;
    int displayHdr = -1;
    float uiWhiteNits = 0;
};

// The caller supplies a NEW directory. Existing files are never overwritten.
// A metadata.json file is written last and is the completion marker. On error,
// partial files remain local for diagnosis; failure never changes video policy.
inline HRESULT SavePair(ID3D11DeviceContext* context, ID3D11Texture2D* input,
                        ID3D11Texture2D* output, const std::wstring& directory,
                        const Interpretation& info) {
    if (!input || !output) return E_POINTER;
    D3D11_TEXTURE2D_DESC d{};
    input->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_P010) return DXGI_ERROR_UNSUPPORTED;
    Image raw, rgb;
    HRESULT hr = Read(context, input, raw);
    if (SUCCEEDED(hr)) hr = Read(context, output, rgb);
    if (FAILED(hr)) return hr;
    if (!CreateDirectoryW(directory.c_str(), nullptr))
        return HRESULT_FROM_WIN32(GetLastError());
    hr = WriteNew(directory + L"\\input.p010", raw.bytes.data(),
                  static_cast<DWORD>(raw.bytes.size()));
    if (SUCCEEDED(hr)) hr = WriteNew(directory + L"\\output.raw", rgb.bytes.data(),
                                     static_cast<DWORD>(rgb.bytes.size()));
    if (FAILED(hr)) return hr;
    char text[2048]{};
    const auto& m = info.metadata;
    const int size = std::snprintf(text, sizeof(text),
        "{\n  \"schema\": 1, \"complete\": true,\n"
        "  \"stage\": \"same frame; uploaded P010 and post-conversion, pre-overlay/pre-Present RGB\",\n"
        "  \"storage\": \"little endian, tightly packed rows; P010 Y then interleaved UV; 10 bits in high bits\",\n"
        "  \"input\": {\"width\": %u, \"height\": %u, \"dxgi_format\": %u, \"row_bytes\": %u},\n"
        "  \"output\": {\"width\": %u, \"height\": %u, \"dxgi_format\": %u, \"row_bytes\": %u},\n"
        "  \"force_hdr\": %u, \"hdr_output\": %u,\n"
        "  \"hdr_chroma_selection\": %u,\n"
        "  \"hdr_input_colorspace\": %u, \"vp_output_colorspace\": %u,\n"
        "  \"sdr_matrix_enum\": %u, \"sdr_range_enum\": %u,\n"
        "  \"display_hdr\": %d, \"ui_white_nits\": %.3f,\n"
        "  \"metadata_present\": %u, \"control_flags\": %lu,\n"
        "  \"chroma\": %u, \"range\": %u, \"matrix\": %u, \"primaries\": %u, \"transfer\": %u,\n"
        "  \"note\": \"No tone mapping or screenshot conversion. HDR labels describe interpretation, not proof of source encoding. SDR enums: matrix 0=601 1=709; range 0=limited 1=full.\"\n}\n",
        raw.desc.Width, raw.desc.Height, static_cast<unsigned>(raw.desc.Format), raw.rowBytes,
        rgb.desc.Width, rgb.desc.Height, static_cast<unsigned>(rgb.desc.Format), rgb.rowBytes,
        unsigned(info.forceHdr), unsigned(info.hdrOutput), info.hdrChromaSelection, info.inputColorSpace,
        info.outputColorSpace, info.sdrMatrix, info.sdrRange, info.displayHdr, info.uiWhiteNits,
        unsigned(m.present), m.controlFlags, m.chromaSubsampling, m.nominalRange,
        m.transferMatrix, m.primaries, m.transferFunction);
    if (size < 0 || static_cast<size_t>(size) >= sizeof(text)) return E_UNEXPECTED;
    return WriteNew(directory + L"\\metadata.json", text, static_cast<DWORD>(size));
}
} // namespace llcv::hdr_audit
