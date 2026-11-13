// Tests for packaging/wasm/library.js, the page's half of the Level Library
// (src/Gui/jkGUILibrary.c is the game menu that draws it), in a real browser.
// Needs Playwright with Chromium, as move-test.mjs does:
// node packaging/wasm/test/library-test.mjs
//
// The test calls Module.jkLibrary as the menu does: begin(), then text() and
// number() for what to draw, query(), select(), use(), cancel() and shot()
// for what the player does, then take() and end(). The levels are made up by
// the test and served the way the site's Worker serves them (src/levels.js
// in jkdf2mp-bb-server: /levels/jk1.json, /levels/d/<id>.json,
// /levels/z/<id>.zip, /levels/s/<id>-<n>.jpg). maps.js runs as it is, on a
// stand-in for Emscripten's FS and the browser's own IndexedDB; the game
// doesn't run.

import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import { createHash } from 'node:crypto';
import assert from 'node:assert/strict';
import { makeGob, makeZip, makeFS } from './helpers.mjs';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

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
const byTitle = Object.fromEntries(LEVELS.map(l => [l.entry.t, l.entry.id]));
// Letters the game's fonts don't have, in a description and a title.
byId[507].detail.desc = 'It\u2019s \u201ctaken\u201d\tfor sure\u2026 Caf\u00e9 \u2013 \u00a0done';
byId[507].detail.files[0].n = 'taken.gob';

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

// A page with maps.js and library.js on a made-up game folder.
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

// What the menu does, through Module.jkLibrary.
const call = (page, fn, ...args) => page.evaluate(([fn, args]) => Module.jkLibrary[fn](...args), [fn, args]);
const libText = (page, what, arg = 0) => call(page, 'text', what, arg);
const number = (page, what) => call(page, 'number', what);
const T = { STATUS: 0, ROW: 1, SORT: 2, TITLE: 3, OVERVIEW: 4, README: 5, FILES: 6, MESSAGE: 7 };
const N = { CHANGED: 0, COUNT: 1, SELECTED: 2, CAN_USE: 3, BUSY: 4, DONE: 5, SHOTS: 6 };
const F = { MP: 1, CTF: 2, SP: 4 };

const rowTexts = page => page.evaluate(() => {
    const out = [];
    for (let i = 0; i < Module.jkLibrary.number(1); i++)
        out.push(Module.jkLibrary.text(1, i));
    return out;
});
const idOf = row => byTitle[row.replace(/^\* /, '').split('\t')[0]];
const rows = async page => (await rowTexts(page)).map(idOf);
async function open(page, g = 'jk1', query = ['', F.MP | F.CTF, 0]) {
    assert.equal(await call(page, 'begin', g), true);
    await call(page, 'query', ...query);
    await page.waitForFunction(() => Module.jkLibrary.number(1) > 0, null, { timeout: 10000 });
}
async function pick(page, id) {
    const at = (await rows(page)).indexOf(id);
    assert.ok(at >= 0, id + ' is in the list');
    await call(page, 'select', at);
    assert.equal(await number(page, N.SELECTED), at);
}
const details = page => page.waitForFunction(() => !/^Getting /m.test(Module.jkLibrary.text(4)) &&
                                                  !/^Getting /.test(Module.jkLibrary.text(5)));
// Use Level, as the menu waits for it: done, then take() and end().
async function useLevel(page, id) {
    await pick(page, id);
    assert.equal(await number(page, N.CAN_USE), 1);
    assert.equal(await call(page, 'use'), true);
    await page.waitForFunction(() => Module.jkLibrary.number(5) === 1, null, { timeout: 15000 });
    const name = await call(page, 'take');
    await call(page, 'end');
    return name;
}
const fileSha = (page, path) => page.evaluate(async p => {
    const b = Module.FS.readFile(p);
    const h = await crypto.subtle.digest('SHA-256', b);
    return Array.from(new Uint8Array(h), x => x.toString(16).padStart(2, '0')).join('');
}, path);
const shotBox = page => page.evaluate(() => {
    const img = document.getElementById('jk-lib-shot');
    if (!img || img.style.display === 'none')
        return null;
    const r = img.getBoundingClientRect();
    return { src: new URL(img.src).pathname, x: Math.round(r.x), y: Math.round(r.y), w: Math.round(r.width), h: Math.round(r.height) };
});

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
    assert.equal(await call(page, 'begin', 'jk1'), true);
    // Everything is new to a menu that just opened.
    assert.equal(await number(page, N.CHANGED), 31);
    await call(page, 'query', '', F.MP | F.CTF, 0);
    await page.waitForFunction(() => Module.jkLibrary.number(1) > 0);
    assert.deepEqual(await rows(page), [501, 502, 506, 505, 503, 507, 508].sort((a, b) =>
        byId[a].entry.t.localeCompare(byId[b].entry.t)));
    assert.equal(await libText(page, T.STATUS), '8 levels from Massassi.net so far, 3 more coming');
    assert.equal(await libText(page, T.SORT), 'Name');
    // A row: the title, a tab, the year and the size, and a tab and the kind
    // for a level that isn't plain multiplayer.
    const r = await rowTexts(page);
    assert.match(r[0], /^Alpha Arena\t1999 \d+ KB$/);
    assert.match(r[1], /^Beta Base CTF\t2001 \d+ KB\tCTF$/);
    // The first level is picked, with its Overview: a label, a tab, its value.
    assert.equal(await number(page, N.SELECTED), 0);
    assert.equal(await libText(page, T.TITLE), 'Alpha Arena');
    await details(page);
    const facts = (await libText(page, T.OVERVIEW)).split('\n');
    assert.equal(facts[0], 'Author:\tTester 501');
    assert.equal(facts[1], 'Released:\tMay 2, 1999 (from its readme; Massassi says added Jun 2, 2000, the day it imported most older levels)');
    assert.ok(facts.includes('Episode:\tAlpha Arena (1 level)'));
    assert.ok(facts.includes('Type:\tMultiplayer'));
    assert.ok(facts.includes('Rating:\t7.5 of 10 (4 votes), 499 downloads on Massassi'));
    assert.ok(facts.some(f => /^Size:\t\d+ KB zip$/.test(f)));
    assert.deepEqual(facts.slice(-2), ['The description of Alpha Arena.', 'Second line.']);
    assert.equal(await number(page, N.SHOTS), 2);
    assert.equal(await number(page, N.CAN_USE), 1);
    assert.equal(await number(page, N.BUSY), 0);
    // Once it has all come in and been asked for, nothing is new.
    assert.notEqual(await number(page, N.CHANGED), 0);
    assert.equal(await number(page, N.CHANGED), 0);
    assert.equal(await libText(page, T.MESSAGE), 'Levels you use are saved in this browser, not in your game folder.');
});

await test('the screenshot goes over the menu\'s frame for it, and only while the menu asks', async () => {
    // The canvas is 1280x800: the menu is 800 high and 1066.67 wide in the
    // middle, so a menu pixel is 1.667 of the page's, from x 106.67.
    await call(page, 'shot', 1, 311, 105, 318, 178);
    await page.waitForFunction(() => document.getElementById('jk-lib-shot').style.display === 'block');
    const box = await shotBox(page);
    assert.equal(box.src, '/levels/s/501-1.jpg');
    assert.deepEqual([box.x, box.y, box.w, box.h], [625, 175, 530, 297]);
    await call(page, 'nextShot');
    await page.waitForFunction(() => /501-2\.jpg$/.test(document.getElementById('jk-lib-shot').src));
    await call(page, 'nextShot');
    await page.waitForFunction(() => /501-1\.jpg$/.test(document.getElementById('jk-lib-shot').src));
    // The canvas grows: the picture follows.
    await page.setViewportSize({ width: 1600, height: 900 });
    await call(page, 'shot', 1, 311, 105, 318, 178);
    await page.waitForFunction(() => Math.round(document.getElementById('jk-lib-shot').getBoundingClientRect().height) === 334);
    await page.setViewportSize({ width: 1280, height: 800 });
    // A menu that stops asking (it closed, or the game stopped) loses it.
    await page.waitForFunction(() => document.getElementById('jk-lib-shot').style.display === 'none', null, { timeout: 2000 });
    await call(page, 'shot', 1, 311, 105, 318, 178);
    await page.waitForFunction(() => document.getElementById('jk-lib-shot').style.display === 'block');
    await call(page, 'shot', 0, 0, 0, 0, 0);
    await page.waitForFunction(() => document.getElementById('jk-lib-shot').style.display === 'none');
    // The picture never takes the mouse from the game.
    assert.equal(await page.evaluate(() => getComputedStyle(document.getElementById('jk-lib-shot')).pointerEvents), 'none');
});

await test('sorts: newest and oldest by the real release date, then most downloaded', async () => {
    await call(page, 'query', '', F.MP | F.CTF, 1);
    assert.equal(await libText(page, T.SORT), 'Newest first');
    const newest = await rows(page);
    assert.equal(newest[0], 503);  // 2003-11-20
    assert.equal(newest[1], 502);  // 2001-03
    assert.equal(newest[newest.length - 1], 505);  // 1998
    await call(page, 'query', '', F.MP | F.CTF, 2);
    assert.equal(await libText(page, T.SORT), 'Oldest first');
    assert.deepEqual(await rows(page), newest.slice().reverse().sort((a, b) => {
        const d = l => byId[l].entry.rel;
        return d(a).localeCompare(d(b)) || byId[a].entry.t.localeCompare(byId[b].entry.t);
    }));
    await call(page, 'query', '', F.MP | F.CTF, 3);
    assert.equal(await libText(page, T.SORT), 'Most downloaded');
    assert.deepEqual(await rows(page), [501, 502, 503, 505, 506, 507, 508]);
    await call(page, 'query', '', F.MP | F.CTF, 4);
    assert.equal(await libText(page, T.SORT), 'Top rated');
    await call(page, 'query', '', F.MP | F.CTF, 5);
    assert.equal(await libText(page, T.SORT), 'Smallest first');
    const sizes = (await rows(page)).map(id => byId[id].entry.sz);
    assert.deepEqual(sizes, sizes.slice().sort((a, b) => a - b));
    await call(page, 'query', '', F.MP | F.CTF, 0);
    assert.equal(await libText(page, T.SORT), 'Name');
    // The pick stays where it was.
    assert.equal(await libText(page, T.TITLE), 'Alpha Arena');
});

await test('filters and search', async () => {
    await call(page, 'number', N.CHANGED);
    await call(page, 'query', '', F.MP, 0);
    assert.ok(!(await rows(page)).includes(502));
    assert.equal((await number(page, N.CHANGED)) & 2, 2);
    await call(page, 'query', '', F.MP | F.CTF | F.SP, 0);
    assert.ok((await rows(page)).includes(504));
    await call(page, 'query', 'beta', F.MP | F.CTF, 0);
    assert.deepEqual(await rows(page), [502]);
    // A pick that's no longer listed gives way to the first that is.
    assert.equal(await libText(page, T.TITLE), 'Beta Base CTF');
    await call(page, 'query', 'TESTER 503', F.MP | F.CTF, 0);
    assert.deepEqual(await rows(page), [503]);
    await call(page, 'query', 'nothing like this', F.MP | F.CTF, 0);
    assert.deepEqual(await rows(page), []);
    assert.equal(await number(page, N.SELECTED), -1);
    await call(page, 'query', '', F.MP | F.CTF, 0);
    assert.equal((await rows(page)).length, 7);
    // Nothing asked again: nothing changed.
    await call(page, 'number', N.CHANGED);
    await call(page, 'query', '', F.MP | F.CTF, 0);
    assert.equal(await number(page, N.CHANGED), 0);
});

await test('Readme and Files', async () => {
    await pick(page, 502);
    await details(page);
    assert.match(await libText(page, T.README), /^--- readme\.txt ---\n\nTitle: Beta Base CTF\nAuthor: Tester/);
    const files = (await libText(page, T.FILES)).split('\n');
    assert.ok(files.some(f => /^\d+ KB\tBeta\/beta\.gob  \(episode: Beta Base\)$/.test(f)), files.join('|'));
    assert.ok(files.some(f => /^\d+ KB\treadme\.txt$/.test(f)));
    const facts = (await libText(page, T.OVERVIEW)).split('\n');
    assert.ok(facts.includes('Released:\tMarch 2001 (the date of the level files in its zip; Massassi says added Apr 10, 2001)'));
    assert.ok(facts.includes('Type:\tCapture the Flag'));
    assert.equal(await number(page, N.SHOTS), 2);
});

await test('text the game\'s fonts can draw: no curly quotes, accents or tabs in a description', async () => {
    await pick(page, 507);
    await details(page);
    const desc = (await libText(page, T.OVERVIEW)).split('\n').pop();
    assert.equal(desc, 'It\'s "taken"    for sure... Cafe -  done');
    assert.ok(!/[^\x00-\x7f]/.test(await libText(page, T.OVERVIEW)));
});

await test('Use Level: gets the zip, checks it, saves the GOB and writes it into episode/', async () => {
    requests.length = 0;
    await pick(page, 501);
    assert.equal(await call(page, 'use'), true);
    // While it comes in: no other level, no second Use, and it says how far it is.
    assert.equal(await number(page, N.BUSY), 1);
    assert.equal(await number(page, N.CAN_USE), 0);
    assert.equal(await call(page, 'use'), false);
    await call(page, 'select', 1);
    assert.equal(await libText(page, T.TITLE), 'Alpha Arena');
    assert.match(await libText(page, T.MESSAGE), /^Getting Alpha Arena|^Opening |^Checking |^Saved in this browser/);
    await page.waitForFunction(() => Module.jkLibrary.number(5) === 1, null, { timeout: 15000 });
    assert.equal(await call(page, 'take'), 'alpha');
    assert.equal(await call(page, 'take'), '');
    await call(page, 'end');
    assert.equal(await fileSha(page, '/jk1/episode/alpha.gob'), sha(alpha));
    assert.deepEqual(requests.filter(r => r.includes('/levels/z/')), ['GET /levels/z/501.zip?h=' + byId[501].entry.h]);
    const saved = await page.evaluate(async () => (await Module.jkMaps.core.store.list('jk1')).map(m => [m.name, m.ext, m.sha256, m.lib]));
    assert.deepEqual(saved, [['alpha', 'gob', sha(alpha), 501]]);
});

await test('a saved level has a mark, and comes from the browser the next time', async () => {
    await open(page);
    const r = await rowTexts(page);
    assert.ok(r.some(x => x.startsWith('* Alpha Arena\t')), r.join('|'));
    await pick(page, 501);
    assert.match(await libText(page, T.OVERVIEW), /Saved in this browser as alpha\./);
    await page.evaluate(() => Module.FS.unlink('/jk1/episode/alpha.gob'));
    requests.length = 0;
    assert.equal(await useLevel(page, 501), 'alpha');
    assert.equal(await fileSha(page, '/jk1/episode/alpha.gob'), sha(alpha));
    assert.deepEqual(requests.filter(r => r.includes('/levels/z/')), []);
});

await test('a GOB in a zip in the zip; a subfolder in the zip', async () => {
    await open(page);
    assert.equal(await useLevel(page, 503), 'gamma');
    assert.equal(await fileSha(page, '/jk1/episode/gamma.gob'), sha(gamma));
    await open(page);
    assert.equal(await useLevel(page, 502), 'beta');
    assert.equal(await fileSha(page, '/jk1/episode/beta.gob'), sha(beta));
});

await test('names: an original episode\'s, or one the player has, get the level\'s number', async () => {
    await open(page);
    assert.equal(await useLevel(page, 506), 'JK1MP_506');
    assert.equal(await fileSha(page, '/jk1/episode/JK1MP_506.gob'), sha(clash));
    assert.equal(new TextDecoder().decode(new Uint8Array(await page.evaluate(() => Array.from(Module.FS.readFile('/jk1/episode/JK1MP.gob'))))), 'the original');
    await open(page);
    assert.equal(await useLevel(page, 507), 'taken_507');
    assert.equal(await fileSha(page, '/jk1/episode/taken.gob'), sha(theTaken));
    assert.equal(await fileSha(page, '/jk1/episode/taken_507.gob'), sha(taken));
});

await test('a level with no episode GOB can\'t be used; a bad download is refused and said so', async () => {
    await open(page);
    await pick(page, 505);
    assert.equal(await number(page, N.CAN_USE), 0);
    assert.equal(await call(page, 'use'), false);
    assert.match(await libText(page, T.OVERVIEW), /no episode GOB for Jedi Knight/);
    await pick(page, 508);
    assert.equal(await call(page, 'use'), true);
    await page.waitForFunction(() => Module.jkLibrary.number(4) === 0 && /come through/.test(Module.jkLibrary.text(7)), null, { timeout: 15000 });
    assert.equal(await libText(page, T.MESSAGE), "The download didn't come through whole. Try again.");
    assert.equal(await number(page, N.DONE), 0);
    assert.equal(await number(page, N.CAN_USE), 1);
    assert.equal(await page.evaluate(() => !!Module.FS.files.get('/jk1/episode/broken.gob')), false);
    // Picking another level clears it.
    await pick(page, 501);
    assert.match(await libText(page, T.MESSAGE), /^Levels you use are saved in this browser/);
    // Cancel with nothing coming in closes the menu: nothing taken.
    assert.equal(await call(page, 'cancel'), false);
    assert.equal(await call(page, 'take'), '');
    await call(page, 'end');
});

await test('Cancel while a level comes in stops it and keeps the library open', async () => {
    await page.route('**/levels/z/504.zip*', async route => {
        await new Promise(r => setTimeout(r, 1500));
        route.continue().catch(() => {});
    });
    await open(page, 'jk1', ['', F.MP | F.CTF | F.SP, 0]);
    await pick(page, 504);
    assert.equal(await call(page, 'use'), true);
    assert.equal(await number(page, N.BUSY), 1);
    assert.equal(await call(page, 'cancel'), true);
    await page.waitForFunction(() => Module.jkLibrary.number(4) === 0);
    assert.match(await libText(page, T.MESSAGE), /^Levels you use are saved in this browser/);
    assert.equal(await number(page, N.CAN_USE), 1);
    await page.waitForTimeout(1800);
    assert.equal(await page.evaluate(() => !!Module.FS.files.get('/jk1/episode/delta.gob')), false);
    assert.equal(await number(page, N.DONE), 0);
    // Closing while one comes in stops it too.
    assert.equal(await call(page, 'use'), true);
    await call(page, 'end');
    await page.waitForTimeout(1800);
    assert.equal(await page.evaluate(() => !!Module.FS.files.get('/jk1/episode/delta.gob')), false);
    assert.equal(await call(page, 'take'), '');
});

await test('no errors on the page', async () => {
    assert.deepEqual(jk.errors.filter(e => !/Failed to load resource/.test(e)), []);
});
await page.close();

await test('Mysteries of the Sith: Kill the Fool, and GOO files', async () => {
    const m = await game({ mots: true });
    await open(m.page, 'mots');
    const r = await rowTexts(m.page);
    assert.deepEqual(r.map(idOf), [601]);
    assert.match(r[0], /^Fool Yard\t1999 [^\t]+\tKFY$/);
    assert.ok((await libText(m.page, T.OVERVIEW)).split('\n').includes('Type:\tKill the Fool'));
    assert.equal(await useLevel(m.page, 601), 'foolyard');
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

await test('a list that can\'t be got says so', async () => {
    const m = await game();
    await m.page.route('**/levels/jk1.json', route => route.fulfill({ status: 500, body: 'no' }));
    await m.page.evaluate(() => { Module.jkLibrary._L.index = {}; });
    assert.equal(await call(m.page, 'begin', 'jk1'), true);
    await m.page.waitForFunction(() => /can't be reached/.test(Module.jkLibrary.text(0)));
    assert.equal(await number(m.page, N.COUNT), 0);
    assert.equal(await number(m.page, N.CAN_USE), 0);
    await call(m.page, 'end');
    await m.page.close();
});

await browser.close();
server.close();
console.log(failed ? failed + ' failed' : 'all ok');
process.exit(failed ? 1 : 0);
