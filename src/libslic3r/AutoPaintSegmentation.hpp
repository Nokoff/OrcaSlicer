#ifndef slic3r_AutoPaintSegmentation_hpp_
#define slic3r_AutoPaintSegmentation_hpp_

#include "TriangleMesh.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r::AutoPaint {

struct SegmentationOptions
{
    // Number of colours available to the allocator. The geometry decides how
    // many natural surface regions are needed; colours may be reused.
    size_t target_regions = 2;

    // 0 keeps only the strongest part boundaries. 1 preserves finer concave
    // seams and modeled creases.
    float boundary_preference = 0.75f;
};

struct SegmentationResult
{
    // One zero-based region index for each input facet.
    std::vector<size_t> face_regions;

    // The facet used to seed each region. Region numbering follows this vector.
    std::vector<size_t> region_seed_faces;

    // Palette entry allocated to each region. Adjacent regions are assigned
    // different entries where possible, and all available entries are balanced
    // across the surface.
    std::vector<size_t> region_palette;

    [[nodiscard]] size_t region_count() const { return region_seed_faces.size(); }
};

// Partitions a triangle mesh into natural, geometry-aware surface regions.
// Concave seams and sharp modeled creases separate parts, while uninterrupted
// rounded surfaces remain together instead of being divided into arbitrary
// geodesic blobs.
SegmentationResult segment_by_geometry(const indexed_triangle_set& mesh, const SegmentationOptions& options = {});

} // namespace Slic3r::AutoPaint

#endif // slic3r_AutoPaintSegmentation_hpp_
