#ifndef slic3r_GUI_PrinterFileManager_hpp_
#define slic3r_GUI_PrinterFileManager_hpp_

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/longlong.h>
#include <wx/stattext.h>

#include "GUI_Utils.hpp"

class Button;

namespace Slic3r {

class PrintHost;

namespace GUI {

// Native file browser for the files that live on the connected printer.
//
// The Device tab itself is a Flutter web view we cannot extend, and it offers no way to download or
// delete what it lists - so this dialog talks to the same Moonraker APIs directly:
//   * model files  - server.files.get_directory / server.files.delete_file, downloaded over HTTP
//                    from <base>/server/files/gcodes/<path>
//   * timelapses   - camera.get_timelapse_instance, which hands back a thumbnail and a local URL
//                    suffix per recording
class PrinterFilesDialog : public DPIDialog
{
public:
    explicit PrinterFilesDialog(wxWindow *parent);
    ~PrinterFilesDialog() override;

    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    enum class View { Models, Timelapses };

    // One row of the list. Model files and timelapses fill in different subsets of this.
    struct Entry
    {
        bool        is_dir = false;
        std::string name;          // shown in the Name column
        std::string path;          // root-relative path, e.g. "sub/part.gcode.3mf"
        std::string download_url;  // absolute URL, empty when it has to be derived from `path`
        std::string detail;        // extra column text (duration for timelapses)
        wxULongLong size{0};
        time_t      modified = 0;
        int         image = -1;    // index into m_images, -1 = generic icon
        bool        thumb_done = false;
        nlohmann::json raw;        // original listing entry, used to identify timelapses on delete
    };

    // --- construction -------------------------------------------------------
    void build_ui();
    void bind_events();

    // --- listing ------------------------------------------------------------
    void switch_view(View view);
    void refresh();
    void load_models();
    void load_timelapses();
    void on_models_loaded(const nlohmann::json &response);
    void on_timelapses_loaded(const nlohmann::json &response);
    void populate_list();
    void set_status(const wxString &text);
    void set_busy(bool busy);

    // --- thumbnails ---------------------------------------------------------
    void queue_thumbnails();
    void request_next_thumbnail();
    // Decodes an image payload into m_images; returns the image index, or -1 if it was not an image.
    int  decode_thumbnail(const std::string &image_bytes);
    void apply_thumbnail(size_t entry_index, const std::string &image_bytes);
    void reset_thumbnails();

    // --- actions ------------------------------------------------------------
    std::vector<size_t> selected_entries() const;
    void on_activate(size_t entry_index);
    void download_selected();
    void download_and_open(size_t entry_index);
    void delete_selected();
    // `on_done` runs on the GUI thread once the printer has answered, whether or not it worked.
    void delete_timelapse(const Entry &entry, std::function<void()> on_done);
    void delete_model_file(const std::string &root_relative_path, std::function<void()> on_done);
    void enter_dir(const std::string &name);
    void go_up();

    // --- helpers ------------------------------------------------------------
    std::shared_ptr<PrintHost> host() const;
    // Absolute URL a given entry is downloaded from, empty when it cannot be built.
    std::string       entry_url(const Entry &entry) const;
    static wxString   format_size(const wxULongLong &bytes);
    static wxString   format_time(time_t t);
    // Pulls the first base64-encoded image out of an arbitrarily shaped response.
    static bool       find_base64_image(const nlohmann::json &node, std::string &out_base64);

    View        m_view = View::Models;
    std::string m_dir;             // model sub-directory being browsed, relative to "gcodes"
    std::vector<Entry> m_entries;
    bool        m_loading = false;

    // Thumbnails are fetched one at a time so a large library does not flood the printer.
    std::vector<size_t> m_thumb_queue;
    bool                m_thumb_in_flight = false;
    // Firmware that does not implement the thumbnail call would make every request sit out its
    // timeout, so the queue is abandoned after a couple of failures.
    int                 m_thumb_failures = 0;

    // Handed to every async callback; reset in the destructor so late replies become no-ops.
    std::shared_ptr<std::atomic<bool>> m_alive;

    wxListCtrl  *m_list = nullptr;
    wxImageList *m_images = nullptr;
    wxStaticText *m_path_label = nullptr;
    wxStaticText *m_status = nullptr;

    ::Button *m_btn_models = nullptr;
    ::Button *m_btn_timelapses = nullptr;
    ::Button *m_btn_up = nullptr;
    ::Button *m_btn_refresh = nullptr;
    ::Button *m_btn_download = nullptr;
    ::Button *m_btn_delete = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PrinterFileManager_hpp_
