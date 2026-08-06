#pragma once

#include <cstddef>
#include <vector>

#include "FilamentData.hpp"

namespace Slic3r {

// One merge instruction, expressed in *original* project filament indices
// (0-based). `source` is removed and everything painted with it is repainted
// as `target`.
struct FilamentMergeStep
{
    size_t source = 0;
    size_t target = 0;
};

// A whole batch merge, computed up front so it can be previewed before any of
// it runs. Nothing here mutates the preset bundle.
struct FilamentMergePlan
{
    std::vector<FilamentMergeStep> steps;

    // Filaments that keep a slot, in ascending original index order.
    std::vector<size_t> survivors;

    // Parallel to `survivors`: index into the machine list that survivor was
    // matched against, or -1 when it did not represent a loaded filament.
    // Always empty for plan_merge_similar().
    std::vector<int> survivor_machine_match;

    // Filaments that had to survive because an enabled mixed filament uses
    // them as a component. Reported so the UI can explain the leftover slots.
    std::vector<size_t> pinned;

    bool empty() const { return steps.empty(); }
};

// Merge every project filament into whichever survivor best represents the
// same loaded filament. Matching is type-aware first and colour-aware second
// (see compute_color_match); the survivor of each group is the member
// perceptually closest to that loaded filament, so the slot that is kept is
// the best stand-in for the spool actually on the printer.
//
// `mergeable[i] == false` pins filament i: it keeps its own slot and is never
// merged away. Indices at or beyond mergeable.size() are treated as mergeable.
FilamentMergePlan plan_merge_to_loaded(const std::vector<GUI::FilamentData> &design,
                                       const std::vector<GUI::FilamentData> &machine,
                                       const std::vector<bool>              &mergeable);

// Cluster filaments whose colours fall within `max_delta_e` (CIEDE2000) of a
// cluster representative. Filaments are visited in slot order and each joins
// the closest representative within tolerance, so the lowest slot number of a
// group is the one that survives.
FilamentMergePlan plan_merge_similar(const std::vector<GUI::FilamentData> &design,
                                     float                                 max_delta_e,
                                     bool                                  require_same_type,
                                     const std::vector<bool>              &mergeable);

} // namespace Slic3r
