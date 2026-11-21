#pragma once
#include <cstdint>
#include <limits>

namespace kh02asset
{
    constexpr uint32_t magic = 0x3241484b, version = 3, max_read = 1024 * 1024;
    // A v4 index-size request negotiates paired reads. Regular v3 requests retain their layout.
    constexpr uint32_t paired_version = 4;
    constexpr uint32_t max_pair_read = 64 * 1024;
    inline constexpr uint32_t index_file_id = std::numeric_limits<uint32_t>::max();
    inline constexpr uint32_t pair_file_id = index_file_id - 1;

    // In a paired request, offset carries the two complete file IDs and length is their sum.
    constexpr auto pair_ids(uint32_t header, uint32_t exports) -> uint64_t
    {
        return (static_cast<uint64_t>(header) << 32) | exports;
    }

    constexpr auto pair_header(uint64_t ids) -> uint32_t
    {
        return static_cast<uint32_t>(ids >> 32);
    }

    constexpr auto pair_exports(uint64_t ids) -> uint32_t
    {
        return static_cast<uint32_t>(ids);
    }

    constexpr auto can_pair(uint64_t header_size, uint64_t export_size) -> bool
    {
        return header_size && export_size && header_size < max_pair_read && export_size <= max_pair_read - header_size;
    }

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
