#pragma once
#include <span>
#include <string_view>

namespace kh02
{
    enum class RuntimeMode
    {
        disabled,
        companion,
        diagnostics
    };

    inline auto select_runtime_mode(std::span<const std::wstring_view> arguments, bool diagnostics_available)
        -> RuntimeMode
    {
        bool diagnostics_requested = false;
        for (const auto argument : arguments)
        {
            if (argument == L"-KH02AssetCompanion")
            {
                // A companion must never initialize gameplay diagnostics.
                return RuntimeMode::companion;
            }
            diagnostics_requested |= argument == L"-KH02Diagnostics";
        }
        return diagnostics_available && diagnostics_requested ? RuntimeMode::diagnostics : RuntimeMode::disabled;
    }
} // namespace kh02
