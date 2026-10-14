#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <vector>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Geometry.hpp"

using namespace Slic3r;

namespace {

indexed_triangle_set painting_grid(int side)
{
    indexed_triangle_set its;
    for (int y = 0; y <= side; ++y)
        for (int x = 0; x <= side; ++x)
            its.vertices.emplace_back(float(x), float(y), 0.f);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            const int a = y * (side + 1) + x;
            its.indices.emplace_back(a, a + 1, a + side + 1);
            its.indices.emplace_back(a + side + 1, a + 1, a + side + 2);
        }
    return its;
}

class PaintingUpdateSelector : public TriangleSelector
{
public:
    using TriangleSelector::TriangleSelector;
    std::set<int> touched_facets;

    // Reproduce the former exact-sized storage for the opt-in first-stroke benchmark.
    void discard_subdivision_headroom()
    {
        m_triangles.shrink_to_fit();
        m_vertices.shrink_to_fit();
    }

    // Mirror the renderer's traversal of a stable original triangle and its leaves.
    std::vector<float> render_data(int facet_idx) const
    {
        std::vector<float> data;
        append_render_data(facet_idx, data);
        return data;
    }

protected:
    void on_triangle_painted(int facet_idx) override { touched_facets.insert(facet_idx); }

private:
    void append_render_data(int facet_idx, std::vector<float>& data) const
    {
        const Triangle& triangle = m_triangles[facet_idx];
        if (!triangle.valid())
            return;
        if (triangle.is_split()) {
            for (int i = 0; i <= triangle.number_of_split_sides(); ++i)
                append_render_data(triangle.children[i], data);
            return;
        }
        data.push_back(float(triangle.get_state()));
        for (int vertex_idx : triangle.verts_idxs) {
            const Vec3f& vertex = m_vertices[vertex_idx].v;
            data.insert(data.end(), {vertex.x(), vertex.y(), vertex.z()});
        }
    }
};

void paint_grid_cell(TriangleSelector& selector, int side, int x, int y, EnforcerBlockerType state, bool splitting = true)
{
    const Vec3f hit(float(x) + 0.25f, float(y) + 0.25f, 0.f);
    auto        cursor = TriangleSelector::SinglePointCursor::cursor_factory(hit, Vec3f(0.f, 0.f, 100.f), 0.6f, TriangleSelector::SPHERE,
                                                                             Transform3d::Identity(), TriangleSelector::ClippingPlane());
    selector.select_patch(2 * (y * side + x), std::move(cursor), state, Transform3d::Identity(), splitting);
}

} // namespace

TEST_CASE("Sparse brush updates cover all changed leaves on a dense mesh", "[TriangleSelector][PaintingUpdates]")
{
    constexpr int                   side = 192; // 73,728 triangles spanning several render chunks.
    TriangleMesh                    mesh(painting_grid(side));
    PaintingUpdateSelector          selector(mesh);
    std::vector<std::vector<float>> cached_geometry;
    for (int facet = 0; facet < int(mesh.its.indices.size()); ++facet)
        cached_geometry.push_back(selector.render_data(facet));

    const auto apply_and_check = [&](int x, int y, EnforcerBlockerType state) {
        selector.touched_facets.clear();
        paint_grid_cell(selector, side, x, y, state);
        REQUIRE_FALSE(selector.touched_facets.empty());
        CHECK(selector.touched_facets.size() < 32);
        for (int facet : selector.touched_facets)
            cached_geometry[facet] = selector.render_data(facet);
        for (int facet = 0; facet < int(mesh.its.indices.size()); ++facet) {
            if (cached_geometry[facet] != selector.render_data(facet)) {
                FAIL("Paint changed an original triangle without notifying its renderer: " << facet);
            }
        }
    };

    apply_and_check(10, 10, EnforcerBlockerType::Extruder1);
    apply_and_check(130, 100, EnforcerBlockerType::Extruder2);
    // Revisit a previously traversed area, including subdivision storage reuse.
    apply_and_check(10, 10, static_cast<EnforcerBlockerType>(33));
    selector.garbage_collect();
    for (int facet = 0; facet < int(mesh.its.indices.size()); ++facet)
        REQUIRE(cached_geometry[facet] == selector.render_data(facet));
    apply_and_check(10, 10, EnforcerBlockerType::NONE);
}

TEST_CASE("Brush traversal can revisit rejected and previously painted triangles", "[TriangleSelector][PaintingUpdates]")
{
    constexpr int          side = 8;
    TriangleMesh           mesh(painting_grid(side));
    PaintingUpdateSelector selector(mesh);
    paint_grid_cell(selector, side, 3, 3, EnforcerBlockerType::Extruder1, false);
    const int painted_count = selector.num_facets(EnforcerBlockerType::Extruder1);
    REQUIRE(painted_count > 0);

    selector.set_replace_filter({EnforcerBlockerType::Extruder2});
    paint_grid_cell(selector, side, 3, 3, EnforcerBlockerType::Extruder3, false);
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder1) == painted_count);
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder3) == 0);

    selector.clear_replace_filter();
    paint_grid_cell(selector, side, 3, 3, EnforcerBlockerType::Extruder2, false);
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder1) == 0);
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder2) == painted_count);
}

TEST_CASE("Fill contours survive selecting, clearing, and resetting", "[TriangleSelector][PaintingUpdates]")
{
    TriangleMesh     mesh(painting_grid(8));
    TriangleSelector selector(mesh);
    CHECK(selector.get_seed_fill_contour().empty());
    const Vec3f hit(0.25f, 0.25f, 0.f);

    selector.bucket_fill_select_triangles(hit, 0, TriangleSelector::ClippingPlane(), -1.f, false, true);
    const auto contour = selector.get_seed_fill_contour();
    REQUIRE_FALSE(contour.empty());
    selector.seed_fill_unselect_all_triangles();
    CHECK(selector.get_seed_fill_contour().empty());

    selector.seed_fill_select_triangles(hit, 0, Transform3d::Identity(), TriangleSelector::ClippingPlane(), 30.f);
    selector.seed_fill_apply_on_triangles(EnforcerBlockerType::Extruder2);
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder2) == int(mesh.its.indices.size()));
    selector.reset();
    CHECK(selector.get_seed_fill_contour().empty());

    selector.bucket_fill_select_triangles(hit, 0, TriangleSelector::ClippingPlane(), -1.f, false, true);
    CHECK(selector.get_seed_fill_contour() == contour);
}

TEST_CASE("Cleared fill selection cannot reappear when a painted parent becomes a leaf", "[TriangleSelector][PaintingUpdates]")
{
    TriangleMesh     mesh(painting_grid(8));
    TriangleSelector selector(mesh);
    const Vec3f      hit(0.25f, 0.25f, 0.f);
    selector.bucket_fill_select_triangles(hit, 0, TriangleSelector::ClippingPlane(), -1.f, false, false);
    REQUIRE_FALSE(selector.get_seed_fill_contour().empty());

    paint_grid_cell(selector, 8, 0, 0, EnforcerBlockerType::Extruder2);
    selector.seed_fill_unselect_all_triangles();
    selector.set_facet(0, EnforcerBlockerType::Extruder1);
    selector.bucket_fill_select_triangles(hit, 0, TriangleSelector::ClippingPlane(), -1.f, false, false);
    CHECK_FALSE(selector.get_seed_fill_contour().empty());
}

TEST_CASE("Single triangle fill updates stay local after subdivision and compaction", "[TriangleSelector][PaintingUpdates]")
{
    constexpr int          side = 192;
    TriangleMesh           mesh(painting_grid(side));
    PaintingUpdateSelector selector(mesh);
    paint_grid_cell(selector, side, 10, 10, EnforcerBlockerType::Extruder1);
    selector.garbage_collect();
    selector.touched_facets.clear();

    const int facet = 2 * (10 * side + 10);
    selector.bucket_fill_select_triangles(Vec3f(10.25f, 10.25f, 0.f), facet, TriangleSelector::ClippingPlane(), -1.f, false, true);
    REQUIRE_FALSE(selector.get_seed_fill_contour().empty());
    selector.seed_fill_apply_on_triangles(EnforcerBlockerType::Extruder2);
    CHECK(selector.touched_facets == std::set<int>{facet});
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder2) == 1);
    selector.garbage_collect();
    selector.seed_fill_unselect_all_triangles();
    CHECK(selector.get_seed_fill_contour().empty());

    selector.touched_facets.clear();
    selector.seed_fill_apply_on_triangles(EnforcerBlockerType::Extruder3);
    CHECK(selector.touched_facets.empty());
    CHECK_FALSE(selector.has_facets(EnforcerBlockerType::Extruder3));
}

TEST_CASE("Overhang painting uses the transformed normal", "[TriangleSelector][PaintingUpdates]")
{
    TriangleMesh mesh(painting_grid(4));
    Transform3d  transform = Transform3d::Identity();
    transform.linear() = Eigen::AngleAxisd(Geometry::deg2rad(150.), Vec3d::UnitX()).toRotationMatrix() * Vec3d(2., 1., 0.5).asDiagonal();

    const auto paint = [&](TriangleSelector& selector, float overhang_angle) {
        auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f(0.25f, 0.25f, 0.f), Vec3f(0.f, 0.f, 100.f), 100.f,
                                                                          TriangleSelector::SPHERE, transform,
                                                                          TriangleSelector::ClippingPlane());
        selector.select_patch(0, std::move(cursor), EnforcerBlockerType::ENFORCER, transform, false, overhang_angle);
    };
    TriangleSelector accepted(mesh);
    paint(accepted, 45.f);
    CHECK(accepted.num_facets(EnforcerBlockerType::ENFORCER) == int(mesh.its.indices.size()));
    TriangleSelector rejected(mesh);
    paint(rejected, 10.f);
    CHECK_FALSE(rejected.has_facets(EnforcerBlockerType::ENFORCER));
    paint(rejected, 0.f);
    CHECK(rejected.num_facets(EnforcerBlockerType::ENFORCER) == int(mesh.its.indices.size()));
}

TEST_CASE("Dense mesh painting CPU benchmark", "[.][TriangleSelector][PaintingBenchmark]")
{
    constexpr int          side = 700;
    TriangleMesh           mesh(painting_grid(side)); // 980,000 original triangles.
    PaintingUpdateSelector exact_storage(mesh);
    exact_storage.discard_subdivision_headroom();
    PaintingUpdateSelector selector(mesh);
    using Clock        = std::chrono::steady_clock;
    const auto time_ms = [](const auto& operation) {
        const auto start = Clock::now();
        operation();
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };

    const double exact_first_ms    = time_ms([&] { paint_grid_cell(exact_storage, side, 10, 10, EnforcerBlockerType::Extruder1); });
    const double reserved_first_ms = time_ms([&] { paint_grid_cell(selector, side, 10, 10, EnforcerBlockerType::Extruder1); });
    CHECK(exact_storage.render_data(2 * (10 * side + 10)) == selector.render_data(2 * (10 * side + 10)));

    constexpr int iterations = 200;
    const double  brush_ms   = time_ms([&] {
        for (int i = 0; i < iterations; ++i)
            paint_grid_cell(selector, side, 100 + i % 20, 100 + i / 20, EnforcerBlockerType::Extruder2, false);
    });
    const double  pointer_ms = time_ms([&] {
        for (int i = 0; i < iterations; ++i) {
            selector.bucket_fill_select_triangles(Vec3f(0.25f, 0.25f, 0.f), 0, TriangleSelector::ClippingPlane(), -1.f, false, true);
            selector.seed_fill_apply_on_triangles(EnforcerBlockerType::Extruder3);
            REQUIRE_FALSE(selector.get_seed_fill_contour().empty());
            selector.seed_fill_unselect_all_triangles();
        }
    });
    std::cout << "980000-triangle CPU benchmark (excludes raycast and OpenGL):\n"
              << "  First subdividing stroke, exact storage: " << exact_first_ms << " ms\n"
              << "  First subdividing stroke, reserved storage: " << reserved_first_ms << " ms\n"
              << "  Local brush average: " << brush_ms / iterations << " ms\n"
              << "  Single-triangle fill/apply/contour/clear average: " << pointer_ms / iterations << " ms\n";
    CHECK(selector.num_facets(EnforcerBlockerType::Extruder3) == 1);
}
