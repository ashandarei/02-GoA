#define NOMINMAX
#include <Windows.h>
#include "movie_frames.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <emmintrin.h>

namespace kh02
{
    namespace
    {
        template <class T> T field(const uint8_t* info, size_t at)
        {
            T value;
            std::memcpy(&value, info + at, sizeof(value));
            return value;
        }

        // x64 guarantees SSE2. Keep signed 32-bit products and the original
        // rounding before saturating, so all four color matrices remain exact.
        auto multiply_low(__m128i words, int coefficient) -> __m128i
        {
            return _mm_madd_epi16(_mm_unpacklo_epi16(words, _mm_setzero_si128()), _mm_set1_epi32(coefficient));
        }

        auto multiply_high(__m128i words, int coefficient) -> __m128i
        {
            return _mm_madd_epi16(_mm_unpackhi_epi16(words, _mm_setzero_si128()), _mm_set1_epi32(coefficient));
        }

        auto channel(__m128i low, __m128i high) -> __m128i
        {
            const auto rounded_low = _mm_srai_epi32(low, 8);
            const auto rounded_high = _mm_srai_epi32(high, 8);
            return _mm_packus_epi16(_mm_packs_epi32(rounded_low, rounded_high), _mm_setzero_si128());
        }

        template <bool full, bool bt709>
        void convert_pixels(const std::array<std::vector<uint8_t>, 3>& planes, const std::array<unsigned, 3>& pitches,
                            uint8_t* output)
        {
            constexpr unsigned width = 1920;
            constexpr unsigned height = 1080;
            constexpr int luma_scale = full ? 256 : 298;
            constexpr int blue_coefficient = bt709 ? (full ? 475 : 541) : (full ? 454 : 516);
            constexpr int red_coefficient = bt709 ? (full ? 403 : 459) : (full ? 359 : 409);
            constexpr int green_u_coefficient = bt709 ? (full ? 48 : 55) : (full ? 88 : 100);
            constexpr int green_v_coefficient = bt709 ? (full ? 120 : 136) : (full ? 183 : 208);
            const auto zero = _mm_setzero_si128();
            const auto chroma_offset = _mm_set1_epi16(128);
            const auto luma_offset = _mm_set1_epi16(full ? 0 : 16);
            const auto rounding = _mm_set1_epi32(128);
            const auto alpha = _mm_set1_epi8(-1);
            for (unsigned row = 0; row < height; row += 2)
            {
                const auto* u_row = planes[1].data() + (row / 2) * pitches[1];
                const auto* v_row = planes[2].data() + (row / 2) * pitches[2];
                for (unsigned column = 0; column < width; column += 8)
                {
                    uint32_t u_bytes{};
                    uint32_t v_bytes{};
                    std::memcpy(&u_bytes, u_row + column / 2, sizeof(u_bytes));
                    std::memcpy(&v_bytes, v_row + column / 2, sizeof(v_bytes));
                    const auto u_raw = _mm_cvtsi32_si128(static_cast<int>(u_bytes));
                    const auto v_raw = _mm_cvtsi32_si128(static_cast<int>(v_bytes));
                    const auto u =
                        _mm_sub_epi16(_mm_unpacklo_epi8(_mm_unpacklo_epi8(u_raw, u_raw), zero), chroma_offset);
                    const auto v =
                        _mm_sub_epi16(_mm_unpacklo_epi8(_mm_unpacklo_epi8(v_raw, v_raw), zero), chroma_offset);
                    const auto blue_low = _mm_add_epi32(multiply_low(u, blue_coefficient), rounding);
                    const auto blue_high = _mm_add_epi32(multiply_high(u, blue_coefficient), rounding);
                    const auto red_low = _mm_add_epi32(multiply_low(v, red_coefficient), rounding);
                    const auto red_high = _mm_add_epi32(multiply_high(v, red_coefficient), rounding);
                    const auto green_low = _mm_sub_epi32(_mm_sub_epi32(rounding, multiply_low(u, green_u_coefficient)),
                                                         multiply_low(v, green_v_coefficient));
                    const auto green_high =
                        _mm_sub_epi32(_mm_sub_epi32(rounding, multiply_high(u, green_u_coefficient)),
                                      multiply_high(v, green_v_coefficient));
                    // YUV420 shares chroma across both adjacent rows and columns.
                    for (unsigned subrow = 0; subrow < 2; ++subrow)
                    {
                        const auto* luma_row = planes[0].data() + (row + subrow) * pitches[0];
                        const auto lum_raw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(luma_row + column));
                        const auto lum = _mm_sub_epi16(_mm_unpacklo_epi8(lum_raw, zero), luma_offset);
                        const auto lum_low = multiply_low(lum, luma_scale);
                        const auto lum_high = multiply_high(lum, luma_scale);
                        const auto blue = channel(_mm_add_epi32(lum_low, blue_low), _mm_add_epi32(lum_high, blue_high));
                        const auto green =
                            channel(_mm_add_epi32(lum_low, green_low), _mm_add_epi32(lum_high, green_high));
                        const auto red = channel(_mm_add_epi32(lum_low, red_low), _mm_add_epi32(lum_high, red_high));
                        const auto blue_green = _mm_unpacklo_epi8(blue, green);
                        const auto red_alpha = _mm_unpacklo_epi8(red, alpha);
                        auto* destination = reinterpret_cast<__m128i*>(output + ((row + subrow) * width + column) * 4);
                        _mm_storeu_si128(destination, _mm_unpacklo_epi16(blue_green, red_alpha));
                        _mm_storeu_si128(destination + 1, _mm_unpackhi_epi16(blue_green, red_alpha));
                    }
                }
            }
        }
    } // namespace

    auto MovieFrameConverter::convert(const uint8_t* info) -> std::span<const uint8_t>
    {
        constexpr unsigned width = 1920, height = 1080;
        if (!info || field<unsigned>(info, 0x10) != width || field<unsigned>(info, 0x14) != height ||
            field<unsigned>(info, 0x44) != 3)
        {
            throw std::runtime_error("movie_frames_image_layout");
        }
        const auto color = field<unsigned>(info, 0xc8);
        if (color != 0 && color != 1 && color != 10 && color != 11)
        {
            throw std::runtime_error("movie_frames_color_space:" + std::to_string(color));
        }
        std::array<unsigned, 3> pitches{};
        for (unsigned i = 0; i < 3; ++i)
        {
            const auto offset = 0x48 + i * 24;
            const auto* data = field<const uint8_t*>(info, offset);
            const auto bytes = field<unsigned>(info, offset + 8), pitch = field<unsigned>(info, offset + 12);
            const auto line = field<unsigned>(info, offset + 16), lines = field<unsigned>(info, offset + 20),
                       wanted = i ? height / 2 : height;
            if (!data || pitch < (i ? width / 2 : width) || pitch > 4096 || line < (i ? width / 2 : width) ||
                lines < wanted || uint64_t(pitch) * wanted > bytes)
            {
                throw std::runtime_error("movie_frames_plane_layout");
            }
            const auto plane_bytes = size_t(pitch) * wanted;
            buffer_growths += planes[i].capacity() < plane_bytes;
            planes[i].resize(plane_bytes);
            pitches[i] = pitch;
            SIZE_T done{};
            if (!ReadProcessMemory(GetCurrentProcess(), data, planes[i].data(), planes[i].size(), &done) ||
                done != planes[i].size())
            {
                throw std::runtime_error("movie_frames_plane_copy");
            }
        }
        constexpr size_t output_bytes = size_t(width) * height * 4;
        buffer_growths += pixels.capacity() < output_bytes;
        pixels.resize(output_bytes);
        switch (color)
        {
        case 0:
            convert_pixels<false, false>(planes, pitches, pixels.data());
            break;
        case 1:
            convert_pixels<true, false>(planes, pitches, pixels.data());
            break;
        case 10:
            convert_pixels<false, true>(planes, pitches, pixels.data());
            break;
        case 11:
            convert_pixels<true, true>(planes, pitches, pixels.data());
            break;
        }
        ++converted;
        return pixels;
    }

} // namespace kh02
