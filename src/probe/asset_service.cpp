#include "shared/kh02/kh02_memory_signature.h"
#include "shared/kh02/kh02_startup_gate.h"
#include "asset_service.h"
#include "asset_reader.h"
#include "movie_service.h"
#include "movie_control_service.h"
#define NOMINMAX
#include <Windows.h>
#include "build_profile.h"
#include <polyhook2/Detour/x64Detour.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace kh02
{
    namespace
    {
        std::unique_ptr<PLH::x64Detour> start_hook;
        std::unique_ptr<PLH::x64Detour> pak_initialize_hook;
        uint64_t original_pak_initialize{};
        HANDLE pak_initialized{};
        uint64_t original_start = 0;
        HANDLE shutdown_event = nullptr;
        unsigned char* image = nullptr;
        std::atomic<bool> parked{false};
        std::atomic<bool> startup_gate_armed{false};
        std::unique_ptr<StartupGate> control_startup;
        std::atomic<uint64_t> assets_ready_at_ms{}, decoder_ready_at_ms{};
        std::atomic<DWORD> startup_thread{0};
        std::atomic<unsigned> start_calls{0};
        std::filesystem::path state_path;
        std::string error;
        std::mutex publish_mutex;
        std::thread early_bootstrap;
        std::atomic<bool> observer_stop{};
        std::atomic<uint64_t> probe_constructed_ms{}, native_pak_available_ms{}, unreal_ready_ms{};

        bool pak_initialize(void* platform, void* lower_level, const wchar_t* command_line)
        {
            using Initialize = bool(__fastcall*)(void*, void*, const wchar_t*);
            const bool initialized =
                reinterpret_cast<Initialize>(original_pak_initialize)(platform, lower_level, command_line);
            if (initialized && native_pak_available())
            {
                native_pak_available_ms.store(GetTickCount64());
                SetEvent(pak_initialized);
            }
            return initialized;
        }

        BOOL CALLBACK hide_service_window(HWND window, LPARAM)
        {
            DWORD owner{};
            GetWindowThreadProcessId(window, &owner);
            if (owner == GetCurrentProcessId())
            {
                ShowWindow(window, SW_HIDE);
            }
            return TRUE;
        }

        void parked_start(void* engine)
        {
            // This is UGameEngine::Start, before StartGameInstance/Browse. Ordinary
            // startup never resumes. Between explicit decoder probes the game thread
            // only waits; UE4SS and the native pak-reader worker remain independent.
            // Early bootstrap runs independently of the engine thread. Never
            // consume its control handles until their publication is complete.
            const auto startup = control_startup->wait(shutdown_event, 90'000);
            if (startup == StartupResult::stopped)
            {
                ExitProcess(0);
            }
            if (startup != StartupResult::ready)
            {
                ExitProcess(72);
            }
            if (engine != *reinterpret_cast<void**>(image + kh02build::rva(0x480b5d0)))
            {
                ExitProcess(71);
            }
            startup_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
            if (++start_calls != 1)
            {
                ExitProcess(71);
            }
            if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"))
            {
                // Finish hiding this engine thread's startup windows before it
                // parks and can no longer service asynchronous window messages.
                EnumThreadWindows(GetCurrentThreadId(), hide_service_window, 0);
            }
            parked.store(true, std::memory_order_release);
            decoder_ready_at_ms.store(GetTickCount64());
            mark_movie_control_ready();
            publish_asset_service();
            const HANDLE events[]{shutdown_event, movie_control_owner(), movie_control_event()};
            while (WaitForMultipleObjects(3, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 2)
            {
                // Only this verified original engine thread may execute the decoder.
                // The pipe worker delivers bounded requests and never calls game code.
                if (const auto request = take_movie_control_request())
                {
                    try
                    {
                        execute_movie_request(state_path.parent_path(), *request);
                    }
                    catch (...)
                    {
                    }
                    complete_movie_control_request();
                    publish_asset_service();
                }
            }
            // Companion-only teardown must not resume title/gameplay startup after
            // its reader and probe have been unloaded. Owner exit normally kills its job.
            ExitProcess(0);
        }
    } // namespace

    auto asset_service_ready() -> bool
    {
        const bool available = startup_gate_armed.load(std::memory_order_acquire) && asset_reader_ready();
        if (available)
        {
            uint64_t empty{};
            assets_ready_at_ms.compare_exchange_strong(empty, GetTickCount64());
        }
        return available;
    }

    void notify_asset_service_ready()
    {
        if (asset_service_ready())
        {
            signal_asset_readiness();
            publish_asset_service();
        }
    }

    void note_asset_unreal_ready()
    {
        unreal_ready_ms.store(GetTickCount64());
        // An Initialize already in flight when the detour was installed cannot
        // hit its entry hook. UE4SS initialization is the second engine-owned
        // completion boundary; the same encrypted preflight still must pass.
        if (pak_initialized)
        {
            SetEvent(pak_initialized);
        }
    }

    void start_early_asset_service()
    {
        probe_constructed_ms.store(GetTickCount64());
        shutdown_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pak_initialized = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!shutdown_event || !pak_initialized)
        {
            ExitProcess(72);
        }
        control_startup = std::make_unique<StartupGate>();
        early_bootstrap = std::thread([] { initialize_asset_service(); });
    }

    void publish_asset_service(const std::string& failure) noexcept
    try
    {
        // Lifecycle and explicit inspection snapshots only: no idle heartbeat.
        // Serialize the shared temporary file across bootstrap/reader/engine threads.
        const std::lock_guard lock(publish_mutex);
        if (!failure.empty())
        {
            error = failure;
        }
        nlohmann::json reader = {{"running", false}, {"preflight_ok", false}};
        try
        {
            reader = nlohmann::json::parse(asset_reader_status());
        }
        catch (...)
        {
        }
        const bool stopped = parked.load(std::memory_order_acquire);
        const auto epoch =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        nlohmann::json state = {{"protocol", 1},
                                {"mode", "asset_service"},
                                {"control_protocol", kh02control::version},
                                {"control_owner_pid", movie_control_identity().owner_pid},
                                {"control_session_generation", movie_control_identity().session_generation},
                                {"pid", GetCurrentProcessId()},
                                {"observed_at_ms", epoch},
                                {"status_delivery", "snapshot"},
                                {"stage", error.empty() ? (stopped ? "parked_before_game_start" : "armed") : "failed"},
                                {"profile_verified", startup_gate_armed.load(std::memory_order_acquire)},
                                {"normal_startup_blocked", stopped},
                                {"assets_ready", asset_service_ready()},
                                {"assets_ready_at_ms", assets_ready_at_ms.load()},
                                {"decoder_ready_at_ms", decoder_ready_at_ms.load()},
                                {"probe_constructed_ms", probe_constructed_ms.load()},
                                {"native_pak_available_ms", native_pak_available_ms.load()},
                                {"unreal_ready_ms", unreal_ready_ms.load()},
                                {"start_calls", start_calls.load()},
                                {"startup_thread_id", startup_thread.load()},
                                {"gate_rva", kh02build::rva(0x1df7a80)},
                                {"last_error", error},
                                {"reader", reader},
                                {"windows_audio_muting", false},
                                {"ready", stopped && reader.value("running", false) &&
                                              reader.value("preflight_ok", false) && error.empty()}};
        const auto temporary = state_path.wstring() + L".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << state.dump();
            out.flush();
            if (!out)
            {
                return;
            }
        }
        // A reader can briefly deny rename on Windows. Retry only this pending
        // publication; there is no heartbeat to repair a missed transition later.
        for (unsigned attempt = 0; attempt < 50; ++attempt)
        {
            if (MoveFileExW(temporary.c_str(), state_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                return;
            }
            const auto rename_error = GetLastError();
            if (rename_error != ERROR_SHARING_VIOLATION && rename_error != ERROR_ACCESS_DENIED)
            {
                return;
            }
            Sleep(2);
        }
    }
    catch (...)
    {
        // Evidence publication must not stop the native service. Readiness and
        // liveness use authenticated IPC and process handles, never this file.
    }

    void initialize_asset_service()
    try
    {
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, 32768);
        state_path = std::filesystem::path(path).parent_path() / L"kh02_bridge" / L"asset_service_state.json";
        std::filesystem::create_directories(state_path.parent_path());
        // Clear a previous ordinary session's gameplay readiness. Service mode has
        // its own status snapshots and must reject ordinary gameplay bridge tools.
        {
            const auto temporary = state_path.parent_path() / L"state.json.tmp";
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << nlohmann::json{{"protocol", 1},  {"game", "kh02"},          {"pid", GetCurrentProcessId()},
                                  {"ready", false}, {"mode", "asset_service"}, {"reason", "gameplay_bridge_disabled"}}
                       .dump();
            out.close();
            MoveFileExW(temporary.c_str(), (state_path.parent_path() / L"state.json").c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
        {
            throw std::runtime_error("service_image_header");
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != kh02build::current().timestamp ||
            nt->OptionalHeader.SizeOfImage != kh02build::current().image_size)
        {
            throw std::runtime_error("service_image_profile");
        }
        // Verified from the loaded TresGameEngine vtable (+0x250), beside the
        // verified Tick slot (+0x268). This tail-calls GameInstance +0x278.
        constexpr auto signature = "90a17df5869c837330c6f3fcfbe5bc860d533bb418ff24281eacf44e1fce6523";
        if (!kh02integrity::signature_matches(image + kh02build::rva(0x1df7a80), 17, signature) ||
            *reinterpret_cast<void**>(image + kh02build::rva(0x3762a70) + 0x250) != image + kh02build::rva(0x1df7a80))
        {
            throw std::runtime_error("service_start_signature");
        }
        constexpr auto initialize_signature = "41285b93e1303caf42e4748032d8976257e5d5244267932de1c8c0adbe76270d";
        if (!kh02integrity::signature_matches(image + kh02build::rva(0x2b14f90), 15, initialize_signature))
        {
            throw std::runtime_error("service_pak_initialize_signature");
        }
        pak_initialize_hook =
            std::make_unique<PLH::x64Detour>(reinterpret_cast<uint64_t>(image + kh02build::rva(0x2b14f90)),
                                             reinterpret_cast<uint64_t>(&pak_initialize), &original_pak_initialize);
        if (!pak_initialize_hook->hook())
        {
            throw std::runtime_error("service_pak_initialize_hook");
        }
        start_hook = std::make_unique<PLH::x64Detour>(reinterpret_cast<uint64_t>(image + kh02build::rva(0x1df7a80)),
                                                      reinterpret_cast<uint64_t>(&parked_start), &original_start);
        if (!start_hook->hook())
        {
            start_hook.reset();
            throw std::runtime_error("service_start_hook");
        }
        startup_gate_armed.store(true, std::memory_order_release);
        initialize_movie_control();
        control_startup->ready();
        // Enumerate only after the engine's mount-completion boundary. A
        // readable control file alone does not prove all content paks mounted.
        {
            const HANDLE events[]{shutdown_event, pak_initialized};
            const auto result = WaitForMultipleObjects(2, events, FALSE, 90'000);
            if (result == WAIT_OBJECT_0)
            {
                return;
            }
            if (result != WAIT_OBJECT_0 + 1 || !start_asset_reader(movie_control_identity().owner_pid))
            {
                throw std::runtime_error("service_pak_reader_not_initialized");
            }
        }
        if (observer_stop.load(std::memory_order_acquire))
        {
            return;
        }
        publish_asset_service();
    }
    catch (const std::exception& e)
    {
        startup_gate_armed.store(false, std::memory_order_release);
        publish_asset_service(e.what());
        control_startup->fail();
        ExitProcess(72); // Never fall through into normal startup in service mode.
    }

    void stop_asset_service()
    {
        observer_stop.store(true, std::memory_order_release);
        if (shutdown_event)
        {
            SetEvent(shutdown_event);
        }
        if (early_bootstrap.joinable())
        {
            early_bootstrap.join();
            // Companion teardown must never free a live native trampoline or
            // resume ordinary title startup, including before the parked gate.
            ExitProcess(0);
        }
        startup_gate_armed.store(false, std::memory_order_release);
        if (parked.load(std::memory_order_acquire))
        {
            SetEvent(shutdown_event);
            // Keep this DLL loaded until the gated thread exits the companion.
            // Returning here could unload the code underneath its wait callback.
            WaitForSingleObject(GetCurrentProcess(), INFINITE);
            return;
        }
        stop_movie_control();
        if (start_hook)
        {
            start_hook->unHook();
            start_hook.reset();
        }
        if (shutdown_event)
        {
            CloseHandle(shutdown_event);
            shutdown_event = nullptr;
        }
    }
} // namespace kh02
