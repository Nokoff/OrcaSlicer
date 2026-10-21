#ifndef slic3r_SurfaceWrap_hpp_
#define slic3r_SurfaceWrap_hpp_

#include "TriangleMesh.hpp"
#include <array>

namespace Slic3r {

struct SurfaceWrapOptions
{
    // Source local XY is the footprint, local Z is the preserved relief thickness.
    // Corners: bottom left, bottom right, top right, top left, viewed along -Z.
    std::array<Vec2d, 4> corners{Vec2d::Zero(), Vec2d::Zero(), Vec2d::Zero(), Vec2d::Zero()};
    double               corner_lift   = 0.;
    double               offset        = -0.05;
    double               max_edge      = 1.;
    size_t               max_triangles = 250000;
};

// Both meshes are in the source's local coordinates. Projects onto the first
// target surface seen from +Z. Throws std::runtime_error without changing inputs
// when coverage, corner orientation or the tessellation budget is insufficient.
indexed_triangle_set surface_wrap(const indexed_triangle_set& source, const indexed_triangle_set& target, const SurfaceWrapOptions& options);

} // namespace Slic3r
#endif
