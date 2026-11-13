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
// /levels/z/<id>.zip, /levels/s/<id>-<n>.jpg, and the 3D view's
// /levels/p/<id>-<m>.map.json, .mat.json and /levels/p/<id>.tex.json).
// maps.js and preview3d.js run as they are, on a stand-in for Emscripten's
// FS and the browser's own IndexedDB; the game doesn't run.

import { readFileSync } from 'node:fs';
import { deflateSync, crc32 } from 'node:zlib';
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
const previewJs = readFileSync(new URL('../preview3d.js', import.meta.url), 'utf8');
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

// ---- Made-up textures, colormaps and pictures, laid out as the game's

// A MAT: textures (type 2) of w x h, 8-bit through a colormap or 16-bit
// (RGB 565), or a color (type 0).
function makeMat({ type = 2, bits = 8, w = 1, h = 1, pixels = [], transparent = 0, hasT = 0, color = 0 }) {
    const recs = 1, recSize = type === 0 ? 24 : 40;
    const px = type === 0 ? 0 : 24 + w * h * (bits / 8);
    const b = Buffer.alloc(76 + recs * recSize + px);
    b.write('MAT ', 0, 'latin1');
    b.writeInt32LE(50, 4);
    b.writeInt32LE(type, 8);
    b.writeInt32LE(recs, 12);
    b.writeInt32LE(type === 0 ? 0 : recs, 16);
    b.writeInt32LE(bits, 24);
    [5, 6, 5, 11, 5, 0, 3, 2, 3].forEach((v, i) => b.writeInt32LE(bits === 16 ? v : 0, 28 + i * 4));
    b.writeInt32LE(type === 0 ? 0 : 8, 76);
    b.writeInt32LE(type === 0 ? color : transparent, 80);
    if (type === 2) {
        const at = 76 + recs * recSize;
        b.writeInt32LE(w, at);
        b.writeInt32LE(h, at + 4);
        b.writeInt32LE(hasT, at + 8);
        b.writeInt32LE(1, at + 20);
        pixels.forEach((v, i) => bits === 8 ? b.writeUInt8(v, at + 24 + i) : b.writeUInt16LE(v, at + 24 + i * 2));
    }
    return new Uint8Array(b);
}

// A colormap: its header, then 256 colors.
function makeCmp(colors) {
    const b = Buffer.alloc(64 + 768 + 256 * 64);
    b.write('CMP ', 0, 'latin1');
    b.writeInt32LE(30, 4);
    colors.forEach((c, i) => { b[64 + i * 3] = c[0]; b[64 + i * 3 + 1] = c[1]; b[64 + i * 3 + 2] = c[2]; });
    return new Uint8Array(b);
}

// A PNG of w x h RGBA, rows top first.
function makePng(w, h, rgba) {
    const chunk = (type, data) => {
        const len = Buffer.alloc(4), crc = Buffer.alloc(4), td = Buffer.concat([Buffer.from(type, 'latin1'), data]);
        len.writeUInt32BE(data.length);
        crc.writeUInt32BE(crc32(td) >>> 0);
        return Buffer.concat([len, td, crc]);
    };
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w, 0);
    ihdr.writeUInt32BE(h, 4);
    ihdr[8] = 8;
    ihdr[9] = 6;
    const raw = Buffer.alloc(h * (1 + w * 4));
    for (let y = 0; y < h; y++)
        Buffer.from(rgba.slice(y * w * 4, (y + 1) * w * 4)).copy(raw, y * (1 + w * 4) + 1);
    return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
const dataUrl = png => 'data:image/png;base64,' + png.toString('base64');

// The game's own files: a colormap and a texture of the game's, in a
// resource GOB as Res2.gob has them. Its colors: index i is (i, 255 - i, 7).
const GAME_PAL = Array.from({ length: 256 }, (_, i) => [i, 255 - i, 7]);
const VANILLA = [10, 20, 30, 40];  // 2 x 2, rows as the file has them
const res2 = makeGob([
    { name: 'mat\\vanilla.mat', bytes: makeMat({ w: 2, h: 2, pixels: VANILLA }) },
    { name: 'misc\\cmp\\dflt.cmp', bytes: makeCmp(GAME_PAL) },
]);
// The level's own colormap (in its tex.json), and own texture's PNG: top
// row red and green, bottom row blue and white.
const LEVEL_PAL = Array.from({ length: 256 }, (_, i) => [255 - i, i, 99]);
const OWN_PNG = makePng(2, 2, [255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255]);
// The preview's blurred stand-in for a game texture: grey.
const STANDIN_PNG = makePng(2, 2, [].concat(...new Array(4).fill([128, 128, 128, 255])));
const box = (mat, z) => ({ vertices: [[[0, 0, z], [0, 0], 1], [[1, 0, z], [1, 0], 0.5], [[1, 1, z], [1, 1], 0], [[0, 1, z], [0, 1], 1]], material: mat });
const MAP = {
    surfaces: [box(0, 0), box(1, 1), box(2, 0.5)], model_surfaces: [], sky_surfaces: [box(3, 2)],
    material_colors: ['#ff0000', '#00ff00', '#0000ff', '#808080'],
    spawn_points: [{ pos: [0.5, 0.5, 0], rot: [0, 0, 0] }, { pos: [0.2, 0.8, 0], rot: [0, 90, 0] }],
};
const MATS = [
    { name: 'mat/own.mat', data: dataUrl(OWN_PNG) },
    { name: 'mat/vanilla.mat', data: dataUrl(STANDIN_PNG) },
    { name: 'mat/gone.mat', data: dataUrl(STANDIN_PNG) },
    '',
];
// What the site keeps of a level for its 3D view: path -> [status, body].
const PV = {
    '501-0.map.json': [200, MAP],
    '501-0.mat.json': [200, MATS.slice(0, 2)],
    '501.tex.json': [200, { own: ['mat/own.mat'], cmp: { alpha: 'alpha.cmp' }, pal: { 'alpha.cmp': Buffer.from(makeCmp(LEVEL_PAL).subarray(64, 64 + 768)).toString('base64') } }],
    '502-0.map.json': [200, MAP],
    '502-0.mat.json': [200, MATS],
    '502-1.map.json': [500, 'no'],
    '502.tex.json': [200, { own: ['mat/own.mat'], cmp: { beta1: 'dflt.cmp', beta2: 'dflt.cmp' } }],
};

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
    level(501, 'mp', 'Alpha Arena', 'jk1', [{ n: 'alpha.gob', bytes: alpha, ep: 'Alpha Arena', ty: 2, lv: ['alpha.jkl'] }], { pv: ['alpha'] }),
    level(502, 'ctf', 'Beta Base CTF', 'jk1', [{ n: 'Beta/beta.gob', bytes: beta, ep: 'Beta Base', ty: 8, lv: ['beta1.jkl', 'beta2.jkl'] }],
          { rel: '2001-03', rp: 'm', src: 'f', add: '2001-04-10', pv: ['beta1', 'beta2'] }),
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
    if ((m = /^\/levels\/p\/(.+)$/.exec(path)) && PV[m[1]])
        return send(PV[m[1]][0], 'application/json', typeof PV[m[1]][1] === 'string' ? PV[m[1]][1] : JSON.stringify(PV[m[1]][1]));
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
    await page.evaluate(([maps, preview, library]) => {
        Module.canvas = document.getElementById('canvas');
        new Function('Module', maps)(Module);
        new Function('Module', preview)(Module);
        new Function('Module', library)(Module);
        for (const f of Module.preRun) f();
    }, [mapsJs, previewJs, libraryJs]);
    await page.waitForFunction(() => Module.jkLibrary.available());
    return { page, errors };
}

// What the menu does, through Module.jkLibrary.
const call = (page, fn, ...args) => page.evaluate(([fn, args]) => Module.jkLibrary[fn](...args), [fn, args]);
const libText = (page, what, arg = 0) => call(page, 'text', what, arg);
const number = (page, what) => call(page, 'number', what);
const T = { STATUS: 0, ROW: 1, SORT: 2, TITLE: 3, OVERVIEW: 4, README: 5, FILES: 6, MESSAGE: 7,
            PV: 8, VIEW_TITLE: 9, VIEW_HELP: 10, VIEW_NOTE: 11, VIEW_MODE: 12, VIEW_NEXT: 13 };
const N = { CHANGED: 0, COUNT: 1, SELECTED: 2, CAN_USE: 3, BUSY: 4, DONE: 5, SHOTS: 6, PV: 7 };
const F = { MP: 1, CTF: 2, SP: 4, MINE: 8 };

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

// The menu asks for a frame every frame it's up: this asks every 50 ms
// until stop().
const keep = (page, fn, ...args) => page.evaluate(([fn, args]) => {
    clearInterval(window.__keep);
    Module.jkLibrary[fn](...args);
    window.__keep = setInterval(() => Module.jkLibrary[fn](...args), 50);
}, [fn, args]);
const stop = page => page.evaluate(() => clearInterval(window.__keep));
const view3d = page => page.evaluate(() => {
    const c = document.getElementById('jk-lib-3d'), i = document.getElementById('jk-lib-3d-icon');
    if (!c || c.style.display !== 'block')
        return null;
    const r = c.getBoundingClientRect(), ir = i.getBoundingClientRect();
    return { x: Math.round(r.x), y: Math.round(r.y), w: Math.round(r.width), h: Math.round(r.height), mouse: c.style.pointerEvents,
             icon: i.style.display === 'block' ? { right: Math.round(r.right - ir.right), bottom: Math.round(r.bottom - ir.bottom) } : null };
});
// A texture the 3D view drew with, read back: rows from v 0, [r, g, b, a].
const texels = (page, i) => page.evaluate(i => {
    const r = Module.jkPreview3d._V.renderer, gl = r.gl, t = r.textures[i];
    if (!t)
        return null;
    gl.bindTexture(gl.TEXTURE_2D, t);
    const fb = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, t, 0);
    const px = new Uint8Array(16);
    gl.readPixels(0, 0, 2, 2, gl.RGBA, gl.UNSIGNED_BYTE, px);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.deleteFramebuffer(fb);
    return [[Array.from(px.slice(0, 4)), Array.from(px.slice(4, 8))], [Array.from(px.slice(8, 12)), Array.from(px.slice(12, 16))]];
}, i);
const through = (pal, k) => [...pal[k], 255];

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
const jk = await game({ files: { '/jk1/episode/JK1MP.gob': text('the original'), '/jk1/episode/taken.gob': theTaken, '/jk1/resource/Res2.gob': res2 } });
const page = jk.page;

await test('the list: multiplayer and CTF levels, by name, with the counts', async () => {
    assert.equal(await call(page, 'begin', 'jk1'), true);
    // Everything is new to a menu that just opened.
    assert.equal(await number(page, N.CHANGED), 127);
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

await test('the 3D view\'s pieces: MATs, colormaps, GOBs, the map\'s triangles, which textures it swaps', async () => {
    const got = await page.evaluate(([mats, cmp, gob, pngUrl, pal]) => {
        const P = Module.jkPreview3d, b = a => new Uint8Array(a), px = r => r && { w: r.w, h: r.h, rgba: Array.from(r.rgba) };
        const palette = b(pal);
        const out = { mats: mats.map(m => px(P._decodeMat(b(m), palette))), grey: px(P._decodeMat(b(mats[0]), null)) };
        out.badMat = P._decodeMat(b([1, 2, 3]), palette);
        out.cmp = Array.from(P._cmpColors(b(cmp)) || []).slice(0, 6);
        out.cmpLen = (P._cmpColors(b(cmp)) || []).length;
        out.badCmp = P._cmpColors(b(cmp).fill(0, 0, 4));
        out.gob = Object.keys(P._gobFiles(b(gob)) || {}).sort();
        const quad = { vertices: [[[0, 0, 0], [0, 0], 1], [[2, 0, 0], [1, 0], 0.5], [[2, 3, 0], [1, 1]], [[0, 3, 0], [0, 1], 0]], material: 0 };
        const tri = { vertices: [[[0, 0, -1], [0, 0], 1], [[1, 0, 4], [1, 0], 1], [[0, 1, 0], [0, 1], 1]], material: 1 };
        const geo = P._buildGeometry([[quad, tri, quad], [{ vertices: [[[9, 9, 9], [0, 0], 1], [[9, 9, 9], [0, 0], 1]], material: 2 }], null]);
        out.geo = { n: geo.verts.length / 5, index: Array.from(geo.index), draws: geo.draws, min: geo.min, max: geo.max, empty: geo.empty,
                    v2: Array.from(geo.verts.slice(10, 15)) };
        out.emptyGeo = P._buildGeometry([[]]).empty;
        // The player standing at a starting place isn't drawn; a model beside
        // it, or reaching past it, is.
        const tri3 = (x, y, z, h) => ({ vertices: [[[x, y, z], [0, 0]], [[x + 0.03, y, z], [1, 0]], [[x, y + 0.03, z + h], [0, 1]]], material: 0 });
        const atStart = P._prepareMap({ surfaces: [], sky_surfaces: [], spawn_points: [{ pos: [1, 1, 1], rot: [0, 0, 0] }],
                                        model_surfaces: [tri3(0.97, 0.97, 0.89, 0.15), tri3(1.2, 1, 1, 0.05), tri3(1, 1, 0.9, 0.3)] });
        out.startTris = atStart.world.index.length / 3;
        // Which texture comes from where.
        const files = { find: n => n === 'mat/vanilla.mat' ? b(mats[0]) : n === 'misc/cmp/dflt.cmp' ? b(cmp) : null };
        const list = [{ name: 'mat/own.mat', data: 'x' }, { name: 'MAT\\Vanilla.MAT', data: 'x' }, { name: 'mat/gone.mat', data: 'x' },
                      { name: '3do/mat/vanilla.mat', data: 'x' }, { name: 'mat/own2.mat', data: 'x' }, ''];
        const src = r => r.list.map(e => e.src);
        const withPal = P._textureSources(list, { own: ['mat/own.mat', '3do/mat/own2.mat'], cmp: { a: 'a.cmp' }, pal: { 'a.cmp': btoa(String.fromCharCode(...palette)) } }, 'a', files);
        out.withPal = { src: src(withPal), cmp: withPal.cmp, pal: Array.from(withPal.pal.slice(0, 6)), names: withPal.list.map(e => e.name) };
        const gamePal = P._textureSources(list, { own: [], cmp: { a: 'dflt.cmp' } }, 'jkl/A.JKL', files);
        out.gamePal = { src: src(gamePal), pal: Array.from(gamePal.pal.slice(0, 6)) };
        out.noTex = src(P._textureSources(list, null, 'a', files));
        out.noPal = src(P._textureSources(list, { own: [], cmp: { a: 'missing.cmp' } }, 'a', files));
        out.noFiles = src(P._textureSources(list, { own: ['mat/own.mat'], cmp: { a: 'a.cmp' }, pal: withPal.pal && { 'a.cmp': btoa(String.fromCharCode(...palette)) } }, 'a', null));
        return P._pngPixels(pngUrl).then(r => (out.png = px(r), out));
    }, [[makeMat({ w: 2, h: 2, pixels: VANILLA }), makeMat({ w: 2, h: 1, pixels: [0, 5], transparent: 0, hasT: 1 }),
         makeMat({ w: 2, h: 1, pixels: [0, 5], transparent: 0, hasT: 0 }),
         makeMat({ bits: 16, w: 2, h: 2, pixels: [0xF800, 0x07E0, 0x001F, 0xFFFF] }), makeMat({ type: 0, color: 7 })].map(m => Array.from(m)),
        Array.from(makeCmp(GAME_PAL)), Array.from(res2), dataUrl(OWN_PNG), Array.from(makeCmp(GAME_PAL).subarray(64, 64 + 768))]);
    const P8 = k => through(GAME_PAL, k);
    // 8-bit: through the colormap, rows as the file has them.
    assert.deepEqual(got.mats[0], { w: 2, h: 2, rgba: [...P8(10), ...P8(20), ...P8(30), ...P8(40)] });
    // Its transparent color clear, only when the texture says it has one.
    assert.deepEqual(got.mats[1].rgba, [...P8(0).slice(0, 3), 0, ...P8(5)]);
    assert.deepEqual(got.mats[2].rgba, [...P8(0), ...P8(5)]);
    // 16-bit, by the file's own channels (RGB 565 here).
    assert.deepEqual(got.mats[3].rgba, [248, 0, 0, 255, 0, 252, 0, 255, 0, 0, 248, 255, 248, 252, 248, 255]);
    // A color MAT: one pixel of its color.
    assert.deepEqual(got.mats[4], { w: 1, h: 1, rgba: P8(7) });
    // No colormap: the indices as grey.
    assert.deepEqual(got.grey.rgba.filter((_, i) => i % 4 === 0), VANILLA);
    assert.equal(got.badMat, null);
    assert.equal(got.cmpLen, 768);
    assert.deepEqual(got.cmp, [...GAME_PAL[0], ...GAME_PAL[1]]);
    assert.equal(got.badCmp, null);
    assert.deepEqual(got.gob, ['mat/vanilla.mat', 'misc/cmp/dflt.cmp']);
    // Fans of triangles, one draw a texture; a surface of two corners left out.
    assert.equal(got.geo.n, 11);
    assert.deepEqual(got.geo.index, [0, 1, 2, 0, 2, 3, 7, 8, 9, 7, 9, 10, 4, 5, 6]);
    assert.deepEqual(got.geo.draws, [{ mat: 0, first: 0, count: 12 }, { mat: 1, first: 12, count: 3 }]);
    assert.deepEqual([got.geo.min, got.geo.max], [[0, 0, -1], [2, 3, 4]]);
    assert.deepEqual(got.geo.v2, [2, 3, 0, 1, 1], 'each corner: where it is, and where on its texture');
    assert.equal(got.geo.empty, false);
    assert.equal(got.emptyGeo, true);
    assert.equal(got.startTris, 2, 'the player at the starting place left out, the two others drawn');
    // The level's own textures as the preview has them; the game's own from
    // the player's files (by name, in mat/ or 3do/mat/); one in neither a
    // stand-in; no picture, a color.
    assert.deepEqual(got.withPal.src, ['level', 'game', 'standin', 'game', 'level', 'color']);
    assert.equal(got.withPal.names[1], 'mat/vanilla.mat');
    assert.equal(got.withPal.cmp, 'a.cmp');
    assert.deepEqual(got.withPal.pal, [...GAME_PAL[0], ...GAME_PAL[1]]);
    // A level on one of the game's colormaps: it comes from the game's files.
    assert.deepEqual(got.gamePal.src, ['standin', 'game', 'standin', 'game', 'standin', 'color']);
    assert.deepEqual(got.gamePal.pal, [...GAME_PAL[0], ...GAME_PAL[1]]);
    // Without what the level ships, or its colormap: nothing swapped.
    assert.deepEqual(got.noTex, ['standin', 'standin', 'standin', 'standin', 'standin', 'color']);
    assert.deepEqual(got.noPal, ['standin', 'standin', 'standin', 'standin', 'standin', 'color']);
    // No game files: the level's own still exact, the game's stand-ins.
    assert.deepEqual(got.noFiles, ['level', 'standin', 'standin', 'standin', 'standin', 'color']);
    // A PNG's rows bottom first, as the preview made it from the MAT.
    assert.deepEqual(got.png, { w: 2, h: 2, rgba: [0, 0, 255, 255, 255, 255, 255, 255, 255, 0, 0, 255, 0, 255, 0, 255] });
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

await test('My levels: only the levels saved in this browser, of the kinds ticked, searched', async () => {
    await open(page, 'jk1', ['', F.MP | F.CTF | F.MINE, 0]);
    assert.deepEqual(await rows(page), [501, 502, 506, 503, 507]);
    assert.match(await libText(page, T.STATUS), /levels from Massassi\.net/);
    await call(page, 'query', '', F.CTF | F.MINE, 0);
    assert.deepEqual(await rows(page), [502]);
    await call(page, 'query', 'gamma', F.MP | F.CTF | F.MINE, 0);
    assert.deepEqual(await rows(page), [503]);
    await call(page, 'number', N.CHANGED);
    await call(page, 'query', 'nothing like this', F.MP | F.CTF | F.MINE, 0);
    assert.deepEqual(await rows(page), []);
    assert.equal((await number(page, N.CHANGED)) & 1, 1, 'the line under the title drawn again');
    assert.equal(await libText(page, T.STATUS), 'None of the levels saved in this browser match.');
    await call(page, 'query', '', F.MP | F.CTF, 0);
    assert.equal((await rows(page)).length, 7);
    assert.match(await libText(page, T.STATUS), /levels from Massassi\.net/);
    await call(page, 'end');
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

await test('the 3D view in the Overview\'s frame: the level\'s own textures exact, the game\'s from the player\'s files', async () => {
    await open(page);
    await pick(page, 503);
    // A level with no 3D view says so, and nothing is drawn.
    assert.equal(await number(page, N.PV), 0);
    assert.equal(await libText(page, T.PV), 'No 3D view for this level yet');
    await keep(page, 'preview', 1, 475, 105, 154, 115);
    await page.waitForTimeout(300);
    assert.equal(await view3d(page), null);
    requests.length = 0;
    await pick(page, 501);
    assert.equal(await number(page, N.PV), 1);
    assert.equal(await libText(page, T.PV), 'Loading the 3D view...');
    await page.waitForFunction(() => Module.jkLibrary.number(7) === 2, null, { timeout: 10000 });
    assert.deepEqual(requests.filter(r => r.includes('/levels/p/')).sort(), ['GET /levels/p/501-0.map.json', 'GET /levels/p/501-0.mat.json', 'GET /levels/p/501.tex.json']);
    assert.equal(await libText(page, T.PV), '');
    assert.equal(await page.evaluate(() => !Module.jkPreview3d._worker.off && !!Module.jkPreview3d._worker.url), true, 'the map read in a worker');
    // Over the frame, inside its border: 1280 x 800, a menu pixel 1.667 of the page's.
    await page.waitForFunction(() => document.getElementById('jk-lib-3d').style.display === 'block');
    const box = await view3d(page);
    assert.deepEqual([box.x, box.y, box.w, box.h], [898, 175, 257, 192]);
    assert.equal(box.mouse, 'none', 'the small view never takes the mouse from the game');
    assert.ok(box.icon && box.icon.right >= 0 && box.icon.right < 10 && box.icon.bottom >= 0 && box.icon.bottom < 10, JSON.stringify(box.icon));
    await page.waitForFunction(() => Module.jkPreview3d._V.scene && Module.jkPreview3d._V.scene.total === 2 &&
                                     Module.jkPreview3d._V.renderer.textures.filter(Boolean).length === 2);
    assert.deepEqual(await page.evaluate(() => [Module.jkPreview3d.swapped(), Module.jkPreview3d.missing()]), [1, 0]);
    // One side of each surface, the front, as Massassi's preview draws them.
    assert.deepEqual(await page.evaluate(() => {
        const gl = Module.jkPreview3d._V.renderer.gl;
        return [gl.isEnabled(gl.CULL_FACE), gl.getParameter(gl.CULL_FACE_MODE) === gl.BACK, gl.getParameter(gl.FRONT_FACE) === gl.CCW];
    }), [true, true, true]);
    // Its own texture is the preview's PNG, rows bottom first ...
    assert.deepEqual(await texels(page, 0), [[[0, 0, 255, 255], [255, 255, 255, 255]], [[255, 0, 0, 255], [0, 255, 0, 255]]]);
    // ... the game's is the player's own, through the level's own master
    // colormap (the one in its zip), not the preview's grey stand-in.
    assert.deepEqual(await texels(page, 1), [[through(LEVEL_PAL, 10), through(LEVEL_PAL, 20)], [through(LEVEL_PAL, 30), through(LEVEL_PAL, 40)]]);
    assert.equal(await libText(page, T.VIEW_TITLE), 'Alpha Arena');
    assert.equal(await libText(page, T.VIEW_NOTE), '3D view from Massassi.net\'s 3D Preview (jkview, by Stephan Reiter).');
    // The menu stops asking (Readme, or it closed): it goes.
    await stop(page);
    await call(page, 'preview', 0, 0, 0, 0, 0);
    await page.waitForFunction(() => document.getElementById('jk-lib-3d').style.display === 'none');
});

await test('the big 3D view: orbit, walk from the starting places, the keys and the mouse, then back to the small one', async () => {
    await call(page, 'number', N.CHANGED);
    await keep(page, 'view', 1, 21, 59, 598, 310);
    assert.equal((await number(page, N.CHANGED)) & 64, 64, 'its lines drawn');
    await page.waitForFunction(() => document.getElementById('jk-lib-3d').style.display === 'block' &&
                                     document.getElementById('jk-lib-3d').style.pointerEvents === 'auto');
    const box = await view3d(page);
    assert.deepEqual([box.x, box.y, box.w, box.h, box.icon], [142, 98, 997, 517, null]);
    assert.equal(await libText(page, T.VIEW_HELP), 'Drag to turn it, right-drag to move it, the wheel to zoom.');
    assert.equal(await libText(page, T.VIEW_MODE), 'Walk');
    assert.equal(await libText(page, T.VIEW_NEXT), '', 'one map: no Next Map');
    // The wheel zooms.
    const dist = () => page.evaluate(() => Module.jkPreview3d._V.orbit && Module.jkPreview3d._V.orbit.dist);
    await page.mouse.move(640, 350);
    await page.mouse.wheel(0, 100);
    await page.waitForFunction(() => Module.jkPreview3d._V.orbit);
    const d1 = await dist();
    await page.mouse.wheel(0, -200);
    await page.waitForFunction(d => Module.jkPreview3d._V.orbit.dist < d, d1);
    // Walk: at the first starting place, the eye over it.
    await call(page, 'viewCommand', 1);
    assert.equal(await libText(page, T.VIEW_MODE), 'Orbit');
    assert.equal(await libText(page, T.VIEW_NEXT), 'Next Start');
    assert.equal(await libText(page, T.VIEW_NOTE), 'Starting place 1 of 2.');
    assert.match(await libText(page, T.VIEW_HELP), /^Drag to look, W A S D to move/);
    const walk = () => page.evaluate(() => { const w = Module.jkPreview3d._V.walk; return { pos: w.pos.slice(), yaw: w.yaw, pitch: w.pitch }; });
    let w = await walk();
    assert.deepEqual(w.pos, [0.5, 0.5, 1 / 16]);
    // W walks forward (+y at the first place's turn).
    await page.keyboard.down('KeyW');
    await page.waitForTimeout(400);
    await page.keyboard.up('KeyW');
    const w2 = await walk();
    assert.ok(w2.pos[1] > 0.6 && Math.abs(w2.pos[0] - 0.5) < 1e-6, JSON.stringify(w2.pos));
    // A drag turns and tilts the eye.
    await page.mouse.move(600, 300);
    await page.mouse.down();
    await page.mouse.move(700, 250, { steps: 4 });
    await page.mouse.up();
    const w3 = await walk();
    assert.ok(w3.yaw > w2.yaw && w3.pitch > 0, JSON.stringify(w3));
    // The wheel steps back.
    await page.mouse.wheel(0, 100);
    await page.waitForTimeout(50);
    const w4 = await walk();
    assert.ok(Math.hypot(w4.pos[0] - w3.pos[0], w4.pos[1] - w3.pos[1], w4.pos[2] - w3.pos[2]) > 0.2);
    // The next starting place, turned as it is.
    await call(page, 'viewCommand', 2);
    assert.equal(await libText(page, T.VIEW_NOTE), 'Starting place 2 of 2.');
    w = await walk();
    assert.deepEqual(w.pos, [0.2, 0.8, 1 / 16]);
    assert.ok(Math.abs(w.yaw + Math.PI / 2) < 1e-9, String(w.yaw));
    // Keys do nothing once the big view is down.
    await stop(page);
    await call(page, 'view', 0, 0, 0, 0, 0);
    await page.keyboard.down('KeyW');
    await page.waitForTimeout(200);
    await page.keyboard.up('KeyW');
    assert.deepEqual((await walk()).pos, [0.2, 0.8, 1 / 16]);
    await page.waitForFunction(() => document.getElementById('jk-lib-3d').style.display === 'none');
    // Back on the Overview: the small view, which doesn't take the mouse.
    await keep(page, 'preview', 1, 475, 105, 154, 115);
    await page.waitForFunction(() => document.getElementById('jk-lib-3d').style.display === 'block');
    assert.equal((await view3d(page)).mouse, 'none');
    await call(page, 'viewCommand', 1);
    await stop(page);
    await call(page, 'end');
    await page.waitForFunction(() => document.getElementById('jk-lib-3d').style.display === 'none');
    assert.equal(await page.evaluate(() => Module.jkPreview3d._V.want), null);
});

await test('the big 3D view of a level with two maps: Next Map, a texture not in the player\'s files, one that doesn\'t load', async () => {
    // Where the page can't start a worker: the same, on the page's thread.
    await page.evaluate(() => { Module.jkPreview3d._worker.off = true; });
    await open(page);
    await pick(page, 502);
    // Opened straight away, it doesn't wait for the pick to rest.
    await keep(page, 'view', 1, 21, 59, 598, 310);
    await page.waitForFunction(() => Module.jkLibrary.number(7) === 2, null, { timeout: 10000 });
    assert.equal(await libText(page, T.VIEW_TITLE), 'Beta Base CTF: beta1 (1 of 2)');
    assert.equal(await libText(page, T.VIEW_NEXT), 'Next Map');
    await page.waitForFunction(() => /isn't in your game files/.test(Module.jkLibrary.text(11)));
    assert.equal(await libText(page, T.VIEW_NOTE), '1 of the game\'s own textures isn\'t in your game files: shown blurred.');
    // On one of the game's own colormaps: from the player's files too.
    assert.deepEqual(await texels(page, 1), [[through(GAME_PAL, 10), through(GAME_PAL, 20)], [through(GAME_PAL, 30), through(GAME_PAL, 40)]]);
    assert.deepEqual(await texels(page, 2), [[[128, 128, 128, 255], [128, 128, 128, 255]], [[128, 128, 128, 255], [128, 128, 128, 255]]]);
    assert.equal(await texels(page, 3), null, 'no picture: its color');
    // The second map doesn't load: it says so, and Next Map goes on. While
    // it's coming, Next Map is there too.
    await call(page, 'viewCommand', 2);
    if (await number(page, N.PV) === 1)
        assert.equal(await libText(page, T.VIEW_NEXT), 'Next Map', 'while the next map loads');
    await page.waitForFunction(() => Module.jkLibrary.number(7) === 3, null, { timeout: 10000 });
    assert.equal(await libText(page, T.VIEW_TITLE), 'Beta Base CTF: beta2 (2 of 2)');
    assert.equal(await libText(page, T.PV), 'The 3D view didn\'t load. Click to try again.');
    assert.equal(await libText(page, T.VIEW_MODE), '');
    assert.equal(await libText(page, T.VIEW_NEXT), 'Next Map');
    // Again: asked for once more.
    requests.length = 0;
    await call(page, 'viewCommand', 3);
    await page.waitForFunction(() => Module.jkLibrary.number(7) === 3, null, { timeout: 10000 });
    assert.ok(requests.includes('GET /levels/p/502-1.map.json'), requests.join('|'));
    // Next Map: the first again.
    await call(page, 'viewCommand', 2);
    await page.waitForFunction(() => Module.jkLibrary.number(7) === 2, null, { timeout: 10000 });
    assert.equal(await libText(page, T.VIEW_TITLE), 'Beta Base CTF: beta1 (1 of 2)');
    // Another level starts from its first map.
    await stop(page);
    await call(page, 'view', 0, 0, 0, 0, 0);
    await pick(page, 501);
    assert.equal(await libText(page, T.VIEW_TITLE), 'Alpha Arena');
    await call(page, 'end');
    await page.evaluate(() => { Module.jkPreview3d._worker.off = false; });
});

await test('a game texture that isn\'t in its own files: from the other game\'s files saved in this browser, else blurred and said whose', async () => {
    const res1hi = makeGob([{ name: 'misc\\x.txt', bytes: text('x') }]);
    const m = await game({ files: { '/jk1/resource/Res2.gob': res2, '/jk1/resource/Res1hi.gob': res1hi } });
    // This browser has no Mysteries of the Sith files saved.
    await m.page.evaluate(() => { Module.jkSavedGameFile = async () => null; });
    await open(m.page);
    await pick(m.page, 502);
    await keep(m.page, 'view', 1, 21, 59, 598, 310);
    await m.page.waitForFunction(() => /from Mysteries of the Sith/.test(Module.jkLibrary.text(11)), null, { timeout: 10000 });
    assert.equal(await libText(m.page, T.VIEW_NOTE), '1 texture here is from Mysteries of the Sith: shown blurred.');
    assert.deepEqual(await texels(m.page, 2), [[[128, 128, 128, 255], [128, 128, 128, 255]], [[128, 128, 128, 255], [128, 128, 128, 255]]]);
    await stop(m.page);
    await call(m.page, 'view', 0, 0, 0, 0, 0);
    await call(m.page, 'end');
    // Now it has: the texture comes from MotS's JKMRES.GOO, through the
    // level's colormap, read from the saved file in pieces.
    const gone = [50, 60, 70, 80];
    const jkmres = makeGob([{ name: 'misc\\y.txt', bytes: text('y') }, { name: '3do\\mat\\gone.mat', bytes: makeMat({ w: 2, h: 2, pixels: gone }) }]);
    const asked = await m.page.evaluate(async bytes => {
        const asked = [];
        Module.jkPreview3d._forgetOther();
        const blob = new Blob([new Uint8Array(bytes)]);
        Module.jkSavedGameFile = async (game, rel) => { asked.push(game + ' ' + rel); return rel === 'resource/jkmres.goo' ? blob : null; };
        window.__asked = asked;
        return asked;
    }, Array.from(jkmres));
    assert.deepEqual(asked, []);
    await open(m.page);
    const loaded = id => m.page.waitForFunction(id => { const s = Module.jkPreview3d._V.scene; return s && s.id === id && s.loaded; }, id, { timeout: 10000 });
    await pick(m.page, 501);
    await keep(m.page, 'view', 1, 21, 59, 598, 310);
    await loaded(501);
    await pick(m.page, 502);
    await loaded(502);
    assert.deepEqual(await m.page.evaluate(() => window.__asked), ['mots resource/jkmres.goo']);
    assert.deepEqual(await m.page.evaluate(() => [Module.jkPreview3d.swapped(), Module.jkPreview3d.missing()]), [2, 0]);
    assert.deepEqual(await texels(m.page, 2), [[through(GAME_PAL, 50), through(GAME_PAL, 60)], [through(GAME_PAL, 70), through(GAME_PAL, 80)]]);
    assert.equal(await libText(m.page, T.VIEW_NOTE), '3D view from Massassi.net\'s 3D Preview (jkview, by Stephan Reiter).');
    // The GOB's pieces only, not all of it.
    assert.deepEqual(await m.page.evaluate(async bytes => {
        const g = await Module.jkPreview3d._blobGob(new Blob([new Uint8Array(bytes)]));
        return [Object.keys(g.files).sort(), Array.from(await g.read(g.files['misc/y.txt']))];
    }, Array.from(jkmres)), [['3do/mat/gone.mat', 'misc/y.txt'], Array.from(text('y'))]);
    assert.equal(await m.page.evaluate(() => Module.jkPreview3d._blobGob(new Blob([new Uint8Array([1, 2, 3])]))), null);
    await stop(m.page);
    await call(m.page, 'view', 0, 0, 0, 0, 0);
    await call(m.page, 'end');
    assert.deepEqual(m.errors.filter(e => !/Failed to load resource/.test(e)), []);
    await m.page.close();
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

await test('My levels with nothing saved yet says so', async () => {
    const m = await game();
    assert.equal(await call(m.page, 'begin', 'jk1'), true);
    await call(m.page, 'query', '', F.MP | F.CTF | F.MINE, 0);
    await m.page.waitForFunction(() => /saved in this browser/.test(Module.jkLibrary.text(0)));
    assert.equal(await libText(m.page, T.STATUS), 'No levels saved in this browser yet.');
    assert.equal(await number(m.page, N.COUNT), 0);
    assert.equal(await number(m.page, N.SELECTED), -1);
    assert.equal(await libText(m.page, T.TITLE), 'My levels');
    assert.match(await libText(m.page, T.OVERVIEW), /^No levels saved in this browser yet\.\n\n/);
    // Searched: says none match.
    await call(m.page, 'query', 'zzz', F.MP | F.CTF | F.MINE, 0);
    assert.equal(await libText(m.page, T.OVERVIEW), 'None of the levels saved in this browser match the search.');
    // Unticked, the first level is picked again; ticked with one picked, the
    // pick goes with the list, and so does what the details showed.
    await call(m.page, 'query', '', F.MP | F.CTF, 0);
    await m.page.waitForFunction(() => Module.jkLibrary.number(1) > 0 && Module.jkLibrary.number(2) === 0);
    assert.notEqual(await libText(m.page, T.TITLE), '');
    await m.page.evaluate(() => Module.jkLibrary.number(0));
    await call(m.page, 'query', '', F.MP | F.CTF | F.MINE, 0);
    assert.equal(await number(m.page, N.SELECTED), -1);
    assert.ok((await number(m.page, N.CHANGED)) & 4, 'the details told to change');
    assert.equal(await libText(m.page, T.TITLE), 'My levels');
    assert.equal(await number(m.page, N.SHOTS), 0);
    assert.equal(await number(m.page, N.PV), 0);
    await call(m.page, 'end');
    await m.page.close();
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
