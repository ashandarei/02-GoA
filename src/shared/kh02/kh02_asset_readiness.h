#pragma once
#include "kh02_control_protocol.h"
#define NOMINMAX
#include <Windows.h>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace kh02control
{
    inline auto asset_readiness_name(const Identity& identity) -> std::wstring
    {
        return L"Local\\KH02AssetsReady_" + std::to_wstring(identity.owner_pid) + L"_" +
               std::to_wstring(identity.session_generation) + L"_" +
               std::wstring(identity.nonce.begin(), identity.nonce.end());
    }

    // This event only wakes the owner. The authenticated control exchange still
    // proves readiness after the signal, including the exact child identity.
    class AssetReadinessSignal
    {
        HANDLE event{};

      public:
        explicit AssetReadinessSignal(const Identity& identity)
        {
            event = CreateEventW(nullptr, TRUE, FALSE, asset_readiness_name(identity).c_str());
            const auto error = GetLastError();
            if (!event || error == ERROR_ALREADY_EXISTS)
            {
                if (event)
                {
                    CloseHandle(event);
                }
                throw std::runtime_error("kh02_asset_readiness_create");
            }
        }

        ~AssetReadinessSignal()
        {
            CloseHandle(event);
        }

        AssetReadinessSignal(const AssetReadinessSignal&) = delete;
        auto operator=(const AssetReadinessSignal&) -> AssetReadinessSignal& = delete;

        void wait(HANDLE source_process, std::stop_token stopping, DWORD timeout_ms) const
        {
            const auto canceled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!canceled)
            {
                throw std::runtime_error("kh02_asset_readiness_cancel_event");
            }
            DWORD result{};
            {
                std::stop_callback callback(stopping, [canceled] { SetEvent(canceled); });
                const HANDLE events[]{canceled, source_process, event};
                result = WaitForMultipleObjects(3, events, FALSE, timeout_ms);
            }
            CloseHandle(canceled);
            if (result != WAIT_OBJECT_0 + 2)
            {
                throw std::runtime_error(result == WAIT_OBJECT_0       ? "kh02_asset_readiness_canceled"
                                         : result == WAIT_OBJECT_0 + 1 ? "kh02_provider_source_exited"
                                         : result == WAIT_TIMEOUT      ? "kh02_provider_readiness_timeout"
                                                                       : "kh02_asset_readiness_wait_failed");
            }
        }
    };

    inline auto open_asset_readiness_publisher(const Identity& identity) -> HANDLE
    {
        const auto event = OpenEventW(EVENT_MODIFY_STATE, FALSE, asset_readiness_name(identity).c_str());
        if (!event)
        {
            throw std::runtime_error("kh02_asset_readiness_open");
        }
        return event;
    }
} // namespace kh02control
