#include "GLGizmoSurfaceWrap.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "libslic3r/AABBMesh.hpp"

#include <GL/glew.h>
#include <cmath>
#include <stdexcept>

namespace Slic3r::GUI {

GLGizmoSurfaceWrap::GLGizmoSurfaceWrap(GLCanvas3D& parent, const std::string& icon, unsigned int sprite_id)
    : GLGizmoBase(parent, icon, sprite_id)
{}

std::string GLGizmoSurfaceWrap::on_get_name() const { return _u8L("Wrap onto surface"); }

ModelVolume* GLGizmoSurfaceWrap::selected_volume() const
{
    const Selection& selection = m_parent.get_selection();
    if (selection.get_volume_idxs().size() != 1)
        return nullptr;
    const GLVolume* gl_volume = selection.get_first_volume();
    const auto&     objects   = wxGetApp().model().objects;
    const int       object = gl_volume->object_idx(), part = gl_volume->volume_idx();
    if (object < 0 || size_t(object) >= objects.size() || part < 0 || size_t(part) >= objects[object]->volumes.size())
        return nullptr;
    ModelVolume* volume = objects[object]->volumes[part];
    return volume->is_model_part() && !volume->mesh().its.indices.empty() ? volume : nullptr;
}

bool GLGizmoSurfaceWrap::on_is_activable() const
{
    const ModelVolume* volume = selected_volume();
    return volume && !volume->emboss_shape && !volume->is_the_only_one_part();
}

void GLGizmoSurfaceWrap::finish_drag()
{
    if (m_surface_drag) {
        // Complete the shared surface-drag lifecycle before releasing its GLVolume pointer.
        wxMouseEvent release(wxEVT_LEFT_UP);
        on_mouse_surface_drag(release, wxGetApp().plater()->get_camera(), m_surface_drag, m_parent, m_raycast_manager, UP_LIMIT);
    }
}

void GLGizmoSurfaceWrap::on_set_state()
{
    if (m_state == Off) {
        finish_corner_drag();
        finish_drag();
        m_expected_mesh.reset();
        m_source = {};
        m_history.clear();
        m_wrapped = false;
    } else
        sync_source();
}

void GLGizmoSurfaceWrap::sync_source()
{
    ModelVolume* volume = selected_volume();
    if (!volume) {
        m_expected_mesh.reset();
        m_source = {};
        m_history.clear();
        m_wrapped = false;
        return;
    }
    if (m_expected_mesh == volume->mesh_ptr() && m_volume_id == volume->id())
        return;
    m_expected_mesh     = volume->mesh_ptr();
    m_volume_id         = volume->id();
    const auto restored = m_history.find(m_volume_id.id);
    if (restored != m_history.end()) {
        m_options  = restored->second.options;
        m_corner_z = restored->second.corner_z;
        m_target   = restored->second.target;
        m_wrapped  = restored->second.wrapped;
        m_message  = _u8L("Undo/Redo restored the shape and corner settings.");
        return;
    }
    m_history.clear();
    m_source = volume->mesh().its;
    m_corner_z.fill(volume->mesh().bounding_box().max.z());
    m_options = {};
    m_wrapped = false;
    m_message.clear();
    m_target            = -1;
    const auto& volumes = volume->get_object()->volumes;
    for (size_t i = 0; i < volumes.size(); ++i)
        if (volumes[i] != volume && volumes[i]->is_model_part()) {
            m_target = int(i);
            break;
        }
    m_history.emplace(m_volume_id.id, EditState{m_options, m_corner_z, m_target, m_wrapped});
}

void GLGizmoSurfaceWrap::data_changed(bool is_serializing)
{
    finish_corner_drag();
    // Scene reloads invalidate the GLVolume pointer held by SurfaceDrag.
    if (m_surface_drag) {
        m_surface_drag.reset();
        m_parent.enable_moving(true);
        m_parent.enable_picking(true);
    }
    sync_source();
}

bool GLGizmoSurfaceWrap::on_mouse(const wxMouseEvent& event)
{
    if (!on_is_activable())
        return false;
    sync_source();
    const bool  was_dragging = m_surface_drag.has_value();
    const Vec2d mouse(event.GetX(), event.GetY());
    if (m_corner_drag >= 0) {
        if (event.Dragging()) {
            if (auto point = mouse_on_corner_plane(mouse, m_corner_z[m_corner_drag])) {
                m_options.corners[m_corner_drag] = m_corner_offset_start + *point - m_corner_start;
                m_parent.set_as_dirty();
            }
        } else {
            finish_corner_drag();
            if (m_wrapped)
                apply();
        }
        return true;
    }
    if (!m_surface_drag && event.LeftDown()) {
        const auto  corners = corner_positions();
        const auto& camera  = wxGetApp().plater()->get_camera();
        const auto  world   = m_parent.get_selection().get_first_volume()->world_matrix();
        for (int i = 0; i < 4; ++i) {
            if (!corners[i].allFinite())
                continue;
            const Vec2d screen = CameraUtils::project(camera, world * corners[i]).cast<double>();
            if ((screen - mouse).norm() > 10 * m_parent.get_scale())
                continue;
            if (auto point = mouse_on_corner_plane(mouse, m_corner_z[i])) {
                m_corner_drag         = i;
                m_corner_start        = *point;
                m_corner_offset_start = m_options.corners[i];
                m_parent.enable_moving(false);
                m_parent.enable_picking(false);
                return true;
            }
        }
    }
    const ModelVolume* volume = selected_volume();
    if (m_target < 0 || size_t(m_target) >= volume->get_object()->volumes.size())
        return false;
    const RaycastManager::AllowVolumes allowed({volume->get_object()->volumes[m_target]->id().id});
    const bool used = on_mouse_surface_drag(event, wxGetApp().plater()->get_camera(), m_surface_drag, m_parent, m_raycast_manager, UP_LIMIT,
                                            &allowed);
    if (was_dragging && !m_surface_drag && m_wrapped)
        apply();
    return used;
}

void GLGizmoSurfaceWrap::apply()
{
    sync_source();
    ModelVolume* volume = selected_volume();
    if (!volume || m_target < 0 || size_t(m_target) >= volume->get_object()->volumes.size())
        return;
    const ModelVolume* target = volume->get_object()->volumes[m_target];
    if (target == volume || !target->is_model_part())
        return;
    if (!volume->supported_facets.empty() || !volume->seam_facets.empty() || !volume->mmu_segmentation_facets.empty() ||
        !volume->fuzzy_skin_facets.empty()) {
        m_message = _u8L("Remove painting from the shape before wrapping. Target painting can be retained.");
        return;
    }
    try {
        const Transform3d source_tr = volume->get_matrix();
        if (std::abs(source_tr.linear().determinant()) < 1e-12)
            throw std::runtime_error("The selected part has a zero scale.");
        indexed_triangle_set surface   = target->mesh().its;
        const Transform3d    to_source = source_tr.inverse() * target->get_matrix();
        for (auto& vertex : surface.vertices)
            vertex = (to_source * vertex.cast<double>()).cast<float>();
        auto           result = surface_wrap(m_source, surface, m_options);
        const AABBMesh target_surface(surface);
        double         top = surface.vertices.front().z();
        for (const auto& vertex : surface.vertices)
            top = std::max(top, double(vertex.z()));
        Vec3d lo = m_source.vertices.front().cast<double>(), hi = lo;
        for (const auto& vertex : m_source.vertices) {
            lo = lo.cwiseMin(vertex.cast<double>());
            hi = hi.cwiseMax(vertex.cast<double>());
        }
        const auto corners = corner_positions();
        for (size_t i = 0; i < 4; ++i) {
            const auto hit = target_surface.query_ray_hit(Vec3d(corners[i].x(), corners[i].y(), top + 1.), -Vec3d::UnitZ());
            if (hit.is_hit())
                m_corner_z[i] = hit.position().z() + hi.z() - lo.z() + m_options.offset;
        }
        auto      plater     = wxGetApp().plater();
        const int object_idx = m_parent.get_selection().get_object_idx();
        plater->take_snapshot(_u8L("Wrap onto surface"));
        volume->set_mesh(std::move(result));
        volume->calculate_convex_hull();
        volume->invalidate_convex_hull_2d();
        volume->set_new_unique_id();
        volume->get_object()->invalidate_bounding_box();
        // Keep the original source throughout this editing session. Repeated
        // corner edits and surface moves must never warp an already warped mesh.
        m_expected_mesh = volume->mesh_ptr();
        m_volume_id     = volume->id();
        m_wrapped       = true;
        m_history.emplace(m_volume_id.id, EditState{m_options, m_corner_z, m_target, m_wrapped});
        m_message = _u8L("Shape wrapped. Adjust corners and apply again, or drag it along the surface.");
        plater->changed_mesh(object_idx);
        wxGetApp().obj_list()->update_item_error_icon(object_idx, -1);
        m_parent.set_as_dirty();
    } catch (const std::exception& error) {
        m_message = error.what();
    }
}

void GLGizmoSurfaceWrap::on_render_input_window(float x, float y, float bottom_limit)
{
    sync_source();
    y = std::max(0.f, std::min(y, bottom_limit - m_imgui->scaled(24.f)));
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always);
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    m_imgui->text(_L("Select the shape as a part of the same object as the target."));
    m_imgui->text(_L("Drag it onto the target, then click Apply wrap."));
    m_imgui->text(_L("Drag the coloured corner rings to adjust individual corners."));
    m_imgui->text(_L("Local XY is the face; local Z is its thickness."));
    ModelVolume* volume = selected_volume();
    m_imgui->disabled_begin(!volume || bool(m_surface_drag) || m_corner_drag >= 0);
    if (volume) {
        const auto& volumes = volume->get_object()->volumes;
        const char* preview = m_target >= 0 && size_t(m_target) < volumes.size() ? volumes[m_target]->name.c_str() : "";
        if (ImGui::BeginCombo(_u8L("Target surface").c_str(), preview)) {
            for (size_t i = 0; i < volumes.size(); ++i) {
                if (volumes[i] == volume || !volumes[i]->is_model_part())
                    continue;
                ImGui::PushID(int(i));
                if (ImGui::Selectable(volumes[i]->name.c_str(), m_target == int(i)))
                    m_target = int(i);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::PushItemWidth(m_imgui->scaled(15.f));
    ImGui::InputDouble(_u8L("Corner lift (mm)").c_str(), &m_options.corner_lift, 0., 0., "%.2f");
    ImGui::InputDouble(_u8L("Surface offset (mm)").c_str(), &m_options.offset, 0., 0., "%.2f");
    ImGui::InputDouble(_u8L("Mesh spacing (mm)").c_str(), &m_options.max_edge, 0., 0., "%.2f");
    ImGui::Separator();
    m_imgui->text(_L("Individual corner offsets: X / Y (part coordinates, mm)"));
    const std::array<std::string, 4> labels{_u8L("Bottom left (red)"), _u8L("Bottom right (green)"), _u8L("Top right (blue)"),
                                            _u8L("Top left (yellow)")};
    for (size_t i = 0; i < labels.size(); ++i) {
        ImGui::PushID(int(i));
        float offset[2] = {float(m_options.corners[i].x()), float(m_options.corners[i].y())};
        if (ImGui::InputFloat2(labels[i].c_str(), offset, "%.2f"))
            m_options.corners[i] = Vec2d(offset[0], offset[1]);
        ImGui::PopID();
    }
    ImGui::PopItemWidth();
    if (m_imgui->button(_L("Apply wrap")))
        apply();
    ImGui::SameLine();
    if (m_imgui->button(_L("Reset corner offsets"))) {
        m_options.corners = {};
        for (auto& corner : m_options.corners)
            corner.setZero();
        m_options.corner_lift = 0.;
    }
    m_imgui->disabled_end();
    m_imgui->text(_L("Changes can be undone. Closing the tool keeps the resulting mesh."));
    if (!m_message.empty())
        ImGui::TextWrapped("%s", m_message.c_str());
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoSurfaceWrap::finish_corner_drag()
{
    if (m_corner_drag < 0)
        return;
    m_corner_drag = -1;
    m_parent.enable_moving(true);
    m_parent.enable_picking(true);
}

std::array<Vec3d, 4> GLGizmoSurfaceWrap::corner_positions() const
{
    std::array<Vec3d, 4> result{Vec3d::Zero(), Vec3d::Zero(), Vec3d::Zero(), Vec3d::Zero()};
    if (m_source.vertices.empty())
        return result;
    Vec3d lo = m_source.vertices.front().cast<double>(), hi = lo;
    for (const auto& vertex : m_source.vertices) {
        lo = lo.cwiseMin(vertex.cast<double>());
        hi = hi.cwiseMax(vertex.cast<double>());
    }
    for (int i = 0; i < 4; ++i) {
        result[i] = Vec3d(i == 1 || i == 2 ? hi.x() : lo.x(), i >= 2 ? hi.y() : lo.y(), m_corner_z[i]);
        result[i].head<2>() += m_options.corners[i];
        result[i].y() += m_options.corner_lift;
    }
    return result;
}

std::optional<Vec2d> GLGizmoSurfaceWrap::mouse_on_corner_plane(const Vec2d& mouse, double z) const
{
    const GLVolume* volume = m_parent.get_selection().get_first_volume();
    if (!volume || std::abs(volume->world_matrix().linear().determinant()) < 1e-12)
        return {};
    Vec3d point, direction;
    CameraUtils::ray_from_screen_pos(wxGetApp().plater()->get_camera(), mouse, point, direction);
    const Transform3d inverse = volume->world_matrix().inverse();
    point                     = inverse * point;
    direction                 = inverse.linear() * direction;
    if (std::abs(direction.z()) < 1e-8)
        return {};
    const Vec3d hit = point + direction * ((z - point.z()) / direction.z());
    return hit.head<2>().eval();
}

void GLGizmoSurfaceWrap::on_render()
{
    if (!selected_volume() || m_source.vertices.empty())
        return;
    const Size size = m_parent.get_canvas_size();
    if (size.get_width() <= 0 || size.get_height() <= 0)
        return;
    const auto&                    camera  = wxGetApp().plater()->get_camera();
    const auto                     world   = m_parent.get_selection().get_first_volume()->world_matrix();
    const auto                     corners = corner_positions();
    const std::array<ColorRGBA, 4> colors{ColorRGBA(1.f, 0.2f, 0.2f, 1.f), ColorRGBA(0.2f, 1.f, 0.3f, 1.f), ColorRGBA(0.3f, 0.6f, 1.f, 1.f),
                                          ColorRGBA(1.f, 0.85f, 0.2f, 1.f)};
    GLShaderProgram*               shader = wxGetApp().get_shader("flat");
    if (!shader)
        return;
    glsafe(::glPushAttrib(GL_ENABLE_BIT | GL_LINE_BIT));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glLineWidth(2.f));
    shader->start_using();
    shader->set_uniform("view_model_matrix", Transform3d::Identity());
    shader->set_uniform("projection_matrix", Transform3d::Identity());
    for (int i = 0; i < 4; ++i) {
        if (!corners[i].allFinite())
            continue;
        const Vec2d screen = CameraUtils::project(camera, world * corners[i]).cast<double>();
        if (!screen.allFinite())
            continue;
        GLModel::Geometry geometry;
        geometry.format    = {GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P2};
        geometry.color     = colors[i];
        const float radius = (m_corner_drag == i ? 10.f : 7.f) * m_parent.get_scale();
        for (unsigned j = 0; j < 32; ++j) {
            for (unsigned k = 0; k < 2; ++k) {
                const double angle = 2 * PI * (j + k) / 32;
                geometry.add_vertex(Vec2f(float(2 * (screen.x() + radius * std::cos(angle)) / size.get_width() - 1),
                                          float(1 - 2 * (screen.y() + radius * std::sin(angle)) / size.get_height())));
                geometry.add_index(2 * j + k);
            }
        }
        m_corner_handles.reset();
        m_corner_handles.init_from(std::move(geometry));
        m_corner_handles.render();
    }
    shader->stop_using();
    glsafe(::glPopAttrib());
}

} // namespace Slic3r::GUI
