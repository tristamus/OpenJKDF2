// Tests for the room list in packaging/wasm/index.html: every room opens a
// room window, Jedi Knight's rooms Nar Shaddaa's and the Sith rooms Sith:
// Asteroid's, since the lobby keeps one room for each game. And for how many
// tables a room window shows: at least nine, more as games fill them. In Node:
// node packaging/wasm/test/rooms-test.mjs
//
// The room list and the code that wires it up are taken from index.html as
// they are; the document and the room window are made up by the test.

import { readFileSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
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
    // (The pictures' idle fetch waits for a load event that never comes.)
    const window = { addEventListener() {} };
    new Function('document', 'openRoom', 'room', 'roomGames', 'window', wiring)(document, key => opened.push(key), {}, {}, window);
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

// The room window's tables: the games' and the open ones fillTables adds,
// with the page's tableSlots and ROOM_MIN_TABLES, on a made-up grid. It has
// cols columns, in a room body laid out as the page's CSS would (stacked: a
// column, who's on under the tables, as on a phone).
const minTables = Number(/var ROOM_MIN_TABLES = (\d+);/.exec(html)?.[1]);
const fill = cut('      function tableSlots(', '      function gameList(');
function makeRoom(n) {
    const children = Array.from({ length: n }, () => ({ className: 'table' }));
    const ul = {
        children,
        querySelectorAll: sel => {
            assert.equal(sel, '.table.empty');
            return children.filter(c => c.className === 'table empty').map(c => ({
                remove() { children.splice(children.indexOf(c), 1); }
            }));
        },
        append(c) { children.push(c); }
    };
    const body = {};
    const room = { cols: 3, stacked: false, children, lobbyLive: { cols: 0, stacked: false } };
    const getComputedStyle = e => e === ul ? { gridTemplateColumns: Array(room.cols).fill('208px').join(' ') } :
                                 e === body ? { flexDirection: room.stacked ? 'column' : 'row' } : assert.fail('styles of what?');
    room.fillTables = new Function('ROOM_MIN_TABLES', 'roomGames', 'getComputedStyle', 'emptyTable', 'lobbyLive',
                                   fill + 'return fillTables;')(
        minTables, { querySelector: sel => sel === '.tables' ? ul : null, parentNode: body }, getComputedStyle,
        i => ({ className: 'table empty', i }), room.lobbyLive);
    room.count = () => {
        const open = children.filter(c => c.className === 'table empty');
        open.forEach((c, k) => assert.equal(c.i, children.length - open.length + k,
                                            'open tables are numbered on from the games'));
        return { tables: children.length, open: open.length };
    };
    return room;
}
function fillRoom(n, cols, stacked = false) {
    const room = makeRoom(n);
    room.cols = cols;
    room.stacked = stacked;
    room.fillTables(true);
    return room.count();
}

test('a room shows nine tables on a computer, however few games there are', () => {
    assert.equal(minTables, 9);
    for (let n = 0; n <= 8; n++)
        assert.deepEqual(fillRoom(n, 3), { tables: 9, open: 9 - n }, n + ' games in 3 columns');
});

test('the ninth game opens another row of three, and so on', () => {
    assert.deepEqual(fillRoom(9, 3), { tables: 12, open: 3 });
    assert.deepEqual(fillRoom(11, 3), { tables: 12, open: 1 });
    assert.deepEqual(fillRoom(12, 3), { tables: 15, open: 3 });
    assert.deepEqual(fillRoom(14, 3), { tables: 15, open: 1 });
    assert.deepEqual(fillRoom(15, 3), { tables: 18, open: 3 });
});

test('narrower windows beside who\'s on fill their last row, at least nine tables', () => {
    assert.deepEqual(fillRoom(0, 2), { tables: 10, open: 10 });
    assert.deepEqual(fillRoom(9, 2), { tables: 10, open: 1 });
    assert.deepEqual(fillRoom(10, 2), { tables: 12, open: 2 });
    assert.deepEqual(fillRoom(0, 1), { tables: 9, open: 9 });
    assert.deepEqual(fillRoom(9, 1), { tables: 10, open: 1 });
});

test("a phone, who's on under the tables, keeps one open table after the games as before", () => {
    // The rule before nine tables: fill out the last row, always one open.
    const before = (n, cols) => Math.max(cols, Math.ceil((n + 1) / cols) * cols);
    for (const cols of [1, 2, 3])
        for (let n = 0; n <= 12; n++)
            assert.deepEqual(fillRoom(n, cols, true), { tables: before(n, cols), open: before(n, cols) - n },
                             n + ' games in ' + cols + ' columns');
});

test('a redraw replaces the open tables, and a resize redraws them for the new layout', () => {
    const room = makeRoom(1);
    room.fillTables(true);
    room.fillTables(true);
    assert.deepEqual(room.count(), { tables: 9, open: 8 }, 'still nine after a second fill');
    room.fillTables();
    assert.deepEqual(room.count(), { tables: 9, open: 8 }, 'nothing changed, nothing redrawn');
    room.cols = 2;
    room.fillTables();
    assert.deepEqual(room.count(), { tables: 10, open: 9 }, 'two columns');
    room.stacked = true;
    room.fillTables();
    assert.deepEqual(room.count(), { tables: 2, open: 1 }, 'the phone layout, still two columns');
    room.cols = 1;
    room.fillTables();
    assert.deepEqual(room.count(), { tables: 2, open: 1 }, 'one column on a phone');
    room.stacked = false;
    room.cols = 3;
    room.fillTables();
    assert.deepEqual(room.count(), { tables: 9, open: 8 }, 'back to a computer');
});

// The page's resize wiring, on a made-up observer and frames.
test('a resize redraws the tables on the next frame, not while the observer reports it', () => {
    const wire = cut("      if (typeof ResizeObserver === 'function') {",
                     '      // The page watches the lobby while');
    let observer = null, observed = null, frames = [], fills = 0;
    function ResizeObserver(fn) { observer = fn; this.observe = e => { observed = e; }; }
    const requestAnimationFrame = fn => frames.push(fn);
    const room = { open: true }, roomGames = {};
    new Function('ResizeObserver', 'requestAnimationFrame', 'room', 'roomGames', 'fillTables', wire)(
        ResizeObserver, requestAnimationFrame, room, roomGames, () => fills++);
    assert.equal(observed, roomGames, 'it watches the tables\' box');
    observer([]);
    observer([]);
    assert.equal(fills, 0, 'nothing redrawn while it reports');
    assert.equal(frames.length, 1, 'one frame asked for, however many reports');
    frames.shift()();
    assert.equal(fills, 1, 'redrawn on the frame');
    observer([]);
    room.open = false;
    frames.shift()();
    assert.equal(fills, 1, 'not redrawn once the window has closed');
    observer([]);
    assert.equal(frames.length, 0, 'a closed window asks for no frame');
});

// ---- Room pictures: files beside the page, named by their content, asked
// for with the page when the address names a room, and when a player points
// at a room.

const pictures = new Function(cut('      var ROOM_PICTURES = {', '      (function() {') + 'return ROOM_PICTURES;')();

test('each room picture is a file named by its content, the same in the styles', () => {
    assert.deepEqual(Object.keys(pictures).sort(), Object.keys(ROOMS).sort());
    assert.ok(!/data:image\/(webp|jpeg|png)/.test(html), 'no picture left in the page');
    for (const [key, name] of Object.entries(pictures)) {
        const css = new RegExp('\\.room-games\\[data-room-key="' + key + '"\\] \\{[^}]*--backdrop: url\\("([^"]+)"\\)').exec(html);
        assert.ok(css, key + ' has a picture in the styles');
        assert.equal(css[1], name, key + ': the styles and ROOM_PICTURES name the same file');
        const m = /^rooms\/[a-z-]+\.([0-9a-f]{8})\.(webp|jpg)$/.exec(name);
        assert.ok(m, name + ' is rooms/<room>.<hash>.<type>');
        const file = new URL('../' + name, import.meta.url);
        assert.ok(existsSync(file), name + ' is there');
        assert.equal(createHash('sha256').update(readFileSync(file)).digest('hex').slice(0, 8), m[1], name + "'s name matches its content");
    }
});

test("a link to a room window asks for that room's picture with the page, and nothing else does", () => {
    const head = cut('      var ROOM_PICTURES = {', '    </script>\n    <style>\n      body {');
    for (const [hash, want] of [['#nar-shaddaa', [pictures['nar-shaddaa']]], ['#sith-asteroid', [pictures['sith-asteroid']]],
                                ['', []], ['#rooms', []], ['#bundled', []], ['#__proto__', []], ['#toString', []]]) {
        const links = [];
        const document = { createElement: () => ({}), head: { appendChild: l => links.push(l) } };
        new Function('document', 'location', head)(document, { hash });
        assert.deepEqual(links.map(l => l.href), want, hash || '(none)');
        links.forEach(l => { assert.equal(l.rel, 'preload'); assert.equal(l.as, 'image'); });
    }
});

// The room buttons' wiring and the pictures' idle fetch, as the page has
// them, in a made-up page that is still loading (or has loaded), with or
// without Save-Data and requestIdleCallback. Nothing runs by itself: the
// test fires the load event, the idle callback and the timers.
function roomPictureWiring({ readyState = 'loading', saveData, idle = true } = {}) {
    const code = cut('      var roomPicturesAsked = {};', "      if (typeof ResizeObserver === 'function') {");
    const asked = [];
    class Image { set src(v) { asked.push({ src: v, priority: this.fetchPriority }); } }
    const els = roomButtons().map(b => ({ dataset: b.dataset, listeners: {}, addEventListener(type, fn) { this.listeners[type] = fn; } }));
    const document = { querySelectorAll: () => els, readyState };
    const loads = [], idles = [], timers = [];
    const window = { addEventListener(type, fn) { if (type === 'load') loads.push(fn); } };
    const navigator = saveData === undefined ? {} : { connection: { saveData } };
    const requestIdleCallback = idle ? fn => idles.push(fn) : undefined;
    const setTimeout = (fn, ms) => timers.push({ fn, ms });
    new Function('document', 'openRoom', 'Image', 'ROOM_PICTURES', 'window', 'navigator', 'requestIdleCallback', 'setTimeout', code)(
        document, () => {}, Image, pictures, window, navigator, requestIdleCallback, setTimeout);
    const run = list => list.splice(0).forEach(f => (f.fn || f)());
    return { els, asked, loads, idles, timers, srcs: () => asked.map(a => a.src),
             load: () => run(loads), idle: () => run(idles), timer: () => run(timers) };
}

test("pointing at a room, tabbing to it or touching it asks for its picture once", () => {
    const w = roomPictureWiring(), els = w.els;
    const jk = els.filter(e => (e.dataset.room || e.dataset.opensRoom) === 'nar-shaddaa');
    const mots = els.filter(e => (e.dataset.room || e.dataset.opensRoom) === 'sith-asteroid');
    jk[0].listeners.pointerenter();
    jk[1].listeners.focus();
    jk[2].listeners.touchstart();
    assert.deepEqual(w.srcs(), [pictures['nar-shaddaa']], 'once for Nar Shaddaa, from any of its rooms');
    mots[1].listeners.pointerenter();
    assert.deepEqual(w.srcs(), [pictures['nar-shaddaa'], pictures['sith-asteroid']]);
    assert.deepEqual(w.asked.map(a => a.priority), [undefined, undefined], 'at the usual priority');
    els.forEach(e => assert.equal(typeof e.listeners.click, 'function', 'each still opens its window'));
    w.load();
    w.idle();
    assert.equal(w.asked.length, 2, 'the idle fetch asks for neither again');
});

test('once the page has loaded and the browser is idle, both pictures are asked for at low priority', () => {
    let w = roomPictureWiring();
    assert.equal(w.loads.length, 1, 'waits for the load event');
    assert.equal(w.idles.length + w.timers.length + w.asked.length, 0, 'nothing before it');
    w.load();
    assert.equal(w.idles.length, 1, 'then for the browser to be idle');
    assert.equal(w.asked.length, 0);
    w.idle();
    assert.deepEqual(w.asked, Object.values(pictures).map(src => ({ src, priority: 'low' })));
    w.els.forEach(e => { e.listeners.pointerenter(); e.listeners.focus(); });
    assert.equal(w.asked.length, 2, 'and never again');

    // One pointed at before the browser was idle isn't asked for twice.
    w = roomPictureWiring();
    w.els.find(e => (e.dataset.room || e.dataset.opensRoom) === 'sith-asteroid').listeners.pointerenter();
    w.load();
    w.idle();
    assert.deepEqual(w.srcs(), [pictures['sith-asteroid'], pictures['nar-shaddaa']]);

    // A page that has already loaded asks for the idle time at once.
    w = roomPictureWiring({ readyState: 'complete' });
    assert.equal(w.loads.length, 0);
    w.idle();
    assert.equal(w.asked.length, 2);

    // No requestIdleCallback (Safari): a second after the load event.
    w = roomPictureWiring({ idle: false });
    w.load();
    assert.equal(w.timers.length, 1);
    assert.equal(w.timers[0].ms, 1000);
    w.timer();
    assert.equal(w.asked.length, 2);

    // Save-Data off is the same as no Save-Data.
    w = roomPictureWiring({ saveData: false });
    w.load();
    w.idle();
    assert.equal(w.asked.length, 2);
});

test('with Save-Data on, only a room pointed at asks for its picture', () => {
    const w = roomPictureWiring({ saveData: true });
    w.load();
    assert.equal(w.idles.length + w.timers.length, 0, 'no idle fetch');
    assert.equal(w.asked.length, 0);
    w.els.find(e => (e.dataset.room || e.dataset.opensRoom) === 'nar-shaddaa').listeners.pointerenter();
    assert.deepEqual(w.srcs(), [pictures['nar-shaddaa']]);
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
