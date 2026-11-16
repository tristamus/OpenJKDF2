// Tests for the start page's "From the Mod Library" list in Choose mods
// (packaging/wasm/index.html): the mods the game's Mod Library saved in this
// browser (modlibrary.js, mods.js's library.save), listed on their own, in a
// real browser. Needs Playwright with Chromium, as move-test.mjs does:
// node packaging/wasm/test/library-mods-test.mjs
//
// The page is index.html as it is, served on its own (the engine's files
// aren't there, so the game doesn't start). The saved mods are made up by
// the test and put in the store mods.js keeps them in. The test calls the
// page's own functions in the order onFolderPicked does: detectMods,
// libraryMods, addLibraryMods, placeModFiles, whatsNew, chooseMods, then
// placeGraphicsPacks.

import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';
import { makeGob } from './helpers.mjs';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const PORT = 8974, ORIGIN = 'http://127.0.0.1:' + PORT;
const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const server = createServer((req, res) => {
    const body = new URL(req.url, ORIGIN).pathname === '/' ? html : null;
    res.writeHead(body ? 200 : 404, { 'Content-Type': 'text/html' });
    res.end(body || 'not found');
});
await new Promise(resolve => server.listen(PORT, '127.0.0.1', resolve));

const text = s => Buffer.from(s);
// A saved pack is a GOB of the mod's files by their place in it (mods.js).
const sbxPack = makeGob([{ name: 'cog/force_x.cog', bytes: text('# force') },
                         { name: 'sbx.gob', bytes: makeGob([{ name: 'cog\\saber.cog', bytes: text('# saber') }]) }]);
const oldSbxPack = makeGob([{ name: 'cog/force_x.cog', bytes: text('# old force') }]);
const jkePack = makeGob([{ name: 'JKE.gob', bytes: makeGob([{ name: 'mat\\wall.mat', bytes: text('MAT') }]) }]);
const hostPack = makeGob([{ name: 'cog/host.cog', bytes: text('# host') }]);
const motsPack = makeGob([{ name: 'cog/m.cog', bytes: text('# mots') }]);
const meta = (key, game, name, sha, used, size, extra = {}) =>
    ({ key, game, name, ext: game === 'mots' ? 'goo' : 'gob', sha256: sha, size, used, ...extra });
const SAVED = [
    [meta('jk1/sbx-jk v3.1/' + 'a'.repeat(64), 'jk1', 'SBX-JK v3.1', 'a'.repeat(64), 30, sbxPack.length,
          { lib: 634, title: 'SBX-JK v3.1', author: 'Tester', kind: 'gameplay' }), sbxPack],
    // An older copy of the same mod, saved before the mod was updated.
    [meta('jk1/sbx-jk v3.0/' + 'b'.repeat(64), 'jk1', 'SBX-JK v3.0', 'b'.repeat(64), 10, oldSbxPack.length,
          { lib: 634, title: 'SBX-JK v3.0', author: 'Tester', kind: 'gameplay' }), oldSbxPack],
    [meta('jk1/jk enhanced/' + 'c'.repeat(64), 'jk1', 'JK Enhanced', 'c'.repeat(64), 20, jkePack.length,
          { lib: 705, title: 'JK Enhanced', kind: 'graphics', gp: 'JKE.gob' }), jkePack],
    // A host's set from an online game, with the same pack as the library's SBX.
    [meta('jk1/a host/' + 'a'.repeat(64), 'jk1', 'A Host', 'a'.repeat(64), 40, sbxPack.length), sbxPack],
    [meta('jk1/other host/' + 'd'.repeat(64), 'jk1', 'Other Host', 'd'.repeat(64), 5, hostPack.length), hostPack],
    [meta('mots/mots mod/' + 'e'.repeat(64), 'mots', 'MotS Mod', 'e'.repeat(64), 50, motsPack.length,
          { lib: 801, title: 'MotS Mod', kind: 'gameplay' }), motsPack],
].map(([m, bytes]) => [m, Array.from(bytes)]);
const FOLDER_GOB = Array.from(makeGob([{ name: 'cog\\mine.cog', bytes: text('# mine') }]));

const browser = await chromium.launch();
const page = await browser.newPage();
await page.goto(ORIGIN + '/', { waitUntil: 'load' });
await page.evaluate(async saved => {
    await new Promise((resolve, reject) => {
        const req = indexedDB.open('jkdf2mp-mods', 1);
        req.onupgradeneeded = () => {
            req.result.createObjectStore('meta', { keyPath: 'key' });
            req.result.createObjectStore('data');
        };
        req.onsuccess = () => {
            const tx = req.result.transaction(['meta', 'data'], 'readwrite');
            for (const [m, bytes] of saved) {
                tx.objectStore('meta').put(m);
                tx.objectStore('data').put(new Blob([new Uint8Array(bytes)]), m.key);
            }
            tx.oncomplete = () => { req.result.close(); resolve(); };
            tx.onerror = () => reject(tx.error);
        };
        req.onerror = () => reject(req.error);
    });
}, SAVED);

// What onFolderPicked does up to the start window, for a folder with one mod
// of its own (MyMod). Leaves the window open; window.chosen is its answer.
async function openChooseMods() {
    return page.evaluate(async gob => {
        function picked(path, bytes) {
            const file = new Blob([new Uint8Array(bytes)]);
            Object.defineProperty(file, 'webkitRelativePath', { value: 'Game/' + path });
            Object.defineProperty(file, 'lastModified', { value: 1 });
            return { path, file };
        }
        const files = [picked('MyMod/mymod.gob', gob)];
        const folderMods = detectMods(files, false);
        const saved = await storedMods('jk1');
        await addLibraryMods(await libraryMods('jk1'), folderMods, files, '.gob');
        placeModFiles(files, folderMods);
        const news = whatsNew({ time: 1, files: {} }, files, folderMods, '.gob');
        window.state = { files, folderMods };
        if (!picker.open) picker.showModal();
        window.chosen = chooseMods(folderMods, saved, {}, 'jk1', false, 1, news, null);
        return {
            mods: folderMods.map(m => ({ name: m.name, path: m.path, optional: !!m.optional, bytes: m.bytes,
                                         library: m.library ? m.library.lib : null })),
            files: files.map(e => e.path),
            news: news.mods,
            saved: saved.map(s => s.name),
        };
    }, FOLDER_GOB);
}

// The rows of one of Choose mods' lists: name, tag, where text, ticked.
function rowsOf(list) {
    return page.evaluate(sel => Array.from(document.querySelectorAll('#mod-choice ' + sel + ' label')).map(l => ({
        name: l.querySelector('b').textContent,
        tag: l.querySelector('.tag') ? l.querySelector('.tag').textContent : '',
        where: l.querySelector('.where').textContent,
        on: l.querySelector('input').checked,
        remove: !!l.querySelector('button'),
    })), list);
}
const hidden = sel => page.evaluate(sel => document.querySelector('#mod-choice ' + sel).hidden, sel);
const local = key => page.evaluate(key => localStorage.getItem(key), key);
const savedKeys = () => page.evaluate(() => storedModMetas('jk1', true).then(list => list.map(m => m.key).sort()));

async function start() {
    await page.locator('#mod-choice button.play').click();
    return page.evaluate(async () => (await window.chosen).map(o => ({ name: o.name, index: o.index, saved: !!o.saved })));
}

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test("the library's mods are mods of the folder, each in its own folder, read from the saved pack", async () => {
    const r = await openChooseMods();
    assert.deepEqual(r.mods.map(m => [m.name, m.path, m.optional, m.library]), [
        ['MyMod', 'jkmods/0-MyMod', false, null],
        ['SBX-JK v3.1', 'jkmods/1-SBX-JK_v3.1', false, 634],
        ['JK Enhanced', 'jkmods/2-JK_Enhanced', true, 705],
    ], 'one for each mod in the library (its latest copy), and none of the other game\'s');
    assert.deepEqual(r.files, ['jkmods/0-MyMod/mymod.gob', 'jkmods/1-SBX-JK_v3.1/cog/force_x.cog',
                               'jkmods/1-SBX-JK_v3.1/sbx.gob', 'jkmods/2-JK_Enhanced/JKE.gob']);
    const bytes = await page.evaluate(async () => {
        const e = window.state.files.find(e => e.path.endsWith('force_x.cog'));
        return new TextDecoder().decode(await e.file.arrayBuffer());
    });
    assert.equal(bytes, '# force');
    assert.deepEqual(Object.keys(r.news), ['0'], "the library's mods aren't the folder's news");
    assert.deepEqual(r.saved, ['A Host', 'Other Host'], "the hosts' sets are listed apart, the same pack too");
});

test('they have their own list, unticked and tagged New the first time, with Remove', async () => {
    assert.equal(await hidden('.folder-mods'), false);
    assert.equal(await hidden('.library-mods'), false);
    assert.equal(await hidden('.saved-mods'), false);
    assert.deepEqual(await rowsOf('.mod-list'), [
        { name: 'MyMod', tag: 'New', where: 'In your game folder (MyMod), under 1 MB', on: true, remove: false },
    ]);
    const lib = await rowsOf('.library-list');
    assert.deepEqual(lib.map(r => [r.name, r.tag, r.on, r.remove]),
                     [['SBX-JK v3.1', 'New', false, true], ['JK Enhanced', 'New', false, true]]);
    assert.equal(lib[0].where, 'By Tester, saved from the Mod Library, under 1 MB');
    assert.match(lib[1].where, /^Saved from the Mod Library, under 1 MB\. Graphics pack: only you see it/);
    assert.match(await page.locator('#mod-choice .library-about').textContent(),
                 /Tick one to play with it, on its own or with the mods above\.$/);
    assert.equal(await page.locator('#mod-choice .mod-all').isHidden(), true, 'Select all is for the folder\'s two or more');
});

test('ticked, it starts with the folder\'s mods, below them; the ticks and what was seen are kept', async () => {
    await page.locator('#mod-choice .library-list label', { hasText: 'SBX-JK v3.1' }).locator('input').check();
    const chosen = await start();
    assert.deepEqual(chosen, [{ name: 'MyMod', index: 0, saved: false }, { name: 'SBX-JK v3.1', index: 1, saved: false }]);
    assert.equal(await local('jkdf2mp-library-mods-on-jk1'), '["634"]');
    assert.equal(await local('jkdf2mp-library-mods-seen-jk1'), '["634","705"]');
    assert.equal(await local('jkdf2mp-mods-left-out-jk1'), '[]', "the folder's are remembered as before");
});

test('next time it starts ticked, and nothing is New', async () => {
    await openChooseMods();
    const lib = await rowsOf('.library-list');
    assert.deepEqual(lib.map(r => [r.name, r.tag, r.on]), [['SBX-JK v3.1', '', true], ['JK Enhanced', '', false]]);
    assert.equal((await rowsOf('.mod-list'))[0].on, true);
});

test('a host\'s set picked sets the library\'s aside too', async () => {
    await page.locator('#mod-choice .saved-list label', { hasText: 'Other Host' }).locator('input').check();
    assert.equal(await page.evaluate(() => document.querySelector('#mod-choice .library-mods').disabled), true);
    assert.equal(await page.evaluate(() => document.querySelector('#mod-choice .folder-mods').disabled), true);
    await page.locator('#mod-choice .saved-list label', { hasText: 'None' }).locator('input').check();
    assert.equal(await page.evaluate(() => document.querySelector('#mod-choice .library-mods').disabled), false);
});

test('a ticked graphics pack from the library goes to mods/ like the folder\'s', async () => {
    await page.locator('#mod-choice .library-list label', { hasText: 'JK Enhanced' }).locator('input').check();
    const chosen = await start();
    assert.deepEqual(chosen.map(o => o.name), ['MyMod', 'SBX-JK v3.1', 'JK Enhanced']);
    const placed = await page.evaluate(async () => {
        const picked = await window.chosen;
        const split = placeGraphicsPacks(picked, window.state.folderMods, window.state.files);
        return { mods: split.mods.map(o => o.name), packs: split.packs.map(o => o.name),
                 files: window.state.files.map(e => e.path) };
    });
    assert.deepEqual(placed.mods, ['MyMod', 'SBX-JK v3.1']);
    assert.deepEqual(placed.packs, ['JK Enhanced']);
    assert.ok(placed.files.includes('mods/001-JKE.gob'), placed.files.join(', '));
    assert.equal(await local('jkdf2mp-library-mods-on-jk1'), '["634","705"]');
});

test('Remove forgets every copy saved of it, and it leaves the list', async () => {
    await openChooseMods();
    const before = await savedKeys();
    assert.equal(before.length, 5);
    await page.locator('#mod-choice .library-list label', { hasText: 'SBX-JK v3.1' }).locator('button').click();
    await page.locator('#mod-choice .library-list label', { hasText: 'SBX-JK v3.1' }).waitFor({ state: 'detached' });
    assert.deepEqual(await savedKeys(), before.filter(k => !/^jk1\/sbx-jk v3\.[01]\//.test(k)),
                     "both copies, and not the host's set with the same pack");
    assert.equal(await local('jkdf2mp-library-mods-on-jk1'), '["705"]');
    const chosen = await start();
    assert.deepEqual(chosen.map(o => o.name), ['MyMod', 'JK Enhanced']);
    assert.equal(await local('jkdf2mp-library-mods-seen-jk1'), '["705"]');

    await openChooseMods();
    await page.locator('#mod-choice .library-list label', { hasText: 'JK Enhanced' }).locator('button').click();
    await page.waitForFunction(() => document.querySelector('#mod-choice .library-mods').hidden);
    assert.deepEqual((await start()).map(o => o.name), ['MyMod']);
});

test('without mods of its own in the folder, the list says so', async () => {
    await page.evaluate(async saved => {
        // SBX back in the store.
        await new Promise(resolve => {
            const req = indexedDB.open('jkdf2mp-mods', 1);
            req.onsuccess = () => {
                const tx = req.result.transaction(['meta', 'data'], 'readwrite');
                tx.objectStore('meta').put(saved[0][0]);
                tx.objectStore('data').put(new Blob([new Uint8Array(saved[0][1])]), saved[0][0].key);
                tx.oncomplete = () => { req.result.close(); resolve(); };
            };
        });
    }, SAVED);
    await page.evaluate(async () => {
        const files = [];
        const folderMods = detectMods(files, false);
        await addLibraryMods(await libraryMods('jk1'), folderMods, files, '.gob');
        placeModFiles(files, folderMods);
        if (!picker.open) picker.showModal();
        window.chosen = chooseMods(folderMods, [], {}, 'jk1', false, null, null, null);
    });
    assert.equal(await hidden('.folder-mods'), true);
    assert.equal(await hidden('.library-mods'), false);
    assert.match(await page.locator('#mod-choice .library-about').textContent(), /saved in this browser\. Tick one to play with it\.$/);
    assert.deepEqual((await rowsOf('.library-list')).map(r => [r.name, r.tag, r.on]), [['SBX-JK v3.1', 'New', false]]);
    assert.deepEqual(await start(), []);
});

let failed = 0;
const errors = [];
page.on('pageerror', e => errors.push(String(e)));
for (const t of tests) {
    try {
        await t.fn();
        console.log('ok   ' + t.name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + t.name + '\n' + (e.stack || e));
    }
}
if (errors.length)
    console.log('page errors:\n' + errors.join('\n'));
await browser.close();
server.close();
console.log(failed ? failed + ' of ' + tests.length + ' failed' : 'all ' + tests.length + ' passed');
process.exit(failed ? 1 : 0);
