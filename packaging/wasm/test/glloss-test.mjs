// Tests for what packaging/wasm/index.html does when the browser takes the
// game's graphics away (a lost WebGL context) and gives them back: the note,
// the black screen, the timers and the box with Reload, and the engine's
// 'jkgl' events (std3D_WasmGlReady in src/Platform/GL/std3D.c). In Node:
// node packaging/wasm/test/glloss-test.mjs
//
// The page's code is taken from index.html as it is; the canvas, the page,
// the clock and the box are made up by the test.

import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to, a);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const code = cut('          // ---- Lost graphics', '          // Ask for the faster graphics card');

// A page with the game's canvas, a clock that only moves when told, and a box
// (showProblem) that records what it was asked to show.
function page({ engine = true } = {}) {
    let now = 1000000, timers = [], nextId = 1;
    const listeners = {};
    const canvas = {
        addEventListener(type, fn) { (listeners[type] = listeners[type] || []).push(fn); },
        fire(type, detail) {
            const e = { type, detail, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; } };
            (listeners[type] || []).forEach(fn => fn(e));
            return e;
        },
    };
    const classes = new Set();
    const body = { children: [], appendChild(el) { this.children.push(el); } };
    const slow = { className: 'slow-note', hidden: false };  // a slow-frame note already showing
    const document = {
        documentElement: { classList: { add: c => classes.add(c), remove: c => classes.delete(c), contains: c => classes.has(c) } },
        body,
        createElement: tag => ({ tag, hidden: false, attrs: {}, setAttribute(k, v) { this.attrs[k] = v; } }),
        querySelectorAll: sel => sel === '.slow-note' ? [slow].concat(body.children.filter(c => c.className === 'slow-note')) : [],
    };
    const boxes = [], logs = [];
    const ctx = {
        canvas, document, boxes, logs,
        Module: engine ? { jkGl: { down: 0 } } : {},
        gameLost: false,
        Date: { now: () => now },
        console: { warn: m => logs.push(m), log: m => logs.push(m) },
        setTimeout(fn, ms) { const id = nextId++; timers.push({ id, at: now + ms, fn }); return id; },
        clearTimeout(id) { timers = timers.filter(t => t.id !== id); },
        showProblem(title, text, reload) { boxes.push({ title, text, reload }); ctx.gameLost = true; },
    };
    vm.createContext(ctx);
    vm.runInContext(code, ctx);
    return {
        ctx, canvas, boxes, logs, slow,
        note: () => body.children.find(c => c.className === 'slow-note'),
        lost: () => classes.has('gl-lost'),
        // moves the clock on, running the timers that come due
        wait(ms) {
            const end = now + ms;
            for (;;) {
                timers.sort((x, y) => x.at - y.at);
                const t = timers[0];
                if (!t || t.at > end) break;
                timers.shift();
                now = t.at;
                t.fn();
            }
            now = end;
        },
    };
}

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

test('a loss is cancelled, and shows the note and a black screen, not the box', () => {
    const p = page();
    const e = p.canvas.fire('webglcontextlost');
    assert.equal(e.defaultPrevented, true);
    assert.equal(p.boxes.length, 0);
    assert.equal(p.lost(), true);
    assert.equal(p.note().hidden, false);
    assert.match(p.note().innerHTML, /The graphics stopped for a moment\.<\/b> Getting them back\.\.\./);
    assert.equal(p.slow.hidden, true, 'the slow-frame note gives way');
});

test('back and drawn again: the note goes, the screen shows, no box', () => {
    const p = page();
    p.canvas.fire('webglcontextlost');
    p.wait(3000);
    p.canvas.fire('webglcontextrestored');
    p.canvas.fire('jkgl', 'rebuilding');
    p.canvas.fire('jkgl', 'back');
    assert.equal(p.lost(), false);
    assert.equal(p.note().hidden, true);
    p.wait(120000);
    assert.equal(p.boxes.length, 0);
});

test('not given back within 30 s: the box says so', () => {
    const p = page();
    p.canvas.fire('webglcontextlost');
    p.wait(29999);
    assert.equal(p.boxes.length, 0);
    p.wait(1);
    assert.equal(p.boxes.length, 1);
    assert.equal(p.boxes[0].reload, true);
    assert.match(p.boxes[0].text, /hasn't given them back/);
    assert.equal(p.note().hidden, true);
    assert.equal(p.lost(), true, 'still black: nothing to show');
});

test('a long level load after the restore shows no box: the 10 s start at the engine\'s first frame', () => {
    const p = page();
    p.canvas.fire('webglcontextlost');
    p.wait(2000);
    p.canvas.fire('webglcontextrestored');
    p.wait(60000);                          // the engine is loading a level: no frame yet
    assert.equal(p.boxes.length, 0);
    p.canvas.fire('jkgl', 'rebuilding');
    p.wait(9999);
    assert.equal(p.boxes.length, 0);
    p.canvas.fire('jkgl', 'back');
    p.wait(60000);
    assert.equal(p.boxes.length, 0);
    assert.equal(p.lost(), false);
});

test('the engine ran a frame but didn\'t draw within 10 s: the box says that', () => {
    const p = page();
    p.canvas.fire('webglcontextlost');
    p.canvas.fire('webglcontextrestored');
    p.canvas.fire('jkgl', 'rebuilding');
    p.wait(10000);
    assert.equal(p.boxes.length, 1);
    assert.match(p.boxes[0].text, /gave the game's 3D graphics back, but the game didn't draw/);
});

test('the engine couldn\'t make them again: the box at once', () => {
    const p = page();
    p.canvas.fire('webglcontextlost');
    p.canvas.fire('webglcontextrestored');
    p.canvas.fire('jkgl', 'rebuilding');
    p.canvas.fire('jkgl', 'failed');
    assert.equal(p.boxes.length, 1);
    assert.match(p.boxes[0].text, /couldn't set them up again/);
    p.wait(60000);
    assert.equal(p.boxes.length, 1, 'one box');
});

test('a second loss within 2 minutes recovers; the third shows the box, saying how often', () => {
    const p = page();
    for (let i = 0; i < 2; i++) {
        p.canvas.fire('webglcontextlost');
        p.wait(2000);
        p.canvas.fire('webglcontextrestored');
        p.canvas.fire('jkgl', 'rebuilding');
        p.canvas.fire('jkgl', 'back');
        p.wait(20000);
    }
    assert.equal(p.boxes.length, 0);
    p.canvas.fire('webglcontextlost');
    assert.equal(p.boxes.length, 1);
    assert.match(p.boxes[0].text, /away 3 times in 2 minutes/);
    // the engine still gets them back behind the box if the browser gives them
    p.canvas.fire('webglcontextrestored');
    p.canvas.fire('jkgl', 'rebuilding');
    p.canvas.fire('jkgl', 'back');
    assert.equal(p.lost(), false);
});

test('losses more than 2 minutes apart never add up', () => {
    const p = page();
    for (let i = 0; i < 5; i++) {
        p.canvas.fire('webglcontextlost');
        p.wait(1000);
        p.canvas.fire('webglcontextrestored');
        p.canvas.fire('jkgl', 'rebuilding');
        p.canvas.fire('jkgl', 'back');
        p.wait(61000);
    }
    assert.equal(p.boxes.length, 0);
});

test('lost again before the engine drew: the 30 s start again', () => {
    const p = page();
    p.canvas.fire('webglcontextlost');
    p.canvas.fire('webglcontextrestored');
    p.canvas.fire('jkgl', 'rebuilding');
    p.wait(5000);
    p.canvas.fire('webglcontextlost');
    p.wait(29000);
    assert.equal(p.boxes.length, 0);
    p.wait(1000);
    assert.equal(p.boxes.length, 1);
    assert.match(p.boxes[0].text, /hasn't given them back/);
});

test('an engine that can\'t make them again gets the box when they come back', () => {
    const p = page({ engine: false });
    p.canvas.fire('webglcontextlost');
    assert.equal(p.boxes.length, 0);
    p.canvas.fire('webglcontextrestored');
    assert.equal(p.boxes.length, 1);
    assert.match(p.boxes[0].text, /can't get them back by itself/);
});

test('a box already up (the game stopped) is not replaced', () => {
    const p = page();
    p.ctx.gameLost = true;
    p.canvas.fire('webglcontextlost');
    p.wait(30000);
    assert.equal(p.boxes.length, 0);
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
