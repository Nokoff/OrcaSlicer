#include "SpoolmanSpoolDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <boost/algorithm/string.hpp>

#include <wx/button.h>
#include <wx/dcmemory.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>

namespace Slic3r
{
namespace GUI
{

namespace
{

enum Column
{
    ColumnSpool = 0,
    ColumnMaterial,
    ColumnRemaining,
    ColumnLocation,
    ColumnId
};

wxBitmap MakeSwatch(const SpoolmanSpool& spool, const wxSize& size)
{
    wxBitmap bitmap(size.x, size.y);
    wxMemoryDC dc(bitmap);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();

    if (spool.colors.empty())
    {
        // No colour stored in Spoolman: draw a hatched box so it is clearly "unknown".
        dc.SetBrush(wxBrush(wxColour(200, 200, 200), wxBRUSHSTYLE_BDIAGONAL_HATCH));
    }
    else if (spool.longitudinal && spool.colors.size() > 1)
    {
        const int segments = static_cast<int>(spool.colors.size()) - 1;
        for (int i = 0; i < segments; ++i)
        {
            const int x0 = size.x * i / segments;
            const int x1 = size.x * (i + 1) / segments;
            dc.GradientFillLinear(wxRect(x0, 0, x1 - x0, size.y), wxColour(spool.colors[i]), wxColour(spool.colors[i + 1]));
        }
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
    }
    else
    {
        const int count = static_cast<int>(spool.colors.size());
        for (int i = 0; i < count; ++i)
        {
            const int x0 = size.x * i / count;
            const int x1 = size.x * (i + 1) / count;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(spool.colors[i])));
            dc.DrawRectangle(x0, 0, x1 - x0, size.y);
        }
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
    }

    dc.SetPen(wxPen(wxColour(120, 120, 120)));
    dc.DrawRectangle(0, 0, size.x, size.y);
    dc.SelectObject(wxNullBitmap);
    return bitmap;
}

wxString FormatRemaining(double grams)
{
    if (grams < 0.0)
        return wxEmptyString;
    return wxString::Format("%d g", static_cast<int>(std::lround(grams)));
}

bool MatchesFilter(const SpoolmanSpool& spool, const std::string& filter)
{
    if (filter.empty())
        return true;

    const std::string haystack = spool.DisplayName() + " " + spool.material + " " + spool.location + " " +
                                 spool.lotNumber + " #" + std::to_string(spool.id) + " " +
                                 boost::algorithm::join(spool.colors, " ");
    std::vector<std::string> words;
    boost::algorithm::split(words, filter, boost::is_space(), boost::token_compress_on);
    return std::all_of(words.begin(), words.end(), [&haystack](const std::string& word) {
        return word.empty() || boost::algorithm::icontains(haystack, word);
    });
}

} // namespace

SpoolmanSpoolDialog::SpoolmanSpoolDialog(wxWindow* parent, int filamentIndex)
    : DPIDialog(parent, wxID_ANY, wxString::Format(_L("Select spool from Spoolman for filament %d"), filamentIndex + 1),
                wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , _alive(std::make_shared<bool>(true))
{
    if (wxGetApp().app_config != nullptr)
        _baseUrl = Spoolman::NormalizeUrl(wxGetApp().app_config->get(Spoolman::ConfigKey));

    BuildUi();
    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();

    if (_baseUrl.empty())
        CallAfter([this]() { EditServerAddress(); });
    else
        FetchSpools();
}

SpoolmanSpoolDialog::~SpoolmanSpoolDialog()
{
    _alive.reset();
    // The list does not own the image list, so detach it before freeing.
    if (_list != nullptr)
        _list->SetImageList(nullptr, wxIMAGE_LIST_SMALL);
    delete _swatches;
}

void SpoolmanSpoolDialog::BuildUi()
{
    const int margin = FromDIP(10);
    wxBoxSizer* topSizer = new wxBoxSizer(wxVERTICAL);

    _filter = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize);
    _filter->SetHint(_L("Search by name, material, colour, location or ID"));
    _filter->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { PopulateList(); });
    topSizer->Add(_filter, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);

    _list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(640), FromDIP(360)),
                           wxLC_REPORT | wxLC_SINGLE_SEL);
    _list->AppendColumn(_L("Spool"), wxLIST_FORMAT_LEFT, FromDIP(280));
    _list->AppendColumn(_L("Material"), wxLIST_FORMAT_LEFT, FromDIP(90));
    _list->AppendColumn(_L("Remaining"), wxLIST_FORMAT_RIGHT, FromDIP(90));
    _list->AppendColumn(_L("Location"), wxLIST_FORMAT_LEFT, FromDIP(110));
    _list->AppendColumn(_L("ID"), wxLIST_FORMAT_RIGHT, FromDIP(50));
    _list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { _okButton->Enable(true); });
    _list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) {
        _okButton->Enable(_list->GetSelectedItemCount() > 0);
    });
    _list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { AcceptSelection(); });
    topSizer->Add(_list, 1, wxEXPAND | wxALL, margin);

    _status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    topSizer->Add(_status, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

    wxBoxSizer* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton* serverButton = new wxButton(this, wxID_ANY, _L("Server..."));
    serverButton->SetToolTip(_L("Change the Spoolman server address"));
    serverButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EditServerAddress(); });
    _refreshButton = new wxButton(this, wxID_ANY, _L("Refresh"));
    _refreshButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { FetchSpools(); });
    _okButton = new wxButton(this, wxID_OK, _L("Use spool"));
    _okButton->Enable(false);
    _okButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AcceptSelection(); });
    wxButton* cancelButton = new wxButton(this, wxID_CANCEL, _L("Cancel"));

    buttonSizer->Add(serverButton, 0, wxRIGHT, FromDIP(6));
    buttonSizer->Add(_refreshButton, 0);
    buttonSizer->AddStretchSpacer();
    buttonSizer->Add(_okButton, 0, wxRIGHT, FromDIP(6));
    buttonSizer->Add(cancelButton, 0);
    topSizer->Add(buttonSizer, 0, wxEXPAND | wxALL, margin);

    SetSizerAndFit(topSizer);
    SetMinSize(GetSize());
}

void SpoolmanSpoolDialog::FetchSpools()
{
    if (_baseUrl.empty())
    {
        _status->SetLabel(_L("Set the Spoolman server address to list your spools."));
        return;
    }

    _status->SetLabel(wxString::Format(_L("Loading spools from %s..."), from_u8(_baseUrl)));
    _refreshButton->Enable(false);

    std::weak_ptr<bool> alive = _alive;
    Spoolman::FetchSpools(_baseUrl, [this, alive](std::vector<SpoolmanSpool> spools, std::string error) {
        wxTheApp->CallAfter([this, alive, spools = std::move(spools), error = std::move(error)]() mutable {
            if (alive.expired())
                return;
            OnSpoolsFetched(std::move(spools), error);
        });
    });
}

void SpoolmanSpoolDialog::OnSpoolsFetched(std::vector<SpoolmanSpool> spools, const std::string& error)
{
    _refreshButton->Enable(true);
    if (!error.empty())
    {
        _spools.clear();
        PopulateList();
        _status->SetLabel(wxString::Format(_L("Could not reach Spoolman at %s: %s"), from_u8(_baseUrl), from_u8(error)));
        return;
    }

    _spools = std::move(spools);
    PopulateList();
}

void SpoolmanSpoolDialog::PopulateList()
{
    const std::string filter = boost::algorithm::trim_copy(into_u8(_filter->GetValue()));

    _list->Freeze();
    _list->DeleteAllItems();
    _visible.clear();

    const wxSize swatchSize(FromDIP(28), FromDIP(14));
    delete _swatches;
    _swatches = new wxImageList(swatchSize.x, swatchSize.y, false);
    _list->SetImageList(_swatches, wxIMAGE_LIST_SMALL);

    for (size_t i = 0; i < _spools.size(); ++i)
    {
        const SpoolmanSpool& spool = _spools[i];
        if (!MatchesFilter(spool, filter))
            continue;

        const int image = _swatches->Add(MakeSwatch(spool, swatchSize));
        const long row = _list->InsertItem(_list->GetItemCount(), from_u8(spool.DisplayName()), image);
        _list->SetItem(row, ColumnMaterial, from_u8(spool.material));
        _list->SetItem(row, ColumnRemaining, FormatRemaining(spool.remainingWeight));
        _list->SetItem(row, ColumnLocation, from_u8(spool.location));
        _list->SetItem(row, ColumnId, wxString::Format("%d", spool.id));
        _list->SetItemData(row, static_cast<long>(_visible.size()));
        _visible.push_back(i);
    }
    _list->Thaw();
    _okButton->Enable(false);

    if (_spools.empty())
        _status->SetLabel(_L("No active spools found in Spoolman."));
    else
        _status->SetLabel(wxString::Format(_L("Showing %d of %d spools."), static_cast<int>(_visible.size()),
                                            static_cast<int>(_spools.size())));
}

void SpoolmanSpoolDialog::AcceptSelection()
{
    const long row = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (row < 0)
        return;

    const size_t visibleIndex = static_cast<size_t>(_list->GetItemData(row));
    if (visibleIndex >= _visible.size())
        return;

    _selection = _spools[_visible[visibleIndex]];
    if (_selection.colors.empty())
    {
        _status->SetLabel(_L("This spool has no colour set in Spoolman. Add one there or pick another spool."));
        return;
    }
    EndModal(wxID_OK);
}

void SpoolmanSpoolDialog::EditServerAddress()
{
    wxTextEntryDialog dialog(this, _L("Spoolman server address (for example http://192.168.1.10:7912):"),
                             _L("Spoolman server"), from_u8(_baseUrl));
    if (dialog.ShowModal() != wxID_OK)
    {
        if (_baseUrl.empty())
            _status->SetLabel(_L("Set the Spoolman server address to list your spools."));
        return;
    }

    _baseUrl = Spoolman::NormalizeUrl(into_u8(dialog.GetValue()));
    if (wxGetApp().app_config != nullptr)
    {
        wxGetApp().app_config->set(Spoolman::ConfigKey, _baseUrl);
        wxGetApp().app_config->save();
    }
    FetchSpools();
}

void SpoolmanSpoolDialog::on_dpi_changed(const wxRect& suggestedRect)
{
    PopulateList();
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
