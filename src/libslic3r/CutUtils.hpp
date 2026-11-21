#ifndef slic3r_CutUtils_hpp_
#define slic3r_CutUtils_hpp_

#include "enum_bitmask.hpp"
#include "Point.hpp"
#include "Model.hpp"

#include <vector>

namespace Slic3r {

using ModelObjectPtrs = std::vector<ModelObject*>;

enum class ModelObjectCutAttribute : int { KeepUpper, KeepLower, KeepAsParts, FlipUpper, FlipLower, PlaceOnCutUpper, PlaceOnCutLower, CreateDowels, InvalidateCutInfo, KeepPaint };
using ModelObjectCutAttributes = enum_bitmask<ModelObjectCutAttribute>;
ENABLE_ENUM_BITMASK_OPERATORS(ModelObjectCutAttribute);


class Cut {

    struct SavedVolumePainting
    {
        ObjectID source_volume_id;
        std::optional<TriangleSelector::SavedPainting> painting;
    };

    Model                       m_model;
    int                         m_instance;
    const Transform3d           m_cut_matrix;
    ModelObjectCutAttributes    m_attributes;
    int                         m_new_face_filament{ 0 };

    void post_process(ModelObject* object, ModelObjectPtrs& objects, bool keep, bool place_on_cut, bool flip);
    void post_process(ModelObject* upper_object, ModelObject* lower_object, ModelObjectPtrs& objects);
    void finalize(const ModelObjectPtrs& objects, const std::vector<SavedVolumePainting>& saved_paintings);

public:

    Cut(const ModelObject* object, int instance, const Transform3d& cut_matrix, 
        ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper |
                                              ModelObjectCutAttribute::KeepLower |
                                              ModelObjectCutAttribute::KeepAsParts );
    ~Cut() { m_model.clear_objects(); }

    void set_new_face_filament(int filament) { m_new_face_filament = filament > 0 ? filament : 0; }

    struct Groove
    {
        float depth{ 0.f };
        float width{ 0.f };
        float flaps_angle{ 0.f };
        float angle{ 0.f };
        float depth_init{ 0.f };
        float width_init{ 0.f };
        float flaps_angle_init{ 0.f };
        float angle_init{ 0.f };
        float depth_tolerance{ 0.1f };
        float width_tolerance{ 0.1f };
    };

    struct Part
    {
        bool selected;
        bool is_modifier;
    };

    const ModelObjectPtrs& perform_with_plane();
    const ModelObjectPtrs& perform_by_contour(const ModelObject* src_object, std::vector<Part> parts, int dowels_count);
    const ModelObjectPtrs& perform_with_groove(const Groove& groove, const Transform3d& rotation_m, bool keep_as_parts = false);

}; // namespace Cut

} // namespace Slic3r

#endif /* slic3r_CutUtils_hpp_ */
