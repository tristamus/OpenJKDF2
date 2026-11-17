// Tests for the free multiplayer demo in packaging/wasm/index.html: the Free
// Demo Room has the demo's games and Nar Shaddaa the rest, the demo's files
// are asked for where jkdf2mp-bb-server's levels.js serves them, and a file
// is only used when it's the demo's, byte for byte. In Node:
// node packaging/wasm/test/demo-test.mjs
//
// The code is taken from index.html as it is.

import { readFileSync } from 'node:fs';
import { createHash, webcrypto } from 'node:crypto';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to, a);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}

const rooms = new Function('imperialLive',
    cut('      var ROOMS = {', "      var room = document.getElementById('room');") +
    cut("      // A game of the free demo's", '      // ---- The Imperial relay') +
    'return { ROOMS, isDemoGame, roomStatus, roomImperial };');
const demo = new Function('fetch', 'crypto',
    cut('      var DEMO_FILES = [', '      // Downloads the demo, in the folder') +
    'return { DEMO_FILES, DEMO_WANT, demoUrl, demoDownload };');

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

const relay = [{ uid: 'iabc', imperial: true, game: 'jk1', listed: true }];
const { ROOMS, isDemoGame, roomStatus, roomImperial } = rooms({ games: relay });

const status = {
    online: 9, lobby: ['Waiting'],
    byGame: { jk1: { online: 7, lobby: ['Waiting'] }, mots: { online: 2, lobby: [] } },
    games: [
        { id: 1, game: 'jk1', episode: 'JK1MP.gob', players: 2 },
        { id: 2, game: 'jk1', episode: 'jk1mpdem', players: 3 },
        { id: 3, game: 'jk1', episode: 'JK1MPDEM.GOB', players: 1 },
        { id: 4, game: 'mots', episode: 'JKM_MP.goo', players: 2 },
        { id: 5, game: 'jk1', episode: 'jk1mpdemo.gob', players: 0 }
    ]
};

test("only the demo's episode is a demo game, whatever its case and with or without .gob", () => {
    assert.deepEqual(status.games.map(isDemoGame), [false, true, true, false, false]);
    assert.equal(isDemoGame({ episode: 'jk1mpdem.goo' }), false);
    assert.equal(isDemoGame({ episode: '' }), false);
});

test("the Free Demo Room has the demo's games and their players, Nar Shaddaa the rest of Jedi Knight's", () => {
    const d = roomStatus(status, 'bespin-demo');
    assert.deepEqual(d.games.map(g => g.id), [2, 3]);
    assert.equal(d.online, 4, "the players in the demo's games");
    assert.deepEqual(d.lobby, [], "who's waiting isn't known to run the demo");
    const n = roomStatus(status, 'nar-shaddaa');
    assert.deepEqual(n.games.map(g => g.id), [1, 5]);
    assert.equal(n.online, 3, "Jedi Knight's players less the demo's");
    assert.deepEqual(n.lobby, ['Waiting']);
    const s = roomStatus(status, 'sith-asteroid');
    assert.deepEqual([s.games.map(g => g.id), s.online], [[4], 2], "Sith: Asteroid's as it was");
    assert.equal(roomStatus(null, 'bespin-demo'), null);
});

test("the Free Demo Room lists none of the Imperial relay's games", () => {
    assert.deepEqual(roomImperial('bespin-demo'), []);
    assert.equal(roomImperial('nar-shaddaa'), relay);
    assert.equal(roomImperial('sith-asteroid'), relay);
});

test("the demo's room starts Jedi Knight's demo and comes back to its window", () => {
    const { DEMO_WANT } = demo();
    assert.deepEqual(ROOMS['bespin-demo'], { game: 'jk1', name: 'Bespin Mining Station',
        title: 'Free Demo Room - Bespin Mining Station', demo: true });
    assert.deepEqual(DEMO_WANT, { game: 'jk1', room: 'Bespin Mining Station', lobbyRoom: 'bespin-demo', demo: true });
});

test("the demo's files are asked for by the names levels.js serves", () => {
    const { DEMO_FILES, demoUrl } = demo();
    assert.deepEqual(DEMO_FILES.map(f => f.path), ['episode/jk1mpdem.gob', 'resource/res1demo.gob']);
    assert.deepEqual(DEMO_FILES.map(demoUrl), ['/levels/demo/jk1mpdem.b7bab76a6552.gob',
                                               '/levels/demo/res1demo.9708ac558b00.gob']);
    // levels.js's route for them (jkdf2mp-bb-server).
    for (const f of DEMO_FILES)
        assert.match(demoUrl(f), /^\/levels\/demo\/([a-z0-9_]{1,32}\.[0-9a-f]{12}\.gob)$/);
    assert.deepEqual(DEMO_FILES.map(f => f.size), [674061, 31326451]);
    DEMO_FILES.forEach(f => assert.match(f.sha256, /^[0-9a-f]{64}$/));
});

// demoDownload with a made-up file and site: the body comes in parts, as a
// browser's does, already unzipped (the site sends it gzipped, and the
// browser undoes that).
function site(bodies) {
    const asked = [];
    const fetch = async (url, opts) => {
        asked.push({ url, cache: opts.cache });
        const body = bodies.shift();
        if (typeof body === 'number') return { ok: false, status: body };
        let i = 0;
        return { ok: true, body: { getReader: () => ({
            read: async () => i < body.length ? { done: false, value: body.subarray(i, i += 1000) } : { done: true },
            cancel: async () => {}
        }) } };
    };
    return { asked, fetch };
}
const bytes = new Uint8Array(4321).map((_, i) => (i * 7) & 255);
const file = { path: 'episode/test.gob', size: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') };

test("a file is used when it's the demo's, and progress is told as it comes", async () => {
    const s = site([bytes]);
    const { demoDownload } = demo(s.fetch, webcrypto);
    const seen = [];
    const got = await demoDownload(file, n => seen.push(n), null, false);
    assert.deepEqual(Array.from(got), Array.from(bytes));
    assert.deepEqual(seen, [1000, 2000, 3000, 4000, 4321]);
    assert.deepEqual(s.asked, [{ url: '/levels/demo/test.' + file.sha256.slice(0, 12) + '.gob', cache: 'default' }]);
});

test("a file that isn't the demo's, byte for byte, isn't used", async () => {
    const other = bytes.slice();
    other[100] ^= 1;
    const cases = [[other, "a file came that isn't the demo's"], [bytes.subarray(0, 4000), 'a file came incomplete'],
                   [new Uint8Array(5000), 'a file came bigger than it is']];
    for (const [body, why] of cases) {
        const { demoDownload } = demo(site([body]).fetch, webcrypto);
        await assert.rejects(demoDownload(file, () => {}, null, false), e => e.wrong === true && e.message === why, why);
    }
    // Asked for again, it's the site's, not the browser's copy.
    const s = site([bytes]);
    await demo(s.fetch, webcrypto).demoDownload(file, () => {}, null, true);
    assert.equal(s.asked[0].cache, 'reload');
});

test("the site not answering is said, and isn't taken for a wrong file", async () => {
    const { demoDownload } = demo(site([404]).fetch, webcrypto);
    await assert.rejects(demoDownload(file, () => {}, null, false), e => !e.wrong && /answered 404/.test(e.message));
});

test('the demo starts in demo mode, with no Imperial relay, from its own files and the standard key', () => {
    const start = cut('      async function startDemo(want) {', '      // Closing the box while the demo downloads');
    assert.ok(start.includes('Module.jkDemo = true;'));
    assert.ok(start.includes('delete Module.jkImperialUrl;'));
    assert.ok(start.includes("CD_KEYS.jk1[0]") && start.includes('CD_KEY_PATH'));
    assert.ok(start.includes("mountSaves(FS, [], '/jk1/');"), "the full game's characters and settings");
    // The Level Library and the Mod Library hide themselves in demo mode.
    for (const js of ['library.js', 'modlibrary.js']) {
        const src = readFileSync(new URL('../' + js, import.meta.url), 'utf8');
        assert.match(src, /function available\(\) \{\n\s+if \(Module\.jkDemo\)[^\n]*\n\s+return false;/, js);
    }
});

// maps.js as the engine bundles it, with a made-up Module: what its preRun
// does at the engine's start.
function mapsStart(demo) {
    const deps = [];
    const Module = { jkDemo: demo, preRun: [], arguments: [],
                     addRunDependency: d => deps.push('+' + d), removeRunDependency: d => deps.push('-' + d) };
    // An IndexedDB that never opens: only whether it's asked matters here.
    const indexedDB = { open: () => { deps.push('open'); return {}; } };
    new Function('Module', 'indexedDB', readFileSync(new URL('../maps.js', import.meta.url), 'utf8'))(Module, indexedDB);
    Module.preRun.forEach(f => f());
    return deps;
}

test("the demo's Host screen doesn't get the maps saved for the full game", () => {
    assert.deepEqual(mapsStart(true), [], "the demo doesn't load them");
    assert.ok(mapsStart(false).includes('+jk-maps'), 'the full game does, as before');
});

let failed = 0;
for (const t of tests) {
    try {
        await t.fn();
        console.log('ok   ' + t.name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + t.name + '\n     ' + String(e && e.stack || e).split('\n').join('\n     '));
    }
}
console.log(failed ? failed + ' of ' + tests.length + ' failed' : 'all ' + tests.length + ' passed');
process.exit(failed ? 1 : 0);
