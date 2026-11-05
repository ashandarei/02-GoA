#include "asset_index_provider.h"
#include "build_profile.h"
#include "kh3ap/features/kh02/transport/kh02_index_state.h"
#include <exception>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kh02
{
    namespace
    {
        constexpr std::size_t native_file_size_slot = 14;
        constexpr std::size_t native_iterate_recursive_slot = 35;
        constexpr std::size_t maximum_discovery_visits = 131'072;
        std::string wire_index;

        class DirectoryVisitor
        {
          public:
            virtual ~DirectoryVisitor() = default;
            virtual bool visit(const wchar_t* path, bool directory) = 0;
        };

        class AssetVisitor final : public DirectoryVisitor
        {
          public:
            explicit AssetVisitor(void* platform) : platform_file(platform) {}

            bool visit(const wchar_t* path, bool directory) override
            {
                try
                {
                    if (!path || ++visits > maximum_discovery_visits)
                    {
                        throw std::runtime_error("kh02_source_index_visit_limit");
                    }
                    if (directory)
                    {
                        return true;
                    }
                    const std::wstring_view wide(path);
                    if (wide.size() > kh02asset::maximum_asset_path)
                    {
                        throw std::runtime_error("kh02_source_index_path_limit");
                    }
                    std::string source;
                    source.reserve(wide.size());
                    for (const auto character : wide)
                    {
                        if (character < 32 || character > 126)
                        {
                            throw std::runtime_error("kh02_source_index_nonascii");
                        }
                        source.push_back(character == L'\\' ? '/' : static_cast<char>(character));
                    }
                    const auto lower = kh02asset::ascii_lower(source);
                    const bool supported = kh02asset::advertise_source(source);
                    if (!supported)
                    {
                        return true;
                    }
                    if (!kh02asset::valid_source_path(source))
                    {
                        throw std::runtime_error("kh02_source_index_outside_content");
                    }
                    const auto size = file_size(path);
                    if (size < 0 || static_cast<uint64_t>(size) > kh02asset::maximum_asset_bytes)
                    {
                        throw std::runtime_error("kh02_source_index_file_size");
                    }
                    files[lower] = {std::move(source), static_cast<uint64_t>(size)};
                    if (files.size() >= kh02asset::maximum_index_files)
                    {
                        throw std::runtime_error("kh02_source_index_file_limit");
                    }
                    return true;
                }
                catch (...)
                {
                    failure = std::current_exception();
                    return false;
                }
            }

            auto file_size(const wchar_t* path) const -> int64_t
            {
                const auto table = *static_cast<void***>(platform_file);
                return reinterpret_cast<int64_t (*)(void*, const wchar_t*)>(table[native_file_size_slot])(platform_file,
                                                                                                          path);
            }

            std::map<std::string, kh02asset::SourceFile> files;
            std::exception_ptr failure;

          private:
            void* platform_file;
            std::size_t visits{};
        };
    } // namespace

    void discover_asset_index(void* platform_file)
    {
        if (kh02asset::index_ready())
        {
            return;
        }
        kh02asset::configure_source_identity(kh02build::current().executable_sha256);
        AssetVisitor visitor(platform_file);
        const auto table = *static_cast<void***>(platform_file);
        const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        for (const auto slot : {native_file_size_slot, native_iterate_recursive_slot})
        {
            const auto address = reinterpret_cast<uintptr_t>(table[slot]);
            MEMORY_BASIC_INFORMATION region{};
            if (address < image || address - image >= kh02build::current().image_size ||
                !VirtualQuery(table[slot], &region, sizeof(region)) || region.State != MEM_COMMIT ||
                (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
                !(region.Protect &
                  (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            {
                throw std::runtime_error("kh02_source_index_native_method");
            }
        }
        const std::wstring directory(kh02asset::source_content_prefix.begin(), kh02asset::source_content_prefix.end());
        const bool complete = reinterpret_cast<bool (*)(void*, const wchar_t*, DirectoryVisitor&)>(
            table[native_iterate_recursive_slot])(platform_file, directory.c_str(), visitor);
        if (visitor.failure)
        {
            std::rethrow_exception(visitor.failure);
        }
        if (!complete || visitor.files.empty())
        {
            throw std::runtime_error("kh02_source_index_enumeration_failed");
        }
        const std::wstring control(kh02asset::source_control_file.begin(), kh02asset::source_control_file.end());
        const auto control_size = visitor.file_size(control.c_str());
        if (control_size <= 0)
        {
            throw std::runtime_error("kh02_source_index_control_missing");
        }
        std::vector<kh02asset::SourceFile> files;
        files.reserve(visitor.files.size() + 1);
        files.push_back({std::string(kh02asset::source_control_file), static_cast<uint64_t>(control_size)});
        for (auto& [key, file] : visitor.files)
        {
            files.push_back(std::move(file));
        }
        auto index = kh02asset::build_index(std::move(files), kh02build::current().executable_sha256);
        wire_index = kh02asset::serialize_index(index);
        kh02asset::publish_index(std::move(index));
    }

    auto serialized_asset_index() -> const std::string&
    {
        return wire_index;
    }
} // namespace kh02
