#pragma once
#include <cstdint>
#include <string>

namespace kh02
{
    void record_pak_read(uint32_t file, uint64_t offset, uint32_t length, uint64_t microseconds,
                         uint64_t started_ms = 0, uint64_t finished_ms = 0);
    auto pak_read_profile_status() -> std::string;
} // namespace kh02
