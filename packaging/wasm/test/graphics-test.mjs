// Tests for the graphics acceleration notice in packaging/wasm/index.html:
// when the page says acceleration is off, and the steps it gives. In Node:
// node packaging/wasm/test/graphics-test.mjs
//
// The page's functions are taken from index.html as they are; the browser
// (WebGL, user agent, the notice element) is made up by the test.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const code = cut('      // ---- Graphics acceleration', '      // Checked once, when the page opens');

// A browser whose WebGL 2 draws with `renderer`, or has none (null). With
// caveat set, asking for a context that needs the graphics card fails.
function browser({ renderer = null, caveat = false, ua = 'Mozilla/5.0 Chrome/155.0.0.0', brands = [], brave = false } = {}) {
    const lost = [];
    const gl = () => ({
        UNMASKED_RENDERER_WEBGL: 0x9246,
        RENDERER: 0x1F01,
        getExtension(name) {
            if (name === 'WEBGL_debug_renderer_info') return { UNMASKED_RENDERER_WEBGL: 0x9246 };
            if (name === 'WEBGL_lose_context') return { loseContext() { lost.push(1); } };
            return null;
        },
        getParameter(p) { return p === 0x9246 ? renderer : 'WebKit WebGL'; },
    });
    const document = {
        createElement() {
            return { getContext(kind, attrs) {
                if (renderer === null) return null;
                if (attrs && attrs.failIfMajorPerformanceCaveat && caveat) return null;
                return gl();
            } };
        },
    };
    const navigator = { userAgent: ua, userAgentData: { brands: brands.map(brand => ({ brand })) } };
    if (brave) navigator.brave = {};
    const fns = new Function('document', 'navigator', 'window',
        code + '\nreturn { graphicsCheck, showGfxNotice, browserKind };')(document, navigator, {});
    return { ...fns, lost };
}

// A stand-in for the notice element: keeps the HTML it's given, and the
// text put in its renderer line.
function notice() {
    const el = { hidden: true, innerHTML: '', rendererText: '', copy: null };
    el.querySelector = sel => {
        if (sel === '.renderer') return { set textContent(t) { el.rendererText = t; } };
        if (sel === '.copy') return el.innerHTML.includes('class="copy"') ? (el.copy = { addEventListener() {} }) : null;
        return null;
    };
    return el;
}
const text = el => el.innerHTML.replace(/<[^>]+>/g, '');

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

const EDGE = { ua: 'Mozilla/5.0 (Windows NT 10.0) Chrome/155.0.0.0 Safari/537.36 Edg/155.0.0.0',
               brands: ['Microsoft Edge', 'Chromium', 'Not(A:Brand'] };

test('a graphics card is fine', () => {
    for (const renderer of [
        'ANGLE (NVIDIA, NVIDIA GeForce RTX 5090 (0x00002B85) Direct3D11 vs_5_0 ps_5_0, D3D11)',
        'ANGLE (Intel, Intel(R) UHD Graphics 620 (0x00003EA0) Direct3D11 vs_5_0 ps_5_0, D3D11)',
        'ANGLE (AMD, AMD Radeon RX 7900 XTX Direct3D11 vs_5_0 ps_5_0, D3D11)',
        'ANGLE (Apple, ANGLE Metal Renderer: Apple M2, Unspecified Version)',
        'Mesa Intel(R) Xe Graphics (TGL GT2)',
        'Apple GPU',
    ]) {
        assert.equal(browser({ renderer }).graphicsCheck().state, 'ok', renderer);
    }
});

test('drawing on the CPU is software', () => {
    for (const renderer of [
        'ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero) (0x0000C0DE)), SwiftShader driver)',
        'ANGLE (Microsoft, Microsoft Basic Render Driver (0x0000008C) Direct3D11 vs_5_0 ps_5_0, D3D11)',
        'ANGLE (Mesa, llvmpipe (LLVM 20.1.2 256 bits), OpenGL 4.5 (Core Profile) Mesa 25.2.8)',
        'llvmpipe, or similar',
        'Apple Software Renderer',
    ]) {
        const check = browser({ renderer }).graphicsCheck();
        assert.equal(check.state, 'software', renderer);
        assert.equal(check.renderer, renderer);
    }
});

test('no WebGL 2 at all is none', () => {
    assert.deepEqual(browser({ renderer: null }).graphicsCheck(), { state: 'none', renderer: '' });
});

test('a hidden renderer name is checked by asking for the graphics card', () => {
    assert.equal(browser({ renderer: 'Mozilla' }).graphicsCheck().state, 'ok');
    assert.equal(browser({ renderer: 'Mozilla', caveat: true }).graphicsCheck().state, 'software');
    assert.equal(browser({ renderer: '', caveat: true }).graphicsCheck().state, 'software');
});

test('a named card is trusted even where the browser reports a caveat', () => {
    const renderer = 'ANGLE (NVIDIA, NVIDIA GeForce GTX 1060 Direct3D11 vs_5_0 ps_5_0, D3D11)';
    assert.equal(browser({ renderer, caveat: true }).graphicsCheck().state, 'ok');
});

test('test contexts are let go', () => {
    const b = browser({ renderer: 'Mozilla' });
    b.graphicsCheck();
    assert.equal(b.lost.length, 2);
});

test('the browser is told apart', () => {
    assert.equal(browser(EDGE).browserKind(), 'edge');
    assert.equal(browser({ ua: 'Mozilla/5.0 Chrome/120 Safari/537.36 Edg/120.0' }).browserKind(), 'edge');
    assert.equal(browser({ brands: ['Google Chrome', 'Chromium'] }).browserKind(), 'chrome');
    assert.equal(browser({ ua: 'Mozilla/5.0 (Windows NT 10.0; rv:140.0) Gecko/20100101 Firefox/140.0' }).browserKind(), 'firefox');
    assert.equal(browser({ brands: ['Brave', 'Chromium'], brave: true }).browserKind(), 'brave');
    assert.equal(browser({ brands: ['Opera', 'Chromium'], ua: 'Mozilla/5.0 Chrome/120 OPR/106' }).browserKind(), null);
});

test('nothing shows when acceleration is on', () => {
    const b = browser(EDGE), el = notice();
    b.showGfxNotice(el, { state: 'ok', renderer: 'ANGLE (NVIDIA, ...)' });
    assert.equal(el.hidden, true);
    b.showGfxNotice(el, null);
    assert.equal(el.hidden, true);
});

test('Edge with software drawing gets the Edge steps', () => {
    const b = browser(EDGE), el = notice();
    b.showGfxNotice(el, { state: 'software', renderer: 'ANGLE (Microsoft, Microsoft Basic Render Driver)' });
    assert.equal(el.hidden, false);
    const t = text(el);
    assert.match(t, /Turn on graphics acceleration/);
    assert.match(t, /Microsoft Edge is drawing without your graphics card/);
    assert.match(t, /go to edge:\/\/settings\/system\/manageSystem/);
    assert.match(t, /Turn on Use graphics acceleration when available/);
    assert.match(t, /Restart Edge: go to edge:\/\/restart the same way/);
    assert.ok(el.copy, 'has a Copy button');
    assert.equal(el.rendererText, ' Drawing with: ANGLE (Microsoft, Microsoft Basic Render Driver).');
});

test('no WebGL says the game needs it', () => {
    const b = browser(EDGE), el = notice();
    b.showGfxNotice(el, { state: 'none', renderer: '' });
    assert.match(text(el), /to play/);
    assert.match(text(el), /can't draw 3D graphics \(WebGL 2\)/);
    assert.equal(el.rendererText, '');
});

test('Chrome, Brave and Firefox get their own steps', () => {
    let el = notice();
    browser({ brands: ['Google Chrome'] }).showGfxNotice(el, { state: 'software', renderer: 'SwiftShader' });
    assert.match(text(el), /chrome:\/\/settings\/system.*Click Relaunch\. Then come back to this page/);
    el = notice();
    browser({ brands: ['Brave'], brave: true }).showGfxNotice(el, { state: 'software', renderer: 'SwiftShader' });
    assert.match(text(el), /brave:\/\/settings\/system/);
    el = notice();
    browser({ ua: 'Mozilla/5.0 Gecko/20100101 Firefox/140.0' }).showGfxNotice(el, { state: 'software', renderer: 'llvmpipe' });
    assert.match(text(el), /about:preferences#general/);
    assert.match(text(el), /untick Use recommended performance settings, then tick Use hardware acceleration when available/);
    assert.match(text(el), /Close Firefox completely and open it again/);
});

test('other browsers get general steps and no address', () => {
    const el = notice();
    browser({ brands: ['Opera'], ua: 'Mozilla/5.0 OPR/106' }).showGfxNotice(el, { state: 'software', renderer: 'SwiftShader' });
    assert.match(text(el), /Your browser is drawing without your graphics card/);
    assert.match(text(el), /search for acceleration/);
    assert.equal(el.copy, null);
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
