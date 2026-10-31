#pragma once
#include "recording/RecordingBufferPool.h"
#include "recording/RgbRecordingPolicy.h"
#include <chrono>
#include <atomic>
#include <array>
#include <algorithm>
#include <cmath>
#include <mferror.h>
#include <immintrin.h>
#include <intrin.h>

namespace llcv::recording {
// Recording-worker only. Native packed BGR/BGRX -> top-down BT.709 Limited NV12.
// No capture/render-thread conversion, shared D3D context or GPU readback.
class RgbRecordingConverter {
    RecordingBufferPool buffers_;
    std::array<std::array<int, 256>, 3> y_{}, u_{}, v_{};
    int width_ = 0, height_ = 0, step_ = 0;
    int outWidth_ = 0, outHeight_ = 0;
    struct Tap {
        int a, b, weight;
    };
    std::vector<Tap> x_, rows_;
    bool topDown_ = true;
    bool limited_ = false;
    bool useSimd_ = true;
    static bool FastAvailable() noexcept {
        int c[4]{};
        __cpuid(c, 0);
        if (c[0] < 7)
            return false;
        __cpuid(c, 1);
        if (!(c[2] & (1 << 27)) || !(c[2] & (1 << 28)) || (_xgetbv(0) & 6) != 6)
            return false;
        __cpuidex(c, 7, 0);
        return (c[1] & (1 << 5)) != 0;
    }
    // Explicitly gated and not inlined into the portable scalar entry point.
    // No /arch:AVX2 requirement is imposed on the rest of the application.
    __declspec(noinline) int FastRows(const BYTE* src, BYTE* dst, int row, bool half) const {
        const int stride = width_ * step_, advance = step_ * (half ? 2 : 1);
        const __m256i indices = _mm256_setr_epi32(0, advance, 2 * advance, 3 * advance, 4 * advance,
                                                  5 * advance, 6 * advance, 7 * advance);
        const __m256i mask = _mm256_set1_epi32(255);
        const __m256 zero = _mm256_setzero_ps();
        int x = 0;
        // Leave a scalar tail so the final BGR24 gather never reads beyond a row.
        for (; x + 8 < outWidth_; x += 8) {
            __m256 uu[2], vv[2];
            for (int dy = 0; dy < 2; ++dy) {
                const int sy = (row + dy) * (half ? 2 : 1);
                const int a = topDown_ ? sy : height_ - 1 - sy;
                const int b = topDown_ ? sy + 1 : height_ - 2 - sy;
                const BYTE* pa = src + static_cast<size_t>(a) * stride + x * advance;
                const __m256i first =
                    _mm256_i32gather_epi32(reinterpret_cast<const int*>(pa), indices, 1);
                __m256i extra[3]{};
                if (half) {
                    const BYTE* pb = src + static_cast<size_t>(b) * stride + x * advance;
                    extra[0] = _mm256_i32gather_epi32(reinterpret_cast<const int*>(pa + step_),
                                                      indices, 1);
                    extra[1] = _mm256_i32gather_epi32(reinterpret_cast<const int*>(pb), indices, 1);
                    extra[2] = _mm256_i32gather_epi32(reinterpret_cast<const int*>(pb + step_),
                                                      indices, 1);
                }
                __m256 channels[3];
                for (int k = 0; k < 3; ++k) {
                    const __m128i shift = _mm_cvtsi32_si128(k * 8);
                    __m256i n = _mm256_and_si256(_mm256_srl_epi32(first, shift), mask);
                    if (half) {
                        for (const auto& item : extra)
                            n = _mm256_add_epi32(
                                n, _mm256_and_si256(_mm256_srl_epi32(item, shift), mask));
                        n = _mm256_srli_epi32(_mm256_add_epi32(n, _mm256_set1_epi32(2)), 2);
                    }
                    auto value = _mm256_cvtepi32_ps(n);
                    if (limited_)
                        value = _mm256_min_ps(
                            _mm256_set1_ps(255),
                            _mm256_max_ps(zero,
                                          _mm256_mul_ps(_mm256_sub_ps(value, _mm256_set1_ps(16)),
                                                        _mm256_set1_ps(255.0f / 219))));
                    channels[k] = value;
                }
                const auto lum =
                    _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(channels[2], _mm256_set1_ps(.2126f)),
                                                _mm256_mul_ps(channels[1], _mm256_set1_ps(.7152f))),
                                  _mm256_mul_ps(channels[0], _mm256_set1_ps(.0722f)));
                const auto yy = _mm256_add_ps(_mm256_set1_ps(16),
                                              _mm256_mul_ps(lum, _mm256_set1_ps(219.0f / 255)));
                alignas(32) int values[8];
                _mm256_store_si256(reinterpret_cast<__m256i*>(values), _mm256_cvtps_epi32(yy));
                for (int i = 0; i < 8; ++i)
                    dst[static_cast<size_t>(row + dy) * outWidth_ + x + i] =
                        static_cast<BYTE>(std::clamp(values[i], 16, 235));
                uu[dy] = _mm256_mul_ps(_mm256_sub_ps(channels[0], lum),
                                       _mm256_set1_ps(224.0f / (255 * 1.8556f)));
                vv[dy] = _mm256_mul_ps(_mm256_sub_ps(channels[2], lum),
                                       _mm256_set1_ps(224.0f / (255 * 1.5748f)));
            }
            alignas(32) float us[8], vs[8];
            _mm256_store_ps(us, _mm256_add_ps(uu[0], uu[1]));
            _mm256_store_ps(vs, _mm256_add_ps(vv[0], vv[1]));
            BYTE* uv = dst + static_cast<size_t>(outWidth_) * outHeight_ +
                       static_cast<size_t>(row / 2) * outWidth_ + x;
            for (int i = 0; i < 8; i += 2) {
                uv[i] = static_cast<BYTE>(
                    std::clamp(128 + int(std::lround((us[i] + us[i + 1]) * .25f)), 16, 240));
                uv[i + 1] = static_cast<BYTE>(
                    std::clamp(128 + int(std::lround((vs[i] + vs[i + 1]) * .25f)), 16, 240));
            }
        }
        _mm256_zeroupper();
        return x;
    }

public:
    void Clear() noexcept {
        buffers_.Clear();
        x_.clear();
        rows_.clear();
        width_ = height_ = step_ = outWidth_ = outHeight_ = 0;
    }
    void Configure(int width, int height, int step, bool topDown, bool limited, int outWidth = 0,
                   int outHeight = 0, bool useSimd = true) {
        Clear();
        if (width <= 0 || height <= 0 || width > 3840 || height > 2160 || (width & 1) ||
            (height & 1) || (step != 3 && step != 4))
            throw E_INVALIDARG;
        width_ = width;
        height_ = height;
        step_ = step;
        topDown_ = topDown;
        limited_ = limited;
        useSimd_ = useSimd;
        outWidth_ = outWidth ? outWidth : width;
        outHeight_ = outHeight ? outHeight : height;
        if (outWidth_ <= 0 || outHeight_ <= 0 || outWidth_ > width || outHeight_ > height ||
            (outWidth_ & 1) || (outHeight_ & 1))
            throw E_INVALIDARG;
        auto taps = [](int source, int target) {
            std::vector<Tap> result;
            result.reserve(target);
            for (int i = 0; i < target; ++i) {
                const auto p = std::clamp(((int64_t(i) * 2 + 1) * source * 128 / target) - 128,
                                          int64_t{0}, int64_t(source - 1) * 256);
                result.push_back(
                    {int(p / 256), std::min(source - 1, int(p / 256) + 1), int(p % 256)});
            }
            return result;
        };
        x_ = taps(width, outWidth_);
        rows_ = taps(height, outHeight_);
        constexpr double kr = .2126, kb = .0722, kg = 1 - kr - kb;
        const double yc[] = {kr, kg, kb};
        const double uc[] = {-kr / (2 * (1 - kb)), -kg / (2 * (1 - kb)), .5};
        const double vc[] = {.5, -kg / (2 * (1 - kr)), -kb / (2 * (1 - kr))};
        for (int c = 0; c < 3; ++c)
            for (int n = 0; n < 256; ++n) {
                const double value = limited ? std::clamp((n - 16) * 255.0 / 219.0, 0.0, 255.0) : n;
                y_[c][n] = static_cast<int>(std::lround(value * yc[c] * 219 / 255 * 65536));
                u_[c][n] = static_cast<int>(std::lround(value * uc[c] * 224 / 255 * 65536));
                v_[c][n] = static_cast<int>(std::lround(value * vc[c] * 224 / 255 * 65536));
            }
        const DWORD bytes = static_cast<DWORD>(outWidth_ * outHeight_ * 3 / 2);
        const size_t count = RgbBufferCount(bytes, kRgbOutputBudget);
        if (count < 4)
            throw E_OUTOFMEMORY;
        buffers_.Allocate(count, bytes);
    }
    HRESULT Preflight(IMFMediaBuffer* input, int fps, const std::atomic<bool>& stop,
                      RgbPreflight& result) {
        if (fps <= 0)
            return E_INVALIDARG;
        result = {};
        result.budgetMs = 1000.0 / fps;
        double sum = 0;
        for (unsigned i = 0; i < 7; ++i) {
            if (stop.load())
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            const auto begin = std::chrono::steady_clock::now();
            Microsoft::WRL::ComPtr<IMFMediaBuffer> output;
            const HRESULT hr = Apply(input, &output);
            if (FAILED(hr))
                return hr;
            const double elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                    .count();
            if (i) {
                sum += elapsed;
                ++result.samples;
                result.maximumMs = std::max(result.maximumMs, elapsed);
            }
            if (result.samples >= 3 && sum > 250)
                break;
        }
        result.meanMs = result.samples ? sum / result.samples : 0;
        return S_OK;
    }
    HRESULT Apply(IMFMediaBuffer* input, IMFMediaBuffer** output) {
        if (!input || !output)
            return E_POINTER;
        *output = nullptr;
        if (!width_)
            return E_UNEXPECTED;
        auto frame = buffers_.Acquire();
        if (!frame)
            return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
        BYTE *src = nullptr, *dst = nullptr;
        DWORD capacity = 0, length = 0;
        HRESULT hr = input->Lock(&src, &capacity, &length);
        if (FAILED(hr))
            return hr;
        const size_t pitch = static_cast<size_t>(width_) * step_;
        if (!src || length > capacity || length < pitch * height_)
            hr = MF_E_INVALID_STREAM_DATA;
        if (SUCCEEDED(hr))
            hr = frame->Lock(&dst, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            const auto round = [](int value, int denominator) {
                return value >= 0 ? (value + denominator / 2) / denominator
                                  : -((-value + denominator / 2) / denominator);
            };
            const bool scaled = width_ != outWidth_ || height_ != outHeight_;
            const bool half = width_ == outWidth_ * 2 && height_ == outHeight_ * 2;
            static const bool fast = FastAvailable();
            for (int row = 0; row < outHeight_; row += 2) {
                const int start =
                    useSimd_ && fast && (!scaled || half) ? FastRows(src, dst, row, half) : 0;
                for (int x = start; x < outWidth_; x += 2) {
                    int us = 0, vs = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            int c[3];
                            if (!scaled) {
                                const int sy = topDown_ ? row + dy : height_ - 1 - row - dy;
                                const BYTE* p =
                                    src + static_cast<size_t>(sy) * pitch + (x + dx) * step_;
                                c[0] = p[0];
                                c[1] = p[1];
                                c[2] = p[2];
                            } else if (half) {
                                const int sy = (row + dy) * 2;
                                const int a = topDown_ ? sy : height_ - 1 - sy,
                                          b = topDown_ ? sy + 1 : height_ - 2 - sy;
                                const BYTE* pa =
                                    src + static_cast<size_t>(a) * pitch + (x + dx) * 2 * step_;
                                const BYTE* pb =
                                    src + static_cast<size_t>(b) * pitch + (x + dx) * 2 * step_;
                                for (int k = 0; k < 3; ++k)
                                    c[k] = (pa[k] + pa[step_ + k] + pb[k] + pb[step_ + k] + 2) >> 2;
                            } else {
                                const auto& tx = x_[x + dx];
                                const auto& ty = rows_[row + dy];
                                const int a = topDown_ ? ty.a : height_ - 1 - ty.a,
                                          b = topDown_ ? ty.b : height_ - 1 - ty.b;
                                const BYTE* pa = src + static_cast<size_t>(a) * pitch;
                                const BYTE* pb = src + static_cast<size_t>(b) * pitch;
                                for (int k = 0; k < 3; ++k) {
                                    const int top = pa[tx.a * step_ + k] * (256 - tx.weight) +
                                                    pa[tx.b * step_ + k] * tx.weight;
                                    const int bottom = pb[tx.a * step_ + k] * (256 - tx.weight) +
                                                       pb[tx.b * step_ + k] * tx.weight;
                                    c[k] = (top * (256 - ty.weight) + bottom * ty.weight + 32768) >>
                                           16;
                                }
                            }
                            const int r = c[2], g = c[1], b = c[0];
                            const int yy = y_[0][r] + y_[1][g] + y_[2][b];
                            dst[static_cast<size_t>(row + dy) * outWidth_ + x + dx] =
                                static_cast<BYTE>(std::clamp(16 + round(yy, 65536), 16, 235));
                            us += u_[0][r] + u_[1][g] + u_[2][b];
                            vs += v_[0][r] + v_[1][g] + v_[2][b];
                        }
                    const size_t uv = static_cast<size_t>(outWidth_) * outHeight_ +
                                      static_cast<size_t>(row / 2) * outWidth_ + x;
                    dst[uv] = static_cast<BYTE>(std::clamp(128 + round(us, 4 * 65536), 16, 240));
                    dst[uv + 1] =
                        static_cast<BYTE>(std::clamp(128 + round(vs, 4 * 65536), 16, 240));
                }
            }
            hr = frame->Unlock();
        }
        const HRESULT unlocked = input->Unlock();
        if (SUCCEEDED(hr))
            hr = unlocked;
        if (SUCCEEDED(hr))
            hr = frame->SetCurrentLength(static_cast<DWORD>(outWidth_ * outHeight_ * 3 / 2));
        if (SUCCEEDED(hr))
            *output = frame.Detach();
        return hr;
    }
};
}
