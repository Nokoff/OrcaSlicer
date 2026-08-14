#ifndef slic3r_AutoPaintSegmentation_hpp_
#define slic3r_AutoPaintSegmentation_hpp_

#include "TriangleMesh.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r::AutoPaint {

struct SegmentationOptions
{
    // Desired number of independently coloured surface regions. Disconnected
    // mesh components always receive their own region, even when there are more
    // components than requested regions.
    size_t target_regions = 2;

    // 0 favors evenly distributed regions. 1 strongly prefers sharp geometric
    // edges as boundaries between regions.
    float boundary_preference = 0.75f;
};

struct SegmentationResult
{
    // One zero-based region index for each input facet.
    std::vector<size_t> face_regions;

    // The facet used to seed each region. Region numbering follows this vector.
    std::vector<size_t> region_seed_faces;

    [[nodiscard]] size_t region_count() const { return region_seed_faces.size(); }
};

// Partitions a triangle mesh into connected, geometry-aware surface regions.
// Sharp changes in face normals are expensive to cross, so the geodesic region
// boundaries tend to follow modeled creases instead of cutting across them.
SegmentationResult segment_by_geometry(const indexed_triangle_set& mesh, const SegmentationOptions& options = {});

} // namespace Slic3r::AutoPaint

#endif // slic3r_AutoPaintSegmentation_hpp_
