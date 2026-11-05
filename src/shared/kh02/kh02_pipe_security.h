#pragma once
#define NOMINMAX
#include <Windows.h>
#include <sddl.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace kh02ipc
{
    struct Handle
    {
        HANDLE value{};

        explicit Handle(HANDLE handle = nullptr) : value(handle) {}

        ~Handle()
        {
            if (value && value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
            }
        }

        Handle(const Handle&) = delete;
        auto operator=(const Handle&) -> Handle& = delete;
    };

    // Use Win32 identities under Wine/Proton too. Never substitute Unix PIDs,
    // a platform-specific account name, or a permissive DACL fallback.
    struct UserSecurity
    {
        PSECURITY_DESCRIPTOR descriptor{};
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};

        UserSecurity()
        {
            HANDLE raw_token{};
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
            {
                throw std::runtime_error("pipe_user_token");
            }
            Handle token(raw_token);
            DWORD bytes{};
            GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
            std::vector<unsigned char> data(bytes);
            if (!GetTokenInformation(token.value, TokenUser, data.data(), bytes, &bytes))
            {
                throw std::runtime_error("pipe_user_identity");
            }
            LPWSTR sid{};
            if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid))
            {
                throw std::runtime_error("pipe_user_sid");
            }

            struct LocalString
            {
                LPWSTR value;

                ~LocalString()
                {
                    LocalFree(value);
                }
            } sid_storage{sid};

            const auto dacl = std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;") + sid + L")";
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(), SDDL_REVISION_1, &descriptor,
                                                                      nullptr))
            {
                throw std::runtime_error("pipe_user_dacl");
            }
            attributes.lpSecurityDescriptor = descriptor;
        }

        ~UserSecurity()
        {
            if (descriptor)
            {
                LocalFree(descriptor);
            }
        }

        UserSecurity(const UserSecurity&) = delete;
        auto operator=(const UserSecurity&) -> UserSecurity& = delete;
    };
} // namespace kh02ipc
