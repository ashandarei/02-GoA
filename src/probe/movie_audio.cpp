#include "shared/kh02/kh02_memory_signature.h"
#include "build_profile.h"
#include "movie_audio.h"
#include <cstring>
#include <stdexcept>

namespace kh02
{
    namespace
    {
        template <typename T> auto field(void* object, size_t offset) -> T
        {
            T value{};
            std::memcpy(&value, static_cast<unsigned char*>(object) + offset, sizeof(value));
            return value;
        }
    } // namespace

    MovieAudioTap::MovieAudioTap(unsigned char* image, void* player, kh02movie::SharedFrames& transport,
                                 int channel_count, int rate)
        : shared(transport), channels(channel_count)
    {
        // This is the already verified 2024 shipping image's float PCM renderer.
        // Register a callback on our own player; no detours, codec replacement,
        // private decoder initialization or process-wide audio interception.
        constexpr auto registration_signature = "bcbf46eb467c897c3eb846a0f6e090bbc339d451c5ebe7fbc0dd9ddeb3dd1de7";
        constexpr auto read_signature = "9267165084757c2546644f7e7ed99e80b21e12cb41870d893fa3b8fd3aab9580";
        if (!kh02integrity::signature_matches(image + kh02build::rva(0x3470940), 10, registration_signature) ||
            !kh02integrity::signature_matches(image + kh02build::rva(0x345f400), 11, read_signature))
        {
            throw std::runtime_error("movie_audio_native_signature");
        }
        if ((channels != 1 && channels != 2 && channels != 6) || rate < 8000 || rate > 192000)
        {
            throw std::runtime_error("movie_audio_format_not_audited:channels=" + std::to_string(channels) +
                                     ":rate=" + std::to_string(rate));
        }
        const unsigned char channel_order[] = {0, 1, 4, 5, 2, 3};
        if (std::memcmp(image + kh02build::rva(0x41f9c98), channel_order, sizeof(channel_order)))
        {
            throw std::runtime_error("movie_audio_channel_order_signature");
        }
        auto* renderer = field<void*>(player, 0x70);
        auto* core = field<void*>(player, 0x60);
        if (!renderer || !core || field<void*>(renderer, 0) != image + kh02build::rva(0x41f7f28) ||
            field<void*>(core, 0x500) != renderer ||
            field<void*>(renderer, 0x3b0) != image + kh02build::rva(0x345f400) || field<void*>(renderer, 0x3a8) != core)
        {
            throw std::runtime_error("movie_audio_player_profile");
        }
        original = reinterpret_cast<ReadPcm>(image + kh02build::rva(0x345f400));
        original_context = core;
        auto& info = shared.header();
        header = &info;
        InterlockedExchange(&info.audio_channels, channels);
        InterlockedExchange(&info.audio_sample_rate, rate);
        using Register = void (*)(void*, ReadPcm, void*);
        reinterpret_cast<Register>(image + kh02build::rva(0x3470940))(renderer, &read_pcm, this);
    }

    int MovieAudioTap::read_pcm(void* context, int channel_count, float** samples, int requested) noexcept
    {
        auto& tap = *static_cast<MovieAudioTap*>(context);
        const int count = tap.original(tap.original_context, channel_count, samples, requested);
        if (count < 0 || count > requested || requested < 0 || requested > 65536 || channel_count != tap.channels ||
            !samples)
        {
            InterlockedExchange(&tap.header->audio_error, 3);
            return 0;
        }
        if (count > 0)
        {
            // Match the shipping renderer's CRI -> Windows 5.1 channel mapping:
            // L, R, surround L, surround R, center, LFE -> L, R, center, LFE, rear L, rear R.
            float* windows_channels[6]{};
            constexpr int channel_order[] = {0, 1, 4, 5, 2, 3};
            for (int channel = 0; channel < channel_count; ++channel)
            {
                const int output_channel = channel_count == 6 ? channel_order[channel] : channel;
                windows_channels[output_channel] = samples[channel];
            }
            tap.shared.publish_audio(channel_count, count, windows_channels);
            tap.frames.fetch_add(count, std::memory_order_relaxed);
        }
        // Preserve the original reader's sample count and timing, but give the
        // source renderer silence. KH3 owns playback of the copied PCM in RAM.
        for (int channel = 0; channel < channel_count; ++channel)
        {
            if (samples[channel])
            {
                std::memset(samples[channel], 0, static_cast<size_t>(requested) * sizeof(float));
            }
        }
        return count;
    }

    auto MovieAudioTap::captured_frames() const -> uint64_t
    {
        return frames.load(std::memory_order_relaxed);
    }
} // namespace kh02
