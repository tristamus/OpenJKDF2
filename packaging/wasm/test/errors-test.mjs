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

// The page's error handling, with the parts of the page it uses made up.
function page() {
    const listeners = {}, shown = [], logged = [];
    const element = () => ({ style: {}, hidden: false, textContent: '' });
    const env = {
        window: { addEventListener: (type, fn) => { listeners[type] = fn; } },
        Module: { printErr: () => {}, setStatus: () => {} },
        engineLog: [],
        spinnerElement: element(), progressElement: element(), statusElement: element(),
        showProblem: (title, text, reload) => shown.push({ title, text, reload }),
        console: { error: line => logged.push(line) },
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

    // The script element's error event.
    const env = { leaveGuard: true, stopped: [], scripts: [] };
    const document = { createElement: () => ({}), body: { appendChild: s => env.scripts.push(s) } };
    new Function('document', 'engineStopped', 'var leaveGuard;\n' + loadCode + '\nloadEngine(false);')(
        document, why => env.stopped.push(why));
    assert.equal(env.scripts.length, 1);
    assert.equal(env.scripts[0].src, 'openjkdf2.js');
    env.scripts[0].onerror();
    assert.deepEqual(env.stopped, ["openjkdf2.js didn't download"]);
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
