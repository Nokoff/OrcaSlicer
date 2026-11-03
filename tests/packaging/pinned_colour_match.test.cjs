// Run against the pinned Control release extracted by stage_connect_bundle.py.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const original = fs.readFileSync(process.argv[2], 'utf8');
const patched = fs.readFileSync(process.argv[3], 'utf8');
assert.equal(crypto.createHash('sha256').update(original).digest('hex'),
    '96109093573873627edd92bc538d46eaa0f46470f337a8d1f281e0f4db07f657');
function method(text, signature, next) {
    const start = text.indexOf(signature), end = text.indexOf(next, start);
    assert.ok(start >= 0 && end > start);
    return text.slice(start, end - 1);
}
const type = { h: () => type };
const A = {
    Z: () => type, a0: a => a, c: a => a,
    aD: function (values, predicate) { return values.filter(a => predicate.$1(a)); },
    aTy: function () { this.$1 = a => a.b && a.c; },
    aTz: function () { this.$1 = a => a.d && !a.w; },
    aTA: function () { this.$1 = a => a.d; },
    aTB: function () { this.$2 = (a, b) => a.e - b.e || a.a.a - b.a.a; },
    xf: hex => hex ? hex.split(';').map(s => parseInt(s.replace('#', '').slice(0, 6), 16)) : []
};
const B = { e: { di: (a, b) => a >>> b }, b: { hp: (a, compare) => a.sort(compare.$2), ga1: a => a[0] } };
const context = { A, B, t: { n: null } };
A.bZW = vm.runInNewContext('({' + method(original, 'bZW(a,b,c){', '\nxf(a){') + '}).bZW', context);
A.bZX = vm.runInNewContext('({' + method(original, 'bZX(a6,a7){', '\nbZW(a,b,c){') + '}).bZX', context);
const oldSelect = vm.runInNewContext('({' + method(original, 'aJR(a){var s,r,q,p=A.Z(a)', '\nb75(a){') + '}).aJR', context);
const newSelect = vm.runInNewContext('({' + method(patched, 'aJR(a,source){var s,r,q,p=A.Z(a)', '\nb75(a){') + '}).aJR', context);
function candidate(source, colour, id, flags = {}) {
    return { a: { a: id, e: colour, f: [] }, b: true, c: true, d: true, w: false,
        e: A.bZX(parseInt(source.slice(1), 16), parseInt(colour.slice(1), 16)), ...flags };
}
function run(source, colours, expected, baseline) {
    const options = colours.map((c, i) => candidate(source, c, i));
    if (baseline !== undefined) assert.equal(oldSelect(options).a.a, baseline);
    assert.equal(newSelect(options, { gaqO: () => A.xf(source) }).a.a, expected);
}
run('#003000', ['#000000', '#409040'], 1, 0);
run('#500808', ['#000000', '#C04040'], 1, 0);
run('#080840', ['#000000', '#8080FF'], 1, 0);
run('#000000', ['#000000', '#409040'], 0);
run('#202020', ['#000000', '#409040'], 0);
run('#003000', ['#000000', '#E03030'], 0);
run('#003000', ['#000000', '#003000', '#409040'], 1);
for (const flags of [{ b: false }, { c: false }, { d: false }, { w: true }]) {
    const options = [candidate('#003000', '#000000', 0), candidate('#003000', '#409040', 1, flags)];
    assert.equal(newSelect(options, { gaqO: () => [0x003000] }).a.a, 0);
}
assert.equal(newSelect([], { gaqO: () => [] }), null);
assert.ok(patched.includes('if(g!=null&&g.b&&g.c)d.m(0,j,g.a.a)\nelse g=this.aJR(h,k)'));
assert.ok(original.includes('if(g!=null&&g.b&&g.c)d.m(0,j,g.a.a)\nelse g=this.aJR(h)'));
console.log('PASS pinned release: dark colours, neutrals, exact matches, compatibility, flow priority, empty slots and manual-selection branch');
