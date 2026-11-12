// Tests for the match record in the browser build: packaging/wasm/ladder.js
// (what the game reports to the lobby) and the latest matches in
// packaging/wasm/index.html. In Node:
// node packaging/wasm/test/ladder-test.mjs
//
// With the lobby's own code beside this repository, the reports are also put
// through its reader, to check the two agree on the format:
// node packaging/wasm/test/ladder-test.mjs ../jkdf2mp-bb-server/cloudflare/src/ladder.js
//
// ladder.js is loaded as openjkdf2.js loads it, with a Module of the test's
// and a clock the test moves by hand. The page's functions are taken from
// index.html as they are.

import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
import assert from 'node:assert/strict';

const read = path => readFileSync(new URL('../../../' + path, import.meta.url), 'utf8');
const source = read('packaging/wasm/ladder.js');
const html = read('packaging/wasm/index.html');
const relayC = read('src/Platform/Networking/WSRelay/stdComm_WSRelay.c');
const cmake = read('cmake_modules/plat_wasm.cmake');

const lobbyPath = process.argv[2];
const lobby = lobbyPath ? await import(pathToFileURL(resolve(lobbyPath)).href) : null;

// One clock for a game. Timers run in order as it moves on.
function makeWorld() {
    const timers = [];
    let now = 1000, seq = 0;
    return {
        now: () => now,
        timeout(fn, ms) {
            const t = { at: now + Math.max(0, ms || 0), fn, seq: seq++ };
            timers.push(t);
            return t;
        },
        interval(fn, ms) {
            const t = { at: now + ms, fn, every: ms, seq: seq++ };
            timers.push(t);
            return t;
        },
        clear(t) {
            const i = timers.indexOf(t);
            if (i >= 0) timers.splice(i, 1);
        },
        run(ms) {
            const end = now + ms;
            for (;;) {
                let next = null;
                for (const t of timers)
                    if (!next || t.at < next.at || (t.at === next.at && t.seq < next.seq)) next = t;
                if (!next || next.at > end) break;
                now = Math.max(now, next.at);
                if (next.every) next.at += next.every;
                else timers.splice(timers.indexOf(next), 1);
                next.fn();
            }
            now = end;
        },
    };
}

// A game in a room of a lobby that keeps a record, unless told otherwise.
function makeGame({ can = ['links', 'pw', 'match'], key = 'ab'.repeat(16), anon = false } = {}) {
    const world = makeWorld();
    const sent = [];
    const listeners = {};
    const Module = {
        jkNet: { ws: { send: line => sent.push(line) }, state: 1, session: true, seat: { room: 7, id: 2, key: 'k' },
                 lobbyCan: can },
    };
    if (key) Module.jkLadderKey = key;
    if (anon) Module.jkLadderAnon = true;
    const window = { addEventListener: (type, fn) => { listeners[type] = fn; } };
    new Function('Module', 'window', 'performance', 'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', source)(
        Module, window, { now: world.now }, world.timeout, world.clear, world.interval, world.clear);
    // The reports sent so far, read back, and forgotten.
    function reports() {
        return sent.splice(0).map(line => {
            const m = /^match ([A-Za-z0-9+/]+={0,2})$/.exec(line);
            assert.ok(m, 'a report is "match <base64>": ' + line);
            const r = JSON.parse(Buffer.from(m[1], 'base64').toString('utf8'));
            if (lobby) {
                const c = lobby.cleanReport(m[1]);
                assert.ok(c, "the lobby takes the report");
                assert.equal(c.lv, r.lv);
                assert.equal(c.map, r.map.toLowerCase());
                assert.equal(c.title, r.title);
                assert.equal(c.pk, r.pk || '');
                assert.equal(c.anon, r.anon === 1);
                assert.equal(c.idle, r.idle === 1);
                assert.equal(c.build, r.build);
                assert.equal(c.end, r.end || '');
                assert.deepEqual(c.ev, r.ev);
                assert.deepEqual(c.rules, r.rules || null);
                assert.deepEqual(c.sb, r.sb || null);
                assert.deepEqual(c.teams, r.teams || null);
            }
            return r;
        });
    }
    return { world, Module, L: Module.jkLadder, net: Module.jkNet, sent, reports, listeners };
}

const SB = [[1, 3, 4, 2, 1, 0], [2, 1, 1, 3, 0, 0]];
const TEAMS = [0, 0, 0, 0, 0];

function start(g, { host = false, idle = false, map = 'm4.jkl', title = 'Canyon Oasis' } = {}) {
    g.L.start(map, title, 0x18, 15, 20, host, idle, 'abc1234');
    g.L.scores(SB, TEAMS);
}

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

test('a lobby that keeps no record gets nothing', () => {
    for (const can of [['links', 'pw'], [], null]) {
        const g = makeGame({ can });
        start(g, { host: true });
        g.L.kill(2, 1, 1);
        g.world.run(120000);
        g.L.end(false);
        assert.equal(g.sent.length, 0);
    }
});

test('nor does one this game has no seat in', () => {
    const g = makeGame();
    g.net.seat = null;
    start(g, { host: true });
    g.L.kill(2, 1, 1);
    g.world.run(5000);
    assert.equal(g.sent.length, 0);
});

test("the level's start goes at once, and from the host with its rules and scoreboard", () => {
    const g = makeGame();
    start(g, { host: true });
    assert.equal(g.sent.length, 0, 'once the engine has given the scoreboard');
    g.world.run(0);
    const [r] = g.reports();
    assert.deepEqual(r, { v: 1, lv: 1, map: 'm4.jkl', title: 'Canyon Oasis', t: 0, build: 'abc1234', ev: [],
                          pk: 'ab'.repeat(16), rules: { mode: 0x18, sl: 15, tl: 20 }, sb: SB, teams: TEAMS });
});

test("a joiner's has neither", () => {
    const g = makeGame();
    start(g);
    g.world.run(0);
    const [r] = g.reports();
    assert.equal(r.rules, undefined);
    assert.equal(r.sb, undefined);
    assert.equal(r.teams, undefined);
    assert.equal(r.idle, undefined);
});

test('a host that only serves says its player is idle', () => {
    const g = makeGame();
    start(g, { host: true, idle: true });
    g.world.run(0);
    assert.equal(g.reports()[0].idle, 1);
});

test('a kill goes within two seconds, and kills close together in one report', () => {
    const g = makeGame();
    start(g);
    g.world.run(100);
    g.reports();
    g.L.kill(2, 1, 1);
    g.world.run(700);
    g.L.kill(1, 0, 5);
    g.world.run(1000);
    assert.equal(g.sent.length, 0, 'not before two seconds');
    g.world.run(1400);
    const got = g.reports();
    assert.equal(got.length, 1);
    assert.equal(got[0].ev.length, 2);
    const [a, b] = got[0].ev;
    assert.deepEqual([a[0], a[2], a[3], a[4]], [1, 2, 1, 1]);
    assert.deepEqual([b[0], b[2], b[3], b[4]], [2, 1, 0, 5]);
    assert.ok(a[1] >= 2000 && a[1] <= 3100, 'how long ago the first was: ' + a[1]);
    assert.equal(a[1] - b[1], 700);
    assert.ok(got[0].t >= 2000);
});

test('with no kills, a report every 30 seconds', () => {
    const g = makeGame();
    start(g);
    g.world.run(100);
    g.reports();
    g.world.run(29000);
    assert.equal(g.sent.length, 0);
    g.world.run(60000);
    assert.equal(g.reports().length, 2);
});

test("the level's end goes with what's waiting, and nothing after it", () => {
    for (const [left, word] of [[false, 'level'], [true, 'left']]) {
        const g = makeGame();
        start(g, { host: true });
        g.world.run(5000);
        g.reports();
        g.L.kill(2, 1, 2);
        g.L.scores([[1, 4, 5, 2, 1, 0], [2, 1, 1, 4, 0, 0]], TEAMS);
        g.L.end(left);
        const got = g.reports();
        assert.equal(got.length, 1);
        assert.equal(got[0].end, word);
        assert.equal(got[0].ev.length, 1);
        assert.equal(got[0].sb[0][2], 5, 'with the scoreboard as it ended');
        g.L.kill(1, 2, 2);
        g.world.run(120000);
        assert.equal(g.sent.length, 0);
    }
});

test('kills wait while the lobby is out of reach, each with how long ago it was', () => {
    const g = makeGame();
    start(g);
    g.world.run(100);
    g.reports();
    g.net.state = 0;
    g.L.kill(2, 1, 1);
    g.world.run(4000);
    g.L.kill(1, 2, 4);
    g.world.run(6000);
    assert.equal(g.sent.length, 0);
    g.net.state = 1;
    g.world.run(1000);
    const [r] = g.reports();
    assert.equal(r.ev.length, 2);
    assert.ok(r.ev[0][1] >= 10000 && r.ev[0][1] <= 11100, String(r.ev[0][1]));
    assert.equal(r.ev[0][1] - r.ev[1][1], 4000);
});

test('a send that fails keeps the kills for the next try', () => {
    const g = makeGame();
    start(g);
    g.world.run(100);
    g.reports();
    const send = g.net.ws.send;
    g.net.ws.send = () => { throw new Error('closing'); };
    g.L.kill(2, 1, 1);
    g.world.run(3000);
    g.net.ws.send = send;
    g.world.run(1000);
    assert.equal(g.reports()[0].ev.length, 1);
});

test('more kills than a report takes go in several, the end with the last', () => {
    const g = makeGame();
    start(g);
    g.world.run(100);
    g.reports();
    for (let i = 0; i < 150; i++) g.L.kill(1 + i % 2, 2 - i % 2, 2);
    g.L.end(false);
    const got = g.reports();
    assert.deepEqual(got.map(r => r.ev.length), [64, 64, 22]);
    assert.deepEqual(got.map(r => r.end), [undefined, undefined, 'level']);
    assert.deepEqual(got.flatMap(r => r.ev.map(e => e[0])), Array.from({ length: 150 }, (_, i) => i + 1));
});

// A level played for five seconds whose last kill hasn't gone yet, with the
// lobby out of reach (down) as it ends.
function endsUnheard(g, { host = false } = {}) {
    start(g, { host });
    g.world.run(5000);
    g.reports();
    g.net.state = 0;
    g.L.kill(2, 1, 2);
    g.world.run(1000);
    g.L.scores([[1, 4, 5, 2, 1, 0], [2, 1, 1, 4, 0, 0]], TEAMS);
    g.L.end(false);
    assert.equal(g.sent.length, 0);
}

test("a level's end the lobby couldn't be told is kept, and goes when the lobby is back", () => {
    const g = makeGame();
    endsUnheard(g, { host: true });
    g.world.run(20000);
    assert.equal(g.sent.length, 0, 'not while the lobby is out of reach');
    g.net.state = 1;
    g.world.run(1000);
    const got = g.reports();
    assert.equal(got.length, 1);
    const [r] = got;
    assert.deepEqual([r.lv, r.map, r.end, r.t], [1, 'm4.jkl', 'level', 6000], "the level's own, its time stopped where it ended");
    assert.equal(r.ev.length, 1);
    assert.ok(r.ev[0][1] >= 21000 && r.ev[0][1] <= 22100, 'the kill with how long ago it really was: ' + r.ev[0][1]);
    assert.equal(r.sb[0][2], 5, 'with the scoreboard as it ended');
    g.world.run(120000);
    assert.equal(g.sent.length, 0, 'once');
});

test("a host's goes ahead of its next level's first report, never after it", () => {
    const g = makeGame();
    endsUnheard(g, { host: true });
    g.world.run(3000);
    start(g, { host: true });
    g.L.kill(2, 1, 1);
    g.world.run(4000);
    assert.equal(g.sent.length, 0);
    // A send that fails keeps both, in their order.
    const send = g.net.ws.send;
    g.net.ws.send = () => { throw new Error('closing'); };
    g.net.state = 1;
    g.world.run(2000);
    g.net.ws.send = send;
    g.world.run(1000);
    const got = g.reports();
    assert.deepEqual(got.map(r => [r.lv, r.end || '', r.ev.length]), [[1, 'level', 1], [2, '', 1]]);
    assert.equal(got[0].t, 6000);
    assert.equal(got[0].sb[0][2], 5, "the first level's scoreboard, not the second's");
    assert.equal(got[1].sb[0][2], 4);
    assert.equal(got[1].ev[0][0], 1, "the second level's kills are counted from one");
});

test("another game's goes only until its own next level begins", () => {
    const back = makeGame();
    endsUnheard(back);
    back.world.run(2000);
    back.net.state = 1;
    back.world.run(1000);
    assert.deepEqual(back.reports().map(r => [r.lv, r.end, r.ev.length]), [[1, 'level', 1]], 'back before it: it goes');
    const late = makeGame();
    endsUnheard(late);
    late.world.run(2000);
    start(late);
    late.net.state = 1;
    late.world.run(40000);
    assert.deepEqual(late.reports().map(r => [r.lv, r.end || '']), [[2, ''], [2, '']], "after it: the host's next match is open, so it's dropped");
});

test("nothing is kept for a player who left, a lobby with no record, or a level the lobby never heard of", () => {
    // Leaving gives up the seat.
    const left = makeGame();
    start(left);
    left.world.run(5000);
    left.reports();
    left.net.state = 0;
    left.L.kill(2, 1, 2);
    left.L.end(true);
    left.net.state = 1;
    left.world.run(10000);
    assert.equal(left.sent.length, 0);
    // No record, so no report of the level ever went.
    const none = makeGame({ can: ['links', 'pw'] });
    start(none, { host: true });
    none.world.run(5000);
    none.L.end(false);
    none.net.lobbyCan = ['links', 'pw', 'match'];
    none.world.run(10000);
    assert.equal(none.sent.length, 0);
    // Out of reach for the whole level.
    const never = makeGame();
    never.net.state = 0;
    start(never, { host: true });
    never.world.run(5000);
    never.L.kill(2, 1, 2);
    never.L.end(false);
    never.net.state = 1;
    never.world.run(10000);
    assert.equal(never.sent.length, 0);
});

test("what's kept is dropped after five minutes, when the game leaves, and in another seat", () => {
    const old = makeGame();
    endsUnheard(old, { host: true });
    old.world.run(5 * 60 * 1000 + 1000);
    old.net.state = 1;
    old.world.run(5000);
    assert.equal(old.sent.length, 0, 'the lobby has closed the match by itself by then');
    const gone = makeGame();
    endsUnheard(gone, { host: true });
    gone.net.session = false;
    gone.net.seat = null;
    gone.world.run(2000);
    // Hosting again: another room.
    gone.net.session = true;
    gone.net.seat = { room: 9, id: 1, key: 'n' };
    gone.net.state = 1;
    start(gone, { host: true });
    gone.world.run(2000);
    assert.deepEqual(gone.reports().map(r => [r.lv, r.end || '']), [[2, '']], "the old room's last report doesn't go to the new one");
    const moved = makeGame();
    endsUnheard(moved, { host: true });
    moved.net.seat = { room: 9, id: 1, key: 'n' };
    moved.net.state = 1;
    moved.world.run(2000);
    assert.equal(moved.sent.length, 0);
});

test('an end with more kills waiting than go at once is finished a moment later', () => {
    const g = makeGame();
    start(g);
    g.world.run(100);
    g.reports();
    for (let i = 0; i < 300; i++) g.L.kill(1 + i % 2, 2 - i % 2, 2);
    g.L.end(false);
    assert.deepEqual(g.reports().map(r => [r.ev.length, r.end || '']), [[64, ''], [64, ''], [64, ''], [64, '']]);
    g.world.run(1000);
    assert.deepEqual(g.reports().map(r => [r.ev.length, r.end || '']), [[44, 'level']]);
    g.world.run(60000);
    assert.equal(g.sent.length, 0);
});

test('a player who asked not to be recorded by name sends no key', () => {
    const g = makeGame({ anon: true });
    start(g);
    g.world.run(0);
    const [r] = g.reports();
    assert.equal(r.anon, 1);
    assert.equal(r.pk, undefined);
});

test('nor does one whose page has no key, and neither is anonymous', () => {
    const g = makeGame({ key: '' });
    start(g);
    g.world.run(0);
    const [r] = g.reports();
    assert.equal(r.anon, undefined);
    assert.equal(r.pk, undefined);
});

test('the next level counts its kills from one again', () => {
    const g = makeGame();
    start(g);
    g.world.run(100);
    g.L.kill(2, 1, 1);
    g.L.end(false);
    g.reports();
    start(g, { map: 'm5.jkl', title: 'Battleground Jedi' });
    g.world.run(100);
    g.L.kill(1, 2, 1);
    g.world.run(3000);
    const got = g.reports();
    assert.equal(got[0].lv, 2);
    assert.equal(got[0].map, 'm5.jkl');
    assert.deepEqual(got[got.length - 1].ev.map(e => e[0]), [1]);
});

test("a level's file is named as the lobby takes it", () => {
    for (const [file, name] of [['M4.JKL', 'M4.JKL'], ['my level (v2).jkl', 'my_level__v2_.jkl'], ['', 'unknown'],
                                ['a'.repeat(40) + '.jkl', 'a'.repeat(32)]]) {
        const g = makeGame();
        start(g, { map: file });
        g.world.run(0);
        assert.equal(g.reports()[0].map, name);
    }
});

test('a title in another alphabet arrives whole', () => {
    const g = makeGame();
    const title = 'Каньон Оазис ' + String.fromCodePoint(0x1F5E1);
    start(g, { title });
    g.world.run(0);
    assert.equal(g.reports()[0].title, title);
});

test('a page closing in a match says it left', () => {
    const g = makeGame();
    start(g, { host: true });
    g.world.run(4000);
    g.reports();
    g.listeners.pagehide();
    const [r] = g.reports();
    assert.equal(r.end, 'left');
    g.listeners.pagehide();
    assert.equal(g.sent.length, 0, 'once');
});

test('how the last level ended is there for the page until the next begins', () => {
    // The page takes the game's picture of the final scoreboard only of a
    // level that ended as a level does, in a match the lobby was told of.
    const g = makeGame();
    assert.equal(g.L.ended, null);
    start(g, { host: true });
    g.world.run(4000);
    assert.equal(g.L.ended, null);
    g.L.end(false);
    assert.deepEqual(g.L.ended, { at: g.world.now(), left: false, told: true, host: true });
    start(g);
    assert.equal(g.L.ended, null, 'the next level began');
    g.world.run(1000);
    g.L.end(true);
    assert.deepEqual(g.L.ended, { at: g.world.now(), left: true, told: true, host: false });
    g.L.end(false);
    assert.equal(g.L.ended.left, true, 'no level to end');
    // A lobby that keeps no record was told nothing.
    const quiet = makeGame({ can: ['links'] });
    start(quiet, { host: true });
    quiet.world.run(4000);
    quiet.L.end(false);
    assert.equal(quiet.L.ended.told, false);
    // The page closing is the player leaving.
    const closed = makeGame();
    start(closed);
    closed.world.run(4000);
    closed.listeners.pagehide();
    assert.equal(closed.L.ended.left, true);
});

test("the lobby's word that a match was saved reaches the page", () => {
    const g = makeGame();
    const saved = [];
    g.Module.jkLadderSaved = id => saved.push(id);
    g.L.saved(12);
    g.L.saved(NaN);
    g.L.saved(0);
    assert.deepEqual(saved, [12]);
    const error = console.error;
    console.error = () => {};
    try {
        g.Module.jkLadderSaved = () => { throw new Error('page'); };
        g.L.saved(13);
    } finally {
        console.error = error;
    }
    assert.equal(g.L.lastSaved, 13);
});

test('the engine calls what ladder.js has, and is built with it', () => {
    for (const call of ['Module.jkLadder.start(', 'Module.jkLadder.kill(', 'Module.jkLadder.scores(',
                        'Module.jkLadder.end(', 'Module.jkLadder.saved('])
        assert.ok(relayC.includes(call), 'stdComm_WSRelay.c has ' + call);
    for (const name of ['start', 'kill', 'scores', 'end', 'saved'])
        assert.equal(typeof makeGame().L[name], 'function');
    assert.match(cmake, /--pre-js \$\{PROJECT_SOURCE_DIR\}\/packaging\/wasm\/ladder\.js/);
    // The lobby's word for keeping a record, as both sides spell it.
    assert.ok(source.includes("indexOf('match')"));
    assert.ok(relayC.includes("op === 'matched'"));
});

// ---- The page.

function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to, a);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const GAME_NAMES = { jk1: 'Jedi Knight', mots: 'Mysteries of the Sith' };
const cleanStatus = new Function('GAME_NAMES', cut('      function cleanStatus(s) {', "      // One room's part of the status") +
    'return cleanStatus;')(GAME_NAMES);
const matchesCode = cut('      var MATCH_KEY_SETTING', '      function matchLink(');
const page = new Function('GAME_NAMES', matchesCode +
    'return { cleanRecent, cleanTop, topShown, ordinal, playerHref, standingsHref, matchId, matchList, matchWinner, ' +
    'matchLength, matchAgo, myMatch, MATCH_ROWS, TOP_ROWS, MATCH_MINE_MS };')(GAME_NAMES);

test("the page reads the lobby's latest match from its status, and nothing from a lobby without a record", () => {
    const status = { online: 0, lobby: [], byGame: {}, games: [] };
    assert.equal(cleanStatus({ ...status, match: 12 }).match, 12);
    assert.equal(cleanStatus({ ...status, match: 0 }).match, 0);
    assert.equal(cleanStatus(status).match, null);
    for (const bad of [-1, '12', null, NaN, Infinity, {}])
        assert.equal(cleanStatus({ ...status, match: bad }).match, null);
});

test('the list of latest matches is taken only as what the page shows', () => {
    for (const bad of [null, 'x', 12, {}, { games: null }, { games: 'x' }])
        assert.equal(page.cleanRecent(bad), null);
    const got = page.cleanRecent({ v: 1, games: {
        jk1: [
            { id: 41, title: 'Canyon Oasis', mod: 'SBX 3.1', pack: 0, custom: 1, players: 4, secs: 754, at: 1794300000000,
              teams: false, name: 'Kyle_K', team: 0, score: 15, extra: 'x' },
            { id: '40', title: 'A text id' }, { id: 0 }, { id: 1.5 }, { id: 1e12 }, null, 'x',
            { id: 39, title: 'a\x07b' + 'c'.repeat(100), mod: 7, players: -3, secs: 'x', at: null, name: null, team: 9,
              score: 'x' },
            { id: 38, title: '', name: '<b>x</b>', team: 2, score: -4.4, teams: 1, pack: 1 },
        ],
        mots: 'x',
        other: [{ id: 5 }],
    } });
    assert.deepEqual(Object.keys(got), ['jk1', 'mots']);
    assert.deepEqual(got.mots, []);
    assert.deepEqual(got.jk1.map(m => m.id), [41, 39, 38]);
    assert.deepEqual(got.jk1[0], { id: 41, game: 'jk1', title: 'Canyon Oasis', mod: 'SBX 3.1', pack: false, custom: true,
                                   players: 4, secs: 754, at: 1794300000000, teams: false, name: 'Kyle_K', team: 0,
                                   score: 15 });
    assert.equal(got.jk1[1].title, 'ab' + 'c'.repeat(46));
    assert.deepEqual([got.jk1[1].mod, got.jk1[1].players, got.jk1[1].secs, got.jk1[1].at, got.jk1[1].name,
                      got.jk1[1].team, got.jk1[1].score], ['', 0, 0, 0, null, 4, null]);
    assert.deepEqual([got.jk1[2].title, got.jk1[2].name, got.jk1[2].score, got.jk1[2].teams, got.jk1[2].pack],
                     ['Unknown level', '<b>x</b>', -4, true, true]);
    // More than the lobby sends is cut.
    const many = page.cleanRecent({ games: { jk1: Array.from({ length: 40 }, (_, i) => ({ id: i + 1 })) } });
    assert.equal(many.jk1.length, 16);
});

test("one game's matches, or both games' together, newest first", () => {
    const recent = page.cleanRecent({ games: { jk1: [{ id: 9 }, { id: 7 }, { id: 4 }, { id: 3 }, { id: 2 }, { id: 1 }],
                                               mots: [{ id: 8 }, { id: 5 }] } });
    assert.equal(page.MATCH_ROWS, 5);
    assert.deepEqual(page.matchList(recent, 'all', page.MATCH_ROWS).map(m => m.id), [9, 8, 7, 5, 4]);
    assert.deepEqual(page.matchList(recent, 'all', 3).map(m => [m.id, m.game]), [[9, 'jk1'], [8, 'mots'], [7, 'jk1']]);
    assert.deepEqual(page.matchList(recent, 'mots', 6).map(m => m.id), [8, 5]);
    assert.deepEqual(page.matchList(recent, 'jk1', 2).map(m => m.id), [9, 7]);
    assert.deepEqual(page.matchList(null, 'all', 6), []);
    assert.deepEqual(page.matchList(recent, 'nonsense', 6), []);
});

test('the standings are taken only as what the page shows', () => {
    for (const bad of [null, 'x', 12, {}, { top: null }, { top: 'x', you: 'x' }, { top: { jk1: 'x', mots: [] } }]) {
        const got = page.cleanTop(bad);
        assert.deepEqual(Object.keys(got.games), ['jk1', 'mots']);
        assert.deepEqual(got.games.jk1, { period: 'week', week: { players: [], of: 0, matches: 0, you: null },
                                          month: { players: [], of: 0, matches: 0, you: null },
                                          all: { players: [], of: 0, matches: 0, you: null } });
        assert.equal(got.you, null);
    }
    const got = page.cleanTop({ v: 1, top: {
        jk1: { period: 'month', extra: 1,
               week: { players: 'x', of: 'x', matches: -1, you: { rank: 0 } },
               month: { players: [{ id: 7, name: 'Kyle_K', score: 12.4, kills: 14, deaths: 9, matches: 2, extra: 'x' },
                                  { id: '8', name: 'A text id' }, null, 'x', { id: 0 },
                                  { id: 9, name: 'a\x07b' + 'c'.repeat(100), score: -3, kills: 'x', deaths: -1, matches: 1.9 },
                                  { id: 10, name: '', score: 'x' }],
                        of: 2, matches: 5, you: { rank: 2, score: -3, extra: 1 } },
               all: { players: Array.from({ length: 40 }, (_, i) => ({ id: i + 1, name: '<b>x</b>' })), of: 61, matches: 9 } },
        mots: { period: 'decade' },
        other: { period: 'week' },
    }, you: { id: 9, name: 'a\x07b', last: 41.5, extra: 1 } });
    assert.equal(got.games.jk1.period, 'month');
    assert.equal(got.games.mots.period, 'week', 'a period the page has no word for');
    assert.deepEqual(got.games.jk1.week, { players: [], of: 0, matches: 0, you: null });
    assert.deepEqual(got.games.jk1.month.players, [
        { id: 7, name: 'Kyle_K', score: 12, kills: 14, deaths: 9, matches: 2 },
        { id: 9, name: 'ab' + 'c'.repeat(30), score: -3, kills: 0, deaths: 0, matches: 1 },
        { id: 10, name: 'Anonymous', score: 0, kills: 0, deaths: 0, matches: 0 }]);
    assert.deepEqual([got.games.jk1.month.of, got.games.jk1.month.matches, got.games.jk1.month.you], [3, 5, { rank: 2, score: -3 }]);
    assert.equal(got.games.jk1.all.players.length, page.TOP_ROWS);
    assert.equal(got.games.jk1.all.players[0].name, '<b>x</b>', 'as text: the page never writes it as markup');
    assert.deepEqual(got.you, { id: 9, name: 'ab', last: 0 });
    assert.deepEqual(page.cleanTop({ you: { id: 9, name: 'Maw', last: 41 } }).you, { id: 9, name: 'Maw', last: 41 });
});

test("the standings shown: the game and period picked, else the game played last and the lobby's period", () => {
    const top = page.cleanTop({ top: { jk1: { period: 'month' }, mots: { period: 'all' } } });
    const recent = page.cleanRecent({ games: { jk1: [{ id: 9 }], mots: [{ id: 12 }] } });
    assert.deepEqual(page.topShown(top, recent, null, null), { game: 'mots', period: 'all' });
    assert.deepEqual(page.topShown(top, recent, 'jk1', null), { game: 'jk1', period: 'month' });
    assert.deepEqual(page.topShown(top, recent, 'jk1', 'week'), { game: 'jk1', period: 'week' });
    assert.deepEqual(page.topShown(top, recent, 'all', 'nonsense'), { game: 'mots', period: 'all' }, 'an older setting');
    assert.deepEqual(page.topShown(top, page.cleanRecent({ games: {} }), null, null), { game: 'jk1', period: 'month' });
    assert.deepEqual(page.topShown(null, null, null, null), { game: 'jk1', period: 'week' }, 'before anything has come');
});

test('places and links, as the record pages word them', () => {
    assert.deepEqual([1, 2, 3, 4, 11, 12, 13, 21, 22, 103, 111].map(page.ordinal),
                     ['1st', '2nd', '3rd', '4th', '11th', '12th', '13th', '21st', '22nd', '103rd', '111th']);
    assert.equal(page.playerHref(7, 'jk1', 'week'), '/player/7?game=jk&period=week');
    assert.equal(page.playerHref(7, 'mots', ''), '/player/7?game=mots');
    assert.equal(page.standingsHref('jk1', ''), '/players');
    assert.equal(page.standingsHref('mots', 'all'), '/players?game=mots&period=all');
});

test('who won: a player, one recorded without a name, a team, or nobody alone', () => {
    assert.equal(page.matchWinner({ name: 'Kyle_K', team: 0 }), 'Kyle_K');
    assert.equal(page.matchWinner({ name: '', team: 0 }), 'Anonymous');
    assert.equal(page.matchWinner({ name: null, team: 1 }), 'Red team');
    assert.equal(page.matchWinner({ name: null, team: 4 }), 'Green team');
    assert.equal(page.matchWinner({ name: null, team: 0 }), '');
});

test('lengths and times, worded as the match pages word them', () => {
    assert.equal(page.matchLength(45), '45 s');
    assert.equal(page.matchLength(754), '13 min');
    assert.equal(page.matchLength(7400), '2 h 3 min');
    const now = 1794300000000;
    assert.equal(page.matchAgo(now - 30000, now), 'just now');
    assert.equal(page.matchAgo(now + 5000, now), 'just now');
    assert.equal(page.matchAgo(now - 3 * 60000, now), '3 min ago');
    assert.equal(page.matchAgo(now - 7 * 3600000, now), '7 h ago');
    assert.equal(page.matchAgo(now - 9 * 86400000, now), '9 days ago');
    assert.match(page.matchAgo(now - 90 * 86400000, now), /20\d\d/);
});

test("the player's last match is offered for a month, and only if it's a match", () => {
    const now = 1794300000000;
    assert.equal(page.myMatch(JSON.stringify({ id: 40, at: now - 1000 }), now), 40);
    assert.equal(page.myMatch(JSON.stringify({ id: 40, at: now - page.MATCH_MINE_MS - 1 }), now), 0);
    for (const bad of [null, '', 'x', '{}', '{"id":"40","at":1}', '{"id":-1,"at":' + now + '}', '[40]'])
        assert.equal(page.myMatch(bad, now), 0);
});

test('the page shows nothing of the record until the lobby says it keeps one', () => {
    const marked = [...html.matchAll(/<[a-z]+[^>]*\sdata-matches(?=[\s>])[^>]*>/g)].map(m => m[0]);
    assert.ok(marked.length >= 5, 'the link, the sidebar box, the section and the notices: ' + marked.length);
    for (const tag of marked)
        assert.match(tag, /\shidden(?=[\s>])/, tag);
    // The section is right under the rooms, and the box above Go To.
    const rooms = html.indexOf('<section id="rooms"'), board = html.indexOf('<section id="leaderboard"'),
          features = html.indexOf('<section id="features"');
    assert.ok(rooms > 0 && board > rooms && features > board);
    assert.ok(!html.substring(html.indexOf('</section>', rooms) + 10, board).trim(), 'nothing between them');
    assert.ok(html.indexOf('id="side-top"') > 0 && html.indexOf('id="side-top"') < html.indexOf('<div class="goto">'));
    // A link to the section's old name still lands on it.
    assert.ok(html.indexOf('id="matches"') > board && html.indexOf('id="matches"') < features);
    // Every game and period has its button in the section, and each game one in the box.
    assert.equal((html.match(/data-top-game="jk1"/g) || []).length, 2);
    assert.equal((html.match(/data-top-game="mots"/g) || []).length, 2);
    for (const period of ['week', 'month', 'all'])
        assert.equal((html.match(new RegExp(`data-top-period="${period}"`, 'g')) || []).length, 1);
    // Where the viewer stands is said under each list that may be too short to have them.
    assert.match(html, /class="side-you" data-my-rank="5" hidden/);
    assert.match(html, /class="top-you" data-my-rank="10" hidden/);
    // What the page's code fills in is all there.
    const code = cut('      var MATCH_KEY_SETTING', '      var hideZoneBeforeLobby = hideZone;');
    for (const id of [...code.matchAll(/getElementById\('([^']+)'\)/g)].map(m => m[1]).filter(id => id !== 'zone'))
        assert.ok(html.includes(`id="${id}"`), 'the page has #' + id);
    for (const mark of [...code.matchAll(/querySelectorAll\('\[(data-[a-z-]+)\]'\)/g)].map(m => m[1]))
        assert.ok(new RegExp(`<[a-z]+[^>]*\\s${mark}(?=[\\s>=])`).test(html), 'the page has [' + mark + ']');
});

test("players' names only go into the page as text", () => {
    const code = cut('      var MATCH_KEY_SETTING', '      var hideZoneBeforeLobby = hideZone;');
    assert.ok(!/innerHTML|outerHTML|insertAdjacentHTML|document\.write/.test(code));
});

test('the key is asked for when a game starts, not by a page that only shows', () => {
    const code = cut('      var MATCH_KEY_SETTING', '      var hideZoneBeforeLobby = hideZone;');
    assert.equal(code.split('askPlayerKey()').length - 1, 2, 'its definition, and one call');
    assert.match(code, /function matchesGameStart\(\) \{\s+if \(!matchesWanted\(\)\) return;\s+askPlayerKey\(\);/);
    assert.match(html, /hideZone = function\(\) \{\s+matchesGameStart\(\);/);
});

test("a player who presses Play before the lobby's first status gets a key too", () => {
    // Wanted until a status says the lobby keeps no record: the first match
    // of someone quick isn't left without its player.
    const wanted = new Function('matchesLive', cut('      function matchesWanted()', '      // Gets this browser') +
                                'return matchesWanted();');
    assert.equal(wanted({ on: false, told: false }), true, 'no status yet');
    assert.equal(wanted({ on: true, told: true }), true);
    assert.equal(wanted({ on: false, told: true }), false, 'a lobby without a record');
    const code = cut('      function matchesStatus(status)', '      function storedPlayerKey()');
    assert.match(code, /var on = status\.match !== null;\s+matchesLive\.told = true;/);
    assert.match(cut('      function askPlayerKey()', '      // A game is starting'), /if \(!matchesWanted\(\) \|\| Module\.jkLadderAnon/);
});

test("the standings are asked for with the lobby's cookie, so they can say where the viewer stands", () => {
    assert.match(html, /fetch\('\/ladder\/recent', \{ cache: 'no-store', credentials: 'same-origin' \}\)/);
});

let failed = 0;
for (const t of tests) {
    try {
        await t.fn();
        console.log('ok   ' + t.name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + t.name + '\n' + (e && e.stack || e));
    }
}
console.log(failed ? failed + ' of ' + tests.length + ' failed' : 'all ' + tests.length + ' passed' +
            (lobby ? ", the lobby's reader included" : ''));
process.exit(failed ? 1 : 0);
