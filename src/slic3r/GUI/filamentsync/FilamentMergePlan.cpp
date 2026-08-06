#include "FilamentMergePlan.hpp"

#include <algorithm>
#include <limits>

#include "FilamentSyncAlgorithm.hpp"

namespace Slic3r {

namespace {

float color_distance(const GUI::FilamentData &lhs, const GUI::FilamentData &rhs)
{
    const wxColour a = GUI::getMainColor(lhs.m_color);
    const wxColour b = GUI::getMainColor(rhs.m_color);
    return delta_e_ciede2000(a.Red(), a.Green(), a.Blue(), b.Red(), b.Green(), b.Blue());
}

bool is_mergeable(const std::vector<bool> &mergeable, size_t i)
{
    return i >= mergeable.size() || mergeable[i];
}

// Turn a "who absorbs whom" table into survivors + merge steps. A filament
// pointing at itself keeps its slot; anything else becomes one merge.
void collect(FilamentMergePlan &plan, const std::vector<int> &survivor_of)
{
    for (size_t i = 0; i < survivor_of.size(); ++i) {
        if (survivor_of[i] == static_cast<int>(i))
            plan.survivors.push_back(i);
        else if (survivor_of[i] >= 0)
            plan.steps.push_back({i, static_cast<size_t>(survivor_of[i])});
    }
}

} // namespace

FilamentMergePlan plan_merge_to_loaded(const std::vector<GUI::FilamentData> &design,
                                       const std::vector<GUI::FilamentData> &machine,
                                       const std::vector<bool>              &mergeable)
{
    FilamentMergePlan plan;
    const size_t      count = design.size();
    if (count == 0 || machine.empty())
        return plan;

    // Which loaded filament each project filament looks most like. Same
    // material wins over a marginally closer colour.
    const std::vector<int> match = compute_color_match(design, machine);

    // Each loaded filament is represented by the project filament closest to
    // it, so the surviving slot is the best available stand-in for the spool.
    std::vector<int>   group_survivor(machine.size(), -1);
    std::vector<float> group_best(machine.size(), std::numeric_limits<float>::max());
    for (size_t i = 0; i < count; ++i) {
        const int m = match[i];
        if (m < 0)
            continue;
        const float dist = color_distance(design[i], machine[m]);
        if (dist < group_best[m]) {
            group_best[m]     = dist;
            group_survivor[m] = static_cast<int>(i);
        }
    }

    std::vector<int> survivor_of(count, -1);
    for (size_t i = 0; i < count; ++i) {
        if (!is_mergeable(mergeable, i)) {
            survivor_of[i] = static_cast<int>(i);
            plan.pinned.push_back(i);
            continue;
        }
        // No loaded filament to fall back on: leave the slot alone rather than
        // guessing.
        const int m    = match[i];
        survivor_of[i] = (m < 0 || group_survivor[m] < 0) ? static_cast<int>(i) : group_survivor[m];
    }

    collect(plan, survivor_of);

    // Only the filament that actually represents a loaded spool may be
    // retinted to it; a pinned leftover keeps whatever colour it had, because
    // a mixed filament is built from it.
    plan.survivor_machine_match.assign(plan.survivors.size(), -1);
    for (size_t k = 0; k < plan.survivors.size(); ++k) {
        const size_t s = plan.survivors[k];
        const int    m = match[s];
        if (m >= 0 && group_survivor[m] == static_cast<int>(s))
            plan.survivor_machine_match[k] = m;
    }

    return plan;
}

FilamentMergePlan plan_merge_similar(const std::vector<GUI::FilamentData> &design,
                                     float                                 max_delta_e,
                                     bool                                  require_same_type,
                                     const std::vector<bool>              &mergeable)
{
    FilamentMergePlan   plan;
    const size_t        count = design.size();
    std::vector<int>    survivor_of(count, -1);
    std::vector<size_t> representatives;

    for (size_t i = 0; i < count; ++i) {
        if (!is_mergeable(mergeable, i)) {
            // Pinned filaments never merge away, but others may still merge
            // into them.
            survivor_of[i] = static_cast<int>(i);
            representatives.push_back(i);
            plan.pinned.push_back(i);
            continue;
        }

        int   best      = -1;
        float best_dist = max_delta_e;
        for (size_t r : representatives) {
            if (require_same_type && design[r].m_type != design[i].m_type)
                continue;
            const float dist = color_distance(design[r], design[i]);
            if (dist <= best_dist) {
                best_dist = dist;
                best      = static_cast<int>(r);
            }
        }

        if (best < 0) {
            survivor_of[i] = static_cast<int>(i);
            representatives.push_back(i);
        } else {
            survivor_of[i] = best;
        }
    }

    collect(plan, survivor_of);
    return plan;
}

} // namespace Slic3r
