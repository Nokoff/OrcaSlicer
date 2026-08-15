#include <catch2/catch.hpp>

#include "libslic3r/AutoPaintSegmentation.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
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

indexed_triangle_set make_smooth_tapered_limb()
{
    constexpr size_t rings    = 32;
    constexpr size_t segments = 96;

    indexed_triangle_set mesh;
    mesh.vertices.emplace_back(0.f, 0.f, 0.f);
    for (size_t ring = 1; ring < rings; ++ring) {
        const float t      = float(ring) / float(rings);
        const float radius = 3.2f - 1.2f * t + 0.35f * std::sin(2.f * float(PI) * t);
        for (size_t segment = 0; segment < segments; ++segment) {
            const float angle = 2.f * float(PI) * float(segment) / float(segments);
            mesh.vertices.emplace_back(radius * std::cos(angle), radius * std::sin(angle), 10.f * t);
        }
    }
    const int top_vertex = static_cast<int>(mesh.vertices.size());
    mesh.vertices.emplace_back(0.f, 0.f, 10.f);

    for (size_t segment = 0; segment < segments; ++segment) {
        const int current = 1 + static_cast<int>(segment);
        const int next    = 1 + static_cast<int>((segment + 1) % segments);
        mesh.indices.emplace_back(0, next, current);
    }
    for (size_t ring = 0; ring + 2 < rings; ++ring) {
        const int lower = 1 + static_cast<int>(ring * segments);
        const int upper = lower + static_cast<int>(segments);
        for (size_t segment = 0; segment < segments; ++segment) {
            const int current = static_cast<int>(segment);
            const int next    = static_cast<int>((segment + 1) % segments);
            mesh.indices.emplace_back(lower + current, lower + next, upper + next);
            mesh.indices.emplace_back(lower + current, upper + next, upper + current);
        }
    }
    const int last_ring = 1 + static_cast<int>((rings - 2) * segments);
    for (size_t segment = 0; segment < segments; ++segment) {
        const int current = last_ring + static_cast<int>(segment);
        const int next    = last_ring + static_cast<int>((segment + 1) % segments);
        mesh.indices.emplace_back(current, next, top_vertex);
    }
    return mesh;
}

size_t nearest_face(const indexed_triangle_set& mesh, const Vec3f& landmark)
{
    size_t nearest          = 0;
    float  nearest_distance = std::numeric_limits<float>::max();
    for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx) {
        const Vec3i32& face     = mesh.indices[face_idx];
        const Vec3f    center   = (mesh.vertices[face[0]] + mesh.vertices[face[1]] + mesh.vertices[face[2]]) / 3.f;
        const float    distance = (center - landmark).squaredNorm();
        if (distance < nearest_distance) {
            nearest_distance = distance;
            nearest          = face_idx;
        }
    }
    return nearest;
}

bool load_exact_mesh(const std::string& path, indexed_triangle_set& mesh)
{
    static_assert(sizeof(Vec3f) == 3 * sizeof(float));
    static_assert(sizeof(Vec3i32) == 3 * sizeof(int32_t));

    std::ifstream input(path, std::ios::binary);
    char          magic[8] = {};
    uint64_t      vertex_count;
    uint64_t      face_count;
    input.read(magic, sizeof(magic));
    input.read(reinterpret_cast<char*>(&vertex_count), sizeof(vertex_count));
    input.read(reinterpret_cast<char*>(&face_count), sizeof(face_count));
    if (!input.good() || std::string(magic, sizeof(magic)) != "APGMESH1" || vertex_count > 10'000'000 || face_count > 20'000'000)
        return false;

    mesh.vertices.resize(vertex_count);
    mesh.indices.resize(face_count);
    input.read(reinterpret_cast<char*>(mesh.vertices.data()), std::streamsize(mesh.vertices.size() * sizeof(Vec3f)));
    input.read(reinterpret_cast<char*>(mesh.indices.data()), std::streamsize(mesh.indices.size() * sizeof(Vec3i32)));
    return input.good();
}

} // namespace

TEST_CASE("Auto paint handles an empty mesh", "[AutoPaint]")
{
    const AutoPaint::SegmentationResult result = AutoPaint::segment_by_geometry({});
    CHECK(result.face_regions.empty());
    CHECK(result.region_seed_faces.empty());
}

TEST_CASE("Auto paint reports monotonic progress and cancels without partial output", "[AutoPaint][Progress]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    std::vector<int>                          progress_values;
    std::vector<AutoPaint::SegmentationStage> stages;
    AutoPaint::SegmentationOptions            options;
    options.progress_callback = [&](AutoPaint::SegmentationStage stage, int progress) {
        stages.push_back(stage);
        progress_values.push_back(progress);
        return true;
    };

    const AutoPaint::SegmentationResult completed = AutoPaint::segment_by_geometry(cube, options);
    REQUIRE_FALSE(completed.face_regions.empty());
    REQUIRE_FALSE(progress_values.empty());
    CHECK(progress_values.front() == 0);
    CHECK(progress_values.back() == 100);
    CHECK(std::is_sorted(progress_values.begin(), progress_values.end()));
    CHECK(stages.front() == AutoPaint::SegmentationStage::PreparingGeometry);
    CHECK(stages.back() == AutoPaint::SegmentationStage::Complete);

    options.progress_callback                     = [](AutoPaint::SegmentationStage, int progress) { return progress < 55; };
    const AutoPaint::SegmentationResult cancelled = AutoPaint::segment_by_geometry(cube, options);
    CHECK(cancelled.face_regions.empty());
    CHECK(cancelled.region_seed_faces.empty());
    CHECK(cancelled.region_palette.empty());
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

TEST_CASE("Auto paint keeps a smoothly tapered limb together", "[AutoPaint]")
{
    const indexed_triangle_set     limb = make_smooth_tapered_limb();
    AutoPaint::SegmentationOptions options;
    options.target_regions      = 5;
    options.boundary_preference = 0.8f;

    const AutoPaint::SegmentationResult result = AutoPaint::segment_by_geometry(limb, options);

    REQUIRE(result.region_count() == 1);
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

TEST_CASE("Auto paint preserves the semantic parts of the gnome", "[AutoPaint][GnomeRegression]")
{
    const std::string    fixture = std::string(TEST_DATA_DIR) + "/auto_paint/gnome_regression.mesh";
    indexed_triangle_set mesh;
    REQUIRE(load_exact_mesh(fixture, mesh));

    AutoPaint::SegmentationOptions options;
    options.target_regions                     = 4;
    options.boundary_preference                = 0.8f;
    const AutoPaint::SegmentationResult result = AutoPaint::segment_by_geometry(mesh, options);

    REQUIRE(result.face_regions.size() == mesh.indices.size());
    REQUIRE(result.region_count() >= 14);
    CHECK(result.region_count() <= 32);

    const auto   region_at       = [&](const Vec3f& landmark) { return result.face_regions[nearest_face(mesh, landmark)]; };
    const size_t hat_tip         = region_at(Vec3f(0.0047f, 0.3108f, 0.8112f));
    const size_t hat_middle      = region_at(Vec3f(-0.0002f, -0.0154f, 0.2664f));
    const size_t hat_brim        = region_at(Vec3f(-0.0023f, -0.1753f, 0.0226f));
    const size_t nose            = region_at(Vec3f(-0.0030f, -0.3589f, -0.2073f));
    const size_t left_brow       = region_at(Vec3f(-0.1405f, -0.2907f, -0.2775f));
    const size_t right_brow      = region_at(Vec3f(0.1370f, -0.2908f, -0.2765f));
    const size_t left_moustache  = region_at(Vec3f(-0.1929f, -0.2945f, -0.3183f));
    const size_t right_moustache = region_at(Vec3f(0.1912f, -0.2950f, -0.3172f));
    const size_t upper_beard     = region_at(Vec3f(0.0005f, -0.3878f, -0.4384f));
    const size_t lower_beard     = region_at(Vec3f(-0.0002f, -0.4425f, -0.6906f));
    const size_t left_sleeve     = region_at(Vec3f(-0.3840f, 0.0322f, -0.4796f));
    const size_t right_sleeve    = region_at(Vec3f(0.3804f, 0.0309f, -0.4773f));
    const size_t left_hand       = region_at(Vec3f(-0.4435f, -0.0058f, -0.6549f));
    const size_t right_hand      = region_at(Vec3f(0.4434f, -0.0067f, -0.6555f));
    const size_t left_foot       = region_at(Vec3f(-0.2152f, -0.3508f, -0.8498f));
    const size_t right_foot      = region_at(Vec3f(0.2235f, -0.3507f, -0.8488f));

    CAPTURE(hat_tip, hat_middle, hat_brim, nose, left_brow, right_brow, left_moustache, right_moustache, upper_beard, lower_beard,
            left_sleeve, right_sleeve, left_hand, right_hand, left_foot, right_foot);
    CHECK(hat_tip == hat_middle);
    CHECK(hat_middle == hat_brim);
    CHECK(upper_beard == lower_beard);
    CHECK(nose != hat_brim);
    CHECK(nose != upper_beard);
    CHECK(left_moustache != upper_beard);
    CHECK(right_moustache != upper_beard);
    CHECK(left_moustache != nose);
    CHECK(right_moustache != nose);
    CHECK(left_brow == left_moustache);
    CHECK(right_brow == right_moustache);
    CHECK(left_brow != upper_beard);
    CHECK(right_brow != upper_beard);
    CHECK(left_sleeve != left_hand);
    CHECK(right_sleeve != right_hand);
    CHECK(left_sleeve != upper_beard);
    CHECK(right_sleeve != upper_beard);
    CHECK(left_foot != right_foot);
    CHECK(left_foot != left_hand);
    CHECK(right_foot != right_hand);
    CHECK(left_foot != lower_beard);
    CHECK(right_foot != lower_beard);

    CHECK(result.region_palette[left_moustache] == result.region_palette[right_moustache]);
    CHECK(result.region_palette[left_sleeve] == result.region_palette[right_sleeve]);
    CHECK(result.region_palette[left_hand] == result.region_palette[right_hand]);
    CHECK(result.region_palette[left_foot] == result.region_palette[right_foot]);
    CHECK(result.region_palette[hat_tip] != result.region_palette[nose]);
    CHECK(result.region_palette[hat_tip] != result.region_palette[upper_beard]);
    CHECK(result.region_palette[nose] != result.region_palette[upper_beard]);
    CHECK(result.region_palette[nose] != result.region_palette[left_moustache]);
    CHECK(result.region_palette[upper_beard] != result.region_palette[left_moustache]);

    const std::vector<std::vector<size_t>> neighbors = face_neighbors(mesh);
    for (size_t region = 0; region < result.region_count(); ++region)
        CHECK(region_is_connected(neighbors, result.face_regions, region));
}
