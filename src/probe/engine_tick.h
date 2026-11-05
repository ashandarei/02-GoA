#pragma once
#include <cstdint>

namespace kh02
{
    bool install_engine_tick(void (*callback)(bool modal));
    void uninstall_engine_tick();
    bool install_chest_observer(void (*callback)(void* chest, bool before));
    bool install_script_reward_filter(bool (*callback)(void* context, uint64_t token));
} // namespace kh02
