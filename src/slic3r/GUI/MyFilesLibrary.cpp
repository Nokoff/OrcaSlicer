#include "MyFilesLibrary.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "WebViewDialog.hpp"
#include "slic3r/Utils/Http.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/base64.h>
#include <wx/dirdlg.h>
#include <wx/datetime.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/mstream.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

constexpr const char *k_config_key = "my_files_path";
constexpr int         k_max_files  = 300;
constexpr int         k_max_depth  = 2;
constexpr unsigned    k_thumb_size = 192;
constexpr int         k_max_thumbs = 300;
// Bumped whenever the renderer changes so previously cached (and possibly broken) tiles are redrawn.
constexpr int         k_thumb_cache_version = 2;

constexpr float k_inv_sqrt2 = 0.70710678f;
constexpr float k_inv_sqrt3 = 0.57735027f;
constexpr float k_inv_sqrt6 = 0.40824829f;

// Thumbnail generation runs on background threads: loading and rasterising a model takes
// long enough that doing it on the UI thread froze the whole application (including the 3D
// viewport) until the whole library had been processed.
std::mutex               g_thumb_mutex;
std::condition_variable  g_thumb_cv;
std::deque<std::string>  g_pending_thumbs;   // guarded by g_thumb_mutex
unsigned                 g_thumb_generation = 0; // bumped when the list is rebuilt
int                      g_thumb_total = 0;   // queue size of the current generation, for progress
int                      g_thumb_done  = 0;
std::vector<std::thread> g_thumb_workers;
bool                     g_thumb_workers_started = false; // guarded by g_thumb_mutex
std::atomic<bool>        g_thumb_abort{false};

// Kept deliberately small: each worker can hold a fully expanded mesh, so a big library of
// large STLs would otherwise multiply peak memory by the thread count.
size_t thumbnail_worker_count()
{
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 2;
    return (size_t) std::min<unsigned>(3u, std::max<unsigned>(1u, hw / 2));
}

const std::unordered_set<std::string> &supported_extensions()
{
    static const std::unordered_set<std::string> exts = {
        ".3mf", ".stl", ".oltp", ".stp", ".step", ".svg", ".amf", ".obj",
#ifdef __APPLE__
        ".usd", ".usda", ".usdc", ".usdz", ".abc", ".ply",
#endif
    };
    return exts;
}

std::string lower_extension(const std::string &path)
{
    std::string ext = fs::path(path).extension().string();
    boost::algorithm::to_lower(ext);
    return ext;
}

bool can_generate_mesh_thumbnail(const std::string &path)
{
    const std::string ext = lower_extension(path);
    // Mesh formats we can load quickly enough for library icons.
    // STEP/SVG are skipped (too heavy / not triangle meshes).
    return ext == ".stl" || ext == ".oltp" || ext == ".obj" || ext == ".glb" || ext == ".gltf" || ext == ".3mf" || ext == ".amf"
#ifdef __APPLE__
           || ext == ".ply"
#endif
        ;
}

std::string to_data_url_png(const void *data, size_t size)
{
    if (!data || size == 0)
        return {};
    std::stringstream ss;
    ss << "data:image/png;base64,";
    ss << wxBase64Encode(data, size);
    return ss.str();
}

fs::path thumbnail_cache_dir()
{
    return fs::path(data_dir()) / "cache" / "my_files_thumbs";
}

std::string cache_file_name(const std::string &path, std::time_t mtime)
{
    const size_t h = std::hash<std::string>{}(path) ^ (static_cast<size_t>(mtime) * 1315423911ull);
    std::ostringstream ss;
    ss << std::hex << h << "_" << static_cast<long long>(mtime) << "_v" << k_thumb_cache_version << ".png";
    return ss.str();
}

fs::path cache_path_for(const std::string &path, std::time_t mtime)
{
    return thumbnail_cache_dir() / cache_file_name(path, mtime);
}

bool save_png_file(const fs::path &png_path, const void *data, size_t size)
{
    if (!data || size == 0)
        return false;
    boost::system::error_code ec;
    fs::create_directories(png_path.parent_path(), ec);
    boost::nowide::ofstream ofs(png_path.string(), std::ios::binary);
    if (!ofs)
        return false;
    ofs.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(ofs);
}

// Embedded 3MF covers are full-size plate renders (often 300-400 kB each). Decode them,
// scale down to tile size and re-encode, so 300 of them stay cheap to serve and display.
bool save_scaled_png_file(const fs::path &png_path, const std::string &image_bytes)
{
    if (image_bytes.empty())
        return false;

    // Runs on worker threads: use the per-image quiet flag rather than wxLogNull, which
    // toggles a global logging counter and would corrupt it when threads overlap.
    wxImage img;
    img.SetLoadFlags(img.GetLoadFlags() & ~wxImage::Load_Verbose);
    {
        wxMemoryInputStream mis(image_bytes.data(), image_bytes.size());
        if (!img.LoadFile(mis, wxBITMAP_TYPE_ANY))
            return false;
    }
    if (!img.IsOk() || img.GetWidth() <= 0 || img.GetHeight() <= 0)
        return false;

    const int longest = std::max(img.GetWidth(), img.GetHeight());
    if (longest > (int) k_thumb_size) {
        const double f = double(k_thumb_size) / double(longest);
        img            = img.Scale(std::max(1, (int) std::lround(img.GetWidth() * f)),
                                   std::max(1, (int) std::lround(img.GetHeight() * f)), wxIMAGE_QUALITY_HIGH);
        if (!img.IsOk())
            return false;
    }

    boost::system::error_code ec;
    fs::create_directories(png_path.parent_path(), ec);
    return img.SaveFile(wxString::FromUTF8(png_path.string()), wxBITMAP_TYPE_PNG);
}

// Returns true when a cover was extracted from the package and cached.
bool cache_embedded_3mf_thumbnail(const std::string &path, const fs::path &cache)
{
    std::string png;
    try {
        png = bbs_3mf_get_thumbnail(path.c_str());
    } catch (...) {
        return false;
    }
    if (png.empty())
        return false;
    return save_scaled_png_file(cache, png);
}

std::string localfile_url_for_path(const fs::path &abs_path)
{
    return std::string("/localfile/") + Http::url_encode(abs_path.string());
}

// CPU isometric preview — reliable without a live OpenGL context on the home page.
std::string render_model_thumbnail_png_bytes(const Model &model)
{
    struct Part {
        const indexed_triangle_set *its{nullptr};
        Transform3f                 trafo{Transform3f::Identity()};
    };
    std::vector<Part> parts;
    BoundingBoxf3     bbox;

    auto collect = [&](bool model_parts_only) {
        parts.clear();
        bbox = BoundingBoxf3();
        for (const ModelObject *object : model.objects) {
            if (!object)
                continue;
            for (const ModelVolume *volume : object->volumes) {
                if (!volume || (model_parts_only && !volume->is_model_part()))
                    continue;
                const TriangleMesh &mesh = volume->mesh();
                if (mesh.its.indices.empty())
                    continue;
                for (const ModelInstance *instance : object->instances) {
                    if (!instance)
                        continue;
                    const Transform3d trafo = instance->get_matrix() * volume->get_matrix();
                    bbox.merge(mesh.transformed_bounding_box(trafo));
                    parts.push_back({&mesh.its, trafo.cast<float>()});
                }
            }
        }
    };

    collect(true);
    if (parts.empty())
        collect(false); // Some packages only carry modifier / negative volumes.
    if (parts.empty() || !bbox.defined)
        return {};

    const unsigned w = k_thumb_size;
    const unsigned h = k_thumb_size;
    ThumbnailData  data;
    data.set(w, h);
    // Transparent background so the tile styling shows through in both themes.
    std::fill(data.pixels.begin(), data.pixels.end(), (unsigned char) 0);
    std::vector<float> zbuf(size_t(w) * size_t(h), -std::numeric_limits<float>::infinity());

    // Isometric camera sitting at (+1,+1,+1) looking at the model, world Z pointing up on screen.
    // right = (-1,1,0)/sqrt(2), up = (-1,-1,2)/sqrt(6), depth = (1,1,1)/sqrt(3) towards the camera.
    const Vec3f center = bbox.center().cast<float>();

    auto view = [&center](const Vec3f &p) -> Vec3f {
        const Vec3f q = p - center;
        return Vec3f((q.y() - q.x()) * k_inv_sqrt2,
                     (2.f * q.z() - q.x() - q.y()) * k_inv_sqrt6,
                     (q.x() + q.y() + q.z()) * k_inv_sqrt3);
    };

    // Fit the projected bounding box exactly instead of guessing from its longest edge,
    // which used to crop tall or diagonally elongated models.
    float min_r = std::numeric_limits<float>::max();
    float max_r = std::numeric_limits<float>::lowest();
    float min_u = min_r;
    float max_u = max_r;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3f p(float((corner & 1) ? bbox.max.x() : bbox.min.x()),
                      float((corner & 2) ? bbox.max.y() : bbox.min.y()),
                      float((corner & 4) ? bbox.max.z() : bbox.min.z()));
        const Vec3f v = view(p);
        min_r = std::min(min_r, v.x());
        max_r = std::max(max_r, v.x());
        min_u = std::min(min_u, v.y());
        max_u = std::max(max_u, v.y());
    }

    const float margin = 0.08f * float(w);
    const float scale  = std::min((float(w) - 2.f * margin) / std::max(max_r - min_r, 1e-3f),
                                  (float(h) - 2.f * margin) / std::max(max_u - min_u, 1e-3f));
    const float mid_r  = 0.5f * (min_r + max_r);
    const float mid_u  = 0.5f * (min_u + max_u);

    // NOTE: rows are written bottom-up because compress_thumbnail_png() flips the buffer
    // vertically (it expects an OpenGL framebuffer).
    auto project = [&](const Vec3f &p) -> Vec3f {
        const Vec3f v = view(p);
        return Vec3f((v.x() - mid_r) * scale + float(w) * 0.5f,
                     (v.y() - mid_u) * scale + float(h) * 0.5f,
                     v.z());
    };

    const Vec3f light = Vec3f(-0.35f, 0.25f, 0.9f).normalized();

    auto put_pixel = [&](int x, int y, float z, float shade) {
        if (x < 0 || y < 0 || x >= (int) w || y >= (int) h)
            return;
        const size_t idx = size_t(y) * w + size_t(x);
        if (z <= zbuf[idx])
            return;
        zbuf[idx] = z;
        const float s = std::clamp(shade, 0.2f, 1.f);
        // Soft teal-gray shaded mesh.
        data.pixels[idx * 4 + 0] = (unsigned char) std::clamp(int(70 + 140 * s), 0, 255);
        data.pixels[idx * 4 + 1] = (unsigned char) std::clamp(int(90 + 145 * s), 0, 255);
        data.pixels[idx * 4 + 2] = (unsigned char) std::clamp(int(95 + 140 * s), 0, 255);
        data.pixels[idx * 4 + 3] = 255;
    };

    size_t faces_done = 0;
    for (const Part &part : parts) {
        for (const Vec3i32 &face : part.its->indices) {
            // Keep app shutdown from waiting on a multi-million triangle render.
            if ((++faces_done & 0xFFFF) == 0 && g_thumb_abort.load())
                return {};

            const Vec3f v0 = part.trafo * part.its->vertices[face(0)];
            const Vec3f v1 = part.trafo * part.its->vertices[face(1)];
            const Vec3f v2 = part.trafo * part.its->vertices[face(2)];

            Vec3f       n    = (v1 - v0).cross(v2 - v0);
            const float nlen = n.norm();
            if (nlen < 1e-12f)
                continue;
            n /= nlen;

            const Vec3f p0 = project(v0);
            const Vec3f p1 = project(v1);
            const Vec3f p2 = project(v2);

            const float area = (p1.x() - p0.x()) * (p2.y() - p0.y()) - (p2.x() - p0.x()) * (p1.y() - p0.y());
            if (std::fabs(area) < 1e-6f)
                continue;

            // No back-face culling: the depth buffer already resolves visibility, and open or
            // inconsistently wound meshes (AI/scan exports) would otherwise render empty.
            // Two-sided lighting for the same reason.
            const float shade = 0.35f + 0.65f * std::fabs(n.dot(light));

            const int minx = std::max(0, (int) std::floor(std::min({p0.x(), p1.x(), p2.x()})));
            const int maxx = std::min((int) w - 1, (int) std::ceil(std::max({p0.x(), p1.x(), p2.x()})));
            const int miny = std::max(0, (int) std::floor(std::min({p0.y(), p1.y(), p2.y()})));
            const int maxy = std::min((int) h - 1, (int) std::ceil(std::max({p0.y(), p1.y(), p2.y()})));

            for (int y = miny; y <= maxy; ++y) {
                for (int x = minx; x <= maxx; ++x) {
                    const float px = float(x) + 0.5f;
                    const float py = float(y) + 0.5f;
                    const float w0 = ((p1.x() - px) * (p2.y() - py) - (p2.x() - px) * (p1.y() - py)) / area;
                    const float w1 = ((p2.x() - px) * (p0.y() - py) - (p0.x() - px) * (p2.y() - py)) / area;
                    const float w2 = 1.f - w0 - w1;
                    if (w0 < 0.f || w1 < 0.f || w2 < 0.f)
                        continue;
                    const float z = w0 * p0.z() + w1 * p1.z() + w2 * p2.z();
                    put_pixel(x, y, z, shade);
                }
            }
        }
    }

    auto compressed = GCodeThumbnails::compress_thumbnail(data, GCodeThumbnailsFormat::PNG);
    if (!compressed || !compressed->data || compressed->size == 0)
        return {};
    return std::string(reinterpret_cast<const char *>(compressed->data), compressed->size);
}

bool load_model_for_thumbnail(const std::string &path, Model &model)
{
    try {
        model = Model::read_from_file(path, nullptr, nullptr,
                                      LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances);
        if (!model.objects.empty())
            return true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(debug) << "My Files: read_from_file failed for " << path << ": " << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(debug) << "My Files: read_from_file failed for " << path;
    }

    // read_from_file() only ever tries load_bbs_3mf(); PrusaSlicer-flavoured packages need the
    // archive path, which sniffs the producer first. Without this they never get a preview.
    if (lower_extension(path) != ".3mf")
        return false;

    try {
        model = Model();
        DynamicPrintConfig        config;
        ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
        En3mfType                 type = En3mfType::From_Other;
        model = Model::read_from_archive(path, &config, &substitutions, type,
                                         LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances);
        return !model.objects.empty();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "My Files: failed to load 3mf for thumbnail " << path << ": " << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "My Files: failed to load 3mf for thumbnail " << path;
    }
    return false;
}

// Produces the cached tile for one file: embedded 3MF cover when there is one, otherwise a
// rendered mesh preview. Returns the URL to serve, or an empty string when nothing could be made.
std::string generate_thumbnail_url(const std::string &path, std::time_t mtime)
{
    if (mtime <= 0)
        return {};

    const fs::path cache = cache_path_for(path, mtime);

    if (lower_extension(path) == ".3mf" && cache_embedded_3mf_thumbnail(path, cache))
        return localfile_url_for_path(cache);

    Model model;
    if (!load_model_for_thumbnail(path, model)) {
        BOOST_LOG_TRIVIAL(warning) << "My Files: no model to render a thumbnail from for " << path;
        return {};
    }

    const std::string png_bytes = render_model_thumbnail_png_bytes(model);
    if (png_bytes.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "My Files: CPU thumbnail render produced no image for " << path;
        return {};
    }

    if (!save_png_file(cache, png_bytes.data(), png_bytes.size())) {
        BOOST_LOG_TRIVIAL(warning) << "My Files: failed to write thumbnail cache " << cache.string();
        return to_data_url_png(png_bytes.data(), png_bytes.size());
    }
    return localfile_url_for_path(cache);
}

// Returns the cached tile URL, or queues the file into `enqueue` (the caller hands the batch to
// the worker under g_thumb_mutex) and returns empty.
std::string thumbnail_data_url(const std::string &path, std::time_t mtime, std::vector<std::string> *enqueue)
{
    // Everything is served from the disk cache: opening 300 packages (and inlining their
    // 300-400 kB covers as base64) while building the list froze the UI and produced a
    // RunScript payload far too large for the web view to accept.
    if (mtime > 0) {
        const fs::path            cache = cache_path_for(path, mtime);
        boost::system::error_code ec;
        if (fs::is_regular_file(cache, ec) && !ec)
            return localfile_url_for_path(cache);
    }

    if (enqueue && mtime > 0 && can_generate_mesh_thumbnail(path))
        enqueue->push_back(path);

    return {};
}
struct FileEntry {
    fs::path    path;
    std::time_t mtime{0};
};

void collect_recursive(const fs::path &dir, int depth, std::vector<FileEntry> &out)
{
    if (depth > k_max_depth || (int) out.size() >= k_max_files)
        return;

    boost::system::error_code ec;
    if (!fs::is_directory(dir, ec) || ec)
        return;

    fs::directory_iterator it(dir, ec), end;
    if (ec)
        return;

    for (; it != end && (int) out.size() < k_max_files; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::path &p = it->path();
        boost::system::error_code status_ec;
        if (fs::is_directory(p, status_ec)) {
            if (!status_ec)
                collect_recursive(p, depth + 1, out);
            continue;
        }
        if (!MyFilesLibrary::is_supported_model_file(p.string()))
            continue;

        FileEntry entry;
        entry.path = p;
        try {
            entry.mtime = fs::last_write_time(p, status_ec);
            if (status_ec)
                entry.mtime = 0;
        } catch (...) {
            entry.mtime = 0;
        }
        out.push_back(std::move(entry));
    }
}

// Must be called on the UI thread.
void dispatch_to_page(const nlohmann::json &req)
{
    WebViewPanel *webview = wxGetApp().mainframe ? wxGetApp().mainframe->m_webview : nullptr;
    if (!webview)
        return;
    const wxString payload = wxString::FromUTF8(req.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore));
    webview->RunScript(wxString::Format(
        "window.dispatchMyFilesMessage ? window.dispatchMyFilesMessage(%s) : window.postMessage(%s)", payload, payload));
}

// Called from the worker thread; hops onto the UI thread to touch the web view.
void post_thumbnail_update(const std::string &path, const std::string &image_url, int done, int total)
{
    nlohmann::json req;
    req["sequence_id"] = "";
    req["command"]     = "my_files_thumbnail";
    req["path"]        = path;
    req["image"]       = image_url;
    req["done"]        = done;
    req["total"]       = total;
    wxGetApp().CallAfter([req] { dispatch_to_page(req); });
}

// done == total tells the page to hide the progress bar.
void post_thumbnail_progress(int done, int total)
{
    nlohmann::json req;
    req["sequence_id"] = "";
    req["command"]     = "my_files_thumbnail_progress";
    req["done"]        = done;
    req["total"]       = total;
    wxGetApp().CallAfter([req] { dispatch_to_page(req); });
}

// Long-lived workers that sleep while the queue is empty and are joined at app exit.
// Everything they touch is either thread-local or guarded by g_thumb_mutex; the wxImage
// handlers they use are registered once at startup (wxInitAllImageHandlers) and are read-only
// from here on.
void thumbnail_worker_main()
{
    for (;;) {
        std::string path;
        unsigned    generation = 0;

        {
            std::unique_lock<std::mutex> lock(g_thumb_mutex);
            g_thumb_cv.wait(lock, [] { return g_thumb_abort.load() || !g_pending_thumbs.empty(); });
            if (g_thumb_abort.load())
                return;
            path       = g_pending_thumbs.front();
            generation = g_thumb_generation;
            g_pending_thumbs.pop_front();
        }

        std::string image_url;
        try {
            boost::system::error_code ec;
            std::time_t               mtime = fs::last_write_time(path, ec);
            if (ec)
                mtime = 0;
            if (mtime > 0) {
                const fs::path            cache = cache_path_for(path, mtime);
                boost::system::error_code exists_ec;
                image_url = (fs::is_regular_file(cache, exists_ec) && !exists_ec) ? localfile_url_for_path(cache)
                                                                                 : generate_thumbnail_url(path, mtime);
            }
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "My Files: thumbnail generation failed for " << path << ": " << e.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "My Files: thumbnail generation failed for " << path;
        }

        if (g_thumb_abort.load())
            return;

        int done  = 0;
        int total = 0;
        {
            std::lock_guard<std::mutex> lock(g_thumb_mutex);
            if (generation != g_thumb_generation)
                continue; // The list was rebuilt underneath us; this result is stale.
            done  = ++g_thumb_done;
            total = g_thumb_total;
        }

        if (image_url.empty())
            post_thumbnail_progress(done, total);
        else
            post_thumbnail_update(path, image_url, done, total);
    }
}

} // namespace

std::string MyFilesLibrary::get_folder_path()
{
    if (!wxGetApp().app_config)
        return {};
    return wxGetApp().app_config->get(k_config_key);
}

void MyFilesLibrary::set_folder_path(const std::string &path)
{
    if (!wxGetApp().app_config)
        return;
    wxGetApp().app_config->set(k_config_key, path);
    wxGetApp().app_config->save();
}

bool MyFilesLibrary::select_folder(wxWindow *parent)
{
    std::string current = get_folder_path();
    wxString    start   = current.empty() ? wxString() : wxString::FromUTF8(current);

    wxDirDialog dialog(parent, _L("Choose My Files Folder"), start,
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST | wxDD_NEW_DIR_BUTTON);
    if (dialog.ShowModal() != wxID_OK)
        return false;

    std::string path = dialog.GetPath().ToUTF8().data();
    set_folder_path(path);
    return true;
}

namespace {

std::vector<std::string> filter_library_paths(const std::vector<std::string> &paths)
{
    std::vector<std::string> out;
    out.reserve(paths.size());
    for (const auto &path : paths) {
        if (MyFilesLibrary::is_file_in_library(path))
            out.push_back(path);
    }
    return out;
}

bool move_one_file(const fs::path &from, const fs::path &to, std::string &error)
{
    boost::system::error_code ec;
    fs::rename(from, to, ec);
    if (!ec)
        return true;

    // Cross-volume moves fall back to copy + delete.
    if (copy_file(from.string(), to.string(), error, false) != SUCCESS)
        return false;
    fs::remove(from, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

} // namespace

bool MyFilesLibrary::delete_files(wxWindow *parent, const std::vector<std::string> &paths)
{
    const auto targets = filter_library_paths(paths);
    if (targets.empty())
        return false;

    wxString message;
    if (targets.size() == 1) {
        message = wxString::Format(_L("Permanently delete \"%s\"?\n\nThis cannot be undone."),
                                   wxString::FromUTF8(fs::path(targets.front()).filename().string()));
    } else {
        message = wxString::Format(_L("Permanently delete %d selected files?\n\nThis cannot be undone."),
                                   int(targets.size()));
    }

    MessageDialog dlg(parent, message, _L("Delete Files"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES)
        return false;

    int failed = 0;
    for (const auto &path : targets) {
        boost::system::error_code ec;
        if (!fs::remove(fs::path(path), ec) || ec) {
            ++failed;
            BOOST_LOG_TRIVIAL(warning) << "My Files: failed to delete " << path << ": " << ec.message();
        }
    }

    if (failed > 0) {
        MessageDialog(parent,
                      wxString::Format(_L("Could not delete %d of %d files. They may be open or in use."), failed,
                                       int(targets.size())),
                      _L("Delete Files"), wxOK | wxICON_WARNING)
            .ShowModal();
    }
    return failed < (int) targets.size();
}

bool MyFilesLibrary::move_files(wxWindow *parent, const std::vector<std::string> &paths)
{
    const auto targets = filter_library_paths(paths);
    if (targets.empty())
        return false;

    std::string current = get_folder_path();
    wxString    start   = current.empty() ? wxString() : wxString::FromUTF8(current);
    wxDirDialog dialog(parent, _L("Move Files To Folder"), start,
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST | wxDD_NEW_DIR_BUTTON);
    if (dialog.ShowModal() != wxID_OK)
        return false;

    const fs::path dest_dir(dialog.GetPath().ToUTF8().data());
    boost::system::error_code ec;
    if (!fs::is_directory(dest_dir, ec) || ec)
        return false;

    // Avoid no-op moves into the same directory for every file.
    int moved = 0;
    int failed = 0;
    int skipped = 0;
    for (const auto &path : targets) {
        const fs::path from(path);
        const fs::path to = dest_dir / from.filename();
        if (fs::equivalent(from, to, ec)) {
            ++skipped;
            continue;
        }
        if (fs::exists(to, ec)) {
            MessageDialog overwrite_dlg(
                parent,
                wxString::Format(_L("\"%s\" already exists in the destination folder. Overwrite it?"),
                                 wxString::FromUTF8(to.filename().string())),
                _L("Move Files"), wxYES_NO | wxCANCEL | wxNO_DEFAULT | wxICON_WARNING);
            const int answer = overwrite_dlg.ShowModal();
            if (answer == wxID_CANCEL)
                break;
            if (answer != wxID_YES) {
                ++skipped;
                continue;
            }
            fs::remove(to, ec);
        }

        std::string error;
        if (move_one_file(from, to, error)) {
            ++moved;
        } else {
            ++failed;
            BOOST_LOG_TRIVIAL(warning) << "My Files: failed to move " << path << " -> " << to.string() << ": " << error;
        }
    }

    if (failed > 0) {
        MessageDialog(parent,
                      wxString::Format(_L("Moved %d file(s). %d failed."), moved, failed),
                      _L("Move Files"), wxOK | wxICON_WARNING)
            .ShowModal();
    }
    return moved > 0;
}

bool MyFilesLibrary::is_supported_model_file(const std::string &path)
{
    std::string ext = fs::path(path).extension().string();
    boost::algorithm::to_lower(ext);
    return supported_extensions().count(ext) > 0;
}

bool MyFilesLibrary::is_file_in_library(const std::string &path)
{
    const std::string folder = get_folder_path();
    if (folder.empty() || !is_supported_model_file(path))
        return false;

    boost::system::error_code ec;
    const fs::path root = fs::canonical(fs::path(folder), ec);
    if (ec)
        return false;
    const fs::path file = fs::canonical(fs::path(path), ec);
    if (ec || !fs::is_regular_file(file, ec) || ec)
        return false;

    auto root_it = root.begin();
    auto file_it = file.begin();
    for (; root_it != root.end(); ++root_it, ++file_it) {
        if (file_it == file.end() || *root_it != *file_it)
            return false;
    }
    return true;
}

void MyFilesLibrary::collect_files(nlohmann::json &out, int images)
{
    out = nlohmann::json::array();
    {
        // Drop whatever the previous list queued; results still in flight are discarded by
        // the generation check in the worker.
        std::lock_guard<std::mutex> lock(g_thumb_mutex);
        g_pending_thumbs.clear();
        ++g_thumb_generation;
        g_thumb_total = 0;
        g_thumb_done  = 0;
    }

    const std::string folder = get_folder_path();
    if (folder.empty())
        return;

    boost::system::error_code ec;
    fs::path                  root(folder);
    if (!fs::is_directory(root, ec) || ec) {
        BOOST_LOG_TRIVIAL(warning) << "My Files folder missing or inaccessible: " << folder;
        return;
    }

    std::vector<FileEntry> files;
    files.reserve(64);
    collect_recursive(root, 0, files);

    std::sort(files.begin(), files.end(), [](const FileEntry &a, const FileEntry &b) {
        if (a.mtime != b.mtime)
            return a.mtime > b.mtime;
        return a.path.filename().string() < b.path.filename().string();
    });

    std::vector<std::string> queued;
    const int image_limit = images < 0 ? k_max_thumbs : std::min(images, k_max_thumbs);
    for (size_t i = 0; i < files.size(); ++i) {
        nlohmann::json item;
        const std::string path_u8 = files[i].path.string();
        item["project_name"]      = files[i].path.filename().string();
        item["path"]              = path_u8;
        if (files[i].mtime > 0) {
            item["time"] = wxDateTime(files[i].mtime).FormatISOCombined(' ').ToStdString();
            if ((int) i < image_limit) {
                auto thumb = thumbnail_data_url(path_u8, files[i].mtime, &queued);
                if (!thumb.empty())
                    item["image"] = thumb;
            }
        } else {
            item["time"] = "File is missing";
        }
        out.push_back(std::move(item));
    }

    std::lock_guard<std::mutex> lock(g_thumb_mutex);
    g_pending_thumbs.assign(queued.begin(), queued.end());
    g_thumb_total = (int) g_pending_thumbs.size();
}

void MyFilesLibrary::pump_thumbnail_generation(WebViewPanel *webview)
{
    if (!webview || g_thumb_abort.load())
        return;

    int total = 0;
    {
        std::lock_guard<std::mutex> lock(g_thumb_mutex);
        if (g_pending_thumbs.empty())
            return;
        total = g_thumb_total;
        if (!g_thumb_workers_started) {
            g_thumb_workers_started = true;
            const size_t n          = thumbnail_worker_count();
            g_thumb_workers.reserve(n);
            for (size_t i = 0; i < n; ++i)
                g_thumb_workers.emplace_back(thumbnail_worker_main);
            BOOST_LOG_TRIVIAL(info) << "My Files: started " << n << " thumbnail workers";
        }
    }
    // Show the bar straight away; the workers take over reporting from here.
    post_thumbnail_progress(0, total);
    g_thumb_cv.notify_all();
}

void MyFilesLibrary::stop_thumbnail_generation()
{
    {
        std::lock_guard<std::mutex> lock(g_thumb_mutex);
        g_thumb_abort.store(true);
        g_pending_thumbs.clear();
    }
    g_thumb_cv.notify_all();
    for (auto &worker : g_thumb_workers) {
        if (worker.joinable())
            worker.join();
    }

    std::lock_guard<std::mutex> lock(g_thumb_mutex);
    g_thumb_workers.clear();
    g_thumb_workers_started = false;
}

} // namespace GUI
} // namespace Slic3r
