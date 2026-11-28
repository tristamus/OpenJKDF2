// Tests for the room chat in packaging/wasm/index.html (Room chat): the page
// is in a room's chat while its window is open and Chat is on (chat in
// <room> <browser id> [<name>]), only where the lobby says it takes chat; it
// shows the lobby's lines, "name> text", (Enter)> and (Exit)>, what came
// before it did with the time; Enter sends, Shift+Enter makes a second line
// and no more; Ignore is the browser's own; Report; the name box when the
// page has no name; a moderator's link and tools. The lobby's side is
// jkdf2mp-bb-server's test/chat.mjs. Needs Playwright with Chromium, as
// move-test.mjs does:
// node packaging/wasm/test/chat-test.mjs
//
// The page is index.html as it is, served on its own (the game doesn't
// start), and its lobby a stand-in on the page's WebSocket
// (page.routeWebSocket) that answers as the lobby would and keeps what the
// page said.

import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';

const require = createRequire(import.meta.url);
const { chromium, devices } = require('playwright');

const PORT = 8978, ORIGIN = 'http://127.0.0.1:' + PORT;
const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const server = createServer((req, res) => {
    const body = new URL(req.url, ORIGIN).pathname === '/' ? html : null;
    res.writeHead(body ? 200 : 404, { 'Content-Type': 'text/html' });
    res.end(body || 'not found');
});
await new Promise(resolve => server.listen(PORT, '127.0.0.1', resolve));

let passed = 0, failed = 0;
async function test(name, fn) {
    try {
        await fn();
        passed++;
        console.log('ok   ' + name);
    } catch (err) {
        failed++;
        console.log('FAIL ' + name);
        console.log(err);
    }
}

const b64 = t => Buffer.from(t, 'utf8').toString('base64');
const unb64 = t => Buffer.from(t, 'base64').toString('utf8');
const KEY = '0123456789abcdef0123456789abcdef';
const H = 3600 * 1000;

// The lobby stand-in: the lobby line, then what a page's chat lines get.
// said: every line the page sent, in order.
function lobby({ chat = true } = {}) {
    const lb = { said: [], ws: null, room: '', name: '', mod: false, nextId: 1000 };
    lb.send = (op, v) => lb.ws.send('chat ' + op + (v === undefined ? '' : typeof v === 'string' ? ' ' + v : ' ' + b64(JSON.stringify(v))));
    lb.you = () => ({ name: lb.name, muted: 0, mod: lb.mod ? 1 : 0 });
    lb.chats = () => lb.said.filter(l => l.startsWith('chat '));
    lb.route = ws => {
        lb.ws = ws;
        ws.send('lobby links pw match spect feed look' + (chat ? ' chat' : ''));
        ws.onMessage(line => {
            line = String(line);
            lb.said.push(line);
            const a = line.split(' ');
            if (a[0] !== 'chat') return;
            if (a[1] === 'in') {
                const was = lb.room;
                lb.room = a[2];
                lb.name = a[4] ? unb64(a[4]) : '';
                if (lb.taken && lb.name === lb.taken) {
                    lb.name = '';
                    lb.send('log', { room: lb.room, lines: [], slow: false, paused: false, who: { names: [], n: 1 }, you: lb.you() });
                    lb.send('no taken');
                    return;
                }
                if (was !== lb.room) {
                    const now = Date.now();
                    lb.send('log', { room: lb.room, slow: false, paused: false, who: { names: ['DarkJedi_7'], n: 2 }, you: lb.you(),
                                     lines: [{ id: 11, at: now - 26 * H, name: 'Corran_H', text: 'GG last night' },
                                             { id: 12, at: now - H, name: 'DarkJedi_7', text: 'hosting CTF\nanyone?' },
                                             { id: 13, at: now - H / 2, kind: 'note', text: 'Slow mode is off.' }] });
                } else lb.send('you', lb.you());
            } else if (a[1] === 'out') {
                lb.room = '';
            } else if (a[1] === 'say') {
                const text = unb64(a[2]);
                if (/example\.com/.test(text)) return lb.send('no link');
                lb.send('ok');
                lb.send('line', { id: lb.nextId++, at: Date.now(), name: lb.name, text, ...(lb.mod ? { mod: 1 } : {}) });
            } else if (a[1] === 'report') {
                lb.send('reported ' + a[2]);
            } else if (a[1] === 'mod') {
                if (unb64(a[2]) !== KEY) return lb.send('no mod');
                lb.mod = true;
                lb.send('you', lb.you());
                lb.send('mods', { mutes: [{ id: 7, name: 'Spam_Bot_9', until: Date.now() + 2 * H, by: 'mod' }],
                                  reports: [{ id: 8, at: Date.now() - 60000, room: 'nar-shaddaa', line: 12, name: 'DarkJedi_7',
                                              text: 'hosting CTF', by: 'Qu_Rahn' }] });
            }
        });
    };
    return lb;
}

const browser = await chromium.launch();

// A page with the stand-in lobby, its player named Rogue_Nine unless
// noName; store: more localStorage.
async function open({ chat = true, noName = false, hash = '', store = {}, device = null } = {}) {
    const ctx = await browser.newContext(device ? { ...device } : { viewport: { width: 1100, height: 800 } });
    const page = await ctx.newPage();
    page.errors = [];
    page.on('pageerror', e => page.errors.push(String(e)));
    const lb = lobby({ chat });
    await page.routeWebSocket(/\/relay$/, lb.route);
    await page.addInitScript(([noName, store]) => {
        if (sessionStorage.getItem('chat-test-set')) return;
        sessionStorage.setItem('chat-test-set', '1');
        if (noName) localStorage.setItem('jkdf2mp-match-name', 'off');
        else localStorage.setItem('jkdf2mp-player-name', JSON.stringify({ jk1: btoa('Rogue_Nine'), mots: btoa('Rogue_Nine') }));
        for (const k in store) localStorage.setItem(k, store[k]);
    }, [noName, store]);
    await page.goto(ORIGIN + '/' + hash);
    await page.waitForFunction(() => document.documentElement.classList.contains('zone-open'));
    page.lb = lb;
    page.ctx = ctx;
    return page;
}
async function openRoom(page, key = 'nar-shaddaa') {
    await page.evaluate(k => document.querySelector('[data-room="' + k + '"]').click(), key);
}
const logText = page => page.$$eval('#chat-log p', ps => ps.map(p => p.textContent));
async function eventually(fn, what, ms = 2000) {
    const end = Date.now() + ms;
    for (;;) {
        if (await fn()) return;
        if (Date.now() > end) assert.fail(what);
        await new Promise(r => setTimeout(r, 25));
    }
}

await test('no chat where the lobby doesn\'t take it: no Chat button, no panel, nothing said', async () => {
    const page = await open({ chat: false });
    await openRoom(page);
    await page.waitForTimeout(300);
    assert.equal(await page.isVisible('#room-chat'), false);
    assert.equal(await page.isVisible('#room [data-chat]'), false);
    assert.deepEqual(page.lb.chats(), []);
    await page.ctx.close();
});

await test('in a room\'s chat while its window is open: what came before, then the room\'s talk', async () => {
    const page = await open();
    await openRoom(page);
    await eventually(() => page.lb.chats().length > 0, 'chat in');
    const [first] = page.lb.chats();
    assert.match(first, /^chat in nar-shaddaa [0-9a-f]{32} /);
    assert.equal(unb64(first.split(' ')[4]), 'Rogue_Nine');
    await page.waitForSelector('#chat-log p.old');
    const log = await logText(page);
    assert.equal(log[0], 'Before you came in (chat is kept 48 hours)');
    assert.match(log[1], /^Yesterday \d{1,2}:\d\d [AP]MCorran_H> GG last night$/);
    assert.match(log[2], /DarkJedi_7> hosting CTF\nanyone\?$/);
    assert.equal(log[3], '* Slow mode is off.');
    assert.match(log[4], /^You came in at /);
    assert.equal(await page.isVisible('#room [data-chat][aria-pressed=true]'), true);
    // The same browser id next time.
    const bid = first.split(' ')[3];
    await page.click('#room .close');
    await eventually(() => page.lb.chats().at(-1) === 'chat out', 'chat out when the window closes');
    await openRoom(page, 'sith-asteroid');
    await eventually(() => /^chat in sith-asteroid /.test(page.lb.chats().at(-1)), 'chat in the Sith room');
    assert.equal(page.lb.chats().at(-1).split(' ')[3], bid);
    assert.deepEqual(page.errors, []);
    await page.ctx.close();
});

await test('Enter sends, the box clears once the lobby takes it; a turned-down line stays, with why', async () => {
    const page = await open();
    await openRoom(page);
    await page.waitForSelector('#chat-log p.old');
    await page.fill('#chat-text', 'hello room');
    await page.press('#chat-text', 'Enter');
    await eventually(async () => (await page.inputValue('#chat-text')) === '', 'box cleared');
    assert.equal(page.lb.chats().filter(l => l.startsWith('chat say ')).map(l => unb64(l.split(' ')[2]))[0], 'hello room');
    await page.waitForSelector('#chat-log p.me');
    assert.equal(await page.textContent('#chat-log p.me'), 'Rogue_Nine> hello room');
    await page.fill('#chat-text', 'see example.com');
    await page.press('#chat-text', 'Enter');
    await page.waitForSelector('#chat-log p.note');
    assert.match(await page.textContent('#chat-log p.note'), /Links can't be posted in room chat, in any form\. \(only you see this\)/);
    assert.equal(await page.inputValue('#chat-text'), 'see example.com');
    await page.ctx.close();
});

await test('Shift+Enter makes a second line, and only one; pasted lines past that are joined', async () => {
    const page = await open();
    await openRoom(page);
    await page.waitForSelector('#chat-log p.old');
    await page.click('#chat-text');
    await page.keyboard.type('one');
    await page.keyboard.press('Shift+Enter');
    await page.keyboard.type('two');
    await page.keyboard.press('Shift+Enter');
    await page.keyboard.type(' three');
    assert.equal(await page.inputValue('#chat-text'), 'one\ntwo three');
    await page.fill('#chat-text', '');
    await page.evaluate(() => {
        const t = document.getElementById('chat-text');
        t.value = 'a\nb\nc\n\nd';
        t.dispatchEvent(new Event('input'));
    });
    assert.equal(await page.inputValue('#chat-text'), 'a\nb c d');
    assert.equal(await page.textContent('#chat-count'), '7/400');
    await page.ctx.close();
});

await test('Chat in the toolbar turns it off (out of the chat, kept for next time) and on again', async () => {
    const page = await open();
    await openRoom(page);
    await page.waitForSelector('#chat-log p.old');
    await page.click('#room [data-chat]');
    await eventually(() => page.lb.chats().at(-1) === 'chat out', 'chat out');
    assert.equal(await page.isVisible('#room-chat'), false);
    assert.equal(await page.getAttribute('#room [data-chat]', 'aria-pressed'), 'false');
    await page.reload();
    await page.waitForFunction(() => document.documentElement.classList.contains('zone-open'));
    const before = page.lb.chats().length;
    await openRoom(page);
    await page.waitForTimeout(300);
    assert.equal(page.lb.chats().length, before, 'stays off');
    await page.click('#room [data-chat]');
    await eventually(() => /^chat in nar-shaddaa /.test(page.lb.chats().at(-1)), 'chat in again');
    await page.waitForSelector('#room-chat:not([hidden])');
    await page.ctx.close();
});

await test('a name\'s menu: Ignore hides its lines on this browser (Undo, and kept), Report tells the lobby', async () => {
    const page = await open();
    await openRoom(page);
    await page.waitForSelector('#chat-log p.old');
    // Nothing in force: no line under the box.
    assert.equal(await page.isVisible('#chat-rule'), false);
    await page.click('#chat-log [data-chat-who="12"]');
    const items = await page.$$eval('.chat-menu button', bs => bs.map(b => b.textContent));
    assert.deepEqual(items, ['Ignore DarkJedi_7', 'Report this line', 'Cancel']);
    await page.click('.chat-menu button:text("Report this line")');
    await eventually(() => page.lb.chats().includes('chat report 12'), 'chat report');
    await page.waitForSelector('#chat-log p.note');
    assert.match(await page.textContent('#chat-log p.note'), /Reported\. A moderator will look at it\./);
    await page.click('#chat-log [data-chat-who="12"]');
    await page.click('.chat-menu button:text("Ignore DarkJedi_7")');
    assert.equal(await page.$('#chat-log [data-chat-who="12"]'), null, 'hidden');
    assert.equal(await page.evaluate(() => localStorage.getItem('jkdf2mp-chat-ignore')), '["darkjedi_7"]');
    assert.equal(await page.isVisible('#chat-rule'), true);
    assert.match(await page.textContent('#chat-rule'), /^Ignoring 1 name on this browser/);
    await page.click('#chat-log [data-chat-undo]');
    await page.waitForSelector('#chat-log [data-chat-who="12"]');
    // Esc closes the menu, not the room window.
    await page.click('#chat-log [data-chat-who="11"]');
    await page.keyboard.press('Escape');
    assert.equal(await page.$('.chat-menu'), null);
    assert.equal(await page.evaluate(() => document.getElementById('room').open), true);
    await page.ctx.close();
});

await test('without a name: the name box, then in by that name; a name someone has is turned down', async () => {
    const page = await open({ noName: true });
    page.lb.taken = 'DarkJedi_7';
    await openRoom(page, 'bespin-demo');
    await eventually(() => page.lb.chats().length > 0, 'chat in');
    assert.match(page.lb.chats()[0], /^chat in bespin-demo [0-9a-f]{32}$/);
    await page.waitForSelector('#chat-name:not([hidden])');
    assert.equal(await page.isVisible('#chat-say'), false);
    await page.fill('#chat-name-text', 'DarkJedi_7');
    await page.click('#chat-name button[type=submit]');
    await page.waitForFunction(() => document.getElementById('chat-name-why').textContent !== '');
    assert.match(await page.textContent('#chat-name-why'), /Someone else in this room is using that name/);
    await page.fill('#chat-name-text', 'Bespin Fan');
    await page.click('#chat-name button[type=submit]');
    await eventually(() => / /.test(page.lb.chats().at(-1)) && unb64(page.lb.chats().at(-1).split(' ')[4] || '') === 'Bespin Fan', 'in by the name');
    await page.waitForSelector('#chat-say:not([hidden])');
    assert.equal(await page.isVisible('#chat-name'), false);
    assert.equal(await page.evaluate(() => localStorage.getItem('jkdf2mp-watch-name')), 'Bespin Fan');
    await page.ctx.close();
});

// Characters as the game keeps them (mountSaves: IDBFS, a database for each
// game's player/ and persist/ folders): [name, ms ago]; picked: the
// registry's playerShortName.
function saveCharacters(page, chars, picked) {
    return page.evaluate(([chars, picked]) => {
        function put(dbName, files) {
            return new Promise((resolve, reject) => {
                const req = indexedDB.open(dbName, 21);
                req.onupgradeneeded = () => req.result.createObjectStore('FILE_DATA').createIndex('timestamp', 'timestamp', { unique: false });
                req.onsuccess = () => {
                    const tx = req.result.transaction('FILE_DATA', 'readwrite');
                    for (const [path, v] of files) tx.objectStore('FILE_DATA').put(v, path);
                    tx.oncomplete = () => { req.result.close(); resolve(); };
                    tx.onerror = () => reject(tx.error);
                };
                req.onerror = () => reject(req.error);
            });
        }
        const files = [];
        for (const [name, ago] of chars) {
            const at = new Date(Date.now() - ago);
            files.push(['/jk1/player/' + name, { timestamp: at, mode: 16895 }],
                       ['/jk1/player/' + name + '/' + name + '.plr', { timestamp: at, mode: 33206, contents: new Uint8Array(4) }]);
        }
        const reg = new TextEncoder().encode(JSON.stringify({ playerShortName: picked, fullscreen: 0 }));
        return put('/jk1/player', files).then(() => picked === undefined ? null :
            put('/jk1/persist', [['/jk1/persist', { timestamp: new Date(), mode: 16895 }],
                                 ['/jk1/persist/registry.json', { timestamp: new Date(), mode: 33206, contents: reg }]]));
    }, [chars, picked]);
}
async function nameSaid(page) {
    await page.reload();
    await page.waitForFunction(() => document.documentElement.classList.contains('zone-open'));
    page.lb.said.length = 0;
    await openRoom(page);
    await eventually(() => page.lb.chats().some(l => /^chat in nar-shaddaa [0-9a-f]{32} /.test(l)), 'chat in with a name');
    return unb64(page.lb.chats().find(l => /^chat in nar-shaddaa [0-9a-f]{32} /.test(l)).split(' ')[4]);
}

await test('the game\'s character picked last in this browser is the name, before any box', async () => {
    const page = await open({ noName: true });
    await saveCharacters(page, [['Antonio', 0], ['OldGuy', 86400000]], 'oldguy');
    assert.equal(await nameSaid(page), 'OldGuy');
    await page.waitForSelector('#chat-say:not([hidden]) textarea:not([disabled])');
    assert.equal(await page.isVisible('#chat-name'), false, 'never asked');
    await page.ctx.close();
});

await test('without the game\'s pick, the character whose file was written last', async () => {
    const page = await open({ noName: true });
    await saveCharacters(page, [['Antonio', 0], ['OldGuy', 86400000]]);
    assert.equal(await nameSaid(page), 'Antonio');
    await page.ctx.close();
});

await test('a page without saved characters doesn\'t make their database', async () => {
    const page = await open({ noName: true });
    await openRoom(page);
    await page.waitForSelector('#chat-name:not([hidden])');
    const dbs = await page.evaluate(async () => (await indexedDB.databases()).map(d => d.name));
    assert.deepEqual(dbs.filter(n => /player/.test(n)), []);
    await page.ctx.close();
});

await test('a moderator\'s link: kept, out of the address, given in the room; Moderate and the menu\'s tools', async () => {
    const page = await open({ hash: '#chatmod=' + KEY });
    assert.equal(await page.evaluate(() => location.hash), '');
    assert.equal(await page.evaluate(() => localStorage.getItem('jkdf2mp-chat-mod')), KEY);
    // It opens Nar Shaddaa's window.
    await eventually(() => page.lb.chats().includes('chat mod ' + b64(KEY)), 'chat mod after the log');
    const i = page.lb.chats().findIndex(l => l.startsWith('chat mod'));
    assert.match(page.lb.chats()[i - 1], /^chat in nar-shaddaa /);
    await page.waitForSelector('#mod-button:not([hidden])');
    assert.equal(await page.textContent('#mod-button'), 'Moderate (1)');
    await page.click('#chat-log [data-chat-who="12"]');
    const items = await page.$$eval('.chat-menu button', bs => bs.map(b => b.textContent));
    assert.deepEqual(items, ['Ignore', 'Report this line', 'Mute for 1 hour', 'Mute for 24 hours', 'Mute for 7 days',
                             'Delete this line', 'Cancel']);
    await page.click('.chat-menu button:text("Mute for 24 hours")');
    await page.click('#chat-log [data-chat-who="12"]');
    await page.click('.chat-menu button:text("Delete this line")');
    assert.deepEqual(page.lb.chats().slice(-2), ['chat mute 86400 12', 'chat del 12']);
    // The panel.
    await page.click('#mod-button');
    await page.check('.mod-panel input[data-mod=slow]');
    await page.click('.mod-panel button:text("Clear this room\'s chat…")');
    await page.click('.mod-panel button:text("Clear it for everyone")');
    await page.click('.mod-panel button:text("Unmute")');
    await page.click('.mod-panel button:text("Mute 24 h")');
    await eventually(() => page.lb.chats().includes('chat dismiss 8'), 'panel lines');
    assert.deepEqual(page.lb.chats().slice(-5), ['chat slow on', 'chat clear', 'chat unmute 7', 'chat mute 86400 r8', 'chat dismiss 8']);
    await page.keyboard.press('Escape');
    assert.equal(await page.$('.mod-panel'), null);
    assert.equal(await page.evaluate(() => document.getElementById('room').open), true);
    // His lines carry MOD.
    await page.fill('#chat-text', 'keep it friendly');
    await page.press('#chat-text', 'Enter');
    await page.waitForSelector('#chat-log p.me .chat-tag');
    assert.equal(await page.textContent('#chat-log p.me'), 'MODRogue_Nine> keep it friendly');
    await page.ctx.close();
});

await test('a moderator key the lobby turns down is forgotten', async () => {
    const page = await open({ store: { 'jkdf2mp-chat-mod': 'wrongwrongwrong1234' } });
    await openRoom(page);
    await eventually(() => page.lb.chats().some(l => l.startsWith('chat mod ')), 'chat mod');
    await eventually(async () => (await page.evaluate(() => localStorage.getItem('jkdf2mp-chat-mod'))) === null, 'forgotten');
    assert.equal(await page.isVisible('#mod-button'), false);
    await page.ctx.close();
});

await test('muted: the box says until when, and takes nothing', async () => {
    const page = await open();
    await openRoom(page);
    await page.waitForSelector('#chat-log p.old');
    page.lb.send('you', { name: 'Rogue_Nine', muted: Date.now() + H, mod: 0 });
    await page.waitForFunction(() => document.getElementById('chat-text').disabled);
    assert.match(await page.getAttribute('#chat-text', 'placeholder'), /^You're muted in room chat until \d{1,2}:\d\d [AP]M\.$/);
    await page.ctx.close();
});

await test('on a big screen the room window grows as a whole; Moderate and a name\'s menu open by what opened them', async () => {
    const page = await open({ hash: '#chatmod=' + KEY, device: { viewport: { width: 2560, height: 1310 } } });
    await page.waitForSelector('#mod-button:not([hidden])');
    const win = await page.evaluate(() => {
        const r = document.getElementById('room'), b = r.getBoundingClientRect();
        return { zoom: r.style.zoom, w: Math.round(b.width), h: Math.round(b.height) };
    });
    assert.equal(win.zoom, '1.42');
    assert.ok(win.h <= 1310 - 32 && win.h > 1200 && win.w > 1800, JSON.stringify(win));
    await page.click('#mod-button');
    const panel = await page.evaluate(() => {
        const b = document.getElementById('mod-button').getBoundingClientRect();
        const p = document.querySelector('.mod-panel').getBoundingClientRect();
        return { dx: p.left - b.left, dy: p.top - b.bottom };
    });
    assert.ok(Math.abs(panel.dx) < 6 && panel.dy >= 0 && panel.dy < 8, 'under its button: ' + JSON.stringify(panel));
    await page.keyboard.press('Escape');
    await page.click('#chat-log [data-chat-who="12"]');
    const menu = await page.evaluate(() => {
        const b = document.querySelector('#chat-log [data-chat-who="12"]').getBoundingClientRect();
        const m = document.querySelector('.chat-menu').getBoundingClientRect();
        return { dx: m.left - b.right, dy: m.top - b.top };
    });
    assert.ok(menu.dx > 0 && menu.dx < 20 && Math.abs(menu.dy) < 10, 'beside the name: ' + JSON.stringify(menu));
    // Esc on a second menu closes the menu, not the room window (Chrome
    // lets the window's own close be stopped only once between clicks).
    await page.keyboard.press('Escape');
    assert.deepEqual(await page.evaluate(() => [document.getElementById('room').open, !!document.querySelector('.chat-menu')]),
                     [true, false]);
    // A smaller browser window: back to its own size.
    await page.setViewportSize({ width: 1366, height: 657 });
    await page.waitForFunction(() => document.getElementById('room').style.zoom === '');
    assert.equal(await page.evaluate(() => Math.round(document.getElementById('room').getBoundingClientRect().width)), 1284);
    await page.ctx.close();
});

await test('on a phone the chat fits under the room, with the box in sight', async () => {
    const page = await open({ device: devices['Pixel 7'] });
    await openRoom(page);
    await page.waitForSelector('#chat-log p.old');
    const box = await page.locator('#chat-text').boundingBox();
    const vp = page.viewportSize();
    assert.ok(box.y + box.height <= vp.height, 'the box is in the window: ' + JSON.stringify(box));
    assert.equal(await page.getAttribute('#chat-text', 'placeholder'), 'Talk to the room');
    await page.ctx.close();
});

await browser.close();
server.close();
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
