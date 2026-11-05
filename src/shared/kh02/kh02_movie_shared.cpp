#include "kh02_movie_shared.h"
#include <sddl.h>
#include <objbase.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace kh02movie
{
    namespace
    {
        struct UserSecurity
        {
            PSECURITY_DESCRIPTOR descriptor{};
            SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};

            UserSecurity()
            {
                HANDLE token{};
                if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
                {
                    throw std::runtime_error("movie_shared_user_token");
                }
                DWORD bytes{};
                GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
                std::vector<unsigned char> data(bytes);
                const bool queried = GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes);
                CloseHandle(token);
                LPWSTR sid{};
                if (!queried || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid))
                {
                    throw std::runtime_error("movie_shared_user_sid");
                }
                const std::wstring dacl = std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;") + sid + L")";
                LocalFree(sid);
                if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(), SDDL_REVISION_1, &descriptor,
                                                                          nullptr))
                {
                    throw std::runtime_error("movie_shared_user_dacl");
                }
                attributes.lpSecurityDescriptor = descriptor;
            }

            ~UserSecurity()
            {
                LocalFree(descriptor);
            }
        };

        auto wide(const std::string& value) -> std::wstring
        {
            return {value.begin(), value.end()};
        }
    } // namespace

    SharedFrames::~SharedFrames()
    {
        if (memory)
        {
            UnmapViewOfFile(memory);
        }
        for (const auto handle : {mapping, ready_event, control_event})
        {
            if (handle)
            {
                CloseHandle(handle);
            }
        }
    }

    auto SharedFrames::slot(uint32_t index) const -> Slot*
    {
        return reinterpret_cast<Slot*>(memory + sizeof(Header) + index * (sizeof(Slot) + frame_bytes));
    }

    auto SharedFrames::audio_slot(uint32_t index) const noexcept -> AudioSlot*
    {
        return reinterpret_cast<AudioSlot*>(memory + video_mapping_bytes + index * sizeof(AudioSlot));
    }

    void SharedFrames::validate() const
    {
        if (!memory)
        {
            throw std::runtime_error("movie_shared_not_mapped");
        }
        const auto& info = *reinterpret_cast<const Header*>(memory);
        if (info.magic != 0x4d323048 || info.version != 2 || info.owner_pid != owner || info.source_pid != source ||
            info.image_width != width || info.image_height != height || info.image_pitch != pitch ||
            info.slots != slot_count || std::memcmp(info.generation, identity.data() + identity.size() - 32, 32) ||
            info.generation[32])
        {
            throw std::runtime_error("movie_shared_identity_or_layout");
        }
    }

    void SharedFrames::create(uint32_t source_pid)
    {
        if (mapping || !source_pid)
        {
            throw std::runtime_error("movie_shared_create_lifetime");
        }
        owner = GetCurrentProcessId();
        source = source_pid;
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid)))
        {
            throw std::runtime_error("movie_shared_generation");
        }
        constexpr char digits[] = "0123456789abcdef";
        std::string generation;
        for (const auto byte : std::span(reinterpret_cast<const uint8_t*>(&guid), sizeof(guid)))
        {
            generation.push_back(digits[byte >> 4]);
            generation.push_back(digits[byte & 15]);
        }
        identity = "Local\\KH02Movie_" + std::to_string(owner) + "_" + std::to_string(source) + "_" + generation;
        UserSecurity security;
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security.attributes, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(mapping_bytes), wide(identity).c_str());
        if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            throw std::runtime_error("movie_shared_mapping_create");
        }
        memory = static_cast<unsigned char*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapping_bytes));
        ready_event = CreateEventW(&security.attributes, FALSE, FALSE, wide(identity + "_ready").c_str());
        control_event = CreateEventW(&security.attributes, FALSE, FALSE, wide(identity + "_control").c_str());
        if (!memory || !ready_event || !control_event)
        {
            throw std::runtime_error("movie_shared_event_create");
        }
        std::memset(memory, 0, mapping_bytes);
        auto& info = *reinterpret_cast<Header*>(memory);
        info.magic = 0x4d323048;
        info.version = 2;
        info.owner_pid = owner;
        info.source_pid = source;
        info.image_width = width;
        info.image_height = height;
        info.image_pitch = pitch;
        info.slots = slot_count;
        std::memcpy(info.generation, generation.c_str(), 33);
        info.last_frame = -1;
        validate();
    }

    void SharedFrames::open(const std::string& name, uint32_t owner_pid)
    {
        if (mapping)
        {
            throw std::runtime_error("movie_shared_open_lifetime");
        }
        owner = owner_pid;
        source = GetCurrentProcessId();
        const auto prefix = "Local\\KH02Movie_" + std::to_string(owner) + "_" + std::to_string(source) + "_";
        if (!owner || name.size() != prefix.size() + 32 || !name.starts_with(prefix) ||
            !std::all_of(name.begin() + prefix.size(), name.end(),
                         [](char value) { return value >= '0' && value <= '9' || value >= 'a' && value <= 'f'; }))
        {
            throw std::runtime_error("movie_shared_name");
        }
        identity = name;
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide(identity).c_str());
        if (mapping)
        {
            memory = static_cast<unsigned char*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapping_bytes));
        }
        ready_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, wide(identity + "_ready").c_str());
        control_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, wide(identity + "_control").c_str());
        if (!memory || !ready_event || !control_event)
        {
            throw std::runtime_error("movie_shared_open");
        }
        validate();
    }

    auto SharedFrames::header() const -> Header&
    {
        validate();
        return *reinterpret_cast<Header*>(memory);
    }

    void SharedFrames::signal(Command value)
    {
        InterlockedExchange(&header().command, static_cast<LONG>(value));
        SetEvent(control_event);
    }

    auto SharedFrames::command() const -> Command
    {
        const auto value = read(header().command);
        if (value < static_cast<LONG>(Command::paused) || value > static_cast<LONG>(Command::cancel))
        {
            throw std::runtime_error("movie_shared_control_value");
        }
        return static_cast<Command>(value);
    }

    void SharedFrames::wait_control(DWORD milliseconds) const
    {
        WaitForSingleObject(control_event, milliseconds);
    }

    void SharedFrames::state(State value)
    {
        InterlockedExchange(&header().state, static_cast<LONG>(value));
        SetEvent(ready_event);
    }

    auto SharedFrames::state() const -> State
    {
        const auto value = read(header().state);
        if (value < static_cast<LONG>(State::preparing) || value > static_cast<LONG>(State::failed))
        {
            throw std::runtime_error("movie_shared_state_value");
        }
        return static_cast<State>(value);
    }

    bool SharedFrames::publish(int number, std::span<const uint8_t> pixels)
    {
        if (number < 0 || number >= read(header().total_frames) || pixels.size() != frame_bytes)
        {
            throw std::runtime_error("movie_shared_frame_bounds");
        }
        for (uint32_t attempt = 0; attempt < slot_count; ++attempt)
        {
            auto* frame = slot((number + attempt) % slot_count);
            LONG expected = read(frame->state);
            if ((expected != 0 && expected != 2) || InterlockedCompareExchange(&frame->state, 1, expected) != expected)
            {
                continue;
            }
            frame->number = number;
            frame->bytes = frame_bytes;
            std::memcpy(reinterpret_cast<unsigned char*>(frame) + sizeof(Slot), pixels.data(), frame_bytes);
            InterlockedExchange(&frame->state, 2);
            InterlockedExchange(&header().last_frame, number);
            InterlockedIncrement(&header().published);
            SetEvent(ready_event);
            return true;
        }
        InterlockedIncrement(&header().dropped);
        return false;
    }

    bool SharedFrames::consume(int after, int& number, std::vector<uint8_t>& pixels)
    {
        const auto total_frames = read(header().total_frames);
        // No waits on the game thread. Slot ownership prevents torn frame copies.
        WaitForSingleObject(ready_event, 0);

        struct ReadingSlot
        {
            Slot* frame{};

            ~ReadingSlot()
            {
                release();
            }

            void release()
            {
                if (frame)
                {
                    InterlockedExchange(&frame->state, 0);
                    frame = nullptr;
                }
            }
        } newest;

        for (uint32_t index = 0; index < slot_count; ++index)
        {
            auto* frame = slot(index);
            if (InterlockedCompareExchange(&frame->state, 3, 2) != 2)
            {
                continue;
            }
            if (frame->bytes != frame_bytes || frame->number < 0 || frame->number >= total_frames)
            {
                InterlockedExchange(&frame->state, 0);
                throw std::runtime_error("movie_shared_frame_header");
            }
            if (frame->number > after && (!newest.frame || frame->number > newest.frame->number))
            {
                newest.release();
                newest.frame = frame;
            }
            else
            {
                InterlockedExchange(&frame->state, 0);
            }
        }
        if (!newest.frame)
        {
            return false;
        }
        // Select metadata while holding slot ownership, then copy only the frame
        // that will be displayed. Allocation failures also release the slot.
        pixels.resize(frame_bytes);
        std::memcpy(pixels.data(), reinterpret_cast<const unsigned char*>(newest.frame) + sizeof(Slot), frame_bytes);
        number = newest.frame->number;
        return true;
    }

    bool SharedFrames::publish_audio(int channels, int frames, float* const* planar) noexcept
    {
        auto& info = *reinterpret_cast<Header*>(memory);
        if (channels < 1 || channels > audio_max_channels || frames < 0 || !planar ||
            channels != read(info.audio_channels))
        {
            InterlockedExchange(&info.audio_error, 1);
            return false;
        }
        for (int channel = 0; channel < channels; ++channel)
        {
            if (!planar[channel])
            {
                InterlockedExchange(&info.audio_error, 1);
                return false;
            }
        }
        int offset = 0;
        while (offset < frames)
        {
            const auto sequence = static_cast<uint32_t>(read(info.audio_published));
            auto* packet = audio_slot(sequence % audio_slot_count);
            if (InterlockedCompareExchange(&packet->state, 1, 0) != 0)
            {
                InterlockedExchange(&info.audio_error, 2);
                return false;
            }
            const auto count = std::min<uint32_t>(frames - offset, audio_packet_frames);
            for (uint32_t frame = 0; frame < count; ++frame)
            {
                for (int channel = 0; channel < channels; ++channel)
                {
                    packet->samples[frame * channels + channel] = planar[channel][offset + frame];
                }
            }
            packet->frames = count;
            InterlockedExchange(&packet->state, 2);
            InterlockedIncrement(&info.audio_published);
            offset += count;
        }
        return true;
    }

    bool SharedFrames::consume_audio(std::span<float> samples, uint32_t& frames)
    {
        return consume_audio_packet(samples, &frames);
    }

    bool SharedFrames::discard_audio()
    {
        return consume_audio_packet({}, nullptr);
    }

    bool SharedFrames::consume_audio_packet(std::span<float> samples, uint32_t* copied_frames)
    {
        auto& info = header();
        const auto channels = read(info.audio_channels);
        const auto sequence = static_cast<uint32_t>(read(info.audio_consumed));
        auto* packet = audio_slot(sequence % audio_slot_count);
        if (InterlockedCompareExchange(&packet->state, 3, 2) != 2)
        {
            return false;
        }
        if (channels < 1 || channels > audio_max_channels || packet->frames < 1 ||
            packet->frames > audio_packet_frames ||
            (copied_frames && samples.size() < packet->frames * channels))
        {
            InterlockedExchange(&packet->state, 0);
            throw std::runtime_error("movie_shared_audio_packet");
        }
        if (copied_frames)
        {
            *copied_frames = packet->frames;
            std::memcpy(samples.data(), packet->samples.data(), *copied_frames * channels * sizeof(float));
        }
        InterlockedExchange(&packet->state, 0);
        InterlockedIncrement(&info.audio_consumed);
        return true;
    }
} // namespace kh02movie
