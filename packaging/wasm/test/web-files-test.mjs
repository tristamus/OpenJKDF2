// The files packaging/wasm/index.html loads or opens by name beside itself
// (each engine's script and wasm and unzip.js, from ENGINE_FILES; the room
// pictures, from ROOM_PICTURES; move-out.html), and the page itself, are
// all in both web folders a build makes: the one the CI collects (web/, in
// .github/workflows/wasm.yml) and the one build_run_openjkdf2_wasm.sh makes
// for local testing (wasm_out/). Read from the cp lines of each, nothing
// built or copied. In Node: node packaging/wasm/test/web-files-test.mjs

import { readFileSync, readdirSync, existsSync } from 'node:fs';
import assert from 'node:assert/strict';

const repo = new URL('../../../', import.meta.url);
const text = path => readFileSync(new URL(path, repo), 'utf8');
const html = text('packaging/wasm/index.html');
// What s has from from (included) to to.
function cut(s, from, to) {
    const a = s.indexOf(from), b = s.indexOf(to, a + from.length);
    assert.ok(a >= 0 && b > a, 'has ' + from);
    return s.substring(a, b);
}

const files = JSON.parse(cut(html, '/* kit-files */', '/* /kit-files */').slice('/* kit-files */'.length));
const pictures = new Function(cut(html, '      var ROOM_PICTURES = {', '      (function() {') + 'return ROOM_PICTURES;')();
const wanted = ['index.html', 'move-out.html', files.unzip, ...['jspi', 'asyncify'].flatMap(e => [files[e].js, files[e].wasm]),
                ...Object.values(pictures)];
assert.ok(html.includes("MOVE_FROM + '/move-out.html'"), 'the page opens move-out.html by name');

// What a script's cp lines put under out/ (lines joined at a trailing
// backslash): each as its path there. A source ending in /* is every file
// in that folder, and cp -r of a folder every file in it, under its name.
function copied(script, out) {
    const placed = new Set();
    for (const line of script.replace(/\\\n\s*/g, ' ').split('\n')) {
        const m = /^\s*cp\s+(.*)$/.exec(line);
        if (!m) continue;
        const args = m[1].trim().split(/\s+/);
        const recursive = args[0] === '-r';
        if (recursive) args.shift();
        const dest = args.pop();
        if (dest !== out && !dest.startsWith(out + '/')) continue;
        const at = dest.slice(out.length + 1);
        const into = at === '' || at.endsWith('/');
        for (const src of args) {
            const name = src.replace(/\/\*?$/, '').split('/').pop();
            if (src.endsWith('/*'))
                readdirSync(new URL(src.slice(0, -1), repo)).forEach(f => placed.add(at + f));
            else if (recursive)
                readdirSync(new URL(src + '/', repo)).forEach(f => placed.add(at + (into ? name + '/' : '/') + f));
            else
                placed.add(into ? at + name : at);
        }
    }
    return placed;
}

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

test('what the page loads by name is in the source tree', () => {
    for (const name of ['index.html', 'move-out.html', files.unzip, ...Object.values(pictures)])
        assert.ok(existsSync(new URL('packaging/wasm/' + name, repo)), name);
});

for (const [where, script, out] of [
    ['the CI\'s web folder', cut(text('.github/workflows/wasm.yml'), '- name: Collect web build', '- uses:'), 'web'],
    ['build_run_openjkdf2_wasm.sh\'s wasm_out', text('build_run_openjkdf2_wasm.sh'), 'wasm_out'],
]) {
    test(`${where} has every file the page loads or opens by name`, () => {
        const placed = copied(script, out);
        const missing = wanted.filter(name => !placed.has(name));
        assert.deepEqual(missing, [], 'not copied: ' + missing.join(', '));
    });
}

let failed = 0;
for (const t of tests) {
    try {
        await t.fn();
        console.log('ok   ' + t.name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + t.name + '\n' + (e.stack || e));
    }
}
console.log(failed ? `${failed} of ${tests.length} failed` : `all ${tests.length} passed`);
process.exit(failed ? 1 : 0);
