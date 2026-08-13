#ifndef slic3r_Format_glTF_hpp_
#define slic3r_Format_glTF_hpp_

#include "libslic3r/Color.hpp"
#include <string>
#include <vector>

namespace Slic3r {

class TriangleMesh;
class Model;
class ModelObject;

// Colour information recovered from a glTF/GLB asset.
// Deliberately shaped like ObjInfo so the existing ObjColorDialog plumbing
// (Model::obj_import_vertex_color_deal / obj_import_face_color_deal) can consume it
// without a second dialog implementation.
struct GltfInfo
{
    // Per-vertex colours from the COLOR_0 attribute, parallel to mesh.its.vertices.
    // Empty unless the asset actually carries vertex colours.
    std::vector<RGBA> vertex_colors;
    // Per-triangle colours, parallel to mesh.its.indices. Populated either from each
    // primitive's flat material baseColorFactor, or by sampling that material's
    // baseColorTexture at the triangle's UV centroid.
    std::vector<RGBA> face_colors;
    // True when every face resolved to the same colour - lets the dialog skip clustering.
    bool is_single_color{false};
    // True when face_colors came from a texture rather than flat material factors.
    bool from_texture{false};
    // Set when the asset referenced textures we could not read (external URI, unsupported
    // codec). The mesh still loads; the caller may want to warn that colours are partial.
    bool missing_textures{false};
};

// Load a glTF 2.0 asset (.glb binary container or .gltf JSON) into a mesh.
// All primitives across all scene nodes are merged into one mesh with node
// transforms applied, matching how the OBJ importer produces a single volume.
extern bool load_gltf(const char *path, TriangleMesh *mesh, GltfInfo &info, std::string &message);
extern bool load_gltf(const char *path, Model *model, GltfInfo &info, std::string &message, const char *object_name = nullptr);

}; // namespace Slic3r

#endif /* slic3r_Format_glTF_hpp_ */
