// Tests for the mod list in packaging/wasm/index.html: which mods the page
// finds in a picked game folder, and opening zipped mods. In Node (22 or
// newer, for Blob and DecompressionStream):
// node packaging/wasm/test/picker-test.mjs
//
// Every file is made up by the test. The page's functions are taken from
// index.html as they are.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { makeGob, makeZip } from './helpers.mjs';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const { openZips, detectMods, isSkipped } = new Function(
    cut('      var REQUIRED_JK', '      function loadEngine') +
    cut('      var MOD_RESOURCE_DIRS', '      // ---- Which files each mod changes') +
    '\nreturn { openZips, detectMods, isSkipped };')();

const gob = (name, text) => makeGob([{ name, bytes: Buffer.from(text) }]);
const install = ['episode/JK1.gob', 'episode/JK1CTF.gob', 'episode/JK1MP.gob', 'resource/Res1hi.gob', 'resource/Res2.gob'];
// Picked files: paths, made-up GOBs for names ending in .gob, and the
// bytes given for the rest.
function pick(extra) {
    const files = install.map(p => [p, null]).concat(Object.entries(extra));
    return files.map(([path, bytes]) => ({
        path, file: new Blob([bytes || (/\.gob$/i.test(path) ? gob('x', path) : Buffer.from(path))]) }));
}
const list = (files) => detectMods(files, false).map(m => m.name + ' (' + m.from + ')');

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test('a folder with several GOBs is a mod for each', () => {
    assert.deepEqual(list(pick({ 'mod/SBX.gob': null, 'mod/Other.gob': null, 'mod/readme.txt': null })),
                     ['Other (mod)', 'SBX (mod)']);
});

test('its loose files are one more, first, since the game reads them before its GOBs', () => {
    assert.deepEqual(list(pick({ 'mod/A.gob': null, 'mod/B.gob': null, 'mod/cog/x.cog': null })),
                     ['mod (loose files) (mod)', 'A (mod)', 'B (mod)']);
});

test('mod folders inside a folder are mods of their own', () => {
    assert.deepEqual(list(pick({ 'mod/A.gob': null, 'mod/SBX/sbx.gob': null, 'mod/Two/cog/t.cog': null })),
                     ['A (mod)', 'SBX (mod/SBX)', 'Two (mod/Two)']);
    assert.deepEqual(list(pick({ 'pack/One/one.gob': null, 'pack/Two/two.gob': null })),
                     ['One (pack/One)', 'Two (pack/Two)']);
});

test('a classic mod folder is still one mod', () => {
    const mods = detectMods(pick({ 'SBX/sbx.gob': null, 'SBX/cog/x.cog': null, 'SBX/readme.txt': null }), false);
    assert.deepEqual(mods.map(m => m.name), ['SBX']);
    assert.equal(mods[0].path, 'jkmods/0-SBX');
});

test('mods and Resource keep the original game order', () => {
    assert.deepEqual(list(pick({ 'mods/m1.gob': null, 'mods/Folder/f.gob': null, 'resource/Saber.gob': null })),
                     ['Folder (mods/Folder)', 'm1 (mods)', 'Saber (resource)']);
});

test('zipped mods are opened in place and each is a mod', async () => {
    const sbx = gob('3do\\bow0.3do', 'sbx bow');
    const files = await openZips(pick({
        'mod/Maps.gob': null,
        'mod/SBX.zip': makeZip([{ name: 'Saber Battle X - JK.gob', bytes: sbx }, { name: 'readme.txt', bytes: Buffer.from('hi') }]),
        'mod/Blade.zip': makeZip([{ name: 'Blade/', bytes: Buffer.alloc(0), store: true },
                                  { name: 'Blade/blade.gob', bytes: gob('cog\\b.cog', 'blade') },
                                  { name: '__MACOSX/Blade/._blade.gob', bytes: Buffer.from('junk') }]),
        'mod/Loose.zip': makeZip([{ name: 'cog/l.cog', bytes: Buffer.from('L1'), store: true },
                                  { name: 'mat\\l.mat', bytes: Buffer.from('L2') }]),
        'mods/Extra.zip': makeZip([{ name: 'Resource/extra.gob', bytes: gob('ui\\e.bm', 'extra') },
                                   { name: 'Episode/level.gob', bytes: gob('jkl\\x.jkl', 'level') }]),
    }), false);
    const paths = files.map(e => e.path);
    for (const p of ['mod/SBX.zip/Saber Battle X - JK.gob', 'mod/Blade.zip/blade.gob', 'mod/Loose.zip/cog/l.cog',
                     'mod/Loose.zip/mat/l.mat', 'mods/Extra.zip/extra.gob'])
        assert.ok(paths.includes(p), p);
    assert.ok(!paths.some(p => /\.zip$|readme|macosx|level/i.test(p)), paths.join(', '));
    const opened = (p) => files.find(e => e.path === p).file.arrayBuffer().then(b => Buffer.from(b));
    assert.ok((await opened('mod/SBX.zip/Saber Battle X - JK.gob')).equals(Buffer.from(sbx)));
    assert.equal((await opened('mod/Loose.zip/cog/l.cog')).toString(), 'L1');
    assert.equal((await opened('mod/Loose.zip/mat/l.mat')).toString(), 'L2');
    assert.deepEqual(list(files), ['Maps (mod)', 'Blade (mod/Blade.zip)', 'Loose (mod/Loose.zip)',
                                   'SBX (mod/SBX.zip)', 'Extra (mods/Extra.zip)']);
});

test('zips that are not mods stay closed', async () => {
    const before = pick({
        'Backup.zip': makeZip([{ name: 'JK/Episode/JK1.GOB', bytes: Buffer.from('game') }, { name: 'JK/sbx.gob', bytes: gob('x', 'y') }]),
        'notes.zip': makeZip([{ name: 'a.txt', bytes: Buffer.from('a') }]),
        'mod/Level.zip': makeZip([{ name: 'Episode/level.gob', bytes: gob('jkl\\x.jkl', 'level') }]),
        'episode/levels.zip': makeZip([{ name: 'l.gob', bytes: gob('x', 'y') }]),
        'mod/broken.zip': Buffer.from('not a zip'),
    });
    const after = await openZips(before, false);
    assert.deepEqual(after.map(e => e.path), before.map(e => e.path));
    assert.deepEqual(list(after), []);
});

// Laid out like a Steam Mysteries of the Sith folder with the MotS
// Remastered pack in it, as the page has it after skipping what it doesn't load.
const goo = (name, text) => makeGob([{ name, bytes: Buffer.from(text) }]);
function pickMots(extra) {
    const files = {
        'episode/JKM.goo': null, 'episode/JKM_KFY.goo': null, 'episode/JKM_MP.goo': null, 'episode/JKM_SABER.goo': null,
        'episode/MotS Secret Level/jkl/S6L1.jkl': 'jkl', 'resource/Jkmres.goo': null, 'resource/JKMsndLO.goo': null,
        'resource/jk_.cd': 'cd', 'resource/Backup Sounds/JKMSNDLO.GOO': null, 'resource/ui/BM/bkmain.bm': 'bm',
        'mods/JKMRemasteredAssets.goo': null, 'jkmcutscenes/JKM4B.GOO': null,
        'jkgm/materials/SABER19/metadata.json': '{}', 'Controls/assassin.ctm': 'c', 'registry.json': '{}',
        ...extra };
    return Object.entries(files).filter(([path]) => !isSkipped(path.toLowerCase())).map(([path, bytes]) => ({
        path, file: new Blob([bytes || (/\.goo$/i.test(path) ? goo('x', path) : Buffer.from(path))]) }));
}
const listMots = (files) => detectMods(files, true).map(m => m.name + ' (' + m.from + ')');

test('the MotS Remastered pack is loaded like the game, not listed', () => {
    const files = pickMots({});
    assert.deepEqual(listMots(files), []);
    assert.ok(files.some(e => e.path === 'mods/JKMRemasteredAssets.goo'));
    assert.ok(!files.some(e => /^jkmcutscenes\//.test(e.path)));
});

test('MotS mods are still listed next to it', () => {
    assert.deepEqual(listMots(pickMots({ 'mods/More.goo': null, 'resource/Saber.goo': null, 'SBX/sbx.goo': null })),
                     ['SBX (SBX)', 'More (mods)', 'Saber (resource)']);
});

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
