#include "MergeFilamentsDialog.hpp"

#include <algorithm>
#include <iterator>

#include <wx/checkbox.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/SegmentedToggle.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"

namespace
{

// --- Dialog layout (DIP), matching the sync dialog's proportions ---
constexpr int g_dialogWidth   = 520;
constexpr int g_previewHeight = 300;
constexpr int g_padding       = 20;
constexpr int g_btnW          = 220;
constexpr int g_btnH          = 36;
constexpr int g_btnRowGap     = 12;

// --- Preview row metrics (DIP) ---
constexpr int g_rowHeight    = 30;
constexpr int g_rowMargin    = 12;
constexpr int g_targetSwatch = 18;
constexpr int g_sourceSwatch = 14;
constexpr int g_swatchGap    = 4;
constexpr int g_labelWidth   = 88;
constexpr int g_arrowWidth   = 22;
constexpr int g_swatchRadius = 3;

// --- Colours, shared with the sync dialog ---
constexpr const char *g_dialogBg       = "#F8F7F7";
constexpr const char *g_blockBg        = "#FFFFFF";
constexpr const char *g_labelColor     = "#242424";
constexpr const char *g_mutedColor     = "#6B6A6A";
constexpr const char *g_swatchBorder   = "#D1D5DC";
constexpr const char *g_secondaryBorder= "#D1D5DC";
constexpr const char *g_secondaryText  = "#242424";
constexpr const char *g_secondaryHover = "#F3F4F6";
constexpr const char *g_primaryBg      = "#019687";
constexpr const char *g_primaryHoverBg = "#26A69A";
constexpr const char *g_primaryText    = "#FEFEFE";
constexpr const char *g_disabledBg     = "#DFDFDF";
constexpr const char *g_disabledText   = "#6B6A6A";

// Tolerance presets for "merge similar", as CIEDE2000 distances. Roughly:
// 2.5 is the threshold where a side-by-side difference stops being obvious,
// 6 keeps a colour family together, 12 lumps in anything broadly alike, and
// 25 allows substantially different shades to merge as a last-resort option.
constexpr float g_toleranceDeltaE[] = {2.5f, 6.0f, 12.0f, 25.0f};
constexpr int   g_toleranceDefault  = 1;

} // namespace

namespace Slic3r
{
namespace GUI
{

// =====================================================================
// MergePreviewList — one row per surviving filament, custom painted so a
// recomputed plan is a single repaint instead of a widget rebuild.
// =====================================================================

class MergePreviewList : public wxScrolledWindow
{
public:
    struct Row
    {
        wxColour              target_color;
        wxString              target_label;
        std::vector<wxColour> source_colors;
        bool                  pinned = false;
    };

    explicit MergePreviewList(wxWindow *parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_blockBg)));
        Bind(wxEVT_PAINT, &MergePreviewList::onPaint, this);
    }

    void setRows(std::vector<Row> rows)
    {
        m_rows = std::move(rows);
        // Vertical only, one row per scroll unit.
        SetScrollbars(0, FromDIP(g_rowHeight), 0, static_cast<int>(m_rows.size()));
        Scroll(0, 0);
        Refresh();
    }

private:
    void onPaint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour(g_blockBg))));
        dc.Clear();
        DoPrepareDC(dc);

        const wxSize size = GetClientSize();
        wxGCDC       gdc(dc);
        gdc.SetFont(GetFont());

        const int rowH   = FromDIP(g_rowHeight);
        const int margin = FromDIP(g_rowMargin);

        for (size_t i = 0; i < m_rows.size(); ++i) {
            const Row &row = m_rows[i];
            const int  y   = static_cast<int>(i) * rowH;
            int        x   = margin;

            x += drawSwatch(gdc, x, y, rowH, FromDIP(g_targetSwatch), row.target_color) + FromDIP(g_swatchGap) * 2;

            gdc.SetTextForeground(StateColor::darkModeColorFor(wxColour(row.pinned ? g_mutedColor : g_labelColor)));
            const wxSize textSize = gdc.GetTextExtent(row.target_label);
            gdc.DrawText(row.target_label, x, y + (rowH - textSize.y) / 2);
            x += FromDIP(g_labelWidth);

            if (row.source_colors.empty()) {
                // Nothing merged in — say why the slot is still here.
                gdc.SetTextForeground(StateColor::darkModeColorFor(wxColour(g_mutedColor)));
                const wxString note = row.pinned ? _L("kept — used by a mixed filament") : _L("kept as is");
                gdc.DrawText(note, x, y + (rowH - textSize.y) / 2);
                continue;
            }

            gdc.SetTextForeground(StateColor::darkModeColorFor(wxColour(g_mutedColor)));
            gdc.DrawText(wxString::FromUTF8("\xE2\x86\x90"), x, y + (rowH - textSize.y) / 2);
            x += FromDIP(g_arrowWidth);

            // Swatches of everything being absorbed, truncated with a count
            // once the row runs out of width.
            const int swatch = FromDIP(g_sourceSwatch);
            const int step   = swatch + FromDIP(g_swatchGap);
            const int limit  = size.x - margin;
            for (size_t s = 0; s < row.source_colors.size(); ++s) {
                const bool last = (s + 1 == row.source_colors.size());
                if (!last && x + step * 2 > limit) {
                    gdc.DrawText(wxString::Format("+%d", static_cast<int>(row.source_colors.size() - s)), x,
                                 y + (rowH - textSize.y) / 2);
                    break;
                }
                if (x + swatch > limit)
                    break;
                x += drawSwatch(gdc, x, y, rowH, swatch, row.source_colors[s]) + FromDIP(g_swatchGap);
            }
        }
    }

    // Returns the width consumed so callers can keep advancing.
    int drawSwatch(wxGCDC &gdc, int x, int y, int rowH, int size, const wxColour &color)
    {
        gdc.SetPen(wxPen(wxColour(g_swatchBorder)));
        gdc.SetBrush(wxBrush(color));
        gdc.DrawRoundedRectangle(x, y + (rowH - size) / 2, size, size, FromDIP(g_swatchRadius));
        return size;
    }

    std::vector<Row> m_rows;
};

// =====================================================================
// MergeFilamentsDialog
// =====================================================================

MergeFilamentsDialog::MergeFilamentsDialog(wxWindow                        *parent,
                                           Mode                             mode,
                                           const std::vector<FilamentData> &design,
                                           const std::vector<FilamentData> &machine,
                                           const std::vector<bool>         &mergeable)
    : wxDialog(parent,
               wxID_ANY,
               mode == Mode::ToLoaded ? _L("Merge to Loaded Filaments") : _L("Merge Similar Filaments"),
               wxDefaultPosition,
               wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
    , m_mode(mode)
    , m_design(design)
    , m_machine(machine)
    , m_mergeable(mergeable)
    , m_toleranceIndex(g_toleranceDefault)
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_dialogBg)));

    auto *topSizer = new wxBoxSizer(wxVERTICAL);

    // --- Summary ---
    {
        m_pSummary = new wxStaticText(this, wxID_ANY, wxEmptyString);
        wxFont summaryFont = m_pSummary->GetFont();
        summaryFont.SetPointSize(summaryFont.GetPointSize() + 2);
        summaryFont.SetWeight(wxFONTWEIGHT_BOLD);
        m_pSummary->SetFont(summaryFont);
        m_pSummary->SetForegroundColour(StateColor::darkModeColorFor(wxColour(g_labelColor)));

        m_pDetail = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_pDetail->SetForegroundColour(StateColor::darkModeColorFor(wxColour(g_mutedColor)));
        m_pDetail->Wrap(FromDIP(g_dialogWidth - 2 * g_padding));

        topSizer->Add(m_pSummary, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(g_padding));
        topSizer->Add(m_pDetail, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(g_padding));
    }

    // --- Mode-specific control ---
    if (m_mode == Mode::Similar) {
        std::vector<wxString> options = {_L("Strict"), _L("Balanced"), _L("Loose"), _L("Very Loose")};
        m_pTolerance                  = new SegmentedToggle(this, options, m_toleranceIndex);
        m_pTolerance->bindSelectionCallback([this](int index) {
            m_toleranceIndex = index;
            recompute();
        });
        topSizer->Add(m_pTolerance, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(g_padding - 8));
    } else {
        m_pMatchLoaded = new wxCheckBox(this, wxID_ANY, _L("Also set colour and material to match the loaded filament"));
        m_pMatchLoaded->SetValue(true);
        m_pMatchLoaded->SetForegroundColour(StateColor::darkModeColorFor(wxColour(g_labelColor)));
        topSizer->Add(m_pMatchLoaded, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(g_padding));
    }

    // --- Preview ---
    {
        m_pPreview = new MergePreviewList(this);
        m_pPreview->SetMinSize(FromDIP(wxSize(g_dialogWidth - 2 * g_padding, g_previewHeight)));
        topSizer->Add(m_pPreview, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(g_padding));
    }

    // --- Buttons ---
    {
        auto *btnRow = new wxBoxSizer(wxHORIZONTAL);

        auto *cancelBtn = new Button(this, _L("Cancel"));
        cancelBtn->SetMinSize(FromDIP(wxSize(g_btnW, g_btnH)));
        cancelBtn->SetCornerRadius(FromDIP(4));
        cancelBtn->SetBorderWidth(FromDIP(1));
        auto cancelBg = StateColor(std::make_pair(wxColour(g_secondaryHover), (int) StateColor::Hovered),
                                   std::make_pair(wxColour(g_blockBg), (int) StateColor::Normal));
        cancelBg.setTakeFocusedAsHovered(false);
        cancelBtn->SetBackgroundColor(cancelBg);
        cancelBtn->SetBorderColor(StateColor(wxColour(g_secondaryBorder)));
        cancelBtn->SetTextColor(StateColor(wxColour(g_secondaryText)));
        cancelBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
        btnRow->Add(cancelBtn, 0, wxEXPAND);

        btnRow->AddSpacer(FromDIP(g_btnRowGap));

        m_pMergeBtn = new Button(this, _L("Merge"));
        m_pMergeBtn->SetMinSize(FromDIP(wxSize(g_btnW, g_btnH)));
        m_pMergeBtn->SetCornerRadius(FromDIP(4));
        m_pMergeBtn->SetBorderWidth(0);
        m_pMergeBtn->SetBackgroundColor(StateColor(std::pair(wxColour(g_disabledBg), (int) StateColor::Disabled),
                                                   std::pair(wxColour(g_primaryHoverBg), (int) StateColor::Hovered),
                                                   std::pair(wxColour(g_primaryBg), (int) StateColor::Normal)));
        m_pMergeBtn->SetTextColor(StateColor(std::pair(wxColour(g_disabledText), (int) StateColor::Disabled),
                                             std::pair(wxColour(g_primaryText), (int) StateColor::Normal)));
        m_pMergeBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });
        btnRow->Add(m_pMergeBtn, 1, wxEXPAND);

        topSizer->Add(btnRow, 0, wxEXPAND | wxALL, FromDIP(g_padding));
    }

    SetSizer(topSizer);
    recompute();
    topSizer->SetSizeHints(this);
    CentreOnParent();
}

bool MergeFilamentsDialog::matchLoadedAppearance() const
{
    return m_pMatchLoaded != nullptr && m_pMatchLoaded->GetValue();
}

void MergeFilamentsDialog::recompute()
{
    const int tolerance = std::clamp(m_toleranceIndex, 0, static_cast<int>(std::size(g_toleranceDeltaE)) - 1);
    m_plan              = (m_mode == Mode::ToLoaded) ?
                              plan_merge_to_loaded(m_design, m_machine, m_mergeable) :
                              plan_merge_similar(m_design, g_toleranceDeltaE[tolerance], true, m_mergeable);

    // Rows follow the surviving slots so the list reads as the result, not as
    // a list of deletions.
    std::vector<MergePreviewList::Row> rows;
    rows.reserve(m_plan.survivors.size());
    for (size_t survivor : m_plan.survivors) {
        MergePreviewList::Row row;
        row.target_color = getMainColor(m_design[survivor].m_color);
        row.target_label = wxString::Format(_L("Filament %d"), static_cast<int>(survivor) + 1);
        row.pinned = std::find(m_plan.pinned.begin(), m_plan.pinned.end(), survivor) != m_plan.pinned.end();
        for (const FilamentMergeStep &step : m_plan.steps)
            if (step.target == survivor)
                row.source_colors.push_back(getMainColor(m_design[step.source].m_color));
        rows.push_back(std::move(row));
    }
    m_pPreview->setRows(std::move(rows));

    const int      before = static_cast<int>(m_design.size());
    const int      after  = static_cast<int>(m_plan.survivors.size());
    const wxString arrow  = wxString::FromUTF8("\xE2\x86\x92");
    m_pSummary->SetLabel(wxString::Format(_L("%d filaments %s %d"), before, arrow, after));

    wxString detail;
    if (m_plan.empty()) {
        detail = m_mode == Mode::ToLoaded ?
                     _L("Every filament already matches a different loaded filament, so there is nothing to merge.") :
                     _L("No two filaments are close enough at this tolerance. Try a looser setting.");
    } else if (m_mode == Mode::ToLoaded) {
        detail = _L("Each filament is merged into whichever slot best matches the filament loaded on the printer. "
                    "Painted regions and per-object assignments follow the merge.");
    } else {
        detail = _L("Filaments of the same material whose colours are within the chosen tolerance are merged together. "
                    "Painted regions and per-object assignments follow the merge.");
    }
    if (!m_plan.pinned.empty()) {
        detail += "\n";
        detail += wxString::Format(_L("%d filament(s) are kept because a mixed filament is built from them."),
                                   static_cast<int>(m_plan.pinned.size()));
    }
    m_pDetail->SetLabel(detail);
    m_pDetail->Wrap(FromDIP(g_dialogWidth - 2 * g_padding));

    m_pMergeBtn->Enable(!m_plan.empty());
    Layout();
}

} // namespace GUI
} // namespace Slic3r
