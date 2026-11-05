#include "kh02_control_pipe.h"
#include "kh02_pipe_security.h"
#define NOMINMAX
#include <Windows.h>
#include <sddl.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace kh02control
{
    namespace
    {
        struct Handle
        {
            HANDLE value{};

            explicit Handle(HANDLE handle = nullptr) : value(handle) {}

            ~Handle()
            {
                if (value && value != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(value);
                }
            }

            Handle(const Handle&) = delete;
            auto operator=(const Handle&) -> Handle& = delete;
        };

        using kh02ipc::UserSecurity;

        auto complete(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, HANDLE process,
                      const std::atomic<bool>* canceled, DWORD milliseconds) -> DWORD
        {
            const auto deadline = milliseconds == INFINITE ? 0 : GetTickCount64() + milliseconds;
            const auto cancel_operation = [&]
            {
                CancelIoEx(pipe, &operation);
                DWORD ignored{};
                GetOverlappedResult(pipe, &operation, &ignored, TRUE);
            };
            HANDLE events[3]{operation.hEvent};
            DWORD event_count = 1;
            if (stop)
            {
                events[event_count++] = stop;
            }
            if (process)
            {
                events[event_count++] = process;
            }
            for (;;)
            {
                const auto now = GetTickCount64();
                if ((canceled && canceled->load(std::memory_order_acquire)) ||
                    (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) ||
                    (process && WaitForSingleObject(process, 0) != WAIT_TIMEOUT) || (deadline && now >= deadline))
                {
                    cancel_operation();
                    throw std::runtime_error("control_io_canceled_or_timeout");
                }
                DWORD timeout = deadline ? static_cast<DWORD>(deadline - now) : INFINITE;
                if (canceled)
                {
                    timeout = std::min<DWORD>(timeout, 50);
                }
                const auto wait = WaitForMultipleObjects(event_count, events, FALSE, timeout);
                if (wait == WAIT_OBJECT_0)
                {
                    DWORD transferred{};
                    if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE))
                    {
                        throw std::runtime_error("control_io_failed");
                    }
                    return transferred;
                }
                if (wait != WAIT_TIMEOUT && (wait < WAIT_OBJECT_0 || wait >= WAIT_OBJECT_0 + event_count))
                {
                    cancel_operation();
                    throw std::runtime_error("control_io_wait_failed");
                }
            }
        }

        void transfer(HANDLE pipe, void* data, DWORD bytes, bool writing, HANDLE stop, HANDLE process,
                      const std::atomic<bool>* canceled)
        {
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event.value)
            {
                throw std::runtime_error("control_io_event");
            }
            OVERLAPPED operation{};
            operation.hEvent = event.value;
            DWORD transferred{};
            const bool succeeded = writing ? WriteFile(pipe, data, bytes, &transferred, &operation)
                                           : ReadFile(pipe, data, bytes, &transferred, &operation);
            if (!succeeded)
            {
                if (GetLastError() != ERROR_IO_PENDING)
                {
                    throw std::runtime_error("control_io_begin");
                }
                transferred = complete(pipe, operation, stop, process, canceled, 3000);
            }
            if (transferred != bytes)
            {
                throw std::runtime_error("control_io_size");
            }
        }
    } // namespace

    auto pipe_name(uint32_t source_pid) -> std::wstring
    {
        return L"\\\\.\\pipe\\KH02Control_" + std::to_wstring(source_pid);
    }

    auto exchange(const Request& request, const std::atomic<bool>* canceled) -> Reply
    {
        Reply result;
        result.identity = request.identity;
        result.generation = request.generation;
        result.status = Status::transport_error;
        try
        {
            if (canceled && canceled->load(std::memory_order_acquire))
            {
                result.status = Status::canceled;
                return result;
            }
            Handle process(OpenProcess(SYNCHRONIZE, FALSE, request.identity.source_pid));
            if (!process.value || WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT)
            {
                throw std::runtime_error("control_source_exited");
            }
            Handle pipe;
            const auto connect_deadline = GetTickCount64() + 500;
            for (;;)
            {
                pipe.value = CreateFileW(pipe_name(request.identity.source_pid).c_str(), GENERIC_READ | GENERIC_WRITE,
                                         0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                if (pipe.value != INVALID_HANDLE_VALUE)
                {
                    break;
                }
                if (GetLastError() != ERROR_PIPE_BUSY || GetTickCount64() >= connect_deadline ||
                    (canceled && canceled->load(std::memory_order_acquire)) ||
                    WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT)
                {
                    throw std::runtime_error("control_connect");
                }
                WaitNamedPipeW(pipe_name(request.identity.source_pid).c_str(), 50);
            }
            ULONG server_pid{};
            if (!GetNamedPipeServerProcessId(pipe.value, &server_pid) || server_pid != request.identity.source_pid)
            {
                throw std::runtime_error("control_server_identity");
            }
            auto copy = request;
            transfer(pipe.value, &copy, sizeof(copy), true, nullptr, process.value, canceled);
            transfer(pipe.value, &result, sizeof(result), false, nullptr, process.value, canceled);
            const bool rejected_session = result.signature == magic && result.protocol == version &&
                                          result.bytes == sizeof(Reply) && result.generation == request.generation &&
                                          result.status == Status::wrong_session &&
                                          result.identity.source_pid == request.identity.source_pid;
            if (!valid_reply(result, request) && !rejected_session)
            {
                throw std::runtime_error("control_reply_identity_or_layout");
            }
        }
        catch (const std::exception& error)
        {
            result = {};
            result.identity = request.identity;
            result.generation = request.generation;
            result.status = canceled && canceled->load() ? Status::canceled : Status::transport_error;
            assign(result.error, error.what());
        }
        return result;
    }

    struct Client::Impl
    {
        struct Work
        {
            Request request;
            std::shared_ptr<Ticket> ticket;
        };

        std::mutex mutex;
        std::condition_variable changed;
        std::deque<Work> queue;
        bool stopping{};
        std::shared_ptr<Ticket> active;
        std::thread worker;

        Impl() : worker([this] { run(); }) {}

        ~Impl()
        {
            {
                std::lock_guard lock(mutex);
                stopping = true;
                if (active)
                {
                    active->canceled = true;
                }
                for (auto& work : queue)
                {
                    work.ticket->canceled = true;
                }
            }
            changed.notify_one();
            worker.join();
        }

        void run()
        {
            for (;;)
            {
                Work work;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [this] { return stopping || !queue.empty(); });
                    if (stopping && queue.empty())
                    {
                        return;
                    }
                    work = queue.front();
                    queue.pop_front();
                    active = work.ticket;
                }
                const auto deadline = GetTickCount64() + 5000;
                do
                {
                    work.ticket->reply = kh02control::exchange(work.request, &work.ticket->canceled);
                    const auto status = work.ticket->reply.status;
                    if (status != Status::busy && status != Status::not_ready && status != Status::transport_error)
                    {
                        break;
                    }
                    // Replacing a canceled decoder may briefly leave the source busy.
                    // All retry latency belongs to this I/O worker.
                    std::unique_lock lock(mutex);
                    changed.wait_for(lock, std::chrono::milliseconds(25),
                                     [this, &work] { return stopping || work.ticket->canceled.load(); });
                } while (!work.ticket->canceled.load() && GetTickCount64() < deadline);
                if (work.ticket->canceled.load())
                {
                    work.ticket->reply.status = Status::canceled;
                }
                work.ticket->complete.store(true, std::memory_order_release);
                {
                    std::lock_guard lock(mutex);
                    active.reset();
                }
            }
        }
    };

    Client::Client() : impl(std::make_unique<Impl>()) {}

    Client::~Client() = default;

    auto Client::submit(const Request& request) -> std::shared_ptr<Ticket>
    {
        auto ticket = std::make_shared<Ticket>();
        ticket->generation = request.generation;
        {
            std::lock_guard lock(impl->mutex);
            if (impl->stopping || impl->queue.size() >= 4)
            {
                throw std::runtime_error("control_client_queue_full_or_stopped");
            }
            impl->queue.push_back({request, ticket});
        }
        impl->changed.notify_one();
        return ticket;
    }

    struct Server::Impl
    {
        Identity identity;
        Handler handler;
        Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Handle owner;
        Handle pipe;
        std::thread worker;

        Impl(const Identity& value, Handler callback)
            : identity(value), handler(std::move(callback)), owner(OpenProcess(SYNCHRONIZE, FALSE, identity.owner_pid))
        {
            if (!valid_identity(identity) || !stop.value || !owner.value)
            {
                throw std::runtime_error("control_server_session");
            }
            UserSecurity security;
            pipe.value =
                CreateNamedPipeW(pipe_name(identity.source_pid).c_str(),
                                 PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                 PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                 sizeof(Reply), sizeof(Request), 0, &security.attributes);
            if (pipe.value == INVALID_HANDLE_VALUE)
            {
                throw std::runtime_error("control_server_create");
            }
            worker = std::thread([this] { run(); });
        }

        ~Impl()
        {
            SetEvent(stop.value);
            if (worker.joinable())
            {
                worker.join();
            }
        }

        void run() noexcept
        {
            while (WaitForSingleObject(stop.value, 0) == WAIT_TIMEOUT &&
                   WaitForSingleObject(owner.value, 0) == WAIT_TIMEOUT)
            {
                try
                {
                    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                    if (!event.value)
                    {
                        return;
                    }
                    OVERLAPPED operation{};
                    operation.hEvent = event.value;
                    if (!ConnectNamedPipe(pipe.value, &operation))
                    {
                        const auto error = GetLastError();
                        if (error == ERROR_IO_PENDING)
                        {
                            complete(pipe.value, operation, stop.value, owner.value, nullptr, INFINITE);
                        }
                        else if (error != ERROR_PIPE_CONNECTED)
                        {
                            throw std::runtime_error("control_server_connect");
                        }
                    }
                    Request request;
                    transfer(pipe.value, &request, sizeof(request), false, stop.value, owner.value, nullptr);
                    ULONG client_pid{};
                    GetNamedPipeClientProcessId(pipe.value, &client_pid);
                    Reply reply;
                    reply.identity = identity;
                    reply.generation = request.generation;
                    reply.status = validate(request, identity, client_pid);
                    if (reply.status == Status::accepted)
                    {
                        reply.status = handler(request);
                    }
                    transfer(pipe.value, &reply, sizeof(reply), true, stop.value, owner.value, nullptr);
                    // Wait for client close (bounded and cancelable) before disconnecting;
                    // DisconnectNamedPipe would discard an unread buffered reply.
                    unsigned char receipt{};
                    transfer(pipe.value, &receipt, sizeof(receipt), false, stop.value, owner.value, nullptr);
                }
                catch (...)
                {
                    // A malformed, stalled, or disconnected client cannot stop the service.
                }
                DisconnectNamedPipe(pipe.value);
            }
        }
    };

    Server::Server(const Identity& identity, Handler handler)
        : impl(std::make_unique<Impl>(identity, std::move(handler)))
    {
    }

    Server::~Server() = default;
} // namespace kh02control
