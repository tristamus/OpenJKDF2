// Checks the viewing page's box showing the match itself (index.html, "The
// match itself in the viewing page's box"): one frame of the page per match
// watched, kept while the page is drawn again around it, what the box's page
// asks and says, and the page counted once while the box's game watches
// (sayWatching). Also what Watch in the game starts with (watchStartMods).
// Runs index.html's code for it on stand-ins for the page.
//
//   node test/watch-box-test.mjs

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from);
    assert.ok(a >= 0, 'index.html has ' + from.trim());
    const b = html.indexOf(to, a);
    assert.ok(b > a, 'index.html has ' + to.trim() + ' after ' + from.trim());
    return html.substring(a, b);
}
const boxCode = cut('      function boxCan(w) {', '      window.jkPlayWatched = function(id) {');
const sayCode = cut('      function sayWatching() {', '      // Every game of the lobby');
const endCode = cut('      var BOX_END_MS = ', '      function boxCan(w) {');
const modsCode = cut('      function watchStartMods(folderMods, game) {', '      // The name of several mods together');

// A stand-in element: children, classes and the few calls the code makes.
function elem(tag, cls, text) {
    const e = {
        tagName: tag.toUpperCase(), className: cls || '', textContent: text || '', dataset: {}, children: [],
        parentNode: null, removed: 0, classes: new Set(),
        classList: {
            toggle(c, on) { on ? e.classes.add(c) : e.classes.delete(c); },
            remove(c) { e.classes.delete(c); },
            contains(c) { return e.classes.has(c); },
        },
        append(...xs) { for (const x of xs) { if (x.parentNode) x.remove(); x.parentNode = e; e.children.push(x); } },
        prepend(x) { if (x.parentNode) x.remove(); x.parentNode = e; e.children.unshift(x); },
        remove() {
            const p = e.parentNode;
            if (!p) return;
            p.children.splice(p.children.indexOf(e), 1);
            e.parentNode = null;
            e.removed++;
        },
    };
    if (tag === 'iframe') e.contentWindow = { name: 'frame' };
    return e;
}

function page({ phone = '', mock = false, why = '' } = {}) {
    const ROOMS = { narshaddaa: { game: 'jk1', name: 'Nar Shaddaa' }, asteroid: { game: 'mots', name: 'Sith: Asteroid' },
                    'bespin-demo': { game: 'jk1', name: 'Bespin Mining Station', demo: true } };
    const games = {
        5: { key: 'narshaddaa', no: 'Game 1', g: { id: 5, uid: 'g5', mod: '' } },
        6: { key: 'asteroid', no: 'Game 2', g: { id: 6, uid: 'g6', mod: 'SBX, Remastered pack' } },
        7: { key: 'bespin-demo', no: 'Game 1', g: { id: 7, uid: 'g7', mod: '' } },
    };
    const env = {
        said: [], renders: 0,
        watchLive: { id: 5, sent: 5 },
        watchBox: { frame: null, uid: '', id: 0, counted: false, ended: 0, left: {} },
        watchScreen: elem('div', 'watch-screen'),
        watchDlg: { open: true },
        location: { pathname: '/', search: '?x=1', protocol: 'https:' },
        window: mock ? { jkLobbyMock: {} } : {},
        document: { createElement: tag => elem(tag) },
        ROOMS, PACK_ONLY: 'Remastered pack',
    };
    const names = ['watchLive', 'watchBox', 'watchScreen', 'watchDlg', 'location', 'window', 'document', 'ROOMS', 'PACK_ONLY',
                   'watchInGame', 'phoneNote', 'watchedGame', 'el', 'renderWatch', 'lobbySay'];
    const api = new Function(...names, endCode + boxCode + sayCode + '\nreturn { boxCan, boxShow, boxStop, boxScreen, boxNote, boxAgain, ' +
                             'startWatchEmbed: typeof startWatchEmbed, sayWatching };')(
        env.watchLive, env.watchBox, env.watchScreen, env.watchDlg, env.location, env.window, env.document, ROOMS,
        env.PACK_ONLY,
        w => (w ? { why } : null), () => phone,
        id => games[id === undefined ? env.watchLive.id : id] || null,
        (tag, cls, text) => elem(tag, cls, text),
        () => { env.renders++; },
        line => { env.said.push(line); return true; });
    return { ...api, env, games };
}

let failed = 0;
function test(name, fn) {
    try {
        fn();
        console.log('ok   ' + name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + name);
        console.log(e.stack);
    }
}

test('a match watched gets one frame of the page, kept while the page is drawn again', () => {
    const p = page();
    const w = p.games[5];
    const f = p.boxShow(w);
    assert.equal(f.tagName, 'IFRAME');
    assert.equal(f.className, 'watch-frame');
    assert.equal(f.src, '/?x=1#watch-5-embed');
    assert.equal(p.boxShow(w), f, 'the same frame again');
    const view1 = elem('div', 'watch-view'), hud1 = elem('div', 'watch-hud');
    p.boxScreen(f, view1, hud1);
    assert.deepEqual(p.env.watchScreen.children, [f, view1, hud1]);
    const view2 = elem('div', 'watch-view'), hud2 = elem('div', 'watch-hud');
    p.boxScreen(p.boxShow(w), view2, hud2);
    assert.deepEqual(p.env.watchScreen.children, [f, view2, hud2], 'the rest is new, the frame the same');
    assert.equal(f.removed, 0, 'never taken out (a frame put back loads again)');
    assert.equal(view1.removed, 1);
});

test('another match gets a new frame, and the old one goes', () => {
    const p = page();
    const f = p.boxShow(p.games[5]);
    p.boxScreen(f, elem('div'));
    p.env.watchLive.id = 6;
    const g = p.boxShow(p.games[6]);
    assert.notEqual(g, f);
    assert.equal(f.removed, 1);
    assert.equal(g.src, '/?x=1#watch-6-embed');
    assert.deepEqual([p.env.watchBox.uid, p.env.watchBox.id], ['g6', 6]);
});

test('no frame on a phone, in the mock-up, or for a match the game can\'t watch', () => {
    assert.equal(page({ phone: 'On a phone' }).boxShow(page().games[5]), null);
    assert.equal(page({ mock: true }).boxShow(page().games[5]), null);
    assert.equal(page({ why: 'Older version' }).boxShow(page().games[5]), null);
    const p = page();
    p.env.location.protocol = 'file:';
    assert.equal(p.boxShow(p.games[5]), null);
});

test('the box\'s page is told its match, and only the box\'s', () => {
    const p = page();
    assert.equal(p.env.window.jkWatchEmbedWant(5), null, 'no frame yet');
    p.boxShow(p.games[5]);
    assert.deepEqual(p.env.window.jkWatchEmbedWant(5),
                     { game: 'jk1', room: 'Nar Shaddaa', lobbyRoom: 'narshaddaa', demo: false, pack: false });
    assert.equal(p.env.window.jkWatchEmbedWant(6), null, 'not another match');
    p.env.watchLive.id = 6;
    p.boxShow(p.games[6]);
    assert.equal(p.env.window.jkWatchEmbedWant(6).pack, true, 'a mod with the Remastered pack');
    p.env.watchLive.id = 7;
    p.boxShow(p.games[7]);
    assert.equal(p.env.window.jkWatchEmbedWant(7).demo, true);
});

test('while the box\'s game is on the match\'s feed, the page says it watches none (the feed counts the game)', () => {
    const p = page();
    const f = p.boxShow(p.games[5]);
    p.env.window.jkWatchBoxSays({}, 'counted', '');
    assert.deepEqual(p.env.said, [], 'another window is ignored');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'live', '');
    assert.deepEqual(p.env.said, [], 'showing the match says nothing about the count');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'counted', '');
    assert.equal(p.env.watchBox.counted, true);
    assert.deepEqual(p.env.said, ['spectate 0']);
    p.sayWatching();
    assert.deepEqual(p.env.said, ['spectate 0'], 'once');
    // Off the feed for a moment (restarting into the match's mod): the page counts.
    p.env.window.jkWatchBoxSays(f.contentWindow, 'uncounted', '');
    assert.deepEqual(p.env.said, ['spectate 0', 'spectate 5']);
    p.env.window.jkWatchBoxSays(f.contentWindow, 'counted', '');
    p.boxStop();
    assert.deepEqual(p.env.said, ['spectate 0', 'spectate 5', 'spectate 0', 'spectate 5'],
                     'counted by the page again once the box stops');
});

test('the box\'s own panel goes over the badges, and comes off with the frame', () => {
    const p = page();
    const f = p.boxShow(p.games[5]);
    p.env.window.jkWatchBoxSays(f.contentWindow, 'panel', '');
    assert.ok(p.env.watchScreen.classList.contains('box-panel'));
    p.env.window.jkWatchBoxSays(f.contentWindow, 'busy', '');
    assert.ok(!p.env.watchScreen.classList.contains('box-panel'));
    p.env.window.jkWatchBoxSays(f.contentWindow, 'panel', '');
    p.boxStop();
    assert.ok(!p.env.watchScreen.classList.contains('box-panel'));
});

test('a box that stopped says why, with Watch again, until it is clicked', () => {
    const p = page();
    const f = p.boxShow(p.games[5]);
    p.env.window.jkWatchBoxSays(f.contentWindow, 'counted', '');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'left', 'Lost the match.');
    assert.equal(f.removed, 0, 'never put in the screen here');
    assert.equal(p.env.watchBox.frame, null);
    assert.equal(p.env.renders, 1);
    assert.deepEqual(p.env.said, ['spectate 0', 'spectate 5']);
    assert.equal(p.boxShow(p.games[5]), null, 'not again by itself');
    const note = p.boxNote(p.games[5]);
    assert.equal(note.children[0].textContent, 'Lost the match.');
    assert.equal(p.env.watchLive.note, undefined, 'the match is still on: the box says it');
    assert.equal(note.children[1].dataset.wact, 'again');
    assert.equal(p.boxNote(p.games[5]).children[1].textContent, 'Watch again');
    p.boxAgain();
    assert.equal(p.env.renders, 2);
    assert.ok(p.boxShow(p.games[5]), 'a new frame after Watch again');
});

test('a match over: what the box said goes on the page, which says the match ended', () => {
    const p = page();
    const f = p.boxShow(p.games[5]);
    delete p.games[5];
    p.env.window.jkWatchBoxSays(f.contentWindow, 'left', 'The match is over.');
    assert.equal(p.env.watchLive.note, 'The match is over.');
    assert.equal(p.env.watchBox.frame, null);
});

test('a box that stopped with nothing to say says the game stopped', () => {
    const p = page();
    const f = p.boxShow(p.games[5]);
    p.env.window.jkWatchBoxSays(f.contentWindow, 'left', '');
    assert.equal(p.boxNote(p.games[5]).children[0].textContent, 'The game stopped watching this match.');
});

test('Watch in the game starts with only the graphics packs kept on, with no start window', () => {
    const left = ['mods/jkhd'];
    const watchStartMods = new Function('modsLeftOut', 'modKey', modsCode + '\nreturn watchStartMods;')(
        () => left, m => (m.from + '/' + m.name).toLowerCase());
    const folderMods = [
        { name: 'SomeMod', path: 'mods/somemod', from: 'mods' },
        { name: 'JKE', path: 'mods/jke', from: 'mods', optional: true },
        { name: 'JKHD', path: 'mods/jkhd', from: 'mods', optional: true },
        { name: 'Library mod', path: 'jkmods/lib', from: 'library', library: { lib: 3 } },
    ];
    assert.deepEqual(watchStartMods(folderMods, 'jk1'), [{ index: 1, name: 'JKE', path: 'mods/jke', key: 'mods/jke' }]);
    assert.ok(/pickerWant && pickerWant\.watch \? watchStartMods\(folderMods, game\) :/.test(html), 'used for watching');
});

if (failed) {
    console.log(failed + ' of 10 failed');
    process.exit(1);
}
console.log('all 10 passed');
