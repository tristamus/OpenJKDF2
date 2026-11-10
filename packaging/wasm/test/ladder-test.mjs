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
        jkNet: { ws: { send: line => sent.push(line) }, state: 1, session: 3, seat: 'k', lobbyCan: can },
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
    'return { cleanRecent, matchId, matchList, matchWinner, matchLength, matchAgo, myMatch, MATCH_ROWS, MATCH_MINE_MS };')(GAME_NAMES);

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
    assert.deepEqual(page.matchList(recent, 'all', page.MATCH_ROWS).map(m => m.id), [9, 8, 7, 5, 4, 3]);
    assert.deepEqual(page.matchList(recent, 'all', 3).map(m => [m.id, m.game]), [[9, 'jk1'], [8, 'mots'], [7, 'jk1']]);
    assert.deepEqual(page.matchList(recent, 'mots', 6).map(m => m.id), [8, 5]);
    assert.deepEqual(page.matchList(recent, 'jk1', 2).map(m => m.id), [9, 7]);
    assert.deepEqual(page.matchList(null, 'all', 6), []);
    assert.deepEqual(page.matchList(recent, 'nonsense', 6), []);
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
    const rooms = html.indexOf('<section id="rooms"'), matches = html.indexOf('<section id="matches"'),
          features = html.indexOf('<section id="features"');
    assert.ok(rooms > 0 && matches > rooms && features > matches);
    assert.ok(!html.substring(html.indexOf('</section>', rooms) + 10, matches).trim(), 'nothing between them');
    assert.ok(html.indexOf('id="side-matches"') < html.indexOf('<div class="goto">'));
});

test("players' names only go into the page as text", () => {
    const code = cut('      var MATCH_KEY_SETTING', '      var hideZoneBeforeLobby = hideZone;');
    assert.ok(!/innerHTML|outerHTML|insertAdjacentHTML|document\.write/.test(code));
});

test('the key is asked for when a game starts, not by a page that only shows', () => {
    const code = cut('      var MATCH_KEY_SETTING', '      var hideZoneBeforeLobby = hideZone;');
    assert.equal(code.split('askPlayerKey()').length - 1, 2, 'its definition, and one call');
    assert.match(code, /function matchesGameStart\(\) \{\s+if \(!matchesLive\.on\) return;\s+askPlayerKey\(\);/);
    assert.match(html, /hideZone = function\(\) \{\s+matchesGameStart\(\);/);
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
