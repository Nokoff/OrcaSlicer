#ifndef slic3r_MeshSmoothing_hpp_
#define slic3r_MeshSmoothing_hpp_

#include "TriangleMesh.hpp"
#include "AABBTreeIndirect.hpp"

#include <array>

namespace Slic3r {

struct MeshSmoothingBarycentricLineage
{
    size_t target_face = 0;
    std::array<Vec3f, 3> source_barycentrics;
};

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
        size_t rejected_clearance  = 0;
        double max_displacement_mm = 0.0;
        bool local_refinement       = false;
    };
    explicit MeshSmoothingBrush(const indexed_triangle_set& mesh, bool preserve_sharp_edges = false,
                                const Transform3d& world = Transform3d::Identity());
    // An optional origin vector makes the displacement bound cumulative across
    // repeated dabs; it is indexed like the current mesh, including refinement.
    bool apply(indexed_triangle_set& mesh, size_t facet, const Vec3f& hit, const Transform3d& world, float radius, float strength,
               float max_displacement_mm = 1.f, const std::vector<Vec3f>* stroke_origin = nullptr) const;
    // Locally split brush-intersecting triangles until their world-space edges
    // are small enough for a fine dab. Each output face records its source face.
    bool refine_for_brush(indexed_triangle_set& mesh, size_t seed_face, const Vec3f& hit, const Transform3d& world,
                          float radius, size_t& refined_seed_face, std::vector<size_t>& source_face_lineage,
                          std::vector<MeshSmoothingBarycentricLineage>& source_face_barycentrics,
                          size_t max_added_faces = 250000) const;
    static bool map_stroke_origin_to_refined_mesh(const indexed_triangle_set& source_mesh, const std::vector<Vec3f>& source_origin,
                                                  const indexed_triangle_set& refined_mesh,
                                                  const std::vector<size_t>& source_face_lineage,
                                                  const std::vector<MeshSmoothingBarycentricLineage>& source_face_barycentrics,
                                                  std::vector<Vec3f>& refined_origin);
    const Statistics& last_statistics() const { return m_statistics; }

private:
    void ensure_face_neighbors(const indexed_triangle_set& mesh) const;

    mutable Statistics            m_statistics;
    std::vector<size_t>           m_neighbor_offsets;
    std::vector<int>              m_neighbors;
    mutable std::vector<std::vector<size_t>> m_face_neighbors;
    mutable bool                  m_face_neighbors_built = false;
    std::vector<bool>             m_pinned;
    bool                          m_preserve_sharp_edges = false;
    bool                          m_has_coincident_vertices = false;
    // Coincident seam vertices move together without altering triangle/paint IDs.
    std::vector<int>                 m_representative;
    std::vector<size_t>              m_incident_face_offsets;
    std::vector<size_t>              m_incident_faces;
    AABBTreeIndirect::Tree3f         m_collision_tree;
};

} // namespace Slic3r
#endif
