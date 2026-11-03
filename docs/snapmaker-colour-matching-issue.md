# Print Preprocessing auto-maps a dark greenish colour to black despite a coloured alternative

## Observed behaviour
In Snapmaker Orca's Print Preprocessing filament mapping screen, automatic mapping selected the printer's black filament for a greenish model colour, despite a non-black filament being available. The user reports that dark colours appear particularly prone to mapping to black.

This is a user-observed case, not an independently reproduced test. Exact source/slot RGB values and the Snapmaker Control version have not yet been captured.

## Steps to investigate
1. Load a multicolour model containing a dark greenish filament colour.
2. Configure printer slots with black and a compatible coloured alternative.
3. Open Print Preprocessing and inspect automatic filament mapping.
4. Compare the chosen slot against the model hue and available slot colours.

## Expected behaviour
When a visibly chromatic model colour has an available, material-compatible filament of a reasonably similar hue, prefer that filament over neutral black solely favoured by its darkness. Preserve sensible matching for true black, charcoal and grey.

## Suggested investigation
Please check whether lightness dominates the matching score or whether dark chromatic colours are classified as neutral. The actual matching implementation has not been verified. Consider a hue/chroma-aware preference before perceptual-distance ranking, with a near-black noise floor. Preserve exact matches, material compatibility, slot availability and manual overrides. Do not force a match to an unrelated hue merely because it is non-black.

Useful regression cases: dark green/blue/red versus black and a lighter same-hue filament; true black and grey; nearly black RGB noise; exact matches; and palettes without any compatible similar-hue filament.

## Environment and integration
- Windows; Snapmaker Orca checkout. Exact running app and Control versions: not yet recorded.
- The native WebPreprintDialog loads the connection gateway's pre_paint_page.
- Snapmaker Control's public repository distributes binaries without source, so this report requests a change to the upstream screen.

## Current workaround
Manually select the desired coloured printer slot before printing.
