#include "MeshSmoothing.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <array>
#include <set>
#include <numeric>
#include <limits>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <system_error>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

namespace Slic3r {

namespace {
using SmoothingEdge = std::pair<int, int>;

uint64_t smoothing_edge_key(int a, int b)
{
    if (a > b)
        std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

SmoothingEdge smoothing_edge(int a, int b)
{
    if (a > b)
        std::swap(a, b);
    return {a, b};
}

double point_triangle_distance_squared(const Vec3d& point, const Vec3d& a, const Vec3d& b, const Vec3d& c)
{
    const Vec3d ab = b - a, ac = c - a, ap = point - a;
    const double d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0.0 && d2 <= 0.0) return ap.squaredNorm();

    const Vec3d bp = point - b;
    const double d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0.0 && d4 <= d3) return bp.squaredNorm();

    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        const double v = d1 / (d1 - d3);
        return (point - (a + v * ab)).squaredNorm();
    }

    const Vec3d cp = point - c;
    const double d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0.0 && d5 <= d6) return cp.squaredNorm();

    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        const double w = d2 / (d2 - d6);
        return (point - (a + w * ac)).squaredNorm();
    }

    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return (point - (b + w * (c - b))).squaredNorm();
    }

    const double denom = 1.0 / (va + vb + vc);
    if (!std::isfinite(denom))
        return std::min({ap.squaredNorm(), bp.squaredNorm(), cp.squaredNorm()});
    const double v = vb * denom, w = vc * denom;
    return (point - (a + ab * v + ac * w)).squaredNorm();
}

double point_segment_distance_squared(const Vec3d& point, const Vec3d& a, const Vec3d& b)
{
    const Vec3d edge = b - a;
    const double length_squared = edge.squaredNorm();
    if (length_squared <= 1e-24)
        return (point - a).squaredNorm();
    const double t = std::clamp((point - a).dot(edge) / length_squared, 0.0, 1.0);
    return (point - (a + t * edge)).squaredNorm();
}

bool point_is_in_triangle(const Vec3d& point, const Vec3d& a, const Vec3d& b, const Vec3d& c, double tolerance)
{
    const Vec3d v0 = b - a, v1 = c - a, v2 = point - a;
    const double d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1);
    const double denom = d00 * d11 - d01 * d01;
    if (denom <= 1e-24 || point_triangle_distance_squared(point, a, b, c) > tolerance * tolerance)
        return false;
    const double d20 = v2.dot(v0), d21 = v2.dot(v1);
    const double v = (d11 * d20 - d01 * d21) / denom;
    const double w = (d00 * d21 - d01 * d20) / denom;
    const double u = 1.0 - v - w;
    return u >= -1e-5 && v >= -1e-5 && w >= -1e-5;
}

bool segment_intersects_triangle(const Vec3d& start, const Vec3d& end, const Vec3d& a, const Vec3d& b, const Vec3d& c)
{
    const Vec3d direction = end - start;
    const Vec3d edge1 = b - a, edge2 = c - a;
    const Vec3d p = direction.cross(edge2);
    const double determinant = edge1.dot(p);
    if (std::abs(determinant) <= 1e-12)
        return false;
    const double inverse = 1.0 / determinant;
    const Vec3d t = start - a;
    const double u = t.dot(p) * inverse;
    const Vec3d q = t.cross(edge1);
    const double v = direction.dot(q) * inverse;
    const double along = edge2.dot(q) * inverse;
    return u >= 0.0 && v >= 0.0 && u + v <= 1.0 && along >= 0.0 && along <= 1.0;
}

double segment_segment_distance_squared(const Vec3d& p1, const Vec3d& q1, const Vec3d& p2, const Vec3d& q2)
{
    const Vec3d d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const double a = d1.squaredNorm(), e = d2.squaredNorm(), f = d2.dot(r);
    double s = 0.0, t = 0.0;
    if (a <= 1e-24 && e <= 1e-24)
        return (p1 - p2).squaredNorm();
    if (a <= 1e-24)
        t = std::clamp(f / e, 0.0, 1.0);
    else {
        const double c = d1.dot(r);
        if (e <= 1e-24)
            s = std::clamp(-c / a, 0.0, 1.0);
        else {
            const double b = d1.dot(d2), denominator = a * e - b * b;
            if (denominator > 1e-24)
                s = std::clamp((b * f - c * e) / denominator, 0.0, 1.0);
            t = (b * s + f) / e;
            if (t < 0.0) {
                t = 0.0;
                s = std::clamp(-c / a, 0.0, 1.0);
            } else if (t > 1.0) {
                t = 1.0;
                s = std::clamp((b - c) / a, 0.0, 1.0);
            }
        }
    }
    return (r + s * d1 - t * d2).squaredNorm();
}

bool triangles_intersect(const std::array<Vec3d, 3>& first, const std::array<Vec3d, 3>& second)
{
    for (size_t edge = 0; edge < 3; ++edge) {
        if (segment_intersects_triangle(first[edge], first[(edge + 1) % 3], second[0], second[1], second[2]) ||
            segment_intersects_triangle(second[edge], second[(edge + 1) % 3], first[0], first[1], first[2]))
            return true;
        for (size_t other_edge = 0; other_edge < 3; ++other_edge)
            if (segment_segment_distance_squared(first[edge], first[(edge + 1) % 3], second[other_edge],
                                                second[(other_edge + 1) % 3]) <= 1e-12)
                return true;
    }
    for (const Vec3d& point : first)
        if (point_triangle_distance_squared(point, second[0], second[1], second[2]) <= 1e-12)
            return true;
    for (const Vec3d& point : second)
        if (point_triangle_distance_squared(point, first[0], first[1], first[2]) <= 1e-12)
            return true;
    return false;
}

} // namespace

void MeshSmoothingBrush::ensure_face_neighbors(const indexed_triangle_set& mesh) const
{
    if (m_face_neighbors_built)
        return;
    m_face_neighbors.assign(mesh.indices.size(), {});
    struct EdgeInfo { size_t first_face = 0; unsigned count = 0; };
    std::unordered_map<uint64_t, EdgeInfo> face_edges;
    face_edges.reserve(mesh.indices.size() * 2);
    for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx)
        for (int corner = 0; corner < 3; ++corner) {
            int a = mesh.indices[face_idx][corner], b = mesh.indices[face_idx][(corner + 1) % 3];
            if (a > b)
                std::swap(a, b);
            EdgeInfo& edge = face_edges[smoothing_edge_key(a, b)];
            if (edge.count == 0)
                edge.first_face = face_idx;
            else if (edge.count == 1) {
                m_face_neighbors[edge.first_face].push_back(face_idx);
                m_face_neighbors[face_idx].push_back(edge.first_face);
            }
            ++edge.count;
        }
    m_face_neighbors_built = true;
}

bool MeshSmoothingBrush::refine_for_brush(indexed_triangle_set& mesh, size_t seed_face, const Vec3f& hit, const Transform3d& world,
                                          float radius, size_t& refined_seed_face, std::vector<size_t>& source_face_lineage,
                                          std::vector<MeshSmoothingBarycentricLineage>& source_face_barycentrics,
                                          size_t max_added_faces) const
{
    if (seed_face >= mesh.indices.size() || mesh.vertices.size() + 1 != m_neighbor_offsets.size() ||
        !std::isfinite(radius) || radius <= 0.f || max_added_faces > std::numeric_limits<size_t>::max() - mesh.indices.size() ||
        !world.matrix().allFinite() ||
        !std::isfinite(world.linear().determinant()) || world.linear().determinant() == 0.0)
        return false;
    ensure_face_neighbors(mesh);
    const size_t max_output_faces = mesh.indices.size() + max_added_faces;

    indexed_triangle_set candidate = mesh;
    std::vector<size_t> lineage(candidate.indices.size());
    std::iota(lineage.begin(), lineage.end(), size_t(0));
    using FaceBarycentrics = std::array<Vec3f, 3>;
    const FaceBarycentrics identity_barycentrics{{Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 0.f, 1.f)}};
    std::map<size_t, FaceBarycentrics> barycentrics;

    // Walk only the connected surface under the brush. Precomputed adjacency
    // uses shared mesh edges, so coincident independent shells remain separate.
    std::vector<bool> connected(candidate.indices.size(), false);
    std::vector<size_t> queue{seed_face};
    connected[seed_face] = true;
    const Vec3d world_hit = world * hit.cast<double>();
    const double radius_squared = double(radius) * radius;
    for (size_t idx = 0; idx < queue.size(); ++idx) {
        for (size_t neighbour : m_face_neighbors[queue[idx]])
            if (!connected[neighbour]) {
                const Vec3i32& face = candidate.indices[neighbour];
                const Vec3d a = world * candidate.vertices[face[0]].cast<double>();
                const Vec3d b = world * candidate.vertices[face[1]].cast<double>();
                const Vec3d c = world * candidate.vertices[face[2]].cast<double>();
                if (point_triangle_distance_squared(world_hit, a, b, c) <= radius_squared) {
                    connected[neighbour] = true;
                    queue.push_back(neighbour);
                }
            }
    }

    // Keep the local refinement coarse enough that the small patch remains
    // interactive, while the inserted hit-centred vertex makes a sub-triangle
    // dab effective on the original surface.
    const double target_edge_mm = std::max(1e-4, double(radius) * 4.0);
    bool converged = false;
    for (int pass = 0; pass <= 16; ++pass) {
        std::set<SmoothingEdge> split_edges;
        std::set<size_t> split_face_centroids;
        for (size_t face_index = 0; face_index < candidate.indices.size(); ++face_index) {
            if (!connected[lineage[face_index]])
                continue;
            const Vec3i32& face = candidate.indices[face_index];
            const Vec3d a = world * candidate.vertices[face[0]].cast<double>();
            const Vec3d b = world * candidate.vertices[face[1]].cast<double>();
            const Vec3d c = world * candidate.vertices[face[2]].cast<double>();
            if (point_triangle_distance_squared(world_hit, a, b, c) > radius_squared)
                continue;
            const std::array<double, 3> lengths{{(a - b).norm(), (b - c).norm(), (c - a).norm()}};
            if (*std::max_element(lengths.begin(), lengths.end()) <= target_edge_mm)
                continue;
            const std::array<Vec3d, 3> points{{a, b, c}};
            bool split_near_edge = false;
            for (int edge = 0; edge < 3; ++edge) {
                const size_t next = size_t((edge + 1) % 3);
                if (lengths[edge] > target_edge_mm &&
                    point_segment_distance_squared(world_hit, points[edge], points[next]) <= radius_squared) {
                    split_edges.insert(smoothing_edge(face[edge], face[(edge + 1) % 3]));
                    split_near_edge = true;
                }
            }
            // A hit near the interior of a coarse triangle can be far from all
            // three edges. Split the face interior first, then adaptively
            // refine only child edges that approach the requested footprint.
            if (!split_near_edge)
                split_face_centroids.insert(face_index);
        }
        if (split_edges.empty() && split_face_centroids.empty()) {
            converged = true;
            break;
        }
        if (pass == 16)
            return false;

        std::map<SmoothingEdge, int> midpoint_vertex;
        for (const SmoothingEdge& edge : split_edges) {
            if (candidate.vertices.size() >= size_t(std::numeric_limits<int>::max()))
                return false;
            const int index = int(candidate.vertices.size());
            candidate.vertices.emplace_back(0.5f * (candidate.vertices[edge.first] + candidate.vertices[edge.second]));
            midpoint_vertex.emplace(edge, index);
        }

        indexed_triangle_set refined;
        refined.vertices = std::move(candidate.vertices);
        refined.indices.reserve(std::min(max_output_faces, candidate.indices.size() * 2));
        std::vector<size_t> refined_lineage;
        refined_lineage.reserve(std::min(max_output_faces, candidate.indices.size() * 2));
        std::map<size_t, FaceBarycentrics> refined_barycentrics;
        for (size_t face_index = 0; face_index < candidate.indices.size(); ++face_index) {
            const Vec3i32& face = candidate.indices[face_index];
            const auto existing_barycentrics = barycentrics.find(face_index);
            const FaceBarycentrics& face_barycentrics = existing_barycentrics == barycentrics.end() ?
                identity_barycentrics : existing_barycentrics->second;
            std::array<int, 6> polygon{};
            std::array<Vec3f, 6> polygon_barycentrics{};
            size_t polygon_size = 0;
            for (int edge = 0; edge < 3; ++edge) {
                polygon[polygon_size++] = face[edge];
                polygon_barycentrics[polygon_size - 1] = face_barycentrics[edge];
                const auto midpoint = midpoint_vertex.find(smoothing_edge(face[edge], face[(edge + 1) % 3]));
                if (midpoint != midpoint_vertex.end()) {
                    polygon[polygon_size++] = midpoint->second;
                    polygon_barycentrics[polygon_size - 1] = 0.5f * (face_barycentrics[edge] + face_barycentrics[(edge + 1) % 3]);
                }
            }
            if (split_face_centroids.find(face_index) != split_face_centroids.end() || polygon_size > 3) {
                if (refined.vertices.size() >= size_t(std::numeric_limits<int>::max()))
                    return false;
                const int center_index = int(refined.vertices.size());
                refined.vertices.emplace_back((refined.vertices[face[0]] + refined.vertices[face[1]] + refined.vertices[face[2]]) / 3.f);
                const Vec3f center_barycentrics = (face_barycentrics[0] + face_barycentrics[1] + face_barycentrics[2]) / 3.f;
                for (size_t i = 0; i < polygon_size; ++i) {
                    const size_t next = (i + 1) % polygon_size;
                    refined.indices.emplace_back(polygon[i], polygon[next], center_index);
                    refined_lineage.push_back(lineage[face_index]);
                    const FaceBarycentrics child_barycentrics{{polygon_barycentrics[i], polygon_barycentrics[next], center_barycentrics}};
                    refined_barycentrics.emplace(refined.indices.size() - 1, child_barycentrics);
                    if (refined.indices.size() > max_output_faces)
                        return false;
                }
                continue;
            }
            for (size_t i = 1; i + 1 < polygon_size; ++i) {
                refined.indices.emplace_back(polygon[0], polygon[i], polygon[i + 1]);
                refined_lineage.push_back(lineage[face_index]);
                const FaceBarycentrics child_barycentrics{{polygon_barycentrics[0], polygon_barycentrics[i], polygon_barycentrics[i + 1]}};
                if (polygon_size != 3 || existing_barycentrics != barycentrics.end())
                    refined_barycentrics.emplace(refined.indices.size() - 1, child_barycentrics);
                if (refined.indices.size() > max_output_faces)
                    return false;
            }
        }
        candidate = std::move(refined);
        lineage = std::move(refined_lineage);
        barycentrics = std::move(refined_barycentrics);
    }
    if (!converged)
        return false;

    const double containment_tolerance = std::max(1e-5, double(radius) * 1e-3);
    for (size_t face_index = 0; face_index < candidate.indices.size(); ++face_index) {
        if (lineage[face_index] != seed_face)
            continue;
        const Vec3i32& face = candidate.indices[face_index];
        const Vec3d a = world * candidate.vertices[face[0]].cast<double>();
        const Vec3d b = world * candidate.vertices[face[1]].cast<double>();
        const Vec3d c = world * candidate.vertices[face[2]].cast<double>();
        if (point_is_in_triangle(world_hit, a, b, c, containment_tolerance)) {
            if (candidate.indices.size() == mesh.indices.size())
                return false;
            mesh = std::move(candidate);
            source_face_lineage = std::move(lineage);
            source_face_barycentrics.clear();
            source_face_barycentrics.reserve(barycentrics.size());
            for (const auto& entry : barycentrics)
                source_face_barycentrics.push_back({entry.first, entry.second});
            refined_seed_face = face_index;
            return true;
        }
    }
    return false;
}

MeshSmoothingBrush::MeshSmoothingBrush(const indexed_triangle_set& mesh, bool preserve_sharp_edges, const Transform3d& world)
    : m_neighbor_offsets(mesh.vertices.size() + 1, 0)
    , m_pinned(mesh.vertices.size(), false)
    , m_preserve_sharp_edges(preserve_sharp_edges)
    , m_representative(mesh.vertices.size())
    , m_incident_face_offsets(mesh.vertices.size() + 1, 0)
{
    std::vector<Vec3f> world_vertices;
    world_vertices.resize(mesh.vertices.size());
    auto transform_vertices = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
            world_vertices[i] = (world * mesh.vertices[i].cast<double>()).cast<float>();
    };
    if (mesh.vertices.size() >= 250'000) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, mesh.vertices.size(), 16384), [&](const auto& range) {
            transform_vertices(range.begin(), range.end());
        });
    } else {
        transform_vertices(0, mesh.vertices.size());
    }
    std::future<AABBTreeIndirect::Tree3f> collision_tree_future;
    try {
        collision_tree_future = std::async(std::launch::async, [&world_vertices, &mesh] {
            return AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(world_vertices, mesh.indices);
        });
    } catch (const std::system_error&) {
        m_collision_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(world_vertices, mesh.indices);
    }

    // Synchronize duplicated vertices along a coincident edge, but do not merge
    // every vertex that merely occupies the same point. Point-touching shells
    // are independent surfaces and must not become one brush neighborhood.
    using Position = std::array<float, 3>;
    struct PositionVertex { Position position; int vertex; };
    struct GeometricEdgeInfo { std::array<int, 2> vertices; size_t face; };
    struct CoincidentEdgeFaces { std::array<int, 2> vertices; size_t first_face; size_t second_face; };
    std::iota(m_representative.begin(), m_representative.end(), 0);
    auto find_root = [&](int vertex) {
        int root = vertex;
        while (m_representative[root] != root)
            root = m_representative[root];
        while (m_representative[vertex] != vertex) {
            const int next = m_representative[vertex];
            m_representative[vertex] = root;
            vertex = next;
        }
        return root;
    };
    auto position = [&](int vertex) {
        const Vec3f& p = mesh.vertices[vertex];
        return Position{p.x(), p.y(), p.z()};
    };
    std::vector<PositionVertex> sorted_positions;
    sorted_positions.resize(mesh.vertices.size());
    auto fill_position_keys = [&](size_t begin, size_t end) {
        for (size_t vertex = begin; vertex < end; ++vertex)
            sorted_positions[vertex] = { position(int(vertex)), int(vertex) };
    };
    if (sorted_positions.size() >= 250'000) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, sorted_positions.size(), 16384), [&](const auto& range) {
            fill_position_keys(range.begin(), range.end());
        });
    } else {
        fill_position_keys(0, sorted_positions.size());
    }
    auto position_less = [](const PositionVertex& lhs, const PositionVertex& rhs) {
        if (lhs.position != rhs.position)
            return lhs.position < rhs.position;
        return lhs.vertex < rhs.vertex;
    };
    if (sorted_positions.size() >= 250'000)
        tbb::parallel_sort(sorted_positions.begin(), sorted_positions.end(), position_less);
    else
        std::sort(sorted_positions.begin(), sorted_positions.end(), position_less);
    const bool has_duplicate_positions = std::adjacent_find(sorted_positions.begin(), sorted_positions.end(),
        [](const PositionVertex& lhs, const PositionVertex& rhs) { return lhs.position == rhs.position; }) != sorted_positions.end();
    std::vector<int> geometric_vertex;
    if (has_duplicate_positions) {
        geometric_vertex.resize(mesh.vertices.size());
        for (size_t begin = 0; begin < sorted_positions.size();) {
            size_t end = begin + 1;
            while (end < sorted_positions.size() && sorted_positions[end].position == sorted_positions[begin].position)
                ++end;
            const int representative = sorted_positions[begin].vertex;
            for (size_t i = begin; i < end; ++i)
                geometric_vertex[sorted_positions[i].vertex] = representative;
            begin = end;
        }
    }
    std::vector<PositionVertex>().swap(sorted_positions);
    std::unordered_map<uint64_t, GeometricEdgeInfo> coincident_edges;
    std::vector<CoincidentEdgeFaces> coincident_edge_faces;
    // Ordinary indexed meshes already share vertices along their edges. Only
    // build the geometric edge table when duplicate vertex positions exist.
    if (has_duplicate_positions) {
        coincident_edges.reserve(mesh.indices.size() * 2);
        for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx) {
            const auto& face = mesh.indices[face_idx];
            for (int k = 0; k < 3; ++k) {
                int a = face[k], b = face[(k + 1) % 3];
                int geometric_a = geometric_vertex[a], geometric_b = geometric_vertex[b];
                if (geometric_b < geometric_a) {
                    std::swap(geometric_a, geometric_b);
                    std::swap(a, b);
                }
                if (geometric_a == geometric_b)
                    continue;
                const uint64_t edge = smoothing_edge_key(geometric_a, geometric_b);
                const auto [it, inserted] = coincident_edges.emplace(edge, GeometricEdgeInfo{{a, b}, face_idx});
                if (!inserted) {
                    const int root_a = find_root(a);
                    const int root_b = find_root(it->second.vertices[0]);
                    m_representative[root_a] = root_b;
                    const int root_c = find_root(b);
                    const int root_d = find_root(it->second.vertices[1]);
                    m_representative[root_c] = root_d;
                    if (m_preserve_sharp_edges)
                        coincident_edge_faces.push_back({{a, b}, it->second.face, face_idx});
                }
            }
        }
    }
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        m_representative[i] = find_root(int(i));
        m_has_coincident_vertices |= m_representative[i] != int(i);
    }
    std::vector<Vec3d> face_normals;
    if (m_preserve_sharp_edges) {
        face_normals.reserve(mesh.indices.size());
        for (const Vec3i32& face : mesh.indices) {
            const Vec3d a = world * mesh.vertices[face[0]].cast<double>();
            const Vec3d b = world * mesh.vertices[face[1]].cast<double>();
            const Vec3d c = world * mesh.vertices[face[2]].cast<double>();
            Vec3d normal = (b - a).cross(c - a);
            if (normal.squaredNorm() > 1e-24)
                normal.normalize();
            face_normals.push_back(normal);
        }
        for (const CoincidentEdgeFaces& edge : coincident_edge_faces)
            if (face_normals[edge.first_face].squaredNorm() > 0.0 && face_normals[edge.second_face].squaredNorm() > 0.0 &&
                face_normals[edge.first_face].dot(face_normals[edge.second_face]) < 0.5) {
                m_pinned[m_representative[edge.vertices[0]]] = true;
                m_pinned[m_representative[edge.vertices[1]]] = true;
            }
    }
    std::vector<uint64_t> edges(mesh.indices.size() * 3);
    std::vector<size_t> incident_face_counts(mesh.vertices.size(), 0);
    for (const Vec3i32& face : mesh.indices)
        for (int corner = 0; corner < 3; ++corner)
            ++incident_face_counts[m_representative[face[corner]]];
    for (size_t vertex = 0; vertex < incident_face_counts.size(); ++vertex)
        m_incident_face_offsets[vertex + 1] = m_incident_face_offsets[vertex] + incident_face_counts[vertex];
    m_incident_faces.resize(m_incident_face_offsets.back());
    std::vector<size_t> incident_face_cursor(m_incident_face_offsets.begin(), m_incident_face_offsets.end() - 1);
    for (size_t i = 0; i < mesh.indices.size(); ++i)
        for (int corner = 0; corner < 3; ++corner)
            m_incident_faces[incident_face_cursor[m_representative[mesh.indices[i][corner]]]++] = i;
    if (m_preserve_sharp_edges) {
        struct EdgeInfo { size_t first_face = 0; unsigned count = 0; };
        std::unordered_map<uint64_t, EdgeInfo> face_edges;
        face_edges.reserve(mesh.indices.size() * 2);
        for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx)
            for (int corner = 0; corner < 3; ++corner) {
                int a = mesh.indices[face_idx][corner], b = mesh.indices[face_idx][(corner + 1) % 3];
                if (a > b)
                    std::swap(a, b);
                const uint64_t edge_key = smoothing_edge_key(a, b);
                EdgeInfo& edge = face_edges[edge_key];
                if (edge.count == 0)
                    edge.first_face = face_idx;
                else if (edge.count == 1 && face_normals[edge.first_face].squaredNorm() > 0.0 &&
                         face_normals[face_idx].squaredNorm() > 0.0 && face_normals[edge.first_face].dot(face_normals[face_idx]) < 0.5) {
                    m_pinned[m_representative[a]] = true;
                    m_pinned[m_representative[b]] = true;
                }
                ++edge.count;
            }
    }
    auto fill_edge_keys = [&](size_t begin, size_t end) {
        for (size_t face_idx = begin; face_idx < end; ++face_idx) {
            const auto& face = mesh.indices[face_idx];
            for (int i = 0; i < 3; ++i) {
                int a = m_representative[face[i]], b = m_representative[face[(i + 1) % 3]];
                if (a > b)
                    std::swap(a, b);
                edges[face_idx * 3 + i] = a == b ? std::numeric_limits<uint64_t>::max() : smoothing_edge_key(a, b);
            }
        }
    };
    if (mesh.indices.size() >= 250'000) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, mesh.indices.size(), 4096), [&](const auto& range) {
            fill_edge_keys(range.begin(), range.end());
        });
    } else {
        fill_edge_keys(0, mesh.indices.size());
    }
    if (edges.size() >= 750'000)
        tbb::parallel_sort(edges.begin(), edges.end());
    else
        std::sort(edges.begin(), edges.end());
    while (!edges.empty() && edges.back() == std::numeric_limits<uint64_t>::max())
        edges.pop_back();
    std::vector<size_t> neighbor_counts(mesh.vertices.size(), 0);
    for (size_t edge = 0; edge < edges.size();) {
        size_t end = edge + 1;
        while (end < edges.size() && edges[end] == edges[edge])
            ++end;
        const int a = int(uint32_t(edges[edge] >> 32)), b = int(uint32_t(edges[edge]));
        ++neighbor_counts[a];
        ++neighbor_counts[b];
        // Keep open boundaries and non-manifold edges fixed.
        if (end - edge != 2 || a == b)
            m_pinned[a] = m_pinned[b] = true;
        edge = end;
    }
    for (size_t vertex = 0; vertex < neighbor_counts.size(); ++vertex)
        m_neighbor_offsets[vertex + 1] = m_neighbor_offsets[vertex] + neighbor_counts[vertex];
    m_neighbors.resize(m_neighbor_offsets.back());
    std::vector<size_t> neighbor_cursor(m_neighbor_offsets.begin(), m_neighbor_offsets.end() - 1);
    for (size_t edge = 0; edge < edges.size();) {
        size_t end = edge + 1;
        while (end < edges.size() && edges[end] == edges[edge])
            ++end;
        const int a = int(uint32_t(edges[edge] >> 32)), b = int(uint32_t(edges[edge]));
        m_neighbors[neighbor_cursor[a]++] = b;
        m_neighbors[neighbor_cursor[b]++] = a;
        edge = end;
    }
    // Edge keys are sorted lexicographically by (min_vertex, max_vertex).
    // Consequently, each vertex's neighbors are already appended in ascending
    // order: lower endpoints arrive in ascending groups, followed by its upper
    // endpoints in ascending order. Avoid sorting every CSR slice separately.
    if (collision_tree_future.valid())
        m_collision_tree = collision_tree_future.get();
}

bool MeshSmoothingBrush::map_stroke_origin_to_refined_mesh(
    const indexed_triangle_set& source_mesh, const std::vector<Vec3f>& source_origin, const indexed_triangle_set& refined_mesh,
    const std::vector<size_t>& source_face_lineage, const std::vector<MeshSmoothingBarycentricLineage>& source_face_barycentrics,
    std::vector<Vec3f>& refined_origin)
{
    if (source_origin.size() != source_mesh.vertices.size() || source_face_lineage.size() != refined_mesh.indices.size())
        return false;

    const std::array<Vec3f, 3> identity{{Vec3f(1.f, 0.f, 0.f), Vec3f(0.f, 1.f, 0.f), Vec3f(0.f, 0.f, 1.f)}};
    std::vector<std::array<Vec3f, 3>> barycentrics(refined_mesh.indices.size(), identity);
    for (const MeshSmoothingBarycentricLineage& lineage : source_face_barycentrics) {
        if (lineage.target_face >= barycentrics.size() ||
            !lineage.source_barycentrics[0].allFinite() || !lineage.source_barycentrics[1].allFinite() ||
            !lineage.source_barycentrics[2].allFinite())
            return false;
        barycentrics[lineage.target_face] = lineage.source_barycentrics;
    }

    refined_origin.resize(refined_mesh.vertices.size());
    std::vector<bool> initialized(refined_mesh.vertices.size(), false);
    for (size_t face_idx = 0; face_idx < refined_mesh.indices.size(); ++face_idx) {
        const size_t source_face_idx = source_face_lineage[face_idx];
        if (source_face_idx >= source_mesh.indices.size())
            return false;
        const Vec3i32& source_face = source_mesh.indices[source_face_idx];
        const Vec3i32& refined_face = refined_mesh.indices[face_idx];
        for (int corner = 0; corner < 3; ++corner) {
            if (source_face[corner] < 0 || static_cast<size_t>(source_face[corner]) >= source_origin.size() || refined_face[corner] < 0)
                return false;
            const Vec3f& weights = barycentrics[face_idx][corner];
            const Vec3f origin = weights[0] * source_origin[source_face[0]] + weights[1] * source_origin[source_face[1]] +
                                 weights[2] * source_origin[source_face[2]];
            const size_t refined_vertex = static_cast<size_t>(refined_face[corner]);
            if (refined_vertex >= refined_origin.size() || !origin.allFinite())
                return false;
            if (initialized[refined_vertex] && (refined_origin[refined_vertex] - origin).squaredNorm() > 1e-8f)
                return false;
            refined_origin[refined_vertex] = origin;
            initialized[refined_vertex] = true;
        }
    }
    return std::all_of(initialized.begin(), initialized.end(), [](bool value) { return value; });
}

bool MeshSmoothingBrush::apply(
    indexed_triangle_set& mesh, size_t facet, const Vec3f& hit, const Transform3d& world, float radius, float strength,
    float max_displacement_mm, const std::vector<Vec3f>* stroke_origin) const
{
    m_statistics = {};
    if (facet >= mesh.indices.size() || mesh.vertices.size() + 1 != m_neighbor_offsets.size() || !std::isfinite(radius) || !std::isfinite(strength) ||
        !std::isfinite(max_displacement_mm) || radius <= 0.f || strength <= 0.f || max_displacement_mm <= 0.f ||
        (stroke_origin && stroke_origin->size() != mesh.vertices.size()))
        return false;
    strength = std::min(strength, 1.f);
    std::vector<int> vertex_queue;
    std::unordered_set<int> visited_vertices;
    visited_vertices.reserve(64);
    for (int corner = 0; corner < 3; ++corner) {
        const int vertex = m_representative[mesh.indices[facet][corner]];
        if (visited_vertices.insert(vertex).second)
            vertex_queue.push_back(vertex);
    }
    std::vector<std::pair<int, float>> affected;
    for (size_t i = 0; i < vertex_queue.size(); ++i) {
        const int v = vertex_queue[i];
        const double distance = (world.linear() * (mesh.vertices[v] - hit).cast<double>()).norm();
        if (distance >= radius)
            continue;
        ++m_statistics.inside_radius;
        // Expand only within the world-space brush. Flooding through the
        // entire connected mesh made each dab O(total vertices).
        for (size_t edge = m_neighbor_offsets[v]; edge < m_neighbor_offsets[v + 1]; ++edge) {
            const int neighbour = m_neighbors[edge];
            if (visited_vertices.insert(neighbour).second)
                vertex_queue.push_back(neighbour);
        }
        if (m_pinned[v] || m_neighbor_offsets[v] == m_neighbor_offsets[v + 1]) {
            ++m_statistics.pinned;
            continue;
        }
        const float falloff = float(1.0 - distance / radius);
        affected.emplace_back(v, 0.5f * strength * falloff * falloff);
    }
    bool changed         = false;
    m_statistics.movable = affected.size();
    std::vector<Vec3f> original_positions;
    original_positions.reserve(affected.size());
    for (const auto& item : affected) {
        if (stroke_origin && !(*stroke_origin)[item.first].allFinite())
            return false;
        original_positions.push_back(mesh.vertices[item.first]);
    }
    std::vector<size_t> affected_faces;
    for (const auto& item : affected)
        affected_faces.insert(affected_faces.end(), m_incident_faces.begin() + m_incident_face_offsets[item.first],
                              m_incident_faces.begin() + m_incident_face_offsets[item.first + 1]);
    std::sort(affected_faces.begin(), affected_faces.end());
    affected_faces.erase(std::unique(affected_faces.begin(), affected_faces.end()), affected_faces.end());
    std::vector<size_t> local_faces = affected_faces;
    for (const auto& item : affected) {
        const int v = item.first;
        for (size_t edge = m_neighbor_offsets[v]; edge < m_neighbor_offsets[v + 1]; ++edge) {
            const int neighbor = m_neighbors[edge];
            local_faces.insert(local_faces.end(), m_incident_faces.begin() + m_incident_face_offsets[neighbor],
                               m_incident_faces.begin() + m_incident_face_offsets[neighbor + 1]);
        }
    }
    std::sort(local_faces.begin(), local_faces.end());
    local_faces.erase(std::unique(local_faces.begin(), local_faces.end()), local_faces.end());
    // Several local relaxation steps make a dab useful on dense generated meshes.
    std::vector<Vec3f> vertices = mesh.vertices;
    for (int pass = 0; pass < 12 && !affected.empty(); ++pass) {
        if (pass > 0) {
            for (const auto& item : affected)
                vertices[item.first] = mesh.vertices[item.first];
            if (m_has_coincident_vertices)
                for (size_t vertex = 0; vertex < vertices.size(); ++vertex)
                    if (m_representative[vertex] != int(vertex))
                        vertices[vertex] = mesh.vertices[m_representative[vertex]];
        }
        for (const auto& item : affected) {
            const int v = item.first;
            Vec3f     average = Vec3f::Zero();
            for (size_t edge = m_neighbor_offsets[v]; edge < m_neighbor_offsets[v + 1]; ++edge)
                average += mesh.vertices[m_neighbors[edge]];
            average /= float(m_neighbor_offsets[v + 1] - m_neighbor_offsets[v]);
            vertices[v] += item.second * (average - mesh.vertices[v]);
        }
        // Limit motion in world mm from the start of the stroke when an origin
        // is supplied; standalone brush calls use the start of this dab.
        for (size_t i = 0; i < affected.size(); ++i) {
            const int v = affected[i].first;
            const Vec3f& origin = stroke_origin ? (*stroke_origin)[v] : original_positions[i];
            Vec3d delta = (vertices[v] - origin).cast<double>();
            const double displacement = (world.linear() * delta).norm();
            if (displacement > max_displacement_mm)
                vertices[v] = origin + (delta * (max_displacement_mm / displacement)).cast<float>();
        }
        // Clamp only troublesome vertices, rather than cancelling the entire dab
        // because a single skinny/non-manifold triangle would flip.
        bool valid = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (m_has_coincident_vertices)
                for (size_t v = 0; v < vertices.size(); ++v)
                    vertices[v] = vertices[m_representative[v]];
            std::vector<int> blocked;
            for (size_t face : affected_faces) {
                const auto& f      = mesh.indices[face];
                const Vec3d before = (mesh.vertices[f[1]] - mesh.vertices[f[0]])
                                         .cast<double>()
                                         .cross((mesh.vertices[f[2]] - mesh.vertices[f[0]]).cast<double>());
                const Vec3d after = (vertices[f[1]] - vertices[f[0]]).cast<double>().cross((vertices[f[2]] - vertices[f[0]]).cast<double>());
                if (!after.allFinite() || (before.squaredNorm() > 0.0 && before.dot(after) <= 0.0)) {
                    ++m_statistics.rejected_faces;
                    for (int i = 0; i < 3; ++i)
                        blocked.push_back(m_representative[f[i]]);
                }
            }

            // Reject motion that crosses or moves into a nearby, unrelated
            // surface. This protects thin walls and nearby shells in addition
            // to the local face-orientation test above. The AABB tree is built
            // once for the stroke; face IDs and connectivity remain stable.
            if (!m_collision_tree.empty()) {
                const double clearance = std::max(0.005, std::min(double(max_displacement_mm) * 0.1, double(radius) * 0.05));
                // The tree is built at stroke start. Other vertices may since
                // have moved by as much as the stroke displacement cap, so
                // include that possible drift when finding candidate faces.
                const double broad_phase = clearance + double(max_displacement_mm);
                auto face_moved_since_stroke_start = [&](size_t face_idx) {
                    if (!stroke_origin)
                        return false;
                    const Vec3i32& face = mesh.indices[face_idx];
                    for (int corner = 0; corner < 3; ++corner)
                        if (mesh.vertices[face[corner]] != (*stroke_origin)[face[corner]])
                            return true;
                    return false;
                };
                for (size_t i = 0; i < affected.size(); ++i) {
                    const int v = affected[i].first;
                    const Vec3d start = world * original_positions[i].cast<double>();
                    const Vec3d end = world * vertices[v].cast<double>();
                    if ((end - start).squaredNorm() <= 1e-16)
                        continue;
                    Eigen::AlignedBox<float, 3> swept(start.cast<float>(), start.cast<float>());
                    swept.extend(end.cast<float>());
                    swept.min().array() -= float(broad_phase);
                    swept.max().array() += float(broad_phase);
                    Eigen::AlignedBox<float, 3> narrow_swept = swept;
                    narrow_swept.min().array() += float(max_displacement_mm);
                    narrow_swept.max().array() -= float(max_displacement_mm);
                    bool unsafe = false;
                    AABBTreeIndirect::traverse(m_collision_tree, AABBTreeIndirect::intersecting(swept), [&](const auto& node) {
                        const size_t face_idx = node.idx;
                        if (face_idx >= mesh.indices.size() || std::binary_search(local_faces.begin(), local_faces.end(), face_idx))
                            return true;
                        if (stroke_origin && !face_moved_since_stroke_start(face_idx) && !narrow_swept.intersects(node.bbox))
                            return true;
                        const auto& face = mesh.indices[face_idx];
                        const Vec3d a = world * mesh.vertices[face[0]].cast<double>();
                        const Vec3d b = world * mesh.vertices[face[1]].cast<double>();
                        const Vec3d c = world * mesh.vertices[face[2]].cast<double>();
                        const Vec3d motion = end - start;
                        const double start_fraction = std::min(0.05, std::max(1e-6, 1e-5 / motion.norm()));
                        const bool crosses = segment_intersects_triangle(start + start_fraction * motion, end, a, b, c);
                        const double start_distance = point_triangle_distance_squared(start, a, b, c);
                        const double end_distance = point_triangle_distance_squared(end, a, b, c);
                        unsafe = crosses || (end_distance < clearance * clearance && end_distance + 1e-10 < start_distance);
                        return !unsafe;
                    });
                    if (unsafe) {
                        blocked.push_back(m_representative[v]);
                        ++m_statistics.rejected_clearance;
                    }
                }

                // Also guard the swept patch faces. Vertex paths alone can
                // miss a surface crossing that occurs along an edge or in the
                // interior of a deforming triangle.
                auto world_face = [&](const Vec3i32& face, const std::vector<Vec3f>& positions) {
                    return std::array<Vec3d, 3>{world * positions[face[0]].cast<double>(),
                                                world * positions[face[1]].cast<double>(),
                                                world * positions[face[2]].cast<double>()};
                };
                for (size_t affected_face_idx = 0; affected_face_idx < affected_faces.size(); ++affected_face_idx) {
                    const size_t face_idx = affected_faces[affected_face_idx];
                    const Vec3i32& face = mesh.indices[face_idx];
                    const auto before = world_face(face, mesh.vertices);
                    const auto after = world_face(face, vertices);
                    if (before == after)
                        continue;

                    Eigen::AlignedBox<float, 3> swept_box;
                    double max_motion = 0.0;
                    for (size_t corner = 0; corner < 3; ++corner) {
                        swept_box.extend(before[corner].cast<float>());
                        swept_box.extend(after[corner].cast<float>());
                        max_motion = std::max(max_motion, (after[corner] - before[corner]).norm());
                    }
                    swept_box.min().array() -= float(broad_phase);
                    swept_box.max().array() += float(broad_phase);
                    Eigen::AlignedBox<float, 3> narrow_swept_box = swept_box;
                    narrow_swept_box.min().array() += float(max_displacement_mm);
                    narrow_swept_box.max().array() -= float(max_displacement_mm);

                    const double sample_spacing = std::max(0.0025, clearance * 0.5);
                    const size_t sample_count = std::clamp(size_t(std::ceil(max_motion / sample_spacing)), size_t(1), size_t(128));
                    // Exclude the initial boundary of the swept prism. It may
                    // touch an unrelated shell at a pre-existing shared point;
                    // intermediate samples still detect motion into that shell.
                    const double sweep_start_fraction = std::min(0.05, std::max(1e-6, 1e-5 / max_motion));
                    std::array<std::array<Vec3d, 3>, 6> swept_edges;
                    for (size_t edge = 0; edge < 3; ++edge) {
                        const size_t next = (edge + 1) % 3;
                        const Vec3d start = before[edge] + sweep_start_fraction * (after[edge] - before[edge]);
                        const Vec3d start_next = before[next] + sweep_start_fraction * (after[next] - before[next]);
                        swept_edges[edge * 2] = {start, start_next, after[next]};
                        swept_edges[edge * 2 + 1] = {start, after[next], after[edge]};
                    }
                    bool unsafe_face = false;
                    AABBTreeIndirect::traverse(m_collision_tree, AABBTreeIndirect::intersecting(swept_box), [&](const auto& node) {
                        const size_t other_face_idx = node.idx;
                        if (other_face_idx >= mesh.indices.size() ||
                            std::binary_search(local_faces.begin(), local_faces.end(), other_face_idx) || other_face_idx == face_idx)
                            return true;
                        if (stroke_origin && !face_moved_since_stroke_start(other_face_idx) &&
                            !narrow_swept_box.intersects(node.bbox))
                            return true;
                        const Vec3i32& other_face = mesh.indices[other_face_idx];
                        const auto other = world_face(other_face, mesh.vertices);
                        if (triangles_intersect(before, other))
                            return true; // Retain pre-existing malformed geometry without making it worse here.

                        for (size_t sample = 1; sample <= sample_count && !unsafe_face; ++sample) {
                            const double t = double(sample) / double(sample_count);
                            std::array<Vec3d, 3> intermediate;
                            for (size_t corner = 0; corner < 3; ++corner)
                                intermediate[corner] = before[corner] + t * (after[corner] - before[corner]);
                            unsafe_face = triangles_intersect(intermediate, other);
                        }
                        for (const auto& swept_edge : swept_edges)
                            if (!unsafe_face && triangles_intersect(swept_edge, other))
                                unsafe_face = true;
                        return !unsafe_face;
                    });
                    if (unsafe_face) {
                        for (int corner = 0; corner < 3; ++corner)
                            blocked.push_back(m_representative[face[corner]]);
                        ++m_statistics.rejected_clearance;
                    }
                }
            }
            if (blocked.empty()) {
                valid = true;
                break;
            }
            for (int v : blocked)
                vertices[v] = mesh.vertices[v];
        }
        if (!valid)
            break;
        if (!valid)
            break;
        bool pass_changed = false;
        for (const auto& item : affected)
            pass_changed |= (vertices[item.first] - mesh.vertices[item.first]).squaredNorm() > 1e-16f;
        if (!pass_changed)
            break;
        mesh.vertices.swap(vertices);
        changed = true;
    }
    for (size_t i = 0; i < affected.size(); ++i) {
        const double dab_displacement =
            (world.linear() * (mesh.vertices[affected[i].first] - original_positions[i]).cast<double>()).norm();
        const Vec3f& origin = stroke_origin ? (*stroke_origin)[affected[i].first] : original_positions[i];
        const double stroke_displacement = (world.linear() * (mesh.vertices[affected[i].first] - origin).cast<double>()).norm();
        if (dab_displacement > 0.0)
            ++m_statistics.moved;
        m_statistics.max_displacement_mm = std::max(m_statistics.max_displacement_mm, stroke_displacement);
    }
    return changed;
}

} // namespace Slic3r
