#include "shared/kh02/kh02_asset_index.h"
#include <cassert>

template <typename Action> void rejects(Action action)
{
    bool rejected{};
    try
    {
        action();
    }
    catch (const std::exception&)
    {
        rejected = true;
    }
    assert(rejected);
}

int main()
{
    using namespace kh02asset;
    const std::string build(64, 'a');
    const auto prefix = std::string(source_content_prefix);
    auto index = build_index({{std::string(source_control_file), 12},
                              {prefix + "Sound/VOICE/Localization/en/test.uasset", 5},
                              {prefix + "Sound/VOICE/Localization/ja/test.uasset", 6},
                              {prefix + "Localization/Game/en/kh02.locres", 7}},
                             build);
    assert(index.files[0].suffix.empty());
    assert(index.voice_pairs.size() == 1);
    assert(target_suffix(prefix + "Blueprints/Test.uasset") == "/content/zlueprints/test.uasset");
    assert(target_suffix(prefix + "Localization/Game/en/kh02.locres") ==
           "/content/localization/game/en/kh02.locres");
    const auto wire = serialize_index(index);
    assert(serialize_index(parse_index(wire, build)) == wire);
    rejects(
        [&]
        {
            parse_index(wire, std::string(64, 'b'));
        });
    rejects(
        [&]
        {
            parse_index(" " + wire, build);
        });
    rejects(
        [&]
        {
            parse_index(std::string(maximum_index_bytes + 1, ' '), build);
        });
    rejects(
        [&]
        {
            build_index({{std::string(source_control_file), 1}, {prefix + "../escape.uasset", 1}},
                        build);
        });
    rejects(
        [&]
        {
            build_index({{std::string(source_control_file), 1},
                         {prefix + "Test.uasset", 1},
                         {prefix + "test.uasset", 1}},
                        build);
        });
    rejects(
        [&]
        {
            build_index({{prefix + "Test.uasset", 1}}, build);
        });
    rejects(
        [&]
        {
            index_tag("bad");
        });
    auto malformed = nlohmann::json::parse(wire);
    malformed["files"][0]["size"] = -1;
    rejects(
        [&]
        {
            parse_index(malformed.dump(), build);
        });
}
