#include "probe/movie_file.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <chrono>

int main(int argc, char** argv)
{
    using namespace kh02;
    if (argc == 2)
    {
        for (const auto& story : kh02movie_metadata::stories)
        {
            nlohmann::json evidence;
            const auto selected = resolve_movie_file(argv[1], story.file, evidence);
            assert(selected.story == &story);
            std::cout << evidence.dump() << '\n';
        }
        return 0;
    }
    const auto root =
        std::filesystem::temp_directory_path() /
        ("kh02-movie-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);

    struct Cleanup
    {
        std::filesystem::path root;

        ~Cleanup()
        {
            std::filesystem::remove_all(root);
        }
    } cleanup{root};

    const auto put = [&](const std::string& name, uintmax_t bytes = 48995424)
    {
        const auto file = root / name;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file).close();
        std::filesystem::resize_file(file, bytes);
    };
    nlohmann::json evidence;
    const auto select = [&](const std::string& request)
    { return resolve_movie_file(root, request, evidence).path.lexically_relative(root).generic_string(); };
    put("dt/cs_dw029_mv.usm");
    assert(select("ja/cs_dw029_mv.usm") == "dt/cs_dw029_mv.usm");
    assert(evidence["fallback"] == true);
    assert(evidence["attempts"].size() == 3);
    put("fr/cs_dw029_mv.usm", 49000000);
    assert(select("fr/cs_dw029_mv.usm") == "fr/cs_dw029_mv.usm");
    assert(evidence["fallback"] == false);
    put("en/cs_dw029_mv.usm");
    assert(select("ja/cs_dw029_mv.usm") == "en/cs_dw029_mv.usm");
    put("ja/cs_dw029_mv.usm", 48000000);
    assert(select("ja/cs_dw029_mv.usm") == "ja/cs_dw029_mv.usm");
    put("ja/cs_dw029_mv.usm", 5);
    assert(select("ja/cs_dw029_mv.usm") == "en/cs_dw029_mv.usm");
    put("en/cs_dw029_mv.usm", 1000001);
    assert(select("ja/cs_dw029_mv.usm") == "dt/cs_dw029_mv.usm");
    put("gear_a.usm", 16032320);
    assert(select("gear_a.usm") == "gear_a.usm");
    put("movie/dw026_movie.usm", 26591360);
    assert(select("movie/dw026_movie.usm") == "movie/dw026_movie.usm");
    for (const auto* request : {"../cs_dw029_mv.usm", "ja/../cs_dw029_mv.usm", "ja/unknown.usm", "en/gear_a.usm"})
    {
        bool rejected = false;
        try
        {
            select(request);
        }
        catch (const std::runtime_error& error)
        {
            rejected = std::string(error.what()) == "movie_not_allowlisted";
        }
        assert(rejected);
    }
    std::filesystem::remove_all(root / "dt");
    std::filesystem::remove_all(root / "fr");
    bool missing = false;
    try
    {
        select("ja/cs_dw029_mv.usm");
    }
    catch (const std::runtime_error& error)
    {
        missing = std::string(error.what()).starts_with("movie_file_unavailable:");
    }
    assert(missing);
    assert(evidence["attempts"].size() == 2);
}
