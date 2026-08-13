#include <catch2/catch.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/glTF.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// Assemble a .glb container around a JSON document and its binary buffer.
// Both chunks must be padded to a 4 byte boundary - JSON with spaces, BIN with zeros.
std::vector<unsigned char> make_glb(const std::string &json, const std::vector<unsigned char> &bin)
{
    std::string padded_json = json;
    while (padded_json.size() % 4 != 0) padded_json.push_back(' ');
    std::vector<unsigned char> padded_bin = bin;
    while (padded_bin.size() % 4 != 0) padded_bin.push_back(0);

    const uint32_t json_len = (uint32_t) padded_json.size();
    const uint32_t bin_len  = (uint32_t) padded_bin.size();
    const uint32_t total    = 12 + 8 + json_len + (bin_len ? 8 + bin_len : 0);

    std::vector<unsigned char> out;
    auto push32 = [&out](uint32_t v) {
        unsigned char b[4];
        std::memcpy(b, &v, 4);
        out.insert(out.end(), b, b + 4);
    };

    out.insert(out.end(), {'g', 'l', 'T', 'F'});
    push32(2);
    push32(total);

    push32(json_len);
    push32(0x4E4F534A); // 'JSON'
    out.insert(out.end(), padded_json.begin(), padded_json.end());

    if (bin_len) {
        push32(bin_len);
        push32(0x004E4942); // 'BIN\0'
        out.insert(out.end(), padded_bin.begin(), padded_bin.end());
    }
    return out;
}

void append_floats(std::vector<unsigned char> &buf, const std::vector<float> &values)
{
    for (float v : values) {
        unsigned char b[4];
        std::memcpy(b, &v, 4);
        buf.insert(buf.end(), b, b + 4);
    }
}

void append_ushorts(std::vector<unsigned char> &buf, const std::vector<uint16_t> &values)
{
    for (uint16_t v : values) {
        unsigned char b[2];
        std::memcpy(b, &v, 2);
        buf.insert(buf.end(), b, b + 2);
    }
}

// RAII temp file so a failing assertion still cleans up.
struct TempGlb
{
    boost::filesystem::path path;
    explicit TempGlb(const std::vector<unsigned char> &bytes)
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca_gltf_%%%%%%%%.glb"))
    {
        std::ofstream f(path.string(), std::ios::binary);
        f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    ~TempGlb()
    {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
    }
    std::string str() const { return path.string(); }
};

} // namespace

SCENARIO("Reading a GLB file", "[gltf]")
{
    GIVEN("a single triangle with a flat base colour material")
    {
        // A triangle in the glTF XY plane, one unit (i.e. one metre) on each leg.
        std::vector<unsigned char> bin;
        append_floats(bin, {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f});
        const size_t index_offset = bin.size();
        append_ushorts(bin, {0, 1, 2});

        const std::string json = R"({
            "asset": {"version": "2.0"},
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
            "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.0, 0.0, 1.0]}}],
            "buffers": [{"byteLength": )" + std::to_string(bin.size()) + R"(}],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": 36},
                {"buffer": 0, "byteOffset": )" + std::to_string(index_offset) + R"(, "byteLength": 6}
            ],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
                {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
            ]
        })";

        TempGlb      glb(make_glb(json, bin));
        TriangleMesh mesh;
        GltfInfo     info;
        std::string  message;

        WHEN("the file is loaded")
        {
            const bool ok = load_gltf(glb.str().c_str(), &mesh, info, message);

            THEN("the load succeeds and yields one triangle")
            {
                REQUIRE(ok);
                REQUIRE(mesh.its.indices.size() == 1);
                REQUIRE(mesh.its.vertices.size() == 3);
            }

            THEN("Y-up metres are converted to Z-up millimetres")
            {
                REQUIRE(ok);
                // glTF (1,0,0) -> (1000, 0, 0); glTF (0,1,0) -> (0, 0, 1000).
                const BoundingBoxf3 bb = mesh.bounding_box();
                REQUIRE_THAT(bb.size().x(), WithinAbs(1000., 1e-3));
                REQUIRE_THAT(bb.size().y(), WithinAbs(0., 1e-3));
                REQUIRE_THAT(bb.size().z(), WithinAbs(1000., 1e-3));
            }

            THEN("the material colour is reported per face, encoded as sRGB")
            {
                REQUIRE(ok);
                REQUIRE(info.face_colors.size() == 1);
                // linear 0.5 encodes to roughly 0.7354 in sRGB.
                REQUIRE_THAT(info.face_colors[0][0], WithinAbs(0.7354, 1e-3));
                REQUIRE_THAT(info.face_colors[0][1], WithinAbs(0., 1e-3));
                REQUIRE_THAT(info.face_colors[0][2], WithinAbs(0., 1e-3));
                REQUIRE(info.is_single_color);
                REQUIRE(!info.from_texture);
            }
        }
    }

    GIVEN("a triangle carrying COLOR_0 vertex colours")
    {
        std::vector<unsigned char> bin;
        append_floats(bin, {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f});
        const size_t color_offset = bin.size();
        append_floats(bin, {1.f, 0.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f});
        const size_t index_offset = bin.size();
        append_ushorts(bin, {0, 1, 2});

        const std::string json = R"({
            "asset": {"version": "2.0"},
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "COLOR_0": 1}, "indices": 2, "material": 0}]}],
            "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [1.0, 1.0, 1.0, 1.0]}}],
            "buffers": [{"byteLength": )" + std::to_string(bin.size()) + R"(}],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": 36},
                {"buffer": 0, "byteOffset": )" + std::to_string(color_offset) + R"(, "byteLength": 48},
                {"buffer": 0, "byteOffset": )" + std::to_string(index_offset) + R"(, "byteLength": 6}
            ],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
                {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC4"},
                {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}
            ]
        })";

        TempGlb      glb(make_glb(json, bin));
        TriangleMesh mesh;
        GltfInfo     info;
        std::string  message;

        WHEN("the file is loaded")
        {
            const bool ok = load_gltf(glb.str().c_str(), &mesh, info, message);
            THEN("one colour per vertex is reported")
            {
                REQUIRE(ok);
                REQUIRE(info.vertex_colors.size() == 3);
                REQUIRE_THAT(info.vertex_colors[0][0], WithinAbs(1., 1e-3));
                REQUIRE_THAT(info.vertex_colors[1][1], WithinAbs(1., 1e-3));
                REQUIRE_THAT(info.vertex_colors[2][2], WithinAbs(1., 1e-3));
            }
        }
    }

    GIVEN("a node transform expressed as a matrix")
    {
        std::vector<unsigned char> bin;
        append_floats(bin, {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f});
        const size_t index_offset = bin.size();
        append_ushorts(bin, {0, 1, 2});

        // Column-major, scale 2 with a translation of 3 along glTF X.
        const std::string json = R"({
            "asset": {"version": "2.0"},
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": [{"mesh": 0, "matrix": [2,0,0,0, 0,2,0,0, 0,0,2,0, 3,0,0,1]}],
            "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
            "buffers": [{"byteLength": )" + std::to_string(bin.size()) + R"(}],
            "bufferViews": [
                {"buffer": 0, "byteOffset": 0, "byteLength": 36},
                {"buffer": 0, "byteOffset": )" + std::to_string(index_offset) + R"(, "byteLength": 6}
            ],
            "accessors": [
                {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
                {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
            ]
        })";

        TempGlb      glb(make_glb(json, bin));
        TriangleMesh mesh;
        GltfInfo     info;
        std::string  message;

        WHEN("the file is loaded")
        {
            const bool ok = load_gltf(glb.str().c_str(), &mesh, info, message);
            THEN("the node matrix is applied before the unit conversion")
            {
                REQUIRE(ok);
                const BoundingBoxf3 bb = mesh.bounding_box();
                REQUIRE_THAT(bb.min.x(), WithinAbs(3000., 1e-3));
                REQUIRE_THAT(bb.size().x(), WithinAbs(2000., 1e-3));
                REQUIRE_THAT(bb.size().z(), WithinAbs(2000., 1e-3));
            }
            THEN("no material means no colour mapping is offered")
            {
                REQUIRE(ok);
                REQUIRE(info.face_colors.empty());
            }
        }
    }

    GIVEN("a real textured asset, if one was supplied")
    {
        // Point ORCA_GLTF_TEST_FILE at a .glb to exercise the image decode and UV sampling
        // path; without it this block just reports a skip. Real textured assets run to
        // hundreds of megabytes, so none is committed.
        const char *env = std::getenv("ORCA_GLTF_TEST_FILE");
        if (env == nullptr || !boost::filesystem::exists(env)) {
            WARN("ORCA_GLTF_TEST_FILE not set or missing; skipping real-asset check");
        } else {
            TriangleMesh mesh;
            GltfInfo     info;
            std::string  message;

            WHEN("the file is loaded")
            {
                const bool ok = load_gltf(env, &mesh, info, message);
                THEN("geometry and a colour per face come back")
                {
                    INFO("message: " << message);
                    REQUIRE(ok);
                    REQUIRE(mesh.its.indices.size() > 0);
                    REQUIRE(info.face_colors.size() == mesh.its.indices.size());

                    size_t                     distinct = 0;
                    std::vector<unsigned char> seen(1 << 18, 0);
                    for (const RGBA &c : info.face_colors) {
                        const unsigned idx = (unsigned(c[0] * 63.f) << 12) | (unsigned(c[1] * 63.f) << 6) | unsigned(c[2] * 63.f);
                        if (!seen[idx]) {
                            seen[idx] = 1;
                            ++distinct;
                        }
                    }
                    const BoundingBoxf3 bb = mesh.bounding_box();
                    WARN("triangles=" << mesh.its.indices.size() << " vertices=" << mesh.its.vertices.size()
                                      << " distinct_colours(6bit)=" << distinct << " from_texture=" << info.from_texture
                                      << " missing_textures=" << info.missing_textures << " size_mm=" << bb.size().x() << "x"
                                      << bb.size().y() << "x" << bb.size().z());
                }
            }
        }
    }

    GIVEN("a malformed file")
    {
        TempGlb      glb(std::vector<unsigned char>{'n', 'o', 'p', 'e', 0, 0, 0, 0});
        TriangleMesh mesh;
        GltfInfo     info;
        std::string  message;

        WHEN("the file is loaded")
        {
            THEN("it fails without throwing") { REQUIRE(!load_gltf(glb.str().c_str(), &mesh, info, message)); }
        }
    }
}
