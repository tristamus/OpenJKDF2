// Checks a build of the JSPI engine (OPENJKDF2_WASM_JSPI, see
// cmake_modules/toolchain_wasm.cmake) for the ways a JavaScript frame can end
// up under a pause:
//
//   node packaging/wasm/test/jspi-check.mjs <folder with openjkdf2.wasm and openjkdf2.js> [--expect a,b,...]
//        [--reach <wasm-dis>]
//
// Under -sJSPI, Emscripten's glue wraps every wasm import whose name matches
// its importPattern (/^(invoke_.*|__asyncjs__.*)$/ in 6.0.12) or whose
// JavaScript function has .isAsync set in `new WebAssembly.Suspending`. A
// Suspending import entered while a JavaScript frame sits between it and the
// promising export throws "SuspendError: trying to suspend JS frames" (the
// first JSPI build's crash in MotS: libpng's setjmp, an invoke_* inside an
// invoke_*). JavaScript setjmp/longjmp and C++ exceptions are made of invoke_*
// imports, so any of them coming back brings the crash back. This fails
// (exit 1) when:
//   - the wasm imports any invoke_*, emscripten_longjmp,
//     _emscripten_throw_longjmp, __cxa_*, _Unwind_* or __resumeException, or
//     exports setThrew;
//   - the set of imports the glue makes Suspending differs from the expected
//     set (default: emscripten_sleep and the three __asyncjs__Window_wasm*
//     waits in Window.c); a new EM_ASYNC_JS adds one, which goes in --expect
//     if only the engine's own code (main) reaches it;
//   - the glue makes any export other than main/__main_argc_argv promising;
//   - with --reach (binaryen's wasm-dis, from the emsdk: $(em-config
//     BINARYEN_ROOT)/bin/wasm-dis), any other function export (the page's
//     and SDL's calls into the engine, outside main's promise) reaches a
//     Suspending import through direct calls: that pause would throw
//     "SuspendError: trying to suspend without WebAssembly.promising". Calls
//     through function pointers, and callbacks JavaScript calls by their
//     table index, aren't followed; those still need a look by hand when they
//     change (a new emscripten_sleep caller adds no import, so only this
//     catches it, and only on a direct path).
// Exit 2: not a JSPI build. No dependencies but wasm-dis for --reach; reads
// the wasm's import, export and tag sections directly.
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';

const args = process.argv.slice(2);
let dir = null, wasmDis = null;
let expect = ['emscripten_sleep', '__asyncjs__Window_wasmWaitForTick',
  '__asyncjs__Window_wasmMenuHiddenWait', '__asyncjs__Window_wasmEndFrame'];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--expect') expect = args[++i].split(',').filter(Boolean);
  else if (args[i] === '--reach') wasmDis = args[++i];
  else dir = args[i];
}
if (!dir || wasmDis === undefined) {
  console.error('usage: node packaging/wasm/test/jspi-check.mjs <build folder> [--expect a,b,...] [--reach <wasm-dis>]');
  process.exit(2);
}

const wasm = fs.readFileSync(path.join(dir, 'openjkdf2.wasm'));
const glue = fs.readFileSync(path.join(dir, 'openjkdf2.js'), 'utf8');

// ---- wasm sections
let p = 8;
const u32 = () => { let r = 0, s = 0, b; do { b = wasm[p++]; r |= (b & 0x7f) << s; s += 7; } while (b & 0x80); return r >>> 0; };
const str = () => { const n = u32(); const t = wasm.toString('utf8', p, p + n); p += n; return t; };
if (wasm.readUInt32LE(0) !== 0x6d736100) { console.error('not a wasm file'); process.exit(2); }
const imports = [], wexports = [];
let tags = 0, importedTags = 0;
while (p < wasm.length) {
  const id = wasm[p++]; const size = u32(); const end = p + size;
  if (id === 2) {
    const n = u32();
    for (let i = 0; i < n; i++) {
      const mod = str(), name = str(), kind = wasm[p++];
      if (kind === 0) u32();                                  // func: type index
      else if (kind === 1) { p++; const f = u32(); u32(); if (f & 1) u32(); } // table
      else if (kind === 2) { const f = u32(); u32(); if (f & 1) u32(); }      // memory
      else if (kind === 3) { p++; p++; }                      // global: type, mut
      else if (kind === 4) { p++; u32(); importedTags++; }    // tag: attr, type
      else throw new Error('unknown import kind ' + kind);
      imports.push({ mod, name, kind });
    }
  } else if (id === 7) {
    const n = u32();
    for (let i = 0; i < n; i++) { const name = str(); const kind = wasm[p++]; u32(); wexports.push({ name, kind }); }
  } else if (id === 13) {
    tags = u32();
  }
  p = end;
}

// ---- what the glue makes Suspending / promising
const ip = /importPattern\s*=\s*\/(.+?)\/;/.exec(glue);
const ep = /exportPattern\s*=\s*\/(.+?)\/;/.exec(glue);
if (!ip || !ep) { console.error('glue has no Asyncify.instrumentWasmImports/Exports: not a JSPI build?'); process.exit(2); }
const importRe = new RegExp(ip[1]), exportRe = new RegExp(ep[1]);
const asyncFns = new Set([...glue.matchAll(/([A-Za-z_$][\w$]*)\.isAsync\s*=\s*true/g)].map(m => m[1]));
const fnImports = imports.filter(i => i.kind === 0).map(i => i.name);
const suspending = fnImports.filter(n => importRe.test(n) || asyncFns.has(n) || asyncFns.has('_' + n)).sort();
const promising = wexports.filter(e => e.kind === 0 && exportRe.test(e.name)).map(e => e.name);

const bad = [];
const badImport = /^(invoke_|_emscripten_throw_longjmp$|emscripten_longjmp$|__cxa_|_Unwind_|__resumeException$|llvm_eh_typeid_for$)/;
const jsEh = fnImports.filter(n => badImport.test(n));
if (jsEh.length) bad.push(`JS setjmp/longjmp or C++ EH imports (${jsEh.length}): ${jsEh.join(', ')}`);
if (wexports.some(e => e.name === 'setThrew')) bad.push('exports setThrew (JS-based setjmp/longjmp or exceptions)');
const exp = [...expect].sort();
const extra = suspending.filter(n => !exp.includes(n)), missing = exp.filter(n => !suspending.includes(n));
if (extra.length) bad.push(`unexpected Suspending imports: ${extra.join(', ')}`);
if (missing.length) bad.push(`expected Suspending imports missing: ${missing.join(', ')}`);
if (promising.some(n => !/^(main|__main_argc_argv)$/.test(n))) bad.push(`promising exports other than main: ${promising.join(', ')}`);
if (!promising.length) bad.push('no promising export (main)');

// ---- --reach: direct calls from the other exports to a Suspending import
let reachNote = '';
if (wasmDis) {
  const wat = execFileSync(wasmDis, [path.join(dir, 'openjkdf2.wasm')], { maxBuffer: 1 << 30, encoding: 'utf8' });
  const NAME = String.raw`\$(?:"(?:[^"\\]|\\.)*"|[^\s()]+)`;
  const importLine = new RegExp(String.raw`^ \(import "[^"]+" "([^"]+)" \(func (${NAME})`);
  const exportLine = new RegExp(String.raw`^ \(export "([^"]+)" \(func (${NAME})\)\)`);
  const funcLine = new RegExp(String.raw`^ \(func (${NAME})`);
  const callRe = new RegExp(String.raw`\((?:return_)?call (${NAME})`, 'g');
  const importName = new Map(), funcExports = [], calls = new Map();
  let cur = null;
  for (const line of wat.split('\n')) {
    let m;
    if (line.startsWith(' (import ')) { if ((m = importLine.exec(line))) importName.set(m[2], m[1]); cur = null; }
    else if (line.startsWith(' (export ')) { if ((m = exportLine.exec(line))) funcExports.push({ name: m[1], fn: m[2] }); }
    else if (line.startsWith(' (func ')) { cur = funcLine.exec(line)[1]; calls.set(cur, new Set()); }
    else if (line.startsWith(' (') && !line.startsWith(' (func')) cur = null;
    else if (cur) for (const c of line.matchAll(callRe)) calls.get(cur).add(c[1]);
  }
  if (!calls.size) { console.error('wasm-dis gave no functions'); process.exit(2); }
  // SDL_ShouldInit waits (SDL_Delay) only for another thread's SDL_Init; the
  // engine has one thread.
  const SAFE = new Set(['$SDL_ShouldInit>$SDL_Delay']);
  const pause = new Set([...importName].filter(([, field]) => suspending.includes(field)).map(([fn]) => fn));
  const pathTo = start => {
    const from = new Map([[start, null]]), todo = [start];
    while (todo.length) {
      const f = todo.shift();
      if (pause.has(f)) { const p = []; for (let x = f; x; x = from.get(x)) p.unshift(x); return p; }
      for (const c of calls.get(f) || []) if (!from.has(c) && !SAFE.has(f + '>' + c)) { from.set(c, f); todo.push(c); }
    }
    return null;
  };
  const others = funcExports.filter(e => !/^(main|__main_argc_argv)$/.test(e.name));
  for (const e of others) {
    const p = pathTo(e.fn);
    if (p) bad.push(`export ${e.name} reaches ${importName.get(p[p.length - 1])} outside main: ${p.join(' > ')}`);
  }
  const main = funcExports.find(e => /^(main|__main_argc_argv)$/.test(e.name));
  reachNote = `  direct calls: ${others.length} other exports checked` +
              (main && pathTo(main.fn) ? '; main reaches the Suspending imports' : '; main reaches none of them?');
  if (!(main && pathTo(main.fn))) bad.push('main reaches no Suspending import (wasm-dis output not understood?)');
}

console.log(`${dir}`);
console.log(`  imports ${imports.length} (functions ${fnImports.length}), exports ${wexports.length}, wasm tags defined ${tags} / imported ${importedTags}`);
console.log(`  Suspending (${suspending.length}): ${suspending.join(', ')}`);
console.log(`  promising: ${promising.join(', ')}`);
if (reachNote) console.log(reachNote);
if (bad.length) { console.log('  JSPI check FAILED:\n    ' + bad.join('\n    ')); process.exit(1); }
console.log('  JSPI check OK: no invoke_*/JS EH imports, Suspending set as expected, only main is promising' +
            (wasmDis ? ', no other export reaches a Suspending import directly' : ''));
