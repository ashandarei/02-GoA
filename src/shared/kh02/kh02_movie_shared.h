#pragma once
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace kh02movie
{
    inline constexpr uint32_t width = 1920, height = 1080, pitch = width * 4;
    inline constexpr uint32_t frame_bytes = pitch * height, slot_count = 3;
    inline constexpr uint32_t audio_slot_count = 32, audio_packet_frames = 2048, audio_max_channels = 8;
    enum class State : LONG
    {
        preparing,
        ready,
        playing,
        ended,
        canceled,
        failed
    };
    enum class Command : LONG
    {
        paused,
        start,
        cancel
    };

    struct alignas(64) Header
    {
        uint32_t magic, version, owner_pid, source_pid;
        uint32_t image_width, image_height, image_pitch, slots;
        char generation[33];
        volatile LONG state, command, total_frames, fps_numerator, fps_denominator;
        volatile LONG last_frame, published, dropped, released;
        volatile LONG audio_channels, audio_sample_rate, audio_error, audio_published, audio_consumed;
    };

    struct alignas(64) Slot
    {
        volatile LONG state; // 0 free, 1 writing, 2 ready, 3 reading.
        int32_t number;
        uint32_t bytes;
    };

    struct alignas(64) AudioSlot
    {
        volatile LONG state;
        uint32_t frames;
        std::array<float, audio_packet_frames * audio_max_channels> samples;
    };

    inline constexpr size_t video_mapping_bytes = sizeof(Header) + slot_count * (sizeof(Slot) + frame_bytes);
    inline constexpr size_t mapping_bytes = video_mapping_bytes + audio_slot_count * sizeof(AudioSlot);

    inline LONG read(const volatile LONG& value)
    {
        return InterlockedCompareExchange(const_cast<volatile LONG*>(&value), 0, 0);
    }

    class SharedFrames
    {
        HANDLE mapping{}, ready_event{}, control_event{};
        unsigned char* memory{};
        std::string identity;
        uint32_t owner{}, source{};
        auto slot(uint32_t index) const -> Slot*;
        auto audio_slot(uint32_t index) const noexcept -> AudioSlot*;
        bool consume_audio_packet(std::span<float> samples, uint32_t* copied_frames);
        void validate() const;

      public:
        ~SharedFrames();
        SharedFrames() = default;
        SharedFrames(const SharedFrames&) = delete;
        auto operator=(const SharedFrames&) -> SharedFrames& = delete;
        void create(uint32_t source_pid);
        void open(const std::string& name, uint32_t owner_pid);

        auto name() const -> const std::string&
        {
            return identity;
        }

        auto header() const -> Header&;
        void signal(Command command);
        auto command() const -> Command;
        void wait_control(DWORD milliseconds) const;
        void state(State state);
        auto state() const -> State;
        bool publish(int number, std::span<const uint8_t> pixels);
        bool consume(int after, int& number, std::vector<uint8_t>& pixels);
        // Single producer/consumer, bounded RAM only. The decoder callback never waits or allocates.
        bool publish_audio(int channels, int frames, float* const* planar) noexcept;
        bool consume_audio(std::span<float> samples, uint32_t& frames);
        // Same validation and queue advancement, without reading unused PCM.
        bool discard_audio();
    };
} // namespace kh02movie
