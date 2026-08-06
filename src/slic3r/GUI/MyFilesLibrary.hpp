#pragma once

#include <nlohmann/json.hpp>
#include <string>

class wxWindow;

namespace Slic3r {
namespace GUI {

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
    // images: max number of entries that may include a thumbnail data-URL (-1 / INT_MAX = all).
    static void collect_files(nlohmann::json &out, int images);

    static bool is_supported_model_file(const std::string &path);
    static bool is_file_in_library(const std::string &path);
};

} // namespace GUI
} // namespace Slic3r
