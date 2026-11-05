#include "pak_read_profile.h"
#include "shared/kh02/kh02_pak_read_profile.h"
#include "kh3ap/features/kh02/transport/kh02_index_state.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <utility>
#include <vector>

namespace kh02
{
    namespace
    {
        auto profile() -> kh02asset::PakReadProfile&
        {
            static kh02asset::PakReadProfile value;
            return value;
        }

        struct Totals
        {
            uint64_t calls{}, bytes{}, microseconds{};

            auto json() const -> nlohmann::json
            {
                return {{"calls", calls}, {"bytes", bytes}, {"read_us", microseconds}};
            }

            void add(const kh02asset::PakReadProfile::Read& read)
            {
                ++calls;
                bytes += read.length;
                microseconds += read.microseconds;
            }
        };

        struct FileTotals
        {
            uint32_t id{};
            Totals totals;
            std::vector<std::pair<uint64_t, uint64_t>> ranges;
        };

        auto unique_bytes(FileTotals& file) -> uint64_t
        {
            std::sort(file.ranges.begin(), file.ranges.end());
            uint64_t unique{}, end{};
            for (const auto& range : file.ranges)
            {
                if (range.second > end)
                {
                    unique += range.second - std::max(end, range.first);
                    end = range.second;
                }
            }
            return unique;
        }
    } // namespace

    void record_pak_read(uint32_t file, uint64_t offset, uint32_t length, uint64_t microseconds, uint64_t started_ms,
                         uint64_t finished_ms)
    {
        profile().record(file, offset, length, microseconds, started_ms, finished_ms);
    }

    auto pak_read_profile_status() -> std::string
    {
        const auto snapshot = profile().snapshot();
        thread_local size_t previous_count = static_cast<size_t>(-1);
        thread_local uint64_t previous_dropped{};
        thread_local std::string previous_status;
        if (previous_count == snapshot.reads.size() && previous_dropped == snapshot.dropped)
        {
            return previous_status;
        }
        std::map<uint32_t, FileTotals> files;
        std::map<std::string, Totals> types;
        std::map<uint64_t, Totals> timeline;
        uint64_t first_read_ms{}, last_read_ms{};
        constexpr std::array<uint32_t, 4> size_bounds{4096, 65'536, 262'144, 1'048'576};
        std::array<Totals, size_bounds.size()> sizes;
        const auto& catalog = kh02asset::asset_index();
        for (const auto& read : snapshot.reads)
        {
            if (read.started_ms && read.finished_ms >= read.started_ms)
            {
                if (!first_read_ms || read.started_ms < first_read_ms)
                {
                    first_read_ms = read.started_ms;
                }
                last_read_ms = std::max(last_read_ms, read.finished_ms);
            }
        }
        for (const auto& read : snapshot.reads)
        {
            if (read.started_ms && read.finished_ms >= read.started_ms)
            {
                const auto bucket = (read.started_ms - first_read_ms) / 100;
                if (bucket < 128)
                {
                    timeline[bucket].add(read);
                }
            }
            auto& file = files[read.file];
            file.id = read.file;
            file.totals.add(read);
            file.ranges.emplace_back(read.offset, read.offset + read.length);
            const auto& source = catalog.files.at(read.file).source;
            const auto type = source.ends_with(L".uasset")  ? "uasset"
                              : source.ends_with(L".uexp")  ? "uexp"
                              : source.ends_with(L".ubulk") ? "ubulk"
                                                            : "other";
            types[type].add(read);
            const auto bucket = std::lower_bound(size_bounds.begin(), size_bounds.end(), read.length);
            if (bucket != size_bounds.end())
            {
                sizes[bucket - size_bounds.begin()].add(read);
            }
        }
        std::vector<FileTotals*> ordered;
        uint64_t repeated{};
        for (auto& [id, file] : files)
        {
            const auto unique = unique_bytes(file);
            const auto overlap = file.totals.bytes - unique;
            repeated += overlap;
            ordered.push_back(&file);
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right)
                  { return left->totals.microseconds > right->totals.microseconds; });
        if (ordered.size() > 20)
        {
            ordered.resize(20);
        }
        std::vector<nlohmann::json> rows;
        for (auto* file : ordered)
        {
            const auto id = file->id;
            const auto unique = unique_bytes(*file);
            const auto& source = catalog.files.at(id).source;
            auto row = file->totals.json();
            row["file_id"] = id;
            std::string source_name;
            source_name.reserve(source.size());
            for (const auto character : source)
            {
                // The catalog loader accepts printable ASCII source paths only.
                source_name.push_back(static_cast<char>(character));
            }
            row["source"] = std::move(source_name);
            row["overlap_bytes"] = file->totals.bytes - unique;
            row["unique_bytes"] = unique;
            row["file_bytes"] = catalog.files.at(id).size;
            rows.push_back(std::move(row));
        }
        nlohmann::json result = {{"sampled_reads", snapshot.reads.size()},
                                 {"dropped_reads", snapshot.dropped},
                                 {"file_count", files.size()},
                                 {"overlap_bytes", repeated},
                                 {"slowest_files", rows}};
        result["first_read_ms"] = first_read_ms;
        result["last_read_ms"] = last_read_ms;
        result["timeline"] = nlohmann::json::array();
        for (const auto& [bucket, totals] : timeline)
        {
            auto row = totals.json();
            row["at_ms"] = first_read_ms + bucket * 100;
            result["timeline"].push_back(std::move(row));
        }
        for (const auto& [type, totals] : types)
        {
            result["types"][type] = totals.json();
        }
        for (size_t index = 0; index < size_bounds.size(); ++index)
        {
            auto row = sizes[index].json();
            row["upper_bytes"] = size_bounds[index];
            result["sizes"].push_back(std::move(row));
        }
        previous_count = snapshot.reads.size();
        previous_dropped = snapshot.dropped;
        previous_status = result.dump();
        return previous_status;
    }
} // namespace kh02
