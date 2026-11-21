#include <catch2/catch_test_macros.hpp>
#include <test_utils.hpp>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/AABBTreeIndirect.hpp>

#include <algorithm>

using namespace Slic3r;

TEST_CASE("Building a tree over a box, ray caster and closest query", "[AABBIndirect]")
{
    TriangleMesh tmesh = make_cube(1., 1., 1.);

    auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(tmesh.its.vertices, tmesh.its.indices);
    REQUIRE(! tree.empty());
    std::vector<size_t> reached_faces;
    const AABBTreeIndirect::Tree3f::BoundingBox all_faces(Vec3f::Constant(-2.f), Vec3f::Constant(2.f));
    AABBTreeIndirect::traverse(tree, AABBTreeIndirect::within(all_faces), [&](const auto& node) {
        reached_faces.push_back(node.idx);
        return true;
    });
    std::sort(reached_faces.begin(), reached_faces.end());
    REQUIRE(reached_faces.size() == tmesh.its.indices.size());
    for (size_t face = 0; face < reached_faces.size(); ++face)
        CHECK(reached_faces[face] == face);

    igl::Hit hit;
	bool intersected = AABBTreeIndirect::intersect_ray_first_hit(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
		Vec3d(0.5, 0.5, -5.),
		Vec3d(0., 0., 1.),
		hit);

    REQUIRE(intersected);
    REQUIRE_THAT(hit.t, WithinRel(5., 0.001));

    std::vector<igl::Hit> hits;
	bool intersected2 = AABBTreeIndirect::intersect_ray_all_hits(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, -5.),
		Vec3d(0., 0., 1.),
		hits);
    REQUIRE(intersected2);
    REQUIRE(hits.size() == 2);
    REQUIRE_THAT(hits.front().t, WithinRel(5., 0.001));
    REQUIRE_THAT(hits.back().t, WithinRel(6., 0.001));

    size_t hit_idx;
    Vec3d  closest_point;
    double squared_distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, -5.),
		hit_idx, closest_point);
    REQUIRE_THAT(squared_distance, WithinRel(5. * 5., 0.001));
    REQUIRE_THAT(closest_point.x(), WithinRel(0.3, 0.001));
    REQUIRE_THAT(closest_point.y(), WithinRel(0.5, 0.001));
    REQUIRE_THAT(closest_point.z(), WithinRel(0., 0.001));

    squared_distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, 5.),
		hit_idx, closest_point);
    REQUIRE_THAT(squared_distance, WithinRel(4. * 4., 0.001));
    REQUIRE_THAT(closest_point.x(), WithinRel(0.3, 0.001));
    REQUIRE_THAT(closest_point.y(), WithinRel(0.5, 0.001));
    REQUIRE_THAT(closest_point.z(), WithinRel(1., 0.001));
}

TEST_CASE("Parallel AABB tree build preserves ray and closest queries", "[AABBIndirect]")
{
    constexpr int side = 201;
    indexed_triangle_set mesh;
    mesh.vertices.reserve(side * side);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
            mesh.vertices.emplace_back(float(x), float(y), 0.f);

    mesh.indices.reserve(size_t(side - 1) * size_t(side - 1) * 2);
    for (int y = 0; y < side - 1; ++y)
        for (int x = 0; x < side - 1; ++x) {
            const int a = y * side + x;
            const int b = a + 1;
            const int c = a + side;
            const int d = c + 1;
            mesh.indices.emplace_back(a, b, d);
            mesh.indices.emplace_back(a, d, c);
        }

    auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.vertices, mesh.indices);
    REQUIRE(!tree.empty());

    igl::Hit hit;
    REQUIRE(AABBTreeIndirect::intersect_ray_first_hit(
        mesh.vertices, mesh.indices, tree, Vec3d(40.25, 70.25, 10.), Vec3d(0., 0., -1.), hit));
    CHECK_THAT(hit.t, WithinRel(10., 0.001));

    size_t hit_idx = 0;
    Vec3d closest_point;
    const double squared_distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
        mesh.vertices, mesh.indices, tree, Vec3d(40.25, 70.25, 3.), hit_idx, closest_point);
    CHECK_THAT(squared_distance, WithinRel(9., 0.001));
    CHECK_THAT(closest_point.z(), WithinAbs(0., 0.001));
}
