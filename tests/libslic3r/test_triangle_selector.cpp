#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>

#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

TEST_CASE("Triangle selector round-trips painted states above sixteen", "[TriangleSelector][MMUPaint]")
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(1.f, 0.f, 0.f),
        Vec3f(0.f, 1.f, 0.f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
    };

    TriangleMesh mesh(its);
    TriangleSelector selector(mesh);

    constexpr int painted_state = 120;
    selector.set_facet(0, static_cast<EnforcerBlockerType>(painted_state));

    auto data = selector.serialize();
    REQUIRE_FALSE(data.triangles_to_split.empty());
    REQUIRE(data.used_states.size() > painted_state);
    CHECK(data.used_states[painted_state]);

    data.reset_used_states();
    data.update_used_states(size_t(data.triangles_to_split.front().bitstream_start_idx));
    CHECK(data.used_states[painted_state]);
    CHECK(TriangleSelector::has_facets(data, static_cast<EnforcerBlockerType>(painted_state)));

    TriangleSelector restored(mesh);
    restored.deserialize(data, true, static_cast<EnforcerBlockerType>(painted_state));
    CHECK(restored.has_facets(static_cast<EnforcerBlockerType>(painted_state)));
}

TEST_CASE("Magic fill recolors only the connected area with the clicked color", "[TriangleSelector][MMUPaint][MagicFill]")
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(1.f, 0.f, 0.f),
        Vec3f(0.f, 1.f, 0.f),
        Vec3f(1.f, 1.f, 0.f),
        Vec3f(2.f, 0.f, 0.f),
        Vec3f(2.f, 1.f, 0.f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
        stl_triangle_vertex_indices(2, 1, 3),
        stl_triangle_vertex_indices(1, 4, 3),
        stl_triangle_vertex_indices(3, 4, 5),
    };

    TriangleSelector selector{TriangleMesh(its)};
    constexpr auto source_color   = static_cast<EnforcerBlockerType>(2);
    constexpr auto boundary_color = static_cast<EnforcerBlockerType>(3);
    constexpr auto replacement    = static_cast<EnforcerBlockerType>(4);

    selector.set_facet(0, source_color);
    selector.set_facet(1, source_color);
    selector.set_facet(2, boundary_color);
    selector.set_facet(3, source_color);

    selector.bucket_fill_select_triangles(Vec3f(1.f / 3.f, 1.f / 3.f, 0.f), 0, TriangleSelector::ClippingPlane(), -1.f, true, true);
    selector.seed_fill_apply_on_triangles(replacement);

    CHECK(selector.num_facets(replacement) == 2);
    CHECK(selector.num_facets(boundary_color) == 1);
    CHECK(selector.num_facets(source_color) == 1);
}

static indexed_triangle_set single_triangle()
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(1.f, 0.f, 0.f),
        Vec3f(0.f, 1.f, 0.f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
    };
    return its;
}

TEST_CASE("OBJ face colors preserve filament IDs above sixteen", "[OBJ][MMUPaint]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(TriangleMesh(single_triangle()));

    constexpr unsigned char painted_state = 33;
    REQUIRE(Model::obj_import_face_color_deal({painted_state}, painted_state, &model));
    CHECK(TriangleSelector::has_facets(volume->mmu_segmentation_facets.get_data(), static_cast<EnforcerBlockerType>(painted_state)));
}

TEST_CASE("OBJ vertex colors preserve multiple filament IDs above sixteen", "[OBJ][MMUPaint]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(TriangleMesh(single_triangle()));

    const std::vector<unsigned char> painted_states {17, 33, 64};
    REQUIRE(Model::obj_import_vertex_color_deal(painted_states, painted_states.front(), &model));
    for (const unsigned char painted_state : painted_states) {
        CHECK(TriangleSelector::has_facets(volume->mmu_segmentation_facets.get_data(), static_cast<EnforcerBlockerType>(painted_state)));
    }
}

TEST_CASE("Planar cuts with connectors preserve painted colors", "[Cut][MMUPaint]")
{
    Model model;
    model.add_material("painted-material");

    ModelObject *object = model.add_object();
    ModelVolume *painted_volume = object->add_volume(make_cube(20., 20., 20.));
    painted_volume->set_material_id("painted-material");
    object->add_instance();

    constexpr int painted_state = 33;
    TriangleSelector selector(painted_volume->mesh());
    // This side facet crosses the cut plane, so both resulting solids must retain it.
    selector.set_facet(4, static_cast<EnforcerBlockerType>(painted_state));
    REQUIRE(painted_volume->mmu_segmentation_facets.set(selector));

    ModelVolume *connector = object->add_volume(make_cube(4., 4., 4.));
    connector->set_material_id("painted-material");
    connector->set_type(ModelVolumeType::NEGATIVE_VOLUME);
    connector->cut_info = ModelVolume::CutInfo(CutConnectorType::Plug, 0.f, 0.f, false);

    const ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                                ModelObjectCutAttribute::KeepPaint;
    Cut cut(object, 0, Geometry::translation_transform(Vec3d(0., 0., 10.)), attributes);
    const ModelObjectPtrs &cut_objects = cut.perform_with_plane();

    REQUIRE(cut_objects.size() == 2);
    for (const ModelObject *cut_object : cut_objects) {
        bool found_painted_solid = false;
        for (const ModelVolume *volume : cut_object->volumes) {
            if (volume->is_cut_connector()) {
                CHECK(volume->mmu_segmentation_facets.empty());
            } else if (volume->is_model_part()) {
                found_painted_solid = true;
                CHECK(TriangleSelector::has_facets(volume->mmu_segmentation_facets.get_data(),
                                                   static_cast<EnforcerBlockerType>(painted_state)));
            }
        }
        CHECK(found_painted_solid);
    }
}

static TriangleSelector::TriangleSplittingData painted_corner_data(int facet, int corner)
{
    TriangleSelector::TriangleSplittingData data;
    data.triangles_to_split.emplace_back(facet, 0);
    // A three-edge split produces three corner triangles and one center triangle.
    // Children are serialized in reverse order; state 2 fits in one nibble.
    auto append_nibble = [&data](int value) {
        for (int bit = 0; bit < 4; ++bit)
            data.bitstream.push_back((value & (1 << bit)) != 0);
    };
    append_nibble(3);
    for (int child = 3; child >= 0; --child)
        append_nibble(child == corner ? 8 : 0);
    data.update_used_states(0);
    return data;
}

static bool contains_surface_point(const indexed_triangle_set& facets, const Vec3f& point)
{
    for (const Vec3i32& face : facets.indices) {
        const Vec3f& a      = facets.vertices[face[0]];
        const Vec3f  ab     = facets.vertices[face[1]] - a;
        const Vec3f  ac     = facets.vertices[face[2]] - a;
        const Vec3f  normal = ab.cross(ac);
        if (normal.squaredNorm() == 0.f || std::abs((point - a).dot(normal.normalized())) > 1e-4f)
            continue;
        const Vec3f offset      = point - a;
        const float denominator = ab.squaredNorm() * ac.squaredNorm() - ab.dot(ac) * ab.dot(ac);
        const float u           = (offset.dot(ab) * ac.squaredNorm() - offset.dot(ac) * ab.dot(ac)) / denominator;
        const float v           = (offset.dot(ac) * ab.squaredNorm() - offset.dot(ab) * ab.dot(ac)) / denominator;
        if (u >= -1e-5f && v >= -1e-5f && u + v <= 1.f + 1e-5f)
            return true;
    }
    return false;
}

TEST_CASE("Painting remap retains a painted corner sharing its parent's edges", "[TriangleSelector][MMUPaint][Remap]")
{
    const TriangleMesh         mesh(single_triangle());
    const auto                 painted_state = static_cast<EnforcerBlockerType>(2);
    const std::array<Vec3f, 3> corner_points{{Vec3f(0.1f, 0.1f, 0.f), Vec3f(0.8f, 0.1f, 0.f), Vec3f(0.1f, 0.8f, 0.f)}};

    for (int corner = 0; corner < 3; ++corner) {
        CAPTURE(corner);
        const auto remapped = TriangleSelector::remap_painting(mesh.its, painted_corner_data(0, corner), mesh.its, Transform3d::Identity(),
                                                               std::nullopt);
        TriangleSelector restored(mesh);
        restored.deserialize(remapped);
        const indexed_triangle_set painted = restored.get_facets(painted_state);
        REQUIRE_FALSE(painted.indices.empty());
        for (int point = 0; point < 3; ++point)
            CHECK(contains_surface_point(painted, corner_points[point]) == (point == corner));
        CHECK_FALSE(contains_surface_point(painted, Vec3f(1.f / 3.f, 1.f / 3.f, 0.f)));
    }
}

TEST_CASE("Painting remap does not color a neighboring face that only shares an edge", "[TriangleSelector][MMUPaint][Remap]")
{
    const TriangleMesh   source(single_triangle());
    indexed_triangle_set neighbor;
    neighbor.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(0.f, -1.f, 0.f), Vec3f(1.f, 0.f, 0.f)};
    neighbor.indices  = {Vec3i32(0, 1, 2)};
    // Both triangles face +Z, so normals alone cannot reject this neighbor.
    const auto       remapped = TriangleSelector::remap_painting(source.its, painted_corner_data(0, 0), neighbor, Transform3d::Identity(),
                                                                 std::nullopt);
    TriangleMesh     target(neighbor);
    TriangleSelector restored(target);
    restored.deserialize(remapped);
    CHECK_FALSE(restored.has_facets(static_cast<EnforcerBlockerType>(2)));
}

TEST_CASE("Painting remap does not project color onto a shallow angled cut cap", "[TriangleSelector][MMUPaint][Remap][CutCap]")
{
    indexed_triangle_set source_its;
    source_its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(20.f, 0.f, 1.f), Vec3f(0.f, 20.f, 0.f)};
    source_its.indices  = {Vec3i32(0, 1, 2)};
    const TriangleMesh source(source_its);
    TriangleSelector   painting(source);
    const auto         painted_state = static_cast<EnforcerBlockerType>(2);
    painting.set_facet(0, painted_state);

    // This cap lies in the source face's bounding box and projects entirely
    // inside it. Its normal differs by less than three degrees, but it is below
    // the painted surface and must not inherit that surface's color.
    indexed_triangle_set cap;
    cap.vertices = {Vec3f(12.f, 1.f, 0.5f), Vec3f(15.f, 1.f, 0.5f), Vec3f(12.f, 3.f, 0.5f)};
    cap.indices  = {Vec3i32(0, 1, 2)};
    const auto remapped = TriangleSelector::remap_painting(source.its, painting.serialize(), cap, Transform3d::Identity(), std::nullopt);
    const TriangleMesh target(cap);
    TriangleSelector   restored(target);
    restored.deserialize(remapped);
    CHECK_FALSE(restored.has_facets(painted_state));
    CHECK(restored.num_facets(EnforcerBlockerType::NONE) == 1);
}

TEST_CASE("Partial painting remap does not spread across an edge onto a cut cap", "[TriangleSelector][MMUPaint][Remap][CutCap]")
{
    indexed_triangle_set source_its;
    source_its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(20.f, 0.f, 0.f), Vec3f(0.f, 20.f, 0.f)};
    source_its.indices  = {Vec3i32(0, 1, 2)};
    const TriangleMesh source(source_its);

    // Face 0 is an original surface clipped at y=3 and only partly painted.
    // Face 1 shares that cut edge and slopes below it by less than two degrees.
    // Its projection overlaps the paint, so brush propagation must not bypass
    // the remapper's per-face validation when painting face 0.
    indexed_triangle_set target_its;
    target_its.vertices = {Vec3f(0.f, 3.f, 0.f), Vec3f(17.f, 3.f, 0.f), Vec3f(0.f, 20.f, 0.f), Vec3f(17.f, 0.f, -0.1f)};
    target_its.indices  = {Vec3i32(0, 1, 2), Vec3i32(1, 0, 3)};
    const auto remapped = TriangleSelector::remap_painting(source.its, painted_corner_data(0, 1), target_its,
                                                          Transform3d::Identity(), std::nullopt);
    const TriangleMesh target(target_its);
    TriangleSelector   restored(target);
    restored.deserialize(remapped);
    const indexed_triangle_set painted = restored.get_facets(static_cast<EnforcerBlockerType>(2));
    REQUIRE_FALSE(painted.indices.empty());
    CHECK(contains_surface_point(painted, Vec3f(14.f, 4.f, 0.f)));
    CHECK_FALSE(contains_surface_point(painted, Vec3f(4.f, 8.f, 0.f)));
    CHECK_FALSE(contains_surface_point(painted, Vec3f(14.f, 1.f, -1.f / 15.f)));
    CHECK(contains_surface_point(restored.get_facets(EnforcerBlockerType::NONE), Vec3f(14.f, 1.f, -1.f / 15.f)));
    CHECK(std::all_of(painted.vertices.begin(), painted.vertices.end(), [](const Vec3f& vertex) {
        return std::abs(vertex.z()) < 1e-6f;
    }));
}

static void paint_cube_corner(ModelVolume& volume)
{
    TriangleSelector selector(volume.mesh());
    // Facet 4 lies on x=20. Its painted corner occupies y+z >= 30, y<=20, z<=20.
    // Painting corner 1 also detects mirrors accidentally swapping corners 1 and 2.
    selector.deserialize(painted_corner_data(4, 1));
    REQUIRE(volume.mmu_segmentation_facets.set(selector));
}

static void check_cut_corner_painting(const ModelObject& object,
                                      const Transform3d& source_transform,
                                      const Vec3f&       painted_point,
                                      const Vec3f&       unpainted_point,
                                      const Vec3f&       cap_point)
{
    bool found_painted_point   = false;
    bool found_unpainted_point = false;
    bool found_unpainted_cap   = false;
    for (const ModelVolume* volume : object.volumes) {
        if (!volume->is_model_part())
            continue;
        TriangleSelector selector(volume->mesh());
        selector.deserialize(volume->mmu_segmentation_facets.get_data());
        indexed_triangle_set painted   = selector.get_facets(static_cast<EnforcerBlockerType>(2));
        indexed_triangle_set unpainted = selector.get_facets(EnforcerBlockerType::NONE);
        const Transform3f to_source = (source_transform.inverse() * object.instances[0]->get_matrix() * volume->get_matrix()).cast<float>();
        for (Vec3f& vertex : painted.vertices)
            vertex = to_source * vertex;
        for (Vec3f& vertex : unpainted.vertices)
            vertex = to_source * vertex;
        found_painted_point |= contains_surface_point(painted, painted_point);
        found_unpainted_point |= contains_surface_point(unpainted, unpainted_point);
        found_unpainted_cap |= contains_surface_point(unpainted, cap_point);
        CHECK_FALSE(contains_surface_point(painted, unpainted_point));
        CHECK_FALSE(contains_surface_point(painted, cap_point));
        CHECK(std::all_of(painted.vertices.begin(), painted.vertices.end(),
                          [](const Vec3f& vertex) { return std::abs(vertex.x() - 20.f) < 1e-4f; }));
    }
    CHECK(found_painted_point);
    CHECK(found_unpainted_point);
    CHECK(found_unpainted_cap);
}

TEST_CASE("Planar cuts preserve partial facet painting through transforms", "[Cut][MMUPaint][PartialPaint]")
{
    Model          model;
    ModelObject*   object   = model.add_object();
    ModelVolume*   volume   = object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::MODEL_PART, false);
    ModelInstance* instance = object->add_instance();
    paint_cube_corner(*volume);

    SECTION("Untransformed") {}
    SECTION("Mirrored instance") { instance->set_mirror(Vec3d(-1., 1., 1.)); }
    SECTION("Mirrored volume") { volume->set_mirror(Vec3d(-1., 1., 1.)); }
    SECTION("Rotated, translated and nonuniformly scaled volume")
    {
        volume->set_rotation(Vec3d(0.3, 0.4, 0.2));
        volume->set_scaling_factor(Vec3d(1.5, 0.75, 1.25));
        volume->set_offset(Vec3d(6., -4., 2.));
    }

    const Transform3d source_transform = instance->get_matrix_no_offset() * volume->get_matrix();
    const Transform3d cut_matrix       = Geometry::translation_transform(source_transform * Vec3d(0., 0., 15.)) *
                                   Geometry::rotation_transform(volume->get_rotation());
    const ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                                ModelObjectCutAttribute::KeepPaint;
    Cut                    cut(object, 0, cut_matrix, attributes);
    const ModelObjectPtrs& parts = cut.perform_with_plane();
    REQUIRE(parts.size() == 2);
    check_cut_corner_painting(*parts[0], source_transform, Vec3f(20.f, 19.f, 17.f), Vec3f(20.f, 6.f, 17.f), Vec3f(10.f, 10.f, 15.f));
    check_cut_corner_painting(*parts[1], source_transform, Vec3f(20.f, 19.f, 13.f), Vec3f(20.f, 16.f, 11.f), Vec3f(10.f, 10.f, 15.f));
}

TEST_CASE("Repeated cuts preserve painted regions and leave new caps unpainted", "[Cut][MMUPaint][PartialPaint]")
{
    Model        model;
    ModelObject* object = model.add_object();
    ModelVolume* volume = object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::MODEL_PART, false);
    object->add_instance();
    paint_cube_corner(*volume);
    const ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                                ModelObjectCutAttribute::KeepPaint;
    Cut                    first_cut(object, 0, Geometry::translation_transform(Vec3d(0., 0., 15.)), attributes);
    const ModelObjectPtrs& first_parts = first_cut.perform_with_plane();
    REQUIRE(first_parts.size() == 2);

    Cut                    second_cut(first_parts[0], 0, Geometry::translation_transform(Vec3d(0., 0., 17.5)), attributes);
    const ModelObjectPtrs& second_parts = second_cut.perform_with_plane();
    REQUIRE(second_parts.size() == 2);
    check_cut_corner_painting(*second_parts[0], Transform3d::Identity(), Vec3f(20.f, 19.f, 19.f), Vec3f(20.f, 6.f, 19.f),
                              Vec3f(10.f, 10.f, 17.5f));
    check_cut_corner_painting(*second_parts[1], Transform3d::Identity(), Vec3f(20.f, 19.f, 16.f), Vec3f(20.f, 6.f, 16.f),
                              Vec3f(10.f, 10.f, 17.5f));
}

TEST_CASE("Contour cuts restore partial painting from the original mirrored object", "[Cut][MMUPaint][PartialPaint]")
{
    Model        model;
    ModelObject* object = model.add_object();
    ModelVolume* volume = object->add_volume(make_cube(20., 20., 20.), ModelVolumeType::MODEL_PART, false);
    object->add_instance()->set_mirror(Vec3d(-1., 1., 1.));
    paint_cube_corner(*volume);
    const Transform3d      source_transform = object->instances[0]->get_matrix_no_offset() * volume->get_matrix();
    const Transform3d      cut_matrix       = Geometry::translation_transform(Vec3d(0., 0., 15.));
    Cut                    preview_cut(object, 0, cut_matrix);
    const ModelObjectPtrs& preview = preview_cut.perform_with_plane();
    REQUIRE(preview.size() == 1);
    REQUIRE(preview[0]->volumes.size() == 2);

    const ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                                ModelObjectCutAttribute::KeepPaint;
    Cut                    contour_cut(preview[0], 0, cut_matrix, attributes);
    const ModelObjectPtrs& parts = contour_cut.perform_by_contour(object, {{true, false}, {false, false}}, 0);
    REQUIRE(parts.size() == 2);
    check_cut_corner_painting(*parts[0], source_transform, Vec3f(20.f, 19.f, 17.f), Vec3f(20.f, 6.f, 17.f), Vec3f(10.f, 10.f, 15.f));
    check_cut_corner_painting(*parts[1], source_transform, Vec3f(20.f, 19.f, 13.f), Vec3f(20.f, 16.f, 11.f), Vec3f(10.f, 10.f, 15.f));
}
