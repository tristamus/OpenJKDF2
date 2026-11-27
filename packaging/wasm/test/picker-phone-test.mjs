// Tests for the folder prompt and the start window (Choose your mods) on a
// phone, in packaging/wasm/index.html: they scroll inside, under a title bar
// that stays, and what the page is doing with the files shows where it can be
// seen (Antonio, 2026-11-27: on his phone they couldn't be scrolled, and
// choosing the folder seemed to do nothing). Needs Playwright with Chromium,
// as move-test.mjs does:
// node packaging/wasm/test/picker-phone-test.mjs
//
// The page is index.html as it is, served on its own (the engine's files
// aren't there, so the game doesn't start). The game folder is made up by the
// test, with made-up GOBs. Touches are sent as the phone sends them (CDP
// Input.dispatchTouchEvent), since a synthesized scroll gesture doesn't scroll
// what's inside a page in headless Chromium, even where a finger does.

import { mkdtempSync, mkdirSync, writeFileSync, rmSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';
import { makeGob } from './helpers.mjs';

const require = createRequire(import.meta.url);
const { chromium, devices } = require('playwright');

const PORT = 8977, ORIGIN = 'http://127.0.0.1:' + PORT;
const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const server = createServer((req, res) => {
    const body = new URL(req.url, ORIGIN).pathname === '/' ? html : null;
    res.writeHead(body ? 200 : 404, { 'Content-Type': 'text/html' });
    res.end(body || 'not found');
});
await new Promise(resolve => server.listen(PORT, '127.0.0.1', resolve));

// A game folder: the game's files, one of them big enough to be read in
// pieces, and a dozen mods in folders of their own, so the start window is
// taller than a phone.
const root = mkdtempSync(join(tmpdir(), 'picker-phone-'));
const game = join(root, 'Jedi Knight');
function put(rel, bytes) {
    mkdirSync(join(game, rel, '..'), { recursive: true });
    writeFileSync(join(game, rel), bytes);
}
const gob = (name, text, pad = 0) => {
    const g = makeGob([{ name, bytes: Buffer.from(text) }]);
    return pad ? Buffer.concat([Buffer.from(g), Buffer.alloc(pad)]) : g;
};
for (const rel of ['episode/JK1.gob', 'episode/JK1CTF.gob', 'episode/JK1MP.gob', 'resource/Res1hi.gob'])
    put(rel, gob('misc\\x.txt', rel));
put('resource/Res2.gob', gob('misc\\x.txt', 'res2', 40 * 1048576));
put('resource/jk_.cd', Buffer.from([0xC4, 0x32, 0x92, 0x69]));
const MODS = ['BotPack', 'CTFPlus', 'DarkJedi', 'ForceRemix', 'HDModels2', 'MapPack', 'NewSkins', 'SaberFX',
              'SoundFix', 'WeaponsPlus', 'ArenaPack', 'Duelists'];
MODS.forEach((m, i) => put(m + '/' + m.toLowerCase() + '.gob', gob('misc\\m' + i + '.txt', m)));
// A folder without the game: only mods.
const notGame = join(game, 'BotPack');

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

const PHONES = {
    'an upright Android phone': devices['Pixel 7'],
    'a sideways Android phone': { ...devices['Pixel 7'], viewport: { width: 803, height: 360 } },
    'an iPhone': devices['iPhone 13'],
};

const browser = await chromium.launch();

// A page at the given size with the folder prompt open, and what it shows in
// the prompt's status line, as it changes.
async function openPrompt(device) {
    const context = await browser.newContext({ ...device, deviceScaleFactor: 1 });
    const page = await context.newPage();
    await page.goto(ORIGIN + '/', { waitUntil: 'load' });
    await page.evaluate(() => {
        window.statusSeen = [];
        const el = document.getElementById('picker-result');
        new MutationObserver(() => {
            const say = el.querySelector('.say'), bar = el.querySelector('.bar');
            const line = { busy: el.className === 'busy', text: (say || el).textContent,
                           fine: el.querySelector('.fine') ? el.querySelector('.fine').textContent : '',
                           bar: !bar ? null : bar.classList.contains('unknown') ? 'sweep' : parseFloat(bar.firstChild.style.width) };
            const last = window.statusSeen[window.statusSeen.length - 1];
            if (!last || JSON.stringify(last) !== JSON.stringify(line)) window.statusSeen.push(line);
        }).observe(el, { subtree: true, childList: true, characterData: true, attributes: true });
    });
    const touch = !!device.hasTouch;
    await page.locator('[data-play]:visible').first()[touch ? 'tap' : 'click']();
    await page.waitForFunction(() => document.getElementById('picker').open);
    const cdp = touch ? await context.newCDPSession(page) : null;
    return { context, page, cdp };
}

// A finger drawn dy page pixels down (up when negative) over the prompt.
async function drag({ page, cdp }, dy) {
    const box = await page.locator('#picker .body').boundingBox();
    const x = Math.round(box.x + box.width / 2), y = Math.round(box.y + box.height * (dy < 0 ? 0.7 : 0.3));
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y }] });
    for (let i = 1; i <= 10; i++) {
        await cdp.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x, y: Math.round(y + i * dy / 10) }] });
        await page.waitForTimeout(16);
    }
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
    await page.waitForTimeout(600);
}

const layout = page => page.evaluate(() => {
    const d = document.getElementById('picker'), body = d.querySelector('.body');
    const rect = e => { const r = e.getBoundingClientRect(); return { top: r.top, bottom: r.bottom }; };
    const start = document.querySelector('#mod-choice button.play'), result = document.getElementById('picker-result');
    return {
        screen: innerHeight, prompt: rect(d), title: rect(d.querySelector('.titlebar')), body: rect(body),
        promptScrolls: d.scrollHeight > d.clientHeight + 1, bodyOverflow: getComputedStyle(body).overflowY,
        bodyScroll: body.scrollTop, bodyMax: body.scrollHeight - body.clientHeight,
        start: start.offsetParent ? rect(start) : null, result: result.offsetParent ? rect(result) : null,
        title2: document.getElementById('picker-title').textContent,
    };
});
const inSight = (box, body) => box && box.top >= body.top - 1 && box.bottom <= body.bottom + 1;

for (const [phone, device] of Object.entries(PHONES)) {
    await test(phone + ': the folder prompt fits the screen and scrolls inside, under its title bar', async () => {
        const p = await openPrompt(device);
        try {
            let l = await layout(p.page);
            assert.ok(l.prompt.top >= 0 && l.prompt.bottom <= l.screen, 'the prompt is on the screen: ' + JSON.stringify(l.prompt));
            assert.equal(l.promptScrolls, false, 'the prompt itself has nothing to scroll');
            assert.equal(l.bodyOverflow, 'auto');
            assert.ok(l.bodyMax > 0, 'there is more than fits');
            const titleAt = l.title.top;
            await drag(p, 400);
            l = await layout(p.page);
            assert.equal(l.bodyScroll, 0, 'a finger drawn down goes to the top');
            await drag(p, -150);
            l = await layout(p.page);
            assert.ok(l.bodyScroll > 50, 'a finger drawn up scrolls down: ' + l.bodyScroll);
            assert.equal(l.title.top, titleAt, 'the title bar stays');
        } finally {
            await p.context.close();
        }
    });

    await test(phone + ': choosing the folder says it waits for the files, and closing that window puts back what was said', async () => {
        const p = await openPrompt(device);
        try {
            const before = await p.page.locator('#picker-result').textContent();
            assert.match(before, /On a phone or tablet/);
            const chooser = p.page.waitForEvent('filechooser');
            await p.page.locator('#picker label.play').tap();
            await chooser;
            const seen = await p.page.evaluate(() => window.statusSeen.at(-1));
            assert.deepEqual([seen.busy, seen.text, seen.bar], [true, 'Waiting for the folder you choose...', 'sweep']);
            assert.match(seen.fine, /your phone gets its files ready for this page first/);
            const l = await layout(p.page);
            assert.ok(inSight(l.result, l.body), 'the line is in sight: ' + JSON.stringify(l));
            await p.page.evaluate(() => document.getElementById('gamedir').dispatchEvent(new Event('cancel')));
            assert.equal(await p.page.locator('#picker-result').textContent(), before);
            assert.equal(await p.page.locator('#picker-result').getAttribute('class'), '');
        } finally {
            await p.context.close();
        }
    });

    await test(phone + ': the files say they came, the start window opens at its top with Start in sight, and scrolls', async () => {
        const p = await openPrompt(device);
        try {
            const chooser = p.page.waitForEvent('filechooser');
            await p.page.locator('#picker label.play').tap();
            await (await chooser).setFiles(game);
            await p.page.waitForFunction(() => !document.getElementById('mod-choice').hidden);
            const seen = await p.page.evaluate(() => window.statusSeen);
            const got = seen.find(s => /^Got \d+ files from your folder\. Looking through them\.\.\.$/.test(s.text));
            assert.ok(got && got.busy && got.bar === 'sweep', 'said the files came: ' + JSON.stringify(seen));
            let l = await layout(p.page);
            assert.equal(l.title2, 'Choose your mods');
            assert.equal(l.bodyScroll, 0, 'opens at the top, with the mods');
            assert.ok(l.bodyMax > 100, 'more mods than fit');
            assert.ok(inSight(l.start, l.body), 'Start is in sight: ' + JSON.stringify(l));
            await drag(p, -150);
            l = await layout(p.page);
            assert.ok(l.bodyScroll > 50, 'a finger drawn up scrolls the mods: ' + l.bodyScroll);
            assert.ok(inSight(l.start, l.body), 'Start is still in sight');
            await drag(p, 400);
            assert.equal((await layout(p.page)).bodyScroll, 0, 'and back to the top');
        } finally {
            await p.context.close();
        }
    });
}

await test('an upright Android phone: after Start, the bar fills by bytes read, in sight, to 100%', async () => {
    const p = await openPrompt(PHONES['an upright Android phone']);
    try {
        await p.page.setInputFiles('#gamedir', game);
        await p.page.waitForFunction(() => !document.getElementById('mod-choice').hidden);
        await p.page.evaluate(() => { window.statusSeen = []; });
        await p.page.locator('#mod-choice button.play').tap();
        await p.page.waitForFunction(() => window.statusSeen.some(s => / \(100%, 40 of 40 MB\)\.\.\.$/.test(s.text)));
        const seen = await p.page.evaluate(() => window.statusSeen.filter(s => s.busy));
        assert.match(seen[0].text, /^Found your copy of Jedi Knight\. Loading it into this tab( \(0%, 0 of 40 MB\))?\.\.\.$/);
        assert.equal(seen[0].bar, 0);
        const bars = seen.map(s => s.bar);
        assert.ok(bars.every((b, i) => i === 0 || b >= bars[i - 1]), 'the bar only grows: ' + bars);
        assert.ok(bars.filter(b => b > 0 && b < 100).length >= 3, 'a big file moves it on the way: ' + bars);
        assert.equal(bars.at(-1), 100);
    } finally {
        await p.context.close();
    }
});

await test('a sideways Android phone: a folder without the game says so where it can be seen', async () => {
    const p = await openPrompt(PHONES['a sideways Android phone']);
    try {
        await p.page.evaluate(() => { document.querySelector('#picker .body').scrollTop = 0; });
        await p.page.setInputFiles('#gamedir', notGame);
        await p.page.waitForFunction(() => document.getElementById('picker-result').className === 'error');
        await p.page.waitForTimeout(300);
        const l = await layout(p.page);
        assert.ok(l.result.top < l.body.bottom && l.result.bottom <= l.body.bottom + 1,
                  'the error is scrolled to: ' + JSON.stringify(l));
    } finally {
        await p.context.close();
    }
});

await test('a computer: the start window opens at its top, Start in sight, and the prompt is on the screen', async () => {
    const p = await openPrompt({ viewport: { width: 1280, height: 800 } });
    try {
        let l = await layout(p.page);
        assert.ok(l.prompt.top >= 0 && l.prompt.bottom <= l.screen);
        await p.page.setInputFiles('#gamedir', game);
        await p.page.waitForFunction(() => !document.getElementById('mod-choice').hidden);
        l = await layout(p.page);
        assert.equal(l.bodyScroll, 0);
        assert.ok(inSight(l.start, l.body), 'Start is in sight: ' + JSON.stringify(l));
        assert.equal(await p.page.evaluate(() => document.activeElement.textContent), 'Start', 'Start has the keys');
    } finally {
        await p.context.close();
    }
});

await browser.close();
server.close();
rmSync(root, { recursive: true, force: true });
console.log(failed ? failed + ' of ' + (passed + failed) + ' failed' : 'all ' + passed + ' passed');
process.exitCode = failed ? 1 : 0;
