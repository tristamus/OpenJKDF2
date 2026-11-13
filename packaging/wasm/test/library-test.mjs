// Tests for packaging/wasm/library.js, the Level Library screen the Host
// screen opens, in a real browser. Needs Playwright with Chromium, as
// move-test.mjs does: node packaging/wasm/test/library-test.mjs
//
// The levels are made up by the test and served the way the site's Worker
// serves them (src/levels.js in jkdf2mp-bb-server: /levels/jk1.json,
// /levels/d/<id>.json, /levels/z/<id>.zip, /levels/s/<id>-<n>.jpg). maps.js
// runs as it is, on a stand-in for Emscripten's FS and the browser's own
// IndexedDB; the game doesn't run. With --shots <folder>, it saves pictures
// of the screen there.

import { readFileSync, mkdirSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { createHash } from 'node:crypto';
import assert from 'node:assert/strict';
import { makeGob, makeZip, makeFS } from './helpers.mjs';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const shotsAt = process.argv.indexOf('--shots') > 0 ? process.argv[process.argv.indexOf('--shots') + 1] : null;
if (shotsAt)
    mkdirSync(shotsAt, { recursive: true });

const PORT = 8972, ORIGIN = 'http://127.0.0.1:' + PORT;
const mapsJs = readFileSync(new URL('../maps.js', import.meta.url), 'utf8');
const libraryJs = readFileSync(new URL('../library.js', import.meta.url), 'utf8');
const sha = b => createHash('sha256').update(b).digest('hex');
const text = s => new TextEncoder().encode(s);

// ---- Made-up levels

function episode(title, type, levels) {
    return makeGob([
        { name: 'episode.jk', bytes: text(`"${title}"\nTYPE ${type}\nSEQ ${levels.length}\n# a test level\n`) },
        ...levels.map(n => ({ name: 'jkl\\' + n, bytes: text('# ' + n + ' ' + title + '\n'.repeat(20)) })),
    ]);
}

function level(id, k, t, game, gobs, extra = {}) {
    // gobs: [{ n, bytes, ep, ty, lv, in }]
    const files = [];
    const inner = {};
    for (const g of gobs) {
        if (g.in)
            (inner[g.in] = inner[g.in] || []).push({ name: g.n, bytes: g.bytes });
        else
            files.push({ name: g.n, bytes: g.bytes });
    }
    for (const name in inner)
        files.push({ name, bytes: makeZip(inner[name]) });
    files.push({ name: 'readme.txt', bytes: text('Title: ' + t + '\nAuthor: Tester\nDate of Release: 1999-05-02\n') });
    const zip = makeZip(files);
    const entry = {
        id, k, t, a: 'Tester ' + id, rel: '1999-05-02', rp: 'd', src: 'r', add: '2000-06-02', sz: zip.length,
        r: 7.5, v: 4, dl: 1000 - id, sh: 2, z: t.toLowerCase().replace(/\W+/g, '') + '.zip', h: sha(zip),
        g: gobs.map(g => ({ n: g.n, h: sha(g.bytes), b: g.bytes.length, ep: g.ep, ty: g.ty, lv: g.lv, in: g.in })),
        ...extra,
    };
    const detail = {
        id, title: t, author: entry.a, desc: 'The description of ' + t + '.\nSecond line.', zip: entry.z, size: zip.length,
        sha256: entry.h, files: files.map(f => ({ n: f.name, b: f.bytes.length })),
        texts: [{ n: 'readme.txt', t: 'Title: ' + t + '\nAuthor: Tester\nDate of Release: 1999-05-02\n' }],
    };
    return { entry, detail, zip, game };
}

const alpha = episode('Alpha Arena', 2, ['alpha.jkl']);
const beta = episode('Beta Base', 8, ['beta1.jkl', 'beta2.jkl']);
const gamma = episode('Gamma Nested', 2, ['gamma.jkl']);
const delta = episode('Delta Story', 1, ['d1.jkl']);
const clash = episode('Clashing Name', 2, ['c.jkl']);
const taken = episode('Taken Name', 2, ['t.jkl']);
const other = episode('Somebody Else', 2, ['o.jkl']);
const kfy = episode('Fool Yard', 2, ['fool.jkl']);
const broken = episode('Broken', 2, ['b.jkl']);

const LEVELS = [
    level(501, 'mp', 'Alpha Arena', 'jk1', [{ n: 'alpha.gob', bytes: alpha, ep: 'Alpha Arena', ty: 2, lv: ['alpha.jkl'] }]),
    level(502, 'ctf', 'Beta Base CTF', 'jk1', [{ n: 'Beta/beta.gob', bytes: beta, ep: 'Beta Base', ty: 8, lv: ['beta1.jkl', 'beta2.jkl'] }],
          { rel: '2001-03', rp: 'm', src: 'f', add: '2001-04-10' }),
    level(503, 'mp', 'Gamma Nested', 'jk1', [{ n: 'gamma.gob', bytes: gamma, ep: 'Gamma Nested', ty: 2, lv: ['gamma.jkl'], in: 'inner.zip' }],
          { rel: '2003-11-20', sh: 0 }),
    level(504, 'sp', 'Delta Story', 'jk1', [{ n: 'delta.gob', bytes: delta, ep: 'Delta Story', ty: 1, lv: ['d1.jkl'] }]),
    level(505, 'mp', 'Epsilon Sounds', 'jk1', [{ n: 'sounds.gob', bytes: makeGob([{ name: 'sound/x.wav', bytes: text('x') }]) }],
          { rel: '1998-01-01' }),
    level(506, 'mp', 'Clashing Name', 'jk1', [{ n: 'JK1MP.gob', bytes: clash, ep: 'Clashing Name', ty: 2, lv: ['c.jkl'] }]),
    level(507, 'mp', 'Taken Name', 'jk1', [{ n: 'taken.gob', bytes: taken, ep: 'Taken Name', ty: 2, lv: ['t.jkl'] }]),
    level(508, 'mp', 'Broken Download', 'jk1', [{ n: 'broken.gob', bytes: broken, ep: 'Broken', ty: 2, lv: ['b.jkl'] }],
          { h: 'f'.repeat(64) }),
    level(601, 'kfy', 'Fool Yard', 'mots', [{ n: 'foolyard.goo', bytes: kfy, ep: 'Fool Yard', ty: 2, lv: ['fool.jkl'] }]),
];
const byId = Object.fromEntries(LEVELS.map(l => [l.entry.id, l]));

// A 4x3 PNG, for the screenshots.
const PNG = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAIAAAA7ljmRAAAAFklEQVR4nGP4z8DwHwyBNCMDhPQPABqkB/k8OVZ+AAAAAElFTkSuQmCC', 'base64');

const requests = [];
const server = createServer((req, res) => {
    const url = new URL(req.url, ORIGIN);
    const path = url.pathname;
    requests.push(req.method + ' ' + path + url.search);
    const send = (status, type, body) => {
        res.writeHead(status, { 'Content-Type': type, 'Content-Length': Buffer.byteLength(body) });
        res.end(req.method === 'HEAD' ? undefined : body);
    };
    let m;
    if (path === '/')
        return send(200, 'text/html', '<!doctype html><meta charset="utf-8"><title>library test</title>' +
                    '<style>body{margin:0;background:#000}#canvas{position:absolute;inset:0;width:100%;height:100%;background:#123}</style>' +
                    '<canvas id="canvas" tabindex="-1"></canvas>');
    if ((m = /^\/levels\/(jk1|mots)\.json$/.exec(path)))
        return send(200, 'application/json', JSON.stringify({
            v: 1, game: m[1], built: '2026-11-13T00:00:00Z', source: 'https://www.massassi.net', listed: 12, left: 3,
            levels: LEVELS.filter(l => l.game === m[1]).map(l => l.entry),
        }));
    if ((m = /^\/levels\/d\/(\d+)\.json$/.exec(path)) && byId[m[1]])
        return send(200, 'application/json', JSON.stringify(byId[m[1]].detail));
    if ((m = /^\/levels\/z\/(\d+)\.zip$/.exec(path)) && byId[m[1]])
        return send(200, 'application/zip', Buffer.from(byId[m[1]].zip));
    if ((m = /^\/levels\/s\/(\d+)-(\d+)\.jpg$/.exec(path)) && byId[m[1]])
        return send(200, 'image/png', PNG);
    send(404, 'text/plain', 'not found');
});
await new Promise(resolve => server.listen(PORT, '127.0.0.1', resolve));

const browser = await chromium.launch();

// A page with maps.js and library.js on a made-up game folder, and a
// stand-in for SDL's key listener on the window.
async function game({ mots = false, files = {} } = {}) {
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
    const errors = [];
    page.on('pageerror', e => errors.push(String(e)));
    page.on('console', m => { if (m.type() === 'error') errors.push(m.text()); });
    const cwd = mots ? '/mots' : '/jk1';
    const fileList = Object.entries(files).map(([p, b]) => [p, Array.from(b)]);
    await page.addInitScript(([makeFSSource, fileList, cwd, mots]) => {
        const makeFS = new Function('return (' + makeFSSource + ')')();
        const files = Object.fromEntries(fileList.map(([p, b]) => [p, new Uint8Array(b)]));
        window.Module = { arguments: mots ? ['-motsCompat'] : [], preRun: [], FS: makeFS(files, cwd),
                          addRunDependency() {}, removeRunDependency() {} };
        window.gameKeys = [];
        window.addEventListener('keydown', e => window.gameKeys.push(e.key));
    }, [makeFS.toString(), fileList, cwd, mots]);
    await page.goto(ORIGIN + '/', { waitUntil: 'load' });
    await page.evaluate(([maps, library]) => {
        Module.canvas = document.getElementById('canvas');
        new Function('Module', maps)(Module);
        new Function('Module', library)(Module);
        for (const f of Module.preRun) f();
    }, [mapsJs, libraryJs]);
    await page.waitForFunction(() => Module.jkLibrary.available());
    return { page, errors };
}

const openLib = (page, which) => page.evaluate(g => Module.jkLibrary.open(g), which);
const state = page => page.evaluate(() => Module.jkLibrary.state());
const take = page => page.evaluate(() => Module.jkLibrary.take());
const rows = page => page.$$eval('#jk-lib [data-level]', bs => bs.map(b => Number(b.dataset.level)));
const fileSha = (page, path) => page.evaluate(async p => {
    const b = Module.FS.readFile(p);
    const h = await crypto.subtle.digest('SHA-256', b);
    return Array.from(new Uint8Array(h), x => x.toString(16).padStart(2, '0')).join('');
}, path);
async function waitClosed(page) {
    await page.waitForFunction(() => Module.jkLibrary.state() === 1, null, { timeout: 15000 });
}
async function useLevel(page, id) {
    await page.click(`#jk-lib [data-level="${id}"]`);
    await page.click('#jk-lib [data-id="use"]');
}

let failed = 0;
async function test(name, fn) {
    try {
        await fn();
        console.log('ok   ' + name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + name + '\n' + (e && e.stack || e));
    }
}

const theTaken = new Uint8Array(other);
const jk = await game({ files: { '/jk1/episode/JK1MP.gob': text('the original'), '/jk1/episode/taken.gob': theTaken } });
const page = jk.page;

await test('the list: multiplayer and CTF levels, by name, with the counts', async () => {
    assert.equal(await state(page), 1);
    assert.equal(await openLib(page, 'jk1'), true);
    assert.equal(await state(page), 0);
    await page.waitForSelector('#jk-lib [data-level]');
    assert.deepEqual(await rows(page), [501, 502, 506, 505, 503, 507, 508].sort((a, b) =>
        byId[a].entry.t.localeCompare(byId[b].entry.t)));
    assert.match(await page.textContent('#jk-lib [data-id="sub"]'), /^8 levels from Massassi\.net, with 3 more of its 12 still being copied$/);
    assert.equal(await page.textContent('#jk-lib [data-id="sortName"]'), 'Name');
    // The first level is picked, with its Overview.
    await page.waitForSelector('#jk-lib .desc');
    assert.match(await page.textContent('#jk-lib .p-title'), /Alpha Arena/);
    const facts = await page.textContent('#jk-lib .facts');
    assert.match(facts, /Released:May 2, 1999 \(from its readme; Massassi says added Jun 2, 2000, the day it imported most older levels\)/);
    assert.match(facts, /Episode:Alpha Arena \(1 level\)/);
    assert.equal(await page.getAttribute('#jk-lib .shot img', 'src'), '/levels/s/501-1.jpg');
    if (shotsAt)
        await page.screenshot({ path: shotsAt + '/library-jk.png' });
});

await test('sorts: newest and oldest by the real release date, then most downloaded', async () => {
    await page.click('#jk-lib [data-id="sortNext"]');
    assert.equal(await page.textContent('#jk-lib [data-id="sortName"]'), 'Newest first');
    const newest = await rows(page);
    assert.equal(newest[0], 503);  // 2003-11-20
    assert.equal(newest[1], 502);  // 2001-03
    assert.equal(newest[newest.length - 1], 505);  // 1998
    await page.click('#jk-lib [data-id="sortNext"]');
    assert.equal(await page.textContent('#jk-lib [data-id="sortName"]'), 'Oldest first');
    assert.deepEqual(await rows(page), newest.slice().reverse().sort((a, b) => {
        const d = l => byId[l].entry.rel;
        return d(a).localeCompare(d(b)) || byId[a].entry.t.localeCompare(byId[b].entry.t);
    }));
    await page.click('#jk-lib [data-id="sortNext"]');
    assert.equal(await page.textContent('#jk-lib [data-id="sortName"]'), 'Most downloaded');
    assert.deepEqual(await rows(page), [501, 502, 503, 505, 506, 507, 508]);
    await page.click('#jk-lib [data-id="sortPrev"]');
    await page.click('#jk-lib [data-id="sortPrev"]');
    await page.click('#jk-lib [data-id="sortPrev"]');
    assert.equal(await page.textContent('#jk-lib [data-id="sortName"]'), 'Name');
});

await test('filters and search; keys typed there never reach the game', async () => {
    await page.click('#jk-lib [data-filter="ctf"]');
    assert.ok(!(await rows(page)).includes(502));
    await page.click('#jk-lib [data-filter="sp"]');
    assert.ok((await rows(page)).includes(504));
    await page.click('#jk-lib [data-filter="ctf"]');
    await page.click('#jk-lib [data-filter="sp"]');
    await page.evaluate(() => { window.gameKeys = []; });
    await page.click('#jk-lib [data-id="q"]');
    await page.keyboard.type('beta');
    assert.deepEqual(await rows(page), [502]);
    await page.keyboard.press('Backspace');
    await page.keyboard.press('Backspace');
    await page.keyboard.press('Backspace');
    await page.keyboard.press('Backspace');
    assert.equal((await rows(page)).length, 7);
    assert.deepEqual(await page.evaluate(() => window.gameKeys), []);
    // The arrow keys move through the list from the search box.
    const list = await rows(page), sel = () => page.getAttribute('#jk-lib [aria-selected="true"][data-level]', 'data-level');
    const at = list.indexOf(Number(await sel()));
    await page.keyboard.press('ArrowDown');
    assert.equal(await sel(), String(list[at + 1]));
    await page.keyboard.press('ArrowUp');
    assert.equal(await sel(), String(list[at]));
    assert.deepEqual(await page.evaluate(() => window.gameKeys), []);
});

await test('Readme and Files', async () => {
    await page.click('#jk-lib [data-level="502"]');
    await page.click('#jk-lib [data-tab="readme"]');
    await page.waitForSelector('#jk-lib .readme');
    assert.match(await page.textContent('#jk-lib .readme'), /Title: Beta Base CTF/);
    await page.click('#jk-lib [data-tab="files"]');
    const files = await page.textContent('#jk-lib .tree');
    assert.match(files, /Beta\/beta\.gob episode: Beta Base/);
    assert.match(files, /readme\.txt/);
    if (shotsAt)
        await page.screenshot({ path: shotsAt + '/library-files.png' });
    await page.click('#jk-lib [data-tab="lib"]');
    const facts = await page.textContent('#jk-lib .facts');
    assert.match(facts, /Released:March 2001 \(the date of the level files in its zip; Massassi says added Apr 10, 2001\)/);
    assert.match(facts, /Type:Capture the Flag/);
});

await test('Use Level: gets the zip, checks it, saves the GOB and writes it into episode/', async () => {
    requests.length = 0;
    await useLevel(page, 501);
    await waitClosed(page);
    assert.equal(await take(page), 'alpha');
    assert.equal(await take(page), '');
    assert.equal(await fileSha(page, '/jk1/episode/alpha.gob'), sha(alpha));
    assert.deepEqual(requests.filter(r => r.includes('/levels/z/')), ['GET /levels/z/501.zip?h=' + byId[501].entry.h]);
    const saved = await page.evaluate(async () => (await Module.jkMaps.core.store.list('jk1')).map(m => [m.name, m.ext, m.sha256, m.lib]));
    assert.deepEqual(saved, [['alpha', 'gob', sha(alpha), 501]]);
    assert.equal(await page.evaluate(() => document.activeElement === Module.canvas), true);
});

await test('a saved level has a mark, and comes from the browser the next time', async () => {
    await openLib(page, 'jk1');
    await page.waitForSelector('#jk-lib [data-level="501"] .mark');
    await page.evaluate(() => Module.FS.unlink('/jk1/episode/alpha.gob'));
    requests.length = 0;
    await useLevel(page, 501);
    await waitClosed(page);
    assert.equal(await take(page), 'alpha');
    assert.equal(await fileSha(page, '/jk1/episode/alpha.gob'), sha(alpha));
    assert.deepEqual(requests.filter(r => r.includes('/levels/z/')), []);
});

await test('a GOB in a zip in the zip; a subfolder in the zip', async () => {
    await openLib(page, 'jk1');
    await page.waitForSelector('#jk-lib [data-level="503"]');
    await useLevel(page, 503);
    await waitClosed(page);
    assert.equal(await take(page), 'gamma');
    assert.equal(await fileSha(page, '/jk1/episode/gamma.gob'), sha(gamma));
    await openLib(page, 'jk1');
    await page.waitForSelector('#jk-lib [data-level="502"]');
    await useLevel(page, 502);
    await waitClosed(page);
    assert.equal(await take(page), 'beta');
    assert.equal(await fileSha(page, '/jk1/episode/beta.gob'), sha(beta));
});

await test('names: an original episode\'s, or one the player has, get the level\'s number', async () => {
    await openLib(page, 'jk1');
    await page.waitForSelector('#jk-lib [data-level="506"]');
    await useLevel(page, 506);
    await waitClosed(page);
    assert.equal(await take(page), 'JK1MP_506');
    assert.equal(await fileSha(page, '/jk1/episode/JK1MP_506.gob'), sha(clash));
    assert.equal(new TextDecoder().decode(new Uint8Array(await page.evaluate(() => Array.from(Module.FS.readFile('/jk1/episode/JK1MP.gob'))))), 'the original');
    await openLib(page, 'jk1');
    await page.waitForSelector('#jk-lib [data-level="507"]');
    await useLevel(page, 507);
    await waitClosed(page);
    assert.equal(await take(page), 'taken_507');
    assert.equal(await fileSha(page, '/jk1/episode/taken.gob'), sha(theTaken));
    assert.equal(await fileSha(page, '/jk1/episode/taken_507.gob'), sha(taken));
});

await test('a level with no episode GOB can\'t be used; a bad download is refused and said so', async () => {
    await openLib(page, 'jk1');
    await page.waitForSelector('#jk-lib [data-level="505"]');
    await page.click('#jk-lib [data-level="505"]');
    assert.equal(await page.isDisabled('#jk-lib [data-id="use"]'), true);
    assert.match(await page.textContent('#jk-lib .panel'), /no episode GOB for Jedi Knight/);
    await useLevel(page, 508);
    await page.waitForSelector('#jk-lib[data-error]');
    assert.match(await page.textContent('#jk-lib [data-id="ptext"]'), /didn't come through whole/);
    assert.equal(await state(page), 0);
    assert.equal(await page.evaluate(() => !!Module.FS.files.get('/jk1/episode/broken.gob')), false);
    if (shotsAt)
        await page.screenshot({ path: shotsAt + '/library-error.png' });
    // Escape closes it with nothing picked, and isn't the game's.
    await page.evaluate(() => { window.gameKeys = []; });
    await page.keyboard.press('Escape');
    await waitClosed(page);
    assert.equal(await take(page), '');
    assert.deepEqual(await page.evaluate(() => window.gameKeys), []);
});

await test('Cancel while a level comes in stops it and keeps the library open', async () => {
    await openLib(page, 'jk1');
    await page.waitForSelector('#jk-lib [data-level]');
    await page.route('**/levels/z/504.zip*', async route => {
        await new Promise(r => setTimeout(r, 1500));
        route.continue().catch(() => {});
    });
    await page.click('#jk-lib [data-filter="sp"]');
    await page.waitForSelector('#jk-lib [data-level="504"]');
    await useLevel(page, 504);
    await page.waitForSelector('#jk-lib[data-busy]');
    // Keys while it comes in are the library's too.
    await page.evaluate(() => { window.gameKeys = []; });
    await page.keyboard.press('ArrowDown');
    await page.keyboard.press('a');
    assert.deepEqual(await page.evaluate(() => window.gameKeys), []);
    await page.click('#jk-lib [data-id="cancel"]');
    await page.waitForFunction(() => !('busy' in document.getElementById('jk-lib').dataset));
    assert.equal(await state(page), 0);
    await page.waitForTimeout(1800);
    assert.equal(await page.evaluate(() => !!Module.FS.files.get('/jk1/episode/delta.gob')), false);
    await page.click('#jk-lib [data-id="cancel"]');
    await waitClosed(page);
    assert.equal(await take(page), '');
});

await test('no errors on the page', async () => {
    assert.deepEqual(jk.errors.filter(e => !/Failed to load resource/.test(e)), []);
});
await page.close();

await test('Mysteries of the Sith: its colors and Kill the Fool, and GOO files', async () => {
    const m = await game({ mots: true });
    await openLib(m.page, 'mots');
    await m.page.waitForSelector('#jk-lib [data-level="601"]');
    assert.equal(await m.page.getAttribute('#jk-lib', 'data-game'), 'mots');
    assert.equal(await m.page.textContent('#jk-lib [data-filter="ctf"] span'), 'Kill the Fool');
    assert.match(await m.page.textContent('#jk-lib [data-level="601"] .s'), /^KFY/);
    if (shotsAt)
        await m.page.screenshot({ path: shotsAt + '/library-mots.png' });
    await useLevel(m.page, 601);
    await waitClosed(m.page);
    assert.equal(await take(m.page), 'foolyard');
    assert.equal(await fileSha(m.page, '/mots/episode/foolyard.goo'), sha(kfy));
    assert.deepEqual(m.errors.filter(e => !/Failed to load resource/.test(e)), []);
    await m.page.close();
});

await test('not where the site has no library', async () => {
    const page2 = await browser.newPage();
    await page2.route('**/levels/**', route => route.fulfill({ status: 404, body: 'not found' }));
    await page2.addInitScript(() => { window.Module = { arguments: [], preRun: [] }; });
    await page2.goto(ORIGIN + '/');
    await page2.evaluate(([maps, library]) => {
        Module.FS = { cwd: () => '/jk1' };
        new Function('Module', maps)(Module);
        new Function('Module', library)(Module);
        for (const f of Module.preRun.slice(1)) f();
    }, [mapsJs, libraryJs]);
    await page2.waitForTimeout(500);
    assert.equal(await page2.evaluate(() => Module.jkLibrary.available()), false);
    await page2.close();
});

await browser.close();
server.close();
console.log(failed ? failed + ' failed' : 'all ok');
process.exit(failed ? 1 : 0);
