#include <catch2/catch_test_macros.hpp>

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
    REQUIRE(shell_brush.apply(mesh, 0, mesh.vertices[0], Transform3d::Identity(), 4.f, 1.f));
    for (size_t i = original.vertices.size(); i < mesh.vertices.size(); ++i)
        CHECK(mesh.vertices[i] == shells.vertices[i]);
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
