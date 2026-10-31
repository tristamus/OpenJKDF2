// Tests for which keys reach the game in the browser and the default key
// binds. In Node:
// node packaging/wasm/test/keys-test.mjs
//
// Read from the sources as they are: the SDL-to-DirectInput key table in
// src/Platform/SDL2/stdControl.c, the key names the Controls screen shows,
// the default keyboard binds, and the page's engine arguments.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const read = path => readFileSync(new URL('../../../' + path, import.meta.url), 'utf8');
const stdControlC = read('src/Platform/SDL2/stdControl.c');
const namesC = read('src/Platform/Common/stdControl.c');
const sithControlC = read('src/Devices/sithControl.c');
const enums = read('src/types_win_enums.h');
const html = read('packaging/wasm/index.html');

const DIK = {};
for (const m of enums.matchAll(/#define\s+(DIK_\w+)\s+(0x[0-9A-Fa-f]+|\d+)/g))
    DIK[m[1]] = Number(m[2]);

// The table, one value per SDL scancode, comments left out.
function sdlToDik() {
    const at = stdControlC.indexOf('stdControl_aSdlToDik[256] =');
    assert.ok(at > 0, 'stdControl.c has the key table');
    const body = stdControlC.substring(stdControlC.indexOf('{', at) + 1, stdControlC.indexOf('};', at));
    const out = [];
    for (const line of body.split('\n'))
        for (const tok of line.split('//')[0].split(',').map(t => t.trim()).filter(Boolean))
            out.push(tok === '0' ? 0 : DIK[tok]);
    return out;
}
const table = sdlToDik();

// Each key of a standard keyboard, by the browser's KeyboardEvent.code: the
// SDL scancode SDL gives it (USB HID usage) and the game key it should be.
const KEYS = {
    Escape: [41, 'DIK_ESCAPE'], Backquote: [53, 'DIK_GRAVE'], Minus: [45, 'DIK_MINUS'], Equal: [46, 'DIK_EQUALS'],
    Backspace: [42, 'DIK_BACK'], Tab: [43, 'DIK_TAB'], BracketLeft: [47, 'DIK_LBRACKET'],
    BracketRight: [48, 'DIK_RBRACKET'], Backslash: [49, 'DIK_BACKSLASH'], CapsLock: [57, 'DIK_CAPITAL'],
    Semicolon: [51, 'DIK_SEMICOLON'], Quote: [52, 'DIK_APOSTROPHE'], Enter: [40, 'DIK_RETURN'],
    Comma: [54, 'DIK_COMMA'], Period: [55, 'DIK_PERIOD'], Slash: [56, 'DIK_SLASH'], Space: [44, 'DIK_SPACE'],
    IntlBackslash: [100, 'DIK_BACKSLASH'],
    ShiftLeft: [225, 'DIK_LSHIFT'], ShiftRight: [229, 'DIK_RSHIFT'], ControlLeft: [224, 'DIK_LCONTROL'],
    ControlRight: [228, 'DIK_RCONTROL'], AltLeft: [226, 'DIK_LMENU'], AltRight: [230, 'DIK_RMENU'],
    ContextMenu: [101, 'DIK_APPS'], ScrollLock: [71, 'DIK_SCROLL'], Pause: [72, 'DIK_PAUSE'],
    Insert: [73, 'DIK_INSERT'], Home: [74, 'DIK_HOME'], PageUp: [75, 'DIK_PRIOR'], Delete: [76, 'DIK_DELETE'],
    End: [77, 'DIK_END'], PageDown: [78, 'DIK_NEXT'], ArrowRight: [79, 'DIK_RIGHT'], ArrowLeft: [80, 'DIK_LEFT'],
    ArrowDown: [81, 'DIK_DOWN'], ArrowUp: [82, 'DIK_UP'], NumLock: [83, 'DIK_NUMLOCK'],
    NumpadDivide: [84, 'DIK_DIVIDE'], NumpadMultiply: [85, 'DIK_MULTIPLY'], NumpadSubtract: [86, 'DIK_SUBTRACT'],
    NumpadAdd: [87, 'DIK_ADD'], NumpadEnter: [88, 'DIK_NUMPADENTER'], NumpadDecimal: [99, 'DIK_DECIMAL'],
};
for (let i = 0; i < 26; i++)
    KEYS['Key' + String.fromCharCode(65 + i)] = [4 + i, 'DIK_' + String.fromCharCode(65 + i)];
for (let i = 1; i <= 10; i++)
    KEYS['Digit' + (i % 10)] = [29 + i, 'DIK_' + (i % 10)];
for (let i = 1; i <= 12; i++)
    KEYS['F' + i] = [57 + i, 'DIK_F' + i];
for (let i = 1; i <= 9; i++)
    KEYS['Numpad' + i] = [88 + i, 'DIK_NUMPAD' + i];
KEYS.Numpad0 = [98, 'DIK_NUMPAD0'];

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

test('the table has a value for each of SDL\'s first 232 scancodes', () => {
    assert.ok(table.length >= 232, String(table.length));
    assert.ok(table.every(v => v !== undefined), 'every name in the table is a DIK_ code');
});

test('every key of a standard keyboard reaches the game as its own key', () => {
    const wrong = [];
    for (const [code, [scancode, dik]] of Object.entries(KEYS)) {
        assert.ok(dik in DIK, dik);
        if (table[scancode] !== DIK[dik])
            wrong.push(code + ' is ' + table[scancode] + ', not ' + dik);
    }
    assert.deepEqual(wrong, []);
});

test('Insert, Num Lock and the Menu key reach the game', () => {
    assert.equal(table[73], DIK.DIK_INSERT);
    assert.equal(table[83], DIK.DIK_NUMLOCK);
    assert.equal(table[101], DIK.DIK_APPS);
});

test('a game key that two scancodes share is held while either is', () => {
    // Page Up and SDL's Prior, the two backslash keys, Numpad = and its AS/400 twin.
    const shared = [...new Set(table.filter((d, i) => d && table.indexOf(d) !== i))].sort((a, b) => a - b);
    assert.deepEqual(shared, [DIK.DIK_BACKSLASH, DIK.DIK_NUMPADEQUALS, DIK.DIK_PRIOR]);
    assert.ok(stdControlC.includes('aDikDown[stdControl_aSdlToDik[i]] |= s;'), 'held is or-ed per game key');
    assert.ok(!stdControlC.includes('stdControl_SetSDLKeydown(i, s,'), 'no update per scancode that the other undoes');
});

test('the Controls screen has a name for each key of a standard keyboard', () => {
    const named = new Set([...namesC.matchAll(/\{\s*(DIK_\w+),\s*"DIK_\w+"\s*\}/g)].map(m => DIK[m[1]]));
    // Pause isn't in the original game's list of names. It still binds and works.
    const unnamed = Object.keys(KEYS).filter(code => !named.has(DIK[KEYS[code][1]]));
    assert.deepEqual(unnamed, ['Pause']);
});

test('the Controls screen ignores keys already held when it starts waiting', () => {
    const add = read('src/Gui/jkGUIKeyboard.c');
    assert.ok(/stdControl_Flush\(\);\s*#ifdef SDL2_RENDER[\s\S]{0,120}stdControl_IgnoreHeldKeys\(\);/.test(add),
              'Add calls stdControl_IgnoreHeldKeys');
    assert.ok(stdControlC.includes('stdControl_aDebounce[SDL_SCANCODE_RETURN] = 1;'), 'Enter is debounced by its scancode');
});

// The default keyboard binds, as [function, key, flags].
function defaults() {
    const at = sithControlC.indexOf('void sithControl_RegisterKeyboardBindings()');
    const body = sithControlC.substring(at, sithControlC.indexOf('\n}', at));
    return [...body.matchAll(/sithControl_DefaultHelper\((INPUT_FUNC_\w+), (DIK_\w+), (\d+)\)/g)]
        .map(m => [m[1], m[2], Number(m[3])]);
}

test('each default Turn and Move key is bound once', () => {
    const seen = defaults().map(d => d[1]);
    assert.deepEqual(seen.filter((k, i) => seen.indexOf(k) !== i), []);
});

test('Left Arrow and Numpad 4 turn left, Right Arrow and Numpad 6 turn right', () => {
    const turn = defaults().filter(d => d[0] === 'INPUT_FUNC_TURN').map(d => d[1] + ' ' + d[2]);
    assert.deepEqual(turn.sort(), ['DIK_LEFT 2', 'DIK_NUMPAD4 2', 'DIK_NUMPAD6 6', 'DIK_RIGHT 6']);
    assert.ok(/sithControl_FixNumpadTurn\(\); \/\/ Added\s*return 1;/.test(sithControlC), 'profiles are repaired on load');
});

test('?keycheck, ?netlog, ?bench and the network tests\' options are the page\'s own and not passed to the engine, which quits on them', () => {
    const at = html.indexOf("      Module['arguments'] = window.location.search");
    const code = html.substring(at, html.indexOf('\n      window.onerror', at));
    const run = search => {
        const Module = {};
        new Function('window', 'Module', code)({ location: { search } }, Module);
        return Module;
    };
    assert.deepEqual(run('?keycheck&verbose&bench&path=jkmods/x&netlog&keycheck=1').arguments, ['-verbose', '-path', 'jkmods/x']);
    const net = run('?netstats&netsim=rtt:150,jitter:20&verbose&nettest=attack:bryar&netold');
    assert.deepEqual(net.arguments, ['-verbose']);
    assert.equal(net.jkNetMeasure, true);
    assert.equal(net.jkNetSim, 'rtt:150,jitter:20');
    assert.equal(net.jkNetTest, 'attack:bryar');
    assert.equal(net.jkNetOld, true);
    assert.ok(!('jkNetSim' in run('?netstats')) && !('jkNetTest' in run('?netstats')) && !('jkNetOld' in run('?netstats')));
    assert.ok(!('jkNetMeasure' in run('?netsim=100&nettest=strafe')));
});

test('in fullscreen, Chrome and Edge give Esc, W, T and N to the game', () => {
    assert.ok(html.includes("navigator.keyboard.lock(['Escape', 'KeyW', 'KeyT', 'KeyN'])"));
});

// The console's rule in the browser, as the source has it: the condition after
// "#ifdef ARCH_WASM" in jkQuakeConsole_WmHandler, run as JavaScript (it's
// plain comparisons and && || ! ?:, which mean the same in both).
function consoleToggles() {
    const c = read('src/Main/jkQuakeConsole.c');
    const handler = c.substring(c.indexOf('int jkQuakeConsole_WmHandler('));
    const block = handler.substring(handler.indexOf('#ifdef ARCH_WASM'), handler.indexOf('#else', handler.indexOf('#ifdef ARCH_WASM')));
    const code = block.split('\n').filter(l => !l.trim().startsWith('//') && !l.startsWith('#')).join(' ');
    const at = code.indexOf('else if (') + 'else if '.length;
    let depth = 0, end = at;
    for (; end < code.length; end++) {
        if (code[end] === '(') depth++;
        else if (code[end] === ')' && --depth === 0) break;
    }
    const expr = code.substring(at, end + 1);
    assert.ok(!/SDL_|[{};]/.test(expr), 'only the key, Shift, chat and multiplayer decide: ' + expr);
    const VK = { VK_OEM_3: 0xC0, VK_ESCAPE: 0x1B };
    const f = new Function('VK_OEM_3', 'VK_ESCAPE', 'repeats', 'wParam', 'jkQuakeConsole_bOpen',
                           'jkQuakeConsole_bShiftHeld', 'jkHud_bChatOpen', 'sithNet_isMulti', 'return !!(' + expr + ');');
    return ({ key, open = 0, shift = 0, chat = 0, multi = 1, repeats = 0 }) =>
        f(VK.VK_OEM_3, VK.VK_ESCAPE, repeats, VK[key], open, shift, chat, multi);
}

test('the console opens with Shift+` (~) in a match, as on the desktop, and ` or Esc closes it', () => {
    const toggles = consoleToggles();
    assert.equal(toggles({ key: 'VK_OEM_3', shift: 1 }), true, 'Shift+` opens it');
    assert.equal(toggles({ key: 'VK_OEM_3' }), false, '` on its own is Scores in a match');
    assert.equal(toggles({ key: 'VK_OEM_3', multi: 0 }), true, '` on its own opens it outside multiplayer');
    assert.equal(toggles({ key: 'VK_ESCAPE', shift: 1 }), false, 'not Shift+Esc (Chrome\'s task manager)');
    assert.equal(toggles({ key: 'VK_OEM_3', shift: 1, repeats: 1 }), false, 'not again as the key repeats');
    assert.equal(toggles({ key: 'VK_OEM_3', open: 1 }), true, '` closes it');
    assert.equal(toggles({ key: 'VK_ESCAPE', open: 1 }), true, 'Esc closes it');
});

test('~ and ` type into chat instead of opening the console', () => {
    const toggles = consoleToggles();
    assert.equal(toggles({ key: 'VK_OEM_3', shift: 1, chat: 1 }), false);
    assert.equal(toggles({ key: 'VK_OEM_3', chat: 1, multi: 0 }), false);
});

test('the console key is the one under Esc on every keyboard layout', () => {
    // SDL's key code for that key is the character it types (² on French
    // keyboards, § on Swedish ones), so Window.c also goes by its scancode,
    // as the Scores bind (DIK_GRAVE) does.
    const windowC = read('src/Win95/Window.c');
    const down = /else if \(event\.key\.key == SDLK_GRAVE \|\| event\.key\.scancode == SDL_SCANCODE_GRAVE\)\s*\{\s*Window_msg_main_handler\(g_hWnd, WM_KEYFIRST, VK_OEM_3,/;
    const up = /else if \(event\.key\.key == SDLK_GRAVE \|\| event\.key\.scancode == SDL_SCANCODE_GRAVE\)[^\n]*\s*\{\s*Window_msg_main_handler\(g_hWnd, WM_KEYUP, VK_OEM_3,/;
    assert.ok(down.test(windowC), 'key down');
    assert.ok(up.test(windowC), 'key up');
    assert.equal(table[KEYS.Backquote[0]], DIK.DIK_GRAVE);
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
