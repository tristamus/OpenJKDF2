// Tests for the Imperial relay's games in the room windows of
// packaging/wasm/index.html: what the page keeps of the site's Worker's
// answer (/imperial-games), the order the room window lists games in, and
// when the page asks the Worker. In Node: node packaging/wasm/test/imperial-lobby-test.mjs
//
// The code is taken from index.html as it is; the Worker, the clock and
// the document are made up by the test.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to, a);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const code = cut('      var IMPERIAL_POLL_MS = 20000;', "      // Takes the lobby's latest status, or null with why there's none:");
assert.match(html, /var IMPERIAL_LOBBY_LIST = true;/, 'the page lists the relay\'s games');

// The real Worker's answer on 31 Oct 2026, from the real relay.
const CORUSCANT = { key: '13e5dd93743f59055bbf297508845b2b', name: 'Coruscant', game: 'jk1', map: 'c3.jkl', episode: 'JK1CTF',
                    mod: '', locked: false, players: 0, max: 31, dedicated: true, pinned: true, listed: true,
                    joinable: true, why: '' };
const LIVE = { state: 'live', server: 'lobby.jkjunkie.com', checked: '2026-10-31T19:20:01.000Z', games: [CORUSCANT] };

// The page's code with made-up surroundings. fetch answers with what
// worker() returns: a { status, body } or an Error to throw.
function page({ url = 'lobby.jkjunkie.com', listed = true, protocol = 'https:', worker = () => ({ status: 200, body: LIVE }) } = {}) {
    const timers = [];
    const listeners = {};
    const shown = [];
    const env = {
        NETCODE: 0, IMPERIAL_LOBBY_LIST: listed, Module: url ? { jkImperialUrl: url } : {},
        location: { protocol }, room: { open: true }, asks: 0,
        document: { hidden: false, addEventListener(type, fn) { listeners[type] = fn; } },
        fetch(path, opts) {
            env.asks++;
            assert.equal(path, '/imperial-games');
            assert.equal(opts.cache, 'no-store');
            const respond = r => {
                if (r instanceof Error) return Promise.reject(r);
                return Promise.resolve({ status: r.status, ok: r.status >= 200 && r.status < 300,
                                         json: () => typeof r.body === 'string' ? Promise.reject(new SyntaxError('JSON')) :
                                                                                  Promise.resolve(r.body) });
            };
            const r = worker();
            return r instanceof Promise ? r.then(respond) : respond(r);
        },
        setTimeout(fn, ms) { const t = { fn, ms, id: timers.length + 1 }; timers.push(t); return t.id; },
        clearTimeout(id) { const i = timers.findIndex(t => t.id === id); if (i >= 0) timers.splice(i, 1); },
        renderRoomCount() {}, renderRoom() { shown.push(JSON.parse(JSON.stringify(env.p.imperialLive))); }
    };
    const names = Object.keys(env).filter(k => k !== 'asks' && k !== 'p');
    env.p = new Function(...names, code + '\nreturn { cleanImperial, roomGameList, imperialStop, imperialShow, ' +
                         'startImperialWatch, stopImperialWatch, imperialLive, watch: () => imperialWatch, IMPERIAL_STATE };')(
        ...names.map(k => env[k]));
    env.timers = timers;
    env.shown = shown;
    env.visibility = () => listeners.visibilitychange();
    // Runs the next timer, as if its time had come.
    env.tick = () => { const t = timers.shift(); assert.ok(t, 'a timer is set'); t.fn(); return t.ms; };
    return env;
}
const settle = async () => { for (let i = 0; i < 5; i++) await new Promise(r => setImmediate(r)); };

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

test("the real answer: Coruscant, joinable, credited to the relay's operator", () => {
    const { p } = page();
    const a = p.cleanImperial(LIVE);
    assert.equal(a.state, 'live');
    assert.equal(a.server, 'lobby.jkjunkie.com');
    assert.deepEqual(a.games, [{
        uid: 'i13e5dd93743f59055bbf297508845b2b', key: '13e5dd93743f59055bbf297508845b2b', imperial: true,
        name: 'Coruscant', game: 'jk1', map: 'c3.jkl', episode: 'JK1CTF', mod: '', locked: false, players: 0, max: 31,
        names: [], net: 0, dedicated: true, pinned: true, listed: true, joinable: true, why: '', runBy: 'sithmartyr' }]);
    assert.equal(p.imperialStop(a.games[0]), null);
});

test('a pinned server that isn\'t running, and the relay down', () => {
    const { p } = page();
    const off = { key: '', name: 'Coruscant', game: 'jk1', map: '', episode: '', mod: '', locked: false, players: 0, max: 0,
                  dedicated: true, pinned: true, listed: false, joinable: false, why: 'not-listed' };
    const a = p.cleanImperial({ ...LIVE, games: [off] });
    assert.deepEqual([a.games[0].uid, a.games[0].listed, a.games[0].joinable, p.imperialStop(a.games[0])],
                     ['ipin0', false, false, 'Not running right now']);
    const d = p.cleanImperial({ ...LIVE, state: 'down', games: [{ ...off, why: 'relay-down' }] });
    assert.equal(d.state, 'down');
    assert.equal(p.imperialStop(d.games[0]), 'Imperial relay not answering');
});

test('why each game has no Join', () => {
    const { p } = page();
    const g = why => p.cleanImperial({ ...LIVE, games: [{ ...CORUSCANT, pinned: false, joinable: false, why }] }).games[0];
    assert.equal(p.imperialStop(g('desktop')), 'Desktop OpenJKDF2 only');
    assert.equal(p.imperialStop(g('closed')), 'Not taking players');
    assert.equal(p.imperialStop(g('')), 'Not taking players');
    assert.equal(p.imperialStop(g('toString')), 'Not taking players');
    // A why with joinable left on still means no Join.
    assert.equal(p.cleanImperial({ ...LIVE, games: [{ ...CORUSCANT, why: 'desktop' }] }).games[0].joinable, false);
    // Lobby games are the lobby's business.
    assert.equal(p.imperialStop({ imperial: false, joinable: false }), null);
});

test('whatever arrives, only clean text and numbers, and no credit for others', () => {
    const { p } = page();
    assert.equal(p.cleanImperial(null), null);
    assert.equal(p.cleanImperial({ state: 'maybe', games: [] }), null);
    assert.equal(p.cleanImperial('live'), null);
    const a = p.cleanImperial({ state: 'live', server: 'other.example', games: [
        { ...CORUSCANT, name: 'Co\u0000rus\u001bcant'.padEnd(80, 'x'), players: -3, max: 1e9, map: 7, game: 'jk2' },
        { ...CORUSCANT, key: '13E5DD93743F59055BBF297508845B2B' }, null, 'game',
        ...Array.from({ length: 40 }, () => CORUSCANT)] });
    assert.equal(a.games.length, 32 - 2);
    const [g, upper] = a.games;
    assert.deepEqual([g.name.length, g.name.startsWith('Coruscant'), g.players, g.max, g.map, g.game, g.runBy],
                     [32, true, 0, 64, '', 'jk1', '']);
    assert.deepEqual([upper.key, upper.listed, upper.joinable], ['', false, false]);
    // The credit goes only with a relay the page knows, and only on its pinned servers.
    assert.equal(p.cleanImperial({ ...LIVE, games: [{ ...CORUSCANT, pinned: false }] }).games[0].runBy, '');
    assert.equal(p.cleanImperial({ ...LIVE, server: '__proto__' }).games[0].runBy, '');
});

test('a room lists the pinned servers, then the lobby\'s games, then the relay\'s', () => {
    const { p } = page();
    const relay = p.cleanImperial({ ...LIVE, games: [
        CORUSCANT,
        { ...CORUSCANT, key: 'a'.repeat(32), name: 'Hosted', dedicated: false, pinned: false },
        { ...CORUSCANT, key: 'b'.repeat(32), name: 'Sith game', game: 'mots', dedicated: false, pinned: false }] }).games;
    const lobby = [{ id: 41, name: 'No mods dueling', game: 'jk1' }, { id: 47, name: 'CTF practice', game: 'jk1' }];
    assert.deepEqual(p.roomGameList(lobby, relay, 'jk1').map(g => [g.uid, g.name, g.imperial]), [
        ['i13e5dd93743f59055bbf297508845b2b', 'Coruscant', true], ['g41', 'No mods dueling', false],
        ['g47', 'CTF practice', false], ['i' + 'a'.repeat(32), 'Hosted', true]]);
    assert.deepEqual(p.roomGameList([], relay, 'mots').map(g => g.name), ['Sith game']);
    // The lobby's own games aren't changed.
    assert.equal('uid' in lobby[0], false);
});

test('asks the Worker on start, then every 20 s', async () => {
    const env = page();
    env.p.startImperialWatch();
    assert.equal(env.p.imperialLive.state, 'connecting');
    await settle();
    assert.equal(env.asks, 1);
    assert.equal(env.p.imperialLive.state, 'live');
    assert.deepEqual(env.shown.at(-1).games.map(g => g.name), ['Coruscant']);
    assert.equal(env.tick(), 20000);
    await settle();
    assert.equal(env.asks, 2);
    assert.equal(env.timers.length, 1);
    // Starting again doesn't make a second watch.
    env.p.startImperialWatch();
    await settle();
    assert.equal(env.asks, 2);
});

for (const [what, opts] of [['no Imperial connection', { url: '' }], ['IMPERIAL_LOBBY_LIST false', { listed: false }],
                            ['a page opened from a file', { protocol: 'file:' }]]) {
    test(what + ': never asks', async () => {
        const env = page(opts);
        env.p.startImperialWatch();
        await settle();
        assert.equal(env.asks, 0);
        assert.equal(env.timers.length, 0);
        assert.equal(env.p.imperialLive.state, 'off');
    });
}

for (const [what, answer] of [['the Worker\'s IMPERIAL_LIST empty (404)', { status: 404, body: 'Not found' }],
                              ['another relay than the engine\'s', { status: 200, body: { ...LIVE, server: 'other.example' } }],
                              ['an answer that isn\'t one', { status: 200, body: null }]]) {
    test(what + ': nothing shown, and it stops asking', async () => {
        const env = page({ worker: () => answer });
        env.p.startImperialWatch();
        await settle();
        assert.equal(env.p.imperialLive.state, 'off');
        assert.deepEqual(env.p.imperialLive.games, []);
        assert.equal(env.timers.length, 0);
        assert.equal(env.p.watch().stopped, true);
    });
}

test('one failed ask keeps the games, two hide them, and it asks less often', async () => {
    let answer = { status: 200, body: LIVE };
    const env = page({ worker: () => answer });
    env.p.startImperialWatch();
    await settle();
    answer = new TypeError('Failed to fetch');
    env.tick();
    await settle();
    assert.equal(env.p.imperialLive.state, 'live');
    assert.equal(env.p.imperialLive.games.length, 1);
    answer = { status: 502, body: 'Bad gateway' };
    assert.equal(env.tick(), 20000);
    await settle();
    assert.equal(env.p.imperialLive.state, 'error');
    assert.deepEqual(env.p.imperialLive.games, []);
    answer = { status: 200, body: 'not json' };
    assert.equal(env.tick(), 40000);
    await settle();
    assert.equal(env.tick(), 80000);
    await settle();
    answer = { status: 200, body: LIVE };
    assert.equal(env.tick(), 160000);
    await settle();
    assert.equal(env.p.imperialLive.state, 'live');
    assert.equal(env.tick(), 20000);
});

test('a hidden page doesn\'t ask until it shows again', async () => {
    const env = page();
    env.p.startImperialWatch();
    await settle();
    env.document.hidden = true;
    env.tick();
    await settle();
    assert.equal(env.asks, 1);
    assert.equal(env.timers.length, 0);
    env.document.hidden = false;
    env.visibility();
    await settle();
    assert.equal(env.asks, 2);
    // Shown again while an ask is due: it waits for it.
    env.visibility();
    await settle();
    assert.equal(env.asks, 2);
    assert.equal(env.timers.length, 1);
});

test('stopped (the game starts): no more asks, even with one under way', async () => {
    let answer = { status: 200, body: LIVE };
    const env = page({ worker: () => answer });
    env.p.startImperialWatch();
    await settle();
    env.p.stopImperialWatch();
    assert.equal(env.timers.length, 0);
    env.visibility();
    await settle();
    assert.equal(env.asks, 1);

    // The answer to an ask made before it stopped is let go.
    let release;
    answer = new Promise(r => { release = r; });
    const busy = page({ worker: () => answer });
    busy.p.startImperialWatch();
    busy.p.stopImperialWatch();
    release({ status: 200, body: LIVE });
    await settle();
    assert.equal(busy.p.imperialLive.state, 'connecting');
    assert.equal(busy.shown.length, 0);
    assert.equal(busy.timers.length, 0);
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
