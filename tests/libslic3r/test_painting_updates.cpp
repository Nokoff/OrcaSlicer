#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <vector>

#include <tbb/global_control.h>
#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>
#include <sstream>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"

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

    bool cached_neighbors_match_fresh()
    {
        const auto  fresh  = this->precompute_all_neighbors();
        const auto& cached = this->all_neighbors();
        return cached.first == fresh.first && cached.second == fresh.second;
    }

    size_t leaf_count() const
    {
        size_t count = 0;
        for (const Triangle& triangle : m_triangles)
            if (triangle.valid() && !triangle.is_split())
                ++count;
        return count;
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

TEST_CASE("Paint export preserves hexadecimal nibble order and triangle boundaries", "[PaintingSave]")
{
    TriangleSelector::TriangleSplittingData data;
    data.triangles_to_split.emplace_back(2, 0);
    data.triangles_to_split.emplace_back(7, 64);
    // First triangle contains every hex digit; the second is a large split tree payload.
    constexpr size_t digits = 65536;
    for (size_t i = 0; i < 16 + digits; ++i)
        for (int bit = 0; bit < 4; ++bit)
            data.bitstream.push_back(((i % 16) >> bit) & 1);

    Model        model;
    ModelVolume* volume     = model.add_object()->add_volume(TriangleMesh(painting_grid(1)));
    auto&        annotation = volume->mmu_segmentation_facets;
    annotation.set_data(std::move(data));
    CHECK(annotation.get_triangle_as_string(0).empty());
    CHECK(annotation.get_triangle_as_string(3).empty());
    CHECK(annotation.get_triangle_as_string(8).empty());
    CHECK(annotation.get_triangle_as_string(2) == "FEDCBA9876543210");
    std::string expected;
    for (size_t i = 0; i < digits / 16; ++i)
        expected += "FEDCBA9876543210";
    CHECK(annotation.get_triangle_as_string(7) == expected);
}

TEST_CASE("Paint thumbnail partitions preserve per-color geometry", "[TriangleSelector][PaintingSave]")
{
    TriangleMesh     mesh(painting_grid(12));
    TriangleSelector selector(mesh);
    const auto       high_state = static_cast<EnforcerBlockerType>(120);
    for (int i = 0; i < int(mesh.its.indices.size()); ++i)
        if (i % 3 != 0)
            selector.set_facet(i, i % 3 == 1 ? EnforcerBlockerType::Extruder1 : high_state);
    paint_grid_cell(selector, 12, 6, 6, EnforcerBlockerType::Extruder2);

    std::vector<indexed_triangle_set> partitions;
    selector.get_facets(partitions);
    REQUIRE(partitions.size() == 121);
    for (size_t state = 0; state < partitions.size(); ++state) {
        const auto expected = selector.get_facets(static_cast<EnforcerBlockerType>(state));
        CHECK(partitions[state].vertices == expected.vertices);
        CHECK(partitions[state].indices == expected.indices);
    }
    selector.reset();
    selector.get_facets(partitions);
    REQUIRE(partitions.size() == 1);
    CHECK(partitions.front().indices.size() == mesh.its.indices.size());
}

TEST_CASE("Benchmark painting save preparation", "[.benchmark][PaintingSave]")
{
    TriangleMesh     mesh(painting_grid(384));
    TriangleSelector selector(mesh);
    for (int i = 0; i < int(mesh.its.indices.size()); ++i)
        selector.set_facet(i, static_cast<EnforcerBlockerType>(i % 2 ? 120 : 1));

    const auto                        start = std::chrono::steady_clock::now();
    std::vector<indexed_triangle_set> previous;
    for (int state = 0; state <= 120; ++state)
        previous.push_back(selector.get_facets(static_cast<EnforcerBlockerType>(state)));
    const auto                        grouped_start = std::chrono::steady_clock::now();
    std::vector<indexed_triangle_set> grouped;
    selector.get_facets(grouped);
    const auto grouped_end = std::chrono::steady_clock::now();
    REQUIRE(previous.size() == grouped.size());
    for (size_t i = 0; i < grouped.size(); ++i) {
        REQUIRE(previous[i].vertices == grouped[i].vertices);
        REQUIRE(previous[i].indices == grouped[i].indices);
    }

    Model model;
    auto& annotation = model.add_object()->add_volume(TriangleMesh(painting_grid(1)))->mmu_segmentation_facets;
    TriangleSelector::TriangleSplittingData data;
    data.triangles_to_split.emplace_back(0, 0);
    data.bitstream.resize(4 * 262144);
    for (size_t i = 0; i < data.bitstream.size(); ++i)
        data.bitstream[i] = i % 3 == 0;
    annotation.set_data(std::move(data));
    const auto& bits         = annotation.get_data().bitstream;
    const auto  encode_start = std::chrono::steady_clock::now();
    std::string previous_encoding;
    for (size_t offset = 0; offset < bits.size(); offset += 4) {
        int code = 0;
        for (int bit = 3; bit >= 0; --bit)
            code = (code << 1) | int(bits[offset + bit]);
        previous_encoding.insert(previous_encoding.begin(), "0123456789ABCDEF"[code]);
    }
    const auto encode_new_start = std::chrono::steady_clock::now();
    const auto encoding         = annotation.get_triangle_as_string(0);
    const auto encode_end       = std::chrono::steady_clock::now();
    REQUIRE(encoding == previous_encoding);
    const auto ms = [](auto elapsed) { return std::chrono::duration<double, std::milli>(elapsed).count(); };
    std::cout << "Painting thumbnail mesh: previous=" << ms(grouped_start - start) << " ms optimized=" << ms(grouped_end - grouped_start)
              << " ms\n"
              << "Painting hex export: previous=" << ms(encode_new_start - encode_start)
              << " ms optimized=" << ms(encode_end - encode_new_start) << " ms\n";
}

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

TEST_CASE("Cached neighbors follow changes of the triangle tree", "[TriangleSelector][PaintingUpdates]")
{
    constexpr int          side = 40;
    TriangleMesh           mesh(painting_grid(side));
    PaintingUpdateSelector selector(mesh);
    REQUIRE(selector.cached_neighbors_match_fresh());

    for (int i = 0; i < 30; ++i) {
        INFO("Iteration " << i);
        // Subdivide.
        paint_grid_cell(selector, side, (i * 7) % side, (i * 11) % side, EnforcerBlockerType(1 + i % 3));
        REQUIRE(selector.cached_neighbors_match_fresh());
        // Paint a whole cell without splitting, which removes the children painted before.
        paint_grid_cell(selector, side, (i * 7) % side, (i * 11) % side, EnforcerBlockerType(1 + i % 3), false);
        REQUIRE(selector.cached_neighbors_match_fresh());
        if (i % 4 == 0) {
            const Vec3f hit(float((i * 7) % side) + 0.25f, float((i * 11) % side) + 0.25f, 0.f);
            selector.bucket_fill_select_triangles(hit, 2 * (((i * 11) % side) * side + (i * 7) % side), TriangleSelector::ClippingPlane(), -1.f, true, true);
            selector.seed_fill_apply_on_triangles(EnforcerBlockerType(4));
            selector.seed_fill_unselect_all_triangles();
            REQUIRE(selector.cached_neighbors_match_fresh());
        }
        if (i % 10 == 9) {
            selector.garbage_collect();
            REQUIRE(selector.cached_neighbors_match_fresh());
            const TriangleSelector::TriangleSplittingData data = selector.serialize();
            selector.deserialize(data);
            REQUIRE(selector.cached_neighbors_match_fresh());
        }
    }
}

TEST_CASE("Brush edge limit scales with the brush radius", "[TriangleSelector][PaintingUpdates]")
{
    constexpr int side = 8;
    TriangleMesh  mesh(painting_grid(side));
    const auto    leaves_after_stroke = [&](float radius) {
        PaintingUpdateSelector selector(mesh);
        auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(Vec3f(4.1f, 4.2f, 0.f), Vec3f(0.f, 0.f, 100.f), radius,
                                                                         TriangleSelector::SPHERE, Transform3d::Identity(),
                                                                         TriangleSelector::ClippingPlane());
        selector.select_patch(2 * (4 * side + 4), std::move(cursor), EnforcerBlockerType::Extruder1, Transform3d::Identity(), true);
        return selector.leaf_count();
    };
    // Twice the radius doubles the boundary length but also doubles the allowed edge length, up to the 0.2 mm cap.
    const size_t small  = leaves_after_stroke(0.5f);
    const size_t medium = leaves_after_stroke(1.f);
    const size_t large  = leaves_after_stroke(2.f);
    CHECK(small > 128);
    CHECK(medium > 128);
    CHECK(large > 128);
    CHECK(medium < 2 * small);
    CHECK(large < 2 * medium);
}

TEST_CASE("Height range painting does not depend on the thread count", "[TriangleSelector][PaintingUpdates]")
{
    TriangleMesh mesh(its_make_sphere(10., PI / 60.));
    const auto paint = [&](bool full_depth) {
        TriangleSelector selector(mesh);
        auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(2.f, Vec3f(0.f, -50.f, 2.f), 1.5f, Transform3d::Identity(),
                                                                         TriangleSelector::ClippingPlane(), full_depth);
        selector.select_patch(0, std::move(cursor), EnforcerBlockerType::Extruder2, Transform3d::Identity(), true);
        return selector.serialize();
    };
    for (bool full_depth : {false, true}) {
        DYNAMIC_SECTION("full depth " << full_depth) {
            TriangleSelector::TriangleSplittingData serial;
            {
                tbb::global_control single_thread(tbb::global_control::max_allowed_parallelism, 1);
                serial = paint(full_depth);
            }
            const TriangleSelector::TriangleSplittingData parallel = paint(full_depth);
            REQUIRE_FALSE(serial.triangles_to_split.empty());
            CHECK(serial.triangles_to_split == parallel.triangles_to_split);
            CHECK(serial.bitstream == parallel.bitstream);
        }
    }
}

TEST_CASE("Painting data survives the undo stack encoding", "[TriangleSelector][PaintingUpdates]")
{
    constexpr int          side = 60;
    TriangleMesh           mesh(painting_grid(side));
    PaintingUpdateSelector selector(mesh);
    for (int i = 0; i < 40; ++i)
        paint_grid_cell(selector, side, (i * 7) % side, (i * 13) % side, EnforcerBlockerType(1 + i % 5));
    const TriangleSelector::TriangleSplittingData data = selector.serialize();
    REQUIRE_FALSE(data.bitstream.empty());

    std::stringstream stream;
    {
        cereal::BinaryOutputArchive archive(stream);
        archive(data);
    }
    TriangleSelector::TriangleSplittingData loaded;
    {
        cereal::BinaryInputArchive archive(stream);
        archive(loaded);
    }
    CHECK(loaded == data);
    // Packed bits take an eighth of a byte each, plus the 8-byte mapping per split triangle.
    CHECK(stream.str().size() < data.bitstream.size() / 4 + 8 * data.triangles_to_split.size() + 256);
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

TEST_CASE("Dense mesh painting tool benchmark", "[.][TriangleSelector][PaintingBenchmark]")
{
    constexpr int side = 700;
    TriangleMesh  mesh(painting_grid(side)); // 980,000 original triangles.
    using Clock        = std::chrono::steady_clock;
    const auto time_ms = [](const auto& operation) {
        const auto start = Clock::now();
        operation();
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    const Vec3f camera(0.f, 0.f, 100.f);

    // A large brush dragged across the mesh, sampled every 0.25 mm like a mouse drag.
    PaintingUpdateSelector selector(mesh);
    const double large_brush_ms = time_ms([&] {
        for (int i = 0; i < 400; ++i) {
            const Vec3f a(50.f + 0.25f * i, 50.25f, 0.f);
            const Vec3f b(50.25f + 0.25f * i, 50.25f, 0.f);
            const int   facet = 2 * (50 * side + int(a.x()));
            auto cursor = TriangleSelector::DoublePointCursor::cursor_factory(a, b, camera, 4.f, TriangleSelector::SPHERE,
                                                                             Transform3d::Identity(), TriangleSelector::ClippingPlane());
            selector.select_patch(facet, std::move(cursor), EnforcerBlockerType::Extruder2, Transform3d::Identity(), true);
        }
    });
    const size_t leaves = selector.leaf_count();
    TriangleSelector::TriangleSplittingData serialized;
    const double serialize_ms = time_ms([&] { serialized = selector.serialize(); });
    size_t       undo_bytes   = 0;
    const double undo_ms      = time_ms([&] {
        std::stringstream stream;
        cereal::BinaryOutputArchive archive(stream);
        archive(serialized);
        undo_bytes = stream.str().size();
    });

    // Magic fill on the subdivided mesh: one hover and a simulated 50-sample drag.
    const Vec3f  fill_hit(300.25f, 300.25f, 0.f);
    const int    fill_facet = 2 * (300 * side + 300);
    const double magic_hover_ms = time_ms([&] {
        selector.bucket_fill_select_triangles(fill_hit, fill_facet, TriangleSelector::ClippingPlane(), -1.f, true, true);
    });
    selector.seed_fill_unselect_all_triangles();
    const double magic_drag_ms = time_ms([&] {
        // Mirrors the gizmo: an unchanged selection is applied once, and a sample inside the region that was just
        // painted skips the region search.
        uint64_t applied = std::numeric_limits<uint64_t>::max();
        for (int i = 0; i < 50; ++i) {
            if (applied != selector.seed_fill_revision()) {
                selector.seed_fill_apply_on_triangles(EnforcerBlockerType::Extruder3);
                applied = selector.seed_fill_revision();
            }
            if (!selector.is_selected_by_seed_fill(fill_hit, fill_facet))
                selector.bucket_fill_select_triangles(fill_hit, fill_facet, TriangleSelector::ClippingPlane(), -1.f, true, true);
        }
    });
    selector.seed_fill_unselect_all_triangles();

    // Full-depth height band covering the whole flat mesh.
    PaintingUpdateSelector height_selector(mesh);
    const double height_range_ms = time_ms([&] {
        auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(-0.5f, camera, 1.f, Transform3d::Identity(),
                                                                         TriangleSelector::ClippingPlane(), true);
        height_selector.select_patch(0, std::move(cursor), EnforcerBlockerType::Extruder4, Transform3d::Identity(), true);
    });

    std::cout << "980000-triangle tool benchmark (excludes raycast and OpenGL):" << std::endl
              << "  Large brush (r=4) 400-sample stroke: " << large_brush_ms << " ms, leaf triangles: " << leaves << std::endl
              << "  Magic fill hover on subdivided mesh: " << magic_hover_ms << " ms" << std::endl
              << "  Magic fill 50-sample drag: " << magic_drag_ms << " ms" << std::endl
              << "  Full-depth height range stroke: " << height_range_ms << " ms" << std::endl
              << "  Serialize after the large brush stroke: " << serialize_ms << " ms" << std::endl
              << "  Undo stack encoding of it: " << undo_ms << " ms, " << undo_bytes << " bytes" << std::endl;
    CHECK(height_selector.num_facets(EnforcerBlockerType::Extruder4) == int(mesh.its.indices.size()));
}
