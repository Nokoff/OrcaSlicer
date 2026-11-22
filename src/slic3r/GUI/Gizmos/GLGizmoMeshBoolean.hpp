#ifndef slic3r_GLGizmoMeshBoolean_hpp_
#define slic3r_GLGizmoMeshBoolean_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmosCommon.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/Model.hpp"

#include <atomic>
#include <limits>
#include <mutex>

namespace Slic3r {

namespace GUI {

enum class MeshBooleanSelectingState {
    Undef,
    SelectSource,
    SelectTool,

};
enum class MeshBooleanOperation{
    Undef,
    Union,
    Difference,
    Intersection,
};
struct VolumeInfo {
    ModelVolume* mv{ nullptr };
    int volume_idx{-1};
    Transform3d trafo;
    void reset() {
        mv = nullptr;
        volume_idx = -1;
        trafo = Transform3d::Identity();
    };
    template<class Archive>
    void serialize(Archive& ar) {
        ar(volume_idx, trafo);
    }
};
class GLGizmoMeshBoolean : public GLGizmoBase
{
public:
    GLGizmoMeshBoolean(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    ~GLGizmoMeshBoolean();

    void set_enable(bool enable) { m_enable = enable; }
    bool get_enable() { return m_enable; }
    MeshBooleanSelectingState get_selecting_state() { return m_selecting_state; }
    void set_src_volume(ModelVolume* mv) { 
        clear_preview();
        m_src.mv = mv;
        if (m_src.mv == m_tool.mv)
            m_tool.reset();
        m_cutter_preview_model.reset();
        initialize_cutter_position();
        update_cutter_preview();
    }
    void set_tool_volume(ModelVolume* mv) { 
        clear_preview();
        m_tool.mv = mv;
        if (m_tool.mv == m_src.mv)
            m_src.reset();
    }

    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down);

    /// <summary>
    /// Implement when want to process mouse events in gizmo
    /// Click, Right click, move, drag, ...
    /// </summary>
    /// <param name="mouse_event">Keep information about mouse click</param>
    /// <returns>Return True when use the information and don't want to
    /// propagate it otherwise False.</returns>
    bool on_mouse(const wxMouseEvent &mouse_event) override;

protected:
    virtual bool on_init() override;
    virtual std::string on_get_name() const override;
    virtual bool on_is_activable() const override;
    virtual void on_render() override;
    virtual void on_set_state() override;
    virtual CommonGizmosDataID on_get_requirements() const override;
    virtual void on_render_input_window(float x, float y, float bottom_limit);

    void on_load(cereal::BinaryInputArchive &ar) override;
    void on_save(cereal::BinaryOutputArchive &ar) const override;

private:
    enum class CutterKind { Mesh, Box, Cylinder, Sphere, Brush };

    struct PreviewSurface
    {
        std::unique_ptr<GLModel> model;
        ColorRGBA color;
    };

    struct HiddenPreviewVolume
    {
        int object_idx = -1;
        int volume_idx = -1;
        int instance_idx = -1;
        ObjectID volume_id;
        bool was_active = true;
    };

    struct PreviewFacetData
    {
        indexed_triangle_set facets;
        int filament = 1;
    };

    struct PreviewAnnotations
    {
        TriangleSelector::TriangleSplittingData supported;
        TriangleSelector::TriangleSplittingData seam;
        TriangleSelector::TriangleSplittingData mmu;
        TriangleSelector::TriangleSplittingData fuzzy;
    };

    struct Preview
    {
        MeshBooleanOperation operation = MeshBooleanOperation::Undef;
        ObjectID object_id;
        ObjectID source_id;
        ObjectID tool_id;
        uint64_t cutter_revision = 0;
        bool primitive_cutter = false;
        const TriangleMesh* source_mesh = nullptr;
        const TriangleMesh* tool_mesh = nullptr;
        ObjectBase::Timestamp source_config_timestamp = 0;
        ObjectBase::Timestamp source_mmu_timestamp = 0;
        ObjectBase::Timestamp source_support_timestamp = 0;
        ObjectBase::Timestamp source_seam_timestamp = 0;
        ObjectBase::Timestamp source_fuzzy_timestamp = 0;
        Transform3d source_transform = Transform3d::Identity();
        Transform3d tool_transform = Transform3d::Identity();
        Transform3d cutter_instance_transform = Transform3d::Identity();
        int cutter_instance_idx = -1;
        Vec3d result_center = Vec3d::Zero();
        bool delete_input = false;
        bool empty = false;
        TriangleMesh mesh;
        std::vector<uint8_t> source_faces;
        PreviewAnnotations annotations;
    };

    struct PreviewWork
    {
        std::atomic<bool> cancelled{ false };
        std::mutex mutex;
        bool done = false;
        bool success = false;
        bool ambiguous_source_provenance = false;
        Preview snapshot;
        TriangleMesh mesh;
        std::vector<uint8_t> source_faces;
        std::vector<PreviewFacetData> surfaces;
        std::optional<TriangleSelector::SavedPainting> saved_painting;
        std::shared_ptr<const TriangleMesh> source_paint_mesh;
        PreviewAnnotations annotations;
        int source_filament = 1;
        int new_face_filament = 0;
    };

    bool m_enable{ false };
    MeshBooleanOperation m_operation_mode;
    MeshBooleanSelectingState m_selecting_state;
    bool m_diff_delete_input = false;
    bool m_inter_delete_input = false;
    int m_new_face_filament = 0; // 0 inherits the target volume's base filament.
    CutterKind m_cutter_kind = CutterKind::Brush;
    Vec3d m_cutter_size = Vec3d(10.0, 10.0, 10.0);
    Vec3d m_cutter_position = Vec3d::Zero();
    Vec3d m_cutter_rotation = Vec3d::Zero();
    bool m_cutter_position_initialized = false;
    bool m_snap_cutter_position = false;
    double m_brush_radius = 3.0;
    bool m_brush_stroke_active = false;
    std::vector<Vec3d> m_brush_stroke_points;
    bool m_section_view_enabled = false;
    bool m_section_clipper_owned = false;
    uint64_t m_cutter_revision = 0;
    VolumeInfo m_src;
    VolumeInfo m_tool;
    std::optional<Preview> m_preview;
    std::shared_ptr<PreviewWork> m_preview_work;
    std::vector<PreviewSurface> m_preview_surfaces;
    std::unique_ptr<GLModel> m_cutter_preview_model;
    GLModel m_brush_cursor_circle;
    Vec2d m_brush_cursor_center = Vec2d::Constant(std::numeric_limits<double>::lowest());
    double m_brush_cursor_screen_radius = -1.0;
    Transform3d m_cutter_preview_instance_transform = Transform3d::Identity();
    int m_cutter_preview_instance_idx = -1;
    std::vector<HiddenPreviewVolume> m_preview_hidden_volumes;

    void generate_new_volume(bool delete_input, const TriangleMesh& mesh_result, PreviewAnnotations&& annotations);
    void initialize_cutter_position();
    TriangleMesh make_primitive_cutter_mesh() const;
    TriangleMesh make_brush_cutter_mesh() const;
    void update_cutter_preview();
    void render_cutter_preview();
    void render_brush_cursor();
    bool create_preview(const std::string& operation, bool delete_input);
    void finish_preview(Preview preview, TriangleMesh mesh, std::vector<uint8_t> source_faces,
                        std::vector<PreviewFacetData> surfaces);
    void poll_preview_work();
    void cancel_preview_work();
    bool preview_is_current() const;
    void apply_preview();
    void clear_preview();
    void hide_preview_inputs();
    void restore_preview_inputs();
    void render_preview();
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoMeshBoolean_hpp_
