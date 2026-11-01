// Tests for the room list in packaging/wasm/index.html: every room opens a
// room window, Jedi Knight's rooms Nar Shaddaa's and the Sith rooms Sith:
// Asteroid's, since the lobby keeps one room for each game. In Node:
// node packaging/wasm/test/rooms-test.mjs
//
// The room list and the code that wires it up are taken from index.html as
// they are; the document and the room window are made up by the test.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to, a);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const ROOMS = new Function(cut('      var ROOMS = {', "      var room = document.getElementById('room');") +
    'return ROOMS;')();
const wiring = cut("      document.querySelectorAll('[data-room], [data-opens-room]')",
                   "      if (typeof ResizeObserver === 'function') {");

// Each room button in the list, under its heading: its name, its data-
// attributes and whether it has the count spans the live counts fill in.
function roomButtons() {
    const list = cut('<section id="rooms"', '</section>');
    const out = [];
    let heading = '';
    for (const m of list.matchAll(/<h3>([^<]*)<\/h3>|<button([^>]*)>([\s\S]*?)<\/button>/g)) {
        if (m[1] !== undefined) { heading = m[1]; continue; }
        const dataset = {};
        for (const a of m[2].matchAll(/data-([a-z-]+)(?:="([^"]*)")?/g))
            dataset[a[1].replace(/-([a-z])/g, (_, c) => c.toUpperCase())] = a[2] ?? '';
        const nameTag = /<span class="room-name">([^<]*)<\/span>/.exec(m[3]);
        out.push({ heading, dataset, haspopup: /aria-haspopup="dialog"/.test(m[2]),
                   name: nameTag ? nameTag[1] : m[3].trim(),
                   count: m[3].includes('class="count'), gameCount: m[3].includes('game-count') });
    }
    return out;
}

// The page's click wiring run on made-up buttons: the room each one opens.
function clicked(buttons) {
    const opened = [];
    const els = buttons.map(b => ({
        dataset: b.dataset, listeners: {},
        addEventListener(type, fn) { this.listeners[type] = fn; },
        matches(sel) {
            return sel.split(',').some(s => {
                const key = s.trim().replace(/^\[data-|\]$/g, '').replace(/-([a-z])/g, (_, c) => c.toUpperCase());
                return key in this.dataset;
            });
        }
    }));
    const document = { querySelectorAll: sel => els.filter(e => e.matches(sel)) };
    new Function('document', 'openRoom', 'room', 'roomGames', wiring)(document, key => opened.push(key), {}, {});
    return els.map((e, i) => {
        opened.length = 0;
        if (e.listeners.click) e.listeners.click();
        return { name: buttons[i].name, opened: opened.slice() };
    });
}

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

const GAME_OF_HEADING = { 'Standard Rooms': 'jk1', 'Capture the Flag Rooms': 'jk1', 'Mysteries of the Sith Rooms': 'mots' };
const MAIN_ROOM = { jk1: 'nar-shaddaa', mots: 'sith-asteroid' };

test('the list has the rooms it always had', () => {
    assert.deepEqual(roomButtons().map(b => b.name), [
        'Nar Shaddaa', 'The Cantina', 'Sulon', 'Bespin', 'Canyon Oasis', 'Dark Palace',
        'CTF Room 1', 'CTF Room 2', 'Sith: Asteroid', 'Sith: Altyr 5']);
});

test("every room opens its game's room window", () => {
    const buttons = roomButtons();
    const opens = clicked(buttons);
    buttons.forEach((b, i) => {
        const game = GAME_OF_HEADING[b.heading];
        assert.ok(game, b.name + ' is under a known heading');
        assert.deepEqual(opens[i].opened, [MAIN_ROOM[game]], b.name + ' opens ' + MAIN_ROOM[game]);
        assert.equal(ROOMS[MAIN_ROOM[game]].game, game, MAIN_ROOM[game] + ' is ' + game + "'s room");
        assert.ok(b.haspopup, b.name + ' says it opens a window');
        assert.ok(!('play' in b.dataset), b.name + ' does not skip the window for the folder prompt');
    });
});

test('only the two main rooms show live counts', () => {
    const counted = roomButtons().filter(b => 'room' in b.dataset);
    assert.deepEqual(counted.map(b => b.dataset.room), ['nar-shaddaa', 'sith-asteroid']);
    // renderRoomCount fills in both spans on every [data-room] button.
    counted.forEach(b => assert.ok(b.count && b.gameCount, b.name + ' has the count spans'));
    roomButtons().filter(b => 'opensRoom' in b.dataset).forEach(b =>
        assert.ok(b.dataset.opensRoom in ROOMS, b.name + ' opens a room the page has'));
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
