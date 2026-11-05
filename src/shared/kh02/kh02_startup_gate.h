#pragma once
#include "kh02_pipe_security.h"

namespace kh02
{
    enum class StartupResult
    {
        ready,
        stopped,
        failed,
        timed_out
    };

    // Construct before installing the engine hook. The engine can then wait
    // without touching control handles that the bootstrap is still creating.
    class StartupGate
    {
        kh02ipc::Handle ready_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        kh02ipc::Handle failed_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};

      public:
        StartupGate()
        {
            if (!ready_event.value || !failed_event.value)
            {
                throw std::runtime_error("service_startup_events");
            }
        }

        void ready()
        {
            if (!SetEvent(ready_event.value))
            {
                throw std::runtime_error("service_startup_publish");
            }
        }

        void fail() noexcept
        {
            SetEvent(failed_event.value);
        }

        auto wait(HANDLE shutdown, DWORD timeout_ms) const -> StartupResult
        {
            const HANDLE events[]{shutdown, failed_event.value, ready_event.value};
            switch (WaitForMultipleObjects(3, events, FALSE, timeout_ms))
            {
            case WAIT_OBJECT_0:
                return StartupResult::stopped;
            case WAIT_OBJECT_0 + 2:
                return StartupResult::ready;
            case WAIT_TIMEOUT:
                return StartupResult::timed_out;
            default:
                return StartupResult::failed;
            }
        }
    };
} // namespace kh02
