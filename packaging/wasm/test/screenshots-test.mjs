// Tests for screenshots in packaging/wasm/index.html: their names, turning
// the engine's pixels the right way up, storing them, telling the engine how
// each one went, the zip of them all, and sending the ones from a recorded
// match to the lobby for its match pages. In Node (22 or newer):
// node packaging/wasm/test/screenshots-test.mjs
//
// The page's functions are taken from index.html as they are. Making the PNG
// needs a browser, so a stand-in makes the images here, and a made-up worker
// with made-up canvases stands in for the one that makes them off the page.

import { readFileSync } from 'node:fs';
import { crc32 } from 'node:zlib';
import assert from 'node:assert/strict';
import { makeIDB } from './helpers.mjs';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const code = cut('      var SHOT_GAMES', '      // ---- Your Screenshots on this page.')
    // A worker that never says it's ready is given up on sooner here.
    .replace('SHOT_WORKER_START_MS = 5000', 'SHOT_WORKER_START_MS = 50') +
    '\nreturn { shotLevel, shotName, shotPixels, shotStore, shotSaver, zipFiles, shotImagesInWorker, shotLimits, shotSharer, shotScreenPart, ' +
    'SHOT_FINAL_WAIT_MS, SHOT_FINAL_SPREAD_MS, SHOT_FINAL_RETRY_MS, SHOT_FINAL_TRIES, SHOT_FINAL_MS };';

// The page's code, with its clock set to the given times, one per new Date(),
// and a made-up Worker (see fakeWorker).
function page(times = [], Worker = undefined, URL = undefined) {
    const RealDate = Date;
    const queue = [...times];
    class FakeDate extends RealDate {
        constructor(...args) {
            if (args.length)
                super(...args);
            else
                super(queue.length ? queue.shift() : RealDate.now());
        }
    }
    return new Function('Date', 'Worker', 'URL', code)(FakeDate, Worker, URL);
}

// ---- A made-up worker: it runs the page's worker code on its own, with
// messages that arrive later and move what they transfer (as structuredClone
// does), and made-up canvases. Its "PNG" is the canvas's size and pixels, and
// its "JPEG" the small canvas's size (two bytes each) and the first byte of
// what was drawn on it.
class FakeImageData {
    constructor(data, width, height) { Object.assign(this, { data, width, height }); }
}
function fakeCanvas(opts) {
    return class FakeOffscreenCanvas {
        constructor(width, height) { Object.assign(this, { width, height, pixels: null, drawnFrom: null }); }
        getContext() {
            const canvas = this;
            return {
                putImageData(img) { canvas.pixels = img.data; },
                drawImage(from, x, y, w, h) { assert.deepEqual([w, h], [canvas.width, canvas.height]); canvas.drawnFrom = from; },
            };
        }
        async convertToBlob({ type, quality }) {
            if (opts.fail) throw new Error('out of memory');
            return type === 'image/png'
                ? new Blob([Uint8Array.from([0x89, this.width, this.height, ...this.pixels])])
                : (assert.equal(quality, 0.85), new Blob([Uint8Array.from([0xFF, this.width >> 8, this.width & 255,
                                                                          this.height >> 8, this.height & 255, this.drawnFrom.pixels[0]])]));
        }
    };
}
function fakeWorker(opts = {}) {
    const blobs = new Map(), made = [];
    const URL = { createObjectURL(blob) { const u = 'blob:' + blobs.size; blobs.set(u, blob); return u; } };
    class Worker {
        constructor(url) {
            if (opts.refused) throw new Error('blocked');
            made.push(this);
            this.terminated = false;
            const worker = this;
            const scope = {
                onmessage: null,
                OffscreenCanvas: opts.noCanvas ? undefined : fakeCanvas(opts),
                ImageData: FakeImageData,
                postMessage(m, transfer) {
                    const copy = structuredClone(m, { transfer });
                    setTimeout(() => { if (!worker.terminated && worker.onmessage) worker.onmessage({ data: copy }); });
                },
            };
            this.toWorker = m => scope.onmessage({ data: m });
            blobs.get(url).text().then(src => {
                if (opts.silent) return;
                new Function('scope', 'with (scope) {\n' + src + '\n}')(scope);
            });
        }
        postMessage(m, transfer) {
            const copy = structuredClone(m, { transfer });
            setTimeout(() => this.toWorker(copy));
        }
        terminate() { this.terminated = true; }
    }
    return { Worker, URL, made };
}

const at = (h, m, s) => new Date(2026, 9, 12, h, m, s);
const pixels = (w, h, fill) => new Uint8Array(w * h * 4).fill(fill);
const images = async (px, w, h) => ({ png: Uint8Array.from([0x89, 0x50, 0x4E, 0x47, px[0], w, h]), thumb: Uint8Array.from([0xFF, 0xD8, px[0]]) });
const settle = () => new Promise(r => setTimeout(r, 20));

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test('names say the game, when it was taken and the level', () => {
    const { shotName } = page();
    assert.equal(shotName('jk1', 'm10.jkl', at(2, 42, 16)), 'JK 2026-10-12 02-42-16 m10');
    assert.equal(shotName('mots', 'JKM_MP.JKL', at(14, 5, 9)), 'MotS 2026-10-12 14-05-09 JKM_MP');
    assert.equal(shotName('jk1', '', at(0, 0, 0)), 'JK 2026-10-12 00-00-00');
});

test('level names are safe in a file name', () => {
    const { shotLevel } = page();
    assert.equal(shotLevel('../evil name?.jkl'), 'evil_name_');
    assert.equal(shotLevel('jkl\\ctf1.jkl'), 'ctf1');
    assert.equal(shotLevel('x'.repeat(40) + '.jkl').length, 31);
});

test('the picture is turned the right way up and made opaque', () => {
    const { shotPixels } = page();
    // Two rows of two pixels, bottom row first, as glReadPixels gives them.
    const bottomUp = Uint8Array.from([1, 1, 1, 0, 2, 2, 2, 0, 3, 3, 3, 9, 4, 4, 4, 9]);
    assert.deepEqual([...shotPixels(bottomUp, 2, 2)],
                     [3, 3, 3, 255, 4, 4, 4, 255, 1, 1, 1, 255, 2, 2, 2, 255]);
});

test('screenshots are stored, newest first, and the engine hears how each went', async () => {
    const { shotStore, shotSaver } = page([at(2, 42, 16), at(2, 42, 16), at(2, 43, 0)]);
    const store = shotStore(makeIDB());
    const saver = shotSaver(store, images);
    assert.equal(saver.result(), 0);
    assert.equal(saver.save(pixels(4, 2, 7), 4, 2, 'jk1', 'm10.jkl'), true);
    saver.save(pixels(4, 2, 8), 4, 2, 'jk1', 'm10.jkl');
    saver.save(pixels(4, 2, 9), 4, 2, 'mots', 'jkm_mp.jkl');
    assert.equal(saver.result(), 0, 'not done yet');
    await settle();
    assert.deepEqual([saver.result(), saver.result(), saver.result(), saver.result()], [1, 1, 1, 0]);

    const list = await store.list();
    assert.deepEqual(list.map(m => m.key), ['MotS 2026-10-12 02-43-00 jkm_mp.png',
                                           'JK 2026-10-12 02-42-16 m10.png',
                                           'JK 2026-10-12 02-42-16 m10 (2).png']);
    const first = list.find(m => m.key === 'JK 2026-10-12 02-42-16 m10.png');
    assert.deepEqual({ ...first, thumb: [...first.thumb] }, {
        key: 'JK 2026-10-12 02-42-16 m10.png', game: 'jk1', level: 'm10', time: at(2, 42, 16).getTime(),
        width: 4, height: 2, size: 7, thumb: [0xFF, 0xD8, 7] });
    assert.deepEqual([...await store.read('JK 2026-10-12 02-42-16 m10 (2).png')], [0x89, 0x50, 0x4E, 0x47, 8, 4, 2]);
    assert.equal(list[0].game, 'mots');
    assert.equal(list[0].level, 'jkm_mp');
});

test('a screenshot that can\'t be made or stored is reported, and later ones still save', async () => {
    const { shotStore, shotSaver } = page();
    const store = shotStore(makeIDB());
    let fail = true;
    const saver = shotSaver(store, async (px, w, h) => {
        if (fail) throw new Error('no PNG');
        return images(px, w, h);
    });
    saver.save(pixels(2, 2, 1), 2, 2, 'jk1', 'm2.jkl');
    await settle();
    assert.equal(saver.result(), 2);
    assert.equal((await store.list()).length, 0);
    fail = false;
    saver.save(pixels(2, 2, 1), 2, 2, 'jk1', 'm2.jkl');
    await settle();
    assert.equal(saver.result(), 1);
    assert.equal((await store.list()).length, 1);

    const full = shotSaver({ keys: async () => [], add: async () => { throw new Error('quota'); } }, images);
    full.save(pixels(2, 2, 1), 2, 2, 'jk1', 'm2.jkl');
    await settle();
    assert.equal(full.result(), 2);
});

test('screenshots can be deleted one at a time or all at once', async () => {
    const { shotStore, shotSaver } = page([at(1, 0, 0), at(1, 0, 1), at(1, 0, 2)]);
    const store = shotStore(makeIDB());
    const saver = shotSaver(store, images);
    for (let i = 0; i < 3; i++)
        saver.save(pixels(2, 2, i), 2, 2, 'jk1', 'm2.jkl');
    await settle();
    await store.remove(['JK 2026-10-12 01-00-01 m2.png']);
    assert.deepEqual((await store.list()).map(m => m.key), ['JK 2026-10-12 01-00-02 m2.png', 'JK 2026-10-12 01-00-00 m2.png']);
    assert.equal(await store.read('JK 2026-10-12 01-00-01 m2.png'), undefined);
    await store.remove((await store.list()).map(m => m.key));
    assert.deepEqual(await store.list(), []);
});

test('names are picked from the stored names alone, not every small picture', async () => {
    const { shotStore, shotSaver } = page([at(3, 0, 0), at(3, 0, 0)]);
    const store = shotStore(makeIDB());
    const saver = shotSaver({ ...store, list: () => { throw new Error('read every picture'); } }, images);
    saver.save(pixels(2, 2, 1), 2, 2, 'jk1', 'm2.jkl');
    saver.save(pixels(2, 2, 2), 2, 2, 'jk1', 'm2.jkl');
    await settle();
    assert.deepEqual([saver.result(), saver.result()], [1, 1]);
    assert.deepEqual((await store.list()).map(m => m.key).sort(), ['JK 2026-10-12 03-00-00 m2 (2).png', 'JK 2026-10-12 03-00-00 m2.png']);
});

// The engine's copy of a frame: two rows of two pixels, bottom row first,
// in a buffer of its own (HEAPU8.slice).
const frame = () => Uint8Array.from([1, 1, 1, 0, 2, 2, 2, 0, 3, 3, 3, 9, 4, 4, 4, 9]);
const UPRIGHT = [3, 3, 3, 255, 4, 4, 4, 255, 1, 1, 1, 255, 2, 2, 2, 255];

test('a worker makes the PNG and the small JPEG, and the pixels move there without a copy', async () => {
    const w = fakeWorker();
    const { shotStore, shotSaver, shotImagesInWorker } = page([at(4, 0, 0), at(4, 0, 1)], w.Worker, w.URL);
    const store = shotStore(makeIDB());
    let onPage = 0;
    const saver = shotSaver(store, shotImagesInWorker(async (...a) => { onPage++; return images(...a); }));
    const px = frame();
    saver.save(px, 2, 2, 'jk1', 'm10.jkl');
    await settle();
    assert.equal(saver.result(), 1);
    assert.equal(px.byteLength, 0, 'moved to the worker');
    const shot = await store.read('JK 2026-10-12 04-00-00 m10.png');
    assert.deepEqual([...shot], [0x89, 2, 2, ...UPRIGHT], 'turned the right way up and made opaque');
    const meta = (await store.list())[0];
    assert.deepEqual([...meta.thumb], [0xFF, 0, 2, 0, 2, 3]);
    assert.equal(meta.size, shot.length);
    // The next one uses the same worker.
    saver.save(frame(), 2, 2, 'jk1', 'm10.jkl');
    await settle();
    assert.equal(saver.result(), 1);
    assert.equal(w.made.length, 1);
    assert.equal(onPage, 0);
});

test('a big picture gets a small copy 320 pixels wide', async () => {
    const w = fakeWorker();
    const { shotImagesInWorker } = page([], w.Worker, w.URL);
    const make = shotImagesInWorker(() => { throw new Error('made on the page'); });
    const { png, thumb } = await make(new Uint8Array(1920 * 1080 * 4), 1920, 1080);
    assert.equal(png.length, 3 + 1920 * 1080 * 4);
    assert.deepEqual([thumb[1] << 8 | thumb[2], thumb[3] << 8 | thumb[4]], [320, 180]);
});

test('where a worker has no OffscreenCanvas, can\'t start, or never answers, the page makes them', async () => {
    for (const opts of [{ noCanvas: true }, { refused: true }, { silent: true }]) {
        const w = fakeWorker(opts);
        const { shotStore, shotSaver, shotImagesInWorker } = page([], w.Worker, w.URL);
        const store = shotStore(makeIDB());
        const seen = [];
        const saver = shotSaver(store, shotImagesInWorker(async (px, ww, hh) => { seen.push([...px]); return images(px, ww, hh); }));
        saver.save(frame(), 2, 2, 'jk1', 'm10.jkl');
        await new Promise(r => setTimeout(r, 120));
        assert.equal(saver.result(), 1, JSON.stringify(opts));
        assert.deepEqual(seen, [[...frame()]], 'the page got the pixels as they were');
        assert.ok(w.made.every(x => x.terminated), 'the worker is let go');
    }
});

test('a worker that fails at one gives its pixels back, and the page makes it and those after', async () => {
    const w = fakeWorker({ fail: true });
    const { shotStore, shotSaver, shotImagesInWorker } = page([], w.Worker, w.URL);
    const store = shotStore(makeIDB());
    const seen = [];
    const saver = shotSaver(store, shotImagesInWorker(async (px, ww, hh) => { seen.push([...px]); return images(px, ww, hh); }));
    saver.save(frame(), 2, 2, 'jk1', 'm10.jkl');
    saver.save(frame(), 2, 2, 'jk1', 'm10.jkl');
    await settle();
    assert.deepEqual([saver.result(), saver.result()], [1, 1]);
    assert.deepEqual(seen, [[...frame()], [...frame()]]);
    assert.equal(w.made.length, 1);
    assert.equal(w.made[0].terminated, true);
});

// Reads a zip the way unzip tools do: from its central directory.
function readZip(zip) {
    const v = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
    const end = zip.length - 22;
    assert.equal(v.getUint32(end, true), 0x06054b50);
    const count = v.getUint16(end + 10, true);
    let p = v.getUint32(end + 16, true);
    assert.equal(p + v.getUint32(end + 12, true), end);
    const out = [];
    for (let i = 0; i < count; i++) {
        assert.equal(v.getUint32(p, true), 0x02014b50);
        const method = v.getUint16(p + 10, true), time = v.getUint16(p + 12, true), day = v.getUint16(p + 14, true);
        const crc = v.getUint32(p + 16, true), size = v.getUint32(p + 20, true);
        assert.equal(v.getUint32(p + 24, true), size);
        const nameLen = v.getUint16(p + 28, true), local = v.getUint32(p + 42, true);
        const name = Buffer.from(zip.subarray(p + 46, p + 46 + nameLen)).toString('utf8');
        assert.equal(v.getUint32(local, true), 0x04034b50);
        assert.equal(v.getUint32(local + 14, true), crc);
        const start = local + 30 + v.getUint16(local + 26, true) + v.getUint16(local + 28, true);
        const data = zip.subarray(start, start + size);
        assert.equal(crc32(data), crc, 'CRC of ' + name);
        out.push({ name, method, data: [...data],
                   date: new Date(1980 + (day >> 9), ((day >> 5) & 15) - 1, day & 31, time >> 11, (time >> 5) & 63, (time & 31) * 2) });
        p += 46 + nameLen;
    }
    return out;
}

test('the zip holds every screenshot as it is, with its name and time', () => {
    const { zipFiles } = page();
    const files = [
        { name: 'JK 2026-10-12 02-42-16 m10.png', data: Uint8Array.from([1, 2, 3, 4, 5]), date: at(2, 42, 16) },
        { name: 'MotS 2026-10-12 14-05-08 Ünïcode.png', data: new Uint8Array(70000).map((_, i) => i * 7), date: at(14, 5, 8) },
    ];
    const entries = readZip(zipFiles(files));
    assert.deepEqual(entries.map(e => e.name), files.map(f => f.name));
    assert.deepEqual(entries.map(e => e.method), [0, 0]);
    entries.forEach((e, i) => {
        assert.deepEqual(e.data, [...files[i].data]);
        assert.equal(e.date.getTime(), files[i].date.getTime());
    });
});

// ---- Sending them to the lobby for its match pages.

const LIMITS = { perPlayer: 12, perMatch: 60, bytes: 1500000, thumbBytes: 200000, edge: 1920, thumbEdge: 480, days: 7 };
const DAY = 24 * 60 * 60 * 1000;

// A sharer over a store with the given screenshots ([key, minutes ago,
// share]), a made-up lobby (lobby(method, url, body) gives [status, answer]
// or throws), a clock and timers moved by hand.
async function sharing(P, shotsIn, lobby, opts = {}) {
    const store = P.shotStore(makeIDB());
    const w = { now: 1_800_000_000_000, allowed: true, level: false, calls: [], changed: 0, warned: [], timers: [], random: 0.5 };
    for (const [key, minutesAgo, share, more] of shotsIn) {
        await store.add({ key, game: 'jk1', level: 'm10', time: w.now - minutesAgo * 60000, width: 1920, height: 1080, size: 9,
                          thumb: Uint8Array.of(1), ...(share ? { share } : {}), ...(more || {}) }, Uint8Array.of(0x89, minutesAgo));
    }
    const sharer = P.shotSharer({
        store,
        fetch: async (url, init = {}) => {
            const method = init.method || 'GET';
            w.calls.push(method + ' ' + url + (init.body ? ' ' + JSON.stringify(init.body) : ''));
            assert.deepEqual([init.cache, init.credentials], ['no-store', 'same-origin'], 'never from a cache, with the cookie');
            const [status, answer] = await lobby(method, url, init.body);
            return { status, json: async () => { if (answer === undefined) throw new Error('not JSON'); return answer; } };
        },
        now: () => w.now,
        allowed: () => w.allowed,
        inLevel: () => w.level,
        encode: async (png, width, height, limits) => {
            if (opts.encodeFails) throw new Error('no canvas');
            return { full: 'full of ' + [...png], thumb: 'thumb', for: [width, height, limits.edge] };
        },
        form: images => ({ form: images.full }),
        changed: () => { w.changed++; },
        warn: text => w.warned.push(text),
        setTimeout: (fn, ms) => { const t = { fn, at: w.now + ms }; w.timers.push(t); return t; },
        clearTimeout: t => { w.timers = w.timers.filter(x => x !== t); },
        random: () => w.random,
    });
    // Moves the clock on, running the timers that come due.
    w.pass = async ms => {
        const until = w.now + ms;
        for (;;) {
            const next = w.timers.filter(t => t.at <= until).sort((a, b) => a.at - b.at)[0];
            if (!next) break;
            w.timers = w.timers.filter(t => t !== next);
            w.now = next.at;
            await next.fn();
        }
        w.now = until;
    };
    w.shares = async () => Object.fromEntries((await store.list()).map(m => [m.key, m.shared ? [m.share, m.shared.id, m.shared.match] : m.share]));
    w.take = () => w.calls.splice(0);
    return { store, sharer, w };
}
const ID = n => String(n).repeat(24);

test('a screenshot\'s record can be changed in part, or only while it waits', async () => {
    const { shotStore } = page();
    const store = shotStore(makeIDB());
    await store.add({ key: 'a.png', time: 5, size: 1, share: 'wait' }, Uint8Array.of(1));
    assert.deepEqual(await store.patch('a.png', { share: 'on', shared: { id: 'x', match: 3 } }, 'wait'),
                     { key: 'a.png', time: 5, size: 1, share: 'on', shared: { id: 'x', match: 3 } });
    assert.equal(await store.patch('a.png', { share: 'no' }, 'wait'), null, 'not waiting any more');
    assert.deepEqual(await store.patch('a.png', { share: 'off', shared: undefined }), { key: 'a.png', time: 5, size: 1, share: 'off' });
    assert.deepEqual(await store.list(), [{ key: 'a.png', time: 5, size: 1, share: 'off' }]);
    assert.equal(await store.patch('gone.png', { share: 'on' }), null);
    assert.deepEqual([...await store.read('a.png')], [1], 'the picture itself is left alone');
});

test('a picture taken in a reported level is marked to go, and the engine hears which ones do', async () => {
    const { shotStore, shotSaver } = page([at(4, 0, 0), at(4, 0, 1), at(4, 0, 2), at(4, 0, 3)]);
    const store = shotStore(makeIDB());
    const marks = [{ rec: 1, share: 'wait', tell: true }, { rec: 1, share: 'wait', tell: false }, { rec: 1, share: '', tell: false }, null];
    let added = 0;
    const finals = [];
    let wants = true;
    const saver = shotSaver(store, images, { mark: () => marks.shift(), added: () => { added++; }, wantsFinal: () => wants,
                                             final: (...a) => finals.push(a) });
    for (let i = 0; i < 4; i++)
        saver.save(pixels(2, 2, i), 2, 2, 'jk1', 'm2.jkl');
    assert.equal(marks.length, 0, 'asked as each is taken, not when it is stored');
    await settle();
    // To the engine every one is saved; it asks after each whether that one goes.
    const told = [];
    for (let i = 0; i < 4; i++)
        told.push([saver.result(), saver.shared()]);
    assert.deepEqual(told, [[1, 1], [1, 0], [1, 0], [1, 0]]);
    assert.deepEqual([saver.result(), saver.shared()], [0, 0]);
    assert.equal(added, 2);
    const list = (await store.list()).reverse();
    assert.deepEqual(list.map(m => [m.rec, m.share]), [[1, 'wait'], [1, 'wait'], [1, undefined], [undefined, undefined]]);
    assert.equal('tell' in list[0], false);
    // The final scoreboard is handed on, never stored.
    assert.equal(saver.wantsFinal(), true);
    assert.equal(saver.saveFinal(pixels(2, 2, 9), 2, 2), true);
    assert.deepEqual(finals.map(f => [f[0][0], f[1], f[2]]), [[9, 2, 2]]);
    wants = false;
    assert.deepEqual([saver.wantsFinal(), saver.saveFinal(pixels(2, 2, 9), 2, 2)], [false, false]);
    assert.equal(finals.length, 1);
    assert.equal((await store.list()).length, 4);
    // A page that shares nothing has neither.
    const plain = shotSaver(store, images);
    assert.deepEqual([plain.wantsFinal(), plain.saveFinal(pixels(2, 2, 9), 2, 2), plain.shared()], [false, false, 0]);
});

test("the lobby's limits are taken only as numbers that make sense", () => {
    const { shotLimits } = page();
    assert.deepEqual(shotLimits({ ...LIMITS, extra: 'x' }), { perPlayer: 12, bytes: 1500000, thumbBytes: 200000, edge: 1920, thumbEdge: 480, days: 7 });
    for (const bad of [undefined, null, 'x', 7, {}, { ...LIMITS, bytes: '1500000' }, { ...LIMITS, edge: 0 }, { ...LIMITS, days: -1 },
                       { ...LIMITS, thumbEdge: NaN }, { ...LIMITS, bytes: 1e12 }, { ...LIMITS, perPlayer: Infinity }])
        assert.equal(shotLimits(bad), null, JSON.stringify(bad));
});

test("only the score screen goes of the final scoreboard, not the bars a wide window has beside it", () => {
    const { shotScreenPart } = page();
    assert.deepEqual(shotScreenPart(1920, 1080), [240, 1440]);
    assert.deepEqual(shotScreenPart(640, 480), [0, 640]);
    assert.deepEqual(shotScreenPart(3440, 1440), [760, 1920]);
    assert.deepEqual(shotScreenPart(1001, 750), [0, 1000]);
    // A window narrower than 4:3 shows less of it: what it shows.
    assert.deepEqual(shotScreenPart(800, 800), [0, 800]);
});

test('a waiting screenshot is asked about, sent once its match is saved, and marked with where it went', async () => {
    const P = page();
    let saved = false;
    const { sharer, w } = await sharing(P, [['new.png', 2, 'wait'], ['old.png', 30, 'wait'], ['mine.png', 5, ''], ['full.png', 1, 'wait']],
        (method, url) => {
            const taken = Number(/taken=(\d+)/.exec(url)[1]);
            if (taken === 1_800_000_000_000 - 60000) return [409, { error: 'full' }];
            if (!saved) return [404, { error: 'none', wait: true }];
            return method === 'GET' ? [200, { match: 12, secs: 40, left: 3 }] : [200, { id: ID(taken % 7), match: 12, secs: 40, left: 2 }];
        });
    // Nothing without the lobby's limits: it takes no pictures.
    await sharer.run();
    assert.deepEqual(w.take(), []);
    assert.equal(sharer.limits(P.shotLimits(LIMITS)).edge, 1920);
    await sharer.run();
    // Oldest first, each asked with how long ago it was taken and its own stamp; the one not marked is left out.
    assert.deepEqual(w.take(), [
        `GET /ladder/shot?age=${30 * 60000}&taken=${w.now - 30 * 60000}`,
        `GET /ladder/shot?age=${2 * 60000}&taken=${w.now - 2 * 60000}`,
        `GET /ladder/shot?age=60000&taken=${w.now - 60000}`,
    ]);
    assert.deepEqual(await w.shares(), { 'full.png': 'no', 'new.png': 'wait', 'mine.png': undefined, 'old.png': 'wait' });
    assert.equal(w.changed, 1);
    // The game hears its match was saved.
    saved = true;
    w.now += 5000;
    sharer.saved();
    await settle();
    const calls = w.take();
    assert.deepEqual(calls, [
        `GET /ladder/shot?age=${30 * 60000 + 5000}&taken=${w.now - 5000 - 30 * 60000}`,
        `POST /ladder/shot?age=${30 * 60000 + 5000}&taken=${w.now - 5000 - 30 * 60000} {"form":"full of 137,30"}`,
        `GET /ladder/shot?age=${2 * 60000 + 5000}&taken=${w.now - 5000 - 2 * 60000}`,
        `POST /ladder/shot?age=${2 * 60000 + 5000}&taken=${w.now - 5000 - 2 * 60000} {"form":"full of 137,2"}`,
    ]);
    const shares = await w.shares();
    assert.deepEqual([shares['old.png'][0], shares['old.png'][2], shares['new.png'][0], shares['mine.png']], ['on', 12, 'on', undefined]);
    assert.match(shares['old.png'][1], /^[0-9a-f]{24}$/);
    // Nothing is left to ask about.
    await sharer.run();
    assert.deepEqual(w.take(), []);
});

test('nothing goes for a player who doesn\'t share, during a level, or once the lobby can\'t be reached', async () => {
    const P = page();
    let answer = () => [200, { match: 3, secs: 1, left: 1 }];
    const { sharer, w } = await sharing(P, [['a.png', 3, 'wait'], ['b.png', 2, 'wait'], ['c.png', 1, 'wait']],
        (method, url) => method === 'GET' ? answer(url) : [200, { id: ID(4), match: 3, secs: 1, left: 0 }]);
    sharer.limits(P.shotLimits(LIMITS));
    w.allowed = false;
    await sharer.run();
    assert.deepEqual(w.take(), [], 'the box is unticked, or the player was never shown it');
    w.allowed = true;
    // In a level nothing is sent: it waits for the level to end.
    w.level = true;
    await sharer.run();
    sharer.tick();
    await settle();
    assert.deepEqual(w.take(), []);
    w.level = false;
    // A level that begins while they go: the one on its way finishes, the rest wait.
    answer = () => { w.level = true; return [200, { match: 3, secs: 1, left: 1 }]; };
    sharer.tick();
    await settle();
    assert.equal(w.take().length, 2, 'asked and sent, one picture');
    assert.deepEqual(Object.values(await w.shares()).map(v => Array.isArray(v) ? v[0] : v).sort(), ['on', 'wait', 'wait']);
    sharer.tick();
    await settle();
    assert.deepEqual(w.take(), []);
    w.level = false;
    // No key, the lobby away, with no room left for pictures, or answering something else: nothing more is asked this time, and all
    // of it another time.
    for (const bad of [() => [401, { error: 'key' }], () => [503, { error: 'busy' }], () => [507, { error: 'room' }], () => [404, undefined],
                       () => [200, { match: 'x' }],
                       () => { throw new Error('offline'); }]) {
        answer = bad;
        await sharer.run();
        assert.equal(w.take().length, 1);
    }
    assert.deepEqual(Object.values(await w.shares()).map(v => Array.isArray(v) ? v[0] : v).sort(), ['on', 'wait', 'wait']);
    // The lobby has one already (sent before, the answer lost): marked without sending it again.
    answer = () => [200, { match: 3, secs: 1, left: 1, id: ID(9) }];
    await sharer.run();
    assert.deepEqual(w.take().map(c => c.split(' ')[0]), ['GET', 'GET']);
    assert.deepEqual(Object.values(await w.shares()).map(v => v[1]).sort(), [ID(4), ID(9), ID(9)]);
});

test('what the lobby has no place for stops waiting, and so does one too old to ask about', async () => {
    const P = page();
    const answers = { 1: [404, { error: 'none', wait: false }], 2: [409, { error: 'have' }], 3: [403, { error: 'hidden' }],
                      4: [413, { error: 'big' }], 5: [415, { error: 'jpeg' }], 6: [404, { error: 'none', wait: true }] };
    const { sharer, w } = await sharing(P, [1, 2, 3, 6].map(n => [n + '.png', n, 'wait']).concat([['week.png', 8 * 24 * 60, 'wait'],
                                                                                              ['4.png', 4, 'wait'], ['5.png', 5, 'wait']]),
        (method, url) => {
            const n = Number(/age=(\d+)/.exec(url)[1]) / 60000;
            return method === 'POST' || n < 4 || n === 6 ? answers[n] : [200, { match: 2, secs: 1, left: 1 }];
        });
    sharer.limits(P.shotLimits(LIMITS));
    await sharer.run();
    assert.deepEqual(await w.shares(), { '1.png': 'no', '2.png': 'no', '3.png': 'no', '4.png': 'no', '5.png': 'no', '6.png': 'wait',
                                         'week.png': 'no' });
    assert.equal(w.take().some(c => c.includes('age=' + 8 * DAY)), false, 'the old one was never asked about');
    // One the page can't make into a JPEG isn't tried again either.
    const broken = await sharing(P, [['a.png', 1, 'wait']], () => [200, { match: 2, secs: 1, left: 1 }], { encodeFails: true });
    broken.sharer.limits(P.shotLimits(LIMITS));
    await broken.sharer.run();
    assert.deepEqual(await broken.w.shares(), { 'a.png': 'no' });
    assert.equal(broken.w.warned.length, 1);
    assert.deepEqual(broken.w.take().map(c => c.split(' ')[0]), ['GET']);
});

test('a player takes one off its match\'s page, keeps one back before it goes, or sends one by hand', async () => {
    const P = page();
    let status = 200, keep = null;
    const lobby = async (method, url) => {
        if (method === 'DELETE') return [status, status === 200 ? { gone: url.split('/').pop() } : { error: 'none' }];
        if (method === 'GET') return [200, { match: 8, secs: 1, left: 5 }];
        if (keep) await keep();
        return [200, { id: ID(5), match: 8, secs: 1, left: 4 }];
    };
    const { store, sharer, w } = await sharing(P, [['up.png', 9, 'on', { shared: { id: ID(2), match: 7 }, rec: 1 }], ['wait.png', 3, 'wait', { rec: 1 }],
                                                   ['kept.png', 2, '', { rec: 1 }], ['slow.png', 1, 'wait', { rec: 1 }]], lobby);
    sharer.limits(P.shotLimits(LIMITS));
    const meta = async key => (await store.list()).find(m => m.key === key);
    // The lobby away: it stays as it is.
    status = 503;
    assert.equal(await sharer.takeOff(await meta('up.png')), false);
    assert.deepEqual((await w.shares())['up.png'], ['on', ID(2), 7]);
    status = 200;
    assert.equal(await sharer.takeOff(await meta('up.png')), true);
    assert.deepEqual(w.take(), ['DELETE /ladder/shot/' + ID(2), 'DELETE /ladder/shot/' + ID(2)]);
    assert.deepEqual(await meta('up.png').then(m => [m.share, 'shared' in m]), ['off', false]);
    // Gone from the lobby already (taken off from the match's own page) is off too.
    await store.patch('up.png', { share: 'on', shared: { id: ID(3), match: 7 } });
    status = 404;
    assert.equal(await sharer.takeOff(await meta('up.png')), true);
    assert.equal((await w.shares())['up.png'], 'off');
    w.take();
    // Kept back before it went: never asked about.
    await sharer.keepBack(await meta('wait.png'));
    assert.equal((await w.shares())['wait.png'], 'off');
    // Kept back while it was on its way: taken off again as soon as it's there.
    status = 200;
    keep = () => { keep = null; return sharer.keepBack({ key: 'slow.png' }); };
    await sharer.run();
    await settle();
    assert.deepEqual(w.take().map(c => c.split('?')[0]), ['GET /ladder/shot', 'POST /ladder/shot', 'DELETE /ladder/shot/' + ID(5)]);
    assert.equal((await w.shares())['slow.png'], 'off');
    // Kept back a moment after it got there (the list still showed it waiting): taken off as well.
    await store.patch('slow.png', { share: 'on', shared: { id: ID(4), match: 8 } });
    assert.equal(await sharer.keepBack({ key: 'slow.png', share: 'wait' }), true);
    assert.deepEqual(w.take(), ['DELETE /ladder/shot/' + ID(4)]);
    assert.equal((await w.shares())['slow.png'], 'off');
    // One from a recorded match that wasn't marked goes when its player says so.
    await sharer.putUp(await meta('kept.png'));
    await settle();
    assert.deepEqual(w.take().map(c => c.split('?')[0]), ['GET /ladder/shot', 'POST /ladder/shot']);
    assert.deepEqual((await w.shares())['kept.png'], ['on', ID(5), 8]);
    // Unticking the box keeps everything that waits.
    await store.patch('up.png', { share: 'wait' });
    await store.patch('wait.png', { share: 'wait' });
    await sharer.keepAllBack();
    assert.deepEqual(await w.shares(), { 'up.png': 'off', 'wait.png': 'off', 'slow.png': 'off', 'kept.png': ['on', ID(5), 8] });
});

test("the final scoreboard goes from the host at once and from the others a little later, and is given up on in time", async () => {
    const P = page();
    let answers = [];
    const lobby = (method) => method === 'GET' ? answers.shift() : [200, { id: ID(6), match: 4, secs: 300, left: 1 }];
    const { sharer, w } = await sharing(P, [], lobby);
    sharer.limits(P.shotLimits(LIMITS));
    const taken = w.now;
    // The host's: asked at once. Its match isn't saved yet, so again in a while, and at once when the game hears it is.
    answers = [[404, { error: 'none', wait: true }], [404, { error: 'none', wait: true }], [200, { match: 4, secs: 300, left: 1 }]];
    sharer.takeFinal({ full: 'final', thumb: 't' }, taken, true);
    assert.equal(sharer.pendingFinal(), true);
    await w.pass(0);
    assert.deepEqual(w.take(), [`GET /ladder/shot?age=0&taken=${taken}&final=1`]);
    await w.pass(P.SHOT_FINAL_RETRY_MS);
    assert.deepEqual(w.take(), [`GET /ladder/shot?age=${P.SHOT_FINAL_RETRY_MS}&taken=${taken}&final=1`]);
    w.now += 1000;
    sharer.saved();
    await w.pass(0);
    assert.deepEqual(w.take(), [`GET /ladder/shot?age=${P.SHOT_FINAL_RETRY_MS + 1000}&taken=${taken}&final=1`,
                                `POST /ladder/shot?age=${P.SHOT_FINAL_RETRY_MS + 1000}&taken=${taken}&final=1 {"form":"final"}`]);
    assert.equal(sharer.pendingFinal(), false);
    await w.pass(60000);
    assert.deepEqual(w.take(), [], 'once');

    // Another game's: not before the host's has had its chance, and the lobby has one by then.
    answers = [[409, { error: 'have' }]];
    w.random = 0.5;
    sharer.takeFinal({ full: 'final', thumb: 't' }, w.now, false);
    await w.pass(P.SHOT_FINAL_WAIT_MS + P.SHOT_FINAL_SPREAD_MS / 2 - 1);
    assert.deepEqual(w.take(), []);
    await w.pass(1);
    assert.equal(w.take().length, 1);
    assert.equal(sharer.pendingFinal(), false);
    // Refused for good (a player shown as Anonymous in it, a level its host didn't end): dropped.
    for (const no of [[409, { error: 'anonymous' }], [404, { error: 'none', wait: false }]]) {
        answers = [no];
        sharer.takeFinal({ full: 'final', thumb: 't' }, w.now, true);
        await w.pass(0);
        assert.equal(sharer.pendingFinal(), false, JSON.stringify(no));
        w.take();
    }
    // A match that's never saved: so many tries, then no more.
    answers = Array.from({ length: 20 }, () => [404, { error: 'none', wait: true }]);
    sharer.takeFinal({ full: 'final', thumb: 't' }, w.now, true);
    await w.pass(P.SHOT_FINAL_RETRY_MS * (P.SHOT_FINAL_TRIES + 2));
    assert.equal(w.take().length, P.SHOT_FINAL_TRIES);
    assert.equal(sharer.pendingFinal(), false);
    // A player who stopped sharing meanwhile sends none, and neither does one kept too long.
    sharer.takeFinal({ full: 'final', thumb: 't' }, w.now, false);
    w.allowed = false;
    await w.pass(P.SHOT_FINAL_WAIT_MS + P.SHOT_FINAL_SPREAD_MS);
    assert.deepEqual([w.take().length, sharer.pendingFinal()], [0, false]);
    w.allowed = true;
    answers = [[404, { error: 'none', wait: true }]];
    sharer.takeFinal({ full: 'final', thumb: 't' }, w.now - P.SHOT_FINAL_MS - 1, true);
    await w.pass(0);
    assert.deepEqual([w.take().length, sharer.pendingFinal()], [0, false]);
    // A newer one takes the place of one still waiting.
    answers = [[200, { match: 5, secs: 9, left: 1 }]];
    sharer.takeFinal({ full: 'first', thumb: 't' }, w.now, false);
    sharer.takeFinal({ full: 'second', thumb: 't' }, w.now, true);
    await w.pass(P.SHOT_FINAL_WAIT_MS + P.SHOT_FINAL_SPREAD_MS);
    assert.deepEqual(w.take().map(c => c.split(' ').pop()), [`/ladder/shot?age=0&taken=${w.now - P.SHOT_FINAL_WAIT_MS - P.SHOT_FINAL_SPREAD_MS}&final=1`, '{"form":"second"}']);
});

test('the page has what the sharing fills in, and the box is in the prompt a game starts from', () => {
    const glue = cut('      // ---- Sharing them (shotSharer above).', '      function sizeText(bytes)');
    for (const mark of [...glue.matchAll(/querySelector(?:All)?\('(?:#picker )?\[(data-[a-z-]+)\]'\)/g)].map(m => m[1]))
        assert.ok(new RegExp(`<[a-z]+[^>]*\\s${mark}(?=[\\s>=])`).test(html), 'the page has [' + mark + ']');
    // Hidden until the lobby says it takes pictures, in both places.
    const labels = [...html.matchAll(/<label class="[^"]*" data-shots hidden><input type="checkbox" data-shots-share checked>/g)];
    assert.equal(labels.length, 2);
    const picker = html.substring(html.indexOf('<dialog id="picker"'), html.indexOf('</dialog>', html.indexOf('<dialog id="picker"')));
    assert.ok(picker.includes('data-shots-share'), 'in the prompt');
    assert.ok(html.substring(html.indexOf('<section id="screenshots"')).includes('data-shots-share'), 'and under Your Screenshots');
    // Nothing a lobby or a player typed goes into the page as markup.
    assert.ok(!/innerHTML|outerHTML|insertAdjacentHTML|document\.write/.test(cut('      var SHOT_GAMES', '      showShots();\n    </script>')));
    // Told only by a game started from a prompt that showed the box, or by changing it.
    assert.match(html, /hideZone = function\(\) \{\s+matchesGameStart\(\);[^}]*?shotGameStart\(\);/);
    assert.match(glue, /function shotShareAllowed\(\) \{\s+return shotSharing\.on && shotSharing\.told && !Module\.jkLadderAnon &&/);
});

test("the game asks the page before it reads the final scoreboard back, and only of a score screen's own frame", () => {
    const src = name => readFileSync(new URL('../../../src/' + name, import.meta.url), 'utf8');
    const tally = src('Gui/jkGUIMultiTally.c'), win = src('Win95/Window.c'), game = src('Main/jkGame.c');
    // What the engine calls is what the page has.
    for (const call of ['Module.jkScreenshots.wantsFinal()', 'Module.jkScreenshots.saveFinal('])
        assert.ok(tally.includes(call), 'jkGUIMultiTally.c has ' + call);
    assert.ok(game.includes('Module.jkScreenshots.shared()'), 'jkGame.c asks whether a screenshot is shared');
    const { shotSaver } = page();
    const saver = shotSaver({}, async () => ({}), {});
    for (const name of ['wantsFinal', 'saveFinal', 'shared'])
        assert.equal(typeof saver[name], 'function');
    // Read between the menu's frame being drawn and the browser showing it.
    assert.match(win, /std3D_DrawMenu\(\);\n#ifdef ARCH_WASM\n\s+jkGuiMultiTally_MenuFrameDrawn\(\);[^\n]*\n#endif\n\s+std3D_EndScene\(\);/);
    // One frame after a round of a score screen's own menu, so never a dialog's; the page is asked before the pixels are read.
    const drawn = tally.substring(tally.indexOf('void jkGuiMultiTally_MenuFrameDrawn(void)'), tally.indexOf('static jkGuiElement jkGuiMultiTally_buttons['));
    const order = ['jkGuiMultiTally_bPictureDue = 0;', 'std3D_WasmFrameDrawn()', 'jkGuiMultiTally_pictureRounds = -1;',
                   'jkGuiMultiTally_js_wantsPicture()', 'std3D_ReadWindowRGBA(', 'jkGuiMultiTally_js_takePicture('].map(t => drawn.indexOf(t));
    assert.ok(order.every((at, i) => at > 0 && (!i || at > order[i - 1])), 'in that order: ' + order);
    assert.match(tally, /void jkGuiMultiTally_sub_4188B0\(jkGuiMenu \*pMenu\)\n\{[^}]*?jkGuiMultiTally_PictureRound\(\);/);
    // The page wants one only of a level that ended as a level does, in a match the lobby was told of, once.
    const glue = cut('      // ---- Sharing them (shotSharer above).', '      function sizeText(bytes)');
    assert.match(glue, /function shotWantsFinal\(\) \{[^}]*shotShareAllowed\(\) && ended && !ended\.left && ended\.told && !ended\.shot &&/);
    assert.match(glue, /function shotFinal\(rgba, w, h\) \{[^}]*ended\.shot = 1;/);
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
