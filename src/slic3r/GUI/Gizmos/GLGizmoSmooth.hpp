#ifndef slic3r_GLGizmoSmooth_hpp_
#define slic3r_GLGizmoSmooth_hpp_

#include "GLGizmoBase.hpp"
#include "libslic3r/MeshSmoothing.hpp"

#include <chrono>
#include <map>

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
    float                                 m_radius         = 6.f;
    float                                 m_strength       = 0.65f;
    bool                                  m_stroke         = false;
    bool                                  m_snapshot_taken = false;
    ModelObject*                          m_stroke_object  = nullptr;
    ModelVolume*                          m_stroke_volume  = nullptr;
    std::unique_ptr<MeshSmoothingBrush>   m_brush;
    std::chrono::steady_clock::time_point m_last_dab;
    struct Surface
    {
        std::shared_ptr<const TriangleMesh> mesh;
        std::unique_ptr<MeshRaycaster>      raycaster;
    };
    std::map<ModelVolume*, Surface> m_surfaces;
    GLModel                         m_cursor;
    bool                            m_last_dab_changed = false;
    bool                            m_gesture          = false;
    size_t                          m_hits             = 0;
    size_t                          m_misses           = 0;
    size_t                          m_changed_dabs     = 0;
    size_t                          m_unchanged_dabs   = 0;
    MeshSmoothingBrush::Statistics  m_statistics;
    enum class Feedback { Ready, Miss, Unchanged, Changed, Restored };
    Feedback m_feedback = Feedback::Ready;
};

} // namespace Slic3r::GUI
#endif
