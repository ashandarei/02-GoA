#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kh02asset
{
    inline constexpr std::string_view source_content_prefix =
        "../../../KINGDOM HEARTS 0.2 Birth by Sleep/Content/";
    inline constexpr std::string_view source_control_file = "../../../Engine/Config/Base.ini";
    inline constexpr std::string_view index_schema = "kh02.asset_index.v1";
    inline constexpr std::size_t maximum_index_files = 65'536;
    inline constexpr std::size_t maximum_index_bytes = 16 * 1024 * 1024;
    inline constexpr std::size_t maximum_asset_path = 1024;
    inline constexpr uint64_t maximum_asset_bytes = 8ULL * 1024 * 1024 * 1024;

    struct SourceFile
    {
        std::string source;
        uint64_t size{};
    };

    struct File
    {
        std::wstring source;
        std::wstring suffix;
        uint64_t size{};
    };

    struct AssetIndex
    {
        uint64_t tag{};
        std::vector<File> files;
        std::vector<std::array<uint32_t, 2>> voice_pairs;
        std::string source_build;
    };

    inline auto ascii_lower(std::string_view value) -> std::string
    {
        std::string result(value);
        for (auto& character : result)
        {
            if (character >= 'A' && character <= 'Z')
            {
                character += 'a' - 'A';
            }
        }
        return result;
    }

    inline auto source_ascii(std::wstring_view value) -> std::string
    {
        std::string result;
        result.reserve(value.size());
        for (const auto character : value)
        {
            if (character < 32 || character > 126)
            {
                throw std::runtime_error("kh02_index_nonascii_path");
            }
            result.push_back(static_cast<char>(character));
        }
        return result;
    }

    inline auto advertise_source(std::string_view source) -> bool
    {
        if (!source.starts_with(source_content_prefix))
        {
            return false;
        }
        const auto relative = ascii_lower(source.substr(source_content_prefix.size()));
        const auto dot = relative.find_last_of('.');
        if (dot == std::string::npos)
        {
            return false;
        }
        const auto extension = relative.substr(dot);
        if (extension != ".uasset" && extension != ".umap" && extension != ".uexp" &&
            extension != ".ubulk" && extension != ".locres")
        {
            return false;
        }
        // These packages are authored for KH3's shader ABI. The source game's
        // versions must not shadow their host implementations.
        constexpr std::array<std::string_view, 12> host_packages = {
            "maps/dw/uasset/materials/pp_ed_sky_change",
            "maps/dw/uasset/materials/m_dw_env_mirrorsky_horizon",
            "blueprints/camera/postprocessmaterial",
            "maps/dw/uasset/materials/m_dw_arc_bigmirror",
            "maps/dw/uasset/materials/mi_dw_pro_mirrorinvisiblesurface",
            "maps/dw/uasset/materials/pp_dw10_colorgrading",
            "maps/dw/uasset/materials/m_dw_env_di_clouda",
            "maps/dw/uasset/materials/mi_dw_env_di_clouda",
            "maps/dw/uasset/materials/mi_dw_env_di_cloudb",
            "maps/dw/gimmick/g_dw_thorna/mdl/m_dw_thorn",
            "cutscene/materials/pp_charaev",
            "maps/dw/uasset/materials/m_ed_puddlee"};
        return std::find(host_packages.begin(), host_packages.end(), relative.substr(0, dot)) ==
               host_packages.end();
    }

    inline auto valid_source_path(std::string_view value) -> bool
    {
        if (value.empty() || value.size() > maximum_asset_path ||
            std::any_of(value.begin(), value.end(),
                        [](unsigned char character)
                        {
                            return character < 32 || character > 126 || character == '\\' ||
                                   character == ':';
                        }))
        {
            return false;
        }
        if (value == source_control_file)
        {
            return true;
        }
        if (!value.starts_with(source_content_prefix))
        {
            return false;
        }
        const auto relative = value.substr(source_content_prefix.size());
        return !relative.empty() && !relative.starts_with('/') &&
               relative.find("..") == std::string_view::npos &&
               relative.find("//") == std::string_view::npos && !relative.starts_with("./") &&
               relative.find("/./") == std::string_view::npos;
    }

    inline auto index_tag(std::string_view build) -> uint64_t
    {
        if (build.size() != 64 || std::any_of(build.begin(), build.end(),
                                              [](char character)
                                              {
                                                  return !((character >= '0' && character <= '9') ||
                                                           (character >= 'a' && character <= 'f'));
                                              }))
        {
            throw std::runtime_error("kh02_index_build_identity");
        }
        // The authenticated session binds this protocol/rule revision and the
        // verified executable. File identities arrive from that owned provider.
        constexpr uint64_t fnv_basis = 14695981039346656037ULL;
        constexpr uint64_t fnv_prime = 1099511628211ULL;
        uint64_t result = fnv_basis;
        for (const auto value : {std::string_view{"KH02.AssetIndex1.AssetRead3.Rules1"}, build})
        {
            for (const auto character : value)
            {
                result = (result ^ static_cast<unsigned char>(character)) * fnv_prime;
            }
        }
        return result ? result : fnv_basis;
    }

    inline auto target_suffix(std::string_view source) -> std::string
    {
        if (!valid_source_path(source))
        {
            throw std::runtime_error("kh02_index_source_path");
        }
        if (source == source_control_file)
        {
            return {};
        }
        auto relative = std::string(source.substr(source_content_prefix.size()));
        const auto lower = ascii_lower(relative);
        const bool title_control = lower == "maps/dw/uasset/textures/t_dw_titlebg.uasset" ||
                                   lower == "maps/dw/uasset/textures/t_dw_titlebg.uexp";
        if (!lower.ends_with(".locres") && !title_control)
        {
            relative.front() = 'Z';
        }
        return "/content/" + ascii_lower(relative);
    }

    inline auto build_index(std::vector<SourceFile> sources, std::string build) -> AssetIndex
    {
        if (sources.empty() || sources.size() > maximum_index_files)
        {
            throw std::runtime_error("kh02_index_file_count");
        }
        std::sort(sources.begin(), sources.end(),
                  [](const SourceFile& left, const SourceFile& right)
                  {
                      if (left.source == source_control_file || right.source == source_control_file)
                      {
                          return left.source == source_control_file &&
                                 right.source != source_control_file;
                      }
                      return ascii_lower(left.source) < ascii_lower(right.source);
                  });
        if (sources.front().source != source_control_file || !sources.front().size)
        {
            throw std::runtime_error("kh02_index_control_missing");
        }
        AssetIndex result{index_tag(build), {}, {}, std::move(build)};
        result.files.reserve(sources.size());
        std::map<std::string, uint32_t> suffixes;
        std::string previous;
        for (const auto& source : sources)
        {
            const auto key = ascii_lower(source.source);
            if (!valid_source_path(source.source) || source.size > maximum_asset_bytes ||
                key == previous ||
                (source.source != source_control_file && !advertise_source(source.source)))
            {
                throw std::runtime_error("kh02_index_file_identity");
            }
            previous = key;
            const auto suffix = target_suffix(source.source);
            const auto id = static_cast<uint32_t>(result.files.size());
            if (!suffix.empty() && !suffixes.emplace(suffix, id).second)
            {
                throw std::runtime_error("kh02_index_duplicate_target");
            }
            result.files.push_back({{source.source.begin(), source.source.end()},
                                    {suffix.begin(), suffix.end()},
                                    source.size});
        }
        constexpr std::string_view english = "/content/zound/voice/localization/en/";
        constexpr std::string_view japanese = "/content/zound/voice/localization/ja/";
        for (const auto& [suffix, id] : suffixes)
        {
            if (!suffix.starts_with(english))
            {
                continue;
            }
            const auto counterpart =
                suffixes.find(std::string(japanese) + suffix.substr(english.size()));
            if (counterpart != suffixes.end())
            {
                result.voice_pairs.push_back({id, counterpart->second});
            }
        }
        return result;
    }

    inline auto serialize_index(const AssetIndex& index) -> std::string
    {
        nlohmann::json files = nlohmann::json::array();
        for (const auto& file : index.files)
        {
            files.push_back({{"source", source_ascii(file.source)}, {"size", file.size}});
        }
        auto result = nlohmann::json{{"schema", index_schema},
                                     {"source_build", index.source_build},
                                     {"tag", index.tag},
                                     {"files", std::move(files)}}
                          .dump();
        if (result.size() > maximum_index_bytes)
        {
            throw std::runtime_error("kh02_index_byte_limit");
        }
        return result;
    }

    inline auto parse_index(std::string_view text, std::string_view expected_build) -> AssetIndex
    {
        if (text.empty() || text.size() > maximum_index_bytes)
        {
            throw std::runtime_error("kh02_index_byte_limit");
        }
        const auto document =
            nlohmann::json::parse(text,
                                  [](int depth, nlohmann::json::parse_event_t, nlohmann::json&)
                                  {
                                      if (depth > 8)
                                      {
                                          throw std::runtime_error("kh02_index_depth");
                                      }
                                      return true;
                                  });
        if (!document.is_object() || document.size() != 4 ||
            document.at("schema") != std::string(index_schema) ||
            document.at("source_build") != std::string(expected_build) ||
            document.at("tag") != index_tag(expected_build))
        {
            throw std::runtime_error("kh02_index_identity");
        }
        const auto& files = document.at("files");
        if (!files.is_array() || files.empty() || files.size() > maximum_index_files)
        {
            throw std::runtime_error("kh02_index_file_count");
        }
        std::vector<SourceFile> sources;
        sources.reserve(files.size());
        for (const auto& file : files)
        {
            if (!file.is_object() || file.size() != 2 || !file.at("source").is_string() ||
                !file.at("size").is_number_unsigned())
            {
                throw std::runtime_error("kh02_index_record");
            }
            sources.push_back(
                {file.at("source").get<std::string>(), file.at("size").get<uint64_t>()});
        }
        auto result = build_index(std::move(sources), std::string(expected_build));
        if (serialize_index(result) != text)
        {
            throw std::runtime_error("kh02_index_noncanonical");
        }
        return result;
    }
} // namespace kh02asset
