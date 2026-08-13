#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class wxWindow;

namespace Slic3r {
namespace GUI {

class WebViewPanel;

// Local folder library shown from the "My Files" home navigation item.
// Folder path is persisted in AppConfig key "my_files_path".
class MyFilesLibrary
{
public:
    static std::string get_folder_path();
    static void        set_folder_path(const std::string &path);

    // Returns true if the user selected a folder (and it was saved).
    static bool select_folder(wxWindow *parent);

    // Sub-folder currently being browsed, relative to the mapped root ("" = root itself).
    // Kept here so a refresh after delete / move / thumbnail work stays where the user is.
    static std::string get_current_subdir();
    // `rel` comes from the web page: it is sanitised and silently falls back to the root when it
    // does not name an existing directory inside the mapped folder.
    static void        set_current_subdir(const std::string &rel);
    // Absolute path of the folder currently being browsed.
    static std::string get_current_dir();

    // When on, the listing also carries every model file below the current folder instead of only
    // the ones directly inside it. Persisted in AppConfig key "my_files_recursive".
    static bool get_recursive();
    static void set_recursive(bool on);

    // Fill JSON array describing the folder currently being browsed: sub-folders first, as
    // { is_dir: true, project_name, path, rel, file_count, folder_count, time }, then the model
    // files as { project_name, path, time, image? } — plus rel_dir naming the sub-folder a file
    // came from when get_recursive() is on.
    // images: max number of files that may carry a thumbnail URL (-1 / INT_MAX = all).
    // Only disk-cached thumbnails are attached here; everything missing is queued for
    // pump_thumbnail_generation() so building the list never touches a model file.
    static void collect_files(nlohmann::json &out, int images);

    // True when the last collect_files() hit its per-folder cap and left entries out.
    static bool last_listing_truncated();

    // Start (or top up) the background worker that renders the queued thumbnails and streams
    // them plus progress into the web view. Returns immediately; safe to call repeatedly.
    static void pump_thumbnail_generation(WebViewPanel *webview);

    // Stop the worker and wait for it. Called from GUI_App::OnExit().
    static void stop_thumbnail_generation();

    // Delete library files after a confirmation prompt. Returns true if any files were removed.
    static bool delete_files(wxWindow *parent, const std::vector<std::string> &paths);

    // Move library files to a user-chosen folder. Returns true if any files were moved.
    static bool move_files(wxWindow *parent, const std::vector<std::string> &paths);

    static bool is_supported_model_file(const std::string &path);
    static bool is_file_in_library(const std::string &path);
    static bool is_folder_in_library(const std::string &path);
};

} // namespace GUI
} // namespace Slic3r
