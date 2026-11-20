// Checks the viewing page's box showing the match itself (index.html, "The
// match itself in the viewing page's box"): one frame of the page per match
// watched, kept while the page is drawn again around it, what the box's page
// asks and says, and the page counting itself the whole time the box's game
// watches (sayWatching; the feed leaves that game out). Also what Watch in the game starts with (watchStartMods).
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
        tagName: tag.toUpperCase(), className: cls || '', textContent: text || '', dataset: {}, children: [], style: {},
        parentNode: null, removed: 0, classes: new Set(),
        classList: {
            toggle(c, on) { on ? e.classes.add(c) : e.classes.delete(c); },
            add(...cs) { for (const c of cs) e.classes.add(c); },
            remove(...cs) { for (const c of cs) e.classes.delete(c); },
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
    e.hasAttribute = a => a in e;
    e.focus = () => { e.focused = (e.focused || 0) + 1; };
    return e;
}
// All the text in a stand-in element and those in it.
const textOf = e => (e.textContent || '') + e.children.map(textOf).join('|');

function page({ phone = '', mock = false, why = '', screen = { width: 1920, height: 1080 } } = {}) {
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
        watchBox: { frame: null, uid: '', id: 0, w: 0, h: 0, zoom: 1, keys: null, ended: 0, left: {} },
        watchScreen: Object.assign(elem('div', 'watch-screen'), { clientWidth: 684, clientHeight: 386 }),
        full: false,
        watchDlg: { open: true },
        location: { pathname: '/', search: '?x=1', protocol: 'https:' },
        window: mock ? { jkLobbyMock: {} } : {},
        document: { createElement: tag => elem(tag) },
        ROOMS, PACK_ONLY: 'Remastered pack',
    };
    const names = ['watchLive', 'watchBox', 'watchScreen', 'watchDlg', 'location', 'window', 'document', 'ROOMS', 'PACK_ONLY',
                   'watchInGame', 'phoneNote', 'watchedGame', 'el', 'renderWatch', 'lobbySay', 'screen', 'watchFullOn'];
    const api = new Function(...names, endCode + boxCode + sayCode + '\nreturn { boxCan, boxShow, boxStop, boxScreen, boxNote, boxAgain, ' +
                             'boxSize, boxFit, watchKeysPanel, watchKeysToggle, keyLabel, startWatchEmbed: typeof startWatchEmbed, sayWatching };')(
        env.watchLive, env.watchBox, env.watchScreen, env.watchDlg, env.location, env.window, env.document, ROOMS,
        env.PACK_ONLY,
        w => (w ? { why } : null), () => phone,
        id => games[id === undefined ? env.watchLive.id : id] || null,
        (tag, cls, text) => elem(tag, cls, text),
        () => { env.renders++; },
        line => { env.said.push(line); return true; },
        screen, () => env.full);
    return { ...api, env, games };
}

let failed = 0, ran = 0;
function test(name, fn) {
    ran++;
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

test('the box\'s page is the screen\'s size, no more than 2560x1440 in pixels, scaled to the box', () => {
    const p = page();
    assert.deepEqual(p.boxSize(), { w: 1920, h: 1080 });
    const wide = page({ screen: { width: 3440, height: 1440 } }).boxSize();
    assert.ok(wide.w * wide.h <= 2560 * 1440 + 3000, 'about 2560x1440 in pixels at most');
    assert.ok(Math.abs(wide.w / wide.h - 3440 / 1440) < 0.005, 'the screen\'s shape');
    assert.deepEqual(page({ screen: {} }).boxSize(), { w: 1280, h: 720 }, 'no screen size');
    const f = p.boxShow(p.games[5]);
    assert.deepEqual([f.style.width, f.style.height], ['1920px', '1080px']);
    assert.equal(f.style.transform, undefined, 'not scaled until it is in the box');
    p.boxScreen(f, elem('div'));
    // A 684x386 box: all of it, fitted, a sliver of black above and below
    // (a 16:9 screen is a hair wider)
    const s = Math.min(684 / 1920, 386 / 1080);
    assert.equal(f.style.transform, 'translate(0.00px, ' + ((386 - 1080 * s) / 2).toFixed(2) + 'px) scale(' + s + ')');
    assert.equal(f.style.clipPath, undefined, 'nothing cut off');
    assert.equal(p.env.window.jkWatchBoxZoom(f.contentWindow), 1 / s, 'its page draws its prompt that much bigger');
    assert.equal(p.env.window.jkWatchBoxZoom({}), 1, 'another window');
    // Full screen on a 1920x1080 screen: as it is
    p.env.full = true;
    Object.assign(p.env.watchScreen, { clientWidth: 1920, clientHeight: 1080 });
    p.boxFit();
    assert.equal(f.style.transform, 'translate(0.00px, 0.00px) scale(1)');
    assert.equal(p.env.window.jkWatchBoxZoom(f.contentWindow), 1);
    assert.deepEqual([f.style.width, f.style.height], ['1920px', '1080px'], 'its page never changes size');
    // A box narrower than the screen's shape, in full screen: all of it, fitted
    Object.assign(p.env.watchScreen, { clientWidth: 1000, clientHeight: 1000 });
    p.boxFit();
    assert.equal(f.style.transform, 'translate(0.00px, ' + ((1000 - 1080 * 1000 / 1920) / 2).toFixed(2) + 'px) scale(' + 1000 / 1920 + ')');
    // A screen bigger than 2560x1440: scaled up in full screen
    const q = page({ screen: { width: 3840, height: 2160 } });
    const g = q.boxShow(q.games[5]);
    assert.deepEqual([g.style.width, g.style.height], ['2560px', '1440px']);
    Object.assign(q.env.watchScreen, { clientWidth: 3840, clientHeight: 2160 });
    q.boxScreen(g, elem('div'));
    assert.equal(g.style.transform, 'translate(0.00px, 0.00px) scale(1.5)');
    // A frame not in the box isn't touched
    p.boxStop();
    p.boxFit();
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

test('the page counts itself the whole time its box\'s game watches (the feed leaves that game out)', () => {
    const p = page();
    p.env.watchLive.sent = 0;
    p.sayWatching();
    assert.deepEqual(p.env.said, ['spectate 5']);
    const f = p.boxShow(p.games[5]);
    p.env.window.jkWatchBoxSays(f.contentWindow, 'live', '');
    // What a kit 163 box said: nothing moves now
    p.env.window.jkWatchBoxSays(f.contentWindow, 'counted', '');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'uncounted', '');
    p.sayWatching();
    assert.equal(p.env.watchBox.counted, undefined);
    p.boxStop();
    p.sayWatching();
    assert.deepEqual(p.env.said, ['spectate 5'], 'once, and still after the box stops');
    p.env.watchLive.id = 0;
    p.sayWatching();
    assert.deepEqual(p.env.said, ['spectate 5', 'spectate 0'], 'none once the viewing page shows no match');
});

test('black around the box\'s game once it plays the match, not the picture; off with the frame', () => {
    const p = page();
    const f = p.boxShow(p.games[5]);
    assert.ok(!p.env.watchScreen.classList.contains('box-live'));
    p.env.window.jkWatchBoxSays({}, 'live', '');
    assert.ok(!p.env.watchScreen.classList.contains('box-live'), 'not another window');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'live', '');
    assert.ok(p.env.watchScreen.classList.contains('box-live'));
    p.boxStop();
    assert.ok(!p.env.watchScreen.classList.contains('box-live'));
});

test('Controls: the keys for watching as the player bound them, once the box\'s game plays the match', () => {
    const p = page();
    const f = p.boxShow(p.games[5]);
    // Before the game is there: what the game calls each command.
    let text = textOf(p.watchKeysPanel());
    assert.match(text, /Next player\|Fire\|/);
    assert.match(text, /Up \/ down\|Jump \/ Crouch/);
    assert.match(text, /Give back the mouse\|Esc/);
    f.contentWindow.jkWatchKeys = () => ({
        forward: [['DIK_W', false], ['DIK_S', true], ['DIK_UP', false]], slide: [['DIK_D', false], ['DIK_A', true]],
        jump: [['DIK_SPACE', false]], duck: [['DIK_C', false], ['KEY_JOY1_B2', false]], fast: [['DIK_LSHIFT', false]],
        fire1: [['KEY_MOUSE_B1', false], ['DIK_LCONTROL', false]], fire2: [['KEY_MOUSE_B2', false]], map: [['DIK_TAB', false]],
        camera: [['DIK_F1', false]], talk: [],
    });
    p.env.window.jkWatchBoxSays({}, 'live', '');
    assert.equal(p.env.watchBox.keys, null, 'not another window');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'live', '');
    assert.equal(p.env.renders, 1, 'drawn again with them');
    text = textOf(p.watchKeysPanel());
    assert.match(text, /Next player\|Left mouse, Left Ctrl/);
    assert.match(text, /Player before\|Right mouse/);
    assert.match(text, /First \/ third person\|F1/);
    assert.match(text, /Move\|W, A, S, D\|/, 'forward, left, back, right: a letter each');
    assert.match(text, /Up \/ down\|Space \/ C, Pad button 2/);
    assert.match(text, /Faster\|Left Shift/);
    assert.match(text, /Map\|Tab/);
    assert.match(text, /Message the players\|Talk \(not bound\)/);
    // Hidden, then shown again; none without a frame.
    p.watchKeysToggle();
    assert.equal(p.watchKeysPanel(), null);
    p.watchKeysToggle();
    assert.ok(p.watchKeysPanel());
    assert.equal(p.watchKeysPanel().children[0].children[1].dataset.wact, 'keys', 'its Hide');
    p.boxStop();
    assert.equal(p.env.watchBox.keys, null);
    assert.equal(p.watchKeysPanel(), null);
    // A game whose page can't say: what the game calls them.
    const q = page();
    const g = q.boxShow(q.games[5]);
    g.contentWindow.jkWatchKeys = () => { throw new Error('gone'); };
    q.env.window.jkWatchBoxSays(g.contentWindow, 'live', '');
    assert.match(textOf(q.watchKeysPanel()), /Next player\|Fire\|/);
});

test('the game\'s names for keys, as players know them', () => {
    const k = page().keyLabel;
    assert.equal(k('DIK_SPACE'), 'Space');
    assert.equal(k('DIK_Q'), 'Q');
    assert.equal(k('DIK_7'), '7');
    assert.equal(k('DIK_F12'), 'F12');
    assert.equal(k('DIK_NUMPAD5'), 'Numpad 5');
    assert.equal(k('KEY_MOUSE_B3'), 'Middle mouse');
    assert.equal(k('KEY_JOY2_B11'), 'Pad 2 button 11');
    assert.equal(k('DIK_BACKSLASH'), '\\');
    assert.equal(k('DIK_SYSRQ'), 'Sysrq');
    assert.equal(k('KEY_UNKNOWN'), '');
    assert.equal(k(''), '');
});

test('Esc in the box: the keys come back to the viewing page', () => {
    const p = page();
    const f = p.boxShow(p.games[5]);
    p.env.window.jkWatchBoxSays({}, 'letgo', '');
    assert.equal(p.env.watchScreen.focused, undefined, 'not another window');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'letgo', '');
    assert.equal(p.env.watchScreen.focused, 1);
    assert.equal(p.env.watchScreen.tabIndex, -1);
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
    p.env.window.jkWatchBoxSays(f.contentWindow, 'left', 'Lost the match.');
    assert.equal(f.removed, 0, 'never put in the screen here');
    assert.equal(p.env.watchBox.frame, null);
    assert.equal(p.env.renders, 1);
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
    console.log(failed + ' of ' + ran + ' failed');
    process.exit(1);
}
console.log('all ' + ran + ' passed');
