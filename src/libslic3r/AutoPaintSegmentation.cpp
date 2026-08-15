#include "AutoPaintSegmentation.hpp"
#include "MeshBoolean.hpp"
#include "QuadricEdgeCollapse.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Slic3r::AutoPaint {
namespace {

struct FaceGeometry
{
    Vec3f normal = Vec3f::UnitZ();
    Vec3f center = Vec3f::Zero();
    float area   = 0.f;
    float max_z  = std::numeric_limits<float>::lowest();
};

struct SurfaceEdge
{
    size_t first_face        = 0;
    size_t second_face       = 0;
    float  boundary_strength = 0.f;
    float  concave_strength  = 0.f;
};

struct OpenEdge
{
    size_t face          = 0;
    int    first_vertex  = 0;
    int    second_vertex = 0;
};

class DisjointSet
{
public:
    explicit DisjointSet(const std::vector<FaceGeometry>& faces)
        : m_parent(faces.size()), m_sizes(faces.size(), 1), m_areas(faces.size(), 0.), m_max_z(faces.size(), 0.f)
    {
        std::iota(m_parent.begin(), m_parent.end(), size_t(0));
        for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
            m_areas[face_idx] = faces[face_idx].area;
            m_max_z[face_idx] = faces[face_idx].max_z;
        }
    }

    size_t find(size_t item)
    {
        size_t root = item;
        while (m_parent[root] != root)
            root = m_parent[root];
        while (m_parent[item] != item) {
            const size_t parent = m_parent[item];
            m_parent[item]      = root;
            item                = parent;
        }
        return root;
    }

    bool unite(size_t first, size_t second)
    {
        first  = find(first);
        second = find(second);
        if (first == second)
            return false;
        if (m_sizes[first] < m_sizes[second] || (m_sizes[first] == m_sizes[second] && first > second))
            std::swap(first, second);
        m_parent[second] = first;
        m_sizes[first] += m_sizes[second];
        m_areas[first] += m_areas[second];
        m_max_z[first] = std::max(m_max_z[first], m_max_z[second]);
        return true;
    }

    [[nodiscard]] double area(size_t item) { return m_areas[find(item)]; }
    [[nodiscard]] float  max_z(size_t item) { return m_max_z[find(item)]; }

private:
    std::vector<size_t> m_parent;
    std::vector<size_t> m_sizes;
    std::vector<double> m_areas;
    std::vector<float>  m_max_z;
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
        geometry.max_z  = std::max({a.z(), b.z(), c.z()});
        if (twice_area > std::numeric_limits<float>::epsilon())
            geometry.normal = cross / twice_area;
        result.emplace_back(geometry);
    }

    return result;
}

std::vector<SurfaceEdge> build_surface_edges(const indexed_triangle_set& mesh, const std::vector<FaceGeometry>& faces)
{
    std::vector<SurfaceEdge>               edges;
    std::unordered_map<uint64_t, OpenEdge> first_face_by_edge;
    edges.reserve(mesh.indices.size() * 3 / 2);
    first_face_by_edge.reserve(mesh.indices.size() * 3);

    for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx) {
        const Vec3i32& face = mesh.indices[face_idx];
        for (int edge_idx = 0; edge_idx < 3; ++edge_idx) {
            const int      first_vertex    = face[edge_idx];
            const int      second_vertex   = face[(edge_idx + 1) % 3];
            const uint64_t key             = edge_key(first_vertex, second_vertex);
            const auto [edge_it, inserted] = first_face_by_edge.emplace(key, OpenEdge{face_idx, first_vertex, second_vertex});
            if (inserted || edge_it->second.face == face_idx)
                continue;

            // Non-manifold edges are connected to their first incident facet.
            const OpenEdge& open_edge   = edge_it->second;
            const Vec3f     edge_vector = mesh.vertices[open_edge.second_vertex] - mesh.vertices[open_edge.first_vertex];
            const float     edge_length = edge_vector.norm();
            Vec3f           edge_axis   = Vec3f::UnitX();
            if (edge_length > 1e-8f)
                edge_axis = edge_vector / edge_length;
            const Vec3f& first_normal  = faces[open_edge.face].normal;
            const Vec3f& second_normal = faces[face_idx].normal;
            const float  normal_dot    = std::clamp(first_normal.dot(second_normal), -1.f, 1.f);
            const float  normal_sine   = edge_axis.dot(first_normal.cross(second_normal));
            const float  signed_angle  = std::atan2(normal_sine, normal_dot) * 180.f / float(M_PI);

            // Smooth convex curvature belongs to one rounded part. Concave
            // curvature usually marks an attachment seam, while a convex fold
            // is only a boundary when it is visibly sharp.
            const float concave_strength    = std::max(-signed_angle, 0.f);
            const float sharp_fold_strength = std::max(std::abs(signed_angle) - 22.f, 0.f);
            edges.push_back({open_edge.face, face_idx, std::max(concave_strength, sharp_fold_strength), concave_strength});
        }
    }

    return edges;
}

void snap_shape_boundaries_to_geometry(const std::vector<SurfaceEdge>& edges, float boundary_threshold, std::vector<size_t>& labels)
{
    struct WeightedNeighbor
    {
        size_t face              = 0;
        float  weight            = 0.f;
        float  boundary_strength = 0.f;
    };
    struct FaceNeighborhood
    {
        std::array<WeightedNeighbor, 3> neighbors;
        size_t                          count = 0;

        void add(size_t face, float weight, float boundary_strength)
        {
            if (count < neighbors.size())
                neighbors[count++] = {face, weight, boundary_strength};
        }
    };

    if (labels.empty())
        return;

    const float                   threshold = std::max(0.5f * boundary_threshold, 1.f);
    std::vector<FaceNeighborhood> neighborhoods(labels.size());
    for (const SurfaceEdge& edge : edges) {
        // Crossing a smooth edge is expensive, while a modeled crease nearly
        // disconnects its two faces. This lets the label contour slide locally
        // until it sits on the strongest nearby geometric boundary.
        const float relative_strength = edge.boundary_strength / threshold;
        const float squared_strength  = relative_strength * relative_strength;
        const float weight            = 1.f / (1.f + squared_strength * squared_strength);
        neighborhoods[edge.first_face].add(edge.second_face, weight, edge.boundary_strength);
        neighborhoods[edge.second_face].add(edge.first_face, weight, edge.boundary_strength);
    }

    const std::vector<size_t> original_labels = labels;
    constexpr float           data_weight     = 0.4f;
    constexpr float           switch_margin   = 0.1f;
    // In-place updates monotonically improve the local boundary energy and
    // avoid the checkerboard oscillation of a synchronous majority filter.
    for (size_t pass = 0; pass < 8; ++pass) {
        size_t changed_count = 0;
        for (size_t face_idx = 0; face_idx < labels.size(); ++face_idx) {
            std::array<size_t, 5> candidate_labels;
            std::array<float, 5>  candidate_scores{};
            size_t                candidate_count = 0;
            const auto            add_score       = [&](size_t label, float score) {
                for (size_t candidate = 0; candidate < candidate_count; ++candidate) {
                    if (candidate_labels[candidate] == label) {
                        candidate_scores[candidate] += score;
                        return;
                    }
                }
                candidate_labels[candidate_count] = label;
                candidate_scores[candidate_count] = score;
                ++candidate_count;
            };

            add_score(labels[face_idx], 0.f);
            add_score(original_labels[face_idx], data_weight);
            for (size_t neighbor_idx = 0; neighbor_idx < neighborhoods[face_idx].count; ++neighbor_idx) {
                const WeightedNeighbor& neighbor = neighborhoods[face_idx].neighbors[neighbor_idx];
                add_score(labels[neighbor.face], neighbor.weight);
            }

            size_t current_candidate = 0;
            size_t best_candidate    = 0;
            for (size_t candidate = 0; candidate < candidate_count; ++candidate) {
                if (candidate_labels[candidate] == labels[face_idx])
                    current_candidate = candidate;
                if (candidate_scores[candidate] > candidate_scores[best_candidate])
                    best_candidate = candidate;
            }

            const auto boundary_support = [&](size_t label) {
                float  strength = 0.f;
                size_t count    = 0;
                for (size_t neighbor_idx = 0; neighbor_idx < neighborhoods[face_idx].count; ++neighbor_idx) {
                    const WeightedNeighbor& neighbor = neighborhoods[face_idx].neighbors[neighbor_idx];
                    if (labels[neighbor.face] != label) {
                        strength += neighbor.boundary_strength;
                        ++count;
                    }
                }
                return count == 0 ? 0.f : strength / float(count);
            };

            const float current_support = boundary_support(labels[face_idx]);
            const float new_support     = boundary_support(candidate_labels[best_candidate]);
            // Never let a shape label simply wander across an uninterrupted
            // limb. Boundary snapping is allowed only when it actually moves
            // the contour onto a stronger crease or concave seam.
            const bool geometry_support = new_support >= 0.35f * boundary_threshold &&
                                          new_support > current_support + 0.05f * boundary_threshold;
            if (candidate_labels[best_candidate] != labels[face_idx] &&
                candidate_scores[best_candidate] > candidate_scores[current_candidate] + switch_margin && geometry_support) {
                labels[face_idx] = candidate_labels[best_candidate];
                ++changed_count;
            }
        }
        if (changed_count == 0)
            break;
    }
}

std::vector<size_t> stabilize_bilateral_regions(const indexed_triangle_set&      mesh,
                                                const std::vector<FaceGeometry>& faces,
                                                const std::vector<SurfaceEdge>&  edges,
                                                double                           minimum_detail_area,
                                                SegmentationResult&              result)
{
    constexpr size_t no_region = std::numeric_limits<size_t>::max();
    if (result.region_count() < 2 || mesh.vertices.empty())
        return {};

    Vec3f minimum = mesh.vertices.front();
    Vec3f maximum = mesh.vertices.front();
    for (const Vec3f& vertex : mesh.vertices) {
        minimum = minimum.cwiseMin(vertex);
        maximum = maximum.cwiseMax(vertex);
    }
    const float diagonal  = (maximum - minimum).norm();
    const float cell_size = diagonal / 300.f;
    if (cell_size <= std::numeric_limits<float>::epsilon())
        return {};

    const auto cell_coordinate = [&](const Vec3f& point) {
        return Eigen::Vector3i(int(std::floor((point.x() - minimum.x()) / cell_size)),
                               int(std::floor((point.y() - minimum.y()) / cell_size)),
                               int(std::floor((point.z() - minimum.z()) / cell_size)));
    };
    const auto cell_key = [](int x, int y, int z) {
        constexpr uint64_t mask = (uint64_t(1) << 21) - 1;
        return ((uint64_t(x) & mask) << 42) | ((uint64_t(y) & mask) << 21) | (uint64_t(z) & mask);
    };

    std::unordered_multimap<uint64_t, size_t> faces_by_cell;
    faces_by_cell.reserve(faces.size());
    for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
        const Eigen::Vector3i cell = cell_coordinate(faces[face_idx].center);
        faces_by_cell.emplace(cell_key(cell.x(), cell.y(), cell.z()), face_idx);
    }

    const float         symmetry_x               = minimum.x() + maximum.x();
    const float         maximum_distance_squared = 2.25f * cell_size * cell_size;
    std::vector<size_t> mirrored_face(faces.size(), no_region);
    double              matched_area = 0.;
    for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
        Vec3f query                      = faces[face_idx].center;
        query.x()                        = symmetry_x - query.x();
        Vec3f reflected_normal           = faces[face_idx].normal;
        reflected_normal.x()             = -reflected_normal.x();
        const Eigen::Vector3i query_cell = cell_coordinate(query);

        size_t best_face  = no_region;
        float  best_score = std::numeric_limits<float>::max();
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto [begin,
                                end] = faces_by_cell.equal_range(cell_key(query_cell.x() + dx, query_cell.y() + dy, query_cell.z() + dz));
                    for (auto candidate_it = begin; candidate_it != end; ++candidate_it) {
                        const size_t candidate        = candidate_it->second;
                        const float  distance_squared = (faces[candidate].center - query).squaredNorm();
                        if (distance_squared > maximum_distance_squared)
                            continue;
                        const float normal_agreement = reflected_normal.dot(faces[candidate].normal);
                        if (normal_agreement < 0.6f)
                            continue;
                        const float score = distance_squared + 0.1f * cell_size * cell_size * (1.f - normal_agreement);
                        if (score < best_score) {
                            best_score = score;
                            best_face  = candidate;
                        }
                    }
                }
            }
        }
        if (best_face != no_region) {
            mirrored_face[face_idx] = best_face;
            matched_area += faces[face_idx].area;
        }
    }

    const double total_area = std::accumulate(faces.begin(), faces.end(), 0.,
                                              [](double area, const FaceGeometry& face) { return area + face.area; });
    if (matched_area < 0.75 * total_area)
        return {};

    const size_t                                    original_region_count = result.region_count();
    const std::vector<size_t>                       original_labels       = result.face_regions;
    std::vector<double>                             region_areas(original_region_count, 0.);
    std::vector<double>                             region_x_moments(original_region_count, 0.);
    std::vector<std::unordered_map<size_t, double>> mirrored_areas(original_region_count);
    for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
        const size_t region = original_labels[face_idx];
        region_areas[region] += faces[face_idx].area;
        region_x_moments[region] += faces[face_idx].area * faces[face_idx].center.x();
        if (mirrored_face[face_idx] != no_region)
            mirrored_areas[region][original_labels[mirrored_face[face_idx]]] += faces[face_idx].area;
    }

    std::vector<size_t> source_regions(original_region_count);
    std::iota(source_regions.begin(), source_regions.end(), size_t(0));
    std::sort(source_regions.begin(), source_regions.end(), [&](size_t lhs, size_t rhs) { return region_areas[lhs] < region_areas[rhs]; });

    std::vector<size_t>              partners(original_region_count, no_region);
    std::vector<std::vector<size_t>> surface_neighbors;
    const auto                       expand_to_mirrored_partner = [&](size_t region, size_t partner) {
        const double region_center_x = region_x_moments[region] / region_areas[region];
        const double symmetry_plane  = 0.5 * symmetry_x;
        if (std::abs(region_center_x - symmetry_plane) < 0.05 * (maximum.x() - minimum.x()))
            return;

        if (surface_neighbors.empty()) {
            surface_neighbors.resize(faces.size());
            for (const SurfaceEdge& edge : edges) {
                surface_neighbors[edge.first_face].push_back(edge.second_face);
                surface_neighbors[edge.second_face].push_back(edge.first_face);
            }
        }

        std::vector<bool>  reached(faces.size(), false);
        std::queue<size_t> pending;
        for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
            if (original_labels[face_idx] == region) {
                reached[face_idx] = true;
                pending.push(face_idx);
            }
        }
        while (!pending.empty()) {
            const size_t face_idx = pending.front();
            pending.pop();
            for (const size_t neighbor : surface_neighbors[face_idx]) {
                if (reached[neighbor])
                    continue;
                const bool same_side = (faces[neighbor].center.x() - symmetry_plane) * (region_center_x - symmetry_plane) > 0.;
                const bool mirrored_candidate = same_side && mirrored_face[neighbor] != no_region &&
                                                original_labels[mirrored_face[neighbor]] == partner &&
                                                result.face_regions[neighbor] < original_region_count;
                if (original_labels[neighbor] == region || mirrored_candidate) {
                    reached[neighbor] = true;
                    pending.push(neighbor);
                }
            }
        }
        for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx)
            if (reached[face_idx] && original_labels[face_idx] != region && result.face_regions[face_idx] < original_region_count)
                result.face_regions[face_idx] = region;
    };
    for (const size_t source : source_regions) {
        if (region_areas[source] < minimum_detail_area || mirrored_areas[source].empty())
            continue;
        const auto   dominant = std::max_element(mirrored_areas[source].begin(), mirrored_areas[source].end(),
                                                 [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
        const size_t target   = dominant->first;
        if (target == source)
            continue;
        const double mapped_area = std::accumulate(mirrored_areas[source].begin(), mirrored_areas[source].end(), 0.,
                                                   [](double area, const auto& item) { return area + item.second; });
        if (mapped_area < 0.65 * region_areas[source] || dominant->second < 0.5 * mapped_area)
            continue;

        const double area_ratio = region_areas[target] / region_areas[source];
        if (area_ratio >= 0.5 && area_ratio <= 2.) {
            const auto reverse_dominant = std::max_element(mirrored_areas[target].begin(), mirrored_areas[target].end(),
                                                           [](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
            if (reverse_dominant != mirrored_areas[target].end() && reverse_dominant->first == source && partners[source] == no_region &&
                partners[target] == no_region) {
                expand_to_mirrored_partner(source, target);
                expand_to_mirrored_partner(target, source);
                partners[source] = target;
                partners[target] = source;
                continue;
            }
        }
        if (area_ratio < 1.2 || partners[source] != no_region || partners[target] != no_region)
            continue;

        const double source_center_x = region_x_moments[source] / region_areas[source];
        const double symmetry_plane  = 0.5 * symmetry_x;
        if (std::abs(source_center_x - symmetry_plane) < 0.05 * (maximum.x() - minimum.x()))
            continue;

        std::vector<bool> candidate_faces(faces.size(), false);
        for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
            if (result.face_regions[face_idx] != target || mirrored_face[face_idx] == no_region)
                continue;
            if (original_labels[mirrored_face[face_idx]] == source)
                candidate_faces[face_idx] = true;
        }

        DisjointSet candidate_sets(faces);
        for (const SurfaceEdge& edge : edges)
            if (candidate_faces[edge.first_face] && candidate_faces[edge.second_face])
                candidate_sets.unite(edge.first_face, edge.second_face);

        size_t largest_root = no_region;
        double largest_area = 0.;
        for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
            if (!candidate_faces[face_idx])
                continue;
            const size_t root = candidate_sets.find(face_idx);
            if (candidate_sets.area(root) > largest_area) {
                largest_root = root;
                largest_area = candidate_sets.area(root);
            }
        }
        if (largest_root == no_region || largest_area < 0.5 * region_areas[source])
            continue;

        const size_t new_region = result.region_seed_faces.size();
        size_t       seed       = no_region;
        for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
            if (candidate_faces[face_idx] && candidate_sets.find(face_idx) == largest_root) {
                result.face_regions[face_idx] = new_region;
                if (seed == no_region)
                    seed = face_idx;
            }
        }

        // A mirrored cut may enclose small leftovers of the old target. Fold
        // any such component that touches the new region into it, keeping the
        // large target remainder and the carved counterpart both contiguous.
        DisjointSet remaining_sets(faces);
        for (const SurfaceEdge& edge : edges)
            if (result.face_regions[edge.first_face] == target && result.face_regions[edge.second_face] == target)
                remaining_sets.unite(edge.first_face, edge.second_face);
        size_t largest_remaining_root = no_region;
        double largest_remaining_area = 0.;
        for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
            if (result.face_regions[face_idx] != target)
                continue;
            const size_t root = remaining_sets.find(face_idx);
            if (remaining_sets.area(root) > largest_remaining_area) {
                largest_remaining_root = root;
                largest_remaining_area = remaining_sets.area(root);
            }
        }
        std::unordered_set<size_t> enclosed_roots;
        for (const SurfaceEdge& edge : edges) {
            if (result.face_regions[edge.first_face] == new_region && result.face_regions[edge.second_face] == target)
                enclosed_roots.insert(remaining_sets.find(edge.second_face));
            else if (result.face_regions[edge.second_face] == new_region && result.face_regions[edge.first_face] == target)
                enclosed_roots.insert(remaining_sets.find(edge.first_face));
        }
        enclosed_roots.erase(largest_remaining_root);
        for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx)
            if (result.face_regions[face_idx] == target && enclosed_roots.count(remaining_sets.find(face_idx)) != 0)
                result.face_regions[face_idx] = new_region;

        result.region_seed_faces.push_back(seed);
        partners.resize(result.region_count(), no_region);
        partners[source]     = new_region;
        partners[new_region] = source;
    }

    std::vector<size_t> compact_region(result.region_count(), no_region);
    std::vector<size_t> compact_seeds;
    for (size_t face_idx = 0; face_idx < result.face_regions.size(); ++face_idx) {
        const size_t old_region = result.face_regions[face_idx];
        if (compact_region[old_region] == no_region) {
            compact_region[old_region] = compact_seeds.size();
            compact_seeds.push_back(face_idx);
        }
        result.face_regions[face_idx] = compact_region[old_region];
    }
    std::vector<size_t> compact_partners(compact_seeds.size(), no_region);
    for (size_t old_region = 0; old_region < partners.size(); ++old_region) {
        const size_t old_partner = partners[old_region];
        if (compact_region[old_region] != no_region && old_partner != no_region && old_partner < compact_region.size() &&
            compact_region[old_partner] != no_region)
            compact_partners[compact_region[old_region]] = compact_region[old_partner];
    }
    result.region_seed_faces = std::move(compact_seeds);
    return compact_partners;
}

void allocate_palette(const std::vector<SurfaceEdge>&  edges,
                      const std::vector<FaceGeometry>& faces,
                      size_t                           palette_size,
                      SegmentationResult&              result)
{
    const size_t region_count = result.region_count();
    if (region_count == 0)
        return;

    palette_size = std::max(palette_size, size_t(1));
    std::vector<double>                     region_areas(region_count, 0.);
    std::vector<std::unordered_set<size_t>> neighbors(region_count);
    for (size_t face_idx = 0; face_idx < result.face_regions.size(); ++face_idx)
        region_areas[result.face_regions[face_idx]] += faces[face_idx].area;
    for (const SurfaceEdge& edge : edges) {
        const size_t first_region  = result.face_regions[edge.first_face];
        const size_t second_region = result.face_regions[edge.second_face];
        if (first_region != second_region) {
            neighbors[first_region].insert(second_region);
            neighbors[second_region].insert(first_region);
        }
    }

    std::vector<std::unordered_set<size_t>> nearby_regions(region_count);
    // Directly touching parts must differ first. Two-hop neighbours are a
    // softer conflict so close details around a shared face do not all reuse
    // one colour when another selected colour is available.
    for (size_t region = 0; region < region_count; ++region) {
        for (const size_t neighbor : neighbors[region]) {
            for (const size_t nearby : neighbors[neighbor]) {
                if (nearby != region && neighbors[region].count(nearby) == 0)
                    nearby_regions[region].insert(nearby);
            }
        }
    }

    constexpr size_t unassigned = std::numeric_limits<size_t>::max();
    result.region_palette.assign(region_count, unassigned);
    std::vector<double> palette_areas(palette_size, 0.);
    for (size_t assigned_count = 0; assigned_count < region_count; ++assigned_count) {
        size_t region          = unassigned;
        size_t best_saturation = 0;
        size_t best_degree     = 0;
        for (size_t candidate = 0; candidate < region_count; ++candidate) {
            if (result.region_palette[candidate] != unassigned)
                continue;
            std::unordered_set<size_t> adjacent_palettes;
            for (const size_t neighbor : neighbors[candidate])
                if (result.region_palette[neighbor] != unassigned)
                    adjacent_palettes.insert(result.region_palette[neighbor]);
            const size_t saturation = adjacent_palettes.size();
            const size_t degree     = neighbors[candidate].size();
            if (region == unassigned || saturation > best_saturation || (saturation == best_saturation && degree > best_degree) ||
                (saturation == best_saturation && degree == best_degree && region_areas[candidate] > region_areas[region]) ||
                (saturation == best_saturation && degree == best_degree && region_areas[candidate] == region_areas[region] &&
                 candidate < region)) {
                region          = candidate;
                best_saturation = saturation;
                best_degree     = degree;
            }
        }

        size_t best_palette          = 0;
        size_t best_conflicts        = std::numeric_limits<size_t>::max();
        size_t best_nearby_conflicts = std::numeric_limits<size_t>::max();
        for (size_t palette = 0; palette < palette_size; ++palette) {
            size_t conflicts = 0;
            for (const size_t neighbor : neighbors[region])
                if (result.region_palette[neighbor] == palette)
                    ++conflicts;
            size_t nearby_conflicts = 0;
            for (const size_t nearby : nearby_regions[region])
                if (result.region_palette[nearby] == palette)
                    ++nearby_conflicts;
            if (conflicts < best_conflicts || (conflicts == best_conflicts && nearby_conflicts < best_nearby_conflicts) ||
                (conflicts == best_conflicts && nearby_conflicts == best_nearby_conflicts &&
                 palette_areas[palette] < palette_areas[best_palette]) ||
                (conflicts == best_conflicts && nearby_conflicts == best_nearby_conflicts &&
                 palette_areas[palette] == palette_areas[best_palette] && palette < best_palette)) {
                best_palette          = palette;
                best_conflicts        = conflicts;
                best_nearby_conflicts = nearby_conflicts;
            }
        }
        result.region_palette[region] = best_palette;
        palette_areas[best_palette] += region_areas[region];
    }
}

} // namespace

SegmentationResult segment_by_geometry(const indexed_triangle_set& mesh, const SegmentationOptions& options)
{
    SegmentationResult result;
    const auto         report_progress = [&options](SegmentationStage stage, int progress) {
        return !options.progress_callback || options.progress_callback(stage, progress);
    };
    if (!report_progress(SegmentationStage::PreparingGeometry, 0))
        return result;
    if (mesh.indices.empty() || mesh.vertices.empty())
        return result;

    const std::vector<FaceGeometry> faces = calculate_face_geometry(mesh);
    std::vector<SurfaceEdge>        edges = build_surface_edges(mesh, faces);
    std::sort(edges.begin(), edges.end(), [](const SurfaceEdge& lhs, const SurfaceEdge& rhs) {
        if (lhs.boundary_strength != rhs.boundary_strength)
            return lhs.boundary_strength < rhs.boundary_strength;
        if (lhs.first_face != rhs.first_face)
            return lhs.first_face < rhs.first_face;
        return lhs.second_face < rhs.second_face;
    });
    if (!report_progress(SegmentationStage::PreparingGeometry, 12))
        return {};

    const float  preference         = std::clamp(options.boundary_preference, 0.f, 1.f);
    const float  boundary_threshold = 18.f - 14.f * preference;
    const size_t palette_size       = std::clamp(options.target_regions, size_t(1), mesh.indices.size());
    // High detail asks the global shape pass for more candidates. The later
    // crease pass and cleanup decide which candidates are genuine parts.
    const size_t detail_multiplier   = 6 + static_cast<size_t>(std::lround(5.f * preference));
    const size_t maximum_regions     = std::min(mesh.indices.size(), std::max(palette_size * detail_multiplier, palette_size + 4));
    const double total_area          = std::accumulate(faces.begin(), faces.end(), 0.,
                                                       [](double area, const FaceGeometry& face) { return area + face.area; });
    double       minimum_region_area = total_area / std::max<double>(maximum_regions * 250., 1.);

    const bool          use_fine_shape_labels = mesh.indices.size() >= 10000;
    std::vector<size_t> coarse_shape_labels;
    std::vector<size_t> shape_labels;
    if (!report_progress(SegmentationStage::AnalyzingShape, 20))
        return {};
    if (mesh.indices.size() >= 2000) {
        try {
            indexed_triangle_set proxy_mesh       = mesh;
            const size_t         proxy_face_limit = 20000 + static_cast<size_t>(std::lround(20000.f * preference));
            if (proxy_mesh.indices.size() > proxy_face_limit)
                its_quadric_edge_collapse(proxy_mesh, uint32_t(proxy_face_limit));
            const size_t coarse_multiplier = 4 + static_cast<size_t>(std::lround(2.f * preference));
            const size_t coarse_regions    = std::min(mesh.indices.size(), std::max(palette_size * coarse_multiplier, palette_size + 4));
            const double coarse_smoothing  = 0.8 - 0.72 * preference;
            if (!use_fine_shape_labels) {
                coarse_shape_labels = MeshBoolean::cgal::segment_face_ids(proxy_mesh, mesh, coarse_smoothing, coarse_regions, 25);
                shape_labels        = coarse_shape_labels;
            } else {
                // At high detail, retain enough independent thickness
                // candidates for small relief such as a moustache above a
                // beard. Geometry cleanup below rejoins candidates whose
                // interface is seamless.
                const double                     fine_smoothing = std::clamp(0.72 - 0.8 * preference, 0.05, 0.72);
                std::vector<std::vector<size_t>> label_levels =
                    MeshBoolean::cgal::segment_face_ids_multi(proxy_mesh, mesh,
                                                              {{coarse_smoothing, coarse_regions}, {fine_smoothing, maximum_regions}}, 25);
                coarse_shape_labels = std::move(label_levels[0]);
                shape_labels        = std::move(label_levels[1]);
            }
        } catch (...) {
            // Open, non-manifold, or otherwise unsuitable meshes still receive
            // the fast local crease-based fallback below.
            shape_labels.clear();
        }
    }
    if (!report_progress(SegmentationStage::DetectingBoundaries, 55))
        return {};

    const bool has_shape_labels = shape_labels.size() == mesh.indices.size() && coarse_shape_labels.size() == mesh.indices.size();
    if (has_shape_labels)
        snap_shape_boundaries_to_geometry(edges, boundary_threshold, shape_labels);
    std::vector<size_t> coarse_part_labels(mesh.indices.size(), 0);
    if (has_shape_labels) {
        struct InterfaceStats
        {
            size_t first_root   = 0;
            size_t second_root  = 0;
            double strength_sum = 0.;
            size_t edge_count   = 0;

            [[nodiscard]] double mean_strength() const { return strength_sum / double(edge_count); }
        };

        // First resolve the conservative SDF level into stable large parts.
        // This is the parent partition that the fine-detail pass may subdivide
        // but never cross.
        DisjointSet coarse_sets(faces);
        size_t      coarse_component_count = mesh.indices.size();
        for (const SurfaceEdge& edge : edges)
            if (coarse_shape_labels[edge.first_face] == coarse_shape_labels[edge.second_face] &&
                edge.concave_strength <= boundary_threshold && coarse_sets.unite(edge.first_face, edge.second_face))
                --coarse_component_count;

        const size_t coarse_multiplier   = 4 + static_cast<size_t>(std::lround(2.f * preference));
        const size_t coarse_regions      = std::min(mesh.indices.size(), std::max(palette_size * coarse_multiplier, palette_size + 4));
        const double coarse_minimum_area = total_area / std::max<double>(coarse_regions * 100., 1.);
        for (size_t pass = 0; pass < 8; ++pass) {
            size_t merged_count = 0;
            for (const SurfaceEdge& edge : edges) {
                const size_t first_root  = coarse_sets.find(edge.first_face);
                const size_t second_root = coarse_sets.find(edge.second_face);
                if (first_root != second_root &&
                    (coarse_sets.area(first_root) < coarse_minimum_area || coarse_sets.area(second_root) < coarse_minimum_area) &&
                    coarse_sets.unite(first_root, second_root)) {
                    --coarse_component_count;
                    ++merged_count;
                }
            }
            if (merged_count == 0)
                break;
        }

        for (size_t pass = 0; pass < 8; ++pass) {
            std::unordered_map<uint64_t, InterfaceStats> interfaces_by_roots;
            interfaces_by_roots.reserve(coarse_component_count * 3);
            for (const SurfaceEdge& edge : edges) {
                size_t first_root  = coarse_sets.find(edge.first_face);
                size_t second_root = coarse_sets.find(edge.second_face);
                if (first_root == second_root)
                    continue;
                if (first_root > second_root)
                    std::swap(first_root, second_root);
                const uint64_t key            = (static_cast<uint64_t>(first_root) << 32) | static_cast<uint32_t>(second_root);
                auto [interface_it, inserted] = interfaces_by_roots.emplace(key, InterfaceStats{first_root, second_root, 0., 0});
                interface_it->second.strength_sum += edge.boundary_strength;
                ++interface_it->second.edge_count;
            }

            std::vector<InterfaceStats> interfaces;
            interfaces.reserve(interfaces_by_roots.size());
            for (const auto& [key, stats] : interfaces_by_roots)
                interfaces.push_back(stats);
            std::sort(interfaces.begin(), interfaces.end(),
                      [](const InterfaceStats& lhs, const InterfaceStats& rhs) { return lhs.mean_strength() < rhs.mean_strength(); });
            size_t merged_count = 0;
            for (const InterfaceStats& interface : interfaces) {
                const size_t first_root  = coarse_sets.find(interface.first_root);
                const size_t second_root = coarse_sets.find(interface.second_root);
                if (first_root == second_root || interface.mean_strength() > boundary_threshold)
                    continue;
                const double first_area          = coarse_sets.area(first_root);
                const double second_area         = coarse_sets.area(second_root);
                const bool   seamless_interface  = interface.mean_strength() <= 0.15 * boundary_threshold;
                const bool   projection_fragment = std::min(first_area, second_area) < 0.2 * std::max(first_area, second_area);
                if ((seamless_interface || projection_fragment) && coarse_sets.unite(first_root, second_root)) {
                    --coarse_component_count;
                    ++merged_count;
                }
            }
            if (merged_count == 0)
                break;
        }

        for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx)
            coarse_part_labels[face_idx] = coarse_sets.find(face_idx);
    }

    DisjointSet sets(faces);
    DisjointSet crease_sets(faces);
    size_t      component_count = mesh.indices.size();
    if (has_shape_labels) {
        minimum_region_area = total_area / std::max<double>(maximum_regions * 100., 1.);
        // Establish the geometry-only part boundaries independently of SDF.
        // Later cleanup may combine artificial SDF thickness bands, but it
        // must never cross one of these modeled seams.
        if (use_fine_shape_labels) {
            for (const SurfaceEdge& edge : edges) {
                if (edge.boundary_strength > boundary_threshold)
                    break;
                crease_sets.unite(edge.first_face, edge.second_face);
            }
        }
        // SDF finds broad thickness bands; their intersection with the local
        // crease components exposes small attached relief without weakening
        // the established boundaries between larger parts.
        for (const SurfaceEdge& edge : edges)
            if (shape_labels[edge.first_face] == shape_labels[edge.second_face] &&
                coarse_part_labels[edge.first_face] == coarse_part_labels[edge.second_face] &&
                (!use_fine_shape_labels || crease_sets.find(edge.first_face) == crease_sets.find(edge.second_face)) &&
                sets.unite(edge.first_face, edge.second_face))
                --component_count;
    } else {
        for (const SurfaceEdge& edge : edges) {
            if (edge.boundary_strength > boundary_threshold)
                break;
            if (sets.unite(edge.first_face, edge.second_face))
                --component_count;
        }
    }

    // Remove specks caused by tessellation noise before imposing the generous
    // upper limit on useful natural parts.
    for (size_t pass = 0; pass < 8; ++pass) {
        size_t merged_count = 0;
        for (const SurfaceEdge& edge : edges) {
            const size_t first_root  = sets.find(edge.first_face);
            const size_t second_root = sets.find(edge.second_face);
            if (first_root != second_root &&
                (!has_shape_labels || coarse_part_labels[edge.first_face] == coarse_part_labels[edge.second_face]) &&
                (sets.area(first_root) < minimum_region_area || sets.area(second_root) < minimum_region_area) &&
                sets.unite(first_root, second_root)) {
                --component_count;
                ++merged_count;
            }
        }
        if (merged_count == 0)
            break;
    }

    if (has_shape_labels) {
        struct InterfaceStats
        {
            size_t first_root   = 0;
            size_t second_root  = 0;
            double strength_sum = 0.;
            size_t edge_count   = 0;

            [[nodiscard]] double mean_strength() const { return strength_sum / double(edge_count); }
        };

        // Collapse seamless thickness bands on a continuous rounded part.
        // Do not merge merely because one candidate is small: moustaches,
        // eyebrows and similar relief are legitimate small parts, and a
        // size-ratio merge can cascade across their modeled seam.
        for (size_t pass = 0; pass < 8; ++pass) {
            std::unordered_map<uint64_t, InterfaceStats>          interfaces_by_roots;
            std::unordered_map<size_t, std::pair<size_t, size_t>> edge_counts_by_root;
            interfaces_by_roots.reserve(component_count * 3);
            edge_counts_by_root.reserve(component_count);
            for (const SurfaceEdge& edge : edges) {
                if (coarse_part_labels[edge.first_face] != coarse_part_labels[edge.second_face])
                    continue;
                if (crease_sets.find(edge.first_face) != crease_sets.find(edge.second_face))
                    continue;
                size_t first_root  = sets.find(edge.first_face);
                size_t second_root = sets.find(edge.second_face);
                if (first_root == second_root) {
                    auto& [all_edges, strong_edges] = edge_counts_by_root[first_root];
                    ++all_edges;
                    if (edge.boundary_strength > boundary_threshold)
                        ++strong_edges;
                    continue;
                }
                if (first_root > second_root)
                    std::swap(first_root, second_root);
                const uint64_t key            = (static_cast<uint64_t>(first_root) << 32) | static_cast<uint32_t>(second_root);
                auto [interface_it, inserted] = interfaces_by_roots.emplace(key, InterfaceStats{first_root, second_root, 0., 0});
                interface_it->second.strength_sum += edge.boundary_strength;
                ++interface_it->second.edge_count;
            }

            std::vector<InterfaceStats> interfaces;
            interfaces.reserve(interfaces_by_roots.size());
            for (const auto& [key, stats] : interfaces_by_roots)
                interfaces.push_back(stats);
            std::sort(interfaces.begin(), interfaces.end(),
                      [](const InterfaceStats& lhs, const InterfaceStats& rhs) { return lhs.mean_strength() < rhs.mean_strength(); });
            size_t merged_count = 0;
            for (const InterfaceStats& interface : interfaces) {
                const size_t first_root  = sets.find(interface.first_root);
                const size_t second_root = sets.find(interface.second_root);
                if (first_root == second_root || interface.mean_strength() > boundary_threshold)
                    continue;
                const auto density = [&](size_t root) {
                    const auto edge_count = edge_counts_by_root.find(root);
                    return edge_count == edge_counts_by_root.end() || edge_count->second.first == 0 ?
                               0. :
                               double(edge_count->second.second) / double(edge_count->second.first);
                };
                const double first_density              = density(first_root);
                const double second_density             = density(second_root);
                const bool   both_regions_smooth        = first_density < 0.01 && second_density < 0.01;
                const double seamless_fraction          = both_regions_smooth ? 0.15 : 0.08;
                const bool   seamless_interface         = interface.mean_strength() <= seamless_fraction * boundary_threshold;
                const bool   smooth_projection_fragment = std::min(first_density, second_density) < 0.01 &&
                                                        interface.mean_strength() <= 0.12 * boundary_threshold &&
                                                        std::min(sets.area(first_root), sets.area(second_root)) <
                                                            0.5 * std::max(sets.area(first_root), sets.area(second_root));
                if ((seamless_interface || smooth_projection_fragment) && sets.unite(first_root, second_root)) {
                    --component_count;
                    ++merged_count;
                }
            }
            if (merged_count == 0)
                break;
        }
    }

    // A flat printed base or a rounded foot can contain strong triangulated
    // folds around its hidden underside. Keep each connected near-bed part
    // together instead of assigning several colours to those artifacts.
    const auto [minimum_z_it, maximum_z_it] = std::minmax_element(mesh.vertices.begin(), mesh.vertices.end(),
                                                                  [](const Vec3f& lhs, const Vec3f& rhs) { return lhs.z() < rhs.z(); });
    const float base_ceiling                = minimum_z_it->z() + 0.08f * (maximum_z_it->z() - minimum_z_it->z());
    for (const SurfaceEdge& edge : edges) {
        const size_t first_root  = sets.find(edge.first_face);
        const size_t second_root = sets.find(edge.second_face);
        if (first_root != second_root && sets.max_z(first_root) <= base_ceiling && sets.max_z(second_root) <= base_ceiling &&
            sets.unite(first_root, second_root))
            --component_count;
    }

    if (!has_shape_labels && component_count > maximum_regions) {
        for (const SurfaceEdge& edge : edges) {
            if (sets.unite(edge.first_face, edge.second_face) && --component_count <= maximum_regions)
                break;
        }
    }
    if (!report_progress(SegmentationStage::RefiningRegions, 76))
        return {};

    std::unordered_map<size_t, size_t> region_by_root;
    region_by_root.reserve(component_count);
    result.face_regions.resize(mesh.indices.size());
    for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx) {
        const size_t root                = sets.find(face_idx);
        const auto [region_it, inserted] = region_by_root.emplace(root, region_by_root.size());
        if (inserted)
            result.region_seed_faces.push_back(face_idx);
        result.face_regions[face_idx] = region_it->second;
    }

    std::vector<size_t> symmetry_partners;
    if (!report_progress(SegmentationStage::MatchingSymmetry, 84))
        return {};
    if (has_shape_labels && preference >= 0.6f) {
        const double minimum_detail_area = total_area / std::max<double>(maximum_regions * 20., 1.);
        symmetry_partners                = stabilize_bilateral_regions(mesh, faces, edges, minimum_detail_area, result);
    }

    if (!symmetry_partners.empty()) {
        constexpr size_t no_region = std::numeric_limits<size_t>::max();
        for (size_t pass = 0; pass < 8; ++pass) {
            DisjointSet connected_sets(faces);
            for (const SurfaceEdge& edge : edges)
                if (result.face_regions[edge.first_face] == result.face_regions[edge.second_face])
                    connected_sets.unite(edge.first_face, edge.second_face);

            std::vector<size_t> largest_root_by_region(result.region_count(), no_region);
            for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
                const size_t region = result.face_regions[face_idx];
                const size_t root   = connected_sets.find(face_idx);
                if (largest_root_by_region[region] == no_region ||
                    connected_sets.area(root) > connected_sets.area(largest_root_by_region[region]))
                    largest_root_by_region[region] = root;
            }

            std::unordered_map<uint64_t, size_t> boundary_counts;
            for (const SurfaceEdge& edge : edges) {
                const size_t first_region  = result.face_regions[edge.first_face];
                const size_t second_region = result.face_regions[edge.second_face];
                if (first_region == second_region)
                    continue;
                const size_t first_root  = connected_sets.find(edge.first_face);
                const size_t second_root = connected_sets.find(edge.second_face);
                if (first_root != largest_root_by_region[first_region])
                    ++boundary_counts[(uint64_t(first_root) << 32) | uint32_t(second_region)];
                if (second_root != largest_root_by_region[second_region])
                    ++boundary_counts[(uint64_t(second_root) << 32) | uint32_t(first_region)];
            }

            std::vector<size_t> replacement_by_root(faces.size(), no_region);
            std::vector<size_t> best_boundary_count(faces.size(), 0);
            for (const auto& [key, count] : boundary_counts) {
                const size_t root        = size_t(key >> 32);
                const size_t replacement = size_t(uint32_t(key));
                if (count > best_boundary_count[root]) {
                    best_boundary_count[root] = count;
                    replacement_by_root[root] = replacement;
                }
            }

            size_t changed_count = 0;
            for (size_t face_idx = 0; face_idx < faces.size(); ++face_idx) {
                const size_t replacement = replacement_by_root[connected_sets.find(face_idx)];
                if (replacement != no_region && result.face_regions[face_idx] != replacement) {
                    result.face_regions[face_idx] = replacement;
                    ++changed_count;
                }
            }
            if (changed_count == 0)
                break;
        }
    }

    // Mirrored coverage can add a detached island to an otherwise valid
    // region. Split those islands before palette allocation so every region
    // remains usable by contiguous fill tools. The source mapping below keeps
    // all pieces of a bilateral pair on the same palette entry.
    std::vector<size_t> source_region_by_region;
    const bool          has_symmetry_partners = std::any_of(symmetry_partners.begin(), symmetry_partners.end(),
                                                            [](size_t partner) { return partner != std::numeric_limits<size_t>::max(); });
    if (has_symmetry_partners) {
        const std::vector<size_t> source_regions = result.face_regions;
        DisjointSet               connected_sets(faces);
        for (const SurfaceEdge& edge : edges)
            if (source_regions[edge.first_face] == source_regions[edge.second_face])
                connected_sets.unite(edge.first_face, edge.second_face);

        region_by_root.clear();
        result.region_seed_faces.clear();
        for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx) {
            const size_t root                = connected_sets.find(face_idx);
            const auto [region_it, inserted] = region_by_root.emplace(root, region_by_root.size());
            if (inserted) {
                result.region_seed_faces.push_back(face_idx);
                source_region_by_region.push_back(source_regions[face_idx]);
            }
            result.face_regions[face_idx] = region_it->second;
        }
    }

    if (!report_progress(SegmentationStage::AssigningColors, 95))
        return {};
    allocate_palette(edges, faces, palette_size, result);
    if (has_symmetry_partners) {
        std::unordered_map<size_t, size_t> compact_group;
        std::vector<size_t>                group_by_region(result.region_count());
        for (size_t region = 0; region < result.region_count(); ++region) {
            const size_t source     = source_region_by_region[region];
            const size_t partner    = source < symmetry_partners.size() ? symmetry_partners[source] : std::numeric_limits<size_t>::max();
            const size_t old_group  = partner == std::numeric_limits<size_t>::max() ? source : std::min(source, partner);
            group_by_region[region] = compact_group.emplace(old_group, compact_group.size()).first->second;
        }

        std::vector<double>                     group_areas(compact_group.size(), 0.);
        std::vector<std::unordered_set<size_t>> group_neighbors(compact_group.size());
        for (size_t face_idx = 0; face_idx < result.face_regions.size(); ++face_idx)
            group_areas[group_by_region[result.face_regions[face_idx]]] += faces[face_idx].area;
        for (const SurfaceEdge& edge : edges) {
            const size_t first_group  = group_by_region[result.face_regions[edge.first_face]];
            const size_t second_group = group_by_region[result.face_regions[edge.second_face]];
            if (first_group != second_group) {
                group_neighbors[first_group].insert(second_group);
                group_neighbors[second_group].insert(first_group);
            }
        }

        std::vector<size_t> group_order(compact_group.size());
        std::iota(group_order.begin(), group_order.end(), size_t(0));
        std::sort(group_order.begin(), group_order.end(), [&](size_t lhs, size_t rhs) { return group_areas[lhs] > group_areas[rhs]; });
        std::vector<size_t> group_palette(compact_group.size(), std::numeric_limits<size_t>::max());
        std::vector<double> palette_areas(palette_size, 0.);
        for (const size_t group : group_order) {
            size_t best_palette   = 0;
            size_t best_conflicts = std::numeric_limits<size_t>::max();
            for (size_t palette = 0; palette < palette_size; ++palette) {
                const size_t conflicts = std::count_if(group_neighbors[group].begin(), group_neighbors[group].end(),
                                                       [&](size_t neighbor) { return group_palette[neighbor] == palette; });
                if (conflicts < best_conflicts || (conflicts == best_conflicts && palette_areas[palette] < palette_areas[best_palette])) {
                    best_palette   = palette;
                    best_conflicts = conflicts;
                }
            }
            group_palette[group] = best_palette;
            palette_areas[best_palette] += group_areas[group];
        }
        for (size_t region = 0; region < result.region_count(); ++region)
            result.region_palette[region] = group_palette[group_by_region[region]];
    }
    if (!report_progress(SegmentationStage::Complete, 100))
        return {};
    return result;
}

} // namespace Slic3r::AutoPaint
