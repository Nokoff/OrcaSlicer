
#include "CutUtils.hpp"
#include "Geometry.hpp"
#include "libslic3r.h"
#include "Model.hpp"
#include "TriangleMeshSlicer.hpp"
#include "TriangleSelector.hpp"
#include "ObjectID.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

using namespace Geometry;

static void apply_tolerance(ModelVolume* vol)
{
    ModelVolume::CutInfo& cut_info = vol->cut_info;

    assert(cut_info.is_connector);
    if (!cut_info.is_processed)
        return;

    Vec3d sf = vol->get_scaling_factor();

    // make a "hole" wider
    sf[X] += double(cut_info.radius_tolerance);
    sf[Y] += double(cut_info.radius_tolerance);

    // make a "hole" dipper
    sf[Z] += double(cut_info.height_tolerance);

    vol->set_scaling_factor(sf);

    // correct offset in respect to the new depth
    Vec3d rot_norm = rotation_transform(vol->get_rotation()) * Vec3d::UnitZ();
    if (rot_norm.norm() != 0.0)
        rot_norm.normalize();

    double z_offset = 0.5 * static_cast<double>(cut_info.height_tolerance);
    if (cut_info.connector_type == CutConnectorType::Plug || 
        cut_info.connector_type == CutConnectorType::Snap)
        z_offset -= 0.05; // add small Z offset to better preview

    vol->set_offset(vol->get_offset() + rot_norm * z_offset);
}

static void add_cut_volume(TriangleMesh& mesh, ModelObject* object, const ModelVolume* src_volume, const Transform3d& cut_matrix,
                           const std::string& suffix = {}, ModelVolumeType type = ModelVolumeType::MODEL_PART,
                           const std::vector<uint8_t>* cap_faces = nullptr)
{
    if (mesh.empty())
        return;

    mesh.transform(cut_matrix);
    ModelVolume* vol = object->add_volume(mesh);
    vol->set_type(type);

    vol->name = src_volume->name + suffix;
    // Don't copy the config's ID.
    vol->config.assign_config(src_volume->config);
    assert(vol->config.id().valid());
    assert(vol->config.id() != src_volume->config.id());
    if (const ModelMaterial* material = src_volume->material())
        vol->set_material(src_volume->material_id(), *material);
    else if (!src_volume->material_id().empty())
        vol->set_material_id(src_volume->material_id());
    vol->cut_info = src_volume->cut_info;
    vol->cut_info.cap_faces = cap_faces ? *cap_faces : std::vector<uint8_t>();
}

static void process_volume_cut( ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                                ModelObjectCutAttributes attributes, TriangleMesh& upper_mesh, TriangleMesh& lower_mesh,
                                std::vector<uint8_t>* upper_cap_faces = nullptr, std::vector<uint8_t>* lower_cap_faces = nullptr)
{
    const auto volume_matrix = volume->get_matrix();

    const Transformation cut_transformation = Transformation(cut_matrix);
    const Transform3d invert_cut_matrix = cut_transformation.get_rotation_matrix().inverse() * translation_transform(-1 * cut_transformation.get_offset());

    // Transform the mesh by the combined transformation matrix.
    // Flip the triangles in case the composite transformation is left handed.
    TriangleMesh mesh(volume->mesh());
    mesh.transform(invert_cut_matrix * instance_matrix * volume_matrix, true);

    indexed_triangle_set upper_its, lower_its;
    cut_mesh(mesh.its, 0.0f, &upper_its, &lower_its, true, upper_cap_faces, lower_cap_faces);
    if (attributes.has(ModelObjectCutAttribute::KeepUpper))
        upper_mesh = TriangleMesh(upper_its);
    if (attributes.has(ModelObjectCutAttribute::KeepLower))
        lower_mesh = TriangleMesh(lower_its);
}

static void process_connector_cut(  ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                                    ModelObjectCutAttributes attributes, ModelObject* upper, ModelObject* lower,
                                    std::vector<ModelObject*>& dowels)
{
    assert(volume->cut_info.is_connector);
    volume->cut_info.set_processed();

    const auto volume_matrix = volume->get_matrix();

    // ! Don't apply instance transformation for the conntectors.
    // This transformation is already there
    if (volume->cut_info.connector_type != CutConnectorType::Dowel) {
        if (attributes.has(ModelObjectCutAttribute::KeepUpper)) {
            ModelVolume* vol = nullptr;
            if (volume->cut_info.connector_type == CutConnectorType::Snap) {
                TriangleMesh mesh = TriangleMesh(its_make_cylinder(1.0, 1.0, PI / 180.));

                vol = upper->add_volume(std::move(mesh));
                vol->set_transformation(volume->get_transformation());
                vol->set_type(ModelVolumeType::NEGATIVE_VOLUME);

                vol->cut_info = volume->cut_info;
                vol->name = volume->name;
            }
            else
                vol = upper->add_volume(*volume);

            vol->set_transformation(volume_matrix);
            apply_tolerance(vol);
        }
        if (attributes.has(ModelObjectCutAttribute::KeepLower)) {
            ModelVolume* vol = lower->add_volume(*volume);
            vol->set_transformation(volume_matrix);
            // for lower part change type of connector from NEGATIVE_VOLUME to MODEL_PART if this connector is a plug
            vol->set_type(ModelVolumeType::MODEL_PART);
        }
    }
    else {
        if (attributes.has(ModelObjectCutAttribute::CreateDowels)) {
            ModelObject* dowel{ nullptr };
            // Clone the object to duplicate instances, materials etc.
            volume->get_object()->clone_for_cut(&dowel);

            // add one more solid part same as connector if this connector is a dowel
            ModelVolume* vol = dowel->add_volume(*volume);
            vol->set_type(ModelVolumeType::MODEL_PART);

            // But discard rotation and Z-offset for this volume
            vol->set_rotation(Vec3d::Zero());
            vol->set_offset(Z, 0.0);

            dowels.push_back(dowel);
        }

        // Cut the dowel
        apply_tolerance(volume);

        // Perform cut
        TriangleMesh upper_mesh, lower_mesh;
        process_volume_cut(volume, Transform3d::Identity(), cut_matrix, attributes, upper_mesh, lower_mesh);

        // add small Z offset to better preview
        upper_mesh.translate((-0.05 * Vec3d::UnitZ()).cast<float>());
        lower_mesh.translate((0.05 * Vec3d::UnitZ()).cast<float>());

        // Add cut parts to the related objects
        add_cut_volume(upper_mesh, upper, volume, cut_matrix, "_A", volume->type());
        add_cut_volume(lower_mesh, lower, volume, cut_matrix, "_B", volume->type());
    }
}

static void process_modifier_cut(ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& inverse_cut_matrix,
                                 ModelObjectCutAttributes attributes, ModelObject* upper, ModelObject* lower)
{
    const auto volume_matrix = instance_matrix * volume->get_matrix();

    // Modifiers are not cut, but we still need to add the instance transformation
    // to the modifier volume transformation to preserve their shape properly.
    volume->set_transformation(Transformation(volume_matrix));

    if (attributes.has(ModelObjectCutAttribute::KeepAsParts)) {
        upper->add_volume(*volume);
        return;
    }

    // Some logic for the negative volumes/connectors. Add only needed modifiers
    auto bb = volume->mesh().transformed_bounding_box(inverse_cut_matrix * volume_matrix);
    bool is_crossed_by_cut = bb.min[Z] <= 0 && bb.max[Z] >= 0;
    if (attributes.has(ModelObjectCutAttribute::KeepUpper) && (bb.min[Z] >= 0 || is_crossed_by_cut))
        upper->add_volume(*volume);
    if (attributes.has(ModelObjectCutAttribute::KeepLower) && (bb.max[Z] <= 0 || is_crossed_by_cut))
        lower->add_volume(*volume);
}

static void process_solid_part_cut(ModelVolume* volume, const Transform3d& instance_matrix, const Transform3d& cut_matrix,
                            ModelObjectCutAttributes attributes, ModelObject* upper, ModelObject* lower)
{
    // Perform cut
    TriangleMesh upper_mesh, lower_mesh;
    std::vector<uint8_t> upper_cap_faces, lower_cap_faces;
    process_volume_cut(volume, instance_matrix, cut_matrix, attributes, upper_mesh, lower_mesh, &upper_cap_faces, &lower_cap_faces);

    // Add required cut parts to the objects

    if (attributes.has(ModelObjectCutAttribute::KeepAsParts)) {
        add_cut_volume(upper_mesh, upper, volume, cut_matrix, "_A", ModelVolumeType::MODEL_PART, &upper_cap_faces);
        if (!lower_mesh.empty()) {
            add_cut_volume(lower_mesh, upper, volume, cut_matrix, "_B", ModelVolumeType::MODEL_PART, &lower_cap_faces);
            upper->volumes.back()->cut_info.is_from_upper = false;
        }
        return;
    }

    if (attributes.has(ModelObjectCutAttribute::KeepUpper))
        add_cut_volume(upper_mesh, upper, volume, cut_matrix, {}, ModelVolumeType::MODEL_PART, &upper_cap_faces);

    if (attributes.has(ModelObjectCutAttribute::KeepLower) && !lower_mesh.empty())
        add_cut_volume(lower_mesh, lower, volume, cut_matrix, {}, ModelVolumeType::MODEL_PART, &lower_cap_faces);
}

static void reset_instance_transformation(ModelObject* object, size_t src_instance_idx, 
                                          const Transform3d& cut_matrix = Transform3d::Identity(),
                                          bool place_on_cut = false, bool flip = false)
{
    // Reset instance transformation except offset and Z-rotation

    for (size_t i = 0; i < object->instances.size(); ++i) {
        auto& obj_instance = object->instances[i];
        const double rot_z = obj_instance->get_rotation().z();
        
        Transformation inst_trafo = Transformation(obj_instance->get_transformation().get_matrix_no_scaling_factor());
        // add respect to mirroring
        if (obj_instance->is_left_handed())
            inst_trafo = inst_trafo * Transformation(scale_transform(Vec3d(-1, 1, 1)));

        obj_instance->set_transformation(inst_trafo);

        Vec3d rotation = Vec3d::Zero();
        if (!flip && !place_on_cut) {
            if ( i != src_instance_idx)
            rotation[Z] = rot_z;
        }
        else {
            Transform3d rotation_matrix = Transform3d::Identity();
            if (flip)
                rotation_matrix = rotation_transform(PI * Vec3d::UnitX());

            if (place_on_cut)
                rotation_matrix = rotation_matrix * Transformation(cut_matrix).get_rotation_matrix().inverse();

            if (i != src_instance_idx)
                rotation_matrix = rotation_transform(rot_z * Vec3d::UnitZ()) * rotation_matrix;

            rotation = Transformation(rotation_matrix).get_rotation();
        }

        obj_instance->set_rotation(rotation);
    }
}


Cut::Cut(const ModelObject* object, int instance, const Transform3d& cut_matrix,
         ModelObjectCutAttributes attributes/*= ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepAsParts*/)
    : m_instance(instance), m_cut_matrix(cut_matrix), m_attributes(attributes)
{
    m_model = Model();
    if (object) {
        m_model.add_object(*object);
        ModelObject* cut_object = m_model.objects.front();
        const size_t volume_count = std::min(object->volumes.size(), cut_object->volumes.size());
        for (size_t volume_idx = 0; volume_idx < volume_count; ++volume_idx)
            if (cut_object->volumes[volume_idx]->cut_info.painting_source_ids.empty())
                cut_object->volumes[volume_idx]->cut_info.painting_source_ids = {object->volumes[volume_idx]->id()};
    }
}

void Cut::post_process(ModelObject* object, ModelObjectPtrs& cut_object_ptrs, bool keep, bool place_on_cut, bool flip)
{
    if (!object) return;

    if (keep && !object->volumes.empty()) {
        reset_instance_transformation(object, m_instance, m_cut_matrix, place_on_cut, flip);
        cut_object_ptrs.push_back(object);
    }
    else
        m_model.objects.push_back(object); // will be deleted in m_model.clear_objects();
}

void Cut::post_process(ModelObject* upper, ModelObject* lower, ModelObjectPtrs& cut_object_ptrs)
{
    post_process(upper, cut_object_ptrs,
        m_attributes.has(ModelObjectCutAttribute::KeepUpper),
        m_attributes.has(ModelObjectCutAttribute::PlaceOnCutUpper),
        m_attributes.has(ModelObjectCutAttribute::FlipUpper));

    post_process(lower, cut_object_ptrs,
        m_attributes.has(ModelObjectCutAttribute::KeepLower),
        m_attributes.has(ModelObjectCutAttribute::PlaceOnCutLower),
        m_attributes.has(ModelObjectCutAttribute::PlaceOnCutLower) || m_attributes.has(ModelObjectCutAttribute::FlipLower));
}


void Cut::finalize(const ModelObjectPtrs& objects, const std::vector<SavedVolumePainting>& saved_paintings)
{
    // Restrict restoration to descendants of the source volume. Coincident parts
    // must never donate paint to one another through a global nearest-surface map.
    for (const SavedVolumePainting& saved : saved_paintings) {
        if (!saved.painting)
            continue;
        for (ModelObject* object : objects)
            for (ModelVolume* volume : object->volumes)
                if (volume->is_model_part() && !volume->is_cut_connector() &&
                    std::find(volume->cut_info.painting_source_ids.begin(), volume->cut_info.painting_source_ids.end(),
                              saved.source_volume_id) != volume->cut_info.painting_source_ids.end())
                    volume->restore_painting(saved.painting, true);
    }

    const EnforcerBlockerType cap_state = m_new_face_filament > 0 ? static_cast<EnforcerBlockerType>(m_new_face_filament) :
                                                                    EnforcerBlockerType::NONE;
    for (ModelObject* object : objects)
        for (ModelVolume* volume : object->volumes) {
            const std::vector<uint8_t>& cap_faces = volume->cut_info.cap_faces;
            if (cap_faces.empty() || std::none_of(cap_faces.begin(), cap_faces.end(), [](uint8_t is_cap) { return is_cap != 0; }))
                continue;
            const auto update_annotation = [&](FacetsAnnotation& annotation, EnforcerBlockerType state) {
                TriangleSelector selector(volume->mesh());
                if (!annotation.empty())
                    selector.deserialize(annotation.get_data(), false, EnforcerBlockerType::ExtruderMax);
                for (size_t face = 0; face < std::min(cap_faces.size(), volume->mesh().its.indices.size()); ++face)
                    if (cap_faces[face])
                        selector.set_facet(static_cast<int>(face), state);
                annotation.set(selector);
            };
            update_annotation(volume->mmu_segmentation_facets, cap_state);
            update_annotation(volume->supported_facets, EnforcerBlockerType::NONE);
            update_annotation(volume->seam_facets, EnforcerBlockerType::NONE);
            update_annotation(volume->fuzzy_skin_facets, EnforcerBlockerType::NONE);
        }

    if (!saved_paintings.empty())
        for (ModelObject* object : objects)
            for (ModelVolume* volume : object->volumes)
                volume->cut_info.painting_source_ids.clear();
    for (ModelObject* object : objects)
        for (ModelVolume* volume : object->volumes)
            volume->cut_info.cap_faces.clear();

    // Clear model from temporary objects.
    m_model.clear_objects();

    // add to model result objects
    m_model.objects = objects;
}


const ModelObjectPtrs& Cut::perform_with_plane()
{
    if (!m_attributes.has(ModelObjectCutAttribute::KeepUpper) && !m_attributes.has(ModelObjectCutAttribute::KeepLower)) {
        m_model.clear_objects();
        return m_model.objects;
    }

    ModelObject* mo = m_model.objects.front();

    BOOST_LOG_TRIVIAL(trace) << "ModelObject::cut - start";

    // Clone the object to duplicate instances, materials etc.
    ModelObject* upper{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepUpper))
        mo->clone_for_cut(&upper);

    ModelObject* lower{ nullptr };
    if (m_attributes.has(ModelObjectCutAttribute::KeepLower) && !m_attributes.has(ModelObjectCutAttribute::KeepAsParts))
        mo->clone_for_cut(&lower);

    std::vector<ModelObject*> dowels;

    // Because transformations are going to be applied to meshes directly,
    // we reset transformation of all instances and volumes,
    // except for translation and Z-rotation on instances, which are preserved
    // in the transformation matrix and not applied to the mesh transform.

    const auto              instance_matrix = mo->instances[m_instance]->get_transformation().get_matrix_no_offset();
    const Transformation    cut_transformation = Transformation(m_cut_matrix);
    const Transform3d       inverse_cut_matrix = cut_transformation.get_rotation_matrix().inverse() * translation_transform(-1. * cut_transformation.get_offset());

    std::vector<SavedVolumePainting> saved_paintings;
    for (ModelVolume* volume : mo->volumes) {
        if (m_attributes.has(ModelObjectCutAttribute::KeepPaint) || volume->cut_info.painting_source_ids.empty())
            volume->cut_info.painting_source_ids = {volume->id()};
        if (m_attributes.has(ModelObjectCutAttribute::KeepPaint)) {
            saved_paintings.push_back({volume->id(), volume->save_painting()});
            if (saved_paintings.back().painting)
                saved_paintings.back().painting->transform = instance_matrix * volume->get_matrix();
        }

        volume->reset_extra_facets();

        if (!volume->is_model_part()) {
            if (volume->cut_info.is_processed)
                process_modifier_cut(volume, instance_matrix, inverse_cut_matrix, m_attributes, upper, lower);
            else
                process_connector_cut(volume, instance_matrix, m_cut_matrix, m_attributes, upper, lower, dowels);
        }
        else if (!volume->mesh().empty())
            process_solid_part_cut(volume, instance_matrix, m_cut_matrix, m_attributes, upper, lower);
    }

    // Post-process cut parts

    if (m_attributes.has(ModelObjectCutAttribute::KeepAsParts) && upper->volumes.empty()) {
        m_model = Model();
        m_model.objects.push_back(upper);
        return m_model.objects;
    }

    ModelObjectPtrs cut_object_ptrs;

    if (m_attributes.has(ModelObjectCutAttribute::KeepAsParts) && !upper->volumes.empty()) {
        reset_instance_transformation(upper, m_instance, m_cut_matrix);
        cut_object_ptrs.push_back(upper);
    }
    else {
        // Delete all modifiers which are not intersecting with solid parts bounding box
        auto delete_extra_modifiers = [this](ModelObject* mo) {
            if (!mo) return;
            const BoundingBoxf3 obj_bb = mo->instance_bounding_box(m_instance);
            const Transform3d inst_matrix = mo->instances[m_instance]->get_transformation().get_matrix();

            for (int i = int(mo->volumes.size()) - 1; i >= 0; --i)
                if (const ModelVolume* vol = mo->volumes[i];
                    !vol->is_model_part() && !vol->is_cut_connector()) {
                    auto bb = vol->mesh().transformed_bounding_box(inst_matrix * vol->get_matrix());
                    if (!obj_bb.intersects(bb))
                        mo->delete_volume(i);
                }
        };

        post_process(upper, lower, cut_object_ptrs);
        delete_extra_modifiers(upper);
        delete_extra_modifiers(lower);

        if (m_attributes.has(ModelObjectCutAttribute::CreateDowels) && !dowels.empty()) {
            for (auto dowel : dowels) {
                reset_instance_transformation(dowel, m_instance);
                dowel->name += "-Dowel-" + dowel->volumes[0]->name;
                cut_object_ptrs.push_back(dowel);
            }
        }
    }

    BOOST_LOG_TRIVIAL(trace) << "ModelObject::cut - end";

    finalize(cut_object_ptrs, saved_paintings);

    return m_model.objects;
}

static void distribute_modifiers_from_object(ModelObject* from_obj, const int instance_idx, ModelObject* to_obj1, ModelObject* to_obj2)
{
    auto              obj1_bb = to_obj1 ? to_obj1->instance_bounding_box(instance_idx) : BoundingBoxf3();
    auto              obj2_bb = to_obj2 ? to_obj2->instance_bounding_box(instance_idx) : BoundingBoxf3();
    const Transform3d inst_matrix = from_obj->instances[instance_idx]->get_transformation().get_matrix();

    for (ModelVolume* vol : from_obj->volumes)
        if (!vol->is_model_part()) {
            // Don't add modifiers which are processed connectors
            if (vol->cut_info.is_connector && !vol->cut_info.is_processed)
                continue;
            auto bb = vol->mesh().transformed_bounding_box(inst_matrix * vol->get_matrix());
            // Don't add modifiers which are not intersecting with solid parts
            if (obj1_bb.intersects(bb))
                to_obj1->add_volume(*vol);
            if (obj2_bb.intersects(bb))
                to_obj2->add_volume(*vol);
        }
}

static void merge_solid_parts_inside_object(ModelObjectPtrs& objects)
{
    for (ModelObject* mo : objects) {
        struct MergeGroup
        {
            const ModelVolume* source;
            TriangleMesh       mesh;
            std::vector<ObjectID> source_ids;
            std::vector<uint8_t> cap_faces;
        };
        std::vector<MergeGroup> groups;
        const size_t            original_volume_count = mo->volumes.size();
        // Parts with different filament or material assignments must stay
        // separate so their original surfaces and new cut caps keep their color.
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part() || mv->is_cut_connector() || mv->mesh().empty())
                continue;

            TriangleMesh mesh = mv->mesh();
            mesh.transform(mv->get_matrix());
            auto group = std::find_if(groups.begin(), groups.end(), [&](const MergeGroup& candidate) {
                if (candidate.source->config.get() != mv->config.get() || candidate.source->material_id() != mv->material_id())
                    return false;
                const bool same_owner = std::any_of(mv->cut_info.painting_source_ids.begin(), mv->cut_info.painting_source_ids.end(),
                                                   [&](ObjectID id) {
                    return std::find(candidate.source_ids.begin(), candidate.source_ids.end(), id) != candidate.source_ids.end();
                });
                // Keep overlapping source volumes separate so their annotations cannot
                // compete on coincident output faces. Disjoint volumes may still merge.
                return same_owner || !candidate.mesh.bounding_box().intersects(mesh.bounding_box());
            });
            if (group == groups.end()) {
                groups.push_back({mv, TriangleMesh(), mv->cut_info.painting_source_ids, {}});
                group = std::prev(groups.end());
            } else {
                for (ObjectID id : mv->cut_info.painting_source_ids)
                    if (std::find(group->source_ids.begin(), group->source_ids.end(), id) == group->source_ids.end())
                        group->source_ids.push_back(id);
            }
            if (mv->cut_info.cap_faces.size() == mv->mesh().its.indices.size())
                group->cap_faces.insert(group->cap_faces.end(), mv->cut_info.cap_faces.begin(), mv->cut_info.cap_faces.end());
            else
                group->cap_faces.insert(group->cap_faces.end(), mv->mesh().its.indices.size(), 0);
            group->mesh.merge(mesh);
        }
        if (groups.empty())
            continue;

        // Add every replacement before deleting originals. Deleting down to one
        // volume folds its transform into the instances and must happen last.
        for (MergeGroup& group : groups) {
            ModelVolume* new_volume = mo->add_volume(std::move(group.mesh));
            new_volume->name        = groups.size() == 1 ? mo->name : group.source->name;
            new_volume->config.assign_config(group.source->config);
            new_volume->cut_info = group.source->cut_info;
            new_volume->cut_info.painting_source_ids = group.source_ids;
            new_volume->cut_info.cap_faces = group.cap_faces;
            assert(new_volume->config.id() != group.source->config.id());
            if (const ModelMaterial* material = group.source->material())
                new_volume->set_material(group.source->material_id(), *material);
            else if (!group.source->material_id().empty())
                new_volume->set_material_id(group.source->material_id());
        }
        for (size_t i = original_volume_count; i > 0; --i) {
            const ModelVolume* mv = mo->volumes[i - 1];
            if (mv->is_model_part() && !mv->is_cut_connector())
                mo->delete_volume(i - 1);
        }
        // Ensuring that volumes start with solid parts for proper slicing
        mo->sort_volumes(true);
    }
}

const ModelObjectPtrs& Cut::perform_by_contour(const ModelObject* src_object, std::vector<Part> parts, int dowels_count)
{
    ModelObject* cut_mo = m_model.objects.front();

    // Clone the object to duplicate instances, materials etc.
    ModelObject* upper{nullptr};
    if (m_attributes.has(ModelObjectCutAttribute::KeepUpper))
        cut_mo->clone_for_cut(&upper);
    ModelObject* lower{nullptr};
    if (m_attributes.has(ModelObjectCutAttribute::KeepLower))
        cut_mo->clone_for_cut(&lower);

    if (upper && lower) {
        upper->name = upper->name + "_A";
        lower->name = lower->name + "_B";
    }

    // Part selection uses a temporary, already-split object. Preserve paint from
    // the original object so cuts with connectors can restore it after merging.
    std::vector<SavedVolumePainting> saved_paintings;
    if (m_attributes.has(ModelObjectCutAttribute::KeepPaint)) {
        const Transform3d instance_matrix = src_object->instances[m_instance]->get_transformation().get_matrix_no_offset();
        const size_t source_count = std::min(src_object->volumes.size(), cut_mo->volumes.size());
        for (size_t id = 0; id < source_count; ++id)
            if (cut_mo->volumes[id]->cut_info.painting_source_ids.empty())
                cut_mo->volumes[id]->cut_info.painting_source_ids = {src_object->volumes[id]->id()};
        for (const ModelVolume* volume : src_object->volumes) {
            saved_paintings.push_back({volume->id(), volume->save_painting()});
            if (saved_paintings.back().painting)
                saved_paintings.back().painting->transform = instance_matrix * volume->get_matrix();
        }
    }

    const size_t cut_parts_cnt = parts.size();
    bool has_modifiers = false;

    // Distribute SolidParts to the Upper/Lower object
    for (size_t id = 0; id < cut_parts_cnt; ++id) {
        if (parts[id].is_modifier)
            has_modifiers = true; // modifiers will be added later to the related parts
        else if (ModelObject* obj = (parts[id].selected ? upper : lower))
            obj->add_volume(*(cut_mo->volumes[id]));
    }

    if (has_modifiers) {
        // Distribute Modifiers to the Upper/Lower object
        distribute_modifiers_from_object(cut_mo, m_instance, upper, lower);
    }

    ModelObjectPtrs cut_object_ptrs;

    ModelVolumePtrs& volumes = cut_mo->volumes;
    if (volumes.size() == cut_parts_cnt) {
        // Means that object is cut without connectors

        // Just add Upper and Lower objects to cut_object_ptrs
        post_process(upper, lower, cut_object_ptrs);

        // Now merge all model parts together:
        merge_solid_parts_inside_object(cut_object_ptrs);

        // replace initial objects in model with cut object 
        finalize(cut_object_ptrs, saved_paintings);
    }
    else if (volumes.size() > cut_parts_cnt) {
        // Means that object is cut with connectors

        // All volumes are distributed to Upper / Lower object,
        // So we don’t need them anymore
        for (size_t id = 0; id < cut_parts_cnt; id++)
            delete* (volumes.begin() + id);
        volumes.erase(volumes.begin(), volumes.begin() + cut_parts_cnt);

        // Perform cut just to get connectors
        Cut cut(cut_mo, m_instance, m_cut_matrix, m_attributes);
        const ModelObjectPtrs& cut_connectors_obj = cut.perform_with_plane();
        assert(dowels_count > 0 ? cut_connectors_obj.size() >= 3 : cut_connectors_obj.size() == 2);

        // Connectors from upper object
        for (const ModelVolume* volume : cut_connectors_obj[0]->volumes)
            upper->add_volume(*volume, volume->type());

        // Connectors from lower object
        for (const ModelVolume* volume : cut_connectors_obj[1]->volumes)
            lower->add_volume(*volume, volume->type());

        // Add Upper and Lower objects to cut_object_ptrs
        post_process(upper, lower, cut_object_ptrs);

        // Now merge all model parts together:
        merge_solid_parts_inside_object(cut_object_ptrs);

        // replace initial objects in model with cut object
        finalize(cut_object_ptrs, saved_paintings);

        // Add Dowel-connectors as separate objects to model
        if (cut_connectors_obj.size() >= 3)
            for (size_t id = 2; id < cut_connectors_obj.size(); id++)
                m_model.add_object(*cut_connectors_obj[id]);
    }

    return m_model.objects;
}


const ModelObjectPtrs& Cut::perform_with_groove(const Groove&       groove,
                                                const Transform3d&  rotation_m,
                                                const int           groove_count,
                                                const float         groove_gap,
                                                const float         radius,
                                                bool                keep_as_parts/* = false*/)
{
    ModelObject* cut_mo = m_model.objects.front();
    // sanitize the multi-groove parameters: clamp the count to the UI range (1..100) and
    // replace non-finite / out-of-range gap and radius with safe defaults
    // max_groove_gap is a defensive upper bound (in mm), far above any plate size reachable
    // from the UI; it only keeps gap + groove_width finite and never applies to valid input
    constexpr float max_groove_gap  = 100000.f;
    const int       valid_groove_count = std::min(std::max(groove_count, 1), 100);
    const float     valid_groove_gap   = std::isfinite(groove_gap) && groove_gap >= 0.f ? std::min(groove_gap, max_groove_gap) : 0.f;
    const float     valid_radius       = std::isfinite(radius) && radius > 0.f ? radius : 1.f;

    // Clone the object to duplicate instances, materials etc.
    ModelObject* upper{ nullptr };
    cut_mo->clone_for_cut(&upper);
    ModelObject* lower{ nullptr };
    cut_mo->clone_for_cut(&lower);

    if (upper && lower) {
        upper->name = upper->name + "_A";
        lower->name = lower->name + "_B";
    }

    std::vector<SavedVolumePainting> saved_paintings;
    if (m_attributes.has(ModelObjectCutAttribute::KeepPaint)) {
        const Transform3d instance_matrix = cut_mo->instances[m_instance]->get_transformation().get_matrix_no_offset();
        for (ModelVolume* volume : cut_mo->volumes) {
            volume->cut_info.painting_source_ids = {volume->id()};
            saved_paintings.push_back({volume->id(), volume->save_painting()});
            if (saved_paintings.back().painting)
                saved_paintings.back().painting->transform = instance_matrix * volume->get_matrix();
        }
    }
    const double groove_half_depth = 0.5 * double(groove.depth);

    Model tmp_model_for_cut = Model();

    Model tmp_model = Model();
    tmp_model.add_object(*cut_mo);
    ModelObject* tmp_object = tmp_model.objects.front();

    auto add_volumes_from_cut = [](ModelObject* object, const ModelObjectCutAttribute attribute, const Model& tmp_model_for_cut) {
        const auto& volumes = tmp_model_for_cut.objects.front()->volumes;
        for (const ModelVolume* volume : volumes)
            if (volume->is_model_part()) {
                if ((attribute == ModelObjectCutAttribute::KeepUpper && volume->is_from_upper()) ||
                    (attribute != ModelObjectCutAttribute::KeepUpper && !volume->is_from_upper())) {
                    ModelVolume* new_vol = object->add_volume(*volume);
                    new_vol->reset_from_upper();
                }
            }
    };

    auto cut = [this, add_volumes_from_cut]
                (ModelObject* object, const Transform3d& cut_matrix, const ModelObjectCutAttribute add_volumes_attribute, Model& tmp_model_for_cut) {
        Cut cut(object, m_instance, cut_matrix);

        tmp_model_for_cut = Model();
        tmp_model_for_cut.add_object(*cut.perform_with_plane().front());
        assert(!tmp_model_for_cut.objects.empty());

        object->clear_volumes();
        add_volumes_from_cut(object, add_volumes_attribute, tmp_model_for_cut);
        reset_instance_transformation(object, m_instance);
    };

    // cut by upper plane

    const Transform3d cut_matrix_upper = translation_transform(rotation_m * (groove_half_depth * Vec3d::UnitZ())) * m_cut_matrix;
    {
        cut(tmp_object, cut_matrix_upper, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
        add_volumes_from_cut(upper, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
    }

    // cut by lower plane

    const Transform3d cut_matrix_lower = translation_transform(rotation_m * (-groove_half_depth * Vec3d::UnitZ())) * m_cut_matrix;
    {
        cut(tmp_object, cut_matrix_lower, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
        add_volumes_from_cut(lower, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
    }

    // Compute same slot outer width used in preview plane
    const float groove_width = calculate_groove_width(groove, valid_radius);

    // multiple cuts
    for (int i = 0; i < valid_groove_count; ++i)
    {
        bool is_first_groove = i == 0;
        bool is_last_groove  = i == valid_groove_count - 1;

        // Calculate the x-axis offset for this dovetail
        float groove_offset_factor_start = -0.5f * static_cast<float>(valid_groove_count - 1);
        float groove_offset_factor       = groove_offset_factor_start + static_cast<float>(i);

        // spacing between adjacent groove centers: guard the sum and the product against
        // overflow from extreme inputs so the cut transforms always stay finite
        float groove_spacing = valid_groove_gap + groove_width;
        if (!std::isfinite(groove_spacing))
            groove_spacing = groove_width;
        float offset_x = groove_offset_factor * groove_spacing;
        if (!std::isfinite(offset_x))
            offset_x = 0.f;

        // each groove works on its own temporary model copy: the repeated angle/tolerance
        // cuts would otherwise pollute the state of the other grooves cut from tmp_object
        Model        groove_model;
        ModelObject* groove_object = groove_model.add_object(*tmp_object);
        // add_object() already deep-copied all volumes of tmp_object, only the cut state needs resetting
        for (ModelVolume* volume : groove_object->volumes)
            volume->reset_from_upper();

        // isolate area of current groove
        if (!is_first_groove)
        {
            float left_cut_position = (-valid_groove_gap / 2.f) - (groove_width / 2.f) + offset_x;

            const Transform3d cut_matrix_left = translation_transform(rotation_m * (left_cut_position * Vec3d::UnitX())) *
                                                m_cut_matrix * rotation_transform(Vec3d(0, PI / 2.0, 0));

            cut(groove_object, cut_matrix_left, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
        }
        if (!is_last_groove)
        {
            float right_cut_position = (valid_groove_gap / 2.f) + (groove_width / 2.f) + offset_x;

            const Transform3d cut_matrix_right = translation_transform(rotation_m * (right_cut_position * Vec3d::UnitX())) *
                                                 m_cut_matrix * rotation_transform(Vec3d(0, PI / 2.0, 0));
            cut(groove_object, cut_matrix_right, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
        }

        const Transform3d groove_translation = translation_transform(rotation_m * (offset_x * Vec3d::UnitX()));
        // cut middle part with 2 angles and add parts to related upper/lower objects
        const double h_side_shift = 0.5 * double(groove.width + groove.depth / tan(groove.flaps_angle));

        // cut by angle1 plane
        {
            const Transform3d cut_matrix_angle1 = groove_translation * translation_transform(rotation_m * (-h_side_shift * Vec3d::UnitX())) *
                                                  m_cut_matrix * rotation_transform(Vec3d(0, -groove.flaps_angle, -groove.angle));

            cut(groove_object, cut_matrix_angle1, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
            add_volumes_from_cut(lower, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
        }

        // cut by angle2 plane
        {
            const Transform3d cut_matrix_angle2 = groove_translation * translation_transform(rotation_m * (h_side_shift * Vec3d::UnitX())) *
                                                  m_cut_matrix * rotation_transform(Vec3d(0, groove.flaps_angle, groove.angle));

            cut(groove_object, cut_matrix_angle2, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
            add_volumes_from_cut(lower, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
        }

        // apply tolerance to the middle part
        {
            const double h_groove_shift_tolerance = groove_half_depth - static_cast<double>(groove.depth_tolerance);

            const Transform3d cut_matrix_lower_tolerance = groove_translation * translation_transform(rotation_m * (-h_groove_shift_tolerance * Vec3d::UnitZ())) *
                                                           m_cut_matrix;
            cut(groove_object, cut_matrix_lower_tolerance, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);

            const double h_side_shift_tolerance = h_side_shift - 0.5 * double(groove.width_tolerance);

            const Transform3d cut_matrix_angle1_tolerance = groove_translation * translation_transform(rotation_m * (-h_side_shift_tolerance * Vec3d::UnitX())) *
                                                            m_cut_matrix * rotation_transform(Vec3d(0, -groove.flaps_angle, -groove.angle));
            cut(groove_object, cut_matrix_angle1_tolerance, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);

            const Transform3d cut_matrix_angle2_tolerance = groove_translation * translation_transform(rotation_m * (h_side_shift_tolerance * Vec3d::UnitX())) *
                                                            m_cut_matrix * rotation_transform(Vec3d(0, groove.flaps_angle, groove.angle));
            cut(groove_object, cut_matrix_angle2_tolerance, ModelObjectCutAttribute::KeepUpper, tmp_model_for_cut);
        }

        add_volumes_from_cut(upper, ModelObjectCutAttribute::KeepLower, tmp_model_for_cut);
    }

    ModelObjectPtrs cut_object_ptrs;

    if (keep_as_parts) {
        // add volumes from lower object to the upper, but mark them as a lower
        const auto& volumes = lower->volumes;
        for (const ModelVolume* volume : volumes) {
            ModelVolume* new_vol = upper->add_volume(*volume);
            new_vol->cut_info.is_from_upper = false;
        }

        // add modifiers
        for (const ModelVolume* volume : cut_mo->volumes)
            if (!volume->is_model_part())
                upper->add_volume(*volume);

        cut_object_ptrs.push_back(upper);

        // add lower object to the cut_object_ptrs just to correct delete it from the Model destructor and avoid memory leaks
        cut_object_ptrs.push_back(lower);
    }
    else {
        // add modifiers if object has any
        for (const ModelVolume* volume : cut_mo->volumes)
            if (!volume->is_model_part()) {
                distribute_modifiers_from_object(cut_mo, m_instance, upper, lower);
                break;
            }

        assert(!upper->volumes.empty() && !lower->volumes.empty());

        // Add Upper and Lower parts to cut_object_ptrs

        post_process(upper, lower, cut_object_ptrs);

        // Now merge all model parts together:
        merge_solid_parts_inside_object(cut_object_ptrs);
    }

    finalize(cut_object_ptrs, saved_paintings);

    return m_model.objects;
}

const ModelObjectPtrs& Cut::perform_with_groove(const Groove&      groove,
                                                const Transform3d& rotation_m,
                                                bool               keep_as_parts/* = false*/)
{
    // Legacy single-groove entry point: with groove_count == 1 the spacing width (and therefore
    // the radius) has no effect on the result, so this stays equivalent to the old behavior.
    return perform_with_groove(groove, rotation_m, 1, 0.f, 1.f, keep_as_parts);
}

float Cut::calculate_groove_width(const Cut::Groove& groove, const float radius)
{
    // Compute same slot outer width used in preview plane
    const float valid_radius = std::isfinite(radius) && radius > 0.f ? radius : 1.f;

    // Conservative finite fallback: the neck half width only
    const double slot_neck_half_width = std::isfinite(groove.width) ? 0.5f * groove.width : 0.0;
    double       slot_outer_x_max     = slot_neck_half_width;

    if (std::isfinite(groove.depth) && std::isfinite(groove.width) &&
        std::isfinite(groove.flaps_angle) && std::isfinite(groove.angle))
    {
        const double flap_width            = is_approx(groove.flaps_angle, 0.f) ? groove.depth : groove.depth / sin(groove.flaps_angle);
        const double total_flap_width      = 2.0 * flap_width * cos(groove.flaps_angle);
        const double slot_mouth_half_width = 0.5 * (groove.width + total_flap_width);
        const double plane_half_height     = 0.5f * (1.5f * (1.5f * valid_radius));
        const double flap_taper_offset     = plane_half_height * tan(groove.angle);

        const double slot_outer_x_full = std::max(slot_mouth_half_width + flap_taper_offset, slot_neck_half_width + flap_taper_offset);
        // tan() is singular near +-90 degrees: keep the conservative width when the full one is not finite
        if (std::isfinite(slot_outer_x_full))
            slot_outer_x_max = slot_outer_x_full;
    }

    const float groove_width = static_cast<float>(2.0 * slot_outer_x_max);
    // guard the double-to-float conversion against overflow for extreme inputs
    return std::isfinite(groove_width) ? groove_width : std::numeric_limits<float>::max();
}

} // namespace Slic3r

