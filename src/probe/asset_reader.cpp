#include "shared/kh02/kh02_memory_signature.h"
#include "asset_reader.h"
#include "asset_index_provider.h"
#include "shared/kh02/kh02_owner_pipe.h"
#include "asset_service.h"
#include "pak_read_profile.h"
#include "shared/kh02/kh02_parallel_pak_read.h"
#include "kh3ap/features/kh02/transport/kh02_asset_protocol.h"
#include "kh3ap/features/kh02/transport/kh02_index_state.h"
#define NOMINMAX
#include <Windows.h>
#include "build_profile.h"
#include <atomic>
#include <chrono>
#include <array>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <memory>
#include <span>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace kh02
{
    namespace
    {
        std::thread worker;
        std::atomic<bool> running{false};
        std::atomic<uint32_t> owner_pid{};
        std::atomic<uint64_t> rejected_clients{};
        std::unique_ptr<kh02ipc::OwnerPipe> asset_pipe;
        std::atomic<bool> preflight_ok{false};
        std::atomic<uint64_t> requests{0}, bytes{0}, errors{0};
        unsigned char* image_base{};
        void* pak_file{};

        struct NativeFile
        {
            void* handle{};
            uint64_t used{};
            NativeFile() = default;
            NativeFile(const NativeFile&) = delete;

            ~NativeFile()
            {
                if (handle)
                {
                    reinterpret_cast<void (*)(void*, uint32_t)>((*static_cast<void***>(handle))[0])(handle, 1);
                }
            }
        };

        // Each reader thread owns its handles and seek positions. The helper
        // never borrows a handle from the thread serving the pipe.
        thread_local std::map<uint32_t, NativeFile> open_files;
        thread_local uint64_t handle_clock{};
        std::atomic<uint64_t> native_opens{}, handle_hits{};
        std::atomic<uint64_t> native_metadata_us{}, native_read_us{}, reply_write_us{};
        std::atomic<uint64_t> parallel_reads{}, parallel_verified{}, parallel_verification_us{};
        constexpr bool parallel_pak_reads_enabled = true;
        std::unique_ptr<kh02asset::ParallelPakRead> parallel_reader;
        std::unique_ptr<unsigned char[]> verification_buffer;
        std::vector<uint32_t> verified_files;

        auto elapsed_microseconds(std::chrono::steady_clock::time_point began) -> uint64_t
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began)
                .count();
        }

        template <class T> bool safe_read(const void* p, T& out) noexcept
        {
            __try
            {
                std::memcpy(&out, p, sizeof(T));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool verified_pak()
        {
            void** table{};
            return pak_file && safe_read(pak_file, table) &&
                   table == reinterpret_cast<void**>(image_base + kh02build::rva(0x3fa6ae8));
        }

        auto native_file(uint32_t file_id) -> NativeFile*
        {
            auto cached = open_files.find(file_id);
            if (cached == open_files.end())
            {
                if (open_files.size() >= 128)
                {
                    const auto oldest =
                        std::min_element(open_files.begin(), open_files.end(), [](const auto& left, const auto& right)
                                         { return left.second.used < right.second.used; });
                    open_files.erase(oldest);
                }
                const auto& file = kh02asset::asset_index().files.at(file_id);
                auto** table = *static_cast<void***>(pak_file);
                auto* opened = reinterpret_cast<void* (*)(void*, const wchar_t*, bool)>(table[23])(
                    pak_file, file.source.c_str(), false);
                if (!opened)
                {
                    return nullptr;
                }
                cached = open_files.try_emplace(file_id).first;
                cached->second.handle = opened;
                ++native_opens;
            }
            else
            {
                ++handle_hits;
            }
            cached->second.used = ++handle_clock;
            return &cached->second;
        }

        auto read_segment(uint32_t file, uint64_t offset, std::span<unsigned char> output) -> bool
        {
            auto* cached = native_file(file);
            if (!cached)
            {
                return false;
            }
            auto* handle = cached->handle;
            auto** functions = *static_cast<void***>(handle);
            return reinterpret_cast<bool (*)(void*, int64_t)>(functions[2])(handle, offset) &&
                   reinterpret_cast<bool (*)(void*, unsigned char*, int64_t)>(functions[4])(handle, output.data(),
                                                                                            output.size());
        }

        auto read_asset(const kh02asset::Request& request, std::span<unsigned char> destination) -> kh02asset::Response
        {
            using namespace kh02asset;
            const auto metadata_started = std::chrono::steady_clock::now();
            const auto& assets = asset_index();
            Response response{magic, 1, GetCurrentProcessId(), 0, 0, request.offset, request.nonce, assets.tag};
            if (request.magic == magic && request.version == version && request.catalog_tag == assets.tag &&
                request.file_id == index_file_id && request.length <= max_read && request.length <= destination.size())
            {
                const auto& index = serialized_asset_index();
                response.file_size = index.size();
                if (request.offset <= index.size() && request.length <= index.size() - request.offset)
                {
                    if (request.length)
                    {
                        std::memcpy(destination.data(), index.data() + request.offset, request.length);
                    }
                    response.status = 0;
                    response.length = request.length;
                }
                return response;
            }
            if (request.magic != magic || request.version != version || request.catalog_tag != assets.tag ||
                request.file_id >= assets.files.size() || request.length > max_read ||
                request.length > destination.size() || !verified_pak())
            {
                return response;
            }
            const auto& file = assets.files[request.file_id];
            // These are the actual game's FPakPlatformFile::FileSize/OpenRead slots,
            // recovered from its PakFile GetName and guarded by its complete image profile.
            auto** table = *static_cast<void***>(pak_file);
            auto cached_file = open_files.find(request.file_id);
            const auto size =
                cached_file != open_files.end()
                    ? static_cast<int64_t>(file.size)
                    : reinterpret_cast<int64_t (*)(void*, const wchar_t*)>(table[14])(pak_file, file.source.c_str());
            if (size < 0 || uint64_t(size) != file.size || request.offset > uint64_t(size) ||
                request.length > uint64_t(size) - request.offset)
            {
                response.status = 2;
                return response;
            }
            response.file_size = size;
            if (!request.length)
            {
                response.status = 0;
                return response;
            }
            if (!native_file(request.file_id))
            {
                response.status = 3;
                return response;
            }
            native_metadata_us += elapsed_microseconds(metadata_started);
            const auto read_started = std::chrono::steady_clock::now();
            const auto read_started_ms = GetTickCount64();
            const auto output = destination.first(request.length);
            const bool parallel = parallel_pak_reads_enabled && request.length >= 256 * 1024;
            if (parallel && !parallel_reader)
            {
                parallel_reader = std::make_unique<kh02asset::ParallelPakRead>(read_segment);
            }
            const bool read_ok = parallel ? parallel_reader->read(request.file_id, request.offset, output)
                                          : read_segment(request.file_id, request.offset, output);
            if (!read_ok)
            {
                open_files.erase(request.file_id);
                response.status = 4;
                return response;
            }
            const auto elapsed = elapsed_microseconds(read_started);
            native_read_us += elapsed;
            record_pak_read(request.file_id, request.offset, request.length, elapsed, read_started_ms,
                            GetTickCount64());
            if (parallel)
            {
                ++parallel_reads;
                // Compare the first large read from sixteen distinct files to
                // the original serial reader before allowing successful replies.
                if (verified_files.size() < 16 &&
                    std::find(verified_files.begin(), verified_files.end(), request.file_id) == verified_files.end())
                {
                    const auto verification_started = std::chrono::steady_clock::now();
                    if (!verification_buffer)
                    {
                        verification_buffer.reset(new unsigned char[max_read]);
                    }
                    if (!read_segment(request.file_id, request.offset, {verification_buffer.get(), request.length}) ||
                        std::memcmp(verification_buffer.get(), output.data(), request.length) != 0)
                    {
                        response.status = 4;
                        return response;
                    }
                    parallel_verification_us += elapsed_microseconds(verification_started);
                    verified_files.push_back(request.file_id);
                    ++parallel_verified;
                }
            }
            response.status = 0;
            response.length = request.length;
            return response;
        }

        void serve()
        {
            // The worker owns one reply buffer for its lifetime. Native reads overwrite
            // the payload directly; only response.length bytes are ever sent. Reusing
            // this storage avoids allocation and zero-fill for every asset range.
            constexpr auto reply_capacity = sizeof(kh02asset::Response) + kh02asset::max_read;
            std::unique_ptr<unsigned char[]> reply(new unsigned char[reply_capacity]);
            const std::span<unsigned char> payload(reply.get() + sizeof(kh02asset::Response), kh02asset::max_read);
            running.store(true, std::memory_order_release);
            notify_asset_service_ready();
            while (!asset_pipe->stopped())
            {
                if (asset_pipe->connect())
                {
                    while (!asset_pipe->stopped())
                    {
                        kh02asset::Request request{};
                        if (!asset_pipe->read(&request, sizeof(request)))
                        {
                            break;
                        }
                        ++requests;
                        auto response = read_asset(request, payload);
                        if (response.status)
                        {
                            ++errors;
                        }
                        std::memcpy(reply.get(), &response, sizeof(response));
                        const auto reply_size = static_cast<uint32_t>(sizeof(response) + response.length);
                        const auto write_started = std::chrono::steady_clock::now();
                        const bool sent = asset_pipe->write(reply.get(), reply_size);
                        reply_write_us += elapsed_microseconds(write_started);
                        if (!sent)
                        {
                            break;
                        }
                        bytes += response.length;
                    }
                }
                else if (!asset_pipe->stopped())
                {
                    ++rejected_clients;
                }
                asset_pipe->disconnect();
            }
            open_files.clear();
            parallel_reader.reset();
            running = false;
        }
    } // namespace

    bool start_asset_reader(uint32_t expected_owner_pid)
    {
        image_base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!safe_read(image_base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
            dos.e_lfanew > 0x1000 || !safe_read(image_base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.TimeDateStamp != kh02build::current().timestamp ||
            nt.OptionalHeader.SizeOfImage != kh02build::current().image_size)
        {
            return false;
        }
        constexpr auto open_read = "85bc664b2ad32220cc7e1cdacbcd21fa8740667b53a5a0e312affcf24137ed32";
        if (!kh02integrity::signature_matches(image_base + kh02build::rva(0x2b183b0), 11, open_read) ||
            !safe_read(image_base + kh02build::rva(0x4831e70), pak_file) || !verified_pak())
        {
            return false;
        }
        void* lower_level{};
        if (!safe_read(static_cast<unsigned char*>(pak_file) + 8, lower_level) || !lower_level)
        {
            return false;
        }
        discover_asset_index(pak_file);
        // FPakPlatformFile is published after its constructor. Its Initialize
        // installs LowerLevel at +8 before mounting paks under its own lock.
        // Retry this fixed encrypted control until mounting/decryption is ready.
        const auto& assets = kh02asset::asset_index();
        std::array<unsigned char, 4096> preflight_buffer;
        kh02asset::Request preflight_request{
            kh02asset::magic,
            kh02asset::version,
            0,
            static_cast<uint32_t>(std::min<uint64_t>(assets.files[0].size, preflight_buffer.size())),
            0,
            0,
            assets.tag};
        const auto preflight_response = read_asset(preflight_request, preflight_buffer);
        if (preflight_response.status != 0 || !preflight_response.length)
        {
            return false;
        }
        preflight_ok.store(true, std::memory_order_release);
        const auto pipe_name = L"\\\\.\\pipe\\KH02AssetRead_" + std::to_wstring(GetCurrentProcessId());
        asset_pipe = std::make_unique<kh02ipc::OwnerPipe>(
            pipe_name, expected_owner_pid, static_cast<uint32_t>(kh02asset::max_read + sizeof(kh02asset::Response)));
        owner_pid.store(expected_owner_pid);

        worker = std::thread(serve);
        return true;
    }

    auto native_pak_available() -> bool
    {
        const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        void* candidate{};
        void* table{};
        void* lower_level{};
        return safe_read(base + kh02build::rva(0x4831e70), candidate) && candidate && safe_read(candidate, table) &&
               table == base + kh02build::rva(0x3fa6ae8) &&
               safe_read(static_cast<unsigned char*>(candidate) + 8, lower_level) && lower_level;
    }

    void stop_asset_reader()
    {
        if (asset_pipe)
        {
            asset_pipe->stop();
        }
        if (worker.joinable())
        {
            worker.join();
        }
        asset_pipe.reset();
        owner_pid.store(0);
    }

    auto asset_reader_ready() -> bool
    {
        return running.load() && preflight_ok.load();
    }

    auto asset_reader_status() -> std::string
    {
        std::ostringstream out;
        out << std::boolalpha << "{\"running\":" << running << ",\"pid\":" << GetCurrentProcessId()
            << ",\"owner_pid\":" << owner_pid << ",\"rejected_clients\":" << rejected_clients
            << ",\"requests\":" << requests << ",\"payload_bytes\":" << bytes << ",\"errors\":" << errors
            << ",\"preflight_ok\":" << preflight_ok << ",\"native_opens\":" << native_opens
            << ",\"handle_hits\":" << handle_hits << ",\"file_count\":" << kh02asset::asset_index().files.size()
            << ",\"native_metadata_us\":" << native_metadata_us << ",\"native_read_us\":" << native_read_us
            << ",\"parallel_reads\":" << parallel_reads << ",\"parallel_verified\":" << parallel_verified
            << ",\"parallel_enabled\":" << parallel_pak_reads_enabled
            << ",\"parallel_verification_us\":" << parallel_verification_us << ",\"reply_write_us\":" << reply_write_us
            << ",\"catalog_tag\":" << kh02asset::asset_index().tag << ",\"max_read\":" << kh02asset::max_read
            << ",\"reader\":\"game_FPakPlatformFile\""
            << ",\"native_read_profile\":" << pak_read_profile_status() << '}';
        return out.str();
    }
} // namespace kh02
