#include "AutoPaintSegmentation.hpp"
#include "MeshBoolean.hpp"
#include "QuadricEdgeCollapse.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Slic3r::AutoPaint {
namespace {

struct FaceGeometry
{
    Vec3f normal = Vec3f::UnitZ();
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
        geometry.area  = 0.5f * twice_area;
        geometry.max_z = std::max({a.z(), b.z(), c.z()});
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

    const float  preference         = std::clamp(options.boundary_preference, 0.f, 1.f);
    const float  boundary_threshold = 18.f - 14.f * preference;
    const size_t palette_size       = std::clamp(options.target_regions, size_t(1), mesh.indices.size());
    // High detail asks the global shape pass for more candidates. The later
    // crease pass and cleanup decide which candidates are genuine parts.
    const size_t detail_multiplier   = 4 + static_cast<size_t>(std::lround(2.f * preference));
    const size_t maximum_regions     = std::min(mesh.indices.size(), std::max(palette_size * detail_multiplier, palette_size + 4));
    const double total_area          = std::accumulate(faces.begin(), faces.end(), 0.,
                                                       [](double area, const FaceGeometry& face) { return area + face.area; });
    double       minimum_region_area = total_area / std::max<double>(maximum_regions * 250., 1.);

    std::vector<size_t> shape_labels;
    if (mesh.indices.size() >= 2000) {
        try {
            indexed_triangle_set proxy_mesh       = mesh;
            const size_t         proxy_face_limit = 20000 + static_cast<size_t>(std::lround(10000.f * preference));
            if (proxy_mesh.indices.size() > proxy_face_limit)
                its_quadric_edge_collapse(proxy_mesh, uint32_t(proxy_face_limit));
            const double smoothing = 0.8 - 0.72 * preference;
            shape_labels           = MeshBoolean::cgal::segment_face_ids(proxy_mesh, mesh, smoothing, maximum_regions, 25);
        } catch (...) {
            // Open, non-manifold, or otherwise unsuitable meshes still receive
            // the fast local crease-based fallback below.
            shape_labels.clear();
        }
    }

    DisjointSet sets(faces);
    size_t      component_count  = mesh.indices.size();
    const bool  has_shape_labels = shape_labels.size() == mesh.indices.size();
    if (has_shape_labels) {
        minimum_region_area = total_area / std::max<double>(maximum_regions * 100., 1.);
        // SDF finds the broad thickness bands; preserving modeled concave
        // creases within a band adds the smaller attached features.
        for (const SurfaceEdge& edge : edges)
            if (shape_labels[edge.first_face] == shape_labels[edge.second_face] && edge.concave_strength <= boundary_threshold &&
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

        // Collapse seamless thickness bands on a continuous rounded part and
        // absorb highly unbalanced projection fragments. Genuine small
        // features remain separated by their concave or sharp boundary.
        for (size_t pass = 0; pass < 8; ++pass) {
            std::unordered_map<uint64_t, InterfaceStats> interfaces_by_roots;
            interfaces_by_roots.reserve(component_count * 3);
            for (const SurfaceEdge& edge : edges) {
                size_t first_root  = sets.find(edge.first_face);
                size_t second_root = sets.find(edge.second_face);
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
                const size_t first_root  = sets.find(interface.first_root);
                const size_t second_root = sets.find(interface.second_root);
                if (first_root == second_root || interface.mean_strength() > boundary_threshold)
                    continue;
                const double first_area          = sets.area(first_root);
                const double second_area         = sets.area(second_root);
                const bool   seamless_interface  = interface.mean_strength() <= 0.15 * boundary_threshold;
                const bool   projection_fragment = std::min(first_area, second_area) < 0.2 * std::max(first_area, second_area);
                if ((seamless_interface || projection_fragment) && sets.unite(first_root, second_root)) {
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

    if (has_shape_labels) {
        snap_shape_boundaries_to_geometry(edges, boundary_threshold, result.face_regions);

        // Boundary movement may trim a narrow bridge. Rebuild connected
        // regions so every resulting paint region remains flood-fillable.
        DisjointSet snapped_sets(faces);
        for (const SurfaceEdge& edge : edges)
            if (result.face_regions[edge.first_face] == result.face_regions[edge.second_face])
                snapped_sets.unite(edge.first_face, edge.second_face);

        region_by_root.clear();
        result.region_seed_faces.clear();
        for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx) {
            const size_t root                = snapped_sets.find(face_idx);
            const auto [region_it, inserted] = region_by_root.emplace(root, region_by_root.size());
            if (inserted)
                result.region_seed_faces.push_back(face_idx);
            result.face_regions[face_idx] = region_it->second;
        }
    }

    allocate_palette(edges, faces, palette_size, result);
    return result;
}

} // namespace Slic3r::AutoPaint
