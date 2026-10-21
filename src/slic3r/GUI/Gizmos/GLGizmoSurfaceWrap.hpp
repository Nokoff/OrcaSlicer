#ifndef slic3r_GLGizmoSurfaceWrap_hpp_
#define slic3r_GLGizmoSurfaceWrap_hpp_

#include "GLGizmoBase.hpp"
#include "libslic3r/SurfaceWrap.hpp"
#include "slic3r/GUI/SurfaceDrag.hpp"
#include <map>

namespace Slic3r::GUI {

class GLGizmoSurfaceWrap : public GLGizmoBase
{
public:
    GLGizmoSurfaceWrap(GLCanvas3D& parent, const std::string& icon, unsigned int sprite_id);
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
    ModelVolume*                        selected_volume() const;
    void                                sync_source();
    void                                finish_drag();
    void                                apply();
    std::array<Vec3d, 4>                corner_positions() const;
    std::optional<Vec2d>                mouse_on_corner_plane(const Vec2d& mouse, double z) const;
    void                                finish_corner_drag();
    SurfaceWrapOptions                  m_options;
    indexed_triangle_set                m_source;
    std::shared_ptr<const TriangleMesh> m_expected_mesh;
    ObjectID                            m_volume_id;
    int                                 m_target  = -1;
    bool                                m_wrapped = false;
    std::string                         m_message;
    std::optional<SurfaceDrag>          m_surface_drag;
    RaycastManager                      m_raycast_manager;
    std::array<double, 4>               m_corner_z{0., 0., 0., 0.};
    int                                 m_corner_drag         = -1;
    Vec2d                               m_corner_start        = Vec2d::Zero();
    Vec2d                               m_corner_offset_start = Vec2d::Zero();
    GLModel                             m_corner_handles;
    struct EditState
    {
        SurfaceWrapOptions    options;
        std::array<double, 4> corner_z;
        int                   target;
        bool                  wrapped;
    };
    std::map<size_t, EditState> m_history;
};

} // namespace Slic3r::GUI
#endif
