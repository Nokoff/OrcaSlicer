# Local Print Preprocessing colour fix

Implemented 2026-11-02. Upstream report: https://github.com/Snapmaker/snapmaker_control/issues/2

## What changed

The compiled Control web app already uses CIEDE2000, including correct sRGB-to-Lab conversion. Its automatic selector can nevertheless choose a neutral filament for a dark chromatic target. The local patch changes only the selector after upstream nozzle/material filtering; it preserves the original distance implementation and returns the original slot objects.

If the ordinary winner is neutral (Lab chroma below 8), a source with chroma at least 10 and lightness at least 5 may instead choose a chromatic candidate within 30 degrees of its Lab hue. Eligible alternatives must have CIEDE2000 distance at most 45 and be no more than 25 worse than the original winner. The closest eligible alternative wins, with upstream slot-index tie breaking. These thresholds are conservative product-policy choices, not CIE standards.

Exact matches, neutral sources, multicolour strings, invalid colours, already-chromatic winners and palettes without a suitable alternative retain the upstream result. Manual mapping and the compatibility filters are unchanged. The actual user's colour values have not yet been captured, so the fix is validated with representative regressions, not that specific model.

## Applied copies

- Repository: `C:\code\OrcaSlicer\resources\web\flutter_web`
- Local runnable build: `C:\code\OrcaSlicer\build-native\Snapmaker_Orca\resources\web\flutter_web`
- `build-native\src\Release\resources` resolves to the repository assets and therefore also uses the patch.

No C++ rebuild is required. Close and reopen Print Preprocessing (restart Orca if it retains the previous page). Use the local runnable build's `snapmaker-orca.exe` to try it. Installed Program Files copies are separate and were not patched.

## Reapply after staging / restore

From the repository root:

```powershell
python scripts/packaging/patch_connect_colour_match.py resources/web/flutter_web
python scripts/packaging/patch_connect_colour_match.py build-native/Snapmaker_Orca/resources/web/flutter_web
# Roll back either folder with:
python scripts/packaging/patch_connect_colour_match.py resources/web/flutter_web --restore
```

The staging script accepts `--colour-match-fix` to apply the fix to extracted assets after archive verification. Normal upstream staging without that flag replaces the local patch. The patcher validates the complete original bundle SHA-256 and supports only the two bundles inspected in this checkout. Future Control versions need a new review; unknown versions are rejected before writing. Original JavaScript/bootstrap files remain intact. New filenames contain the modified content hash, preventing stale immutable-cache entries from hiding the fix. An index backup and hash manifest support rollback.

## Validation

- Regression tests execute the shipped CIEDE2000 implementation and the patched selector, rather than substituting an approximate distance function.
- Dark green, red and blue examples reproduce the original black winner and select the same-hue alternative after the fix.
- Black, charcoal, near-black noise, exact matches, unsuitable palettes, empty lists, alpha suffixes, multicolour input and tie-breaking are covered.
- Python tests cover both supported bundles, unchanged surrounding code, unknown-version rejection without writes, cache URL changes, idempotence, tamper detection and rollback/reapplication.
- Full patched JavaScript passes `node --check`.

```powershell
node tests/packaging/filament_colour_match.test.cjs
python -m unittest discover -s tests/packaging -p 'test_*connect*.py'
```

No live printer job was submitted. Full visual testing of Print Preprocessing with the user's printer and model remains outstanding.

## Consolidation update
The installed Control 1.8.5 bundle is now supported separately by its full SHA-256. Its newer selector retains diameter/material/flow filtering and manual selections. The Windows installer always applies this reviewed patch after staging the pinned archive. See `docs/consolidated-local-build.md` for the installed launcher and full feature audit; generic launchers in development directories can be older than the AVX-512 build.
