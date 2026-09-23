#ifndef slic3r_MeshSmoothing_hpp_
#define slic3r_MeshSmoothing_hpp_

#include "TriangleMesh.hpp"

namespace Slic3r {

// Connectivity is cached for a stroke. Never weld, split, remove or reorder faces:
// FacetsAnnotation (including subdivided colour painting) depends on these IDs.
class MeshSmoothingBrush
{
public:
    struct Statistics
    {
        size_t inside_radius       = 0;
        size_t pinned              = 0;
        size_t movable             = 0;
        size_t moved               = 0;
        size_t rejected_faces      = 0;
        double max_displacement_mm = 0.0;
    };
    explicit MeshSmoothingBrush(const indexed_triangle_set& mesh);
    bool apply(indexed_triangle_set& mesh, size_t facet, const Vec3f& hit, const Transform3d& world, float radius, float strength) const;
    const Statistics& last_statistics() const { return m_statistics; }

private:
    mutable Statistics            m_statistics;
    std::vector<std::vector<int>> m_neighbors;
    std::vector<bool>             m_pinned;
    // Coincident seam vertices move together without altering triangle/paint IDs.
    std::vector<int>                 m_representative;
    std::vector<std::vector<size_t>> m_incident_faces;
};

} // namespace Slic3r
#endif
