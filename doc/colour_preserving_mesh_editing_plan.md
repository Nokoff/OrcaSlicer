# Plan: basic mesh editing with colour preservation

Prepared: 20 November 2026. Implementation status updated: 21 November 2026. Core implementation and automated verification are in place; desktop interaction, reference-machine acceptance, and the 5M-face brush setup target remain open.

## Worktree status

- The smooth brush supports small numeric diameters, a displacement cap accumulated from each vertex's stroke-start position across repeated dabs, optional local refinement with barycentrically mapped stroke origins and source correspondence (target edge length is four brush radii around the inserted hit-centred vertex to avoid over-refining tiny footprints), an optional world-space sharp-edge pinning control (>60° dihedral angle), and conservative swept-vertex plus swept-patch collision/clearance rejection against nearby unrelated faces. Stale collision-tree queries include the stroke displacement bound for previously moved faces while filtering unchanged faces against the tight brush sweep. Duplicate positions are detected from compact sorted coordinate keys; ordinary indexed meshes skip the duplicate-edge table and release the temporary key array before adjacency construction, while coincident-edge vertices remain synchronized and point-touching shells remain separate. Large world-vertex, position-key, edge-key, and AABB input arrays are filled in parallel; AABB construction also splits independent subtrees into TBB tasks above 1,024 leaves. Neighbor CSR slices now inherit ascending vertex order from globally sorted edge keys, avoiding a redundant sort per vertex. Fine-detail regression coverage spans 0.1 / 0.25 / 0.5 mm features, coarse and dense meshes, 10 mm and 1,000 mm extents, and a rotated, translated, non-uniformly scaled world transform; the minimum 0.05 mm brush is constrained to its world-space footprint and displacement cap. The latest full `libslic3r_tests` run passed 410 cases with 3 known expected failures (413 total) and 234,404 assertions (234,395 passed, 9 known expected failures).
- Cut and Boolean mesh replacement preserve source painting through face provenance or source-filtered restoration; MCUT carries original source face IDs to each target-derived output face, rejects ambiguous coincident-face lineage, preserves disconnected target components outside the cutter bounds, reports an actionable repair message, and returns an empty result when the cutter fully contains the target. New cut/cavity faces can receive an explicit filament. Plane-cut caps leave support, seam, and fuzzy-skin channels unset, and Boolean provenance transfers those channels only to surviving target faces. Regression tests cover repeated painted plane cuts, connector-bearing cuts, and scale-aware 0.025 / 0.1 / 0.25 / 0.5 mm removal depths on 10 mm and 1,000 mm targets using both coarse and eight-division-per-face cube meshes. They verify the requested cavity wall, retained adjacent exterior geometry, and surviving exterior filament labels; the 0.025 mm case leaves a 0.05 mm cutter dimension, within 0.0002 mm and 0.001 mm wall-position tolerances respectively.
- Boolean Difference now supports box, cylinder, sphere, and mesh cutters, plus translucent cutter and exact colour previews with Apply and Cancel. Primitive cutters, Boolean operands, and preview output now use consistent model-object coordinates; result centering is retained when previewed and committed. Primitive-cutter editing includes Shift-assisted fine numeric steps, optional position snapping, and a section-view depth slider backed by the shared object clipper; the clipper is released when section view is disabled, the gizmo exits, the selection clears, or the selected object changes. MCUT geometry runs on a worker; cancellation discards its result and revision checks reject stale results.
- Boolean preview temporarily hides the inputs and shows the result on loaded linked instances. Annotation remapping and facet partitioning run on the worker; OpenGL preview-surface construction remains on the UI thread.
- Smooth and Boolean tool panels state that applying an edit changes every linked instance of the source part.
- Committing a smoothing or Boolean mesh edit invalidates stored slice results on every plate, clears stale preview toolpaths, and schedules the current plate for processing. Boolean commits and subtract-everything route through the shared mesh-change handler.
- Plane offset entry and primitive cutter controls use fine millimetre increments; the plane input converts both increments and display precision when imperial units are selected. Primitive cutter numeric controls use 0.01 / 0.1 mm steps, Shift reduces these to 0.001 / 0.01 mm, and placement can optionally snap to a 0.05 mm grid (off by default).
- Focused regressions pass: Earlier focused results were `[MeshSmoothing]` (14 cases, 1,471 assertions) and a combined 53 cases / 2,575 assertions. The latest smoothing, Boolean, cut, 3MF, mixed-filament slicing, selector, and AABB run passed 55 cases / 1,983 assertions. After broadening the fine-detail depth matrix and adding the large-coordinate transformed-model case, the latest `[FineDetail]`, `[MeshBoolean]`, and `[Cut][MMUPaint]` runs passed 3 cases / 802 assertions, 15 / 998, and 11 / 217 respectively. The expanded coarse and dense Boolean depth matrix passed 128 assertions across 0.025 / 0.1 / 0.25 / 0.5 mm cuts and 10 mm / 1,000 mm targets. After removing per-vertex adjacency sorting, `[MeshSmoothing],[FineDetail]` passed 15 cases / 1,715 assertions; the latest full library suite completed with 413 cases (410 passed, 3 known expected failures) and 234,404 assertions (234,395 passed, 9 known expected failures). The new two-plate invalidation regression passed as part of `[PartPlate]` (2 cases / 25 assertions). An invalid Boolean operation now fails with an empty result while leaving both input meshes unchanged across all three public MCUT entry points (55 assertions). The GUI commit helper rejects empty results and stale source-volume pointers before taking an Undo snapshot. A combined smooth → vertical cut → subtract → save → reopen regression confirms exterior, cap, and cavity filament labels survive 3MF reload and each is used by an actual extrusion move in generated G-code (3 tools total). The vertical cap fixture ensures the cap has printable side-wall paths; sliced-region labels alone do not guarantee a tool appears in extrusion moves. Boolean face-paint lineage is also covered under a translated, rotated, mirrored, non-uniform target transform. The large parallel AABB build regression passes 2 cases / 20 assertions for ray and closest-point queries, and a new traversal check confirms all 12 box faces remain reachable through the tree. The full desktop `Snapmaker_Orca` target compiled and linked after disabling debug information in the generated local Release GUI project; this build-only setting is ignored and tracked build configuration is unchanged.
- A hidden `[.][MeshSmoothingBenchmark]` suite measures setup and dab time on synthetic 100k / 1M / 5M face grids. Compact sorted position keys replaced the node-based all-vertex hash and let ordinary meshes avoid two full-size duplicate-welding arrays. TBB parallel sorting is used above 250k position keys and 750k edge keys; AABB subtrees split above 1,024 leaves, with parallel input generation above 50k faces. Before the neighbor-order optimization, a local Release run on an AMD Ryzen 7 9800X3D (16 logical CPUs, 64 GB RAM) measured brush construction at 15 / 47 / 287 ms on 100k / 1M / 5M faces. After removing redundant per-vertex adjacency sorting, four baseline 5M-face runs measured 262 / 263 / 268 / 266 ms; dab time remained 5–7 ms. The user selected this Ryzen 7 9800X3D as the reference PC. Three current Release runs measured 12 / 13 / 12 ms setup for 100k faces, 50 / 58 / 51 ms for 1M, and 329 / 321 / 338 ms for 5M; 5M dab time was 5 / 5 / 6 ms. This confirms that 5M setup exceeds the proposed 50 ms target by a wide margin and 1M is around the threshold. These are constructor timings; actual first-dab feedback under asynchronous prewarm is still unverified. The smooth gizmo now reuses the scene raycaster when it references the editable mesh, prewarms brush topology and collision data asynchronously on tool activation and hover, and queues/remaps dabs until preparation finishes. This removes constructor work from the normal first click when prewarming completes; a fast click can still defer its first dab while preparation continues in the background. During that wait, the viewport ring turns amber and the panel reports preparation; setup failure turns the ring red and reports that no mesh change was made. Runtime responsiveness has not been verified. The fallback raycaster construction is still synchronous when the scene cache is missing or stale. Prior repeated runs sampled peak working set at about 1.31 GB on the 5M case. Profiling the 0.05 mm brush on the four-triangle coarse fixture showed 5,078 refined faces and a 2.55 s dab with the former target edge length. Using a four-radius target around the hit-centred subdivision reduced it to 494 faces and a 15 ms dab in the stroke-origin path (under 1 ms each for refinement and brush construction), while the focused geometry and colour regressions continued to pass. These are synthetic Windows measurements on the selected reference PC; real GUI responsiveness and the 50 ms target remain open.

- Runtime GUI interaction and Undo/redo, section-view interaction, linked-instance preview behavior and actual toolpath inspection in the desktop UI remain unverified. The direct all-plate invalidation method now has a two-plate regression, but its full GUI integration is not exercised. The desktop automation inventory reports no native applications, so these interaction checks cannot be run in this session. The full `slic3rutils_tests` run reported 8 non-expected failures, all in unrelated `SnapLogClient_test.cpp` batch lifecycle cases, and 3 expected failures; the focused `[PartPlate]` tests passed. The latest direct Release MSBuild of `Snapmaker_Orca.vcxproj` completed successfully and linked `build/src/Release/Snapmaker_Orca.dll`, including the new section-view control. Performance acceptance on the selected reference PC remains open because 5M setup exceeds the target and GUI response is unmeasured. Automated coverage now includes repeated plane cuts with connectors, the repeated smooth → cut → subtract → save → reopen workflow, transformed smoothing and Boolean paint mapping, a split paint island surviving a curved Boolean cut, scale-aware 0.025 / 0.1 / 0.25 / 0.5 mm removal depths, and a 0.05 mm brush on a 2,000 mm extent mesh under translation and non-uniform scaling; it does not yet cover every combination in the acceptance matrix.

## Intended result

Let a user select a printed part, smooth a bump, trim off a piece with a plane, or subtract a box, cylinder, sphere, or another mesh. Preserve filament painting on every surviving original surface. Show the resulting geometry and colours before committing a cut or subtraction, with Apply, Cancel, and Undo.

These operations have different purposes:

| Tool | User action | Geometry result | Colour rule |
| --- | --- | --- | --- |
| Smooth | Brush over a rough area | Move existing vertices to soften bumps | Keep original triangle IDs and paint subdivisions |
| Cut / trim | Position a plane and choose the side to retain | Remove a section and close the exposed opening | Transfer surviving exterior paint; assign a filament to the new cap |
| Subtract | Position a cutter shape inside the part | Remove the cutter's overlapping volume | Transfer target exterior paint; assign a filament to newly exposed cavity walls |

Smoothing is not the same operation as removing a feature. Offer cut/subtract for deliberate removal. Defer freehand erasing, automatic hole repair, general remeshing, and CAD history to later releases. Limited local subdivision needed for fine-detail smoothing is included below.

## Fine-detail repair requirements

Small tools are a first-release requirement: users must be able to remove tiny bumps, burrs, spikes, and patches of excess material without disturbing neighbouring geometry or colour. A small cursor alone does not satisfy this requirement; the geometry operation must work at the displayed size.

- Target a minimum smooth-brush diameter and primitive-cutter dimension of **0.05 mm**, with direct numeric entry and **0.01 mm** adjustment increments. Display brush diameter explicitly even if the algorithm stores radius. These are implementation targets to validate, not claims about current capability or printer resolution.
- Allow fine numeric control of cut-plane offset, cutter dimensions, position, and removal depth. Provide modifier-assisted fine adjustment, optional snapping, and a way to disable snapping. Do not round sub-millimetre values to zero or impose a minimum tied to nozzle size.
- Keep dimensions in world millimetres at every zoom level and under object scaling. Support close zoom, a visible tool footprint, and a local inspection/clipping view so a user can position a tiny tool accurately on a large part. Screen-space handles may stay large enough to grab without enlarging the actual cutter.
- For local excess material, use a small bounded box, sphere, cylinder, or mesh cutter. A plane cut affects the selected part across the plane; a small-looking plane handle must not imply a local-only cut. Show the complete affected region before Apply.
- Restrict smoothing to the connected surface under the brush and enforce a displacement limit in millimetres. Show when mesh resolution prevents an effective small edit; never silently enlarge the brush or move vertices outside its footprint to force a result.
- If the brush is smaller than the local triangles, support bounded local subdivision with source-face/barycentric lineage before smoothing. Preserve split paint and other annotations through that lineage, keep refinement conforming at its boundary, and restrict subsequent movement to the requested footprint. This is a separate topology-changing path; the normal smoothing path retains exact connectivity and annotation streams.
- Bound geometric tolerances and backend perturbations relative to the requested feature size as well as model scale. A repair must not disappear because a global epsilon, cleanup threshold, or colour-transfer threshold exceeds its dimensions. Reject numerically unreliable edits with an explanation and retain the original model.

Acceptance must include **0.1 mm, 0.25 mm, and 0.5 mm excess-material features**, plus a **0.05 mm tool-size boundary case**, on both dense and coarse meshes and on small and large models. Check actual affected extent, requested removal depth, nearby wall/detail retention, and surviving filament labels. Establish measured geometry and colour tolerances before implementation is considered complete.

## What already exists in this checkout

The initial scope is to extend and validate existing functionality, rather than build all three tools from scratch. Source inspection establishes the following; tests were located but not executed for this planning task.

| Existing component | Evidence | Implication |
| --- | --- | --- |
| Smooth brush and mesh algorithm | `src/slic3r/GUI/Gizmos/GLGizmoSmooth.cpp`; `src/libslic3r/MeshSmoothing.{hpp,cpp}` | Already moves vertices without changing connectivity and takes an Undo snapshot per changed stroke. It pins open/non-manifold edges and attempts to reject local triangle flips. |
| Facet painting storage | `src/libslic3r/Model.hpp`, `FacetsAnnotation` and `ModelVolume` | Multi-material colours include subdivided facet annotations, not just one colour per base triangle. Volume material/configuration also matters. |
| Painting capture and spatial restore | `src/libslic3r/Model.cpp`, `ModelVolume::save_painting()` and `restore_painting()`; `TriangleSelector::SavedPainting` | Reuse these helpers for topology changes. They also capture support, seam, and fuzzy-skin annotations. |
| Cut preservation | `src/libslic3r/CutUtils.cpp`; `GLGizmoCut.cpp`; `GLGizmoAdvancedCut.cpp` | Existing cut paths request `KeepPaint` and restore painting. Extend coverage and expose a new-face filament policy. |
| Boolean operations | `src/slic3r/GUI/Gizmos/GLGizmoMeshBoolean.cpp`, `generate_new_volume()` | Copies source configuration/material, but facet assignment lines are commented out and it does not call painting restore. This is the clearest preservation gap. Copying raw facet data would be incorrect after retriangulation. |
| Boolean backend | `src/libslic3r/MeshBoolean.{hpp,cpp}` | Existing MCUT path produces result meshes. The inspected wrapper does not expose per-output-face origin information. Investigate that capability before choosing the implementation. |
| Regression coverage | `tests/libslic3r/test_mesh_smoothing.cpp`; `test_triangle_selector.cpp` | Existing cases cover smoothing with split painting, transformed cuts, repeated cuts, and avoiding paint leakage onto caps. Extend these fixtures. |

## Colour contract

1. Surviving original surfaces retain their filament assignments, including partially painted triangles and unpainted areas that inherit a base filament.
2. Smoothing carries paint with the deforming surface. Exact annotation retention does not mean preserving the original world-space shape of a painted mark.
3. Newly generated caps and cavity walls use the target part's base filament by default. Offer an explicit filament selector. These surfaces have no previous colour to recover.
4. A subtraction cutter contributes shape only. Its paint must not become the target's cavity colour.
5. Preserve part configuration, material identity, filament mapping, and transforms. Start with one target volume to avoid silently merging independently configured coloured parts.
6. Preserve support, seam, and fuzzy-skin annotations only on their surviving source surfaces; new faces start without those annotations.
7. Failures and Cancel leave the complete original model intact. Preview, committed geometry, saved 3MF, and sliced output must agree.

Scope for the first release is filament-based multi-material painting and part colours. Imported UV textures, vertex colours, or other colour encodings need a separate import/storage audit before claiming support. Save editable colour projects as 3MF; STL cannot serve as the colour-preserving project format.

## Implementation sequence

### 1. Establish the baseline and shared edit transaction

- Build the current checkout and run the existing smoothing and painted-cut tests first. Record any existing failures separately.
- Audit mesh replacement paths used by the three tools, including transforms, mesh initialization shifts, linked instances, selection, and Undo.
- Define an internal edit result containing the candidate mesh, output-face origin classification, restored annotations, and validation diagnostics. Keep it temporary until Apply.
- Capture source mesh, annotations, configuration, and transforms before computation. Use a common coordinate frame throughout geometry calculation and paint transfer.
- Commit geometry and annotations together with one Undo action. Invalidate geometry, picking, painting, bounds, and slicing caches through the existing update mechanisms. Recompute normals and derived geometry data.
- Initial instance policy: editing a shared volume affects its linked instances; make this clear in the tool. Defer a “make unique” workflow unless an existing mechanism can be reused safely.

Deliverable: consistent Apply/Cancel/Undo behaviour and a documented colour contract.

### 2. Harden the existing smooth brush

- Retain vertex and triangle ordering and every existing annotation bitstream. Avoid remapping when connectivity is unchanged.
- Keep radius in millimetres and strength controls. Add a per-stroke displacement limit and an optional sharp-edge preservation control.
- Implement the fine-detail diameter controls and coarse-mesh local-subdivision path above. Track source correspondence through refinement so sub-triangle repairs preserve painted boundaries.
- Assess shrinkage on repeated strokes; evaluate a less-shrinking relaxation method if the current averaging changes dimensions excessively.
- Verify that coincident seam vertices move together without joining independent touching shells. The current position-based representative grouping needs a regression fixture for that case.
- Prevent edits from collapsing thin walls or causing self-intersection. Local flip rejection alone is not sufficient; evaluate candidate displacement against nearby unrelated triangles and reduce or reject unsafe motion.
- Keep one Undo action per stroke and restore the exact previous mesh and annotations. Ensure coarse meshes show useful feedback when brush radius contains no movable vertices.
- Profile current full vertex-buffer copies during brush passes before optimizing; cache local adjacency and spatial queries where measurements justify it.

Deliverable: predictable local smoothing, including tiny defects; unchanged topology and paint data in the standard path, and verified colour-preserving correspondence in the refinement path.

### 3. Complete colour-preserving plane cuts

- Reuse the current Cut tools with Keep upper / Keep lower / Keep both controls and an unmistakable retained-side preview.
- Support fine plane offsets for shaving off a small amount of excess material; show other geometry the plane would remove and direct local-only repairs to a bounded cutter.
- Verify that all supported cut paths retain `KeepPaint`, including repeated cuts and cuts with connectors.
- Classify generated cap faces explicitly where possible. Restore paint only onto surviving exterior faces, using the existing mapping helpers.
- Keep the cap unpainted to inherit the base filament by default; when a filament is chosen, assign it only to cap facets. Leave other annotation channels unset there.
- Check source-volume ownership during multi-part cuts so nearby or coincident parts cannot donate each other's colours.

Deliverable: trim a painted feature without recolouring the retained exterior.

### 4. Fix Boolean subtraction, then add simple cutters

- Extend `GLGizmoMeshBoolean::generate_new_volume()` to capture the target's painting before deleting/replacing source volumes, and restore annotations to the validated result.
- Do not implement preservation by uncommenting the facet-copy lines: output triangle numbering differs from input numbering.
- First investigate whether the bundled backend can provide reliable output-face provenance. If available, extend the application wrapper to return source volume/face identity and generated-face classification without editing vendored dependencies.
- For faces derived from the target, transfer subdivided paint by correspondence within the source face. Faces derived from the cutter become newly exposed cavity walls and receive the new-face filament policy.
- Reuse spatial restoration as an initial implementation only where it passes the preservation fixtures. Guard it by source ownership, distance, and surface compatibility; never globally project nearest colour onto cavity walls. If provenance is unavailable and a case is ambiguous, reject the operation with an actionable message instead of silently losing paint.
- Handle small backend position perturbations with a scale-aware tolerance and tests. Preserve sharp colour boundaries rather than choosing a majority colour for an entire output triangle.
- Validate empty, disconnected, and failed results before replacing the original. Make “subtract everything” an explicit empty-result case, and retain the cutter by default for adjustment.
- Add temporary box, cylinder, and sphere cutters using existing primitive creation/transformation facilities where available. Provide dimensions, position, rotation, and a translucent preview. These must perform a real mesh difference, not only change the slice preview.
- Apply the fine-detail minimum dimensions, positioning increments, depth controls, and feature-relative tolerances above. Validate shallow and tiny intersections rather than treating them as no-ops.

Deliverable: subtract a simple shape or another mesh while retaining the target's exterior paint.

### 5. Finish interaction, persistence, and performance

- Group the existing tools under a clear mesh-editing entry point if that improves discoverability; retain familiar controls and shortcuts.
- For cut/subtraction, provide a quick cutter preview followed by an exact geometry-and-colour preview. Apply only the validated exact result.
- Run expensive geometry and paint transfer off the GUI thread with cancellation. Use a source revision token so an outdated result cannot replace a subsequently edited part. Audit whether existing backend calls can be interrupted; otherwise discard their results after cancellation.
- Record the operation's effect on all linked instances and invalidate every affected plate's slice result.
- Verify 3MF save/reopen and actual filament toolpaths, including filament remapping after an edit. Keep project serialization compatible if annotations fit the current format.

## Verification and acceptance

| Fixture / workflow | Required outcome |
| --- | --- |
| Painted bump with split paint inside one triangle | Smoothing reduces roughness; connectivity and annotation data remain identical; Undo restores vertices exactly |
| Sub-millimetre bumps and excess material on dense and coarse meshes | Small brushes and cutters repair 0.1 / 0.25 / 0.5 mm defects within the requested footprint; coarse-mesh refinement retains source paint boundaries |
| 0.05 mm brush/cutter on a large, transformed model | Numeric size and position remain accurate; the edit is effective on suitable geometry or explicitly rejected without changing the model |
| Tiny defect beside a colour boundary, thin wall, or fine ridge | Only the requested region is changed; adjacent geometry and surviving filament labels are retained |
| Thin wall, nearby disconnected shells, touching shells, open edges | Brush does not unintentionally affect another shell or create invalid geometry |
| Two-colour object cut across a painted boundary | Surviving painted regions match the original; cap gets only its selected/default filament |
| Sphere and box subtraction through several colours | Exterior paint survives; cavity walls use the configured filament; cutter paint never leaks |
| Tiny colour islands and output triangles crossing paint boundaries | Preserve the split boundary within a documented tolerance; no majority-colour shortcut |
| Rotated, translated, mirrored, and non-uniformly scaled parts | Geometry, colour mapping, and brush radius agree in the viewport and saved model |
| Repeated smooth → cut → subtract → save → reopen | No cumulative loss of surviving paint; annotations remain editable |
| Undo / redo / Cancel / backend failure | Restore geometry, configuration, annotations, and selection; leave no partial replacement |
| Multiple volumes, linked instances, multiple plates | No cross-volume paint transfer; documented shared-instance behaviour; affected slices invalidated |
| Slice and inspect filament toolpaths | Surviving regions and new faces use the intended filament IDs |

Use deterministic geometry fixtures in `tests/libslic3r/` and representative painted 3MF files under `tests/data/`. Compare sampled filament labels and boundary geometry on surviving surfaces, not just viewport screenshots or triangle counts. Standard smoothing additionally requires byte-identical annotation streams; refinement requires equivalent filament labels through recorded source correspondence. Establish explicit tolerances for cut-boundary mapping during the baseline phase; do not promise exact geometric correspondence from a spatial fallback.

Measure brush latency, exact-preview time, and peak memory on approximately 100k, 1M, and 5M triangle models. Proposed interaction target: brush feedback within 50 ms on the agreed reference Windows machine. Treat this as a target to validate, not a current capability; exact Boolean work can use a progress indicator.

## Recommended delivery order

1. Baseline, edit transaction, and smoothing validation.
2. Plane trimming with guaranteed exterior-paint retention and explicit cap colour.
3. Boolean subtraction preservation with mesh cutters.
4. Primitive cutter workflow and performance improvements.

Release each stage only after its geometry, painting, Undo, 3MF, and slicing checks pass. The main implementation uncertainty is reliable paint transfer through Boolean retriangulation, especially at tiny painted features and nearly coincident surfaces. Investigate face provenance before committing to a delivery estimate.

After implementation, distinguish source commit, Windows installer build, and installed application update. Follow the repository's installer-update instruction if a later task includes committing these changes.
