#include "shared/kh02/kh02_memory_signature.h"
#include "engine_tick.h"
#include <polyhook2/Detour/x64Detour.hpp>
#include <atomic>
#include <cstring>
#include <memory>
#define NOMINMAX
#include <Windows.h>
#include "build_profile.h"

namespace kh02
{
    namespace
    {
        std::unique_ptr<PLH::x64Detour> hook;
        std::unique_ptr<PLH::x64Detour> message_hook;
        std::unique_ptr<PLH::x64Detour> chest_hook;
        uint64_t chest_trampoline = 0;
        std::atomic<void (*)(void*, bool)> chest_callback{nullptr};
        std::unique_ptr<PLH::x64Detour> script_reward_hook;
        uint64_t script_reward_trampoline = 0;
        std::atomic<bool (*)(void*, uint64_t)> script_reward_filter{nullptr};

        void hooked_script_reward(void* context, uint64_t token, bool show)
        {
            if (auto fn = script_reward_filter.load(std::memory_order_acquire))
            {
                try
                {
                    if (fn(context, token))
                    {
                        token = 0;
                    }
                }
                catch (...)
                {
                }
            }
            reinterpret_cast<void (*)(void*, uint64_t, bool)>(script_reward_trampoline)(context, token, show);
        }

        void hooked_chest(void* chest)
        {
            auto fn = chest_callback.load(std::memory_order_acquire);
            if (fn)
            {
                try
                {
                    fn(chest, true);
                }
                catch (...)
                {
                }
            }
            reinterpret_cast<void (*)(void*)>(chest_trampoline)(chest);
            if (fn)
            {
                try
                {
                    fn(chest, false);
                }
                catch (...)
                {
                }
            }
        }

        uint64_t trampoline = 0;
        uint64_t message_trampoline = 0;
        std::atomic<void (*)(bool)> callback{nullptr};
        thread_local bool inside_engine_tick = false;
        thread_local bool dispatching = false;

        void dispatch(bool modal)
        {
            if (dispatching)
            {
                return;
            }
            if (auto fn = callback.load(std::memory_order_acquire))
            {
                dispatching = true;
                try
                {
                    fn(modal);
                }
                catch (...)
                {
                } // Never unwind through the engine.
                dispatching = false;
            }
        }

        void hooked_messages(bool main_loop)
        {
            reinterpret_cast<void (*)(bool)>(message_trampoline)(main_loop);
            // A loading/help modal pumps messages inside a still-active engine tick.
            // Restrict this fallback to that same game thread; no worker dispatch.
            if (inside_engine_tick && main_loop)
            {
                dispatch(true);
            }
        }

        void hooked_tick(void* engine, float delta, bool idle)
        {
            inside_engine_tick = true;
            reinterpret_cast<void (*)(void*, float, bool)>(trampoline)(engine, delta, idle);
            inside_engine_tick = false;
            dispatch(false);
        }
    } // namespace

    bool install_engine_tick(void (*fn)(bool))
    {
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
        {
            return false;
        }
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != kh02build::current().timestamp ||
            nt->OptionalHeader.SizeOfImage != kh02build::current().image_size)
        {
            return false;
        }
        // Rooted through the Negative-delta-time diagnostic and confirmed as
        // UGameEngine vtable slot 0x268 in this loaded Steam build.
        constexpr auto prologue = "6c24aeb7ed00fd831e482bb3ad219c863398c4a71581d7931fb8338632990064";
        auto* target = base + kh02build::rva(0x1DFA030);
        if (!kh02integrity::signature_matches(target, 27, prologue))
        {
            return false;
        }
        DWORD64 image = 0;
        auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(target), &image, nullptr);
        if (!entry || image != reinterpret_cast<DWORD64>(base) || entry->BeginAddress != kh02build::rva(0x1DFA030))
        {
            return false;
        }
        // Confirmed in a passive game-thread stack: the modal at 0x1601850
        // repeatedly calls this platform message pump while the engine tick waits.
        constexpr auto pump = "694fd6cadadfec59847a59bdf828122717e09adc278ee367964104bc806479c1";
        if (!kh02integrity::signature_matches(base + kh02build::rva(0xB8D6B0), 12, pump))
        {
            return false;
        }
        message_hook =
            std::make_unique<PLH::x64Detour>(reinterpret_cast<uint64_t>(base + kh02build::rva(0xB8D6B0)),
                                             reinterpret_cast<uint64_t>(&hooked_messages), &message_trampoline);
        if (!message_hook->hook())
        {
            message_hook.reset();
            return false;
        }
        hook = std::make_unique<PLH::x64Detour>(reinterpret_cast<uint64_t>(target),
                                                reinterpret_cast<uint64_t>(&hooked_tick), &trampoline);
        if (!hook->hook())
        {
            hook.reset();
            message_hook->unHook();
            message_hook.reset();
            return false;
        }
        callback.store(fn, std::memory_order_release);
        return true;
    }

    void uninstall_engine_tick()
    {
        script_reward_filter.store(nullptr, std::memory_order_release);
        if (script_reward_hook)
        {
            script_reward_hook->unHook();
            script_reward_hook.reset();
        }
        chest_callback.store(nullptr, std::memory_order_release);
        if (chest_hook)
        {
            chest_hook->unHook();
            chest_hook.reset();
        }
        callback.store(nullptr, std::memory_order_release);
        if (hook)
        {
            hook->unHook();
            hook.reset();
        }
        if (message_hook)
        {
            message_hook->unHook();
            message_hook.reset();
        }
    }

    bool install_chest_observer(void (*fn)(void*, bool))
    {
        if (!hook)
        {
            return false; // The engine hook already verified this image profile.
        }
        if (!chest_hook)
        {
            auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
            // Native chest reward completion, vtable +0x8F8. Original routine owns
            // item grant, chest journal flag and opened state; observer never skips it.
            constexpr auto signature = "16ef19d28c772e2849cc6e478193e36fb93c0d2afecf83ef3d5b2718eac12b2f";
            if (!kh02integrity::signature_matches(base + kh02build::rva(0x483F80), 13, signature))
            {
                return false;
            }
            chest_hook = std::make_unique<PLH::x64Detour>(reinterpret_cast<uint64_t>(base + kh02build::rva(0x483F80)),
                                                          reinterpret_cast<uint64_t>(&hooked_chest), &chest_trampoline);
            if (!chest_hook->hook())
            {
                chest_hook.reset();
                return false;
            }
        }
        chest_callback.store(fn, std::memory_order_release);
        return true;
    }

    bool install_script_reward_filter(bool (*fn)(void*, uint64_t))
    {
        if (!hook)
        {
            return false;
        }
        if (!script_reward_hook)
        {
            auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
            // BP_TresGetItem native implementation, reached by its reflected thunk
            // at 0x85C550. ABI: UObject*, eight-byte FName, bool; returns void.
            constexpr auto signature = "2e1b6743f52d5b52b44f270a0058dd5409efe92df8d6dc1aaedc08ab9a811fd4";
            if (!kh02integrity::signature_matches(base + kh02build::rva(0x42D7D0), 14, signature))
            {
                return false;
            }
            script_reward_hook = std::make_unique<PLH::x64Detour>(
                reinterpret_cast<uint64_t>(base + kh02build::rva(0x42D7D0)),
                reinterpret_cast<uint64_t>(&hooked_script_reward), &script_reward_trampoline);
            if (!script_reward_hook->hook())
            {
                script_reward_hook.reset();
                return false;
            }
        }
        script_reward_filter.store(fn, std::memory_order_release);
        return true;
    }
} // namespace kh02
