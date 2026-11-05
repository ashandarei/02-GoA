#include "shared/kh02/kh02_owner_pipe.h"
#include "shared/kh02/kh02_pipe_security.h"
#include "shared/kh02/kh02_memory_signature.h"
#include "shared/kh02/kh02_runtime_mode.h"
#include "shared/kh02/kh02_control_protocol.h"
#include "shared/kh02/kh02_startup_gate.h"
#include "support/kh3ap/features/kh02/transport/kh02_asset_protocol.h"
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
    using kh02ipc::Handle;

    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    void check_startup_gate()
    {
        Handle shutdown(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        require(shutdown.value != nullptr, "startup shutdown event");
        kh02::StartupGate delayed;
        Handle entered(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        Handle finished(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        require(entered.value && finished.value, "startup test events");
        kh02::StartupResult result = kh02::StartupResult::failed;
        int initialized_value = 0;
        int observed_value = 0;
        std::jthread engine(
            [&]
            {
                SetEvent(entered.value);
                result = delayed.wait(shutdown.value, 3000);
                if (result == kh02::StartupResult::ready)
                {
                    observed_value = initialized_value;
                }
                SetEvent(finished.value);
            });
        require(WaitForSingleObject(entered.value, 1000) == WAIT_OBJECT_0, "engine entered startup gate");
        require(WaitForSingleObject(finished.value, 50) == WAIT_TIMEOUT, "early engine waits for initialization");
        initialized_value = 42;
        delayed.ready();
        engine.join();
        require(result == kh02::StartupResult::ready && observed_value == 42, "engine observes published handles");
        require(delayed.wait(shutdown.value, 0) == kh02::StartupResult::ready,
                "already-ready bootstrap does not block");

        kh02::StartupGate pending;
        require(pending.wait(shutdown.value, 10) == kh02::StartupResult::timed_out, "startup wait is bounded");
        pending.fail();
        require(pending.wait(shutdown.value, 0) == kh02::StartupResult::failed, "bootstrap failure wakes engine");
        SetEvent(shutdown.value);
        require(delayed.wait(shutdown.value, 0) == kh02::StartupResult::stopped, "shutdown wins over readiness");
        require(pending.wait(shutdown.value, 0) == kh02::StartupResult::stopped, "shutdown cancels unfinished startup");

        ResetEvent(shutdown.value);
        ResetEvent(entered.value);
        kh02::StartupGate interrupted;
        std::jthread waiting_engine(
            [&]
            {
                SetEvent(entered.value);
                result = interrupted.wait(shutdown.value, 3000);
            });
        require(WaitForSingleObject(entered.value, 1000) == WAIT_OBJECT_0, "engine entered cancelable wait");
        SetEvent(shutdown.value);
        waiting_engine.join();
        require(result == kh02::StartupResult::stopped, "shutdown wakes a waiting engine");

        ResetEvent(shutdown.value);
        ResetEvent(entered.value);
        kh02::StartupGate failing;
        std::jthread failed_engine(
            [&]
            {
                SetEvent(entered.value);
                result = failing.wait(shutdown.value, 3000);
            });
        require(WaitForSingleObject(entered.value, 1000) == WAIT_OBJECT_0, "engine entered failure wait");
        failing.fail();
        failed_engine.join();
        require(result == kh02::StartupResult::failed, "bootstrap failure wakes a waiting engine");
    }

    auto name(const wchar_t* suffix) -> std::wstring
    {
        return L"02GoATest_" + std::to_wstring(GetCurrentProcessId()) + suffix;
    }

    struct Child
    {
        PROCESS_INFORMATION info{};

        Child(const std::wstring& arguments)
        {
            wchar_t executable[32768]{};
            require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "test executable path");
            auto command = L"\"" + std::wstring(executable) + L"\" " + arguments;
            STARTUPINFOW startup{sizeof(startup)};
            require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &startup, &info) != FALSE,
                    "start test child");
        }

        ~Child()
        {
            if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT)
            {
                // Only this test-created process; never a game or unrelated process.
                TerminateProcess(info.hProcess, 99);
                WaitForSingleObject(info.hProcess, 1000);
            }
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }

        void finished()
        {
            require(WaitForSingleObject(info.hProcess, 10000) == WAIT_OBJECT_0, "test child timed out");
            DWORD result{};
            require(GetExitCodeProcess(info.hProcess, &result) && result == 0, "test child failed");
        }
    };

    auto open_client(const std::wstring& path) -> HANDLE
    {
        const auto deadline = GetTickCount64() + 3000;
        do
        {
            auto pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr);
            if (pipe != INVALID_HANDLE_VALUE)
            {
                return pipe;
            }
            require(GetLastError() == ERROR_PIPE_BUSY, "unexpected pipe open failure");
            WaitNamedPipeW(path.c_str(), 50);
        } while (GetTickCount64() < deadline);
        throw std::runtime_error("pipe open timed out");
    }

    auto transfer(HANDLE pipe, void* data, DWORD bytes, bool writing, DWORD timeout = 3000) -> bool
    {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED operation{};
        operation.hEvent = event.value;
        DWORD transferred{};
        bool completed = writing ? WriteFile(pipe, data, bytes, &transferred, &operation) != FALSE
                                 : ReadFile(pipe, data, bytes, &transferred, &operation) != FALSE;
        if (!completed && GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(event.value, timeout) != WAIT_OBJECT_0)
            {
                CancelIoEx(pipe, &operation);
                GetOverlappedResult(pipe, &operation, &transferred, TRUE);
                throw std::runtime_error("client I/O timed out");
            }
            completed = GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
        }
        return completed && transferred == bytes;
    }

    // Uses production OwnerPipe and the unchanged request/reply layouts, with
    // synthetic payloads only. No game, assets, source keys or executable bytes.
    struct EchoServer
    {
        kh02ipc::OwnerPipe pipe;
        std::atomic<unsigned> requests{};
        std::atomic<unsigned> rejected{};
        Handle connected{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        std::thread worker;

        EchoServer(const std::wstring& path, uint32_t owner)
            : pipe(path, owner, kh02asset::max_read + sizeof(kh02asset::Response)), worker([this] { run(); })
        {
        }

        ~EchoServer()
        {
            pipe.stop();
            worker.join();
        }

        void run()
        {
            while (!pipe.stopped())
            {
                if (pipe.connect())
                {
                    SetEvent(connected.value);
                    kh02asset::Request request{};
                    while (pipe.read(&request, sizeof(request)))
                    {
                        if (request.magic != kh02asset::magic || request.version != kh02asset::version ||
                            request.length > kh02asset::max_read)
                        {
                            break;
                        }
                        ++requests;
                        kh02asset::Response response{kh02asset::magic,      0,
                                                     GetCurrentProcessId(), request.length,
                                                     kh02asset::max_read,   request.offset,
                                                     request.nonce,         request.catalog_tag};
                        std::vector<unsigned char> output(sizeof(response) + request.length, 0x5a);
                        std::memcpy(output.data(), &response, sizeof(response));
                        if (!pipe.write(output.data(), static_cast<uint32_t>(output.size())))
                        {
                            break;
                        }
                    }
                }
                else if (!pipe.stopped())
                {
                    ++rejected;
                }
                pipe.disconnect();
            }
            SetEvent(done.value);
        }
    };

    void check_signatures()
    {
        using kh02integrity::signature_matches;
        constexpr auto sha256_abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        std::array<char, 4> sample{'a', 'b', 'c', 'd'};
        require(signature_matches(sample.data(), 3, sha256_abc), "known SHA-256 vector");
        sample[1] = 'x';
        require(!signature_matches(sample.data(), 3, sha256_abc), "changed instructions must fail");
        sample[1] = 'b';
        require(!signature_matches(sample.data(), 4, sha256_abc), "length mismatch must fail");
        require(!signature_matches(sample.data(), 0, sha256_abc), "empty signature must fail");
        require(!signature_matches(sample.data(), 65, sha256_abc), "signature bound must hold");
        require(!signature_matches(sample.data(), 3, "not-a-hash"), "malformed digest must fail");
        require(!signature_matches(nullptr, 3, sha256_abc), "null signature address must fail");
        require(!signature_matches(reinterpret_cast<void*>(1), 3, sha256_abc), "unreadable memory must fail");
    }

    void check_modes()
    {
        using kh02::RuntimeMode;
        auto select = [](const wchar_t* command, bool compiled)
        {
            int count{};
            auto* arguments = CommandLineToArgvW(command, &count);
            require(arguments != nullptr, "parse test command line");
            std::vector<std::wstring_view> options;
            for (int index = 1; index < count; ++index)
            {
                options.emplace_back(arguments[index]);
            }
            const auto mode = kh02::select_runtime_mode(options, compiled);
            LocalFree(arguments);
            return mode;
        };
        for (const bool compiled : {false, true})
        {
            require(select(L"game.exe", compiled) == RuntimeMode::disabled, "ordinary game must remain inactive");
            require(select(L"game.exe -KH02AssetCompanion", compiled) == RuntimeMode::companion, "companion mode");
            require(select(L"game.exe -KH02Diagnostics -KH02AssetCompanion", compiled) == RuntimeMode::companion,
                    "companion never enables diagnostics");
            require(select(L"game.exe -KH02DiagnosticsExtra", compiled) == RuntimeMode::disabled,
                    "exact flag required");
            require(select(L"game.exe \"value -KH02Diagnostics\"", compiled) == RuntimeMode::disabled,
                    "embedded flag text is not an option");
            require(select(L"game.exe \"-KH02Diagnostics\"", compiled) ==
                        (compiled ? RuntimeMode::diagnostics : RuntimeMode::disabled),
                    "diagnostic build opt-in");
        }
    }

    void check_security_descriptor()
    {
        kh02ipc::UserSecurity security;
        PACL dacl{};
        BOOL present{};
        BOOL defaulted{};
        require(GetSecurityDescriptorDacl(security.descriptor, &present, &dacl, &defaulted) && present && dacl,
                "explicit pipe DACL");
        require(dacl->AceCount == 2, "only current user and SYSTEM allowed");
        for (DWORD index = 0; index < dacl->AceCount; ++index)
        {
            void* raw{};
            require(GetAce(dacl, index, &raw) != FALSE, "DACL entry");
            auto* entry = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            auto* sid = reinterpret_cast<PSID>(&entry->SidStart);
            require(!IsWellKnownSid(sid, WinWorldSid) && !IsWellKnownSid(sid, WinAnonymousSid) &&
                        !IsWellKnownSid(sid, WinBuiltinUsersSid),
                    "broad accounts must not be allowed");
        }
    }

    void check_transport()
    {
        const auto path = L"\\\\.\\pipe\\" + name(L"_assets");
        EchoServer server(path, GetCurrentProcessId());
        {
            Child rogue(L"--rogue " + path);
            rogue.finished();
            require(server.rejected == 1 && server.requests == 0, "unrelated process must receive no payload");
        }
        {
            Handle client(open_client(path));
            for (const uint32_t length : {0u, 17u, kh02asset::max_read})
            {
                kh02asset::Request request{kh02asset::magic, kh02asset::version, 0, length, 7, 123, 456};
                require(transfer(client.value, &request, 3, true), "first request fragment");
                require(transfer(client.value, reinterpret_cast<char*>(&request) + 3, sizeof(request) - 3, true),
                        "second request fragment");
                kh02asset::Response response{};
                require(transfer(client.value, &response, sizeof(response), false), "response header");
                require(response.length == length && response.nonce == 123 && response.catalog_tag == 456 &&
                            response.offset == 7 && response.pid == GetCurrentProcessId(),
                        "wire format preserved");
                std::vector<unsigned char> payload(length);
                if (length)
                {
                    require(transfer(client.value, payload.data(), length, false), "response payload");
                    require(std::all_of(payload.begin(), payload.end(), [](auto byte) { return byte == 0x5a; }),
                            "payload intact");
                }
            }
        }
        {
            Handle client(open_client(path));
            unsigned char partial = 1;
            require(transfer(client.value, &partial, 1, true), "partial request");
            require(!transfer(client.value, &partial, 1, false, 8000), "stalled partial request must disconnect");
        }
        require(server.requests == 3, "partial request must not reach reader");
        ResetEvent(server.connected.value);
        {
            Handle client(open_client(path));
            require(WaitForSingleObject(server.connected.value, 1000) == WAIT_OBJECT_0, "owner connected");
            server.pipe.stop();
            require(WaitForSingleObject(server.done.value, 1000) == WAIT_OBJECT_0, "cancel pending I/O promptly");
        }
    }

    void check_owner_exit()
    {
        const auto event_name = L"Local\\" + name(L"_owner_exit");
        Handle exit_event(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
        require(exit_event.value != nullptr, "owner exit event");
        Child owner(L"--owner " + event_name);
        EchoServer server(L"\\\\.\\pipe\\" + name(L"_lifetime"), owner.info.dwProcessId);
        SetEvent(exit_event.value);
        owner.finished();
        require(WaitForSingleObject(server.done.value, 1000) == WAIT_OBJECT_0 && server.pipe.stopped(),
                "owner exit cancels pending connect and closes session");
    }

    void check_cross_process_owner()
    {
        const auto path = L"\\\\.\\pipe\\" + name(L"_cross_process");
        const auto ready_name = L"Local\\" + name(L"_server_ready");
        const auto stop_name = L"Local\\" + name(L"_server_stop");
        Handle ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
        Handle stop(CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str()));
        require(ready.value && stop.value, "cross-process events");
        Child source(L"--server " + path + L" " + std::to_wstring(GetCurrentProcessId()) + L" " + ready_name + L" " +
                     stop_name);
        require(WaitForSingleObject(ready.value, 3000) == WAIT_OBJECT_0, "child server ready");
        Handle client(open_client(path));
        ULONG source_pid{};
        require(GetNamedPipeServerProcessId(client.value, &source_pid) && source_pid == source.info.dwProcessId,
                "owner verifies source Win32 PID");
        kh02asset::Request request{kh02asset::magic, kh02asset::version, 0, 0, 7, 123, 456};
        require(transfer(client.value, &request, sizeof(request), true), "cross-process request");
        kh02asset::Response response{};
        require(transfer(client.value, &response, sizeof(response), false) && response.pid == source_pid &&
                    response.nonce == request.nonce,
                "source authenticates owner Win32 PID");
        SetEvent(stop.value);
        source.finished();
    }
} // namespace

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 6 && std::wstring_view(argv[1]) == L"--server")
        {
            Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[4]));
            Handle stop(OpenEventW(SYNCHRONIZE, FALSE, argv[5]));
            require(ready.value && stop.value, "child server events");
            EchoServer server(argv[2], static_cast<uint32_t>(std::stoul(argv[3])));
            SetEvent(ready.value);
            require(WaitForSingleObject(stop.value, 10000) == WAIT_OBJECT_0, "child server shutdown");
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--rogue")
        {
            Handle client(open_client(argv[2]));
            kh02asset::Request request{kh02asset::magic, kh02asset::version, 0, 17, 7, 123, 456};
            (void)transfer(client.value, &request, sizeof(request), true);
            unsigned char byte{};
            require(!transfer(client.value, &byte, 1, false), "unauthorized client was not disconnected");
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--owner")
        {
            Handle event(OpenEventW(SYNCHRONIZE, FALSE, argv[2]));
            require(event.value && WaitForSingleObject(event.value, 10000) == WAIT_OBJECT_0, "owner lifetime test");
            return 0;
        }
        check_signatures();
        kh02control::Identity control_identity{101, 202, 303, 404};
        control_identity.nonce.fill('a');
        kh02control::Request control_request;
        control_request.identity = control_identity;
        control_request.generation = 1;
        control_request.client_pid = control_identity.owner_pid;
        require(kh02control::validate(control_request, control_identity, control_identity.owner_pid) ==
                    kh02control::Status::accepted,
                "movie control accepts its actual owner");
        control_request.client_pid = 909;
        require(kh02control::validate(control_request, control_identity, 909) == kh02control::Status::wrong_session,
                "movie control rejects a non-owner even with matching session credentials");
        check_modes();
        check_startup_gate();
        check_security_descriptor();
        check_transport();
        check_owner_exit();
        check_cross_process_owner();
        std::cout << "PASS: SHA-256 guards, diagnostic opt-in, pipe ACL, peer rejection, fragmented/max-size RPC, "
                     "partial timeout, cancellation, owner exit and cross-process ownership\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
