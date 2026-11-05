#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace kh02ipc
{
    // A single owner process may exchange bytes. Holds the original owner's
    // process handle for the session; PID reuse after its exit is not accepted.
    class OwnerPipe
    {
        struct Impl;
        std::unique_ptr<Impl> impl;

      public:
        OwnerPipe(const std::wstring& name, uint32_t owner_pid, uint32_t output_buffer_bytes);
        ~OwnerPipe();
        OwnerPipe(const OwnerPipe&) = delete;
        auto operator=(const OwnerPipe&) -> OwnerPipe& = delete;
        auto connect() -> bool;
        void disconnect();
        auto read(void* data, uint32_t bytes) -> bool;
        auto write(const void* data, uint32_t bytes) -> bool;
        auto stopped() const -> bool;
        void stop();
    };
} // namespace kh02ipc
