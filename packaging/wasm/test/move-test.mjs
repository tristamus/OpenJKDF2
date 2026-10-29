// Tests moving a player's saved files from the old address to the new one
// (moveIn in packaging/wasm/index.html and move-out.html) in a real browser:
// two local servers stand in for the two addresses, and the page's MOVE_FROM
// and MOVE_TO are pointed at them. Needs Playwright with Chromium, which the
// other tests in this folder don't:
// node packaging/wasm/test/move-test.mjs
//
// Every file is made up by the test.

import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const dir = new URL('../', import.meta.url);
const OLD = 'http://localhost:8961', NEW = 'http://127.0.0.1:8962', OTHER = 'http://localhost:8963';

function page(name) {
    let html = readFileSync(new URL(name, dir), 'utf8');
    for (const [from, to] of [["var MOVE_FROM = 'https://jkdf2mp.jkdf2mp-lobby.workers.dev';", `var MOVE_FROM = '${OLD}';`],
                              ["var MOVE_TO = 'https://jediknight.zone';", `var MOVE_TO = '${NEW}';`]]) {
        if (html.includes(from)) html = html.replace(from, to);
        else assert.ok(name !== 'index.html' || !from.includes('MOVE_FROM'), name + ' has ' + from);
    }
    assert.ok(html.includes(`var MOVE_TO = '${NEW}';`), name + ' has MOVE_TO');
    return html;
}
const files = { '/': page('index.html'), '/move-out.html': page('move-out.html'),
                '/blank.html': '<!doctype html><title>blank</title>' };
const servers = [OLD, NEW, OTHER].map(origin => {
    const server = createServer((req, res) => {
        const body = files[new URL(req.url, origin).pathname];
        res.writeHead(body ? 200 : 404, { 'Content-Type': 'text/html' });
        res.end(body || 'not found');
    });
    server.listen(Number(new URL(origin).port), new URL(origin).hostname === 'localhost' ? 'localhost' : '127.0.0.1');
    return server;
});

// ---- Run in the browser: what the page saves, made up, and a summary of
// what's there to compare the two addresses by.

const seedOld = async () => {
    const put = (name, version, stores, records) => new Promise((resolve, reject) => {
        const req = indexedDB.open(name, version);
        req.onupgradeneeded = () => {
            for (const [store, opts, indexes] of stores) {
                const os = req.result.createObjectStore(store, opts);
                for (const [ix, keyPath] of indexes || []) os.createIndex(ix, keyPath, { unique: false });
            }
        };
        req.onsuccess = () => {
            const db = req.result, tx = db.transaction(stores.map(s => s[0]), 'readwrite');
            for (const [store, value, key] of records) {
                if (key === undefined) tx.objectStore(store).put(value);
                else tx.objectStore(store).put(value, key);
            }
            tx.oncomplete = () => { db.close(); resolve(); };
            tx.onerror = () => reject(tx.error);
        };
        req.onerror = () => reject(req.error);
    });
    const bytes = (n, seed) => {
        const a = new Uint8Array(n);
        for (let i = 0; i < n; i++) a[i] = (i * 31 + seed) & 255;
        return a;
    };
    localStorage.setItem('jkdf2mp-page-style', '2001');
    localStorage.setItem('jkdf2mp-raw-mouse', 'on');
    localStorage.setItem('jkdf2mp-room-view', 'list');
    localStorage.setItem('someone-else', 'x');
    // IDBFS, as Emscripten keeps it.
    const when = new Date(1790000000000);
    await put('/jk1/player', 21, [['FILE_DATA', {}, [['timestamp', 'timestamp']]]], [
        ['FILE_DATA', { timestamp: when, mode: 16893 }, '/jk1/player'],
        ['FILE_DATA', { timestamp: when, mode: 16893 }, '/jk1/player/Tester'],
        ['FILE_DATA', { timestamp: when, mode: 33206, contents: bytes(1234, 1) }, '/jk1/player/Tester/Tester.plr']]);
    await put('/jk1/persist', 21, [['FILE_DATA', {}, [['timestamp', 'timestamp']]]], [
        ['FILE_DATA', { timestamp: when, mode: 33206, contents: bytes(300, 2) }, '/jk1/persist/openjkdf2.json']]);
    await put('jkdf2mp-game-files', 1, [['meta', { keyPath: 'game' }], ['data', {}]], [
        ['meta', { game: 'jk1', folder: 'Jedi Knight', files: 2, bytes: 3000, time: 1,
                   list: [{ path: 'resource/Res2.gob', size: 2000, time: 5 }, { path: 'episode/JK1.gob', size: 1000, time: 6 }] }],
        ['data', { file: new File([bytes(2000, 3)], 'Res2.gob', { lastModified: 5 }) }, 'jk1/resource/res2.gob'],
        ['data', { file: new File([bytes(1000, 4)], 'JK1.gob', { lastModified: 6 }) }, 'jk1/episode/jk1.gob']]);
    // Two mods past a batch's 32 MB, so they come over in more than one.
    await put('jkdf2mp-mods', 1, [['meta', { keyPath: 'key' }], ['data', {}]], [
        ['meta', { key: 'jk1/a', name: 'a', bytes: 20 << 20 }], ['meta', { key: 'jk1/b', name: 'b', bytes: 20 << 20 }],
        ['meta', { key: 'jk1/c', name: 'c', bytes: 10 }],
        ['data', bytes(20 << 20, 5), 'jk1/a'], ['data', bytes(20 << 20, 6), 'jk1/b'], ['data', bytes(10, 7), 'jk1/c']]);
    await put('jkdf2mp-maps', 1, [['meta', { keyPath: 'key' }], ['data', {}]], [
        ['meta', { key: 'jk1/old-map', name: 'old-map' }], ['data', bytes(50, 8), 'jk1/old-map']]);
    await put('jkdf2mp-screenshots', 1, [['meta', { keyPath: 'key' }], ['data', {}]], [
        ['meta', { key: 's1', time: 2 }], ['data', new Blob([bytes(5000, 9)], { type: 'image/png' }), 's1']]);
    await put('jkdf2mp-kept-folders', 1, [['folders', { keyPath: 'game' }]], [['folders', { game: 'jk1', name: 'x' }]]);
};

// Already on the new address before the move: its own map and a page setting.
const seedNew = async () => {
    localStorage.setItem('jkdf2mp-raw-mouse', 'off');
    await new Promise((resolve, reject) => {
        const req = indexedDB.open('jkdf2mp-maps', 1);
        req.onupgradeneeded = () => {
            req.result.createObjectStore('meta', { keyPath: 'key' });
            req.result.createObjectStore('data');
        };
        req.onsuccess = () => {
            const tx = req.result.transaction(['meta', 'data'], 'readwrite');
            tx.objectStore('meta').put({ key: 'jk1/new-map', name: 'new-map' });
            tx.objectStore('data').put(new Uint8Array([1, 2, 3]), 'jk1/new-map');
            tx.oncomplete = () => { req.result.close(); resolve(); };
        };
        req.onerror = () => reject(req.error);
    });
};

// Every database and page setting, with a hash of each record's bytes.
const summary = async () => {
    const hex = async (buf) => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', buf)))
        .map(b => b.toString(16).padStart(2, '0')).join('').slice(0, 16);
    const flat = async (v) => {
        if (v instanceof Blob) return { blob: v.type + ':' + v.size + ':' + (v.name || '') + ':' + (v.lastModified || '') + ':' + await hex(await v.arrayBuffer()) };
        if (ArrayBuffer.isView(v)) return { bytes: v.byteLength + ':' + await hex(v) };
        if (v instanceof Date) return { date: v.getTime() };
        if (Array.isArray(v)) return Promise.all(v.map(flat));
        if (v && typeof v === 'object') {
            const out = {};
            for (const k of Object.keys(v).sort()) out[k] = await flat(v[k]);
            return out;
        }
        return v;
    };
    const out = { local: {}, dbs: {} };
    for (let i = 0; i < localStorage.length; i++) out.local[localStorage.key(i)] = localStorage.getItem(localStorage.key(i));
    for (const { name } of await indexedDB.databases()) {
        const db = await new Promise((resolve, reject) => {
            const req = indexedDB.open(name);
            req.onsuccess = () => resolve(req.result);
            req.onerror = () => reject(req.error);
        });
        const d = { version: db.version, stores: {} };
        for (const s of Array.from(db.objectStoreNames)) {
            const [keys, values, meta] = await new Promise((resolve) => {
                const store = db.transaction(s).objectStore(s);
                const k = store.getAllKeys(), v = store.getAll();
                const m = { keyPath: store.keyPath, indexes: Array.from(store.indexNames) };
                v.onsuccess = () => resolve([k.result, v.result, m]);
            });
            d.stores[s] = { ...meta, records: await Promise.all(keys.map(async (k, i) => [k, await flat(values[i])])) };
        }
        db.close();
        out.dbs[name] = d;
    }
    return out;
};

const tests = [];
const test = (name, fn) => tests.push({ name, fn });
const browser = await chromium.launch();

async function fresh() {
    const context = await browser.newContext();
    const oldPage = await context.newPage();
    await oldPage.goto(OLD + '/blank.html');
    const newPage = await context.newPage();
    return { context, oldPage, newPage };
}

test('everything comes over, nothing already here is changed, and the page reloads', async () => {
    const { context, oldPage, newPage } = await fresh();
    await oldPage.evaluate(seedOld);
    await newPage.goto(NEW + '/blank.html');
    await newPage.evaluate(seedNew);
    const before = await oldPage.evaluate(summary);

    await newPage.goto(NEW + '/?netstats&moved#room-7');
    assert.equal(await newPage.evaluate(() => location.search + location.hash), '?netstats#room-7');
    assert.ok(!(await newPage.evaluate(() => Module['arguments'])).includes('-moved'), 'moved goes to the engine');
    await newPage.locator('#move-notice').waitFor({ state: 'visible' });
    assert.equal(await newPage.evaluate(() => localStorage.getItem('jkdf2mp-move-in')), 'offer');

    const popup = newPage.waitForEvent('popup');
    const reloaded = newPage.waitForEvent('load', { timeout: 60000 });
    await newPage.locator('#move-notice [data-move-in]').click();
    const win = await popup;
    await newPage.locator('#move-notice .move-status', { hasText: 'Moved:' }).waitFor({ timeout: 60000 });
    const said = await newPage.locator('#move-notice .move-status').textContent();
    assert.match(said, /Moved: page settings, Jedi Knight settings, Jedi Knight characters and controls, saved game files, saved mods and screenshots\./);
    assert.match(said, /Already here, so left as they were: downloaded maps\./);
    await reloaded;
    assert.ok(win.isClosed(), 'the window from the old address closed');

    assert.equal(await newPage.locator('#move-notice').isVisible(), false);
    assert.equal(await newPage.evaluate(() => location.search + location.hash), '?netstats#room-7');
    const after = await newPage.evaluate(summary);
    assert.equal(after.local['jkdf2mp-page-style'], '2001');
    assert.equal(after.local['jkdf2mp-room-view'], 'list');
    assert.equal(after.local['jkdf2mp-raw-mouse'], 'off', 'a setting already here stays');
    assert.equal(after.local['someone-else'], undefined);
    assert.equal(after.local['jkdf2mp-move-in'], 'done');
    for (const name of ['/jk1/player', '/jk1/persist', 'jkdf2mp-game-files', 'jkdf2mp-mods', 'jkdf2mp-screenshots'])
        assert.deepEqual(after.dbs[name], before.dbs[name], name + ' came over as it was');
    assert.deepEqual(after.dbs['jkdf2mp-maps'].stores.meta.records, [['jk1/new-map', { key: 'jk1/new-map', name: 'new-map' }]]);
    // The page makes its own, empty, as it opens.
    assert.deepEqual(after.dbs['jkdf2mp-kept-folders']?.stores.folders.records || [], [], 'kept folders stay');
    assert.deepEqual(await oldPage.evaluate(summary), before, 'the old address keeps its own');
    await context.close();
});

test('asked again, everything already here is left as it is', async () => {
    const { context, oldPage, newPage } = await fresh();
    await oldPage.evaluate(seedOld);
    await newPage.goto(NEW + '/');
    await newPage.locator('#move-files [data-move-in]').click();
    await newPage.waitForEvent('load', { timeout: 60000 });
    await newPage.locator('#move-files [data-move-in]').click();
    await newPage.locator('#move-notice .move-status', { hasText: 'Already here' }).waitFor({ timeout: 60000 });
    const said = await newPage.locator('#move-notice .move-status').textContent();
    assert.ok(!said.includes('Moved:'), said);
    await context.close();
});

test('with nothing at the old address, it says so and stays', async () => {
    const { context, newPage } = await fresh();
    await newPage.goto(NEW + '/?moved');
    await newPage.locator('#move-notice [data-move-in]').click();
    await newPage.locator('#move-notice .move-status', { hasText: 'Nothing was saved' }).waitFor({ timeout: 30000 });
    assert.equal(await newPage.evaluate(() => localStorage.getItem('jkdf2mp-move-in')), 'done');
    await context.close();
});

test('Start fresh hides the notice for good; Your Files still offers the move', async () => {
    const { context, newPage } = await fresh();
    await newPage.goto(NEW + '/?moved');
    await newPage.locator('#move-notice [data-move-fresh]').click();
    assert.equal(await newPage.locator('#move-notice').isVisible(), false);
    await newPage.goto(NEW + '/?moved');
    await newPage.waitForTimeout(500);
    assert.equal(await newPage.locator('#move-notice').isVisible(), false);
    assert.equal(await newPage.locator('#move-files').count(), 1);
    assert.equal(await newPage.evaluate(() => document.getElementById('move-files').hidden), false);
    await context.close();
});

test('without ?moved there is no notice, and on another address no offer at all', async () => {
    const { context, oldPage, newPage } = await fresh();
    await newPage.goto(NEW + '/');
    await newPage.waitForTimeout(500);
    assert.equal(await newPage.locator('#move-notice').isVisible(), false);
    await oldPage.goto(OLD + '/?moved');
    await oldPage.waitForTimeout(500);
    assert.equal(await oldPage.evaluate(() => location.search), '');
    assert.equal(await oldPage.locator('#move-notice').isVisible(), false);
    assert.equal(await oldPage.evaluate(() => document.getElementById('move-files').hidden), true);
    await context.close();
});

test('the old address gives nothing to any other page', async () => {
    const { context, oldPage } = await fresh();
    await oldPage.evaluate(seedOld);
    const other = await context.newPage();
    await other.goto(OTHER + '/blank.html');
    const got = await other.evaluate(async (from) => {
        const seen = [];
        window.addEventListener('message', (ev) => seen.push(ev.data));
        const win = window.open(from + '/move-out.html', 'x', 'popup');
        await new Promise(r => setTimeout(r, 1500));
        win.postMessage({ jkdf2mpMove: 'list' }, '*');
        win.postMessage({ jkdf2mpMove: 'records', db: '/jk1/player', store: 'FILE_DATA', after: null }, '*');
        await new Promise(r => setTimeout(r, 1500));
        win.close();
        return seen;
    }, OLD);
    assert.deepEqual(got, []);
    // Opened on its own, it only points to the new address.
    await oldPage.goto(OLD + '/move-out.html');
    assert.match(await oldPage.locator('#status').textContent(), /Nothing to do here/);
    await context.close();
});

let failed = 0;
for (const t of tests) {
    try {
        await t.fn();
        console.log('ok   ' + t.name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + t.name + '\n     ' + (e.stack || e).toString().split('\n').slice(0, 12).join('\n     '));
    }
}
await browser.close();
servers.forEach(s => s.close());
console.log(failed ? failed + ' of ' + tests.length + ' failed' : 'all ' + tests.length + ' passed');
process.exit(failed ? 1 : 0);
