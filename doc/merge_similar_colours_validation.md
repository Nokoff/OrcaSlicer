# Merge Similar Colours validation

The dialog offers a Delta E slider from 0.0 to 25.0 in increments of 0.1.
Strict, Balanced, Loose and Very Loose select 2.5, 6.0, 12.0 and 25.0.
Between preset values, no preset button is selected.

Click a target row or source swatch in the preview, or choose a source filament
from the dropdown. Hover a swatch to see its filament number. Choose Automatic,
Keep separate (block merge), or a target filament of the same material.
Explicit choices survive slider and preset changes until reset or dialog closure.
Manual targets can be outside the colour tolerance and always retain their slot.
A filament receiving a manual merge must have those sources released or remapped
before it can itself be manually merged away.

Mixed-filament components cannot be merged away. Other filaments may merge into
them. The preview and final Merge action use the same computed plan.

## Automated checks

`tests/slic3rutils/FilamentMergePlan_test.cpp` covers intermediate tolerances,
cancelled automatic merges, persistent remaps, surviving targets, material
restrictions, protected components, and invalid or cyclic mappings.

Build `filament_merge_tests` and run it with the `[filament_merge]` Catch2 filter.

Validated on Windows in Release configuration: all 3 cases passed (39 assertions).
The changed dialog, planner and segmented toggle translation units also compiled
successfully with MSVC. The full application was not relinked or run interactively.

## Manual UI regression checks

1. Open a project with two nearby PLA colours, a distinct PLA colour, and PETG.
2. Open Merge Similar Filaments. Confirm Balanced and Delta E 6.0 initially.
3. Move the slider between presets, then select each preset. Verify the displayed
   value, button selection, preview, filament count and Merge availability agree.
4. Click an absorbed swatch and choose Keep separate. Confirm its slot returns.
   Change the tolerance and confirm this source remains separate.
5. Remap a source to a distant PLA colour. Confirm the chosen destination survives,
   PETG is unavailable, and the preview and result label agree.
6. Return a source to Automatic, then use Reset manual choices. Confirm the
   automatic plan is restored at the current tolerance.
7. Repeat with mixed-filament components; confirm their outgoing mapping is disabled.
8. Cancel and verify project filaments, painting and assignments remain unchanged.
9. Reopen, apply a manual remap and merge. Verify the surviving slots, object
   assignments and painted regions match the preview. Undo and verify restoration.
10. Check keyboard selection, scrolling, resizing, dark mode and display scaling.
    Also verify Merge to Loaded Filaments retains its previous behaviour.

Native UI checks require an interactive application run; they are not automated
by the planner tests.
