// Local Print Preprocessing policy. Candidates have already passed the upstream
// material/nozzle filters. Return an index; never modify colours or slot objects.
function selectFilamentColour(source, candidates) {
    function lab(hex) {
        if (typeof hex !== "string") return null;
        hex = hex.trim().replace(/^#/, "");
        if (!/^[0-9a-f]{6}([0-9a-f]{2})?$/i.test(hex)) return null;
        var rgb = [0, 2, 4].map(function (offset) {
            var c = parseInt(hex.slice(offset, offset + 2), 16) / 255;
            return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
        });
        function f(t) {
            return t > 216 / 24389 ? Math.pow(t, 1 / 3) : (24389 / 27 * t + 16) / 116;
        }
        var x = f((0.4124564 * rgb[0] + 0.3575761 * rgb[1] + 0.1804375 * rgb[2]) / 0.95047);
        var y = f(0.2126729 * rgb[0] + 0.7151522 * rgb[1] + 0.072175 * rgb[2]);
        var z = f((0.0193339 * rgb[0] + 0.119192 * rgb[1] + 0.9503041 * rgb[2]) / 1.08883);
        var a = 500 * (x - y), b = 200 * (y - z);
        return { lightness: 116 * y - 16, chroma: Math.sqrt(a * a + b * b), hue: Math.atan2(b, a) };
    }
    function better(index, best) {
        return best < 0 || candidates[index].distance < candidates[best].distance ||
            (candidates[index].distance === candidates[best].distance && candidates[index].id < candidates[best].id);
    }
    var best = -1;
    for (var i = 0; i < candidates.length; ++i) {
        if (better(i, best)) best = i;
    }
    if (best < 0 || candidates[best].distance === 0) return best;

    // Leave multicolour, unknown, truly neutral and barely visible colours to
    // the upstream matcher. Chroma alone is unreliable very close to black.
    var target = lab(source), winner = lab(candidates[best].colour);
    if (!target || !winner || target.lightness < 5 || target.chroma < 10 || winner.chroma >= 8) return best;

    var preferred = -1;
    for (var j = 0; j < candidates.length; ++j) {
        var candidate = lab(candidates[j].colour);
        if (!candidate || candidate.lightness < 5 || candidate.chroma < 10) continue;
        var hueDifference = Math.abs(candidate.hue - target.hue);
        hueDifference = Math.min(hueDifference, 2 * Math.PI - hueDifference);
        // Product-policy bounds, not CIE standards: only nearby hues, and no
        // extreme replacement when the available coloured filament is poor.
        if (hueDifference > Math.PI / 6 || candidates[j].distance > 45 ||
            candidates[j].distance > candidates[best].distance + 25) continue;
        if (better(j, preferred)) preferred = j;
    }
    return preferred < 0 ? best : preferred;
}
