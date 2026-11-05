#pragma once

#include <nlohmann/json.hpp>
#include "kh3ap/bridge/native_json_limits.h"
#include <optional>
#include <string>
#include <string_view>

namespace kh3ap::json
{
    using Value = nlohmann::json;

    // Complete documents only. Bounds apply before the decoder recurses; a
    // failed read replaces the output with null, never a partially parsed tree.
    auto parse(Value& output, std::string_view text) -> bool;
    auto utf8_to_wide(std::string_view text) -> std::optional<std::wstring>;
}
