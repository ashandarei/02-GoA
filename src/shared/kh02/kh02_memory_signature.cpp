#include "kh02_memory_signature.h"
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <vector>

namespace kh02integrity
{
    auto signature_matches(const void* address, size_t bytes, std::string_view expected_sha256) noexcept -> bool
    {
        if (!address || !bytes || bytes > 64 || expected_sha256.size() != 64)
        {
            return false;
        }
        std::array<unsigned char, 64> snapshot{};
        SIZE_T copied{};
        if (!ReadProcessMemory(GetCurrentProcess(), address, snapshot.data(), bytes, &copied) || copied != bytes)
        {
            return false;
        }

        struct Hash
        {
            BCRYPT_ALG_HANDLE algorithm{};
            BCRYPT_HASH_HANDLE value{};

            ~Hash()
            {
                if (value)
                {
                    BCryptDestroyHash(value);
                }
                if (algorithm)
                {
                    BCryptCloseAlgorithmProvider(algorithm, 0);
                }
            }
        };

        // Keep hash backing storage alive until after BCryptDestroyHash.
        std::vector<unsigned char> storage;
        Hash hash;
        try
        {
            DWORD storage_bytes{};
            DWORD returned{};
            if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
                BCryptGetProperty(hash.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&storage_bytes),
                                  sizeof(storage_bytes), &returned, 0) < 0)
            {
                return false;
            }
            storage.resize(storage_bytes);
            std::array<unsigned char, 32> digest{};
            if (BCryptCreateHash(hash.algorithm, &hash.value, storage.data(), storage_bytes, nullptr, 0, 0) < 0 ||
                BCryptHashData(hash.value, snapshot.data(), static_cast<ULONG>(bytes), 0) < 0 ||
                BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            {
                return false;
            }
            constexpr char digits[] = "0123456789abcdef";
            for (size_t index = 0; index < digest.size(); ++index)
            {
                if (expected_sha256[index * 2] != digits[digest[index] >> 4] ||
                    expected_sha256[index * 2 + 1] != digits[digest[index] & 15])
                {
                    return false;
                }
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
} // namespace kh02integrity
