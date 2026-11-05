#include "shared/kh02/kh02_memory_signature.h"
#include "bridge.h"
#include "asset_reader.h"
#include "companion_audio.h"
#include "engine_tick.h"
#include "kh3ap/bridge/json_fields.h"
#include "kh3ap/core/uobject_validity.h"
#include <Unreal/AActor.hpp>
#include <Unreal/FProperty.hpp>
#include <Unreal/FWeakObjectPtr.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/Property/FBoolProperty.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#define NOMINMAX
#include <Windows.h>
#include "build_profile.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace kh02
{
    using namespace RC::Unreal;
    using Clock = std::chrono::steady_clock;

    namespace
    {
        std::filesystem::path root;
        std::mutex exchange;
        std::string queued, result, snapshot;
        std::wstring last_request;
        std::wstring session;
        std::atomic<bool> game_work{true};
        std::atomic<bool> objective_due{true};
        std::string objective_report;
        Clock::time_point next_objective_player_search{};
        FName receive_tick, aqua_class;
        FName direct_reward_script_class;
        uint32_t suppressed_script_rewards = 0;
        FWeakObjectPtr aqua_controller, aqua_pawn;
        FWeakObjectPtr help_ui;
        std::atomic<DWORD> game_thread{0};
        bool announced = false;

        struct FrameProfile
        {
            bool active = false, legacy_poll = false;
            Clock::time_point end{}, last{}, next_scan{};
            std::vector<double> intervals, scans;
            std::wstring id;
        } profile;

        std::string profile_result;

        struct EventTrace
        {
            bool active = false;
            Clock::time_point end{};
            std::wstring id;
            std::vector<FName> names;
            std::vector<std::string> events;
        } trace;

        std::string trace_result;
        std::string chest_test_report;
        bool chest_test_armed = false, chest_test_attempted = false;
        UObject* opening_chest = nullptr; // Valid only across one original native call.
        std::wstring test_chest_id;
        std::map<std::wstring, int32_t> test_inventory;
        std::map<std::wstring, FName> original_rewards;
        Clock::time_point next_poll{}, next_tick{};

        int64_t epoch_ms()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        std::string narrow(const std::wstring& s)
        {
            if (s.empty())
            {
                return {};
            }
            const auto n =
                WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
            std::string out(n, 0);
            WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
            return out;
        }

        std::string quote(const std::wstring& s)
        {
            std::string out = "\"";
            for (unsigned char c : narrow(s))
            {
                if (c == '"' || c == '\\')
                {
                    out += '\\';
                    out += c;
                }
                else if (c >= 32)
                {
                    out += c;
                }
            }
            return out + '"';
        }

        bool valid(UObject* p)
        {
            return kh3ap::is_uobject_valid_fast(p);
        }

        template <class T> T property(UObject* p, const wchar_t* name)
        {
            if (!valid(p))
            {
                return {};
            }
            auto* v = p->GetValuePtrByPropertyNameInChain<T>(name);
            return v ? *v : T{};
        }

        std::vector<UObject*> instances(const wchar_t* name)
        {
            std::vector<UObject*> out;
            UObjectGlobals::FindAllOf(name, out);
            std::erase_if(out,
                          [](UObject* p)
                          {
                              return !valid(p) || p->HasAnyFlags(static_cast<EObjectFlags>(RF_ClassDefaultObject |
                                                                                           RF_ArchetypeObject));
                          });
            return out;
        }

        UObject* aqua()
        {
            auto* controller = aqua_controller.Get();
            auto* cached = aqua_pawn.Get();
            if (valid(controller) && valid(cached) && property<UObject*>(controller, L"Pawn") == cached &&
                cached->GetClassPrivate()->GetNamePrivate() == aqua_class)
            {
                return cached;
            }
            aqua_controller = nullptr;
            aqua_pawn = nullptr;
            for (auto* controller : instances(L"TresPlayerControllerSora"))
            {
                auto* pawn = property<UObject*>(controller, L"Pawn");
                if (valid(pawn) && pawn->GetClassPrivate()->GetNamePrivate() == aqua_class)
                {
                    aqua_controller = controller;
                    aqua_pawn = pawn;
                    return pawn;
                }
            }
            return nullptr;
        }

        std::string describe()
        {
            auto* pawn = aqua();
            std::ostringstream out;
            out << "{\"protocol\":1,\"game\":\"kh02\",\"pid\":" << GetCurrentProcessId()
                << ",\"session\":" << quote(session) << ",\"ready\":true,\"game_thread_id\":" << GetCurrentThreadId()
                << ",\"aqua\":" << (pawn ? quote(pawn->GetFullName()) : "null");
            out << ",\"save_points\":[";
            bool first = true;
            for (auto* p : instances(L"TresSavePointActor"))
            {
                if (!first)
                {
                    out << ',';
                }
                first = false;
                auto* check = property<UObject*>(p, L"m_pCheckPawn");
                out << "{\"actor\":" << quote(p->GetFullName())
                    << ",\"player_start\":" << quote(property<FName>(p, L"PlayerStartTag").ToString())
                    << ",\"aqua_in_range\":" << (pawn && check == pawn ? "true" : "false") << "}";
            }
            out << "],\"chests\":[";
            first = true;
            for (auto* p : instances(L"TresTreasureBox"))
            {
                if (!first)
                {
                    out << ',';
                }
                first = false;
                out << "{\"id\":" << quote(property<FName>(p, L"m_TreasureID").ToString())
                    << ",\"actor\":" << quote(p->GetFullName()) << "}";
            }
            out << "]}";
            return out.str();
        }

        struct Row
        {
            FName key;
            unsigned char* value;
            uint64_t sparse;
        };

        struct Header
        {
            Row* data;
            int32_t count, capacity;
        };

        static_assert(sizeof(Row) == 24);

        template <class T> bool read_native(const void* p, T& out) noexcept
        {
            __try
            {
                std::memcpy(&out, p, sizeof(T));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        template <class T> T native_field(const void* p, size_t offset)
        {
            T value{};
            if (!p || !read_native(static_cast<const unsigned char*>(p) + offset, value))
            {
                throw std::runtime_error("unreadable_native_field");
            }
            return value;
        }

        std::string inspect_objectives(bool modal)
        {
            // Constant-size read on the game thread. Never retain the game-instance
            // pointer across frames or scan GUObjectArray in the steady polling path.
            auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
            const auto dos = native_field<IMAGE_DOS_HEADER>(base, 0);
            if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000)
            {
                throw std::runtime_error("unsupported_image");
            }
            const auto nt = native_field<IMAGE_NT_HEADERS64>(base, dos.e_lfanew);
            if (nt.FileHeader.TimeDateStamp != kh02build::current().timestamp ||
                nt.OptionalHeader.SizeOfImage != kh02build::current().image_size)
            {
                throw std::runtime_error("unsupported_objective_layout");
            }
            constexpr auto getter = "f0a4e1725e08774646e7f9b9890bd3d8e54187e04d857cc07d0b0fd5c8be3ad4";
            if (!kh02integrity::signature_matches(base + kh02build::rva(0x44BDD0), 8, getter))
            {
                throw std::runtime_error("objective_getter_signature_mismatch");
            }
            auto* controller = aqua_controller.Get();
            auto* pawn = aqua_pawn.Get();
            bool playable =
                !modal && valid(controller) && valid(pawn) && property<UObject*>(controller, L"Pawn") == pawn;
            if (!modal && !playable && Clock::now() >= next_objective_player_search)
            {
                next_objective_player_search = Clock::now() + std::chrono::seconds(5);
                playable = aqua() != nullptr;
            }
            std::string out = "{\"protocol\":1,\"game\":\"kh02\",\"session\":" + quote(session) +
                              ",\"observed_at_ms\":" + std::to_string(epoch_ms()) +
                              ",\"ready\":" + (playable ? "true" : "false");
            if (!playable)
            {
                return out + ",\"states\":[]}";
            }
            auto* engine = native_field<UObject*>(base, kh02build::rva(0x480B5D0));
            if (!valid(engine))
            {
                throw std::runtime_error("objective_engine_unavailable");
            }
            auto* viewport = native_field<UObject*>(engine, 0x230);
            if (!valid(viewport))
            {
                throw std::runtime_error("objective_viewport_unavailable");
            }
            auto* instance = native_field<UObject*>(viewport, 0x50);
            if (!valid(instance) || instance->GetClassPrivate()->GetName() != L"TresGameInstance")
            {
                throw std::runtime_error("objective_instance_unavailable");
            }
            auto* manager = property<UObject*>(instance, L"m_pChallengeManager");
            if (!valid(manager) || manager->GetClassPrivate()->GetName() != L"TresGameChallengeManager")
            {
                throw std::runtime_error("objective_manager_unavailable");
            }
            out += ",\"states\":[";
            for (size_t i = 0; i < 51; ++i)
            {
                auto state = native_field<uint32_t>(instance, 0x338 + 0xC20 + i * 4) & 15;
                if (state > 2)
                {
                    throw std::runtime_error("invalid_objective_state");
                }
                if (i)
                {
                    out += ',';
                }
                out += std::to_string(state);
            }
            return out + "]}";
        }

        std::string load_latest()
        {
            if (aqua())
            {
                throw std::runtime_error("refusing_load_over_gameplay");
            }
            auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
            auto dos = native_field<IMAGE_DOS_HEADER>(base, 0);
            if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000)
            {
                throw std::runtime_error("unsupported_image");
            }
            auto nt = native_field<IMAGE_NT_HEADERS64>(base, dos.e_lfanew);
            // Steam English build 15194283; verified from the loaded module, whose
            // layout differs from the launcher-protected on-disk executable.
            if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.TimeDateStamp != kh02build::current().timestamp ||
                nt.OptionalHeader.SizeOfImage != kh02build::current().image_size)
            {
                throw std::runtime_error("unsupported_image");
            }
            constexpr auto signature = "b07ffc3bc7811d6006b28212c28eaf4366f86b32bbfc7555a6bc841c83ca2898";
            if (!kh02integrity::signature_matches(base + kh02build::rva(0x56E0D0), 16, signature))
            {
                throw std::runtime_error("continue_signature_mismatch");
            }
            auto titles = instances(L"Rebellis02HUDTitle");
            if (titles.size() != 1)
            {
                throw std::runtime_error("title_hud_not_unique");
            }
            auto* hud = titles.front();
            auto* ui = property<UObject*>(hud, L"m_Title");
            if (!valid(ui) || ui->GetClassPrivate()->GetName() != L"Rebellis02UIP_Title")
            {
                throw std::runtime_error("title_ui_not_ready");
            }
            if (native_field<uint32_t>(hud, 0x550) != 20)
            {
                throw std::runtime_error("title_menu_not_ready");
            }
            auto* delegate = native_field<void*>(ui, 0x1E0);
            auto* target = native_field<void*>(delegate, 0x10);
            if (target != base + kh02build::rva(0x56E0D0) || native_field<FWeakObjectPtr>(delegate, 8).Get() != hud)
            {
                throw std::runtime_error("continue_delegate_mismatch");
            }
            // Normal Continue delegate: selects the newest runtime save-catalog entry,
            // loads it, and schedules the game's own world-loading operation. This is
            // not the adjacent New Game delegate, a Debug call, or a guessed UI index.
            auto accepted = reinterpret_cast<bool(__fastcall*)(UObject*)>(target)(hud);
            return std::string("{\"accepted\":") + (accepted ? "true" : "false") + ",\"ready\":false}";
        }

        UObject* startup_help()
        {
            auto* cached = help_ui.Get();
            if (valid(cached))
            {
                return native_field<uint8_t>(cached, 0x99) ? cached : nullptr;
            }
            for (auto* ui : instances(L"Rebellis02UIPUB_GameHelp"))
            {
                help_ui = ui;
                if (native_field<uint8_t>(ui, 0x99))
                {
                    return ui;
                }
            }
            return nullptr;
        }

        std::string dismiss_help()
        {
            auto* ui = startup_help();
            if (!ui)
            {
                return "{\"active\":false}";
            }
            auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
            // Active unblockable help uses its native cancel action at 0x5A60A0,
            // reached from its input handler at 0x5AEF49. It invokes "closeGameHelp"
            // on the movie player; normal animation completion retires the delegates.
            auto* vtable = native_field<void*>(ui, 0);
            if (native_field<void*>(vtable, 0x278) != base + kh02build::rva(0x5ABDB0))
            {
                throw std::runtime_error("help_callback_mismatch");
            }
            auto* target = base + kh02build::rva(0x5A60A0);
            constexpr auto signature = "9332b65ec57b77d107767fea996d268daaffcb242aa1c68486272d40d8f9b5ed";
            if (!kh02integrity::signature_matches(target, 16, signature))
            {
                throw std::runtime_error("help_signature_mismatch");
            }
            if (native_field<uint8_t>(ui, 0x982))
            {
                throw std::runtime_error("help_subdialog_active");
            }
            if (!native_field<uint8_t>(ui, 0x1B4))
            {
                throw std::runtime_error("help_not_interactive");
            }
            reinterpret_cast<void(__fastcall*)(UObject*)>(target)(ui);
            return "{\"active\":true,\"dismiss_requested\":true}";
        }

        std::string continue_loading(bool modal)
        {
            if (!modal || !aqua() || startup_help())
            {
                throw std::runtime_error("loading_continue_not_ready");
            }
            auto found = instances(L"Rebellis02UIPUB_NowLoading");
            if (found.size() != 1)
            {
                throw std::runtime_error("loading_ui_not_unique");
            }
            auto* ui = found.front();
            auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
            if (native_field<void*>(ui, 0) != base + kh02build::rva(0x37923E8))
            {
                throw std::runtime_error("loading_vtable_mismatch");
            }
            auto* delegate = native_field<void*>(ui, 0x180);
            if (native_field<void*>(delegate, 0x18) != base + kh02build::rva(0x58BBE0))
            {
                throw std::runtime_error("loading_delegate_mismatch");
            }
            auto* manager = native_field<unsigned char*>(delegate, 8);
            if (native_field<UObject*>(manager, 0x2A8) != ui)
            {
                throw std::runtime_error("loading_owner_mismatch");
            }
            if (native_field<int32_t>(manager, 0x2BC) != 6 || native_field<int32_t>(manager, 0x2C8) != 2 ||
                !native_field<uint8_t>(ui, 0x99) || native_field<float>(ui, 0x1A4) < 1.0f)
            {
                throw std::runtime_error("loading_continue_not_ready");
            }
            constexpr auto signature = "644d25aa3ff943f64b90a82976dea184e47432eb540f85f3638a2f60cd7c98c2";
            auto* target = base + kh02build::rva(0x599ED0);
            if (!kh02integrity::signature_matches(target, 13, signature))
            {
                throw std::runtime_error("loading_tick_signature_mismatch");
            }
            const auto confirm = native_field<int32_t>(manager, 0x354);
            if (confirm < 0 || confirm > 4)
            {
                throw std::runtime_error("loading_confirm_index_invalid");
            }
            auto* edge = reinterpret_cast<int32_t*>(manager + 0x314 + confirm * 12);
            const auto previous = native_field<int32_t>(edge, 0);
            // Invoke the verified phase-6 native Confirm branch once. This closes both
            // loading UI parts, starts the normal two-second completion delay, and
            // advances to phase 7. Restore the input edge before the message pump resumes;
            // no OS input is sent and no subsequent menu receives this confirmation.
            *edge = 1;
            reinterpret_cast<void(__fastcall*)(void*, float)>(target)(manager, 0.0f);
            *edge = previous;
            if (native_field<int32_t>(manager, 0x2BC) != 7)
            {
                throw std::runtime_error("loading_continue_not_accepted");
            }
            return "{\"continue_requested\":true}";
        }

        // Stock loaded DataTable only. Validate every row before mutating anything.
        std::vector<std::pair<std::wstring, FName*>> treasure_rows()
        {
            auto* table = UObjectGlobals::StaticFindObject<UObject*>(
                nullptr, nullptr, L"/Game/Load/Rebellis/TresTreasureData.TresTreasureData");
            if (!valid(table))
            {
                throw std::runtime_error("treasure_table_not_loaded");
            }
            auto* structure = property<UObject*>(table, L"RowStruct");
            if (!valid(structure) || structure->GetName() != L"TresTreasureDataTable")
            {
                throw std::runtime_error("unexpected_row_structure");
            }
            auto* item_field = structure->GetPropertyByNameInChain(L"m_TreasureName");
            if (!item_field || item_field->GetOffset_Internal() != 8 || item_field->GetElementSize() != sizeof(FName))
            {
                throw std::runtime_error("unexpected_treasure_item_layout");
            }
            Header h{};
            if (!read_native(reinterpret_cast<unsigned char*>(table) + 0x48, h) || h.count != 41 ||
                h.capacity < h.count || h.capacity > 256)
            {
                throw std::runtime_error("unexpected_treasure_row_map");
            }
            std::vector<std::pair<std::wstring, FName*>> rows;
            for (int i = 0; i < h.count; i++)
            {
                Row row{};
                FName value{};
                if (!read_native(h.data + i, row) || !row.value || !read_native(row.value + 8, value))
                {
                    throw std::runtime_error("unreadable_treasure_row");
                }
                auto name = row.key.ToString();
                if (!name.starts_with(L"DW_BOX") && !name.starts_with(L"DW_LBOX"))
                {
                    throw std::runtime_error("unknown_treasure_row");
                }
                rows.emplace_back(name, reinterpret_cast<FName*>(row.value + 8));
            }
            return rows;
        }

        UObject* item_manager()
        {
            auto managers = instances(L"TresGameInstance");
            if (managers.size() != 1)
            {
                throw std::runtime_error("game_instance_not_unique");
            }
            return managers.front();
        }

        int32_t item_count(UObject* manager, const wchar_t* token)
        {
            auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
            auto* target = native_field<void*>(native_field<void*>(manager, 0), 0x440);
            if (target != base + kh02build::rva(0x44C4C0))
            {
                throw std::runtime_error("inventory_count_profile_mismatch");
            }
            return reinterpret_cast<int32_t(__fastcall*)(UObject*, FName, int32_t)>(target)(manager,
                                                                                            FName(token, FNAME_Add), 0);
        }

        std::string inventory_json(const std::map<std::wstring, int32_t>& counts)
        {
            std::string out = "{";
            bool first = true;
            for (auto& [name, count] : counts)
            {
                if (!first)
                {
                    out += ',';
                }
                first = false;
                out += quote(name) + ":" + std::to_string(count);
            }
            return out + "}";
        }

        std::map<std::wstring, int32_t> consumable_counts()
        {
            std::map<std::wstring, int32_t> result;
            auto* manager = item_manager();
            for (auto name : {L"ITEM_POTION", L"ITEM_HIGHPOTION", L"ITEM_ETHER", L"ITEM_MEGAETHER", L"ITEM_MEGAPOTION",
                              L"ITEM_ELIXIR", L"ITEM_LASTELIXIR"})
            {
                result[name] = item_count(manager, name);
            }
            return result;
        }

        bool suppress_script_reward(void* raw, uint64_t token_bits)
        {
            auto* context = static_cast<UObject*>(raw);
            if (!valid(context) || context->GetClassPrivate()->GetNamePrivate() != direct_reward_script_class)
            {
                return false;
            }
            FName token{};
            static_assert(sizeof(token) == sizeof(token_bits));
            std::memcpy(&token, &token_bits, sizeof(token));
            if (!token.ToString().starts_with(L"KEY_ITEM_SIGN_"))
            {
                return false;
            }
            // Twelve verified BP_TresGetItem call sites in dw_03_challenge grant zodiac
            // keys directly. Only suppress them when the empty treasure table is active.
            // No changes to the script's branch flow, challenge flags or owning actor.
            for (auto& [key, reward] : treasure_rows())
            {
                if (*reward != FName(0, 0))
                {
                    return false;
                }
            }
            ++suppressed_script_rewards;
            return true;
        }

        void observe_chest(void* raw, bool before)
        {
            if (!chest_test_armed)
            {
                return;
            }
            auto* chest = static_cast<UObject*>(raw);
            if (GetCurrentThreadId() != game_thread.load() || !valid(chest))
            {
                return;
            }
            try
            {
                if (before)
                {
                    if (chest_test_attempted || opening_chest)
                    {
                        return;
                    }
                    if (!(native_field<uint32_t>(chest, 0x5F0) & 1))
                    {
                        return;
                    }
                    if (property<UObject*>(chest, L"m_pOpenner") != aqua())
                    {
                        return;
                    }
                    auto id = property<FName>(chest, L"m_TreasureID").ToString();
                    if (!id.starts_with(L"DW_BOX"))
                    {
                        return; // First delivery test: consumable/small chest.
                    }
                    if (native_field<int32_t>(chest, 0x59C) != 0)
                    {
                        throw std::runtime_error("chest_reward_not_empty");
                    }
                    test_chest_id = id;
                    test_inventory = consumable_counts();
                    opening_chest = chest;
                    return;
                }
                if (opening_chest != chest)
                {
                    return;
                }
                opening_chest = nullptr;
                if ((native_field<uint32_t>(chest, 0x5F0) & 3) != 2)
                {
                    return;
                }
                auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
                auto* opened = native_field<void*>(native_field<void*>(chest, 0), 0x900);
                if (opened != base + kh02build::rva(0x494B70) ||
                    !reinterpret_cast<bool(__fastcall*)(UObject*)>(opened)(chest))
                {
                    throw std::runtime_error("chest_completion_not_recorded");
                }
                chest_test_attempted = true; // Never retry an ambiguous grant in this process.
                auto after_vanilla = consumable_counts();
                if (after_vanilla != test_inventory)
                {
                    throw std::runtime_error("vanilla_inventory_changed");
                }
                if (after_vanilla[L"ITEM_ETHER"] >= 99)
                {
                    throw std::runtime_error("replacement_inventory_full");
                }
                auto* pawn = aqua();
                if (!valid(pawn))
                {
                    throw std::runtime_error("aqua_not_playable");
                }
                auto* function = UObjectGlobals::StaticFindObject<UFunction*>(
                    nullptr, nullptr, L"/Script/TresGame.TresGameBlueprintLibrary:BP_TresGetItem");
                auto* library = UObjectGlobals::StaticFindObject<UObject*>(
                    nullptr, nullptr, L"/Script/TresGame.Default__TresGameBlueprintLibrary");
                if (!valid(function) || !valid(library) || function->GetParmsSize() != 17)
                {
                    throw std::runtime_error("grant_function_layout_mismatch");
                }

                struct Grant
                {
                    UObject* context;
                    FName token;
                    bool show;
                };

                static_assert(offsetof(Grant, token) == 8 && offsetof(Grant, show) == 16);
                Grant args{pawn, FName(L"ITEM_ETHER", FNAME_Add), true};
                library->ProcessEvent(function, &args);
                auto after_grant = consumable_counts();
                auto expected = after_vanilla;
                ++expected[L"ITEM_ETHER"];
                const bool verified = after_grant == expected;
                auto report = "{\"session\":" + quote(session) + ",\"chest\":" + quote(test_chest_id) +
                              ",\"checks\":1,\"grant_attempts\":1,\"replacement\":\"ITEM_ETHER\",\"verified\":" +
                              (verified ? std::string("true") : std::string("false")) +
                              ",\"before\":" + inventory_json(test_inventory) +
                              ",\"after_vanilla\":" + inventory_json(after_vanilla) +
                              ",\"after_replacement\":" + inventory_json(after_grant) + "}";
                {
                    std::lock_guard lock(exchange);
                    chest_test_report = std::move(report);
                }
                chest_test_armed = false;
            }
            catch (const std::exception& e)
            {
                chest_test_armed = false;
                chest_test_attempted = true;
                opening_chest = nullptr;
                std::lock_guard lock(exchange);
                chest_test_report = "{\"session\":" + quote(session) + ",\"verified\":false,\"error\":" +
                                    quote(std::wstring(e.what(), e.what() + strlen(e.what()))) + "}";
            }
        }

        std::string execute(const std::string& request, bool modal)
        {
            using J = kh3ap::JsonFieldReader;
            if (J::string_field(request, "session").value_or(L"") != session)
            {
                throw std::runtime_error("stale_game_session");
            }
            auto deadline = J::int64_field(request, "deadline_ms");
            if (!deadline || *deadline < epoch_ms())
            {
                throw std::runtime_error("request_expired");
            }
            auto op = J::string_field(request, "op").value_or(L"");
            if (op == L"asset_reader_status")
            {
                return asset_reader_status();
            }
            if (op == L"companion_audio_status")
            {
                return companion_audio_status();
            }
            if (op == L"inspect_objectives")
            {
                return inspect_objectives(modal);
            }
            if (modal && op != L"inspect_player" && op != L"dismiss_startup_help" && op != L"continue_loading" &&
                op != L"inspect")
            {
                throw std::runtime_error("operation_unavailable_during_modal_load");
            }
            if (op == L"load_latest")
            {
                return load_latest();
            }
            if (op == L"dismiss_startup_help")
            {
                return dismiss_help();
            }
            if (op == L"continue_loading")
            {
                return continue_loading(modal);
            }
            if (op == L"arm_chest_test")
            {
                if (chest_test_armed || chest_test_attempted)
                {
                    throw std::runtime_error("chest_test_already_armed_or_attempted");
                }
                if (!aqua())
                {
                    throw std::runtime_error("aqua_not_playable");
                }
                for (auto& [key, reward] : treasure_rows())
                {
                    if (*reward != FName(0, 0))
                    {
                        throw std::runtime_error("empty_reward_pak_not_active");
                    }
                }
                if (!install_chest_observer(&observe_chest))
                {
                    throw std::runtime_error("chest_hook_profile_mismatch");
                }
                chest_test_armed = true;
                return "{\"armed\":true,\"replacement\":\"ITEM_ETHER\",\"trigger\":\"next_small_chest_completion\"}";
            }
            if (op == L"inspect_inventory")
            {
                return inventory_json(consumable_counts());
            }
            if (op == L"inspect_script_rewards")
            {
                return "{\"filter\":\"dw_03_challenge_zodiac\",\"suppressed_calls\":" +
                       std::to_string(suppressed_script_rewards) + "}";
            }
            if (op == L"inspect_empty_rewards")
            {
                auto rows = treasure_rows();
                size_t empty = 0;
                for (auto& [key, reward] : rows)
                {
                    if (*reward == FName(0, 0))
                    {
                        ++empty;
                    }
                }
                auto* table = UObjectGlobals::StaticFindObject<UObject*>(
                    nullptr, nullptr, L"/Game/Load/Rebellis/RebellisVBonusData.RebellisVBonusData");
                if (!valid(table))
                {
                    throw std::runtime_error("vbonus_table_not_loaded");
                }
                auto* structure = property<UObject*>(table, L"RowStruct");
                if (!valid(structure) || structure->GetName() != L"TresVictoryBonusDataTable")
                {
                    throw std::runtime_error("unexpected_bonus_structure");
                }
                auto h = native_field<Header>(table, 0x48);
                if (h.count != 14 || h.capacity < 14 || h.capacity > 64)
                {
                    throw std::runtime_error("unexpected_bonus_row_map");
                }
                size_t none = 0;
                std::string bonus = "[";
                for (int i = 0; i < h.count; ++i)
                {
                    auto row = native_field<Row>(h.data, i * sizeof(Row));
                    if (i)
                    {
                        bonus += ',';
                    }
                    bonus += "{\"row\":" + quote(row.key.ToString()) + ",\"values\":[";
                    bool first = true;
                    for (auto name : {L"m_BonusAqua0", L"m_BonusAqua1", L"m_BonusAqua2", L"m_BonusMickey0",
                                      L"m_BonusMickey1", L"m_BonusMickey2"})
                    {
                        auto* field = structure->GetPropertyByNameInChain(name);
                        if (!field || field->GetElementSize() != 1)
                        {
                            throw std::runtime_error("bonus_field_layout_mismatch");
                        }
                        auto value = native_field<uint8_t>(row.value, field->GetOffset_Internal());
                        if (value == 0)
                        {
                            ++none;
                        }
                        if (!first)
                        {
                            bonus += ',';
                        }
                        first = false;
                        bonus += std::to_string(value);
                    }
                    bonus += "]}";
                }
                return "{\"treasures\":41,\"empty_treasures\":" + std::to_string(empty) +
                       ",\"bonus_fields\":84,\"zero_bonus_fields\":" + std::to_string(none) +
                       ",\"bonus_rows\":" + bonus + "]}";
            }
            if (op == L"inspect_player")
            {
                auto* pawn = aqua();
                auto* controller = aqua_controller.Get();
                bool help = startup_help() != nullptr;
                bool ready = valid(pawn) && valid(controller) &&
                             property<UObject*>(controller, L"AcknowledgedPawn") == pawn && !help && !modal;
                return "{\"ready\":" + std::string(ready ? "true" : "false") +
                       ",\"modal_loading\":" + std::string(modal ? "true" : "false") +
                       ",\"startup_help_active\":" + std::string(help ? "true" : "false") +
                       ",\"aqua\":" + (pawn ? quote(pawn->GetFullName()) : "null") + "}";
            }
            if (op == L"find_objects")
            {
                auto class_name = J::string_field(request, "class").value_or(L"");
                if (class_name.empty() || class_name.size() > 256)
                {
                    throw std::runtime_error("invalid_class_name");
                }
                auto found = instances(class_name.c_str());
                std::ostringstream out;
                out << "{\"total\":" << found.size() << ",\"objects\":[";
                for (size_t i = 0; i < std::min<size_t>(found.size(), 64); ++i)
                {
                    if (i)
                    {
                        out << ',';
                    }
                    out << quote(found[i]->GetFullName());
                }
                out << "]}";
                return out.str();
            }
            if (op == L"inspect_class")
            {
                auto path = J::string_field(request, "path").value_or(L"");
                auto* object = UObjectGlobals::StaticFindObject<UObject*>(nullptr, nullptr, path.c_str());
                if (!valid(object) || !object->IsA<UClass>())
                {
                    throw std::runtime_error("class_not_loaded");
                }
                auto* cls = static_cast<UClass*>(object);
                std::ostringstream out;
                out << "{\"class\":" << quote(cls->GetFullName()) << ",\"properties\":[";
                size_t n = 0;
                for (auto* field : cls->ForEachPropertyInChain())
                {
                    if (n == 256)
                    {
                        break;
                    }
                    if (n++)
                    {
                        out << ',';
                    }
                    out << "{\"name\":" << quote(field->GetFName().ToString())
                        << ",\"type\":" << quote(field->GetClass().GetName())
                        << ",\"offset\":" << field->GetOffset_Internal() << ",\"size\":" << field->GetElementSize()
                        << "}";
                }
                out << "],\"functions\":[";
                n = 0;
                for (auto* function : cls->ForEachFunctionInChain())
                {
                    if (n == 256)
                    {
                        break;
                    }
                    if (!valid(function))
                    {
                        continue;
                    }
                    if (n++)
                    {
                        out << ',';
                    }
                    out << "{\"name\":" << quote(function->GetFullName())
                        << ",\"params_size\":" << function->GetParmsSize() << "}";
                }
                out << "]}";
                return out.str();
            }
            if (op == L"watch_events")
            {
                if (trace.active)
                {
                    throw std::runtime_error("trace_already_active");
                }
                auto names = J::string_array_field(request, "names");
                auto seconds = J::int64_field(request, "seconds").value_or(10);
                if (names.empty() || names.size() > 16 || seconds < 2 || seconds > 30)
                {
                    throw std::runtime_error("invalid_trace_bounds");
                }
                trace = {};
                trace.id = J::string_field(request, "request_id").value_or(L"");
                for (auto& name : names)
                {
                    if (name == L"ReceiveTick" || name.size() > 256)
                    {
                        throw std::runtime_error("tick_trace_disallowed");
                    }
                    auto fname = FName(name, FNAME_Find);
                    if (fname != FName(0, 0))
                    {
                        trace.names.push_back(fname);
                    }
                }
                if (trace.names.empty())
                {
                    throw std::runtime_error("no_loaded_event_names");
                }
                trace.events.reserve(256);
                trace.end = Clock::now() + std::chrono::seconds(seconds);
                trace.active = true;
                return "{\"started\":true,\"max_events\":256}";
            }
            if (op == L"profile_frames")
            {
                if (profile.active)
                {
                    throw std::runtime_error("profile_already_active");
                }
                if (!aqua())
                {
                    throw std::runtime_error("aqua_not_playable");
                }
                auto seconds = J::int64_field(request, "seconds").value_or(10);
                if (seconds < 2 || seconds > 30)
                {
                    throw std::runtime_error("profile_seconds_out_of_range");
                }
                profile = {};
                profile.active = true;
                profile.id = J::string_field(request, "request_id").value_or(L"");
                profile.legacy_poll = J::string_field(request, "mode").value_or(L"idle") == L"legacy_poll";
                profile.end = Clock::now() + std::chrono::seconds(seconds);
                profile.intervals.reserve(12000);
                profile.scans.reserve(60);
                return "{\"started\":true}";
            }
            if (op == L"inspect")
            {
                return describe();
            }
            if (op == L"inspect_treasures")
            {
                std::ostringstream out;
                out << "{\"rewards\":[";
                bool first = true;
                for (auto& [key, p] : treasure_rows())
                {
                    if (!first)
                    {
                        out << ',';
                    }
                    first = false;
                    out << "{\"chest\":" << quote(key) << ",\"item\":" << quote(p->ToString()) << "}";
                }
                out << "]}";
                return out.str();
            }
            if (op == L"restore_consumables")
            {
                if (!aqua())
                {
                    throw std::runtime_error("aqua_not_playable");
                }
                auto rows = treasure_rows();
                size_t restored = 0;
                for (auto& [key, p] : rows)
                {
                    if (auto it = original_rewards.find(key); it != original_rewards.end())
                    {
                        *p = it->second;
                        ++restored;
                    }
                }
                return "{\"restored\":" + std::to_string(restored) + "}";
            }
            if (op == L"shuffle_consumables")
            {
                if (!aqua())
                {
                    throw std::runtime_error("aqua_not_playable");
                }
                const auto seed = J::int64_field(request, "seed");
                if (!seed)
                {
                    throw std::runtime_error("missing_seed");
                }
                auto rows = treasure_rows();
                std::vector<FName*> destinations;
                std::vector<FName> rewards;
                for (auto& [key, p] : rows)
                {
                    if (p->ToString().starts_with(L"ITEM_"))
                    {
                        original_rewards.try_emplace(key, *p);
                        destinations.push_back(p);
                        rewards.push_back(original_rewards.at(key));
                    }
                }
                if (destinations.size() != 25)
                {
                    throw std::runtime_error("unexpected_consumable_pool");
                }
                std::mt19937_64 rng(static_cast<uint64_t>(*seed));
                std::shuffle(rewards.begin(), rewards.end(), rng);
                std::ostringstream out;
                out << "{\"rewards\":[";
                size_t index = 0;
                for (auto& [key, p] : rows)
                {
                    if (p->ToString().starts_with(L"ITEM_"))
                    {
                        *p = rewards[index];
                        if (index++)
                        {
                            out << ',';
                        }
                        out << "{\"chest\":" << quote(key) << ",\"item\":" << quote(p->ToString()) << "}";
                    }
                }
                out << "]}";
                return out.str();
            }
            throw std::runtime_error("unsupported_operation");
        }

        std::string timing_summary(std::vector<double> values)
        {
            if (values.empty())
            {
                return "{\"count\":0}";
            }
            std::sort(values.begin(), values.end());
            std::ostringstream out;
            out << "{\"count\":" << values.size() << ",\"median_ms\":" << values[values.size() / 2]
                << ",\"p95_ms\":" << values[(values.size() - 1) * 95 / 100] << ",\"max_ms\":" << values.back()
                << ",\"over_50ms\":" << std::count_if(values.begin(), values.end(), [](double v) { return v > 50; })
                << "}";
            return out.str();
        }

        void tick(bool modal)
        {
            const auto now = Clock::now();
            if (objective_due.exchange(false, std::memory_order_acq_rel))
            {
                std::string report;
                try
                {
                    report = inspect_objectives(modal);
                }
                catch (const std::exception&)
                {
                    report = "{\"protocol\":1,\"game\":\"kh02\",\"session\":" + quote(session) +
                             ",\"observed_at_ms\":" + std::to_string(epoch_ms()) + ",\"ready\":false,\"states\":[]}";
                }
                std::lock_guard lock(exchange);
                objective_report = std::move(report);
            }
            if (trace.active && now >= trace.end)
            {
                std::string report =
                    "{\"request_id\":" + quote(trace.id) + ",\"session\":" + quote(session) + ",\"events\":[";
                for (size_t i = 0; i < trace.events.size(); ++i)
                {
                    if (i)
                    {
                        report += ',';
                    }
                    report += trace.events[i];
                }
                report += "],\"limit_reached\":" + std::string(trace.events.size() == 256 ? "true" : "false") + "}";
                {
                    std::lock_guard lock(exchange);
                    trace_result = std::move(report);
                }
                trace.active = false;
            }
            if (profile.active && !modal)
            {
                // Engine tick intervals measure game-thread cadence, not GPU presentation.
                {
                    if (profile.last != Clock::time_point{} && profile.intervals.size() < 12000)
                    {
                        profile.intervals.push_back(
                            std::chrono::duration<double, std::milli>(now - profile.last).count());
                    }
                    profile.last = now;
                }
                if (now >= profile.end)
                {
                    auto report = "{\"request_id\":" + quote(profile.id) + ",\"session\":" + quote(session) +
                                  ",\"mode\":" + quote(profile.legacy_poll ? L"legacy_poll" : L"idle") +
                                  ",\"engine_tick_intervals\":" + timing_summary(profile.intervals) +
                                  ",\"inspection_cost\":" + timing_summary(profile.scans) + "}";
                    {
                        std::lock_guard lock(exchange);
                        profile_result = std::move(report);
                    }
                    profile.active = false;
                }
                else if (profile.legacy_poll && now >= profile.next_scan)
                {
                    profile.next_scan = now + std::chrono::milliseconds(500);
                    const auto start = Clock::now();
                    describe();
                    profile.scans.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
                }
            }
            if (now < next_tick)
            {
                return;
            }
            next_tick = now + std::chrono::milliseconds(100);
            std::string request;
            {
                std::lock_guard lock(exchange);
                request = std::move(queued);
                queued.clear();
            }
            try
            {
                std::string reply;
                if (!request.empty())
                {
                    auto id = kh3ap::JsonFieldReader::string_field(request, "request_id").value_or(L"");
                    auto start = Clock::now();
                    try
                    {
                        auto payload = execute(request, modal);
                        auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                        reply = "{\"request_id\":" + quote(id) + ",\"ok\":true,\"result\":" + payload +
                                ",\"execution_ms\":" + std::to_string(elapsed) + "}";
                    }
                    catch (const std::exception& e)
                    {
                        reply = "{\"request_id\":" + quote(id) + ",\"ok\":false,\"error\":" +
                                quote(std::wstring(e.what(), e.what() + strlen(e.what()))) + "}";
                    }
                }
                // Publish a request error even when an independent snapshot fails.
                {
                    std::lock_guard lock(exchange);
                    if (!reply.empty())
                    {
                        result = std::move(reply);
                    }
                }
                std::lock_guard lock(exchange);
                if (!announced)
                {
                    snapshot = "{\"protocol\":1,\"game\":\"kh02\",\"ready\":true,\"inspection_mode\":\"on_request\","
                               "\"session\":" +
                               quote(session) + ",\"pid\":" + std::to_string(GetCurrentProcessId()) + "}";
                    announced = true;
                }
                game_work.store(profile.active || trace.active || !queued.empty(), std::memory_order_release);
            }
            catch (const std::exception& e)
            {
                RC::Output::send<RC::LogLevel::Default>(STR("[KH02] bridge tick error: {}\n"),
                                                        std::wstring(e.what(), e.what() + strlen(e.what())));
            }
        }

        void write(const wchar_t* name, const std::string& text)
        {
            auto target = root / name;
            auto temporary = target;
            temporary += L".tmp";
            {
                std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
                f << text;
                f.flush();
                if (!f)
                {
                    throw std::runtime_error("bridge_write_failed");
                }
            }
            if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                throw std::runtime_error("bridge_replace_failed");
            }
        }
    } // namespace

    void initialize_bridge()
    {
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, 32768);
        root = std::filesystem::path(path).parent_path() / L"kh02_bridge";
        std::filesystem::create_directories(root);
        session = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(epoch_ms());
        write(L"state.json", "{\"protocol\":1,\"game\":\"kh02\",\"ready\":false,\"session\":" + quote(session) +
                                 ",\"pid\":" + std::to_string(GetCurrentProcessId()) + "}");
        receive_tick = FName(L"ReceiveTick", FNAME_Find);
        aqua_class = FName(L"p_ex002_Pawn_C", FNAME_Add);
        direct_reward_script_class = FName(L"dw_03_challenge_C", FNAME_Add);
        if (!install_engine_tick(
                [](bool modal)
                {
                    if (!game_work.load(std::memory_order_acquire) && !objective_due.load(std::memory_order_acquire))
                    {
                        return;
                    }
                    game_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
                    tick(modal);
                }))
        {
            throw std::runtime_error("engine_tick_profile_or_hook_failed");
        }
        RC::Output::send<RC::LogLevel::Default>(STR("[KH02] verified engine tick installed; inspection=on_request\n"));
        if (!install_script_reward_filter(&suppress_script_reward))
        {
            throw std::runtime_error("script_reward_profile_or_hook_failed");
        }
        Hook::RegisterProcessEventPostCallback(
            [](UObject* context, UFunction* function, void*)
            {
                // Idle path: one atomic read, no object scan, string conversion, locks,
                // timers, logging, or file IO. FName comparison only while work is queued.
                if (!game_work.load(std::memory_order_acquire))
                {
                    return;
                }
                auto thread = game_thread.load(std::memory_order_relaxed);
                if (thread && thread != GetCurrentThreadId())
                {
                    return;
                }
                if (thread && trace.active && trace.events.size() < 256 && function &&
                    std::find(trace.names.begin(), trace.names.end(), function->GetNamePrivate()) != trace.names.end())
                {
                    trace.events.push_back("{\"function\":" + quote(function->GetFullName()) +
                                           ",\"context\":" + (valid(context) ? quote(context->GetFullName()) : "null") +
                                           ",\"thread\":" + std::to_string(thread) + "}");
                }
                // Requests run from native engine tick, including while actors are paused.
            });
    }

    void poll_bridge()
    {
        auto now = Clock::now();
        if (now < next_poll)
        {
            return;
        }
        next_poll = now + std::chrono::milliseconds(500);
        objective_due.store(true, std::memory_order_release);
        try
        {
            auto input = root / L"request.json";
            if (std::filesystem::exists(input) && std::filesystem::file_size(input) < 65536)
            {
                std::ifstream f(input, std::ios::binary);
                std::string request((std::istreambuf_iterator<char>(f)), {});
                auto id = kh3ap::JsonFieldReader::string_field(request, "request_id");
                if (id && !id->empty() && *id != last_request)
                {
                    std::lock_guard lock(exchange);
                    if (queued.empty())
                    {
                        queued = std::move(request);
                        last_request = *id;
                        game_work.store(true, std::memory_order_release);
                    }
                }
            }
            std::string state, reply, report, events, chest, objectives;
            {
                std::lock_guard lock(exchange);
                state = std::move(snapshot);
                reply = std::move(result);
                report = std::move(profile_result);
                events = std::move(trace_result);
                snapshot.clear();
                result.clear();
                profile_result.clear();
                trace_result.clear();
            }
            {
                std::lock_guard lock(exchange);
                chest = std::move(chest_test_report);
                chest_test_report.clear();
            }
            {
                std::lock_guard lock(exchange);
                objectives = std::move(objective_report);
                objective_report.clear();
            }
            try
            {
                if (!state.empty())
                {
                    write(L"state.json", state);
                }
                if (!reply.empty())
                {
                    write(L"result.json", reply);
                }
                if (!report.empty())
                {
                    write(L"profile.json", report);
                }
                if (!events.empty())
                {
                    write(L"events.json", events);
                }
                if (!chest.empty())
                {
                    write(L"chest_test.json", chest);
                }
                if (!objectives.empty())
                {
                    write(L"objectives.json", objectives);
                }
            }
            catch (...)
            {
                std::lock_guard lock(exchange);
                if (snapshot.empty())
                {
                    snapshot = std::move(state);
                }
                if (result.empty())
                {
                    result = std::move(reply);
                }
                if (profile_result.empty())
                {
                    profile_result = std::move(report);
                }
                if (trace_result.empty())
                {
                    trace_result = std::move(events);
                }
                if (chest_test_report.empty())
                {
                    chest_test_report = std::move(chest);
                }
                if (objective_report.empty())
                {
                    objective_report = std::move(objectives);
                }
                throw;
            }
        }
        catch (const std::exception& e)
        {
            RC::Output::send<RC::LogLevel::Default>(STR("[KH02] bridge IO error: {}\n"),
                                                    std::wstring(e.what(), e.what() + strlen(e.what())));
        }
    }
} // namespace kh02
