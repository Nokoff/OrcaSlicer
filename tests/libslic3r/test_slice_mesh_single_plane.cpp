#include <catch2/catch_test_macros.hpp>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>
#include <libslic3r/Geometry.hpp>

using namespace Slic3r;

static void require_same_slices(const indexed_triangle_set &its, const Transform3d &trafo)
{
    MeshSlicingParams params;
    params.trafo = trafo;
    TriangleMesh mesh(its);
    BoundingBoxf3 bb = mesh.transformed_bounding_box(trafo);
    const int     steps = 37;
    for (int i = -1; i <= steps + 1; ++ i) {
        float z = float(bb.min.z() + (bb.max.z() - bb.min.z()) * double(i) / double(steps));
        Polygons expected = detail::slice_mesh_single_plane_reference(its, z, params);
        Polygons actual   = slice_mesh(its, z, params);
        REQUIRE(actual.size() == expected.size());
        for (size_t j = 0; j < actual.size(); ++ j)
            REQUIRE(actual[j].points == expected[j].points);
    }
    // Planes exactly through vertices.
    for (size_t i = 0; i < its.vertices.size(); i += std::max<size_t>(1, its.vertices.size() / 20)) {
        float z = trafo.matrix().isIdentity() ? its.vertices[i].z() : float((trafo * its.vertices[i].cast<double>()).z());
        Polygons expected = detail::slice_mesh_single_plane_reference(its, z, params);
        Polygons actual   = slice_mesh(its, z, params);
        REQUIRE(actual.size() == expected.size());
        for (size_t j = 0; j < actual.size(); ++ j)
            REQUIRE(actual[j].points == expected[j].points);
    }
}

TEST_CASE("Single plane slice_mesh matches reference implementation", "[TriangleMeshSlicer]")
{
    std::vector<std::pair<std::string, indexed_triangle_set>> meshes {
        { "cube",     its_make_cube(20., 20., 20.) },
        { "cylinder", its_make_cylinder(10., 30.) },
        { "sphere",   its_make_sphere(15., PI / 40.) },
        { "cone",     its_make_cone(12., 25.) },
    };
    // Two disjoint parts in one mesh.
    {
        indexed_triangle_set two = its_make_sphere(8., PI / 20.);
        indexed_triangle_set cube = its_make_cube(10., 10., 10.);
        its_translate(cube, Vec3f(30.f, 0.f, -5.f));
        its_merge(two, cube);
        meshes.emplace_back("two_parts", std::move(two));
    }

    Transform3d rotated = Geometry::assemble_transform(Vec3d(3., -7., 11.), Vec3d(0.4, -0.3, 1.1), Vec3d(1.2, 0.8, 1.5));
    Transform3d mirrored = Geometry::assemble_transform(Vec3d(-2., 5., 1.), Vec3d(0.2, 0.5, -0.7), Vec3d(1., 1., 1.), Vec3d(-1., 1., 1.));
    Transform3d mirrored_z = Geometry::assemble_transform(Vec3d::Zero(), Vec3d(0., 0., 0.3), Vec3d(1., 1., 1.), Vec3d(1., 1., -1.));

    for (const auto &[name, its] : meshes) {
        SECTION(name + " identity")   { require_same_slices(its, Transform3d::Identity()); }
        SECTION(name + " rotated")    { require_same_slices(its, rotated); }
        SECTION(name + " mirrored")   { require_same_slices(its, mirrored); }
        SECTION(name + " mirrored z") { require_same_slices(its, mirrored_z); }
    }
}
