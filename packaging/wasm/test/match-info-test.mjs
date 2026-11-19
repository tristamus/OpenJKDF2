// Tests for a match as the room windows show it while it's played
// (packaging/wasm/index.html): what the page keeps of a game's live part of
// the lobby's status (liveView in the lobby's src/ladder.js), and the words
// and times its Info and its table's line are made of. In Node:
// node packaging/wasm/test/match-info-test.mjs
//
// The code is taken from index.html as it is, with a clock the test sets.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
function cut(from, to) {
    const a = html.indexOf(from), b = html.indexOf(to, a);
    assert.ok(a > 0 && b > a, 'index.html has ' + from);
    return html.substring(a, b);
}
const clean = cut('      function cleanStatus(s) {', "      // One room's part of the status");
const live = cut("      // ---- A match as it's played", '      // Every second while a room window');

let NOW = 1_800_000_000_000;
const FakeDate = { now: () => NOW };
const lobbyLive = { skew: 0 };
const lib = new Function('GAME_NAMES', 'lobbyLive', 'Date', clean + live +
    'return { cleanStatus, clockText, agoText, gameLive, modeText, liveClock, liveFeed, liveSummary, liveTeams, ruleRows, lobbyNow };')(
    { jk1: 'Jedi Knight', mots: 'Mysteries of the Sith' }, lobbyLive, FakeDate);

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

const SEC = 1000, MIN = 60 * SEC;
// A game of the lobby's status with this live part.
const game = (v, more = {}) => ({ id: 3, name: 'Duel', game: 'jk1', map: 'm10.jkl', episode: 'JK1MP.gob', mod: '',
                                  locked: false, players: 2, max: 4, names: ['Kyle_K', 'MaraJade'], net: 0, rank: 8,
                                  live: v, ...more });
const status = games => lib.cleanStatus({ online: 2, lobby: [], byGame: {}, games, now: NOW });
const FFA = { mode: 0x18, sl: 20, tl: 15, at: NOW - 6 * MIN - 18 * SEC, count: 120,
              sb: [['Kyle_K', 12, 12, 7, 0], ['MaraJade', 7, 7, 12, 0]], last: ['Kyle_K', 'MaraJade', 1, NOW - 14 * SEC],
              streak: ['Kyle_K', 4] };

check("the page keeps a game's live part as it shows it, whatever arrives", () => {
    const s = status([game(FFA)]);
    assert.equal(s.now, NOW);
    assert.equal(s.games[0].rank, 8);
    assert.deepEqual(s.games[0].live, { mode: 0x18, sl: 20, tl: 15, at: FFA.at, over: 0, nocount: '', count: 120, pl: null,
                                        sb: FFA.sb, teams: null, last: FFA.last, kills: null, streak: FFA.streak });
    // Nothing live, or nothing it can use.
    for (const v of [undefined, null, 5, 'x', [], {}, { at: -1 }, { at: 'now' }])
        assert.equal(status([game(v)]).games[0].live, null, JSON.stringify(v));
    // An older lobby's status: no rank, no clock.
    const old = lib.cleanStatus({ online: 0, lobby: [], byGame: {}, games: [game(undefined, { rank: undefined })] });
    assert.deepEqual([old.games[0].rank, old.now], [-1, 0]);
    // Scores below zero stay, names are cut and cleaned, odd rows are dropped.
    const v = status([game({ ...FFA, sb: [['Sabe\u0007rStorm', -3, 0, 2, 9], 'x', ['A'.repeat(40), 1.6, 'x', null, 1]],
                                    teams: [0, 1, 2, 3], pl: { i: 3, n: 3 }, nocount: 'other', last: ['', 'X', 9, 0],
                                    streak: ['Kyle_K', 0] })]).games[0].live;
    assert.deepEqual(v.sb, [['SaberStorm', -3, 0, 2, 4], ['A'.repeat(32), 2, 0, 0, 1]]);
    assert.deepEqual([v.teams, v.pl, v.nocount, v.last, v.streak], [null, null, '', null, null]);
    const p = status([game({ ...FFA, pl: { i: 2, n: 3, loop: 1, next: 'Canyon\u0000 Oasis' }, nocount: 'coop' })]).games[0].live;
    assert.deepEqual([p.pl, p.nocount], [{ i: 2, n: 3, loop: true, next: 'Canyon Oasis' }, 'coop']);
    // The latest kills, for watching it: at most 8, each as last is, odd ones dropped.
    const k = status([game({ ...FFA, kills: [['Kyle_K', 'Maw', 1, NOW - 5 * SEC], 'x', ['Maw', 'Kyle_\u0007K', 9, NOW], [1, 2, 3, 0]] })]).games[0].live;
    assert.deepEqual(k.kills, [['Kyle_K', 'Maw', 1, NOW - 5 * SEC], ['Maw', 'Kyle_K', 5, NOW]]);
    const many = Array.from({ length: 12 }, (_, i) => ['A', 'B', 1, NOW - (12 - i) * SEC]);
    assert.deepEqual(status([game({ ...FFA, kills: many })]).games[0].live.kills, many.slice(-8));
    for (const bad of [[], 'x', [['A', 'B', 1, 0]]])
        assert.equal(status([game({ ...FFA, kills: bad })]).games[0].live.kills, null, JSON.stringify(bad));
});

check('the page keeps whether a game can be watched, and how many are watching it', () => {
    const g = more => status([game(null, more)]).games[0];
    assert.deepEqual([g({ spect: 1, watching: 4 }).spect, g({ spect: 1, watching: 4 }).watching], [1, 4]);
    assert.deepEqual([g({ spect: 0 }).spect, g({ spect: 0 }).watching], [0, 0]);
    assert.equal(g({}).spect, -1, "a lobby that doesn't say: no Watch at all");
    assert.deepEqual([g({ spect: 'yes', watching: -3 }).spect, g({ spect: 'yes', watching: 'x' }).watching], [1, 0]);
});

check('the clock: the time left with a time limit, the time played without, and a level that ended', () => {
    const v = status([game(FFA)]).games[0].live;
    let c = lib.liveClock(v);
    assert.deepEqual([c.big, c.small], ['8:42', 'left of 15:00']);
    assert.ok(Math.abs(c.part - 0.42) < 0.001);
    NOW += 8 * MIN + 42 * SEC;
    c = lib.liveClock(v);
    assert.deepEqual([c.big, c.small, c.part], ["Time's up", '', 1]);
    NOW -= 8 * MIN + 42 * SEC;
    assert.deepEqual(lib.liveClock({ ...v, tl: 0 }).big, '6:18');
    assert.deepEqual(lib.liveClock({ ...v, tl: 0 }).small, 'played');
    assert.deepEqual(lib.liveClock({ ...v, over: v.at + 15 * MIN }), { big: 'Level over', small: '15:00 played' });
    assert.equal(lib.clockText(3 * 3600 * SEC + 62 * SEC), '3:01:02');
    assert.equal(lib.clockText(-5), '0:00');
    // The lobby's clock is ahead of this computer's: the times go by the lobby's.
    lobbyLive.skew = 60 * SEC;
    assert.equal(lib.liveClock(v).big, '7:42');
    lobbyLive.skew = 0;
});

check("a level that ended is shown for two minutes after, as the lobby keeps it, not more", () => {
    const g = status([game({ ...FFA, over: NOW - 20 * SEC })]).games[0];
    assert.ok(lib.gameLive(g));
    NOW += 2 * MIN;
    assert.equal(lib.gameLive(g), null);
    NOW -= 2 * MIN;
});

check("the table's line: who leads, or the teams, and the clock", () => {
    const v = status([game(FFA)]).games[0].live;
    assert.equal(lib.liveSummary(v), 'Kyle_K leads with 12 · 8:42 left');
    assert.equal(lib.liveSummary({ ...v, sb: [['A', 7, 7, 1, 0], ['B', 7, 7, 2, 0]] }), 'Tied at 7 · 8:42 left');
    assert.equal(lib.liveSummary({ ...v, tl: 0 }), 'Kyle_K leads with 12 · 6:18 played');
    assert.equal(lib.liveSummary({ ...v, sb: [] }), '8:42 left', 'a password game: no scores');
    assert.equal(lib.liveSummary({ ...v, over: NOW }), 'Level over');
    const t = { ...v, mode: 0x15, teams: [0, 4, 6, 0, 0], sb: [['Maw', 6, 9, 4, 2], ['Boc', 3, 5, 6, 1]] };
    assert.deepEqual(lib.liveTeams(t), [2, 1]);
    assert.equal(lib.liveSummary(t), 'Gold 6, Red 4 · 8:42 left');
    assert.deepEqual(lib.liveTeams({ ...t, mode: 0x14 }), [], 'teams only in a team game');
});

check('the last kill and the run going, in words', () => {
    const v = status([game(FFA)]).games[0].live;
    assert.equal(lib.liveFeed(v), 'Kyle_K killed MaraJade with a lightsaber, 14 s ago. Kyle_K: 4 in a row.');
    const feed = (last, ago = 2 * SEC) => lib.liveFeed({ last: [...last, NOW - ago], streak: null });
    assert.equal(feed(['Maw', 'Boc', 2]), 'Maw shot Boc, just now.');
    assert.equal(feed(['Maw', 'Boc', 3], 3 * MIN), 'Maw blew up Boc, 3 min ago.');
    assert.equal(feed(['Maw', 'Boc', 4]), 'Maw killed Boc with the Force, just now.');
    assert.equal(feed(['Maw', 'Boc', 0]), 'Maw killed Boc, just now.');
    assert.equal(feed(['', 'Boc', 5]), 'Boc died, just now.');
    assert.equal(lib.liveFeed({ last: null, streak: null }), '');
});

check('the rules, as Info has them', () => {
    const g = status([game(FFA)]).games[0];
    assert.deepEqual(lib.ruleRows(g, g.live), [['Mode', 'Free-for-all'], ['Time limit', '15 min'], ['Score limit', '20'],
                                               ['Max Force rank', '8'], ['Leaderboard', 'Counts']]);
    const early = { ...g.live, at: NOW - 30 * SEC };
    assert.deepEqual(lib.ruleRows(g, early).pop(), ['Leaderboard', 'Counts after 2 min']);
    assert.deepEqual(lib.ruleRows(g, { ...g.live, nocount: 'password' }).pop(), ['Leaderboard', "Doesn't count (password)"]);
    assert.deepEqual(lib.ruleRows(g, { ...g.live, nocount: 'coop' })[0], ['Mode', 'Co-op']);
    assert.deepEqual(lib.ruleRows(g, { ...g.live, nocount: 'coop' }).pop(), ['Leaderboard', "Doesn't count (co-op level)"]);
    assert.deepEqual(lib.ruleRows(g, { ...g.live, tl: 0, sl: 0 }).slice(1, 3), [['Time limit', 'None'], ['Score limit', 'None']]);
    assert.equal(lib.ruleRows({ ...g, rank: -1 }, g.live).length, 4, 'no rank from an older lobby');
    const ctf = { ...g, episode: 'JK1CTF.gob' };
    assert.equal(lib.modeText(ctf, 0x15, false), 'Capture the Flag');
    assert.equal(lib.modeText(g, 0x1, false), 'Teams');
    assert.equal(lib.modeText(g, 0x4, false), 'Free-for-all, scored by the level');
});

check('the room windows draw the live part, keep it moving, and pass it to the relay\'s games only as a mode', () => {
    assert.match(html, /if \(v\) \{\n\s+liveRows\(dl, g, v\);/);
    assert.match(html, /setInterval\(tickLive, 1000\);/);
    assert.match(html, /liveGames = \{\};/, 'forgotten at each redraw');
    assert.match(html, /mode: listed && typeof g\.mode === 'number'/);
});

console.log(failed ? `${failed} of ${passed + failed} failed` : `all ${passed} passed`);
process.exit(failed ? 1 : 0);
