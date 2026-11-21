#pragma once
#include "support/kh3ap/features/kh02/transport/kh02_asset_protocol.h"
#include <span>

namespace kh02asset
{
    // Reuse the single-file reader for each half. Publish a payload only after both succeed.
    template <class Files, class Read>
    auto read_asset_pair(const Request& request, const Files& files, uint64_t tag, uint32_t pid,
                         std::span<unsigned char> output, Read&& read) -> Response
    {
        Response reply{magic, 1, pid, 0, 0, request.offset, request.nonce, tag};
        const auto header = pair_header(request.offset);
        const auto exports = pair_exports(request.offset);
        if (request.magic != magic || request.version != paired_version || request.catalog_tag != tag ||
            request.file_id != pair_file_id || header == exports || header >= files.size() || exports >= files.size())
        {
            return reply;
        }
        const auto header_size = files[header].size;
        const auto export_size = files[exports].size;
        if (!can_pair(header_size, export_size) || request.length != header_size + export_size ||
            output.size() < request.length)
        {
            return reply;
        }
        const auto read_file = [&](uint32_t id, std::span<unsigned char> part)
        {
            Request single{magic, version, id, static_cast<uint32_t>(part.size()), 0, request.nonce, tag};
            const auto response = read(single, part);
            return !response.status && response.length == part.size();
        };
        if (!read_file(header, output.first(header_size)) ||
            !read_file(exports, output.subspan(header_size, export_size)))
        {
            return reply;
        }
        reply.status = 0;
        reply.length = request.length;
        reply.file_size = request.length;
        return reply;
    }
} // namespace kh02asset
