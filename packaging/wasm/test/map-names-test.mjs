// Tests for the level names in the room windows of packaging/wasm/index.html:
// a game's Info and the list's Map column name its level as the game does
// (the host's game's name for it, which the lobby passes on, else the
// retail level's from the page's own list, else its file), and what the
// page keeps of the name the lobby sends. In Node:
// node packaging/wasm/test/map-names-test.mjs
//
// The code is taken from index.html as it is.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to, a);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const names = cut('      var RETAIL_LEVELS = {', '      function isFull(g)');
const clean = cut('      function cleanStatus(s) {', '      // One room\'s part of the status');
const imperial = cut('      var IMPERIAL_STATE = {', '      // state: \'off\' (nothing to show)') +
                 cut('      function cleanImperial(a) {', '      // The games a room window lists');
const lib = new Function('GAME_NAMES', 'NETCODE',
    names + clean + imperial + 'return { RETAIL_LEVELS, levelTitle, episodeTitle, cleanStatus, cleanImperial };')(
    { jk1: 'Jedi Knight', mots: 'Mysteries of the Sith' }, 0);
const { RETAIL_LEVELS, levelTitle, episodeTitle, cleanStatus, cleanImperial } = lib;

let passed = 0, failed = 0;
function check(name, fn) {
    try {
        fn();
        passed++;
        console.log('ok   ' + name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + name + '\n     ' + String(e && e.stack || e).split('\n').join('\n     '));
    }
}

check('the retail list has every level the games ship, under its episode', () => {
    const counts = Object.fromEntries(Object.entries(RETAIL_LEVELS).map(([k, e]) => [k, Object.keys(e.levels).length]));
    assert.deepEqual(counts, { jk1mp: 6, jk1ctf: 3, jk1: 25, jkm_mp: 16, jkm_kfy: 5, jkm_saber: 4, jkm: 14 });
    for (const [k, e] of Object.entries(RETAIL_LEVELS)) {
        assert.ok(e.title, k + ' has a title');
        for (const [file, name] of Object.entries(e.levels)) {
            assert.match(file, /^[a-z0-9_]+\.jkl$/, k + ': ' + file + ' in lower case');
            assert.ok(name && name === name.trim(), k + ': ' + file + ' has a name');
        }
    }
});

check('a retail level is named before its host has said, whatever the case of its file and episode', () => {
    assert.equal(levelTitle({ map: 'm10.jkl', episode: 'JK1MP.gob' }), 'Canyon Oasis');
    assert.equal(levelTitle({ map: 'M_BOSS17.JKL', episode: 'jk1mp.GOB' }), 'Battleground Jedi');
    assert.equal(levelTitle({ map: 'c3.jkl', episode: 'JK1CTF' }), 'The Challenge at Nar Shaddaa', "the relay's, without .gob");
    assert.equal(levelTitle({ map: 'MSB1_Home.jkl', episode: 'JKM_SABER.goo' }), 'Tatooine Homestead');
    assert.equal(levelTitle({ map: 'k3.jkl', episode: 'JKM_KFY.goo' }), 'Imperial Training Facility');
    assert.equal(levelTitle({ map: '01narshadda.jkl', episode: 'JK1.gob' }), '1 - Double-Cross on Nar Shaddaa', 'Co-op');
    assert.equal(levelTitle({ map: 's5l4_lowersith.jkl', episode: 'JKM.goo' }), '14 - Sith Temple Catacombs');
});

check("the host's name for its level comes first, unless it's only the file's", () => {
    assert.equal(levelTitle({ map: 'jhs.jkl', episode: 'jhs.gob', title: "Jedi's High School" }), "Jedi's High School");
    assert.equal(levelTitle({ map: 'm10.jkl', episode: 'JK1MP.gob', title: 'Canyon Oasis Remix' }), 'Canyon Oasis Remix',
                 "a mod's own name for a retail level");
    assert.equal(levelTitle({ map: 'APG_Deathmatch_Castle.jkl', episode: 'APG_Deathmatch_Castle.gob',
                              title: 'APG_DEATHMATCH_CASTLE' }), '', "the game's fallback for a level with no name");
    assert.equal(levelTitle({ map: 'm10.jkl', episode: 'JK1MP.gob', title: 'M10' }), 'Canyon Oasis');
});

check('anything else has no name, so the page shows its file', () => {
    assert.equal(levelTitle({ map: 'jhs.jkl', episode: 'jhs.gob' }), '');
    assert.equal(levelTitle({ map: 'm10.jkl', episode: 'mymod.gob' }), '', 'a retail file in another episode');
    assert.equal(levelTitle({ map: 'constructor', episode: 'JK1MP.gob' }), '');
    assert.equal(levelTitle({ map: 'm10.jkl', episode: '__proto__' }), '');
    assert.equal(levelTitle({ map: 'm10.jkl', episode: 'constructor.gob' }), '');
    assert.equal(levelTitle({ map: '', episode: '' }), '');
    assert.equal(levelTitle({}), '');
});

check('a retail episode has its title, any other none', () => {
    assert.equal(episodeTitle({ episode: 'JK1MP.gob' }), 'Jedi Training');
    assert.equal(episodeTitle({ episode: 'JK1CTF' }), 'Jedi Capture the Flag');
    assert.equal(episodeTitle({ episode: 'JKM_KFY.goo' }), 'Kill The Fool With The Ysalamiri');
    assert.equal(episodeTitle({ episode: 'jhs.gob' }), '');
    assert.equal(episodeTitle({ episode: 'hasOwnProperty' }), '');
});

check("the page keeps the lobby's name for a level as text, and the relay's games have none", () => {
    const s = cleanStatus({ online: 2, games: [
        { id: 1, game: 'jk1', map: 'jhs.jkl', title: "Jedi's\u0007 High School", episode: 'jhs.gob', players: 2, max: 4 },
        { id: 2, game: 'jk1', map: 'm10.jkl', episode: 'JK1MP.gob', players: 1, max: 4 },
        { id: 3, game: 'jk1', map: 'm2.jkl', title: 'x'.repeat(100), episode: 'JK1MP.gob' },
        { id: 4, game: 'jk1', map: 'm2.jkl', title: { html: '<b>' }, episode: 'JK1MP.gob' }] });
    assert.deepEqual(s.games.map(g => g.title), ["Jedi's High School", '', 'x'.repeat(48), '']);
    const i = cleanImperial({ state: 'live', server: 'lobby.jkjunkie.com', games: [
        { key: '13e5dd93743f59055bbf297508845b2b', name: 'Coruscant', game: 'jk1', map: 'c3.jkl', episode: 'JK1CTF',
          title: 'Not from the relay', listed: true, joinable: true, players: 0, max: 31 }] });
    assert.equal(i.games[0].title, '');
    assert.equal(levelTitle(i.games[0]), 'The Challenge at Nar Shaddaa');
});

check("Info shows the name, then the file, and the episode's title; the list's Map column the name", () => {
    assert.match(html, /\['Map', level \|\| g\.map \|\| 'Unknown'\], level && g\.map \? \['File', g\.map\] : null,/);
    assert.match(html, /\['Episode', episodeTitle\(g\) \|\| g\.episode \|\| 'Unknown'\]/);
    assert.match(html, /var map = el\('td', null, off \? '–' : level \|\| g\.map \|\| 'Unknown'\);/);
});

console.log(failed ? `${failed} of ${passed + failed} failed` : `all ${passed} passed`);
process.exit(failed ? 1 : 0);
