// Tests for screenshots in packaging/wasm/index.html: their names, turning
// the engine's pixels the right way up, storing them, telling the engine how
// each one went, and the zip of them all. In Node (22 or newer):
// node packaging/wasm/test/screenshots-test.mjs
//
// The page's functions are taken from index.html as they are. Making the PNG
// needs a browser, so a stand-in makes the images here.

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
const code = cut('      var SHOT_GAMES', '      // ---- Your Screenshots on this page.') +
    '\nreturn { shotLevel, shotName, shotPixels, shotStore, shotSaver, zipFiles };';

// The page's code, with its clock set to the given times, one per new Date().
function page(times = []) {
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
    return new Function('Date', code)(FakeDate);
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

    const full = shotSaver({ list: async () => [], add: async () => { throw new Error('quota'); } }, images);
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
