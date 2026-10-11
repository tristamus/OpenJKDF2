// Shared by the tests in this folder: made-up GOBs and zips, an in-memory
// stand-in for Emscripten's FS and one for IndexedDB.

import { createHash } from 'node:crypto';
import { deflateRawSync, crc32 } from 'node:zlib';

// ---- Made-up GOBs, laid out like the real ones: header, file table, data.

export function makeGob(files, { overlap } = {}) {
    const table = 12 + 4;
    let dataAt = table + files.length * 136;
    const placed = files.map(f => {
        const at = overlap && f.sameAs !== undefined ? null : dataAt;
        if (at !== null)
            dataAt += f.bytes.length;
        return { ...f, at };
    });
    const out = new Uint8Array(dataAt);
    const v = new DataView(out.buffer);
    out.set([0x47, 0x4F, 0x42, 0x20]);
    v.setUint32(4, 20, true);
    v.setUint32(8, 12, true);
    v.setUint32(12, files.length, true);
    placed.forEach((f, i) => {
        if (f.at === null)
            f.at = placed[f.sameAs].at + f.offsetIn;
        const e = table + i * 136;
        v.setUint32(e, f.at, true);
        v.setUint32(e + 4, f.bytes.length, true);
        for (let j = 0; j < f.name.length; j++)
            out[e + 8 + j] = f.name.charCodeAt(j);
        if (!(overlap && f.sameAs !== undefined))
            out.set(f.bytes, f.at);
    });
    return out;
}

// ---- Made-up zips: [{ name, bytes, store }], deflated unless store is set.

export function makeZip(entries) {
    const locals = [], centrals = [];
    let at = 0;
    for (const e of entries) {
        const name = Buffer.from(e.name, 'utf8');
        const data = e.store ? Buffer.from(e.bytes) : deflateRawSync(e.bytes);
        const method = e.store ? 0 : 8, crc = crc32(e.bytes);
        const local = Buffer.alloc(30);
        local.writeUInt32LE(0x04034b50, 0);
        local.writeUInt16LE(20, 4);
        local.writeUInt16LE(0x800, 6);
        local.writeUInt16LE(method, 8);
        local.writeUInt32LE(crc, 14);
        local.writeUInt32LE(data.length, 18);
        local.writeUInt32LE(e.bytes.length, 22);
        local.writeUInt16LE(name.length, 26);
        locals.push(local, name, data);
        const central = Buffer.alloc(46);
        central.writeUInt32LE(0x02014b50, 0);
        central.writeUInt16LE(20, 4);
        central.writeUInt16LE(20, 6);
        central.writeUInt16LE(0x800, 8);
        central.writeUInt16LE(method, 10);
        central.writeUInt32LE(crc, 16);
        central.writeUInt32LE(data.length, 20);
        central.writeUInt32LE(e.bytes.length, 24);
        central.writeUInt16LE(name.length, 28);
        central.writeUInt32LE(at, 42);
        centrals.push(central, name);
        at += 30 + name.length + data.length;
    }
    const cd = Buffer.concat(centrals);
    const end = Buffer.alloc(22);
    end.writeUInt32LE(0x06054b50, 0);
    end.writeUInt16LE(entries.length, 8);
    end.writeUInt16LE(entries.length, 10);
    end.writeUInt32LE(cd.length, 12);
    end.writeUInt32LE(at, 16);
    return Buffer.concat([...locals, cd, end]);
}

export function sha(bytes) {
    return createHash('sha256').update(bytes).digest('hex');
}

export function contains(hay, needle) {
    return Buffer.from(hay).indexOf(Buffer.from(needle)) >= 0;
}

// ---- An in-memory stand-in for Emscripten's FS, the calls maps.js makes.

export function makeFS(files, cwd = '/jk1') {
    const map = new Map(Object.entries(files));
    const DIR = 0o40000, FILE = 0o100000;
    const dirs = () => {
        const d = new Set(['/']);
        for (const p of map.keys()) {
            const parts = p.split('/');
            for (let i = 2; i < parts.length; i++)
                d.add(parts.slice(0, i).join('/'));
        }
        return d;
    };
    const extraDirs = new Set();
    return {
        files: map,
        cwd: () => cwd,
        readdir(path) {
            path = path.replace(/\/+$/, '') || '/';
            if (!dirs().has(path) && !extraDirs.has(path))
                throw new Error('ENOENT ' + path);
            const names = new Set(['.', '..']);
            for (const p of [...map.keys(), ...dirs(), ...extraDirs]) {
                if (p.startsWith(path + '/') && p !== path) {
                    const rest = p.slice(path.length + 1);
                    if (rest)
                        names.add(rest.split('/')[0]);
                }
            }
            return [...names];
        },
        stat(path) {
            if (map.has(path))
                return { mode: FILE, size: map.get(path).length };
            if (dirs().has(path) || extraDirs.has(path))
                return { mode: DIR, size: 4096 };
            throw new Error('ENOENT ' + path);
        },
        isFile: mode => (mode & FILE) === FILE && !(mode & DIR & ~FILE),
        isDir: mode => mode === DIR,
        readFile(path) {
            if (!map.has(path))
                throw new Error('ENOENT ' + path);
            return new Uint8Array(map.get(path));
        },
        writeFile(path, bytes) { map.set(path, new Uint8Array(bytes)); },
        unlink(path) {
            if (!map.delete(path))
                throw new Error('ENOENT ' + path);
        },
        mkdirTree(path) { extraDirs.add(path); },
        open(path) {
            if (!map.has(path))
                throw new Error('ENOENT ' + path);
            return { path };
        },
        read(stream, buf, offset, length, position) {
            const data = map.get(stream.path);
            const part = data.subarray(position, position + length);
            buf.set(part, offset);
            return part.length;
        },
        close() {},
    };
}


// ---- An in-memory stand-in for IndexedDB, the calls makeStore (maps.js)
// makes. Several tabs can share one, like tabs of one site in a browser.

export function makeIDB() {
    const dbs = new Map();
    const later = fn => setImmediate(fn);

    function makeTx(db) {
        const tx = { pending: 0, finished: false, oncomplete: null, onerror: null, onabort: null };
        const settle = () => {
            later(() => {
                if (tx.pending || tx.finished)
                    return;
                tx.finished = true;
                if (tx.oncomplete)
                    tx.oncomplete();
            });
        };
        const request = fn => {
            const req = { onsuccess: null, onerror: null };
            tx.pending++;
            later(() => {
                req.result = fn();
                tx.pending--;
                if (req.onsuccess)
                    req.onsuccess();
                settle();
            });
            return req;
        };
        tx.objectStore = name => {
            const s = db.stores.get(name);
            const keyOf = (value, key) => (s.keyPath ? value[s.keyPath] : key);
            const copy = v => (v instanceof Uint8Array ? v.slice() : structuredClone(v));
            return {
                get: k => request(() => (s.data.has(k) ? copy(s.data.get(k)) : undefined)),
                getAll: () => request(() => [...s.data.values()].map(copy)),
                put: (v, k) => request(() => { s.data.set(keyOf(v, k), copy(v)); return keyOf(v, k); }),
                delete: k => request(() => { s.data.delete(k); }),
            };
        };
        settle();
        return tx;
    }

    return {
        dbs,
        open(name) {
            const req = { onsuccess: null, onerror: null, onupgradeneeded: null, onblocked: null };
            later(() => {
                let db = dbs.get(name);
                const fresh = !db;
                if (fresh) {
                    db = {
                        stores: new Map(),
                        createObjectStore(n, opts) { this.stores.set(n, { keyPath: opts && opts.keyPath, data: new Map() }); },
                        transaction() { return makeTx(this); },
                    };
                    dbs.set(name, db);
                }
                req.result = db;
                if (fresh && req.onupgradeneeded)
                    req.onupgradeneeded();
                if (req.onsuccess)
                    req.onsuccess();
            });
            return req;
        },
    };
}
