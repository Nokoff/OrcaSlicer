# Emboss on cut objects

Text creation previously rejected any selected object with a valid cut ID.
Text is created as a separate volume, so adding it can preserve the original
cut parts, connector geometry, tolerances, and correspondence. Separate text
volumes can now be positioned and deleted without invalidating cut info.
Original cut parts and connectors retain their existing protections.

Hidden connector rows now resolve to model indices within their own object for
selection, filament changes, manipulation, reordering, and deletion. Surface
projection and placement exclude connectors. Parts are transformed into the
reference part's coordinates before projection; the nearest-surface fallback
queries in local coordinates and compares distances in world coordinates.

## Automated regression

`tests/libslic3r/test_cut_colors.cpp`, tagged `[Cut][Emboss]`, checks raised,
engraved, and modifier text on an object with a connector and a related cut
object. The Use Surface geometry path projects a fixed glyph footprint onto a
curved body and extrudes it, checking that the surface varies in height and that
the emboss depth is preserved. Adding, editing, copying into a model snapshot,
and deleting text must preserve both objects' cut IDs and the original volumes' meshes, transforms,
types, and connector tolerances.

The regressions also cover assigning filament to text after hidden connectors,
alternating between objects with different connector layouts, and projecting
onto offset and scaled parts. A nearest-point regression covers the placement
fallback on a translated, rotated, and scaled volume.

Validation result: the Release test executable passed the `[Cut]` suite with
852 assertions across 11 test cases. The GUI library compiled and the app DLL
linked successfully in `build/emboss_cut_release_v2`. Interactive GUI validation
remains necessary.

The read-only CLI check of a local copy of the reported project could not verify
project loading: `--info` rejects the newer project version, and
`--allow-newer-file --info` terminates with access violation code `0xC0000005`
after plate exclude-triangle errors. The earlier build gives the same result.
This CLI limitation is separate from the tested text indexing and surface fixes.

## Manual GUI validation

These steps require an interactive app run; the model regression does not
exercise the GUI or project serialization.

1. Cut a cube with plug or dowel connectors, retaining both halves.
2. Select one cut half and use the Text toolbar button or Ctrl+T. Text should
   appear and the editor should open. Repeat through Add part > Text and
   Add negative part > Text.
3. Edit the text, enable surface projection, and switch between raised text,
   engraved text, and modifier text. Select the individual text volume and
   change its position through the manipulation panel. Change the raised text's
   filament and check that only the text changes colour. Repeat after selecting
   another object with a different number of connectors.
4. Delete the text. There should be no request to invalidate cut info. Undo
   and redo creation, editing, and deletion; the connectors should remain.
5. Select an original cut part: its individual manipulation should remain
   locked. Attempt to delete it or a connector through the existing connector
   controls: the existing cut correspondence protection should remain.
6. Save and reopen a 3MF project, edit the text again, and slice both halves.
   Inspect the preview for text placement and unchanged connector fit.
7. With an empty plate, create standalone text with Ctrl+T, then delete it.
   Ordinary objects should also continue to support text creation and editing.
8. Repeat surface placement on an assembly whose body parts have different
   offsets or scales, with both solid connectors and negative connector holes.
   Text should follow body surfaces without snapping onto a connector or hole.
