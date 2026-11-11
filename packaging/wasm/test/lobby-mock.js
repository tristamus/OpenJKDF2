// Sample lobby data for designing the live counts and the room windows
// (Nar Shaddaa and Sith: Asteroid) in packaging/wasm/index.html without a
// lobby. Load it before the
// page's own scripts (a <script src> in <head>) and the page shows these
// instead of watching the lobby, with a bar to switch between them and to
// make players come and go. Each scenario has the Imperial relay's games
// too, as the site's Worker would answer for them (/imperial-games), in
// place of asking it, and the lobby's latest matches, as its /ladder/recent
// would answer (none in "Nobody on"). The names are made up, except
// Coruscant, the relay's own server.

window.jkLobbyMock = (function() {
    // n of these from the one at from on, for games too big to name everyone
    // by hand.
    var CROWD = ['MaraJade', 'Jerec', 'Sariss', 'Yun', 'Gorc', 'Boc', 'Maw', 'RahnJedi', 'Katarn99', 'NightSister',
                 'ForceGrip', 'Rodian_Ace', 'Gran_Pilot', 'SaberStorm', 'Ewok_Assassin', 'Qu_Rahn', 'DarkJedi_7',
                 'Tusken_Raider', 'Corran_H', 'Saber_Kyp', 'Dash_R', 'Vima_D', 'Rax_J', 'Talon_K', 'Kyle_K',
                 'Greedo_Shot', 'BountyHntr', 'Mon_Calamari', 'GammoreanG', 'TrandoshanX', 'Sith_Lurker', 'Duros_Dan'];
    function crowd(n, from) { return CROWD.concat(CROWD).slice(from, from + n); }

    // The Imperial relay's games, as the Worker answers for them: its own
    // server first (pinned), with this many players, then games hosted on
    // it by players.
    function relay(players, hosted) {
        var coruscant = players === null ?
            { key: '', name: 'Coruscant', game: 'jk1', map: '', episode: '', mod: '', locked: false, players: 0, max: 0,
              dedicated: true, pinned: true, listed: false, joinable: false, why: 'not-listed' } :
            { key: '13e5dd93743f59055bbf297508845b2b', name: 'Coruscant', game: 'jk1', map: 'c3.jkl', episode: 'JK1CTF',
              mod: '', locked: false, players: players, max: 31, dedicated: true, pinned: true, listed: true,
              joinable: true, why: '' };
        return { state: 'live', server: 'lobby.jkjunkie.com', checked: '2026-10-31T19:00:00.000Z',
                 games: [coruscant].concat(hosted || []) };
    }
    var RELAY_HOSTED = [
        { key: '5a1f0c9e2b7d4e8f9a3c6b0d1e2f3a4b', name: 'Kessel Run FFA', game: 'jk1', map: 'm10.jkl',
          episode: 'JK1MP', mod: '', locked: false, players: 2, max: 8, dedicated: false, pinned: false,
          listed: true, joinable: true, why: '' },
        { key: '6b2e1d0f3c8e5f9a0b4d7c1e2f3a4b5c', name: 'Old school duels', game: 'jk1', map: 'm_boss17.jkl',
          episode: 'JK1MP', mod: '', locked: false, players: 1, max: 4, dedicated: false, pinned: false,
          listed: true, joinable: false, why: 'desktop' },
        { key: '7c3f2e1a4d9f6a0b1c5e8d2f3a4b5c6d', name: 'Asteroid run', game: 'mots', map: 'k1.jkl',
          episode: 'JKM_KFY', mod: '', locked: false, players: 2, max: 4, dedicated: false, pinned: false,
          listed: true, joinable: true, why: '' }
    ];

    // Each scenario's players not in a game, by the game they run, and
    // its games. The status the page gets is made from these. imperial is
    // the relay's games.
    var SCENARIOS = {
        busy: {
            label: 'Busy night',
            imperial: relay(3, RELAY_HOSTED),
            status: {
                lobby: { jk1: ['DarkJedi_7', 'Tusken_Raider', 'Qu_Rahn'], mots: ['Corran_H', 'Saber_Kyp'] },
                games: [
                    { id: 41, name: 'No mods dueling', game: 'jk1', map: 'm_boss17.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 2, max: 4, names: ['Kyle_K', 'MaraJade'] },
                    { id: 44, name: 'SBX 3.1 sabers', game: 'jk1', map: 'jhs.jkl', title: "Jedi's High School", episode: 'jhs.gob',
                      mod: 'Saber Battle X', locked: true, players: 4, max: 4, names: ['Jerec', 'Sariss', 'Yun', 'Gorc'] },
                    { id: 45, name: 'Asteroid duels', game: 'mots', map: 'mdm02_freezer.jkl', episode: 'JKM_MP.goo',
                      mod: '', locked: false, players: 1, max: 4, names: ['Dash_R'] },
                    { id: 46, name: 'MotS FFA', game: 'mots', map: 'msb2_gantry.jkl', episode: 'JKM_SABER.goo',
                      mod: '', locked: false, players: 3, max: 8, names: ['Vima_D', 'Rax_J', 'Talon_K'] },
                    { id: 47, name: 'CTF practice', game: 'jk1', map: 'c3.jkl', episode: 'JK1CTF.gob',
                      mod: '', locked: false, players: 3, max: 8, names: ['Boc', 'Maw', 'RahnJedi'] },
                    { id: 52, name: 'Rotating maps', game: 'jk1', map: 'APG_Deathmatch_Castle.jkl', title: 'APG_DEATHMATCH_CASTLE',
                      episode: 'APG_Deathmatch_Castle.gob',
                      mod: '', locked: false, players: 6, max: 8,
                      names: ['Katarn99', 'NightSister', 'ForceGrip', 'Rodian_Ace', 'Gran_Pilot', 'SaberStorm'] },
                    { id: 53, name: 'newbies welcome', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1.gob',
                      mod: '', locked: false, players: 1, max: 2, names: ['Ewok_Assassin'] }
                ]
            }
        },
        big: {
            label: 'Big games',
            imperial: relay(12),
            status: {
                lobby: { jk1: ['Jawa_Juice'], mots: ['KelDor_K'] },
                games: [
                    { id: 71, name: 'Deathmatch night', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 3, max: 16, names: crowd(3, 0) },
                    { id: 72, name: 'Full house', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 16, max: 16, names: crowd(16, 3) },
                    { id: 73, name: 'Team sabers', game: 'mots', map: 'jkm_mp.jkl', episode: 'JKM_MP.goo',
                      mod: '', locked: false, players: 9, max: 24, names: crowd(9, 19) },
                    { id: 74, name: 'CTF all night', game: 'jk1', map: 'ctf1.jkl', episode: 'JK1CTF.gob',
                      mod: '', locked: false, players: 11, max: 32, names: crowd(11, 28) },
                    { id: 75, name: 'Big CTF', game: 'jk1', map: 'ctf1.jkl', episode: 'JK1CTF.gob',
                      mod: '', locked: false, players: 29, max: 32, names: crowd(29, 7) },
                    { id: 76, name: 'Everyone on', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 32, max: 32, names: crowd(32, 13) }
                ]
            }
        },
        quiet: {
            label: 'Quiet',
            imperial: relay(0),
            status: {
                lobby: { jk1: ['MaraJade'], mots: ['Dash_R'] },
                games: [
                    { id: 12, name: 'Anyone up for a duel?', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 1, max: 4, names: ['Kyle_K'] }
                ]
            }
        },
        // A game whose host's build speaks a newer netcode (net) than the
        // page's (NETCODE in index.html), as one hosted after an update would
        // look to a page opened before it.
        versions: {
            label: 'Another version',
            imperial: relay(0),
            status: {
                lobby: { jk1: ['MaraJade'], mots: [] },
                games: [
                    { id: 31, name: 'No mods dueling', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 2, max: 4, names: ['Kyle_K', 'Jerec'] },
                    { id: 32, name: 'After the update', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 1, max: 4, names: ['Rahn_J'], net: 1 }
                ]
            }
        },
        empty: { label: 'Nobody on', imperial: relay(null), status: { lobby: { jk1: [], mots: [] }, games: [] } },
        down: { label: 'Lobby not answering', imperial: relay(1), status: null },
        relayDown: {
            label: 'Imperial relay not answering',
            imperial: { state: 'down', server: 'lobby.jkjunkie.com', checked: '2026-10-31T19:00:00.000Z',
                        games: [{ key: '', name: 'Coruscant', game: 'jk1', map: '', episode: '', mod: '',
                                  locked: false, players: 0, max: 0, dedicated: true, pinned: true, listed: false,
                                  joinable: false, why: 'relay-down' }] },
            status: {
                lobby: { jk1: ['MaraJade'], mots: ['Dash_R'] },
                games: [
                    { id: 12, name: 'Anyone up for a duel?', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 1, max: 4, names: ['Kyle_K'] }
                ]
            }
        }
    };
    var NEWCOMERS = ['Greedo_Shot', 'BountyHntr', 'Mon_Calamari', 'GammoreanG', 'TrandoshanX', 'Sith_Lurker',
                     'Duros_Dan', 'Ithorian', 'Jawa_Juice', 'KelDor_K', 'Twilek_T', 'Wookiee_W'];

    var show = null;
    var showImperial = null;
    var current = 'busy';

    // The latest matches, as the lobby's /ladder/recent answers: newest
    // first, each so many minutes ago.
    var MATCHES = {
        jk1: [
            { id: 41, title: 'Canyon Oasis', mod: '', pack: 0, custom: 0, players: 4, secs: 754, ago: 3,
              teams: false, name: 'Kyle_K', team: 0, score: 15 },
            { id: 40, title: 'Battleground Jedi', mod: 'SBX 3.1', pack: 0, custom: 0, players: 6, secs: 1210, ago: 26,
              teams: false, name: 'MaraJade', team: 0, score: 22 },
            { id: 38, title: 'Drazen Isle', mod: 'SBX 3.1', pack: 0, custom: 1, players: 8, secs: 1805, ago: 95,
              teams: true, name: null, team: 1, score: 40 },
            { id: 37, title: 'Nar Shaddaa Loading Terminal', mod: '', pack: 1, custom: 0, players: 2, secs: 412,
              ago: 190, teams: false, name: '', team: 0, score: 9 },
            { id: 35, title: 'Blades of Death', mod: 'Saber Battle X', pack: 0, custom: 0, players: 3, secs: 660,
              ago: 60 * 26, teams: false, name: 'Jerec', team: 0, score: 11 },
            { id: 33, title: 'Valley of the Jedi Tower', mod: '', pack: 0, custom: 0, players: 5, secs: 900,
              ago: 60 * 50, teams: false, name: null, team: 0, score: null },
            { id: 31, title: 'Oasis Night (a level with a very long name indeed)', mod: 'JK Enhanced with everything', pack: 0,
              custom: 1, players: 12, secs: 7400, ago: 60 * 24 * 9, teams: false, name: 'Ewok_Assassin_The_Third', team: 0,
              score: 104 }
        ],
        mots: [
            { id: 39, title: 'Asteroid Outpost', mod: '', pack: 0, custom: 0, players: 3, secs: 540, ago: 48,
              teams: false, name: 'Dash_R', team: 0, score: 8 },
            { id: 36, title: 'Altyr 5', mod: 'MotS Tweaks', pack: 0, custom: 0, players: 4, secs: 1320, ago: 60 * 7,
              teams: true, name: null, team: 3, score: 31 }
        ]
    };
    // The standings, as /ladder/recent answers them: each game's leaders
    // over each period, and the viewer (Kyle_K, with their latest match).
    function board(players, of, matches, you) {
        return { players: players.map(function(p) {
                     return { id: p[0], name: p[1], score: p[2], kills: p[3], deaths: p[4], matches: p[5] };
                 }), of: of, matches: matches, you: you || null };
    }
    var JK_WEEK = [[7, 'Kyle_K', 213, 215, 103, 11], [12, 'Boc', 172, 191, 254, 14], [3, 'Maw', 169, 177, 112, 8],
                   [5, 'Jerec', 168, 171, 142, 10], [9, 'Yun', 166, 169, 71, 8], [21, 'Jan_Ors', 126, 130, 193, 11],
                   [14, 'Qu_Rahn', 86, 87, 120, 6], [18, 'Pic', 51, 56, 114, 5], [2, 'Sariss', 24, 25, 12, 2],
                   [25, 'Gorc', 17, 20, 43, 3]];
    var JK_ALL = [[9, 'Yun', 1840, 1866, 902, 96], [7, 'Kyle_K', 1622, 1650, 1011, 88],
                  [5, 'Jerec', 1433, 1470, 1204, 104], [12, 'Boc', 1318, 1402, 1877, 131], [3, 'Maw', 990, 1012, 760, 61],
                  [21, 'Jan_Ors', 811, 840, 1190, 77], [30, 'Ewok_Assassin_The_Third', 640, 655, 512, 40],
                  [14, 'Qu_Rahn', 512, 530, 701, 44], [18, 'Pic', 401, 433, 820, 52], [2, 'Sariss', 288, 290, 143, 19]];
    var TOP = {
        jk1: { period: 'week', week: board(JK_WEEK, 14, 19, { rank: 1, score: 213 }),
               month: board(JK_ALL.slice(1).concat(JK_ALL.slice(0, 1)), 23, 61, { rank: 1, score: 1622 }),
               all: board(JK_ALL, 37, 244, { rank: 2, score: 1622 }) },
        mots: { period: 'month', week: board([], 0, 0),
                month: board([[31, 'Dash_R', 41, 44, 30, 4], [33, 'Vima_D', 29, 30, 35, 3], [7, 'Kyle_K', 12, 14, 9, 1]], 3, 5,
                             { rank: 3, score: 12 }),
                all: board([[31, 'Dash_R', 96, 101, 80, 9], [33, 'Vima_D', 54, 57, 71, 7], [7, 'Kyle_K', 12, 14, 9, 1]], 3, 12,
                           { rank: 3, score: 12 }) }
    };
    var live = null;      // the status as players come and go
    var moving = null;    // the timer making them
    var nextId = 60;
    var controls = [];

    function copy(s) { return s ? JSON.parse(JSON.stringify(s)) : null; }
    function pick(list) { return list[Math.floor(Math.random() * list.length)]; }

    // The status the lobby would send for a scenario's players and games.
    function toStatus(s) {
        if (!s) return null;
        var byGame = {}, all = [], online = 0;
        Object.keys(s.lobby).forEach(function(k) {
            var playing = s.games.reduce(function(n, g) { return n + (g.game === k ? g.players : 0); }, 0);
            byGame[k] = { online: s.lobby[k].length + playing, lobby: s.lobby[k].slice().sort() };
            all = all.concat(s.lobby[k]);
            online += byGame[k].online;
        });
        return { online: online, lobby: all.sort(), byGame: byGame, games: copy(s.games),
                 match: current === 'empty' ? 0 : MATCHES.jk1[0].id };
    }

    // One thing happens in one game: someone comes online, leaves, joins or
    // leaves a game, or hosts one. A game whose host leaves closes.
    function step(s) {
        var k = Math.random() < 0.65 ? 'jk1' : 'mots';
        var lobby = s.lobby[k];
        var mine = s.games.filter(function(g) { return g.game === k; });
        var open = mine.filter(function(g) { return !g.locked && g.players < g.max; });
        var r = Math.random();
        if (r < 0.3 && lobby.length && open.length) {
            var g = pick(open);
            var who = lobby.splice(Math.floor(Math.random() * lobby.length), 1)[0];
            g.names.push(who);
            g.players++;
        } else if (r < 0.5 && mine.length) {
            var h = pick(mine);
            if (h.players > 1) {
                lobby.push(h.names.splice(1 + Math.floor(Math.random() * (h.players - 1)), 1)[0]);
                h.players--;
            } else {
                lobby.push(h.names[0]);
                s.games.splice(s.games.indexOf(h), 1);
            }
        } else if (r < 0.7) {
            var fresh = NEWCOMERS.filter(function(n) {
                return s.lobby.jk1.indexOf(n) < 0 && s.lobby.mots.indexOf(n) < 0 &&
                       !s.games.some(function(g) { return g.names.indexOf(n) >= 0; });
            });
            if (fresh.length) lobby.push(pick(fresh));
        } else if (r < 0.85 && lobby.length > 1) {
            lobby.splice(Math.floor(Math.random() * lobby.length), 1);
        } else if (lobby.length && mine.length < 12) {   // past nine, so a room opens another row
            var host = lobby.shift();
            s.games.push(k === 'mots' ?
                { id: nextId++, name: host + "'s game", game: 'mots', map: 'jkm_mp.jkl', episode: 'JKM_MP.goo',
                  mod: '', locked: false, players: 1, max: 4, names: [host] } :
                { id: nextId++, name: host + "'s game", game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                  mod: '', locked: false, players: 1, max: 4, names: [host] });
        }
    }

    function setMoving(on) {
        clearInterval(moving);
        moving = null;
        if (on && live) {
            moving = setInterval(function() {
                step(live);
                show(toStatus(live));
            }, 2500);
        }
        controls.forEach(function(c) { c.check.checked = on; });
    }

    function choose(name) {
        current = name;
        var wasMoving = !!moving;
        live = copy(SCENARIOS[name].status);
        show(toStatus(live), 'down');
        if (showImperial) showImperial(copy(SCENARIOS[name].imperial), 'off');
        controls.forEach(function(c) { c.select.value = name; c.check.disabled = !live; });
        setMoving(wasMoving && !!live);
    }

    // A scenario picker and a "players come and go" switch; one copy on the
    // page and one in the room window, whose modal hides the page's.
    function makeControls(idSuffix) {
        var wrap = document.createElement('span');
        wrap.className = 'mock-controls';
        var label = document.createElement('label');
        label.textContent = 'Sample data ';
        var select = document.createElement('select');
        select.id = 'mock-scenario-' + idSuffix;
        Object.keys(SCENARIOS).forEach(function(k) {
            var o = document.createElement('option');
            o.value = k;
            o.textContent = SCENARIOS[k].label;
            select.append(o);
        });
        select.value = current;
        select.addEventListener('change', function() { choose(select.value); });
        label.append(select);
        var checkLabel = document.createElement('label');
        var check = document.createElement('input');
        check.type = 'checkbox';
        check.id = 'mock-moving-' + idSuffix;
        check.addEventListener('change', function() { setMoving(check.checked); });
        checkLabel.append(check, ' Players come and go');
        wrap.append(label, checkLabel);
        controls.push({ select: select, check: check });
        return wrap;
    }

    function addStyles() {
        var css = document.createElement('style');
        css.textContent =
            '#mock-bar { position: fixed; z-index: 20; left: 0; right: 0; bottom: 0; display: flex; flex-wrap: wrap;' +
            ' align-items: center; gap: 6px 16px; padding: 8px max(16px, env(safe-area-inset-right, 0px))' +
            ' calc(8px + env(safe-area-inset-bottom, 0px)) max(16px, env(safe-area-inset-left, 0px));' +
            ' background: #fffbe6; border-top: 2px solid #000; color: #000; font: 13px Verdana, Geneva, sans-serif; }' +
            '#mock-bar b { font-weight: bold; }' +
            '.mock-controls { display: inline-flex; flex-wrap: wrap; align-items: center; gap: 4px 14px; }' +
            '.mock-controls select { margin-left: 4px; font: inherit; }' +
            '#zone .footer { padding-bottom: 72px; }';
        document.head.append(css);
    }

    return {
        attach: function(showStatus, showRelay) {
            show = showStatus;
            showImperial = showRelay || null;
            addStyles();
            var bar = document.createElement('div');
            bar.id = 'mock-bar';
            var note = document.createElement('span');
            var NOTE = '<b>Mock-up.</b> Live counts use sample data; nothing here connects to the lobby.';
            note.innerHTML = NOTE;
            note.setAttribute('role', 'status');
            bar.append(note, makeControls('page'));
            document.body.append(bar);
            // Play and the rooms open the folder prompt as they do live, but
            // its button says what would happen instead of asking for a game
            // folder the mock-up can't start.
            function chooser(ev) {
                if (!ev.target.closest('#picker label.play')) return;
                if (ev.type === 'keydown' && ev.key !== 'Enter' && ev.key !== ' ') return;
                ev.preventDefault();
                ev.stopPropagation();
                var result = document.getElementById('picker-result');
                result.className = '';
                if (!pickerWant) {
                    result.textContent = 'Mock-up: on the live page, this asks for a game folder and starts ' +
                        'whichever game is in it, Jedi Knight or Mysteries of the Sith.';
                    return;
                }
                var other = pickerWant.game === 'mots' ? 'jk1' : 'mots';
                result.textContent = 'Mock-up: on the live page, this asks for your ' + GAME_NAMES[pickerWant.game] +
                    ' folder and starts it. A ' + GAME_NAMES[other] + ' folder gets this instead: "' +
                    wrongGameText(pickerWant, other) + '"';
            }
            document.addEventListener('click', chooser, true);
            document.addEventListener('keydown', chooser, true);
            var extra = document.getElementById('room-status-extra');
            if (extra) extra.append(makeControls('room'));
            show(null, 'connecting');
            setTimeout(function() { choose(current); }, 700);
        },
        // What the lobby's /ladder/recent would answer.
        recent: function() {
            var now = Date.now(), games = {};
            Object.keys(MATCHES).forEach(function(k) {
                games[k] = current === 'empty' ? [] : MATCHES[k].map(function(m) {
                    var out = copy(m);
                    out.at = now - m.ago * 60000;
                    delete out.ago;
                    return out;
                });
            });
            if (current === 'empty') {
                var none = { period: 'week', week: board([], 0, 0), month: board([], 0, 0), all: board([], 0, 0) };
                return { v: 1, games: games, top: { jk1: none, mots: none }, you: null };
            }
            return { v: 1, games: games, top: copy(TOP), you: { id: 7, name: 'Kyle_K', last: 41 } };
        },
        describe: function(kind, label, quick, want) {
            var folder = 'your ' + GAME_NAMES[want.game] + ' folder';
            if (kind === 'host')
                return 'Mock-up: on the live page, Host asks for ' + folder + '. Once the game starts, ' +
                       'choose Multiplayer, then Host.';
            if (kind === 'join-imperial')
                return 'Mock-up: on the live page, this asks for ' + folder + ', and the game starts with the ' +
                       "Imperial relay's connection picked on its Multiplayer menu and " + label +
                       ' already selected on its Join screen.';
            if (!label)
                return 'Mock-up: on the live page, Quick Join asks for ' + folder + ', then opens the Join screen. ' +
                       "There's no open game without a password to pick here.";
            return 'Mock-up: on the live page, this asks for ' + folder + ', and the game starts with ' + label +
                   ' already selected on its Join screen' +
                   (quick ? ' (Quick Join picks the open game with the most players).' : '.');
        }
    };
})();
