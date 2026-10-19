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
const { openZips, detectMods, isSkipped, pickedFiles, placeModFiles, filesToRead } = new Function(
    cut('      var REQUIRED_JK', '      function loadEngine') +
    cut('      var MOD_RESOURCE_DIRS', '      // ---- Which files each mod changes') +
    '\nreturn { openZips, detectMods, isSkipped, pickedFiles, placeModFiles, filesToRead };')();

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

// The page's memory of which mods were left unticked, with a stand-in for
// the browser's localStorage.
function memory(store) {
    return new Function('localStorage',
        cut('      // Which mods from the game folder this browser left', '      function megabytes') +
        '\nreturn { modKey, modsLeftOut, rememberModsLeftOut };')(store);
}
function makeStore() {
    const items = {};
    return { getItem: k => k in items ? items[k] : null, setItem: (k, v) => { items[k] = String(v); }, items };
}

test('each mod keeps its key for remembering it when other mods come and go', () => {
    const { modKey } = memory(makeStore());
    const folder = { 'mod/SBX.gob': null, 'mod/Two/t.gob': null, 'mods/SBX.gob': null, 'resource/SBX.gob': null, 'SBX/sbx.gob': null };
    const before = detectMods(pick(folder), false);
    const keys = before.map(modKey);
    assert.equal(new Set(keys).size, keys.length, keys.join(', '));
    const after = detectMods(pick({ ...folder, 'mod/A.gob': null }), false);
    for (const m of before) {
        const same = after.find(n => modKey(n) === modKey(m));
        assert.ok(same && same.from === m.from && same.name === m.name, modKey(m));
    }
    // Its folder under jkmods/ is numbered by its place in the list, so it moves.
    assert.notEqual(after.find(n => modKey(n) === 'mod/sbx').path, before.find(m => modKey(m) === 'mod/sbx').path);
});

test('the mods left unticked are remembered for each game', () => {
    const store = makeStore();
    const { modsLeftOut, rememberModsLeftOut } = memory(store);
    assert.deepEqual(modsLeftOut('jk1'), []);
    rememberModsLeftOut('jk1', ['mod/sbx', 'mods/blade']);
    assert.deepEqual(modsLeftOut('jk1'), ['mod/sbx', 'mods/blade']);
    assert.deepEqual(modsLeftOut('mots'), []);
    rememberModsLeftOut('jk1', []);
    assert.deepEqual(modsLeftOut('jk1'), []);
});

test('a broken or blocked store leaves every mod ticked', () => {
    for (const bad of ['not json', '{"a":1}', '3']) {
        const store = makeStore();
        store.items['jkdf2mp-mods-left-out-jk1'] = bad;
        assert.deepEqual(memory(store).modsLeftOut('jk1'), [], bad);
    }
    const blocked = { getItem() { throw new Error('blocked'); }, setItem() { throw new Error('blocked'); } };
    const { modsLeftOut, rememberModsLeftOut } = memory(blocked);
    assert.deepEqual(modsLeftOut('jk1'), []);
    rememberModsLeftOut('jk1', ['mod/sbx']);
});

// ---- What the page reads from a picked folder: the steps onFolderPicked
// takes before reading anything, on folders laid out like real ones. Paths
// are given as they are under the picked folder; GOBs are made up, other
// files hold their own path.
async function whatIsRead(folder, { mots = false, modPath = null } = {}) {
    const ext = mots ? '.goo' : '.gob';
    const fileList = Object.entries(folder).map(([path, bytes]) => {
        const file = new Blob([bytes || (path.toLowerCase().endsWith(ext) ? gob('x', path) : Buffer.from(path))]);
        file.webkitRelativePath = 'Picked/' + path;
        return file;
    });
    const sorted = pickedFiles(fileList);
    let files = sorted.files;
    if (!modPath && files.some(e => /\.zip$/i.test(e.path)))
        files = await openZips(files, mots);
    const mods = modPath ? [] : detectMods(files, mots);
    placeModFiles(files, mods);
    return { read: filesToRead(files, ext, modPath).map(e => e.path).sort(),
             players: sorted.playerFiles.map(e => e.path).sort(), mods };
}

// Antonio's Steam folders (2026-10-19), a few of each kind of file.
const STEAM_JK = Object.fromEntries([
    '32380_install.vdf', 'DeIsL1.isu', 'JediKnight.EXE', 'ipxwrapper.log', 'registry.json', 'openjkdf2_cvars.json',
    'removeRegistry.bat', 'Controls/set0000.ctl', 'Episode/APG_Deathmatch_Castle.gob', 'Episode/JK1.GOB',
    'Episode/JK1CTF.GOB', 'Episode/JK1MP.GOB', 'MUSIC/Track12.ogg', 'Resource/JK_.CD', 'Resource/Res1hi.gob',
    'Resource/Res2.gob', 'Resource/VIDEO/01-02A.SMK', 'player/BahamutZero0o/BahamutZero0o.plr',
    'player/BahamutZero0o/Tristamus.mpc', 'player/BahamutZero0o/openjkdf2.json', 'player/BahamutZero0o/save0000.jks',
    'player/BahamutZero0o/_JKAUTO_01narshadda.jks', 'player/steam_autocloud.vdf',
].map(p => [p, null]));
const STEAM_JK_READ = ['Controls/set0000.ctl', 'Episode/APG_Deathmatch_Castle.gob', 'Episode/JK1.GOB', 'Episode/JK1CTF.GOB',
                       'Episode/JK1MP.GOB', 'Resource/JK_.CD', 'Resource/Res1hi.gob', 'Resource/Res2.gob'];
const STEAM_MOTS = Object.fromEntries([
    '32390_install.vdf', 'JediKnightM.exe', 'JKMRemastered.bat', 'JediKnightM.RPT', 'Sith.doc', 'MysteriesOfTheSith.sdb',
    'openjkdf2_cvars.json', 'registry.json', 'motsr.ico', 'Controls/MS SIDEWINDER GAMEPAD.CTM', 'Episode/JKM.GOO',
    'Episode/JKM_KFY.GOO', 'Episode/JKM_MP.GOO', 'Episode/JKM_SABER.GOO', 'Episode/JKM/cog/s1l1_endlevel.cog',
    'Episode/MotS Secret Level/episode.jk', 'Episode/MotS Secret Level/jkl/S6L1.jkl',
    'Episode/MotS Secret Level/3do/mat/vader_cape.mat', 'Episode/MotS Secret Level/misc/pup/mjdarkjedi.pup',
    'MUSIC/Track02.ogg', 'MUSIC/Backup/Track04.ogg', 'Mods/JKMRemasteredAssets.goo',
    'Player/BahamutZero0o/BahamutZero0o.plr', 'Player/BahamutZero0o/Tristamus.mpc', 'Player/BahamutZero0o/quicksave.jks',
    'Player/steam_autocloud.vdf', 'Resource/Backup Sounds/JKMSNDLO.GOO', 'Resource/JKMRES.GOO', 'Resource/JKMsndLO.goo',
    'Resource/JK_.CD', 'Resource/VIDEO/CUTSCENES.ZIP', 'Resource/VIDEO/S1L1OCS.SAN', 'Resource/ui/BM/bkmain.bm',
    'Resource/ui/credits.uni', 'doc/config.md', 'jkgm/materials/SABER19/saber19.mat.saber.albedo.png',
    'jkgm/materials/MOTSGFXEXT1/metadata.json', 'jkgm/shaders/menu.frag', 'jkmcutscenes/JKM4B.GOO',
].map(p => [p, null]));

test('a Steam Jedi Knight folder reads only the game files and characters', async () => {
    const { read, players, mods } = await whatIsRead(STEAM_JK);
    assert.deepEqual(read, STEAM_JK_READ.slice().sort());
    assert.deepEqual(players, ['BahamutZero0o/BahamutZero0o.plr', 'BahamutZero0o/Tristamus.mpc', 'BahamutZero0o/openjkdf2.json']);
    assert.deepEqual(mods, []);
});

test('a Steam MotS folder with the Remastered pack leaves its textures, backups and docs unread', async () => {
    const { read, players, mods } = await whatIsRead(STEAM_MOTS, { mots: true });
    assert.deepEqual(read, [
        'Controls/MS SIDEWINDER GAMEPAD.CTM', 'Episode/JKM.GOO', 'Episode/JKM/cog/s1l1_endlevel.cog', 'Episode/JKM_KFY.GOO',
        'Episode/JKM_MP.GOO', 'Episode/JKM_SABER.GOO', 'Episode/MotS Secret Level/3do/mat/vader_cape.mat',
        'Episode/MotS Secret Level/episode.jk', 'Episode/MotS Secret Level/jkl/S6L1.jkl',
        'Episode/MotS Secret Level/misc/pup/mjdarkjedi.pup', 'Mods/JKMRemasteredAssets.goo', 'Resource/JKMRES.GOO',
        'Resource/JKMsndLO.goo', 'Resource/JK_.CD', 'Resource/ui/BM/bkmain.bm', 'Resource/ui/credits.uni'].sort());
    assert.deepEqual(players, ['BahamutZero0o/BahamutZero0o.plr', 'BahamutZero0o/Tristamus.mpc']);
    assert.deepEqual(mods, []);
});

test('an install with upper-case names reads the same files', async () => {
    const { read, players } = await whatIsRead(Object.fromEntries([
        'JK.EXE', 'README.TXT', 'UNINST.ISU', 'EPISODE/JK1.GOB', 'EPISODE/JK1CTF.GOB', 'EPISODE/JK1MP.GOB',
        'RESOURCE/RES1HI.GOB', 'RESOURCE/RES2.GOB', 'RESOURCE/JK_.CD', 'RESOURCE/VIDEO/01-02A.SMK',
        'CONTROLS/SET0000.CTL', 'PLAYER/KYLE/KYLE.PLR', 'PLAYER/KYLE/SAVE0000.JKS',
    ].map(p => [p, null])));
    assert.deepEqual(read, ['CONTROLS/SET0000.CTL', 'EPISODE/JK1.GOB', 'EPISODE/JK1CTF.GOB', 'EPISODE/JK1MP.GOB',
                            'RESOURCE/JK_.CD', 'RESOURCE/RES1HI.GOB', 'RESOURCE/RES2.GOB']);
    assert.deepEqual(players, ['KYLE/KYLE.PLR']);
});

test('loose game files, mods and maps are read; screenshots, readmes and copies are not', async () => {
    const { read, mods } = await whatIsRead({
        ...STEAM_JK,
        'cog/00_door.cog': null, 'mat/dflt.mat': null, 'JK1MP.GOB': null, 'Screenshots/shot0001.png': null,
        'SBX/sbx.gob': null, 'SBX/cog/sbx.cog': null, 'SBX/readme.txt': null, 'SBX/screenshot.jpg': null,
        'SBX/install.bat': null,
        'mod/Blade.zip': makeZip([{ name: 'blade.gob', bytes: gob('cog\\b.cog', 'blade') },
                                  { name: 'preview.png', bytes: Buffer.from('png') }]),
        'mods/readme.md': null,
        'episode/MyLevel.gob': null, 'episode/MyLevel.jpg': null, 'episode/MyEp/episode.jk': null,
        'episode/MyEp/jkl/x.jkl': null, 'episode/MyEp/notes.doc': null, 'episode/MyEp/extra/a.gob': null,
        'resource/cog/fix.cog': null, 'resource/Backup/Res2.gob': null, 'resource/notes.doc': null,
    });
    const sbx = mods.find(m => m.name === 'SBX'), blade = mods.find(m => m.name === 'Blade');
    assert.ok(sbx && blade, mods.map(m => m.name).join(', '));
    assert.deepEqual(read, STEAM_JK_READ.concat([
        'cog/00_door.cog', 'mat/dflt.mat', sbx.path + '/sbx.gob', sbx.path + '/cog/sbx.cog', blade.path + '/blade.gob',
        'episode/MyLevel.gob', 'episode/MyEp/episode.jk', 'episode/MyEp/jkl/x.jkl', 'resource/cog/fix.cog']).sort());
    // The size shown for a mod is what's read of it.
    assert.equal(sbx.bytes, gob('x', 'SBX/sbx.gob').length + 'SBX/cog/sbx.cog'.length);
});

test('a folder given with ?path= is read like a mod', async () => {
    const { read } = await whatIsRead({ ...STEAM_JK, 'MyMod/x.gob': null, 'MyMod/cog/c.cog': null,
                                        'MyMod/shot.jpg': null, 'Other/o.gob': null }, { modPath: 'mymod' });
    assert.deepEqual(read, STEAM_JK_READ.concat(['MyMod/x.gob', 'MyMod/cog/c.cog']).sort());
});

test("the other game's GOBs aren't read", async () => {
    const mots = await whatIsRead({ ...STEAM_MOTS, 'Episode/stray.gob': null }, { mots: true });
    assert.ok(!mots.read.includes('Episode/stray.gob'));
    const jk = await whatIsRead({ ...STEAM_JK, 'Episode/stray.goo': null });
    assert.ok(!jk.read.includes('Episode/stray.goo'));
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
