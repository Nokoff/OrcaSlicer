#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "slic3r/Utils/ProjectPaths.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

TEST_CASE("A model opened as a project gives Save no file to overwrite", "[ProjectPaths][Regression]")
{
    const std::string name = GENERATE(std::string("part.stl"), std::string("part.STL"), std::string("part.step"),
                                      std::string("part.obj"), std::string("part.zip.amf"), std::string("part.zip"),
                                      std::string("part.3mf.stl"), std::string("part"));
    CAPTURE(name);

    CHECK(ProjectPaths::project_folder(fs::path("models") / name, false).empty());
}

TEST_CASE("A 3MF project is saved beside where it was opened", "[ProjectPaths]")
{
    const std::string name = GENERATE(std::string("part.3mf"), std::string("part.3MF"), std::string("part.gcode.3mf"));
    CAPTURE(name);

    CHECK(ProjectPaths::project_folder(fs::path("models") / name, false) == fs::path("models"));
}

TEST_CASE("A G-code preview keeps the folder it was opened from", "[ProjectPaths]")
{
    CHECK(ProjectPaths::project_folder(fs::path("models") / "part.gcode", true) == fs::path("models"));
}

TEST_CASE("A 3MF imported as geometry gives Save no file to overwrite", "[ProjectPaths][Regression]")
{
    const std::string name = GENERATE(std::string("part.3mf"), std::string("part.3MF"));
    CAPTURE(name);

    CHECK(ProjectPaths::project_folder(fs::path("models") / name, false, true).empty());
}

TEST_CASE("Importing into a saved project named Untitled keeps its name", "[ProjectPaths][Regression]")
{
    const fs::path folder = ProjectPaths::project_folder(fs::path("models") / "Untitled.3mf", false);

    CHECK_FALSE(ProjectPaths::import_may_rename(folder, true));
}

TEST_CASE("Importing names only an untitled project that was never saved", "[ProjectPaths]")
{
    CHECK(ProjectPaths::import_may_rename(fs::path(), true));
    CHECK_FALSE(ProjectPaths::import_may_rename(fs::path(), false));
}

TEST_CASE("An untitled project has no folder", "[ProjectPaths]")
{
    const std::string name = GENERATE(std::string(), std::string("Untitled"));
    CAPTURE(name);

    CHECK(ProjectPaths::project_folder(fs::path(name), false).empty());
}

TEST_CASE("Crash recovery does not return to a file that was not opened as a project", "[ProjectPaths][Regression]")
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("orca-project-paths-%%%%-%%%%");
    fs::create_directories(dir);
    fs::ofstream(dir / ".3mf") << "backup";
    fs::ofstream(dir / "origin.txt") << "models/part.3mf";

    ProjectPaths::forget_origin(dir);

    std::string path = dir.string();
    std::string origin;
    const bool restorable = has_restore_data(path, origin);
    boost::system::error_code ignored;
    fs::remove_all(dir, ignored);

    CHECK(restorable);
    CHECK(origin.empty());
}
