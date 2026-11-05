#pragma once
#include "shared/kh02/kh02_asset_index.h"

namespace kh02asset
{
    // Immutable snapshots live for the process lifetime. Publication occurs
    // only while the provider is being prepared, before native reads bind.
    auto asset_index() -> const AssetIndex&;
    void configure_source_identity(std::string build);
    void publish_index(AssetIndex index);
    auto index_ready() -> bool;
} // namespace kh02asset
