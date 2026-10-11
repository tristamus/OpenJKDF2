// Tests for packaging/wasm/mods.js, sharing the host's mod with joiners, in
// Node (22 or newer, for WebCrypto): node packaging/wasm/test/mods-test.mjs
//
// As in maps-test.mjs, every file is made up by the test. Each "tab" loads
// maps.js and then mods.js, as openjkdf2.js does, with its own in-memory file
// system and IndexedDB.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { makeGob, sha, contains, makeFS, makeIDB } from './helpers.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const mapsSource = readFileSync(join(here, '..', 'maps.js'), 'utf8');
const modsSource = readFileSync(join(here, '..', 'mods.js'), 'utf8');

const STATE_BUSY = 0, STATE_READY = 1, STATE_FAILED = 2, STATE_RESTART = 3;

// A tab as the page starts it: picked files written, then the engine's
// preRun steps, which mods.js uses to work out the running mod.
async function makeTab({ files, args = [], folders = [], idb = makeIDB(), cwd = '/jk1' }) {
    const tab = {
        FS: makeFS(files, cwd),
        arguments: args,
        jkModFolders: folders,
        preRun: [],
        idb,
        deps: new Set(),
        addRunDependency(d) { this.deps.add(d); },
        removeRunDependency(d) { this.deps.delete(d); },
    };
    new Function('Module', 'indexedDB', mapsSource)(tab, idb);
    new Function('Module', 'indexedDB', modsSource)(tab, idb);
    for (const step of tab.preRun)
        step();
    const until = Date.now() + 10000;
    while (tab.deps.size) {
        if (Date.now() > until)
            throw new Error('preRun never finished: ' + [...tab.deps]);
        await new Promise(r => setTimeout(r, 5));
    }
    return tab;
}

// Two tabs and the link between them, as in maps-test.mjs.
function connect(host, join, { direct = true, drop = () => false } = {}) {
    const log = [];
    const link = { direct, drop, log };
    const deliver = (to, from, bytes, via) => {
        const copy = bytes.slice(0);
        log.push({ to: to === host ? 'host' : 'join', via, bytes: new Uint8Array(copy) });
        if (via === 'relay')
            assert.ok(copy.byteLength <= 64 * 1024, 'relay frame over the lobby cap: ' + copy.byteLength);
        if (link.drop(log[log.length - 1]))
            return;
        setImmediate(() => to.jkMaps.onFrame(from, copy, via));
    };
    host.jkNet = {
        mapDirect: id => id === 2 && link.direct,
        mapSend(id, bytes, via) {
            const used = via !== 'relay' && link.direct ? 'direct' : 'relay';
            deliver(join, 1, bytes, used);
            return used;
        },
    };
    join.jkNet = {
        mapDirect: id => id === 1 && link.direct,
        mapSend(id, bytes, via) {
            const used = via !== 'relay' && link.direct ? 'direct' : 'relay';
            deliver(host, 2, bytes, used);
            return used;
        },
    };
    return link;
}

async function waitMod(tab, ms = 20000) {
    const until = Date.now() + ms;
    while (tab.jkMods.state() === STATE_BUSY) {
        if (Date.now() > until)
            throw new Error('still busy');
        await new Promise(r => setTimeout(r, 5));
    }
    return tab.jkMods.state();
}

const settle = (ms = 50) => new Promise(r => setTimeout(r, ms));

// The host's lobby tail, as the engine sends it and the joiner reads it.
const tailOf = tab => tab.jkMods.tail();

// Files under a folder, relative, from a tab's file system.
function filesUnder(tab, dir) {
    const out = {};
    for (const [p, b] of tab.FS.files) {
        if (p.startsWith(dir + '/'))
            out[p.slice(dir.length + 1)] = Buffer.from(b);
    }
    return out;
}

function framesTo(log, who, type) {
    return log.filter(f => f.to === who && f.bytes[12] === type);
}

// ---- Fixtures: a retail install, and a mod that carries copies of some
// retail files three ways (a loose copy, inside a GOB of its own, and a whole
// retail GOB).

const retailA = randomBytes(3000), retailB = randomBytes(5000), retailC = randomBytes(700);
const retailRes = makeGob([
    { name: 'mat\\a.mat', bytes: retailA },
    { name: 'sound\\b.wav', bytes: retailB },
    { name: 'cog\\c.cog', bytes: retailC },
]);
const retailEp = makeGob([{ name: 'jkl\\01narshadda.jkl', bytes: randomBytes(4000) }]);
const retailInstall = {
    '/jk1/resource/Res2.gob': retailRes,
    '/jk1/resource/Res1hi.gob': makeGob([{ name: 'x\\y', bytes: randomBytes(10) }]),
    '/jk1/episode/JK1MP.gob': retailEp,
};

const sabers = randomBytes(150000), forceCog = randomBytes(2000);
const modGob = makeGob([
    { name: 'cog\\force_saber.cog', bytes: forceCog },
    { name: 'mat\\a.mat', bytes: retailA },
]);
const sbxFiles = {
    'sbx.gob': modGob,
    'cog/sabers.cog': sabers,
    '3do/b.wav': retailB,              // a loose copy of a retail file
    'misc/JK1MP.gob': retailEp,        // a whole retail GOB
    'readme.txt': Buffer.from('not part of the mod'),
    '.DS_Store': randomBytes(30),
};
const sbxWanted = ['3do/b.wav', 'cog/sabers.cog', 'misc/JK1MP.gob', 'sbx.gob'];

function install(dir, files, root = '/jk1') {
    const out = {};
    for (const [rel, bytes] of Object.entries(files))
        out[root + '/' + dir + '/' + rel] = bytes;
    return out;
}

function sbxHost(extra = {}) {
    return makeTab({
        files: { ...retailInstall, ...install('jkmods/0-SBX', sbxFiles), ...extra },
        args: ['-path', 'jkmods/0-SBX'],
        folders: [{ name: 'SBX', path: 'jkmods/0-SBX' }],
    });
}

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

// ---- The pack.

test('the same mod files make the same pack, whatever else is in the folder', async () => {
    const a = await makeTab({ files: { ...install('m', sbxFiles) } });
    const shuffled = Object.fromEntries(Object.entries(sbxFiles).reverse());
    const b = await makeTab({ files: { ...install('other', shuffled), '/jk1/other/Thumbs.db': randomBytes(9) } });
    const pa = a.jkMods.lib.pack('/jk1/m'), pb = b.jkMods.lib.pack('/jk1/other');
    assert.deepEqual(pa.entries.map(e => e.name), sbxWanted);
    assert.equal(sha(pa.bytes), sha(pb.bytes));
    // It's a GOB the map tools read.
    const dir = a.jkMaps.lib.parseGob((o, l) => pa.bytes.subarray(o, o + l), pa.bytes.length);
    assert.deepEqual(dir.entries.map(e => e.name), sbxWanted);

    // And it unpacks to the same files.
    a.jkMods.lib.unpack(pa.bytes, '/jk1/out');
    const back = filesUnder(a, '/jk1/out');
    assert.deepEqual(Object.keys(back).sort(), sbxWanted);
    for (const rel of sbxWanted)
        assert.deepEqual(back[rel], Buffer.from(sbxFiles[rel]));

    const changed = await makeTab({ files: install('m', { ...sbxFiles, 'cog/sabers.cog': randomBytes(10) }) });
    assert.notEqual(sha(changed.jkMods.lib.pack('/jk1/m').bytes), sha(pa.bytes));
});

test('a pack from another player only unpacks safe names, and only into its folder', async () => {
    const tab = await makeTab({ files: {} });
    const { unpack } = tab.jkMods.lib;
    const bad = [
        [{ name: '../escape', bytes: randomBytes(5) }],
        [{ name: 'a/../../escape', bytes: randomBytes(5) }],
        [{ name: '/abs', bytes: randomBytes(5) }],
        [{ name: 'a\\b', bytes: randomBytes(5) }],
        [{ name: 'c:x', bytes: randomBytes(5) }],
        [{ name: 'x', bytes: randomBytes(5) }, { name: 'X', bytes: randomBytes(5) }],
        [{ name: 'cog', bytes: randomBytes(5) }, { name: 'cog/x.cog', bytes: randomBytes(5) }],
        [{ name: 'a/b/c/d/e/f/g/h/i/j/k', bytes: randomBytes(5) }],
        [{ name: 'trailing.', bytes: randomBytes(5) }],
        [{ name: 'naïve', bytes: randomBytes(5) }],
    ];
    for (const files of bad) {
        assert.throws(() => unpack(makeGob(files), '/jk1/netmods/x'), e => e.reason === 'bad-files', files[0].name);
    }
    assert.throws(() => unpack(randomBytes(400), '/jk1/netmods/x'), e => e.reason === 'corrupt');
    assert.equal([...tab.FS.files.keys()].length, 0);
    unpack(makeGob([{ name: 'cog/ok.cog', bytes: randomBytes(5) }, { name: 'My Sabers/x.mat', bytes: randomBytes(5) }]),
           '/jk1/netmods/x');
    assert.deepEqual([...tab.FS.files.keys()].sort(), ['/jk1/netmods/x/My Sabers/x.mat', '/jk1/netmods/x/cog/ok.cog']);
});

test('the lobby tail carries the mod\'s name and fingerprint, and nothing else reads as one', async () => {
    const tab = await makeTab({ files: {} });
    const { encodeTail, parseTail } = tab.jkMods.lib;
    const fp = sha(Buffer.from('x'));
    const t = encodeTail(fp, 'Saber Battle X');
    assert.deepEqual(parseTail(t), { fp, name: 'Saber Battle X' });
    assert.equal(parseTail(''), null);
    assert.ok(parseTail('00').bad);
    assert.ok(parseTail(t.slice(0, -2)).bad);
    assert.ok(parseTail(t + '00').bad);
    assert.ok(parseTail('zz' + t.slice(2)).bad);
    assert.equal(parseTail(encodeTail(fp, 'x'.repeat(200))).name.length, 63);
    assert.equal(parseTail(encodeTail(fp, 'café')).name, 'caf?');
    // 240-byte entry and the longest tail fit in the lobby's 2048 hex digits
    // and the engine's 1024-byte line.
    assert.ok(480 + encodeTail(fp, 'x'.repeat(63)).length + 'welcome 64 '.length < 1024);
});

// ---- Host and joiner.

test('the running mod is worked out before the engine starts, and named in the lobby tail', async () => {
    const host = await sbxHost();
    const cur = host.jkMods.current();
    assert.equal(cur.name, 'SBX');
    assert.equal(cur.path, 'jkmods/0-SBX');
    assert.equal(cur.fp, sha(host.jkMods.lib.pack('/jk1/jkmods/0-SBX').bytes));
    assert.deepEqual(host.jkMods.lib.parseTail(tailOf(host)), { fp: cur.fp, name: 'SBX' });

    const vanilla = await makeTab({ files: { ...retailInstall } });
    assert.equal(vanilla.jkMods.current(), null);
    assert.equal(tailOf(vanilla), '');
});

test('a joiner without the mod gets it from the host, without any original game files', async () => {
    const host = await sbxHost();
    const join = await makeTab({ files: { ...retailInstall } });
    const link = connect(host, join);
    host.jkMods.host();
    await settle();
    join.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(join), STATE_RESTART);
    const fp = host.jkMods.current().fp;
    const path = 'netmods/' + fp.slice(0, 16);
    assert.equal(join.jkMods.restart(), path);
    assert.equal(join.jkMods.current().fp, fp);
    assert.equal(join.jkMods.current().name, 'SBX');

    const got = filesUnder(join, '/jk1/' + path);
    assert.deepEqual(Object.keys(got).sort(), sbxWanted);
    for (const rel of sbxWanted)
        assert.deepEqual(got[rel], Buffer.from(sbxFiles[rel]), rel);

    for (const f of link.log.filter(f => f.to === 'join')) {
        assert.equal(f.bytes[14], 1, 'a mod frame');
        assert.ok(!contains(f.bytes, retailA) && !contains(f.bytes, retailB) && !contains(f.bytes, retailEp),
                  'original game file bytes were sent');
    }
    const sent = framesTo(link.log, 'join', 4).reduce((n, f) => n + f.bytes.length - 28, 0);
    const pack = host.jkMods.lib.pack('/jk1/jkmods/0-SBX').bytes;
    assert.ok(sent < pack.length - retailA.length - retailB.length - retailEp.length + 4 + 4 * 40, 'sent ' + sent);
    console.log(`    ${pack.length} byte mod: ${sent} bytes sent, the rest filled in from the joiner's own game`);

    // Now joining runs the host's mod: nothing more to do.
    join.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(join), STATE_READY);
});

test('through the lobby, mod frames stay under its cap', async () => {
    const host = await sbxHost({ '/jk1/jkmods/0-SBX/big.dat': randomBytes(400000) });
    const join = await makeTab({ files: { ...retailInstall } });
    const link = connect(host, join, { direct: false });
    join.jkNet.rtcEnabled = () => false;
    host.jkMods.host();
    await settle();
    join.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(join), STATE_RESTART);
    assert.ok(link.log.every(f => f.via === 'relay'));
    assert.deepEqual(filesUnder(join, '/jk1/' + join.jkMods.restart())['big.dat'],
                     Buffer.from(host.FS.files.get('/jk1/jkmods/0-SBX/big.dat')));
});

test('a joiner with the same mod in a folder of its own uses that, and nothing is sent', async () => {
    const host = await sbxHost();
    const join = await makeTab({
        files: { ...retailInstall, ...install('jkmods/1-sbx', sbxFiles), ...install('jkmods/0-Other', { 'x.cog': randomBytes(50) }) },
        folders: [{ name: 'Other', path: 'jkmods/0-Other' }, { name: 'sbx', path: 'jkmods/1-sbx' }],
    });
    const link = connect(host, join);
    host.jkMods.host();
    join.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(join), STATE_RESTART);
    assert.equal(join.jkMods.restart(), 'jkmods/1-sbx');
    assert.equal(link.log.length, 0);
});

test('under another folder name, the host\'s size picks it out, and only that is asked', async () => {
    const host = await sbxHost();
    const other = { 'x.cog': randomBytes(50) };
    const join = await makeTab({
        files: { ...retailInstall, ...install('jkmods/0-Other', other), ...install('jkmods/1-Saber_Battle_X', sbxFiles) },
        folders: [{ name: 'Other', path: 'jkmods/0-Other' }, { name: 'Saber Battle X', path: 'jkmods/1-Saber_Battle_X' }],
    });
    const link = connect(host, join);
    host.jkMods.host();
    join.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(join), STATE_RESTART);
    assert.equal(join.jkMods.restart(), 'jkmods/1-Saber_Battle_X');
    assert.deepEqual(link.log.map(f => f.bytes[12]), [1, 2]); // INFO_REQ, INFO
});

test('players already on the same mod, or both on none, just join', async () => {
    const host = await sbxHost();
    const join = await sbxHost();
    const link = connect(host, join);
    join.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(join), STATE_READY);

    const v1 = await makeTab({ files: { ...retailInstall } });
    const v2 = await makeTab({ files: { ...retailInstall } });
    connect(v1, v2);
    v2.jkMods.fetch(tailOf(v1));
    assert.equal(await waitMod(v2), STATE_READY);
    assert.equal(link.log.length, 0);
});

test('a joiner on a mod restarts without it for a host on none', async () => {
    const host = await makeTab({ files: { ...retailInstall } });
    const join = await sbxHost();
    connect(host, join);
    join.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(join), STATE_RESTART);
    assert.equal(join.jkMods.restart(), '');
    assert.equal(join.jkMods.current(), null);
    assert.equal(tailOf(join), '');
});

test('a mod saved from an earlier game is used again without asking the host', async () => {
    const idb = makeIDB();
    const host = await sbxHost();
    const first = await makeTab({ files: { ...retailInstall }, idb });
    connect(host, first);
    host.jkMods.host();
    await settle();
    first.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(first), STATE_RESTART);
    const path = first.jkMods.restart();
    await settle(100); // saving happens after the restart is ready

    // Next visit: a new tab in the same browser.
    const again = await makeTab({ files: { ...retailInstall }, idb });
    const link = connect(host, again);
    again.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(again), STATE_RESTART);
    assert.equal(again.jkMods.restart(), path);
    assert.equal(link.log.length, 0);
    assert.deepEqual(filesUnder(again, '/jk1/' + path)['cog/sabers.cog'], Buffer.from(sabers));

    // Or picked in the folder prompt: started with -path netmods/<id>.
    const picked = await makeTab({ files: { ...retailInstall }, idb, args: ['-path', path] });
    assert.equal(picked.jkMods.current().fp, host.jkMods.current().fp);
    assert.equal(picked.jkMods.current().name, 'SBX');
    assert.deepEqual(filesUnder(picked, '/jk1/' + path)['cog/sabers.cog'], Buffer.from(sabers));
});

test('a joiner missing the original files the mod needs is told so, and nothing is set up', async () => {
    const host = await sbxHost();
    const join = await makeTab({ files: { '/jk1/episode/JK1MP.gob': retailEp } });
    connect(host, join);
    host.jkMods.host();
    await settle();
    join.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(join), STATE_FAILED);
    assert.ok(![...join.FS.files.keys()].some(p => p.includes('netmods')));
});

test('a MotS mod isn\'t loaded into Jedi Knight', async () => {
    const host = await makeTab({
        files: install('jkmods/0-M', { 'm.goo': modGob }, '/mots'), cwd: '/mots',
        args: ['-motsCompat', '-path', 'jkmods/0-M'], folders: [{ name: 'M', path: 'jkmods/0-M' }],
    });
    const join = await makeTab({ files: { ...retailInstall } });
    const link = connect(host, join);
    host.jkMods.host();
    await settle();
    join.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(join), STATE_FAILED);
    assert.equal(framesTo(link.log, 'host', 3).length, 0);
});

test('a host whose mod files changed after it started says so', async () => {
    const host = await sbxHost();
    host.FS.files.set('/jk1/jkmods/0-SBX/cog/sabers.cog', randomBytes(20));
    const join = await makeTab({ files: { ...retailInstall } });
    connect(host, join);
    host.jkMods.host();
    await settle();
    join.jkMods.fetch(tailOf(host));
    assert.equal(await waitMod(join), STATE_FAILED);
});

test('a lobby tail this page can\'t read stops the join instead of guessing', async () => {
    const join = await makeTab({ files: { ...retailInstall } });
    join.jkMods.fetch('4a4b4d4402');
    assert.equal(await waitMod(join), STATE_FAILED);
});

test('a map and a mod from the same host don\'t mix', async () => {
    const customMap = makeGob([{ name: 'episode.jk', bytes: randomBytes(900) }, { name: 'jkl\\x.jkl', bytes: randomBytes(70000) }]);
    const host = await sbxHost({ '/jk1/episode/ctf_x.gob': customMap });
    const join = await makeTab({ files: { ...retailInstall } });
    connect(host, join);
    host.jkMaps.host('ctf_x', 'gob');
    host.jkMods.host();
    await settle();
    join.jkMods.fetch(tailOf(host));
    join.jkMaps.fetch('ctf_x', 'gob');
    assert.equal(await waitMod(join), STATE_RESTART);
    while (join.jkMaps.state() === STATE_BUSY)
        await settle(5);
    assert.equal(join.jkMaps.state(), STATE_READY);
    assert.deepEqual(Buffer.from(join.FS.files.get('/jk1/episode/ctf_x.gob')), Buffer.from(customMap));
    assert.deepEqual(filesUnder(join, '/jk1/' + join.jkMods.restart())['cog/sabers.cog'], Buffer.from(sabers));
});

test('the join screen says what the selected game\'s mod means for this player', async () => {
    const host = await sbxHost();
    const vanilla = await makeTab({ files: { ...retailInstall } });
    const t = tailOf(host);
    assert.equal(vanilla.jkMods.describe(t), 'Mod: SBX (the game restarts to load it)');
    assert.equal(host.jkMods.describe(t), 'Mod: SBX');
    assert.equal(host.jkMods.describe(''), 'No mod (the game restarts without SBX)');
    assert.equal(vanilla.jkMods.describe(''), '');
    assert.equal(vanilla.jkMods.describe('00'), 'Mod: unknown');
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
