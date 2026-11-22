// Tests for the HD menu pictures (Setup > Display > Advanced > HD Menus and
// Backgrounds, src/Gui/jkGUIHd.h): the files the page names, what the engine
// knows of them, and how the engine fetches them and makes their textures
// (std3D_wasmHdTexture in src/Platform/GL/std3D.c). In Node:
// node packaging/wasm/test/hdmenus-test.mjs
//
// The page's list and the engine's JS are taken from index.html and std3D.c
// as they are; WebGL, fetch, decoding and the clock are made up by the test.

import { readFileSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const std3D = readFileSync(new URL('../../../src/Platform/GL/std3D.c', import.meta.url), 'utf8');
const data = readFileSync(new URL('../../../src/Gui/jkGUIHd_data.h', import.meta.url), 'utf8');
const hdc = readFileSync(new URL('../../../src/Gui/jkGUIHd.c', import.meta.url), 'utf8');
function cut(text, from, to) {
    const a = text.indexOf(from), b = text.indexOf(to, a);
    assert.ok(a >= 0 && b > a, 'has ' + from);
    return text.substring(a, b);
}

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

const PICTURES = new Function(cut(html, '      var HD_MENU_PICTURES = {', '\n      };\n') + '\n      };\nreturn HD_MENU_PICTURES;')();
const NAMES = ['bkmain', 'bksingle', 'bkmulti', 'bksetup', 'bkesc', 'bkloading', 'bkfieldlog', 'bkforce', 'bktally',
               'bkbuildmulti', 'bkbuildload'];

// The rows of jkGuiHd_aPictureInfo: name, 4:3 size, wide size, the part under the menu
const INFO = [...data.matchAll(/\{ "(\w+)", (\d+), (\d+), (\d+), (\d+), \{ ([\d.]+)f, ([\d.]+)f, ([\d.]+)f, ([\d.]+)f \} \}/g)]
    .map(m => ({ name: m[1], std: [+m[2], +m[3]], wide: [+m[4], +m[5]], part: [+m[6], +m[7], +m[8], +m[9]] }));

// A WebP file's width and height (RIFF, then a VP8, VP8L or VP8X chunk)
function webpSize(b) {
    assert.equal(b.toString('latin1', 0, 4), 'RIFF');
    assert.equal(b.toString('latin1', 8, 12), 'WEBP');
    const kind = b.toString('latin1', 12, 16);
    if (kind === 'VP8 ') return [b.readUInt16LE(26) & 0x3fff, b.readUInt16LE(28) & 0x3fff];
    if (kind === 'VP8L') { const v = b.readUInt32LE(21); return [(v & 0x3fff) + 1, ((v >> 14) & 0x3fff) + 1]; }
    if (kind === 'VP8X') return [b.readUIntLE(24, 3) + 1, b.readUIntLE(27, 3) + 1];
    throw new Error('not a WebP chunk: ' + kind);
}

test('the page names both sets, 11 pictures each, in the engine\'s order, named by their content', () => {
    assert.deepEqual(Object.keys(PICTURES), ['43', 'wide']);
    const count = +/#define JKGUIHD_NUM_PICTURES \((\d+)\)/.exec(readFileSync(new URL('../../../src/Gui/jkGUIHd.h', import.meta.url), 'utf8'))[1];
    assert.equal(count, NAMES.length);
    for (const set of ['43', 'wide']) {
        assert.equal(PICTURES[set].length, count, set);
        PICTURES[set].forEach((name, i) => {
            // What jkdf2mp-bb-server's build-kit.mjs copies into a kit and _headers keeps a year
            const m = /^rooms\/hdmenu-(43|wide)-([a-z]+)\.([0-9a-f]{8})\.webp$/.exec(name);
            assert.ok(m, name);
            assert.equal(m[1], set, name);
            assert.equal(m[2], NAMES[i], name + ' is picture ' + i);
            const file = new URL('../' + name, import.meta.url);
            assert.ok(existsSync(file), name + ' is there');
            assert.equal(createHash('sha256').update(readFileSync(file)).digest('hex').slice(0, 8), m[3], name + "'s name matches its content");
        });
    }
});

test("the engine's sizes are the files' own, and each wide picture's part under the menu is 4:3 and inside it", () => {
    assert.deepEqual(INFO.map(r => r.name), NAMES);
    INFO.forEach((r, i) => {
        assert.deepEqual(webpSize(readFileSync(new URL('../' + PICTURES['43'][i], import.meta.url))), r.std, r.name + ' 4:3');
        assert.deepEqual(webpSize(readFileSync(new URL('../' + PICTURES.wide[i], import.meta.url))), r.wide, r.name + ' wide');
        assert.ok(Math.abs(r.std[0] / r.std[1] - 4 / 3) < 0.01, r.name + ': the 4:3 picture is 4:3');
        const [x, y, w, h] = r.part;
        assert.ok(Math.abs(w / h - 4 / 3) < 0.01, r.name + ': the part is 4:3');
        assert.ok(x >= 0 && y >= 0 && x + w <= r.wide[0] + 0.5 && y + h <= r.wide[1] + 0.5, r.name + ': the part is inside the picture');
        assert.ok(r.wide[0] / r.wide[1] > 2.2, r.name + ': the wide picture is wide');
    });
    const thumbs = cut(data, 'jkGuiHd_aThumbs[JKGUIHD_NUM_PICTURES][16*12*3] = {', '\n};');
    const rows = thumbs.split('{ // ').slice(1);
    assert.deepEqual(rows.map(r => r.split('\n')[0].trim()), NAMES);
    rows.forEach(r => assert.equal(r.split('\n').slice(1).join('').split(',').filter(v => v.trim() !== '' && v.trim() !== '}').length, 576));
});

test('the box is offered only with both lists, and the choice is kept with the game\'s settings', () => {
    const offered = new Function('Module', 'n', cut(hdc, 'EM_JS(int, jkGuiHd_wasmOffered, (int n), {', '\n});').split('{').slice(1).join('{'));
    assert.equal(offered({ jkHdMenus: { pictures: PICTURES } }, 11), 1);
    assert.equal(offered({ jkHdMenus: { pictures: { '43': PICTURES['43'] } } }, 11), 0, 'no wide set');
    assert.equal(offered({ jkHdMenus: { pictures: { '43': PICTURES['43'], wide: PICTURES.wide.slice(1) } } }, 11), 0, 'one short');
    assert.equal(offered({}, 11), 0, 'an older page');
    assert.match(html, /jkHdMenus: \{ pictures: HD_MENU_PICTURES \}/);
    assert.match(hdc, /wuRegistry_GetBool\("bHdMenus", 0\)/, 'off until ticked');
    assert.match(hdc, /wuRegistry_SaveBool\("bHdMenus", bEnabled\)/);
    assert.match(hdc, /!Main_bMotsCompat && !Main_bDroidWorks/, 'not in Mysteries of the Sith');
});

// ---- The textures (std3D_wasmHdTexture)

const hdTextureBody = cut(std3D, 'EM_JS(int, std3D_wasmHdTexture, (int picture, int bWide), {', '\n});').split('{').slice(1).join('{');
assert.ok(!hdTextureBody.includes('\\'), 'no backslashes in EM_JS (emjs-escapes-test)');

// A made-up browser: WebGL with Emscripten's texture table, fetch, decoding,
// each answered when the test says (settle), and a clock.
function browser({ failUrls = [] } = {}) {
    let now = 1000;
    const pending = [];
    const fetched = [], decoded = [], uploads = [], deleted = [], closed = [];
    let textures = 0;
    const gl = {
        TEXTURE_2D: 1, TEXTURE_BINDING_2D: 2, PIXEL_UNPACK_BUFFER_BINDING: 3, PIXEL_UNPACK_BUFFER: 4,
        TEXTURE_MIN_FILTER: 5, TEXTURE_MAG_FILTER: 6, LINEAR: 7, TEXTURE_WRAP_S: 8, TEXTURE_WRAP_T: 9, CLAMP_TO_EDGE: 10,
        RGBA: 11, UNSIGNED_BYTE: 12,
        lost: false, bound: 'was', pbo: null,
        isContextLost() { return this.lost; },
        createTexture() { return { t: ++textures }; },
        deleteTexture(t) { deleted.push(t.t); },
        getParameter(p) { return p === this.TEXTURE_BINDING_2D ? this.bound : this.pbo; },
        bindTexture(_, t) { this.bound = t; },
        bindBuffer(_, b) { this.pbo = b; },
        texParameteri() {},
        texImage2D(...a) { uploads.push(a[5].url); },
    };
    const GL = { textures: [null], getNewId(table) { table.push(null); return table.length - 1; } };
    const Module = { ctx: gl, jkHdMenus: { pictures: PICTURES }, jkGl: { rebuilds: 0 } };
    const later = v => new Promise((resolve, reject) => pending.push({ resolve, reject, v }));
    const fetch = url => {
        fetched.push(url);
        return later({ ok: !failUrls.includes(url), status: failUrls.includes(url) ? 404 : 200, blob: () => Promise.resolve({ url }) });
    };
    const createImageBitmap = blob => {
        decoded.push(blob.url);
        return later({ url: blob.url, close() { closed.push(blob.url); } });
    };
    const console = { warn() {} };
    const fn = new Function('Module', 'GL', 'fetch', 'createImageBitmap', 'performance', 'console', 'picture', 'bWide', hdTextureBody);
    const b = {
        Module, GL, gl, fetched, decoded, uploads, deleted, closed,
        ask: (picture, wide) => fn(Module, GL, fetch, createImageBitmap, { now: () => now }, console, picture, wide ? 1 : 0),
        tick: ms => { now += ms; },
        // Answers what's waiting, then lets the promises run
        async settle() {
            for (let i = 0; i < 6; i++) {
                for (const p of pending.splice(0)) p.resolve(p.v);
                await new Promise(r => setImmediate(r));
            }
        },
    };
    return b;
}

test('the picture asked for comes first, one file at a time, and only one texture is made a frame', async () => {
    const b = browser();
    assert.equal(b.ask(3, true), 0, 'nothing yet');
    assert.deepEqual(b.fetched, [PICTURES.wide[3]], 'the one asked for, alone');
    assert.equal(b.ask(3, true), 0);
    assert.equal(b.fetched.length, 1, 'no second file while the first is coming');
    await b.settle();
    const id = b.ask(3, true);
    assert.ok(id > 0, 'its texture, once it has come');
    assert.equal(b.GL.textures[id].t, 1);
    assert.deepEqual(b.uploads, [PICTURES.wide[3]]);
    assert.deepEqual(b.closed, [PICTURES.wide[3]], 'the decoded picture is let go once uploaded');
    assert.equal(b.gl.bound, 'was', 'the texture binding is put back');
    assert.deepEqual(b.fetched, [PICTURES.wide[3], PICTURES.wide[0]], 'then the rest of the set, in order');
    for (let i = 0; i < 30; i++) { await b.settle(); b.ask(3, true); }
    assert.equal(b.fetched.length, 11, 'the whole set, once each');
    assert.equal(b.uploads.length, 11);
    assert.equal(new Set(b.Module.jkHdTex.pics.map(p => p.id)).size, 11);
    assert.equal(b.ask(3, true), id, 'the same texture after');
});

test('another set (the window changed shape) or turning it off lets the textures go', async () => {
    const b = browser();
    b.ask(0, false);
    await b.settle();
    const id = b.ask(0, false);
    assert.ok(id > 0);
    assert.equal(b.ask(0, true), 0, 'the wide set starts over');
    assert.deepEqual(b.deleted, [b.gl.createTexture().t - 1]);
    assert.equal(b.GL.textures[id], null);
    assert.equal(b.fetched.at(-1), PICTURES.wide[0]);
    await b.settle();
    const wid = b.ask(0, true);
    assert.ok(wid > 0);
    b.Module.jkHdTex.free();
    assert.equal(b.GL.textures[wid], null, 'turned off');
    assert.deepEqual(b.Module.jkHdTex.pics, []);
});

test('lost graphics: the textures are made again from the files already fetched', async () => {
    const b = browser();
    b.ask(5, true);
    await b.settle();
    assert.ok(b.ask(5, true) > 0);
    b.Module.jkGl.rebuilds++;
    b.GL.textures.fill(null);  // as std3D_wasmGlPoll does
    b.Module.jkHdTex.free(true);  // as std3D_FreeResources does (std3D_wasmHdFree)
    assert.equal(b.ask(5, true), 0, 'gone with the old graphics');
    let id = 0;
    for (let i = 0; i < 3 && !id; i++) { await b.settle(); id = b.ask(5, true); }
    assert.ok(id > 0, 'made again once the file under way has come');
    assert.equal(b.fetched.filter(u => u === PICTURES.wide[5]).length, 1, 'without fetching it again');
    b.gl.lost = true;
    assert.equal(b.ask(5, true), 0, 'nothing while the graphics are away');
});

test("a file that doesn't come is asked for again later, 4 times at most, and the rest still come", async () => {
    const b = browser({ failUrls: [PICTURES['43'][2]] });
    b.ask(2, false);
    await b.settle();
    assert.equal(b.ask(2, false), 0);
    assert.equal(b.fetched.at(-1), PICTURES['43'][0], 'the next one meanwhile');
    for (let i = 0; i < 200; i++) { await b.settle(); b.ask(2, false); b.tick(5000); }
    assert.equal(b.fetched.filter(u => u === PICTURES['43'][2]).length, 4);
    assert.equal(b.uploads.length, 10);
    assert.equal(b.ask(2, false), 0, 'the game\'s own picture stays');
});

test('a page without the lists, or a picture past them, gets nothing and fetches nothing', () => {
    const b = browser();
    delete b.Module.jkHdMenus;
    assert.equal(b.ask(0, false), 0);
    b.Module.jkHdMenus = { pictures: PICTURES };
    assert.equal(b.ask(11, false), 0);
    assert.deepEqual(b.fetched, []);
});

let failed = 0;
for (const t of tests) {
    try {
        await t.fn();
        console.log('ok   ' + t.name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + t.name + '\n     ' + (e && e.stack || e));
    }
}
console.log(failed ? `${failed} of ${tests.length} failed` : `all ${tests.length} passed`);
process.exit(failed ? 1 : 0);
