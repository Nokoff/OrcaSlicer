# Preserving project settings when a foreign printer cannot be resolved

## Symptom

Importing a MakerWorld / Bambu Studio project authored for a printer this build
does not ship (e.g. `Bambu Lab H2C`, `Bambu Lab P2S`) silently loses the
project's process settings. Reported 2026-08-03 with `Buzz+Colored.3mf`: all six
support customisations were replaced by `0.20 Standard @Snapmaker U1` defaults.

| Setting | In the 3mf | After import |
|---|---|---|
| `enable_support` | 1 | unchecked |
| `support_top_z_distance` | 0 | 0.2 |
| `support_interface_filament` | 7 | Default |
| `support_interface_spacing` | 0 | U1 default |
| `support_interface_pattern` | rectilinear_interlaced | U1 default |
| `independent_support_layer_height` | 0 | U1 default |

## Why it happens

1. `printer_model = Bambu Lab H2C`; `resources/profiles` contains **no** `*H2C*`
   file. The printer preset cannot inherit from a system preset
   (`Preset.cpp:1977`, "no inherit, set to not found").
2. `validate_presets()` detects this but only drives a warning dialog.
   `Plater.cpp:10824` then says `//always load config`, so the config *is* read.
3. Process presets are printer scoped. When the printer resolves to something
   else (fallback, or the user switching manually), the project's process preset
   is deselected and `0.20 Standard @Snapmaker U1 (0.4 nozzle)` takes over.
4. The project's values lived **in** that preset, not as modifications on top of
   a system preset. So there is no dirty diff, the "Transfer or discard changes"
   dialog comes up empty, and Transfer carries nothing.

Point 4 is the crux: the existing transfer mechanism can only move
*modifications relative to a selected preset*, which a project-embedded preset
does not have.

## The data we already have

`Metadata/project_settings.config` carries `different_settings_to_system`, a per
preset list of exactly the keys the author changed away from their system preset.
For `Buzz+Colored.3mf` entry `[0]` (process) is 7 keys:

```
enable_support, independent_support_layer_height, support_interface_filament,
support_interface_pattern, support_interface_spacing, support_top_z_distance,
timelapse_type
```

That is precisely the set that must survive. It needs no inference.

## Correction (2026-08-03, verified against a running build)

The symptom table above is right about the outcome but wrong about *when* it
happens, and the fix proposed below does not work as written. Verified:

- The project's process preset **does** import correctly, as a project-embedded
  preset (`0.2mm supports @BBL P2S ...`), holding every authored value. Nothing
  is lost during import. The printer imports the same way.
- The loss happens on the **printer switch**. `Tab.cpp:5521` computes
  `pu.old_preset_dirty = ... && pu.presets->current_is_dirty()`. A project
  -embedded preset is *clean*, so `may_discard_current_dirty_preset` is never
  called for it and `Tab.cpp:5532` drops it with no transfer ever offered.
- The empty transfer table has a structural cause, not the `coString` skip at
  `UnsavedChangesDialog.cpp:1694`. `update_tree` builds its diff from
  `old_config = selected.config` vs `new_config = edited.config`
  (`UnsavedChangesDialog.cpp:1656`). For a project-embedded preset those are the
  same object, so the diff is empty by construction. The dialog has no second
  config to compare against.

A first attempt (re-applying the captured keys at import, guarded on "only touch
values that differ") was a no-op for exactly this reason: at import nothing
differs yet. Guarding on value equality inverts the fix. Left in the tree behind
`[nokoff]` diagnostics; it should be removed or replaced.

### What actually has to change

The values must be expressed as *modifications relative to a baseline*, because
modifications are the only thing the transfer machinery can carry. Chosen
approach (user's call, 2026-08-03): leave the import faithful and fix the switch
path, rather than rebasing on open.

Implemented 2026-08-03, in three parts:

1. **`Preset::project_changed_keys`** (`Preset.hpp`). Populated in
   `load_external_preset` where the project-embedded preset is created, from the
   `different_settings_list` that function already receives.

   Trap worth remembering: `load_preset()` copies the preset into
   `m_edited_preset` *before* `is_project_embedded` is set on it, which is why
   the pre-existing code re-sets `is_external` on the edited copy by hand. It
   never did the same for `is_project_embedded`, so that flag reads false on the
   edited preset — and the switch path reads exactly that preset. Both flags plus
   the key list are now carried across.

2. **`Tab.cpp:5521`** ORs in "project preset carrying author changes" alongside
   `current_is_dirty()`, so `may_discard_current_dirty_preset` runs instead of
   the preset being dropped at `Tab.cpp:5532`.

3. **`UnsavedChangesDialog::update_tree`** no longer binds `old_config`
   unconditionally to the selected preset. When the diff is empty *and* the
   preset is project-embedded, it compares against `presets->default_preset()`,
   restricted to the author's changed keys, filtering out the ignore-list keys
   folded into that list, keys this build no longer defines, and values the
   default already agrees with. `old_config` then binds to whichever basis was
   chosen, leaving every downstream use untouched.

   Known cosmetic limitation: the "old value" column shows built-in defaults
   rather than the incoming preset's values, because the replacement preset is
   not known at that point. This does not affect what Transfer carries —
   `Tab.cpp:5806` caches the moved values from the *edited* preset, which holds
   the project's real settings.

Verified working in the GUI on 2026-08-03 with `Buzz+Colored.3mf`: opening the
project and switching the printer to a Snapmaker U1 now lists the author's
support settings in the transfer dialog, and Transfer carries them onto the U1
process preset. CLI slicing segfaults in this environment, so this path can only
be exercised by hand — there is no automated coverage of it.

## Proposed fix (superseded — see the correction above)

When a project's process preset cannot be matched to a system preset, re-apply
the project's customised keys on top of whichever process preset ends up
selected, instead of discarding them.

- **Where:** `Plater::priv::load_files`, after preset selection, near the
  `//always load config` block at `Plater.cpp:10824`.
- **What:** read `different_settings_to_system[0]`, and for each key present in
  the loaded project config, apply it to the edited process preset. Leave the
  preset marked dirty so the user sees the values as modifications they can save
  or discard — which also makes them visible to the transfer dialog on any later
  printer switch, fixing that path for free.
- **Guard:** only when the project's process preset did not resolve to a system
  preset. A project whose printer *is* installed must keep today's behaviour.
- **Filament indices:** `support_interface_filament = 7` cannot be honoured on a
  printer with fewer filaments. Clamp to the available count and report it,
  rather than dropping the whole setting.

## Related, not covered here

- **Empty transfer dialog.** `UnsavedChangesDialog.cpp:1694` silently skips
  options the searcher cannot resolve when they are `coString`/`coStrings`.
  Introduced by `ac3dafe08a6` (2026-05-26), upstream and pre-existing. Even with
  the fix above it should never render an empty table; it should say there is
  nothing transferable.
- **Relative E validation.** The imported P2S preset triggers "Relative extruder
  addressing requires resetting the extruder position at each layer ... Add
  \"G92 E0\" to layer_gcode". This is what prompted the printer switch in the
  first place. Foreign printer presets should either carry their own layer gcode
  through or fail more gently.

## Test

Add to `tests/libslic3r/test_config.cpp`: load this project config with a printer
that does not exist in the bundle, assert the six support keys survive on the
resulting process config.
