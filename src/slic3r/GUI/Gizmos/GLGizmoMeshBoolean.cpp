#include "GLGizmoMeshBoolean.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/BitmapCache.hpp"

#include <GL/glew.h>

#include <cmath>
#include <thread>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include <imgui/imgui_internal.h>

#include <algorithm>
namespace Slic3r {
namespace GUI {

static const std::string warning_text = _u8L("Unable to perform boolean operation on selected parts");
static const std::string ambiguous_provenance_text = _u8L("Boolean paint transfer is ambiguous because source faces overlap. Repair or separate coincident faces and try again.");
static const std::string preview_busy_text = _u8L("A cancelled Boolean preview is still finishing. Try again in a moment.");
static std::atomic<bool> boolean_preview_worker_active{ false };

GLGizmoMeshBoolean::GLGizmoMeshBoolean(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{
}

GLGizmoMeshBoolean::~GLGizmoMeshBoolean() 
{
    if (m_preview_work)
        m_preview_work->cancelled.store(true, std::memory_order_release);
    m_preview_work.reset();
}

bool GLGizmoMeshBoolean::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down) 
{
    const bool brush_mode = m_operation_mode == MeshBooleanOperation::Difference && m_cutter_kind == CutterKind::Brush;
    if (brush_mode && action == SLAGizmoEventType::LeftUp) {
        if (m_brush_stroke_active) {
            m_brush_stroke_active = false;
            if (!control_down && m_src.mv && !m_brush_stroke_points.empty()) {
                ++m_cutter_revision;
                create_preview("A_NOT_B", false);
            }
            m_brush_stroke_points.clear();
            m_parent.set_as_dirty();
        }
        return true;
    }

    if (action == SLAGizmoEventType::LeftDown ||
        (brush_mode && action == SLAGizmoEventType::Dragging && m_brush_stroke_active)) {
        const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
        if (mo == nullptr)
            return true;
        const int instance_idx = m_parent.get_selection().get_instance_idx();
        if (instance_idx < 0 || size_t(instance_idx) >= mo->instances.size())
            return true;
        const ModelInstance* mi = mo->instances[instance_idx];
        std::vector<Transform3d> trafo_matrices;
        for (const ModelVolume* mv : mo->volumes) {
            //if (mv->is_model_part()) { 
                trafo_matrices.emplace_back(mi->get_transformation().get_matrix() * mv->get_matrix()); 
            //}
        }

        const Camera& camera = wxGetApp().plater()->get_camera();
        Vec3f  normal = Vec3f::Zero();
        Vec3f  hit = Vec3f::Zero();
        size_t facet = 0;
        Vec3f  closest_hit = Vec3f::Zero();
        Vec3f  closest_normal = Vec3f::Zero();
        double closest_hit_squared_distance = std::numeric_limits<double>::max();
        int    closest_hit_mesh_id = -1;

        // Cast a ray on all meshes, pick the closest hit and save it for the respective mesh
        for (int mesh_id = 0; mesh_id < int(trafo_matrices.size()); ++mesh_id) {
            MeshRaycaster mesh_raycaster = MeshRaycaster(mo->volumes[mesh_id]->mesh_ptr());
            if (mesh_raycaster.unproject_on_mesh(mouse_position, trafo_matrices[mesh_id], camera, hit, normal,
                m_c->object_clipper()->get_clipping_plane(), &facet)) {
                // Is this hit the closest to the camera so far?
                double hit_squared_distance = (camera.get_position() - trafo_matrices[mesh_id] * hit.cast<double>()).squaredNorm();
                if (hit_squared_distance < closest_hit_squared_distance) {
                    closest_hit_squared_distance = hit_squared_distance;
                    closest_hit_mesh_id = mesh_id;
                    closest_hit = hit;
                    closest_normal = normal;
                }
            }
        }

        if (closest_hit == Vec3f::Zero() && closest_normal == Vec3f::Zero())
            return true;

        if (brush_mode) {
            if (action == SLAGizmoEventType::LeftDown) {
                m_brush_stroke_points.clear();
                m_brush_stroke_active = true;
                if (!m_src.mv || m_src.volume_idx != closest_hit_mesh_id) {
                    m_src.trafo = mo->volumes[closest_hit_mesh_id]->get_matrix();
                    m_src.volume_idx = closest_hit_mesh_id;
                    set_src_volume(mo->volumes[closest_hit_mesh_id]);
                }
            }
            if (!m_brush_stroke_active || !m_src.mv || closest_hit_mesh_id != m_src.volume_idx)
                return true;

            const Vec3d world_hit = trafo_matrices[closest_hit_mesh_id] * closest_hit.cast<double>();
            const Vec3d object_hit = mi->get_matrix().inverse() * world_hit;
            if (m_brush_stroke_points.empty() || (m_brush_stroke_points.back() - object_hit).norm() >= std::max(0.5, m_brush_radius * 0.3))
                m_brush_stroke_points.emplace_back(object_hit);
            m_parent.set_as_dirty();
            return true;
        }

        if (get_selecting_state() == MeshBooleanSelectingState::SelectTool) {
            m_tool.trafo = mo->volumes[closest_hit_mesh_id]->get_matrix();
            m_tool.volume_idx = closest_hit_mesh_id;
            set_tool_volume(mo->volumes[closest_hit_mesh_id]);
            return true;
        }
        if (get_selecting_state() == MeshBooleanSelectingState::SelectSource) {
            m_src.trafo = mo->volumes[closest_hit_mesh_id]->get_matrix();
            m_src.volume_idx = closest_hit_mesh_id;
            set_src_volume(mo->volumes[closest_hit_mesh_id]);
            m_selecting_state = MeshBooleanSelectingState::SelectTool;
            return true;
        }
    }
    return true;
}

bool GLGizmoMeshBoolean::on_mouse(const wxMouseEvent &mouse_event)
{
    // wxCoord == int --> wx/types.h
    Vec2i32 mouse_coord(mouse_event.GetX(), mouse_event.GetY());
    Vec2d mouse_pos = mouse_coord.cast<double>();

    // when control is down we allow scene pan and rotation even when clicking
    // over some object
    bool control_down           = mouse_event.CmdDown();
    bool grabber_contains_mouse = (get_hover_id() != -1);
    const bool brush_mode = m_state == On && m_operation_mode == MeshBooleanOperation::Difference && m_cutter_kind == CutterKind::Brush;
    if (brush_mode && mouse_event.Moving()) {
        m_parent.set_as_dirty();
        return false;
    }
    if (brush_mode && mouse_event.Dragging() && mouse_event.LeftIsDown()) {
        if (!control_down && gizmo_event(SLAGizmoEventType::Dragging, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false))
            return true;
    }
    if (brush_mode && mouse_event.LeftUp() && m_brush_stroke_active) {
        gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), control_down);
        return true;
    }
    if (mouse_event.LeftDown()) {
        if ((!control_down || grabber_contains_mouse) &&            
            gizmo_event(SLAGizmoEventType::LeftDown, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false))
            // the gizmo got the event and took some action, there is no need
            // to do anything more
            return true;
    }

    return false;
}

bool GLGizmoMeshBoolean::on_init()
{
    m_shortcut_key = WXK_CONTROL_B;
    return true;
}

std::string GLGizmoMeshBoolean::on_get_name() const
{
    return _u8L("Mesh Boolean");
}

bool GLGizmoMeshBoolean::on_is_activable() const
{
    return m_parent.get_selection().is_single_full_instance();
}

void GLGizmoMeshBoolean::on_render()
{
    if (m_parent.get_selection().get_object_idx() < 0) {
        if (m_section_clipper_owned && m_c)
            m_c->object_clipper()->release();
        m_section_view_enabled = false;
        m_section_clipper_owned = false;
        return;
    }
    static ModelObject* last_mo = nullptr;
    ModelObject* curr_mo = m_parent.get_selection().get_model()->objects[m_parent.get_selection().get_object_idx()];
    if (last_mo != curr_mo) {
        if (m_section_clipper_owned && m_c)
            m_c->object_clipper()->release();
        m_section_view_enabled = false;
        m_section_clipper_owned = false;
        last_mo = curr_mo;
        clear_preview();
        m_src.reset();
        m_tool.reset();
        m_cutter_position_initialized = false;
        m_cutter_preview_model.reset();
        m_operation_mode = MeshBooleanOperation::Difference;
        m_selecting_state = MeshBooleanSelectingState::SelectSource;
        return;
    }
    BoundingBoxf3 src_bb;
    BoundingBoxf3 tool_bb;
    const ModelObject* mo = m_c->selection_info()->model_object();
    if (!mo)
        return;
    const auto refresh_volume_index = [this, mo](VolumeInfo& volume) {
        if (!volume.mv)
            return;
        const auto it = std::find(mo->volumes.begin(), mo->volumes.end(), volume.mv);
        if (it == mo->volumes.end()) {
            volume.reset();
            clear_preview();
        } else {
            volume.volume_idx = int(it - mo->volumes.begin());
        }
    };
    refresh_volume_index(m_src);
    refresh_volume_index(m_tool);
    poll_preview_work();
    const int instance_idx = m_parent.get_selection().get_instance_idx();
    if (instance_idx < 0 || size_t(instance_idx) >= mo->instances.size())
        return;
    const Selection& selection = m_parent.get_selection();
    const Selection::IndicesList& idxs = selection.get_volume_idxs();
    for (unsigned int i : idxs) {
        const GLVolume* volume = selection.get_volume(i);
        if(volume->volume_idx() == m_src.volume_idx) {
            src_bb = volume->transformed_convex_hull_bounding_box();
        }
        if (volume->volume_idx() == m_tool.volume_idx) {
            tool_bb = volume->transformed_convex_hull_bounding_box();
        }
    }

    ColorRGB src_color = { 1.0f, 1.0f, 1.0f };
    ColorRGB tool_color = {0.0f, 150.0f / 255.0f, 136.0f / 255.0f};
    m_parent.get_selection().render_bounding_box(src_bb, src_color, m_parent.get_scale());
    m_parent.get_selection().render_bounding_box(tool_bb, tool_color, m_parent.get_scale());

    if (m_section_view_enabled)
        m_c->object_clipper()->render_cut();

    if (m_preview && !preview_is_current())
        clear_preview();
    if (m_preview)
        render_preview();
    else if (m_operation_mode == MeshBooleanOperation::Difference && m_cutter_kind != CutterKind::Mesh && m_cutter_kind != CutterKind::Brush) {
        if (!m_cutter_preview_model)
            update_cutter_preview();
        render_cutter_preview();
    }
    if (m_operation_mode == MeshBooleanOperation::Difference && m_cutter_kind == CutterKind::Brush)
        render_brush_cursor();
}

void GLGizmoMeshBoolean::on_set_state()
{
    if (m_state == EState::On) {
        clear_preview();
        m_brush_stroke_active = false;
        m_brush_stroke_points.clear();
        m_src.reset();
        m_tool.reset();
        m_cutter_position_initialized = false;
        m_section_view_enabled = false;
        m_section_clipper_owned = false;
        m_diff_delete_input = false;
        m_inter_delete_input = false;
        m_operation_mode = MeshBooleanOperation::Difference;
        m_selecting_state = MeshBooleanSelectingState::SelectSource;
    }
    else if (m_state == EState::Off) {
        clear_preview();
        m_brush_stroke_active = false;
        m_brush_stroke_points.clear();
        if (m_section_clipper_owned && m_c)
            m_c->object_clipper()->release();
        m_section_view_enabled = false;
        m_section_clipper_owned = false;
        m_src.reset();
        m_tool.reset();
        m_cutter_position_initialized = false;
        m_diff_delete_input = false;
        m_inter_delete_input = false;
        m_operation_mode = MeshBooleanOperation::Undef;
        m_selecting_state = MeshBooleanSelectingState::Undef;
        wxGetApp().notification_manager()->close_plater_warning_notification(warning_text);
    }
}

CommonGizmosDataID GLGizmoMeshBoolean::on_get_requirements() const
{
    return CommonGizmosDataID(
        int(CommonGizmosDataID::SelectionInfo)
        | int(CommonGizmosDataID::InstancesHider)
        | int(CommonGizmosDataID::Raycaster)
        | int(CommonGizmosDataID::ObjectClipper));
}

void GLGizmoMeshBoolean::on_render_input_window(float x, float y, float bottom_limit)
{
    y = std::min(y, bottom_limit - ImGui::GetWindowHeight());

    static float last_y = 0.0f;
    static float last_w = 0.0f;

    const float currt_scale = m_parent.get_scale();
    ImGuiWrapper::push_toolbar_style(currt_scale);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin("MeshBoolean", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    m_imgui->text(_L("Applying this edit changes every linked instance of the source part."));

    const int max_tab_length = 2 * ImGui::GetStyle().FramePadding.x + std::max(ImGui::CalcTextSize(_u8L("Union").c_str()).x,
        std::max(ImGui::CalcTextSize(_u8L("Difference").c_str()).x, ImGui::CalcTextSize(_u8L("Intersection").c_str()).x));
    const int max_cap_length = ImGui::GetStyle().WindowPadding.x + ImGui::GetStyle().ItemSpacing.x +
                               std::max({ImGui::CalcTextSize(_u8L("Source Volume").c_str()).x,
                                         ImGui::CalcTextSize(_u8L("Tool Volume").c_str()).x,
                                         ImGui::CalcTextSize(_u8L("Subtract from").c_str()).x,
                                         ImGui::CalcTextSize(_u8L("Subtract with").c_str()).x});

    const int select_btn_length = 2 * ImGui::GetStyle().FramePadding.x + std::max(ImGui::CalcTextSize(("1 " + _u8L("selected")).c_str()).x, ImGui::CalcTextSize(_u8L("Select").c_str()).x);

    auto selectable = [this](const std::string& label, bool selected, const ImVec2& size_arg) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0,0 });

        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImVec2 label_size = ImGui::CalcTextSize(label.c_str(), NULL, true);
        ImVec2 pos = window->DC.CursorPos;
        ImVec2 size = ImGui::CalcItemSize(size_arg, label_size.x + ImGui::GetStyle().FramePadding.x * 2.0f, label_size.y + ImGui::GetStyle().FramePadding.y * 2.0f);
        bool hovered = ImGui::IsMouseHoveringRect(pos, pos + size);

        if (selected || hovered) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Button, { 0, 150.0f / 255.0f, 136.0f / 255.0f, 1.0f });
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, { 0, 150.0f / 255.0f, 136.0f / 255.0f, 1.0f });
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, { 0, 150.0f / 255.0f, 136.0f / 255.0f, 1.0f });
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, { 0, 150.0f / 255.0f, 136.0f / 255.0f, 1.0f });
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, { 0, 150.0f / 255.0f, 136.0f / 255.0f, 1.0f });
        }

        bool res = ImGui::Button(label.c_str(), size_arg);

        if (selected || hovered) {
            ImGui::PopStyleColor(4);
        }
        else {
            ImGui::PopStyleColor(2);
        }

        ImGui::PopStyleVar(1);
        return res;
    };

    auto operate_button = [this](const wxString &label, bool enable) {
        if (!enable) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            if (m_is_dark_mode) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(39.0f / 255.0f, 39.0f / 255.0f, 39.0f / 255.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(108.0f / 255.0f, 108.0f / 255.0f, 108.0f / 255.0f, 1.0f));
            }
            else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(230.0f / 255.0f, 230.0f / 255.0f, 230.0f / 255.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(163.0f / 255.0f, 163.0f / 255.0f, 163.0f / 255.0f, 1.0f));
            }
        }

        bool res = m_imgui->button(label.c_str());

        if (!enable) {
            ImGui::PopItemFlag();
            ImGui::PopStyleColor(2);
        }
        return res;
    };

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    if (selectable(_u8L("Union"), m_operation_mode == MeshBooleanOperation::Union, ImVec2(max_tab_length, 0.0f))) {
        if (m_operation_mode != MeshBooleanOperation::Union)
            clear_preview();
        m_operation_mode = MeshBooleanOperation::Union;
    }
    ImGui::SameLine(0, 0);
    if (selectable(_u8L("Difference"), m_operation_mode == MeshBooleanOperation::Difference, ImVec2(max_tab_length, 0.0f))) {
        if (m_operation_mode != MeshBooleanOperation::Difference)
            clear_preview();
        m_operation_mode = MeshBooleanOperation::Difference;
    }
    ImGui::SameLine(0, 0);
    if (selectable(_u8L("Intersection"), m_operation_mode == MeshBooleanOperation::Intersection, ImVec2(max_tab_length, 0.0f))) {
        if (m_operation_mode != MeshBooleanOperation::Intersection)
            clear_preview();
        m_operation_mode = MeshBooleanOperation::Intersection;
    }
    ImGui::PopStyleVar();

    ImGui::AlignTextToFramePadding();
    std::string cap_str1 = m_operation_mode != MeshBooleanOperation::Difference ? _u8L("Part 1") : _u8L("Subtract from");
    m_imgui->text(cap_str1);
    ImGui::SameLine(max_cap_length);
    std::string select_src_str = m_src.mv ? "1 " + _u8L("selected") :
        (m_cutter_kind == CutterKind::Brush ? _u8L("Auto on stroke") : _u8L("Select"));
    select_src_str += "##select_source_volume";
    ImGui::PushItemWidth(select_btn_length);
    if (selectable(select_src_str, m_selecting_state == MeshBooleanSelectingState::SelectSource, ImVec2(select_btn_length, 0)))
        m_selecting_state = MeshBooleanSelectingState::SelectSource;
    ImGui::PopItemWidth();
    if (m_src.mv) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_src.mv->name);

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_Border, { 0, 0, 0, 0 });
        if (ImGui::Button((into_u8(ImGui::TextSearchCloseIcon) + "##src").c_str(), {18, 18}))
        {
            clear_preview();
            m_src.reset();
        }
        ImGui::PopStyleColor(5);
    }

    if (m_operation_mode == MeshBooleanOperation::Difference) {
        const char* cutter_names[] = { "Mesh", "Box", "Cylinder", "Sphere", "Brush" };
        const char* cutter_name = cutter_names[static_cast<int>(m_cutter_kind)];
        if (ImGui::BeginCombo(_u8L("Cutter type").c_str(), _u8L(cutter_name).c_str())) {
            for (int i = 0; i < 5; ++i) {
                if (ImGui::Selectable(_u8L(cutter_names[i]).c_str(), static_cast<int>(m_cutter_kind) == i)) {
                    m_cutter_kind = static_cast<CutterKind>(i);
                    m_brush_stroke_active = false;
                    m_brush_stroke_points.clear();
                    if (m_cutter_kind == CutterKind::Mesh)
                        m_selecting_state = m_tool.mv ? MeshBooleanSelectingState::Undef : MeshBooleanSelectingState::SelectTool;
                    else
                        m_selecting_state = MeshBooleanSelectingState::Undef;
                    ++m_cutter_revision;
                    clear_preview();
                    update_cutter_preview();
                }
            }
            ImGui::EndCombo();
        }
    }
    m_imgui->text(_L("The edit applies to all linked instances."));

    if (m_operation_mode != MeshBooleanOperation::Difference || m_cutter_kind == CutterKind::Mesh) {
        ImGui::AlignTextToFramePadding();
        std::string cap_str2 = m_operation_mode != MeshBooleanOperation::Difference ? _u8L("Part 2") : _u8L("Subtract with");
        m_imgui->text(cap_str2);
        ImGui::SameLine(max_cap_length);
        std::string select_tool_str = m_tool.mv ? "1 " + _u8L("selected") : _u8L("Select");
        select_tool_str += "##select_tool_volume";
        ImGui::PushItemWidth(select_btn_length);
        if (selectable(select_tool_str, m_selecting_state == MeshBooleanSelectingState::SelectTool, ImVec2(select_btn_length, 0)))
            m_selecting_state = MeshBooleanSelectingState::SelectTool;
        ImGui::PopItemWidth();
        if (m_tool.mv) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_tool.mv->name);

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_Button));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button));
            ImGui::PushStyleColor(ImGuiCol_Border, { 0, 0, 0, 0 });
            if (ImGui::Button((into_u8(ImGui::TextSearchCloseIcon) + "tool").c_str(), {18, 18})) {
                clear_preview();
                m_tool.reset();
            }
            ImGui::PopStyleColor(5);
        }
    } else if (m_cutter_kind == CutterKind::Brush) {
        const double min_radius = 0.25;
        const double max_radius = 100.0;
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::DragScalar(_u8L("Brush radius (mm)").c_str(), ImGuiDataType_Double, &m_brush_radius,
                              0.1f, &min_radius, &max_radius, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
            m_brush_radius = std::clamp(m_brush_radius, min_radius, max_radius);
            ++m_cutter_revision;
            clear_preview();
        }
        m_imgui->text(_L("The first stroke selects the part under the brush."));
        m_imgui->text(_L("Click and drag over the model to paint a subtract stroke."));
        m_imgui->text(_L("Release the mouse to preview the cut."));
    } else {
        bool cutter_changed = false;
        auto input_mm = [&cutter_changed](const char* label, double& value) {
            ImGui::SetNextItemWidth(80.0f);
            const double step = ImGui::GetIO().KeyShift ? 0.001 : 0.01;
            const double fast_step = ImGui::GetIO().KeyShift ? 0.01 : 0.1;
            cutter_changed |= ImGui::InputDouble(label, &value, step, fast_step, "%.3f", ImGuiInputTextFlags_CharsDecimal);
        };
        auto input_position_mm = [&](const char* label, double& value) {
            input_mm(label, value);
            if (m_snap_cutter_position && ImGui::IsItemDeactivatedAfterEdit()) {
                const double snapped = std::round(value / 0.05) * 0.05;
                if (snapped != value) {
                    value = snapped;
                    cutter_changed = true;
                }
            }
        };
        bool section_view = m_section_view_enabled;
        if (ImGui::Checkbox(_u8L("Section view").c_str(), &section_view)) {
            m_section_view_enabled = section_view;
            if (m_section_view_enabled) {
                m_section_clipper_owned = true;
                m_c->object_clipper()->set_behavior(true, true, 0.0);
                m_c->object_clipper()->set_position_by_ratio(0.5, false);
            } else {
                m_c->object_clipper()->release();
                m_section_clipper_owned = false;
            }
            m_parent.set_as_dirty();
        }
        if (m_section_view_enabled) {
            float section_depth = float(m_c->object_clipper()->get_position());
            if (m_imgui->slider_float("Section depth##boolean_section", &section_depth, 0.0f, 1.0f, "%.2f"))
                m_c->object_clipper()->set_position_by_ratio(section_depth, true);
        }
        ImGui::TextUnformatted(_u8L("Dimensions (mm)").c_str());
        if (m_cutter_kind == CutterKind::Box) {
            input_mm("X##cutter_size_x", m_cutter_size.x()); ImGui::SameLine();
            input_mm("Y##cutter_size_y", m_cutter_size.y()); ImGui::SameLine();
            input_mm("Z##cutter_size_z", m_cutter_size.z());
        } else if (m_cutter_kind == CutterKind::Cylinder) {
            input_mm("Diameter##cutter_diameter", m_cutter_size.x()); ImGui::SameLine();
            input_mm("Height##cutter_height", m_cutter_size.z());
        } else {
            input_mm("Diameter##cutter_sphere_diameter", m_cutter_size.x());
        }
        ImGui::TextUnformatted(_u8L("Center position (world mm)").c_str());
        ImGui::Checkbox(_u8L("Snap position to 0.05 mm").c_str(), &m_snap_cutter_position);
        input_position_mm("X##cutter_position_x", m_cutter_position.x()); ImGui::SameLine();
        input_position_mm("Y##cutter_position_y", m_cutter_position.y()); ImGui::SameLine();
        input_position_mm("Z##cutter_position_z", m_cutter_position.z());
        m_imgui->text(_L("Hold Shift for 0.001 mm steps."));
        if (m_cutter_kind != CutterKind::Sphere) {
            ImGui::TextUnformatted(_u8L("Rotation (degrees)").c_str());
            input_mm("X##cutter_rotation_x", m_cutter_rotation.x()); ImGui::SameLine();
            input_mm("Y##cutter_rotation_y", m_cutter_rotation.y()); ImGui::SameLine();
            input_mm("Z##cutter_rotation_z", m_cutter_rotation.z());
        }
        if (cutter_changed) {
            for (int axis = 0; axis < 3; ++axis)
                m_cutter_size[axis] = std::max(0.05, m_cutter_size[axis]);
            ++m_cutter_revision;
            clear_preview();
            update_cutter_preview();
        }
    }

    if (m_operation_mode == MeshBooleanOperation::Difference) {
        int filament_count = 1;
        if (wxGetApp().preset_bundle) {
            const ConfigOptionStrings* filament_colours = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            if (filament_colours)
                filament_count = std::max(1, int(filament_colours->values.size()));
        }
        m_new_face_filament = std::min(m_new_face_filament, filament_count);
        const std::string preview_label = m_new_face_filament == 0 ? _u8L("Part default") :
            _u8L("Filament") + " " + std::to_string(m_new_face_filament);
        if (ImGui::BeginCombo(_u8L("Cavity wall filament").c_str(), preview_label.c_str())) {
            if (ImGui::Selectable(_u8L("Part default").c_str(), m_new_face_filament == 0)) {
                clear_preview();
                m_new_face_filament = 0;
            }
            for (int filament = 1; filament <= filament_count; ++filament) {
                const std::string label = _u8L("Filament") + " " + std::to_string(filament);
                if (ImGui::Selectable(label.c_str(), m_new_face_filament == filament)) {
                    clear_preview();
                    m_new_face_filament = filament;
                }
            }
            ImGui::EndCombo();
        }
    }

    bool enable_button = m_src.mv && (m_operation_mode == MeshBooleanOperation::Difference && m_cutter_kind != CutterKind::Mesh ? true : m_tool.mv != nullptr);
    if (m_operation_mode == MeshBooleanOperation::Difference && m_cutter_kind == CutterKind::Mesh) {
        const bool delete_input_before = m_diff_delete_input;
        m_imgui->bbl_checkbox(_L("Delete input"), m_diff_delete_input);
        if (delete_input_before != m_diff_delete_input)
            clear_preview();
    } else if (m_operation_mode == MeshBooleanOperation::Intersection) {
        const bool delete_input_before = m_inter_delete_input;
        m_imgui->bbl_checkbox(_L("Delete input"), m_inter_delete_input);
        if (delete_input_before != m_inter_delete_input)
            clear_preview();
    }

    if (m_preview_work) {
        m_imgui->text(_L("Computing exact geometry preview…"));
        if (operate_button(_L("Cancel") + "##cancel_boolean_work", true))
            cancel_preview_work();
    } else if (m_preview) {
        m_imgui->text(_L("Exact geometry preview. The model changes only when you apply it."));
        if (m_preview->empty) {
            if (m_preview->operation == MeshBooleanOperation::Difference) {
                if (operate_button(_L("Remove target part") + "##apply_empty", true))
                    apply_preview();
            } else {
                m_imgui->text(_L("The Boolean result is empty; there is nothing to apply."));
            }
        } else if (operate_button(_L("Apply") + "##apply_boolean", true)) {
            apply_preview();
        }
        ImGui::SameLine();
        if (operate_button(_L("Cancel") + "##cancel_boolean", true))
            clear_preview();
    } else if (m_operation_mode == MeshBooleanOperation::Union) {
        if (operate_button(_L("Preview union") + "##btn", enable_button))
            create_preview("UNION", true);
    } else if (m_operation_mode == MeshBooleanOperation::Difference && m_cutter_kind == CutterKind::Brush) {
        m_imgui->text(_L("Paint a stroke on the model to create a cut preview."));
    } else if (m_operation_mode == MeshBooleanOperation::Difference) {
        if (operate_button(_L("Preview difference") + "##btn", enable_button))
            create_preview("A_NOT_B", m_cutter_kind == CutterKind::Mesh && m_diff_delete_input);
    } else if (m_operation_mode == MeshBooleanOperation::Intersection) {
        if (operate_button(_L("Preview intersection") + "##btn", enable_button))
            create_preview("INTERSECTION", m_inter_delete_input);
    }

    float win_w = ImGui::GetWindowWidth();
    if (last_w != win_w || last_y != y) {
        // ask canvas for another frame to render the window in the correct position
        m_imgui->set_requires_extra_frame();
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        if (last_w != win_w)
            last_w = win_w;
        if (last_y != y)
            last_y = y;
    }

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

bool GLGizmoMeshBoolean::create_preview(const std::string& operation, bool delete_input)
{
    clear_preview();
    const bool primitive_cutter = m_operation_mode == MeshBooleanOperation::Difference && m_cutter_kind != CutterKind::Mesh;
    if (!m_src.mv || (!primitive_cutter && (!m_tool.mv || m_src.mv == m_tool.mv)))
        return false;

    TriangleMesh source_mesh = m_src.mv->mesh();
    // Boolean inputs and the generated result use model-object coordinates.
    source_mesh.transform(m_src.trafo, true);
    TriangleMesh tool_mesh = primitive_cutter ? make_primitive_cutter_mesh() : m_tool.mv->mesh();
    if (!primitive_cutter)
        tool_mesh.transform(m_tool.trafo, true);

    const Selection& selection = m_parent.get_selection();
    ModelObject* object = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const int instance_idx = selection.get_instance_idx();
    if (!object || instance_idx < 0 || size_t(instance_idx) >= object->instances.size())
        return false;

    Preview preview;
    preview.operation = m_operation_mode;
    preview.object_id = object->id();
    preview.cutter_instance_idx = instance_idx;
    preview.cutter_instance_transform = object->instances[instance_idx]->get_matrix();
    preview.source_id = m_src.mv->id();
    preview.primitive_cutter = primitive_cutter;
    preview.cutter_revision = m_cutter_revision;
    if (!primitive_cutter)
        preview.tool_id = m_tool.mv->id();
    preview.source_mesh = m_src.mv->mesh_ptr().get();
    preview.tool_mesh = primitive_cutter ? nullptr : m_tool.mv->mesh_ptr().get();
    preview.source_config_timestamp = static_cast<const ObjectBase&>(m_src.mv->config).timestamp();
    preview.source_mmu_timestamp = m_src.mv->mmu_segmentation_facets.timestamp();
    preview.source_support_timestamp = m_src.mv->supported_facets.timestamp();
    preview.source_seam_timestamp = m_src.mv->seam_facets.timestamp();
    preview.source_fuzzy_timestamp = m_src.mv->fuzzy_skin_facets.timestamp();
    preview.source_transform = m_src.trafo;
    preview.tool_transform = primitive_cutter ? Transform3d::Identity() : m_tool.trafo;
    preview.delete_input = delete_input;
    auto work = std::make_shared<PreviewWork>();
    work->snapshot = std::move(preview);
    if (m_src.mv->is_any_painted() && m_src.mv->is_model_part() && !m_src.mv->mesh().empty()) {
        work->source_paint_mesh = m_src.mv->mesh_ptr();
        work->saved_painting.emplace();
        work->saved_painting->supported = m_src.mv->supported_facets.get_data();
        work->saved_painting->seam = m_src.mv->seam_facets.get_data();
        work->saved_painting->mmu = m_src.mv->mmu_segmentation_facets.get_data();
        work->saved_painting->fuzzy = m_src.mv->fuzzy_skin_facets.get_data();
        work->saved_painting->transform = m_src.trafo;
    }
    work->source_filament = m_src.mv->config.has("extruder") ? m_src.mv->config.extruder() : 1;
    work->new_face_filament = m_new_face_filament;
    bool expected_inactive = false;
    if (!boolean_preview_worker_active.compare_exchange_strong(expected_inactive, true, std::memory_order_acq_rel)) {
        wxGetApp().notification_manager()->push_plater_warning_notification(preview_busy_text);
        return false;
    }
    m_preview_work = work;
    try {
        std::thread([work, source_mesh = std::move(source_mesh), tool_mesh = std::move(tool_mesh), operation]() mutable {
            struct WorkerActiveReset {
                ~WorkerActiveReset() { boolean_preview_worker_active.store(false, std::memory_order_release); }
            } reset_active;
            Slic3r::MeshBoolean::mcut::BooleanResult result;
            bool success = false;
            bool ambiguous_source_provenance = false;
            try {
                success = Slic3r::MeshBoolean::mcut::make_boolean(source_mesh, tool_mesh, result, operation);
                ambiguous_source_provenance = result.failure == Slic3r::MeshBoolean::mcut::BooleanFailure::AmbiguousSourceFaceProvenance;
                success = success && result.source_faces.size() == result.mesh.its.indices.size() &&
                          result.source_face_ids.size() == result.mesh.its.indices.size();
                if (success && !result.mesh.empty() && !work->cancelled.load(std::memory_order_acquire)) {
                    if (work->saved_painting && work->source_paint_mesh)
                        work->saved_painting->mesh = *work->source_paint_mesh;
                    const Vec3d center = result.mesh.bounding_box().center();
                    TriangleMesh display_mesh = result.mesh;
                    display_mesh.transform(Geometry::translation_transform(-center), true);
                    const Transform3d target_transform = Geometry::translation_transform(center);
                    auto remap = [&](const TriangleSelector::TriangleSplittingData& source) {
                        if (!work->saved_painting || source.bitstream.empty())
                            return TriangleSelector::TriangleSplittingData{};
                        return TriangleSelector::remap_painting(work->saved_painting->mesh.its, source,
                            display_mesh.its, target_transform, std::nullopt, work->saved_painting->transform,
                            &result.source_faces, &result.source_face_ids);
                    };

                    if (!work->cancelled.load(std::memory_order_acquire))
                        work->annotations.supported = work->saved_painting ? remap(work->saved_painting->supported) :
                            TriangleSelector::TriangleSplittingData{};
                    if (!work->cancelled.load(std::memory_order_acquire))
                        work->annotations.seam = work->saved_painting ? remap(work->saved_painting->seam) :
                            TriangleSelector::TriangleSplittingData{};
                    if (!work->cancelled.load(std::memory_order_acquire))
                        work->annotations.mmu = work->saved_painting ? remap(work->saved_painting->mmu) :
                            TriangleSelector::TriangleSplittingData{};
                    if (!work->cancelled.load(std::memory_order_acquire))
                        work->annotations.fuzzy = work->saved_painting ? remap(work->saved_painting->fuzzy) :
                            TriangleSelector::TriangleSplittingData{};

                    if (!work->cancelled.load(std::memory_order_acquire)) {
                        TriangleSelector selector(display_mesh);
                        if (!work->annotations.mmu.bitstream.empty())
                            selector.deserialize(work->annotations.mmu);
                        if (work->snapshot.operation == MeshBooleanOperation::Difference && work->new_face_filament > 0) {
                            const EnforcerBlockerType state = static_cast<EnforcerBlockerType>(work->new_face_filament);
                            for (size_t face = 0; face < result.source_faces.size(); ++face)
                                if (!result.source_faces[face])
                                    selector.set_facet(static_cast<int>(face), state);
                            work->annotations.mmu = selector.serialize();
                        }

                        std::vector<EnforcerBlockerType> states =
                            TriangleSelector::extract_used_facet_states(work->annotations.mmu);
                        states.push_back(EnforcerBlockerType::NONE);
                        std::sort(states.begin(), states.end(), [](EnforcerBlockerType lhs, EnforcerBlockerType rhs) {
                            return static_cast<int>(lhs) < static_cast<int>(rhs);
                        });
                        states.erase(std::unique(states.begin(), states.end()), states.end());
                        for (EnforcerBlockerType state : states) {
                            indexed_triangle_set facets = selector.get_facets(state);
                            if (facets.indices.empty())
                                continue;
                            int filament = state == EnforcerBlockerType::NONE ? work->source_filament : static_cast<int>(state);
                            work->surfaces.push_back({ std::move(facets), std::max(1, filament) });
                        }
                    }
                }
            } catch (...) {
                success = false;
            }
            work->saved_painting.reset();
            std::lock_guard<std::mutex> lock(work->mutex);
            if (!work->cancelled.load(std::memory_order_acquire) && success) {
                work->mesh = std::move(result.mesh);
                work->source_faces = std::move(result.source_faces);
                work->success = true;
            }
            work->ambiguous_source_provenance = ambiguous_source_provenance;
            work->done = true;
        }).detach();
    } catch (...) {
        m_preview_work.reset();
        boolean_preview_worker_active.store(false, std::memory_order_release);
        wxGetApp().notification_manager()->push_plater_warning_notification(warning_text);
        return false;
    }
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
    return true;
}

void GLGizmoMeshBoolean::poll_preview_work()
{
    std::shared_ptr<PreviewWork> work = m_preview_work;
    if (!work)
        return;

    bool done = false;
    {
        std::lock_guard<std::mutex> lock(work->mutex);
        done = work->done;
    }
    if (!done) {
        m_parent.request_extra_frame();
        return;
    }
    m_preview_work.reset();
    if (work->cancelled.load(std::memory_order_acquire))
        return;
    if (!work->success || work->source_faces.size() != work->mesh.its.indices.size()) {
        wxGetApp().notification_manager()->push_plater_warning_notification(
            work->ambiguous_source_provenance ? ambiguous_provenance_text : warning_text);
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return;
    }

    m_preview = std::move(work->snapshot);
    if (!preview_is_current()) {
        m_preview.reset();
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return;
    }
    Preview preview = std::move(*m_preview);
    m_preview.reset();
    preview.annotations = std::move(work->annotations);
    finish_preview(std::move(preview), std::move(work->mesh), std::move(work->source_faces), std::move(work->surfaces));
}

void GLGizmoMeshBoolean::cancel_preview_work()
{
    if (!m_preview_work)
        return;
    m_preview_work->cancelled.store(true, std::memory_order_release);
    m_preview_work.reset();
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

void GLGizmoMeshBoolean::finish_preview(Preview preview, TriangleMesh result_mesh, std::vector<uint8_t> source_faces,
                                        std::vector<PreviewFacetData> surfaces)
{
    preview.empty = result_mesh.empty();
    preview.source_faces = std::move(source_faces);

    if (!preview.empty) {
        // Preview surfaces are built from the centered display mesh. Translate
        // them back to the raw Boolean result's object-space position when drawn.
        preview.result_center = result_mesh.bounding_box().center();
        const std::vector<std::string> filament_colours = wxGetApp().plater()->get_extruder_colors_from_plater_config();
        for (PreviewFacetData& surface : surfaces) {
            ColorRGBA color(0.72f, 0.72f, 0.72f, 0.78f);
            if (size_t(surface.filament - 1) < filament_colours.size()) {
                unsigned char rgba[4] = { 0, 0, 0, 255 };
                if (BitmapCache::parse_color4(filament_colours[surface.filament - 1], rgba))
                    color = ColorRGBA(float(rgba[0]) / 255.f, float(rgba[1]) / 255.f, float(rgba[2]) / 255.f, 0.78f);
            }
            auto model = std::make_unique<GLModel>();
            model->init_from(surface.facets);
            m_preview_surfaces.push_back({ std::move(model), color });
        }

        // Keep raw output geometry for Apply. The remapped annotations are stored
        // alongside it so the committed labels exactly match this preview.
        preview.mesh = std::move(result_mesh);
        preview.source_faces.shrink_to_fit();
    }

    m_preview = std::move(preview);
    if (!m_preview->empty)
        hide_preview_inputs();
    wxGetApp().notification_manager()->close_plater_warning_notification(warning_text);
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

TriangleMesh GLGizmoMeshBoolean::make_primitive_cutter_mesh() const
{
    const Vec3d size = m_cutter_size.cwiseMax(Vec3d::Constant(0.05));
    TriangleMesh mesh;
    Vec3d local_center = Vec3d::Zero();
    switch (m_cutter_kind) {
    case CutterKind::Box:
        mesh = make_cube(size.x(), size.y(), size.z());
        local_center = size * 0.5;
        break;
    case CutterKind::Cylinder:
        mesh = make_cylinder(size.x() * 0.5, size.z(), PI / 180.0);
        local_center.z() = size.z() * 0.5;
        break;
    case CutterKind::Sphere:
        mesh = make_sphere(size.x() * 0.5, PI / 36.0);
        break;
    case CutterKind::Mesh:
        return mesh;
    case CutterKind::Brush:
        return make_brush_cutter_mesh();
    }

    const Eigen::Matrix3d rotation =
        Eigen::AngleAxisd(m_cutter_rotation.z() * PI / 180.0, Vec3d::UnitZ()).toRotationMatrix() *
        Eigen::AngleAxisd(m_cutter_rotation.y() * PI / 180.0, Vec3d::UnitY()).toRotationMatrix() *
        Eigen::AngleAxisd(m_cutter_rotation.x() * PI / 180.0, Vec3d::UnitX()).toRotationMatrix();
    const Selection& selection = m_parent.get_selection();
    const int instance_idx = selection.get_instance_idx();
    const ModelObject* object = m_c ? (m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr) : nullptr;
    if (!object || instance_idx < 0 || size_t(instance_idx) >= object->instances.size() || !m_src.mv)
        return TriangleMesh();
    const Transform3d world_transform = Eigen::Translation3d(m_cutter_position) * rotation * Eigen::Translation3d(-local_center);
    const Transform3d instance_transform = object->instances[instance_idx]->get_matrix();
    // Convert the world-space cutter to model-object coordinates, the same frame
    // used by the transformed source mesh. Do not apply the source volume offset
    // again: it is already part of m_src.trafo.
    const Transform3d transform = instance_transform.inverse() * world_transform;
    mesh.transform(transform, true);
    return mesh;
}

TriangleMesh GLGizmoMeshBoolean::make_brush_cutter_mesh() const
{
    if (m_brush_stroke_points.empty() || !(m_brush_radius > 0.0))
        return TriangleMesh();

    // A single click makes a rounded scoop. A drag creates one closed tube
    // along the stroke, so the Boolean sees one watertight cutter instead of
    // many intersecting spheres.
    if (m_brush_stroke_points.size() == 1) {
        TriangleMesh sphere = make_sphere(m_brush_radius, PI / 24.0);
        sphere.translate(m_brush_stroke_points.front().cast<float>());
        return sphere;
    }

    std::vector<Vec3d> path;
    path.emplace_back(m_brush_stroke_points.front());
    const double sample_step = std::max(0.5, m_brush_radius * 0.45);
    for (size_t i = 1; i < m_brush_stroke_points.size(); ++i) {
        const Vec3d delta = m_brush_stroke_points[i] - m_brush_stroke_points[i - 1];
        const double length = delta.norm();
        const int steps = std::max(1, int(std::ceil(length / sample_step)));
        for (int step = 1; step <= steps; ++step)
            path.emplace_back(m_brush_stroke_points[i - 1] + delta * (double(step) / steps));
    }

    constexpr int ring_segments = 16;
    indexed_triangle_set its;
    its.vertices.reserve(path.size() * ring_segments + 2);
    its.indices.reserve((path.size() - 1) * ring_segments * 2 + ring_segments * 2);
    std::vector<Vec3d> frame_u(path.size()), frame_v(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        Vec3d tangent = i == 0 ? path[1] - path[0] :
            i + 1 == path.size() ? path[i] - path[i - 1] : path[i + 1] - path[i - 1];
        if (tangent.squaredNorm() < 1e-12)
            tangent = Vec3d::UnitZ();
        tangent.normalize();
        const Vec3d reference = std::abs(tangent.z()) < 0.9 ? Vec3d::UnitZ() : Vec3d::UnitY();
        frame_u[i] = tangent.cross(reference).normalized();
        frame_v[i] = tangent.cross(frame_u[i]).normalized();
        for (int j = 0; j < ring_segments; ++j) {
            const double angle = 2.0 * PI * double(j) / ring_segments;
            const Vec3d point = path[i] + m_brush_radius *
                (std::cos(angle) * frame_u[i] + std::sin(angle) * frame_v[i]);
            its.vertices.emplace_back(point.cast<float>());
        }
    }

    for (size_t i = 0; i + 1 < path.size(); ++i) {
        for (int j = 0; j < ring_segments; ++j) {
            const int next = (j + 1) % ring_segments;
            const int a = int(i * ring_segments) + j;
            const int b = int(i * ring_segments) + next;
            const int c = int((i + 1) * ring_segments) + next;
            const int d = int((i + 1) * ring_segments) + j;
            its.indices.emplace_back(a, b, c);
            its.indices.emplace_back(a, c, d);
        }
    }

    const int start_center = int(its.vertices.size());
    its.vertices.emplace_back(path.front().cast<float>());
    const int end_center = int(its.vertices.size());
    its.vertices.emplace_back(path.back().cast<float>());
    const int end_ring = int((path.size() - 1) * ring_segments);
    for (int j = 0; j < ring_segments; ++j) {
        const int next = (j + 1) % ring_segments;
        its.indices.emplace_back(start_center, next, j);
        its.indices.emplace_back(end_center, end_ring + j, end_ring + next);
    }
    return TriangleMesh(std::move(its));
}

void GLGizmoMeshBoolean::initialize_cutter_position()
{
    if (m_cutter_position_initialized || !m_src.mv)
        return;
    const Selection& selection = m_parent.get_selection();
    const int instance_idx = selection.get_instance_idx();
    const ModelObject* object = m_c ? (m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr) : nullptr;
    if (!object || instance_idx < 0 || size_t(instance_idx) >= object->instances.size())
        return;

    TriangleMesh source_mesh = m_src.mv->mesh();
    source_mesh.transform(m_src.trafo, true);
    const BoundingBoxf3 bounds = source_mesh.bounding_box();
    if (!bounds.defined)
        return;
    const Vec3d source_center = (bounds.min.cast<double>() + bounds.max.cast<double>()) * 0.5;
    m_cutter_position = object->instances[instance_idx]->get_matrix() * source_center;
    m_cutter_position_initialized = true;
    ++m_cutter_revision;
}

void GLGizmoMeshBoolean::update_cutter_preview()
{
    m_cutter_preview_model.reset();
    m_cutter_preview_instance_idx = -1;
    if (m_operation_mode != MeshBooleanOperation::Difference || m_cutter_kind == CutterKind::Mesh || m_cutter_kind == CutterKind::Brush)
        return;
    const int instance_idx = m_parent.get_selection().get_instance_idx();
    const ModelObject* object = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!object || instance_idx < 0 || size_t(instance_idx) >= object->instances.size() || !m_src.mv)
        return;
    TriangleMesh mesh = make_primitive_cutter_mesh();
    if (mesh.empty())
        return;
    m_cutter_preview_instance_idx = instance_idx;
    m_cutter_preview_instance_transform = object->instances[instance_idx]->get_matrix();
    m_cutter_preview_model = std::make_unique<GLModel>();
    m_cutter_preview_model->init_from(mesh.its);
    m_cutter_preview_model->set_color(ColorRGBA(0.0f, 0.75f, 0.68f, 0.24f));
}

void GLGizmoMeshBoolean::render_cutter_preview()
{
    if (!m_src.mv || m_cutter_kind == CutterKind::Brush)
        return;
    const int instance_idx = m_parent.get_selection().get_instance_idx();
    const ModelObject* object = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!object || instance_idx < 0 || size_t(instance_idx) >= object->instances.size())
        return;
    if (!m_cutter_preview_model || m_cutter_preview_instance_idx != instance_idx ||
        !m_cutter_preview_instance_transform.isApprox(object->instances[instance_idx]->get_matrix()))
        update_cutter_preview();
    if (!m_cutter_preview_model)
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (!shader)
        return;
    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d model_matrix = object->instances[instance_idx]->get_matrix();
    const Transform3d view_matrix = camera.get_view_matrix();
    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
    const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) *
                                       model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
    shader->set_uniform("view_normal_matrix", view_normal_matrix);
    shader->set_uniform("emission_factor", 0.2f);
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-1.f, -1.f));
    m_cutter_preview_model->render();
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();
}

void GLGizmoMeshBoolean::render_brush_cursor()
{
    const Size canvas_size = m_parent.get_canvas_size();
    const float width = float(canvas_size.get_width());
    const float height = float(canvas_size.get_height());
    if (width <= 0.0f || height <= 0.0f)
        return;

    const Vec2d center = m_parent.get_local_mouse_position();
    const float radius = float(m_brush_radius * wxGetApp().plater()->get_camera().get_zoom());
    if (!m_brush_cursor_circle.is_initialized() || !m_brush_cursor_center.isApprox(center) ||
        std::abs(m_brush_cursor_screen_radius - radius) > EPSILON) {
        m_brush_cursor_center = center;
        m_brush_cursor_screen_radius = radius;
        m_brush_cursor_circle.reset();

        GLModel::Geometry geometry;
        constexpr unsigned int steps = 48;
        const float step_angle = 2.0f * float(PI) / float(steps);
        geometry.format = { GLModel::Geometry::EPrimitiveType::LineLoop, GLModel::Geometry::EVertexLayout::P2 };
        geometry.color = { 0.0f, 0.8f, 0.72f, 1.0f };
        geometry.reserve_vertices(steps);
        geometry.reserve_indices(steps);
        for (unsigned int i = 0; i < steps; ++i) {
            const float angle = float(i) * step_angle;
            geometry.add_vertex(Vec2f(2.0f * ((float(center.x()) + std::cos(angle) * radius) / width - 0.5f),
                                      -2.0f * ((float(center.y()) + std::sin(angle) * radius) / height - 0.5f)));
            geometry.add_index(i);
        }
        m_brush_cursor_circle.init_from(std::move(geometry));
    }

    glsafe(::glDisable(GL_DEPTH_TEST));
    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader) {
        shader->start_using();
        shader->set_uniform("view_model_matrix", Transform3d::Identity());
        shader->set_uniform("projection_matrix", Transform3d::Identity());
        m_brush_cursor_circle.set_color(m_brush_stroke_active ? ColorRGBA(1.0f, 0.35f, 0.2f, 1.0f) :
                                                                ColorRGBA(0.0f, 0.8f, 0.72f, 1.0f));
        m_brush_cursor_circle.render();
        shader->stop_using();
    }
    glsafe(::glEnable(GL_DEPTH_TEST));
}

bool GLGizmoMeshBoolean::preview_is_current() const
{
    if (!m_preview)
        return false;

    const Selection& selection = m_parent.get_selection();
    const ModelObject* object = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const int instance_idx = selection.get_instance_idx();
    if (!object || object->id() != m_preview->object_id || instance_idx < 0 ||
        size_t(instance_idx) >= object->instances.size())
        return false;

    // The selected object may have had volumes removed while the gizmo was
    // open. Check pointer membership before dereferencing the cached pointers.
    const auto source_it = std::find(object->volumes.begin(), object->volumes.end(), m_src.mv);
    if (source_it == object->volumes.end() || int(source_it - object->volumes.begin()) != m_src.volume_idx)
        return false;

    if (m_preview->operation != m_operation_mode || m_preview->source_id != m_src.mv->id() ||
        m_preview->source_mesh != m_src.mv->mesh_ptr().get() ||
        m_preview->source_config_timestamp != static_cast<const ObjectBase&>(m_src.mv->config).timestamp() ||
        m_preview->source_mmu_timestamp != m_src.mv->mmu_segmentation_facets.timestamp() ||
        m_preview->source_support_timestamp != m_src.mv->supported_facets.timestamp() ||
        m_preview->source_seam_timestamp != m_src.mv->seam_facets.timestamp() ||
        m_preview->source_fuzzy_timestamp != m_src.mv->fuzzy_skin_facets.timestamp() ||
        !m_preview->source_transform.isApprox(m_src.mv->get_matrix()))
        return false;

    if (m_preview->primitive_cutter)
        return m_operation_mode == MeshBooleanOperation::Difference && m_cutter_kind != CutterKind::Mesh &&
               m_preview->cutter_revision == m_cutter_revision &&
               m_preview->cutter_instance_idx == instance_idx &&
               m_preview->cutter_instance_transform.isApprox(object->instances[instance_idx]->get_matrix());

    const auto tool_it = std::find(object->volumes.begin(), object->volumes.end(), m_tool.mv);
    return tool_it != object->volumes.end() && int(tool_it - object->volumes.begin()) == m_tool.volume_idx && m_src.mv != m_tool.mv &&
           m_preview->tool_id == m_tool.mv->id() && m_preview->tool_mesh == m_tool.mv->mesh_ptr().get() &&
           m_preview->tool_transform.isApprox(m_tool.mv->get_matrix());
}

void GLGizmoMeshBoolean::apply_preview()
{
    if (!preview_is_current()) {
        clear_preview();
        return;
    }

    if (m_preview->empty) {
        if (m_preview->operation != MeshBooleanOperation::Difference) {
            clear_preview();
            return;
        }
        wxGetApp().plater()->take_snapshot(_u8L("Mesh Boolean"));
        ModelObject* object = m_c->selection_info()->model_object();
        const int object_idx = m_parent.get_selection().get_object_idx();
        ModelVolume* tool = m_tool.mv;
        const bool delete_tool = m_preview->delete_input;
        const auto source_it = std::find(object->volumes.begin(), object->volumes.end(), m_src.mv);
        if (source_it == object->volumes.end()) {
            clear_preview();
            return;
        }
        const int source_index = int(source_it - object->volumes.begin());
        object->delete_volume(source_index);
        m_src.reset();
        if (delete_tool) {
            const auto it = std::find(object->volumes.begin(), object->volumes.end(), tool);
            if (it != object->volumes.end())
                object->delete_volume(int(it - object->volumes.begin()));
            m_tool.reset();
        }
        wxGetApp().plater()->changed_mesh(object_idx);
        m_selecting_state = MeshBooleanSelectingState::SelectSource;
        clear_preview();
        return;
    }

    restore_preview_inputs();
    generate_new_volume(m_preview->delete_input, m_preview->mesh, std::move(m_preview->annotations));
    clear_preview();
}

void GLGizmoMeshBoolean::clear_preview()
{
    cancel_preview_work();
    restore_preview_inputs();
    m_preview.reset();
    m_preview_surfaces.clear();
    m_parent.set_as_dirty();
}

void GLGizmoMeshBoolean::hide_preview_inputs()
{
    if (!m_preview || m_preview->empty || !m_preview_hidden_volumes.empty())
        return;
    const int object_idx = m_parent.get_selection().get_object_idx();
    ModelObject* object = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!object || object_idx < 0)
        return;
    const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
    auto hide_volume = [&](int volume_idx) {
        if (volume_idx < 0 || size_t(volume_idx) >= object->volumes.size())
            return;
        const ObjectID volume_id = object->volumes[volume_idx]->id();
        for (GLVolume* volume : volumes) {
            if (!volume || volume->object_idx() != object_idx || volume->volume_idx() != volume_idx)
                continue;
            m_preview_hidden_volumes.push_back({ object_idx, volume_idx, volume->instance_idx(), volume_id, volume->is_active });
            volume->is_active = false;
        }
    };
    hide_volume(m_src.volume_idx);
    if (!m_preview->primitive_cutter && m_tool.mv != m_src.mv)
        hide_volume(m_tool.volume_idx);
}

void GLGizmoMeshBoolean::restore_preview_inputs()
{
    if (m_preview_hidden_volumes.empty())
        return;
    const Model* model = m_parent.get_selection().get_model();
    for (const HiddenPreviewVolume& hidden : m_preview_hidden_volumes) {
        const bool volume_still_exists = model && hidden.object_idx >= 0 &&
            size_t(hidden.object_idx) < model->objects.size() && hidden.volume_idx >= 0 &&
            size_t(hidden.volume_idx) < model->objects[hidden.object_idx]->volumes.size() &&
            model->objects[hidden.object_idx]->volumes[hidden.volume_idx]->id() == hidden.volume_id;
        if (!volume_still_exists)
            continue;
        for (GLVolume* volume : m_parent.get_volumes().volumes) {
            if (volume && volume->object_idx() == hidden.object_idx && volume->volume_idx() == hidden.volume_idx &&
                volume->instance_idx() == hidden.instance_idx) {
                volume->is_active = hidden.was_active;
                break;
            }
        }
    }
    m_preview_hidden_volumes.clear();
    m_parent.set_as_dirty();
}

void GLGizmoMeshBoolean::render_preview()
{
    if (!m_preview || m_preview->empty || m_preview_surfaces.empty())
        return;
    const ModelObject* object = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!object)
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("gouraud_light");
    if (!shader)
        return;
    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d view_matrix = camera.get_view_matrix();
    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("emission_factor", 0.2f);
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-1.f, -1.f));
    std::vector<int> instance_indices;
    for (const HiddenPreviewVolume& hidden : m_preview_hidden_volumes) {
        if (hidden.volume_idx == m_src.volume_idx &&
            std::find(instance_indices.begin(), instance_indices.end(), hidden.instance_idx) == instance_indices.end())
            instance_indices.push_back(hidden.instance_idx);
    }
    if (instance_indices.empty())
        instance_indices.push_back(m_parent.get_selection().get_instance_idx());
    for (int instance_idx : instance_indices) {
        if (instance_idx < 0 || size_t(instance_idx) >= object->instances.size())
            continue;
        const Transform3d model_matrix = object->instances[instance_idx]->get_matrix() *
                                         Geometry::translation_transform(m_preview->result_center);
        shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) *
                                           model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);
        for (PreviewSurface& surface : m_preview_surfaces) {
            surface.model->set_color(surface.color);
            surface.model->render();
        }
    }
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();
}

void GLGizmoMeshBoolean::on_load(cereal::BinaryInputArchive &ar)
{
    clear_preview();
    ar(m_enable, m_operation_mode, m_selecting_state, m_diff_delete_input, m_inter_delete_input, m_src, m_tool);
    ModelObject *curr_model_object = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    m_src.mv = curr_model_object == nullptr || m_src.volume_idx < 0 || size_t(m_src.volume_idx) >= curr_model_object->volumes.size() ?
        nullptr : curr_model_object->volumes[m_src.volume_idx];
    m_tool.mv = curr_model_object == nullptr || m_tool.volume_idx < 0 || size_t(m_tool.volume_idx) >= curr_model_object->volumes.size() ?
        nullptr : curr_model_object->volumes[m_tool.volume_idx];
    initialize_cutter_position();
    update_cutter_preview();
}

void GLGizmoMeshBoolean::on_save(cereal::BinaryOutputArchive &ar) const
{
    ar(m_enable, m_operation_mode, m_selecting_state, m_diff_delete_input, m_inter_delete_input, m_src, m_tool);
}

void GLGizmoMeshBoolean::generate_new_volume(bool delete_input, const TriangleMesh& mesh_result, PreviewAnnotations&& annotations) {
    ModelObject* curr_model_object = m_c && m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const int object_idx = m_parent.get_selection().get_object_idx();
    if (!curr_model_object || mesh_result.empty() || !m_src.mv || m_src.volume_idx < 0 ||
        size_t(m_src.volume_idx) >= curr_model_object->volumes.size() ||
        curr_model_object->volumes[m_src.volume_idx] != m_src.mv)
        return;

    wxGetApp().plater()->take_snapshot("Mesh Boolean");

    ModelVolume* old_volume = m_src.mv;
    ModelVolume* tool_volume = m_tool.mv;

    // generate new volume
    ModelVolume* new_volume = curr_model_object->add_volume(mesh_result);

    // assign to new_volume from old_volume
    std::string suffix;
    switch (m_operation_mode)
    {
    case MeshBooleanOperation::Union:
        suffix = "union";
        break;
    case MeshBooleanOperation::Difference:
        suffix = "difference";
        break;
    case MeshBooleanOperation::Intersection:
        suffix = "intersection";
        break;
    }
    new_volume->name = old_volume->name + " - " + suffix;
    new_volume->set_new_unique_id();
    new_volume->config.apply(old_volume->config);
    new_volume->set_type(old_volume->type());
    new_volume->set_material_id(old_volume->material_id());
    // add_volume centers the object-space result and records that center in the
    // new volume transform. Reusing the source offset here would shift the cut.
    new_volume->supported_facets.set_data(std::move(annotations.supported));
    new_volume->seam_facets.set_data(std::move(annotations.seam));
    new_volume->mmu_segmentation_facets.set_data(std::move(annotations.mmu));
    new_volume->fuzzy_skin_facets.set_data(std::move(annotations.fuzzy));
    //Vec3d translate_z = { 0,0, (new_volume->source.mesh_offset - old_volume->source.mesh_offset).z() };
    //new_volume->translate(new_volume->get_transformation().get_matrix_no_offset() * translate_z);
    //new_volume->supported_facets.assign(old_volume->supported_facets);
    //new_volume->seam_facets.assign(old_volume->seam_facets);
    //new_volume->mmu_segmentation_facets.assign(old_volume->mmu_segmentation_facets);

    // delete old_volume
    std::swap(curr_model_object->volumes[m_src.volume_idx], curr_model_object->volumes.back());
    curr_model_object->delete_volume(curr_model_object->volumes.size() - 1);

    if (delete_input) {
        std::vector<ItemForDelete> items;
        const auto tool_it = std::find(curr_model_object->volumes.begin(), curr_model_object->volumes.end(), tool_volume);
        if (tool_it != curr_model_object->volumes.end()) {
            items.emplace_back(ItemType::itVolume, object_idx, int(tool_it - curr_model_object->volumes.begin()));
            wxGetApp().obj_list()->delete_from_model_and_list(items);
        }
    }

    //bool sinking = curr_model_object->bounding_box().min.z() < SINKING_Z_THRESHOLD;
    //if (!sinking)
    //    curr_model_object->ensure_on_bed();
    //curr_model_object->sort_volumes(true);

    wxGetApp().plater()->changed_mesh(object_idx);
    wxGetApp().obj_list()->select_item([this, new_volume]() {
        wxDataViewItem sel_item;

        wxDataViewItemArray items = wxGetApp().obj_list()->reorder_volumes_and_get_selection(m_parent.get_selection().get_object_idx(), [new_volume](const ModelVolume* volume) { return volume == new_volume; });
        if (!items.IsEmpty())
            sel_item = items.front();

        return sel_item;
        });

    m_src.reset();
    m_tool.reset();
    m_selecting_state = MeshBooleanSelectingState::SelectSource;
}


}}
