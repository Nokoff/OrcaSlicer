# Colour painting remap

Open Color Painting, then **Remap filaments** beside the Replace only controls.
Choose **Selected object** or **Entire plate**, choose each source filament's
destination, and click **Remap**. Mappings apply simultaneously, so swaps and
many-to-one replacements retain the painted regions. Unpainted surfaces retain
their assigned filament. Painting belongs to the object: all its instances,
including copies on other plates, share the result.

## Manual regression checks

Use two painted, multipart objects on one plate and an independent painted object
on a second plate. Include partially painted facets and unpainted faces.

1. With Selected object, swap filaments 1 and 2. Verify all painted parts of that
   object swap; the other objects and unpainted faces retain their colours.
2. Undo and redo inside Color Painting. Close the tool and undo/redo again.
   Verify the complete remap is one undo action.
3. With Entire plate, swap filaments 1 and 2. Verify both objects on that plate
   change and the independent object on the second plate retains its painting.
4. Map two source filaments to the same destination. Verify both painted regions
   change without losing their boundaries or changing the mesh.
5. Make an instance of a painted object on the second plate. Remap the first
   plate and verify the shared instance changes, and both slice results are
   invalidated. An independent copy must retain its painting.
6. Reorder the filament palette and include a mixed filament. Verify the source
   and destination swatches correspond to the displayed filament numbers.
7. Cancel a mapping, then reopen the panel. Verify no painting changed and the
   mapping starts at identity. Applying identity must leave painting unchanged.
8. Save and reopen a 3MF, then slice the affected plates. Verify the remapped
   painting persists in the project and the preview uses the new filaments.

## Automated regression

`[FilamentRemap]` in `tests/libslic3r/test_triangle_selector.cpp` exercises a
filament swap on subdivided painting, serialization, unpainted faces, and the
reverse swap. It also covers remapping only valid leaf triangles rather than
reading a split parent as a painted triangle.

Validation on 2026-10-04: the Release slicer and GUI libraries compiled, and the
Release test executable built. Running `[TriangleSelector]` passed all 81
assertions in 13 test cases. `git diff --check` passed. After closing the running
Orca instances, the Release application target linked successfully, producing
`build/src/Release/Snapmaker_Orca.dll`. The manual GUI checks above have not been
run.
