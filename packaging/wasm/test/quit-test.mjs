// Tests for Quit in packaging/wasm/index.html: the game's Quit brings the
// player back to the lobby once their saves are stored, or after a time
// limit. In Node:
// node packaging/wasm/test/quit-test.mjs
//
// The page's functions are taken from index.html as they are; storing the
// saves is made up by the test.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const mainC = readFileSync(new URL('../../../src/main.c', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const { lobbyAddress, saveThenLeave, QUIT_SAVE_MS } = new Function(
    cut('      var QUIT_SAVE_MS', '      Module.jkQuitToLobby') +
    'return { lobbyAddress, saveThenLeave, QUIT_SAVE_MS };')();

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

const tests = [];
function test(name, fn) { tests.push({ name, fn }); }

// Console warnings are expected in some tests; keep them out of the output.
const warn = console.warn;
const warnings = [];
console.warn = text => warnings.push(text);

test('the lobby address keeps the page and its test options', () => {
    assert.equal(lobbyAddress({ pathname: '/', search: '' }, null), '/');
    assert.equal(lobbyAddress({ pathname: '/', search: '?verbose' }, null), '/?verbose');
    assert.equal(lobbyAddress({ pathname: '/kit/index.html', search: '' }, undefined), '/kit/index.html');
});

test('a game started from a room window comes back to that window', () => {
    assert.equal(lobbyAddress({ pathname: '/', search: '' }, 'nar-shaddaa'), '/#nar-shaddaa');
    assert.equal(lobbyAddress({ pathname: '/', search: '?path=x' }, 'sith-asteroid'), '/?path=x#sith-asteroid');
});

test('the room windows use the keys the page opens from the address', () => {
    for (const key of ['nar-shaddaa', 'sith-asteroid'])
        assert.ok(html.includes("'" + key + "': {"), 'ROOMS has ' + key);
    assert.ok(html.includes('lobbyRoom: lobbyLive.room'), 'a room window remembers itself');
    assert.ok(html.includes('openRoom(location.hash.substring(1))'), 'the page opens the room in the address');
});

test('leaves once the saves are stored', async () => {
    let left = 0, stored = null;
    saveThenLeave(done => { stored = done; }, 200, () => left++);
    assert.equal(left, 0, 'waits for the saves');
    stored(null);
    assert.equal(left, 1);
    await sleep(250);
    assert.equal(left, 1, 'the time limit does not leave again');
});

test('leaves after the time limit if storing hangs', async () => {
    let left = 0, stored = null;
    warnings.length = 0;
    saveThenLeave(done => { stored = done; }, 30, () => left++);
    await sleep(60);
    assert.equal(left, 1);
    assert.equal(warnings.length, 1);
    stored(null);
    assert.equal(left, 1, 'saves stored late do not leave again');
});

test('leaves when storing fails or throws', async () => {
    let left = 0;
    warnings.length = 0;
    saveThenLeave(done => done(new Error('quota')), 200, () => left++);
    saveThenLeave(() => { throw new Error('no IndexedDB'); }, 200, () => left++);
    assert.equal(left, 2);
    assert.equal(warnings.length, 2);
    await sleep(250);
    assert.equal(left, 2);
});

test('the page waits a few seconds at most', () => {
    assert.ok(QUIT_SAVE_MS >= 2000 && QUIT_SAVE_MS <= 10000, String(QUIT_SAVE_MS));
});

test('the engine and the page use the same name', () => {
    assert.ok(mainC.includes('Module.jkQuitToLobby()'), 'main.c calls it');
    assert.ok(html.includes('Module.jkQuitToLobby = function'), 'index.html defines it');
    assert.ok(html.includes('leaveGuard = false;  // no "Leave site?"'), 'Quit turns off the leave question');
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
console.warn = warn;
console.log(failed ? `${failed} of ${tests.length} failed` : `all ${tests.length} passed`);
process.exit(failed ? 1 : 0);
