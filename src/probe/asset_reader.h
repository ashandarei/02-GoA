#pragma once
#include <string>
#include <cstdint>

namespace kh02
{
    auto native_pak_available() -> bool;
    bool start_asset_reader(uint32_t owner_pid);
    void stop_asset_reader();
    auto asset_reader_ready() -> bool;
    auto asset_reader_status() -> std::string;
} // namespace kh02
