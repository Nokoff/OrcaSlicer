#include <catch2/catch_test_macros.hpp>
#include <limits>

#include "slic3r/pchheader.hpp"
#include "slic3r/GUI/PartPlate.hpp"

using namespace Slic3r;

TEST_CASE("Plate picking ignores IDs left behind by a deleted second plate", "[PartPlate][fast]")
{
    GUI::PartPlateList plates(256, 256, 256, nullptr, nullptr, ptFFF);
    GUI::PartPlate*    first_plate = plates.get_plate(0);
    REQUIRE(plates.create_plate(false) == 1);
    REQUIRE(plates.delete_plate(1) == 0);
    REQUIRE(plates.get_plate_count() == 1);

    // After plate 2 is removed, its picking IDs can survive until the next scene reload.
    for (unsigned int action = 0; action < GUI::PartPlate::GRABBER_COUNT; ++action)
        REQUIRE_NOTHROW(plates.set_hover_id(GUI::PartPlate::GRABBER_COUNT + action));

    REQUIRE_NOTHROW(plates.set_hover_id(-1));
    REQUIRE_NOTHROW(plates.set_hover_id(std::numeric_limits<int>::min()));
    REQUIRE_NOTHROW(plates.set_hover_id(std::numeric_limits<int>::max()));
    REQUIRE_NOTHROW(plates.set_hover_id(0));
    REQUIRE_NOTHROW(plates.set_hover_id(GUI::PartPlate::GRABBER_COUNT - 1));
    REQUIRE(plates.get_plate(0) == first_plate);
    REQUIRE(plates.get_curr_plate_index() == 0);
}

TEST_CASE("Mesh changes invalidate slice results on every plate", "[PartPlate][fast]")
{
    GUI::PartPlateList plates(256, 256, 256, nullptr, nullptr, ptFFF);
    REQUIRE(plates.create_plate(false) == 1);

    GUI::PartPlate* first_plate = plates.get_plate(0);
    GUI::PartPlate* second_plate = plates.get_plate(1);
    REQUIRE(first_plate != nullptr);
    REQUIRE(second_plate != nullptr);
    first_plate->update_slice_result_valid_state(true);
    second_plate->update_slice_result_valid_state(true);
    REQUIRE(first_plate->is_slice_result_valid());
    REQUIRE(second_plate->is_slice_result_valid());

    plates.invalid_all_slice_result();

    CHECK_FALSE(first_plate->is_slice_result_valid());
    CHECK_FALSE(second_plate->is_slice_result_valid());
}
