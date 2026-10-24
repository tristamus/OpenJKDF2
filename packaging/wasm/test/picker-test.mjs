// Tests for the mod list in packaging/wasm/index.html: which mods the page
// finds in a picked game folder, and opening zipped mods. In Node (22 or
// newer, for Blob and DecompressionStream):
// node packaging/wasm/test/picker-test.mjs
//
// Every file is made up by the test. The page's functions are taken from
// index.html as they are.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { makeGob, makeZip, makeIDB } from './helpers.mjs';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const { openZips, detectMods, isSkipped, pickedFiles, placeModFiles, filesToRead, placeGraphicsPacks, filesToSave,
        gameFileStore, whatsNew, newsText, savedDay, keptFolderStore, listFolder, folderKind, keptPath, keptMerge,
        REQUIRED_JK, REQUIRED_MOTS, hasGameData, placeCdKey } = new Function(
    cut('      var REQUIRED_JK', '      function loadEngine') +
    cut('      var MOD_RESOURCE_DIRS', '      // ---- Which files each mod changes') +
    '\nreturn { openZips, detectMods, isSkipped, pickedFiles, placeModFiles, filesToRead, placeGraphicsPacks, ' +
    'filesToSave, gameFileStore, whatsNew, newsText, savedDay, keptFolderStore, listFolder, folderKind, keptPath, ' +
    'keptMerge, REQUIRED_JK, REQUIRED_MOTS, hasGameData, placeCdKey };')();
const { checkGraphicsPack } = new Function(
    cut('      async function gobTable', '      async function modContents') + '\nreturn { checkGraphicsPack };')();

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

// ---- Graphics packs: optional by their file name alone (OPTIONAL_PACKS).

const optional = (files, mots = false) => detectMods(files, mots).filter(m => m.optional).map(m => m.name + ' (' + m.from + ')');

test('JKE.GOB, JKR.GOB and JK HD Models.gob are graphics packs in any case, listed last', () => {
    const files = pick({ 'mods/JK HD Models.gob': null, 'resource/jke.gob': null, 'mod/JKR.GOB': null, 'mod/Other.gob': null,
                         'SBX/sbx.gob': null, 'mods/Blade.gob': null });
    assert.deepEqual(list(files), ['Other (mod)', 'SBX (SBX)', 'Blade (mods)',
                                   'JKR (mod)', 'JK HD Models (mods)', 'jke (resource)']);
    assert.deepEqual(optional(files), ['JKR (mod)', 'JK HD Models (mods)', 'jke (resource)']);
    // Numbered in list order, like every mod.
    assert.deepEqual(detectMods(files, false).map(m => m.path.split('-')[0]),
                     [0, 1, 2, 3, 4, 5].map(i => 'jkmods/' + i));
});

test('a folder that holds only a named pack is one, and one with more files is a mod like any other', () => {
    assert.deepEqual(optional(pick({ 'HD/JK HD Models.gob': null, 'HD/readme.txt': null })), ['HD (HD)']);
    assert.deepEqual(optional(pick({ 'HD/JK HD Models.gob': null, 'HD/cog/x.cog': null })), []);
    assert.deepEqual(optional(pick({ 'HD/JK HD Models.gob': null, 'HD/x.gob': null })), ['JK HD Models (HD)']);
});

test('only those names are graphics packs, and only for Jedi Knight', () => {
    assert.deepEqual(optional(pick({ 'mods/JK HD Models SP.gob': null, 'mods/JKE2.gob': null, 'mods/HD Models.gob': null })), []);
    assert.deepEqual(optional(pickMots({ 'mods/jke.goo': null, 'mods/JK HD Models.goo': null })), []);
});

test('ticked graphics packs go to mods/ in list order, and the other ticked mods are what the host shares', () => {
    const files = pick({ 'mods/JK HD Models.gob': null, 'mod/JKE.GOB': null, 'mod/A.gob': null, 'SBX/sbx.gob': null,
                         'SBX/cog/x.cog': null });
    const mods = detectMods(files, false);
    placeModFiles(files, mods);
    const at = name => mods.findIndex(m => m.name === name);
    const option = name => ({ index: at(name), name });
    const picked = [option('SBX'), option('JKE'), option('JK HD Models')];
    const split = placeGraphicsPacks(picked, mods, files);
    assert.deepEqual(split.mods.map(o => o.name), ['SBX']);
    assert.deepEqual(split.packs.map(o => o.name), ['JKE', 'JK HD Models']);
    const paths = files.map(e => e.path);
    assert.ok(paths.includes('mods/001-JKE.GOB'), paths.join(', '));
    assert.ok(paths.includes('mods/002-JK HD Models.gob'), paths.join(', '));
    assert.ok(paths.includes(mods[at('SBX')].path + '/cog/x.cog'));
    // An unticked one stays in its own folder, where the engine doesn't look.
    const alone = pick({ 'mods/JK HD Models.gob': null, 'mod/JKE.GOB': null, 'mod/B.gob': null });
    const aloneMods = detectMods(alone, false);
    placeModFiles(alone, aloneMods);
    const jke = aloneMods.findIndex(m => m.name === 'JKE');
    assert.deepEqual(placeGraphicsPacks([{ index: jke, name: 'JKE' }], aloneMods, alone).packs.length, 1);
    assert.ok(alone.some(e => e.path === 'mods/001-JKE.GOB'));
    assert.ok(alone.some(e => e.path.startsWith('jkmods/') && e.path.endsWith('/JK HD Models.gob')));
    // A saved set is one host's mods, packs and all.
    const saved = { name: 'Saved', path: 'netmods/0123456789abcdef', saved: true };
    assert.deepEqual(placeGraphicsPacks([saved], aloneMods, alone), { mods: [saved], packs: [] });
});

test('a graphics pack with more than textures and models says so in the console, and still loads', async () => {
    const warned = [];
    const warn = console.warn;
    console.warn = (...a) => { warned.push(a.join(' ')); };
    try {
        const pure = new Blob([makeGob([{ name: 'mat\\kyle.mat', bytes: Buffer.from('m') },
                                        { name: '3do\\ky.3do', bytes: Buffer.from('k') },
                                        { name: 'readme.txt', bytes: Buffer.from('r') }])]);
        assert.deepEqual(await checkGraphicsPack('JK HD Models', pure), []);
        assert.equal(warned.length, 0);
        const mixed = new Blob([makeGob([{ name: 'mat\\kyle.mat', bytes: Buffer.from('m') },
                                         { name: 'cog\\kyle.cog', bytes: Buffer.from('c') },
                                         { name: 'sound\\hit.wav', bytes: Buffer.from('s') }])]);
        assert.deepEqual(await checkGraphicsPack('JKE', mixed), ['cog/kyle.cog', 'sound/hit.wav']);
        assert.equal(warned.length, 1);
        assert.match(warned[0], /JKE also has 2 files besides textures and models.*cog\/kyle\.cog, sound\/hit\.wav/);
        // A file that isn't a GOB has nothing to say.
        assert.deepEqual(await checkGraphicsPack('JKR', new Blob([Buffer.from('not a gob')])), []);
    } finally {
        console.warn = warn;
    }
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
    return readFrom(folderFiles(folder, mots), { mots, modPath });
}

// A jk_.cd key as the file holds it: Jedi Knight's on Steam, or MotS's.
const JK_KEY = 0x699232C4, MOTS_KEY = 0x3B426929, OTHER_KEY = 0x12345678;
function cdKey(key) {
    const out = Buffer.alloc(4);
    out.writeUInt32LE(key);
    return out;
}
// What a made-up file holds when the test gives nothing: a GOB for the
// game's GOBs, the game's key for jk_.cd, and its own path for the rest.
function contents(path, bytes, mots) {
    const lower = path.toLowerCase();
    return bytes || (lower.endsWith(mots ? '.goo' : '.gob') ? gob('x', path) :
                     /(^|\/)jk_\.cd$/.test(lower) ? cdKey(mots ? MOTS_KEY : JK_KEY) : Buffer.from(path));
}

function folderFiles(folder, mots) {
    return Object.entries(folder).map(([path, bytes]) => {
        const file = new Blob([contents(path, bytes, mots)]);
        file.webkitRelativePath = 'Picked/' + path;
        return file;
    });
}

async function readFrom(fileList, { mots = false, modPath = null } = {}) {
    const ext = mots ? '.goo' : '.gob';
    const sorted = pickedFiles(fileList);
    const placed = await placeCdKey(sorted.files, mots ? 'mots' : 'jk1');
    let files = placed.files;
    if (!modPath && files.some(e => /\.zip$/i.test(e.path)))
        files = await openZips(files, mots);
    const mods = modPath ? [] : detectMods(files, mots);
    placeModFiles(files, mods);
    const toRead = filesToRead(files, ext, modPath);
    return { read: toRead.map(e => e.path).sort(), players: sorted.playerFiles.map(e => e.path).sort(), mods,
             toSave: filesToSave(fileList, toRead, sorted.playerFiles), cd: placed.cd };
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

// ---- jk_.cd, wherever it is in the folder. A player's CD install
// (C:\GAMES\JK, 2026-10-22) keeps it next to JK.EXE, not in Resource.
const CD_INSTALL = Object.fromEntries([
    'JK.EXE', 'Jedi.EXE', 'JK_.CD', 'README.TXT', 'Episode/JK1.GOB', 'Episode/JK1CTF.GOB', 'Episode/JK1MP.GOB',
    'Resource/Res1hi.gob', 'Resource/Res2.gob', 'Controls/set0000.ctl', 'player/Kyle/Kyle.plr',
].map(p => [p, null]));
const without = (folder, ...paths) => Object.fromEntries(Object.entries(folder).filter(([p]) => !paths.includes(p)));
const keyOf = async (e) => Buffer.from(await e.file.arrayBuffer()).readUInt32LE(0);
const cdRead = (read) => read.filter(p => /jk_\.cd$/i.test(p));

test('jk_.cd next to the EXE is read as resource/jk_.cd', async () => {
    const { read, cd } = await whatIsRead(CD_INSTALL);
    assert.deepEqual(cdRead(read), ['resource/jk_.cd']);
    assert.equal(cd.file.webkitRelativePath, 'Picked/JK_.CD');
    assert.equal(await keyOf(cd), JK_KEY);
});

test('with none higher up, the copy nearest the top is used', async () => {
    const { read, cd } = await whatIsRead({ ...without(CD_INSTALL, 'JK_.CD'),
                                            'Backup/2015/Old/JK_.CD': null, 'GAMEDATA/RESOURCE/JK_.CD': null });
    assert.equal(cd.file.webkitRelativePath, 'Picked/GAMEDATA/RESOURCE/JK_.CD');
    assert.deepEqual(cdRead(read), ['resource/jk_.cd']);
});

test('Resource comes first, then the top, and a copy without a key the game takes is passed over', async () => {
    let r = await whatIsRead({ ...CD_INSTALL, 'Resource/JK_.CD': null });
    assert.equal(r.cd.file.webkitRelativePath, 'Picked/Resource/JK_.CD');
    assert.deepEqual(cdRead(r.read), ['Resource/JK_.CD']);
    // MotS's key in Resource: the one at the top, and Resource's isn't read.
    r = await whatIsRead({ ...CD_INSTALL, 'Resource/JK_.CD': cdKey(MOTS_KEY) });
    assert.equal(r.cd.file.webkitRelativePath, 'Picked/JK_.CD');
    assert.deepEqual(cdRead(r.read), ['resource/jk_.cd']);
    // Too short at the top and an unknown key in Resource: one further down.
    r = await whatIsRead({ ...CD_INSTALL, 'JK_.CD': Buffer.from('ab'), 'Resource/JK_.CD': cdKey(OTHER_KEY),
                           'Disc/JK_.CD': null });
    assert.equal(r.cd.file.webkitRelativePath, 'Picked/Disc/JK_.CD');
    // Each Jedi Knight key the engine takes at startup (src/engine_config.h).
    for (const key of [0x69973284, 0x699232C4, 0x69923384]) {
        r = await whatIsRead({ ...CD_INSTALL, 'JK_.CD': cdKey(key) });
        assert.equal(r.cd.file.webkitRelativePath, 'Picked/JK_.CD', key.toString(16));
    }
});

test('with no jk_.cd it can use, the game starts with the standard key', async () => {
    for (const [folder, mots, key] of [
        [without(CD_INSTALL, 'JK_.CD'), false, JK_KEY],
        [{ ...CD_INSTALL, 'JK_.CD': cdKey(MOTS_KEY) }, false, JK_KEY],
        [{ ...CD_INSTALL, 'JK_.CD': cdKey(0x69923284) }, false, JK_KEY],
        [without(STEAM_MOTS, 'Resource/JK_.CD'), true, MOTS_KEY],
        [{ ...STEAM_MOTS, 'Resource/JK_.CD': cdKey(JK_KEY) }, true, MOTS_KEY],
    ]) {
        const { read, cd } = await whatIsRead(folder, { mots });
        assert.deepEqual(cdRead(read), ['resource/jk_.cd']);
        assert.equal(cd.file.webkitRelativePath, undefined);
        assert.deepEqual([...Buffer.from(await cd.file.arrayBuffer())], [...cdKey(key)]);
    }
});

test('MotS takes jk_.cd from the top too', async () => {
    const { read, cd } = await whatIsRead({ ...without(STEAM_MOTS, 'Resource/JK_.CD'), 'JK_.CD': null }, { mots: true });
    assert.equal(cd.file.webkitRelativePath, 'Picked/JK_.CD');
    assert.equal(await keyOf(cd), MOTS_KEY);
    assert.deepEqual(cdRead(read), ['resource/jk_.cd']);
});

test('saved with jk_.cd next to the EXE, or with none, the game starts the same from the browser', async () => {
    for (const folder of [CD_INSTALL, without(CD_INSTALL, 'JK_.CD')]) {
        const store = gameFileStore(makeIDB());
        const first = await readFrom(datedFiles(folder, false));
        assert.equal(savedPaths(first.toSave).includes('JK_.CD'), 'JK_.CD' in folder);
        await store.save('jk1', first.toSave);
        const again = await readFrom(await store.load('jk1'));
        assert.deepEqual(again.read, first.read);
        assert.equal(await keyOf(again.cd), JK_KEY);
    }
});

test('the game files alone say which game a folder has', () => {
    const have = (folder) => Object.fromEntries(Object.keys(folder).map(p => [p.toLowerCase(), true]));
    assert.ok(hasGameData(REQUIRED_JK, have(CD_INSTALL)));
    assert.ok(hasGameData(REQUIRED_JK, have(without(CD_INSTALL, 'JK_.CD'))));
    assert.ok(!hasGameData(REQUIRED_MOTS, have(CD_INSTALL)));
    assert.ok(hasGameData(REQUIRED_MOTS, have(without(STEAM_MOTS, 'Resource/JK_.CD'))));
    assert.ok(!hasGameData(REQUIRED_JK, have(without(CD_INSTALL, 'Resource/Res2.gob'))));
});

test('a kept folder with jk_.cd next to the EXE, or none, is still the game folder', async () => {
    assert.equal(await folderKind(folderHandle('JK', CD_INSTALL), 'jk1'), 'game');
    assert.equal(await folderKind(folderHandle('JK', without(CD_INSTALL, 'JK_.CD')), 'jk1'), 'game');
    assert.equal(await folderKind(folderHandle('JK', CD_INSTALL), 'mots'), 'other');
});

// ---- Saved in the browser for next time: the files read and the
// characters, in the folder's order, which start the game just like the
// folder does.
const savedPaths = (files) => files.map(f => f.webkitRelativePath.substring('Picked/'.length));
const MODS_AND_EXTRAS = {
    'cog/00_door.cog': null, 'Screenshots/shot0001.png': null,
    'SBX/sbx.gob': null, 'SBX/cog/sbx.cog': null, 'SBX/readme.txt': null, 'SBX/screenshot.jpg': null,
    'mod/Blade.zip': makeZip([{ name: 'blade.gob', bytes: gob('cog\\b.cog', 'blade') },
                              { name: 'preview.png', bytes: Buffer.from('png') }]),
    'mod/notes.zip': makeZip([{ name: 'a.txt', bytes: Buffer.from('a') }]),
    'episode/MyLevel.gob': null, 'episode/MyLevel.jpg': null,
};

for (const [name, folder, mots, saved] of [
    ['Jedi Knight with mods, a zipped one and screenshots', { ...STEAM_JK, ...MODS_AND_EXTRAS }, false, [
        ...STEAM_JK_READ, 'player/BahamutZero0o/BahamutZero0o.plr', 'player/BahamutZero0o/Tristamus.mpc',
        'player/BahamutZero0o/openjkdf2.json', 'cog/00_door.cog', 'SBX/sbx.gob', 'SBX/cog/sbx.cog', 'mod/Blade.zip',
        'episode/MyLevel.gob']],
    ['MotS with the Remastered pack', STEAM_MOTS, true, [
        'Controls/MS SIDEWINDER GAMEPAD.CTM', 'Episode/JKM.GOO', 'Episode/JKM_KFY.GOO', 'Episode/JKM_MP.GOO',
        'Episode/JKM_SABER.GOO', 'Episode/JKM/cog/s1l1_endlevel.cog', 'Episode/MotS Secret Level/episode.jk',
        'Episode/MotS Secret Level/jkl/S6L1.jkl', 'Episode/MotS Secret Level/3do/mat/vader_cape.mat',
        'Episode/MotS Secret Level/misc/pup/mjdarkjedi.pup', 'Mods/JKMRemasteredAssets.goo',
        'Player/BahamutZero0o/BahamutZero0o.plr', 'Player/BahamutZero0o/Tristamus.mpc', 'Resource/JKMRES.GOO',
        'Resource/JKMsndLO.goo', 'Resource/JK_.CD', 'Resource/ui/BM/bkmain.bm', 'Resource/ui/credits.uni']],
]) {
    test('saving ' + name + ' keeps what is read, and starts the same from the browser', async () => {
        const fileList = folderFiles(folder, mots);
        const picked = await readFrom(fileList, { mots });
        const kept = savedPaths(picked.toSave);
        assert.deepEqual(kept, Object.keys(folder).filter(p => saved.includes(p)));
        assert.equal(kept.length, saved.length);
        const again = await readFrom(picked.toSave, { mots });
        assert.deepEqual(again.read, picked.read);
        assert.deepEqual(again.players, picked.players);
        const mod = m => ({ name: m.name, from: m.from, path: m.path, bytes: m.bytes });
        assert.deepEqual(again.mods.map(mod), picked.mods.map(mod));
        assert.deepEqual(savedPaths(again.toSave), kept);
    });
}

test('saved game files come back with their paths, and are replaced or removed per game', async () => {
    const store = gameFileStore(makeIDB());
    assert.equal(await store.info('jk1'), null);
    const files = folderFiles({ 'Episode/JK1.GOB': null, 'mod/Blade.zip': Buffer.from('zip'),
                                'player/Kyle/Kyle.plr': Buffer.from('kyle') }, false);
    await store.save('jk1', files);
    const info = await store.info('jk1');
    assert.equal(info.folder, 'Picked');
    assert.equal(info.files, 3);
    assert.equal(info.bytes, files.reduce((n, f) => n + f.size, 0));
    const back = await store.load('jk1');
    assert.deepEqual(back.map(f => f.webkitRelativePath), files.map(f => f.webkitRelativePath));
    for (let i = 0; i < files.length; i++)
        assert.ok(Buffer.from(await back[i].arrayBuffer()).equals(Buffer.from(await files[i].arrayBuffer())));
    assert.deepEqual(pickedFiles(back).playerFiles.map(e => e.path), ['Kyle/Kyle.plr']);
    assert.equal(await store.info('mots'), null);
    await store.save('jk1', files.slice(0, 1));
    assert.equal((await store.info('jk1')).files, 1);
    assert.equal((await store.load('jk1')).length, 1);
    await store.forget('jk1');
    assert.equal(await store.info('jk1'), null);
    await assert.rejects(store.load('jk1'));
});

// ---- Choosing the folder again: only what changed is saved, and what's
// new since is named. Files carry their date modified, as a browser's do.
function datedFiles(folder, mots, times = {}) {
    return Object.entries(folder).map(([path, bytes]) => {
        const file = new File([contents(path, bytes, mots)], path.split('/').pop(), { lastModified: times[path] || 1000 });
        file.webkitRelativePath = 'Picked/' + path;
        return file;
    });
}
const dataKeys = idb => [...idb.dbs.get('jkdf2mp-game-files').stores.get('data').data.keys()].sort();

test('choosing the folder again only writes the files that changed, and removes the ones that are gone', async () => {
    const idb = makeIDB(), store = gameFileStore(idb);
    const first = datedFiles({ 'Episode/JK1.GOB': null, 'Episode/Old.gob': null, 'SBX/sbx.gob': null,
                               'player/Kyle/Kyle.plr': Buffer.from('kyle') }, false);
    assert.equal(await store.save('jk1', first), first.reduce((n, f) => n + f.size, 0));
    const again = datedFiles({ 'Episode/JK1.GOB': null, 'SBX/sbx.gob': Buffer.from('a newer sbx'),
                               'Episode/AODpc.gob': null, 'player/Kyle/Kyle.plr': Buffer.from('kyle') }, false,
                             { 'SBX/sbx.gob': 2000 });
    const written = await store.save('jk1', again);
    assert.equal(written, again[1].size + again[2].size);
    assert.deepEqual(dataKeys(idb), ['jk1/episode/aodpc.gob', 'jk1/episode/jk1.gob', 'jk1/player/kyle/kyle.plr',
                                     'jk1/sbx/sbx.gob']);
    const info = await store.info('jk1');
    assert.equal(info.files, 4);
    assert.equal(info.bytes, again.reduce((n, f) => n + f.size, 0));
    // The files come back in the folder's order, with the newer copies.
    const back = await store.load('jk1');
    assert.deepEqual(back.map(f => f.webkitRelativePath), again.map(f => f.webkitRelativePath));
    for (let i = 0; i < again.length; i++)
        assert.ok(Buffer.from(await back[i].arrayBuffer()).equals(Buffer.from(await again[i].arrayBuffer())));
    // Nothing changed: nothing is written.
    assert.equal(await store.save('jk1', again), 0);
    await store.save('mots', datedFiles({ 'Episode/JKM.GOO': null }, true));
    await store.forget('jk1');
    assert.deepEqual(dataKeys(idb), ['mots/episode/jkm.goo']);
    assert.equal(await store.info('jk1'), null);
});

test('files saved the old way still start the game, and the next save moves them over', async () => {
    const idb = makeIDB(), store = gameFileStore(idb);
    assert.equal(await store.info('jk1'), null);
    const old = datedFiles({ 'Episode/JK1.GOB': null, 'SBX/sbx.gob': null }, false);
    const db = idb.dbs.get('jkdf2mp-game-files');
    db.stores.get('data').data.set('jk1', old.map(f => ({ path: f.webkitRelativePath, file: f })));
    db.stores.get('meta').data.set('jk1', { game: 'jk1', folder: 'Picked', files: 2, bytes: 10, time: 5000 });
    const back = await store.load('jk1');
    assert.deepEqual(back.map(f => f.webkitRelativePath), old.map(f => f.webkitRelativePath));
    const index = await store.index('jk1');
    assert.equal(index.time, 5000);
    assert.ok(index.whole);
    assert.deepEqual(Object.keys(index.files).sort(), ['episode/jk1.gob', 'sbx/sbx.gob']);
    assert.equal(await store.save('jk1', old), old.reduce((n, f) => n + f.size, 0));
    assert.deepEqual(dataKeys(idb), ['jk1/episode/jk1.gob', 'jk1/sbx/sbx.gob']);
    assert.ok(!(await store.index('jk1')).whole);
    assert.equal((await store.load('jk1')).length, 2);
});

// The steps onFolderPicked takes up to the mod window, then whatsNew
// against what was saved from the first folder.
async function newsFor(firstFolder, secondFolder, { mots = false, times = {} } = {}) {
    const store = gameFileStore(makeIDB());
    const first = await readFrom(datedFiles(firstFolder, mots), { mots });
    await store.save(mots ? 'mots' : 'jk1', first.toSave);
    const saved = await store.index(mots ? 'mots' : 'jk1');
    const sorted = pickedFiles(datedFiles(secondFolder, mots, times));
    let files = sorted.files;
    if (files.some(e => /\.zip$/i.test(e.path)))
        files = await openZips(files, mots);
    const mods = detectMods(files, mots);
    placeModFiles(files, mods);
    const news = whatsNew(saved, files, mods, mots ? '.goo' : '.gob');
    const tags = Object.fromEntries(mods.filter((m, i) => news.mods[i]).map(m => [m.name, news.mods[mods.indexOf(m)]]));
    return { news, tags, text: newsText(news, mods, saved.time), day: savedDay(saved.time) };
}

test('a new map, a new mod and an updated mod are named, and nothing else', async () => {
    const before = { ...STEAM_JK, 'SBX/sbx.gob': null, 'SBX/cog/sbx.cog': null, 'mod/Blade.gob': null,
                     'episode/MyEp/episode.jk': null, 'episode/MyEp/jkl/x.jkl': null };
    const { news, tags, text, day } = await newsFor(before, {
        ...before, 'SBX/cog/sbx.cog': Buffer.from('changed'), 'SBX/readme.txt': null, 'Episode/AODpc.gob': null,
        'mod/Saber Battle X.zip': makeZip([{ name: 'sbx2.gob', bytes: gob('cog\\s.cog', 'sbx2') }]),
        'Screenshots/shot0002.png': null, 'episode/MyEp/notes.doc': null,
    }, { times: { 'SBX/cog/sbx.cog': 2000 } });
    assert.deepEqual(tags, { 'SBX': 'updated', 'Saber Battle X': 'new' });
    assert.deepEqual(news.maps, { added: ['AODpc'], updated: [] });
    assert.equal(text, 'New since ' + day + ': map AODpc and mod Saber Battle X. Updated: mod SBX.');
});

test('a new episode folder is a new map, and a changed level in it an updated one', async () => {
    const before = { ...STEAM_JK, 'episode/Old/episode.jk': null, 'episode/Old/jkl/o.jkl': null };
    const { news, text, day } = await newsFor(before, {
        ...before, 'episode/Old/jkl/o.jkl': Buffer.from('v2'), 'episode/New Ep/episode.jk': null,
        'episode/New Ep/jkl/n.jkl': null, 'Episode/JK1MP.GOB': null,
    }, { times: { 'episode/Old/jkl/o.jkl': 3000 } });
    assert.deepEqual(news.maps, { added: ['New Ep'], updated: ['Old'] });
    assert.equal(text, 'New since ' + day + ': map New Ep. Updated: map Old.');
});

test('the same folder again has nothing new', async () => {
    const folder = { ...STEAM_MOTS, 'Episode/ctf_test.goo': null };
    const { news, tags, text, day } = await newsFor(folder, folder, { mots: true });
    assert.deepEqual(tags, {});
    assert.deepEqual(news.maps, { added: [], updated: [] });
    assert.equal(text, 'Nothing new since ' + day + '.');
});

test("the game's own GOBs aren't maps, even when they change", async () => {
    const { news, text, day } = await newsFor(STEAM_MOTS, STEAM_MOTS, { mots: true, times: {
        'Episode/JKM.GOO': 2000, 'Episode/JKM_MP.GOO': 2000 } });
    assert.deepEqual(news.maps, { added: [], updated: [] });
    assert.equal(text, 'Nothing new since ' + day + '.');
});

test('several new maps are counted, with the first few named', async () => {
    const extra = Object.fromEntries(['A', 'B', 'C', 'D', 'E'].map(n => ['Episode/' + n + '.gob', null]));
    const { text, day } = await newsFor(STEAM_JK, { ...STEAM_JK, ...extra, 'Mod Two/two.gob': null, 'Mod One/one.gob': null });
    assert.equal(text, 'New since ' + day + ': 5 maps (A, B, C, ...) and 2 mods (Mod One, Mod Two).');
});

// ---- A folder the browser keeps, read again at every start. A stand-in
// for the browser's folder handle, from paths under the folder, which
// notes the folders it lists and the files it opens.
function folderHandle(name, folder, { mots = false, times = {} } = {}, log = { listed: [], opened: [] }, prefix = '') {
    const inside = new Map();
    for (const [path, bytes] of Object.entries(folder)) {
        const at = path.indexOf('/');
        if (at < 0) {
            inside.set(path, { bytes, path: prefix + path });
        } else {
            const sub = inside.get(path.substring(0, at)) || { folder: {} };
            sub.folder[path.substring(at + 1)] = bytes;
            inside.set(path.substring(0, at), sub);
        }
    }
    return {
        kind: 'directory', name, log,
        async *values() {
            log.listed.push(prefix);
            for (const [n, x] of inside) {
                if (x.folder) {
                    yield folderHandle(n, x.folder, { mots, times }, log, prefix + n + '/');
                } else {
                    yield { kind: 'file', name: n, async getFile() {
                        log.opened.push(x.path);
                        return new File([contents(x.path, x.bytes, mots)], n, { lastModified: times[x.path] || 1000 });
                    } };
                }
            }
        },
    };
}
// listFolder's files as a picked folder named name gives them.
function asPicked(name, listed) {
    return listed.map(e => {
        Object.defineProperty(e.file, 'webkitRelativePath', { value: name + '/' + e.path });
        return e.file;
    });
}

test("a kept folder brings what a picked one does, without opening the folders the game never reads", async () => {
    const dir = folderHandle('Star Wars Jedi Knight Mysteries of the Sith', STEAM_MOTS, { mots: true });
    const listed = await listFolder(dir);
    const want = Object.keys(STEAM_MOTS).filter(p => !isSkipped(p.toLowerCase()) && !/^jkgm\//.test(p));
    assert.deepEqual(listed.map(e => e.path).sort(), want.sort());
    for (const skipped of ['jkgm/', 'MUSIC/', 'Resource/VIDEO/', 'jkmcutscenes/'])
        assert.ok(!dir.log.listed.includes(skipped), skipped + ' was listed');
    assert.ok(!dir.log.opened.some(p => /^jkgm|^MUSIC|VIDEO|\.exe$|\.ico$/i.test(p)), dir.log.opened.join(', '));
    // Each folder's files by name, then its folders.
    assert.deepEqual(listed.slice(0, 4).map(e => e.path), ['32390_install.vdf', 'JediKnightM.RPT', 'JKMRemastered.bat',
                                                           'MysteriesOfTheSith.sdb']);
    assert.equal(listed.find(e => e.path.includes('/')).path, 'Controls/MS SIDEWINDER GAMEPAD.CTM');
    const kept = await readFrom(asPicked('Kept', listed), { mots: true });
    const picked = await whatIsRead(STEAM_MOTS, { mots: true });
    assert.deepEqual(kept.read, picked.read);
    assert.deepEqual(kept.players, picked.players);
});

test('a folder picked to keep is the game folder, the other game, part of one, or mods and maps', async () => {
    const kind = (folder, game) => folderKind(folderHandle('Picked', folder), game);
    assert.equal(await kind(STEAM_JK, 'jk1'), 'game');
    assert.equal(await kind(STEAM_MOTS, 'mots'), 'game');
    assert.equal(await kind(STEAM_MOTS, 'jk1'), 'other');
    assert.equal(await kind(STEAM_JK, 'mots'), 'other');
    assert.equal(await kind({ 'EPISODE/jk1.gob': null, 'resource/JK_.CD': null }, 'jk1'), 'some');
    assert.equal(await kind({ 'Episode/AODpc.gob': null, 'SBX.zip': null, 'resource/jk_.cd': null }, 'jk1'), 'extra');
    assert.equal(await kind({ 'Episode/JK1.GOB/x': null, 'Episode/JK1CTF.GOB': null }, 'jk1'), 'some');
});

test("a kept folder's maps go in episode, and the rest is mods", () => {
    assert.equal(keptPath('Episode/AODpc.gob', 'JKWEB mods'), 'episode/AODpc.gob');
    assert.equal(keptPath('EPISODE/My Ep/episode.jk', 'mod'), 'episode/My Ep/episode.jk');
    assert.equal(keptPath('SBX/cog/sbx.cog', 'mod'), 'mods/SBX/cog/sbx.cog');
    assert.equal(keptPath('Saber.zip', 'mod'), 'mods/Saber.zip');
    assert.equal(keptPath('Episodes/x.gob', 'mod'), 'mods/Episodes/x.gob');
    assert.equal(keptPath('AODpc.gob', 'Episode'), 'episode/AODpc.gob');
});

test('a kept folder of mods and maps is laid over the saved files, and what is new in it named', async () => {
    const store = gameFileStore(makeIDB());
    const fromStart = async (copy, folder, seen, times) => {
        const listed = await listFolder(folderHandle('My mods', folder, { times }));
        const extras = listed.map(e => ({ path: keptPath(e.path, 'My mods'), file: e.file }));
        const merged = keptMerge(copy, extras, seen);
        const now = {};
        for (const e of extras) now[e.path.toLowerCase()] = { size: e.file.size, time: e.file.lastModified };
        return { merged, extras: new Set(extras.map(e => e.file)), seen: now };
    };
    const first = await readFrom(datedFiles(STEAM_JK, false));
    await store.save('jk1', first.toSave);
    // The first start: two mods and a map.
    const one = await fromStart(await store.load('jk1'), {
        'Old.gob': null, 'Episode/Gone.gob': null, 'Episode/JK1MP.GOB': Buffer.from('a copy'), 'readme.txt': null }, {});
    const merged = one.merged.map(f => f.webkitRelativePath);
    assert.ok(merged.includes('Picked/mods/Old.gob') && merged.includes('Picked/episode/Gone.gob'), merged.join(', '));
    // The folder's file wins over the saved one with its path.
    assert.equal(merged.filter(p => /episode\/jk1mp\.gob$/i.test(p)).length, 1);
    assert.ok(one.extras.has(one.merged.find(f => /jk1mp/i.test(f.webkitRelativePath))));
    assert.deepEqual(detectMods(pickedFiles(one.merged).files, false).map(m => m.name + ' (' + m.from + ')'), ['Old (mods)']);
    await store.save('jk1', (await readFrom(one.merged)).toSave);
    // The next start: Gone.gob is gone, Old.gob is the same, and there's a new map and two new mods.
    const copy = await store.load('jk1');
    assert.ok(copy.some(f => /Gone\.gob$/.test(f.webkitRelativePath)));
    const two = await fromStart(copy, {
        'Old.gob': null, 'Episode/JK1MP.GOB': Buffer.from('a copy'), 'Episode/AODpc.gob': null,
        'SBX/sbx.gob': null, 'SBX/cog/sbx.cog': null,
        'Blade.zip': makeZip([{ name: 'blade.gob', bytes: gob('cog\\b.cog', 'blade') }]) }, one.seen);
    const paths = two.merged.map(f => f.webkitRelativePath);
    assert.ok(!paths.some(p => /Gone/.test(p)), paths.join(', '));
    assert.equal(paths.filter(p => /mods\/Old\.gob$/.test(p)).length, 1);
    let files = await openZips(pickedFiles(two.merged).files, false);
    const mods = detectMods(files, false);
    placeModFiles(files, mods);
    assert.deepEqual(mods.map(m => m.name + ' (' + m.from + ')'), ['Blade (mods/Blade.zip)', 'SBX (mods/SBX)', 'Old (mods)']);
    const saved = await store.index('jk1');
    const news = whatsNew({ time: saved.time, files: { ...saved.files, ...one.seen } }, files, mods, '.gob');
    assert.deepEqual(news.maps, { added: ['AODpc'], updated: [] });
    assert.deepEqual(news.mods, { 0: 'new', 1: 'new' });
    assert.equal(newsText(news, mods, saved.time),
                 'New since ' + savedDay(saved.time) + ': map AODpc and 2 mods (Blade, SBX).');
    // Saved, the map that's gone goes from the saved files too.
    await store.save('jk1', (await readFrom(two.merged)).toSave);
    const keys = (await store.load('jk1')).map(f => f.webkitRelativePath.toLowerCase());
    assert.ok(!keys.some(k => k.includes('gone')) && keys.includes('picked/episode/aodpc.gob') &&
              keys.includes('picked/mods/blade.zip'), keys.join(', '));
});

test('kept folders are kept per game, and forgotten', async () => {
    const store = keptFolderStore(makeIDB());
    assert.deepEqual(await store.all(), []);
    const folder = { game: 'jk1', handle: { name: 'mod' }, kind: 'extra', name: 'mod', time: 0, seen: {} };
    await store.put(folder);
    await store.put({ ...folder, game: 'mots', kind: 'game', name: 'MotS' });
    await store.put({ ...folder, time: 5, seen: { 'mods/sbx.gob': { size: 3, time: 1000 } } });
    const all = await store.all();
    assert.deepEqual(all.map(f => f.game + ' ' + f.kind + ' ' + f.time).sort(), ['jk1 extra 5', 'mots game 0']);
    await store.remove('jk1');
    assert.deepEqual((await store.all()).map(f => f.game), ['mots']);
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
