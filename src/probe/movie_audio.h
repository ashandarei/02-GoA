#pragma once
#include "shared/kh02/kh02_movie_shared.h"
#include <atomic>
#include <cstdint>

namespace kh02
{
    // Attached only to our scoped movie player, before Start. The player must be
    // stopped and destroyed before this object (including its callback context).
    class MovieAudioTap
    {
        using ReadPcm = int (*)(void*, int, float**, int);
        ReadPcm original{};
        void* original_context{};
        kh02movie::SharedFrames& shared;
        kh02movie::Header* header{};
        int channels{};
        std::atomic<uint64_t> frames{};
        static int read_pcm(void* context, int channels, float** samples, int requested) noexcept;

      public:
        MovieAudioTap(unsigned char* image, void* player, kh02movie::SharedFrames& shared, int channels, int rate);
        auto captured_frames() const -> uint64_t;
    };
} // namespace kh02
