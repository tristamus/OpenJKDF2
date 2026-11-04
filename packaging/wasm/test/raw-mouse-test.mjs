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
// indexedDB.databases.
async function open(browser, { local = storage(), dbs = [] } = {}) {
    const navigator = { userAgent: browser.ua, platform: browser.platform };
    if (browser.uaPlatform) navigator.userAgentData = { platform: browser.uaPlatform };
    const option = { checked: true, disabled: false, listeners: {},
                     addEventListener(type, fn) { this.listeners[type] = fn; } };
    const about = { textContent: '' };
    const document = {
        getElementById: id => (id === 'raw-mouse-option' ? option : null),
        querySelector: sel => (sel === '#raw-mouse-choice .raw-mouse-about' ? about : null),
        addEventListener() {},
        pointerLockElement: null,
    };
    const locks = [];
    const canvas = { requestPointerLock(options) { locks.push(options || null); return Promise.resolve(); } };
    const Module = { canvas };
    const indexedDB = dbs === null ? {} : { databases: () => Promise.resolve(dbs.map(name => ({ name, version: 21 }))) };
    new Function('Module', 'navigator', 'document', 'localStorage', 'indexedDB', code)(
        Module, navigator, document, local, indexedDB);
    await new Promise(resolve => setTimeout(resolve, 0));
    return { option, about, local, raw: Module.jkRawMouse, lock: () => canvas.requestPointerLock(), locks };
}

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
        "Ticked, mouse look ignores Windows' pointer speed and Enhance pointer precision, so the view can't suddenly " +
        'jolt. Unticked, it uses your Windows mouse settings, acceleration included. Untick it if mouse look ' +
        "doesn't turn, as over remote desktop or with a drawing tablet.");
    p = await open(FIREFOX_MAC);
    assert.doesNotMatch(p.about.textContent, /zoom/);
    p = await open(CHROME_MAC);
    assert.doesNotMatch(p.about.textContent, /zoom/);
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
