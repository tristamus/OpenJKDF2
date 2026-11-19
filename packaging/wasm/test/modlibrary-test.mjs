// Tests for packaging/wasm/modlibrary.js, the page's half of the Mod Library
// (src/Gui/jkGUILibrary.c is the game menu that draws it), in Node (22 or
// newer, for WebCrypto and DecompressionStream):
// node packaging/wasm/test/modlibrary-test.mjs
//
// The test calls Module.jkModLibrary as the menu does: begin(), then text()
// and number() for what to draw, query(), select(), use() and cancel() for
// what the player does, then take() and end(). The mods are made up by the
// test and served the way the site's Worker serves them (src/levels.js in
// jkdf2mp-bb-server: /levels/mods/<game>.json, /levels/d/<id>.json with
// detail.mod, /levels/z/<id>.zip). maps.js, mods.js and library.js run as
// they are, on the in-memory file system and IndexedDB of helpers.mjs.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import assert from 'node:assert/strict';
import { makeGob, makeZip, makeFS, makeIDB, sha } from './helpers.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const source = name => readFileSync(join(here, '..', name), 'utf8');
const SOURCES = ['maps.js', 'mods.js', 'library.js', 'modlibrary.js'].map(source);
const text = s => new TextEncoder().encode(s);

// ---- The game's own files: Res2.gob, with a few of the game's names.

const RES2 = makeGob([
    { name: 'cog\\force_pull.cog', bytes: text('# pull') },
    { name: 'cog\\force_push.cog', bytes: text('# push') },
    { name: '3do\\kyle.3do', bytes: text('3DO kyle') },
    { name: '3do\\key\\kyrun.key', bytes: text('KEY run') },
    { name: 'ui\\bkmain.bm', bytes: text('BM main') },
]);
const GAME = { '/jk1/resource/Res2.gob': RES2, '/jk1/episode/JK1MP.gob': makeGob([{ name: 'episode.jk', bytes: text('"JK MP"\nTYPE 2\n') }]) };
const MOTS = { '/mots/resource/Jkmres.goo': makeGob([{ name: 'cog\\force_pull.cog', bytes: text('# mots pull') }]) };

// ---- Made-up mods: their zips, their index entries and their details, as
// the Worker's modPlan and modKind would make them.

function episodeGob(title, type) {
    return makeGob([{ name: 'episode.jk', bytes: text(`"${title}"\nTYPE ${type}\nSEQ 1\n`) },
                    { name: 'jkl\\' + title.toLowerCase().replace(/\W+/g, '') + '.jkl', bytes: text('# level\n'.repeat(10)) }]);
}

function mod(id, game, k, t, files, plan, extra = {}) {
    const zip = makeZip(files);
    const eps = plan.episodes || [];
    const gobs = files.filter(f => /\.go[bo]$/i.test(f.name) && eps.includes(f.name))
                      .map(f => ({ n: f.name, h: sha(f.bytes), b: f.bytes.length, ep: f.ep, ty: f.ty }));
    const entry = {
        id, k, t, a: extra.a || 'Tester ' + id, rel: extra.rel || '1999-05-02', rp: 'd', src: 'r', add: '2000-06-02',
        sz: zip.length, r: 7, v: 3, dl: extra.dl || 10, sh: extra.sh === undefined ? 2 : extra.sh,
        z: t.toLowerCase().replace(/\W+/g, '') + '.zip', h: extra.h || sha(zip), gk: extra.gk || t.toLowerCase() + '|tester',
        n: (plan.files || []).length, ep: eps.length || undefined, x: (plan.left && plan.left.programs.length) || undefined,
        gp: extra.gp, g: gobs,
    };
    const detail = {
        id, title: t, author: entry.a, desc: 'About ' + t + '.', zip: entry.z, size: zip.length, sha256: sha(zip),
        files: files.map(f => ({ n: f.name, b: f.bytes.length })),
        texts: [{ n: 'readme.txt', t: 'The readme of ' + t }],
        mod: {
            kind: k, pack: extra.gp || null, episodes: gobs.map(g => ({ n: g.n, ep: g.ep, ty: g.ty })),
            programs: (plan.left && plan.left.programs) || [],
            plan: { inner: plan.inner, files: plan.files || [], episodes: eps },
            left: plan.left || { programs: [], other: 0, others: [] }, inside: plan.inside || {},
        },
    };
    return { entry, detail, zip, game };
}

const sbxGob = makeGob([{ name: 'cog\\force_push.cog', bytes: text('# sbx push') }, { name: '3do\\new.3do', bytes: text('3DO new') }]);
const MODS = [
    mod(701, 'jk1', 'gameplay', 'SBX-JK v3.1', [
        { name: 'SBX/sbx.gob', bytes: sbxGob },
        { name: 'SBX/cog/force_pull.cog', bytes: text('# sbx pull') },
        { name: 'SBX/cog/new.cog', bytes: text('# new') },
        { name: 'SBX/readme.txt', bytes: text('readme') },
        { name: 'SBX/setup.exe', bytes: text('MZ') },
    ], {
        files: [['SBX/sbx.gob', 'sbx.gob'], ['SBX/cog/force_pull.cog', 'cog/force_pull.cog'], ['SBX/cog/new.cog', 'cog/new.cog']],
        left: { programs: ['SBX/setup.exe'], other: 0, others: [] },
        inside: { 'SBX/sbx.gob': [['cog\\force_push.cog', 10], ['3do\\new.3do', 7]] },
    }, { gk: 'sbx jk|sbx team', a: 'SBX Team', rel: '2005-03-01', dl: 500 }),
    mod(702, 'jk1', 'gameplay', 'SBX-JK v3.0', [{ name: 'cog/old.cog', bytes: text('# old') }],
        { files: [['cog/old.cog', 'cog/old.cog']] }, { gk: 'sbx jk|sbx team', a: 'SBX Team', rel: '2004-01-01', dl: 900 }),
    mod(703, 'jk1', 'characters', 'Kyle Skin', [{ name: '3do/kyle.3do', bytes: text('3DO new kyle') }],
        { files: [['3do/kyle.3do', '3do/kyle.3do']] }, { dl: 800, sh: 1 }),
    mod(704, 'jk1', 'campaign', 'Big Campaign', [{ name: 'campaign.gob', bytes: episodeGob('Big Campaign', 1), ep: 'Big Campaign', ty: 1 }],
        { episodes: ['campaign.gob'] }, { dl: 50 }),
    mod(705, 'jk1', 'graphics', 'JK Enhanced', [{ name: 'JKE.gob', bytes: makeGob([{ name: 'mat\\wall.mat', bytes: text('MAT') }]) }],
        { files: [['JKE.gob', 'JKE.gob']] }, { gp: 'JKE.gob', dl: 300 }),
    mod(706, 'jk1', 'gameplay', 'MP Pack', [{ name: 'mppack.gob', bytes: episodeGob('MP Pack', 2), ep: 'MP Pack', ty: 2 }],
        { episodes: ['mppack.gob'] }, { dl: 700, sh: 0 }),
    mod(707, 'jk1', 'gameplay', 'Broken Download', [{ name: 'cog/b.cog', bytes: text('# b') }],
        { files: [['cog/b.cog', 'cog/b.cog']] }, { dl: 100, h: 'f'.repeat(64) }),
    mod(801, 'mots', 'gameplay', 'MotS Saber Mod', [{ name: 'cog/force_pull.cog', bytes: text('# saber') }],
        { files: [['cog/force_pull.cog', 'cog/force_pull.cog']] }, { dl: 20 }),
];
// One of jkdf2.net's: no page, rating or downloads, its files loose in its
// zip (placed by kind), named by its zip, found by its models' names.
const masters = mod(1000123, 'jk1', 'characters', 'Masters', [{ name: 'skeletor.3do', bytes: text('3DO skeletor') },
                                                               { name: 'heman.3do', bytes: text('3DO heman') }],
                    { files: [['skeletor.3do', '3do/skeletor.3do'], ['heman.3do', '3do/heman.3do']] }, { sh: 0 });
Object.assign(masters.entry, { o: 'jkdf2.net', z: 'MASTERS.ZIP', f: ['skeletor', 'heman'], ty: true, add: undefined, r: undefined,
                               v: undefined, dl: undefined });
Object.assign(masters.detail, { from: 'jkdf2.net', url: 'https://www.jkdf2.net/JKBot/JKArchive/JKDF2/JKDF2/Game%20Files/jkmod/skins/MASTERS.ZIP' });
const DF2_MODS = [masters];
const byId = Object.fromEntries(MODS.concat(DF2_MODS).map(m => [m.entry.id, m]));
// Whether the site has the list of both sources' mods (a site from before it doesn't).
const SITE = { all: false };

// ---- The site, as fetch() sees it.

const requests = [];
async function siteFetch(url, opts = {}) {
    const method = opts.method || 'GET';
    const path = String(url).split('?')[0];
    requests.push(method + ' ' + url);
    const json = body => new Response(method === 'HEAD' ? null : JSON.stringify(body),
                                      { status: 200, headers: { 'Content-Type': 'application/json' } });
    let m;
    if (SITE.all && (m = /^\/levels\/mods\/all\/(jk1|mots)\.json$/.exec(path)))
        return json({ v: 1, game: m[1], built: '2026-11-17T00:00:00Z', source: 'https://www.massassi.net',
                      sources: ['https://www.massassi.net', 'https://www.jkdf2.net'], listed: 10, left: 1, from: { 'jkdf2.net': 1 },
                      mods: MODS.concat(DF2_MODS).filter(x => x.game === m[1]).map(x => x.entry) });
    if ((m = /^\/levels\/mods\/(jk1|mots)\.json$/.exec(path)))
        return json({ v: 1, game: m[1], built: '2026-11-15T00:00:00Z', source: 'https://www.massassi.net', listed: 9, left: 1,
                      mods: MODS.filter(x => x.game === m[1]).map(x => x.entry) });
    if ((m = /^\/levels\/d\/(\d+)\.json$/.exec(path)) && byId[m[1]])
        return json(byId[m[1]].detail);
    if ((m = /^\/levels\/z\/(\d+)\.zip$/.exec(path)) && byId[m[1]])
        return new Response(method === 'HEAD' ? null : byId[m[1]].zip, { status: 200, headers: { 'Content-Type': 'application/zip' } });
    return new Response('not found', { status: 404, headers: { 'Content-Type': 'text/plain' } });
}

// ---- A tab: the game's files, then the scripts as openjkdf2.js runs them,
// then their preRun steps.

async function makeTab({ files = GAME, args = [], idb = makeIDB(), cwd = '/jk1' } = {}) {
    const tab = {
        FS: makeFS({ ...files }, cwd), arguments: args, preRun: [], idb, deps: new Set(),
        addRunDependency(d) { this.deps.add(d); },
        removeRunDependency(d) { this.deps.delete(d); },
    };
    const location = { protocol: 'https:' }, document = {};
    const kept = new Map();
    tab.local = { getItem: k => (kept.has(k) ? kept.get(k) : null), setItem: (k, v) => kept.set(k, String(v)) };
    for (const src of SOURCES)
        new Function('Module', 'indexedDB', 'fetch', 'location', 'document', 'localStorage', src)(
            tab, idb, siteFetch, location, document, tab.local);
    for (const step of tab.preRun)
        step();
    await until(() => !tab.deps.size, 'the preRun steps');
    return tab;
}

async function until(fn, what, ms = 10000) {
    const end = Date.now() + ms;
    while (!(await fn())) {
        if (Date.now() > end)
            throw new Error('timed out waiting for ' + what);
        await new Promise(r => setTimeout(r, 5));
    }
}

const T = { STATUS: 0, ROW: 1, SORT: 2, TITLE: 3, OVERVIEW: 4, README: 5, FILES: 6, MESSAGE: 7, CHANGES: 14, USE: 15, USE_HINT: 16 };
const N = { CHANGED: 0, COUNT: 1, SELECTED: 2, CAN_USE: 3, BUSY: 4, DONE: 5, SHOTS: 6, PV: 7 };
const F = { GAMEPLAY: 1, CHARACTERS: 2, GRAPHICS: 4, MINE: 8, CAMPAIGNS: 16 };
const ALL_KINDS = F.GAMEPLAY | F.CHARACTERS | F.GRAPHICS;

const L = tab => tab.jkModLibrary;
const rows = tab => Array.from({ length: L(tab).number(N.COUNT) }, (_, i) => L(tab).text(T.ROW, i));
const titleOf = row => row.replace(/^ +/, '').replace(/^\* /, '').split('\t')[0];

async function open(tab, game = 'jk1', mask = ALL_KINDS) {
    assert.equal(L(tab).begin(game), true);
    L(tab).query('', mask, 0);
    await until(() => L(tab).number(N.COUNT) > 0 || /saved/.test(L(tab).text(T.STATUS)), 'the list');
}

function pick(tab, title) {
    const at = rows(tab).map(titleOf).indexOf(title);
    assert.ok(at >= 0, title + ' is in the list: ' + JSON.stringify(rows(tab)));
    L(tab).select(at);
    assert.equal(L(tab).number(N.SELECTED), at);
}

const details = tab => until(() => !/^Getting /m.test(L(tab).text(T.OVERVIEW)) && !/^Getting /.test(L(tab).text(T.CHANGES)), 'the details');
const settled = tab => until(() => !L(tab).number(N.BUSY), 'the job');

const fileSha = (tab, path) => sha(tab.FS.readFile(path));

let passed = 0;
async function test(name, fn) {
    try {
        await fn();
        passed++;
        console.log('ok - ' + name);
    } catch (e) {
        console.log('not ok - ' + name);
        console.log(e && e.stack || e);
        process.exitCode = 1;
    }
}

// ---- The tests

const idb = makeIDB();
const tab = await makeTab({ idb });
let sbxFp = null;

await test('available once the site answers, with maps.js, mods.js and library.js there', async () => {
    await until(() => L(tab).available(), 'available');
    assert.ok(requests.includes('HEAD /levels/mods/jk1.json'));
});

await test('the list: each mod once, its versions under it, newest first, sorted by downloads', async () => {
    await open(tab);
    assert.equal(L(tab).text(T.MESSAGE), '', 'no standing line over the hint line');
    assert.equal(L(tab).text(T.SORT), 'Most downloaded');
    assert.deepEqual(rows(tab).map(r => r.split('\t')[0]),
                     ['Kyle Skin', 'MP Pack', 'SBX-JK v3.1', '  SBX-JK v3.0', 'JK Enhanced', 'Broken Download']);
    const r = rows(tab);
    assert.match(r[0], /^Kyle Skin\t1999 \d+ (KB|B)\tSkin$/);
    assert.match(r[4], /\tGFX$/);
    assert.equal(L(tab).text(T.STATUS), '7 mods from Massassi.net so far, 1 more coming');
    assert.equal(L(tab).number(N.SELECTED), 0, 'the first row picked');
    assert.equal(L(tab).number(N.PV), 0, 'no 3D view');
});

await test('filters: Campaigns, Characters off, My mods', async () => {
    L(tab).query('', ALL_KINDS | F.CAMPAIGNS, 0);
    assert.ok(rows(tab).some(r => /^Big Campaign\t.*\tSP$/.test(r)), 'the campaign with Campaigns ticked');
    L(tab).query('', F.GAMEPLAY | F.GRAPHICS, 0);
    assert.ok(!rows(tab).some(r => r.startsWith('Kyle Skin')), 'no skins with Characters off');
    L(tab).query('', ALL_KINDS | F.MINE, 0);
    assert.equal(L(tab).number(N.COUNT), 0);
    assert.equal(L(tab).text(T.STATUS), 'No mods saved in this browser yet.');
    assert.equal(L(tab).text(T.TITLE), 'My mods');
    assert.match(L(tab).text(T.OVERVIEW), /^No mods saved in this browser yet\./);
    L(tab).query('sbx', ALL_KINDS, 1);
    assert.deepEqual(rows(tab).map(titleOf), ['SBX-JK v3.1', 'SBX-JK v3.0'], 'search, by name');
    L(tab).query('', ALL_KINDS, 0);
});

await test('the Overview: kind, versions, what Get Mod leaves out; both screenshots', async () => {
    pick(tab, 'SBX-JK v3.1');
    await details(tab);
    const o = L(tab).text(T.OVERVIEW);
    assert.match(o, /^Author:\tSBX Team$/m);
    assert.match(o, /^Kind:\tGameplay$/m);
    assert.match(o, /^Size:\t.* zip, 3 files for the game$/m);
    assert.match(o, /^Versions:\tthis is the newest of 2; the older ones are under it in the list$/m);
    assert.match(o, /players who join get it from you/);
    assert.match(o, /1 program, which the browser can't run, so it's left out/);
    assert.match(o, /About SBX-JK v3\.1\./);
    assert.equal(L(tab).number(N.SHOTS), 2);
    assert.equal(L(tab).text(T.USE), 'Get Mod');
    assert.ok(L(tab).text(T.USE_HINT).length <= 70, 'one line on the hint line');
    assert.equal(L(tab).number(N.CAN_USE), 1);
    pick(tab, 'SBX-JK v3.0');
    await details(tab);
    assert.match(L(tab).text(T.OVERVIEW), /^Versions:\tan older one \(2 of 2\); the newest is SBX-JK v3\.1, Mar 1, 2005$/m);
});

await test('Changes: by name against the game\'s own files, inside the mod\'s GOBs too', async () => {
    pick(tab, 'SBX-JK v3.1');
    await details(tab);
    const c = L(tab).text(T.CHANGES).split('\n');
    assert.ok(c.includes("Replaces:\t2 of the game's own files: 2 scripts"), c.join('\n'));
    assert.ok(c.includes('  cog: force_push.cog, force_pull.cog'), c.join('\n'));
    assert.ok(c.includes('Adds:\t2 new files: 1 model, 1 script'), c.join('\n'));
    assert.ok(c.includes("Left out:\tsetup.exe: programs, which the browser can't run."), c.join('\n'));
    pick(tab, 'Kyle Skin');
    await details(tab);
    assert.ok(L(tab).text(T.CHANGES).split('\n').includes("Replaces:\t1 of the game's own files: 1 model"));
    assert.equal(L(tab).number(N.SHOTS), 1);
    // By folder: 3do/key's animations apart from 3do's models.
    const nested = L(tab)._changesText({}, { mod: { plan: { files: [['m/3do/kyle.3do', '3do/kyle.3do'],
                                                                     ['m/3do/key/kyrun.key', '3do/key/kyrun.key']] } } }).split('\n');
    assert.ok(nested.includes('  3do: kyle.3do') && nested.includes('  3do/key: kyrun.key'), nested.join('\n'));
});

await test('a Word readme without the line of Word\'s own bytes before its words', async () => {
    const u = tab.jkLibrary.util;
    assert.equal(u.readmeBody({ n: 'README.DOC', t: '\u0000bjbjU\u0001\u0002\nSaber Battle X\r\n' }), 'Saber Battle X');
    assert.equal(u.readmeBody({ n: 'readme.txt', t: 'bjbj stays in a text file\n' }), 'bjbj stays in a text file');
});

await test('which places a pack keeps, as mods.js packs a folder', async () => {
    const ok = L(tab)._packablePath;
    assert.equal(ok('cog/x.cog'), true);
    assert.equal(ok('sbx.gob'), true);
    assert.equal(ok('cog/readme.txt'), false);
    assert.equal(ok('.hidden/x.cog'), false);
    assert.equal(ok('old.zip/x.cog'), false);
    assert.equal(ok('a/b/c/d/e/f/g/h/i/x.cog'), false);
});

await test('Get Mod: the plan\'s files packed with the fingerprint the same folder has, saved', async () => {
    pick(tab, 'SBX-JK v3.1');
    assert.equal(L(tab).use(), true);
    assert.equal(L(tab).number(N.BUSY), 1);
    assert.equal(L(tab).number(N.CAN_USE), 0, 'one job at a time');
    await settled(tab);
    assert.match(L(tab).text(T.MESSAGE), /^Saved in this browser\. Restart With It plays it now\.$/);
    assert.equal(L(tab).text(T.USE), 'Restart With It');
    assert.match(L(tab).text(T.USE_HINT), /only this mod/);
    assert.ok(L(tab).text(T.USE_HINT).length <= 70, 'one line on the hint line');
    assert.equal(L(tab).number(N.CAN_USE), 1);
    assert.ok(rows(tab).includes(rows(tab).find(r => r.startsWith('* SBX-JK v3.1'))), 'marked saved: ' + rows(tab));
    const saved = await tab.jkMods.library.list('jk1');
    assert.equal(saved.length, 1);
    assert.equal(saved[0].lib, 701);
    assert.equal(saved[0].title, 'SBX-JK v3.1');
    sbxFp = saved[0].sha256;
    // The same files in a folder: the same fingerprint, so a joiner with
    // the folder, or the page's Choose mods, matches it.
    const dir = '/jk1/jkmods/0-sbx';
    tab.FS.writeFile(dir + '/sbx.gob', sbxGob);
    tab.FS.writeFile(dir + '/cog/force_pull.cog', text('# sbx pull'));
    tab.FS.writeFile(dir + '/cog/new.cog', text('# new'));
    tab.FS.writeFile(dir + '/readme.txt', text('not packed'));
    assert.equal(sha(tab.jkMods.lib.pack(dir).bytes), sbxFp);
    assert.ok(!tab.FS.files.has('/jk1/episode/sbx.gob'), 'nothing in episode/');
});

await test('Restart With It: unpacked into netmods/, the running mod from then on', async () => {
    assert.equal(L(tab).use(), true);
    await until(() => L(tab).number(N.DONE), 'done');
    const path = L(tab).take();
    assert.equal(path, 'netmods/' + sbxFp.substring(0, 16));
    assert.equal(fileSha(tab, '/jk1/' + path + '/cog/new.cog'), sha(text('# new')));
    assert.equal(fileSha(tab, '/jk1/' + path + '/sbx.gob'), sha(sbxGob));
    assert.equal(tab.jkMods.current().fp, sbxFp);
    assert.equal(tab.jkMods.current().name, 'SBX-JK v3.1');
    assert.ok(tab.jkMods.tail().length > 0, 'a host announces it');
    assert.equal(tab.local.getItem('jkdf2mp-library-mods-on-jk1'), '["701"]', 'ticked in Choose mods next time');
    assert.equal(L(tab).take(), '', 'taken once');
    L(tab).end();
});

await test('a mod of episodes only: written into episode/, no restart', async () => {
    await open(tab);
    pick(tab, 'MP Pack');
    assert.equal(L(tab).text(T.USE), 'Get Mod');
    assert.ok(L(tab).text(T.USE_HINT).length <= 70, 'one line on the hint line');
    assert.equal(L(tab).number(N.SHOTS), 0);
    L(tab).use();
    await settled(tab);
    assert.equal(L(tab).text(T.MESSAGE), 'Saved in this browser: its episode is on the Host screen.');
    assert.equal(fileSha(tab, '/jk1/episode/mppack.gob'), byId[706].entry.g[0].h);
    assert.equal(L(tab).number(N.CAN_USE), 0, 'nothing more to do');
    assert.match(L(tab).text(T.OVERVIEW), /Saved in this browser: its episodes are on the Host screen\./);
    assert.ok(rows(tab).some(r => r.startsWith('* MP Pack')));
    assert.equal(L(tab).number(N.DONE), 0);
});

await test('a graphics pack: into mods/, and the running mod stays', async () => {
    pick(tab, 'JK Enhanced');
    await details(tab);
    assert.match(L(tab).text(T.OVERVIEW), /^Kind:\tGraphics pack$/m);
    assert.match(L(tab).text(T.OVERVIEW), /only you see it/);
    L(tab).use();
    await settled(tab);
    assert.equal(L(tab).text(T.USE), 'Restart With It');
    assert.match(L(tab).text(T.USE_HINT), /graphics pack and the mod you run now/);
    assert.ok(L(tab).text(T.USE_HINT).length <= 70, 'one line on the hint line');
    L(tab).use();
    await until(() => L(tab).number(N.DONE), 'done');
    assert.equal(L(tab).take(), 'netmods/' + sbxFp.substring(0, 16), 'still SBX');
    assert.equal(fileSha(tab, '/jk1/mods/JKE.gob'), sha(byId[705].detail.mod.plan.files.length && makeGob([{ name: 'mat\\wall.mat', bytes: text('MAT') }])));
    assert.equal(tab.jkMods.current().fp, sbxFp);
    assert.deepEqual(tab.jkGraphicsPacks, ['JK Enhanced'], 'named as the graphics pack that stays on');
    assert.equal(tab.local.getItem('jkdf2mp-library-mods-on-jk1'), '["701","705"]');
    L(tab).end();
});

await test('a download that isn\'t whole: the reason, and Get Mod again', async () => {
    await open(tab);
    pick(tab, 'Broken Download');
    L(tab).use();
    await settled(tab);
    assert.equal(L(tab).text(T.MESSAGE), "The download didn't come through whole. Try again.");
    assert.equal(L(tab).number(N.CAN_USE), 1);
    assert.equal(L(tab).number(N.DONE), 0);
    assert.equal(L(tab).cancel(), false, 'nothing to stop');
    L(tab).end();
});

await test('another tab: My mods, and the saved mod started from its folder name', async () => {
    const other = await makeTab({ idb, args: ['-path', 'netmods/' + sbxFp.substring(0, 16)] });
    assert.equal(other.jkMods.current() && other.jkMods.current().fp, sbxFp, 'mods.js unpacks it from the store');
    await open(other, 'jk1', ALL_KINDS | F.MINE);
    assert.deepEqual(rows(other).map(r => r.split('\t')[0]), ['* MP Pack', '* SBX-JK v3.1', '* JK Enhanced']);
    pick(other, 'SBX-JK v3.1');
    await details(other);
    assert.equal(L(other).number(N.CAN_USE), 0, 'it runs now');
    assert.match(L(other).text(T.OVERVIEW), /The game is running with this mod now\./);
    L(other).end();
});

await test('Mysteries of the Sith: its own list and its own game files', async () => {
    const mots = await makeTab({ files: MOTS, args: ['-motsCompat'], cwd: '/mots' });
    await until(() => L(mots).available(), 'available');
    await open(mots, 'mots');
    assert.deepEqual(rows(mots).map(titleOf), ['MotS Saber Mod']);
    await details(mots);
    assert.ok(L(mots).text(T.CHANGES).split('\n').includes("Replaces:\t1 of the game's own files: 1 script"));
    L(mots).use();
    await settled(mots);
    const saved = await mots.jkMods.library.list('mots');
    assert.equal(saved.length, 1);
    assert.equal(saved[0].ext, 'goo');
});

await test('jkdf2.net\'s mods: from the list of both, said where they\'re from, found by their files\' names', async () => {
    SITE.all = true;
    try {
        const t2 = await makeTab({ idb: makeIDB() });
        await until(() => L(t2).available(), 'available');
        await open(t2);
        assert.ok(requests.includes('GET /levels/mods/all/jk1.json'));
        assert.equal(L(t2).text(T.STATUS), '8 mods from Massassi.net and jkdf2.net so far, 1 more coming');
        pick(t2, 'Masters');
        await details(t2);
        const ov = L(t2).text(T.OVERVIEW).split('\n');
        assert.ok(ov.includes('From:\tjkdf2.net (Massassi doesn\'t have it)'), ov.join('|'));
        assert.ok(ov.includes('Includes:\tskeletor, heman'), ov.join('|'));
        assert.ok(ov.includes('Credit:\tjkdf2.net\'s archive kept it; copied with its owner\'s permission.'), ov.join('|'));
        assert.equal(L(t2).text(T.FILES).split('\n')[0],
                     'Credit:\tjkdf2.net\'s archive (Game Files/jkmod/skins/MASTERS.ZIP), copied with its owner\'s permission.');
        assert.ok(!ov.some(l => /^Rating:/.test(l)), 'no rating or downloads');
        assert.match(L(t2).text(T.OVERVIEW), /Its files were loose in its zip/);
        assert.equal(L(t2).number(8), 1, 'no Massassi Page');
        pick(t2, 'Kyle Skin');
        assert.equal(L(t2).number(8), 0);
        await details(t2);
        assert.ok(L(t2).text(T.OVERVIEW).split('\n').includes('From:\tMassassi.net'));
        assert.ok(!/Credit:/.test(L(t2).text(T.FILES)));
        // Found by a model's name, and by its zip's.
        L(t2).query('skeletor', ALL_KINDS, 1);
        assert.deepEqual(rows(t2).map(titleOf), ['Masters']);
        L(t2).query('masters.zip', ALL_KINDS, 1);
        assert.deepEqual(rows(t2).map(titleOf), ['Masters']);
        // With no downloads, last when sorted by them.
        L(t2).query('', ALL_KINDS, 0);
        assert.equal(titleOf(rows(t2).at(-1)), 'Masters');
        // Get Mod as for one of Massassi's.
        pick(t2, 'Masters');
        L(t2).use();
        await settled(t2);
        const saved = await t2.jkMods.library.list('jk1');
        assert.deepEqual(saved.map(x => x.lib), [1000123]);
    } finally {
        SITE.all = false;
    }
});

await test('a game watching a match played with a library mod gets it by its id, with no menu open (viewGet)', async () => {
    const w = await makeTab({ idb: makeIDB() });
    const progress = [];
    const got = await L(w).viewGet('jk1', 701, (d, t) => progress.push([d, t]));
    assert.equal(got.fp, sbxFp, 'the fingerprint Get Mod gives');
    assert.ok(progress.length && progress.at(-1)[0] === progress.at(-1)[1], 'the download, to its end');
    const saved = await w.jkMods.library.list('jk1');
    assert.deepEqual(saved.map(x => [x.lib, x.sha256]), [[701, sbxFp]], 'saved as Get Mod saves it');
    assert.equal(L(w).text(T.MESSAGE), '', 'nothing left for the menu to show');
    assert.equal(await L(w).viewGet('jk1', 999999, () => {}), null, 'an id it does not list');
    assert.equal(await L(w).viewGet('x', 701, () => {}), null);
    // Through mods.js, as the engine asks for it from the host's keyframe.
    const v = await makeTab({ idb: makeIDB() });
    v.jkMods.viewFetch(v.jkMods.lib.encodeTail(sbxFp, 'SBX-JK v3.1', null), '701', () => {});
    await until(() => v.jkMods.state() !== 0, 'the mod');
    assert.equal(v.jkMods.state(), 3, 'restart');
    const path = v.jkMods.restart();
    assert.equal(path, 'netmods/' + sbxFp.slice(0, 16));
    assert.ok(v.FS.files.has('/jk1/' + path + '/cog/new.cog'));
    // Cancelled while it downloads: aborted, nothing set up.
    const c = await makeTab({ idb: makeIDB() });
    const ac = new AbortController();
    ac.abort();
    await assert.rejects(L(c).viewGet('jk1', 701, () => {}, ac.signal));
});

await test('every request for a zip asked for its SHA-256', async () => {
    for (const r of requests.filter(r => / \/levels\/z\//.test(r)))
        assert.match(r, /\?h=[0-9a-f]{64}$/);
});

console.log(passed + ' passed');
