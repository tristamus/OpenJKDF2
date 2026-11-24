// Checks the touch controls for watching a match in the game on a phone or
// tablet (index.html, "Touch controls for watching a match"): the game's
// commands they press are the engine's (types_enums.h), the message sent is
// the feed's kind of line, and a room's Watch on a phone goes straight into
// the game only when it has what it needs (phoneWatchStraight).
//
//   node packaging/wasm/test/touch-watch-test.mjs

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const enums = readFileSync(new URL('../../../src/types_enums.h', import.meta.url), 'utf8');
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

const ENGINE = {
    forward: 'FORWARD', slide: 'SLIDE', jump: 'JUMP', duck: 'DUCK', fast: 'FAST', fire1: 'FIRE1', fire2: 'FIRE2',
    map: 'MAP', camera: 'CAMERAMODE', tally: 'TALLY', talk: 'TALK',
};
function engineIndex(name) {
    const m = new RegExp('INPUT_FUNC_' + name + '\\s*=\\s*(\\d+)').exec(enums);
    assert.ok(m, 'types_enums.h has INPUT_FUNC_' + name);
    return Number(m[1]);
}

test("the touch controls press the engine's own commands", () => {
    const funcs = new Function(cut('      var TOUCH_FUNCS = ', '\n') + '\nreturn TOUCH_FUNCS;')();
    for (const [k, v] of Object.entries(funcs)) assert.equal(v, engineIndex(ENGINE[k]), k);
    for (const k of ['forward', 'slide', 'jump', 'duck', 'fast', 'fire1', 'fire2', 'map', 'camera', 'tally'])
        assert.ok(k in funcs, k);
});

test("the Controls list's keys are the engine's commands, Scores (Tally) too", () => {
    const funcs = new Function(cut('        var EMBED_KEY_FUNCS = ', ';\n') + ';\nreturn EMBED_KEY_FUNCS;')();
    for (const [k, v] of Object.entries(funcs)) assert.equal(v, engineIndex(ENGINE[k]), k);
    assert.equal(funcs.tally, engineIndex('TALLY'));
    assert.match(html, /\['Scores', keys\('tally', 'Tally'\)\]/);
});

const nameClean = new Function('TOUCH_NAME_MAX', cut('      function touchNameClean(name) {', '      (function() {') +
                              '\nreturn touchNameClean;')(16);

test('the name on a message is cleaned as the feed takes a name', () => {
    assert.equal(nameClean('  Antonio  '), 'Antonio');
    assert.equal(nameClean('Ant\u00f3nio   the   Great Jedi'), 'Antnio the Great');
    assert.equal(nameClean(null), '');
    assert.equal(nameClean(undefined), '');
    assert.equal(nameClean('\u{1F600}'), '');
});

test('a message is cleaned as the feed takes it, and at most 4 wait', () => {
    const code = cut('      function touchSaySend() {', '      touchWatch.form.addEventListener(');
    const runFull = (text, queue, name) => {
        const Module = { jkTouchSay: queue, jkWatchName: 'old' };
        const store = {};
        const localStorage = { setItem: (k, v) => { store[k] = v; }, removeItem: (k) => { delete store[k]; } };
        let closed = 0;
        const touchWatch = { input: { value: text }, nameInput: { value: name || '' } };
        new Function('Module', 'touchWatch', 'touchSayClose', 'TOUCH_SAY_MAX', 'touchNameClean', 'TOUCH_NAME_KEY', 'localStorage',
                     code + '\ntouchSaySend();')(
            Module, touchWatch, () => closed++, 96, nameClean, 'jkdf2mp-watch-name', localStorage);
        assert.equal(closed, 1);
        return { Module, store };
    };
    const run = (text, queue) => runFull(text, queue).Module.jkTouchSay;
    // The name typed goes on what this game says (wsrelay_js_view_say), and is kept.
    let r = runFull('hi', undefined, ' Antonio ');
    assert.equal(r.Module.jkWatchName, 'Antonio');
    assert.equal(r.store['jkdf2mp-watch-name'], 'Antonio');
    r = runFull('hi', undefined, '');
    assert.equal('jkWatchName' in r.Module, false, 'no name: the player of the game files, or Someone');
    assert.equal('jkdf2mp-watch-name' in r.store, false);
    assert.deepEqual(run('  hello   there  '), ['hello there']);
    assert.deepEqual(run('café 😀!'), ['caf? ?!']);
    assert.equal(run('x'.repeat(200))[0].length, 96);
    assert.deepEqual(run('   '), undefined);
    assert.deepEqual(run('five', ['1', '2', '3', '4']), ['1', '2', '3', '4']);
});

test("a room's Watch on a phone: straight into the game with saved files or the demo", () => {
    const code = cut('      function phoneWatchStraight(id) {', '      // The room window');
    const ROOMS = { demo: { game: 'jk1', demo: true }, jk: { game: 'jk1' }, mots: { game: 'mots' } };
    const run = (o) => {
        const calls = { played: 0, full: 0 };
        const w = o.gone ? null : { key: o.key, g: { id: 7 } };
        const api = new Function('watchWindow', 'watchEmbed', 'phoneNote', 'leaveGuard', 'watchedGame', 'watchInGame', 'ROOMS',
                                 'gameFiles', 'savedKnown', 'touchWatchFullscreen', 'playWatched', 'window',
                                 code + '\nreturn phoneWatchStraight;')(
            !!o.window, !!o.embed, () => (o.phone === false ? null : 'On a phone'), !!o.playing, () => w,
            () => (w ? { why: o.why || '' } : null), ROOMS, o.noStore ? null : {}, o.saved || {},
            () => calls.full++, (id) => { assert.equal(id, 7); calls.played++; }, { jkLobbyMock: !!o.mock });
        const went = api(7);
        assert.equal(calls.played, went ? 1 : 0);
        assert.equal(calls.full, went ? 1 : 0);
        return went;
    };
    assert.equal(run({ key: 'demo' }), true);
    assert.equal(run({ key: 'jk', saved: { jk1: true } }), true);
    assert.equal(run({ key: 'jk' }), false, 'no files saved: the preview, as before');
    assert.equal(run({ key: 'jk', saved: { jk1: false } }), false);
    assert.equal(run({ key: 'mots', saved: { jk1: true } }), false, "the other game's files");
    assert.equal(run({ key: 'jk', saved: { jk1: true }, noStore: true }), false);
    assert.equal(run({ key: 'demo', phone: false }), false, 'a computer: the preview');
    assert.equal(run({ key: 'demo', why: 'An older version' }), false);
    assert.equal(run({ gone: true }), false);
    assert.equal(run({ key: 'demo', window: true }), false);
    assert.equal(run({ key: 'demo', embed: true }), false);
    assert.equal(run({ key: 'demo', mock: true }), false);
    assert.equal(run({ key: 'demo', playing: true }), false);
});

test("the room window's Watch asks it first, only for a match not already shown", () => {
    assert.match(html, /if \(t\.dataset\.watch\) \{\n\s*if \(Number\(t\.dataset\.watch\) !== watchLive\.id && phoneWatchStraight\(Number\(t\.dataset\.watch\)\)\) return;/);
});

test('the message box keeps its keys from the game, which listens later', () => {
    const code = cut("      ['keydown', 'keyup', 'keypress'].forEach(function(type) {\n        window.addEventListener(type, function(ev) {\n          var t = touchWatch;",
                     "      window.addEventListener('pointermove'");
    assert.match(code, /if \(ev\.target === t\.input \|\| ev\.target === t\.nameInput\) \{\s*ev\.stopImmediatePropagation\(\);/);
    assert.match(code, /\}, true\);/);
});

test('the screen stays on while a match is watched, and only then', () => {
    const code = cut('      // ---- The screen stays on while a match is watched', '      // ---- The network log');
    const run = (o) => {
        const asked = [];
        let interval = null, onVis = null;
        const lock = { released: 0, release() { this.released++; return Promise.resolve(); }, addEventListener() {} };
        const nav = { wakeLock: { request(kind) { asked.push(kind); return { then(ok) { ok(lock); } }; } } };
        const doc = { hidden: !!o.hidden, addEventListener(type, fn) { if (type === 'visibilitychange') onVis = fn; } };
        new Function('watchEmbed', 'navigator', 'document', 'pickerWant', 'gameLost', 'watchLive', 'watchDlg', 'room',
                     'setInterval', 'performance', code)(
            !!o.embed, nav, doc, o.pickerWant || null, !!o.lost, { id: o.id || 0 }, { open: !!o.dlg }, { open: !!o.room },
            (fn) => { interval = fn; }, { now: () => 1e6 });
        if (interval) interval();
        return { asked: asked.length, lock, interval, onVis, doc, set(k, v) { o[k] = v; } };
    };
    assert.equal(run({ pickerWant: { watch: 5 } }).asked, 1, 'the game watching');
    assert.equal(run({ id: 5, dlg: true }).asked, 1, 'the viewing page on a match');
    assert.equal(run({ id: 5, room: true }).asked, 1, "the room's preview");
    assert.equal(run({ id: 0, dlg: true }).asked, 0, 'the viewing page on no match');
    assert.equal(run({ id: 5 }).asked, 0, 'neither open');
    assert.equal(run({ pickerWant: { watch: 5 }, lost: true }).asked, 0, 'the game stopped');
    assert.equal(run({ pickerWant: { game: 'jk1' } }).asked, 0, 'playing, not watching');
    assert.equal(run({ pickerWant: { watch: 5 }, hidden: true }).asked, 0, 'a hidden page');
    assert.equal(run({ pickerWant: { watch: 5 }, embed: true }).interval, null, "the box's page leaves it to the lobby page");
});

test('Back on a phone leaves watching for the viewing page, once the controls show', () => {
    assert.match(html, /if \(!shown\) touchWatchLetGo\(\);\n\s*else touchWatchBackArm\(\);/);
    const code = cut('      function touchWatchBackArm() {', '      function touchWatchLeave() {');
    assert.match(code, /history\.pushState\(/);
    assert.match(code, /window\.addEventListener\('popstate'/);
    assert.match(code, /if \(t\.saying\) \{\s*touchSayClose\(\);\s*return touchWatchBackArm\(\);\s*\}\s*touchWatchLeave\(\);/);
});

test("a phone that watches isn't told it needs a keyboard and mouse", () => {
    assert.match(html, /var phone = want && want\.watch \? null : phoneNote\(\);/);
    assert.match(html, /var phone = phoneNote\(\) && pickerWant && pickerWant\.watch \?\n\s*'On a phone or tablet, you need the game files on it to watch in the game\.' : phoneNote\(\);/);
});

console.log('all ' + passed + ' passed');
