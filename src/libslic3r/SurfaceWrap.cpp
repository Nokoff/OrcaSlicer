#include "SurfaceWrap.hpp"
#include "AABBMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>

namespace Slic3r {
namespace {
using Edge = std::pair<int, int>;
Edge edge(int a, int b) { return std::minmax(a, b); }

void refine(indexed_triangle_set& mesh, double max_edge, size_t limit)
{
    if (mesh.indices.size() > limit)
        throw std::runtime_error("The shape exceeds the wrap triangle budget. Simplify it first.");
    for (;;) {
        std::map<Edge, int> midpoints;
        for (const auto& face : mesh.indices)
            for (int i = 0; i < 3; ++i) {
                int a = face[i], b = face[(i + 1) % 3];
                if ((mesh.vertices[a].head<2>() - mesh.vertices[b].head<2>()).squaredNorm() > max_edge * max_edge)
                    midpoints.emplace(edge(a, b), -1);
            }
        if (midpoints.empty())
            return;
        size_t count = mesh.indices.size();
        for (const auto& face : mesh.indices)
            for (int i = 0; i < 3; ++i)
                count += midpoints.count(edge(face[i], face[(i + 1) % 3]));
        if (count > limit)
            throw std::runtime_error("Wrapping needs too many triangles. Increase the mesh spacing or simplify the shape.");
        for (auto& item : midpoints) {
            item.second          = int(mesh.vertices.size());
            const Vec3f midpoint = 0.5f * (mesh.vertices[item.first.first] + mesh.vertices[item.first.second]);
            mesh.vertices.push_back(midpoint);
        }
        std::vector<stl_triangle_vertex_indices> faces;
        faces.reserve(count);
        for (const auto& f : mesh.indices) {
            int m[3], n = 0;
            for (int i = 0; i < 3; ++i) {
                const auto it = midpoints.find(edge(f[i], f[(i + 1) % 3]));
                m[i]          = it == midpoints.end() ? -1 : it->second;
                n += m[i] >= 0;
            }
            if (n == 0)
                faces.push_back(f);
            else if (n == 3) {
                faces.emplace_back(f[0], m[0], m[2]);
                faces.emplace_back(m[0], f[1], m[1]);
                faces.emplace_back(m[2], m[1], f[2]);
                faces.emplace_back(m[0], m[1], m[2]);
            } else {
                int i = 0;
                while (n == 1 ? m[i] < 0 : !(m[i] >= 0 && m[(i + 1) % 3] >= 0))
                    ++i;
                const int a = f[i], b = f[(i + 1) % 3], c = f[(i + 2) % 3], ab = m[i];
                if (n == 1) {
                    faces.emplace_back(a, ab, c);
                    faces.emplace_back(ab, b, c);
                } else {
                    const int bc = m[(i + 1) % 3];
                    faces.emplace_back(ab, b, bc);
                    faces.emplace_back(a, ab, c);
                    faces.emplace_back(ab, bc, c);
                }
            }
        }
        mesh.indices = std::move(faces);
    }
}
} // namespace

indexed_triangle_set surface_wrap(const indexed_triangle_set& source, const indexed_triangle_set& target, const SurfaceWrapOptions& options)
{
    if (source.vertices.empty() || source.indices.empty() || target.vertices.empty() || target.indices.empty())
        throw std::runtime_error("Select a shape and a non-empty target surface.");
    if (!std::isfinite(options.max_edge) || options.max_edge < 0.01 || !std::isfinite(options.offset) || !std::isfinite(options.corner_lift))
        throw std::runtime_error("Wrap settings must be finite and mesh spacing must be at least 0.01 mm.");
    Vec3d lo = source.vertices.front().cast<double>(), hi = lo;
    for (const auto& v : source.vertices) {
        if (!v.allFinite())
            throw std::runtime_error("The source contains invalid vertices.");
        lo = lo.cwiseMin(v.cast<double>());
        hi = hi.cwiseMax(v.cast<double>());
    }
    const Vec3d size = hi - lo;
    if (size.x() < 1e-6 || size.y() < 1e-6)
        throw std::runtime_error("The shape must have a non-zero footprint in its local XY plane.");
    for (const auto& corner : options.corners)
        if (!corner.allFinite())
            throw std::runtime_error("Corner offsets must be finite.");
    const Vec2d a         = lo.head<2>() + options.corners[0];
    const Vec2d b         = Vec2d(hi.x(), lo.y()) + options.corners[1];
    const Vec2d c         = hi.head<2>() + options.corners[2];
    const Vec2d d         = Vec2d(lo.x(), hi.y()) + options.corners[3];
    auto        footprint = [&](double u, double v) -> Vec2d {
        Vec2d p = (1 - v) * ((1 - u) * a + u * b) + v * ((1 - u) * d + u * c);
        p.y() += options.corner_lift * std::pow(2 * u - 1, 2);
        return p;
    };
    auto jacobian = [&](double u, double v) {
        Vec2d       du = (1 - v) * (b - a) + v * (c - d);
        const Vec2d dv = (1 - u) * (d - a) + u * (c - b);
        du.y() += 4 * options.corner_lift * (2 * u - 1);
        return du.x() * dv.y() - du.y() * dv.x();
    };
    // The determinant is quadratic in u and linear in v. Check both v
    // boundaries and each quadratic's minimum to reject folded footprints.
    for (double v : {0., 1.}) {
        const double f0 = jacobian(0, v), f1 = jacobian(1, v), fm = jacobian(0.5, v);
        const double qa = 2 * (f0 + f1 - 2 * fm), qb = f1 - f0 - qa;
        double       minimum = std::min(f0, f1);
        if (qa > 0) {
            const double u = std::clamp(-qb / (2 * qa), 0., 1.);
            minimum        = std::min(minimum, jacobian(u, v));
        }
        if (!std::isfinite(minimum) || minimum <= size.x() * size.y() * 1e-6)
            throw std::runtime_error("The corner positions fold or collapse the shape. Reduce the offsets.");
    }
    double top = -std::numeric_limits<double>::infinity();
    for (const auto& v : target.vertices) {
        if (!v.allFinite())
            throw std::runtime_error("The target contains invalid vertices.");
        top = std::max(top, double(v.z()));
    }
    const AABBMesh       surface(target);
    indexed_triangle_set result = source;
    // Compensate for expanded footprints so spacing is also bounded after edits.
    const double expansion = std::max({1., (b - a).norm() / size.x(), (c - d).norm() / size.x(), (d - a).norm() / size.y(),
                                       (c - b).norm() / size.y()}) +
                             4 * std::abs(options.corner_lift) / size.x();
    refine(result, options.max_edge / expansion, options.max_triangles);
    for (auto& vertex : result.vertices) {
        const Vec3d p   = vertex.cast<double>();
        const Vec2d xy  = footprint((p.x() - lo.x()) / size.x(), (p.y() - lo.y()) / size.y());
        const auto  hit = surface.query_ray_hit(Vec3d(xy.x(), xy.y(), top + 1.), -Vec3d::UnitZ());
        if (!hit.is_hit())
            throw std::runtime_error(
                "Part of the shape misses the target. Move it onto the surface, shrink it, or reduce the corner offsets.");
        vertex = Vec3d(xy.x(), xy.y(), hit.position().z() + p.z() - lo.z() + options.offset).cast<float>();
        if (!vertex.allFinite())
            throw std::runtime_error("The wrapped shape exceeds the supported coordinate range.");
    }
    return result;
}
} // namespace Slic3r
