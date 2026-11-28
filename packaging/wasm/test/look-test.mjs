// Checks who's in a room on the page (index.html, look): while a room
// window is open, or the folder prompt its Join or Host opened, the page
// tells the lobby (look <game> [<name>]), with the name the game last said
// (kept while it runs) or the one typed to chat while watching; never for
// the Free Demo Room, nor from a tab hidden for LOOK_AWAY_MS; and the room
// window lists the page's own row first, as You, once the lobby says it
// counts it (looking). The lobby's side is jkdf2mp-bb-server's test/look.mjs.
//
//   node packaging/wasm/test/look-test.mjs

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from);
    assert.ok(a >= 0, 'index.html has ' + from.trim());
    const b = html.indexOf(to, a);
    assert.ok(b > a, 'index.html has ' + to.trim() + ' after ' + from.trim());
    return html.substring(a, b);
}

let passed = 0;
function test(name, fn) {
    fn();
    passed++;
    console.log('ok - ' + name);
}

const ROOMS = new Function(cut('      var ROOMS = {', "      var room = document.getElementById('room');") +
    'return ROOMS;')();
const state = cut('      var LOOK_AWAY_MS = ', '      function el(tag, cls, str) {');
const funcs = cut("      // The room this page's player is in, for look:", '      // A tab hidden for LOOK_AWAY_MS stops counting;');
const nameClean = cut('      function touchNameClean(name) {', '      (function() {');
const b64 = t => Buffer.from(t, 'utf8').toString('base64');

// The look functions on a made-up page: rooms, the folder prompt, the lobby's
// socket (lobbySay) and storage.
function page({ store = {}, roomOpen = false, roomKey = 'nar-shaddaa', pickerOpen = false, want = null } = {}) {
    const said = [];
    const env = {
        room: { open: roomOpen }, picker: { open: pickerOpen }, pickerWant: want,
        lobbyLive: { room: roomKey, status: null },
        localStorage: {
            getItem: k => (k in store ? store[k] : null),
            setItem: (k, v) => { store[k] = String(v); },
        },
        lobbySay: line => { said.push(line); return true; },
        renderRoster: () => { env.rendered = (env.rendered || 0) + 1; },
        Module: {}, rendered: 0,
    };
    const api = new Function('env', 'ROOMS', 'TOUCH_NAME_KEY', 'TOUCH_NAME_MAX', `
        var room = env.room, picker = env.picker, pickerWant = env.pickerWant, lobbyLive = env.lobbyLive;
        var localStorage = env.localStorage, lobbySay = env.lobbySay, renderRoster = env.renderRoster, Module = env.Module;
        ${state}
        ${nameClean}
        ${funcs}
        return { lookSay, lookTake, lookName, playerNameKeep, lookLive, set: function(k, v) { env[k] = v; },
                 setWant: function(w) { pickerWant = w; } };`)(env, ROOMS, 'jkdf2mp-watch-name', 16);
    return { ...api, said, env, store };
}

test('nothing is said until the lobby takes look, then the room window says its game and name', () => {
    const p = page({ roomOpen: true, store: { 'jkdf2mp-player-name': JSON.stringify({ jk1: b64('Rogue Nine') }) } });
    p.lookSay();
    assert.deepEqual(p.said, [], 'a lobby that never said look hears nothing of it');
    p.lookLive.can = true;
    p.lookSay();
    p.lookSay();
    assert.deepEqual(p.said, ['look jk1 ' + b64('Rogue Nine')], 'once, until it changes');
    p.env.room.open = false;
    p.lookSay();
    assert.deepEqual(p.said.at(-1), 'look', 'closed: none');
});

test("the name: this game's, else the other game's, else the one typed to chat, else none", () => {
    const store = { 'jkdf2mp-player-name': JSON.stringify({ mots: b64('Dash') }) };
    const p = page({ store });
    assert.equal(p.lookName('mots'), b64('Dash'));
    assert.equal(p.lookName('jk1'), b64('Dash'), "the other game's");
    store['jkdf2mp-player-name'] = 'not json';
    store['jkdf2mp-watch-name'] = '  Kyle   K  ';
    assert.equal(p.lookName('jk1'), b64('Kyle K'), 'the typed one, cleaned');
    delete store['jkdf2mp-watch-name'];
    store['jkdf2mp-player-name'] = JSON.stringify({ jk1: 'not base64!' });
    assert.equal(p.lookName('jk1'), '');
});

test("the folder prompt from a room's Join or Host keeps it in the room; watching and the demo don't count", () => {
    const p = page({ pickerOpen: true, want: { game: 'mots', lobbyRoom: 'sith-asteroid' } });
    p.lookLive.can = true;
    p.lookSay();
    assert.deepEqual(p.said, ['look mots']);
    p.setWant({ game: 'mots', lobbyRoom: 'sith-asteroid', watch: true });
    p.lookSay();
    assert.equal(p.said.at(-1), 'look', 'going to watch in the game');
    const d = page({ roomOpen: true, roomKey: 'bespin-demo' });
    d.lookLive.can = true;
    d.lookSay();
    assert.deepEqual(d.said, [], 'the Free Demo Room lists no one Looking: nothing to say');
});

test('a tab hidden for LOOK_AWAY_MS stops counting', () => {
    const p = page({ roomOpen: true });
    p.lookLive.can = true;
    p.lookLive.hiddenAt = Date.now() - 60 * 1000;
    p.lookSay();
    assert.deepEqual(p.said, ['look jk1'], 'a minute hidden still counts');
    p.lookLive.hiddenAt = Date.now() - 16 * 60 * 1000;
    p.lookSay();
    assert.equal(p.said.at(-1), 'look');
    p.lookLive.hiddenAt = 0;
    p.lookSay();
    assert.equal(p.said.at(-1), 'look jk1', 'shown again');
});

test("the lobby's looking sets the page's own row", () => {
    const p = page({ roomOpen: true });
    p.env.lobbyLive.status = {};
    p.lookTake('looking jk1 ' + b64('Rogue Nine'));
    assert.deepEqual([p.lookLive.game, p.lookLive.name], ['jk1', 'Rogue Nine']);
    assert.equal(p.env.rendered, 1, 'the window shows it at once');
    p.lookTake('looking mots');
    assert.deepEqual([p.lookLive.game, p.lookLive.name], ['mots', '']);
    p.lookTake('looking off');
    assert.deepEqual([p.lookLive.game, p.lookLive.name], ['', '']);
});

test("while the game runs, the name its hello says is kept for each game", () => {
    const timers = [];
    const realSet = globalThis.setInterval;
    globalThis.setInterval = fn => { timers.push(fn); return 1; };
    try {
        const p = page();
        p.playerNameKeep();
        p.playerNameKeep();
        assert.equal(timers.length, 1, 'one timer');
        timers[0]();
        assert.equal(p.store['jkdf2mp-player-name'], undefined, 'no hello yet');
        p.env.Module.jkNet = { helloSent: 'hello mots ' + b64('Dash') };
        timers[0]();
        p.env.Module.jkNet.helloSent = 'hello jk1 ' + b64('Kyle');
        timers[0]();
        assert.deepEqual(JSON.parse(p.store['jkdf2mp-player-name']), { mots: b64('Dash'), jk1: b64('Kyle') });
    } finally {
        globalThis.setInterval = realSet;
    }
});

test("the room window lists the page's own row first, and not again among the others", () => {
    const roster = cut('        // This page, once the lobby counts it in this room (lookTake)', "        section('Total players', String(s.online));");
    const rows = (lookLive, s, inGames) => new Function('lookLive', 'ROOMS', 'lobbyLive', 's', 'inGames',
        'var playing = [];\n' + roster + 'return [inLobby, lobbyRows];')(lookLive, ROOMS, { room: 'nar-shaddaa' }, s, inGames);
    const s = { online: 5, lobby: ['Boc', 'Rogue Nine', 'Yun'] };
    assert.deepEqual(rows({ game: 'jk1', name: 'Rogue Nine' }, s, 1),
        [4, [['Rogue Nine', 'You', false, true], ['Boc'], ['Yun'], ['+1 without a name', null, true]]]);
    assert.deepEqual(rows({ game: 'jk1', name: '' }, { online: 4, lobby: ['Boc', 'Yun'] }, 1),
        [3, [['You', null, false, true], ['Boc'], ['Yun']]], 'without a name, one of the nameless is this page');
    assert.deepEqual(rows({ game: 'mots', name: 'Rogue Nine' }, s, 1),
        [4, [['Boc'], ['Rogue Nine'], ['Yun'], ['+1 without a name', null, true]]], "another game's room: as before");
    assert.deepEqual(rows({ game: '', name: '' }, { online: 2, lobby: ['Boc'] }, 0),
        [2, [['Boc'], ['+1 without a name', null, true]]], 'not counted: as before');
});

console.log(`all ${passed} passed`);
