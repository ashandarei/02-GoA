#include "kh02_executable_verification.h"
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace kh02integrity
{
    auto verify_executable(const std::filesystem::path& executable) -> const Build&
    {
        // Deny concurrent writes/deletion while hashing the file. Never trust a
        // filename, configurable expected hash, or PE timestamp as identity.
        const auto file = CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            throw std::runtime_error("kh02_executable_open_failed");
        }
        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE hash{};
        bool file_open = true;
        std::vector<unsigned char> object;
        try
        {
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file, &size) ||
                (size.QuadPart != steam.executable_bytes && size.QuadPart != egs.executable_bytes))
            {
                throw std::runtime_error("kh02_executable_size_mismatch");
            }
            DWORD object_bytes{}, returned{};
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
                BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_bytes),
                                  sizeof(object_bytes), &returned, 0) < 0)
            {
                throw std::runtime_error("kh02_executable_hash_provider");
            }
            object.resize(object_bytes);
            if (BCryptCreateHash(algorithm, &hash, object.data(), object_bytes, nullptr, 0, 0) < 0)
            {
                throw std::runtime_error("kh02_executable_hash_create");
            }
            std::array<unsigned char, 65536> buffer{};
            unsigned long long consumed{};
            DWORD received{};
            for (;;)
            {
                if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr))
                {
                    throw std::runtime_error("kh02_executable_read_failed");
                }
                if (!received)
                {
                    break;
                }
                consumed += received;
                if (BCryptHashData(hash, buffer.data(), received, 0) < 0)
                {
                    throw std::runtime_error("kh02_executable_hash_update");
                }
            }
            std::array<unsigned char, 32> digest{};
            if (consumed != size.QuadPart ||
                BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            {
                throw std::runtime_error("kh02_executable_hash_finish");
            }
            std::string actual;
            constexpr char digits[] = "0123456789abcdef";
            for (const auto byte : digest)
            {
                actual.push_back(digits[byte >> 4]);
                actual.push_back(digits[byte & 15]);
            }
            BCryptDestroyHash(hash);
            hash = nullptr;
            BCryptCloseAlgorithmProvider(algorithm, 0);
            algorithm = nullptr;
            CloseHandle(file);
            file_open = false;
            for (const auto* build : {&steam, &egs})
            {
                if (consumed == build->executable_bytes && actual == build->executable_sha256)
                {
                    return *build;
                }
            }
            throw std::runtime_error("kh02_executable_sha256_mismatch:" + actual);
        }
        catch (...)
        {
            if (hash)
            {
                BCryptDestroyHash(hash);
            }
            if (algorithm)
            {
                BCryptCloseAlgorithmProvider(algorithm, 0);
            }
            if (file_open)
            {
                CloseHandle(file);
            }
            throw;
        }
    }

    [[noreturn]] void terminate_failed_verification(const std::string& reason)
    {
        wchar_t image[32768]{};
        if (GetModuleFileNameW(nullptr, image, 32768))
        {
            std::ofstream report(std::filesystem::path(image).parent_path() / L"kh02_verification_failure.log",
                                 std::ios::binary | std::ios::trunc);
            report
                << "KH0.2 executable verification failed. A supported original EGS or Steam executable is required.\n"
                << reason << '\n';
            report.flush();
        }
        // Stop this integration process, including any owned companion job.
        // Do not raise an access violation, corrupt state, or terminate unrelated games.
        TerminateProcess(GetCurrentProcess(), ERROR_BAD_EXE_FORMAT);
        std::abort();
    }
} // namespace kh02integrity
