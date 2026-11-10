// Checks a built engine's openjkdf2.js for what the engine's lost-graphics
// recovery (std3D_wasmGlPoll in src/Platform/GL/std3D.c) takes from
// Emscripten's own WebGL code, which isn't Emscripten's public API
// (src/lib/libwebgl.js in emsdk 6.0.12):
//
//   node packaging/wasm/test/glrestore-check.mjs <build folder> [<build folder> ...]
//
// When the browser gives a lost WebGL context back, the poll empties the GL.*
// object tables, the string caches and the context's mirrors, clears
// initExtensionsDone and calls GL.initExtensions again. If a later Emscripten
// renames any of these, or the glue is minified with Closure, the recovery
// breaks only when a player's graphics are lost: a renamed table is skipped
// without a word (the poll's `if (GL[n])`), and a missing GL.initExtensions
// throws inside the engine's frame. This fails (exit 1) when, in any folder's
// openjkdf2.js:
//   - the engine's own functions std3D_wasmGlListen, std3D_wasmGlPoll,
//     std3D_wasmGlIsLost and std3D_wasmGlSay aren't there;
//   - there is no `var GL = {...}`, or a table the poll empties isn't one of
//     its keys holding an array;
//   - a GL.<name> the poll uses is neither a key of GL nor set as
//     GL.<name> = ... elsewhere in the glue;
//   - GL.initExtensions doesn't stop on context.initExtensionsDone and set it
//     (the flag the poll clears so that the extensions are asked for again);
//   - GL.getNewId doesn't hand out names from GL.counter++ (so a name from
//     the old context never reaches an object of the new one);
//   - the glue never sets Module.ctx to the context it makes current (the
//     poll and the engine's other EM_JS read Module.ctx).
// No dependencies; reads the glue as text.
import fs from 'node:fs';
import path from 'node:path';

const dirs = process.argv.slice(2);
if (!dirs.length) {
  console.error('usage: node packaging/wasm/test/glrestore-check.mjs <build folder> [<build folder> ...]');
  process.exit(2);
}

// From src[open] (a '{'), the index just past its matching '}', skipping
// strings, template literals and comments; with keys, the names of the
// object literal's own properties (`name:`, `"name":`, `name(`).
function block(src, open, keys) {
  let depth = 0, i = open, expectKey = false;
  for (; i < src.length; i++) {
    const c = src[i];
    if (c === '"' || c === "'" || c === '`') {
      const q = c; let j = i + 1;
      for (; j < src.length && src[j] !== q; j++) if (src[j] === '\\') j++;
      if (keys && depth === 1 && expectKey) {
        const m = /^\s*:/.exec(src.slice(j + 1, j + 20));
        if (m) keys.add(src.slice(i + 1, j));
        expectKey = false;
      }
      i = j;
      continue;
    }
    if (c === '/' && src[i + 1] === '/') { i = src.indexOf('\n', i); if (i < 0) i = src.length; continue; }
    if (c === '/' && src[i + 1] === '*') { i = src.indexOf('*/', i + 2) + 1; if (i <= 0) i = src.length; continue; }
    if (c === '{' || c === '(' || c === '[') { depth++; if (c === '{' && depth === 1) expectKey = true; continue; }
    if (c === '}' || c === ')' || c === ']') { if (--depth === 0) return i + 1; continue; }
    if (depth === 1 && c === ',') { expectKey = true; continue; }
    if (keys && depth === 1 && expectKey && /[A-Za-z_$]/.test(c)) {
      const m = /^[A-Za-z_$][\w$]*/.exec(src.slice(i, i + 80));
      const after = src.slice(i + m[0].length, i + m[0].length + 20);
      if (/^\s*[:(]/.test(after)) keys.add(m[0]);
      i += m[0].length - 1;
      expectKey = false;
      continue;
    }
    if (!/\s/.test(c)) expectKey = false;
  }
  return -1;
}

function fnBody(src, name) {
  const m = new RegExp('function\\s+' + name + '\\s*\\(').exec(src);
  if (!m) return null;
  const open = src.indexOf('{', m.index);
  const end = block(src, open);
  return end > 0 ? src.slice(open, end) : null;
}

// The value of GL's key name (from just after its ':' to the next key), for a
// look at what it does.
function member(src, glStart, glEnd, name) {
  const re = new RegExp('[{,]\\s*' + name + '\\s*:', 'g');
  re.lastIndex = glStart;
  const m = re.exec(src);
  if (!m || m.index > glEnd) return null;
  const from = m.index + m[0].length;
  const open = src.indexOf('{', from);
  const end = open > 0 ? block(src, open) : -1;
  return end > 0 ? src.slice(from, end) : null;
}

let failed = 0;
for (const dir of dirs) {
  const file = path.join(dir, 'openjkdf2.js');
  const glue = fs.readFileSync(file, 'utf8');
  const bad = [];

  for (const f of ['std3D_wasmGlListen', 'std3D_wasmGlPoll', 'std3D_wasmGlIsLost', 'std3D_wasmGlSay'])
    if (!new RegExp('function\\s+' + f + '\\s*\\(').test(glue)) bad.push('no ' + f + ' (the engine\'s EM_JS in std3D.c)');
  const poll = fnBody(glue, 'std3D_wasmGlPoll') || '';

  const glm = /\bvar\s+GL\s*=\s*\{/.exec(glue);
  const keys = new Set();
  let glStart = -1, glEnd = -1;
  if (!glm) bad.push('no `var GL = {` (Emscripten\'s WebGL library object)');
  else {
    glStart = glm.index + glm[0].length - 1;
    glEnd = block(glue, glStart, keys);
    if (glEnd < 0) bad.push('the GL object has no end');
  }

  if (glEnd > 0) {
    // The tables the poll empties: each must be an array key of GL.
    const list = /\[((?:\s*["'][A-Za-z]+["']\s*,?)+)\]\s*\.forEach/.exec(poll);
    const tables = list ? list[1].match(/[A-Za-z]+/g) : [];
    if (tables.length < 5) bad.push('the poll\'s list of GL tables not found');
    for (const t of tables) {
      if (!keys.has(t)) bad.push('GL.' + t + ' is not in the GL object (renamed table: the poll would skip it)');
      else if (!new RegExp('[{,]\\s*' + t + '\\s*:\\s*\\[').test(glue.slice(glStart, glEnd))) bad.push('GL.' + t + ' is not an array');
    }
    // Every other GL.<name> the poll uses.
    const used = new Set([...poll.matchAll(/\bGL\.([A-Za-z_$][\w$]*)/g)].map(m => m[1]));
    const elsewhere = (glue.slice(0, glStart) + glue.slice(glEnd)).replace(poll, '');
    for (const n of used)
      if (!keys.has(n) && !new RegExp('\\bGL\\.' + n + '\\s*=[^=]').test(elsewhere))
        bad.push('GL.' + n + ' (used by the poll) is neither in the GL object nor set elsewhere');
    for (const n of ['stringCache', 'stringiCache', 'initExtensions', 'currentContext'])
      if (!used.has(n)) bad.push('the poll no longer uses GL.' + n + ' (check std3D_wasmGlPoll against this script)');

    if (!/\.initExtensionsDone\s*=\s*(false|!1)/.test(poll)) bad.push('the poll no longer clears initExtensionsDone');
    const ie = member(glue, glStart, glEnd, 'initExtensions');
    if (!ie) bad.push('GL.initExtensions not found');
    else {
      const p = /^\s*(?:function\s*)?\(?\s*([A-Za-z_$][\w$]*)/.exec(ie);
      const ctx = p ? p[1] : 'context';
      if (!new RegExp('if\\s*\\(\\s*' + ctx + '\\.initExtensionsDone\\s*\\)\\s*return').test(ie) ||
          !new RegExp(ctx + '\\.initExtensionsDone\\s*=\\s*(true|!0)').test(ie))
        bad.push('GL.initExtensions doesn\'t stop on and set initExtensionsDone');
    }
    const gid = member(glue, glStart, glEnd, 'getNewId');
    if (!gid || !/GL\.counter\+\+/.test(gid)) bad.push('GL.getNewId doesn\'t hand out names from GL.counter++');
  }
  if (!/Module(?:\.ctx|\[\s*["']ctx["']\s*\])\s*=\s*GLctx\s*=\s*GL\.currentContext/.test(glue) &&
      !/GLctx\s*=\s*Module(?:\.ctx|\[\s*["']ctx["']\s*\])\s*=\s*GL\.currentContext/.test(glue))
    bad.push('Module.ctx is not set to the current context (makeContextCurrent)');

  console.log(file + ': GL object with ' + keys.size + ' keys');
  if (bad.length) { failed++; console.log('  lost-graphics check FAILED:\n    ' + bad.join('\n    ')); }
  else console.log('  lost-graphics check OK: the GL tables, caches, initExtensions(Done), getNewId and Module.ctx the poll relies on are there');
}
process.exit(failed ? 1 : 0);
