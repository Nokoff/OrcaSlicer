# Print Preprocessing colour matching

## Verified integration
- src/slic3r/GUI/WebPreprintDialog.cpp: the Print Preprocessing dialog loads gateway_web_url("pre_paint_page").
- src/slic3r/GUI/GUI_App.cpp: gateway_web_url resolves the connection service's web URL.
- src/slic3r/GUI/SSWCP.cpp: build_filament_mapping_json supplies filament colours; it does not choose the closest printer colour.
- resources/web/flutter_web contains compiled Flutter bundles. No Dart source was found in this checkout.
- src/slic3r/GUI/filamentsync/FilamentSyncAlgorithm.cpp implements CIEDE2000 matching for the separate filament sync dialog. Changing it alone would not establish a fix for Print Preprocessing.

## Proposed matching policy
1. Filter candidates by material compatibility and availability before comparing colours.
2. Use a perceptual colour space, with correct sRGB conversion. Confirm the existing screen's metric in its source before replacing it.
3. Identify visibly chromatic source colours using chroma with a near-black noise floor. Darkness alone must not classify a colour as neutral.
4. If a chromatic source has a compatible chromatic candidate with a reasonably similar hue, prefer that group over neutral black/grey candidates. Rank within that group using perceptual distance, with reduced lightness influence if needed.
5. Otherwise retain ordinary perceptual matching. Do not force green to red merely because red is non-black. Preserve exact matches, true neutrals, material restrictions, and manual mappings.
6. Expose a low-confidence indication when no suitable colour exists.

## Regression cases
- Dark green with black and lighter green available: choose green.
- Dark blue/red with black and same-hue filament available: choose same hue.
- Exact colour including exact black: preserve exact match.
- Black, charcoal, grey, and almost-black noisy RGB values: preserve neutral matching.
- Green with only black and unrelated bright colours: do not force unrelated hue.
- Same-hue filament of incompatible material: preserve material restrictions.
- Empty/unavailable slots and manual overrides: preserve existing behaviour.

## Required to implement and validate
The connection app's Flutter/Dart matching source, plus the failing model colour and loaded printer colour values. Chroma/hue thresholds require calibration against these examples and boundary cases. No production code was changed; the deployed screen's distance formula has not been verified.

## Upstream source availability (verified 2026-11-02)
The staging script scripts/packaging/stage_connect_bundle.py obtains releases from Snapmaker/snapmaker_control. Its official README explicitly identifies the repository as a binary distribution channel without source code, and identifies Snapmaker Control as proprietary software. Source: https://github.com/Snapmaker/snapmaker_control#readme

The user is not expected to have a Dart checkout. A direct upstream screen fix requires Snapmaker to change the matcher. A local alternative would require investigating a native pre-print mapping UI and the supported mapping handoff; its feasibility has not yet been established. Manual mapping in the current screen remains the immediate workaround.
