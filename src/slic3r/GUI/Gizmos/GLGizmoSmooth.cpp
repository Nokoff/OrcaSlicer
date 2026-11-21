#include "GLGizmoSmooth.hpp"

#include <GL/glew.h>
#include <algorithm>
#include <cmath>
#include <limits>

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"

namespace Slic3r::GUI {

namespace {
bool point_is_in_triangle(const Vec3d& point, const Vec3d& a, const Vec3d& b, const Vec3d& c, double tolerance)
{
    const Vec3d ab = b - a;
    const Vec3d ac = c - a;
    const Vec3d ap = point - a;
    const double d00 = ab.dot(ab), d01 = ab.dot(ac), d11 = ac.dot(ac);
    const double d20 = ap.dot(ab), d21 = ap.dot(ac);
    const double denom = d00 * d11 - d01 * d01;
    if (!std::isfinite(denom) || std::abs(denom) <= 1e-24)
        return false;
    const double edge_scale = std::sqrt(std::max({d00, d11, (c - b).squaredNorm()}));
    if (!std::isfinite(edge_scale) || edge_scale <= 1e-12)
        return false;
    const double barycentric_tolerance = tolerance / edge_scale;
    const double u = (d11 * d20 - d01 * d21) / denom;
    const double v = (d00 * d21 - d01 * d20) / denom;
    const Vec3d projected = a + u * ab + v * ac;
    return u >= -barycentric_tolerance && v >= -barycentric_tolerance && u + v <= 1.0 + barycentric_tolerance &&
           (point - projected).squaredNorm() <= tolerance * tolerance;
}
} // namespace

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
        // Reuse the scene raycaster when it references the same editable mesh.
        // After a mesh edit, scene reload can lag behind the model pointer; the
        // pointer check below prevents picking stale facet IDs in that window.
        const auto mesh = volume->mesh_ptr();
        const MeshRaycaster* raycaster = nullptr;
        if (gl_volume->mesh_raycaster && gl_volume->m_oriMesh == mesh.get())
            raycaster = gl_volume->mesh_raycaster.get();
        Surface& surface = m_surfaces[volume];
        if (!raycaster) {
            if (surface.mesh != mesh) {
                surface.mesh      = mesh;
                surface.raycaster = std::make_unique<MeshRaycaster>(surface.mesh);
            }
            raycaster = surface.raycaster.get();
        }
        const Transform3d transform = gl_volume->world_matrix();
        Vec3f             point, normal;
        size_t            face;
        if (!raycaster || !raycaster->closest_hit(mouse, transform, camera, point, normal, nullptr, &face))
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

void GLGizmoSmooth::poll_brush_preparation()
{
    const auto preparation = m_preparation;
    if (!preparation || !preparation->ready.load(std::memory_order_acquire))
        return;
    try {
        if (m_brush_future.valid())
            m_brush_future.get();
        std::lock_guard<std::mutex> lock(preparation->mutex);
        if (preparation->discard.load(std::memory_order_acquire)) {
            preparation->brush.reset();
            preparation->mesh.reset();
            m_preparing_mesh.reset();
            m_preparation_failed = false;
        } else {
            m_prepared_brush = std::move(preparation->brush);
            m_preparation_failed = preparation->failed || !m_prepared_brush;
        }
    } catch (...) {
        m_preparation_failed = !preparation->discard.load(std::memory_order_acquire);
        if (preparation->discard.load(std::memory_order_acquire))
            m_preparing_mesh.reset();
    }
    m_preparation.reset();
}

void GLGizmoSmooth::discard_brush_preparation()
{
    m_prepared_brush.reset();
    m_preparation_failed = false;
    if (m_preparation) {
        m_preparation->discard.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_preparation->mutex);
        m_preparation->brush.reset();
        if (m_preparation->ready.load(std::memory_order_acquire))
            m_preparation->mesh.reset();
    }
    m_preparing_mesh.reset();
    if (!m_brush_future.valid()) {
        m_preparation.reset();
    }
}

void GLGizmoSmooth::prepare_brush(ModelVolume* volume, const Transform3d& world)
{
    if (!volume || (m_stroke && (m_brush || m_snapshot_taken)))
        return;
    poll_brush_preparation();
    const auto mesh = volume->mesh_ptr();
    const bool same_mesh = m_preparing_mesh == mesh;
    const bool same_world = m_preparing_world.matrix().isApprox(world.matrix(), 1e-12);
    if (same_mesh && same_world && m_preparing_preserve_sharp_edges == m_preserve_sharp_edges &&
        (m_prepared_brush || m_preparation_failed || m_brush_future.valid()))
        return;
    // Do not destroy an unfinished std::async future: its destructor joins.
    // Let it complete, harvest it on a later frame, then replace stale work.
    if (m_brush_future.valid())
        return;
    m_prepared_brush.reset();
    m_preparation_failed = false;
    m_preparing_mesh = mesh;
    m_preparing_world = world;
    m_preparing_preserve_sharp_edges = m_preserve_sharp_edges;
    m_preparation = std::make_shared<BrushPreparation>();
    m_preparation->mesh = mesh;
    m_preparation->world = world;
    m_preparation->preserve_sharp_edges = m_preserve_sharp_edges;
    const auto preparation = m_preparation;
    try {
        m_brush_future = std::async(std::launch::async, [preparation] {
            std::unique_ptr<MeshSmoothingBrush> brush;
            bool failed = false;
            try {
                brush = std::make_unique<MeshSmoothingBrush>(preparation->mesh->its, preparation->preserve_sharp_edges,
                                                             preparation->world);
            } catch (...) {
                failed = true;
            }
            std::lock_guard<std::mutex> lock(preparation->mutex);
            preparation->mesh.reset();
            if (!preparation->discard.load(std::memory_order_acquire))
                preparation->brush = std::move(brush);
            preparation->failed = failed;
            preparation->ready.store(true, std::memory_order_release);
        });
    } catch (...) {
        m_preparation.reset();
        m_preparation_failed = true;
    }
}

bool GLGizmoSmooth::adopt_prepared_brush(ModelVolume* volume, const Transform3d& world)
{
    poll_brush_preparation();
    if (!m_prepared_brush || !volume || m_preparing_mesh != volume->mesh_ptr() ||
        !m_preparing_world.matrix().isApprox(world.matrix(), 1e-12) ||
        m_preparing_preserve_sharp_edges != m_preserve_sharp_edges)
        return false;
    m_brush = std::move(m_prepared_brush);
    m_preparing_mesh.reset();
    return true;
}

bool GLGizmoSmooth::apply_dab(size_t facet, const Vec3f& hit, const Transform3d& world)
{
    if (!m_brush || !m_stroke_volume)
        return false;
    ModelVolume* volume = m_stroke_volume;
    indexed_triangle_set mesh = volume->mesh().its;
    m_last_dab_changed = m_brush->apply(mesh, facet, hit, world, m_radius, m_strength, m_max_displacement_mm,
                                         &m_stroke_origin_vertices);
    m_statistics = m_brush->last_statistics();
    std::optional<TriangleSelector::SavedPainting> refined_painting;
    std::vector<size_t> refined_source_face_lineage;
    if (!m_last_dab_changed && m_statistics.inside_radius == 0 && m_radius < 1.f) {
        indexed_triangle_set refined_mesh = volume->mesh().its;
        std::vector<MeshSmoothingBarycentricLineage> source_face_barycentrics;
        size_t refined_facet = facet;
        if (m_brush->refine_for_brush(refined_mesh, facet, hit, world, m_radius, refined_facet, refined_source_face_lineage,
                                      source_face_barycentrics)) {
            std::vector<Vec3f> refined_stroke_origin;
            const bool have_refined_origin = MeshSmoothingBrush::map_stroke_origin_to_refined_mesh(
                volume->mesh().its, m_stroke_origin_vertices, refined_mesh, refined_source_face_lineage, source_face_barycentrics,
                refined_stroke_origin);
            auto refined_brush = have_refined_origin ? std::make_unique<MeshSmoothingBrush>(refined_mesh, m_preserve_sharp_edges, world) : nullptr;
            if (refined_brush) {
                mesh = refined_mesh;
                std::vector<size_t> pending_face_map;
                pending_face_map.reserve(m_pending_dabs.size());
                for (const PendingDab& pending : m_pending_dabs) {
                    size_t mapped_face = refined_source_face_lineage.size();
                    const Vec3d pending_hit = pending.world * pending.hit.cast<double>();
                    for (size_t face_idx = 0; face_idx < refined_source_face_lineage.size(); ++face_idx) {
                        if (refined_source_face_lineage[face_idx] != pending.facet)
                            continue;
                        const Vec3i32& face = mesh.indices[face_idx];
                        const Vec3d a = pending.world * mesh.vertices[face[0]].cast<double>();
                        const Vec3d b = pending.world * mesh.vertices[face[1]].cast<double>();
                        const Vec3d c = pending.world * mesh.vertices[face[2]].cast<double>();
                        if (point_is_in_triangle(pending_hit, a, b, c, std::max(1e-4, double(m_radius) * 1e-3))) {
                            mapped_face = face_idx;
                            break;
                        }
                    }
                    pending_face_map.push_back(mapped_face);
                }
                if (refined_brush->apply(mesh, refined_facet, hit, world, m_radius, m_strength, m_max_displacement_mm,
                                         &refined_stroke_origin)) {
                    // Buffered mouse events still use source-mesh facet IDs.
                    // Remap them to the child triangle containing each hit so
                    // a local refinement cannot redirect later dabs elsewhere.
                    size_t pending_idx = 0;
                    for (auto pending = m_pending_dabs.begin(); pending != m_pending_dabs.end();) {
                        const size_t mapped_face = pending_face_map[pending_idx++];
                        if (mapped_face == refined_source_face_lineage.size())
                            pending = m_pending_dabs.erase(pending);
                        else {
                            pending->facet = mapped_face;
                            ++pending;
                        }
                    }
                    refined_painting = volume->save_painting();
                    m_brush = std::move(refined_brush);
                    m_stroke_origin_vertices = std::move(refined_stroke_origin);
                    m_statistics = m_brush->last_statistics();
                    m_statistics.local_refinement = true;
                    m_last_dab_changed = true;
                }
            }
        }
    }
    m_feedback = m_last_dab_changed ? Feedback::Changed : Feedback::Unchanged;
    if (!m_last_dab_changed)
        return false;
    if (!m_snapshot_taken) {
        wxGetApp().plater()->take_snapshot("Smooth brush stroke");
        m_snapshot_taken = true;
    }
    // Only coordinates change. In particular, do NOT call clear_before_change_mesh:
    // normal strokes retain annotation bitstreams exactly. Fine strokes first
    // remap annotations onto the locally refined source-face descendants.
    volume->set_mesh(std::move(mesh));
    if (refined_painting)
        volume->restore_painting(refined_painting, false, nullptr, &refined_source_face_lineage);
    volume->calculate_convex_hull();
    volume->invalidate_convex_hull_2d();
    volume->set_new_unique_id();
    volume->get_object()->invalidate_bounding_box();
    m_parent.reload_scene(false);
    m_surfaces.erase(volume);
    m_parent.set_as_dirty();
    return true;
}

void GLGizmoSmooth::process_pending_dab()
{
    if (!m_brush || m_pending_dabs.empty())
        return;
    const PendingDab dab = m_pending_dabs.front();
    m_pending_dabs.pop_front();
    apply_dab(dab.facet, dab.hit, dab.world);
    if (m_finish_when_ready && m_pending_dabs.empty())
        finish_stroke();
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
    m_stroke = m_snapshot_taken = false;
    m_stroke_object             = nullptr;
    m_stroke_volume             = nullptr;
    m_stroke_origin_vertices.clear();
    m_brush.reset();
    m_pending_dabs.clear();
    m_finish_when_ready = false;
    if (changed && object_idx >= 0)
        wxGetApp().plater()->changed_mesh(object_idx);
}

void GLGizmoSmooth::on_set_state()
{
    if (m_state == Off) {
        finish_stroke();
        discard_brush_preparation();
        m_surfaces.clear();
        m_cursor.reset();
    } else if (ModelVolume* volume = selected_volume()) {
        const Selection& selection = m_parent.get_selection();
        const auto& objects = wxGetApp().model().objects;
        for (unsigned idx : selection.get_volume_idxs()) {
            const GLVolume* gl_volume = selection.get_volume(idx);
            const int object = gl_volume->object_idx(), volume_idx = gl_volume->volume_idx();
            if (object < 0 || size_t(object) >= objects.size() || volume_idx < 0 ||
                size_t(volume_idx) >= objects[object]->volumes.size())
                continue;
            if (objects[object]->volumes[volume_idx] == volume) {
                prepare_brush(volume, gl_volume->world_matrix());
                break;
            }
        }
    }
    m_parent.set_as_dirty();
}

void GLGizmoSmooth::data_changed(bool is_serializing)
{
    m_surfaces.clear();
    if (is_serializing) {
        m_feedback = Feedback::Restored;
        // Undo/redo has already restored the model; discard transient stroke data.
        m_stroke = m_snapshot_taken = false;
        m_stroke_object             = nullptr;
        m_stroke_volume             = nullptr;
        m_stroke_origin_vertices.clear();
        m_brush.reset();
        m_pending_dabs.clear();
        m_finish_when_ready = false;
        discard_brush_preparation();
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
        poll_brush_preparation();
        if (!m_brush && !m_pending_dabs.empty())
            adopt_prepared_brush(m_stroke_volume, m_pending_dabs.front().world);
        if (!m_pending_dabs.empty() && !m_brush) {
            m_finish_when_ready = true;
            m_feedback = Feedback::Preparing;
            m_parent.set_as_dirty();
            return true;
        }
        if (!m_pending_dabs.empty()) {
            process_pending_dab();
            if (!m_pending_dabs.empty()) {
                m_finish_when_ready = true;
                return true;
            }
        }
        finish_stroke();
        return true;
    }
    if (m_stroke && !event.LeftIsDown() && !m_finish_when_ready)
        finish_stroke();
    // Match the painting tools' explicit camera-navigation modifier.
    if (event.CmdDown()) {
        finish_stroke();
        return false;
    }
    if (m_finish_when_ready)
        return true;
    if (!event.LeftDown() && !event.LeftDClick() && !(event.Dragging() && event.LeftIsDown()))
        return false;
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
        return true;
    }
    if (m_stroke && volume != m_stroke_volume)
        return true; // Keep one part and one Undo snapshot per stroke.
    if (!m_stroke) {
        prepare_brush(volume, world);
        m_stroke        = true;
        m_stroke_object = volume->get_object();
        m_stroke_volume = volume;
        m_stroke_origin_vertices = volume->mesh().its.vertices;
    }
    poll_brush_preparation();
    if (!m_brush)
        adopt_prepared_brush(volume, world);
    if (!m_brush) {
        if (m_preparation_failed) {
            m_feedback = Feedback::PreparationFailed;
            finish_stroke();
            return true;
        }
        prepare_brush(volume, world);
        if (m_pending_dabs.size() < 256)
            m_pending_dabs.push_back({facet, hit, world});
        m_last_dab = now;
        m_feedback = Feedback::Preparing;
        m_parent.set_as_dirty();
        return true;
    }
    if (!m_pending_dabs.empty()) {
        if (m_pending_dabs.size() < 256)
            m_pending_dabs.push_back({facet, hit, world});
        process_pending_dab();
        m_last_dab = now;
        return true;
    }
    m_last_dab = now;
    apply_dab(facet, hit, world);
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
    m_imgui->text(_L("Edits affect every linked instance of this part."));
    if (m_feedback == Feedback::Preparing)
        m_imgui->text(_L("Preparing the smoothing brush. Your stroke will continue when it is ready."));
    else if (m_feedback == Feedback::PreparationFailed)
        m_imgui->text(_L("The smoothing brush could not be prepared. No mesh changes were made."));
    else if (m_feedback == Feedback::Miss)
        m_imgui->text(_L("The brush missed the selected surface. Move the green ring onto the model."));
    else if (m_feedback == Feedback::Unchanged && m_statistics.rejected_clearance > 0)
        m_imgui->text(_L("Nearby geometry blocked this motion to protect a thin wall or surface."));
    else if (m_feedback == Feedback::Unchanged && m_statistics.movable == 0)
        m_imgui->text(_L("Mesh resolution or a fixed boundary prevented this edit. Try a larger brush or a finer mesh."));
    else if (m_feedback == Feedback::Unchanged)
        m_imgui->text(_L("No vertices moved. Try a different area or increase strength."));
    else if (m_feedback == Feedback::Changed && m_statistics.local_refinement) {
        m_imgui->text(_L("Refined the local mesh and transferred its paint annotations."));
        ImGui::Text(_u8L("Last dab: %zu vertices moved, up to %.5f mm.").c_str(), m_statistics.moved, m_statistics.max_displacement_mm);
    } else if (m_feedback == Feedback::Changed)
        ImGui::Text(_u8L("Last dab: %zu vertices moved, up to %.5f mm.").c_str(), m_statistics.moved, m_statistics.max_displacement_mm);
    else if (m_feedback == Feedback::Restored)
        m_imgui->text(_L("Undo/Redo restored the model."));
    float brush_diameter = 2.f * m_radius;
    ImGui::SetNextItemWidth(m_imgui->scaled(170.f));
    if (ImGui::InputFloat(_u8L("Brush diameter (mm)").c_str(), &brush_diameter, 0.01f, 0.1f, "%.2f"))
        m_radius = std::max(0.025f, brush_diameter * 0.5f);
    m_radius = std::max(0.025f, std::min(30.f, m_radius));
    brush_diameter = 2.f * m_radius;
    ImGui::SliderFloat(_u8L("Strength").c_str(), &m_strength, 0.01f, 1.f, "%.2f");
    m_imgui->disabled_begin(m_stroke);
    ImGui::Checkbox(_u8L("Preserve sharp edges (>60°)").c_str(), &m_preserve_sharp_edges);
    m_imgui->disabled_end();
    ImGui::SetNextItemWidth(m_imgui->scaled(170.f));
    ImGui::InputFloat(_u8L("Max displacement (mm)").c_str(), &m_max_displacement_mm, 0.01f, 0.1f, "%.2f");
    m_max_displacement_mm = std::max(0.01f, m_max_displacement_mm);
    m_imgui->disabled_begin(m_stroke || !wxGetApp().plater()->can_undo());
    if (m_imgui->button(_L("Undo")))
        wxGetApp().CallAfter([] { wxGetApp().plater()->undo(); });
    m_imgui->disabled_end();
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoSmooth::on_render()
{
    poll_brush_preparation();
    bool replayed_dab = false;
    if (m_stroke && !m_pending_dabs.empty()) {
        if (!m_brush)
            adopt_prepared_brush(m_stroke_volume, m_pending_dabs.front().world);
        if (m_brush) {
            process_pending_dab();
            replayed_dab = true;
        } else if (m_preparation_failed) {
            m_feedback = Feedback::PreparationFailed;
            finish_stroke();
        } else {
            prepare_brush(m_stroke_volume, m_pending_dabs.front().world);
            m_parent.set_as_dirty();
        }
    }
    const Vec2d mouse = m_parent.get_local_mouse_position();
    const Size  size  = m_parent.get_canvas_size();
    if (size.get_width() <= 0 || size.get_height() <= 0 || mouse.x() < 0 || mouse.y() < 0 || mouse.x() >= size.get_width() ||
        mouse.y() >= size.get_height())
        return;
    Transform3d   world;
    Vec3f         hit;
    size_t        facet;
    ModelVolume*  hovered_volume = replayed_dab ? nullptr : pick_surface(mouse, world, hit, facet);
    const bool    on_surface = hovered_volume != nullptr;
    if (!m_stroke && hovered_volume)
        prepare_brush(hovered_volume, world);
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
    if (m_feedback == Feedback::Preparing)
        geometry.color = ColorRGBA(1.f, 0.68f, 0.12f, 1.f);
    else if (m_feedback == Feedback::PreparationFailed)
        geometry.color = ColorRGBA(1.f, 0.2f, 0.2f, 1.f);
    else
        geometry.color = on_surface ? ColorRGBA(0.15f, 1.f, 0.55f, 1.f) : ColorRGBA(0.65f, 0.65f, 0.65f, 1.f);
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
