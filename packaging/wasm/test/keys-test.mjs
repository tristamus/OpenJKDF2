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

test('?keycheck is the page\'s own and is not passed to the engine, which quits on it', () => {
    const at = html.indexOf("      Module['arguments'] = window.location.search");
    const code = html.substring(at, html.indexOf('\n      window.onerror', at));
    const Module = {};
    new Function('window', 'Module', code)({ location: { search: '?keycheck&verbose&path=jkmods/x' } }, Module);
    assert.deepEqual(Module.arguments, ['-verbose', '-path', 'jkmods/x']);
});

test('in fullscreen, Chrome and Edge give Esc, W, T and N to the game', () => {
    assert.ok(html.includes("navigator.keyboard.lock(['Escape', 'KeyW', 'KeyT', 'KeyN'])"));
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
