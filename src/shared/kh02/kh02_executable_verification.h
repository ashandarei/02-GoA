#pragma once
#include <filesystem>
#include <string>
#include <cstdint>

namespace kh02integrity
{
    struct Build
    {
        const char* platform;
        uint32_t timestamp;
        uint32_t image_size;
        unsigned long long executable_bytes;
        const char* executable_sha256;
    };

    inline constexpr Build steam{"steam", 0x666d9eb7, 0x4f5f000, 76522024,
                                 "a5945af5041047f54db67d7667390f4dc90eee59d1c06459434ddab57d0f339c"};
    inline constexpr Build egs{"egs", 0x6662e5cd, 0x4efc000, 76122896,
                               "e4a9e731be7b58012dc8f189f3a033c0828aba4116405dba08122c934a17fa05"};
    // Hash the original disk image, independently of legitimate runtime hooks.
    auto verify_executable(const std::filesystem::path& executable) -> const Build&;
    [[noreturn]] void terminate_failed_verification(const std::string& reason);
} // namespace kh02integrity
