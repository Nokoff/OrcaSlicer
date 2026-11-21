#include <catch2/catch_test_macros.hpp>
#include <test_utils.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <utility>
#include <vector>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleSelector.hpp>

using namespace Slic3r;

namespace {
TriangleMesh make_subdivided_cube(double size, size_t divisions)
{
    TriangleMesh mesh;
    std::map<std::array<size_t, 3>, int> vertex_ids;
    auto vertex = [&](size_t axis, size_t side, size_t u, size_t v) {
        const size_t u_axis = (axis + 1) % 3;
        const size_t v_axis = (axis + 2) % 3;
        std::array<size_t, 3> coordinate{};
        coordinate[axis] = side * divisions;
        coordinate[u_axis] = u;
        coordinate[v_axis] = v;
        auto [it, inserted] = vertex_ids.emplace(coordinate, static_cast<int>(mesh.its.vertices.size()));
        if (inserted)
            mesh.its.vertices.emplace_back(float(size * coordinate[0] / divisions), float(size * coordinate[1] / divisions),
                                           float(size * coordinate[2] / divisions));
        return it->second;
    };

    for (size_t axis = 0; axis < 3; ++axis)
        for (size_t side = 0; side < 2; ++side)
            for (size_t v = 0; v < divisions; ++v)
                for (size_t u = 0; u < divisions; ++u) {
                    const int a = vertex(axis, side, u, v);
                    const int b = vertex(axis, side, u + 1, v);
                    const int c = vertex(axis, side, u + 1, v + 1);
                    const int d = vertex(axis, side, u, v + 1);
                    if (side == 1) {
                        mesh.its.indices.emplace_back(a, b, c);
                        mesh.its.indices.emplace_back(a, c, d);
                    } else {
                        mesh.its.indices.emplace_back(a, c, b);
                        mesh.its.indices.emplace_back(a, d, c);
                    }
                }
    return mesh;
}
} // namespace

TEST_CASE("CGAL and TriangleMesh conversions", "[MeshBoolean]") {
    TriangleMesh sphere = make_sphere(1.);
    
    auto cgalmesh_ptr = MeshBoolean::cgal::triangle_mesh_to_cgal(sphere);
    
    REQUIRE(cgalmesh_ptr);
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(*cgalmesh_ptr));
    
    TriangleMesh M = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalmesh_ptr);
    
    REQUIRE(M.its.vertices.size() == sphere.its.vertices.size());
    REQUIRE(M.its.indices.size() == sphere.its.indices.size());
    
    REQUIRE_THAT(M.volume(), WithinRel(sphere.volume(), 0.001));
    
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(M));
}

TEST_CASE("MCUT reports source faces separately from generated subtraction walls", "[MeshBoolean][MMUPaint]")
{
    TriangleMesh target = make_cube(10., 10., 10.);
    TriangleMesh cutter = make_cube(4., 4., 12.);
    cutter.translate(Vec3f(3.f, 0.f, 0.f));

    MeshBoolean::mcut::BooleanResult result;
    REQUIRE(MeshBoolean::mcut::make_boolean(target, cutter, result, "A_NOT_B"));
    REQUIRE_FALSE(result.mesh.empty());
    REQUIRE(result.source_faces.size() == result.mesh.its.indices.size());
    REQUIRE(result.source_face_ids.size() == result.mesh.its.indices.size());
    CHECK(std::find(result.source_faces.begin(), result.source_faces.end(), uint8_t(1)) != result.source_faces.end());
    CHECK(std::find(result.source_faces.begin(), result.source_faces.end(), uint8_t(0)) != result.source_faces.end());
    for (size_t face = 0; face < result.source_faces.size(); ++face) {
        if (result.source_faces[face])
            CHECK(result.source_face_ids[face] < target.its.indices.size());
        else
            CHECK(result.source_face_ids[face] == MeshBoolean::mcut::no_source_face);
    }
}

TEST_CASE("MCUT reports subtracting the complete target as a valid empty result", "[MeshBoolean]")
{
    TriangleMesh target = make_cube(10., 10., 10.);
    TriangleMesh cutter = make_cube(20., 20., 20.);
    MeshBoolean::mcut::BooleanResult result;
    REQUIRE(MeshBoolean::mcut::make_boolean(target, cutter, result, "A_NOT_B"));
    CHECK(result.failure == MeshBoolean::mcut::BooleanFailure::None);
    CHECK(result.mesh.empty());
    CHECK(result.source_faces.empty());
    CHECK(result.source_face_ids.empty());
    CHECK(target.its.indices.size() == 12);
}

TEST_CASE("MCUT rejects an unknown operation without changing its input meshes", "[MeshBoolean]")
{
    TriangleMesh target = make_cube(10., 10., 10.);
    TriangleMesh cutter = make_cube(4., 4., 12.);
    cutter.translate(Vec3f(3.f, 0.f, 0.f));
    const indexed_triangle_set target_before = target.its;
    const indexed_triangle_set cutter_before = cutter.its;

    MeshBoolean::mcut::BooleanResult result;
    CHECK_FALSE(MeshBoolean::mcut::make_boolean(target, cutter, result, "NOT_A_BOOLEAN_OPERATION"));
    CHECK(result.failure == MeshBoolean::mcut::BooleanFailure::OperationFailed);
    CHECK(result.mesh.empty());
    CHECK(result.source_faces.empty());
    CHECK(result.source_face_ids.empty());

    REQUIRE(target.its.vertices.size() == target_before.vertices.size());
    REQUIRE(target.its.indices.size() == target_before.indices.size());
    REQUIRE(cutter.its.vertices.size() == cutter_before.vertices.size());
    REQUIRE(cutter.its.indices.size() == cutter_before.indices.size());
    for (size_t vertex = 0; vertex < target_before.vertices.size(); ++vertex)
        CHECK((target.its.vertices[vertex] - target_before.vertices[vertex]).squaredNorm() == 0.f);
    for (size_t face = 0; face < target_before.indices.size(); ++face)
        CHECK((target.its.indices[face].array() == target_before.indices[face].array()).all());
    for (size_t vertex = 0; vertex < cutter_before.vertices.size(); ++vertex)
        CHECK((cutter.its.vertices[vertex] - cutter_before.vertices[vertex]).squaredNorm() == 0.f);
    for (size_t face = 0; face < cutter_before.indices.size(); ++face)
        CHECK((cutter.its.indices[face].array() == cutter_before.indices[face].array()).all());

    auto source_backend = MeshBoolean::mcut::triangle_mesh_to_mcut(target.its);
    auto cutter_backend = MeshBoolean::mcut::triangle_mesh_to_mcut(cutter.its);
    REQUIRE(source_backend);
    REQUIRE(cutter_backend);
    CHECK_FALSE(MeshBoolean::mcut::do_boolean(*source_backend, *cutter_backend, "NOT_A_BOOLEAN_OPERATION"));
    CHECK_FALSE(MeshBoolean::mcut::empty(*source_backend));
    CHECK_FALSE(MeshBoolean::mcut::do_boolean_single(*source_backend, *cutter_backend, "NOT_A_BOOLEAN_OPERATION"));
    CHECK_FALSE(MeshBoolean::mcut::empty(*source_backend));
}

TEST_CASE("MCUT retains sub-millimetre removal depths at small and large model scales", "[MeshBoolean][FineDetail]")
{
    for (double model_size : {10.0, 1000.0}) {
        for (size_t subdivisions : {1, 8}) {
            for (double requested_depth : {0.025, 0.1, 0.25, 0.5}) {
                DYNAMIC_SECTION("model=" << model_size << " subdivisions=" << subdivisions << " depth=" << requested_depth)
                {
            TriangleMesh target = make_subdivided_cube(model_size, subdivisions);
            TriangleMesh cutter = make_cube(2.0 * requested_depth, 1.0, 1.0);
            cutter.translate(Vec3f(float(model_size - requested_depth), float(model_size * 0.5 - 0.5), float(model_size * 0.5 - 0.5)));

            constexpr EnforcerBlockerType exterior_filament = static_cast<EnforcerBlockerType>(33);
            constexpr EnforcerBlockerType cavity_filament = static_cast<EnforcerBlockerType>(34);
            TriangleSelector source_selector(target);
            for (size_t face = 0; face < target.its.indices.size(); ++face)
                source_selector.set_facet(static_cast<int>(face), exterior_filament);
            const auto source_painting = source_selector.serialize();

            MeshBoolean::mcut::BooleanResult result;
            REQUIRE(MeshBoolean::mcut::make_boolean(target, cutter, result, "A_NOT_B"));
            REQUIRE_FALSE(result.mesh.empty());
            REQUIRE(result.source_faces.size() == result.mesh.its.indices.size());

            const auto remapped = TriangleSelector::remap_painting(target.its, source_painting, result.mesh.its, Transform3d::Identity(),
                                                                   std::nullopt, Transform3d::Identity(), &result.source_faces,
                                                                   &result.source_face_ids);
            TriangleSelector output_selector(result.mesh);
            output_selector.deserialize(remapped, false);
            const size_t source_face_count = static_cast<size_t>(std::count(result.source_faces.begin(), result.source_faces.end(), uint8_t(1)));
            const size_t cavity_face_count = result.source_faces.size() - source_face_count;
            REQUIRE(cavity_face_count > 0);
            CHECK(output_selector.num_facets(exterior_filament) == static_cast<int>(source_face_count));
            for (size_t face = 0; face < result.source_faces.size(); ++face)
                if (result.source_faces[face] == 0)
                    output_selector.set_facet(static_cast<int>(face), cavity_filament);
            CHECK(output_selector.num_facets(cavity_filament) == static_cast<int>(cavity_face_count));

            const double requested_wall_x = model_size - requested_depth;
            // MCUT's general-position perturbation is scale-aware; keep large
            // model tolerance below 4% of the smallest requested removal depth.
            const double tolerance = model_size > 100.0 ? 0.001 : 0.0002;
            bool preserved_requested_depth = false;
            bool retained_adjacent_wall = false;
            for (size_t face = 0; face < result.source_faces.size(); ++face) {
                const Vec3i32& triangle = result.mesh.its.indices[face];
                bool on_requested_wall = result.source_faces[face] == 0;
                bool on_adjacent_wall = result.source_faces[face] == 1;
                bool on_y_min_wall = true;
                bool on_y_max_wall = true;
                double centroid_x = 0.0;
                double centroid_z = 0.0;
                for (int corner = 0; corner < 3; ++corner) {
                    const Vec3f& point = result.mesh.its.vertices[triangle[corner]];
                    const double x = point.x();
                    centroid_x += x / 3.0;
                    centroid_z += point.z() / 3.0;
                    on_requested_wall &= std::abs(x - requested_wall_x) <= tolerance;
                    on_y_min_wall &= std::abs(point.y()) <= tolerance;
                    on_y_max_wall &= std::abs(point.y() - model_size) <= tolerance;
                }
                preserved_requested_depth |= on_requested_wall;
                const double nearby_tolerance = std::max(2.0 * requested_depth, model_size / subdivisions);
                retained_adjacent_wall |= on_adjacent_wall && (on_y_min_wall || on_y_max_wall) &&
                                          centroid_x > model_size - nearby_tolerance &&
                                          std::abs(centroid_z - model_size * 0.5) < nearby_tolerance;
            }
            CAPTURE(model_size, subdivisions, requested_depth);
            CHECK(preserved_requested_depth);
            CHECK(retained_adjacent_wall);
                }
            }
        }
    }
}

TEST_CASE("MCUT source face IDs preserve target paint and isolate new cavity walls", "[MeshBoolean][MMUPaint]")
{
    TriangleMesh target = make_cube(10., 10., 10.);
    TriangleMesh cutter = make_cube(4., 4., 12.);
    cutter.translate(Vec3f(3.f, 0.f, 0.f));

    TriangleSelector source_selector(target);
    std::vector<EnforcerBlockerType> source_states;
    source_states.reserve(target.its.indices.size());
    for (size_t face = 0; face < target.its.indices.size(); ++face) {
        const EnforcerBlockerType state = static_cast<EnforcerBlockerType>(33 + face);
        source_states.push_back(state);
        source_selector.set_facet(static_cast<int>(face), state);
    }
    const auto source_painting = source_selector.serialize();
    std::vector<TriangleSelector::TriangleSplittingData> source_other_annotations;
    for (int channel = 0; channel < 3; ++channel) {
        TriangleSelector annotation_selector(target);
        for (size_t face = 0; face < target.its.indices.size(); ++face)
            annotation_selector.set_facet(static_cast<int>(face), EnforcerBlockerType::ENFORCER);
        source_other_annotations.push_back(annotation_selector.serialize());
    }

    MeshBoolean::mcut::BooleanResult result;
    REQUIRE(MeshBoolean::mcut::make_boolean(target, cutter, result, "A_NOT_B"));
    REQUIRE(result.source_faces.size() == result.mesh.its.indices.size());
    REQUIRE(result.source_face_ids.size() == result.mesh.its.indices.size());

    const auto remapped = TriangleSelector::remap_painting(target.its, source_painting, result.mesh.its, Transform3d::Identity(),
                                                            std::nullopt, Transform3d::Identity(), &result.source_faces,
                                                            &result.source_face_ids);
    TriangleSelector output_selector(result.mesh);
    output_selector.deserialize(remapped, false);

    std::vector<size_t> source_output_counts(source_states.size(), 0);
    size_t cutter_output_count = 0;
    for (size_t face = 0; face < result.source_faces.size(); ++face) {
        if (result.source_faces[face]) {
            REQUIRE(result.source_face_ids[face] < source_states.size());
            ++source_output_counts[result.source_face_ids[face]];
        } else {
            CHECK(result.source_face_ids[face] == MeshBoolean::mcut::no_source_face);
            ++cutter_output_count;
        }
    }
    for (size_t source_face = 0; source_face < source_output_counts.size(); ++source_face)
        if (source_output_counts[source_face] > 0)
            CHECK(output_selector.num_facets(source_states[source_face]) == static_cast<int>(source_output_counts[source_face]));

    CHECK(output_selector.num_facets(EnforcerBlockerType::NONE) == static_cast<int>(cutter_output_count));
    const size_t source_output_count = std::accumulate(source_output_counts.begin(), source_output_counts.end(), size_t(0));
    for (const TriangleSelector::TriangleSplittingData& source_annotation : source_other_annotations) {
        const auto annotation_remap = TriangleSelector::remap_painting(target.its, source_annotation, result.mesh.its, Transform3d::Identity(),
                                                                       std::nullopt, Transform3d::Identity(), &result.source_faces,
                                                                       &result.source_face_ids);
        TriangleSelector annotation_output(result.mesh);
        annotation_output.deserialize(annotation_remap, false);
        CHECK(annotation_output.num_facets(EnforcerBlockerType::ENFORCER) == static_cast<int>(source_output_count));
        CHECK(annotation_output.num_facets(EnforcerBlockerType::NONE) == static_cast<int>(cutter_output_count));
    }
    constexpr EnforcerBlockerType cavity_filament = static_cast<EnforcerBlockerType>(45);
    for (size_t face = 0; face < result.source_faces.size(); ++face)
        if (!result.source_faces[face])
            output_selector.set_facet(static_cast<int>(face), cavity_filament);
    CHECK(output_selector.num_facets(cavity_filament) == static_cast<int>(cutter_output_count));
}

TEST_CASE("MCUT sphere subtraction preserves painted target faces and isolates cavity walls", "[MeshBoolean][MMUPaint]")
{
    TriangleMesh target = make_cube(10., 10., 10.);
    TriangleMesh cutter = make_sphere(3., PI / 18.);
    TriangleSelector source_selector(target);
    std::vector<EnforcerBlockerType> source_states;
    for (size_t face = 0; face < target.its.indices.size(); ++face) {
        const EnforcerBlockerType state = static_cast<EnforcerBlockerType>(33 + face);
        source_states.push_back(state);
        source_selector.set_facet(static_cast<int>(face), state);
    }

    MeshBoolean::mcut::BooleanResult result;
    REQUIRE(MeshBoolean::mcut::make_boolean(target, cutter, result, "A_NOT_B"));
    REQUIRE_FALSE(result.mesh.empty());
    REQUIRE(result.source_faces.size() == result.mesh.its.indices.size());
    REQUIRE(result.source_face_ids.size() == result.mesh.its.indices.size());
    const auto remapped = TriangleSelector::remap_painting(target.its, source_selector.serialize(), result.mesh.its,
                                                            Transform3d::Identity(), std::nullopt, Transform3d::Identity(),
                                                            &result.source_faces, &result.source_face_ids);
    TriangleSelector output_selector(result.mesh);
    output_selector.deserialize(remapped, false);
    size_t generated_wall_count = 0;
    for (size_t face = 0; face < result.source_faces.size(); ++face) {
        if (result.source_faces[face]) {
            REQUIRE(result.source_face_ids[face] < source_states.size());
            CHECK(output_selector.num_facets(source_states[result.source_face_ids[face]]) > 0);
        } else {
            CHECK(result.source_face_ids[face] == MeshBoolean::mcut::no_source_face);
            ++generated_wall_count;
        }
    }
    REQUIRE(generated_wall_count > 0);
    constexpr EnforcerBlockerType cavity_filament = static_cast<EnforcerBlockerType>(45);
    for (size_t face = 0; face < result.source_faces.size(); ++face)
        if (!result.source_faces[face])
            output_selector.set_facet(static_cast<int>(face), cavity_filament);
    CHECK(output_selector.num_facets(cavity_filament) == static_cast<int>(generated_wall_count));
}

TEST_CASE("Boolean paint provenance survives mirrored and non-uniform target transforms", "[MeshBoolean][MMUPaint]")
{
    TriangleMesh local_target = make_cube(10., 10., 10.);
    TriangleSelector source_selector(local_target);
    std::vector<EnforcerBlockerType> source_states;
    for (size_t face = 0; face < local_target.its.indices.size(); ++face) {
        const auto state = static_cast<EnforcerBlockerType>(33 + face);
        source_states.push_back(state);
        source_selector.set_facet(static_cast<int>(face), state);
    }
    const auto source_painting = source_selector.serialize();

    const Transform3d source_transform = Eigen::Translation3d(Vec3d(17., -8., 3.)) *
        Eigen::AngleAxisd(0.37, Vec3d::UnitZ()) * Eigen::Scaling(Vec3d(-1.5, 2.0, 0.75));
    TriangleMesh target = local_target;
    target.transform(source_transform, true);
    TriangleMesh cutter = make_cube(4., 4., 12.);
    cutter.translate(Vec3f(3.f, 0.f, 0.f));
    cutter.transform(source_transform, true);

    MeshBoolean::mcut::BooleanResult result;
    REQUIRE(MeshBoolean::mcut::make_boolean(target, cutter, result, "A_NOT_B"));
    REQUIRE_FALSE(result.mesh.empty());
    REQUIRE(result.source_faces.size() == result.mesh.its.indices.size());
    REQUIRE(result.source_face_ids.size() == result.mesh.its.indices.size());

    const Vec3d center = result.mesh.bounding_box().center().cast<double>();
    TriangleMesh display_mesh = result.mesh;
    display_mesh.transform(Geometry::translation_transform(-center), true);
    const auto remapped = TriangleSelector::remap_painting(local_target.its, source_painting, display_mesh.its,
        Geometry::translation_transform(center), std::nullopt, source_transform, &result.source_faces, &result.source_face_ids);
    TriangleSelector output_selector(display_mesh);
    output_selector.deserialize(remapped, false);

    std::vector<size_t> source_output_counts(source_states.size(), 0);
    size_t generated_face_count = 0;
    for (size_t face = 0; face < result.source_faces.size(); ++face) {
        if (result.source_faces[face]) {
            REQUIRE(result.source_face_ids[face] < source_output_counts.size());
            ++source_output_counts[result.source_face_ids[face]];
        } else {
            ++generated_face_count;
        }
    }
    for (size_t source_face = 0; source_face < source_output_counts.size(); ++source_face)
        if (source_output_counts[source_face] > 0)
            CHECK(output_selector.num_facets(source_states[source_face]) == static_cast<int>(source_output_counts[source_face]));
    CHECK(output_selector.num_facets(EnforcerBlockerType::NONE) == static_cast<int>(generated_face_count));
}

TEST_CASE("MCUT sphere subtraction preserves a split paint island on a retriangulated face", "[MeshBoolean][MMUPaint][PartialPaint]")
{
    TriangleMesh target = make_cube(10., 10., 10.);
    TriangleMesh cutter = make_sphere(2., PI / 24.);
    cutter.translate(Vec3f(1.f, 4.5f, 6.f));

    constexpr auto painted = static_cast<EnforcerBlockerType>(37);
    TriangleSelector source_selector(target);
    auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(
        Vec3f(0.f, 2.f, 8.f), Vec3f(10.f, 2.f, 8.f), 1.8f, TriangleSelector::SPHERE,
        Transform3d::Identity(), TriangleSelector::ClippingPlane());
    source_selector.select_patch(8, std::move(cursor), painted, Transform3d::Identity(), true);
    REQUIRE(source_selector.num_facets(painted) > 0);
    const auto source_painting = source_selector.serialize();

    MeshBoolean::mcut::BooleanResult result;
    REQUIRE(MeshBoolean::mcut::make_boolean(target, cutter, result, "A_NOT_B"));
    REQUIRE(result.source_faces.size() == result.mesh.its.indices.size());
    REQUIRE(result.source_face_ids.size() == result.mesh.its.indices.size());
    const auto remapped = TriangleSelector::remap_painting(target.its, source_painting, result.mesh.its,
        Transform3d::Identity(), std::nullopt, Transform3d::Identity(), &result.source_faces, &result.source_face_ids);
    TriangleSelector output_selector(result.mesh);
    output_selector.deserialize(remapped, false);

    auto has_x_zero_surface = [](const indexed_triangle_set& facets) {
        for (const Vec3i32& face : facets.indices) {
            bool on_plane = true;
            for (int corner = 0; corner < 3; ++corner)
                on_plane &= std::abs(facets.vertices[face[corner]].x()) < 1e-4f;
            if (on_plane)
                return true;
        }
        return false;
    };
    CHECK(has_x_zero_surface(output_selector.get_facets(painted)));
    CHECK(has_x_zero_surface(output_selector.get_facets(EnforcerBlockerType::NONE)));
    CHECK(std::find(result.source_faces.begin(), result.source_faces.end(), uint8_t(0)) != result.source_faces.end());
}

TEST_CASE("MCUT preserves original face IDs across disconnected source components", "[MeshBoolean][MMUPaint]")
{
    TriangleMesh first = make_cube(10., 10., 10.);
    TriangleMesh second = make_cube(5., 5., 5.);
    second.translate(Vec3f(30.f, 0.f, 0.f));
    indexed_triangle_set disconnected;
    its_merge(disconnected, first.its);
    its_merge(disconnected, second.its);
    TriangleMesh target(disconnected);

    TriangleMesh cutter = make_cube(4., 4., 12.);
    cutter.translate(Vec3f(3.f, 0.f, 0.f));
    MeshBoolean::mcut::BooleanResult result;
    REQUIRE(MeshBoolean::mcut::make_boolean(target, cutter, result, "A_NOT_B"));
    REQUIRE(result.source_face_ids.size() == result.mesh.its.indices.size());
    REQUIRE(result.source_faces.size() == result.mesh.its.indices.size());

    bool preserved_second_component = false;
    for (size_t face = 0; face < result.source_face_ids.size(); ++face) {
        if (result.source_faces[face]) {
            REQUIRE(result.source_face_ids[face] < target.its.indices.size());
            preserved_second_component |= result.source_face_ids[face] >= first.its.indices.size();
        } else {
            CHECK(result.source_face_ids[face] == MeshBoolean::mcut::no_source_face);
        }
    }
    CHECK(preserved_second_component);
}

TEST_CASE("Centered Boolean output retains its model-object position", "[MeshBoolean]")
{
    TriangleMesh result = make_cube(8., 4., 6.);
    result.translate(Vec3f(17.f, -8.f, 5.f));
    const indexed_triangle_set expected = result.its;

    Model model;
    ModelObject* object = model.add_object();
    ModelVolume* volume = object->add_volume(std::move(result));
    TriangleMesh committed = volume->mesh();
    committed.transform(volume->get_matrix(), true);

    REQUIRE(committed.its.indices == expected.indices);
    REQUIRE(committed.its.vertices.size() == expected.vertices.size());
    for (size_t vertex = 0; vertex < expected.vertices.size(); ++vertex)
        CHECK((committed.its.vertices[vertex] - expected.vertices[vertex]).norm() <= 1e-5f);
}

TEST_CASE("MCUT retains provenance for primitive cutters at fine feature sizes", "[MeshBoolean][MMUPaint][fast]")
{
    for (double model_size : { 10.0, 1000.0 }) {
        const double mid = model_size * 0.5;
        std::vector<TriangleMesh> cutters = {
            [&] {
            TriangleMesh mesh = make_cube(0.05, 1.0, 1.0);
            mesh.translate(Vec3f(float(model_size - 0.025), float(mid - 0.5), float(mid - 0.5)));
            return mesh;
            }(),
            [&] {
            TriangleMesh mesh = make_cylinder(0.025, 1.0, PI / 180.0);
            mesh.transform(Transform3d(Eigen::Translation3d(Vec3d(model_size - 0.01, mid, mid - 0.5))), true);
            return mesh;
            }(),
            [&] {
            TriangleMesh mesh = make_sphere(0.025, PI / 36.0);
            mesh.transform(Transform3d(Eigen::Translation3d(Vec3d(model_size - 0.01, mid, mid))), true);
            return mesh;
            }()
        };

        for (const TriangleMesh& cutter : cutters) {
            TriangleMesh target = make_cube(model_size, model_size, model_size);
            MeshBoolean::mcut::BooleanResult result;
            REQUIRE(MeshBoolean::mcut::make_boolean(target, cutter, result, "A_NOT_B"));
            REQUIRE_FALSE(result.mesh.empty());
            REQUIRE(result.source_faces.size() == result.mesh.its.indices.size());
            REQUIRE(result.source_face_ids.size() == result.mesh.its.indices.size());
            CHECK(std::find(result.source_faces.begin(), result.source_faces.end(), uint8_t(1)) != result.source_faces.end());
            CHECK(std::find(result.source_faces.begin(), result.source_faces.end(), uint8_t(0)) != result.source_faces.end());
        }
    }
}
