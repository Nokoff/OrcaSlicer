#include "MyFilesLibrary.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/base64.h>
#include <wx/dirdlg.h>
#include <wx/datetime.h>

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

constexpr const char *k_config_key = "my_files_path";
constexpr int         k_max_files  = 300;
constexpr int         k_max_depth  = 2;

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

std::string thumbnail_data_url(const std::string &path)
{
    std::string png = bbs_3mf_get_thumbnail(path.c_str());
    if (png.empty())
        return {};
    std::stringstream ss;
    ss << "data:image/png;base64,";
    ss << wxBase64Encode(png.data(), png.size());
    return ss.str();
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

    const int max_thumbs  = 80;
    const int image_limit = images < 0 ? max_thumbs : std::min(images, max_thumbs);
    for (size_t i = 0; i < files.size(); ++i) {
        nlohmann::json item;
        const std::string path_u8 = files[i].path.string();
        item["project_name"]      = files[i].path.filename().string();
        item["path"]              = path_u8;
        if (files[i].mtime > 0) {
            item["time"] = wxDateTime(files[i].mtime).FormatISOCombined(' ').ToStdString();
            if ((int) i < image_limit) {
                auto thumb = thumbnail_data_url(path_u8);
                if (!thumb.empty())
                    item["image"] = thumb;
            }
        } else {
            item["time"] = "File is missing";
        }
        out.push_back(std::move(item));
    }
}

} // namespace GUI
} // namespace Slic3r
