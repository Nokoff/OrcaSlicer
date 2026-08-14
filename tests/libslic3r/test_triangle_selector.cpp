#include <catch2/catch.hpp>

#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

TEST_CASE("Triangle selector round-trips painted states above sixteen", "[TriangleSelector][MMUPaint]")
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(1.f, 0.f, 0.f),
        Vec3f(0.f, 1.f, 0.f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
    };

    TriangleMesh mesh(its);
    TriangleSelector selector(mesh);

    constexpr int painted_state = 120;
    selector.set_facet(0, static_cast<EnforcerBlockerType>(painted_state));

    auto data = selector.serialize();
    REQUIRE_FALSE(data.triangles_to_split.empty());
    REQUIRE(data.used_states.size() > painted_state);
    CHECK(data.used_states[painted_state]);

    data.reset_used_states();
    data.update_used_states(size_t(data.triangles_to_split.front().bitstream_start_idx));
    CHECK(data.used_states[painted_state]);
    CHECK(TriangleSelector::has_facets(data, static_cast<EnforcerBlockerType>(painted_state)));

    TriangleSelector restored(mesh);
    restored.deserialize(data, true, static_cast<EnforcerBlockerType>(painted_state));
    CHECK(restored.has_facets(static_cast<EnforcerBlockerType>(painted_state)));
}

static indexed_triangle_set single_triangle()
{
    indexed_triangle_set its;
    its.vertices = {
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(1.f, 0.f, 0.f),
        Vec3f(0.f, 1.f, 0.f),
    };
    its.indices = {
        stl_triangle_vertex_indices(0, 1, 2),
    };
    return its;
}

TEST_CASE("OBJ face colors preserve filament IDs above sixteen", "[OBJ][MMUPaint]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(TriangleMesh(single_triangle()));

    constexpr unsigned char painted_state = 33;
    REQUIRE(Model::obj_import_face_color_deal({painted_state}, painted_state, &model));
    CHECK(TriangleSelector::has_facets(volume->mmu_segmentation_facets.get_data(), static_cast<EnforcerBlockerType>(painted_state)));
}

TEST_CASE("OBJ vertex colors preserve multiple filament IDs above sixteen", "[OBJ][MMUPaint]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(TriangleMesh(single_triangle()));

    const std::vector<unsigned char> painted_states {17, 33, 64};
    REQUIRE(Model::obj_import_vertex_color_deal(painted_states, painted_states.front(), &model));
    for (const unsigned char painted_state : painted_states) {
        CHECK(TriangleSelector::has_facets(volume->mmu_segmentation_facets.get_data(), static_cast<EnforcerBlockerType>(painted_state)));
    }
}

TEST_CASE("Planar cuts with connectors preserve painted colors", "[Cut][MMUPaint]")
{
    Model model;
    model.add_material("painted-material");

    ModelObject *object = model.add_object();
    ModelVolume *painted_volume = object->add_volume(make_cube(20., 20., 20.));
    painted_volume->set_material_id("painted-material");
    object->add_instance();

    constexpr int painted_state = 33;
    TriangleSelector selector(painted_volume->mesh());
    // This side facet crosses the cut plane, so both resulting solids must retain it.
    selector.set_facet(4, static_cast<EnforcerBlockerType>(painted_state));
    REQUIRE(painted_volume->mmu_segmentation_facets.set(selector));

    ModelVolume *connector = object->add_volume(make_cube(4., 4., 4.));
    connector->set_material_id("painted-material");
    connector->set_type(ModelVolumeType::NEGATIVE_VOLUME);
    connector->cut_info = ModelVolume::CutInfo(CutConnectorType::Plug, 0.f, 0.f, false);

    const ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                                ModelObjectCutAttribute::KeepPaint;
    Cut cut(object, 0, Geometry::translation_transform(Vec3d(0., 0., 10.)), attributes);
    const ModelObjectPtrs &cut_objects = cut.perform_with_plane();

    REQUIRE(cut_objects.size() == 2);
    for (const ModelObject *cut_object : cut_objects) {
        bool found_painted_solid = false;
        for (const ModelVolume *volume : cut_object->volumes) {
            if (volume->is_cut_connector()) {
                CHECK(volume->mmu_segmentation_facets.empty());
            } else if (volume->is_model_part()) {
                found_painted_solid = true;
                CHECK(TriangleSelector::has_facets(volume->mmu_segmentation_facets.get_data(),
                                                   static_cast<EnforcerBlockerType>(painted_state)));
            }
        }
        CHECK(found_painted_solid);
    }
}
