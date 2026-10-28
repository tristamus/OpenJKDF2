// Sample lobby data for designing the live counts and the room windows
// (Nar Shaddaa and Sith: Asteroid) in packaging/wasm/index.html without a
// lobby. Load it before the
// page's own scripts (a <script src> in <head>) and the page shows these
// instead of watching the lobby, with a bar to switch between them and to
// make players come and go. The names are made up.

window.jkLobbyMock = (function() {
    // n of these from the one at from on, for games too big to name everyone
    // by hand.
    var CROWD = ['MaraJade', 'Jerec', 'Sariss', 'Yun', 'Gorc', 'Boc', 'Maw', 'RahnJedi', 'Katarn99', 'NightSister',
                 'ForceGrip', 'Rodian_Ace', 'Gran_Pilot', 'SaberStorm', 'Ewok_Assassin', 'Qu_Rahn', 'DarkJedi_7',
                 'Tusken_Raider', 'Corran_H', 'Saber_Kyp', 'Dash_R', 'Vima_D', 'Rax_J', 'Talon_K', 'Kyle_K',
                 'Greedo_Shot', 'BountyHntr', 'Mon_Calamari', 'GammoreanG', 'TrandoshanX', 'Sith_Lurker', 'Duros_Dan'];
    function crowd(n, from) { return CROWD.concat(CROWD).slice(from, from + n); }

    // Each scenario's players not in a game, by the game they run, and
    // its games. The status the page gets is made from these.
    var SCENARIOS = {
        busy: {
            label: 'Busy night',
            status: {
                lobby: { jk1: ['DarkJedi_7', 'Tusken_Raider', 'Qu_Rahn'], mots: ['Corran_H', 'Saber_Kyp'] },
                games: [
                    { id: 41, name: 'No mods dueling', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 2, max: 4, names: ['Kyle_K', 'MaraJade'] },
                    { id: 44, name: 'SBX 3.1 sabers', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: 'Saber Battle X', locked: true, players: 4, max: 4, names: ['Jerec', 'Sariss', 'Yun', 'Gorc'] },
                    { id: 45, name: 'Asteroid duels', game: 'mots', map: 'jkm_mp.jkl', episode: 'JKM_MP.goo',
                      mod: '', locked: false, players: 1, max: 4, names: ['Dash_R'] },
                    { id: 46, name: 'MotS FFA', game: 'mots', map: 'jkm_mp.jkl', episode: 'JKM_MP.goo',
                      mod: '', locked: false, players: 3, max: 8, names: ['Vima_D', 'Rax_J', 'Talon_K'] },
                    { id: 47, name: 'CTF practice', game: 'jk1', map: 'ctf1.jkl', episode: 'JK1CTF.gob',
                      mod: '', locked: false, players: 3, max: 8, names: ['Boc', 'Maw', 'RahnJedi'] },
                    { id: 52, name: 'Rotating maps', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 6, max: 8,
                      names: ['Katarn99', 'NightSister', 'ForceGrip', 'Rodian_Ace', 'Gran_Pilot', 'SaberStorm'] },
                    { id: 53, name: 'newbies welcome', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 1, max: 2, names: ['Ewok_Assassin'] }
                ]
            }
        },
        big: {
            label: 'Big games',
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
            status: {
                lobby: { jk1: ['MaraJade'], mots: ['Dash_R'] },
                games: [
                    { id: 12, name: 'Anyone up for a duel?', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 1, max: 4, names: ['Kyle_K'] }
                ]
            }
        },
        empty: { label: 'Nobody on', status: { lobby: { jk1: [], mots: [] }, games: [] } },
        down: { label: 'Lobby not answering', status: null }
    };
    var NEWCOMERS = ['Greedo_Shot', 'BountyHntr', 'Mon_Calamari', 'GammoreanG', 'TrandoshanX', 'Sith_Lurker',
                     'Duros_Dan', 'Ithorian', 'Jawa_Juice', 'KelDor_K', 'Twilek_T', 'Wookiee_W'];

    var show = null;
    var current = 'busy';
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
        return { online: online, lobby: all.sort(), byGame: byGame, games: copy(s.games) };
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
        } else if (lobby.length && mine.length < 9) {
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
        attach: function(showStatus) {
            show = showStatus;
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
        describe: function(kind, label, quick, want) {
            var folder = 'your ' + GAME_NAMES[want.game] + ' folder';
            if (kind === 'host')
                return 'Mock-up: on the live page, Host asks for ' + folder + '. Once the game starts, ' +
                       'choose Multiplayer, then Host.';
            if (!label)
                return 'Mock-up: on the live page, Quick Join asks for ' + folder + ', then opens the Join screen. ' +
                       "There's no open game without a password to pick here.";
            return 'Mock-up: on the live page, this asks for ' + folder + ', and the game starts with ' + label +
                   ' already selected on its Join screen' +
                   (quick ? ' (Quick Join picks the open game with the most players).' : '.');
        }
    };
})();
