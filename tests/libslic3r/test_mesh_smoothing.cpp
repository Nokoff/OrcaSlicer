#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

#include "libslic3r/MeshSmoothing.hpp"
#include "libslic3r/Model.hpp"

using namespace Slic3r;

namespace {
indexed_triangle_set bump()
{
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0, 0, 1), Vec3f(-2, -2, 0), Vec3f(2, -2, 0), Vec3f(2, 2, 0), Vec3f(-2, 2, 0)};
    mesh.indices  = {stl_triangle_vertex_indices(0, 1, 2), stl_triangle_vertex_indices(0, 2, 3), stl_triangle_vertex_indices(0, 3, 4),
                     stl_triangle_vertex_indices(0, 4, 1)};
    return mesh;
}

indexed_triangle_set smoothing_benchmark_grid(size_t target_faces, Vec3f& hit, size_t& seed_face)
{
    constexpr size_t cells_x = 1000;
    const size_t     cells_y = (target_faces + 2 * cells_x - 1) / (2 * cells_x);
    indexed_triangle_set mesh;
    mesh.vertices.reserve((cells_x + 1) * (cells_y + 1));
    mesh.indices.reserve(2 * cells_x * cells_y);
    for (size_t y = 0; y <= cells_y; ++y)
        for (size_t x = 0; x <= cells_x; ++x)
            mesh.vertices.emplace_back(float(x) * 0.1f, float(y) * 0.1f, 0.f);
    const size_t center_x = cells_x / 2;
    const size_t center_y = cells_y / 2;
    const size_t center_vertex = center_y * (cells_x + 1) + center_x;
    mesh.vertices[center_vertex].z() = 0.2f;
    for (size_t y = 0; y < cells_y; ++y)
        for (size_t x = 0; x < cells_x; ++x) {
            const int a = int(y * (cells_x + 1) + x);
            const int b = a + 1;
            const int c = a + int(cells_x + 1);
            const int d = c + 1;
            mesh.indices.emplace_back(a, b, d);
            mesh.indices.emplace_back(a, d, c);
        }
    hit = mesh.vertices[center_vertex];
    seed_face = 2 * (center_y * cells_x + center_x);
    return mesh;
}

indexed_triangle_set smoothing_feature_grid(float step, float feature_height, size_t& seed_face, size_t& feature_vertex)
{
    constexpr size_t cells_per_axis = 10;
    indexed_triangle_set mesh;
    mesh.vertices.reserve((cells_per_axis + 1) * (cells_per_axis + 1));
    mesh.indices.reserve(2 * cells_per_axis * cells_per_axis);
    for (size_t y = 0; y <= cells_per_axis; ++y)
        for (size_t x = 0; x <= cells_per_axis; ++x)
            mesh.vertices.emplace_back((float(x) - 5.f) * step, (float(y) - 5.f) * step,
                                       x == 5 && y == 5 ? feature_height : 0.f);
    for (size_t y = 0; y < cells_per_axis; ++y)
        for (size_t x = 0; x < cells_per_axis; ++x) {
            const int a = int(y * (cells_per_axis + 1) + x);
            const int b = a + 1;
            const int c = a + int(cells_per_axis + 1);
            const int d = c + 1;
            mesh.indices.emplace_back(a, b, d);
            mesh.indices.emplace_back(a, d, c);
        }
    seed_face = 2 * (4 * cells_per_axis + 4);
    feature_vertex = 5 * (cells_per_axis + 1) + 5;
    return mesh;
}

void run_smoothing_scaling_benchmark(size_t target_faces)
{
    Vec3f hit;
    size_t seed_face = 0;
    auto mesh = smoothing_benchmark_grid(target_faces, hit, seed_face);
    const auto setup_start = std::chrono::steady_clock::now();
    const std::vector<Vec3f> stroke_origin = mesh.vertices;
    const auto copy_finish = std::chrono::steady_clock::now();
    MeshSmoothingBrush brush(mesh);
    const auto construct_finish = std::chrono::steady_clock::now();
    const auto apply_start = std::chrono::steady_clock::now();
    const bool changed = brush.apply(mesh, seed_face, hit, Transform3d::Identity(), 0.5f, 0.5f, 0.1f, &stroke_origin);
    const auto finish = std::chrono::steady_clock::now();
    const auto copy_ms = std::chrono::duration_cast<std::chrono::milliseconds>(copy_finish - setup_start).count();
    const auto construct_ms = std::chrono::duration_cast<std::chrono::milliseconds>(construct_finish - copy_finish).count();
    const auto apply_ms = std::chrono::duration_cast<std::chrono::milliseconds>(finish - apply_start).count();
    std::cout << "[MeshSmoothingBenchmark] faces=" << mesh.indices.size() << " stroke_copy_ms=" << copy_ms
              << " brush_construct_ms=" << construct_ms << " dab_ms=" << apply_ms << " changed=" << changed << std::endl;
    REQUIRE(changed);
}
} // namespace

TEST_CASE("Smooth brush reduces a bump without changing connectivity or boundaries", "[MeshSmoothing]")
{
    auto               mesh     = bump();
    const auto         original = mesh;
    MeshSmoothingBrush brush(mesh);
    REQUIRE(brush.apply(mesh, 0, original.vertices[0], Transform3d::Identity(), 4.f, 0.5f));
    CHECK(mesh.indices == original.indices);
    REQUIRE(mesh.vertices.size() == original.vertices.size());
    CHECK(mesh.vertices[0].z() < original.vertices[0].z());
    CHECK(mesh.vertices[0].z() > 0.f);
    for (size_t i = 1; i < mesh.vertices.size(); ++i)
        CHECK(mesh.vertices[i] == original.vertices[i]);
}

TEST_CASE("Smooth brush respects strength, world radius and disconnected shells", "[MeshSmoothing]")
{
    auto               weak = bump(), strong = weak;
    MeshSmoothingBrush brush(weak);
    REQUIRE(brush.apply(weak, 0, weak.vertices[0], Transform3d::Identity(), 4.f, 0.1f));
    REQUIRE(brush.apply(strong, 0, strong.vertices[0], Transform3d::Identity(), 4.f, 0.8f));
    CHECK(strong.vertices[0].z() < weak.vertices[0].z());

    auto        mesh     = bump();
    const auto  original = mesh;
    Transform3d scaled   = Transform3d::Identity();
    scaled.scale(Vec3d(10, 1, 1));
    CHECK_FALSE(brush.apply(mesh, 0, Vec3f(0.5f, 0, 1), scaled, 2.f, 1.f));
    CHECK(mesh.vertices == original.vertices);
    CHECK_FALSE(brush.apply(mesh, 0, mesh.vertices[0], scaled, 2.f, 0.f));
    CHECK_FALSE(brush.apply(mesh, mesh.indices.size(), mesh.vertices[0], scaled, 2.f, 1.f));

    // A second shell occupies the same brush sphere but is not connected.
    for (const auto& v : original.vertices)
        mesh.vertices.push_back(v + Vec3f(0, 0, -0.1f));
    for (const auto& f : original.indices)
        mesh.indices.emplace_back(f[0] + int(original.vertices.size()), f[1] + int(original.vertices.size()),
                                  f[2] + int(original.vertices.size()));
    const auto         shells = mesh;
    MeshSmoothingBrush shell_brush(mesh);
    CHECK_FALSE(shell_brush.apply(mesh, 0, mesh.vertices[0], Transform3d::Identity(), 4.f, 1.f));
    CHECK(shell_brush.last_statistics().rejected_clearance > 0);
    CHECK(mesh.vertices == shells.vertices);
    for (size_t i = original.vertices.size(); i < mesh.vertices.size(); ++i)
        CHECK(mesh.vertices[i] == shells.vertices[i]);
}

TEST_CASE("Smooth brush does not join shells that touch at one point", "[MeshSmoothing]")
{
    auto mesh = bump();
    const size_t first_shell_vertex_count = mesh.vertices.size();
    const Vec3f shared_point = mesh.vertices[0];
    const Eigen::AngleAxisf rotation(0.63f, Vec3f(1.f, 2.f, 3.f).normalized());
    const auto second_shell = bump();
    for (size_t i = 0; i < first_shell_vertex_count; ++i)
        mesh.vertices.push_back(shared_point + rotation * (second_shell.vertices[i] - shared_point));
    for (const auto& face : second_shell.indices)
        mesh.indices.emplace_back(face[0] + int(first_shell_vertex_count), face[1] + int(first_shell_vertex_count),
                                  face[2] + int(first_shell_vertex_count));
    const auto original = mesh;

    MeshSmoothingBrush brush(mesh);
    REQUIRE(brush.apply(mesh, 0, mesh.vertices[0], Transform3d::Identity(), 4.f, 0.8f));
    for (size_t i = first_shell_vertex_count; i < mesh.vertices.size(); ++i)
        CHECK(mesh.vertices[i] == original.vertices[i]);
}

TEST_CASE("Smooth brush rejects swept face collisions missed by vertex paths", "[MeshSmoothing]")
{
    auto mesh = bump();
    const int obstacle_vertex = int(mesh.vertices.size());
    mesh.vertices.insert(mesh.vertices.end(), {Vec3f(-0.1f, -0.6f, 0.2f), Vec3f(0.1f, -0.6f, 0.2f), Vec3f(0.f, -0.4f, 0.2f)});
    mesh.indices.emplace_back(obstacle_vertex, obstacle_vertex + 1, obstacle_vertex + 2);

    // The brush's collision tree is fixed at stroke start. Model the obstacle
    // moving within the displacement cap before this dab and require the broad
    // phase to account for that drift.
    const std::vector<Vec3f> stroke_origin = mesh.vertices;
    MeshSmoothingBrush brush(mesh);
    for (int vertex = obstacle_vertex; vertex < obstacle_vertex + 3; ++vertex)
        mesh.vertices[vertex].z() += 0.45f;
    const auto original = mesh;
    CHECK_FALSE(brush.apply(mesh, 0, mesh.vertices[0], Transform3d::Identity(), 4.f, 1.f, 0.5f, &stroke_origin));
    CHECK(mesh.vertices == original.vertices);
    CHECK(brush.last_statistics().rejected_clearance > 0);
}

TEST_CASE("Smooth brush keeps duplicated vertices synchronized across a seam", "[MeshSmoothing]")
{
    auto mesh = bump();
    const int duplicate_apex = int(mesh.vertices.size());
    mesh.vertices.push_back(mesh.vertices[0]);
    const int duplicate_corner = int(mesh.vertices.size());
    mesh.vertices.push_back(mesh.vertices[2]);
    mesh.indices[1] = stl_triangle_vertex_indices(duplicate_apex, duplicate_corner, 3);

    MeshSmoothingBrush brush(mesh);
    REQUIRE(brush.apply(mesh, 0, mesh.vertices[0], Transform3d::Identity(), 4.f, 0.8f));
    CHECK(mesh.vertices[0] == mesh.vertices[duplicate_apex]);
    CHECK(mesh.vertices[2] == mesh.vertices[duplicate_corner]);
}

TEST_CASE("Optional sharp-edge preservation pins cube corners", "[MeshSmoothing]")
{
    const TriangleMesh cube = make_cube(10., 10., 10.);
    indexed_triangle_set mesh = cube.its;
    const auto original = mesh;
    MeshSmoothingBrush normal_brush(mesh);
    CHECK(normal_brush.apply(mesh, 0, mesh.vertices[0], Transform3d::Identity(), 100.f, 1.f));
    CHECK(mesh.vertices != original.vertices);

    mesh = original;
    MeshSmoothingBrush brush(mesh, true, Transform3d::Identity());

    CHECK_FALSE(brush.apply(mesh, 0, mesh.vertices[0], Transform3d::Identity(), 100.f, 1.f));
    CHECK(mesh.vertices == original.vertices);
    CHECK(brush.last_statistics().pinned > 0);
}

TEST_CASE("Smooth brush accepts fine radii and enforces a world displacement limit", "[MeshSmoothing]")
{
    auto               mesh = bump();
    MeshSmoothingBrush brush(mesh);
    REQUIRE(brush.apply(mesh, 0, mesh.vertices[0], Transform3d::Identity(), 0.025f, 1.f, 0.01f));
    CHECK(mesh.vertices[0].z() < 1.f);
    CHECK((mesh.vertices[0] - Vec3f(0, 0, 1)).norm() <= 0.01001f);
    CHECK(brush.last_statistics().max_displacement_mm <= 0.01001);
}

TEST_CASE("Smooth brush enforces displacement limit across an entire stroke", "[MeshSmoothing]")
{
    auto mesh = bump();
    const std::vector<Vec3f> stroke_origin = mesh.vertices;
    Transform3d world = Transform3d::Identity();
    world.translation() = Vec3d(17., -8., 3.);
    world.linear() = Eigen::AngleAxisd(0.37, Vec3d::UnitZ()).toRotationMatrix() * Vec3d(2., 3., 4.).asDiagonal();
    MeshSmoothingBrush brush(mesh, false, world);
    for (int dab = 0; dab < 12; ++dab)
        brush.apply(mesh, 0, Vec3f(0.f, 0.f, 1.f), world, 4.f, 1.f, 0.01f, &stroke_origin);

    const double world_displacement = (world.linear() * (mesh.vertices[0] - stroke_origin[0]).cast<double>()).norm();
    CHECK(world_displacement <= 0.01001);
    CHECK(world_displacement > 0.009);
    CHECK(brush.last_statistics().max_displacement_mm <= 0.01001);
}

TEST_CASE("Smooth brush keeps radius and displacement in transformed world space", "[MeshSmoothing]")
{
    auto mesh = bump();
    const auto original = mesh;
    Transform3d world = Transform3d::Identity();
    world.translation() = Vec3d(17., -8., 3.);
    world.linear() = Eigen::AngleAxisd(0.37, Vec3d::UnitZ()).toRotationMatrix() * Vec3d(2., 3., 1.).asDiagonal();
    const Vec3f hit = original.vertices[0];
    MeshSmoothingBrush brush(mesh, false, world);

    REQUIRE(brush.apply(mesh, 0, hit, world, 0.025f, 1.f, 0.01f));
    const Vec3d world_before = world * original.vertices[0].cast<double>();
    const Vec3d world_after = world * mesh.vertices[0].cast<double>();
    CHECK((world_after - world_before).norm() <= 0.01001);
    CHECK(brush.last_statistics().max_displacement_mm <= 0.01001);
    CHECK(mesh.indices == original.indices);
    for (size_t vertex = 1; vertex < mesh.vertices.size(); ++vertex)
        CHECK(mesh.vertices[vertex] == original.vertices[vertex]);
}

TEST_CASE("Fine brush subdivision stays local and tracks source faces", "[MeshSmoothing][MMUPaint]")
{
    const indexed_triangle_set original = bump();
    auto mesh = original;
    size_t refined_seed_face = 0;
    std::vector<size_t> source_faces;
    std::vector<MeshSmoothingBarycentricLineage> source_barycentrics;
    MeshSmoothingBrush brush(original);
    REQUIRE(brush.refine_for_brush(mesh, 0, original.vertices[0], Transform3d::Identity(), 0.025f,
                                   refined_seed_face, source_faces, source_barycentrics));
    REQUIRE(mesh.indices.size() > original.indices.size());
    REQUIRE(source_faces.size() == mesh.indices.size());
    REQUIRE(source_barycentrics.size() == mesh.indices.size());
    CHECK(refined_seed_face < mesh.indices.size());
    CHECK(source_faces[refined_seed_face] == 0);
    CHECK(std::all_of(source_faces.begin(), source_faces.end(), [&](size_t face) { return face < original.indices.size(); }));
    CHECK(std::all_of(source_barycentrics.begin(), source_barycentrics.end(), [&](const auto& lineage) {
        if (lineage.target_face >= mesh.indices.size())
            return false;
        const Vec3i32& source_face = original.indices[source_faces[lineage.target_face]];
        const Vec3i32& target_face = mesh.indices[lineage.target_face];
        for (size_t corner = 0; corner < 3; ++corner) {
            const Vec3f& barycentric = lineage.source_barycentrics[corner];
            if (!(std::abs(barycentric.sum() - 1.f) < 1e-5f && (barycentric.array() >= -1e-5f).all()))
                return false;
            const Vec3f source_point = barycentric[0] * original.vertices[source_face[0]] +
                                       barycentric[1] * original.vertices[source_face[1]] +
                                       barycentric[2] * original.vertices[source_face[2]];
            if ((mesh.vertices[target_face[corner]] - source_point).norm() > 1e-5f)
                return false;
        }
        return true;
    }));

    auto moved_source = original;
    moved_source.vertices[0].z() = 0.995f;
    auto refined_after_prior_dab = moved_source;
    std::vector<size_t> moved_source_faces;
    std::vector<MeshSmoothingBarycentricLineage> moved_source_barycentrics;
    size_t moved_seed_face = 0;
    MeshSmoothingBrush moved_source_brush(moved_source);
    REQUIRE(moved_source_brush.refine_for_brush(refined_after_prior_dab, 0, moved_source.vertices[0], Transform3d::Identity(), 0.025f,
                                                moved_seed_face, moved_source_faces, moved_source_barycentrics));
    std::vector<Vec3f> stroke_origin;
    REQUIRE(MeshSmoothingBrush::map_stroke_origin_to_refined_mesh(moved_source, original.vertices, refined_after_prior_dab,
                                                                  moved_source_faces, moved_source_barycentrics, stroke_origin));
    CHECK(stroke_origin[0] == original.vertices[0]);
    CHECK(stroke_origin[0] != refined_after_prior_dab.vertices[0]);

    auto two_shells = original;
    for (const Vec3f& vertex : original.vertices)
        two_shells.vertices.push_back(vertex + Vec3f(0.f, 0.f, -0.1f));
    for (const Vec3i32& face : original.indices)
        two_shells.indices.emplace_back(face[0] + int(original.vertices.size()), face[1] + int(original.vertices.size()),
                                        face[2] + int(original.vertices.size()));
    std::vector<size_t> shell_lineage;
    std::vector<MeshSmoothingBarycentricLineage> shell_barycentrics;
    size_t shell_seed = 0;
    MeshSmoothingBrush shell_brush(two_shells);
    REQUIRE(shell_brush.refine_for_brush(two_shells, 0, original.vertices[0], Transform3d::Identity(), 0.025f,
                                         shell_seed, shell_lineage, shell_barycentrics));
    CHECK(std::count_if(shell_lineage.begin(), shell_lineage.end(), [&](size_t face) { return face >= original.indices.size(); }) ==
          original.indices.size());

    TriangleSelector source_selector{TriangleMesh(original)};
    const auto color = static_cast<EnforcerBlockerType>(33);
    const auto adjacent_color = static_cast<EnforcerBlockerType>(34);
    source_selector.set_facet(1, adjacent_color);
    source_selector.select_patch(0,
                                 TriangleSelector::SinglePointCursor::cursor_factory(Vec3f(0.f, -1.f, 0.5f), Vec3f(0.f, -1.f, 10.f),
                                                                                     0.4f, TriangleSelector::SPHERE,
                                                                                     Transform3d::Identity(), TriangleSelector::ClippingPlane()),
                                 color, Transform3d::Identity(), true);
    const auto source_painting = source_selector.serialize();
    REQUIRE(source_painting.bitstream.size() > 8);
    REQUIRE(source_selector.num_facets(color) > 0);
    REQUIRE(source_selector.num_facets(adjacent_color) > 0);
    const auto remapped = TriangleSelector::remap_painting(original, source_painting, mesh, Transform3d::Identity(),
                                                            std::nullopt, Transform3d::Identity(), nullptr, &source_faces);
    CHECK(TriangleSelector::has_facets(remapped, color));
    CHECK(TriangleSelector::has_facets(remapped, adjacent_color));
    REQUIRE(!remapped.triangles_to_split.empty());
    CHECK(remapped.triangles_to_split.back().triangle_idx < static_cast<int>(mesh.indices.size()));
    CHECK(std::all_of(remapped.triangles_to_split.begin(), remapped.triangles_to_split.end(), [&](const auto& mapping) {
        return mapping.triangle_idx >= 0 && mapping.triangle_idx < static_cast<int>(mesh.indices.size());
    }));
    TriangleMesh refined_mesh(mesh);
    TriangleSelector refined_selector{refined_mesh};
    CHECK(refined_selector.num_facets(EnforcerBlockerType::NONE) == static_cast<int>(mesh.indices.size()));
    refined_selector.deserialize(remapped);
    CHECK(refined_selector.num_facets(color) > 0);
    CHECK(refined_selector.num_facets(adjacent_color) > 0);
    CHECK(refined_selector.num_facets(EnforcerBlockerType::NONE) > 0);
}

TEST_CASE("Fine brush subdivision makes a sub-triangle dab effective inside its footprint", "[MeshSmoothing]")
{
    const indexed_triangle_set original = bump();
    indexed_triangle_set       refined  = original;
    const Vec3f                hit      = (original.vertices[0] + original.vertices[1] + original.vertices[2]) / 3.f;
    constexpr float            radius   = 0.025f; // 0.05 mm brush diameter boundary case.
    size_t                     seed_face = 0;
    std::vector<size_t>        source_faces;
    std::vector<MeshSmoothingBarycentricLineage> source_barycentrics;

    MeshSmoothingBrush original_brush(original);
    REQUIRE(original_brush.refine_for_brush(refined, 0, hit, Transform3d::Identity(), radius, seed_face, source_faces,
                                            source_barycentrics));
    REQUIRE(refined.indices.size() > original.indices.size());

    const auto before = refined.vertices;
    const auto stroke_origin = refined.vertices;
    MeshSmoothingBrush refined_brush(refined);
    REQUIRE(refined_brush.apply(refined, seed_face, hit, Transform3d::Identity(), radius, 1.f, 0.05f, &stroke_origin));
    size_t moved = 0;
    bool all_moved_within_footprint = true;
    for (size_t vertex = 0; vertex < refined.vertices.size(); ++vertex)
        if (refined.vertices[vertex] != before[vertex]) {
            ++moved;
            all_moved_within_footprint &= (before[vertex] - hit).norm() <= radius + 1e-5f;
        }
    CHECK(moved > 0);
    CHECK(all_moved_within_footprint);
}

TEST_CASE("Fine brush reduces small features within its footprint on coarse and dense meshes", "[MeshSmoothing][FineDetail]")
{
    for (float model_extent : {10.f, 1000.f})
        for (float step : {0.5f, 0.01f})
        for (float feature_height : {0.1f, 0.25f, 0.5f}) {
            DYNAMIC_SECTION("extent=" << model_extent << " step=" << step << " feature height=" << feature_height)
            {
                size_t seed_face = 0;
                size_t feature_vertex = 0;
                auto mesh = smoothing_feature_grid(step, feature_height, seed_face, feature_vertex);
                for (Vec3f& vertex : mesh.vertices)
                    vertex += Vec3f(model_extent * 0.5f, model_extent * 0.5f, 0.f);
                int first_remote_vertex = -1;
                if (model_extent > 10.f) {
                    first_remote_vertex = static_cast<int>(mesh.vertices.size());
                    mesh.vertices.insert(mesh.vertices.end(), {Vec3f(-500.f, -500.f, -1000.f), Vec3f(1500.f, -500.f, -1000.f),
                                                                Vec3f(-500.f, 1500.f, -1000.f)});
                    mesh.indices.emplace_back(first_remote_vertex, first_remote_vertex + 1, first_remote_vertex + 2);
                }
                const auto original = mesh;
                Transform3d world = Transform3d::Identity();
                if (model_extent > 10.f) {
                    world.translation() = Vec3d(17., -8., 3.);
                    world.linear() = Eigen::AngleAxisd(0.37, Vec3d::UnitZ()).toRotationMatrix() *
                                     Vec3d(2., 3., 4.).asDiagonal();
                }
                const Vec3f hit = original.vertices[feature_vertex];
                MeshSmoothingBrush brush(mesh, false, world);
                REQUIRE(brush.apply(mesh, seed_face, hit, world, 0.025f, 0.8f, 0.1f));
                CHECK(mesh.vertices[feature_vertex].z() < feature_height);
                CHECK(mesh.vertices[feature_vertex].z() > 0.f);
                CHECK(brush.last_statistics().max_displacement_mm <= 0.10001);
                for (size_t vertex = 0; vertex < mesh.vertices.size(); ++vertex) {
                    const Vec3d world_offset = world.linear() * (original.vertices[vertex] - hit).cast<double>();
                    const Vec3d world_displacement = world.linear() * (mesh.vertices[vertex] - original.vertices[vertex]).cast<double>();
                    if (world_offset.norm() > 0.02501) {
                        CHECK(mesh.vertices[vertex] == original.vertices[vertex]);
                    } else {
                        CHECK(world_displacement.norm() <= 0.10001);
                    }
                }
                if (first_remote_vertex >= 0) {
                    CHECK(mesh.vertices[first_remote_vertex] == original.vertices[first_remote_vertex]);
                    CHECK(mesh.vertices[first_remote_vertex + 1] == original.vertices[first_remote_vertex + 1]);
                    CHECK(mesh.vertices[first_remote_vertex + 2] == original.vertices[first_remote_vertex + 2]);
                }
            }
        }
}

TEST_CASE("Smoothing preserves subdivided colour painting and restorable model state", "[MeshSmoothing][MMUPaint]")
{
    Model            model;
    ModelObject*     object = model.add_object();
    ModelVolume*     volume = object->add_volume(TriangleMesh(bump()), ModelVolumeType::MODEL_PART, false);
    TriangleSelector painting(volume->mesh());
    const auto       colour = static_cast<EnforcerBlockerType>(33);
    painting.set_facet(0, static_cast<EnforcerBlockerType>(2));
    painting.select_patch(0,
                          TriangleSelector::SinglePointCursor::cursor_factory(Vec3f(0, -1, 0.5f), Vec3f(0, -1, 10), 0.4f,
                                                                              TriangleSelector::SPHERE, Transform3d::Identity(),
                                                                              TriangleSelector::ClippingPlane()),
                          colour, Transform3d::Identity(), true);
    REQUIRE(volume->mmu_segmentation_facets.set(painting));
    const auto annotations = volume->mmu_segmentation_facets.get_data();
    REQUIRE(painting.num_facets(colour) > 0);
    REQUIRE(annotations.bitstream.size() > 8); // Includes a subdivision tree, not just whole-face paint.
    Model              original(model);
    auto               mesh = volume->mesh().its;
    MeshSmoothingBrush brush(mesh);
    REQUIRE(brush.apply(mesh, 0, mesh.vertices[0], Transform3d::Identity(), 4.f, 0.5f));
    volume->set_mesh(std::move(mesh));
    volume->calculate_convex_hull();
    volume->invalidate_convex_hull_2d();
    volume->set_new_unique_id();
    object->invalidate_bounding_box();
    CHECK(volume->mmu_segmentation_facets.get_data() == annotations);
    TriangleSelector restored(volume->mesh());
    restored.deserialize(annotations);
    CHECK(restored.serialize() == annotations);
    CHECK(restored.num_facets(colour) == painting.num_facets(colour));
    CHECK(original.objects[0]->volumes[0]->mesh().its.vertices[0].z() == 1.f);
    CHECK(original.objects[0]->volumes[0]->mmu_segmentation_facets.get_data() == annotations);
}

TEST_CASE("Mesh smoothing benchmark: 100k faces", "[.][MeshSmoothingBenchmark]")
{
    run_smoothing_scaling_benchmark(100000);
}

TEST_CASE("Mesh smoothing benchmark: 1M faces", "[.][MeshSmoothingBenchmark]")
{
    run_smoothing_scaling_benchmark(1000000);
}

TEST_CASE("Mesh smoothing benchmark: 5M faces", "[.][MeshSmoothingBenchmark]")
{
    run_smoothing_scaling_benchmark(5000000);
}
