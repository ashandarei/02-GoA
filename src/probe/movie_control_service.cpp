#include "movie_control_service.h"
#include "build_profile.h"
#include "asset_service.h"
#include "shared/kh02/kh02_asset_readiness.h"
#include "shared/kh02/kh02_control_pipe.h"
#include "kh3ap/features/kh02/transport/kh02_index_state.h"
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>

namespace kh02
{
    namespace
    {
        kh02control::Identity identity;
        HANDLE request_event{};
        HANDLE owner_process{};
        HANDLE assets_ready_event{};
        std::atomic<bool> ready{};
        std::mutex request_mutex;
        kh02control::Mailbox mailbox;
        std::unique_ptr<kh02control::Server> server;

        auto environment(const wchar_t* name) -> std::wstring
        {
            wchar_t value[128]{};
            const auto length = GetEnvironmentVariableW(name, value, 128);
            if (!length || length >= 128)
            {
                throw std::runtime_error("movie_control_environment");
            }
            return {value, length};
        }

        auto accept(const kh02control::Request& request) -> kh02control::Status
        {
            if (!kh02control::ready_for_request(request.operation, asset_service_ready(),
                                                ready.load(std::memory_order_acquire)))
            {
                return kh02control::Status::not_ready;
            }
            if (request.operation == kh02control::Operation::inspect ||
                request.operation == kh02control::Operation::inspect_assets)
            {
                publish_asset_service();
                return kh02control::Status::accepted;
            }
            std::lock_guard lock(request_mutex);
            const auto result = mailbox.accept(request);
            if (mailbox.has_pending())
            {
                SetEvent(request_event);
            }
            return result;
        }
    } // namespace

    void initialize_movie_control()
    {
        const auto owner = std::stoull(environment(L"KH02_CONTROL_OWNER"));
        if (!owner || owner > UINT32_MAX)
        {
            throw std::runtime_error("movie_control_owner_pid");
        }
        identity.owner_pid = static_cast<uint32_t>(owner);
        identity.source_pid = GetCurrentProcessId();
        identity.session_generation = std::stoull(environment(L"KH02_CONTROL_GENERATION"));
        kh02asset::configure_source_identity(kh02build::current().executable_sha256);
        identity.catalog_tag = kh02asset::asset_index().tag;
        const auto nonce = environment(L"KH02_CONTROL_NONCE");
        if (nonce.size() != identity.nonce.size())
        {
            throw std::runtime_error("movie_control_nonce");
        }
        for (std::size_t i = 0; i < nonce.size(); ++i)
        {
            if (nonce[i] > 127)
            {
                throw std::runtime_error("movie_control_nonce");
            }
            identity.nonce[i] = static_cast<char>(nonce[i]);
        }
        if (!kh02control::valid_identity(identity))
        {
            throw std::runtime_error("movie_control_identity");
        }
        request_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        owner_process = OpenProcess(SYNCHRONIZE, FALSE, identity.owner_pid);
        if (!request_event || !owner_process)
        {
            throw std::runtime_error("movie_control_handles");
        }
        assets_ready_event = kh02control::open_asset_readiness_publisher(identity);
        server = std::make_unique<kh02control::Server>(identity, accept);
    }

    void stop_movie_control()
    {
        server.reset();
        if (assets_ready_event)
        {
            CloseHandle(assets_ready_event);
            assets_ready_event = nullptr;
        }
        if (request_event)
        {
            CloseHandle(request_event);
            request_event = nullptr;
        }
        if (owner_process)
        {
            CloseHandle(owner_process);
            owner_process = nullptr;
        }
    }

    void mark_movie_control_ready()
    {
        ready.store(true, std::memory_order_release);
    }

    void signal_asset_readiness()
    {
        if (!assets_ready_event || !SetEvent(assets_ready_event))
        {
            ExitProcess(72);
        }
    }

    auto movie_control_event() -> HANDLE
    {
        return request_event;
    }

    auto movie_control_owner() -> HANDLE
    {
        return owner_process;
    }

    auto movie_control_identity() -> const kh02control::Identity&
    {
        return identity;
    }

    auto take_movie_control_request() -> std::optional<kh02control::Request>
    {
        std::lock_guard lock(request_mutex);
        auto result = mailbox.take();
        ResetEvent(request_event);
        return result;
    }

    void complete_movie_control_request()
    {
        std::lock_guard lock(request_mutex);
        mailbox.complete();
    }
} // namespace kh02
