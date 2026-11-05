#include "probe/movie_frames.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    constexpr unsigned width = 1920;
    constexpr unsigned height = 1080;

    struct Fixture
    {
        std::array<uint8_t, 0xd8> info{};
        std::array<std::vector<uint8_t>, 3> planes;
        std::array<unsigned, 3> pitches{1936, 976, 976};

        template <class T> void field(size_t offset, T value)
        {
            std::memcpy(info.data() + offset, &value, sizeof(value));
        }

        Fixture()
        {
            field(0x10, width);
            field(0x14, height);
            field(0x44, 3u);
            uint32_t random = 0x82025021;
            for (unsigned plane = 0; plane < 3; ++plane)
            {
                const unsigned lines = plane ? height / 2 : height;
                planes[plane].resize(pitches[plane] * lines);
                for (auto& byte : planes[plane])
                {
                    random = random * 1664525u + 1013904223u;
                    byte = static_cast<uint8_t>(random >> 24);
                }
                const auto offset = 0x48 + plane * 24;
                field(offset, planes[plane].data());
                field(offset + 8, static_cast<unsigned>(planes[plane].size()));
                field(offset + 12, pitches[plane]);
                field(offset + 16, plane ? width / 2 : width);
                field(offset + 20, lines);
            }
        }

        auto reference(unsigned color) const -> std::vector<uint8_t>
        {
            std::vector<uint8_t> expected(width * height * 4);
            const bool full = color == 1 || color == 11;
            const bool bt709 = color >= 10;
            auto clamp = [](int value) { return static_cast<uint8_t>(std::clamp(value, 0, 255)); };
            for (unsigned row = 0; row < height; ++row)
            {
                for (unsigned column = 0; column < width; ++column)
                {
                    const int lum = planes[0][row * pitches[0] + column];
                    const int u = int(planes[1][(row / 2) * pitches[1] + column / 2]) - 128;
                    const int v = int(planes[2][(row / 2) * pitches[2] + column / 2]) - 128;
                    const int scaled = full ? 256 * lum : 298 * (lum - 16);
                    const auto offset = (row * width + column) * 4;
                    expected[offset] =
                        clamp((scaled + (bt709 ? (full ? 475 : 541) : (full ? 454 : 516)) * u + 128) >> 8);
                    expected[offset + 1] = clamp((scaled - (bt709 ? (full ? 48 : 55) : (full ? 88 : 100)) * u -
                                                  (bt709 ? (full ? 120 : 136) : (full ? 183 : 208)) * v + 128) >>
                                                 8);
                    expected[offset + 2] =
                        clamp((scaled + (bt709 ? (full ? 403 : 459) : (full ? 359 : 409)) * v + 128) >> 8);
                    expected[offset + 3] = 255;
                }
            }
            return expected;
        }
    };

    template <class F> void rejects(F operation)
    {
        bool rejected = false;
        try
        {
            operation();
        }
        catch (const std::runtime_error&)
        {
            rejected = true;
        }
        assert(rejected);
    }
} // namespace

int main()
{
    Fixture fixture;
    kh02::MovieFrameConverter converter;
    for (unsigned color : {0u, 1u, 10u, 11u})
    {
        fixture.field(0xc8, color);
        const auto actual = converter.convert(fixture.info.data());
        const auto expected = fixture.reference(color);
        assert(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()));
    }
    fixture.field(0xc8, 99u);
    rejects([&] { converter.convert(fixture.info.data()); });
    fixture.field(0xc8, 10u);
    fixture.field(0x48 + 12, 1u);
    rejects([&] { converter.convert(fixture.info.data()); });
    fixture.field(0x48 + 12, fixture.pitches[0]);
    fixture.field(0x48, reinterpret_cast<const uint8_t*>(1));
    rejects([&] { converter.convert(fixture.info.data()); });
    fixture.field(0x48, fixture.planes[0].data());
    assert(converter.allocations() == 4);
    rejects([&] { converter.convert(nullptr); });
    uint64_t checksum = 0;
    for (unsigned trial = 0; trial < 3; ++trial)
    {
        const auto began = std::chrono::steady_clock::now();
        for (unsigned frame = 0; frame < 60; ++frame)
        {
            const auto pixels = converter.convert(fixture.info.data());
            checksum += pixels[frame] + pixels[pixels.size() - 1 - frame];
        }
        const auto elapsed =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - began).count();
        std::cout << "{\"trial\":" << trial << ",\"frames\":60,\"average_us\":" << elapsed / 60
                  << ",\"checksum\":" << checksum << "}\n";
    }
    assert(converter.allocations() == 4);
    assert(converter.converted_frames() == 184);
}
