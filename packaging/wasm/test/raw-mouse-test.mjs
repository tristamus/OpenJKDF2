// Tests for the Raw mouse input box in packaging/wasm/index.html: how it
// starts in each browser, that a new Firefox player on Windows starts with it
// ticked and one who has played here keeps the plain lock, that the start is
// kept, and what the box says. In Node:
// node packaging/wasm/test/raw-mouse-test.mjs
//
// The page's code is taken from index.html as it is; the browser (user agent,
// storage, the saved databases, the box) is made up by the test.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const code = cut('      // ---- Mouse look.', '      // Opera, with its mouse gestures');

const FIREFOX_WIN = { ua: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:159.0) Gecko/20100101 Firefox/159.0',
                      platform: 'Win32' };
const FIREFOX_OLD = { ua: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:140.0) Gecko/20100101 Firefox/140.0',
                      platform: 'Win32' };
const FIREFOX_MAC = { ua: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:159.0) Gecko/20100101 Firefox/159.0',
                      platform: 'MacIntel' };
const FIREFOX_LINUX = { ua: 'Mozilla/5.0 (X11; Linux x86_64; rv:159.0) Gecko/20100101 Firefox/159.0',
                        platform: 'Linux x86_64' };
const EDGE_WIN = { ua: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) ' +
                       'Chrome/156.0.0.0 Safari/537.36 Edg/156.0.0.0', platform: 'Win32', uaPlatform: 'Windows' };
const CHROME_MAC = { ua: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) ' +
                         'Chrome/156.0.0.0 Safari/537.36', platform: 'MacIntel', uaPlatform: 'macOS' };

// A Storage stand-in, or one that throws on every call (blocked storage).
function storage(items = {}, { blocked = false } = {}) {
    const m = new Map(Object.entries(items));
    const no = () => { throw new Error('SecurityError'); };
    return blocked ? { getItem: no, setItem: no, removeItem: no } : {
        getItem: k => (m.has(k) ? m.get(k) : null),
        setItem: (k, v) => m.set(k, String(v)),
        removeItem: k => m.delete(k),
        get items() { return Object.fromEntries(m); },
    };
}

// The page's mouse-look code in a made-up browser. dbs: the IndexedDB
// databases this site has (names), or null for a browser without
// indexedDB.databases. view: how the page is shown (fullscreen: the page's own
// fullscreen; browserFullscreen: the browser's, F11; extended: more than one
// screen), which the test can change later through p.view.
async function open(browser, { local = storage(), dbs = [], view = {} } = {}) {
    const navigator = { userAgent: browser.ua, platform: browser.platform };
    if (browser.uaPlatform) navigator.userAgentData = { platform: browser.uaPlatform };
    const option = { checked: true, disabled: false, listeners: {},
                     addEventListener(type, fn) { this.listeners[type] = fn; } };
    const about = { textContent: '' };
    const v = { fullscreen: false, browserFullscreen: false, extended: undefined, ...view };
    const listeners = {};
    // refuse: the name of the error the next request is turned down with.
    const canvasListeners = {};
    const canvas = { refuse: null, addEventListener(type, fn) { (canvasListeners[type] = canvasListeners[type] || []).push(fn); },
                     requestPointerLock(options) {
        locks.push(options || null);
        const name = this.refuse;
        this.refuse = null;
        return name ? Promise.reject(Object.assign(new Error(name), { name })) : Promise.resolve();
    } };
    const document = {
        getElementById: id => (id === 'raw-mouse-option' ? option : null),
        querySelector: sel => (sel === '#raw-mouse-choice .raw-mouse-about' ? about : null),
        addEventListener(type, fn) { (listeners[type] = listeners[type] || []).push(fn); },
        pointerLockElement: null,
        get fullscreenElement() { return v.fullscreen ? {} : null; },
        exited: 0,
        exitPointerLock() { this.exited++; this.pointerLockElement = null; fire('pointerlockchange'); },
    };
    const screen = { get isExtended() { return v.extended; } };
    const window = { matchMedia: q => ({ matches: q === '(display-mode: fullscreen)' && (v.browserFullscreen || v.fullscreen) }) };
    const locks = [];
    const printed = [];
    const Module = { canvas, print: t => printed.push(t) };
    const indexedDB = dbs === null ? {} : { databases: () => Promise.resolve(dbs.map(name => ({ name, version: 21 }))) };
    new Function('Module', 'navigator', 'document', 'localStorage', 'indexedDB', 'screen', 'window', code)(
        Module, navigator, document, local, indexedDB, screen, window);
    await new Promise(resolve => setTimeout(resolve, 0));
    const fire = type => (listeners[type] || []).forEach(fn => fn());
    // A lock as the page's code takes it: asked for, then granted.
    async function lock() {
        await canvas.requestPointerLock();
        document.pointerLockElement = canvas;
        fire('pointerlockchange');
    }
    function unlock() { document.pointerLockElement = null; fire('pointerlockchange'); }
    // A mouse button pressed on the game.
    const press = () => (canvasListeners.pointerdown || []).forEach(fn => fn({ target: canvas }));
    return { option, about, local, raw: Module.jkRawMouse, lock, unlock, locks, view: v, document, fire, printed, canvas, press };
}
const settle = () => new Promise(resolve => setTimeout(resolve, 0));

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test('Firefox on Windows: a new player starts ticked, and it is kept', async () => {
    const local = storage();
    const p = await open(FIREFOX_WIN, { local });
    assert.equal(p.option.checked, true);
    assert.equal(p.raw.want, true);
    assert.equal(local.items['jkdf2mp-raw-mouse-start'], 'on');
    assert.equal(local.items['jkdf2mp-raw-mouse'], undefined, "the player's own choice isn't made for them");
    await p.lock();
    assert.deepEqual(p.locks, [{ unadjustedMovement: true }]);
    // Next visit, after playing: the saved characters are there now.
    const again = await open(FIREFOX_WIN, { local, dbs: ['/jk1/player', '/jk1/persist'] });
    assert.equal(again.option.checked, true, 'stays ticked once they have played');
    assert.equal(local.items['jkdf2mp-raw-mouse-start'], 'on');
});

test('Firefox on Windows: a player who has played here keeps the plain lock', async () => {
    for (const saved of ['/jk1/player', '/mots/player', '/jk1/persist', '/mots/persist']) {
        const local = storage();
        const p = await open(FIREFOX_WIN, { local, dbs: ['jkdf2mp-screenshots', saved] });
        assert.equal(p.option.checked, false, saved);
        assert.equal(p.raw.want, false, saved);
        assert.equal(local.items['jkdf2mp-raw-mouse-start'], 'off', saved);
        await p.lock();
        assert.deepEqual(p.locks, [null], 'a plain lock');
        const again = await open(FIREFOX_WIN, { local, dbs: [] });
        assert.equal(again.option.checked, false, 'stays unticked even if the saves go: ' + saved);
    }
    // Other databases (maps, mods, screenshots) aren't playing here.
    const p = await open(FIREFOX_WIN, { dbs: ['jkdf2mp-maps', 'jkdf2mp-mods', 'jkdf2mp-screenshots'] });
    assert.equal(p.option.checked, true);
});

test("the player's own choice wins", async () => {
    for (const [mine, dbs, checked] of [['off', [], false], ['on', ['/jk1/player'], true]]) {
        const local = storage({ 'jkdf2mp-raw-mouse': mine });
        const p = await open(FIREFOX_WIN, { local, dbs });
        assert.equal(p.option.checked, checked, mine);
        assert.equal(local.items['jkdf2mp-raw-mouse-start'], undefined, mine);
    }
    // Ticking or unticking the box is kept as theirs.
    const local = storage();
    const p = await open(FIREFOX_WIN, { local });
    p.option.checked = false;
    p.option.listeners.change();
    assert.equal(local.items['jkdf2mp-raw-mouse'], 'off');
    const again = await open(FIREFOX_WIN, { local });
    assert.equal(again.option.checked, false);
});

test('without storage or the database list, Firefox keeps the plain lock', async () => {
    let p = await open(FIREFOX_WIN, { local: storage({}, { blocked: true }) });
    assert.equal(p.option.checked, false);
    p = await open(FIREFOX_WIN, { dbs: null });
    assert.equal(p.option.checked, false);
});

test('other browsers start as before', async () => {
    let p = await open(EDGE_WIN);
    assert.equal(p.option.checked, true, 'Edge on Windows');
    assert.equal(p.local.items['jkdf2mp-raw-mouse-start'], undefined);
    p = await open(EDGE_WIN, { dbs: ['/jk1/player'] });
    assert.equal(p.option.checked, true, 'Edge on Windows, played before');
    p = await open(CHROME_MAC);
    assert.equal(p.option.checked, false, 'Chrome on macOS');
    p = await open(FIREFOX_MAC);
    assert.equal(p.option.checked, false, 'Firefox on macOS');
    assert.equal(p.local.items['jkdf2mp-raw-mouse-start'], undefined);
    p = await open(FIREFOX_OLD);
    assert.equal(p.option.disabled, true, 'Firefox before 152');
    assert.equal(p.option.checked, false);
    assert.equal(p.local.items['jkdf2mp-raw-mouse-start'], undefined);
    p = await open(FIREFOX_LINUX);
    assert.equal(p.option.disabled, true, 'Linux');
});

test('the box says page zoom slows plain mouse look in Firefox, not elsewhere', async () => {
    let p = await open(FIREFOX_WIN);
    assert.equal(p.about.textContent,
        "Ticked, mouse look ignores Windows' pointer speed, Enhance pointer precision and page zoom. Unticked, it " +
        'uses your Windows mouse settings, acceleration included, and zooming the page in (Ctrl and +) makes it turn ' +
        "slower. Untick it if mouse look doesn't turn, as over remote desktop or with a drawing tablet.");
    p = await open(FIREFOX_OLD);
    assert.equal(p.about.textContent,
        'Needs Firefox 152 or newer: update Firefox to use it. Mouse look uses your Windows mouse settings, ' +
        'acceleration included, and zooming the page in (Ctrl and +) makes it turn slower.');
    p = await open(FIREFOX_LINUX);
    assert.match(p.about.textContent, /^Not available here: .*zooming the page in \(Ctrl and \+\) makes it turn slower\.$/);
    p = await open(EDGE_WIN);
    assert.equal(p.about.textContent,
        "Ticked, mouse look in fullscreen ignores Windows' pointer speed and Enhance pointer precision, so the view " +
        "can't suddenly jolt. Unticked, and always in a window (where the hidden cursor could slip off the page), it " +
        'uses your Windows mouse settings, acceleration included. Untick it if mouse look ' +
        "doesn't turn, as over remote desktop or with a drawing tablet.");
    p = await open(FIREFOX_MAC);
    assert.doesNotMatch(p.about.textContent, /zoom/);
    p = await open(CHROME_MAC);
    assert.doesNotMatch(p.about.textContent, /zoom/);
});

test('Chromium on Windows: raw only while the page fills the only screen', async () => {
    // In a window the hidden cursor slipped off the page (Comet, 21 Nov 2026): the plain lock.
    let p = await open(EDGE_WIN);
    await p.lock();
    assert.deepEqual(p.locks, [null], 'in a window');
    assert.equal(p.raw.active, false);
    p.unlock();
    p.view.fullscreen = true;
    await p.lock();
    assert.deepEqual(p.locks, [null, { unadjustedMovement: true }], "the page's fullscreen");
    assert.equal(p.raw.active, true);
    p.unlock();
    assert.equal(p.raw.active, false);
    p.view.fullscreen = false;
    p.view.browserFullscreen = true;
    await p.lock();
    assert.deepEqual(p.locks.at(-1), { unadjustedMovement: true }, "the browser's fullscreen (F11)");
    p.unlock();
    // More than one screen: the cursor could slip onto the other one.
    p.view.extended = true;
    p.view.fullscreen = true;
    await p.lock();
    assert.equal(p.locks.at(-1), null, 'fullscreen, two screens');
    p.unlock();
    // Box unticked: plain everywhere, as before.
    p = await open(EDGE_WIN, { local: storage({ 'jkdf2mp-raw-mouse': 'off' }), view: { fullscreen: true } });
    await p.lock();
    assert.deepEqual(p.locks, [null]);
});

test('Chromium on Windows: the click that asks for fullscreen locks raw', async () => {
    const p = await open(EDGE_WIN);
    p.raw.fullscreenAskedAt = performance.now();  // playFullscreen, earlier in the same click
    await p.lock();
    assert.deepEqual(p.locks, [{ unadjustedMovement: true }]);
    p.unlock();
    p.raw.fullscreenAskedAt = performance.now() - 5000;  // long ago, and still in a window
    await p.lock();
    assert.deepEqual(p.locks.at(-1), null);
});

test('Chromium on Windows: leaving fullscreen with a raw lock lets go of the mouse', async () => {
    const p = await open(EDGE_WIN, { view: { fullscreen: true } });
    await p.lock();
    assert.equal(p.raw.active, true);
    p.view.fullscreen = false;
    p.fire('fullscreenchange');
    assert.equal(p.document.exited, 1);
    assert.equal(p.raw.active, false);
    // Not when the lock is plain, or there's none, or fullscreen just began.
    const q = await open(EDGE_WIN);
    await q.lock();
    q.fire('fullscreenchange');
    assert.equal(q.document.exited, 0, 'a plain lock in a window');
    q.unlock();
    q.view.fullscreen = true;
    await q.lock();
    q.fire('fullscreenchange');
    assert.equal(q.document.exited, 0, 'still fullscreen');
    // Left fullscreen into the browser's own (F11): still fills the screen.
    q.view.fullscreen = false;
    q.view.browserFullscreen = true;
    q.fire('fullscreenchange');
    assert.equal(q.document.exited, 0, "into the browser's fullscreen");
});

test('Chromium on Windows: a plain lock taken in a window turns raw when fullscreen starts', async () => {
    // The game takes the mouse by itself as the level starts, before the
    // first click (the Free Demo, kit 179 on beta): plain, in a window.
    const p = await open(EDGE_WIN);
    await p.lock();
    assert.deepEqual(p.locks, [null]);
    p.view.fullscreen = true;  // the next click starts fullscreen
    p.fire('fullscreenchange');
    await settle();
    assert.deepEqual(p.locks, [null, { unadjustedMovement: true }], 'asked again, raw');
    assert.equal(p.raw.active, true);
    assert.equal(p.document.exited, 0, 'without letting go of the mouse');
    assert.match(p.printed.at(-1), /raw now/);
    // Leaving fullscreen lets go, as for any raw lock.
    p.view.fullscreen = false;
    p.fire('fullscreenchange');
    assert.equal(p.document.exited, 1);
    // Turned down: the plain lock stays, and so does the box.
    const q = await open(EDGE_WIN);
    await q.lock();
    q.view.fullscreen = true;
    q.canvas.refuse = 'NotAllowedError';
    q.fire('fullscreenchange');
    await settle();
    assert.equal(q.locks.length, 2);
    assert.equal(q.raw.active, false);
    assert.notEqual(q.raw.supported, false);
    assert.equal(q.option.disabled, false);
    assert.equal(q.document.exited, 0);
    assert.match(q.printed.at(-1), /stays plain for now \(NotAllowedError\)/);
    // The next click in the game asks once more, and that can be granted.
    q.press();
    await settle();
    assert.deepEqual(q.locks.at(-1), { unadjustedMovement: true });
    assert.equal(q.locks.length, 3);
    assert.equal(q.raw.active, true, 'raw after the click');
    q.press();
    await settle();
    assert.equal(q.locks.length, 3, 'not asked again once raw');
    // Turned down both times: no more asking for this lock, until a new one.
    const t = await open(EDGE_WIN);
    await t.lock();
    t.view.fullscreen = true;
    t.canvas.refuse = 'NotAllowedError';
    t.fire('fullscreenchange');
    await settle();
    t.canvas.refuse = 'NotAllowedError';
    t.press();
    await settle();
    t.press();
    await settle();
    assert.equal(t.locks.length, 3, 'two tries per lock');
    assert.equal(t.raw.active, false);
    t.unlock();
    await t.lock();  // the next lock in fullscreen goes raw from the start
    assert.deepEqual(t.locks.at(-1), { unadjustedMovement: true });
    assert.equal(t.raw.active, true);
    // A click in a window asks for nothing (the plain lock is right there).
    const w = await open(EDGE_WIN);
    await w.lock();
    w.press();
    await settle();
    assert.deepEqual(w.locks, [null], 'in a window');
    // Not asked again with the box unticked, or with two screens.
    for (const [why, setup] of [['box unticked', { local: storage({ 'jkdf2mp-raw-mouse': 'off' }) }],
                                ['two screens', { view: { extended: true } }]]) {
        const r = await open(EDGE_WIN, setup);
        await r.lock();
        r.view.fullscreen = true;
        r.fire('fullscreenchange');
        await settle();
        assert.deepEqual(r.locks, [null], why);
    }
    // Nor with no lock held, nor when the lock is raw already.
    const s = await open(EDGE_WIN);
    s.view.fullscreen = true;
    s.fire('fullscreenchange');
    await settle();
    assert.deepEqual(s.locks, [], 'no lock');
    await s.lock();
    s.fire('fullscreenchange');
    await settle();
    assert.deepEqual(s.locks, [{ unadjustedMovement: true }], 'raw already');
});

test('elsewhere a lock is raw in a window too, as before', async () => {
    for (const browser of [FIREFOX_WIN, CHROME_MAC]) {
        const p = await open(browser, { local: storage({ 'jkdf2mp-raw-mouse': 'on' }) });
        await p.lock();
        assert.deepEqual(p.locks, [{ unadjustedMovement: true }], browser.ua);
        p.view.fullscreen = true;
        p.fire('fullscreenchange');
        await settle();
        assert.equal(p.locks.length, 1, 'not asked again in fullscreen: ' + browser.ua);
    }
});

let failed = 0;
for (const { name, fn } of tests) {
    try {
        await fn();
        console.log('ok   ' + name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + name + '\n     ' + (e && e.stack || e));
    }
}
console.log(failed ? failed + ' of ' + tests.length + ' failed' : 'all ' + tests.length + ' passed');
process.exit(failed ? 1 : 0);
