#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <vector>
#include "asset_reader.h"
#include "asset_service.h"
#include "shared/kh02/kh02_executable_verification.h"
#include "shared/kh02/kh02_runtime_mode.h"
#if KH02_ENABLE_DIAGNOSTICS
#include "bridge.h"
#include "engine_tick.h"
#include "companion_audio.h"
#endif

namespace
{
    auto requested_mode() -> kh02::RuntimeMode
    {
        int count{};
        auto* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!arguments)
        {
            throw std::runtime_error("kh02_command_line_parse_failed");
        }

        struct ArgumentStorage
        {
            LPWSTR* value;

            ~ArgumentStorage()
            {
                LocalFree(value);
            }
        } storage{arguments};

        std::vector<std::wstring_view> options;
        for (int index = 1; index < count; ++index)
        {
            options.emplace_back(arguments[index]);
        }
        return kh02::select_runtime_mode(options, KH02_ENABLE_DIAGNOSTICS != 0);
    }
} // namespace

class Kh02Probe final : public RC::CppUserModBase
{
    bool initialized_ = false;
    kh02::RuntimeMode mode_ = kh02::RuntimeMode::disabled;

  public:
    ~Kh02Probe() override
    {
        if (mode_ == kh02::RuntimeMode::companion)
        {
            kh02::stop_asset_service();
            kh02::stop_asset_reader();
        }
#if KH02_ENABLE_DIAGNOSTICS
        if (mode_ == kh02::RuntimeMode::diagnostics)
        {
            kh02::stop_companion_audio();
            kh02::uninstall_engine_tick();
        }
#endif
    }

    Kh02Probe()
    {
        ModName = STR("KH02APProbeCpp");
        ModVersion = STR("2.0.1");
        ModDescription = STR("KH 0.2 Garden of Assemblage companion");
        ModAuthors = STR("aesais");
        wchar_t executable[32768]{};
        try
        {
            mode_ = requested_mode();
            if (mode_ == kh02::RuntimeMode::disabled)
            {
                RC::Output::send<RC::LogLevel::Default>(
                    STR("[KH02APProbeCpp] inactive; companion launch or an opted-in diagnostic build required\n"));
                return;
            }
            if (!GetModuleFileNameW(nullptr, executable, 32768))
            {
                throw std::runtime_error("kh02_executable_path_failed");
            }
            kh02integrity::verify_executable(executable);
            RC::Output::send<RC::LogLevel::Default>(STR("[KH02APProbeCpp] original executable SHA-256 verified\n"));
        }
        catch (const std::exception& error)
        {
            kh02integrity::terminate_failed_verification(error.what());
        }
        if (mode_ == kh02::RuntimeMode::companion)
        {
            kh02::start_early_asset_service();
        }
        RC::Output::send<RC::LogLevel::Default>(STR("[KH02APProbeCpp] DLL constructed; diagnostics_compiled={}\n"),
                                                KH02_ENABLE_DIAGNOSTICS != 0);
    }

    auto on_unreal_init() -> void override
    {
        if (mode_ == kh02::RuntimeMode::companion)
        {
            kh02::note_asset_unreal_ready();
            RC::Output::send<RC::LogLevel::Default>(STR("[KH02APProbeCpp] early asset service; assets_ready={}\n"),
                                                    kh02::asset_service_ready());
        }
#if KH02_ENABLE_DIAGNOSTICS
        else if (mode_ == kh02::RuntimeMode::diagnostics)
        {
            kh02::initialize_bridge();
            kh02::start_companion_audio(false);
            // Asset reads require an authenticated companion owner. A normal
            // diagnostic game session never exposes an unowned asset pipe.
        }
#endif
        initialized_ = true;
    }

    auto on_update() -> void override
    {
#if KH02_ENABLE_DIAGNOSTICS
        if (initialized_ && mode_ == kh02::RuntimeMode::diagnostics)
        {
            kh02::poll_bridge();
        }
#endif
    }
};

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod()
{
    return new Kh02Probe();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}
