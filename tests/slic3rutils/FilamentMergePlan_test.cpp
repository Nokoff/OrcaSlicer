#include <catch2/catch_test_macros.hpp>
#include <algorithm>

#include "slic3r/GUI/filamentsync/FilamentMergePlan.hpp"
#include "slic3r/GUI/filamentsync/FilamentSyncAlgorithm.hpp"

using namespace Slic3r;

namespace {
GUI::FilamentData filament(const char* color, const char* type = "PLA")
{
    GUI::FilamentData result;
    result.m_type         = type;
    result.m_color.colors = {color};
    return result;
}

void check_targets_survive(const FilamentMergePlan& plan)
{
    for (const auto& step : plan.steps) {
        REQUIRE(step.source != step.target);
        REQUIRE(std::find(plan.survivors.begin(), plan.survivors.end(), step.target) != plan.survivors.end());
    }
}
} // namespace

TEST_CASE("Similar filament tolerance supports values between presets", "[filament_merge][fast]")
{
    const std::vector<GUI::FilamentData> design   = {filament("#FF0000"), filament("#EF1000")};
    const float                          distance = delta_e_ciede2000(255, 0, 0, 239, 16, 0);
    REQUIRE(plan_merge_similar(design, distance - 0.1f, true, {}).empty());
    const auto plan = plan_merge_similar(design, distance + 0.1f, true, {});
    REQUIRE(plan.steps.size() == 1);
    REQUIRE(plan.steps.front().source == 1);
    REQUIRE(plan.steps.front().target == 0);
}

TEST_CASE("Manual filament choices survive tolerance changes", "[filament_merge][fast]")
{
    const std::vector<GUI::FilamentData> design = {filament("#FF0000"), filament("#FF0000"), filament("#0000FF")};
    for (float tolerance : {0.0f, 6.1f, 25.0f}) {
        CAPTURE(tolerance);
        // Keeping a source cancels its automatic merge.
        {
            const auto plan = plan_merge_similar(design, tolerance, true, {}, {-1, 1, -1});
            REQUIRE(plan.empty());
            REQUIRE(plan.survivors.size() == 3);
        }
        // Remapping bypasses tolerance and retains the chosen destination.
        {
            const auto plan = plan_merge_similar(design, tolerance, true, {}, {1, -1, 1});
            REQUIRE(plan.steps.size() == 2);
            REQUIRE(plan.survivors == std::vector<size_t>{1});
            check_targets_survive(plan);
        }
    }
    REQUIRE(plan_merge_similar(design, 6.0f, true, {}).steps.size() == 1);
}

TEST_CASE("Manual filament mappings preserve material and mixed filament constraints", "[filament_merge][fast]")
{
    const std::vector<GUI::FilamentData> design = {filament("#FF0000"), filament("#0000FF"), filament("#FF0000", "PETG")};
    SECTION("Pinned sources cannot be merged away")
    {
        const auto plan = plan_merge_similar(design, 0.0f, true, {false, true, true}, {1, -1, -1});
        REQUIRE(plan.empty());
        REQUIRE(plan.pinned == std::vector<size_t>{0});
    }
    SECTION("Pinned filaments can still receive merges")
    {
        const auto plan = plan_merge_similar(design, 0.0f, true, {false, true, true}, {-1, 0, -1});
        REQUIRE(plan.steps.size() == 1);
        REQUIRE(plan.steps.front().target == 0);
        check_targets_survive(plan);
    }
    SECTION("Different materials cannot be manually merged") { REQUIRE(plan_merge_similar(design, 0.0f, true, {}, {2, -1, -1}).empty()); }
    SECTION("Invalid destinations and cycles are ignored")
    {
        REQUIRE(plan_merge_similar(design, 0.0f, true, {}, {99, -1, -1}).empty());
        REQUIRE(plan_merge_similar(design, 0.0f, true, {}, {1, 0, -1}).empty());
        REQUIRE(plan_merge_similar(design, 0.0f, true, {}, {1, 2, 0}).empty());
    }
}
