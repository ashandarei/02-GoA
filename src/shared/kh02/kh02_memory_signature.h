#pragma once
#include <cstddef>
#include <string_view>

namespace kh02integrity
{
    // Compare a bounded snapshot of loaded code with its SHA-256 fingerprint.
    // Expected executable instructions are never embedded in the companion.
    auto signature_matches(const void* address, size_t bytes, std::string_view expected_sha256) noexcept -> bool;
} // namespace kh02integrity
