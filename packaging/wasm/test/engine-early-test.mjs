// Tests for how packaging/wasm/index.html gets the engine ready early: from
// when the player asks to play, the engine's script is preloaded and its wasm
// downloaded and compiled (prefetchEngine), and the engine gets the compiled
// wasm through Module.instantiateWasm (earlyWasm), or downloads it again as it
// did by itself when that failed. In Node (22 or newer):
// node packaging/wasm/test/engine-early-test.mjs
//
// The page's code is taken from index.html as it is, with made-up page parts
// and real (tiny) wasm modules.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to, a);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const loadCode = cut('      function loadEngine(', '      var picker = ');

// A module importing one function, f, from mod, and exporting g, which calls
// it: mod is 'a' like the Asyncify engine's imports, 'env' like the JSPI one's.
function wasm(mod) {
    const name = [...Buffer.from(mod)];
    const imports = [0x01, name.length, ...name, 0x01, 0x66, 0x00, 0x00];
    return new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
                           0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
                           0x02, imports.length, ...imports,
                           0x03, 0x02, 0x01, 0x00,
                           0x07, 0x05, 0x01, 0x01, 0x67, 0x00, 0x01,
                           0x0a, 0x06, 0x01, 0x04, 0x00, 0x10, 0x00, 0x0b]);
}
const WASM = { asyncify: wasm('a'), jspi: wasm('env') };
const IMPORTS = { asyncify: { a: { f: () => 7 } }, jspi: { env: { f: () => 7 }, wasi_snapshot_preview1: {} } };

const ok = bytes => new Response(bytes, { headers: { 'content-type': 'application/wasm' } });
const failed = status => new Response('no', { status, headers: { 'content-type': 'text/plain' } });
const settle = () => new Promise(resolve => setTimeout(resolve, 20));

// The page's engine loading. answers: what each fetch gets, in turn (a
// Response, or an Error to reject with); after them, the engine's wasm.
function page({ ENGINE = 'jspi', answers = [], WebAssembly: wa = WebAssembly } = {}) {
    const env = { fetched: [], links: [], scripts: [], stopped: [], logged: [], warned: [], engineLog: [] };
    let engineNow = ENGINE;
    const fetch = (url, init) => {
        env.fetched.push({ url, credentials: init && init.credentials });
        const a = answers.length ? answers.shift() : ok(WASM[url.startsWith('jspi/') ? 'jspi' : 'asyncify']);
        return a instanceof Error ? Promise.reject(a) : Promise.resolve(a);
    };
    const document = {
        baseURI: 'https://jediknight.zone/',
        createElement: tag => ({ tag }),
        head: { appendChild: e => env.links.push(e) },
        body: { appendChild: e => env.scripts.push(e) },
    };
    const Module = { jkEngine: ENGINE };
    const console = { log: l => env.logged.push(l), warn: l => env.warned.push(l), error: l => env.logged.push(l) };
    const api = new Function('document', 'engineStopped', 'Module', 'console', 'ENGINE', 'engineLog', 'fetch',
                             'WebAssembly', 'performance',
                             'var leaveGuard;\n' + loadCode +
                             '\nreturn { loadEngine, prefetchEngine, engine: () => ENGINE, early: () => earlyEngine };')(
        document, why => env.stopped.push(why), Module, console, ENGINE, env.engineLog, fetch, wa, performance);
    // Starts the engine as openjkdf2.js does, with this engine's imports.
    function start() {
        assert.equal(typeof Module.instantiateWasm, 'function', 'the engine gets the early wasm');
        return new Promise(resolve => Module.instantiateWasm(IMPORTS[api.engine()], resolve));
    }
    return { ...api, env, Module, start };
}

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

const unhandled = [];
process.on('unhandledRejection', reason => unhandled.push(reason));

test('nothing downloads until the player asks to play', async () => {
    const p = page();
    await settle();
    assert.deepEqual([p.env.fetched, p.env.links, p.env.scripts], [[], [], []]);
    assert.equal(p.early(), null);
});

for (const ENGINE of ['jspi', 'asyncify']) {
    const js = ENGINE === 'jspi' ? 'jspi/openjkdf2.js' : 'openjkdf2.js';
    const wasmFile = ENGINE === 'jspi' ? 'jspi/openjkdf2.wasm' : 'openjkdf2.wasm';
    test(`${ENGINE}: Play preloads the script and compiles the wasm once, and the engine gets it`, async () => {
        const p = page({ ENGINE });
        p.prefetchEngine();
        p.prefetchEngine();
        assert.deepEqual(p.env.links.map(l => [l.rel, l.as, l.href]), [['preload', 'script', js]]);
        assert.deepEqual(p.env.fetched, [{ url: wasmFile, credentials: 'same-origin' }]);
        await settle();
        p.loadEngine(false);
        assert.deepEqual(p.env.scripts.map(s => s.src), [js]);
        const instance = await p.start();
        assert.equal(instance.exports.g(), 7);
        assert.equal(p.env.fetched.length, 1, 'not downloaded again');
        assert.ok(p.env.logged.some(l => /^OpenJKDF2: the engine downloaded and compiled in \d+ ms from Play, \d+ ms before the game needed it$/.test(l)));
    });
}

test('the game starts before the early wasm is ready: the engine waits for it', async () => {
    const p = page();
    p.prefetchEngine();
    p.loadEngine(false);
    const instance = await p.start();
    assert.equal(instance.exports.g(), 7);
    assert.equal(p.env.fetched.length, 1);
    assert.ok(p.env.logged.some(l => /^OpenJKDF2: the engine downloaded and compiled in \d+ ms from Play, \d+ ms after the game needed it$/.test(l)));
});

test('no early download (the bundled test build): the engine gets its wasm itself', () => {
    const p = page();
    p.Module.instantiateWasm = () => {};
    p.loadEngine(false);
    assert.equal('instantiateWasm' in p.Module, false);
});

test('the early download failed before the game started: the engine gets its wasm itself, and nothing stops', async () => {
    for (const answer of [new TypeError('Failed to fetch'), failed(503), ok(WASM.jspi.subarray(0, 30))]) {
        const p = page({ answers: [answer] });
        p.prefetchEngine();
        await settle();
        p.loadEngine(false);
        assert.equal('instantiateWasm' in p.Module, false);
        assert.deepEqual(p.env.stopped, []);
        assert.equal(p.env.warned.length, 1);
        assert.match(p.env.warned[0], /^OpenJKDF2: getting the engine ready early failed \(.+\)$/);
    }
});

test('the early download fails while the engine waits: downloaded again as the engine would', async () => {
    // A dropped connection, a server error during a deploy, a cut-off file.
    for (const answer of [new TypeError('Failed to fetch'), failed(503), ok(WASM.jspi.subarray(0, 30))]) {
        const p = page({ answers: [answer] });
        p.prefetchEngine();
        p.loadEngine(false);
        const instance = await p.start();
        assert.equal(instance.exports.g(), 7);
        assert.deepEqual(p.env.fetched.map(f => f.url), ['jspi/openjkdf2.wasm', 'jspi/openjkdf2.wasm']);
        assert.ok(p.env.engineLog.some(l => /^Downloading the engine again \(.+\)$/.test(l)));
        assert.deepEqual(p.env.stopped, []);
    }
});

test('an early wasm that does not fit the script (a deploy in between): downloaded again', async () => {
    // The Asyncify-style wasm where the JSPI-style script came: a LinkError.
    const p = page({ answers: [ok(WASM.asyncify)] });
    p.prefetchEngine();
    await settle();
    p.loadEngine(false);
    const instance = await p.start();
    assert.equal(instance.exports.g(), 7);
    assert.equal(p.env.fetched.length, 2);
});

test('a browser without streaming compile: the whole file, then the same', async () => {
    const wa = { compile: WebAssembly.compile, instantiate: WebAssembly.instantiate, Module: WebAssembly.Module, Instance: WebAssembly.Instance };
    const p = page({ WebAssembly: wa, answers: [failed(503)] });
    p.prefetchEngine();
    await settle();
    assert.equal(p.early().failed, true);
    const q = page({ WebAssembly: wa });
    q.prefetchEngine();
    q.loadEngine(false);
    assert.equal((await q.start()).exports.g(), 7);
});

test('it fails again: the game stops as it did when the engine got its own', async () => {
    const p = page({ answers: [failed(503), failed(503), failed(503)] });
    p.prefetchEngine();
    p.loadEngine(false);
    let started = false;
    p.Module.instantiateWasm(IMPORTS.jspi, () => { started = true; });
    await settle();
    assert.equal(started, false);
    assert.equal(p.env.fetched.length, 3, 'early, streamed, whole file');
    assert.equal(p.env.stopped.length, 1);
    assert.match(p.env.stopped[0], /503/);
});

test('no memory for the engine: it stops without another download', async () => {
    const wa = Object.create(WebAssembly);
    wa.instantiate = () => Promise.reject(new RangeError('WebAssembly.Instance(): Out of memory: Cannot allocate Wasm memory for new instance'));
    const p = page({ WebAssembly: wa });
    p.prefetchEngine();
    p.loadEngine(false);
    p.Module.instantiateWasm(IMPORTS.jspi, () => assert.fail('started'));
    await settle();
    assert.equal(p.env.fetched.length, 1);
    assert.deepEqual(p.env.stopped, ['RangeError: WebAssembly.Instance(): Out of memory: Cannot allocate Wasm memory for new instance']);
});

test("the JSPI engine's script didn't download: the Asyncify engine loads without the JSPI wasm", async () => {
    const p = page({ ENGINE: 'jspi' });
    p.prefetchEngine();
    await settle();
    p.loadEngine(false);
    assert.equal(typeof p.Module.instantiateWasm, 'function');
    p.env.scripts[0].onerror();
    assert.equal(p.engine(), 'asyncify');
    assert.deepEqual(p.env.scripts.map(s => s.src), ['jspi/openjkdf2.js', 'openjkdf2.js']);
    assert.equal('instantiateWasm' in p.Module, false);
});

test('the engine looks for its wasm where ENGINE_FILES says', () => {
    for (const [ENGINE, url] of [['jspi', 'https://jediknight.zone/jspi/openjkdf2.wasm'], ['asyncify', 'https://jediknight.zone/openjkdf2.wasm']]) {
        const p = page({ ENGINE });
        assert.equal(p.Module.locateFile('openjkdf2.wasm', 'https://jediknight.zone/' + (ENGINE === 'jspi' ? 'jspi/' : '')), url);
        assert.equal(p.Module.locateFile('openjkdf2_data.data', ''), 'openjkdf2_data.data');
    }
});

test('no failure above was left unhandled (it would count as the engine stopping)', async () => {
    await settle();
    assert.deepEqual(unhandled, []);
});

let failedCount = 0;
for (const t of tests) {
    try {
        await t.fn();
        console.log('ok   ' + t.name);
    } catch (e) {
        failedCount++;
        console.log('FAIL ' + t.name + '\n' + (e.stack || e));
    }
}
console.log(failedCount ? `${failedCount} of ${tests.length} failed` : `all ${tests.length} passed`);
process.exit(failedCount ? 1 : 0);
