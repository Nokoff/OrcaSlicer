#ifndef slic3r_ModelVolumeUtils_hpp_
#define slic3r_ModelVolumeUtils_hpp_

#include "libslic3r/Model.hpp"
#include "libslic3r/AABBMesh.hpp"

namespace Slic3r::GUI {

// Cut connectors have no individual row in the object list.
inline int model_volume_index(const ModelObject& object, int visible_index)
{
    if (visible_index < 0)
        return -1;
    for (size_t index = 0; index < object.volumes.size(); ++index) {
        if (object.is_cut() && object.volumes[index]->is_cut_connector())
            continue;
        if (visible_index-- == 0)
            return static_cast<int>(index);
    }
    return -1;
}

inline int visible_volume_index(const ModelObject& object, int model_index)
{
    if (model_index < 0 || static_cast<size_t>(model_index) >= object.volumes.size())
        return -1;
    int visible_index = 0;
    for (int index = 0; index <= model_index; ++index) {
        if (object.is_cut() && object.volumes[index]->is_cut_connector()) {
            if (index == model_index)
                return -1;
            continue;
        }
        if (index == model_index)
            return visible_index;
        ++visible_index;
    }
    return -1;
}

inline bool is_emboss_surface(const ModelVolume& volume) { return volume.is_model_part() && !volume.is_cut_connector(); }

// Convert source-local geometry into the reference volume's coordinates.
inline Transform3d volume_to_volume_transform(const Transform3d& source, const Transform3d& reference)
{
    return reference.inverse() * source;
}

struct VolumeClosestPoint
{
    Vec3d  point;            // In volume-local coordinates.
    double squared_distance; // In world coordinates.
};

inline VolumeClosestPoint closest_point_on_volume(const AABBMesh& mesh, const Transform3d& transform, const Vec3d& world_point)
{
    const Vec3d local_point = transform.inverse() * world_point;
    int         face_index  = 0;
    Vec3d       closest_point;
    mesh.squared_distance(local_point, face_index, closest_point);
    return {closest_point, (world_point - transform * closest_point).squaredNorm()};
}

} // namespace Slic3r::GUI

#endif
