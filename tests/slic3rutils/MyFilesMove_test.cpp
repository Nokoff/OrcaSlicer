#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <iterator>
#include <string>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>

#include "slic3r/Utils/MyFilesMove.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

struct TemporaryFolder
{
    const fs::path path{fs::temp_directory_path() / fs::unique_path("orca-my-files-%%%%-%%%%")};

    TemporaryFolder() { fs::create_directories(path); }
    ~TemporaryFolder()
    {
        boost::system::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void put(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    fs::ofstream out(path, std::ios::binary);
    out << content;
}

std::string get(const fs::path &path)
{
    fs::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int entries_in(const fs::path &dir)
{
    return static_cast<int>(std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
}

} // namespace

TEST_CASE("My Files moves a file to a free name", "[MyFilesMove]")
{
    TemporaryFolder temp;
    const fs::path from = temp.path / "from" / "a.stl";
    const fs::path to   = temp.path / "to" / "a.stl";
    put(from, "model");
    fs::create_directories(to.parent_path());

    std::string error;
    REQUIRE(MyFilesMove::move_file(from, to, false, error));

    CHECK_FALSE(fs::exists(from));
    CHECK(get(to) == "model");
}

TEST_CASE("My Files replaces an existing file only when told to", "[MyFilesMove]")
{
    const bool overwrite = GENERATE(false, true);
    const bool by_copy   = GENERATE(false, true);
    CAPTURE(overwrite, by_copy);
    TemporaryFolder temp;
    const fs::path from = temp.path / "from" / "a.stl";
    const fs::path to   = temp.path / "to" / "a.stl";
    put(from, "new");
    put(to, "old");

    std::string error;
    const bool moved = by_copy ? MyFilesMove::move_by_copy(from, to, overwrite, error) :
                                 MyFilesMove::move_file(from, to, overwrite, error);
#ifdef _WIN32
    const bool expected = overwrite;
#else
    // rename() always replaces, so only move_file() refuses there
    const bool expected = overwrite || by_copy;
#endif
    CHECK(moved == expected);
    CHECK(get(to) == (expected ? "new" : "old"));
    CHECK(fs::exists(from) != expected);
    CHECK(entries_in(to.parent_path()) == 1);
}

TEST_CASE("A failed My Files move leaves the file it would have replaced", "[MyFilesMove][Regression]")
{
    const bool by_copy = GENERATE(false, true);
    CAPTURE(by_copy);
    TemporaryFolder temp;
    const fs::path from = temp.path / "from" / "a.stl";
    const fs::path to   = temp.path / "to" / "a.stl";
    put(to, "precious");

    std::string error;
    CHECK_FALSE((by_copy ? MyFilesMove::move_by_copy(from, to, true, error) : MyFilesMove::move_file(from, to, true, error)));

    CHECK_FALSE(error.empty());
    CHECK(get(to) == "precious");
    CHECK(entries_in(to.parent_path()) == 1);
}

TEST_CASE("A My Files move never replaces a folder or the file itself", "[MyFilesMove]")
{
    TemporaryFolder temp;
    const fs::path file = temp.path / "a.stl";
    put(file, "model");
    fs::create_directories(temp.path / "to" / "a.stl");

    std::string error;
    CHECK_FALSE(MyFilesMove::move_file(file, temp.path / "to" / "a.stl", true, error));
    CHECK_FALSE(MyFilesMove::move_file(file, file, true, error));
    CHECK_FALSE(MyFilesMove::move_file(file, temp.path / "to" / ".." / "a.stl", true, error));

    CHECK(get(file) == "model");
    CHECK(fs::is_directory(temp.path / "to" / "a.stl"));
}

TEST_CASE("A My Files move across volumes keeps the content and date and leaves no temporary file", "[MyFilesMove]")
{
    TemporaryFolder temp;
    const fs::path from = temp.path / "from" / "a.stl";
    const fs::path to   = temp.path / "to" / "a.stl";
    put(from, "new");
    put(to, "old");
    fs::last_write_time(from, 1700000000);

    std::string error;
    REQUIRE(MyFilesMove::move_by_copy(from, to, true, error));

    CHECK_FALSE(fs::exists(from));
    CHECK(get(to) == "new");
    CHECK(fs::last_write_time(to) == 1700000000);
    CHECK(entries_in(to.parent_path()) == 1);
}

#ifdef _WIN32
TEST_CASE("My Files keeps both files when either is in use", "[MyFilesMove][Regression]")
{
    const bool source_in_use = GENERATE(false, true);
    CAPTURE(source_in_use);
    TemporaryFolder temp;
    const fs::path from = temp.path / "from" / "a.stl";
    const fs::path to   = temp.path / "to" / "a.stl";
    put(from, "new");
    put(to, "precious");

    std::string error;
    {
        fs::ifstream in_use(source_in_use ? from : to, std::ios::binary);
        REQUIRE(in_use.good());
        CHECK_FALSE(MyFilesMove::move_file(from, to, true, error));
    }

    CHECK(get(from) == "new");
    CHECK(get(to) == "precious");
}

TEST_CASE("A My Files copy that cannot take the place of the destination is cleaned up", "[MyFilesMove][Regression]")
{
    TemporaryFolder temp;
    const fs::path from = temp.path / "from" / "a.stl";
    const fs::path to   = temp.path / "to" / "a.stl";
    put(from, "new");
    put(to, "precious");

    std::string error;
    {
        fs::ifstream in_use(to, std::ios::binary);
        REQUIRE(in_use.good());
        CHECK_FALSE(MyFilesMove::move_by_copy(from, to, true, error));
    }

    CHECK(get(from) == "new");
    CHECK(get(to) == "precious");
    CHECK(entries_in(to.parent_path()) == 1);
}

TEST_CASE("My Files moves files with names outside the ANSI code page", "[MyFilesMove]")
{
    TemporaryFolder temp;
    const fs::path from = temp.path / L"部品.stl";
    const fs::path to   = temp.path / L"移動" / L"部品.stl";
    put(from, "new");
    put(to, "old");

    std::string error;
    REQUIRE(MyFilesMove::move_file(from, to, true, error));

    CHECK_FALSE(fs::exists(from));
    CHECK(get(to) == "new");
}
#endif
