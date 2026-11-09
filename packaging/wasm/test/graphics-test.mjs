// Tests for the graphics acceleration, Edge enhanced security and
// resist-fingerprinting notices in packaging/wasm/index.html: when the page
// shows them, and the steps they give. In Node:
// node packaging/wasm/test/graphics-test.mjs
//
// The page's functions are taken from index.html as they are; the browser
// (WebGL, user agent, workers, storage, the notice element) is made up by
// the test.

import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const code = cut('      // ---- Graphics acceleration', '      // Checked once, when the page opens');
const jitCode = cut("      // ---- Edge's enhanced security", '      // Timed once, when the page opens');
const timedCode = cut('      // Timed once, when the page opens', '        </script>\n\n        <div class="layout">');

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

const SAFARI = { ua: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15' };
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
    assert.equal(browser({ brands: ['Opera', 'Chromium'], ua: 'Mozilla/5.0 Chrome/120 OPR/106' }).browserKind(), 'opera');
    assert.equal(browser({ brands: ['Opera GX', 'Chromium'], ua: 'Mozilla/5.0 Chrome/120 OPR/106' }).browserKind(), 'operagx');
    assert.equal(browser({ ua: 'Mozilla/5.0 Chrome/120 Safari/537.36 OPR/106.0' }).browserKind(), 'opera');
    assert.equal(browser({ brands: ['Chromium'], ua: 'Mozilla/5.0 Chrome/120 Safari/537.36 Vivaldi/7.0' }).browserKind(), null);
    assert.equal(browser(SAFARI).browserKind(), 'safari');
    assert.equal(browser({ ua: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) ' +
                               'Version/18.0 Mobile/15E148 Safari/604.1' }).browserKind(), 'safari');
    // Chrome, Firefox and Edge on an iPhone draw with Safari's engine but say who they are.
    for (const app of ['CriOS/130.0.6723.90', 'FxiOS/132.0', 'EdgiOS/130.0.2849.80'])
        assert.equal(browser({ ua: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) ' +
                                   app + ' Version/18.0 Mobile/15E148 Safari/604.1' }).browserKind(), null, app);
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

test('Opera and Opera GX get their own steps', () => {
    let el = notice();
    browser({ brands: ['Opera'], ua: 'Mozilla/5.0 OPR/106' }).showGfxNotice(el, { state: 'software', renderer: 'SwiftShader' });
    assert.match(text(el), /^Turn on graphics accelerationOpera is drawing without your graphics card/);
    assert.match(text(el), /opera:\/\/settings\/system.*Click Relaunch\. Then come back to this page/);
    assert.ok(el.copy, 'has a Copy button');
    el = notice();
    browser({ brands: ['Opera GX'], ua: 'Mozilla/5.0 OPR/106' }).showGfxNotice(el, { state: 'software', renderer: 'SwiftShader' });
    assert.match(text(el), /Opera GX is drawing without your graphics card/);
    assert.match(text(el), /opera:\/\/settings\/system/);
});

test('Safari without WebGL 2 is told to update, not to look for a setting', () => {
    const el = notice();
    browser(SAFARI).showGfxNotice(el, { state: 'none', renderer: '' });
    const t = text(el);
    assert.match(t, /^Update Safari to play/);
    assert.match(t, /Safari can't draw 3D graphics \(WebGL 2\) right now, and the game needs them\./);
    assert.match(t, /Safari 15 and later can draw it\. Update macOS, or iOS on an iPhone or iPad/);
    assert.doesNotMatch(t, /acceleration|graphics driver/);
    assert.equal(el.copy, null);
});

test('other browsers get general steps and no address', () => {
    const el = notice();
    browser({ brands: ['Chromium'], ua: 'Mozilla/5.0 Vivaldi/7.0' }).showGfxNotice(el, { state: 'software', renderer: 'SwiftShader' });
    assert.match(text(el), /Your browser is drawing without your graphics card/);
    assert.match(text(el), /search for acceleration/);
    assert.equal(el.copy, null);
});

// The enhanced security check, with browser parts the test can watch.
// `worker` decides what the made-up worker does when started: post a number,
// fail, or nothing.
function jitPage({ ua = EDGE.ua, brands = EDGE.brands, stored = {}, storageThrows = false,
                   worker = 'post', ratio = 0.7, workerThrows = false } = {}) {
    const seen = { revoked: [], terminated: 0, sources: [], others: [] };
    const localStorage = {
        getItem(k) { if (storageThrows) throw new Error('blocked'); return k in stored ? stored[k] : null; },
        setItem(k, v) { if (storageThrows) throw new Error('blocked'); stored[k] = v; },
        removeItem(k) { if (storageThrows) throw new Error('blocked'); delete stored[k]; },
    };
    class Blob { constructor(parts) { seen.sources.push(parts.join('')); } }
    const URL = { createObjectURL: () => 'blob:test', revokeObjectURL: u => seen.revoked.push(u) };
    class Worker {
        constructor(url) { if (workerThrows) throw new Error('no workers'); this.url = url; }
        postMessage() {
            queueMicrotask(() => {
                if (worker === 'post') this.onmessage({ data: ratio });
                if (worker === 'fail') this.onerror(new Error('broke'));
            });
        }
        terminate() { seen.terminated++; }
    }
    const notices = {};
    const document = { querySelectorAll: () => seen.others, getElementById: id => notices[id] };
    const navigator = { userAgent: ua, userAgentData: { brands: brands.map(brand => ({ brand })) } };
    const fns = new Function('document', 'navigator', 'window', 'localStorage', 'Worker', 'URL', 'Blob',
        code + jitCode + '\nreturn { jitBench, jitWorkerSource, jitCheck, showJitNotice, JIT_SLOW_RATIO, jitLastSlow, rememberJit };')(
        document, navigator, {}, localStorage, Worker, URL, Blob);
    // The page's own start of the test (Timed once...), with the notice el.
    const start = (el) => {
        notices['jit-notice'] = el;
        return new Function('document', 'navigator', 'window', 'localStorage', 'Worker', 'URL', 'Blob', 'browserProblem',
            code + jitCode + timedCode + '\nreturn { jitCheck };')(document, navigator, {}, localStorage, Worker, URL, Blob, () => null);
    };
    return { ...fns, seen, stored, start };
}

function jitNotice() {
    const el = { hidden: true, innerHTML: '', hide: null };
    el.querySelector = sel => {
        if (sel !== '.hide' || !el.innerHTML.includes('class="hide"')) return null;
        return el.hide = { addEventListener(type, fn) { el.hide.click = fn; } };
    };
    return el;
}

test('the speed test is quick where WebAssembly is compiled', async () => {
    const p = jitPage();
    const ratio = await p.jitBench();
    assert.ok(Number.isFinite(ratio) && ratio > 0, String(ratio));
    assert.ok(ratio < p.JIT_SLOW_RATIO, 'Node compiles WebAssembly, ratio ' + ratio.toFixed(2));
});

test('the worker runs the speed test and posts the ratio', async () => {
    const posted = await new Promise((resolve, reject) => {
        const scope = {};
        new Function('self', 'postMessage', 'performance', 'WebAssembly',
            jitPage().jitWorkerSource() + '\nself.onmessage = onmessage;')(
            scope, resolve, performance, WebAssembly);
        scope.onmessage({ data: 'go' });
        setTimeout(() => reject(new Error('no message')), 20000);
    });
    assert.equal(typeof posted, 'number');
});

test('the check gives the ratio and cleans up', async () => {
    const p = jitPage({ ratio: 13.5 });
    assert.equal(await p.jitCheck(), 13.5);
    assert.equal(p.seen.terminated, 1);
    assert.deepEqual(p.seen.revoked, ['blob:test']);
    assert.match(p.seen.sources[0], /WebAssembly\.instantiate/);
});

test('the check gives null when the worker fails or is missing', async () => {
    let p = jitPage({ worker: 'fail' });
    assert.equal(await p.jitCheck(), null);
    assert.equal(p.seen.terminated, 1);
    p = jitPage({ workerThrows: true });
    assert.equal(await p.jitCheck(), null);
    assert.deepEqual(p.seen.revoked, ['blob:test']);
    p = jitPage({ ratio: 'oops' });
    assert.equal(await p.jitCheck(), null);
});

test('Edge with a slow test gets the enhanced security steps', () => {
    const p = jitPage(), el = jitNotice();
    p.showJitNotice(el, 13.5);
    assert.equal(el.hidden, false);
    const t = text(el);
    assert.match(t, /Turn off enhanced security for this site/);
    assert.match(t, /Click the lock at the left of the address bar/);
    assert.match(t, /Click Enhanced security is active for this site/);
    assert.match(t, /Turn off Use enhanced security for this site, then reload this page/);
    assert.ok(el.hide, 'has a Hide button');
});

test('no enhanced security notice when the test is fast, failed, or not in Edge', () => {
    const el = jitNotice();
    for (const ratio of [0.7, 3.9, null, NaN]) {
        el.hidden = false;
        jitPage().showJitNotice(el, ratio);
        assert.equal(el.hidden, true, String(ratio));
    }
    for (const b of [{ ua: 'Mozilla/5.0 Chrome/155.0.0.0', brands: ['Google Chrome', 'Chromium'] },
                     { ua: 'Mozilla/5.0 Gecko/20100101 Firefox/140.0', brands: [] }]) {
        el.hidden = false;
        jitPage(b).showJitNotice(el, 13.5);
        assert.equal(el.hidden, true, b.ua);
    }
});

test('Hide keeps the enhanced security notice away', () => {
    const p = jitPage(), el = jitNotice(), other = { hidden: false };
    p.seen.others.push(el, other);
    p.showJitNotice(el, 13.5);
    el.hide.click();
    assert.equal(p.stored['jkdf2mp-jit-notice'], 'hidden');
    assert.equal(el.hidden, true);
    assert.equal(other.hidden, true);
    const again = jitNotice();
    jitPage({ stored: { 'jkdf2mp-jit-notice': 'hidden' } }).showJitNotice(again, 13.5);
    assert.equal(again.hidden, true);
});

test('blocked storage still shows the notice, and Hide still hides it', () => {
    const p = jitPage({ storageThrows: true }), el = jitNotice();
    p.seen.others.push(el);
    p.showJitNotice(el, 13.5);
    assert.equal(el.hidden, false);
    el.hide.click();
    assert.equal(el.hidden, true);
});

// ---- The resist-fingerprinting note. A browser with WebGL 2 that names its
// renderer `renderer` and holds textures and renderbuffers to `most` pixels,
// and a clock that steps by `step` ms.
function privacyBrowser({ renderer = 'Mozilla', most = 2048, step = 100 / 6, ua = FIREFOX, webgl = true } = {}) {
    const lost = [];
    const gl = {
        RENDERER: 0x1F01, MAX_TEXTURE_SIZE: 0x0D33, MAX_RENDERBUFFER_SIZE: 0x84E8,
        getExtension(name) { return name === 'WEBGL_lose_context' ? { loseContext() { lost.push(1); } } : null; },
        getParameter(p) { return p === 0x1F01 ? renderer : most; },
    };
    const document = { createElement: () => ({ getContext: () => (webgl ? gl : null) }) };
    let now = 1000, reads = 0;
    // Each read moves the real clock on 0.05 ms; the browser shows it in steps.
    const performance = { now: () => { reads++; return Math.floor((now += 0.05) / step) * step; } };
    const fns = new Function('document', 'navigator', 'window', 'performance', 'location',
        code + '\nreturn { privacyMode, showPrivacyNote };')(document, { userAgent: ua }, {}, performance, { hostname: 'jediknight.zone' });
    return { ...fns, lost, reads: () => reads };
}
const FIREFOX = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:159.0) Gecko/20100101 Firefox/159.0';

test('resist-fingerprinting is told by its hidden renderer, 2048-pixel limit and 16.7 ms clock together', () => {
    const rfp = privacyBrowser();
    assert.equal(rfp.privacyMode('Mozilla'), true);
    assert.equal(rfp.lost.length, 1, 'the test context is let go');
    // LibreWolf calls itself Firefox too.
    assert.equal(privacyBrowser({ ua: 'Mozilla/5.0 (X11; Linux x86_64; rv:150.0) Gecko/20100101 Firefox/150.0 LibreWolf/150.0' })
        .privacyMode('Mozilla'), true);
});

test('no resist-fingerprinting note for a named card, a real 2048 limit, a fine clock or another browser', () => {
    const named = privacyBrowser({ renderer: 'llvmpipe, or similar' });
    assert.equal(named.privacyMode('llvmpipe, or similar'), false);
    assert.equal(named.reads(), 0, 'the clock is only timed where the other signs are there');
    assert.equal(privacyBrowser({ most: 16384 }).privacyMode('Mozilla'), false, 'Firefox hiding the name only');
    assert.equal(privacyBrowser({ step: 1 }).privacyMode('Mozilla'), false, 'a card that stops at 2048');
    assert.equal(privacyBrowser({ ua: 'Mozilla/5.0 Chrome/155.0.0.0' }).privacyMode('Mozilla'), false);
    assert.equal(privacyBrowser({ webgl: false }).privacyMode('Mozilla'), false);
});

test('the resist-fingerprinting note says what it limits, and how to lift it for this site', () => {
    const { showPrivacyNote } = privacyBrowser();
    const el = notice();
    let site = null;
    el.querySelector = sel => (sel === '.site' ? { set textContent(t) { site = t; } } : null);
    showPrivacyNote(el, true);
    assert.equal(el.hidden, false);
    assert.match(text(el), /resist-fingerprinting is on/);
    assert.match(text(el), /2048 pixels across/);
    assert.match(text(el), /screenshots come out as noise/);
    assert.match(text(el), /privacy\.resistFingerprinting\.exemptedDomains/);
    assert.equal(site, 'jediknight.zone');
    showPrivacyNote(el, false);
    assert.equal(el.hidden, true);
});

// ---- Notices from the first paint. The page fills them in a script just
// after them, before the rest of the page is read, so they never push it
// down; the Edge test's slow answer is remembered for the next visit.

test('the notices are filled in before the rest of the page is read', () => {
    const at = s => { const i = html.indexOf(s); assert.ok(i > 0, 'index.html has ' + s); return i; };
    const layout = at('<div class="layout">');
    for (const call of ["showGfxNotice(document.getElementById('gfx-notice'), gfxCheck);",
                        "jitCheck().then(function(r) {"]) {
        assert.ok(at(call) > at('id="move-notice"'), call + ' comes after the notices');
        assert.ok(at(call) < layout, call + ' comes before the page under them');
    }
    const script = html.lastIndexOf('<script', at("showGfxNotice(document.getElementById('gfx-notice'), gfxCheck);"));
    assert.ok(script > at('id="move-notice"'), 'in a script of its own, after the notices');
});

test('a slow test is remembered, a fast one forgets it, one that could not run leaves it', () => {
    const p = jitPage();
    assert.equal(p.jitLastSlow(), null);
    p.rememberJit(13.5);
    assert.equal(p.stored['jkdf2mp-jit-slow'], '13.5');
    assert.equal(p.jitLastSlow(), 13.5);
    p.rememberJit(null);
    assert.equal(p.jitLastSlow(), 13.5, 'kept when the test could not run');
    p.rememberJit(0.8);
    assert.equal('jkdf2mp-jit-slow' in p.stored, false);
    assert.equal(p.jitLastSlow(), null);
    for (const v of ['', 'x', '3.9', '-20'])
        assert.equal(jitPage({ stored: { 'jkdf2mp-jit-slow': v } }).jitLastSlow(), null, JSON.stringify(v));
    const blocked = jitPage({ storageThrows: true });
    assert.equal(blocked.jitLastSlow(), null);
    blocked.rememberJit(13.5);
});

// A notice element that counts how often it's filled.
function countedNotice() {
    const el = jitNotice();
    let html = '';
    el.fills = 0;
    Object.defineProperty(el, 'innerHTML', { get: () => html, set: v => { html = v; el.fills++; } });
    return el;
}
const settle = () => new Promise(r => setTimeout(r, 0));

test('Edge shows a remembered slow test from the start, and keeps it while still slow', async () => {
    const p = jitPage({ ratio: 14, stored: { 'jkdf2mp-jit-slow': '13.5' } }), el = countedNotice();
    p.start(el);
    assert.equal(el.hidden, false, 'shown before the test ends');
    assert.equal(el.fills, 1);
    await settle();
    assert.equal(el.hidden, false);
    assert.equal(el.fills, 1, 'not filled again, so nothing moves');
    assert.equal(p.stored['jkdf2mp-jit-slow'], '14');
});

test('a remembered slow test that is fast now hides the notice and forgets it', async () => {
    const p = jitPage({ ratio: 0.8, stored: { 'jkdf2mp-jit-slow': '13.5' } }), el = countedNotice();
    p.start(el);
    assert.equal(el.hidden, false);
    await settle();
    assert.equal(el.hidden, true);
    assert.equal('jkdf2mp-jit-slow' in p.stored, false);
});

test('the first slow test shows the notice when it ends, and is remembered', async () => {
    const p = jitPage({ ratio: 13.5 }), el = countedNotice();
    p.start(el);
    assert.equal(el.hidden, true, 'nothing known yet');
    await settle();
    assert.equal(el.hidden, false);
    assert.equal(p.stored['jkdf2mp-jit-slow'], '13.5');
});

test('a test that cannot run leaves a remembered notice up', async () => {
    const p = jitPage({ worker: 'fail', stored: { 'jkdf2mp-jit-slow': '13.5' } }), el = countedNotice();
    p.start(el);
    await settle();
    assert.equal(el.hidden, false);
    assert.equal(p.stored['jkdf2mp-jit-slow'], '13.5');
});

test('Hide still wins over a remembered slow test, and other browsers never start it', async () => {
    let p = jitPage({ ratio: 14, stored: { 'jkdf2mp-jit-slow': '13.5', 'jkdf2mp-jit-notice': 'hidden' } }), el = countedNotice();
    p.start(el);
    await settle();
    assert.equal(el.hidden, true);
    p = jitPage({ ua: 'Mozilla/5.0 Chrome/155.0.0.0', brands: ['Google Chrome'], stored: { 'jkdf2mp-jit-slow': '13.5' } });
    el = countedNotice();
    p.start(el);
    await settle();
    assert.equal(el.hidden, true);
    assert.equal(el.fills, 0);
    assert.equal(p.seen.sources.length, 0, 'no speed test outside Edge');
});

// ---- Something going wrong in the script after the notices. It runs
// before the page's error handling is set up, and a throw would skip the
// rest of it, so its checks are wrapped: what goes wrong is logged, the
// script still defines everything the page uses later, and the page's later
// scripts run (here the game folder prompt's own notices, which read
// gfxCheck, jitRatio and privacyOn). Each script runs in one made-up window,
// as a browser runs them.

const noticesScript = cut('      // ---- The notices above, filled in here', '        </script>\n\n        <div class="layout">');
const pickerNotices = '        var problem = browserProblem();\n' +
    cut("        showGfxNotice(document.getElementById('picker-gfx')", '        if (!picker.open) picker.showModal();');

function noticesWindow({ glThrows = false, missing = [], stored = {}, ratio = 13.5 } = {}) {
    const errors = [], warnings = [];
    const els = { 'gfx-notice': notice(), 'picker-gfx': notice(), 'jit-notice': jitNotice(), 'picker-jit': jitNotice(),
                  'picker-privacy': { hidden: true } };
    missing.forEach(id => delete els[id]);
    const gl = () => ({
        RENDERER: 0x1F01,
        // A WebGL that breaks where the page doesn't expect it to.
        getExtension(name) { if (glThrows) throw new TypeError('getExtension broke'); return null; },
        getParameter() { return 'Google SwiftShader'; },
    });
    const document = {
        createElement: () => ({ getContext: () => gl() }),
        getElementById: id => els[id] || null,
        querySelectorAll: () => [],
    };
    const navigator = { userAgent: EDGE.ua, userAgentData: { brands: EDGE.brands.map(brand => ({ brand })) } };
    stored = { ...stored };
    const localStorage = {
        getItem: k => k in stored ? stored[k] : null,
        setItem: (k, v) => { stored[k] = v; },
        removeItem: k => { delete stored[k]; },
    };
    const sources = [];
    class Blob { constructor(parts) { sources.push(parts.join('')); } }
    class Worker {
        postMessage() { queueMicrotask(() => this.onmessage({ data: ratio })); }
        terminate() {}
    }
    const console = { error: (...a) => errors.push(a), warn: (...a) => warnings.push(a), log() {} };
    const ctx = vm.createContext({ document, navigator, localStorage, Worker, Blob, console,
        URL: { createObjectURL: () => 'blob:test', revokeObjectURL() {} }, location: { hostname: 'example.test' },
        setTimeout, clearTimeout, queueMicrotask, performance });
    ctx.window = ctx;
    return { ctx, els, errors, warnings, sources, stored,
             run: code => vm.runInContext(code, ctx) };
}

test('a throw in the graphics check is logged, and the rest of the page still runs', async () => {
    const w = noticesWindow({ glThrows: true });
    w.run(noticesScript);
    assert.equal(w.errors.length, 1, 'logged once');
    assert.match(w.errors[0][0], /^Graphics: /);
    assert.equal(w.errors[0][1].message, 'getExtension broke', 'with what was thrown');
    assert.equal(w.els['gfx-notice'].hidden, true, 'no notice');
    assert.equal(w.run('gfxCheck'), null);
    assert.equal(w.run('privacyOn'), false);
    // The rest of the script ran: its settings are set and Edge's test started.
    assert.equal(w.run('JIT_SLOW_RATIO'), 4);
    assert.equal(w.run('JIT_NOTICE_SETTING'), 'jkdf2mp-jit-notice');
    assert.equal(w.sources.length, 1, "Edge's test started");
    await settle();
    assert.equal(w.run('jitRatio'), 13.5);
    assert.equal(w.els['jit-notice'].hidden, false);
    // A later script: the game folder prompt's notices.
    w.run(pickerNotices);
    assert.equal(w.els['picker-gfx'].hidden, true);
    assert.equal(w.els['picker-jit'].hidden, false);
    assert.equal(w.els['picker-privacy'].hidden, true);
    assert.equal(w.errors.length, 1, 'nothing else went wrong');
});

test("a throw in the start of Edge's test is logged, and the rest of the page still runs", () => {
    const w = noticesWindow({ missing: ['jit-notice'], stored: { 'jkdf2mp-jit-slow': '13.5' } });
    w.run(noticesScript);
    assert.equal(w.errors.length, 1, 'logged once');
    assert.match(w.errors[0][0], /enhanced security/);
    assert.ok(w.errors[0][1] instanceof w.run('TypeError'));
    assert.equal(w.els['gfx-notice'].hidden, false, 'the graphics notice is up as usual');
    assert.equal(w.run('gfxCheck.state'), 'software');
    assert.equal(w.run('jitRatio'), null);
    w.run(pickerNotices);
    assert.equal(w.els['picker-gfx'].hidden, false);
    assert.equal(w.els['picker-jit'].hidden, true);
    assert.equal(w.errors.length, 1);
});

test('with nothing wrong, the script after the notices logs no error', async () => {
    const w = noticesWindow();
    w.run(noticesScript);
    await settle();
    w.run(pickerNotices);
    assert.deepEqual(w.errors, []);
    assert.equal(w.els['gfx-notice'].hidden, false);
    assert.equal(w.els['jit-notice'].hidden, false);
    assert.equal(w.stored['jkdf2mp-jit-slow'], '13.5');
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
