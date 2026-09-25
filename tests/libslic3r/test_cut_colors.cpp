#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

static ModelVolume* add_colored_cube(ModelObject& object, double x, int extruder, const std::string& material, int wall_loops = 2)
{
    ModelVolume* volume = object.add_volume(make_cube(20., 20., 20.), ModelVolumeType::MODEL_PART, false);
    volume->set_offset(Vec3d(x, 0., 0.));
    volume->config.set_key_value("extruder", new ConfigOptionInt(extruder));
    volume->config.set_key_value("wall_loops", new ConfigOptionInt(wall_loops));
    volume->set_material_id(material);
    return volume;
}

static std::vector<Cut::Part> select_upper_parts(const ModelObject& preview)
{
    std::vector<Cut::Part> selection;
    for (const ModelVolume* volume : preview.volumes)
        selection.push_back({volume->is_from_upper(), false});
    return selection;
}

TEST_CASE("Contour cuts preserve distinct part colors and settings", "[Cut][PartColor]")
{
    Model model;
    model.add_material("red");
    model.add_material("blue");
    ModelObject* object = model.add_object();
    object->add_instance()->set_offset(Vec3d(100., 200., 30.));
    add_colored_cube(*object, 0., 2, "red");
    add_colored_cube(*object, 30., 2, "red");
    add_colored_cube(*object, 60., 3, "blue");
    add_colored_cube(*object, 90., 3, "blue", 4);
    add_colored_cube(*object, 120., 2, "blue");

    const Transform3d      cut_matrix = Geometry::translation_transform(Vec3d(0., 0., 10.));
    Cut                    preview_cut(object, 0, cut_matrix);
    const ModelObjectPtrs& preview = preview_cut.perform_with_plane();
    REQUIRE(preview.size() == 1);
    REQUIRE(preview[0]->volumes.size() == 10);

    Cut                    cut(preview[0], 0, cut_matrix,
                               ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs& results = cut.perform_by_contour(object, select_upper_parts(*preview[0]), 0);
    REQUIRE(results.size() == 2);
    for (size_t side = 0; side < results.size(); ++side) {
        const ModelObject& result = *results[side];
        // Equal assignments merge; differing materials or settings remain separate.
        REQUIRE(result.volumes.size() == 4);
        std::vector<bool> found_groups(4, false);
        for (const ModelVolume* volume : result.volumes) {
            REQUIRE(volume->config.has("extruder"));
            REQUIRE(volume->config.has("wall_loops"));
            const int extruder = volume->config.extruder();
            const int walls    = volume->config.opt_int("wall_loops");
            size_t    group    = 0;
            double    min_x    = 0.;
            double    max_x    = 50.;
            if (volume->material_id() == "red") {
                CHECK(extruder == 2);
                CHECK(walls == 2);
            } else {
                CHECK(volume->material_id() == "blue");
                if (extruder == 2) {
                    group = 3;
                    min_x = 120.;
                    CHECK(walls == 2);
                } else {
                    CHECK(extruder == 3);
                    REQUIRE((walls == 2 || walls == 4));
                    group = walls == 2 ? 1 : 2;
                    min_x = walls == 2 ? 60. : 90.;
                }
                max_x = min_x + 20.;
            }
            CHECK_FALSE(found_groups[group]);
            found_groups[group] = true;
            CHECK(volume->material() != nullptr);
            for (const ModelVolume* original : object->volumes)
                CHECK(volume->config.id() != original->config.id());

            const BoundingBoxf3 bounds = volume->mesh().transformed_bounding_box(result.instances[0]->get_matrix() * volume->get_matrix());
            CHECK(bounds.min.x() == Catch::Approx(100. + min_x));
            CHECK(bounds.max.x() == Catch::Approx(100. + max_x));
            CHECK(bounds.min.y() == Catch::Approx(200.));
            CHECK(bounds.max.y() == Catch::Approx(220.));
            CHECK(bounds.min.z() == Catch::Approx(side == 0 ? 40. : 30.));
            CHECK(bounds.max.z() == Catch::Approx(side == 0 ? 50. : 40.));
        }
        for (bool found : found_groups)
            CHECK(found);
    }
}

TEST_CASE("Contour cuts retain one assigned color when merging to a single volume", "[Cut][PartColor]")
{
    Model model;
    model.add_material("red");
    ModelObject* object = model.add_object();
    object->add_instance()->set_offset(Vec3d(100., 200., 30.));
    add_colored_cube(*object, 0., 2, "red");
    add_colored_cube(*object, 30., 2, "red");

    const Transform3d      cut_matrix = Geometry::translation_transform(Vec3d(0., 0., 10.));
    Cut                    preview_cut(object, 0, cut_matrix);
    const ModelObjectPtrs& preview = preview_cut.perform_with_plane();
    REQUIRE(preview.size() == 1);
    Cut                    cut(preview[0], 0, cut_matrix, ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& results = cut.perform_by_contour(object, select_upper_parts(*preview[0]), 0);
    REQUIRE(results.size() == 2);
    for (size_t side = 0; side < results.size(); ++side) {
        const ModelObject& result = *results[side];
        REQUIRE(result.volumes.size() == 1);
        const ModelVolume& volume = *result.volumes[0];
        REQUIRE(volume.config.has("extruder"));
        CHECK(volume.config.extruder() == 2);
        CHECK(volume.material_id() == "red");
        const BoundingBoxf3 bounds = volume.mesh().transformed_bounding_box(result.instances[0]->get_matrix() * volume.get_matrix());
        CHECK(bounds.min.x() == Catch::Approx(100.));
        CHECK(bounds.max.x() == Catch::Approx(150.));
        CHECK(bounds.min.y() == Catch::Approx(200.));
        CHECK(bounds.max.y() == Catch::Approx(220.));
        CHECK(bounds.min.z() == Catch::Approx(side == 0 ? 40. : 30.));
        CHECK(bounds.max.z() == Catch::Approx(side == 0 ? 50. : 40.));
    }
}

TEST_CASE("Groove cuts retain each part's assigned color", "[Cut][PartColor]")
{
    Model model;
    model.add_material("red");
    model.add_material("blue");
    ModelObject* object = model.add_object();
    object->add_instance();
    add_colored_cube(*object, 0., 2, "red");
    add_colored_cube(*object, 30., 3, "blue");

    Cut::Groove groove;
    groove.depth       = 4.f;
    groove.width       = 10.f;
    groove.flaps_angle = float(PI / 4.);
    Cut                    cut(object, 0, Geometry::translation_transform(Vec3d(0., 0., 10.)),
                               ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs& results = cut.perform_with_groove(groove, Transform3d::Identity());
    REQUIRE(results.size() == 2);
    for (const ModelObject* result : results) {
        REQUIRE(result->volumes.size() == 2);
        bool found_red  = false;
        bool found_blue = false;
        for (const ModelVolume* volume : result->volumes) {
            REQUIRE(volume->config.has("extruder"));
            const bool red = volume->material_id() == "red";
            CHECK(volume->config.extruder() == (red ? 2 : 3));
            if (red)
                found_red = true;
            else {
                CHECK(volume->material_id() == "blue");
                found_blue = true;
            }
            const BoundingBoxf3 bounds = volume->mesh().transformed_bounding_box(result->instances[0]->get_matrix() * volume->get_matrix());
            CHECK(bounds.min.x() == Catch::Approx(red ? 0. : 30.).margin(1e-5));
            CHECK(bounds.max.x() == Catch::Approx(red ? 20. : 50.).margin(1e-5));
        }
        CHECK(found_red);
        CHECK(found_blue);
    }
}
