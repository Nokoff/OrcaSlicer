# Wrap onto surface

Use **Wrap onto surface** to conform a rectangle, cube, or other mesh relief to
a cylinder, tapered cylinder, or another curved model surface.

## Workflow

1. Add the base model and the shape as **parts of the same object**. If they are
   separate objects, assemble them into one object first.
2. Select only the shape part in the object list, then open **Wrap onto surface**
   using the toolbar icon showing a rectangle around a cylinder.
3. Choose the base part under **Target surface**.
4. Drag the shape onto the base, as with the emboss tool. Its local XY face
   aligns with the surface; its local Z direction supplies the relief thickness.
5. Click **Apply wrap**. The tool adds mesh detail and fits the shape to the
   actual target surface, including its taper.
6. Raise **Corner lift** to lift both ends relative to the middle. Move a
   coloured corner ring to adjust that corner independently; after the first
   wrap, releasing a ring updates the mesh. Numeric X/Y offsets provide precise
   control. After typing values, click **Apply wrap**.
7. Drag the wrapped shape to another position on the target to reproject it.

Corner rings correspond to the shape's original local footprint, viewed along
local -Z: bottom left is red, bottom right is green, top right is blue, and top
left is yellow. Corner lift moves the ends along local +Y with a smooth
quadratic transition to an unchanged center. All numeric dimensions are in
part coordinates, in millimetres, before part/instance scaling.

**Surface offset** moves the relief along its local Z direction relative to the
target surface. The default -0.05 mm gives a small overlap with the base.
**Mesh spacing** controls tessellation in the original footprint: lower values
follow curvature more closely but require more triangles. The operation has a
250,000 triangle budget and reports an error instead of applying a partial mesh.

## Editing and limitations

- The tool projects along local -Z onto the first target surface. Like emboss
  projection, this preserves relief depth along that direction; it does not
  preserve arc length or wrap all the way around the back of a cylinder.
- Use an unpainted source part. Source painting is rejected because
  tessellation changes facet identifiers. The target and its painting stay intact.
- Folded/collapsed corner layouts and missing target coverage are rejected.
- Each applied warp has an undo snapshot. During one tool session, adjustments
  always use the original source rather than repeatedly deforming the result.
- Closing the tool commits the resulting mesh. Saving a project preserves that
  mesh; the current corner settings and original source are session-only.
  Reopening the tool starts a new operation on the current mesh. Keep a copy of
  the source if you need to start over after closing or reopening the project.
- Editing a shared part affects all instances of that object.

## Validation

Automated coverage lives in `tests/libslic3r/test_surface_wrap.cpp` under the
`[SurfaceWrap]` tag. It exercises flat and tapered cylindrical targets, relief
thickness, closed and consistently oriented topology after adaptive subdivision,
independent corner edits, repeatability, coverage errors, folded footprints,
triangle budgets, and non-finite settings.

Verified on 21 October 2026: all four geometry test cases passed (94,846
assertions); the affected Release projects compiled and linked; the separate
`build/surface_wrap_release/snapmaker-orca.exe --help` startup check exited with
code 0. The normal application DLL was locked by a running slicer, so the
verified runnable build is in `build/surface_wrap_release/`. Viewport interaction
and physical printer fit have not been manually validated.

Manual viewport regression checklist:

1. Add a cylinder and a cube part. Select the cube and activate the tool.
2. Choose the cylinder and drag the cube onto its side. Apply the wrap and check
   the back of the relief against the cylinder in a cross-section or sliced view.
3. Repeat with a tapered cylinder. Increase corner lift and verify that the
   middle stays in place while the left/right ends move upward.
4. Drag each ring and verify the corresponding numeric corner offset changes.
   Check that releasing the mouse updates the mesh without moving the base.
5. Move the fitted shape along the cylinder; check that it reprojects without
   accumulating curvature from the previous position.
6. Move a corner off the target and check the error message, then correct it.
   Try crossed corners and very small mesh spacing to check safe rejection.
7. Undo/redo applied warps, switch tools during dragging, and save/reload the
   project. Confirm that selection and normal object movement are restored.

Manual printer and viewport validation must be recorded separately; automated
geometry tests do not establish physical fit or mouse interaction correctness.
