# Painting performance validation

## Changes

- Color, support, seam, and fuzzy-skin painting retain render buffers for unchanged mesh regions. Original triangles are grouped in chunks of 16,384; each chunk contains the current leaves of those original triangles, including subdivisions. A local brush or fill edit rebuilds and uploads only touched chunks. Empty color buffers are released, and chunks of the same color share a shader color update.
- Full updates remain the default for loading, undo, resets, palette size changes, remapping, and other bulk operations. Gap fill keeps its connected-patch rendering path. Wireframe changes invalidate each selector's geometry independently.
- Brush traversal reuses its visited bitmap and queue, enqueues each original triangle once, and computes the overhang normal transform once per operation. Normal transforms are skipped when overhang filtering is disabled.
- Fill selection tracks original roots. Applying, clearing, and outlining a small selection visits those roots and their current leaves. Roots remain stable through subdivision and garbage collection. Unsubdivided bucket fills reuse the existing adjacency table.
- Selector storage reserves subdivision headroom to avoid copying the entire triangle/vertex arrays on the first subdividing stroke.

## Automated validation

Validated on Windows x64 in Release configuration:

- `libslic3r_tests` and `libslic3r_gui` compiled successfully from the final sources.
- The application linked successfully to `build/painting_release/Snapmaker_Orca.dll`. The standard output DLL was locked by the running application, so the validation build uses a separate directory with the launcher, runtime DLLs, and a resources junction.
- The `[TriangleSelector],[MMUPaint]` run passed **74,151 assertions in 27 cases**, including the benchmark case. `git diff --check` passed.
- Launcher `--help` exited with code 0. Crash-reporting initialization logged a nonfatal Sentry failure in this execution environment; the Windows help implementation redirects its text to `CONOUT$`. This smoke check does not exercise GPU painting.

Logs: `build/painting_verified_build.log`, `build/painting_link.log`, `build/painting_tests.log`, `build/painting_benchmark.log`, and `build/painting_app_help.log`.

Five separate benchmark runs passed. Median CPU timings on the 980,000-triangle grid:

| Operation | Median |
| --- | ---: |
| First subdividing stroke with former exact-sized storage | 5.07 ms |
| First subdividing stroke with reserved storage | 0.19 ms |
| Local brush selection, average over 200 strokes | 0.83 microseconds |
| Single-triangle fill/apply/contour/clear, average over 200 operations | 0.20 microseconds |

The first-stroke allocation comparison is approximately **26.6 times faster** in this synthetic CPU workload. It measures the same selection algorithm with and without reserved subdivision capacity, rather than comparing entire application versions. The other timings are current CPU operation costs. None of these numbers are end-to-end mouse-to-frame measurements. Raw samples are in `build/painting_benchmark_results.json`.

The regression tests in `tests/libslic3r/test_painting_updates.cpp` cover:

- Sparse strokes on a 73,728-triangle mesh, verifying that all changed geometry is covered by original-facet notifications.
- Subdivision, erasing, high filament IDs, repeat visits, replace filters, and garbage collection.
- Fill contour selection/clearing/reset, a subdivided single-triangle fill, empty-selection updates, and clearing a selection before its subdivided parent merges into a leaf.
- Overhang filtering on rotated, nonuniformly scaled models.

The explicit `[PaintingBenchmark]` test uses a 980,000-triangle grid. It compares the first subdividing stroke with the former exact-sized storage and the new reserved storage, then times repeated local brush and single-triangle fill operations. These are CPU selection timings; they exclude raycasting, OpenGL upload, and frame presentation. Timing thresholds are deliberately not part of pass/fail checks.

Run from the repository root:

```powershell
cmake --build build --target libslic3r_tests Snapmaker_Orca --config Release --parallel 4 -- /nr:false
& .\build\tests\libslic3r\Release\libslic3r_tests.exe '[TriangleSelector],[MMUPaint]' --reporter compact
& .\build\tests\libslic3r\Release\libslic3r_tests.exe '[PaintingBenchmark]' --reporter compact
```

## Manual checks

Use the same dense model and camera position before and after the change. Test small and large circular/spherical brushes, continuous dragging, pointer, smart/bucket/magic fill, erasing, height ranges, replace-only painting, and overhang-only painting. Exercise color, support, seam, and fuzzy-skin modes.

Toggle wireframe with multiple painted volumes; enter and leave gap fill; change the palette size; undo/redo; reload a saved 3MF; and slice the result. Check that old colors disappear completely when a chunk is recolored, contours remain correct, and unchanged regions retain their paint.

For end-to-end latency, measure mouse-event processing, `TriangleSelectorPatch::update_render_data`, GPU upload, and frame presentation separately. Also compare idle orbit/frame time on heavily multicolored meshes, since chunking trades extra draw calls for smaller edit uploads. Initial loading and operations touching the whole model still do proportional work. Triangle ordering affects how many chunks a spatial stroke touches.

Manual GPU/visual checks and actual-model click-to-frame measurements have not been performed in this session.
