#include "PrinterFileManager.hpp"

#include <algorithm>
#include <cstring>
#include <functional>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/artprov.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/menu.h>
#include <wx/mstream.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Collab/CollabProtocol.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ProgressDialog.hpp"

#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/PrintHost.hpp"

using json = nlohmann::json;

namespace Slic3r {
namespace GUI {

// Moonraker root the sliced files live under. The printer's HTTP file endpoint mirrors it as
// <base>/server/files/gcodes/<path>.
static const char *MODEL_ROOT = "gcodes";

// A timelapse video can be a few hundred MB; Http defaults to a 5 MB ceiling. Kept just under 2 GB
// so the constant stays representable in a 32 bit size_t.
static const size_t MAX_DOWNLOAD_BYTES = size_t(2000) * 1024 * 1024;

static const int THUMB_DIP = 48;

// ----------------------------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------------------------

// Async replies arrive on the MQTT / HTTP worker threads and may outlive the dialog, so everything
// hops back to the GUI thread and re-checks the liveness flag before touching the dialog.
static void call_if_alive(std::shared_ptr<std::atomic<bool>> alive, std::function<void()> fn)
{
    wxGetApp().CallAfter([alive, fn]() {
        if (alive && alive->load())
            fn();
    });
}

// The payload the print host hands back is { "data": <rpc result> } or { "error": ... }.
static bool response_failed(const json &response, std::string &error_out)
{
    if (response.is_null()) {
        error_out = "no response";
        return true;
    }
    if (response.contains("error")) {
        const json &err = response["error"];
        if (err.is_string())
            error_out = err.get<std::string>();
        else if (err.is_object() && err.contains("message") && err["message"].is_string())
            error_out = err["message"].get<std::string>();
        else
            error_out = err.dump();
        return true;
    }
    if (!response.contains("data")) {
        error_out = "empty response";
        return true;
    }
    return false;
}

static std::string json_string(const json &node, const char *key)
{
    if (node.is_object() && node.contains(key) && node[key].is_string())
        return node[key].get<std::string>();
    return {};
}

static uint64_t json_uint(const json &node, const char *key)
{
    if (!node.is_object() || !node.contains(key))
        return 0;
    const json &v = node[key];
    if (v.is_number_unsigned())
        return v.get<uint64_t>();
    if (v.is_number_integer())
        return v.get<int64_t>() < 0 ? 0 : uint64_t(v.get<int64_t>());
    if (v.is_number_float())
        return v.get<double>() < 0 ? 0 : uint64_t(v.get<double>());
    if (v.is_string()) {
        try {
            return uint64_t(std::stoull(v.get<std::string>()));
        } catch (...) {}
    }
    return 0;
}

// First non-empty value among `keys`.
static std::string json_first_string(const json &node, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        std::string v = json_string(node, key);
        if (!v.empty())
            return v;
    }
    return {};
}

static std::string basename_of(const std::string &path)
{
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// Joins a base URL that ends in '/' with a possibly '/'-prefixed suffix.
static std::string join_url(const std::string &base, const std::string &suffix)
{
    if (base.empty() || suffix.empty())
        return {};
    if (boost::istarts_with(suffix, "http://") || boost::istarts_with(suffix, "https://"))
        return suffix;
    std::string b = base;
    if (b.back() != '/')
        b.push_back('/');
    std::string s = suffix;
    while (!s.empty() && s.front() == '/')
        s.erase(0, 1);
    return b + s;
}

// Scales `image` to fit a size x size box and centres it, so every list icon has identical bounds.
static wxBitmap fit_icon(wxImage image, int size)
{
    if (!image.IsOk() || size <= 0)
        return wxBitmap();

    double scale = std::min(double(size) / image.GetWidth(), double(size) / image.GetHeight());
    int    w     = std::max(1, int(image.GetWidth() * scale));
    int    h     = std::max(1, int(image.GetHeight() * scale));
    image        = image.Scale(w, h, wxIMAGE_QUALITY_HIGH);

    wxImage canvas(size, size);
    canvas.SetRGB(wxRect(0, 0, size, size), 255, 255, 255);
    canvas.InitAlpha();
    memset(canvas.GetAlpha(), 0, size_t(size) * size);
    canvas.Paste(image, (size - w) / 2, (size - h) / 2);
    if (image.HasAlpha()) {
        // Paste() does not carry the source alpha over, so copy it across explicitly.
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                canvas.SetAlpha(x + (size - w) / 2, y + (size - h) / 2, image.GetAlpha(x, y));
    } else {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                canvas.SetAlpha(x + (size - w) / 2, y + (size - h) / 2, wxIMAGE_ALPHA_OPAQUE);
    }
    return wxBitmap(canvas);
}

static wxBitmap art_icon(const wxArtID &id, int size)
{
    wxBitmap bmp = wxArtProvider::GetBitmap(id, wxART_OTHER, wxSize(size, size));
    if (!bmp.IsOk()) {
        wxImage blank(size, size);
        blank.SetRGB(wxRect(0, 0, size, size), 200, 200, 200);
        return wxBitmap(blank);
    }
    if (bmp.GetWidth() == size && bmp.GetHeight() == size)
        return bmp;
    return fit_icon(bmp.ConvertToImage(), size);
}

bool PrinterFilesDialog::find_base64_image(const json &node, std::string &out_base64)
{
    if (node.is_string()) {
        const std::string &s = node.get_ref<const std::string &>();
        // Either a bare base64 blob or a data: URI; anything shorter is not an image.
        if (s.size() < 64)
            return false;
        size_t comma = s.find(',');
        if (boost::istarts_with(s, "data:image") && comma != std::string::npos) {
            out_base64 = s.substr(comma + 1);
            return true;
        }
        if (s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=\r\n") ==
            std::string::npos) {
            out_base64 = s;
            return true;
        }
        return false;
    }
    if (node.is_array()) {
        for (const auto &child : node)
            if (find_base64_image(child, out_base64))
                return true;
        return false;
    }
    if (node.is_object()) {
        // Prefer keys that name a thumbnail before walking everything else.
        for (const char *key : {"thumbnail_base64", "base64", "thumbnail", "data", "image"})
            if (node.contains(key) && find_base64_image(node[key], out_base64))
                return true;
        for (const auto &item : node.items())
            if (find_base64_image(item.value(), out_base64))
                return true;
    }
    return false;
}

wxString PrinterFilesDialog::format_size(const wxULongLong &bytes)
{
    double v = bytes.ToDouble();
    if (v <= 0)
        return "-";
    if (v < 1024)
        return wxString::Format("%.0f B", v);
    if (v < 1024.0 * 1024)
        return wxString::Format("%.1f KB", v / 1024.0);
    if (v < 1024.0 * 1024 * 1024)
        return wxString::Format("%.1f MB", v / (1024.0 * 1024));
    return wxString::Format("%.2f GB", v / (1024.0 * 1024 * 1024));
}

wxString PrinterFilesDialog::format_time(time_t t)
{
    if (t <= 0)
        return "-";
    return wxDateTime(t).Format("%Y-%m-%d %H:%M");
}

// ----------------------------------------------------------------------------------------------
// download queue
// ----------------------------------------------------------------------------------------------

namespace {

struct DownloadTask
{
    std::string url;
    std::string target;  // absolute local path
    wxString    name;
};

// Downloads a list of files one after another, driven entirely by Http's completion callbacks so
// no extra thread is needed. Kept alive by the shared_ptr captured in those callbacks.
struct DownloadQueue : public std::enable_shared_from_this<DownloadQueue>
{
    std::vector<DownloadTask> tasks;
    size_t                    index = 0;
    size_t                    done = 0;
    std::atomic<bool>         cancelled{false};
    int                       last_percent = -1;
    ProgressDialog           *dialog = nullptr;
    wxWeakRef<wxWindow>       parent;  // the dialog may be gone by the time we report back
    std::vector<wxString>     failures;
    std::function<void(const std::vector<std::string> & /* downloaded paths */)> on_finished;
    std::vector<std::string>  downloaded;

    void start()
    {
        dialog = new ProgressDialog(_L("Downloading from printer"), " ", 100, parent.get(),
                                    wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT);
        next();
    }

    void finish()
    {
        if (dialog) {
            dialog->Destroy();
            dialog = nullptr;
        }
        // A user-cancelled transfer reports as an error too; that is not worth a dialog.
        if (!failures.empty() && !cancelled.load()) {
            wxString msg = _L("Some files could not be downloaded:") + "\n\n";
            for (size_t i = 0; i < failures.size() && i < 10; ++i)
                msg += failures[i] + "\n";
            MessageDialog(parent.get(), msg, _L("Download failed"), wxOK | wxICON_WARNING).ShowModal();
        }
        auto cb = on_finished;
        auto paths = downloaded;
        if (cb)
            cb(paths);
    }

    void next()
    {
        if (cancelled.load() || index >= tasks.size()) {
            finish();
            return;
        }

        auto self = shared_from_this();
        const DownloadTask task = tasks[index++];
        last_percent = -1;

        if (dialog)
            dialog->Update(int(done * 100 / std::max<size_t>(1, tasks.size())),
                           wxString::Format(_L("Downloading %s"), task.name));

        Http::get(Http::encode_url_path(task.url))
            .size_limit(MAX_DOWNLOAD_BYTES)
            .timeout_connect(15)
            .on_progress([self](Http::Progress progress, bool &cancel) {
                if (self->cancelled.load()) {
                    cancel = true;
                    return;
                }
                if (progress.dltotal == 0)
                    return;
                int pct = int(progress.dlnow * 100 / progress.dltotal);
                if (pct == self->last_percent)
                    return;
                self->last_percent = pct;
                // Overall progress: completed files plus the fraction of the current one.
                int overall = int((self->done * 100 + pct) / std::max<size_t>(1, self->tasks.size()));
                wxGetApp().CallAfter([self, overall]() {
                    if (self->dialog && !self->dialog->Update(overall))
                        self->cancelled.store(true);
                });
            })
            .on_complete([self, task](std::string body, unsigned) {
                bool ok = false;
                try {
                    boost::nowide::ofstream file(task.target, std::ios::binary);
                    if (file.is_open()) {
                        file.write(body.c_str(), body.size());
                        file.close();
                        ok = true;
                    }
                } catch (const std::exception &e) {
                    BOOST_LOG_TRIVIAL(error) << "PrinterFiles: writing " << task.target << " failed: " << e.what();
                }
                wxGetApp().CallAfter([self, task, ok]() {
                    if (ok) {
                        ++self->done;
                        self->downloaded.push_back(task.target);
                    } else {
                        self->failures.push_back(task.name + " - " + _L("could not be written to disk"));
                    }
                    self->next();
                });
            })
            .on_error([self, task](std::string body, std::string error, unsigned status) {
                wxString detail = error.empty() ? wxString::Format("HTTP %u", status) : from_u8(error);
                BOOST_LOG_TRIVIAL(error) << "PrinterFiles: download of " << task.url << " failed: "
                                         << into_u8(detail);
                wxGetApp().CallAfter([self, task, detail]() {
                    self->failures.push_back(task.name + " - " + detail);
                    self->next();
                });
            })
            .perform();
    }
};

} // namespace

// ----------------------------------------------------------------------------------------------
// PrinterFilesDialog
// ----------------------------------------------------------------------------------------------

PrinterFilesDialog::PrinterFilesDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Printer files"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_alive(std::make_shared<std::atomic<bool>>(true))
{
    build_ui();
    bind_events();

    SetSize(wxSize(70 * em_unit(), 45 * em_unit()));
    SetMinSize(wxSize(50 * em_unit(), 30 * em_unit()));
    wxGetApp().UpdateDlgDarkUI(this);
    Layout();
    CenterOnParent();

    switch_view(View::Models);
}

PrinterFilesDialog::~PrinterFilesDialog()
{
    // Any reply still in flight becomes a no-op from here on.
    m_alive->store(false);
}

void PrinterFilesDialog::build_ui()
{
    SetBackgroundColour(*wxWHITE);

    auto *root = new wxBoxSizer(wxVERTICAL);

    // Source switch --------------------------------------------------------------------------
    auto *tabs = new wxBoxSizer(wxHORIZONTAL);
    m_btn_models     = new ::Button(this, _L("Model files"));
    m_btn_timelapses = new ::Button(this, _L("Timelapses"));
    for (::Button *b : {m_btn_models, m_btn_timelapses}) {
        b->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        tabs->Add(b, 0, wxRIGHT, FromDIP(8));
    }
    tabs->AddStretchSpacer();
    m_btn_refresh = new ::Button(this, _L("Refresh"));
    m_btn_refresh->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    tabs->Add(m_btn_refresh, 0);
    root->Add(tabs, 0, wxEXPAND | wxALL, FromDIP(12));

    // Breadcrumb -----------------------------------------------------------------------------
    auto *crumb = new wxBoxSizer(wxHORIZONTAL);
    m_btn_up = new ::Button(this, _L("Up"));
    m_btn_up->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    crumb->Add(m_btn_up, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_path_label = new wxStaticText(this, wxID_ANY, "");
    crumb->Add(m_path_label, 1, wxALIGN_CENTER_VERTICAL);
    root->Add(crumb, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    // File list ------------------------------------------------------------------------------
    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                            wxLC_REPORT | wxLC_HRULES | wxBORDER_SIMPLE);
    const int thumb = FromDIP(THUMB_DIP);
    m_images = new wxImageList(thumb, thumb, true);
    m_images->Add(art_icon(wxART_NORMAL_FILE, thumb)); // 0 - generic file
    m_images->Add(art_icon(wxART_FOLDER, thumb));      // 1 - directory
    m_list->AssignImageList(m_images, wxIMAGE_LIST_SMALL);
    root->Add(m_list, 1, wxEXPAND | wxALL, FromDIP(12));

    // Actions --------------------------------------------------------------------------------
    auto *actions = new wxBoxSizer(wxHORIZONTAL);
    m_status = new wxStaticText(this, wxID_ANY, "");
    actions->Add(m_status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_btn_download = new ::Button(this, _L("Download"));
    m_btn_download->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    actions->Add(m_btn_download, 0, wxRIGHT, FromDIP(8));

    m_btn_delete = new ::Button(this, _L("Delete"));
    m_btn_delete->SetStyle(ButtonStyle::Alert, ButtonType::Choice);
    actions->Add(m_btn_delete, 0, wxRIGHT, FromDIP(8));

    auto *close = new ::Button(this, _L("Close"));
    close->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });
    actions->Add(close, 0);

    root->Add(actions, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    SetSizer(root);
}

void PrinterFilesDialog::bind_events()
{
    m_btn_models->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { switch_view(View::Models); });
    m_btn_timelapses->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { switch_view(View::Timelapses); });
    m_btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { refresh(); });
    m_btn_up->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { go_up(); });
    m_btn_download->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { download_selected(); });
    m_btn_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { delete_selected(); });

    m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent &e) {
        long data = long(m_list->GetItemData(e.GetIndex()));
        if (data >= 0 && size_t(data) < m_entries.size())
            on_activate(size_t(data));
    });

    m_list->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this](wxListEvent &e) {
        long data = long(m_list->GetItemData(e.GetIndex()));
        if (data < 0 || size_t(data) >= m_entries.size())
            return;
        // Right-clicking outside the selection acts on the row under the cursor.
        if (!(m_list->GetItemState(e.GetIndex(), wxLIST_STATE_SELECTED) & wxLIST_STATE_SELECTED)) {
            long sel = -1;
            while ((sel = m_list->GetNextItem(sel, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1)
                m_list->SetItemState(sel, 0, wxLIST_STATE_SELECTED);
            m_list->SetItemState(e.GetIndex(), wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }

        const Entry &entry = m_entries[size_t(data)];
        const size_t entry_index = size_t(data);

        wxMenu menu;
        enum { ID_OPEN = wxID_HIGHEST + 1, ID_DOWNLOAD, ID_DELETE, ID_REFRESH };
        if (entry.is_dir) {
            menu.Append(ID_OPEN, _L("Open folder"));
        } else {
            menu.Append(ID_DOWNLOAD, _L("Download..."));
            menu.Append(ID_OPEN, _L("Download and open"));
        }
        menu.AppendSeparator();
        menu.Append(ID_DELETE, _L("Delete"));
        menu.AppendSeparator();
        menu.Append(ID_REFRESH, _L("Refresh"));

        menu.Bind(wxEVT_MENU, [this, entry_index](wxCommandEvent &evt) {
            switch (evt.GetId()) {
            case ID_OPEN:
                if (m_entries[entry_index].is_dir)
                    enter_dir(m_entries[entry_index].name);
                else
                    download_and_open(entry_index);
                break;
            case ID_DOWNLOAD: download_selected(); break;
            case ID_DELETE: delete_selected(); break;
            case ID_REFRESH: refresh(); break;
            default: break;
            }
        });
        PopupMenu(&menu);
    });
}

void PrinterFilesDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Fit();
    Refresh();
}

std::shared_ptr<PrintHost> PrinterFilesDialog::host() const
{
    std::shared_ptr<PrintHost> h;
    wxGetApp().get_connect_host(h);
    return h;
}

// ----------------------------------------------------------------------------------------------
// listing
// ----------------------------------------------------------------------------------------------

void PrinterFilesDialog::switch_view(View view)
{
    m_view = view;
    m_dir.clear();
    // SetStyle carries the whole colour set, so swapping it is what makes the active tab read as
    // selected - Button has no checked-state colour of its own.
    m_btn_models->SetStyle(view == View::Models ? ButtonStyle::Confirm : ButtonStyle::Regular,
                           ButtonType::Choice);
    m_btn_timelapses->SetStyle(view == View::Timelapses ? ButtonStyle::Confirm : ButtonStyle::Regular,
                               ButtonType::Choice);
    m_btn_models->Refresh();
    m_btn_timelapses->Refresh();
    refresh();
}

void PrinterFilesDialog::refresh()
{
    if (m_loading)
        return;

    m_thumb_queue.clear();
    m_entries.clear();
    m_list->DeleteAllItems();
    reset_thumbnails();

    if (!host()) {
        set_status(_L("Not connected to a printer."));
        return;
    }

    set_busy(true);
    if (m_view == View::Models)
        load_models();
    else
        load_timelapses();
}

void PrinterFilesDialog::load_models()
{
    std::string path = m_dir.empty() ? std::string(MODEL_ROOT) : std::string(MODEL_ROOT) + "/" + m_dir;
    m_path_label->SetLabel(from_u8(path));
    m_btn_up->Enable(!m_dir.empty());

    auto alive = m_alive;
    host()->async_machine_files_directory(path, true, [this, alive](const json &response) {
        call_if_alive(alive, [this, response]() { on_models_loaded(response); });
    });
}

void PrinterFilesDialog::load_timelapses()
{
    m_path_label->SetLabel(_L("Timelapse recordings on the printer"));
    m_btn_up->Enable(false);

    json params;
    params["page_index"]       = 0;
    params["page_rows"]        = 500;
    params["thumbnail_direct"] = true;

    auto alive = m_alive;
    host()->async_get_timelapse_instance(params, [this, alive](const json &response) {
        call_if_alive(alive, [this, response]() { on_timelapses_loaded(response); });
    });
}

void PrinterFilesDialog::on_models_loaded(const json &response)
{
    set_busy(false);

    std::string error;
    if (response_failed(response, error)) {
        set_status(wxString::Format(_L("Could not list the files on the printer: %s"), from_u8(error)));
        return;
    }

    const json &data = response["data"];

    if (data.contains("dirs") && data["dirs"].is_array()) {
        for (const auto &d : data["dirs"]) {
            Entry entry;
            entry.is_dir   = true;
            entry.name     = json_first_string(d, {"dirname", "name", "path"});
            if (entry.name.empty())
                continue;
            entry.path     = m_dir.empty() ? entry.name : m_dir + "/" + entry.name;
            entry.modified = time_t(json_uint(d, "modified"));
            entry.image    = 1;
            entry.raw      = d;
            m_entries.push_back(std::move(entry));
        }
    }

    if (data.contains("files") && data["files"].is_array()) {
        for (const auto &f : data["files"]) {
            Entry entry;
            // get_directory names the entry "filename"; server.files.list uses "path".
            std::string rel = json_first_string(f, {"filename", "path", "name"});
            if (rel.empty())
                continue;
            entry.name     = basename_of(rel);
            entry.path     = m_dir.empty() ? rel : m_dir + "/" + rel;
            entry.size     = wxULongLong(json_uint(f, "size"));
            entry.modified = time_t(json_uint(f, "modified"));
            entry.raw      = f;
            m_entries.push_back(std::move(entry));
        }
    }

    populate_list();
    queue_thumbnails();
}

void PrinterFilesDialog::on_timelapses_loaded(const json &response)
{
    set_busy(false);

    std::string error;
    if (response_failed(response, error)) {
        set_status(wxString::Format(_L("Could not list the timelapses on the printer: %s"), from_u8(error)));
        return;
    }

    const json &data = response["data"];
    const json *instances = nullptr;
    if (data.contains("instances") && data["instances"].is_array())
        instances = &data["instances"];
    else if (data.is_array())
        instances = &data;

    if (instances == nullptr) {
        set_status(_L("The printer reported no timelapse recordings."));
        populate_list();
        return;
    }

    const std::string base = host() ? host()->get_file_base_url() : std::string();

    for (const auto &item : *instances) {
        Entry entry;
        std::string video = json_first_string(item, {"video_path", "video_local_url_suffix"});
        std::string label = json_first_string(item, {"gcode_name", "date_index"});
        entry.name = label.empty() ? basename_of(video) : label;
        if (entry.name.empty())
            continue;
        // Keep the file extension visible so it is obvious what will land on disk.
        std::string video_file = basename_of(json_first_string(item, {"video_local_url_suffix", "video_path"}));
        if (!video_file.empty() && !boost::iends_with(entry.name, ".mp4") && boost::iends_with(video_file, ".mp4"))
            entry.name += ".mp4";

        entry.path         = video;
        entry.download_url = join_url(base, json_first_string(item, {"video_local_url_suffix", "video_path"}));
        entry.detail       = json_first_string(item, {"video_duration"});
        entry.size         = wxULongLong(json_uint(item, "video_file_size"));
        entry.modified     = time_t(json_uint(item, "unix_timestamp_s"));
        entry.raw          = item;

        // The listing already carries the preview, so no extra round trip is needed.
        std::string b64 = json_string(item, "thumbnail_base64");
        if (!b64.empty()) {
            size_t comma = b64.find(',');
            if (boost::istarts_with(b64, "data:image") && comma != std::string::npos)
                b64 = b64.substr(comma + 1);
            try {
                entry.image = decode_thumbnail(Collab::base64_decode(b64));
            } catch (...) {}
        }
        entry.thumb_done = true;
        m_entries.push_back(std::move(entry));
    }

    populate_list();
}

void PrinterFilesDialog::populate_list()
{
    m_list->Freeze();
    m_list->ClearAll();

    m_list->InsertColumn(0, _L("Name"), wxLIST_FORMAT_LEFT, FromDIP(320));
    m_list->InsertColumn(1, _L("Size"), wxLIST_FORMAT_RIGHT, FromDIP(90));
    m_list->InsertColumn(2, _L("Modified"), wxLIST_FORMAT_LEFT, FromDIP(140));
    if (m_view == View::Timelapses)
        m_list->InsertColumn(3, _L("Duration"), wxLIST_FORMAT_LEFT, FromDIP(90));

    // Folders first, then newest first - which is what you want when hunting for a fresh timelapse.
    std::vector<size_t> order(m_entries.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [this](size_t a, size_t b) {
        const Entry &ea = m_entries[a];
        const Entry &eb = m_entries[b];
        if (ea.is_dir != eb.is_dir)
            return ea.is_dir;
        if (ea.modified != eb.modified)
            return ea.modified > eb.modified;
        return ea.name < eb.name;
    });

    long row = 0;
    for (size_t idx : order) {
        const Entry &entry = m_entries[idx];
        long item = m_list->InsertItem(row, from_u8(entry.name), entry.image >= 0 ? entry.image : (entry.is_dir ? 1 : 0));
        m_list->SetItemData(item, long(idx));
        m_list->SetItem(item, 1, entry.is_dir ? "-" : format_size(entry.size));
        m_list->SetItem(item, 2, format_time(entry.modified));
        if (m_view == View::Timelapses)
            m_list->SetItem(item, 3, from_u8(entry.detail));
        ++row;
    }

    m_list->Thaw();

    size_t files = 0;
    wxULongLong total(0);
    for (const Entry &e : m_entries)
        if (!e.is_dir) {
            ++files;
            total += e.size;
        }

    if (m_entries.empty())
        set_status(m_view == View::Models ? _L("No files here.") : _L("No timelapses on the printer."));
    else
        set_status(wxString::Format(_L("%d files, %s"), int(files), format_size(total)));
}

void PrinterFilesDialog::set_status(const wxString &text)
{
    m_status->SetLabel(text);
    Layout();
}

void PrinterFilesDialog::set_busy(bool busy)
{
    m_loading = busy;
    m_btn_refresh->Enable(!busy);
    m_btn_models->Enable(!busy);
    m_btn_timelapses->Enable(!busy);
    if (busy)
        set_status(_L("Loading..."));
}

// ----------------------------------------------------------------------------------------------
// thumbnails
// ----------------------------------------------------------------------------------------------

void PrinterFilesDialog::queue_thumbnails()
{
    if (m_view != View::Models)
        return;

    // Previews are cosmetic, so a huge folder should not turn into hundreds of round trips.
    const size_t MAX_THUMBS = 200;

    m_thumb_failures = 0;
    for (size_t i = 0; i < m_entries.size() && m_thumb_queue.size() < MAX_THUMBS; ++i) {
        const Entry &entry = m_entries[i];
        if (entry.is_dir || entry.thumb_done)
            continue;
        // Only sliced projects carry an embedded preview.
        if (boost::iends_with(entry.name, ".3mf") || boost::iends_with(entry.name, ".gcode"))
            m_thumb_queue.push_back(i);
    }
    request_next_thumbnail();
}

void PrinterFilesDialog::request_next_thumbnail()
{
    if (m_thumb_in_flight || m_thumb_queue.empty())
        return;

    auto h = host();
    if (!h) {
        m_thumb_queue.clear();
        return;
    }

    const size_t entry_index = m_thumb_queue.front();
    m_thumb_queue.erase(m_thumb_queue.begin());
    if (entry_index >= m_entries.size())
        return request_next_thumbnail();

    m_thumb_in_flight = true;
    const std::string path = m_entries[entry_index].path;
    auto alive = m_alive;

    h->async_files_thumbnails_base64(path, [this, alive, entry_index](const json &response) {
        call_if_alive(alive, [this, response, entry_index]() {
            m_thumb_in_flight = false;
            std::string error;
            bool        got_image = false;
            if (!response_failed(response, error)) {
                std::string b64;
                if (find_base64_image(response["data"], b64)) {
                    try {
                        apply_thumbnail(entry_index, Collab::base64_decode(b64));
                        got_image = true;
                    } catch (...) {}
                }
            }
            if (entry_index < m_entries.size())
                m_entries[entry_index].thumb_done = true;

            if (got_image) {
                m_thumb_failures = 0;
            } else if (++m_thumb_failures >= 3) {
                // The printer clearly is not going to hand these over; stop asking.
                m_thumb_queue.clear();
                return;
            }
            request_next_thumbnail();
        });
    });
}

int PrinterFilesDialog::decode_thumbnail(const std::string &image_bytes)
{
    if (image_bytes.empty())
        return -1;

    wxMemoryInputStream stream(image_bytes.data(), image_bytes.size());
    wxImage             image;
    {
        wxLogNull no_log; // an unrecognised payload should stay silent, not pop a dialog
        if (!image.LoadFile(stream, wxBITMAP_TYPE_ANY) || !image.IsOk())
            return -1;
    }
    return m_images->Add(fit_icon(image, FromDIP(THUMB_DIP)));
}

void PrinterFilesDialog::apply_thumbnail(size_t entry_index, const std::string &image_bytes)
{
    if (entry_index >= m_entries.size())
        return;

    int image_id = decode_thumbnail(image_bytes);
    if (image_id < 0)
        return;

    m_entries[entry_index].image = image_id;
    // Repaint just this row rather than rebuilding the whole list.
    for (long row = 0; row < m_list->GetItemCount(); ++row) {
        if (long(m_list->GetItemData(row)) == long(entry_index)) {
            m_list->SetItemImage(row, image_id);
            break;
        }
    }
}

void PrinterFilesDialog::reset_thumbnails()
{
    // Every refresh re-adds its previews, so drop the old ones instead of growing the list forever.
    const int thumb = FromDIP(THUMB_DIP);
    m_images->RemoveAll();
    m_images->Add(art_icon(wxART_NORMAL_FILE, thumb)); // 0 - generic file
    m_images->Add(art_icon(wxART_FOLDER, thumb));      // 1 - directory
}

// ----------------------------------------------------------------------------------------------
// actions
// ----------------------------------------------------------------------------------------------

std::vector<size_t> PrinterFilesDialog::selected_entries() const
{
    std::vector<size_t> result;
    long                row = -1;
    while ((row = m_list->GetNextItem(row, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)) != -1) {
        long data = long(m_list->GetItemData(row));
        if (data >= 0 && size_t(data) < m_entries.size())
            result.push_back(size_t(data));
    }
    return result;
}

void PrinterFilesDialog::on_activate(size_t entry_index)
{
    const Entry &entry = m_entries[entry_index];
    if (entry.is_dir)
        enter_dir(entry.name);
    else
        download_selected();
}

void PrinterFilesDialog::enter_dir(const std::string &name)
{
    if (m_view != View::Models)
        return;
    m_dir = m_dir.empty() ? name : m_dir + "/" + name;
    refresh();
}

void PrinterFilesDialog::go_up()
{
    if (m_dir.empty())
        return;
    size_t pos = m_dir.find_last_of('/');
    m_dir      = pos == std::string::npos ? std::string() : m_dir.substr(0, pos);
    refresh();
}

std::string PrinterFilesDialog::entry_url(const Entry &entry) const
{
    if (!entry.download_url.empty())
        return entry.download_url;

    auto h = host();
    if (!h)
        return {};
    std::string base = h->get_file_base_url();
    if (base.empty())
        return {};
    return join_url(base, std::string("server/files/") + MODEL_ROOT + "/" + entry.path);
}

void PrinterFilesDialog::download_selected()
{
    std::vector<size_t> selection = selected_entries();
    std::vector<size_t> files;
    for (size_t idx : selection)
        if (!m_entries[idx].is_dir)
            files.push_back(idx);

    if (files.empty()) {
        MessageDialog(this, _L("Select one or more files to download."), _L("Download"), wxOK | wxICON_INFORMATION)
            .ShowModal();
        return;
    }

    std::vector<DownloadTask> tasks;

    if (files.size() == 1) {
        const Entry &entry = m_entries[files.front()];
        std::string  url   = entry_url(entry);
        if (!url.empty()) {
            wxString     name = from_u8(entry.name);
            wxString     ext  = name.AfterLast('.');
            wxString     wildcard = ext.IsEmpty() ? wxString("All files (*.*)|*.*")
                                                  : wxString::Format("%s files (*.%s)|*.%s|All files (*.*)|*.*", ext, ext, ext);
            wxFileDialog save(this, _L("Save file"), wxGetApp().app_config->get("download_path"), name, wildcard,
                              wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (save.ShowModal() == wxID_CANCEL)
                return;
            tasks.push_back({url, into_u8(save.GetPath()), name});
        }
    } else {
        wxDirDialog dir(this, _L("Choose a folder to download to"), wxGetApp().app_config->get("download_path"),
                        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dir.ShowModal() == wxID_CANCEL)
            return;
        for (size_t idx : files) {
            const Entry &entry = m_entries[idx];
            std::string  url   = entry_url(entry);
            if (url.empty())
                continue;
            wxFileName target(dir.GetPath(), from_u8(entry.name));
            tasks.push_back({url, into_u8(target.GetFullPath()), from_u8(entry.name)});
        }
    }

    if (tasks.empty()) {
        MessageDialog(this,
                      _L("The printer did not provide a download address for these files. "
                         "Downloading needs the printer to be reachable on your local network."),
                      _L("Download"), wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }

    auto queue         = std::make_shared<DownloadQueue>();
    queue->tasks       = std::move(tasks);
    queue->parent      = this;
    queue->on_finished = [](const std::vector<std::string> &paths) {
        // Remember where the user last saved so the next dialog opens there.
        if (!paths.empty())
            wxGetApp().app_config->set("download_path", boost::filesystem::path(paths.front()).parent_path().string());
    };
    queue->start();
}

void PrinterFilesDialog::download_and_open(size_t entry_index)
{
    const Entry &entry = m_entries[entry_index];
    std::string  url   = entry_url(entry);
    if (url.empty()) {
        MessageDialog(this,
                      _L("The printer did not provide a download address for this file. "
                         "Downloading needs the printer to be reachable on your local network."),
                      _L("Download"), wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }

    // Straight into the temp folder so the file can be handed to the system player right away.
    wxFileName target(wxStandardPaths::Get().GetTempDir(), from_u8(entry.name));

    auto queue    = std::make_shared<DownloadQueue>();
    queue->tasks  = {{url, into_u8(target.GetFullPath()), from_u8(entry.name)}};
    queue->parent = this;
    queue->on_finished = [](const std::vector<std::string> &paths) {
        if (!paths.empty())
            wxLaunchDefaultApplication(from_u8(paths.front()));
    };
    queue->start();
}

void PrinterFilesDialog::delete_selected()
{
    std::vector<size_t> selection = selected_entries();
    if (selection.empty()) {
        MessageDialog(this, _L("Select one or more items to delete."), _L("Delete"), wxOK | wxICON_INFORMATION)
            .ShowModal();
        return;
    }

    wxString names;
    for (size_t i = 0; i < selection.size() && i < 8; ++i)
        names += "\n" + from_u8(m_entries[selection[i]].name);
    if (selection.size() > 8)
        names += wxString::Format("\n... (%d more)", int(selection.size() - 8));

    MessageDialog confirm(this,
                          wxString::Format(_L("Permanently delete %d item(s) from the printer?"),
                                           int(selection.size())) +
                              "\n" + names,
                          _L("Delete from printer"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (confirm.ShowModal() != wxID_YES)
        return;

    // Refresh once every delete has been answered, rather than guessing at a delay.
    auto pending = std::make_shared<std::atomic<int>>(int(selection.size()));
    auto alive   = m_alive;
    auto on_done = [this, alive, pending]() {
        if (--(*pending) == 0 && alive->load())
            refresh();
    };

    for (size_t idx : selection) {
        const Entry &entry = m_entries[idx];
        if (m_view == View::Timelapses)
            delete_timelapse(entry, on_done);
        else
            delete_model_file(std::string(MODEL_ROOT) + "/" + entry.path, on_done);
    }
}

void PrinterFilesDialog::delete_model_file(const std::string &root_relative_path, std::function<void()> on_done)
{
    auto h = host();
    if (!h) {
        if (on_done)
            on_done();
        return;
    }

    auto alive = m_alive;
    h->async_delete_machine_file(root_relative_path, [this, alive, root_relative_path, on_done](const json &response) {
        std::string error;
        const bool  failed = response_failed(response, error);
        if (failed)
            BOOST_LOG_TRIVIAL(error) << "PrinterFiles: deleting " << root_relative_path << " failed: " << error;
        call_if_alive(alive, [this, failed, error, on_done]() {
            if (failed)
                set_status(wxString::Format(_L("Delete failed: %s"), from_u8(error)));
            if (on_done)
                on_done();
        });
    });
}

void PrinterFilesDialog::delete_timelapse(const Entry &entry, std::function<void()> on_done)
{
    auto h = host();
    if (!h) {
        if (on_done)
            on_done();
        return;
    }

    // camera.delete_timelapse_instance identifies a recording by the fields the listing handed back.
    // Both the flat and the "instances" shape are sent so whichever the firmware expects is present.
    json identity = json::object();
    for (const char *key : {"date_index", "timelapse_dir", "video_path", "gcode_name", "gcode_path"}) {
        std::string value = json_string(entry.raw, key);
        if (!value.empty())
            identity[key] = value;
    }
    json params         = identity;
    params["instances"] = json::array({identity});

    // If the camera API rejects it, the video is still an ordinary file under a Moonraker root.
    std::string suffix = json_first_string(entry.raw, {"video_local_url_suffix", "video_path"});
    std::string fallback_path;
    {
        const std::string marker = "server/files/";
        size_t            pos    = suffix.find(marker);
        if (pos != std::string::npos)
            fallback_path = suffix.substr(pos + marker.size());
    }

    auto alive = m_alive;
    h->async_delete_camera_timelapse(params, [this, alive, fallback_path, on_done](const json &response) {
        std::string error;
        if (!response_failed(response, error)) {
            call_if_alive(alive, [on_done]() {
                if (on_done)
                    on_done();
            });
            return;
        }

        BOOST_LOG_TRIVIAL(warning) << "PrinterFiles: camera.delete_timelapse_instance failed: " << error;
        call_if_alive(alive, [this, error, fallback_path, on_done]() {
            if (!fallback_path.empty()) {
                delete_model_file(fallback_path, on_done);
                return;
            }
            set_status(wxString::Format(_L("Delete failed: %s"), from_u8(error)));
            if (on_done)
                on_done();
        });
    });
}

}} // namespace Slic3r::GUI
