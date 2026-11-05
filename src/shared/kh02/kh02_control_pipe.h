#pragma once
#include "kh02_control_protocol.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace kh02control
{
    // Synchronous bounded exchange. Call only on an I/O worker, never an engine thread.
    auto exchange(const Request& request, const std::atomic<bool>* canceled = nullptr) -> Reply;
    auto pipe_name(uint32_t source_pid) -> std::wstring;

    struct Ticket
    {
        std::atomic<bool> canceled{false};
        std::atomic<bool> complete{false};
        uint64_t generation{};
        Reply reply; // Read only after acquiring complete.
    };

    class Client
    {
        struct Impl;
        std::unique_ptr<Impl> impl;

      public:
        Client();
        ~Client();
        auto submit(const Request& request) -> std::shared_ptr<Ticket>;
        Client(const Client&) = delete;
        auto operator=(const Client&) -> Client& = delete;
    };

    class Server
    {
        struct Impl;
        std::unique_ptr<Impl> impl;

      public:
        using Handler = std::function<Status(const Request&)>;
        Server(const Identity& identity, Handler handler);
        ~Server();
        Server(const Server&) = delete;
        auto operator=(const Server&) -> Server& = delete;
    };
} // namespace kh02control
