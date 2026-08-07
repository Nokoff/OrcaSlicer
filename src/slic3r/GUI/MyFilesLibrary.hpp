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

    // Fill JSON array of { project_name, path, time, image? } for files under the mapped folder.
    // images: max number of entries that may carry a thumbnail URL (-1 / INT_MAX = all).
    // Only disk-cached thumbnails are attached here; everything missing is queued for
    // pump_thumbnail_generation() so building the list never touches a model file.
    static void collect_files(nlohmann::json &out, int images);

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
};

} // namespace GUI
} // namespace Slic3r
