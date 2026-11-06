# Consolidated local build — 2 November 2026

## Which app to use

The Windows Start Menu entry **Snapmaker Orca (AVX-512)** targets
`C:\Program Files\Snapmaker_Orca\snapmaker-orca-avx512.exe`.
This is the installed fork to update. `C:\Program Files\OrcaSlicer` is a separate
application; `Snapmaker_Orca_Official` was an incomplete leftover directory.
Neither was removed or repurposed.

Development folders contain older and newer launchers side by side. In particular,
the generic `snapmaker-orca.exe` and `Snapmaker_Orca.dll` in build-native predate the
AVX-512 pair. They are not a reliable way to launch the latest build. Use the
installed Start Menu entry after updating. The consolidated installer selects the
launcher matching the configured CPU architecture and records its source revision
and application hash in `local-build-info.json` in the installation folder.

## Findings and fixes

- The installed AVX-512 application originally matched the 1 November AVX-512
  binary byte for byte. The missing library was a navigation regression, not
  absent library files.
- `WebViewPanel::update_mode()` hid sizer item zero for non-developer users. Public
  builds omit the developer toolbar, leaving Home/My Files as item zero. The fix
  addresses the actual developer toolbar only in builds that contain it.
- Installed Control uses the pinned 1.8.5 bundle, whose matcher differs from the
  older development bundle. Both variants now have explicit SHA-256-guarded colour
  patches. The pinned variant retains material, diameter and flow filters and the
  existing manual-choice path.
- Spoolman commit `2106f220e7` was initially present only on a separate branch. It
  was fast-forward merged before the consolidated rebuild.

## Feature inventory

This inventory checks source history, build registration and regression coverage.
It is not a claim that every interactive workflow has been manually exercised.

| Feature | Source/build evidence |
| --- | --- |
| My Files folder browser and thumbnails | `GUI/MyFilesLibrary.cpp`, `GUI/WebViewDialog.cpp`, packaged `resources/web/myfiles` |
| Spoolman spool/colour picker and server preference | `GUI/SpoolmanSpoolDialog.cpp`, `Utils/Spoolman.cpp`, `Preferences.cpp`, `PresetComboBoxes.cpp`; registered in slic3r CMake |
| Auto painting, magic fill, remapping, despeckle and edge smoothing | Painting commits merged on main; `GLGizmoMmSegmentation.cpp`, auto-paint native regressions |
| Painting preservation during cutting and simplification | Existing merged cutting/simplification changes and regression suite |
| Mesh smoothing, surface wrap and embossing | Registered in `GLGizmosManager.cpp` and slic3r CMake |
| Filament merge planning and undo fixes | `filamentsync/FilamentMergePlan.cpp`, merge tests and merged undo commits |
| Collaborative painting | `GUI/Collab` compiled into the GUI; gizmo locking hooks retained |
| GLB/glTF import and colour mapping | Merged import feature history and current format loaders |
| Printer reconnect, MQTT recovery and device file management | Merged connection/device changes; gateway and MQTT test targets |
| Upstream merge and profile corrections | Main includes `8e4d7dbee9` and `bf69910814` |
| Dark-colour matching preference | Version-guarded local Control patch, installed by the packaging script |

## Validation notes

The initial native CTest pass ran 416 cases successfully but also exposed seven
unbuilt test executables and two MQTT harness failures caused by a missing probe
executable. These are not counted as passing tests. The missing targets must be
built before the final verification run. No live printer job is submitted during
this audit.

Packaging and rollback details for the colour change are in
`docs/local-colour-matching-fix.md`. The installer uses a clean payload, verifies
the pinned connection archive, applies the reviewed patch, and checks that My Files
assets and the architecture-specific launcher/DLL are present.

## Completed validation and installation

Verified 6 November 2026: the consolidated installer completed with exit code 0. The installed launcher, application DLL, My Files page, active Flutter entrypoint and build metadata match the tested payload byte for byte. The Start Menu entry Snapmaker Orca (AVX-512) targets the updated installed launcher.

All 660 native regression cases passed across the full run (659 cases) and the corrected packaged-resource check rerun (1 case). The resource check accepts ORCA_TEST_FLUTTER_BUNDLE to validate the actual content-hashed shipping bundle. Eight Python packaging tests, colour-matching regression tests, installer archive integrity, checksum verification and the packaged launcher --help smoke test also passed.

Installed application source revision: 3b55d2cd6673790c8d675519eb41d28e5b521cad, including Spoolman commit 2106f220e7. The subsequent validation-only commit changes a test and this report; application binaries are identical. No live printer job or full interactive UI walkthrough was performed. Open Home, then My Files, in the installed app.
