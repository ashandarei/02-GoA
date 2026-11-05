#include "companion_audio.h"
#define NOMINMAX
#include <Windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace kh02
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        std::thread worker;
        std::atomic<bool> stopping{false};
        std::mutex mutex;
        std::condition_variable changed;
        nlohmann::json status;
        std::filesystem::path journal;

        auto text(const wchar_t* value) -> std::string
        {
            const auto count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
            std::string result(count, 0);
            WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), count, nullptr, nullptr);
            if (!result.empty())
            {
                result.pop_back();
            }
            return result;
        }

        void save(const std::map<std::string, bool>& saved)
        {
            if (saved.empty())
            {
                std::filesystem::remove(journal);
                return;
            }
            const auto temporary = journal.wstring() + L".tmp";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out << nlohmann::json(saved).dump();
                out.flush();
                if (!out)
                {
                    throw std::runtime_error("audio_restore_journal_write");
                }
            }
            if (!MoveFileExW(temporary.c_str(), journal.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                throw std::runtime_error("audio_restore_journal_replace");
            }
        }

        auto load() -> std::map<std::string, bool>
        {
            if (!std::filesystem::exists(journal))
            {
                return {};
            }
            if (std::filesystem::file_size(journal) > 65536)
            {
                throw std::runtime_error("audio_restore_journal_size");
            }
            std::ifstream input(journal);
            const auto doc = nlohmann::json::parse(input);
            if (!doc.is_object() || doc.size() > 128)
            {
                throw std::runtime_error("audio_restore_journal_shape");
            }
            return doc.get<std::map<std::string, bool>>();
        }

        void run(bool companion)
        {
            const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialized))
            {
                std::lock_guard lock(mutex);
                status["last_error"] = "audio_COM_initialization";
                return;
            }
            {
                std::map<std::string, bool> saved;
                std::map<std::string, ComPtr<ISimpleAudioVolume>> held;
                uint64_t writes = 0, restored = 0;
                const auto restore_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                try
                {
                    saved = load();
                }
                catch (const std::exception& e)
                {
                    std::lock_guard lock(mutex);
                    status["last_error"] = e.what();
                    CoUninitialize();
                    return;
                }
                while (!stopping)
                {
                    unsigned observed = 0, muted = 0;
                    std::string error;
                    try
                    {
                        ComPtr<IMMDeviceEnumerator> devices;
                        ComPtr<IMMDeviceCollection> endpoints;
                        UINT number = 0;
                        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                                    IID_PPV_ARGS(&devices))) ||
                            FAILED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints)) ||
                            FAILED(endpoints->GetCount(&number)))
                        {
                            throw std::runtime_error("audio_endpoint_enumeration");
                        }
                        for (UINT i = 0; i < number; ++i)
                        {
                            ComPtr<IMMDevice> device;
                            ComPtr<IAudioSessionManager2> manager;
                            ComPtr<IAudioSessionEnumerator> sessions;
                            int count = 0;
                            if (FAILED(endpoints->Item(i, &device)) ||
                                FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                                        reinterpret_cast<void**>(manager.GetAddressOf()))) ||
                                FAILED(manager->GetSessionEnumerator(&sessions)) || FAILED(sessions->GetCount(&count)))
                            {
                                throw std::runtime_error("audio_session_enumeration");
                            }
                            LPWSTR endpoint_id = nullptr;
                            if (FAILED(device->GetId(&endpoint_id)))
                            {
                                throw std::runtime_error("audio_endpoint_identity");
                            }
                            const auto endpoint = text(endpoint_id);
                            CoTaskMemFree(endpoint_id);
                            for (int j = 0; j < count; ++j)
                            {
                                ComPtr<IAudioSessionControl> control;
                                ComPtr<IAudioSessionControl2> identity;
                                ComPtr<ISimpleAudioVolume> volume;
                                DWORD pid = 0;
                                if (FAILED(sessions->GetSession(j, &control)) || FAILED(control.As(&identity)) ||
                                    identity->GetProcessId(&pid) != S_OK || pid != GetCurrentProcessId())
                                {
                                    continue;
                                }
                                if (FAILED(control.As(&volume)))
                                {
                                    throw std::runtime_error("audio_volume_interface");
                                }
                                LPWSTR session_id = nullptr;
                                if (FAILED(identity->GetSessionIdentifier(&session_id)))
                                {
                                    throw std::runtime_error("audio_session_identity");
                                }
                                const auto key = endpoint + "\n" + text(session_id);
                                CoTaskMemFree(session_id);
                                BOOL before = FALSE, after = FALSE;
                                if (FAILED(volume->GetMute(&before)))
                                {
                                    throw std::runtime_error("audio_mute_read");
                                }
                                if (companion)
                                {
                                    ++observed;
                                    if (!saved.contains(key))
                                    {
                                        saved.emplace(key, before != FALSE);
                                        save(saved);
                                    }
                                    held[key] = volume;
                                    if (FAILED(volume->SetMute(TRUE, nullptr)) || FAILED(volume->GetMute(&after)) ||
                                        !after)
                                    {
                                        throw std::runtime_error("audio_mute_readback");
                                    }
                                    ++muted;
                                    ++writes;
                                }
                                else if (const auto old = saved.find(key); old != saved.end())
                                {
                                    if (FAILED(volume->SetMute(old->second, nullptr)) ||
                                        FAILED(volume->GetMute(&after)) || (after != FALSE) != old->second)
                                    {
                                        throw std::runtime_error("audio_restore_readback");
                                    }
                                    saved.erase(old);
                                    save(saved);
                                    ++restored;
                                }
                            }
                        }
                    }
                    catch (const std::exception& e)
                    {
                        error = e.what();
                    }
                    {
                        std::unique_lock lock(mutex);
                        status.update({{"requested", companion},
                                       {"monitor_running", true},
                                       {"session_count", observed},
                                       {"muted_sessions", muted},
                                       {"muted", companion && observed > 0 && muted == observed && error.empty()},
                                       {"apply_calls", writes},
                                       {"restored_sessions", restored},
                                       {"restore_pending", saved.size()},
                                       {"last_error", error},
                                       {"method", "Windows_own_process_audio_sessions"}});
                        if (!companion && (saved.empty() || std::chrono::steady_clock::now() >= restore_deadline))
                        {
                            break;
                        }
                        changed.wait_for(lock, std::chrono::milliseconds(100), [] { return stopping.load(); });
                    }
                }
                // Normal unload restores captured states. After owner/job termination, the
                // next ordinary 0.2 launch restores these owned session identifiers.
                for (const auto& [key, volume] : held)
                {
                    const auto old = saved.find(key);
                    if (old == saved.end())
                    {
                        continue;
                    }
                    BOOL actual = FALSE;
                    if (SUCCEEDED(volume->SetMute(old->second, nullptr)) && SUCCEEDED(volume->GetMute(&actual)) &&
                        (actual != FALSE) == old->second)
                    {
                        saved.erase(old);
                    }
                }
                try
                {
                    save(saved);
                }
                catch (...)
                {
                }
                {
                    std::lock_guard lock(mutex);
                    status["monitor_running"] = false;
                }
            } // Release audio session interfaces before leaving the COM apartment.
            CoUninitialize();
        }
    } // namespace

    void start_companion_audio(bool companion)
    {
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, 32768);
        journal = std::filesystem::path(path).parent_path() / L"kh02_bridge" / L"companion_audio_restore.json";
        status = {{"requested", companion},
                  {"monitor_running", false},
                  {"muted", false},
                  {"apply_calls", 0},
                  {"last_error", ""}};
        if (!companion && !std::filesystem::exists(journal))
        {
            return;
        }
        stopping = false;
        worker = std::thread(run, companion);
    }

    void stop_companion_audio()
    {
        stopping = true;
        changed.notify_all();
        if (worker.joinable())
        {
            worker.join();
        }
    }

    auto companion_audio_status() -> std::string
    {
        std::lock_guard lock(mutex);
        return status.dump();
    }
} // namespace kh02
