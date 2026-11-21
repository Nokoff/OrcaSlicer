#ifndef slic3r_GLGizmoSmooth_hpp_
#define slic3r_GLGizmoSmooth_hpp_

#include "GLGizmoBase.hpp"
#include "libslic3r/MeshSmoothing.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <vector>

namespace Slic3r::GUI {

class GLGizmoSmooth : public GLGizmoBase
{
public:
    GLGizmoSmooth(GLCanvas3D& parent, const std::string& icon, unsigned int sprite_id);
    bool on_mouse(const wxMouseEvent& event) override;
    void data_changed(bool is_serializing) override;

protected:
    bool        on_init() override { return true; }
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    void        on_set_state() override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;

private:
    ModelVolume*                          selected_volume() const;
    ModelVolume*                          pick_surface(const Vec2d& mouse, Transform3d& world, Vec3f& hit, size_t& facet);
    void                                  finish_stroke();
    void                                  poll_brush_preparation();
    void                                  discard_brush_preparation();
    void                                  prepare_brush(ModelVolume* volume, const Transform3d& world);
    bool                                  adopt_prepared_brush(ModelVolume* volume, const Transform3d& world);
    bool                                  apply_dab(size_t facet, const Vec3f& hit, const Transform3d& world);
    void                                  process_pending_dab();
    float                                 m_radius         = 6.f;
    float                                 m_strength       = 0.65f;
    float                                 m_max_displacement_mm = 0.5f;
    bool                                  m_preserve_sharp_edges = false;
    bool                                  m_stroke         = false;
    bool                                  m_snapshot_taken = false;
    ModelObject*                          m_stroke_object  = nullptr;
    ModelVolume*                          m_stroke_volume  = nullptr;
    std::vector<Vec3f>                    m_stroke_origin_vertices;
    std::unique_ptr<MeshSmoothingBrush>   m_brush;
    struct BrushPreparation
    {
        std::shared_ptr<const TriangleMesh> mesh;
        Transform3d                         world = Transform3d::Identity();
        bool                                preserve_sharp_edges = false;
        std::unique_ptr<MeshSmoothingBrush> brush;
        std::mutex                          mutex;
        std::atomic<bool>                   discard{false};
        std::atomic<bool>                   ready{false};
        bool                                failed = false;
    };
    std::future<void>                     m_brush_future;
    std::shared_ptr<BrushPreparation>     m_preparation;
    std::shared_ptr<const TriangleMesh>   m_preparing_mesh;
    Transform3d                          m_preparing_world = Transform3d::Identity();
    bool                                 m_preparing_preserve_sharp_edges = false;
    std::unique_ptr<MeshSmoothingBrush>   m_prepared_brush;
    bool                                 m_preparation_failed = false;
    struct PendingDab { size_t facet; Vec3f hit; Transform3d world; };
    std::deque<PendingDab>               m_pending_dabs;
    bool                                 m_finish_when_ready = false;
    std::chrono::steady_clock::time_point m_last_dab;
    struct Surface
    {
        std::shared_ptr<const TriangleMesh> mesh;
        std::unique_ptr<MeshRaycaster>      raycaster;
    };
    std::map<ModelVolume*, Surface> m_surfaces;
    GLModel                         m_cursor;
    bool                            m_last_dab_changed = false;
    MeshSmoothingBrush::Statistics  m_statistics;
    enum class Feedback { Ready, Preparing, PreparationFailed, Miss, Unchanged, Changed, Restored };
    Feedback m_feedback = Feedback::Ready;
};

} // namespace Slic3r::GUI
#endif
