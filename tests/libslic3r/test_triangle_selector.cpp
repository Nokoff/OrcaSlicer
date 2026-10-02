#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <tuple>

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

namespace {
struct PaintedExtent
{
    float area  = 0.f;
    float min_z = FLT_MAX;
    float max_z = -FLT_MAX;
};

// Area and height extent of the painted facets whose centroid lies within [min_x, max_x].
PaintedExtent painted_extent(const indexed_triangle_set& facets, float min_x, float max_x)
{
    PaintedExtent extent;
    for (const stl_triangle_vertex_indices& face : facets.indices) {
        const Vec3f& a        = facets.vertices[face(0)];
        const Vec3f& b        = facets.vertices[face(1)];
        const Vec3f& c        = facets.vertices[face(2)];
        const float  centre_x = (a.x() + b.x() + c.x()) / 3.f;
        if (centre_x < min_x || centre_x > max_x)
            continue;
        extent.area += 0.5f * (b - a).cross(c - a).norm();
        for (const Vec3f* v : {&a, &b, &c}) {
            extent.min_z = std::min(extent.min_z, v->z());
            extent.max_z = std::max(extent.max_z, v->z());
        }
    }
    return extent;
}
} // namespace

TEST_CASE("Full depth height band paints disconnected internal shells", "[TriangleSelector][MMUPaint][HeightRange]")
{
    // Two vertical triangles that share no vertex, standing in for an outer wall and a separate internal shell.
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 0.f),   Vec3f(1.f, 0.f, 0.f),   Vec3f(0.f, 0.f, 10.f),
        Vec3f(100.f, 0.f, 0.f), Vec3f(101.f, 0.f, 0.f), Vec3f(100.f, 0.f, 10.f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
        stl_triangle_vertex_indices(3, 4, 5),
    };
    const TriangleMesh mesh(its);
    TriangleSelector   selector(mesh);

    constexpr auto painted = static_cast<EnforcerBlockerType>(3);
    // Band [4, 6] at the height of the first shell only. The cursor never touches the second shell.
    auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(4.f, Vec3f(0.f, -50.f, 5.f), 2.f, Transform3d::Identity(),
                                                                      TriangleSelector::ClippingPlane(), true);
    selector.select_patch(0, std::move(cursor), painted, Transform3d::Identity(), true, 0.f);

    const indexed_triangle_set facets = selector.get_facets(painted);
    for (const auto& [name, min_x, max_x] : {std::tuple{"outer wall", -1.f, 50.f}, std::tuple{"separate shell", 50.f, 102.f}}) {
        INFO(name);
        const PaintedExtent extent = painted_extent(facets, min_x, max_x);
        // Triangle width at height z is (1 - z / 10), so the band covers an area of exactly 1. The cursor is
        // allowed to overshoot by about the edge limit, no more.
        CHECK(extent.area > 0.85f);
        CHECK(extent.area < 1.2f);
        CHECK(extent.min_z > 3.75f);
        CHECK(extent.max_z < 6.25f);
    }
}

TEST_CASE("Full depth height band paints a flat face lying just outside the band edge", "[TriangleSelector][MMUPaint][HeightRange]")
{
    // A horizontal cap 0.01 mm below a band that starts at 4 mm: within the band tolerance, so it counts as inside.
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 3.99f),
        Vec3f(1.f, 0.f, 3.99f),
        Vec3f(0.f, 1.f, 3.99f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
    };
    const TriangleMesh mesh(its);
    TriangleSelector   selector(mesh);

    constexpr auto painted = static_cast<EnforcerBlockerType>(2);
    auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(4.f, Vec3f(0.f, 0.f, 50.f), 2.f, Transform3d::Identity(),
                                                                      TriangleSelector::ClippingPlane(), true);
    selector.select_patch(0, std::move(cursor), painted, Transform3d::Identity(), true, 0.f);

    CHECK(selector.num_facets(painted) == 1);
}

namespace {
// A flat 4 x 4 grid of unit cells, two triangles each. Cell (i, j) owns facets 2 * (j * 4 + i) and 2 * (j * 4 + i) + 1.
indexed_triangle_set flat_grid()
{
    indexed_triangle_set its;
    for (int j = 0; j <= 4; ++j)
        for (int i = 0; i <= 4; ++i)
            its.vertices.emplace_back(float(i), float(j), 0.f);
    const auto v = [](int i, int j) { return j * 5 + i; };
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            its.indices.emplace_back(v(i, j), v(i + 1, j), v(i, j + 1));
            its.indices.emplace_back(v(i + 1, j), v(i + 1, j + 1), v(i, j + 1));
        }
    return its;
}
} // namespace

TEST_CASE("Removing small regions repaints a speck with the color around it", "[TriangleSelector][MMUPaint][Despeckle]")
{
    const TriangleMesh mesh(flat_grid());
    TriangleSelector   selector(mesh);
    constexpr auto     green = static_cast<EnforcerBlockerType>(2);
    constexpr auto     white = static_cast<EnforcerBlockerType>(3);
    for (int facet = 0; facet < 32; ++facet)
        selector.set_facet(facet, green);

    SECTION("a whole-facet speck")
    {
        selector.set_facet(10, white);
        selector.set_facet(11, white);
        REQUIRE(selector.num_facets(white) == 2);

        CHECK(selector.remove_small_regions(2.5f, 3.f, Transform3d::Identity()) == 1);
        CHECK(selector.num_facets(white) == 0);
        CHECK(selector.num_facets(green) == 32);
    }

    SECTION("a speck cut out of a facet by a brush")
    {
        auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f(1.2f, 1.2f, 0.f), Vec3f(1.2f, 1.2f, 10.f), 0.2f,
                                                                          TriangleSelector::CursorType::SPHERE, Transform3d::Identity(),
                                                                          TriangleSelector::ClippingPlane());
        selector.select_patch(10, std::move(cursor), white, Transform3d::Identity(), true, 0.f);
        REQUIRE(selector.num_facets(white) > 0);

        CHECK(selector.remove_small_regions(0.5f, 3.f, Transform3d::Identity()) >= 1);
        CHECK(selector.num_facets(white) == 0);
        // The split facets merge back into the original ones.
        CHECK(selector.num_facets(green) == 32);
    }

    SECTION("a region bigger than the limit stays")
    {
        selector.set_facet(10, white);
        selector.set_facet(11, white);
        CHECK(selector.remove_small_regions(0.8f, 3.f, Transform3d::Identity()) == 0);
        CHECK(selector.num_facets(white) == 2);
    }

    SECTION("the limit is measured in world units")
    {
        selector.set_facet(10, white);
        selector.set_facet(11, white);
        // The speck covers 1, or 4 once scaled by 2, which is above the limit of 2.5.
        CHECK(selector.remove_small_regions(2.5f, 3.f, Eigen::Scaling(2., 2., 2.) * Transform3d::Identity()) == 0);
        CHECK(selector.num_facets(white) == 2);
    }

    SECTION("a speck takes the color that dominates around it")
    {
        // Facet 10 touches red on two sides and green on one, and red covers most of the surface around it.
        constexpr auto red = static_cast<EnforcerBlockerType>(4);
        for (int facet = 0; facet < 32; ++facet)
            selector.set_facet(facet, red);
        selector.set_facet(10, white);
        selector.set_facet(11, green);

        selector.remove_small_regions(1.f, 3.f, Transform3d::Identity());
        CHECK(selector.num_facets(white) == 0);
        CHECK(selector.num_facets(red) == 32);
    }
}

TEST_CASE("Removing small regions looks only as far as the blend radius", "[TriangleSelector][MMUPaint][Despeckle]")
{
    const TriangleMesh mesh(flat_grid());
    TriangleSelector   selector(mesh);
    constexpr auto     red   = static_cast<EnforcerBlockerType>(4);
    constexpr auto     green = static_cast<EnforcerBlockerType>(2);
    constexpr auto     white = static_cast<EnforcerBlockerType>(3);
    for (int facet = 0; facet < 32; ++facet)
        selector.set_facet(facet, red);
    // Facet 10 is the speck. Two of its neighbors (9 and 3) and their cell mates (8 and 2) are green, the third neighbor
    // (11) and the rest of the grid are red.
    for (int facet : {8, 9, 2, 3})
        selector.set_facet(facet, green);
    selector.set_facet(10, white);

    SECTION("a short radius sees only the neighbors")
    {
        CHECK(selector.remove_small_regions(0.6f, 0.3f, Transform3d::Identity()) == 1);
        CHECK(selector.num_facets(white) == 0);
        CHECK(selector.num_facets(green) == 5);
    }

    SECTION("a long radius sees the whole surface around it")
    {
        CHECK(selector.remove_small_regions(0.6f, 6.f, Transform3d::Identity()) == 1);
        CHECK(selector.num_facets(white) == 0);
        CHECK(selector.num_facets(green) == 4);
    }
}

TEST_CASE("Removing small regions keeps a painted region that has no neighbor", "[TriangleSelector][MMUPaint][Despeckle]")
{
    // Two separate triangles: the second is a small painted part of its own and must not be repainted.
    indexed_triangle_set its;
    its.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f),
                    Vec3f(10.f, 0.f, 0.f), Vec3f(11.f, 0.f, 0.f), Vec3f(10.f, 1.f, 0.f)};
    its.indices  = {stl_triangle_vertex_indices(0, 1, 2), stl_triangle_vertex_indices(3, 4, 5)};
    const TriangleMesh mesh(its);
    TriangleSelector   selector(mesh);
    constexpr auto     white = static_cast<EnforcerBlockerType>(3);
    selector.set_facet(1, white);

    CHECK(selector.remove_small_regions(100.f, 3.f, Transform3d::Identity()) == 0);
    CHECK(selector.num_facets(white) == 1);
}

namespace {
// A flat grid of cells x cells unit cells, two triangles each. Cell (i, j) owns facets 2 * (j * cells + i) and the next one.
indexed_triangle_set flat_grid_of(int cells)
{
    indexed_triangle_set its;
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i)
            its.vertices.emplace_back(float(i), float(j), 0.f);
    const auto v = [cells](int i, int j) { return j * (cells + 1) + i; };
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            its.indices.emplace_back(v(i, j), v(i + 1, j), v(i, j + 1));
            its.indices.emplace_back(v(i + 1, j), v(i + 1, j + 1), v(i, j + 1));
        }
    return its;
}

struct StateStats
{
    float area  = 0.f;
    float min_x = FLT_MAX;
    float max_x = -FLT_MAX;
};

// Area and the x range of the centers of the facets of one state, optionally limited to the rows y in [min_y, max_y].
StateStats state_stats(const TriangleSelector& selector, EnforcerBlockerType state, float min_y = -FLT_MAX, float max_y = FLT_MAX)
{
    StateStats                 stats;
    const indexed_triangle_set facets = selector.get_facets(state);
    for (const stl_triangle_vertex_indices& face : facets.indices) {
        const Vec3f& a      = facets.vertices[face(0)];
        const Vec3f& b      = facets.vertices[face(1)];
        const Vec3f& c      = facets.vertices[face(2)];
        const Vec3f  center = (a + b + c) / 3.f;
        stats.area += 0.5f * (b - a).cross(c - a).norm();
        if (center.y() < min_y || center.y() > max_y)
            continue;
        stats.min_x = std::min(stats.min_x, center.x());
        stats.max_x = std::max(stats.max_x, center.x());
    }
    return stats;
}
} // namespace

TEST_CASE("Smoothing edges straightens a jagged border and removes tabs", "[TriangleSelector][MMUPaint][SmoothEdges]")
{
    constexpr int      cells = 20;
    const TriangleMesh mesh(flat_grid_of(cells));
    TriangleSelector   selector(mesh);
    constexpr auto     red   = static_cast<EnforcerBlockerType>(2);
    constexpr auto     green = static_cast<EnforcerBlockerType>(3);

    // Green on the right, with a zig-zag border (a jigsaw one cell deep) and a one cell tab sticking into the red area.
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i) {
            const bool is_green = i >= 10 + (j % 2) || (i == 8 && j == 10);
            selector.set_facet(2 * (j * cells + i), is_green ? green : red);
            selector.set_facet(2 * (j * cells + i) + 1, is_green ? green : red);
        }
    REQUIRE(state_stats(selector, green).area > 0.f);

    SECTION("a radius of two")
    {
        CHECK(selector.smooth_boundaries(2.f, Transform3d::Identity()) > 0);

        // The tab is gone: no green is left to the left of the zig-zag.
        const StateStats green_stats = state_stats(selector, green, 4.f, 16.f);
        INFO("green x range " << green_stats.min_x << " .. " << green_stats.max_x);
        CHECK(green_stats.min_x > 9.f);
        const StateStats red_stats = state_stats(selector, red, 4.f, 16.f);
        INFO("red x range " << red_stats.min_x << " .. " << red_stats.max_x);
        CHECK(red_stats.max_x < 12.f);

        // The border is straight now: every row starts its green at about the same x, where before it alternated by a cell.
        float lowest = FLT_MAX, highest = -FLT_MAX;
        for (int row = 5; row < 15; ++row) {
            const StateStats row_stats = state_stats(selector, green, float(row), float(row + 1));
            lowest                     = std::min(lowest, row_stats.min_x);
            highest                    = std::max(highest, row_stats.min_x);
        }
        INFO("green starts between x = " << lowest << " and " << highest);
        CHECK(highest - lowest < 0.6f);

        // No geometry was lost or invented, and the far areas stayed as they were.
        CHECK_THAT(state_stats(selector, red).area + state_stats(selector, green).area, Catch::Matchers::WithinAbs(400.0, 0.01));
        CHECK(state_stats(selector, green).max_x > 18.f);
        CHECK(state_stats(selector, red).min_x < 2.f);
    }

    SECTION("a model without borders is left alone")
    {
        TriangleSelector uniform(mesh);
        for (int facet = 0; facet < 2 * cells * cells; ++facet)
            uniform.set_facet(facet, red);
        CHECK(uniform.smooth_boundaries(2.f, Transform3d::Identity()) == 0);
        CHECK(uniform.num_facets(red) == 2 * cells * cells);
    }
}
