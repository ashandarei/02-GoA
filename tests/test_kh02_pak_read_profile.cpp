#include "shared/kh02/kh02_pak_read_profile.h"
#include "probe/pak_read_profile.h"
#include "kh3ap/features/kh02/transport/kh02_index_state.h"
#include <nlohmann/json.hpp>
#include <cassert>
#include <thread>

namespace kh02asset
{
    auto asset_index() -> const AssetIndex&
    {
        static const AssetIndex value{
            1, {{L"test.uasset", L"", 1024}, {L"test.uexp", L"", 65'536}, {L"test.ubulk", L"", 1'048'576}}, {}};
        return value;
    }
} // namespace kh02asset

int main()
{
    // Overlapping and adjacent ranges must count each source byte once.
    kh02::record_pak_read(0, 100, 100, 10);
    kh02::record_pak_read(0, 150, 100, 20);
    kh02::record_pak_read(0, 50, 50, 30);
    kh02::record_pak_read(1, 0, 65'536, 50);
    kh02::record_pak_read(2, 0, 1'048'576, 5);
    const auto result = nlohmann::json::parse(kh02::pak_read_profile_status());
    assert(result.at("sampled_reads") == 5);
    assert(result.at("dropped_reads") == 0);
    assert(result.at("overlap_bytes") == 50);
    assert(result.at("file_count") == 3);
    assert(result.at("types").at("uasset").at("read_us") == 60);
    assert(result.at("types").at("uexp").at("bytes") == 65'536);
    assert(result.at("sizes").at(0).at("calls") == 3);
    assert(result.at("sizes").at(1).at("calls") == 1);
    assert(result.at("sizes").at(3).at("calls") == 1);
    assert(result.at("slowest_files").at(0).at("file_id") == 0);
    assert(result.at("slowest_files").at(0).at("unique_bytes") == 200);
    kh02::record_pak_read(0, 250, 25, 1);
    const auto updated = nlohmann::json::parse(kh02::pak_read_profile_status());
    assert(updated.at("sampled_reads") == 6);
    assert(updated.at("overlap_bytes") == 50);
    assert(updated.at("slowest_files").at(0).at("unique_bytes") == 225);
    assert(nlohmann::json::parse(kh02::pak_read_profile_status()) == updated);

    // Concurrent reads may complete out of timestamp order. Anchor every bin
    // to the earliest start and keep the timeline bounded independently of totals.
    kh02::record_pak_read(0, 300, 10, 2, 2'000, 2'002);
    kh02::record_pak_read(0, 310, 20, 3, 1'900, 1'903);
    kh02::record_pak_read(0, 330, 30, 4, 14'700, 14'704);
    kh02::record_pak_read(0, 360, 40, 5, 5'000, 4'999);
    const auto timed = nlohmann::json::parse(kh02::pak_read_profile_status());
    assert(timed.at("first_read_ms") == 1'900);
    assert(timed.at("last_read_ms") == 14'704);
    assert(timed.at("sampled_reads") == 10);
    assert(timed.at("timeline").size() == 2);
    assert(timed.at("timeline").at(0).at("at_ms") == 1'900);
    assert(timed.at("timeline").at(0).at("bytes") == 20);
    assert(timed.at("timeline").at(1).at("at_ms") == 2'000);
    assert(timed.at("timeline").at(1).at("read_us") == 2);

    // Reporting and collection may run concurrently; capacity stays bounded.
    kh02asset::PakReadProfile bounded(128);
    std::thread writer(
        [&]
        {
            for (uint32_t index = 0; index < 1'000; ++index)
            {
                bounded.record(0, index, 1, 1);
            }
        });
    for (unsigned sample = 0; sample < 100; ++sample)
    {
        assert(bounded.snapshot().reads.size() <= 128);
    }
    writer.join();
    const auto snapshot = bounded.snapshot();
    assert(snapshot.reads.size() == 128);
    assert(snapshot.dropped == 872);
    assert(snapshot.reads.back().offset == 127);
    kh02asset::PakReadProfile disabled(0);
    disabled.record(0, 0, 1, 1);
    assert(disabled.snapshot().reads.empty());
    assert(disabled.snapshot().dropped == 1);
}
