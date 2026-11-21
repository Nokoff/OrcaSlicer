#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/MeshSmoothing.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <set>

using namespace Slic3r;

SCENARIO("Reading 3mf file", "[3mf]") {
    GIVEN("umlauts in the path of the file") {
        Model model;
        WHEN("3mf model is read") {
        	std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
        	DynamicPrintConfig config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            bool ret = load_3mf(path.c_str(), config, ctxt, &model, false);
            THEN("load should succeed") {
                REQUIRE(ret);
            }
        }
    }
}

SCENARIO("Export+Import geometry to/from 3mf file cycle", "[3mf]") {
    GIVEN("world vertices coordinates before save") {
        // load a model from stl file
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &src_model);
        src_model.add_default_instances();

        ModelObject* src_object = src_model.objects.front();

        // apply generic transformation to the 1st volume
        Geometry::Transformation src_volume_transform;
        src_volume_transform.set_offset({ 10.0, 20.0, 0.0 });
        src_volume_transform.set_rotation({ Geometry::deg2rad(25.0), Geometry::deg2rad(35.0), Geometry::deg2rad(45.0) });
        src_volume_transform.set_scaling_factor({ 1.1, 1.2, 1.3 });
        src_volume_transform.set_mirror({ -1.0, 1.0, -1.0 });
        src_object->volumes.front()->set_transformation(src_volume_transform);

        // apply generic transformation to the 1st instance
        Geometry::Transformation src_instance_transform;
        src_instance_transform.set_offset({ 5.0, 10.0, 0.0 });
        src_instance_transform.set_rotation({ Geometry::deg2rad(12.0), Geometry::deg2rad(13.0), Geometry::deg2rad(14.0) });
        src_instance_transform.set_scaling_factor({ 0.9, 0.8, 0.7 });
        src_instance_transform.set_mirror({ 1.0, -1.0, -1.0 });
        src_object->instances.front()->set_transformation(src_instance_transform);

        WHEN("model is saved+loaded to/from 3mf file") {
            // save the model to 3mf file
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/prusa.3mf";
            store_3mf(test_file.c_str(), &src_model, nullptr, false);

            // load back the model from the 3mf file
            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false);
            }
            boost::filesystem::remove(test_file);

            // compare meshes
            TriangleMesh src_mesh = src_model.mesh();
            TriangleMesh dst_mesh = dst_model.mesh();

            bool res = src_mesh.its.vertices.size() == dst_mesh.its.vertices.size();
            if (res) {
                for (size_t i = 0; i < dst_mesh.its.vertices.size(); ++i) {
                    res &= dst_mesh.its.vertices[i].isApprox(src_mesh.its.vertices[i]);
                }
            }
            THEN("world vertices coordinates after load match") {
                REQUIRE(res);
            }
        }
    }
}

TEST_CASE("Plane-cut paint and cap filament survive a 3MF round trip", "[3mf][Cut][MMUPaint]")
{
    Model source;
    ModelObject* object = source.add_object();
    object->add_instance();
    ModelVolume* volume = object->add_volume(make_cube(20., 20., 20.));
    constexpr int exterior_filament = 33;
    constexpr int cap_filament = 34;
    TriangleSelector source_selector(volume->mesh());
    for (size_t face = 0; face < volume->mesh().its.indices.size(); ++face)
        source_selector.set_facet(static_cast<int>(face), static_cast<EnforcerBlockerType>(exterior_filament));
    REQUIRE(volume->mmu_segmentation_facets.set(source_selector));

    const Transform3d cut_transform = Geometry::translation_transform(Vec3d(10., 0., 0.)) * Eigen::AngleAxisd(PI / 2., Vec3d::UnitY());
    Cut cut(object, 0, cut_transform,
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    cut.set_new_face_filament(static_cast<int>(cap_filament));
    const ModelObjectPtrs& results = cut.perform_with_plane();
    REQUIRE(results.size() == 2);

    Model edited;
    edited.add_object(*results.front());
    const boost::filesystem::path output = boost::filesystem::temp_directory_path() /
                                           boost::filesystem::unique_path("orcaslicer-cut-paint-%%%%-%%%%.3mf");
    REQUIRE(store_3mf(output.string().c_str(), &edited, nullptr, false));

    Model loaded;
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    const bool loaded_ok = load_3mf(output.string().c_str(), config, substitutions, &loaded, false);
    boost::filesystem::remove(output);
    REQUIRE(loaded_ok);
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.objects.front()->volumes.size() == 1);

    const ModelVolume* loaded_volume = loaded.objects.front()->volumes.front();
    TriangleSelector loaded_selector(loaded_volume->mesh());
    loaded_selector.deserialize(loaded_volume->mmu_segmentation_facets.get_data(), false);
    CHECK(loaded_selector.num_facets(static_cast<EnforcerBlockerType>(exterior_filament)) > 0);
    CHECK(loaded_selector.num_facets(static_cast<EnforcerBlockerType>(cap_filament)) > 0);
}

TEST_CASE("Boolean exterior paint and cavity filament survive a 3MF round trip", "[3mf][MeshBoolean][MMUPaint]")
{
    TriangleMesh target = make_cube(20., 20., 20.);
    TriangleMesh cutter = make_cube(8., 8., 8.);
    constexpr int exterior_filament = 33;
    constexpr int cavity_filament = 34;
    TriangleSelector source_selector(target);
    for (size_t face = 0; face < target.its.indices.size(); ++face)
        source_selector.set_facet(static_cast<int>(face), static_cast<EnforcerBlockerType>(exterior_filament));
    const auto source_painting = source_selector.serialize();

    MeshBoolean::mcut::BooleanResult result;
    REQUIRE(MeshBoolean::mcut::make_boolean(target, cutter, result, "A_NOT_B"));
    REQUIRE_FALSE(result.mesh.empty());
    const auto remapped = TriangleSelector::remap_painting(target.its, source_painting, result.mesh.its,
                                                            Transform3d::Identity(), std::nullopt, Transform3d::Identity(),
                                                            &result.source_faces, &result.source_face_ids);
    TriangleSelector output_selector(result.mesh);
    output_selector.deserialize(remapped, false);
    for (size_t face = 0; face < result.source_faces.size(); ++face)
        if (!result.source_faces[face])
            output_selector.set_facet(static_cast<int>(face), static_cast<EnforcerBlockerType>(cavity_filament));

    Model edited;
    ModelObject* object = edited.add_object();
    object->add_instance();
    ModelVolume* volume = object->add_volume(TriangleMesh(result.mesh.its));
    REQUIRE(volume->mmu_segmentation_facets.set(output_selector));

    const boost::filesystem::path output = boost::filesystem::temp_directory_path() /
                                           boost::filesystem::unique_path("orcaslicer-boolean-paint-%%%%-%%%%.3mf");
    REQUIRE(store_3mf(output.string().c_str(), &edited, nullptr, false));
    Model loaded;
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    const bool loaded_ok = load_3mf(output.string().c_str(), config, substitutions, &loaded, false);
    boost::filesystem::remove(output);
    REQUIRE(loaded_ok);
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.objects.front()->volumes.size() == 1);

    const ModelVolume* loaded_volume = loaded.objects.front()->volumes.front();
    TriangleSelector loaded_selector(loaded_volume->mesh());
    loaded_selector.deserialize(loaded_volume->mmu_segmentation_facets.get_data(), false);
    CHECK(loaded_selector.num_facets(static_cast<EnforcerBlockerType>(exterior_filament)) > 0);
    CHECK(loaded_selector.num_facets(static_cast<EnforcerBlockerType>(cavity_filament)) > 0);
}

TEST_CASE("Smooth cut subtract save and reopen retains all filament annotations", "[3mf][MeshSmoothing][Cut][MeshBoolean][MMUPaint]")
{
    constexpr auto exterior_filament = static_cast<EnforcerBlockerType>(1);
    constexpr auto cap_filament = static_cast<EnforcerBlockerType>(2);
    constexpr auto cavity_filament = static_cast<EnforcerBlockerType>(3);

    Model working;
    ModelObject* object = working.add_object();
    object->add_instance();
    ModelVolume* volume = object->add_volume(make_cube(20., 20., 20.));
    TriangleSelector initial_selector(volume->mesh());
    for (size_t face = 0; face < volume->mesh().its.indices.size(); ++face)
        initial_selector.set_facet(static_cast<int>(face), exterior_filament);
    REQUIRE(volume->mmu_segmentation_facets.set(initial_selector));

    auto smoothed = volume->mesh().its;
    MeshSmoothingBrush smoothing(smoothed);
    REQUIRE(smoothing.apply(smoothed, 0, smoothed.vertices[0], Transform3d::Identity(), 100.f, 1.f));
    volume->set_mesh(TriangleMesh(std::move(smoothed)));
    CHECK(volume->mmu_segmentation_facets.get_data() == initial_selector.serialize());

    Cut cut(object, 0, Geometry::translation_transform(Vec3d(10., 0., 0.)) *
                             Eigen::AngleAxisd(PI / 2., Vec3d::UnitY()),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    cut.set_new_face_filament(static_cast<int>(cap_filament));
    const ModelObjectPtrs& cut_results = cut.perform_with_plane();
    REQUIRE(cut_results.size() == 2);
    REQUIRE_FALSE(cut_results.front()->volumes.empty());
    const ModelVolume* cut_volume = cut_results.front()->volumes.front();
    TriangleSelector cut_selector(cut_volume->mesh());
    cut_selector.deserialize(cut_volume->mmu_segmentation_facets.get_data(), false);
    REQUIRE(cut_selector.num_facets(exterior_filament) > 0);
    REQUIRE(cut_selector.num_facets(cap_filament) > 0);

    TriangleMesh boolean_source = cut_volume->mesh();
    const Transform3d source_transform = cut_volume->get_matrix();
    boolean_source.transform(source_transform, true);
    const BoundingBoxf3 bounds = boolean_source.bounding_box();
    Vec3f center = ((bounds.min + bounds.max) * 0.5).cast<float>();
    center.x() = float(bounds.min.x()) + 1.f;
    TriangleMesh cutter = make_sphere(2., PI / 12.);
    cutter.translate(center);
    MeshBoolean::mcut::BooleanResult boolean_result;
    REQUIRE(MeshBoolean::mcut::make_boolean(boolean_source, cutter, boolean_result, "A_NOT_B"));
    REQUIRE_FALSE(boolean_result.mesh.empty());
    const Vec3d result_center = boolean_result.mesh.bounding_box().center();
    TriangleMesh display_mesh = boolean_result.mesh;
    display_mesh.transform(Geometry::translation_transform(-result_center), true);
    const Transform3d target_transform = Geometry::translation_transform(result_center);
    const auto boolean_painting = TriangleSelector::remap_painting(
        cut_volume->mesh().its, cut_volume->mmu_segmentation_facets.get_data(), display_mesh.its,
        target_transform, std::nullopt, source_transform, &boolean_result.source_faces, &boolean_result.source_face_ids);
    TriangleSelector boolean_selector(display_mesh);
    boolean_selector.deserialize(boolean_painting, false);
    CAPTURE(boolean_selector.num_facets(exterior_filament), boolean_selector.num_facets(cap_filament),
            std::count(boolean_result.source_faces.begin(), boolean_result.source_faces.end(), uint8_t(1)),
            boolean_result.mesh.its.indices.size());
    CHECK(boolean_selector.num_facets(exterior_filament) > 0);
    CHECK(boolean_selector.num_facets(cap_filament) > 0);
    for (size_t face = 0; face < boolean_result.source_faces.size(); ++face)
        if (!boolean_result.source_faces[face])
            boolean_selector.set_facet(static_cast<int>(face), cavity_filament);

    Model edited;
    ModelObject* edited_object = edited.add_object();
    edited_object->add_instance();
    ModelVolume* edited_volume = edited_object->add_volume(std::move(display_mesh));
    edited_volume->config.set("extruder", 1);
    REQUIRE(edited_volume->mmu_segmentation_facets.set(boolean_selector));
    const boost::filesystem::path output = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("orcaslicer-mesh-edit-chain-%%%%-%%%%.3mf");
    REQUIRE(store_3mf(output.string().c_str(), &edited, nullptr, false));

    Model loaded;
    DynamicPrintConfig config;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    const bool loaded_ok = load_3mf(output.string().c_str(), config, substitutions, &loaded, false);
    boost::filesystem::remove(output);
    REQUIRE(loaded_ok);
    REQUIRE(loaded.objects.size() == 1);
    REQUIRE(loaded.objects.front()->volumes.size() == 1);
    TriangleSelector loaded_selector(loaded.objects.front()->volumes.front()->mesh());
    loaded_selector.deserialize(loaded.objects.front()->volumes.front()->mmu_segmentation_facets.get_data(), false);
    CHECK(loaded_selector.num_facets(exterior_filament) > 0);
    CHECK(loaded_selector.num_facets(cap_filament) > 0);
    CHECK(loaded_selector.num_facets(cavity_filament) > 0);

    const FullPrintConfig& defaults = FullPrintConfig::defaults();
    DynamicPrintConfig print_config;
    print_config.apply(static_cast<const PrintObjectConfig&>(defaults), true);
    print_config.apply(static_cast<const PrintRegionConfig&>(defaults), true);
    print_config.apply(static_cast<const PrintConfig&>(defaults), true);
    print_config.set_num_extruders(4);
    print_config.set_num_filaments(4);
    print_config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.76, 1.77, 1.78};
    print_config.option<ConfigOptionFloats>("nozzle_diameter")->values = {0.4, 0.4, 0.4, 0.4};
    print_config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    print_config.set("enable_prime_tower", false);
    print_config.set("layer_height", 0.2);
    print_config.set("line_width", 0.45);
    print_config.set("outer_wall_line_width", 0.45);
    print_config.set("inner_wall_line_width", 0.45);
    print_config.set("sparse_infill_line_width", 0.45);
    print_config.set("internal_solid_infill_line_width", 0.45);
    print_config.set("top_surface_line_width", 0.45);
    print_config.set("initial_layer_line_width", 0.45);
    Print print;
    print.set_status_silent();
    print.apply(loaded, print_config);
    print_config = print.full_print_config();
    print.validate();
    print.process();
    REQUIRE(print.objects().size() == 1);
    const PrintObjectRegions* regions = print.objects().front()->shared_regions();
    REQUIRE(regions != nullptr);
    REQUIRE_FALSE(regions->layer_ranges.empty());
    std::set<unsigned int> sliced_filaments;
    for (const PrintObjectRegions::LayerRangeRegions& range : regions->layer_ranges)
        for (const PrintObjectRegions::PaintedRegion& painted : range.painted_regions)
            sliced_filaments.insert(painted.extruder_id);
    CHECK(sliced_filaments.count(static_cast<unsigned int>(exterior_filament)) == 1);
    CHECK(sliced_filaments.count(static_cast<unsigned int>(cap_filament)) == 1);
    CHECK(sliced_filaments.count(static_cast<unsigned int>(cavity_filament)) == 1);

    const boost::filesystem::path gcode_template = boost::filesystem::current_path() /
                                                   boost::filesystem::unique_path("mesh-edit-toolpath-%%%%");
    GCodeProcessorResult toolpath_result;
    const std::string gcode_path = print.export_gcode(gcode_template.string(), &toolpath_result, nullptr);
    ScopeGuard cleanup_gcode([&gcode_path]() {
        if (boost::filesystem::is_regular_file(gcode_path))
            boost::filesystem::remove(gcode_path);
    });
    REQUIRE(boost::filesystem::is_regular_file(gcode_path));
    std::set<unsigned char> extrusion_tools;
    for (const GCodeProcessorResult::MoveVertex& move : toolpath_result.moves)
        if (move.type == EMoveType::Extrude && move.delta_extruder > 0.f)
            extrusion_tools.insert(move.extruder_id);
    std::string extrusion_tool_ids;
    for (unsigned char tool : extrusion_tools)
        extrusion_tool_ids += (extrusion_tool_ids.empty() ? "" : ",") + std::to_string(unsigned(tool));
    std::string configured_tool_ids;
    for (unsigned int tool : print.extruders())
        configured_tool_ids += (configured_tool_ids.empty() ? "" : ",") + std::to_string(tool);
    CAPTURE(extrusion_tool_ids, configured_tool_ids, gcode_path);
    CHECK(extrusion_tools.size() >= 3);
}

SCENARIO("2D convex hull of sinking object", "[3mf]") {
    GIVEN("model") {
        // load a model
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &model);
        model.add_default_instances();

        WHEN("model is rotated, scaled and set as sinking") {
            ModelObject* object = model.objects.front();
            object->center_around_origin(false);

            // set instance's attitude so that it is rotated, scaled and sinking
            ModelInstance* instance = object->instances.front();
            instance->set_rotation(X, -M_PI / 4.0);
            instance->set_offset(Vec3d::Zero());
            instance->set_scaling_factor({ 2.0, 2.0, 2.0 });

            // calculate 2D convex hull
            Polygon hull_2d = object->convex_hull_2d(instance->get_transformation().get_matrix());

            // verify result
            Points result = {
                { -91501495, -15914144 },
                { 91501495, -15914144 },
                { 91501495, 13792823 },
                { 34846496, 14717717 },
                { -85501495, 13917981 },
                { -91501495, 13792823 }
            };

            // Allow 1um error due to floating point rounding.
            bool res = hull_2d.points.size() == result.size();
            if (res)
                for (size_t i = 0; i < result.size(); ++ i) {
                    const Point &p1 = result[i];
                    const Point &p2 = hull_2d.points[i];
                    if (std::abs(p1.x() - p2.x()) > 1 || std::abs(p1.y() - p2.y()) > 1) {
                        res = false;
                        break;
                    }
                }
            THEN("2D convex hull should match with reference") {
                REQUIRE(res);
            }
        }
    }
}

