#include "MeshSmoothing.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <array>

namespace Slic3r {

MeshSmoothingBrush::MeshSmoothingBrush(const indexed_triangle_set& mesh)
    : m_neighbors(mesh.vertices.size())
    , m_pinned(mesh.vertices.size(), false)
    , m_representative(mesh.vertices.size())
    , m_incident_faces(mesh.vertices.size())
{
    std::map<std::array<float, 3>, int> positions;
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const Vec3f& p      = mesh.vertices[i];
        m_representative[i] = positions.emplace(std::array<float, 3>{p.x(), p.y(), p.z()}, int(i)).first->second;
    }
    std::map<std::pair<int, int>, unsigned> edges;
    for (size_t i = 0; i < mesh.indices.size(); ++i)
        for (int k = 0; k < 3; ++k)
            m_incident_faces[m_representative[mesh.indices[i][k]]].push_back(i);
    for (const auto& face : mesh.indices)
        for (int i = 0; i < 3; ++i) {
            int a = m_representative[face[i]], b = m_representative[face[(i + 1) % 3]];
            if (a == b)
                continue;
            if (a > b)
                std::swap(a, b);
            ++edges[{a, b}];
        }
    for (const auto& entry : edges) {
        const int a = entry.first.first, b = entry.first.second;
        m_neighbors[a].push_back(b);
        m_neighbors[b].push_back(a);
        // Keep open boundaries and non-manifold edges fixed.
        if (entry.second != 2 || a == b)
            m_pinned[a] = m_pinned[b] = true;
    }
}

bool MeshSmoothingBrush::apply(
    indexed_triangle_set& mesh, size_t facet, const Vec3f& hit, const Transform3d& world, float radius, float strength) const
{
    m_statistics = {};
    if (facet >= mesh.indices.size() || mesh.vertices.size() != m_neighbors.size() || !std::isfinite(radius) || !std::isfinite(strength) ||
        radius <= 0.f || strength <= 0.f)
        return false;
    strength = std::min(strength, 1.f);
    std::vector<bool> visited(mesh.vertices.size(), false);
    std::vector<int>  queue;
    for (int i = 0; i < 3; ++i) {
        int v = m_representative[mesh.indices[facet][i]];
        if (!visited[v]) {
            visited[v] = true;
            queue.push_back(v);
        }
    }
    std::vector<std::pair<int, float>> affected;
    for (size_t i = 0; i < queue.size(); ++i) {
        const int    v        = queue[i];
        const double distance = (world.linear() * (mesh.vertices[v] - hit).cast<double>()).norm();
        if (distance >= radius)
            continue;
        ++m_statistics.inside_radius;
        // Walk only the connected surface under the brush, not nearby shells.
        for (int n : m_neighbors[v])
            if (!visited[n]) {
                visited[n] = true;
                queue.push_back(n);
            }
        if (m_pinned[v] || m_neighbors[v].empty()) {
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
    for (const auto& item : affected)
        original_positions.push_back(mesh.vertices[item.first]);
    std::vector<size_t> affected_faces;
    for (const auto& item : affected)
        affected_faces.insert(affected_faces.end(), m_incident_faces[item.first].begin(), m_incident_faces[item.first].end());
    std::sort(affected_faces.begin(), affected_faces.end());
    affected_faces.erase(std::unique(affected_faces.begin(), affected_faces.end()), affected_faces.end());
    // Several local relaxation steps make a dab useful on dense generated meshes.
    for (int pass = 0; pass < 12 && !affected.empty(); ++pass) {
        auto vertices = mesh.vertices;
        for (const auto& item : affected) {
            const int v       = item.first;
            Vec3f     average = Vec3f::Zero();
            for (int n : m_neighbors[v])
                average += mesh.vertices[n];
            average /= float(m_neighbors[v].size());
            vertices[v] += item.second * (average - mesh.vertices[v]);
        }
        // Clamp only troublesome vertices, rather than cancelling the entire dab
        // because a single skinny/non-manifold triangle would flip.
        bool valid = false;
        for (int attempt = 0; attempt < 8; ++attempt) {
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
            if (blocked.empty()) {
                valid = true;
                break;
            }
            for (int v : blocked)
                vertices[v] = mesh.vertices[v];
        }
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
        const double displacement = (world.linear() * (mesh.vertices[affected[i].first] - original_positions[i]).cast<double>()).norm();
        if (displacement > 0.0)
            ++m_statistics.moved;
        m_statistics.max_displacement_mm = std::max(m_statistics.max_displacement_mm, displacement);
    }
    return changed;
}

} // namespace Slic3r
