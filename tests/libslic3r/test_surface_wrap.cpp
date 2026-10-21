#include <catch2/catch_test_macros.hpp>
#include "libslic3r/SurfaceWrap.hpp"

#include <cmath>
#include <limits>
#include <map>

using namespace Slic3r;

namespace {
indexed_triangle_set centered_box(double x, double y, double z)
{
    auto mesh = its_make_cube(x, y, z);
    for (auto& v : mesh.vertices)
        v -= Vec3f(float(x / 2), float(y / 2), 0);
    return mesh;
}

void check_closed(const indexed_triangle_set& mesh)
{
    std::map<std::pair<int, int>, std::pair<int, int>> edges;
    for (const auto& f : mesh.indices) {
        CHECK((mesh.vertices[f[1]] - mesh.vertices[f[0]]).cross(mesh.vertices[f[2]] - mesh.vertices[f[0]]).norm() > 1e-8);
        for (int i = 0; i < 3; ++i) {
            int   a = f[i], b = f[(i + 1) % 3];
            auto& counts = edges[std::minmax(a, b)];
            ++counts.first;
            counts.second += a < b ? 1 : -1;
        }
    }
    for (const auto& item : edges) {
        CHECK(item.second.first == 2);
        CHECK(item.second.second == 0);
    }
    CHECK(its_volume(mesh) > 0);
}

// A tapered cylinder in the target placement frame: local Y is cylinder
// height, local Z is outward at the front, and local X runs across the relief.
indexed_triangle_set tapered_cylinder()
{
    auto mesh = its_make_cylinder(20, 40, PI / 180);
    for (auto& v : mesh.vertices) {
        const float height = v.z();
        const float taper  = 1.f - height / 80.f;
        v                  = Vec3f(v.x() * taper, height - 20.f, v.y() * taper);
    }
    // The change of axes is a reflection; restore the original winding.
    for (auto& f : mesh.indices)
        std::swap(f[1], f[2]);
    return mesh;
}
} // namespace

TEST_CASE("Surface wrap preserves relief thickness and closed topology on a flat target", "[SurfaceWrap]")
{
    const auto         source = centered_box(10, 8, 2);
    const auto         target = centered_box(40, 40, 5);
    SurfaceWrapOptions options;
    options.max_edge  = 1.25;
    options.offset    = -0.1;
    const auto result = surface_wrap(source, target, options);
    CHECK(result.indices.size() > source.indices.size());
    for (size_t i = 0; i < source.vertices.size(); ++i) {
        CHECK(std::abs(result.vertices[i].x() - source.vertices[i].x()) < 1e-5);
        CHECK(std::abs(result.vertices[i].y() - source.vertices[i].y()) < 1e-5);
        CHECK(std::abs(result.vertices[i].z() - (4.9 + source.vertices[i].z())) < 1e-5);
    }
    check_closed(result);
}

TEST_CASE("Rectangle conforms to a tapered cylindrical target with raised corners", "[SurfaceWrap]")
{
    const auto         source = centered_box(12, 16, 1.5);
    SurfaceWrapOptions options;
    options.corner_lift = 2;
    options.offset      = 0;
    options.max_edge    = 0.75;
    const auto result   = surface_wrap(source, tapered_cylinder(), options);
    for (size_t i = 0; i < source.vertices.size(); ++i) {
        const Vec3f p = result.vertices[i];
        CHECK(std::abs(p.y() - source.vertices[i].y() - 2) < 1e-5);
        const double radius   = 15 - p.y() / 4;
        const double expected = std::sqrt(radius * radius - p.x() * p.x()) + source.vertices[i].z();
        CHECK(std::abs(p.z() - expected) < 0.003);
    }
    // Added center vertices follow the cylinder too, instead of leaving the
    // original twelve cube faces as large planar facets.
    Vec3f nearest_center = result.vertices.front();
    for (const auto& p : result.vertices) {
        if ((p - Vec3f(0, 0, 15)).squaredNorm() < (nearest_center - Vec3f(0, 0, 15)).squaredNorm())
            nearest_center = p;
    }
    REQUIRE(nearest_center.head<2>().norm() < 0.5);
    const double center_radius = 15 - nearest_center.y() / 4;
    CHECK(std::abs(nearest_center.z() - std::sqrt(center_radius * center_radius - nearest_center.x() * nearest_center.x())) < 0.003);
    check_closed(result);
}

TEST_CASE("Individual corner offsets are independent and do not accumulate on the source", "[SurfaceWrap]")
{
    const auto         source   = centered_box(10, 8, 2);
    const auto         original = source;
    SurfaceWrapOptions options;
    options.corners[2] = Vec2d(-1, 2);
    const auto first   = surface_wrap(source, centered_box(40, 40, 5), options);
    const auto second  = surface_wrap(source, centered_box(40, 40, 5), options);
    CHECK(first.vertices == second.vertices);
    CHECK(source.vertices == original.vertices);
    CHECK(source.indices == original.indices);
    for (size_t i = 0; i < source.vertices.size(); ++i) {
        const Vec3f p     = source.vertices[i];
        const Vec2f delta = first.vertices[i].head<2>() - p.head<2>();
        if (p.x() > 0 && p.y() > 0)
            CHECK((delta - Vec2f(-1, 2)).norm() < 1e-5);
        else
            CHECK(delta.norm() < 1e-5);
    }
    check_closed(first);
}

TEST_CASE("Surface wrap rejects incomplete coverage, folded corners and excessive refinement", "[SurfaceWrap]")
{
    const auto         source = centered_box(10, 8, 2);
    SurfaceWrapOptions options;
    REQUIRE_THROWS(surface_wrap(source, centered_box(2, 2, 5), options));
    options.corners[0] = Vec2d(20, 20);
    REQUIRE_THROWS(surface_wrap(source, centered_box(40, 40, 5), options));
    options               = {};
    options.max_triangles = 20;
    REQUIRE_THROWS(surface_wrap(source, centered_box(40, 40, 5), options));
    options          = {};
    options.max_edge = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_THROWS(surface_wrap(source, centered_box(40, 40, 5), options));
    options             = {};
    options.corner_lift = std::numeric_limits<double>::infinity();
    REQUIRE_THROWS(surface_wrap(source, centered_box(40, 40, 5), options));
}
