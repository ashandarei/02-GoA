#include "kh3ap/bridge/json_fields.h"

#include "kh3ap/bridge/json.h"

#include <limits>

namespace kh3ap
{
    namespace
    {
        auto cached_object(const std::string& text) -> const JsonObject&
        {
            // Bounded to one document per thread, compared by contents rather
            // than address so a rewritten request cannot reuse stale values.
            static thread_local std::string cached_text;
            static thread_local JsonObject cached;
            if (cached_text != text)
            {
                cached = JsonObject{text};
                cached_text = text.size() <= json::limits::document_bytes ? text : std::string{};
            }
            return cached;
        }
    }

    JsonObject::JsonObject(std::string_view text)
    {
        auto parsed = std::make_shared<json::Value>();
        if (json::parse(*parsed, text) && parsed->is_object())
        {
            impl_ = std::move(parsed);
        }
    }

    auto JsonObject::find_field(std::string_view name) const -> const json::Value*
    {
        if (!impl_)
        {
            return nullptr;
        }
        const auto found = impl_->find(name);
        return found == impl_->end() ? nullptr : &*found;
    }

    auto JsonObject::string_field(std::string_view name) const -> std::optional<std::wstring>
    {
        const auto* field = find_field(name);
        if (!field || !field->is_string())
        {
            return std::nullopt;
        }
        return json::utf8_to_wide(field->get_ref<const std::string&>());
    }

    auto JsonObject::int64_field(std::string_view name) const -> std::optional<int64_t>
    {
        const auto* field = find_field(name);
        if (!field || !field->is_number_integer())
        {
            return std::nullopt;
        }
        if (field->is_number_unsigned() &&
            field->get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        {
            return std::nullopt;
        }
        return field->get<int64_t>();
    }

    auto JsonObject::string_array_field(std::string_view name) const -> std::vector<std::wstring>
    {
        const auto* field = find_field(name);
        if (!field || !field->is_array())
        {
            return {};
        }
        std::vector<std::wstring> result;
        result.reserve(field->size());
        for (const auto& value : *field)
        {
            if (!value.is_string())
            {
                return {};
            }
            auto wide = json::utf8_to_wide(value.get_ref<const std::string&>());
            if (!wide)
            {
                return {};
            }
            result.push_back(std::move(*wide));
        }
        return result;
    }

    auto JsonFieldReader::string_field(const std::string& text, std::string_view name) -> std::optional<std::wstring>
    {
        return cached_object(text).string_field(name);
    }
    auto JsonFieldReader::int64_field(const std::string& text, std::string_view name) -> std::optional<int64_t>
    {
        return cached_object(text).int64_field(name);
    }
    auto JsonFieldReader::string_array_field(const std::string& text, std::string_view name) -> std::vector<std::wstring>
    {
        return cached_object(text).string_array_field(name);
    }
}
