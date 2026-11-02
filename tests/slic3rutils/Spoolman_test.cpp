#include "catch_main.hpp"

#include "slic3r/Utils/Spoolman.hpp"

#include <chrono>
#include <cstdlib>
#include <future>
#include <utility>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace
{

// Trimmed copy of a real GET /api/v1/spool response.
const char* SpoolsJson = R"([
  {"id": 2, "remaining_weight": 120.0, "location": "Dry box 1", "archived": false,
   "filament": {"id": 102, "name": "PolyTerra Charcoal", "material": "PLA", "color_hex": "333333FF",
                "vendor": {"id": 1, "name": "Polymaker"}}},
  {"id": 3, "remaining_weight": 900.0, "location": null, "archived": false,
   "filament": {"id": 103, "name": "Silk Tri-colour", "material": "PLA",
                "multi_color_hexes": "FF0000,00ff00,0000FF", "multi_color_direction": "coaxial",
                "vendor": {"id": 2, "name": "eSun"}}},
  {"id": 4, "remaining_weight": null, "archived": false,
   "filament": {"id": 104, "name": "Rainbow", "material": "PLA",
                "multi_color_hexes": "ff8800,8800ff", "multi_color_direction": "longitudinal"}},
  {"id": 6, "archived": false, "filament": {"id": 106, "name": "Mystery", "material": "TPU"}},
  {"id": 7, "archived": true, "filament": {"id": 107, "name": "Old", "material": "PLA", "color_hex": "00FFFF"}}
])";

const SpoolmanSpool* FindSpool(const std::vector<SpoolmanSpool>& spools, int id)
{
    for (const SpoolmanSpool& spool : spools)
        if (spool.id == id)
            return &spool;
    return nullptr;
}

} // namespace

TEST_CASE("Spoolman URL is normalized", "[Spoolman]")
{
    CHECK(Spoolman::NormalizeUrl("  192.168.1.10:7912/ ") == "http://192.168.1.10:7912");
    CHECK(Spoolman::NormalizeUrl("https://spoolman.local//") == "https://spoolman.local");
    CHECK(Spoolman::NormalizeUrl("HTTP://host:7912") == "HTTP://host:7912");
    CHECK(Spoolman::NormalizeUrl("   ").empty());
}

TEST_CASE("Spoolman spool list is parsed", "[Spoolman]")
{
    std::vector<SpoolmanSpool> spools;
    std::string error;
    REQUIRE(Spoolman::ParseSpools(SpoolsJson, spools, error));
    REQUIRE(spools.size() == 4);
    CHECK(FindSpool(spools, 7) == nullptr);

    const SpoolmanSpool* single = FindSpool(spools, 2);
    REQUIRE(single != nullptr);
    CHECK(single->colors == std::vector<std::string>{"#333333"});
    CHECK(single->vendor == "Polymaker");
    CHECK(single->location == "Dry box 1");
    CHECK_THAT(single->remainingWeight, WithinAbs(120.0, 1e-9));
    CHECK(single->DisplayName() == "Polymaker PLA PolyTerra Charcoal");

    const SpoolmanSpool* coaxial = FindSpool(spools, 3);
    REQUIRE(coaxial != nullptr);
    CHECK(coaxial->colors == std::vector<std::string>{"#FF0000", "#00FF00", "#0000FF"});
    CHECK_FALSE(coaxial->longitudinal);
    CHECK(coaxial->location.empty());

    const SpoolmanSpool* gradient = FindSpool(spools, 4);
    REQUIRE(gradient != nullptr);
    CHECK(gradient->colors.size() == 2);
    CHECK(gradient->longitudinal);
    CHECK(gradient->remainingWeight < 0.0);
    CHECK(gradient->DisplayName() == "PLA Rainbow");

    const SpoolmanSpool* colorless = FindSpool(spools, 6);
    REQUIRE(colorless != nullptr);
    CHECK(colorless->colors.empty());
}

// Needs a running server: set SPOOLMAN_TEST_URL, e.g. to a local mock on http://127.0.0.1:7912.
TEST_CASE("Spoolman spools are fetched over HTTP", "[Spoolman][.network]")
{
    const char* url = std::getenv("SPOOLMAN_TEST_URL");
    if (url == nullptr)
        SKIP("SPOOLMAN_TEST_URL is not set");

    std::promise<std::pair<std::vector<SpoolmanSpool>, std::string>> result;
    Spoolman::FetchSpools(url, [&result](std::vector<SpoolmanSpool> spools, std::string error) {
        result.set_value({std::move(spools), std::move(error)});
    });
    auto future = result.get_future();
    REQUIRE(future.wait_for(std::chrono::seconds(20)) == std::future_status::ready);

    const auto [spools, error] = future.get();
    CHECK(error.empty());
    CHECK_FALSE(spools.empty());
}

TEST_CASE("Spoolman errors are reported", "[Spoolman]")
{
    std::vector<SpoolmanSpool> spools;
    std::string error;
    CHECK_FALSE(Spoolman::ParseSpools("<html>not json</html>", spools, error));
    CHECK_FALSE(error.empty());

    error.clear();
    CHECK_FALSE(Spoolman::ParseSpools(R"({"detail": "Not found"})", spools, error));
    CHECK_FALSE(error.empty());

    CHECK(Spoolman::ParseSpools(R"({"items": []})", spools, error));
    CHECK(spools.empty());
}
