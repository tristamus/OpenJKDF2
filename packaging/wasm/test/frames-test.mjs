// Tests for the frame times the page reads from the game (Module.jkFrames,
// kept by src/Win95/Window.c), the slow-frame note and ?bench in
// packaging/wasm/index.html. In Node:
// node packaging/wasm/test/frames-test.mjs
//
// The page's functions are taken from index.html as they are; the frames,
// the browser and the game's settings are made up by the test.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const read = path => readFileSync(new URL('../../../' + path, import.meta.url), 'utf8');
const html = read('packaging/wasm/index.html');
const windowC = read('src/Win95/Window.c');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to, a);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const gfxCode = cut('      // ---- Graphics acceleration', '      // Checked once, when the page opens');
const framesCode = cut('      // ---- Frame times, from Module.jkFrames', '      (function() {\n        var streak');
const benchCode = cut('      // ---- ?bench in the address', '      if (/[?&]bench(');

// The page's functions, in a browser with this user agent and these brands.
function page({ ua = 'Mozilla/5.0 Chrome/155.0.0.0', brands = [] } = {}) {
    const navigator = { userAgent: ua, userAgentData: { brands: brands.map(brand => ({ brand })) } };
    return new Function('document', 'navigator', 'window', 'Module',
        gfxCode + framesCode + benchCode +
        '\nreturn { frameTimes, frameStats, frameTally, tallyAdd, tallyStats, slowCauses, slowNoteHtml, benchLines, browserLabel, SLOW_FPS };')(
        {}, navigator, {}, {});
}

// Frames as Window_wasmEndFrame keeps them: each [ended, resumed, joined].
function frames(list, size = 2048) {
    const f = { size, count: 0, broken: true, ends: new Float64Array(size), resumes: new Float64Array(size), joined: new Uint8Array(size) };
    for (const [end, resume, joined = 1] of list) {
        const i = f.count++ % size;
        f.ends[i] = end;
        f.resumes[i] = resume;
        f.joined[i] = joined;
    }
    return f;
}

// n frames of ms each, the game working work ms of each.
function steady(n, ms, work, start = 1000) {
    const list = [];
    for (let k = 0; k < n; k++)
        list.push([start + k * ms - (ms - work), start + k * ms, k ? 1 : 0]);
    return list;
}

const WIN_CHROME = { ua: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36',
                     brands: ['Google Chrome', 'Chromium'] };
const WIN_EDGE = { ua: WIN_CHROME.ua + ' Edg/155.0.0.0', brands: ['Microsoft Edge', 'Chromium'] };
const MAC_SAFARI = { ua: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15' };
const IPHONE = { ua: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1' };
const FIREFOX = { ua: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:140.0) Gecko/20100101 Firefox/140.0' };
const NVIDIA = 'ANGLE (NVIDIA, NVIDIA GeForce RTX 4070 (0x00002786) Direct3D11 vs_5_0 ps_5_0, D3D11)';
const env = (b, more = {}) => ({ kind: null, ua: b.ua, touch: 0, renderer: NVIDIA, battery: false, settings: null, scale: 1, ...more });
const strip = s => s.replace(/<[^>]+>/g, '').replace(/&gt;/g, '>').replace(/&lt;/g, '<').replace(/&amp;/g, '&');

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test('frame time runs from one return to the next, the game\'s part from return to finish', () => {
    const p = page();
    const t = p.frameTimes(frames(steady(61, 20, 5)), 0);
    assert.equal(t.times.length, 60);
    assert.ok(t.times.every(ms => Math.abs(ms - 20) < 1e-9));
    const s = p.frameStats(t.times, t.work);
    assert.equal(Math.round(s.fps), 50);
    assert.equal(Math.round(s.work), 5);
    assert.equal(s.over50, 0);
});

test('a frame after a menu or a hidden tab is not counted', () => {
    const p = page();
    // 10 frames, minutes in a menu, then 10 more: the gap would be one frame of 120 s.
    const list = steady(10, 16, 4).concat(steady(10, 16, 4, 121000));
    const t = p.frameTimes(frames(list), 0);
    assert.equal(t.times.length, 18);
    assert.ok(Math.max(...t.times) < 17);
});

test('only kept frames are read: the oldest has no frame before it', () => {
    const p = page();
    const f = frames(steady(100, 10, 2), 32);
    assert.equal(p.frameTimes(f, 0).times.length, 31);
    assert.equal(p.frameTimes(f, 95).times.length, 5);
    // Frames from a time on.
    assert.equal(p.frameTimes(f, 0, 1000 + 95 * 10).times.length, 5);
});

test('percentiles and the slowest frame', () => {
    const p = page();
    const times = Array.from({ length: 100 }, (_, i) => i < 94 ? 10 : i < 99 ? 40 : 300);
    const s = p.frameStats(times, 0);
    assert.equal(s.median, 10);
    assert.equal(s.p95, 40);
    assert.equal(s.worst, 300);
    assert.equal(s.over50, 1);
    assert.equal(p.frameStats([], 0), null);
});

test('a tally gives what sorting gives, for a long run', () => {
    const p = page();
    const times = [];
    for (let i = 0; i < 20000; i++) times.push(6.9 + (i % 7) * 0.1 + (i % 997 === 0 ? 80 : 0));
    times.push(1450);
    const tally = p.frameTally();
    p.tallyAdd(tally, times.slice(0, 5000), 1000);
    p.tallyAdd(tally, times.slice(5000), 3000);
    const a = p.tallyStats(tally), b = p.frameStats(times, 4000);
    assert.equal(a.frames, b.frames);
    assert.ok(Math.abs(a.fps - b.fps) < 1e-6);
    assert.ok(Math.abs(a.median - b.median) <= 0.05, a.median + ' vs ' + b.median);
    assert.ok(Math.abs(a.p95 - b.p95) <= 0.05, a.p95 + ' vs ' + b.p95);
    assert.equal(a.worst, 1450);
    assert.equal(a.over50, b.over50);
    assert.ok(Math.abs(a.work - b.work) < 1e-9);
    assert.equal(p.tallyStats(p.frameTally()), null);
});

test('the slow-frame threshold catches pages held to 30 frames a second', () => {
    assert.ok(page().SLOW_FPS > 30 && page().SLOW_FPS < 40);
});

test('graphics acceleration off comes first, with the browser\'s address', () => {
    const p = page(WIN_EDGE);
    const c = p.slowCauses(env(WIN_EDGE, { kind: 'edge', renderer: 'ANGLE (Microsoft, Microsoft Basic Render Driver)' }));
    assert.match(strip(c[0]), /Graphics acceleration is off, so Microsoft Edge draws the game on the processor/);
    assert.match(c[0], /<code>edge:\/\/settings\/system\/manageSystem<\/code>/);
});

test('Windows with built-in graphics is told about High performance', () => {
    const p = page(WIN_CHROME);
    for (const renderer of [
        'ANGLE (Intel, Intel(R) UHD Graphics 620 (0x00003EA0) Direct3D11 vs_5_0 ps_5_0, D3D11)',
        'ANGLE (AMD, AMD Radeon(TM) Graphics (0x00001638) Direct3D11 vs_5_0 ps_5_0, D3D11)',
        'ANGLE (AMD, AMD Radeon 780M Graphics (0x000015BF) Direct3D11 vs_5_0 ps_5_0, D3D11)',
        'ANGLE (AMD, AMD Radeon(TM) Vega 8 Graphics (0x000015D8) Direct3D11 vs_5_0 ps_5_0, D3D11)',
        'Intel(R) HD Graphics 400, or similar',
    ]) {
        const c = p.slowCauses(env(WIN_CHROME, { kind: 'chrome', renderer })).map(strip).join('\n');
        assert.match(c, /Settings > System > Display > Graphics, set Chrome to High performance/, renderer);
    }
    for (const renderer of [
        NVIDIA,
        'ANGLE (Intel, Intel(R) Arc(TM) A770 Graphics (0x000056A0) Direct3D11 vs_5_0 ps_5_0, D3D11)',
        'ANGLE (AMD, AMD Radeon RX 7900 XTX Direct3D11 vs_5_0 ps_5_0, D3D11)',
    ]) {
        const c = p.slowCauses(env(WIN_CHROME, { kind: 'chrome', renderer })).join('\n');
        assert.doesNotMatch(c, /High performance/, renderer);
    }
    // Not on a Mac: Windows' setting doesn't apply.
    const mac = page(MAC_SAFARI);
    const c = mac.slowCauses(env(MAC_SAFARI, { kind: 'safari', renderer: 'Intel(R) Iris(TM) Plus Graphics' })).join('\n');
    assert.doesNotMatch(c, /High performance/);
});

test('the renderer is shown as text, not markup', () => {
    const p = page(WIN_CHROME);
    const c = p.slowCauses(env(WIN_CHROME, { renderer: 'Intel <b>x</b> & co' })).join('\n');
    assert.match(c, /Intel &lt;b&gt;x&lt;\/b&gt; &amp; co/);
});

test('each browser\'s power saving, battery and Low Power Mode', () => {
    let p = page(WIN_CHROME);
    let c = p.slowCauses(env(WIN_CHROME, { kind: 'chrome', battery: true })).map(strip).join('\n');
    assert.match(c, /Energy saver lowers the frame rate of pages\. Turn it off at chrome:\/\/settings\/performance/);
    assert.match(c, /on battery.*Power mode to Best performance/);
    p = page(WIN_EDGE);
    c = p.slowCauses(env(WIN_EDGE, { kind: 'edge' })).map(strip).join('\n');
    assert.match(c, /Efficiency mode \(Energy saver in newer versions\)/);
    assert.doesNotMatch(c, /battery/);
    p = page(IPHONE);
    c = p.slowCauses(env(IPHONE, { kind: 'safari', battery: null })).map(strip).join('\n');
    assert.match(c, /Low Power Mode holds web pages to 30 frames a second on an iPhone or iPad/);
    // An iPad asks for the desktop site, as a Mac with a touch screen.
    p = page(MAC_SAFARI);
    assert.match(p.slowCauses(env(MAC_SAFARI, { kind: 'safari', touch: 5 })).join('\n'), /Low Power Mode/);
    assert.doesNotMatch(p.slowCauses(env(MAC_SAFARI, { kind: 'safari' })).join('\n'), /Low Power Mode/);
    // On battery outside Windows: no Windows settings.
    p = page(FIREFOX);
    c = p.slowCauses(env({ ua: 'Mozilla/5.0 (X11; Linux x86_64) Firefox/140.0' }, { kind: 'firefox', battery: true })).join('\n');
    assert.match(c, /Plug it in\./);
});

test('costly Display settings are named', () => {
    const p = page(FIREFOX);
    const c = p.slowCauses(env(FIREFOX, { kind: 'firefox', scale: 2,
        settings: { fpsLimit: 0, ssaa: 2, ssao: true, bloom: true, vsync: true } })).map(strip);
    assert.equal(c.length, 1);
    assert.equal(c[0], "In the game's Setup > Display, turn off Enable HiDPI (it draws 4 times the pixels), " +
        'set SSAA Multiplier to 1 (2 draws 4 times the pixels), turn off Enable SSAO, turn off Enable Bloom.');
});

test('with nothing to point at, general advice', () => {
    const p = page(FIREFOX);
    const c = p.slowCauses(env(FIREFOX, { kind: 'firefox', settings: { fpsLimit: 0, ssaa: 1, ssao: false, bloom: false, vsync: true } }));
    assert.deepEqual(c, ['Close other tabs and programs that keep the computer busy, and update your graphics driver.']);
});

test('the note gives the frame rate and the causes as a list', () => {
    const p = page();
    const h = p.slowNoteHtml(23.6, ['one', 'two']);
    assert.match(strip(h), /running slowly here, about 24 frames a second/);
    assert.match(h, /<ul><li>one<\/li><li>two<\/li><\/ul>/);
});

test('?bench lines', () => {
    const p = page(WIN_EDGE);
    const recent = p.frameStats([16, 17, 16, 18], 20);
    const lines = p.benchLines(recent, null, 3000, { renderer: NVIDIA, browser: 'Microsoft Edge 155 on Windows',
        width: 1920, height: 1080, dpr: 1.25, settings: { fpsLimit: 0, ssaa: 1, ssao: false, bloom: true, vsync: true } });
    assert.equal(lines[0], 'Last 5 s: 59.7 fps | frame median 17.0 ms, 95% 18.0, worst 18.0 | game 5.0 ms | over 50 ms: 0');
    assert.equal(lines[1], 'Since Reset (3 s, 0 frames): no frames in a level yet');
    assert.equal(lines[2], 'Renderer: ' + NVIDIA);
    assert.equal(lines[3], 'Browser: Microsoft Edge 155 on Windows | canvas 1920x1080 | devicePixelRatio 1.25');
    assert.equal(lines[4], 'Settings: Vsync on, FPS limit none, SSAA 1, SSAO off, Bloom on');
    assert.equal(p.benchLines(null, null, 0, { browser: '', settings: null })[4], 'Settings: not loaded yet');
});

test('the browser\'s name and version for ?bench', () => {
    assert.equal(page(WIN_EDGE).browserLabel(WIN_EDGE.ua), 'Microsoft Edge 155 on Windows');
    assert.equal(page(WIN_CHROME).browserLabel(WIN_CHROME.ua), 'Chrome 155 on Windows');
    assert.equal(page(FIREFOX).browserLabel(FIREFOX.ua), 'Firefox 140 on Windows');
    assert.equal(page(MAC_SAFARI).browserLabel(MAC_SAFARI.ua), 'Safari 18 on macOS');
    assert.equal(page(IPHONE).browserLabel(IPHONE.ua), 'Safari 18 on iOS');
});

test('the game keeps the frames the page reads, and marks the ones after a break', () => {
    // The fields the page reads, made where Window.c makes them.
    for (const field of ['size: 2048', 'count: 0', 'broken: true', 'ends: new Float64Array', 'resumes: new Float64Array', 'joined: new Uint8Array'])
        assert.ok(windowC.includes(field), 'Module.jkFrames has ' + field);
    assert.match(windowC, /frames\.joined\[i\] = frames\.broken \? 0 : 1;/);
    assert.match(windowC, /Window_wasmMenuFrame\(\);/);
    assert.match(windowC, /EMSCRIPTEN_KEEPALIVE int Window_WebDisplaySetting\(int which\)/);
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
