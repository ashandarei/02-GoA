#pragma once
#include "kh02/story_movie_metadata.h"
#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace kh02
{
    inline bool movie_language_directory(std::string_view language)
    {
        return language.size() >= 2 && language.size() <= 16 &&
               std::all_of(language.begin(), language.end(),
                           [](char value)
                           {
                               return value >= 'a' && value <= 'z' || value >= 'A' && value <= 'Z' ||
                                      value >= '0' && value <= '9' || value == '-' || value == '_';
                           });
    }

    struct MovieFile
    {
        std::filesystem::path path;
        const kh02movie_metadata::Movie* story{};
        bool gear{};
    };

    // Resolve only an allowlisted basename, once per request. Language folders
    // are inspected on demand, never polled while the decoder is idle.
    inline MovieFile resolve_movie_file(const std::filesystem::path& root, const std::string& requested,
                                        nlohmann::json& evidence)
    {
        MovieFile result;
        result.gear = requested == "gear_a.usm" || requested == "gear_b.usm" || requested == "gear_c.usm" ||
                      requested == "gear_d.usm";
        const auto slash = requested.find('/');
        const auto language = requested.substr(0, slash);
        const auto basename = slash == std::string::npos ? requested : requested.substr(slash + 1);
        for (const auto& candidate : kh02movie_metadata::stories)
        {
            const std::string_view audited = candidate.file;
            if (requested == audited ||
                audited.starts_with("en/") && movie_language_directory(language) && basename == audited.substr(3))
            {
                result.story = &candidate;
                break;
            }
        }
        if (!result.gear && !result.story)
        {
            throw std::runtime_error("movie_not_allowlisted");
        }
        evidence = {{"requested_movie", requested}, {"attempts", nlohmann::json::array()}};
        const bool localized = result.story && std::string_view(result.story->file).starts_with("en/");
        std::vector<std::string> candidates;
        if (localized)
        {
            candidates.push_back(language);
            if (language != "en")
            {
                candidates.push_back("en");
            }
            std::vector<std::string> installed;
            for (const auto& entry : std::filesystem::directory_iterator(root))
            {
                const auto name = entry.path().filename().string();
                if (entry.is_directory() && !entry.is_symlink() && movie_language_directory(name) && name != "movie" &&
                    std::find(candidates.begin(), candidates.end(), name) == candidates.end())
                {
                    installed.push_back(name);
                }
            }
            std::sort(installed.begin(), installed.end());
            candidates.insert(candidates.end(), installed.begin(), installed.end());
        }
        else
        {
            candidates.emplace_back();
        }
        for (const auto& candidate : candidates)
        {
            const auto relative = localized ? candidate + "/" + basename : requested;
            const auto file = root / relative;
            std::error_code error;
            const auto bytes = std::filesystem::file_size(file, error);
            nlohmann::json attempt = {{"movie", relative}};
            if (error)
            {
                attempt["error"] = error.message();
            }
            else if (bytes < 1000000 || bytes > (result.gear ? 32000000ULL : 1024ULL * 1024 * 1024) ||
                     result.story && (!localized || candidate == "en") && bytes != result.story->bytes)
            {
                attempt["bytes"] = bytes;
                attempt["error"] = "movie_file_size";
            }
            else
            {
                // Localized encodes may differ in size. Keep bounded file sizes,
                // the audited basename and the decoder's frame/profile validation.
                attempt["bytes"] = bytes;
                evidence["attempts"].push_back(attempt);
                evidence["resolved_movie"] = relative;
                evidence["language_directory"] = localized ? candidate : "";
                evidence["fallback"] = relative != requested;
                result.path = file;
                return result;
            }
            evidence["attempts"].push_back(attempt);
        }
        throw std::runtime_error("movie_file_unavailable:" + requested);
    }
} // namespace kh02
