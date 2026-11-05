#pragma once

namespace RC::Unreal
{
    class UObject;
}

namespace kh3ap
{
    // Constant-time counterpart to UE4SS UObject::IsReal. This validates the
    // exact GUObjectArray slot and rejects pending-kill, unreachable, and
    // destroying objects without scanning the global array.
    // Invalid or stale readable pointers fail closed; the Windows build also
    // guards the initial pointer read against access violations.
    [[nodiscard]] auto is_uobject_valid_fast(
            const RC::Unreal::UObject* object) noexcept -> bool;

}
