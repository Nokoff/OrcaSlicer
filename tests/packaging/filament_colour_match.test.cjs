const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../..');
const policy = fs.readFileSync(path.join(root, 'scripts/packaging/filament_colour_match.js'), 'utf8');
const select = vm.runInNewContext(policy + '\nselectFilamentColour');

// Execute the actual shipped CIEDE2000 implementation, with only Dart's list
// wrapper stubbed. This catches regressions against the matcher being patched.
const bundle = fs.readFileSync(path.join(root, 'resources/web/flutter_web/main.b347974c91352c2f.js'), 'utf8');
function method(start, end) {
    const offset = bundle.indexOf(start);
    assert.ok(offset >= 0);
    const finish = bundle.indexOf(end, offset);
    assert.ok(finish > offset);
    return bundle.slice(offset, finish + end.length - 1);
}
const A = { c: a => a };
const context = { A, t: { n: null } };
A.bPR = vm.runInNewContext('({' + method('bPR(a,b,c){', '],t.n)},') + '}).bPR', context);
A.bPS = vm.runInNewContext('({' + method('bPS(a,b,c){', '],t.n)},') + '}).bPS', context);
const distance = vm.runInNewContext('({' + method('aIm(b7,b8){', 'return s},') + '}).aIm', context);
const receiver = {
    afN: hex => {
        if (!hex) return [];
        return hex.split(';').map(value => {
            const rgb = value.replace('#', '').slice(0, 6);
            return { gvB: () => parseInt(rgb.slice(0, 2), 16), gtB: () => parseInt(rgb.slice(2, 4), 16),
                guF: () => parseInt(rgb.slice(4, 6), 16) };
        });
    }
};
const delta = (a, b) => distance.call(receiver, a, b);
const options = (source, colours) => colours.map((colour, id) => ({ colour, id, distance: delta(source, colour) }));
function check(name, source, colours, expected, baseline) {
    const choices = options(source, colours);
    if (baseline !== undefined) {
        const old = choices.reduce((best, c, i) => c.distance < choices[best].distance ? i : best, 0);
        assert.equal(old, baseline, name + ': reproduce original outcome');
    }
    assert.equal(select(source, choices), expected, name);
    console.log('PASS ' + name);
}
check('dark green prefers green over black', '#003000', ['#000000', '#409040'], 1, 0);
check('dark red prefers red over black', '#500808', ['#000000', '#C04040'], 1, 0);
check('dark blue prefers blue over black', '#080840', ['#000000', '#8080FF'], 1, 0);
check('exact black remains black', '#000000', ['#000000', '#409040'], 0);
check('charcoal remains neutral', '#202020', ['#000000', '#409040'], 0);
check('near-black RGB noise remains neutral', '#020301', ['#000000', '#409040'], 0);
check('unrelated hue does not replace black', '#003000', ['#000000', '#E03030'], 0);
check('exact green remains exact', '#003000', ['#000000', '#003000', '#409040'], 1);
check('no coloured candidate preserves black', '#003000', ['#000000'], 0);
check('already coloured winner is preserved', '#408040', ['#407040', '#80A080'], 0);
check('alpha suffix follows upstream RGB semantics', '#003000FF', ['#000000FF', '#409040FF'], 1);
assert.equal(select('#003000', []), -1);
assert.equal(select(null, [{ colour: '#000000', id: 0, distance: Infinity }]), 0);
assert.equal(select('#003000;#FFFFFF', options('#003000;#FFFFFF', ['#000000', '#409040'])), 0);
assert.equal(select('#003000', [{ colour: '#000000', id: 3, distance: 1 },
    { colour: '#000000', id: 1, distance: 1 }]), 1);
assert.equal(select('#003000', [{ colour: '#000000', id: 0, distance: 1 },
    { colour: '#409040', id: 1, distance: 46 }]), 0);
assert.equal(select('#003000', [{ colour: '#000000', id: 0, distance: 1 },
    { colour: '#409040', id: 1, distance: 30 }]), 0);
console.log('PASS empty, unknown, multicolour, tie and poor-alternative guards');

// Also run the exact rewritten selector in the patched bundle when supplied.
if (process.argv[2]) {
    const patched = fs.readFileSync(process.argv[2], 'utf8');
    const selector = patched.includes('aFq(a,b){\n') ? 'aFq' : 'aGL';
    const begin = patched.indexOf(selector + '(a,b){\n');
    const end = patched.indexOf('return selected<0?null:b[selected]},', begin);
    assert.ok(begin >= 0 && end > begin);
    const matcher = vm.runInNewContext('({' + patched.slice(begin, end + 'return selected<0?null:b[selected]}'.length) + '}).' + selector);
    const slots = [{ f: '#000000', e: 0 }, { f: '#409040', e: 1 }];
    const owner = { aIm: delta, aJo: delta };
    assert.equal(matcher.call(owner, { x: '#003000' }, slots), slots[1]);
    assert.equal(matcher.call(owner, { x: '#000000' }, slots), slots[0]);
    assert.equal(matcher.call(owner, { x: '#003000' }, []), null);
    console.log('PASS patched upstream selector returns original slot objects');
}
