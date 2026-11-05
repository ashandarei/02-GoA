#include "shared/kh02/kh02_memory_signature.h"
#include "movie_service.h"
#include "movie_frames.h"
#include "movie_audio.h"
#include "shared/kh02/kh02_movie_shared.h"
#include "kh02/story_movie_metadata.h"
#include <algorithm>
#include "kh3ap/core/uobject_validity.h"
#include <Unreal/UObject.hpp>
#define NOMINMAX
#include <Windows.h>
#include "build_profile.h"
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace kh02
{
    namespace
    {
        using json = nlohmann::json;

        template <class T> T native(unsigned char* image, size_t rva)
        {
            return reinterpret_cast<T>(image + rva);
        }

        void verify_movie_signature(unsigned char* image, size_t rva, size_t bytes, std::string_view sha256)
        {
            if (!kh02integrity::signature_matches(image + rva, bytes, sha256))
            {
                throw std::runtime_error("movie_native_signature");
            }
        }

        void publish_movie_result(const std::filesystem::path& directory, const json& value)
        {
            const auto temporary = directory / L"movie_probe_result.json.tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out << value.dump();
                out.flush();
                if (!out)
                {
                    throw std::runtime_error("movie_stream_publish");
                }
            }
            for (int attempt = 0; attempt < 50; ++attempt)
            {
                if (MoveFileExW(temporary.c_str(), (directory / L"movie_probe_result.json").c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    return;
                }
                const auto error = GetLastError();
                if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)
                {
                    break;
                }
                Sleep(2);
            }
            throw std::runtime_error("movie_stream_commit");
        }

        json prepare_probe(const std::filesystem::path& directory, const std::string& movie, bool initialize_ui,
                           bool decode, bool streaming = false, bool audible = false,
                           kh02movie::SharedFrames* shared = nullptr)
        {
            auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
            const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
            if (nt->FileHeader.TimeDateStamp != kh02build::current().timestamp ||
                nt->OptionalHeader.SizeOfImage != kh02build::current().image_size)
            {
                throw std::runtime_error("movie_image_profile");
            }
            if (*reinterpret_cast<int*>(image + kh02build::rva(0x48bf920)) != 1)
            {
                throw std::runtime_error("movie_cri_not_initialized");
            }
            verify_movie_signature(image, kh02build::rva(0x3457a50), 12,
                                   "a17908b2262131b289e0e21c2bd5954f0039c7af26128b58463bbbee841e1be5");
            verify_movie_signature(image, kh02build::rva(0x34581a0), 9,
                                   "6e541813d204757cb67f7510a0235066ff8db31e698272dc69c0fe5d052452a1");
            for (auto rva : {kh02build::rva(0x34588b0), kh02build::rva(0x345a4c0), kh02build::rva(0x345bf20)})
            {
                verify_movie_signature(image, rva, 9,
                                       "eb6c63c6563438b09f73c2067e74624f2d9872c94cc4b0a0bf43f8d60598b9d9");
            }
            verify_movie_signature(image, kh02build::rva(0x3459e20), 9,
                                   "9405b3e222255a6694911e3a1c6e7456a25df44310978d40048181c31c648814");
            verify_movie_signature(image, kh02build::rva(0x345b0d0), 12,
                                   "3b0f957f54ee1af48991ea86b5ebfd0bf40f45277137ad41235ba76fccbfeb28");
            verify_movie_signature(image, kh02build::rva(0x3458e40), 12,
                                   "df03191d50a061709f25f57e002557f74d03cd64616f8e7e7128a86df1201f1f");
            if (decode || streaming)
            {
                for (auto rva : {kh02build::rva(0x345be70), kh02build::rva(0x345c030)})
                {
                    verify_movie_signature(image, rva, 9,
                                           "eb6c63c6563438b09f73c2067e74624f2d9872c94cc4b0a0bf43f8d60598b9d9");
                }
                verify_movie_signature(image, kh02build::rva(0x34589b0), 9,
                                       "eb6c63c6563438b09f73c2067e74624f2d9872c94cc4b0a0bf43f8d60598b9d9");
                verify_movie_signature(image, kh02build::rva(0x345a560), 12,
                                       "df03191d50a061709f25f57e002557f74d03cd64616f8e7e7128a86df1201f1f");
                verify_movie_signature(image, kh02build::rva(0x345a0e0), 12,
                                       "df03191d50a061709f25f57e002557f74d03cd64616f8e7e7128a86df1201f1f");
                verify_movie_signature(image, kh02build::rva(0x3458490), 12,
                                       "d9237247610ceecbc2758f0007e0c0d7dcf6e4045d1a7763b12db2de4e694c15");
                verify_movie_signature(image, kh02build::rva(0x345a3f0), 9,
                                       "2b3657b3546b4d8139df8b393ddb945a3baf9cf206eba0904b1adf279b4a4819");
                verify_movie_signature(image, kh02build::rva(0x345a450), 13,
                                       "1d7d58e4ccf647972d1c4f4014ceb44573991c5a8b834cf10cef66684dcae19d");
                verify_movie_signature(image, kh02build::rva(0x345a1d0), 9,
                                       "06c7ff36ec22cdfc78abb2e36291b64ba60c68006221ab10900edcf533717bb2");
                verify_movie_signature(image, kh02build::rva(0x345be10), 11,
                                       "36ca5e368f4c2dc26f31779903cb60693cd79e64fa5ea96c30f94dfb5e0c3010");
            }
            const bool gear =
                movie == "gear_a.usm" || movie == "gear_b.usm" || movie == "gear_c.usm" || movie == "gear_d.usm";
            const kh02movie_metadata::Movie* story = nullptr;
            for (const auto& candidate : kh02movie_metadata::stories)
            {
                if (movie == candidate.file)
                {
                    story = &candidate;
                }
            }
            if (!gear && !story)
            {
                throw std::runtime_error("movie_not_allowlisted");
            }
            if (story && decode && !streaming)
            {
                throw std::runtime_error("story_movie_requires_streaming_decoder");
            }
            const auto file =
                directory.parent_path().parent_path().parent_path() / L"Content" / L"CriMovie" / L"main" / movie;
            const auto bytes = std::filesystem::file_size(file);
            if (bytes < 1000000 || bytes > (gear ? 32000000ULL : 1024ULL * 1024 * 1024) ||
                story && bytes != story->bytes)
            {
                throw std::runtime_error("movie_file_size");
            }
            const auto utf8 = file.u8string();
            const std::string path(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            static bool original_setup_completed = false;
            static uint32_t original_setup_count = 0;
            const bool perform_setup = initialize_ui && !original_setup_completed;
            if (perform_setup)
            {
                // Execute the game's complete, original movie-UI initialization path.
                // Its private decoder setup remains original executable code/data; do
                // not extract its constants or reproduce that setup in this probe.
                verify_movie_signature(image, kh02build::rva(0x45a1d0), 10,
                                       "8bf635ed5e65cd718194ff001a4d3e4e0bd4143af628da0d430de1692649e902");
                if (&kh02build::current() == &kh02integrity::steam)
                {
                    verify_movie_signature(image, kh02build::rva(0x45a400), 9,
                                           "2d326989f0bb66b9dbfc4c3d3cdbc3c1c7e47f3694a994c9f04424c38e497d4d");
                }
                else
                {
                    verify_movie_signature(image, kh02build::rva(0x45a400), 9,
                                           "80c59ada57e0b414cc9bddbe7732ac057a46c8689457d5378d25ede78e495955");
                }
                verify_movie_signature(image, kh02build::rva(0x54b250), 5,
                                       "7d3da8db19f61ef7ce0c397dc55babf303f3a35c7d7e7e46a59f3fc7310e211e");
                auto* ui = native<RC::Unreal::UObject* (*)()>(image, kh02build::rva(0x54b250))();
                if (!kh3ap::is_uobject_valid_fast(ui))
                {
                    throw std::runtime_error("movie_ui_manager_not_ready");
                }
                const std::wstring logical = L"Rebellis02/gear_a.usm";

                struct StringView
                {
                    const wchar_t* data;
                    int count, capacity;
                };

                const StringView name{logical.c_str(), static_cast<int>(logical.size() + 1),
                                      static_cast<int>(logical.size() + 1)};
                native<void (*)(const StringView*, bool)>(image, kh02build::rva(0x45a1d0))(&name, true);
                native<void (*)()>(image, kh02build::rva(0x45a400))();
                if (!*reinterpret_cast<void**>(image + kh02build::rva(0x4537720)))
                {
                    throw std::runtime_error("movie_original_setup_not_completed");
                }
                // StopVideo retires the UI player to a later UI tick. This process is
                // intentionally parked before normal startup, so repeatedly creating
                // that bootstrap player exhausts CRI's finite loader handles.
                original_setup_completed = true;
                ++original_setup_count;
            }
            // Audited original CRI public APIs. No key or codec implementation leaves
            // the original EXE. The optional decode diagnostic consumes original frames
            // with audio volume zero; it never reports playback in the KH3 viewport.
            auto create = native<void* (*)(const void*, void*, int)>(image, kh02build::rva(0x3457a50));
            auto destroy = native<void (*)(void*)>(image, kh02build::rva(0x34581a0));
            auto execute = native<void (*)(void*)>(image, kh02build::rva(0x34588b0));
            auto status = native<int (*)(void*)>(image, kh02build::rva(0x3459e20));
            auto prepare = native<void (*)(void*)>(image, kh02build::rva(0x345a4c0));
            auto stop = native<void (*)(void*)>(image, kh02build::rva(0x345bf20));
            auto set_file = native<void (*)(void*, void*, const char*)>(image, kh02build::rva(0x345b0d0));
            auto info = native<int (*)(void*, void*)>(image, kh02build::rva(0x3458e40));
            void* player = create(nullptr, nullptr, 0);
            if (!player)
            {
                const auto* message = reinterpret_cast<const char*>(image + kh02build::rva(0x4883800));
                throw std::runtime_error("movie_create_failed:" + std::string(message, strnlen_s(message, 512)));
            }

            // Declared first so the callback context outlives PlayerGuard's Stop/Destroy.
            std::unique_ptr<MovieAudioTap> movie_audio;

            struct PlayerGuard
            {
                void* player;
                decltype(stop) stop_fn;
                decltype(execute) execute_fn;
                decltype(status) status_fn;
                decltype(destroy) destroy_fn;

                ~PlayerGuard()
                {
                    if (!player)
                    {
                        return;
                    }
                    stop_fn(player);
                    const auto since = std::chrono::steady_clock::now();
                    while (status_fn(player) != 0 && std::chrono::steady_clock::now() - since < std::chrono::seconds(3))
                    {
                        execute_fn(player);
                        Sleep(5);
                    }
                    destroy_fn(player);
                }
            } guard{player, stop, execute, status, destroy};

            json result = {
                {"movie", movie},
                {"prepared", false},
                {"original_ui_setup_requested", initialize_ui},
                {"original_ui_setup_performed", perform_setup},
                {"original_ui_setup_count", original_setup_count},
                {"original_movie_setup_ready", *reinterpret_cast<void**>(image + kh02build::rva(0x4537720)) != nullptr},
                {"normal_startup_blocked", true},
                {"payload_files_written", 0},
                {"events", json::array()}};
            const auto start = std::chrono::steady_clock::now();
            set_file(player, nullptr, path.c_str());
            prepare(player);
            int previous = -1;
            while (std::chrono::steady_clock::now() - start < std::chrono::seconds(12))
            {
                if (shared && shared->command() == kh02movie::Command::cancel)
                {
                    throw std::runtime_error("movie_prepare_canceled");
                }
                execute(player);
                const int current = status(player);
                const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                        .count();
                if (current != previous)
                {
                    result["events"].push_back({{"at_ms", elapsed}, {"status", current}});
                }
                previous = current;
                if (current == 4)
                {
                    result["prepared"] = true;
                    result["prepared_ms"] = elapsed;
                    break;
                }
                if (current == 7)
                {
                    break;
                }
                Sleep(5);
            }
            if (previous == 7)
            {
                // Original CRI error callback formats its last message here. Copy only
                // that bounded diagnostic text, never decoder state or private data.
                const auto* text = reinterpret_cast<const char*>(image + kh02build::rva(0x4883800));
                result["decoder_error"] = std::string(text, strnlen_s(text, 512));
            }
            std::array<unsigned char, 0x800> metadata{};
            if (info(player, metadata.data()))
            {
                // The native movie-info layout was audited against both games. Only
                // public dimensions/frame count/codec metadata are serialized.
                const auto* fields = reinterpret_cast<const unsigned*>(metadata.data());
                result["video"] = {{"width", fields[5]},          {"height", fields[6]},
                                   {"display_width", fields[7]},  {"display_height", fields[8]},
                                   {"fps_numerator", fields[10]}, {"fps_denominator", fields[11]},
                                   {"frames", fields[12]},        {"codec", fields[15]},
                                   {"audio_tracks", fields[19]},  {"audio_sample_rate", fields[21]},
                                   {"audio_channels", fields[22]}};
            }
            if (streaming && result["prepared"].get<bool>())
            {
                const auto& v = result.at("video");
                const int total = v.at("frames"), numerator = v.at("fps_numerator"),
                          denominator = v.at("fps_denominator");
                if ((!story && !gear) || v.at("width") != 1920 || v.at("height") != 1080 || total < 1 ||
                    total > 60000 || denominator < 1 || numerator < 1 || double(numerator) / denominator < 20 ||
                    double(numerator) / denominator > 60 || v.value("audio_tracks", 99) > 8)
                {
                    throw std::runtime_error("movie_stream_video_profile");
                }
                if (!shared)
                {
                    throw std::runtime_error("movie_stream_shared_transport_required");
                }
                auto& header = shared->header();
                InterlockedExchange(&header.total_frames, total);
                InterlockedExchange(&header.fps_numerator, numerator);
                InterlockedExchange(&header.fps_denominator, denominator);
                // Insert clips can legitimately have no soundtrack. Present
                // them on the video clock instead of rejecting native prepare.
                audible = audible && v.at("audio_tracks").get<unsigned>() != 0;
                if (audible)
                {
                    if (v.at("audio_tracks") != 1)
                    {
                        throw std::runtime_error("movie_audio_requires_one_track");
                    }
                    movie_audio =
                        std::make_unique<MovieAudioTap>(image, player, *shared, v.at("audio_channels").get<int>(),
                                                        v.at("audio_sample_rate").get<int>());
                }
                result["transport"] = "shared_bgra_v1";
                result["stream"] = {{"natural_end", false},
                                    {"last_frame", -1},
                                    {"audio_volume", 0},
                                    {"audio_transport", audible ? "shared_pcm_f32" : "silent"},
                                    {"cache_limit", kh02movie::slot_count}};
                shared->state(kh02movie::State::ready);
                const auto prepared_at = std::chrono::steady_clock::now();
                MovieFrameConverter converter;
                int64_t conversion_microseconds = 0;
                bool started = false;
                bool canceled = false;
                while (std::chrono::steady_clock::now() - prepared_at < std::chrono::seconds(1800))
                {
                    execute(player);
                    const auto command = shared->command();
                    if (command == kh02movie::Command::cancel)
                    {
                        canceled = true;
                        break;
                    }
                    if (command == kh02movie::Command::start)
                    {
                        started = true;
                        break;
                    }
                    shared->wait_control(10);
                }
                if (!started && !canceled)
                {
                    throw std::runtime_error("movie_stream_start_timeout");
                }
                if (started)
                {
                    auto video = native<int (*)(void*)>(image, kh02build::rva(0x34589b0));
                    auto sync = native<void (*)(void*)>(image, kh02build::rva(0x345c030));
                    auto refer = native<int (*)(void*, void*)>(image, kh02build::rva(0x345a560));
                    auto on_time = native<int (*)(void*, const void*)>(image, kh02build::rva(0x345a0e0));
                    auto discard = native<void (*)(void*, const void*)>(image, kh02build::rva(0x3458490));
                    native<void (*)(void*, int)>(image, kh02build::rva(0x345a3f0))(player, 0);
                    native<void (*)(void*, float)>(image, kh02build::rva(0x345be10))(player, 0.f);
                    native<void (*)(void*)>(image, kh02build::rva(0x345be70))(player);
                    shared->state(kh02movie::State::playing);
                    auto started_at = std::chrono::steady_clock::now();
                    auto pause_player = native<void (*)(void*, int)>(image, kh02build::rva(0x345a450));
                    auto is_paused = native<int (*)(void*)>(image, kh02build::rva(0x345a1d0));
                    result["stream"]["pause_count"] = 0;
                    result["stream"]["paused_ms"] = 0;
                    int last = -1;
                    const auto limit =
                        std::chrono::milliseconds(int64_t(total) * denominator * 1000 / numerator + 15000);
                    while (std::chrono::steady_clock::now() - started_at < limit)
                    {
                        const auto command = shared->command();
                        if (command == kh02movie::Command::cancel)
                        {
                            canceled = true;
                            break;
                        }
                        if (command == kh02movie::Command::paused)
                        {
                            // Pause the decoder's own clock and renderer, rather
                            // than letting its bounded PCM queue overflow while
                            // KH3 has stopped consuming samples.
                            const auto pause_started = std::chrono::steady_clock::now();
                            pause_player(player, 1);
                            if (is_paused(player) != 1)
                            {
                                throw std::runtime_error("movie_stream_pause_readback");
                            }
                            result["stream"]["pause_count"] = result["stream"]["pause_count"].get<int>() + 1;
                            while (shared->command() == kh02movie::Command::paused)
                            {
                                execute(player);
                                shared->wait_control(10);
                            }
                            if (shared->command() == kh02movie::Command::cancel)
                            {
                                canceled = true;
                                break;
                            }
                            pause_player(player, 0);
                            if (is_paused(player) != 0)
                            {
                                throw std::runtime_error("movie_stream_resume_readback");
                            }
                            const auto paused_for = std::chrono::steady_clock::now() - pause_started;
                            started_at += paused_for;
                            result["stream"]["paused_ms"] =
                                result["stream"]["paused_ms"].get<int64_t>() +
                                std::chrono::duration_cast<std::chrono::milliseconds>(paused_for).count();
                        }
                        execute(player);
                        if (kh02movie::read(header.audio_error))
                        {
                            throw std::runtime_error("movie_audio_transport_failed");
                        }
                        video(player);
                        sync(player);
                        const auto current = status(player);
                        if (current == 6)
                        {
                            result["stream"]["natural_end"] = true;
                            break;
                        }
                        if (current == 7)
                        {
                            throw std::runtime_error("movie_stream_decoder_error");
                        }
                        alignas(16) std::array<unsigned char, 0xd8> frame{};
                        if (current == 5 && refer(player, frame.data()) && on_time(player, frame.data()))
                        {
                            const auto get = [&](size_t offset)
                            {
                                uint32_t value{};
                                std::memcpy(&value, frame.data() + offset, sizeof(value));
                                return value;
                            };
                            const int number = int(get(0));
                            if (number <= last || number >= total || get(0x24) != unsigned(total) ||
                                get(0x10) != 1920 || get(0x14) != 1080 || get(0x44) != 3)
                            {
                                throw std::runtime_error("movie_stream_frame_profile");
                            }
                            last = number;
                            const auto conversion_started = std::chrono::steady_clock::now();
                            const auto pixels = converter.convert(frame.data());
                            conversion_microseconds += std::chrono::duration_cast<std::chrono::microseconds>(
                                                           std::chrono::steady_clock::now() - conversion_started)
                                                           .count();
                            discard(player, frame.data());
                            if (!shared->publish(number, pixels) && number == total - 1)
                            {
                                throw std::runtime_error("movie_stream_final_frame_not_published");
                            }
                            result["stream"]["last_frame"] = number;
                        }
                        shared->wait_control(1);
                    }
                }
                result["stream"]["canceled"] = canceled;
                result["stream"]["audio_frames"] = movie_audio ? movie_audio->captured_frames() : 0;
                result["stream"]["converted_frames"] = converter.converted_frames();
                result["stream"]["conversion_us"] = conversion_microseconds;
                result["stream"]["conversion_allocations"] = converter.allocations();
                if (!canceled && !result["stream"].value("natural_end", false))
                {
                    throw std::runtime_error("movie_stream_end_timeout");
                }
            }
            if (decode && result["prepared"].get<bool>())
            {
                if (!result.contains("video") || result["video"].value("frames", 0) != 91)
                {
                    throw std::runtime_error("movie_frames_requires_audited_silent_clip");
                }
                auto start_playback = native<void (*)(void*)>(image, kh02build::rva(0x345be70));
                auto sync = native<void (*)(void*)>(image, kh02build::rva(0x345c030));
                auto video = native<int (*)(void*)>(image, kh02build::rva(0x34589b0));
                auto refer = native<int (*)(void*, void*)>(image, kh02build::rva(0x345a560));
                auto on_time = native<int (*)(void*, const void*)>(image, kh02build::rva(0x345a0e0));
                auto discard = native<void (*)(void*, const void*)>(image, kh02build::rva(0x3458490));
                native<void (*)(void*, int)>(image, kh02build::rva(0x345a3f0))(
                    player, 0); // Loop(false); preserve the original timer configuration
                native<void (*)(void*, float)>(image, kh02build::rva(0x345be10))(player, 0.f);
                result["decode"] = {{"frames", json::array()},
                                    {"natural_end", false},
                                    {"presented_in_kh3", false},
                                    {"audio_volume", 0}};
                start_playback(player);
                const auto began = std::chrono::steady_clock::now();
                int last = -1, previous_state = -1;
                while (std::chrono::steady_clock::now() - began < std::chrono::seconds(8))
                {
                    execute(player);
                    video(player);
                    sync(player);
                    const auto current = status(player);
                    const auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began)
                            .count();
                    if (current != previous_state)
                    {
                        result["decode"]["events"].push_back({{"at_ms", elapsed}, {"status", current}});
                    }
                    previous_state = current;
                    if (current == 6)
                    {
                        result["decode"]["natural_end"] = true;
                        break;
                    }
                    if (current == 7)
                    {
                        break;
                    }
                    alignas(16) std::array<unsigned char, 0xd8> frame{};
                    if (current == 5 && refer(player, frame.data()) && on_time(player, frame.data()))
                    {
                        auto field = [&](size_t offset)
                        {
                            uint32_t v;
                            std::memcpy(&v, frame.data() + offset, 4);
                            return v;
                        };
                        const int number = static_cast<int>(field(0));
                        // This original build exposes the public 0xd8 FrameInfo ABI.
                        // Its conversion at 0x3455ca0 was audited field by field.
                        const auto planes = field(0x44);
                        if (number != last + 1 || number > 90 || field(4) != unsigned(number) || field(0x40) != 0 ||
                            field(0x24) != 91 || field(0x10) != 1920 || field(0x14) != 1080 || planes != 3)
                        {
                            throw std::runtime_error(
                                "movie_decoded_frame_layout_or_order:frame=" + std::to_string(number) +
                                ",previous=" + std::to_string(last) + ",width=" + std::to_string(field(0x10)) +
                                ",height=" + std::to_string(field(0x14)) + ",planes=" + std::to_string(planes));
                        }
                        json row = {{"frame", number},
                                    {"frame_per_file", field(4)},
                                    {"total_frames", field(0x24)},
                                    {"concatenations", field(0x40)},
                                    {"color_conversion", field(0xc8)},
                                    {"at_ms", elapsed},
                                    {"planes", json::array()}};
                        for (unsigned i = 0; i < planes; ++i)
                        {
                            const auto offset = 0x48 + i * 24;
                            void* pixels{};
                            std::memcpy(&pixels, frame.data() + offset, 8);
                            const auto bytes = field(offset + 8), pitch = field(offset + 12), line = field(offset + 16),
                                       lines = field(offset + 20);
                            if (!pixels || !bytes || bytes > 4194304 || !pitch || pitch > 4096 || !line ||
                                line > pitch || !lines || lines > 1088 || uint64_t(pitch) * (lines - 1) + line > bytes)
                            {
                                throw std::runtime_error("movie_decoded_plane_bounds");
                            }
                            std::array<unsigned char, 4096> scan{};
                            uint64_t checksum = 14695981039346656037ull;
                            for (unsigned y = 0; y < lines; ++y)
                            {
                                SIZE_T done{};
                                if (!ReadProcessMemory(GetCurrentProcess(),
                                                       static_cast<unsigned char*>(pixels) + size_t(y) * pitch,
                                                       scan.data(), line, &done) ||
                                    done != line)
                                {
                                    throw std::runtime_error("movie_decoded_plane_read");
                                }
                                for (unsigned x = 0; x < line; ++x)
                                {
                                    checksum ^= scan[x];
                                    checksum *= 1099511628211ull;
                                }
                            }
                            row["planes"].push_back({{"bytes", bytes},
                                                     {"pitch", pitch},
                                                     {"line", line},
                                                     {"lines", lines},
                                                     {"checksum", checksum}});
                        }
                        result["decode"]["frames"].push_back(std::move(row));
                        last = number;
                        discard(player, frame.data());
                    }
                    Sleep(1);
                }
                result["decode"]["elapsed_ms"] =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began)
                        .count();
                result["decode"]["all_frames_consumed"] = last + 1 == result["video"].value("frames", 0u);
            }
            stop(player);
            const auto cleanup = std::chrono::steady_clock::now();
            while (status(player) != 0 && std::chrono::steady_clock::now() - cleanup < std::chrono::seconds(3))
            {
                execute(player);
                Sleep(5);
            }
            result["stopped"] = status(player) == 0;
            // Destroy is synchronous in the original library and owns all decoder work.
            destroy(player);
            guard.player = nullptr;
            result["released"] = true;
            if (shared)
            {
                InterlockedExchange(&shared->header().released, 1);
                shared->state(result["stream"].value("natural_end", false) ? kh02movie::State::ended
                                                                           : kh02movie::State::canceled);
            }
            return result;
        }
    } // namespace

    void execute_movie_request(const std::filesystem::path& directory, const kh02control::Request& request)
    {
        json result = {
            {"id", std::to_string(request.generation)}, {"pid", GetCurrentProcessId()}, {"status", "failed"}};
        std::unique_ptr<kh02movie::SharedFrames> shared;
        try
        {
            const std::string_view action = request.operation == kh02control::Operation::stream   ? "stream"
                                            : request.operation == kh02control::Operation::decode ? "decode"
                                                                                                  : "prepare";
            if (request.operation == kh02control::Operation::inspect)
            {
                throw std::runtime_error("unsupported_movie_probe");
            }
            if (action == "stream")
            {
                shared = std::make_unique<kh02movie::SharedFrames>();
                shared->open(std::string(kh02control::text(request.shared_name)), request.identity.owner_pid);
                if (shared->command() == kh02movie::Command::cancel)
                {
                    shared->state(kh02movie::State::canceled);
                    return;
                }
            }
            result["probe"] =
                prepare_probe(directory, std::string(kh02control::text(request.movie)),
                              (request.flags & kh02control::initialize_ui) != 0, action == "decode", action == "stream",
                              (request.flags & kh02control::audible) != 0, shared.get());
            result["status"] = result["probe"].value("prepared", false) ? "prepared" : "not_prepared";
            if (shared && !result["probe"].value("prepared", false))
            {
                shared->state(kh02movie::State::failed);
            }
            if (action == "stream")
            {
                result["status"] = result["probe"].contains("stream") &&
                                           result["probe"]["stream"].value("natural_end", false) &&
                                           result["probe"].value("released", false)
                                       ? "stream_finished"
                                       : "stream_canceled";
            }
            if (action == "decode")
            {
                result["status"] = result["probe"].contains("decode") &&
                                           result["probe"]["decode"].value("natural_end", false) &&
                                           result["probe"]["decode"].value("all_frames_consumed", false)
                                       ? "decoded"
                                       : "not_decoded";
            }
        }
        catch (const std::exception& e)
        {
            result["error"] = e.what();
            if (shared)
            {
                try
                {
                    if (shared->command() == kh02movie::Command::cancel)
                    {
                        // prepare_probe has unwound its native player guard before this catch.
                        InterlockedExchange(&shared->header().released, 1);
                        shared->state(kh02movie::State::canceled);
                        result["status"] = "stream_canceled";
                    }
                    else
                    {
                        shared->state(kh02movie::State::failed);
                    }
                }
                catch (...)
                {
                }
            }
        }
        publish_movie_result(directory, result);
    }
} // namespace kh02
