#pragma once

#include "GUI_Utils.hpp"
#include "slic3r/Utils/Spoolman.hpp"

#include <memory>
#include <string>
#include <vector>

class wxListCtrl;
class wxListEvent;
class wxImageList;
class wxStaticText;
class wxTextCtrl;
class wxButton;

namespace Slic3r
{
namespace GUI
{

/**
 * @brief Lists the spools in Spoolman so the user can pick one for a filament slot.
 */
class SpoolmanSpoolDialog : public DPIDialog
{
public:
    SpoolmanSpoolDialog(wxWindow* parent, int filamentIndex);
    ~SpoolmanSpoolDialog() override;

    /**
     * @brief Gets the chosen spool. Only valid after ShowModal() returned wxID_OK.
     */
    const SpoolmanSpool& Selection() const
    {
        return _selection;
    }

private:
    void BuildUi();
    void FetchSpools();
    void OnSpoolsFetched(std::vector<SpoolmanSpool> spools, const std::string& error);
    void PopulateList();
    void AcceptSelection();
    void EditServerAddress();
    void on_dpi_changed(const wxRect& suggestedRect) override;

private:
    std::string _baseUrl;
    std::vector<SpoolmanSpool> _spools;
    std::vector<size_t> _visible;
    SpoolmanSpool _selection;
    // Cleared on destruction so a late HTTP response does not touch a dead dialog.
    std::shared_ptr<bool> _alive;

    wxTextCtrl* _filter { nullptr };
    wxListCtrl* _list { nullptr };
    wxImageList* _swatches { nullptr };
    wxStaticText* _status { nullptr };
    wxButton* _okButton { nullptr };
    wxButton* _refreshButton { nullptr };
};

} // namespace GUI
} // namespace Slic3r
