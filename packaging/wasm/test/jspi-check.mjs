// Checks a build of the JSPI engine (OPENJKDF2_WASM_JSPI, see
// cmake_modules/toolchain_wasm.cmake) for the ways a JavaScript frame can end
// up under a pause:
//
//   node packaging/wasm/test/jspi-check.mjs <folder with openjkdf2.wasm and openjkdf2.js> [--expect a,b,...]
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
//     waits in Window.c); a new EM_ASYNC_JS or emscripten_sleep caller is
//     fine where the engine's own code (main) calls it, and goes in --expect;
//   - the glue makes any export other than main/__main_argc_argv promising.
// Exit 2: not a JSPI build. No dependencies; reads the wasm's import, export
// and tag sections directly.
import fs from 'node:fs';
import path from 'node:path';

const args = process.argv.slice(2);
let dir = null;
let expect = ['emscripten_sleep', '__asyncjs__Window_wasmWaitForTick',
  '__asyncjs__Window_wasmMenuHiddenWait', '__asyncjs__Window_wasmEndFrame'];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--expect') expect = args[++i].split(',').filter(Boolean);
  else dir = args[i];
}
if (!dir) { console.error('usage: node packaging/wasm/test/jspi-check.mjs <build folder> [--expect a,b,...]'); process.exit(2); }

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
const ip = /importPattern=\/(.+?)\/;/.exec(glue);
const ep = /exportPattern=\/(.+?)\/;/.exec(glue);
if (!ip || !ep) { console.error('glue has no Asyncify.instrumentWasmImports/Exports: not a JSPI build?'); process.exit(2); }
const importRe = new RegExp(ip[1]), exportRe = new RegExp(ep[1]);
const asyncFns = new Set([...glue.matchAll(/([A-Za-z_$][\w$]*)\.isAsync=true/g)].map(m => m[1]));
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

console.log(`${dir}`);
console.log(`  imports ${imports.length} (functions ${fnImports.length}), exports ${wexports.length}, wasm tags defined ${tags} / imported ${importedTags}`);
console.log(`  Suspending (${suspending.length}): ${suspending.join(', ')}`);
console.log(`  promising: ${promising.join(', ')}`);
if (bad.length) { console.log('  JSPI check FAILED:\n    ' + bad.join('\n    ')); process.exit(1); }
console.log('  JSPI check OK: no invoke_*/JS EH imports, Suspending set as expected, only main is promising');
