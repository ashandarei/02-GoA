#pragma once
#include "shared/kh02/kh02_executable_verification.h"
#include <Windows.h>
#include <stdexcept>

namespace kh02build
{
    // Disk integrity is verified before any source service starts. Resolve the
    // matching loaded-module profile once; never scan or rediscover each frame.
    inline auto current() -> const kh02integrity::Build&
    {
        static const auto* build = []() -> const kh02integrity::Build*
        {
            const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
            {
                throw std::runtime_error("kh02_unsupported_image");
            }
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
            for (const auto* candidate : {&kh02integrity::steam, &kh02integrity::egs})
            {
                if (nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == candidate->timestamp &&
                    nt->OptionalHeader.SizeOfImage == candidate->image_size)
                {
                    return candidate;
                }
            }
            throw std::runtime_error("kh02_unsupported_image");
        }();
        return *build;
    }

    // Original Steam RVAs identify the audited call sites. EGS entries were
    // matched offline by instruction signatures and paired RIP references.
    // Vtables are cross-checked against their mapped function slots at runtime.
    inline auto rva(unsigned long steam_rva) -> unsigned long
    {
        if (&current() == &kh02integrity::steam)
        {
            return steam_rva;
        }
        switch (steam_rva)
        {
        case 0x42d7d0:
            return 0x42d7e0;
        case 0x44bdd0:
            return 0x44bde0;
        case 0x44c4c0:
            return 0x44c4d0;
        case 0x45a1d0:
            return 0x45a090;
        case 0x45a400:
            return 0x45a2c0;
        case 0x483f80:
            return 0x483d70;
        case 0x494b70:
            return 0x494960;
        case 0x54b250:
            return 0x54ae00;
        case 0x56e0d0:
            return 0x56dc50;
        case 0x58bbe0:
            return 0x58b0c0;
        case 0x599ed0:
            return 0x599290;
        case 0x5a60a0:
            return 0x5a4f70;
        case 0x5abdb0:
            return 0x5aa310;
        case 0x5aef49:
            return 0x5ad479;
        case 0xb8d6b0:
            return 0xb8b740;
        case 0x1601850:
            return 0x15ff630;
        case 0x1df7a80:
            return 0x1df5860;
        case 0x1dfa030:
            return 0x1df7e10;
        case 0x2b14f90:
            return 0x2b12fc0;
        case 0x2b183b0:
            return 0x2b163e0;
        case 0x3455ca0:
            return 0x34362c0;
        case 0x3457a50:
            return 0x3438070;
        case 0x34581a0:
            return 0x34387c0;
        case 0x3458490:
            return 0x3438ab0;
        case 0x34588b0:
            return 0x3438ed0;
        case 0x34589b0:
            return 0x3438fd0;
        case 0x3458e40:
            return 0x3439460;
        case 0x3459e20:
            return 0x343a440;
        case 0x345a0e0:
            return 0x343a700;
        case 0x345a1d0:
            return 0x343a7f0;
        case 0x345a3f0:
            return 0x343aa10;
        case 0x345a450:
            return 0x343aa70;
        case 0x345a4c0:
            return 0x343aae0;
        case 0x345a560:
            return 0x343ab80;
        case 0x345b0d0:
            return 0x343b6f0;
        case 0x345be10:
            return 0x343c430;
        case 0x345be70:
            return 0x343c490;
        case 0x345bf20:
            return 0x343c540;
        case 0x345c030:
            return 0x343c650;
        case 0x345f400:
            return 0x343fa20;
        case 0x3470940:
            return 0x3450f60;
        case 0x3762a70:
            return 0x3742aa8;
        case 0x37923e8:
            return 0x3772270;
        case 0x3fa6ae8:
            return 0x3f86918;
        case 0x41f7f28:
            return 0x41cbe18;
        case 0x41f9c98:
            return 0x41cdb88;
        case 0x4537720:
            return 0x450a390;
        case 0x480b5d0:
            return 0x47de250;
        case 0x4831e70:
            return 0x4804af0;
        case 0x4883800:
            return 0x48565b0;
        case 0x48bf920:
            return 0x48926d0;
        default:
            throw std::runtime_error("kh02_unmapped_native_address");
        }
    }
} // namespace kh02build
