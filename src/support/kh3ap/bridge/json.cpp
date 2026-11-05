#include "kh3ap/bridge/json.h"

namespace kh3ap::json
{
    namespace
    {

        // Bound recursion before the pinned decoder validates the syntax.
        auto bounded_document(std::string_view text) -> bool
        {
            if (text.empty() || text.size() > limits::document_bytes) return false;
            size_t depth{};
            bool quoted{}, escaped{};
            for (const unsigned char ch : text)
            {
                if (quoted)
                {
                    if (ch < 0x20) return false;
                    if (escaped) escaped = false;
                    else if (ch == '\\') escaped = true;
                    else if (ch == '"') quoted = false;
                }
                else if (ch == '"') quoted = true;
                else if (ch == '{' || ch == '[')
                {
                    if (++depth > limits::document_depth) return false;
                }
                else if (ch == '}' || ch == ']')
                {
                    if (!depth) return false;
                    --depth;
                }
            }
            return !quoted && !depth;
        }

    }

    auto utf8_to_wide(std::string_view text) -> std::optional<std::wstring>
    {
        std::wstring result;
        result.reserve(text.size());
        for (size_t pos = 0; pos < text.size();)
        {
            const auto first = static_cast<unsigned char>(text[pos++]);
            uint32_t codepoint = first;
            unsigned continuation{};
            uint32_t minimum{};
            if (first >= 0xc2 && first <= 0xdf) { continuation = 1; codepoint &= 0x1f; minimum = 0x80; }
            else if (first >= 0xe0 && first <= 0xef) { continuation = 2; codepoint &= 0x0f; minimum = 0x800; }
            else if (first >= 0xf0 && first <= 0xf4) { continuation = 3; codepoint &= 0x07; minimum = 0x10000; }
            else if (first >= 0x80) return std::nullopt;
            if (continuation > text.size() - pos) return std::nullopt;
            while (continuation--)
            {
                const auto next = static_cast<unsigned char>(text[pos++]);
                if ((next & 0xc0) != 0x80) return std::nullopt;
                codepoint = (codepoint << 6) | (next & 0x3f);
            }
            if (codepoint < minimum || codepoint > 0x10ffff ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff)) return std::nullopt;
            if constexpr (sizeof(wchar_t) == 2)
            {
                if (codepoint > 0xffff)
                {
                    codepoint -= 0x10000;
                    result.push_back(static_cast<wchar_t>(0xd800 + (codepoint >> 10)));
                    result.push_back(static_cast<wchar_t>(0xdc00 + (codepoint & 0x3ff)));
                    continue;
                }
            }
            result.push_back(static_cast<wchar_t>(codepoint));
        }
        return result;
    }


    auto parse(Value& output, std::string_view text) -> bool
    {
        output = nullptr;
        if (!bounded_document(text)) return false;
        auto parsed = Value::parse(text, nullptr, false);
        if (parsed.is_discarded()) return false;
        output = std::move(parsed);
        return true;
    }

}
