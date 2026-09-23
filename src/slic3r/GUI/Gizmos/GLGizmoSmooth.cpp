#include "GLGizmoSmooth.hpp"

#include <GL/glew.h>
#include <cmath>
#include <limits>
#include <boost/log/trivial.hpp>

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"

namespace Slic3r::GUI {

GLGizmoSmooth::GLGizmoSmooth(GLCanvas3D& parent, const std::string& icon, unsigned int sprite_id) : GLGizmoBase(parent, icon, sprite_id) {}

std::string GLGizmoSmooth::on_get_name() const { return _u8L("Smooth brush"); }

ModelVolume* GLGizmoSmooth::selected_volume() const
{
    const Selection& selection = m_parent.get_selection();
    if (selection.get_volume_idxs().empty() || (selection.get_volume_idxs().size() != 1 && !selection.is_single_full_instance()))
        return nullptr;
    const auto& objects = wxGetApp().model().objects;
    for (unsigned idx : selection.get_volume_idxs()) {
        const GLVolume* gl_volume = selection.get_volume(idx);
        const int       object = gl_volume->object_idx(), volume = gl_volume->volume_idx();
        if (object < 0 || size_t(object) >= objects.size() || volume < 0 || size_t(volume) >= objects[object]->volumes.size())
            continue;
        ModelVolume* result = objects[object]->volumes[volume];
        if (result->is_model_part() && !result->mesh().its.indices.empty())
            return result;
    }
    return nullptr;
}

ModelVolume* GLGizmoSmooth::pick_surface(const Vec2d& mouse, Transform3d& world, Vec3f& hit, size_t& facet)
{
    if (!selected_volume())
        return nullptr;
    const Selection& selection = m_parent.get_selection();
    const auto&      objects   = wxGetApp().model().objects;
    // GLCanvas3D::get_camera() is the saved view for switching tabs, not the
    // camera rendering this viewport. Match scene picking and paint brushes.
    const Camera& camera  = wxGetApp().plater()->get_camera();
    double        nearest = std::numeric_limits<double>::max();
    ModelVolume*  result  = nullptr;
    for (unsigned idx : selection.get_volume_idxs()) {
        const GLVolume* gl_volume = selection.get_volume(idx);
        const int       object = gl_volume->object_idx(), volume_idx = gl_volume->volume_idx();
        if (object < 0 || size_t(object) >= objects.size() || volume_idx < 0 || size_t(volume_idx) >= objects[object]->volumes.size())
            continue;
        ModelVolume* volume = objects[object]->volumes[volume_idx];
        if (!volume->is_model_part() || volume->mesh().its.indices.empty())
            continue;
        // Use the editable mesh directly: this also works when the display
        // volume has no picking cache, and guarantees matching facet IDs.
        Surface& surface = m_surfaces[volume];
        if (surface.mesh != volume->mesh_ptr()) {
            surface.mesh      = volume->mesh_ptr();
            surface.raycaster = std::make_unique<MeshRaycaster>(surface.mesh);
        }
        const Transform3d transform = gl_volume->world_matrix();
        Vec3f             point, normal;
        size_t            face;
        if (!surface.raycaster->closest_hit(mouse, transform, camera, point, normal, nullptr, &face))
            continue;
        const Vec3d  position = transform * point.cast<double>();
        const double distance = (position - camera.get_position()).squaredNorm();
        if (position.z() >= SINKING_Z_THRESHOLD && distance < nearest) {
            nearest = distance;
            result  = volume;
            world   = transform;
            hit     = point;
            facet   = face;
        }
    }
    return result;
}

bool GLGizmoSmooth::on_is_activable() const
{
    const ModelVolume* volume = selected_volume();
    return volume && !volume->mesh().its.indices.empty();
}

void GLGizmoSmooth::finish_stroke()
{
    const auto& objects    = wxGetApp().model().objects;
    const auto  object     = std::find(objects.begin(), objects.end(), m_stroke_object);
    const int   object_idx = object == objects.end() ? -1 : int(object - objects.begin());
    const bool  changed    = m_snapshot_taken;
    if (m_gesture) {
        BOOST_LOG_TRIVIAL(warning) << "[SmoothBrush] stroke_end hits=" << m_hits << " misses=" << m_misses
                                   << " changed_dabs=" << m_changed_dabs << " unchanged_dabs=" << m_unchanged_dabs
                                   << " snapshot_taken=" << changed;
        m_gesture = false;
    }
    m_stroke = m_snapshot_taken = false;
    m_stroke_object             = nullptr;
    m_stroke_volume             = nullptr;
    m_brush.reset();
    if (changed && object_idx >= 0)
        wxGetApp().plater()->changed_mesh(object_idx);
}

void GLGizmoSmooth::on_set_state()
{
    BOOST_LOG_TRIVIAL(warning) << "[SmoothBrush] diagnostic_build=3 state=" << (m_state == On ? "on" : "off")
                               << " selected_volumes=" << m_parent.get_selection().get_volume_idxs().size();
    if (m_state == Off) {
        finish_stroke();
        m_surfaces.clear();
        m_cursor.reset();
    }
    m_parent.set_as_dirty();
}

void GLGizmoSmooth::data_changed(bool is_serializing)
{
    m_surfaces.clear();
    if (is_serializing) {
        BOOST_LOG_TRIVIAL(warning) << "[SmoothBrush] model_restored stroke_active=" << m_stroke;
        m_gesture  = false;
        m_feedback = Feedback::Restored;
        // Undo/redo has already restored the model; discard transient stroke data.
        m_stroke = m_snapshot_taken = false;
        m_stroke_object             = nullptr;
        m_stroke_volume             = nullptr;
        m_brush.reset();
    } else if (m_stroke && !selected_volume()) {
        finish_stroke();
    }
}

bool GLGizmoSmooth::on_mouse(const wxMouseEvent& event)
{
    if (event.Moving() || event.Dragging())
        m_parent.set_as_dirty();
    // Own the complete left-button gesture, including misses. Letting a miss
    // fall through starts GLCanvas3D's object-move operation instead.
    if (event.LeftUp()) {
        finish_stroke();
        return true;
    }
    if (m_stroke && !event.LeftIsDown())
        finish_stroke();
    // Match the painting tools' explicit camera-navigation modifier.
    if (event.CmdDown()) {
        finish_stroke();
        return false;
    }
    if (!event.LeftDown() && !event.LeftDClick() && !(event.Dragging() && event.LeftIsDown()))
        return false;
    if (!m_gesture) {
        m_gesture = true;
        m_hits = m_misses = m_changed_dabs = m_unchanged_dabs = 0;
        BOOST_LOG_TRIVIAL(warning) << "[SmoothBrush] stroke_begin selected_volumes=" << m_parent.get_selection().get_volume_idxs().size()
                                   << " valid_selection=" << (selected_volume() != nullptr) << " radius_mm=" << m_radius
                                   << " strength=" << m_strength;
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_stroke && now - m_last_dab < std::chrono::milliseconds(40))
        return true;

    Transform3d world;
    Vec3f       hit;
    size_t      facet = 0;
    // unproject_on_mesh assumes a closed solid (an even number of ray hits).
    // AI-generated meshes often have holes or non-manifold edges. Pick their
    // nearest surface without that solid-inside test, just as scene picking does.
    ModelVolume* volume = pick_surface(m_parent.get_local_mouse_position(), world, hit, facet);
    if (!volume) {
        m_feedback = Feedback::Miss;
        if (++m_misses == 1)
            BOOST_LOG_TRIVIAL(warning) << "[SmoothBrush] no_surface_hit valid_selection=" << (selected_volume() != nullptr);
        return true;
    }
    ++m_hits;
    if (m_stroke && volume != m_stroke_volume)
        return true; // Keep one part and one Undo snapshot per stroke.
    if (!m_stroke) {
        m_stroke        = true;
        m_stroke_object = volume->get_object();
        m_stroke_volume = volume;
        m_brush         = std::make_unique<MeshSmoothingBrush>(volume->mesh().its);
    }
    m_last_dab                = now;
    indexed_triangle_set mesh = volume->mesh().its;
    m_last_dab_changed        = m_brush->apply(mesh, facet, hit, world, m_radius, m_strength);
    m_statistics              = m_brush->last_statistics();
    m_feedback                = m_last_dab_changed ? Feedback::Changed : Feedback::Unchanged;
    size_t& count             = m_last_dab_changed ? m_changed_dabs : m_unchanged_dabs;
    if (++count == 1)
        BOOST_LOG_TRIVIAL(warning) << "[SmoothBrush] dab changed=" << m_last_dab_changed << " vertices=" << mesh.vertices.size()
                                   << " faces=" << mesh.indices.size() << " facet=" << facet
                                   << " inside_radius=" << m_statistics.inside_radius << " pinned=" << m_statistics.pinned
                                   << " movable=" << m_statistics.movable << " moved=" << m_statistics.moved
                                   << " rejected_faces=" << m_statistics.rejected_faces
                                   << " max_displacement_mm=" << m_statistics.max_displacement_mm;
    if (!m_last_dab_changed)
        return true;
    if (!m_snapshot_taken) {
        wxGetApp().plater()->take_snapshot(_u8L("Smooth brush stroke"));
        m_snapshot_taken = true;
        BOOST_LOG_TRIVIAL(warning) << "[SmoothBrush] stroke_snapshot_requested";
    }
    // Only coordinates change. In particular, do NOT call clear_before_change_mesh:
    // the original annotation bitstreams must survive, including split facets.
    volume->set_mesh(std::move(mesh));
    volume->calculate_convex_hull();
    volume->invalidate_convex_hull_2d();
    volume->set_new_unique_id();
    volume->get_object()->invalidate_bounding_box();
    m_parent.reload_scene(false);
    m_surfaces.erase(volume);
    m_parent.set_as_dirty();
    return true;
}

void GLGizmoSmooth::on_render_input_window(float x, float y, float bottom_limit)
{
    y = std::max(0.f, std::min(y, bottom_limit - m_imgui->scaled(12.5f)));
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always);
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    m_imgui->text(_L("Drag the left mouse button over bumps to smooth them."));
    m_imgui->text(_L("Colour painting is retained. Each stroke can be undone."));
    if (m_feedback == Feedback::Miss)
        m_imgui->text(_L("The brush missed the selected surface. Move the green ring onto the model."));
    else if (m_feedback == Feedback::Unchanged)
        m_imgui->text(_L("No vertices moved. Try a larger brush or a different area."));
    else if (m_feedback == Feedback::Changed)
        ImGui::Text(_u8L("Last dab: %zu vertices moved, up to %.5f mm.").c_str(), m_statistics.moved, m_statistics.max_displacement_mm);
    else if (m_feedback == Feedback::Restored)
        m_imgui->text(_L("Undo/Redo restored the model."));
    ImGui::SliderFloat(_u8L("Brush radius (mm)").c_str(), &m_radius, 0.2f, 30.f, "%.1f");
    ImGui::SliderFloat(_u8L("Strength").c_str(), &m_strength, 0.01f, 1.f, "%.2f");
    m_imgui->disabled_begin(m_stroke || !wxGetApp().plater()->can_undo());
    if (m_imgui->button(_L("Undo")))
        wxGetApp().CallAfter([] { wxGetApp().plater()->undo(); });
    m_imgui->disabled_end();
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoSmooth::on_render()
{
    const Vec2d mouse = m_parent.get_local_mouse_position();
    const Size  size  = m_parent.get_canvas_size();
    if (size.get_width() <= 0 || size.get_height() <= 0 || mouse.x() < 0 || mouse.y() < 0 || mouse.x() >= size.get_width() ||
        mouse.y() >= size.get_height())
        return;
    Transform3d   world;
    Vec3f         hit;
    size_t        facet;
    const bool    on_surface = pick_surface(mouse, world, hit, facet) != nullptr;
    const Camera& camera     = wxGetApp().plater()->get_camera();
    float         radius     = std::max(5.f, m_radius * float(camera.get_zoom()));
    if (on_surface) {
        const Vec3d center = world * hit.cast<double>();
        const Point edge   = CameraUtils::project(camera, center + m_radius * camera.get_dir_right());
        radius             = std::max(5.f, float((edge.cast<double>() - mouse).norm()));
    }
    // Render in the viewport, using the same shader path as the paint brushes.
    // A grey ring also makes the active tool visible between surface hits.
    GLModel::Geometry geometry;
    geometry.format = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P2};
    geometry.color  = on_surface ? ColorRGBA(0.15f, 1.f, 0.55f, 1.f) : ColorRGBA(0.65f, 0.65f, 0.65f, 1.f);
    auto add_point  = [&](double x, double y) {
        geometry.add_vertex(Vec2f(float(2.0 * x / size.get_width() - 1.0), float(1.0 - 2.0 * y / size.get_height())));
    };
    for (unsigned i = 0; i < 64; ++i) {
        const double a = 2.0 * PI * i / 64.0, b = 2.0 * PI * (i + 1) / 64.0;
        add_point(mouse.x() + radius * std::cos(a), mouse.y() + radius * std::sin(a));
        add_point(mouse.x() + radius * std::cos(b), mouse.y() + radius * std::sin(b));
        geometry.add_index(2 * i);
        geometry.add_index(2 * i + 1);
    }
    add_point(mouse.x() - 4, mouse.y());
    add_point(mouse.x() + 4, mouse.y());
    add_point(mouse.x(), mouse.y() - 4);
    add_point(mouse.x(), mouse.y() + 4);
    for (unsigned i = 128; i < 132; ++i)
        geometry.add_index(i);
    m_cursor.reset();
    m_cursor.init_from(std::move(geometry));
    if (GLShaderProgram* shader = wxGetApp().get_shader("flat")) {
        glsafe(::glPushAttrib(GL_ENABLE_BIT | GL_LINE_BIT));
        glsafe(::glDisable(GL_DEPTH_TEST));
        glsafe(::glLineWidth(2.f));
        shader->start_using();
        shader->set_uniform("view_model_matrix", Transform3d::Identity());
        shader->set_uniform("projection_matrix", Transform3d::Identity());
        m_cursor.render();
        shader->stop_using();
        glsafe(::glPopAttrib());
    }
}

} // namespace Slic3r::GUI
