#include "MergeFilamentsDialog.hpp"

#include <algorithm>
#include <functional>
#include <iterator>

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/SegmentedToggle.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"

namespace {

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
constexpr const char* g_dialogBg        = "#F8F7F7";
constexpr const char* g_blockBg         = "#FFFFFF";
constexpr const char* g_labelColor      = "#242424";
constexpr const char* g_mutedColor      = "#6B6A6A";
constexpr const char* g_swatchBorder    = "#D1D5DC";
constexpr const char* g_secondaryBorder = "#D1D5DC";
constexpr const char* g_secondaryText   = "#242424";
constexpr const char* g_secondaryHover  = "#F3F4F6";
constexpr const char* g_primaryBg       = "#019687";
constexpr const char* g_primaryHoverBg  = "#26A69A";
constexpr const char* g_primaryText     = "#FEFEFE";
constexpr const char* g_disabledBg      = "#DFDFDF";
constexpr const char* g_disabledText    = "#6B6A6A";

// Tolerance presets for "merge similar", as CIEDE2000 distances. Roughly:
// 2.5 is the threshold where a side-by-side difference stops being obvious,
// 6 keeps a colour family together, 12 lumps in anything broadly alike, and
// 25 allows substantially different shades to merge as a last-resort option.
constexpr float g_toleranceDeltaE[] = {2.5f, 6.0f, 12.0f, 25.0f};
constexpr int   g_toleranceDefault  = 1;

} // namespace

namespace Slic3r { namespace GUI {

// =====================================================================
// MergePreviewList — one row per surviving filament, custom painted so a
// recomputed plan is a single repaint instead of a widget rebuild.
// =====================================================================

class MergePreviewList : public wxScrolledWindow
{
public:
    struct Row
    {
        size_t                target_index = 0;
        wxColour              target_color;
        wxString              target_label;
        std::vector<wxColour> source_colors;
        std::vector<size_t>   source_indices;
        bool                  pinned = false;
    };

    explicit MergePreviewList(wxWindow* parent) : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_blockBg)));
        Bind(wxEVT_PAINT, &MergePreviewList::onPaint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            const int index = filamentAt(event.GetPosition());
            if (index >= 0 && onSelect)
                onSelect(static_cast<size_t>(index));
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            const int index = filamentAt(event.GetPosition());
            SetToolTip(index >= 0 ? wxString::Format(_L("Filament %d"), index + 1) : wxString());
            event.Skip();
        });
    }

    std::function<void(size_t)> onSelect;

    void setRows(std::vector<Row> rows)
    {
        m_rows = std::move(rows);
        // Vertical only, one row per scroll unit.
        SetScrollbars(0, FromDIP(g_rowHeight), 0, static_cast<int>(m_rows.size()));
        Scroll(0, 0);
        Refresh();
    }

private:
    int filamentAt(const wxPoint& position) const
    {
        const wxPoint point = CalcUnscrolledPosition(position);
        for (const auto& hit : m_hits)
            if (hit.first.Contains(point))
                return static_cast<int>(hit.second);
        return -1;
    }

    void onPaint(wxPaintEvent&)
    {
        m_hits.clear();
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
            const Row& row = m_rows[i];
            const int  y   = static_cast<int>(i) * rowH;
            int        x   = margin;

            m_hits.emplace_back(wxRect(x, y, FromDIP(g_targetSwatch + g_swatchGap * 2 + g_labelWidth), rowH), row.target_index);

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
                    gdc.DrawText(wxString::Format("+%d", static_cast<int>(row.source_colors.size() - s)), x, y + (rowH - textSize.y) / 2);
                    break;
                }
                if (x + swatch > limit)
                    break;
                m_hits.emplace_back(wxRect(x, y, swatch, rowH), row.source_indices[s]);
                x += drawSwatch(gdc, x, y, rowH, swatch, row.source_colors[s]) + FromDIP(g_swatchGap);
            }
        }
    }

    // Returns the width consumed so callers can keep advancing.
    int drawSwatch(wxGCDC& gdc, int x, int y, int rowH, int size, const wxColour& color)
    {
        gdc.SetPen(wxPen(wxColour(g_swatchBorder)));
        gdc.SetBrush(wxBrush(color));
        gdc.DrawRoundedRectangle(x, y + (rowH - size) / 2, size, size, FromDIP(g_swatchRadius));
        return size;
    }

    std::vector<Row>                       m_rows;
    std::vector<std::pair<wxRect, size_t>> m_hits;
};

// =====================================================================
// MergeFilamentsDialog
// =====================================================================

MergeFilamentsDialog::MergeFilamentsDialog(wxWindow*                        parent,
                                           Mode                             mode,
                                           const std::vector<FilamentData>& design,
                                           const std::vector<FilamentData>& machine,
                                           const std::vector<bool>&         mergeable)
    : wxDialog(parent,
               wxID_ANY,
               mode == Mode::ToLoaded ? _L("Merge to Loaded Filaments") : _L("Merge Similar Filaments"),
               wxDefaultPosition,
               wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_mode(mode)
    , m_design(design)
    , m_machine(machine)
    , m_mergeable(mergeable)
    , m_manualTargets(design.size(), -1)
    , m_toleranceIndex(g_toleranceDefault)
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour(g_dialogBg)));

    auto* topSizer = new wxBoxSizer(wxVERTICAL);

    // --- Summary ---
    {
        m_pSummary         = new wxStaticText(this, wxID_ANY, wxEmptyString);
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
        m_pToleranceValue = new wxStaticText(this, wxID_ANY, wxEmptyString);
        topSizer->Add(m_pToleranceValue, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(g_padding));
        m_pToleranceSlider = new wxSlider(this, wxID_ANY, static_cast<int>(g_toleranceDeltaE[g_toleranceDefault] * 10), 0, 250);
        m_pToleranceSlider->SetName(_L("Colour merge tolerance"));
        m_pToleranceSlider->SetToolTip(_L("Lower values keep more colours separate. Higher values merge more shades. "
                                          "Manual choices are kept when the tolerance changes."));
        topSizer->Add(m_pToleranceSlider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(g_padding));
        m_pToleranceSlider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) {
            m_toleranceIndex = -1;
            for (size_t i = 0; i < std::size(g_toleranceDeltaE); ++i)
                if (m_pToleranceSlider->GetValue() == static_cast<int>(g_toleranceDeltaE[i] * 10))
                    m_toleranceIndex = static_cast<int>(i);
            m_pTolerance->setSelected(m_toleranceIndex);
            recompute();
        });
        std::vector<wxString> options = {_L("Strict"), _L("Balanced"), _L("Loose"), _L("Very Loose")};
        m_pTolerance                  = new SegmentedToggle(this, options, m_toleranceIndex);
        m_pTolerance->bindSelectionCallback([this](int index) {
            m_toleranceIndex = index;
            m_pToleranceSlider->SetValue(static_cast<int>(g_toleranceDeltaE[index] * 10));
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
        m_pPreview->SetMinSize(FromDIP(wxSize(g_dialogWidth - 2 * g_padding, m_mode == Mode::Similar ? 180 : g_previewHeight)));
        topSizer->Add(m_pPreview, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(g_padding));
    }

    if (m_mode == Mode::Similar) {
        auto* editor = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
        editor->AddGrowableCol(1);
        editor->Add(new wxStaticText(this, wxID_ANY, _L("Source filament")), 0, wxALIGN_CENTER_VERTICAL);
        m_pSource = new wxChoice(this, wxID_ANY);
        for (size_t i = 0; i < m_design.size(); ++i)
            m_pSource->Append(wxString::Format(_L("Filament %d"), static_cast<int>(i) + 1) + "  " +
                              getMainColor(m_design[i].m_color).GetAsString(wxC2S_HTML_SYNTAX));
        if (!m_design.empty())
            m_pSource->SetSelection(0);
        editor->Add(m_pSource, 1, wxEXPAND);
        editor->Add(new wxStaticText(this, wxID_ANY, _L("Merge into")), 0, wxALIGN_CENTER_VERTICAL);
        m_pTarget = new wxChoice(this, wxID_ANY);
        editor->Add(m_pTarget, 1, wxEXPAND);
        topSizer->Add(editor, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(g_padding));
        m_pMapping = new wxStaticText(this, wxID_ANY, wxEmptyString);
        // Reserve two lines so choosing a protected destination does not move
        // the action buttons or grow the minimum height on every selection.
        m_pMapping->SetMinSize(wxSize(-1, m_pMapping->GetCharHeight() * 2));
        topSizer->Add(m_pMapping, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(g_padding));
        auto* reset = new Button(this, _L("Reset manual choices"));
        reset->SetMinSize(FromDIP(wxSize(g_btnW, g_btnH)));
        reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            std::fill(m_manualTargets.begin(), m_manualTargets.end(), -1);
            recompute();
        });
        topSizer->Add(reset, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(g_padding));
        m_pSource->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { updateManualEditor(); });
        m_pPreview->onSelect = [this](size_t source) {
            m_pSource->SetSelection(static_cast<int>(source));
            updateManualEditor();
            m_pTarget->SetFocus();
        };
        m_pTarget->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            const int source = m_pSource->GetSelection();
            const int option = m_pTarget->GetSelection();
            if (source >= 0 && option >= 0 && option < static_cast<int>(m_targetOptions.size())) {
                m_manualTargets[source] = m_targetOptions[option];
                recompute();
            }
        });
    }

    // --- Buttons ---
    {
        auto* btnRow = new wxBoxSizer(wxHORIZONTAL);

        auto* cancelBtn = new Button(this, _L("Cancel"));
        cancelBtn->SetMinSize(FromDIP(wxSize(g_btnW, g_btnH)));
        cancelBtn->SetCornerRadius(FromDIP(4));
        cancelBtn->SetBorderWidth(FromDIP(1));
        auto cancelBg = StateColor(std::make_pair(wxColour(g_secondaryHover), (int) StateColor::Hovered),
                                   std::make_pair(wxColour(g_blockBg), (int) StateColor::Normal));
        cancelBg.setTakeFocusedAsHovered(false);
        cancelBtn->SetBackgroundColor(cancelBg);
        cancelBtn->SetBorderColor(StateColor(wxColour(g_secondaryBorder)));
        cancelBtn->SetTextColor(StateColor(wxColour(g_secondaryText)));
        cancelBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
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
        m_pMergeBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        btnRow->Add(m_pMergeBtn, 1, wxEXPAND);

        topSizer->Add(btnRow, 0, wxEXPAND | wxALL, FromDIP(g_padding));
    }

    SetSizer(topSizer);
    recompute();
    topSizer->SetSizeHints(this);
    CentreOnParent();
}

bool MergeFilamentsDialog::matchLoadedAppearance() const { return m_pMatchLoaded != nullptr && m_pMatchLoaded->GetValue(); }

void MergeFilamentsDialog::recompute()
{
    m_plan = (m_mode == Mode::ToLoaded) ?
                 plan_merge_to_loaded(m_design, m_machine, m_mergeable) :
                 plan_merge_similar(m_design, m_pToleranceSlider->GetValue() / 10.0f, true, m_mergeable, m_manualTargets);
    if (m_pToleranceValue)
        m_pToleranceValue->SetLabel(wxString::Format(_L("Colour tolerance (Delta E): %.1f"), m_pToleranceSlider->GetValue() / 10.0));

    // Rows follow the surviving slots so the list reads as the result, not as
    // a list of deletions.
    std::vector<MergePreviewList::Row> rows;
    rows.reserve(m_plan.survivors.size());
    for (size_t survivor : m_plan.survivors) {
        MergePreviewList::Row row;
        row.target_index = survivor;
        row.target_color = getMainColor(m_design[survivor].m_color);
        row.target_label = wxString::Format(_L("Filament %d"), static_cast<int>(survivor) + 1);
        row.pinned       = std::find(m_plan.pinned.begin(), m_plan.pinned.end(), survivor) != m_plan.pinned.end();
        for (const FilamentMergeStep& step : m_plan.steps)
            if (step.target == survivor) {
                row.source_colors.push_back(getMainColor(m_design[step.source].m_color));
                row.source_indices.push_back(step.source);
            }
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
                     _L("No merges are selected. Increase the tolerance or change a manual choice below.");
    } else if (m_mode == Mode::ToLoaded) {
        detail = _L("Each filament is merged into whichever slot best matches the filament loaded on the printer. "
                    "Painted regions and per-object assignments follow the merge.");
    } else {
        detail = _L("Filaments of the same material whose colours are within the chosen tolerance are merged together. "
                    "Use the controls below to keep a colour separate or choose its target. "
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
    updateManualEditor();
    Layout();
}

void MergeFilamentsDialog::updateManualEditor()
{
    if (!m_pSource)
        return;
    const int source = m_pSource->GetSelection();
    m_pTarget->Clear();
    m_targetOptions.clear();
    if (source < 0) {
        m_pTarget->Disable();
        return;
    }

    auto add = [this](const wxString& label, int target) {
        m_pTarget->Append(label);
        m_targetOptions.push_back(target);
    };
    add(_L("Automatic"), -1);
    add(_L("Keep separate (block merge)"), source);
    const bool pinned = std::find(m_plan.pinned.begin(), m_plan.pinned.end(), source) != m_plan.pinned.end();
    // A manually selected destination cannot also be merged away. This keeps
    // every preview arrow pointing directly at a surviving slot.
    bool destination = false;
    for (size_t i = 0; i < m_manualTargets.size(); ++i)
        if (static_cast<int>(i) != source && m_manualTargets[i] == source)
            destination = true;
    if (!pinned && !destination) {
        for (size_t i = 0; i < m_design.size(); ++i) {
            if (static_cast<int>(i) == source || m_design[i].m_type != m_design[source].m_type ||
                (m_manualTargets[i] >= 0 && m_manualTargets[i] != static_cast<int>(i)))
                continue;
            add(wxString::Format(_L("Filament %d"), static_cast<int>(i) + 1) + "  " +
                    getMainColor(m_design[i].m_color).GetAsString(wxC2S_HTML_SYNTAX),
                static_cast<int>(i));
        }
    }
    for (size_t i = 0; i < m_targetOptions.size(); ++i)
        if (m_targetOptions[i] == m_manualTargets[source])
            m_pTarget->SetSelection(static_cast<int>(i));
    m_pTarget->Enable(!pinned);

    wxString mapping = _L("Result: kept as is");
    for (const FilamentMergeStep& step : m_plan.steps)
        if (step.source == static_cast<size_t>(source))
            mapping = wxString::Format(_L("Result: merges into Filament %d"), static_cast<int>(step.target) + 1);
    if (pinned)
        mapping = _L("Kept: used by a mixed filament.");
    else if (destination)
        mapping = _L("Kept: a manual merge targets this filament. Remap its sources first to release it.");
    m_pMapping->SetLabel(mapping);
    m_pMapping->Wrap(FromDIP(g_dialogWidth - 2 * g_padding));
    Layout();
}

}} // namespace Slic3r::GUI
