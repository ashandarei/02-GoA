#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <optional>
#include <type_traits>

namespace kh02control
{
    inline constexpr uint32_t magic = 0x4332304b;
    inline constexpr uint32_t version = 1;
    enum class Operation : uint32_t
    {
        inspect,
        prepare,
        decode,
        stream,
        inspect_assets
    };
    enum class Status : uint32_t
    {
        accepted,
        not_ready,
        busy,
        invalid,
        wrong_session,
        duplicate,
        canceled,
        transport_error
    };
    inline constexpr uint32_t initialize_ui = 1;
    inline constexpr uint32_t audible = 2;

#pragma pack(push, 1)

    struct Identity
    {
        uint32_t owner_pid{};
        uint32_t source_pid{};
        uint64_t catalog_tag{};
        uint64_t session_generation{};
        std::array<char, 32> nonce{};
        bool operator==(const Identity&) const = default;
    };

    struct Request
    {
        uint32_t signature = magic;
        uint32_t protocol = version;
        Operation operation{};
        uint32_t bytes = sizeof(Request);
        Identity identity;
        uint64_t generation{};
        uint32_t client_pid{};
        uint32_t flags{};
        std::array<char, 128> movie{};
        std::array<char, 128> shared_name{};
    };

    struct Reply
    {
        uint32_t signature = magic;
        uint32_t protocol = version;
        Status status = Status::invalid;
        uint32_t bytes = sizeof(Reply);
        Identity identity;
        uint64_t generation{};
        std::array<char, 128> error{};
    };

#pragma pack(pop)
    static_assert(sizeof(Identity) == 56);
    static_assert(sizeof(Request) == 344);
    static_assert(sizeof(Reply) == 208);
    static_assert(std::is_trivially_copyable_v<Request> && std::is_trivially_copyable_v<Reply>);

    template <size_t Size> auto text(const std::array<char, Size>& value) -> std::string_view
    {
        const auto end = std::find(value.begin(), value.end(), '\0');
        return {value.data(), static_cast<size_t>(end - value.begin())};
    }

    template <size_t Size> auto assign(std::array<char, Size>& destination, std::string_view value) -> bool
    {
        if (value.size() >= Size || value.find('\0') != std::string_view::npos)
        {
            return false;
        }
        destination.fill(0);
        std::copy(value.begin(), value.end(), destination.begin());
        return true;
    }

    inline auto valid_identity(const Identity& identity) -> bool
    {
        return identity.owner_pid && identity.source_pid && identity.catalog_tag && identity.session_generation &&
               std::all_of(identity.nonce.begin(), identity.nonce.end(),
                           [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
    }

    inline auto validate(const Request& request, const Identity& expected, uint32_t connected_pid) -> Status
    {
        if (request.signature != magic || request.protocol != version || request.bytes != sizeof(Request) ||
            !request.generation || !connected_pid || request.client_pid != connected_pid ||
            request.operation > Operation::inspect_assets || (request.flags & ~(initialize_ui | audible)) ||
            text(request.movie).size() == request.movie.size() ||
            text(request.shared_name).size() == request.shared_name.size())
        {
            return Status::invalid;
        }
        if (!valid_identity(expected) || request.identity != expected || connected_pid != expected.owner_pid)
        {
            return Status::wrong_session;
        }
        if (request.operation != Operation::inspect && request.operation != Operation::inspect_assets &&
            text(request.movie).empty())
        {
            return Status::invalid;
        }
        if (request.operation == Operation::stream && text(request.shared_name).empty())
        {
            return Status::invalid;
        }
        return Status::accepted;
    }

    // Pak reads may start after native preflight. Movie requests may queue then,
    // but only the parked engine thread consumes the mailbox. Ordinary inspect
    // continues to prove that decoder startup has finished.
    inline auto ready_for_request(Operation operation, bool assets_ready, bool decoder_ready) -> bool
    {
        return operation == Operation::inspect ? decoder_ready : assets_ready;
    }

    // One decoder slot, guarded by the source service mutex. Duplicate delivery
    // is idempotent; reusing a generation for different contents is rejected.
    class Mailbox
    {
        std::array<Request, 64> recent{};
        size_t next{};
        bool busy{};
        std::optional<Request> pending;

      public:
        auto accept(const Request& request) -> Status
        {
            for (const auto& previous : recent)
            {
                if (previous.generation && previous.generation == request.generation)
                {
                    return std::memcmp(&previous, &request, sizeof(Request)) == 0 ? Status::accepted
                                                                                  : Status::duplicate;
                }
            }
            if (busy)
            {
                return Status::busy;
            }
            recent[next++ % recent.size()] = request;
            pending = request;
            busy = true;
            return Status::accepted;
        }

        auto take() -> std::optional<Request>
        {
            auto result = pending;
            pending.reset();
            return result;
        }

        void complete()
        {
            busy = false;
        }

        auto has_pending() const -> bool
        {
            return pending.has_value();
        }
    };

    inline auto valid_reply(const Reply& reply, const Request& request) -> bool
    {
        return reply.signature == magic && reply.protocol == version && reply.bytes == sizeof(Reply) &&
               reply.identity == request.identity && reply.generation == request.generation &&
               reply.status <= Status::transport_error && text(reply.error).size() < reply.error.size();
    }
} // namespace kh02control
