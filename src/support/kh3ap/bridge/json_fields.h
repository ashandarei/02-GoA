#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kh3ap
{
    // Read members from this object, never by searching its descendants.
    class JsonObject
    {
    public:
        JsonObject() = default;
        explicit JsonObject(std::string_view text);
        auto string_field(std::string_view name) const -> std::optional<std::wstring>;
        auto int64_field(std::string_view name) const -> std::optional<int64_t>;
        auto string_array_field(std::string_view name) const -> std::vector<std::wstring>;

    private:
        auto find_field(std::string_view name) const -> const nlohmann::json*;

        std::shared_ptr<const nlohmann::json> impl_;
    };

    class JsonFieldReader
    {
    public:
        static auto string_field(const std::string& json_text, std::string_view field_name) -> std::optional<std::wstring>;
        static auto int64_field(const std::string& json_text, std::string_view field_name) -> std::optional<int64_t>;
        static auto string_array_field(const std::string& json_text, std::string_view field_name) -> std::vector<std::wstring>;
    };
}
