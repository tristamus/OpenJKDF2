// Tests the page's handling of the engine's stdout (Module.print in
// packaging/wasm/index.html) in a real browser. Each line used to be added to
// a text box under the game that was never cleared, so every line made the
// browser lay out everything logged so far, and each HUD message (a pickup, a
// force power) held the game up longer the longer the page was open. Needs
// Playwright with Chromium, as move-test.mjs does:
// node packaging/wasm/test/log-test.mjs
//
// The lines are made up by the test; the game doesn't run.

import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');

const ORIGIN = 'http://127.0.0.1:8971';
const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
const server = createServer((req, res) => {
    const found = new URL(req.url, ORIGIN).pathname === '/';
    res.writeHead(found ? 200 : 404, { 'Content-Type': 'text/html' });
    res.end(found ? html : 'not found');
});
await new Promise(resolve => server.listen(8971, '127.0.0.1', resolve));

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
await page.goto(ORIGIN + '/', { waitUntil: 'load' });
// As in a match: the landing page hidden, the canvas showing.
await page.evaluate(() => { document.getElementById('zone').hidden = true; document.documentElement.classList.remove('zone-open'); });

// A HUD message as jkDev_PrintUniString (src/Main/jkDev.c) writes it, and a
// line of the kind a map change writes hundreds of.
const HUD = '\r                                            \rForce Speed';
const MAP = 'OpenJKDF2: rdMaterial_FreeEntry 3do/mat/kyarm.mat 1f';

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test('the page has no text box for the engine\'s lines', async () => {
    assert.equal(await page.evaluate(() => document.querySelectorAll('#output').length), 0);
    assert.ok(!/id=["']output["']/.test(html), 'index.html has no #output');
});

test('a line costs the same after 50,000 lines as after 100', async () => {
    const r = await page.evaluate(([hud, map]) => {
        const log = console.log;
        console.log = function() {};  // the console's own cost isn't the page's
        const time = n => {
            const t = performance.now();
            for (let i = 0; i < n; i++) Module.print(i % 4 ? map + ' ' + i : hud);
            return (performance.now() - t) / n;
        };
        const early = time(100);
        time(50000);
        const late = time(1000);
        console.log = log;
        return { early, late };
    }, [HUD, MAP]);
    // The old text box took about 260 ms a line at 1 MB of log on the same machine.
    assert.ok(r.late < 1, `a line after 50,000 took ${r.late.toFixed(4)} ms`);
    assert.ok(r.late < Math.max(0.05, r.early * 20), `early ${r.early.toFixed(4)} ms, late ${r.late.toFixed(4)} ms`);
});

test('only the last 2,000 lines are kept, newest last', async () => {
    const r = await page.evaluate(() => {
        Module.print('the last line');
        return { kept: engineLog.length, cap: ENGINE_LOG_LINES, last: engineLog[engineLog.length - 1] };
    });
    assert.equal(r.cap, 2000);
    assert.ok(r.kept >= 2000 && r.kept <= 3000, `${r.kept} lines kept`);
    assert.equal(r.last, 'the last line');
});

test('several arguments are joined with spaces', async () => {
    const last = await page.evaluate(() => { Module.print('a', 'b', 3); return engineLog[engineLog.length - 1]; });
    assert.equal(last, 'a b 3');
});

test('the saved network log ends with the engine\'s last 2,000 lines', async () => {
    const r = await page.evaluate(() => {
        Module.print('engine line to save');
        const lines = netLogText().split('\r\n');
        return { lines, at: lines.indexOf('The engine\'s last 2000 lines:') };
    });
    assert.ok(r.at > 0, 'has the engine section');
    assert.equal(r.lines.length - 1 - (r.at + 1), 2000, 'holds 2,000 lines');
    assert.equal(r.lines[r.lines.length - 2], 'engine line to save');
});

test('"OpenJKDF2:" lines still reach the network log', async () => {
    const r = await page.evaluate(() => {
        const notes = [];
        const was = Module.jkNet;
        Module.jkNet = { note: t => notes.push(t), log: [] };
        Module.print('OpenJKDF2: sithWorld_Load -> m10.jkl');
        Module.jkNet = was;
        return notes;
    });
    assert.deepEqual(r, ['engine: sithWorld_Load -> m10.jkl']);
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
await browser.close();
server.close();
console.log(failed ? `${failed} of ${tests.length} failed` : `all ${tests.length} passed`);
process.exit(failed ? 1 : 0);
