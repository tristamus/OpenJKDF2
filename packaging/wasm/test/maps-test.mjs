// Tests for packaging/wasm/maps.js, the map downloads for joiners, in Node
// (22 or newer, for WebCrypto): node packaging/wasm/test/maps-test.mjs
//
// Every GOB here is made up by the test, so no game files are needed. Two
// "tabs" run maps.js against in-memory file systems and a fake link between
// them that can go through the "relay" or "directly", and drop frames.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { makeGob, sha, contains, makeFS, makeClock } from './helpers.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, '..', 'maps.js'), 'utf8');
// maps.js as the live build (cloudflare-89, commit 503d826) has it: joiners
// with it, which can't be updated, have to keep getting maps from new hosts.
const oldSource = readFileSync(join(here, 'fixtures', 'maps-503d826.js'), 'utf8');

function loadMaps(Module) {
    new Function('Module', source)(Module);
    return Module.jkMaps;
}

// ---- Two tabs and the link between them.

// lanes: the ways each tab says it has to the other (net.mapLanes), such as
// the map channel and the extra links for transfers; direct alone when not
// given. rewrite(frame) can change a frame on its way. cwd: the game folder,
// /mots for Mysteries of the Sith.
function makeGame({ hostFiles, joinFiles, direct = true, drop = () => false, lanes = null, rewrite = null, cwd }) {
    const log = [];
    const link = { direct, drop, log, lanes };
    const host = { FS: makeFS(hostFiles, cwd) };
    const join = { FS: makeFS(joinFiles, cwd) };
    const deliver = (to, from, bytes, via) => {
        let copy = bytes.slice(0);
        if (rewrite)
            copy = rewrite({ to: to === host ? 'host' : 'join', via, bytes: copy }) || copy;
        log.push({ to: to === host ? 'host' : 'join', via, bytes: new Uint8Array(copy) });
        if (via === 'relay')
            assert.ok(copy.byteLength <= 64 * 1024, 'relay frame over the lobby cap: ' + copy.byteLength);
        if (link.drop(log[log.length - 1]))
            return;
        setImmediate(() => to.jkMaps.onFrame(from, copy, via));
    };
    const way = via => {
        if (via === 'relay' || !link.direct)
            return 'relay';
        return link.lanes && link.lanes.includes(via) ? via : 'direct';
    };
    const net = (other, otherId) => ({
        mapDirect: id => id === otherId && link.direct,
        mapSend(id, bytes, via) {
            const used = way(via);
            deliver(other(), otherId === 1 ? 2 : 1, bytes, used);
            return used;
        },
    });
    host.jkNet = net(() => join, 2);
    join.jkNet = net(() => host, 1);
    if (lanes) {
        join.jkNet.mapLanes = () => (link.direct ? link.lanes : []);
        join.jkNet.bulk = [];
        join.jkNet.mapBulk = (id, on) => join.jkNet.bulk.push(on);
    }
    loadMaps(host);
    loadMaps(join);
    return { host, join, link, log };
}

async function waitState(tab, ms = 20000) {
    const until = Date.now() + ms;
    while (tab.jkMaps.state() === 0) {
        if (Date.now() > until)
            throw new Error('still busy');
        await new Promise(r => setTimeout(r, 5));
    }
    return tab.jkMaps.state();
}

async function hostReady(tab) {
    // The host prepares its map in the background; give it a moment.
    await new Promise(r => setTimeout(r, 50));
}

// ---- Fixtures: a "retail" install and a custom map that bundles some of its
// files.

const retailA = randomBytes(3000), retailB = randomBytes(5000), retailC = randomBytes(700);
const retailRes = makeGob([
    { name: 'mat\\a.mat', bytes: retailA },
    { name: 'sound\\b.wav', bytes: retailB },
    { name: 'cog\\c.cog', bytes: retailC },
]);
const retailEp = makeGob([{ name: 'jkl\\01narshadda.jkl', bytes: randomBytes(4000) }]);
const custom1 = randomBytes(200000), custom2 = randomBytes(1200);
const modifiedC = Buffer.from(retailC);
modifiedC[10] ^= 0xFF;
const customMap = makeGob([
    { name: 'episode.jk', bytes: custom2 },
    { name: 'mat\\a.mat', bytes: retailA },          // a copy of a retail file
    { name: 'mat\\renamed.mat', bytes: retailB },    // a retail file under a new name
    { name: 'cog\\c.cog', bytes: modifiedC },        // an edited retail file: the modder's own
    { name: 'jkl\\ctf_test.jkl', bytes: custom1 },
]);
const retailInstall = {
    '/jk1/resource/Res2.gob': retailRes,
    '/jk1/resource/Res1hi.gob': makeGob([{ name: 'x\\y', bytes: randomBytes(10) }]),
    '/jk1/episode/JK1MP.gob': retailEp,
};

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

// ---- The GOB tools.

test('names from other players are checked before they become file names', () => {
    const { lib } = loadMaps({ FS: makeFS({}) });
    for (const ok of ['ctf_hoth', 'My Map 2', 'JK1MP', 'a.b', 'x'.repeat(31)])
        assert.ok(lib.validEpisodeName(ok), ok);
    for (const bad of ['', '../x', 'a/b', 'a\\b', '.hidden', 'a..b', 'x'.repeat(32), 'a:b', 'a\u0001'])
        assert.ok(!lib.validEpisodeName(bad), bad);
    assert.ok(lib.isRetailEpisode('jk1mp') && lib.isRetailEpisode('JKM_SABER') && !lib.isRetailEpisode('ctf_hoth'));
});

test('a GOB file list reads back', () => {
    const { lib } = loadMaps({ FS: makeFS({}) });
    const dir = lib.parseGob(o => customMap, 0) ; // too small: not a GOB
    assert.equal(dir, null);
    const parsed = lib.parseGob((o, l) => customMap.subarray(o, o + l), customMap.length);
    assert.deepEqual(parsed.entries.map(e => e.name),
        ['episode.jk', 'mat\\a.mat', 'mat\\renamed.mat', 'cog\\c.cog', 'jkl\\ctf_test.jkl']);
    assert.equal(lib.parseGob((o, l) => randomBytes(l), 5000), null);
});

function sources(lib, gobs) {
    return gobs.map(g => ({ read: (o, l) => g.subarray(o, o + l), entries: lib.parseGob((o, l) => g.subarray(o, o + l), g.length).entries }));
}

test('retail copies are left out of what is sent, and rebuilt from the joiner\'s own copy', async () => {
    const { lib } = loadMaps({ FS: makeFS({}) });
    const plan = await lib.planShare(customMap, sources(lib, [retailRes, retailEp]));
    assert.ok(plan.ok);
    assert.equal(plan.refs, 2);
    const stream = plan.stream.bytes();
    assert.ok(!contains(stream, retailA) && !contains(stream, retailB), 'retail bytes in the stream');
    assert.ok(contains(stream, custom1) && contains(stream, custom2) && contains(stream, modifiedC));
    assert.equal(stream.length, plan.refsBytes + customMap.length - retailA.length - retailB.length);

    const info = { size: customMap.length, sha256: sha(customMap), streamSize: stream.length, refsBytes: plan.refsBytes };
    const out = await lib.rebuild(info, stream, sources(lib, [retailRes]));
    assert.ok(out.ok, out.reason);
    assert.deepEqual(Buffer.from(out.bytes), Buffer.from(customMap));

    const missing = await lib.rebuild(info, stream, sources(lib, [retailEp]));
    assert.equal(missing.reason, 'missing-retail');

    const tampered = stream.slice();
    tampered[tampered.length - 1] ^= 1;
    assert.equal((await lib.rebuild(info, tampered, sources(lib, [retailRes]))).reason, 'mismatch');
    assert.equal((await lib.rebuild(info, stream.subarray(1), sources(lib, [retailRes]))).reason, 'corrupt');
});

test('a GOB made only of retail files is not sent', async () => {
    const { lib } = loadMaps({ FS: makeFS({}) });
    const renamed = makeGob([{ name: 'mat\\a.mat', bytes: retailA }, { name: 'q', bytes: retailB }]);
    assert.equal((await lib.planShare(renamed, sources(lib, [retailRes]))).reason, 'retail-only');
    assert.equal((await lib.planShare(retailRes, sources(lib, [retailRes]))).reason, 'retail-only');
    assert.equal((await lib.planShare(randomBytes(500), sources(lib, [retailRes]))).reason, 'not-gob');
});

test('entries that share bytes with a retail copy still rebuild exactly', async () => {
    const { lib } = loadMaps({ FS: makeFS({}) });
    const wrapper = Buffer.concat([randomBytes(100), retailA, randomBytes(100)]);
    const gob = makeGob([
        { name: 'big', bytes: wrapper },
        { name: 'inner', bytes: retailA, sameAs: 0, offsetIn: 100 },
    ], { overlap: true });
    const plan = await lib.planShare(gob, sources(lib, [retailRes]));
    assert.ok(plan.ok);
    assert.equal(plan.refs, 1);
    const stream = plan.stream.bytes();
    assert.ok(!contains(stream, retailA));
    const info = { size: gob.length, sha256: sha(gob), streamSize: stream.length, refsBytes: plan.refsBytes };
    const out = await lib.rebuild(info, stream, sources(lib, [retailRes]));
    assert.ok(out.ok, out.reason);
    assert.deepEqual(Buffer.from(out.bytes), Buffer.from(gob));
});


// ---- A joiner whose game has other versions of original files, as the MotS
// Remastered pack's JKMsndLO.goo has of most of the original sounds.

const stockB = randomBytes(4000), stockD = randomBytes(2500);
const changedB = randomBytes(6000);
const stockSounds = makeGob([{ name: 'sound\\b.wav', bytes: stockB }, { name: 'sound\\d.wav', bytes: stockD }]);
const remasteredSounds = makeGob([
    { name: 'sound\\b.wav', bytes: changedB },          // another version
    { name: 'sound\\d.wav', bytes: stockD },            // the same
    { name: 'sound\\new.wav', bytes: randomBytes(900) },
]);
const motsRes = makeGob([{ name: 'cog\\e.cog', bytes: randomBytes(1500) }]);
const shipJkl = randomBytes(150000), shipJk = randomBytes(800);
const sith5 = makeGob([
    { name: 'episode.jk', bytes: shipJk },
    { name: 'sound\\b.wav', bytes: stockB },            // copies of original files
    { name: 'sound\\d.wav', bytes: stockD },
    { name: 'jkl\\ship.jkl', bytes: shipJkl },
]);

// Sources as retailSources has them: always for the resource/ GOBs.
function motsSources(lib, gobs, always = true) {
    return sources(lib, gobs).map(s => ({ ...s, always }));
}

function entriesOf(lib, gob) {
    return lib.parseGob((o, l) => gob.subarray(o, o + l), gob.length).entries
        .map(e => ({ name: e.name, bytes: Buffer.from(gob.subarray(e.offset, e.offset + e.size)) }));
}

async function sith5Plan(lib) {
    const plan = await lib.planShare(sith5, motsSources(lib, [stockSounds, motsRes]));
    assert.ok(plan.ok);
    assert.equal(plan.refs, 2);
    const stream = plan.stream.bytes();
    const info = { size: sith5.length, sha256: sha(sith5), streamSize: stream.length, refsBytes: plan.refsBytes,
                   streamSha256: sha(stream) };
    return { stream, info };
}

test('a joiner with other versions of original files a map bundles plays it with its own', async () => {
    const { lib } = loadMaps({ FS: makeFS({}, '/mots') });
    const { stream, info } = await sith5Plan(lib);
    const theirs = motsSources(lib, [remasteredSounds, motsRes]);

    const out = await lib.rebuild(info, stream, theirs, true);
    assert.ok(out.ok, out.reason);
    assert.equal(out.own, 1);
    // sound\b.wav is left to the joiner's own; sound\d.wav, the same in both,
    // is filled in as before.
    assert.deepEqual(entriesOf(lib, out.bytes), [
        { name: 'episode.jk', bytes: Buffer.from(shipJk) },
        { name: 'sound\\d.wav', bytes: Buffer.from(stockD) },
        { name: 'jkl\\ship.jkl', bytes: Buffer.from(shipJkl) },
    ]);

    // Blocks put back together the same way give the same GOB.
    const rb = lib.rebuilder(info, theirs, true);
    for (let at = stream.length; at > 0; at -= 7000)
        rb.put(Math.max(0, at - 7000), stream.subarray(Math.max(0, at - 7000), at));
    const out2 = await rb.finish();
    assert.ok(out2.ok, out2.reason);
    assert.deepEqual(Buffer.from(out2.bytes), Buffer.from(out.bytes));

    // A mod's transfer doesn't do this.
    assert.equal((await lib.rebuild(info, stream, theirs)).reason, 'missing-retail');
    // A host from before this doesn't say streamSha256.
    const old = { ...info };
    delete old.streamSha256;
    assert.equal((await lib.rebuild(old, stream, theirs, true)).reason, 'retail-changed');
    // What the host sent is checked.
    const tampered = stream.slice();
    tampered[tampered.length - 1] ^= 1;
    assert.equal((await lib.rebuild(info, tampered, theirs, true)).reason, 'mismatch');
    const table = stream.slice();
    table[8] ^= 1; // a reference's SHA-256
    assert.equal((await lib.rebuild(info, table, theirs, true)).reason, 'mismatch');
});

test('only a file the joiner has by the same name, in a GOB always loaded, is left to its own', async () => {
    const { lib } = loadMaps({ FS: makeFS({}, '/mots') });
    const { stream, info } = await sith5Plan(lib);
    // Its other version only in an episode GOB, which the engine doesn't load
    // for this map.
    assert.equal((await lib.rebuild(info, stream, [...motsSources(lib, [motsRes]),
        ...motsSources(lib, [remasteredSounds], false)], true)).reason, 'missing-retail');
    // An original file under a name of the map's own: the engine would find
    // nothing by that name.
    const renamed = makeGob([{ name: 'episode.jk', bytes: shipJk }, { name: 'sound\\mine.wav', bytes: stockB },
                             { name: 'jkl\\ship.jkl', bytes: shipJkl }]);
    const plan = await lib.planShare(renamed, motsSources(lib, [stockSounds]));
    const s = plan.stream.bytes();
    const i = { size: renamed.length, sha256: sha(renamed), streamSize: s.length, refsBytes: plan.refsBytes,
                streamSha256: sha(s) };
    assert.equal((await lib.rebuild(i, s, motsSources(lib, [remasteredSounds]), true)).reason, 'missing-retail');
    // A custom file around an original one can't be put together.
    const wrapper = Buffer.concat([randomBytes(100), stockB, randomBytes(100)]);
    const nested = makeGob([
        { name: 'big', bytes: wrapper },
        { name: 'sound\\b.wav', bytes: stockB, sameAs: 0, offsetIn: 100 },
    ], { overlap: true });
    const p2 = await lib.planShare(nested, motsSources(lib, [stockSounds]));
    const s2 = p2.stream.bytes();
    const i2 = { size: nested.length, sha256: sha(nested), streamSize: s2.length, refsBytes: p2.refsBytes,
                 streamSha256: sha(s2) };
    assert.equal((await lib.rebuild(i2, s2, motsSources(lib, [remasteredSounds]), true)).reason, 'missing-retail');
});

test('a MotS joiner with the Remastered sounds gets a map that bundles original sounds', async () => {
    const stock = { '/mots/resource/JKMsndLO.goo': stockSounds, '/mots/resource/Jkmres.goo': motsRes };
    const remastered = { '/mots/resource/JKMsndLO.goo': remasteredSounds, '/mots/resource/Jkmres.goo': motsRes };
    const g = makeGame({
        hostFiles: { ...stock, '/mots/episode/sith5.goo': sith5 },
        joinFiles: { ...remastered },
        cwd: '/mots',
    });
    g.host.jkMaps.host('sith5', 'goo');
    await hostReady(g.host);
    g.join.jkMaps.fetch('sith5', 'goo');
    assert.equal(await waitState(g.join), 1);
    const got = g.join.FS.files.get('/mots/episode/sith5.goo');
    assert.deepEqual(entriesOf(g.join.jkMaps.lib, got).map(e => e.name), ['episode.jk', 'sound\\d.wav', 'jkl\\ship.jkl']);
    for (const f of g.log.filter(f => f.to === 'join'))
        assert.ok(!contains(f.bytes, stockB) && !contains(f.bytes, stockD), 'original bytes sent');

    // A joiner with the original sounds still gets the host's file exactly.
    const g2 = makeGame({ hostFiles: { ...stock, '/mots/episode/sith5.goo': sith5 }, joinFiles: { ...stock }, cwd: '/mots' });
    g2.host.jkMaps.host('sith5', 'goo');
    await hostReady(g2.host);
    g2.join.jkMaps.fetch('sith5', 'goo');
    assert.equal(await waitState(g2.join), 1);
    assert.deepEqual(Buffer.from(g2.join.FS.files.get('/mots/episode/sith5.goo')), Buffer.from(sith5));
});

// What a host from before streamSha256 sends: its map's INFO without it.
function asOldHost(f) {
    if (f.to !== 'join' || new Uint8Array(f.bytes)[12] !== 2)
        return null;
    const info = JSON.parse(new TextDecoder().decode(new Uint8Array(f.bytes, 16)));
    assert.match(info.streamSha256, /^[0-9a-f]{64}$/);
    delete info.streamSha256;
    const json = new TextEncoder().encode(JSON.stringify(info));
    const out = new Uint8Array(16 + json.length);
    out.set(new Uint8Array(f.bytes, 0, 16));
    out.set(json, 16);
    return out.buffer;
}

test('a host from before this tells a Remastered joiner to have it reload', async () => {
    const g = makeGame({
        hostFiles: { '/mots/resource/JKMsndLO.goo': stockSounds, '/mots/episode/sith5.goo': sith5 },
        joinFiles: { '/mots/resource/JKMsndLO.goo': remasteredSounds },
        cwd: '/mots',
        rewrite: asOldHost,
    });
    const said = [];
    const log = console.log;
    console.log = (...a) => { said.push(a.join(' ')); log(...a); };
    try {
        g.host.jkMaps.host('sith5', 'goo');
        await hostReady(g.host);
        g.join.jkMaps.fetch('sith5', 'goo');
        assert.equal(await waitState(g.join), 2);
    } finally {
        console.log = log;
    }
    assert.ok(said.some(l => /map sith5\.goo not installed: retail-changed/.test(l)), said.join('\n'));
    assert.ok(!g.join.FS.files.has('/mots/episode/sith5.goo'));
});

// The MotS Remastered pack's installer keeps the original JKMsndLO.goo in
// resource/Backup Sounds/, which the page reads when it isn't the one loaded.
const backupSounds = '/mots/resource/Backup Sounds/JKMSNDLO.GOO';

test('original sounds kept in Backup Sounds rebuild a map exactly, from any host', async () => {
    const stock = { '/mots/resource/JKMsndLO.goo': stockSounds, '/mots/resource/Jkmres.goo': motsRes };
    const remastered = { '/mots/resource/JKMsndLO.goo': remasteredSounds, '/mots/resource/Jkmres.goo': motsRes,
                         [backupSounds]: stockSounds };
    for (const rewrite of [null, asOldHost]) {
        const g = makeGame({ hostFiles: { ...stock, '/mots/episode/sith5.goo': sith5 }, joinFiles: { ...remastered },
                             cwd: '/mots', rewrite });
        g.host.jkMaps.host('sith5', 'goo');
        await hostReady(g.host);
        g.join.jkMaps.fetch('sith5', 'goo');
        assert.equal(await waitState(g.join), 1);
        assert.deepEqual(Buffer.from(g.join.FS.files.get('/mots/episode/sith5.goo')), Buffer.from(sith5));
    }
});

test('a Remastered host sends the original sounds a map carries as references', async () => {
    const stock = { '/mots/resource/JKMsndLO.goo': stockSounds, '/mots/resource/Jkmres.goo': motsRes };
    const remastered = { '/mots/resource/JKMsndLO.goo': remasteredSounds, '/mots/resource/Jkmres.goo': motsRes };
    // How many files the host's INFO says it left out as references.
    const refsSent = log => {
        const f = log.find(f => f.to === 'join' && f.bytes[12] === 2);
        return (JSON.parse(new TextDecoder().decode(f.bytes.subarray(16))).refsBytes - 4) / 40;
    };
    const g = makeGame({ hostFiles: { ...remastered, [backupSounds]: stockSounds, '/mots/episode/sith5.goo': sith5 },
                         joinFiles: { ...stock }, cwd: '/mots' });
    g.host.jkMaps.host('sith5', 'goo');
    await hostReady(g.host);
    g.join.jkMaps.fetch('sith5', 'goo');
    assert.equal(await waitState(g.join), 1);
    assert.deepEqual(Buffer.from(g.join.FS.files.get('/mots/episode/sith5.goo')), Buffer.from(sith5));
    assert.equal(refsSent(g.log), 2);

    // Without the spare copy the original sound\b.wav went in the map's
    // bytes: only sound\d.wav, the same in both, was a reference.
    const g2 = makeGame({ hostFiles: { ...remastered, '/mots/episode/sith5.goo': sith5 }, joinFiles: { ...stock },
                          cwd: '/mots' });
    g2.host.jkMaps.host('sith5', 'goo');
    await hostReady(g2.host);
    g2.join.jkMaps.fetch('sith5', 'goo');
    assert.equal(await waitState(g2.join), 1);
    assert.equal(refsSent(g2.log), 1);
});

test('a spare copy is only looked in by content: the engine never loads it', async () => {
    const { lib, core } = loadMaps({ FS: makeFS({ '/mots/resource/Jkmres.goo': motsRes, [backupSounds]: remasteredSounds,
                                                  '/mots/resource/Backup Sounds/notes.goo': stockSounds,
                                                  '/mots/resource/Old/Deeper/JKMsndLO.goo': stockSounds }, '/mots') });
    const found = core.retailSources('/mots', 'goo');
    assert.deepEqual(found.map(s => [s.always, !!s.spare]), [[true, false], [false, true]]);
    // Its other version of sound\b.wav isn't the joiner's own.
    const { stream, info } = await sith5Plan(lib);
    assert.equal((await lib.rebuild(info, stream, found, true)).reason, 'missing-retail');
});

// ---- Host and joiner tabs.

function framesTo(log, who, type) {
    return log.filter(f => f.to === who && f.bytes[12] === type);
}

test('a joiner without the map gets it straight from the host', async () => {
    const g = makeGame({
        hostFiles: { ...retailInstall, '/jk1/episode/CTF_Test.gob': customMap },
        joinFiles: { ...retailInstall },
    });
    g.host.jkMaps.host('CTF_Test', 'gob');
    await hostReady(g.host);
    g.join.jkMaps.fetch('CTF_Test', 'gob');
    assert.equal(await waitState(g.join), 1);
    assert.deepEqual(Buffer.from(g.join.FS.files.get('/jk1/episode/CTF_Test.gob')), Buffer.from(customMap));
    for (const f of g.log.filter(f => f.to === 'join')) {
        assert.ok(!contains(f.bytes, retailA) && !contains(f.bytes, retailB), 'retail bytes sent');
        assert.equal(f.via, 'direct');
    }
    // Random bytes don't get smaller, so the one block goes as it is, in
    // parts of 16 KiB whatever size was asked for.
    const stream = customMap.length - 8000 + 4 + 80;
    assert.equal(framesTo(g.log, 'join', 4).length, 0);
    assert.equal(framesTo(g.log, 'join', 6).length, Math.ceil(stream / 16384));
    assert.equal(framesTo(g.log, 'host', 5).length, 1);
});

test('through the relay, frames stay under its cap', async () => {
    const big = makeGob([{ name: 'episode.jk', bytes: randomBytes(1000) }, { name: 'big.jkl', bytes: randomBytes(3000000) },
                         { name: 'mat\\a.mat', bytes: retailA }]);
    const g = makeGame({
        hostFiles: { ...retailInstall, '/jk1/episode/ctf_big.gob': big },
        joinFiles: { ...retailInstall },
        direct: false,
    });
    // WebRTC off in the joiner's browser: no waiting for a direct link.
    g.join.jkNet.rtcEnabled = () => false;
    g.host.jkMaps.host('ctf_big', 'gob');
    await hostReady(g.host);
    const started = Date.now();
    g.join.jkMaps.fetch('ctf_big', 'gob');
    assert.equal(await waitState(g.join), 1);
    assert.ok(Date.now() - started < 2500, 'waited for a direct link: ' + (Date.now() - started) + ' ms');
    assert.deepEqual(Buffer.from(g.join.FS.files.get('/jk1/episode/ctf_big.gob')), Buffer.from(big));
    assert.ok(g.log.every(f => f.via === 'relay'));
    const stream = big.length - retailA.length + 44;
    const blocks = Math.ceil(stream / 1048576);
    let parts = 0;
    for (let b = 0; b < blocks; b++)
        parts += Math.ceil(Math.min(1048576, stream - b * 1048576) / 16384);
    assert.equal(framesTo(g.log, 'join', 6).length, parts);
    const requests = framesTo(g.log, 'host', 5).length + framesTo(g.log, 'host', 1).length;
    console.log(`    ${big.length} byte map over the relay: ${parts} block parts, ${requests} requests`);
    // One request a block, and the info request.
    assert.equal(requests, blocks + 1);
});

test('a map that squeezes goes squeezed, and comes out the same', async () => {
    const text = Buffer.from('thing 1 template=walkplayer 0.0 0.0 0.0 pos=(1.0/2.0/3.0)\n'.repeat(60000));
    const map = makeGob([{ name: 'episode.jk', bytes: randomBytes(1000) }, { name: 'jkl\\ctf_text.jkl', bytes: text },
                         { name: 'mat\\a.mat', bytes: retailA }]);
    const g = makeGame({
        hostFiles: { ...retailInstall, '/jk1/episode/ctf_text.gob': map },
        joinFiles: { ...retailInstall },
    });
    g.host.jkMaps.host('ctf_text', 'gob');
    await hostReady(g.host);
    g.join.jkMaps.fetch('ctf_text', 'gob');
    assert.equal(await waitState(g.join), 1);
    assert.deepEqual(Buffer.from(g.join.FS.files.get('/jk1/episode/ctf_text.gob')), Buffer.from(map));
    const sent = framesTo(g.log, 'join', 6).reduce((n, f) => n + f.bytes.length - 40, 0);
    console.log(`    ${map.length} byte map sent as ${sent} bytes`);
    assert.ok(sent < map.length / 10, sent + ' bytes sent');
    assert.ok(framesTo(g.log, 'join', 6).every(f => f.bytes[36] === 1 || f.bytes[36] === 0));
});

test('a host from before blocks gets asked for chunks, and an older joiner gets chunks', async () => {
    // Takes z out of the host's info, as a host or joiner from before blocks
    // would have it.
    const g = makeGame({
        hostFiles: { ...retailInstall, '/jk1/episode/ctf_test.gob': customMap },
        joinFiles: { ...retailInstall },
        rewrite: f => {
            if (f.to !== 'join' || new Uint8Array(f.bytes)[12] !== 2)
                return null;
            const info = JSON.parse(new TextDecoder().decode(new Uint8Array(f.bytes, 16)));
            assert.equal(info.z, 1048576);
            delete info.z;
            const json = new TextEncoder().encode(JSON.stringify(info));
            const out = new Uint8Array(16 + json.length);
            out.set(new Uint8Array(f.bytes, 0, 16));
            out.set(json, 16);
            return out.buffer;
        },
    });
    g.host.jkMaps.host('ctf_test', 'gob');
    await hostReady(g.host);
    g.join.jkMaps.fetch('ctf_test', 'gob');
    assert.equal(await waitState(g.join), 1);
    assert.deepEqual(Buffer.from(g.join.FS.files.get('/jk1/episode/ctf_test.gob')), Buffer.from(customMap));
    assert.equal(framesTo(g.log, 'host', 5).length, 0);
    assert.equal(framesTo(g.log, 'join', 4).length, Math.ceil((customMap.length - 8000 + 4 + 80) / 16384));
});

test('blocks go over every lane to the host, and a lane that closes hands its blocks on', async () => {
    const big = makeGob([{ name: 'episode.jk', bytes: randomBytes(1000) }, { name: 'big.jkl', bytes: randomBytes(6500000) }]);
    let closed = false;
    const g = makeGame({
        hostFiles: { ...retailInstall, '/jk1/episode/ctf_lanes.gob': big },
        joinFiles: { ...retailInstall },
        lanes: ['direct', 'direct1', 'direct2', 'direct3'],
        drop: f => closed && f.via === 'direct2',
    });
    g.host.jkMaps.host('ctf_lanes', 'gob');
    await hostReady(g.host);
    g.join.jkMaps.fetch('ctf_lanes', 'gob');
    // direct2 closes once a few blocks are in.
    const watch = setInterval(() => {
        if (framesTo(g.log, 'join', 6).length > 20 && !closed) {
            closed = true;
            g.link.lanes = ['direct', 'direct1', 'direct3'];
        }
    }, 1);
    assert.equal(await waitState(g.join), 1);
    clearInterval(watch);
    assert.ok(closed);
    assert.deepEqual(Buffer.from(g.join.FS.files.get('/jk1/episode/ctf_lanes.gob')), Buffer.from(big));
    const asked = new Set(g.log.filter(f => f.to === 'host' && f.bytes[12] === 5).map(f => f.via));
    assert.deepEqual([...asked].sort(), ['direct', 'direct1', 'direct2', 'direct3']);
    // Each part comes back the way its block was asked for.
    for (const f of framesTo(g.log, 'join', 6))
        assert.ok(['direct', 'direct1', 'direct2', 'direct3'].includes(f.via));
    // The extra links were asked for, then let go.
    assert.deepEqual(g.join.jkNet.bulk, [true, false]);
});

test('blocks put back together in any order make the same GOB', async () => {
    const { lib } = loadMaps({ FS: makeFS({}) });
    const many = [];
    for (let i = 0; i < 3000; i++)
        many.push({ name: 'mat\\r' + i + '.mat', bytes: i % 3 ? randomBytes(700) : retailA });
    many.push({ name: 'big', bytes: randomBytes(2500000) });
    const gob = makeGob(many);
    const plan = await lib.planShare(gob, sources(lib, [retailRes]));
    assert.ok(plan.ok);
    const stream = plan.stream.bytes();
    // A reference table of 1000 references: over 1 block's worth with these
    // small blocks, so the first ones wait for it.
    const info = { size: gob.length, sha256: sha(gob), streamSize: stream.length, refsBytes: plan.refsBytes };
    for (const size of [7, 4096, 65536, 1048576]) {
        const offsets = [];
        for (let at = 0; at < stream.length; at += size)
            offsets.push(at);
        offsets.sort(() => Math.random() - 0.5);
        const rb = lib.rebuilder(info, sources(lib, [retailRes]));
        for (const at of offsets)
            rb.put(at, stream.slice(at, at + size));
        const out = await rb.finish();
        assert.ok(out.ok, out.reason);
        assert.deepEqual(Buffer.from(out.bytes), Buffer.from(gob));
    }
    // One piece short.
    const rb = lib.rebuilder(info, sources(lib, [retailRes]));
    rb.put(0, stream.slice(0, stream.length - 10));
    assert.equal((await rb.finish()).reason, 'corrupt');
});

test('a joiner with the same version downloads nothing', async () => {
    const g = makeGame({
        hostFiles: { ...retailInstall, '/jk1/episode/ctf_test.gob': customMap },
        joinFiles: { ...retailInstall, '/jk1/episode/CTF_TEST.GOB': customMap },
    });
    g.host.jkMaps.host('ctf_test', 'gob');
    await hostReady(g.host);
    g.join.jkMaps.fetch('ctf_test', 'gob');
    assert.equal(await waitState(g.join), 1);
    assert.equal(framesTo(g.log, 'host', 3).length, 0);
    assert.ok(g.join.FS.files.has('/jk1/episode/CTF_TEST.GOB'));
});

test('a joiner with another version gets the host\'s, in place of theirs', async () => {
    const older = makeGob([{ name: 'episode.jk', bytes: randomBytes(1200) }, { name: 'jkl\\ctf_test.jkl', bytes: randomBytes(9000) }]);
    const g = makeGame({
        hostFiles: { ...retailInstall, '/jk1/episode/ctf_test.gob': customMap },
        joinFiles: { '/jk1/Episode/JK1MP.gob': retailEp, '/jk1/resource/Res2.gob': retailRes,
                     '/jk1/Episode/CTF_Test.gob': older },
    });
    g.host.jkMaps.host('ctf_test', 'gob');
    await hostReady(g.host);
    g.join.jkMaps.fetch('ctf_test', 'gob');
    assert.equal(await waitState(g.join), 1);
    // Into the folder the engine already reads, not a second "episode".
    assert.deepEqual([...g.join.FS.files.keys()].filter(p => /ctf_test/i.test(p)), ['/jk1/Episode/ctf_test.gob']);
    assert.deepEqual(Buffer.from(g.join.FS.files.get('/jk1/Episode/ctf_test.gob')), Buffer.from(customMap));
});

test('original levels are never asked for', async () => {
    const g = makeGame({ hostFiles: { ...retailInstall }, joinFiles: { ...retailInstall } });
    g.host.jkMaps.host('JK1MP', 'gob');
    g.join.jkMaps.fetch('JK1MP', 'gob');
    assert.equal(await waitState(g.join), 1);
    g.join.jkMaps.fetch('JKM_MP', 'gob');
    assert.equal(await waitState(g.join), 2);
    assert.equal(g.log.length, 0);
});

test('a joiner missing the retail files a map needs is told so, and nothing fills the gap', async () => {
    const g = makeGame({
        hostFiles: { ...retailInstall, '/jk1/episode/ctf_test.gob': customMap },
        joinFiles: { '/jk1/episode/JK1MP.gob': retailEp },
    });
    g.host.jkMaps.host('ctf_test', 'gob');
    await hostReady(g.host);
    g.join.jkMaps.fetch('ctf_test', 'gob');
    assert.equal(await waitState(g.join), 2);
    assert.ok(!g.join.FS.files.has('/jk1/episode/ctf_test.gob'));
});

test('a JK folder joining a MotS game is told so', async () => {
    const g = makeGame({
        hostFiles: { '/jk1/episode/ctf_test.goo': customMap },
        joinFiles: { ...retailInstall },
    });
    g.host.jkMaps.host('ctf_test', 'goo');
    await hostReady(g.host);
    g.join.jkMaps.fetch('ctf_test', 'gob');
    assert.equal(await waitState(g.join), 2);
    assert.equal(framesTo(g.log, 'host', 3).length, 0);
});

test('a joiner with the map can still join a host that never answers (an older page)', async () => {
    const g = makeGame({
        hostFiles: { ...retailInstall },
        joinFiles: { ...retailInstall, '/jk1/episode/ctf_test.gob': customMap },
        drop: () => true,
    });
    g.join.jkMaps.fetch('ctf_test', 'gob');
    assert.equal(await waitState(g.join, 30000), 1);
    g.join.FS.unlink('/jk1/episode/ctf_test.gob');
    g.join.jkMaps.fetch('ctf_test', 'gob');
    assert.equal(await waitState(g.join, 30000), 2);
});

test('a host without the file says so', async () => {
    const g = makeGame({ hostFiles: { ...retailInstall }, joinFiles: { ...retailInstall } });
    g.host.jkMaps.host('ctf_gone', 'gob');
    await hostReady(g.host);
    g.join.jkMaps.fetch('ctf_gone', 'gob');
    assert.equal(await waitState(g.join), 2);
});

test('the engine can stop a download (host left, or Cancel)', async () => {
    const g = makeGame({
        hostFiles: { ...retailInstall, '/jk1/episode/ctf_test.gob': customMap },
        joinFiles: { ...retailInstall },
        drop: f => f.to === 'join' && f.bytes[12] === 6,
    });
    g.host.jkMaps.host('ctf_test', 'gob');
    await hostReady(g.host);
    g.join.jkMaps.fetch('ctf_test', 'gob');
    await new Promise(r => setTimeout(r, 200));
    assert.equal(g.join.jkMaps.state(), 0);
    g.join.jkMaps.abort('host-left');
    assert.equal(g.join.jkMaps.state(), 2);
    assert.ok(!g.join.FS.files.has('/jk1/episode/ctf_test.gob'));
});

test('blocks lost when the link changes are asked for again', async () => {
    let dropped = 0;
    const g = makeGame({
        hostFiles: { ...retailInstall, '/jk1/episode/ctf_test.gob': customMap },
        joinFiles: { ...retailInstall },
        drop: f => {
            if (f.to !== 'join' || f.bytes[12] !== 6 || f.via !== 'direct' || dropped >= 3)
                return false;
            dropped++;
            return true;
        },
    });
    g.host.jkMaps.host('ctf_test', 'gob');
    await hostReady(g.host);
    g.join.jkMaps.fetch('ctf_test', 'gob');
    // The direct link goes down partway; the rest comes through the relay.
    setTimeout(() => { g.link.direct = false; }, 20);
    assert.equal(await waitState(g.join), 1);
    assert.equal(dropped, 3);
    assert.deepEqual(Buffer.from(g.join.FS.files.get('/jk1/episode/ctf_test.gob')), Buffer.from(customMap));
    assert.ok(g.log.some(f => f.via === 'relay'));
});

// ---- A host sending paced, on a made-up clock.

// maps.js (src) in a tab whose clock is clock's. Joiners here can't unsqueeze
// (no DecompressionStream), so blocks go as they are and only hashing takes
// real time.
function loadOn(clock, src, Module) {
    new Function('Module', 'setTimeout', 'clearTimeout', 'Date', 'DecompressionStream', src)(
        Module, clock.setTimeout, clock.clearTimeout, clock.Date, undefined);
    return Module.jkMaps;
}

const UNIT = 16384;

// A host and joiners on the lobby's connection, as stdComm_WSRelay.c's net
// has it: 'direct' goes on the map channel while it's up, else through the
// lobby, which never gets a frame over 64 KiB (overCap counts any tried).
// joiners: [{ id, src, direct, lanes }]. link.inMatch, link.congested and
// link.backlog(id, via) are what the host's link says; imperial: the host's
// link is the Imperial Network Protocol's, whose parts keep their size.
function pacedGame(clock, hostFiles, joiners, { imperial = false } = {}) {
    const log = [];
    const link = { inMatch: true, congested: true, backlog: () => 0, overCap: 0, delay: 5 };
    const host = { FS: makeFS(hostFiles) };
    const tabs = new Map();
    const deliver = (to, from, bytes, via) => {
        const copy = bytes.slice(0);
        const b = new Uint8Array(copy), v = new DataView(copy);
        log.push({ t: clock.now(), to, from, via, type: b[12], len: copy.byteLength,
                   block: b[12] === 6 || b[12] === 4 ? v.getUint32(24, true) : null,
                   at: b[12] === 6 ? v.getUint32(32, true) : null, bytes: b[12] === 6 ? null : b });
        clock.setTimeout(() => (to === 1 ? host : tabs.get(to).tab).jkMaps.onFrame(from, copy, via), link.delay);
    };
    const hostNet = {
        mapDirect: id => !!(tabs.get(id) && tabs.get(id).direct),
        mapSend(id, bytes, via) {
            const j = tabs.get(id);
            let used = via;
            if (imperial) {
                used = 'direct';
            } else if (!/^direct[1-9]$/.test(via) || !(j.lanes || []).includes(via)) {
                used = via !== 'relay' && j.direct ? 'direct' : 'relay';
                if (used === 'relay' && bytes.byteLength > 65536) {
                    link.overCap++;
                    return null;
                }
            }
            deliver(id, 1, bytes, used);
            return used;
        },
        mapBacklog: (id, via) => link.backlog(id, via),
        mapInMatch: () => link.inMatch,
        mapCongested: () => link.congested,
    };
    if (imperial) {
        host.jkImperial = Object.assign(hostNet, { active: true });
    } else {
        host.jkNet = hostNet;
    }
    loadOn(clock, source, host);
    for (const j of joiners) {
        const tab = { FS: makeFS({ ...retailInstall }) };
        const t = { ...j, tab };
        const net = {
            mapDirect: () => t.direct,
            rtcEnabled: () => t.direct,
            mapSend(id, bytes, via) {
                const used = imperial ? 'direct' : via === 'relay' || !t.direct ? 'relay' : (t.lanes || []).includes(via) ? via : 'direct';
                deliver(1, j.id, bytes, used);
                return used;
            },
        };
        if (j.lanes) {
            net.mapLanes = () => (t.direct ? t.lanes : []);
            net.mapBulk = () => {};
        }
        if (imperial)
            tab.jkImperial = Object.assign(net, { active: true });
        else
            tab.jkNet = net;
        loadOn(clock, j.src || oldSource, tab);
        tabs.set(j.id, t);
    }
    return { host, tabs, link, log, tab: id => tabs.get(id).tab };
}

const blockReqs = (log, from) => log.filter(f => f.to === 1 && f.from === from && f.type === 5);
const blockParts = (log, to) => log.filter(f => f.from === 1 && f.to === to && f.type === 6);

// A map of n random bytes (they don't squeeze) and its stream's size.
function bigMap(n) {
    const gob = makeGob([{ name: 'episode.jk', bytes: randomBytes(1000) }, { name: 'big.jkl', bytes: randomBytes(n) }]);
    return { gob, stream: gob.length + 4 };
}

async function hostOn(clock, g, name) {
    g.host.jkMaps.host(name, 'gob');
    // Hashing the map takes real time.
    await new Promise(r => setTimeout(r, 200));
}

// Joiner id's requests, made up: blocks first..first+count-1 with parts of
// part, or chunks.
function blockRequest(id8, first, count, part) {
    const f = new Uint8Array(16 + 24);
    const v = new DataView(f.buffer);
    f.set([0x4A, 0x4B, 0x4D, 0x54], 8);
    f[12] = 5; f[13] = 1;
    f.set(id8, 16);
    v.setUint32(24, first, true);
    v.setUint32(28, count, true);
    v.setUint32(32, part, true);
    return f.buffer;
}

function chunkRequest(id8, size, first, count) {
    const f = new Uint8Array(16 + 20);
    const v = new DataView(f.buffer);
    f.set([0x4A, 0x4B, 0x4D, 0x54], 8);
    f[12] = 3; f[13] = 1;
    f.set(id8, 16);
    v.setUint32(24, size, true);
    v.setUint32(28, first, true);
    v.setUint32(32, count, true);
    return f.buffer;
}

const idOfGob = gob => Buffer.from(sha(gob).slice(0, 16), 'hex');

// A map whose stream is 3 MiB exactly: joiners that start together finish
// their blocks in the same turn.
const MAP_3MIB = 3 * 1048576 - 4 - 1000 - 288;

for (const [what, joiners, mapBytes = 3500000] of [
    ['through the lobby (two blocks out)', [{ id: 2, direct: false }]],
    ['over the map channel (three blocks out)', [{ id: 2, direct: true }]],
    ['to two joiners at once', [{ id: 2, direct: true }, { id: 3, direct: true }]],
    ['to four joiners at once, whose blocks end together', [2, 3, 4, 5].map(id => ({ id, direct: true })), MAP_3MIB],
]) {
    test('in a match, an old joiner gets a paced host\'s map ' + what + ' in 16 KiB parts, asking for no block twice', async () => {
        const clock = makeClock();
        const { gob, stream } = bigMap(mapBytes);
        const g = pacedGame(clock, { ...retailInstall, '/jk1/episode/ctf_pace.gob': gob }, joiners);
        await hostOn(clock, g, 'ctf_pace');
        const t0 = clock.now();
        for (const j of joiners)
            g.tab(j.id).jkMaps.fetch('ctf_pace', 'gob');
        // The link always says a queue is building, so the rate stays at the
        // least that keeps every block coming.
        assert.ok(await clock.until(() => joiners.every(j => g.tab(j.id).jkMaps.state() !== 0), 600000), 'still busy');
        const secs = (clock.now() - t0) / 1000;
        const blocks = Math.ceil(stream / 1048576);
        for (const j of joiners) {
            assert.equal(g.tab(j.id).jkMaps.state(), 1);
            assert.deepEqual(Buffer.from(g.tab(j.id).FS.files.get('/jk1/episode/ctf_pace.gob')), Buffer.from(gob));
            const parts = blockParts(g.log, j.id);
            assert.ok(parts.every(f => f.len - 40 <= UNIT && f.at % UNIT === 0), 'a part over 16 KiB');
            assert.equal(parts.reduce((n, f) => n + f.len - 40, 0), stream, 'bytes sent');
            assert.equal(blockReqs(g.log, j.id).length, blocks, 'a block asked for again');
            // A part of each block at least every 2.5 s (the joiner asks
            // again after 5 s), however many finish at once.
            const last = {};
            for (const f of parts) {
                if (last[f.block] !== undefined)
                    assert.ok(f.t - last[f.block] < 2600, `block ${f.block} waited ${f.t - last[f.block]} ms`);
                last[f.block] = f.t;
            }
        }
        assert.equal(g.link.overCap, 0);
        const rate = g.host.jkMaps.uploads().rate;
        console.log(`    ${joiners.length} joiner(s), ${(stream / 1e6).toFixed(1)} MB each, in ${secs.toFixed(0)} s at ${(rate / 1000).toFixed(1)} KB/s`);
        assert.ok(rate <= Math.max(24000, joiners.length * 3 * UNIT * 1000 / 2500) + 1, 'rate ' + rate);
    });
}

// The paced rate at each look at the link (every 250 ms), as the link says
// says[i] at its ith answer. Through the lobby an answer comes at every
// look; from the Imperial relay server at every 8th (2 s), with no news in
// between, but a queue it showed stands until the next.
async function rateLooks(imperial, says) {
    const clock = makeClock();
    const { gob } = bigMap(20000000);
    const g = pacedGame(clock, { ...retailInstall, '/jk1/episode/ctf_rate.gob': gob }, [{ id: 2, direct: true }], { imperial });
    const every = imperial ? 8 : 1;
    const rates = [];
    g.host[imperial ? 'jkImperial' : 'jkNet'].mapCongested = () => {
        const n = rates.length, c = says[Math.floor(n / every)];
        rates.push(g.host.jkMaps.uploads().rate);
        return c === true || n % every === 0 ? c : null;
    };
    await hostOn(clock, g, 'ctf_rate');
    g.tab(2).jkMaps.fetch('ctf_rate', 'gob');
    assert.ok(await clock.until(() => rates.length > says.length * every, 60000, 10));
    assert.equal(g.tab(2).jkMaps.state(), 0, 'the map came before the looks ended');
    g.tab(2).jkMaps.abort('cancelled');
    return rates;
}

const near = (a, b) => Math.abs(a - b) < 1;

test('in a match the rate grows by a quarter a look until a queue shows, then halves and grows a step at a time', async () => {
    const says = [...Array(18).fill(false), true, false, false, null, null, false];
    const r = await rateLooks(false, says);
    assert.equal(r[0], 48000);
    for (let i = 0; i < 17; i++)
        assert.ok(near(r[i + 1], Math.min(2e6, r[i] * 1.25)), `look ${i}: ${r[i]} then ${r[i + 1]}`);
    assert.equal(r[18], 2e6);
    assert.ok(near(r[19], 1e6) && near(r[20], 1016000) && near(r[21], 1032000), r.slice(18, 22).join(' '));
    // No news holds it; the next clear look grows it for the time since.
    assert.ok(near(r[22], 1032000) && near(r[23], 1032000) && near(r[24], 1080000), r.slice(21, 25).join(' '));
});

test('on the Imperial Network Protocol the rate grows once for each answer, and a queue halves it at every look till the next', async () => {
    const says = [false, false, false, false, true, false, false, null, false];
    const r = await rateLooks(true, says);
    const least = 3 * 65536 * 1000 / 2500;
    const at = i => r[i * 8 + 1];   // just after the ith answer
    for (let i = 0; i < says.length; i++) {
        if (says[i] !== true)
            assert.ok(r.slice(i * 8 + 1, i * 8 + 9).every(x => x === at(i)), `answer ${i}: ` + r.slice(i * 8, i * 8 + 9).join(' '));
    }
    // Slow start: twice as fast for each answer, not for each look.
    assert.ok(near(at(2), 2 * at(1)) && near(at(3), 2 * at(2)), [at(1), at(2), at(3)].join(' '));
    // The queue halves it down to the least by its last look. Then 16 KB/s
    // for the one look since, and 64 KB/s for each answer 2 s on, also after
    // one that said nothing.
    assert.ok(near(r[40], least), r.slice(32, 41).join(' '));
    assert.ok(near(at(5), least + 16000), 'after the queue ' + at(5));
    assert.ok(near(at(6), at(5) + 64000), 'the next answer ' + at(6));
    assert.ok(near(at(8), at(6) + 64000), 'after no news ' + at(8));
});

test('outside a match a paced host sends as fast as its links take it', async () => {
    const clock = makeClock();
    const { gob } = bigMap(3500000);
    const g = pacedGame(clock, { ...retailInstall, '/jk1/episode/ctf_fast.gob': gob }, [{ id: 2, direct: true }]);
    g.link.inMatch = false;
    await hostOn(clock, g, 'ctf_fast');
    const t0 = clock.now();
    g.tab(2).jkMaps.fetch('ctf_fast', 'gob');
    assert.ok(await clock.until(() => g.tab(2).jkMaps.state() !== 0, 60000, 10));
    assert.equal(g.tab(2).jkMaps.state(), 1);
    assert.ok(clock.now() - t0 < 2000, (clock.now() - t0) + ' ms');
});

test('a block asked for again on another lane goes that way, and no part of it twice', async () => {
    const clock = makeClock();
    const { gob, stream } = bigMap(2500000);
    const g = pacedGame(clock, { ...retailInstall, '/jk1/episode/ctf_lane.gob': gob }, [{ id: 2, direct: true, lanes: ['direct', 'direct1'] }]);
    await hostOn(clock, g, 'ctf_lane');
    const id8 = idOfGob(gob);
    // The map channel takes nothing (its link stalled).
    g.link.backlog = (id, via) => (via === 'direct' ? 1e9 : 0);
    for (const b of [0, 1, 2])
        g.host.jkMaps.onFrame(2, blockRequest(id8, b, 1, 65536), 'direct');
    await clock.run(2000);
    assert.equal(blockParts(g.log, 2).length, 0);
    const queued = g.host.jkMaps.uploads().queued[2];
    assert.equal(queued, stream);
    // Asked again, on the transfer link.
    for (const b of [0, 1, 2])
        g.host.jkMaps.onFrame(2, blockRequest(id8, b, 1, 65536), 'direct1');
    await clock.run(1);
    // Not queued again, and parts start going the new way at once.
    assert.ok(g.host.jkMaps.uploads().queued[2] <= queued);
    assert.ok(await clock.until(() => g.host.jkMaps.uploads().groups === 0, 600000));
    const parts = blockParts(g.log, 2);
    assert.equal(parts.reduce((n, f) => n + f.len - 40, 0), stream);
    assert.equal(new Set(parts.map(f => f.block + ' ' + f.at)).size, parts.length, 'a part sent twice');
    assert.ok(parts.every(f => f.via === 'direct1'));
});

test('chunks asked for while they wait are queued once, and go in turn with blocks', async () => {
    const clock = makeClock();
    const { gob, stream } = bigMap(1500000);
    const g = pacedGame(clock, { ...retailInstall, '/jk1/episode/ctf_chunk.gob': gob }, [{ id: 2, direct: false }, { id: 3, direct: false }]);
    await hostOn(clock, g, 'ctf_chunk');
    const id8 = idOfGob(gob);
    g.link.backlog = () => 1e9;
    g.host.jkMaps.onFrame(2, chunkRequest(id8, 49152, 0, 10), 'relay');
    g.host.jkMaps.onFrame(2, chunkRequest(id8, 49152, 0, 10), 'relay');
    g.host.jkMaps.onFrame(2, chunkRequest(id8, 49152, 5, 10), 'relay');
    g.host.jkMaps.onFrame(3, blockRequest(id8, 0, 1, 49152), 'relay');
    await clock.run(100);
    assert.equal(g.host.jkMaps.uploads().queued[2], 15 * 49152);
    assert.equal(g.host.jkMaps.uploads().groups, 2);
    g.link.backlog = () => 0;
    assert.ok(await clock.until(() => g.host.jkMaps.uploads().groups === 0, 600000));
    const chunks = g.log.filter(f => f.from === 1 && f.to === 2 && f.type === 4);
    assert.deepEqual(chunks.map(f => f.block), [...Array(15).keys()]);
    assert.ok(chunks.every(f => f.len === 16 + 12 + 49152 && f.via === 'relay'));
    // Turn about: player 3's block didn't wait for all of player 2's chunks.
    const firstPart = g.log.findIndex(f => f.to === 3 && f.type === 6);
    const lastChunk = g.log.findLastIndex(f => f.to === 2 && f.type === 4);
    assert.ok(firstPart < lastChunk);
    assert.ok(stream > 15 * 49152);
});

test('a part cut for the map channel never goes through the lobby when the channel closes', async () => {
    const clock = makeClock();
    const { gob } = bigMap(2500000);
    const g = pacedGame(clock, { ...retailInstall, '/jk1/episode/ctf_cap.gob': gob }, [{ id: 2, direct: true }]);
    g.link.inMatch = false;
    await hostOn(clock, g, 'ctf_cap');
    const id8 = idOfGob(gob);
    g.link.backlog = () => 1e9;
    g.host.jkMaps.onFrame(2, blockRequest(id8, 0, 2, 65536), 'direct');
    await clock.run(100);
    // The map channel closes with the parts still waiting: they go through
    // the lobby, none over its 64 KiB.
    g.tabs.get(2).direct = false;
    g.link.backlog = () => 0;
    assert.ok(await clock.until(() => g.host.jkMaps.uploads().groups === 0, 60000));
    const parts = blockParts(g.log, 2);
    assert.equal(parts.length, 2 * 1048576 / UNIT);
    assert.ok(parts.every(f => f.via === 'relay' && f.len <= 65536));
    assert.equal(g.link.overCap, 0);
});

test('what waits for a player who left is dropped, and one joiner can\'t have more than 16 MiB waiting', async () => {
    const clock = makeClock();
    const { gob } = bigMap(20000000);
    const g = pacedGame(clock, { ...retailInstall, '/jk1/episode/ctf_cap.gob': gob }, [{ id: 2, direct: true }, { id: 3, direct: true }]);
    await hostOn(clock, g, 'ctf_cap');
    const id8 = idOfGob(gob);
    g.link.backlog = () => 1e9;
    // A made-up client asking for every block at once.
    for (let b = 0; b < 20; b += 4)
        g.host.jkMaps.onFrame(2, blockRequest(id8, b, 4, 65536), 'direct');
    g.host.jkMaps.onFrame(3, blockRequest(id8, 0, 2, 65536), 'direct');
    g.host.jkMaps.onFrame(3, blockRequest(id8, 2, 1, 49152), 'relay');
    await clock.run(100);
    let up = g.host.jkMaps.uploads();
    assert.equal(up.queued[2], 16 * 1048576);
    assert.equal(up.queued[3], 3 * 1048576);
    g.host.jkMaps.forget(2);
    // Player 3's link closed: what went that way is dropped, not what goes
    // through the lobby.
    g.host.jkMaps.forget(3, true);
    up = g.host.jkMaps.uploads();
    assert.equal(up.queued[2], 0);
    assert.equal(up.queued[3], 1048576);
    assert.equal(up.groups, 1);
    g.link.backlog = () => 0;
    assert.ok(await clock.until(() => g.host.jkMaps.uploads().groups === 0, 600000));
    assert.equal(blockParts(g.log, 2).length, 0);
    assert.ok(blockParts(g.log, 3).every(f => f.block === 2 && f.via === 'relay'));
    // And when the host stops offering it.
    g.link.backlog = () => 1e9;
    g.host.jkMaps.onFrame(3, blockRequest(id8, 3, 1, 65536), 'direct');
    await clock.run(100);
    assert.equal(g.host.jkMaps.uploads().groups, 1);
    g.host.jkMaps.unhost();
    assert.equal(g.host.jkMaps.uploads().groups, 0);
});

test('on the Imperial Network Protocol parts keep the size the joiner asked for, and an old joiner gets the map in a match', async () => {
    const clock = makeClock();
    const { gob, stream } = bigMap(2500000);
    const g = pacedGame(clock, { ...retailInstall, '/jk1/episode/ctf_imp.gob': gob }, [{ id: 2, direct: true }], { imperial: true });
    await hostOn(clock, g, 'ctf_imp');
    g.tab(2).jkMaps.fetch('ctf_imp', 'gob');
    assert.ok(await clock.until(() => g.tab(2).jkMaps.state() !== 0, 600000));
    assert.equal(g.tab(2).jkMaps.state(), 1);
    assert.deepEqual(Buffer.from(g.tab(2).FS.files.get('/jk1/episode/ctf_imp.gob')), Buffer.from(gob));
    const parts = blockParts(g.log, 2);
    assert.ok(parts.every(f => f.len - 40 === 65536 || f.at + f.len - 40 === Math.min(1048576, stream - f.block * 1048576)));
    let expected = 0;
    for (let at = 0; at < stream; at += 1048576)
        expected += Math.ceil(Math.min(1048576, stream - at) / 65536);
    assert.equal(parts.length, expected);
    assert.equal(blockReqs(g.log, 2).length, Math.ceil(stream / 1048576));
});

test('a new joiner gets the map from a paced host as from an old one', async () => {
    const clock = makeClock();
    const { gob } = bigMap(2500000);
    const g = pacedGame(clock, { ...retailInstall, '/jk1/episode/ctf_new.gob': gob }, [{ id: 2, direct: true, src: source, lanes: ['direct'] }]);
    await hostOn(clock, g, 'ctf_new');
    g.tab(2).jkMaps.fetch('ctf_new', 'gob');
    assert.ok(await clock.until(() => g.tab(2).jkMaps.state() !== 0, 600000));
    assert.equal(g.tab(2).jkMaps.state(), 1);
    assert.deepEqual(Buffer.from(g.tab(2).FS.files.get('/jk1/episode/ctf_new.gob')), Buffer.from(gob));
    assert.equal(blockReqs(g.log, 2).length, 3);
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
