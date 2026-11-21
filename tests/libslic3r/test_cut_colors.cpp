#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/CutUtils.hpp"
#include "libslic3r/CutSurface.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/Utils/ModelVolumeUtils.hpp"

using namespace Slic3r;

TEST_CASE("Text rows resolve model volumes independently of hidden connectors", "[Cut][Emboss]")
{
    Model        model;
    ModelObject* cut_object = model.add_object();
    cut_object->cut_id.init();
    cut_object->add_volume(make_cube(20., 20., 10.));
    for (ModelVolumeType type : {ModelVolumeType::MODEL_PART, ModelVolumeType::NEGATIVE_VOLUME}) {
        ModelVolume* connector = cut_object->add_volume(make_cube(2., 2., 2.), type);
        connector->cut_info    = ModelVolume::CutInfo(CutConnectorType::Plug, 0.1f, 0.2f, true);
        connector->config.set_key_value("extruder", new ConfigOptionInt(1));
        CHECK_FALSE(GUI::is_emboss_surface(*connector));
    }
    ModelVolume* text         = cut_object->add_volume(make_cube(5., 2., 1.));
    text->text_configuration  = TextConfiguration{};
    ModelObject* plain_object = model.add_object();
    plain_object->add_volume(make_cube(10., 10., 10.));
    plain_object->add_volume(make_cube(5., 5., 5.));

    CHECK(GUI::model_volume_index(*cut_object, 1) == 3);
    CHECK(GUI::model_volume_index(*plain_object, 1) == 1);
    const int text_index = GUI::model_volume_index(*cut_object, 1);
    REQUIRE(text_index >= 0);
    CHECK(cut_object->volumes[text_index] == text);
    cut_object->volumes[text_index]->config.set_key_value("extruder", new ConfigOptionInt(2));
    CHECK(text->config.extruder() == 2);
    CHECK(cut_object->volumes[1]->config.extruder() == 1);
    CHECK(cut_object->volumes[2]->config.extruder() == 1);
    CHECK(GUI::visible_volume_index(*cut_object, text_index) == 1);
    CHECK(GUI::visible_volume_index(*cut_object, 1) == -1);
    CHECK(GUI::model_volume_index(*cut_object, -1) == -1);
    CHECK(GUI::model_volume_index(*cut_object, 2) == -1);
    CHECK(GUI::visible_volume_index(*cut_object, 4) == -1);
    CHECK(GUI::is_emboss_surface(*cut_object->volumes[0]));

    SECTION("Deleting text preserves connector rows and mappings")
    {
        cut_object->delete_volume(text_index);
        CHECK(GUI::model_volume_index(*cut_object, 1) == -1);
        CHECK(cut_object->volumes[1]->is_cut_connector());
        CHECK(cut_object->volumes[2]->is_cut_connector());
    }
    SECTION("Invalidating cut info exposes the original volume indices")
    {
        cut_object->invalidate_cut();
        CHECK(GUI::model_volume_index(*cut_object, 1) == 1);
        CHECK(GUI::visible_volume_index(*cut_object, 3) == 3);
    }
}

TEST_CASE("Emboss projection preserves surfaces across offset and scaled volumes", "[Cut][Emboss]")
{
    const Transform3d reference = Geometry::translation_transform(Vec3d(30., -20., -70.)) * Eigen::AngleAxisd(0.4, Vec3d::UnitZ()) *
                                  Eigen::Scaling(1.5, 0.8, 2.);
    const Transform3d source   = reference * Geometry::translation_transform(Vec3d(0., 0., 3.));
    const Transform3d relative = GUI::volume_to_volume_transform(source, reference);
    const Vec3d       local_point(2., 4., 9.);
    CHECK((reference * (relative * local_point)).isApprox(source * local_point));

    indexed_triangle_set sphere = make_sphere(10.).its;
    its_transform(sphere, relative);
    const ExPolygons footprint{ExPolygon(Polygon(Points{Point(scale_(-3.), scale_(-2.)), Point(scale_(3.), scale_(-2.)),
                                                        Point(scale_(3.), scale_(2.)), Point(scale_(-3.), scale_(2.))}))};
    Transform3d      projection_matrix = Geometry::translation_transform(Vec3d(0., 0., -11.));
    projection_matrix.scale(SCALING_FACTOR);
    const SurfaceCut surface = cut_surface(footprint, {sphere}, Emboss::OrthoProject(projection_matrix, Vec3d(0., 0., 27.)), 0.95f);
    REQUIRE_FALSE(surface.empty());
    CHECK(bounding_box(surface).min.z() > 12.);
    CHECK(bounding_box(surface).max.z() == Catch::Approx(13.));
}

TEST_CASE("Surface placement fallback queries transformed volumes in local coordinates", "[Cut][Emboss]")
{
    const TriangleMesh cube = make_cube(10., 10., 10.);
    const AABBMesh     mesh(cube);
    const Transform3d  transform = Geometry::translation_transform(Vec3d(100., 200., 30.)) * Eigen::AngleAxisd(0.4, Vec3d::UnitZ()) *
                                  Eigen::Scaling(2., 3., 4.);
    const GUI::VolumeClosestPoint closest = GUI::closest_point_on_volume(mesh, transform, transform * Vec3d(5., 5., 12.));
    CHECK(closest.point.isApprox(Vec3d(5., 5., 10.)));
    CHECK(closest.squared_distance == Catch::Approx(64.));
}

TEST_CASE("Separate embossed text preserves cut correspondence and connectors", "[Cut][Emboss]")
{
    ModelVolumeType type = ModelVolumeType::MODEL_PART;
    SECTION("Raised text") { type = ModelVolumeType::MODEL_PART; }
    SECTION("Engraved text") { type = ModelVolumeType::NEGATIVE_VOLUME; }
    SECTION("Text modifier") { type = ModelVolumeType::PARAMETER_MODIFIER; }

    Model        model;
    ModelObject* upper = model.add_object();
    upper->add_instance();
    upper->add_volume(make_sphere(10.));
    ModelVolume* connector = upper->add_volume(make_cube(3., 3., 4.), ModelVolumeType::NEGATIVE_VOLUME);
    connector->cut_info    = ModelVolume::CutInfo(CutConnectorType::Plug, 0.1f, 0.2f, true);
    connector->set_offset(Vec3d(5., 5., 8.));
    upper->cut_id.init();
    upper->cut_id.set_check_sum(2);
    upper->cut_id.increase_connectors_cnt(1);
    ModelObject* lower = model.add_object(*upper);
    lower->volumes[1]->set_type(ModelVolumeType::MODEL_PART);

    const Model original(model);
    auto        check_cut = [&original](const Model& current) {
        REQUIRE(current.objects.size() == original.objects.size());
        for (size_t i = 0; i < current.objects.size(); ++i) {
            const ModelObject& object = *current.objects[i];
            const ModelObject& before = *original.objects[i];
            CHECK(object.is_cut());
            CHECK(object.cut_id.id() == before.cut_id.id());
            CHECK(object.cut_id.check_sum() == before.cut_id.check_sum());
            CHECK(object.cut_id.connectors_cnt() == before.cut_id.connectors_cnt());
            CHECK(object.has_connectors());
            REQUIRE(object.volumes.size() >= before.volumes.size());
            for (size_t j = 0; j < before.volumes.size(); ++j) {
                const ModelVolume& volume = *object.volumes[j];
                const ModelVolume& saved  = *before.volumes[j];
                CHECK(volume.id() == saved.id());
                CHECK(volume.mesh_ptr() == saved.mesh_ptr());
                CHECK(volume.get_matrix().isApprox(saved.get_matrix()));
                CHECK(volume.type() == saved.type());
                CHECK(volume.is_cut_connector() == saved.is_cut_connector());
                CHECK(volume.cut_info.radius_tolerance == saved.cut_info.radius_tolerance);
                CHECK(volume.cut_info.height_tolerance == saved.cut_info.height_tolerance);
            }
        }
    };

    // Exercise the Use Surface geometry path with a deterministic glyph footprint.
    const ExPolygons footprint{ExPolygon(Polygon(Points{Point(scale_(-3.), scale_(-2.)), Point(scale_(3.), scale_(-2.)),
                                                        Point(scale_(3.), scale_(2.)), Point(scale_(-3.), scale_(2.))}))};
    Transform3d      projection_matrix = Geometry::translation_transform(Vec3d(0., 0., -11.));
    projection_matrix.scale(SCALING_FACTOR);
    const Emboss::OrthoProject projection(projection_matrix, Vec3d(0., 0., 22.));
    const SurfaceCut           surface = cut_surface(footprint, {upper->volumes.front()->mesh().its}, projection, 0.95f);
    REQUIRE_FALSE(surface.empty());
    const BoundingBoxf3 surface_bounds = bounding_box(surface);
    CHECK(surface_bounds.min.z() > 9.);
    CHECK(surface_bounds.size().z() > 0.2);
    const indexed_triangle_set embossed = cut2model(surface, Emboss::OrthoProject3d(Vec3d(0., 0., 1.)));
    REQUIRE_FALSE(embossed.empty());
    CHECK(bounding_box(embossed).size().z() == Catch::Approx(surface_bounds.size().z() + 1.));

    ModelVolume* text                          = upper->add_volume(TriangleMesh(embossed), type);
    text->text_configuration                   = TextConfiguration{};
    text->text_configuration->text             = "Assembly label";
    text->emboss_shape                         = EmbossShape{};
    text->emboss_shape->projection.use_surface = true;
    CHECK_FALSE(text->is_the_only_one_part());
    CHECK_FALSE(text->is_cut_connector());
    check_cut(model);

    text->set_mesh(make_cube(8., 2., 1.));
    text->set_offset(Vec3d(2., 3., 9.));
    text->text_configuration->text = "Edited label";
    check_cut(model);
    // Undo/redo snapshots retain the cut relationship alongside editable text.
    check_cut(Model(model));

    upper->delete_volume(upper->volumes.size() - 1);
    CHECK(upper->volumes.size() == original.objects[0]->volumes.size());
    check_cut(model);
}

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

TEST_CASE("Coincident cut volumes restore paint only from their own source", "[Cut][MMUPaint]")
{
    Model model;
    ModelObject* object = model.add_object();
    object->add_instance();
    ModelVolume* first = add_colored_cube(*object, 0., 2, "same");
    ModelVolume* second = add_colored_cube(*object, 0., 2, "same");
    first->name = "paint-source-first";
    second->name = "paint-source-second";

    const auto paint_face = [](ModelVolume* volume, EnforcerBlockerType state) {
        TriangleSelector selector(volume->mesh());
        for (size_t face = 0; face < volume->mesh().its.indices.size(); ++face)
            selector.set_facet(static_cast<int>(face), state);
        REQUIRE(volume->mmu_segmentation_facets.set(selector));
    };
    const auto first_state = static_cast<EnforcerBlockerType>(33);
    const auto second_state = static_cast<EnforcerBlockerType>(34);
    paint_face(first, first_state);
    paint_face(second, second_state);

    Cut cut(object, 0, Geometry::translation_transform(Vec3d(0., 0., 10.)),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs& results = cut.perform_with_plane();
    REQUIRE(results.size() == 2);
    for (const ModelObject* result : results) {
        REQUIRE(result->volumes.size() == 2);
        for (const ModelVolume* volume : result->volumes) {
            const bool first_source = volume->name.find("first") != std::string::npos;
            REQUIRE((first_source || volume->name.find("second") != std::string::npos));
            const auto own_state = first_source ? first_state : second_state;
            const auto other_state = first_source ? second_state : first_state;
            CHECK(TriangleSelector::has_facets(volume->mmu_segmentation_facets.get_data(), own_state));
            CHECK_FALSE(TriangleSelector::has_facets(volume->mmu_segmentation_facets.get_data(), other_state));
        }
    }
}

TEST_CASE("Repeated plane cuts retain previous paint and each generated cap filament", "[Cut][MMUPaint]")
{
    Model model;
    ModelObject* object = model.add_object();
    object->add_instance();
    ModelVolume* volume = object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::MODEL_PART, false);

    const auto paint_state = static_cast<EnforcerBlockerType>(33);
    TriangleSelector source_selector(volume->mesh());
    for (size_t face = 0; face < volume->mesh().its.indices.size(); ++face)
        source_selector.set_facet(static_cast<int>(face), paint_state);
    REQUIRE(volume->mmu_segmentation_facets.set(source_selector));

    Cut first_cut(object, 0, Geometry::translation_transform(Vec3d(0., 0., 10.)),
                  ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    constexpr int first_cap_filament = 34;
    first_cut.set_new_face_filament(first_cap_filament);
    const ModelObjectPtrs& first_results = first_cut.perform_with_plane();
    REQUIRE(first_results.size() == 2);

    // Cut the lower half again. Its z=10 cap must survive as painted source
    // geometry while the second operation creates a separate z=5 cap.
    Cut second_cut(first_results.back(), 0, Geometry::translation_transform(Vec3d(0., 0., 5.)),
                  ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    constexpr int second_cap_filament = 35;
    second_cut.set_new_face_filament(second_cap_filament);
    const ModelObjectPtrs& second_results = second_cut.perform_with_plane();
    REQUIRE(second_results.size() == 2);

    const ModelVolume* retained = second_results.front()->volumes.front();
    TriangleSelector result_selector(retained->mesh());
    result_selector.deserialize(retained->mmu_segmentation_facets.get_data(), false);
    CHECK(result_selector.num_facets(paint_state) > 0);
    CHECK(result_selector.num_facets(static_cast<EnforcerBlockerType>(first_cap_filament)) > 0);
    CHECK(result_selector.num_facets(static_cast<EnforcerBlockerType>(second_cap_filament)) > 0);
}

TEST_CASE("Plane cuts preserve painted source volumes when connectors are present", "[Cut][MMUPaint]")
{
    Model model;
    ModelObject* object = model.add_object();
    object->add_instance();
    ModelVolume* source = object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::MODEL_PART, false);
    source->name = "painted source";
    TriangleSelector source_selector(source->mesh());
    const auto source_filament = static_cast<EnforcerBlockerType>(33);
    for (size_t face = 0; face < source->mesh().its.indices.size(); ++face)
        source_selector.set_facet(static_cast<int>(face), source_filament);
    REQUIRE(source->mmu_segmentation_facets.set(source_selector));

    object->cut_id.init();
    ModelVolume* connector = object->add_volume(make_cube(3., 3., 3.), ModelVolumeType::NEGATIVE_VOLUME, false);
    connector->name = "cut connector";
    connector->set_offset(Vec3d(5., 5., 8.));
    connector->cut_info = ModelVolume::CutInfo(CutConnectorType::Plug, 0.1f, 0.2f, true);
    object->cut_id.increase_connectors_cnt(1);

    Cut cut(object, 0, Geometry::translation_transform(Vec3d(0., 0., 10.)),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs& results = cut.perform_with_plane();
    REQUIRE(results.size() == 2);
    for (const ModelObject* result : results) {
        const auto painted_source = std::find_if(result->volumes.begin(), result->volumes.end(), [](const ModelVolume* volume) {
            return volume->name == "painted source";
        });
        REQUIRE(painted_source != result->volumes.end());
        CHECK(TriangleSelector::has_facets((*painted_source)->mmu_segmentation_facets.get_data(), source_filament));
        CHECK(std::any_of(result->volumes.begin(), result->volumes.end(), [](const ModelVolume* volume) {
            return volume->is_cut_connector();
        }));
    }
}

TEST_CASE("Plane cuts assign the selected filament only to generated caps", "[Cut][MMUPaint]")
{
    Model model;
    ModelObject* object = model.add_object();
    object->add_instance();
    ModelVolume* volume = add_colored_cube(*object, 0., 2, "red");
    TriangleSelector source_selector(volume->mesh());
    for (size_t face = 0; face < volume->mesh().its.indices.size(); ++face)
        source_selector.set_facet(static_cast<int>(face), static_cast<EnforcerBlockerType>(33));
    REQUIRE(volume->mmu_segmentation_facets.set(source_selector));
    for (FacetsAnnotation* annotation : {&volume->supported_facets, &volume->seam_facets, &volume->fuzzy_skin_facets}) {
        TriangleSelector selector(volume->mesh());
        selector.set_facet(0, EnforcerBlockerType::ENFORCER);
        REQUIRE(annotation->set(selector));
    }

    Cut cut(object, 0, Geometry::translation_transform(Vec3d(0., 0., 10.)),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    constexpr int cap_filament = 34;
    cut.set_new_face_filament(cap_filament);
    const ModelObjectPtrs& results = cut.perform_with_plane();
    REQUIRE(results.size() == 2);
    for (const ModelObject* result : results) {
        REQUIRE(result->volumes.size() == 1);
        const ModelVolume* cut_volume = result->volumes.front();
        TriangleSelector  selector(cut_volume->mesh());
        selector.deserialize(cut_volume->mmu_segmentation_facets.get_data(), false);
        const indexed_triangle_set cap = selector.get_facets(static_cast<EnforcerBlockerType>(cap_filament));
        REQUIRE_FALSE(cap.indices.empty());
        for (const Vec3i32& face : cap.indices)
            for (int corner = 0; corner < 3; ++corner)
                CHECK((cut_volume->get_matrix() * cap.vertices[face[corner]].cast<double>()).z() == Catch::Approx(10.).margin(1e-5));
        CHECK(TriangleSelector::has_facets(cut_volume->mmu_segmentation_facets.get_data(), static_cast<EnforcerBlockerType>(33)));

        for (const FacetsAnnotation* annotation : {&cut_volume->supported_facets, &cut_volume->seam_facets, &cut_volume->fuzzy_skin_facets}) {
            TriangleSelector restored(cut_volume->mesh());
            restored.deserialize(annotation->get_data(), false);
            const indexed_triangle_set marked = restored.get_facets(EnforcerBlockerType::ENFORCER);
            for (const Vec3i32& face : marked.indices) {
                bool on_cap = true;
                for (int corner = 0; corner < 3; ++corner)
                    on_cap &= (cut_volume->get_matrix() * marked.vertices[face[corner]].cast<double>()).z() == Catch::Approx(10.).margin(1e-5);
                CHECK_FALSE(on_cap);
            }
        }
    }
}

TEST_CASE("Plane cuts retain paint through offset, rotated, non-uniformly scaled volumes", "[Cut][MMUPaint]")
{
    Model model;
    ModelObject* object = model.add_object();
    object->add_instance();
    ModelVolume* volume = object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::MODEL_PART, false);
    volume->set_offset(Vec3d(8., -3., 0.));
    volume->set_rotation(Vec3d(0., 0., 0.35));
    volume->set_scaling_factor(Vec3d(1.4, 0.8, 1.0));

    const auto painted_filament = static_cast<EnforcerBlockerType>(33);
    TriangleSelector source_selector(volume->mesh());
    for (size_t face = 0; face < volume->mesh().its.indices.size(); ++face)
        source_selector.set_facet(static_cast<int>(face), painted_filament);
    REQUIRE(volume->mmu_segmentation_facets.set(source_selector));

    Cut cut(object, 0, Geometry::translation_transform(Vec3d(0., 0., 10.)),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs& results = cut.perform_with_plane();
    REQUIRE(results.size() == 2);
    for (const ModelObject* result : results) {
        REQUIRE(result->volumes.size() == 1);
        const ModelVolume* cut_volume = result->volumes.front();
        CHECK(TriangleSelector::has_facets(cut_volume->mmu_segmentation_facets.get_data(), painted_filament));
        TriangleSelector restored(cut_volume->mesh());
        restored.deserialize(cut_volume->mmu_segmentation_facets.get_data());
        CHECK(restored.has_facets(EnforcerBlockerType::NONE));
    }
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
