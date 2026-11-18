#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <propkey.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace llcv::recording::test {
inline void Require(bool ok, const char* message) {
    if (!ok) { std::printf("FAIL %s\n", message); std::exit(1); }
}
inline void CheckShellDuration(const wchar_t* path, LONGLONG expected) {
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    HRESULT hr = SHGetPropertyStoreFromParsingName(path, nullptr,
        GPS_HANDLERPROPERTIESONLY, IID_PPV_ARGS(&properties));
    PROPVARIANT value{};
    if (SUCCEEDED(hr)) hr = properties->GetValue(PKEY_Media_Duration, &value);
    const auto duration = value.vt == VT_UI8 ? value.uhVal.QuadPart : 0;
    std::printf("Explorer duration: hr=%08lX type=%u actual=%llu expected=%lld (100ns)\n",
        static_cast<unsigned long>(hr), value.vt, duration, expected);
    PropVariantClear(&value);
    Require(SUCCEEDED(hr) && duration > 0 && expected > 0 &&
        std::abs(static_cast<LONGLONG>(duration) - expected) < 2000000,
        "Windows Explorer exposes the completed recording's duration within 200 ms");
}

// FFmpeg's hybrid file retains its original ftyp+moov+moof sequence inside
// the final mdat. Extract it for torn-fragment tests; never truncate a completed
// regular MP4 and mistake its missing final index for an interrupted capture.
// Real process-termination tests independently verify the live fragmented path.
inline void FragmentedSnapshot(const wchar_t* source, const wchar_t* destination) {
    Require(!std::filesystem::exists(destination), "snapshot never overwrites a file");
    std::ifstream input(source, std::ios::binary);
    const auto length = std::filesystem::file_size(source);
    uint64_t position = 0;
    while (position + 8 <= length) {
        std::array<unsigned char, 16> header{};
        input.seekg(position); input.read(reinterpret_cast<char*>(header.data()), 8);
        Require(input.good(), "snapshot box header");
        auto integer = [&](unsigned first, unsigned end) {
            uint64_t n = 0; for (unsigned i = first; i < end; ++i) n = (n << 8) | header[i]; return n;
        };
        auto size = integer(0, 4); unsigned headerSize = 8;
        if (size == 1) {
            input.read(reinterpret_cast<char*>(header.data() + 8), 8);
            Require(input.good(), "snapshot extended header");
            size = integer(8, 16); headerSize = 16;
        }
        Require(size >= headerSize && size <= length - position, "snapshot complete box");
        if (header[4] == 'm' && header[5] == 'd' && header[6] == 'a' && header[7] == 't') {
            std::ofstream output(destination, std::ios::binary);
            std::array<char, 65536> chunk{};
            for (auto left = size - headerSize; left;) {
                const auto count = (std::min)(left, uint64_t(chunk.size()));
                input.read(chunk.data(), count); output.write(chunk.data(), count);
                Require(input.good() && output.good(), "copy original fragmented payload");
                left -= count;
            }
            return;
        }
        position += size;
    }
    Require(false, "hybrid recording contains original fragmented payload");
}
} // namespace llcv::recording::test
