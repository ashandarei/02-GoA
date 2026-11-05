#include "kh02_owner_pipe.h"
#include "kh02_pipe_security.h"

namespace kh02ipc
{
    struct OwnerPipe::Impl
    {
        uint32_t owner_pid;
        Handle owner;
        Handle stop_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Handle io_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Handle pipe;
        bool authenticated = false;

        Impl(const std::wstring& name, uint32_t expected_owner, uint32_t output_buffer_bytes)
            : owner_pid(expected_owner), owner(OpenProcess(SYNCHRONIZE, FALSE, expected_owner))
        {
            if (!owner_pid || !owner.value || !stop_event.value || !io_event.value || stopped())
            {
                throw std::runtime_error("asset_pipe_owner_unavailable");
            }
            UserSecurity security;
            pipe.value = CreateNamedPipeW(name.c_str(),
                                          PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                          1, output_buffer_bytes, 4096, 5000, &security.attributes);
            if (pipe.value == INVALID_HANDLE_VALUE)
            {
                throw std::runtime_error("asset_pipe_create_failed");
            }
        }

        auto stopped() const -> bool
        {
            return WaitForSingleObject(stop_event.value, 0) != WAIT_TIMEOUT ||
                   WaitForSingleObject(owner.value, 0) != WAIT_TIMEOUT;
        }

        auto complete(OVERLAPPED& operation, DWORD timeout, DWORD& transferred) -> bool
        {
            const HANDLE events[]{stop_event.value, owner.value, io_event.value};
            if (WaitForMultipleObjects(3, events, FALSE, timeout) == WAIT_OBJECT_0 + 2 && !stopped())
            {
                return GetOverlappedResult(pipe.value, &operation, &transferred, FALSE) != FALSE;
            }
            CancelIoEx(pipe.value, &operation);
            GetOverlappedResult(pipe.value, &operation, &transferred, TRUE);
            return false;
        }

        auto connect() -> bool
        {
            authenticated = false;
            if (stopped())
            {
                return false;
            }
            ResetEvent(io_event.value);
            OVERLAPPED operation{};
            operation.hEvent = io_event.value;
            if (!ConnectNamedPipe(pipe.value, &operation))
            {
                const auto error = GetLastError();
                DWORD transferred{};
                if (error != ERROR_PIPE_CONNECTED &&
                    (error != ERROR_IO_PENDING || !complete(operation, INFINITE, transferred)))
                {
                    return false;
                }
            }
            ULONG client_pid{};
            // Verify before reading even one byte. Unsupported Wine API behavior
            // fails closed; catalog tags and request nonces are not credentials.
            authenticated =
                !stopped() && GetNamedPipeClientProcessId(pipe.value, &client_pid) && client_pid == owner_pid;
            return authenticated;
        }

        auto transfer(void* data, uint32_t bytes, bool writing) -> bool
        {
            if (!authenticated || stopped())
            {
                return false;
            }
            uint32_t total{};
            // Idle reads wait on events. Once a request starts, partial input
            // and output must finish within five seconds, without polling.
            ULONGLONG deadline = writing ? GetTickCount64() + 5000 : 0;
            while (total < bytes && !stopped())
            {
                const auto now = GetTickCount64();
                if (deadline && now >= deadline)
                {
                    return false;
                }
                ResetEvent(io_event.value);
                OVERLAPPED operation{};
                operation.hEvent = io_event.value;
                DWORD transferred{};
                auto* cursor = static_cast<unsigned char*>(data) + total;
                const bool completed =
                    writing ? WriteFile(pipe.value, cursor, bytes - total, &transferred, &operation) != FALSE
                            : ReadFile(pipe.value, cursor, bytes - total, &transferred, &operation) != FALSE;
                if (!completed &&
                    (GetLastError() != ERROR_IO_PENDING ||
                     !complete(operation, deadline ? static_cast<DWORD>(deadline - now) : INFINITE, transferred)))
                {
                    return false;
                }
                if (!transferred)
                {
                    return false;
                }
                total += transferred;
                if (!deadline)
                {
                    deadline = GetTickCount64() + 5000;
                }
            }
            return total == bytes && !stopped();
        }
    };

    OwnerPipe::OwnerPipe(const std::wstring& name, uint32_t owner_pid, uint32_t output_buffer_bytes)
        : impl(std::make_unique<Impl>(name, owner_pid, output_buffer_bytes))
    {
    }

    OwnerPipe::~OwnerPipe() = default;

    auto OwnerPipe::connect() -> bool
    {
        return impl->connect();
    }

    void OwnerPipe::disconnect()
    {
        impl->authenticated = false;
        DisconnectNamedPipe(impl->pipe.value);
    }

    auto OwnerPipe::read(void* data, uint32_t bytes) -> bool
    {
        return impl->transfer(data, bytes, false);
    }

    auto OwnerPipe::write(const void* data, uint32_t bytes) -> bool
    {
        return impl->transfer(const_cast<void*>(data), bytes, true);
    }

    auto OwnerPipe::stopped() const -> bool
    {
        return impl->stopped();
    }

    void OwnerPipe::stop()
    {
        SetEvent(impl->stop_event.value);
    }
} // namespace kh02ipc
