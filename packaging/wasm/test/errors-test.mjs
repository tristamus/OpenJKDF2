// Tests for what packaging/wasm/index.html does when the engine can't start
// or stops: only the engine's own errors show the box (with Reload) and turn
// off the leave warning, and an error from anything else, such as a browser
// add-on's script, is left to the console. In Node (22 or newer):
// node packaging/wasm/test/errors-test.mjs
//
// The page's code is taken from index.html as it is, with made-up page parts.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to, a);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const code = cut('      window.onerror = function(message', '      // ---- Mouse look.');
const loadCode = cut('      function loadEngine(', '      var picker = ');
const engineKind = new Function(cut('      function engineKind(', '      var ENGINE = ') + '\nreturn engineKind;')();
const engineAgain = cut('      var ENGINE_FAILED = ', '      // For netstats.js');

// A made-up sessionStorage.
function storage(items = {}) {
    return { items, getItem: k => k in items ? items[k] : null, setItem: (k, v) => { items[k] = String(v); } };
}

// The page's error handling, with the parts of the page it uses made up.
function page({ ENGINE = 'asyncify', sessionStorage = storage(), search = '' } = {}) {
    const listeners = {}, shown = [], logged = [];
    const element = () => ({ style: {}, hidden: false, textContent: '' });
    const env = {
        window: { addEventListener: (type, fn) => { listeners[type] = fn; }, location: { search } },
        Module: { printErr: () => {}, setStatus: () => {} },
        engineLog: [],
        spinnerElement: element(), progressElement: element(), statusElement: element(),
        showProblem: (title, text, reload) => shown.push({ title, text, reload }),
        console: { error: line => logged.push(line) },
        ENGINE, ENGINE_FAILED: 'jkdf2mp-jspi-failed', sessionStorage,
    };
    const names = Object.keys(env);
    const run = new Function(...names, 'var leaveGuard = true;\n' + code +
        '\nreturn { leaveGuard: () => leaveGuard, engineError, controlFlow };');
    const api = run(...names.map(n => env[n]));
    return {
        ...api, env, shown, logged,
        error: (message, source, error) => env.window.onerror(message, source, 1, 1, error),
        rejection: reason => listeners.unhandledrejection({ reason }),
    };
}

// An error thrown from where the stack says.
function thrown(type, message, stack) {
    const e = new type(message);
    e.stack = type.name + ': ' + message + '\n' + stack;
    return e;
}
const ENGINE_STACK = '    at _fd_write (https://jediknight.zone/openjkdf2.js:1234:5)\n' +
                     '    at openjkdf2.wasm.jkGame_Update (wasm://wasm/0123abcd:wasm-function[4321]:0x1a2b3c)';
const FIREFOX_STACK = 'print@https://jediknight.zone/index.html:1790:11\n' +
                      '_fd_write@https://jediknight.zone/openjkdf2.js:1234:5\n';
const ADDON_STACK = '    at inject (chrome-extension://abcdefghijklmnop/content.js:10:3)';

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test("another site's script (\"Script error.\") changes nothing", () => {
    const p = page();
    p.error('Script error.', '', null);
    assert.deepEqual(p.shown, []);
    assert.equal(p.leaveGuard(), true);
    assert.equal(p.env.statusElement.textContent, '');
});

test("a browser add-on's error, or the page's own outside the engine, changes nothing", () => {
    const p = page();
    p.error('Uncaught TypeError: x is undefined', 'chrome-extension://abcdefghijklmnop/content.js',
            thrown(TypeError, 'x is undefined', ADDON_STACK));
    p.error('Uncaught TypeError: rooms is null', 'https://jediknight.zone/index.html',
            thrown(TypeError, 'rooms is null', '    at tick (https://jediknight.zone/index.html:5000:3)'));
    p.rejection(new DOMException('The quota has been exceeded.', 'QuotaExceededError'));
    p.rejection(thrown(Error, 'fetch failed', '    at poll (https://jediknight.zone/index.html:4000:9)'));
    assert.deepEqual(p.shown, []);
    assert.equal(p.leaveGuard(), true);
});

test('the engine failing to download stops with a Reload button, and turns off the leave warning', () => {
    const p = page();
    // Emscripten's abort when the wasm 404s twice.
    p.env.Module.onAbort('both async and sync fetching of the wasm failed');
    assert.equal(p.shown.length, 1);
    assert.equal(p.shown[0].title, "The game couldn't start");
    assert.equal(p.shown[0].reload, true);
    assert.match(p.shown[0].text, /didn't download or start properly/);
    assert.match(p.shown[0].text, /both async and sync fetching of the wasm failed/);
    assert.equal(p.leaveGuard(), false);
    assert.equal(p.env.spinnerElement.style.display, 'none');
    assert.equal(p.env.progressElement.hidden, true);
    assert.equal(p.env.statusElement.textContent, "The game couldn't start");
    assert.ok(p.env.engineLog.some(l => /The game stopped: both async/.test(l)), 'in the saved log');
    // The RuntimeError Emscripten throws after it only goes to the console.
    p.rejection(new WebAssembly.RuntimeError('Aborted(both async and sync fetching of the wasm failed)'));
    assert.equal(p.shown.length, 1);
    assert.equal(p.logged.length, 2);
});

test('openjkdf2.js that doesn\'t download, or comes cut off, stops with Reload', () => {
    const p = page();
    p.error('Uncaught SyntaxError: Unexpected end of input', 'https://jediknight.zone/openjkdf2.js',
            thrown(SyntaxError, 'Unexpected end of input', ''));
    assert.equal(p.shown[0].title, "The game couldn't start");
    assert.equal(p.shown[0].reload, true);

    // The script element's error event. When the JSPI engine's doesn't
    // download, the Asyncify engine's is tried, and when that doesn't either,
    // the game stops.
    for (const ENGINE of ['asyncify', 'jspi']) {
        const env = { leaveGuard: true, stopped: [], scripts: [], logged: [] };
        const document = { createElement: () => ({}), body: { appendChild: s => env.scripts.push(s) } };
        const Module = { jkEngine: ENGINE };
        const engineLog = [];
        const engine = new Function('document', 'engineStopped', 'Module', 'console', 'ENGINE', 'engineLog',
                                    'var leaveGuard;\n' + loadCode + '\nloadEngine(false);\nreturn () => ENGINE;')(
            document, why => env.stopped.push(why), Module, { log: line => env.logged.push(line) }, ENGINE, engineLog);
        if (ENGINE === 'jspi') {
            assert.deepEqual(env.scripts.map(s => s.src), ['jspi/openjkdf2.js']);
            env.scripts[0].onerror();
            assert.deepEqual(env.stopped, []);
            assert.deepEqual(env.logged, ["OpenJKDF2: jspi/openjkdf2.js didn't download; loading the Asyncify engine instead"]);
            assert.deepEqual([engine(), Module.jkEngine], ['asyncify', 'asyncify']);
            assert.deepEqual(engineLog, ["jspi/openjkdf2.js didn't download; loading the Asyncify engine instead"]);
        }
        assert.equal(env.scripts[env.scripts.length - 1].src, 'openjkdf2.js');
        env.scripts[env.scripts.length - 1].onerror();
        assert.deepEqual(env.stopped, ["openjkdf2.js didn't download"]);
    }
    // An error from the JSPI engine's script counts as the engine's.
    const q = page();
    q.error('Uncaught SyntaxError: Unexpected end of input', 'https://jediknight.zone/jspi/openjkdf2.js',
            thrown(SyntaxError, 'Unexpected end of input', ''));
    assert.equal(q.shown[0].title, "The game couldn't start");
    assert.equal(q.engineError(thrown(Error, 'x', '    at f (https://jediknight.zone/jspi/openjkdf2.js:1:2)'), ''), true);
});

test('the JSPI engine where the browser has JSPI, but not in Safari unless asked; the Asyncify engine elsewhere', () => {
    const jspi = { Suspending: function() {}, promising: function() {} };
    const chrome = 'Google Inc.', firefox = '', safari = 'Apple Computer, Inc.';
    // Chrome, Edge (Google Inc.) and Firefox ('') with JSPI.
    assert.equal(engineKind('', jspi, chrome), 'jspi');
    assert.equal(engineKind('?mp', jspi, firefox), 'jspi');
    assert.equal(engineKind('', jspi, undefined), 'jspi');
    // Without JSPI, or only half of it.
    assert.equal(engineKind('', {}, chrome), 'asyncify');
    assert.equal(engineKind('', { Suspending: function() {} }, chrome), 'asyncify');
    assert.equal(engineKind('', null, chrome), 'asyncify');
    // Safari, and every browser on an iPhone or iPad, even with JSPI.
    assert.equal(engineKind('', jspi, safari), 'asyncify');
    // ?engine= picks one; jspi still needs JSPI.
    assert.equal(engineKind('?engine=asyncify', jspi, chrome), 'asyncify');
    assert.equal(engineKind('?autostart&engine=jspi', jspi, safari), 'jspi');
    assert.equal(engineKind('?engine=jspi', {}, chrome), 'asyncify');
    assert.equal(engineKind('?engine=jspi', null, safari), 'asyncify');
    assert.equal(engineKind('?engine=other', jspi, safari), 'asyncify');
    assert.equal(engineKind('?myengine=asyncify', jspi, chrome), 'jspi');
});

test("a tab where the JSPI engine couldn't start, or hit a SuspendError, gets the Asyncify engine when reloaded", () => {
    // The page's choice on a reload, with what the tab remembers.
    function reload(ENGINE, search, items) {
        const env = { logged: [], engineLog: [] };
        const window = { location: { search } };
        const chosen = new Function('ENGINE', 'window', 'sessionStorage', 'console', 'engineLog',
                                    engineAgain + '\nreturn ENGINE;')(
            ENGINE, window, items === null ? undefined : storage(items), { log: line => env.logged.push(line) }, env.engineLog);
        return { chosen, ...env };
    }
    const FAILED = 'jkdf2mp-jspi-failed';
    // The JSPI engine's wasm didn't download, compile or link: remembered.
    for (const why of ['both async and sync fetching of the wasm failed',
                       "CompileError: WebAssembly.instantiate(): unknown import kind 0x55"]) {
        const tab = storage();
        page({ ENGINE: 'jspi', sessionStorage: tab }).env.Module.onAbort(why);
        assert.equal(tab.items[FAILED], why);
        const again = reload('jspi', '', tab.items);
        assert.equal(again.chosen, 'asyncify');
        assert.deepEqual(again.logged, ["OpenJKDF2: the JSPI engine didn't work in this tab before (" + why +
                                        '); loading the Asyncify engine']);
        assert.equal(again.engineLog.length, 1);
        // ?engine=jspi still gets it, and ?engine=asyncify was Asyncify anyway.
        assert.equal(reload('jspi', '?engine=jspi', tab.items).chosen, 'jspi');
        assert.equal(reload('asyncify', '?engine=asyncify', tab.items).chosen, 'asyncify');
    }
    // A SuspendError during play: remembered too.
    const suspended = storage();
    const p = page({ ENGINE: 'jspi', sessionStorage: suspended });
    p.env.Module.onRuntimeInitialized();
    const SuspendError = typeof WebAssembly.SuspendError === 'function' ? WebAssembly.SuspendError : class SuspendError extends Error {};
    const e = thrown(SuspendError, 'trying to suspend JS frames', ENGINE_STACK);
    e.name = 'SuspendError';
    p.rejection(e);
    assert.equal(p.shown[0].title, 'The game stopped');
    assert.match(p.shown[0].text, /Reload the page to play on/);
    assert.match(suspended.items[FAILED], /SuspendError/);
    // Not remembered: any other crash during play, or the Asyncify engine failing.
    const crashed = storage();
    const q = page({ ENGINE: 'jspi', sessionStorage: crashed });
    q.env.Module.onRuntimeInitialized();
    q.rejection(thrown(WebAssembly.RuntimeError, 'unreachable', ENGINE_STACK));
    assert.deepEqual(crashed.items, {});
    assert.doesNotMatch(q.shown[0].text, /Reload the page to play on/);
    // Nor the browser turning down the memory, which the Asyncify engine needs too.
    const memory = storage();
    page({ ENGINE: 'jspi', sessionStorage: memory }).env.Module.onAbort(
        'RangeError: WebAssembly.instantiate(): Out of memory: Cannot allocate Wasm memory for new instance');
    assert.deepEqual(memory.items, {});
    // A crash before the start that names memory is the JSPI engine's still, and not the browser's memory.
    const oob = storage();
    const o = page({ ENGINE: 'jspi', sessionStorage: oob });
    o.env.Module.onAbort('RuntimeError: memory access out of bounds');
    assert.equal(oob.items[FAILED], 'RuntimeError: memory access out of bounds');
    assert.doesNotMatch(o.shown[0].text, /couldn't give the game the memory/);
    // V8's other words for it (WebAssembly.Memory()).
    const mem2 = storage();
    page({ ENGINE: 'jspi', sessionStorage: mem2 }).env.Module.onAbort('RangeError: WebAssembly.Memory(): could not allocate memory');
    assert.deepEqual(mem2.items, {});
    const mem3 = storage();
    page({ ENGINE: 'jspi', sessionStorage: mem3 }).env.Module.onAbort('RangeError: Array buffer allocation failed');
    assert.deepEqual(mem3.items, {});
    // ?engine=jspi keeps the JSPI engine on a reload, so the box doesn't say the other engine comes.
    const asked = storage();
    const a = page({ ENGINE: 'jspi', sessionStorage: asked, search: '?engine=jspi' });
    a.env.Module.onRuntimeInitialized();
    a.rejection(e);
    assert.match(asked.items[FAILED], /SuspendError/);
    assert.doesNotMatch(a.shown[0].text, /Reload the page to play on/);
    const asyncify = storage();
    page({ ENGINE: 'asyncify', sessionStorage: asyncify }).env.Module.onAbort('both async and sync fetching of the wasm failed');
    assert.deepEqual(asyncify.items, {});
    // A fresh tab, and storage the browser turns down (no sessionStorage, or it throws).
    assert.equal(reload('jspi', '', {}).chosen, 'jspi');
    assert.equal(reload('jspi', '', null).chosen, 'jspi');
    const blocked = { getItem() { throw new Error('SecurityError'); }, setItem() { throw new Error('SecurityError'); } };
    const r = page({ ENGINE: 'jspi', sessionStorage: blocked });
    r.env.Module.onAbort('both async and sync fetching of the wasm failed');
    assert.equal(r.shown.length, 1);
});

test("the browser running out of memory for the engine says so", () => {
    const p = page();
    p.env.Module.onAbort('RangeError: WebAssembly.instantiate(): Out of memory: Cannot allocate Wasm memory for new instance');
    assert.equal(p.shown[0].title, "The game couldn't start");
    assert.match(p.shown[0].text, /couldn't give the game the memory it needs/);
    assert.equal(p.shown[0].reload, true);
});

test('a crash during play stops the game, once', () => {
    const p = page();
    p.env.Module.onRuntimeInitialized();
    p.rejection(thrown(WebAssembly.RuntimeError, 'null function or function signature mismatch', ENGINE_STACK));
    p.rejection(thrown(WebAssembly.RuntimeError, 'unreachable', ENGINE_STACK));
    assert.equal(p.shown.length, 1);
    assert.equal(p.shown[0].title, 'The game stopped');
    assert.match(p.shown[0].text, /null function/);
    assert.match(p.shown[0].text, /Press F12/);
    assert.equal(p.leaveGuard(), false);
});

test("the page's code failing while the engine called it stops the game (Chromium and Firefox stacks)", () => {
    for (const stack of ['    at Object.print (https://jediknight.zone/index.html:1790:11)\n' + ENGINE_STACK, FIREFOX_STACK]) {
        const p = page();
        p.env.Module.onRuntimeInitialized();
        p.error('Uncaught TypeError: netLogLine is not a function', 'https://jediknight.zone/index.html',
                thrown(TypeError, 'netLogLine is not a function', stack));
        assert.equal(p.shown.length, 1);
        assert.equal(p.shown[0].title, 'The game stopped');
    }
    // A wasm frame alone, as Safari writes them.
    assert.equal(page().engineError(thrown(Error, 'x', 'wasm-stub@[wasm code]\n<?>.wasm-function[42]@[wasm code]'), ''), true);
});

test("Emscripten's 'unwind' and ExitStatus aren't crashes", () => {
    const p = page();
    p.env.Module.onRuntimeInitialized();
    p.error('Uncaught unwind', 'https://jediknight.zone/openjkdf2.js', 'unwind');
    p.rejection('unwind');
    const exit = { name: 'ExitStatus', message: 'Program terminated with exit(0)', status: 0 };
    p.error('Uncaught #<ExitStatus>', 'https://jediknight.zone/openjkdf2.js', exit);
    p.rejection(exit);
    assert.deepEqual(p.shown, []);
    assert.equal(p.leaveGuard(), true);
});

test("the browser turning down a request the engine didn't wait on isn't a crash", () => {
    // Firefox rejects a pointer lock asked for while the window isn't focused,
    // and the stack runs through the engine's JavaScript.
    const p = page();
    p.env.Module.onRuntimeInitialized();
    const refused = new DOMException('The document is not focused.', 'WrongDocumentError');
    Object.defineProperty(refused, 'stack', { value: 'requestPointerLock@https://jediknight.zone/openjkdf2.js:1:2345\n' });
    p.rejection(refused);
    assert.deepEqual(p.shown, []);
    assert.equal(p.leaveGuard(), true);
});

// The box itself, and the fullscreen start it holds back.
const boxCode = cut('      // reload: with a Reload button', '      var problemReload = ');
const fullscreenCode = cut('      function playFullscreen()', "      window.addEventListener('pointerdown'");
function box({ locked = true, fullscreen = true } = {}) {
    const calls = [];
    const element = () => ({ hidden: false, textContent: '' });
    const canvas = {}, root = {};
    const document = {
        pointerLockElement: locked ? canvas : null,
        fullscreenElement: fullscreen ? root : null,
        fullscreenEnabled: true,
        exitPointerLock() { calls.push('exitPointerLock'); this.pointerLockElement = null; },
        exitFullscreen() { calls.push('exitFullscreen'); this.fullscreenElement = null; return Promise.resolve(); },
        getElementById: () => ({ hidden: true }),
        querySelector: () => null,
        documentElement: { requestFullscreen() { calls.push('requestFullscreen'); return Promise.resolve(); } },
    };
    const picker = {
        open: false, classList: { remove() {} }, querySelectorAll: () => [],
        showModal() { calls.push('showModal'); this.open = true; },
    };
    const env = {
        document, picker, navigator: {},
        pickerTitle: element(), modChoice: element(), problemReload: element(),
        showPickerResult: () => {}, fullscreenOption: { checked: true },
    };
    const names = Object.keys(env);
    const api = new Function(...names, boxCode + '\n' + fullscreenCode +
        '\nreturn { showProblem, playFullscreen };')(...names.map(n => env[n]));
    return { ...api, calls, document };
}

test('the box with Reload gives back the mouse and leaves fullscreen first, so Reload can be clicked', () => {
    const b = box();
    b.showProblem('The game stopped', 'unreachable', true);
    assert.deepEqual(b.calls, ['exitPointerLock', 'exitFullscreen', 'showModal']);
    // A click on the game after the box is hidden doesn't go fullscreen again.
    b.playFullscreen();
    assert.deepEqual(b.calls, ['exitPointerLock', 'exitFullscreen', 'showModal']);

    const free = box({ locked: false, fullscreen: false });
    free.showProblem('The game lost its graphics', 'Reload the page to play again.', true);
    assert.deepEqual(free.calls, ['showModal']);
});

test('before any box, a click on the game still goes fullscreen', () => {
    const b = box({ locked: false, fullscreen: false });
    b.playFullscreen();
    assert.deepEqual(b.calls, ['requestFullscreen']);
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
