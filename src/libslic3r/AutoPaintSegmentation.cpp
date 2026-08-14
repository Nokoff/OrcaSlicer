#include "AutoPaintSegmentation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>

namespace Slic3r::AutoPaint {
namespace {

struct FaceGeometry
{
    Vec3f center = Vec3f::Zero();
    Vec3f normal = Vec3f::UnitZ();
    float area   = 0.f;
};

struct Neighbor
{
    size_t face = 0;
    float  cost = 0.f;
};

struct QueueEntry
{
    float  distance = 0.f;
    size_t face     = 0;
    size_t region   = 0;

    bool operator<(const QueueEntry& other) const { return distance > other.distance; }
};

uint64_t edge_key(int first, int second)
{
    const uint32_t low  = static_cast<uint32_t>(std::min(first, second));
    const uint32_t high = static_cast<uint32_t>(std::max(first, second));
    return (static_cast<uint64_t>(low) << 32) | high;
}

std::vector<FaceGeometry> calculate_face_geometry(const indexed_triangle_set& mesh)
{
    std::vector<FaceGeometry> result;
    result.reserve(mesh.indices.size());

    for (const Vec3i32& face : mesh.indices) {
        const Vec3f& a          = mesh.vertices[face[0]];
        const Vec3f& b          = mesh.vertices[face[1]];
        const Vec3f& c          = mesh.vertices[face[2]];
        const Vec3f  cross      = (b - a).cross(c - a);
        const float  twice_area = cross.norm();

        FaceGeometry geometry;
        geometry.center = (a + b + c) / 3.f;
        geometry.area   = 0.5f * twice_area;
        if (twice_area > std::numeric_limits<float>::epsilon())
            geometry.normal = cross / twice_area;
        result.emplace_back(geometry);
    }

    return result;
}

std::vector<std::vector<Neighbor>> build_face_graph(const indexed_triangle_set&      mesh,
                                                    const std::vector<FaceGeometry>& faces,
                                                    float                            boundary_preference)
{
    std::vector<std::vector<Neighbor>>   graph(mesh.indices.size());
    std::unordered_map<uint64_t, size_t> first_face_by_edge;
    first_face_by_edge.reserve(mesh.indices.size() * 3);

    const float     preference      = std::clamp(boundary_preference, 0.f, 1.f);
    const float     crease_gain     = 2.f + 48.f * preference * preference;
    constexpr float reference_angle = float(M_PI) / 4.f;

    for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx) {
        const Vec3i32& face = mesh.indices[face_idx];
        for (int edge_idx = 0; edge_idx < 3; ++edge_idx) {
            const int      first_vertex    = face[edge_idx];
            const int      second_vertex   = face[(edge_idx + 1) % 3];
            const uint64_t key             = edge_key(first_vertex, second_vertex);
            const auto [edge_it, inserted] = first_face_by_edge.emplace(key, face_idx);
            if (inserted || edge_it->second == face_idx)
                continue;

            // Non-manifold edges are connected to their first incident facet.
            // This keeps every incident sheet reachable without making the
            // topology builder disproportionately expensive.
            const size_t other_face  = edge_it->second;
            const float  normal_dot  = std::clamp(faces[face_idx].normal.dot(faces[other_face].normal), -1.f, 1.f);
            const float  angle       = std::acos(normal_dot);
            const float  crease      = angle / reference_angle;
            const float  base_length = std::max((faces[face_idx].center - faces[other_face].center).norm(), 1e-6f);
            const float  cost        = base_length * (1.f + crease_gain * crease * crease);

            graph[face_idx].push_back({other_face, cost});
            graph[other_face].push_back({face_idx, cost});
        }
    }

    return graph;
}

std::vector<std::vector<size_t>> connected_components(const std::vector<std::vector<Neighbor>>& graph)
{
    std::vector<std::vector<size_t>> components;
    std::vector<bool>                visited(graph.size(), false);
    std::vector<size_t>              stack;

    for (size_t start = 0; start < graph.size(); ++start) {
        if (visited[start])
            continue;

        components.emplace_back();
        stack.push_back(start);
        visited[start] = true;
        while (!stack.empty()) {
            const size_t face = stack.back();
            stack.pop_back();
            components.back().push_back(face);
            for (const Neighbor& neighbor : graph[face]) {
                if (!visited[neighbor.face]) {
                    visited[neighbor.face] = true;
                    stack.push_back(neighbor.face);
                }
            }
        }
    }

    return components;
}

size_t component_seed(const std::vector<size_t>& component, const std::vector<FaceGeometry>& faces)
{
    // Starting at the lowest substantial facet makes the first palette entry a
    // useful default for bases, shoes, and other grounded model features.
    return *std::min_element(component.begin(), component.end(), [&faces](size_t lhs, size_t rhs) {
        const float z_delta = faces[lhs].center.z() - faces[rhs].center.z();
        if (std::abs(z_delta) > 1e-6f)
            return z_delta < 0.f;
        if (std::abs(faces[lhs].area - faces[rhs].area) > 1e-6f)
            return faces[lhs].area > faces[rhs].area;
        return lhs < rhs;
    });
}

void grow_region(size_t                                    seed,
                 size_t                                    region,
                 const std::vector<std::vector<Neighbor>>& graph,
                 std::vector<float>&                       distances,
                 std::vector<size_t>&                      face_regions)
{
    std::priority_queue<QueueEntry> queue;
    distances[seed]    = 0.f;
    face_regions[seed] = region;
    queue.push({0.f, seed, region});

    while (!queue.empty()) {
        const QueueEntry current = queue.top();
        queue.pop();
        if (current.distance != distances[current.face] || current.region != face_regions[current.face])
            continue;

        for (const Neighbor& neighbor : graph[current.face]) {
            const float candidate = current.distance + neighbor.cost;
            if (candidate < distances[neighbor.face]) {
                distances[neighbor.face]    = candidate;
                face_regions[neighbor.face] = current.region;
                queue.push({candidate, neighbor.face, current.region});
            }
        }
    }
}

} // namespace

SegmentationResult segment_by_geometry(const indexed_triangle_set& mesh, const SegmentationOptions& options)
{
    SegmentationResult result;
    if (mesh.indices.empty() || mesh.vertices.empty())
        return result;

    const std::vector<FaceGeometry>          faces      = calculate_face_geometry(mesh);
    const std::vector<std::vector<Neighbor>> graph      = build_face_graph(mesh, faces, options.boundary_preference);
    std::vector<std::vector<size_t>>         components = connected_components(graph);
    std::sort(components.begin(), components.end(), [&faces](const std::vector<size_t>& lhs, const std::vector<size_t>& rhs) {
        const size_t lhs_seed = component_seed(lhs, faces);
        const size_t rhs_seed = component_seed(rhs, faces);
        const float  z_delta  = faces[lhs_seed].center.z() - faces[rhs_seed].center.z();
        if (std::abs(z_delta) > 1e-6f)
            return z_delta < 0.f;
        if (lhs.size() != rhs.size())
            return lhs.size() > rhs.size();
        return lhs_seed < rhs_seed;
    });

    const size_t       requested_regions = std::clamp(options.target_regions, size_t(1), mesh.indices.size());
    const size_t       target_regions    = std::max(requested_regions, components.size());
    constexpr size_t   unassigned_region = std::numeric_limits<size_t>::max();
    std::vector<float> distances(mesh.indices.size(), std::numeric_limits<float>::infinity());
    result.face_regions.assign(mesh.indices.size(), unassigned_region);
    result.region_seed_faces.reserve(target_regions);

    // Every disconnected component needs a seed because no surface path can
    // legitimately carry a colour into it from another component.
    for (const std::vector<size_t>& component : components) {
        const size_t seed   = component_seed(component, faces);
        const size_t region = result.region_seed_faces.size();
        result.region_seed_faces.push_back(seed);
        grow_region(seed, region, graph, distances, result.face_regions);
    }

    // Incremental farthest-point sampling spreads the remaining seeds over the
    // surface. Each Dijkstra update only changes facets made closer by the new
    // seed, and the crease-weighted costs keep those updates inside likely
    // modeled boundaries whenever possible.
    while (result.region_seed_faces.size() < target_regions) {
        const size_t next_seed = static_cast<size_t>(std::distance(distances.begin(), std::max_element(distances.begin(), distances.end())));
        const size_t region = result.region_seed_faces.size();
        result.region_seed_faces.push_back(next_seed);
        grow_region(next_seed, region, graph, distances, result.face_regions);
    }

    return result;
}

} // namespace Slic3r::AutoPaint
