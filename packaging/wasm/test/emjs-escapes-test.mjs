// Checks the JavaScript in the engine's EM_JS bodies for backslashes the C
// compiler changes. An EM_JS body reaches the browser as a C string made
// from the source text, so outside JS quotes "\s" turns into "s" (a regex's
// \s+ then matches the letter s), "\d" into "d", and "\\" into one backslash;
// inside JS quotes (C sees those as character or string literals) a
// backslash is kept as it is. \x.., \u...., \n, \t and \r mean the same to
// both and are fine.
//
//   node test/emjs-escapes-test.mjs

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import assert from 'node:assert/strict';

const root = new URL('../../../src/', import.meta.url).pathname;
function files(dir, out = []) {
    for (const name of readdirSync(dir)) {
        const p = join(dir, name);
        if (statSync(p).isDirectory()) files(p, out);
        else if (/\.(c|cpp|h)$/.test(name)) out.push(p);
    }
    return out;
}

// The bodies: from "EM_JS(" to its closing parenthesis, comments left out.
function bodies(text) {
    const out = [];
    let at = 0;
    for (;;) {
        const a = text.indexOf('EM_JS(', at);
        if (a < 0) break;
        if (/[A-Za-z0-9_]$/.test(text.substring(0, a)) || /define\s+$/.test(text.substring(Math.max(0, a - 20), a))) {
            at = a + 6;
            continue;
        }
        let depth = 0, i = a + 5, quote = '';
        for (; i < text.length; i++) {
            const c = text[i];
            if (quote) {
                if (c === '\\') i++;
                else if (c === quote) quote = '';
                continue;
            }
            if (c === '/' && text[i + 1] === '/') { i = text.indexOf('\n', i); continue; }
            if (c === '/' && text[i + 1] === '*') { i = text.indexOf('*/', i) + 1; continue; }
            if (c === '"' || c === "'") quote = c;
            else if (c === '(') depth++;
            else if (c === ')' && --depth === 0) break;
        }
        out.push({ at: a, body: text.substring(a, i + 1) });
        at = i + 1;
    }
    return out;
}

// Backslashes outside quotes that C changes.
function bad(body) {
    const found = [];
    let quote = '';
    for (let i = 0; i < body.length; i++) {
        const c = body[i];
        if (quote) {
            if (c === '\\') i++;
            else if (c === quote) quote = '';
            continue;
        }
        if (c === '/' && body[i + 1] === '/') { i = body.indexOf('\n', i); if (i < 0) break; continue; }
        if (c === '/' && body[i + 1] === '*') { i = body.indexOf('*/', i) + 1; continue; }
        if (c === '"' || c === "'") { quote = c; continue; }
        if (c === '\\' && !/[xuntr0-7\n]/.test(body[i + 1])) found.push(body.substring(Math.max(0, i - 20), i + 3));
        if (c === '\\') i++;
    }
    return found;
}

let n = 0;
const problems = [];
for (const f of files(root)) {
    const text = readFileSync(f, 'utf8');
    if (!text.includes('EM_JS(')) continue;
    for (const b of bodies(text)) {
        n++;
        for (const p of bad(b.body))
            problems.push(f.substring(root.length) + ':' + (text.substring(0, b.at).split('\n').length) + ': ' + JSON.stringify(p));
    }
}
assert.ok(n > 50, 'found the EM_JS bodies (' + n + ')');
assert.deepEqual(problems, [], 'backslashes the C compiler changes in EM_JS JavaScript');
console.log('ok   ' + n + ' EM_JS bodies keep their backslashes');
console.log('all 1 passed');
