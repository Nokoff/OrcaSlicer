#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"
#include "../PNGReadWrite.hpp"

#include "glTF.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>
#include <nlohmann/json.hpp>

#include <jpeglib.h>
#include <csetjmp>

#ifdef _WIN32
#define DIR_SEPARATOR '\\'
#else
#define DIR_SEPARATOR '/'
#endif

// Translation
#include "I18N.hpp"
#define _L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

using json = nlohmann::json;

namespace {

// glTF component types
enum : int {
    COMP_BYTE           = 5120,
    COMP_UNSIGNED_BYTE  = 5121,
    COMP_SHORT          = 5122,
    COMP_UNSIGNED_SHORT = 5123,
    COMP_UNSIGNED_INT   = 5125,
    COMP_FLOAT          = 5126,
};

// primitive.mode - we only build geometry from real triangles.
enum : int { MODE_TRIANGLES = 4, MODE_TRIANGLE_STRIP = 5, MODE_TRIANGLE_FAN = 6 };

// sampler wrap modes
enum : int { WRAP_CLAMP_TO_EDGE = 33071, WRAP_MIRRORED_REPEAT = 33648, WRAP_REPEAT = 10497 };

// glTF stores lengths in meters, the slicer works in millimeters.
static const double GLTF_UNITS_TO_MM = 1000.;

size_t num_components(const std::string &type)
{
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
}

size_t component_size(int component_type)
{
    switch (component_type) {
    case COMP_BYTE:
    case COMP_UNSIGNED_BYTE: return 1;
    case COMP_SHORT:
    case COMP_UNSIGNED_SHORT: return 2;
    case COMP_UNSIGNED_INT:
    case COMP_FLOAT: return 4;
    default: return 0;
    }
}

// Convenience accessors that tolerate missing keys, which are pervasive in glTF.
const json *find_member(const json &j, const char *key)
{
    auto it = j.find(key);
    return it == j.end() ? nullptr : &(*it);
}

int int_or(const json &j, const char *key, int fallback)
{
    const json *m = find_member(j, key);
    return (m && m->is_number()) ? m->get<int>() : fallback;
}

const json &array_or_empty(const json &j, const char *key)
{
    static const json empty = json::array();
    const json *      m     = find_member(j, key);
    return (m && m->is_array()) ? *m : empty;
}

// ---------------------------------------------------------------------------
// base64 (for data: URIs in .gltf files)
// ---------------------------------------------------------------------------
bool base64_decode(const std::string &in, std::vector<unsigned char> &out)
{
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    out.reserve(in.size() * 3 / 4);
    int accum = 0, bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = value(c);
        if (v < 0) return false;
        accum = (accum << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((accum >> bits) & 0xFF));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Decoded RGBA8 image used for texture sampling
// ---------------------------------------------------------------------------
struct Image
{
    size_t                     width{0};
    size_t                     height{0};
    std::vector<unsigned char> rgba; // width * height * 4

    bool valid() const { return width > 0 && height > 0 && rgba.size() == width * height * 4; }
};

// --- JPEG ------------------------------------------------------------------
struct JpegErrorMgr
{
    jpeg_error_mgr pub;
    std::jmp_buf   setjmp_buffer;
};

void jpeg_error_exit_longjmp(j_common_ptr cinfo)
{
    JpegErrorMgr *err = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
    std::longjmp(err->setjmp_buffer, 1);
}

void jpeg_output_message_silent(j_common_ptr) {}

// Decode a JPEG from memory into RGBA8. Textures in printable assets are often
// 8k x 8k; since colours are only sampled per triangle and then clustered down to a
// handful of filaments, we let libjpeg downscale during the DCT for a large win in
// time and memory.
bool decode_jpeg(const unsigned char *data, size_t size, size_t max_dimension, Image &out)
{
    jpeg_decompress_struct cinfo;
    JpegErrorMgr           jerr;

    cinfo.err           = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit_longjmp;
    jerr.pub.output_message = jpeg_output_message_silent;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, static_cast<unsigned long>(size));

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    // libjpeg supports N/8 scaling for N in [1..16]; pick the smallest that still
    // covers max_dimension.
    if (max_dimension > 0) {
        unsigned int largest = std::max(cinfo.image_width, cinfo.image_height);
        unsigned int n       = 8;
        while (n > 1 && (largest * (n - 1) + 7) / 8 >= max_dimension) --n;
        cinfo.scale_num   = n;
        cinfo.scale_denom = 8;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    out.width  = cinfo.output_width;
    out.height = cinfo.output_height;
    out.rgba.assign(out.width * out.height * 4, 255);

    const size_t          row_stride = cinfo.output_width * cinfo.output_components;
    std::vector<unsigned char> row(row_stride);
    unsigned char *       row_ptr[1] = {row.data()};

    while (cinfo.output_scanline < cinfo.output_height) {
        size_t y = cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, row_ptr, 1);
        unsigned char *dst = out.rgba.data() + y * out.width * 4;
        if (cinfo.output_components == 3) {
            for (size_t x = 0; x < out.width; ++x) {
                dst[x * 4 + 0] = row[x * 3 + 0];
                dst[x * 4 + 1] = row[x * 3 + 1];
                dst[x * 4 + 2] = row[x * 3 + 2];
                dst[x * 4 + 3] = 255;
            }
        } else if (cinfo.output_components == 1) {
            for (size_t x = 0; x < out.width; ++x) {
                dst[x * 4 + 0] = dst[x * 4 + 1] = dst[x * 4 + 2] = row[x];
                dst[x * 4 + 3]                                   = 255;
            }
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return out.valid();
}

// --- PNG -------------------------------------------------------------------
// Note: decode_colored_png stores rows bottom-up (its other callers feed OpenGL
// textures) and only handles 8 bit RGB/RGBA. We flip back to top-down here so
// sampling matches the glTF UV convention, where v = 0 is the top of the image.
bool decode_png_rgba(const unsigned char *data, size_t size, Image &out)
{
    png::ReadBuf         buf{data, size};
    png::ImageColorscale img;
    if (!png::decode_colored_png(buf, img)) return false;

    const size_t bpp = (size_t) img.bytes_per_pixel;
    if (img.cols == 0 || img.rows == 0 || (bpp != 3 && bpp != 4)) return false;
    // Guards against bit depths the decoder mislabels (it reports 3/4 bytes per pixel
    // even for 16 bit images, where the row stride is actually double).
    if (img.buf.size() != img.rows * img.cols * bpp) return false;

    out.width  = img.cols;
    out.height = img.rows;
    out.rgba.assign(out.width * out.height * 4, 255);

    for (size_t y = 0; y < out.height; ++y) {
        const unsigned char *src_row = img.buf.data() + (out.height - 1 - y) * out.width * bpp;
        unsigned char *      dst_row = out.rgba.data() + y * out.width * 4;
        for (size_t x = 0; x < out.width; ++x) {
            const unsigned char *src = src_row + x * bpp;
            unsigned char *      dst = dst_row + x * 4;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = bpp == 4 ? src[3] : 255;
        }
    }
    return out.valid();
}

// ---------------------------------------------------------------------------
// sRGB <-> linear. baseColorTexture texels and baseColorFactor live in different
// spaces; multiply in linear, hand the result back in sRGB for display.
// ---------------------------------------------------------------------------
float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linear_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f; }

float wrap_coord(float v, int mode)
{
    switch (mode) {
    case WRAP_CLAMP_TO_EDGE: return std::min(std::max(v, 0.f), 1.f);
    case WRAP_MIRRORED_REPEAT: {
        float t = std::fabs(std::fmod(v, 2.f));
        if (t > 1.f) t = 2.f - t;
        return t;
    }
    case WRAP_REPEAT:
    default: {
        float t = std::fmod(v, 1.f);
        if (t < 0.f) t += 1.f;
        return t;
    }
    }
}

// ---------------------------------------------------------------------------
// The parsed document: JSON plus resolved binary buffers.
// ---------------------------------------------------------------------------
struct GltfDocument
{
    json                                    j;
    std::vector<std::vector<unsigned char>> buffers;
    boost::filesystem::path                 base_dir;

    const std::vector<unsigned char> *buffer(size_t idx) const
    {
        return idx < buffers.size() ? &buffers[idx] : nullptr;
    }
};

// Read one accessor into a flat float array (count * num_components).
// Normalized integer types are rescaled to [0,1] per the spec.
bool read_accessor_floats(const GltfDocument &doc, int accessor_idx, std::vector<float> &out, size_t &out_components)
{
    const json &accessors = array_or_empty(doc.j, "accessors");
    if (accessor_idx < 0 || accessor_idx >= (int) accessors.size()) return false;
    const json &acc = accessors[accessor_idx];

    const size_t count      = (size_t) int_or(acc, "count", 0);
    const int    comp_type  = int_or(acc, "componentType", 0);
    const json * type_m     = find_member(acc, "type");
    if (count == 0 || !type_m || !type_m->is_string()) return false;

    const size_t ncomp = num_components(type_m->get<std::string>());
    const size_t csize = component_size(comp_type);
    if (ncomp == 0 || csize == 0) return false;
    out_components = ncomp;

    out.assign(count * ncomp, 0.f);

    const json *bv_m = find_member(acc, "bufferView");
    if (!bv_m || !bv_m->is_number()) {
        // Spec-legal: no bufferView means all zeros (possibly overridden by `sparse`,
        // which we do not support). Zeros are already in place.
        return true;
    }

    const json &bufferViews = array_or_empty(doc.j, "bufferViews");
    const int   bv_idx      = bv_m->get<int>();
    if (bv_idx < 0 || bv_idx >= (int) bufferViews.size()) return false;
    const json &bv = bufferViews[bv_idx];

    const std::vector<unsigned char> *buf = doc.buffer((size_t) int_or(bv, "buffer", -1));
    if (!buf) return false;

    const size_t bv_offset = (size_t) int_or(bv, "byteOffset", 0);
    const size_t acc_offset = (size_t) int_or(acc, "byteOffset", 0);
    const size_t stride     = (size_t) int_or(bv, "byteStride", 0) ? (size_t) int_or(bv, "byteStride", 0) : csize * ncomp;
    const size_t base       = bv_offset + acc_offset;

    const bool normalized = [&] {
        const json *m = find_member(acc, "normalized");
        return m && m->is_boolean() && m->get<bool>();
    }();

    // Bounds check before touching memory.
    if (count > 0 && base + (count - 1) * stride + csize * ncomp > buf->size()) return false;

    const unsigned char *data = buf->data();
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *p = data + base + i * stride;
        for (size_t c = 0; c < ncomp; ++c) {
            const unsigned char *q = p + c * csize;
            float                v = 0.f;
            switch (comp_type) {
            case COMP_FLOAT: {
                float f;
                std::memcpy(&f, q, 4);
                v = f;
                break;
            }
            case COMP_UNSIGNED_BYTE: {
                uint8_t u = *q;
                v         = normalized ? float(u) / 255.f : float(u);
                break;
            }
            case COMP_BYTE: {
                int8_t s;
                std::memcpy(&s, q, 1);
                v = normalized ? std::max(float(s) / 127.f, -1.f) : float(s);
                break;
            }
            case COMP_UNSIGNED_SHORT: {
                uint16_t u;
                std::memcpy(&u, q, 2);
                v = normalized ? float(u) / 65535.f : float(u);
                break;
            }
            case COMP_SHORT: {
                int16_t s;
                std::memcpy(&s, q, 2);
                v = normalized ? std::max(float(s) / 32767.f, -1.f) : float(s);
                break;
            }
            case COMP_UNSIGNED_INT: {
                uint32_t u;
                std::memcpy(&u, q, 4);
                v = float(u);
                break;
            }
            default: return false;
            }
            out[i * ncomp + c] = v;
        }
    }
    return true;
}

// Indices need full 32-bit range, so they get their own reader.
bool read_accessor_indices(const GltfDocument &doc, int accessor_idx, std::vector<uint32_t> &out)
{
    const json &accessors = array_or_empty(doc.j, "accessors");
    if (accessor_idx < 0 || accessor_idx >= (int) accessors.size()) return false;
    const json & acc       = accessors[accessor_idx];
    const size_t count     = (size_t) int_or(acc, "count", 0);
    const int    comp_type = int_or(acc, "componentType", 0);
    const size_t csize     = component_size(comp_type);
    if (count == 0 || csize == 0) return false;

    const json *bv_m = find_member(acc, "bufferView");
    if (!bv_m || !bv_m->is_number()) return false;

    const json &bufferViews = array_or_empty(doc.j, "bufferViews");
    const int   bv_idx      = bv_m->get<int>();
    if (bv_idx < 0 || bv_idx >= (int) bufferViews.size()) return false;
    const json &bv = bufferViews[bv_idx];

    const std::vector<unsigned char> *buf = doc.buffer((size_t) int_or(bv, "buffer", -1));
    if (!buf) return false;

    const size_t stride = (size_t) int_or(bv, "byteStride", 0) ? (size_t) int_or(bv, "byteStride", 0) : csize;
    const size_t base   = (size_t) int_or(bv, "byteOffset", 0) + (size_t) int_or(acc, "byteOffset", 0);
    if (base + (count - 1) * stride + csize > buf->size()) return false;

    out.resize(count);
    const unsigned char *data = buf->data();
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *p = data + base + i * stride;
        switch (comp_type) {
        case COMP_UNSIGNED_BYTE: out[i] = *p; break;
        case COMP_UNSIGNED_SHORT: {
            uint16_t u;
            std::memcpy(&u, p, 2);
            out[i] = u;
            break;
        }
        case COMP_UNSIGNED_INT: {
            uint32_t u;
            std::memcpy(&u, p, 4);
            out[i] = u;
            break;
        }
        default: return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Container loading
// ---------------------------------------------------------------------------
bool read_whole_file(const char *path, std::vector<unsigned char> &out)
{
    boost::system::error_code ec;
    const boost::filesystem::path p(path);
    const uintmax_t size = boost::filesystem::file_size(p, ec);
    if (ec) return false;

    FILE *f = boost::nowide::fopen(path, "rb");
    if (!f) return false;
    out.resize((size_t) size);
    const size_t read = size > 0 ? std::fread(out.data(), 1, (size_t) size, f) : 0;
    std::fclose(f);
    return read == (size_t) size;
}

// Resolve buffers[i].uri - data: URI, or a file next to the .gltf.
bool resolve_buffer_uri(const GltfDocument &doc, const std::string &uri, std::vector<unsigned char> &out)
{
    if (boost::starts_with(uri, "data:")) {
        const size_t comma = uri.find(',');
        if (comma == std::string::npos) return false;
        // Only base64 payloads are supported; the rare percent-encoded text form is not.
        if (uri.rfind("base64", comma) == std::string::npos) return false;
        return base64_decode(uri.substr(comma + 1), out);
    }
    // External file. Percent-decode the common case of spaces.
    std::string decoded;
    decoded.reserve(uri.size());
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const int hi = std::isxdigit((unsigned char) uri[i + 1]) ? std::stoi(uri.substr(i + 1, 2), nullptr, 16) : -1;
            if (hi >= 0) {
                decoded.push_back((char) hi);
                i += 2;
                continue;
            }
        }
        decoded.push_back(uri[i]);
    }
    const boost::filesystem::path full = doc.base_dir / decoded;
    return read_whole_file(full.string().c_str(), out);
}

// Parse a .glb container, or a .gltf JSON document, into a GltfDocument.
bool parse_document(const char *path, GltfDocument &doc, std::string &message)
{
    std::vector<unsigned char> raw;
    if (!read_whole_file(path, raw)) {
        message = _L("Could not read the file.");
        return false;
    }
    doc.base_dir = boost::filesystem::path(path).parent_path();

    std::vector<unsigned char> glb_bin;
    bool                       have_glb_bin = false;

    if (raw.size() >= 12 && raw[0] == 'g' && raw[1] == 'l' && raw[2] == 'T' && raw[3] == 'F') {
        // Binary container: 12 byte header then a sequence of chunks.
        uint32_t version = 0;
        std::memcpy(&version, raw.data() + 4, 4);
        if (version != 2) {
            message = _L("Unsupported glTF version. Only glTF 2.0 is supported.");
            return false;
        }
        size_t off        = 12;
        bool   have_json  = false;
        while (off + 8 <= raw.size()) {
            uint32_t chunk_len = 0, chunk_type = 0;
            std::memcpy(&chunk_len, raw.data() + off, 4);
            std::memcpy(&chunk_type, raw.data() + off + 4, 4);
            const size_t data_off = off + 8;
            if (data_off + chunk_len > raw.size()) break;
            if (chunk_type == 0x4E4F534A) { // 'JSON'
                try {
                    doc.j = json::parse(raw.begin() + data_off, raw.begin() + data_off + chunk_len);
                } catch (const std::exception &e) {
                    message = _L("Malformed glTF JSON: ") + std::string(e.what());
                    return false;
                }
                have_json = true;
            } else if (chunk_type == 0x004E4942) { // 'BIN\0'
                glb_bin.assign(raw.begin() + data_off, raw.begin() + data_off + chunk_len);
                have_glb_bin = true;
            }
            off = data_off + chunk_len;
            if (chunk_len % 4 != 0) off += 4 - (chunk_len % 4); // chunks are 4-byte aligned
        }
        if (!have_json) {
            message = _L("The GLB file contains no JSON chunk.");
            return false;
        }
    } else {
        try {
            doc.j = json::parse(raw.begin(), raw.end());
        } catch (const std::exception &e) {
            message = _L("Malformed glTF JSON: ") + std::string(e.what());
            return false;
        }
    }

    // Resolve every declared buffer.
    const json &buffers = array_or_empty(doc.j, "buffers");
    doc.buffers.resize(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) {
        const json *uri_m = find_member(buffers[i], "uri");
        if (!uri_m || !uri_m->is_string()) {
            // No URI: this is the GLB binary chunk (only legal for buffer 0).
            if (have_glb_bin) doc.buffers[i] = glb_bin;
            continue;
        }
        if (!resolve_buffer_uri(doc, uri_m->get<std::string>(), doc.buffers[i]))
            BOOST_LOG_TRIVIAL(warning) << "glTF: could not resolve buffer " << i;
    }
    return true;
}

// Fetch the bytes of images[idx], wherever they live.
bool image_bytes(const GltfDocument &doc, int idx, std::vector<unsigned char> &out)
{
    const json &images = array_or_empty(doc.j, "images");
    if (idx < 0 || idx >= (int) images.size()) return false;
    const json &img = images[idx];

    if (const json *bv_m = find_member(img, "bufferView"); bv_m && bv_m->is_number()) {
        const json &bufferViews = array_or_empty(doc.j, "bufferViews");
        const int   bv_idx      = bv_m->get<int>();
        if (bv_idx < 0 || bv_idx >= (int) bufferViews.size()) return false;
        const json &                      bv  = bufferViews[bv_idx];
        const std::vector<unsigned char> *buf = doc.buffer((size_t) int_or(bv, "buffer", -1));
        if (!buf) return false;
        const size_t off = (size_t) int_or(bv, "byteOffset", 0);
        const size_t len = (size_t) int_or(bv, "byteLength", 0);
        if (off + len > buf->size()) return false;
        out.assign(buf->begin() + off, buf->begin() + off + len);
        return true;
    }
    if (const json *uri_m = find_member(img, "uri"); uri_m && uri_m->is_string())
        return resolve_buffer_uri(doc, uri_m->get<std::string>(), out);
    return false;
}

bool decode_image(const std::vector<unsigned char> &bytes, size_t max_dimension, Image &out)
{
    if (bytes.size() > 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G')
        return decode_png_rgba(bytes.data(), bytes.size(), out);
    if (bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8)
        return decode_jpeg(bytes.data(), bytes.size(), max_dimension, out);
    return false;
}

// Everything we need to colour the faces of one primitive.
struct MaterialInfo
{
    RGBA  base_color_factor{{1.f, 1.f, 1.f, 1.f}}; // linear
    int   base_color_texture{-1};                  // index into `textures`
    int   tex_coord_set{0};
    int   wrap_s{WRAP_REPEAT};
    int   wrap_t{WRAP_REPEAT};
};

MaterialInfo read_material(const GltfDocument &doc, int material_idx)
{
    MaterialInfo mi;
    const json & materials = array_or_empty(doc.j, "materials");
    if (material_idx < 0 || material_idx >= (int) materials.size()) return mi;
    const json &mat = materials[material_idx];

    const json *pbr = find_member(mat, "pbrMetallicRoughness");
    if (!pbr || !pbr->is_object()) return mi;

    if (const json *bcf = find_member(*pbr, "baseColorFactor"); bcf && bcf->is_array() && bcf->size() >= 3) {
        for (size_t i = 0; i < 4 && i < bcf->size(); ++i)
            mi.base_color_factor[i] = (*bcf)[i].get<float>();
    }
    if (const json *bct = find_member(*pbr, "baseColorTexture"); bct && bct->is_object()) {
        mi.base_color_texture = int_or(*bct, "index", -1);
        mi.tex_coord_set      = int_or(*bct, "texCoord", 0);

        // Resolve the sampler wrap modes via textures[i].sampler.
        const json &textures = array_or_empty(doc.j, "textures");
        if (mi.base_color_texture >= 0 && mi.base_color_texture < (int) textures.size()) {
            const int sampler_idx = int_or(textures[mi.base_color_texture], "sampler", -1);
            const json &samplers  = array_or_empty(doc.j, "samplers");
            if (sampler_idx >= 0 && sampler_idx < (int) samplers.size()) {
                mi.wrap_s = int_or(samplers[sampler_idx], "wrapS", WRAP_REPEAT);
                mi.wrap_t = int_or(samplers[sampler_idx], "wrapT", WRAP_REPEAT);
            }
        }
    }
    return mi;
}

// textures[i].source -> images[j]
int texture_image_index(const GltfDocument &doc, int texture_idx)
{
    const json &textures = array_or_empty(doc.j, "textures");
    if (texture_idx < 0 || texture_idx >= (int) textures.size()) return -1;
    const json &tex = textures[texture_idx];
    if (const json *src = find_member(tex, "source"); src && src->is_number()) return src->get<int>();
    // Compressed-texture extensions (e.g. KHR_texture_basisu) also carry a source.
    if (const json *ext = find_member(tex, "extensions"); ext && ext->is_object())
        for (auto it = ext->begin(); it != ext->end(); ++it)
            if (const json *src = find_member(it.value(), "source"); src && src->is_number()) return src->get<int>();
    return -1;
}

// ---------------------------------------------------------------------------
// Node transforms
// ---------------------------------------------------------------------------
Transform3d node_local_transform(const json &node)
{
    if (const json *m = find_member(node, "matrix"); m && m->is_array() && m->size() == 16) {
        Matrix4d mat;
        // glTF matrices are column-major.
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) mat(r, c) = (*m)[c * 4 + r].get<double>();
        return Transform3d(mat);
    }

    Transform3d t = Transform3d::Identity();
    Vec3d       translation(0., 0., 0.);
    Vec3d       scale(1., 1., 1.);
    Eigen::Quaterniond rotation(1., 0., 0., 0.); // w, x, y, z

    if (const json *m = find_member(node, "translation"); m && m->is_array() && m->size() == 3)
        translation = Vec3d((*m)[0].get<double>(), (*m)[1].get<double>(), (*m)[2].get<double>());
    if (const json *m = find_member(node, "scale"); m && m->is_array() && m->size() == 3)
        scale = Vec3d((*m)[0].get<double>(), (*m)[1].get<double>(), (*m)[2].get<double>());
    if (const json *m = find_member(node, "rotation"); m && m->is_array() && m->size() == 4)
        // glTF stores quaternions as (x, y, z, w).
        rotation = Eigen::Quaterniond((*m)[3].get<double>(), (*m)[0].get<double>(), (*m)[1].get<double>(), (*m)[2].get<double>());

    t.translate(translation);
    t.rotate(rotation);
    t.scale(scale);
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
bool load_gltf(const char *path, TriangleMesh *meshptr, GltfInfo &info, std::string &message)
{
    if (meshptr == nullptr) return false;

    GltfDocument doc;
    if (!parse_document(path, doc, message)) return false;

    indexed_triangle_set its;
    info = GltfInfo{};

    // Lazily decoded textures, keyed by image index.
    std::map<int, Image> decoded_images;
    std::set<int>        failed_images;
    // 8k albedo maps are common; sampling per face then clustering to a few filaments
    // makes anything past ~2k pointless, and keeps peak memory sane.
    const size_t max_texture_dimension = 2048;

    bool any_vertex_colors = false;
    // Only worth offering a colour mapping if the asset actually assigns materials;
    // a plain untextured mesh would otherwise show a dialog listing one white entry.
    bool any_material = false;

    // Collect per-vertex colours in parallel with vertices so indices stay aligned.
    std::vector<RGBA> vertex_colors;

    // Depth-first walk of the scene graph, accumulating transforms.
    const json &nodes  = array_or_empty(doc.j, "nodes");
    const json &meshes = array_or_empty(doc.j, "meshes");

    struct StackEntry
    {
        int         node;
        Transform3d parent;
    };
    std::vector<StackEntry> stack;
    std::vector<char>       visited(nodes.size(), 0);

    // Start from the default scene when present, otherwise from every root node.
    {
        const json &scenes    = array_or_empty(doc.j, "scenes");
        const int   scene_idx = int_or(doc.j, "scene", 0);
        bool        seeded    = false;
        if (scene_idx >= 0 && scene_idx < (int) scenes.size()) {
            for (const json &n : array_or_empty(scenes[scene_idx], "nodes")) {
                stack.push_back({n.get<int>(), Transform3d::Identity()});
                seeded = true;
            }
        }
        if (!seeded)
            for (size_t i = 0; i < nodes.size(); ++i) stack.push_back({(int) i, Transform3d::Identity()});
    }

    while (!stack.empty()) {
        const StackEntry entry = stack.back();
        stack.pop_back();
        if (entry.node < 0 || entry.node >= (int) nodes.size()) continue;
        if (visited[entry.node]) continue; // guards against malformed cyclic graphs
        visited[entry.node] = 1;

        const json &node   = nodes[entry.node];
        const Transform3d world = entry.parent * node_local_transform(node);

        for (const json &child : array_or_empty(node, "children")) stack.push_back({child.get<int>(), world});

        const int mesh_idx = int_or(node, "mesh", -1);
        if (mesh_idx < 0 || mesh_idx >= (int) meshes.size()) continue;

        // Normals transform by the inverse transpose; we only need it to detect mirroring.
        const bool flips_winding = world.linear().determinant() < 0.;

        for (const json &prim : array_or_empty(meshes[mesh_idx], "primitives")) {
            const int mode = int_or(prim, "mode", MODE_TRIANGLES);
            if (mode != MODE_TRIANGLES && mode != MODE_TRIANGLE_STRIP && mode != MODE_TRIANGLE_FAN) continue;

            const json *attrs = find_member(prim, "attributes");
            if (!attrs || !attrs->is_object()) continue;
            const int pos_acc = int_or(*attrs, "POSITION", -1);
            if (pos_acc < 0) continue;

            std::vector<float> positions;
            size_t             pos_comp = 0;
            if (!read_accessor_floats(doc, pos_acc, positions, pos_comp) || pos_comp != 3) continue;
            const size_t vertex_count = positions.size() / 3;
            if (vertex_count == 0) continue;

            // Indices - synthesise them when the primitive is non-indexed.
            std::vector<uint32_t> indices;
            if (const int idx_acc = int_or(prim, "indices", -1); idx_acc >= 0) {
                if (!read_accessor_indices(doc, idx_acc, indices)) continue;
            } else {
                indices.resize(vertex_count);
                for (size_t i = 0; i < vertex_count; ++i) indices[i] = (uint32_t) i;
            }

            // Expand strips and fans into independent triangles.
            std::vector<uint32_t> tri_indices;
            if (mode == MODE_TRIANGLES) {
                tri_indices = std::move(indices);
            } else if (indices.size() >= 3) {
                if (mode == MODE_TRIANGLE_STRIP) {
                    for (size_t i = 0; i + 2 < indices.size(); ++i) {
                        if (i % 2 == 0)
                            tri_indices.insert(tri_indices.end(), {indices[i], indices[i + 1], indices[i + 2]});
                        else
                            tri_indices.insert(tri_indices.end(), {indices[i + 1], indices[i], indices[i + 2]});
                    }
                } else { // fan
                    for (size_t i = 1; i + 1 < indices.size(); ++i)
                        tri_indices.insert(tri_indices.end(), {indices[0], indices[i], indices[i + 1]});
                }
            }
            if (tri_indices.size() < 3) continue;

            // Optional attributes.
            std::vector<float> uvs;
            size_t             uv_comp     = 0;
            const int          material_id = int_or(prim, "material", -1);
            if (material_id >= 0) any_material = true;
            const MaterialInfo mi = read_material(doc, material_id);
            const std::string  uv_key  = "TEXCOORD_" + std::to_string(mi.tex_coord_set);
            const bool         have_uv = read_accessor_floats(doc, int_or(*attrs, uv_key.c_str(), -1), uvs, uv_comp) && uv_comp == 2;

            std::vector<float> colors;
            size_t             color_comp = 0;
            const bool have_color = read_accessor_floats(doc, int_or(*attrs, "COLOR_0", -1), colors, color_comp) &&
                                    (color_comp == 3 || color_comp == 4);

            // Resolve the albedo texture once per primitive.
            const Image *image = nullptr;
            if (mi.base_color_texture >= 0) {
                const int img_idx = texture_image_index(doc, mi.base_color_texture);
                if (img_idx >= 0) {
                    auto it = decoded_images.find(img_idx);
                    if (it != decoded_images.end()) {
                        image = &it->second;
                    } else if (failed_images.count(img_idx) == 0) {
                        std::vector<unsigned char> bytes;
                        Image                      decoded;
                        if (image_bytes(doc, img_idx, bytes) && decode_image(bytes, max_texture_dimension, decoded)) {
                            image = &(decoded_images[img_idx] = std::move(decoded));
                        } else {
                            failed_images.insert(img_idx);
                            info.missing_textures = true;
                            BOOST_LOG_TRIVIAL(warning) << "glTF: could not decode image " << img_idx;
                        }
                    } else {
                        info.missing_textures = true;
                    }
                }
            }

            // Append geometry.
            const uint32_t vertex_base = (uint32_t) its.vertices.size();
            its.vertices.reserve(its.vertices.size() + vertex_count);
            for (size_t i = 0; i < vertex_count; ++i) {
                const Vec3d p = world * Vec3d(positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2]);
                its.vertices.emplace_back(p.cast<float>());
            }

            if (have_color) {
                any_vertex_colors = true;
                vertex_colors.resize(its.vertices.size(), RGBA{{1.f, 1.f, 1.f, 1.f}});
                for (size_t i = 0; i < vertex_count; ++i) {
                    RGBA c{{1.f, 1.f, 1.f, 1.f}};
                    for (size_t k = 0; k < color_comp; ++k) c[k] = colors[i * color_comp + k];
                    // COLOR_0 is linear; present it in sRGB like every other colour we hand out.
                    for (size_t k = 0; k < 3; ++k) c[k] = linear_to_srgb(std::min(std::max(c[k], 0.f), 1.f));
                    vertex_colors[vertex_base + i] = c;
                }
            } else if (any_vertex_colors) {
                // Keep the array aligned when only some primitives carry colours.
                vertex_colors.resize(its.vertices.size(), RGBA{{1.f, 1.f, 1.f, 1.f}});
            }

            // Append faces and resolve a colour for each one.
            const size_t tri_count = tri_indices.size() / 3;
            its.indices.reserve(its.indices.size() + tri_count);
            info.face_colors.reserve(info.face_colors.size() + tri_count);

            for (size_t t = 0; t < tri_count; ++t) {
                uint32_t a = tri_indices[t * 3 + 0];
                uint32_t b = tri_indices[t * 3 + 1];
                uint32_t c = tri_indices[t * 3 + 2];
                if (a >= vertex_count || b >= vertex_count || c >= vertex_count) continue;
                if (flips_winding) std::swap(b, c);
                its.indices.emplace_back(vertex_base + a, vertex_base + b, vertex_base + c);

                // Start from the material's flat colour (linear space).
                float fr = mi.base_color_factor[0];
                float fg = mi.base_color_factor[1];
                float fb = mi.base_color_factor[2];
                float fa = mi.base_color_factor[3];

                if (image != nullptr && have_uv) {
                    // Sample at the UV centroid: one representative colour per facet.
                    const float u = (uvs[a * 2 + 0] + uvs[b * 2 + 0] + uvs[c * 2 + 0]) / 3.f;
                    const float v = (uvs[a * 2 + 1] + uvs[b * 2 + 1] + uvs[c * 2 + 1]) / 3.f;
                    const float uw = wrap_coord(u, mi.wrap_s);
                    const float vw = wrap_coord(v, mi.wrap_t);
                    size_t      px = (size_t) (uw * (float) (image->width - 1) + 0.5f);
                    size_t      py = (size_t) (vw * (float) (image->height - 1) + 0.5f);
                    px            = std::min(px, image->width - 1);
                    py            = std::min(py, image->height - 1);
                    const unsigned char *texel = image->rgba.data() + (py * image->width + px) * 4;
                    // Texels are sRGB-encoded; multiply with the factor in linear space.
                    fr *= srgb_to_linear(texel[0] / 255.f);
                    fg *= srgb_to_linear(texel[1] / 255.f);
                    fb *= srgb_to_linear(texel[2] / 255.f);
                    fa *= texel[3] / 255.f;
                }

                info.face_colors.push_back(RGBA{{linear_to_srgb(std::min(std::max(fr, 0.f), 1.f)),
                                                 linear_to_srgb(std::min(std::max(fg, 0.f), 1.f)),
                                                 linear_to_srgb(std::min(std::max(fb, 0.f), 1.f)),
                                                 std::min(std::max(fa, 0.f), 1.f)}});
                if (image != nullptr) info.from_texture = true;
            }
        }
    }

    if (its.vertices.empty() || its.indices.empty()) {
        message = _L("The glTF file contains no printable triangle geometry.");
        return false;
    }

    // glTF is Y-up, right-handed and expressed in meters; the slicer is Z-up in millimeters.
    for (Vec3f &v : its.vertices) {
        const float y = v.y();
        const float z = v.z();
        v.y()         = -z;
        v.z()         = y;
        v *= (float) GLTF_UNITS_TO_MM;
    }

    if (any_vertex_colors) {
        vertex_colors.resize(its.vertices.size(), RGBA{{1.f, 1.f, 1.f, 1.f}});
        info.vertex_colors = std::move(vertex_colors);
    }
    if (!any_material) info.face_colors.clear();

    // Let the caller skip the colour dialog when there is only ever one colour.
    if (!info.face_colors.empty()) {
        const RGBA &first = info.face_colors.front();
        info.is_single_color =
            std::all_of(info.face_colors.begin(), info.face_colors.end(), [&first](const RGBA &c) { return c == first; });
    }

    *meshptr = TriangleMesh(std::move(its));
    if (meshptr->volume() < 0) meshptr->flip_triangles();
    return true;
}

bool load_gltf(const char *path, Model *model, GltfInfo &info, std::string &message, const char *object_name_in)
{
    TriangleMesh mesh;

    if (!load_gltf(path, &mesh, info, message)) return false;

    std::string object_name;
    if (object_name_in == nullptr) {
        const char *last_slash = strrchr(path, DIR_SEPARATOR);
        object_name.assign((last_slash == nullptr) ? path : last_slash + 1);
    } else
        object_name.assign(object_name_in);

    model->add_object(object_name.c_str(), path, std::move(mesh));
    return true;
}

}; // namespace Slic3r
