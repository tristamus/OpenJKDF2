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
import { makeGob, sha, contains, makeFS } from './helpers.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, '..', 'maps.js'), 'utf8');

function loadMaps(Module) {
    new Function('Module', source)(Module);
    return Module.jkMaps;
}

// ---- Two tabs and the link between them.

function makeGame({ hostFiles, joinFiles, direct = true, drop = () => false }) {
    const log = [];
    const link = { direct, drop, log };
    const host = { FS: makeFS(hostFiles) };
    const join = { FS: makeFS(joinFiles) };
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
    assert.ok(!contains(plan.stream, retailA) && !contains(plan.stream, retailB), 'retail bytes in the stream');
    assert.ok(contains(plan.stream, custom1) && contains(plan.stream, custom2) && contains(plan.stream, modifiedC));
    assert.equal(plan.stream.length, plan.refsBytes + customMap.length - retailA.length - retailB.length);

    const info = { size: customMap.length, sha256: sha(customMap), streamSize: plan.stream.length, refsBytes: plan.refsBytes };
    const out = await lib.rebuild(info, plan.stream, sources(lib, [retailRes]));
    assert.ok(out.ok, out.reason);
    assert.deepEqual(Buffer.from(out.bytes), Buffer.from(customMap));

    const missing = await lib.rebuild(info, plan.stream, sources(lib, [retailEp]));
    assert.equal(missing.reason, 'missing-retail');

    const tampered = plan.stream.slice();
    tampered[tampered.length - 1] ^= 1;
    assert.equal((await lib.rebuild(info, tampered, sources(lib, [retailRes]))).reason, 'mismatch');
    assert.equal((await lib.rebuild(info, plan.stream.subarray(1), sources(lib, [retailRes]))).reason, 'corrupt');
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
    assert.ok(!contains(plan.stream, retailA));
    const info = { size: gob.length, sha256: sha(gob), streamSize: plan.stream.length, refsBytes: plan.refsBytes };
    const out = await lib.rebuild(info, plan.stream, sources(lib, [retailRes]));
    assert.ok(out.ok, out.reason);
    assert.deepEqual(Buffer.from(out.bytes), Buffer.from(gob));
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
    const chunks = framesTo(g.log, 'join', 4);
    assert.equal(chunks.length, Math.ceil((customMap.length - 8000 + 4 + 80) / 16384));
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
    const chunks = framesTo(g.log, 'join', 4);
    assert.equal(chunks.length, Math.ceil((big.length - retailA.length + 44) / (48 * 1024)));
    const requests = framesTo(g.log, 'host', 3).length + framesTo(g.log, 'host', 1).length;
    console.log(`    ${big.length} byte map over the relay: ${chunks.length} chunk frames, ${requests} requests`);
    // Half a window (10 chunks) per request, plus the first full window and
    // the info request.
    assert.ok(requests <= Math.ceil(chunks.length / 10) + 1, requests + ' requests');
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
        drop: f => f.to === 'join' && f.bytes[12] === 4,
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

test('chunks lost when the link changes are asked for again', async () => {
    let dropped = 0;
    const g = makeGame({
        hostFiles: { ...retailInstall, '/jk1/episode/ctf_test.gob': customMap },
        joinFiles: { ...retailInstall },
        drop: f => {
            if (f.to !== 'join' || f.bytes[12] !== 4 || f.via !== 'direct' || dropped >= 3)
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
