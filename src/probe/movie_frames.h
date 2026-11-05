#pragma once
#include <cstdint>
#include <array>
#include <span>
#include <vector>

namespace kh02
{
    // Own scratch buffers for one decoder. Returned pixels live until convert()
    // runs again; callers copy/publish them before releasing the native frame.
    class MovieFrameConverter
    {
        std::array<std::vector<uint8_t>, 3> planes;
        std::vector<uint8_t> pixels;
        uint64_t converted{}, buffer_growths{};

      public:
        auto convert(const uint8_t* frame_info) -> std::span<const uint8_t>;

        auto converted_frames() const -> uint64_t
        {
            return converted;
        }

        auto allocations() const -> uint64_t
        {
            return buffer_growths;
        }
    };
} // namespace kh02
