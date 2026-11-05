#pragma once
#include <string>

namespace kh02
{
    // One bounded metadata enumeration after the game's pak layer is ready.
    // Neither file payloads nor compatibility edits are read during discovery.
    void discover_asset_index(void* platform_file);
    auto serialized_asset_index() -> const std::string&;
} // namespace kh02
