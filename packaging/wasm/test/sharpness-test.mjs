// Checks Sharpness (index.html) and packaging/wasm/pixelratio.js: how many
// canvas pixels the game draws per page pixel at each step, the steps a
// screen has, where a phone and a computer start, the step down when the game
// can't keep up (not past a step picked by hand), and the Graphics panel and
// Enable HiDPI wiring. In Node:
//
//   node packaging/wasm/test/sharpness-test.mjs

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const lib = readFileSync(new URL('../pixelratio.js', import.meta.url), 'utf8');
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

const block = cut('      // ---- Sharpness (Antonio, 2026-11-24', '      // ---- Touch controls for watching a match');

// The block run with a made-up page: a screen of dpr page pixels per screen
// pixel, a canvas of w x h page pixels, saved values in store, phone or not.
// Timers are kept for the test to run.
function page({ dpr = 3, w = 803, h = 360, store = {}, phone = true, frames = null, fpsLimit = 0 } = {}) {
    const timers = [], events = [], notes = [], logs = [];
    const Module = { canvas: { clientWidth: w, clientHeight: h }, jkFrames: frames };
    const localStorage = {
        getItem: (k) => (k in store ? store[k] : null),
        setItem: (k, v) => { store[k] = String(v); },
    };
    const window = {
        devicePixelRatio: dpr,
        dispatchEvent: (ev) => events.push(ev.type),
    };
    const document = { hidden: false, documentElement: { classList: { contains: () => false } } };
    class Event { constructor(type) { this.type = type; } }
    const env = {
        Module, localStorage, window, document, Event, innerWidth: w, innerHeight: h,
        phoneNote: () => (phone ? 'a phone' : null),
        setInterval: (fn, ms) => { timers.push({ fn, ms, every: true }); return timers.length; },
        setTimeout: (fn, ms) => { timers.push({ fn, ms }); return timers.length; },
        displaySettings: () => ({ fpsLimit }),
        frameTimes: (f, from) => {
            const times = [];
            for (let n = Math.max(from, 1); n < f.count; n++) times.push(f.times[n]);
            return { times, work: 0 };
        },
        frameStats: (times) => {
            if (!times.length) return null;
            const total = times.reduce((a, b) => a + b, 0);
            return { fps: times.length * 1000 / total, seconds: total / 1000 };
        },
        touchWatchNote: (t) => notes.push(t),
        console: { log: (t) => logs.push(t) },
    };
    const names = Object.keys(env);
    const api = new Function(...names, block + `
        return { sharpness, sharpSteps, sharpLevel, sharpRatio, sharpRatioOf, sharpLines, sharpSet, sharpCanStepDown };`)(
        ...names.map((k) => env[k]));
    return { ...api, Module, store, timers, events, notes, window, env };
}

// Frames drawn at fps for ms more milliseconds.
function draw(frames, fps, ms) {
    const n = Math.round(ms * fps / 1000);
    for (let i = 0; i < n; i++) frames.times[frames.count++] = 1000 / fps;
}
function tick(p) {
    p.timers.filter((t) => t.every && t.ms === 1000).forEach((t) => t.fn());
}

test("the engine's pixel ratio is the page's pick, and 1 without one", () => {
    const lib2 = {};
    new Function('addToLibrary', 'Module', lib)((o) => Object.assign(lib2, o), {});
    assert.equal(lib2.emscripten_get_device_pixel_ratio(), 1, 'no page: as before');
    const get = (pick) => {
        const o = {};
        new Function('addToLibrary', 'Module', lib)((x) => Object.assign(o, x), { jkPixelRatio: pick });
        return o.emscripten_get_device_pixel_ratio();
    };
    assert.equal(get(() => 3), 3);
    assert.equal(get(() => 1.5), 1.5);
    assert.equal(get(() => 0), 1, 'nonsense: 1');
    assert.equal(get(() => NaN), 1);
    assert.equal(get(() => 100), 1);
    assert.match(readFileSync(new URL('../../../cmake_modules/plat_wasm.cmake', import.meta.url), 'utf8'),
                 /--js-library \$\{PROJECT_SOURCE_DIR\}\/packaging\/wasm\/pixelratio\.js/);
    assert.equal(html.indexOf('Module.jkPixelRatio = sharpRatio;') > 0, true);
});

test('the steps a screen has, and the pixels each draws (his phone: 803x360 at 3)', () => {
    let p = page();
    assert.deepEqual(p.sharpSteps(), ['standard', 'sharp', 'full']);
    assert.equal(p.sharpRatioOf('standard'), 1);
    assert.equal(p.sharpRatioOf('sharp'), 2);
    assert.equal(p.sharpRatioOf('full'), 3);
    assert.equal(p.sharpLines('standard'), '360p');
    assert.equal(p.sharpLines('sharp'), '720p');
    assert.equal(p.sharpLines('full'), '1080p');
    p = page({ dpr: 2 });
    assert.deepEqual(p.sharpSteps(), ['standard', 'sharp', 'full']);
    assert.equal(p.sharpRatioOf('sharp'), 1.5);
    p = page({ dpr: 1.5 });
    assert.deepEqual(p.sharpSteps(), ['standard', 'full'], 'Sharp only well between the two');
    p = page({ dpr: 1, phone: false });
    assert.deepEqual(p.sharpSteps(), ['standard']);
    assert.equal(p.sharpRatio(), 1, 'a plain monitor: as before');
});

test('Full is kept to a 4K picture', () => {
    const p = page({ dpr: 3, w: 1366, h: 1024, phone: true });
    const r = p.sharpRatio();
    assert.ok(Math.abs(1366 * 1024 * r * r - 3840 * 2160) < 1, String(r));
    assert.equal(p.sharpLines('full'), Math.round(1024 * r) + 'p');
});

test('a phone starts at Full, a computer at Standard, a pick is kept', () => {
    assert.equal(page({ phone: true }).sharpLevel(), 'full');
    assert.equal(page({ phone: false, dpr: 2 }).sharpLevel(), 'standard');
    assert.equal(page({ store: { 'jkdf2mp-sharpness': 'sharp', 'jkdf2mp-sharpness-by': 'hand' } }).sharpLevel(), 'sharp');
    assert.equal(page({ store: { 'jkdf2mp-sharpness': 'nonsense' } }).sharpLevel(), 'full');
    // Sharp on a screen without it: the step below.
    assert.equal(page({ dpr: 1.5, store: { 'jkdf2mp-sharpness': 'sharp' } }).sharpLevel(), 'standard');
    assert.equal(page({ dpr: 1.5, store: { 'jkdf2mp-sharpness': 'sharp' } }).sharpRatio(), 1);
});

test('a pick is saved and goes to the game with a resize, after the event that made it', () => {
    const p = page();
    p.sharpSet('sharp', 'hand');
    assert.equal(p.store['jkdf2mp-sharpness'], 'sharp');
    assert.equal(p.store['jkdf2mp-sharpness-by'], 'hand');
    assert.deepEqual(p.events, [], 'not in the call');
    p.timers.filter((t) => !t.every).forEach((t) => t.fn());
    assert.deepEqual(p.events, ['resize']);
    assert.equal(p.sharpRatio(), 2);
});

test('too slow for 10 seconds: one step down, with a word, and again; not past a hand pick', () => {
    const frames = { count: 1, times: [16] };
    const p = page({ frames });
    for (let s = 0; s < 9; s++) { draw(frames, 20, 1000); tick(p); }
    assert.equal(p.sharpLevel(), 'full', '9 slow seconds: not yet');
    draw(frames, 20, 1000); tick(p);
    assert.equal(p.sharpLevel(), 'sharp');
    assert.equal(p.store['jkdf2mp-sharpness-by'], 'auto');
    assert.equal(p.notes.length, 1);
    assert.match(p.notes[0], /Sharpness lowered to Sharp/);
    for (let s = 0; s < 10; s++) { draw(frames, 20, 1000); tick(p); }
    assert.equal(p.sharpLevel(), 'standard');
    for (let s = 0; s < 20; s++) { draw(frames, 20, 1000); tick(p); }
    assert.equal(p.sharpLevel(), 'standard', 'no lower');
    assert.equal(p.sharpCanStepDown(), false);
});

test('a fast second starts the count again', () => {
    const frames = { count: 1, times: [16] };
    const p = page({ frames });
    for (let s = 0; s < 9; s++) { draw(frames, 20, 1000); tick(p); }
    draw(frames, 60, 1000); tick(p);
    for (let s = 0; s < 9; s++) { draw(frames, 20, 1000); tick(p); }
    assert.equal(p.sharpLevel(), 'full');
});

test('no step down from a hand pick, nor under a low FPS limit of their own', () => {
    let frames = { count: 1, times: [16] };
    let p = page({ frames, store: { 'jkdf2mp-sharpness': 'full', 'jkdf2mp-sharpness-by': 'hand' } });
    for (let s = 0; s < 30; s++) { draw(frames, 20, 1000); tick(p); }
    assert.equal(p.sharpLevel(), 'full');
    assert.equal(p.sharpCanStepDown(), false);
    frames = { count: 1, times: [16] };
    p = page({ frames, fpsLimit: 30 });
    for (let s = 0; s < 30; s++) { draw(frames, 20, 1000); tick(p); }
    assert.equal(p.sharpLevel(), 'full');
});

test('Enable HiDPI is the sharpness: ticked above Standard, ticking picks Full', () => {
    const p = page({ phone: false, dpr: 2 });
    assert.equal(p.Module.jkSharpness.isUp(), false);
    p.Module.jkSharpness.fromGame(true);
    assert.equal(p.sharpLevel(), 'full');
    assert.equal(p.store['jkdf2mp-sharpness-by'], 'hand');
    assert.equal(p.Module.jkSharpness.isUp(), true);
    p.Module.jkSharpness.fromGame(false);
    assert.equal(p.sharpLevel(), 'standard');
    const display = readFileSync(new URL('../../../src/Platform/SDL2/jkGUIDisplay.c', import.meta.url), 'utf8');
    assert.match(display, /Window_WasmSharpIsUp\(\)/);
    assert.match(display, /Window_WasmSetSharp\(jkGuiDisplay_aElements\[14\]\.selectedTextEntry\)/);
    const win = readFileSync(new URL('../../../src/Win95/Window.c', import.meta.url), 'utf8');
    assert.match(win, /Module\.jkSharpness\.isUp\(\)/);
    assert.match(win, /Module\.jkSharpness\.fromGame\(!!bUp\)/);
});

test('the page no longer sizes the canvas itself', () => {
    assert.equal(/window\.onresize\s*=/.test(html), false);
    assert.equal(/window\.onresize\(\)/.test(html), false);
});

test('Graphics: a button in the phone controls, its panel, Back and a tap on the picture close it', () => {
    assert.match(html, /class="tw-b" data-tw="gfx">/);
    assert.match(html, /<div class="tw-gfx" role="dialog" aria-label="Graphics" hidden>/);
    assert.match(html, /if \(b\.dataset\.tw === 'gfx' && touchWatch\.live && !touchWatch\.gfx\) touchGfxOpen\(\);/);
    assert.match(html, /if \(!p\.moved && ms < TOUCH_TAP_MS && t\.gfx\) return touchGfxClose\(\);/);
    assert.match(html, /if \(t\.gfx\) touchGfxClose\(\);\n\s*\}\n\n\s*\/\/ Held upright/, 'letting go closes it');
    // The slow note leaves it to the step down first.
    assert.match(html, /if \(sharpCanStepDown\(\)\) \{\s*streak = 0;\s*return;\s*\}/);
});

console.log('all ' + passed + ' passed');
