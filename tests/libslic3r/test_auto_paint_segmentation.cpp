#include <catch2/catch.hpp>

#include "libslic3r/AutoPaintSegmentation.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <queue>
#include <unordered_map>

using namespace Slic3r;

namespace {

uint64_t test_edge_key(int first, int second)
{
    const uint32_t low  = static_cast<uint32_t>(std::min(first, second));
    const uint32_t high = static_cast<uint32_t>(std::max(first, second));
    return (static_cast<uint64_t>(low) << 32) | high;
}

std::vector<std::vector<size_t>> face_neighbors(const indexed_triangle_set& mesh)
{
    std::vector<std::vector<size_t>>     result(mesh.indices.size());
    std::unordered_map<uint64_t, size_t> first_face_by_edge;
    for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx) {
        const Vec3i32& face = mesh.indices[face_idx];
        for (int edge_idx = 0; edge_idx < 3; ++edge_idx) {
            const uint64_t key             = test_edge_key(face[edge_idx], face[(edge_idx + 1) % 3]);
            const auto [edge_it, inserted] = first_face_by_edge.emplace(key, face_idx);
            if (!inserted) {
                result[face_idx].push_back(edge_it->second);
                result[edge_it->second].push_back(face_idx);
            }
        }
    }
    return result;
}

bool region_is_connected(const std::vector<std::vector<size_t>>& neighbors, const std::vector<size_t>& regions, size_t region)
{
    const auto first = std::find(regions.begin(), regions.end(), region);
    if (first == regions.end())
        return false;

    std::vector<bool>  visited(regions.size(), false);
    std::queue<size_t> queue;
    queue.push(static_cast<size_t>(std::distance(regions.begin(), first)));
    visited[queue.front()] = true;
    size_t visited_count   = 0;
    while (!queue.empty()) {
        const size_t face = queue.front();
        queue.pop();
        ++visited_count;
        for (const size_t neighbor : neighbors[face]) {
            if (!visited[neighbor] && regions[neighbor] == region) {
                visited[neighbor] = true;
                queue.push(neighbor);
            }
        }
    }

    return visited_count == static_cast<size_t>(std::count(regions.begin(), regions.end(), region));
}

Vec3f face_normal(const indexed_triangle_set& mesh, size_t face_idx)
{
    const Vec3i32& face = mesh.indices[face_idx];
    return ((mesh.vertices[face[1]] - mesh.vertices[face[0]]).cross(mesh.vertices[face[2]] - mesh.vertices[face[0]])).normalized();
}

indexed_triangle_set make_rounded_dumbbell()
{
    constexpr size_t                           segments = 48;
    const std::vector<std::pair<float, float>> profile  = {
        {0.f, 0.f}, {0.2f, 2.f}, {1.5f, 4.f}, {3.5f, 4.f}, {4.8f, 1.4f}, {6.1f, 4.f}, {8.1f, 4.f}, {9.4f, 2.f}, {9.6f, 0.f},
    };

    indexed_triangle_set mesh;
    mesh.vertices.emplace_back(0.f, 0.f, profile.front().first);
    for (size_t ring = 1; ring + 1 < profile.size(); ++ring) {
        for (size_t segment = 0; segment < segments; ++segment) {
            const float angle = 2.f * float(PI) * float(segment) / float(segments);
            mesh.vertices.emplace_back(profile[ring].second * std::cos(angle), profile[ring].second * std::sin(angle), profile[ring].first);
        }
    }
    const int top_vertex = static_cast<int>(mesh.vertices.size());
    mesh.vertices.emplace_back(0.f, 0.f, profile.back().first);

    for (size_t segment = 0; segment < segments; ++segment) {
        const int current = 1 + static_cast<int>(segment);
        const int next    = 1 + static_cast<int>((segment + 1) % segments);
        mesh.indices.emplace_back(0, next, current);
    }
    for (size_t ring = 0; ring + 2 < profile.size() - 1; ++ring) {
        const int lower = 1 + static_cast<int>(ring * segments);
        const int upper = lower + static_cast<int>(segments);
        for (size_t segment = 0; segment < segments; ++segment) {
            const int current = static_cast<int>(segment);
            const int next    = static_cast<int>((segment + 1) % segments);
            mesh.indices.emplace_back(lower + current, lower + next, upper + next);
            mesh.indices.emplace_back(lower + current, upper + next, upper + current);
        }
    }
    const int last_ring = 1 + static_cast<int>((profile.size() - 3) * segments);
    for (size_t segment = 0; segment < segments; ++segment) {
        const int current = last_ring + static_cast<int>(segment);
        const int next    = last_ring + static_cast<int>((segment + 1) % segments);
        mesh.indices.emplace_back(current, next, top_vertex);
    }
    return mesh;
}

} // namespace

TEST_CASE("Auto paint handles an empty mesh", "[AutoPaint]")
{
    const AutoPaint::SegmentationResult result = AutoPaint::segment_by_geometry({});
    CHECK(result.face_regions.empty());
    CHECK(result.region_seed_faces.empty());
}

TEST_CASE("Auto paint follows the six sharp sides of a cube", "[AutoPaint]")
{
    const indexed_triangle_set     cube = its_make_cube(10., 10., 10.);
    AutoPaint::SegmentationOptions options;
    options.target_regions      = 3;
    options.boundary_preference = 1.f;

    const AutoPaint::SegmentationResult result = AutoPaint::segment_by_geometry(cube, options);

    REQUIRE(result.region_count() == 6);
    REQUIRE(result.face_regions.size() == cube.indices.size());
    for (size_t region = 0; region < result.region_count(); ++region) {
        std::vector<size_t> region_faces;
        for (size_t face_idx = 0; face_idx < result.face_regions.size(); ++face_idx)
            if (result.face_regions[face_idx] == region)
                region_faces.push_back(face_idx);
        REQUIRE(region_faces.size() == 2);
        CHECK(face_normal(cube, region_faces[0]).dot(face_normal(cube, region_faces[1])) > 0.999f);
    }

    REQUIRE(result.region_palette.size() == result.region_count());
    std::vector<bool> used_palette_entries(options.target_regions, false);
    for (const size_t palette : result.region_palette) {
        REQUIRE(palette < options.target_regions);
        used_palette_entries[palette] = true;
    }
    CHECK(std::all_of(used_palette_entries.begin(), used_palette_entries.end(), [](bool used) { return used; }));

    const std::vector<std::vector<size_t>> neighbors = face_neighbors(cube);
    for (size_t face_idx = 0; face_idx < neighbors.size(); ++face_idx)
        for (const size_t neighbor : neighbors[face_idx])
            if (result.face_regions[face_idx] != result.face_regions[neighbor])
                CHECK(result.region_palette[result.face_regions[face_idx]] != result.region_palette[result.face_regions[neighbor]]);
}

TEST_CASE("Auto paint keeps an uninterrupted rounded model together", "[AutoPaint]")
{
    const indexed_triangle_set     sphere = its_make_sphere(10., PI / 32.);
    AutoPaint::SegmentationOptions options;
    options.target_regions = 4;

    const AutoPaint::SegmentationResult result = AutoPaint::segment_by_geometry(sphere, options);

    REQUIRE(result.region_count() == 1);
    REQUIRE(result.face_regions.size() == sphere.indices.size());
    CHECK(std::all_of(result.face_regions.begin(), result.face_regions.end(), [](size_t region) { return region == 0; }));
}

TEST_CASE("Auto paint gives disconnected parts independent regions", "[AutoPaint]")
{
    indexed_triangle_set first_sphere  = its_make_sphere(5., PI / 8.);
    indexed_triangle_set second_sphere = first_sphere;
    its_transform(second_sphere, identity3f().translate(Vec3f(20.f, 0.f, 0.f)));
    const size_t first_sphere_faces = first_sphere.indices.size();
    its_merge(first_sphere, second_sphere);

    AutoPaint::SegmentationOptions options;
    options.target_regions                     = 2;
    const AutoPaint::SegmentationResult result = AutoPaint::segment_by_geometry(first_sphere, options);

    REQUIRE(result.region_count() == 2);
    const size_t first_region  = result.face_regions.front();
    const size_t second_region = result.face_regions[first_sphere_faces];
    CHECK(first_region != second_region);
    CHECK(std::all_of(result.face_regions.begin(), result.face_regions.begin() + first_sphere_faces,
                      [first_region](size_t region) { return region == first_region; }));
    CHECK(std::all_of(result.face_regions.begin() + first_sphere_faces, result.face_regions.end(),
                      [second_region](size_t region) { return region == second_region; }));
}

TEST_CASE("Auto paint separates rounded lobes at a concave neck", "[AutoPaint]")
{
    const indexed_triangle_set     dumbbell = make_rounded_dumbbell();
    AutoPaint::SegmentationOptions options;
    options.target_regions      = 3;
    options.boundary_preference = 0.9f;

    const AutoPaint::SegmentationResult result = AutoPaint::segment_by_geometry(dumbbell, options);

    REQUIRE(result.region_count() >= 2);
    REQUIRE(result.region_count() <= options.target_regions * 6);
    const std::vector<std::vector<size_t>> neighbors = face_neighbors(dumbbell);
    for (size_t region = 0; region < result.region_count(); ++region)
        CHECK(region_is_connected(neighbors, result.face_regions, region));
}
