#pragma once

#include <vector>

#include <wx/dialog.h>

#include "FilamentData.hpp"
#include "FilamentMergePlan.hpp"

class Button;
class wxCheckBox;
class wxStaticText;

namespace Slic3r
{
namespace GUI
{

class SegmentedToggle;
class MergePreviewList;

// Previews a batch filament merge and lets the user commit it. The dialog only
// ever computes plans — applying one is the sidebar's job.
class MergeFilamentsDialog : public wxDialog
{
public:
    enum class Mode
    {
        ToLoaded, // collapse onto the filaments loaded on the printer
        Similar,  // collapse filaments that already look alike
    };

    MergeFilamentsDialog(wxWindow                        *parent,
                         Mode                             mode,
                         const std::vector<FilamentData> &design,
                         const std::vector<FilamentData> &machine,
                         const std::vector<bool>         &mergeable);

    const FilamentMergePlan &plan() const { return m_plan; }

    // ToLoaded only: whether the surviving slots should also take the colour
    // and material of the loaded filament they were matched to.
    bool matchLoadedAppearance() const;

private:
    void recompute();

    Mode                      m_mode;
    std::vector<FilamentData> m_design;
    std::vector<FilamentData> m_machine;
    std::vector<bool>         m_mergeable;
    FilamentMergePlan         m_plan;

    SegmentedToggle  *m_pTolerance   = nullptr;
    wxCheckBox       *m_pMatchLoaded = nullptr;
    MergePreviewList *m_pPreview     = nullptr;
    wxStaticText     *m_pSummary     = nullptr;
    wxStaticText     *m_pDetail      = nullptr;
    Button           *m_pMergeBtn    = nullptr;

    int m_toleranceIndex = 1;
};

} // namespace GUI
} // namespace Slic3r
