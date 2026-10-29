// Tests for unzip.js, the decoders for the zip compression methods the
// browser has none for. Every entry of the zips in fixtures/zips (made by
// PKZIP, 7-Zip, xz, zstd and the rest; see fixtures/make-zips.py) has to come
// out matching its CRC-32, and damaged entries have to fail rather than
// unpack wrong or hang.
//
//   node packaging/wasm/test/unzip-test.mjs

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { zipEntries } from './helpers.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const JkUnzip = new Function(fs.readFileSync(path.join(here, '..', 'unzip.js'), 'utf8') + '\nreturn JkUnzip;')();
const ZIPS = path.join(here, 'fixtures', 'zips');

const all = [];
for (const z of fs.readdirSync(ZIPS).sort())
    for (const en of zipEntries(fs.readFileSync(path.join(ZIPS, z))))
        all.push({ zip: z, ...en });

const unzip = en => JkUnzip.unzip(en.method, en.flags, en.data, en.size, en.crc);

let passed = 0;
function test(name, fn) {
    try {
        fn();
        passed++;
        console.log('ok   ' + name);
    } catch (e) {
        console.log('FAIL ' + name);
        throw e;
    }
}

test('the zips hold every method unzip.js has, and some of them more than once', () => {
    const seen = new Set(all.map(en => en.method));
    for (const m of JkUnzip.methods)
        assert.ok(seen.has(m) || m === 0, 'no entry for method ' + m);
    assert.deepEqual(JkUnzip.methods, [0, 1, 2, 3, 4, 5, 6, 8, 9, 12, 14, 93, 95, 98]);
});

const times = {};
for (const en of all) {
    test(`${en.zip}: ${en.name} (method ${en.method}, ${en.size} bytes)`, () => {
        const t = performance.now();
        const out = unzip(en);
        times[en.method] = (times[en.method] || 0) + performance.now() - t;
        assert.equal(out.length, en.size);
        assert.equal(JkUnzip.crc32(out), en.crc);
    });
}

test('a CRC-32 that does not match fails', () => {
    for (const en of all.filter(e => e.size))
        assert.throws(() => JkUnzip.unzip(en.method, en.flags, en.data, en.size, (en.crc ^ 1) >>> 0), /CRC/, en.name);
});

test('a size that is off by one fails', () => {
    for (const en of all.filter(e => e.method !== 0 && e.size)) {
        assert.throws(() => JkUnzip.unzip(en.method, en.flags, en.data, en.size + 1, en.crc), Error, en.name);
        assert.throws(() => JkUnzip.unzip(en.method, en.flags, en.data, en.size - 1, en.crc), Error, en.name);
    }
});

// Either way round, the CRC-32 has to come out right or the entry fail: a
// format's last byte can be one no decoder needs (LZMA's range coder ends
// with zeros, which reading past the end gives anyway).
function failsOrRight(en, data, what) {
    let out;
    try {
        out = unzip({ ...en, data });
    } catch (e) {
        assert.ok(e instanceof Error, what);
        return true;
    }
    assert.equal(JkUnzip.crc32(out), en.crc, what);
    return false;
}

test('a cut-off entry fails, or comes out right when only padding was cut', () => {
    for (const en of all.filter(e => e.data.length > 8)) {
        assert.ok(failsOrRight(en, en.data.subarray(0, 1), en.name), en.name + ' cut to 1 byte');
        assert.ok(failsOrRight(en, en.data.subarray(0, en.data.length >> 1), en.name), en.name + ' cut in half');
        failsOrRight(en, en.data.subarray(0, en.data.length - 1), en.name + ' without its last byte');
    }
});

test('a damaged entry fails, or unpacks right when the damage was in bits nothing reads', () => {
    let seed = 1;
    const rand = n => ((seed = (seed * 1103515245 + 12345) >>> 0) >>> 8) % n;
    for (const en of all.filter(e => e.data.length > 4)) {
        for (let k = 0; k < 24; k++) {
            const data = en.data.slice();
            const at = rand(data.length);
            data[at] ^= 1 << rand(8);
            failsOrRight(en, data, en.name + ' with byte ' + at + ' changed');
        }
    }
});

test('methods it does not have say so', () => {
    for (const m of [7, 10, 18, 19, 96, 97, 99])
        assert.throws(() => JkUnzip.unzip(m, 0, new Uint8Array(4), 4), /method .* not supported/);
});

console.log(`\n${passed} passed`);
console.log('time per method (ms): ' + Object.entries(times).map(([m, t]) => m + ' ' + t.toFixed(0)).join(', '));
