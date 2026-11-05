#pragma once
#include <cstdint>
#include <limits>

namespace kh02asset
{
    constexpr uint32_t magic = 0x3241484b, version = 3, max_read = 1024 * 1024;
    inline constexpr uint32_t index_file_id = std::numeric_limits<uint32_t>::max();
#pragma pack(push, 1)

    struct Request
    {
        uint32_t magic, version, file_id, length;
        uint64_t offset, nonce, catalog_tag;
    };

    struct Response
    {
        uint32_t magic, status, pid, length;
        uint64_t file_size, offset, nonce, catalog_tag;
    };

#pragma pack(pop)
    static_assert(sizeof(Request) == 40 && sizeof(Response) == 48);
} // namespace kh02asset
