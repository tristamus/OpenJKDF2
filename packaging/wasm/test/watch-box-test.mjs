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
// One made while canMove is on can move another in as it is (moveBefore:
// Chrome, Edge), which a frame takes without loading again (moved).
let canMove = false;
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
        get firstChild() { return e.children[0] || null; },
        // In the page: in one that's in it (connected).
        get isConnected() { let x = e; while (x.parentNode) x = x.parentNode; return !!x.connected; },
        remove() {
            const p = e.parentNode;
            if (!p) return;
            p.children.splice(p.children.indexOf(e), 1);
            e.parentNode = null;
            e.removed++;
        },
    };
    if (canMove) {
        e.moveBefore = (x, ref) => {
            const p = x.parentNode;
            if (p) p.children.splice(p.children.indexOf(x), 1);
            x.parentNode = e;
            const at = ref ? e.children.indexOf(ref) : -1;
            e.children.splice(at < 0 ? e.children.length : at, 0, x);
            x.moved = (x.moved || 0) + 1;
        };
    }
    if (tag === 'iframe') e.contentWindow = { name: 'frame' };
    e.hasAttribute = a => a in e;
    e.focus = () => { e.focused = (e.focused || 0) + 1; };
    return e;
}
// All the text in a stand-in element and those in it.
const textOf = e => (e.textContent || '') + e.children.map(textOf).join('|');

// store: what this browser keeps (localStorage), null for none it can keep.
function page({ phone = '', mock = false, why = '', screen = { width: 1920, height: 1080 }, move = false, store = {} } = {}) {
    canMove = move;
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
        watchBox: { frame: null, uid: '', id: 0, w: 0, h: 0, zoom: 1, keys: null, follow: '', followEls: {}, ended: 0, left: {},
                    live: false, panel: false },
        // The viewing page's box, in the page (its dialog is, open or not).
        watchScreen: Object.assign(elem('div', 'watch-screen'), { clientWidth: 684, clientHeight: 386, connected: true }),
        full: false,
        watchDlg: { open: true },
        location: { pathname: '/', search: '?x=1', protocol: 'https:' },
        window: mock ? { jkLobbyMock: {} } : {},
        document: { createElement: tag => elem(tag) },
        ROOMS, PACK_ONLY: 'Remastered pack',
    };
    const names = ['watchLive', 'watchBox', 'watchScreen', 'watchDlg', 'location', 'window', 'document', 'ROOMS', 'PACK_ONLY',
                   'watchInGame', 'phoneNote', 'watchedGame', 'el', 'renderWatch', 'lobbySay', 'screen', 'watchFullOn', 'localStorage'];
    const storage = store === null
        ? { getItem() { throw new Error('blocked'); }, setItem() { throw new Error('blocked'); } }
        : { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); } };
    const api = new Function(...names, endCode + boxCode + sayCode + '\nreturn { boxCan, boxShow, boxStop, boxScreen, boxNote, boxAgain, ' +
                             'boxSize, boxFit, boxPlace, pvScreen, watchKeysPanel, watchKeysToggle, watchFollowLabel, keyLabel, ' +
                             'startWatchEmbed: typeof startWatchEmbed, sayWatching };')(
        env.watchLive, env.watchBox, env.watchScreen, env.watchDlg, env.location, env.window, env.document, ROOMS,
        env.PACK_ONLY,
        w => (w ? { why } : null), () => phone,
        id => games[id === undefined ? env.watchLive.id : id] || null,
        (tag, cls, text) => elem(tag, cls, text),
        () => { env.renders++; },
        line => { env.said.push(line); return true; },
        screen, () => env.full, storage);
    canMove = false;
    return { ...api, env, games, store };
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
    p.boxScreen(f, elem('div'));
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
        jump: [['DIK_ADD', false], ['DIK_X', false], ['KEY_MOUSE_B2', false], ['KEY_JOY1_B4', false]],
        duck: [['DIK_C', false], ['KEY_JOY1_B2', false]], fast: [['DIK_LSHIFT', false]],
        fire1: [['DIK_RCONTROL', false], ['DIK_LCONTROL', false], ['KEY_MOUSE_B1', false], ['KEY_JOY1_B17', false]],
        fire2: [['DIK_Z', false], ['DIK_NUMPAD0', false], ['KEY_MOUSE_B3', false]], map: [['DIK_H', false], ['DIK_TAB', false]],
        camera: [['DIK_F1', false]], talk: [],
    });
    p.env.window.jkWatchBoxSays({}, 'live', '');
    assert.equal(p.env.watchBox.keys, null, 'not another window');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'live', '');
    assert.equal(p.env.renders, 1, 'drawn again with them');
    text = textOf(p.watchKeysPanel());
    assert.match(text, /Next player\|Left mouse, Right Ctrl\|/, 'two at most: the mouse before other keys, a pad\'s last');
    assert.match(text, /Player before\|Z, Middle mouse\|/, 'a letter first');
    assert.match(text, /First \/ third person\|F1/);
    assert.match(text, /Move\|W, A, S, D\|/, 'forward, left, back, right: a letter each');
    assert.match(text, /Up \/ down\|X \/ C\|/, 'one each way');
    assert.match(text, /Faster\|Left Shift/);
    assert.match(text, /Map\|Tab\|/, 'not H: H shows or hides these');
    assert.match(text, /Message the players\|Talk \(not bound\)/);
    assert.match(text, /Show \/ hide controls\|H\|Give back the mouse\|Esc/);
    assert.equal(p.watchKeysPanel().children[0].children[1].textContent, 'Hide (H)');
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
    assert.equal(k('DIK_ADD'), 'Numpad +');
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

test('who the box\'s game follows shows over it, kept up to date in place', () => {
    const p = page();
    assert.equal(p.watchFollowLabel('watch-follow'), null, 'no box, no label');
    const f = p.boxShow(p.games[5]);
    p.env.window.jkWatchBoxSays(f.contentWindow, 'follow', 'Following Kyle');
    const small = p.watchFollowLabel('watch-follow'), bar = p.watchFollowLabel('hud-follow');
    assert.equal(small.className, 'watch-follow');
    assert.equal(small.textContent, 'Following Kyle', 'what it said before the page was drawn');
    assert.equal(bar.textContent, 'Following Kyle');
    p.env.window.jkWatchBoxSays({}, 'follow', 'Free camera');
    assert.equal(small.textContent, 'Following Kyle', 'not another window');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'follow', 'Free camera');
    assert.equal(small.textContent, 'Free camera', 'in place');
    assert.equal(bar.textContent, 'Free camera');
    assert.equal(p.env.renders, 0, 'the page not drawn again for it');
    const again = p.watchFollowLabel('watch-follow');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'follow', 'Following ' + 'x'.repeat(80));
    assert.equal(again.textContent.length, 48, 'no longer than 48');
    p.boxStop();
    assert.equal(p.env.watchBox.follow, '');
    assert.deepEqual(p.env.watchBox.followEls, {});
});

test('the box\'s own panel goes over the badges, and comes off with the frame', () => {
    const p = page();
    const f = p.boxShow(p.games[5]);
    p.boxScreen(f, elem('div'));
    p.env.window.jkWatchBoxSays(f.contentWindow, 'panel', '');
    assert.ok(p.env.watchScreen.classList.contains('box-panel'));
    p.env.window.jkWatchBoxSays(f.contentWindow, 'busy', '');
    assert.ok(!p.env.watchScreen.classList.contains('box-panel'));
    p.env.window.jkWatchBoxSays(f.contentWindow, 'panel', '');
    p.boxStop();
    assert.ok(!p.env.watchScreen.classList.contains('box-panel'));
});

// The room window's preview, in the page (its pane in the room window).
function inPreview(p) {
    const body = Object.assign(elem('div', 'pv-body'), { connected: true });
    body.append(p.pvScreen);
    Object.assign(p.pvScreen, { clientWidth: 320, clientHeight: 180 });
    return body;
}

test('the room window\'s preview plays the match; Watch full size takes its frame along as it is (moveBefore)', () => {
    const p = page({ move: true });
    p.env.watchDlg.open = false;
    inPreview(p);
    const f = p.boxShow(p.games[5]);
    p.boxPlace(p.pvScreen);
    assert.equal(f.parentNode, p.pvScreen);
    assert.equal(f.moved, undefined, 'put there the first time, as a new frame is');
    assert.equal(f.style.transform, 'translate(0.00px, 0.00px) scale(' + 320 / 1920 + ')', 'scaled to the preview');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'live', '');
    assert.ok(p.pvScreen.classList.contains('box-live'));
    assert.ok(!p.env.watchScreen.classList.contains('box-live'), 'where the frame is, only');
    assert.equal(p.env.renders, 0, 'the viewing page is closed');
    assert.deepEqual(p.store, {}, 'the Controls haven\'t shown yet');
    p.boxPlace(p.pvScreen);
    assert.equal(f.moved, undefined, 'already there');
    // Watch full size (a click on the preview: openWatch): the viewing
    // page's box gets the same frame, its game still watching.
    p.env.watchDlg.open = true;
    const view = elem('div', 'watch-view');
    p.boxScreen(p.boxShow(p.games[5]), view);
    assert.deepEqual(p.env.watchScreen.children, [f, view]);
    assert.equal(f.moved, 1);
    assert.equal(f.removed, 0, 'never taken out (it would load again)');
    assert.equal(p.env.watchBox.live, true, 'still playing the match');
    assert.ok(p.env.watchScreen.classList.contains('box-live'));
    assert.ok(!p.pvScreen.classList.contains('box-live'));
    assert.equal(f.style.transform, 'translate(0.00px, ' + ((386 - 1080 * (684 / 1920)) / 2).toFixed(2) + 'px) scale(' + 684 / 1920 + ')',
                 'scaled to the box');
    assert.equal(p.store['jkdf2mp-watch-keys-seen'], '1', 'the Controls show over the match now');
    // Back in the room window (the viewing page closed): to the preview.
    p.env.watchDlg.open = false;
    p.boxPlace(p.pvScreen);
    assert.equal(f.parentNode, p.pvScreen);
    assert.equal(f.moved, 2);
    assert.ok(p.pvScreen.classList.contains('box-live'));
});

test('where a frame can\'t move as it is, it loads again on the viewing page, and its game starts again', () => {
    const p = page();
    p.env.watchDlg.open = false;
    inPreview(p);
    const f = p.boxShow(p.games[5]);
    p.boxPlace(p.pvScreen);
    p.env.window.jkWatchBoxSays(f.contentWindow, 'live', '');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'panel', '');
    assert.ok(p.pvScreen.classList.contains('box-panel'), 'its folder panel: the badge saying to watch full size');
    p.env.watchDlg.open = true;
    p.boxScreen(p.boxShow(p.games[5]), elem('div'));
    assert.equal(f.parentNode, p.env.watchScreen);
    assert.equal(p.env.watchBox.frame, f, 'the same frame, loading again');
    assert.deepEqual([p.env.watchBox.live, p.env.watchBox.panel, p.env.watchBox.keys], [false, false, null]);
    assert.ok(!p.pvScreen.classList.contains('box-live') && !p.pvScreen.classList.contains('box-panel'));
    assert.ok(!p.env.watchScreen.classList.contains('box-live'), 'not until its game plays the match again');
    assert.deepEqual(p.store, {});
    p.env.window.jkWatchBoxSays(f.contentWindow, 'live', '');
    assert.ok(p.env.watchScreen.classList.contains('box-live'));
    assert.equal(p.store['jkdf2mp-watch-keys-seen'], '1');
    // A preview not in the page yet (a room window drawn for the first
    // time): the frame goes in as a new one does.
    const q = page({ move: true });
    q.env.watchDlg.open = false;
    const g = q.boxShow(q.games[5]);
    q.boxPlace(q.pvScreen);
    assert.equal(g.parentNode, q.pvScreen);
    assert.equal(g.moved, undefined);
});

test('H in the box shows or hides the Controls, on the viewing page only', () => {
    const p = page();
    const f = p.boxShow(p.games[5]);
    p.env.window.jkWatchBoxSays(f.contentWindow, 'keys', '');
    assert.ok(p.watchKeysPanel(), 'not in the viewing page\'s box yet: nothing');
    p.boxScreen(f, elem('div'));
    p.env.window.jkWatchBoxSays({}, 'keys', '');
    assert.ok(p.watchKeysPanel(), 'not another window');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'keys', '');
    assert.equal(p.watchKeysPanel(), null, 'hidden');
    assert.equal(p.env.renders, 1, 'drawn again');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'keys', '');
    assert.ok(p.watchKeysPanel(), 'shown again');
    p.env.watchDlg.open = false;
    p.env.window.jkWatchBoxSays(f.contentWindow, 'keys', '');
    assert.ok(p.watchKeysPanel(), 'the viewing page closed: nothing');
    assert.equal(p.env.renders, 2);
    // The box's page: H there goes to the lobby page, unless the player is
    // typing a message or in the console (sithControl_WebTyping).
    assert.match(html, /if \(ev\.code !== 'KeyH' \|\| ev\.altKey \|\| ev\.ctrlKey \|\| ev\.metaKey\) return;\s*if \(typeof Module\._sithControl_WebTyping === 'function' && Module\._sithControl_WebTyping\(\)\) return;\s*ev\.preventDefault\(\);\s*ev\.stopImmediatePropagation\(\);\s*if \(!ev\.repeat\) embedSays\('keys'\);/);
    // The viewing page's own keys: H there too.
    assert.match(html, /\} else if \(ev\.code === 'KeyH'\) \{/);
});

test('the Controls show by themselves the first time this browser watches in the box, not after', () => {
    const store = {};
    const p = page({ store });
    const f = p.boxShow(p.games[5]);
    p.boxScreen(f, elem('div'));
    assert.ok(p.watchKeysPanel(), 'the first time');
    assert.deepEqual(store, {}, 'not seen until the box plays the match');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'live', '');
    assert.equal(store['jkdf2mp-watch-keys-seen'], '1');
    p.watchKeysToggle();
    assert.equal(p.watchKeysPanel(), null);
    // Next time: hidden, until H or Controls.
    const q = page({ store });
    const g = q.boxShow(q.games[5]);
    q.boxScreen(g, elem('div'));
    q.env.window.jkWatchBoxSays(g.contentWindow, 'live', '');
    assert.equal(q.watchKeysPanel(), null, 'hidden the next time');
    q.watchKeysToggle();
    assert.ok(q.watchKeysPanel());
    assert.equal(store['jkdf2mp-watch-keys-seen'], '1');
    // Hidden before they showed over a match: still shown the next time.
    const fresh = {};
    const r = page({ store: fresh });
    r.watchKeysToggle();
    const h = r.boxShow(r.games[5]);
    r.boxScreen(h, elem('div'));
    r.env.window.jkWatchBoxSays(h.contentWindow, 'live', '');
    assert.deepEqual(fresh, {});
    // A browser that can't keep it: shown each time.
    const t = page({ store: null });
    const k = t.boxShow(t.games[5]);
    t.boxScreen(k, elem('div'));
    t.env.window.jkWatchBoxSays(k.contentWindow, 'live', '');
    assert.ok(t.watchKeysPanel());
    // Opening the viewing page again starts from what the browser kept.
    assert.match(html, /watchKeysOn = !watchKeysSeen\(\);/);
});

test('the Controls shown by themselves go by themselves after 15 s over the match; ones put there stay', () => {
    // The page's timers, run by hand.
    const timers = [];
    const real = [globalThis.setTimeout, globalThis.clearTimeout];
    globalThis.setTimeout = (fn, ms) => { timers.push({ fn, ms, on: true }); return timers.length; };
    globalThis.clearTimeout = id => { if (timers[id - 1]) timers[id - 1].on = false; };
    const waiting = ms => timers.some(t => t.on && t.ms === ms);
    const run = ms => {
        const t = timers.find(x => x.on && x.ms === ms);
        assert.ok(t, 'a timer of ' + ms + ' ms');
        t.on = false;
        t.fn();
    };
    try {
        const p = page({ store: {} });
        const f = p.boxShow(p.games[5]);
        p.boxScreen(f, elem('div'));
        assert.ok(!waiting(15000), 'not before the box plays the match');
        p.env.window.jkWatchBoxSays(f.contentWindow, 'live', '');
        p.boxScreen(f, elem('div'));
        assert.equal(timers.filter(t => t.on && t.ms === 15000).length, 1, 'one, however often it is drawn');
        assert.doesNotMatch(p.watchKeysPanel().className, /fading/);
        const renders = p.env.renders;
        run(15000);
        assert.match(p.watchKeysPanel().className, /fading/, 'drawn again while it fades, it still fades');
        run(800);
        assert.equal(p.watchKeysPanel(), null);
        assert.ok(p.env.renders > renders, 'the page drew it gone');
        // H brings them back, and then they stay.
        p.watchKeysToggle();
        p.boxScreen(f, elem('div'));
        assert.ok(p.watchKeysPanel());
        assert.doesNotMatch(p.watchKeysPanel().className, /fading/);
        assert.ok(!waiting(15000));

        // Hidden with H before 15 s: the fade's timer goes, and H shows
        // them for good.
        const q = page({ store: {} });
        const g = q.boxShow(q.games[5]);
        q.boxScreen(g, elem('div'));
        q.env.window.jkWatchBoxSays(g.contentWindow, 'live', '');
        assert.ok(waiting(15000));
        q.watchKeysToggle();
        assert.equal(q.watchKeysPanel(), null);
        assert.ok(!waiting(15000));
        q.watchKeysToggle();
        q.boxScreen(g, elem('div'));
        assert.ok(q.watchKeysPanel());
        assert.ok(!waiting(15000));

        // H while they fade: they stay, not fading.
        const r = page({ store: {} });
        const h = r.boxShow(r.games[5]);
        r.boxScreen(h, elem('div'));
        r.env.window.jkWatchBoxSays(h.contentWindow, 'live', '');
        run(15000);
        r.watchKeysToggle();
        r.watchKeysToggle();
        assert.ok(!waiting(800));
        assert.doesNotMatch(r.watchKeysPanel().className, /fading/);
    } finally {
        [globalThis.setTimeout, globalThis.clearTimeout] = real;
    }
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

test('an Imperial server\'s card with a new feed is a new match: what the last one\'s box said doesn\'t stop it', () => {
    const p = page();
    p.games[5] = { key: 'narshaddaa', no: 'Game 1', g: { id: 5, uid: 'ipin0', imperial: true, mod: '' } };
    const f = p.boxShow(p.games[5]);
    assert.equal(p.env.watchBox.uid, 'ipin0-5');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'left', 'The host turned watching off.');
    assert.equal(p.boxShow(p.games[5]), null, 'that feed: not again by itself');
    // The server asks for its feed again: the same card, a new room.
    p.games[5] = { key: 'narshaddaa', no: 'Game 1', g: { id: 9, uid: 'ipin0', imperial: true, mod: '' } };
    p.env.watchLive.id = 5;
    const again = p.boxShow(p.games[5]);
    assert.ok(again, 'a box for the new feed');
    assert.equal(p.env.watchBox.id, 9);
    assert.equal(p.boxNote(p.games[5]).children[0].textContent, 'The game stopped watching this match.', 'not the old feed\'s note');
});

test('what the box\'s game says is kept whole up to 400 characters, and why its feed ended is kept for the page', () => {
    const p = page();
    const long = 'x'.repeat(300) + 'y'.repeat(200);
    const f = p.boxShow(p.games[5]);
    p.env.window.jkWatchBoxSays(f.contentWindow, 'said', 'The host turned watching off.');
    assert.equal(p.env.watchBox.said, 'The host turned watching off.');
    p.env.window.jkWatchBoxSays(f.contentWindow, 'left', long);
    assert.equal(p.boxNote(p.games[5]).children[0].textContent, long.substring(0, 400));
    assert.equal(p.env.watchBox.said, '', 'a new box starts with nothing said');
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
