// Sample lobby data for designing the live counts and Nar Shaddaa's room
// window in packaging/wasm/index.html without a lobby. Load it before the
// page's own scripts (a <script src> in <head>) and the page shows these
// instead of watching the lobby, with a bar to switch between them and to
// make players come and go. The names are made up.

window.jkLobbyMock = (function() {
    var SCENARIOS = {
        busy: {
            label: 'Busy night',
            status: {
                online: 20,
                lobby: ['DarkJedi_7', 'Tusken_Raider', 'Qu_Rahn'],
                games: [
                    { id: 41, name: 'No mods dueling', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 2, max: 4, names: ['Kyle_K', 'MaraJade'] },
                    { id: 44, name: 'SBX 3.1 sabers', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: 'Saber Battle X', locked: true, players: 4, max: 4, names: ['Jerec', 'Sariss', 'Yun', 'Gorc'] },
                    { id: 45, name: 'Asteroid duels', game: 'mots', map: 'jkm_mp.jkl', episode: 'JKM_MP.goo',
                      mod: '', locked: false, players: 1, max: 4, names: ['Dash_R'] },
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
        quiet: {
            label: 'Quiet',
            status: {
                online: 2,
                lobby: ['MaraJade'],
                games: [
                    { id: 12, name: 'Anyone up for a duel?', game: 'jk1', map: '01narshadda.jkl', episode: 'JK1MP.gob',
                      mod: '', locked: false, players: 1, max: 4, names: ['Kyle_K'] }
                ]
            }
        },
        empty: { label: 'Nobody on', status: { online: 0, lobby: [], games: [] } },
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

    function total(s) {
        return s.lobby.length + s.games.reduce(function(n, g) { return n + g.players; }, 0);
    }

    // One thing happens: someone comes online, leaves, joins or leaves a
    // game, or hosts one. A game whose host leaves closes.
    function step(s) {
        var open = s.games.filter(function(g) { return !g.locked && g.players < g.max; });
        var r = Math.random();
        if (r < 0.3 && s.lobby.length && open.length) {
            var g = pick(open);
            var who = s.lobby.splice(Math.floor(Math.random() * s.lobby.length), 1)[0];
            g.names.push(who);
            g.players++;
        } else if (r < 0.5 && s.games.length) {
            var h = pick(s.games);
            if (h.players > 1) {
                s.lobby.push(h.names.splice(1 + Math.floor(Math.random() * (h.players - 1)), 1)[0]);
                h.players--;
            } else {
                s.lobby.push(h.names[0]);
                s.games.splice(s.games.indexOf(h), 1);
            }
        } else if (r < 0.7) {
            var fresh = NEWCOMERS.filter(function(n) {
                return s.lobby.indexOf(n) < 0 && !s.games.some(function(g) { return g.names.indexOf(n) >= 0; });
            });
            if (fresh.length) s.lobby.push(pick(fresh));
        } else if (r < 0.85 && s.lobby.length > 1) {
            s.lobby.splice(Math.floor(Math.random() * s.lobby.length), 1);
        } else if (s.lobby.length && s.games.length < 9) {
            var host = s.lobby.shift();
            s.games.push({ id: nextId++, name: host + "'s game", game: 'jk1', map: '01narshadda.jkl',
                           episode: 'JK1MP.gob', mod: '', locked: false, players: 1, max: 4, names: [host] });
        }
        s.online = total(s);
    }

    function setMoving(on) {
        clearInterval(moving);
        moving = null;
        if (on && live) {
            moving = setInterval(function() {
                step(live);
                show(copy(live));
            }, 2500);
        }
        controls.forEach(function(c) { c.check.checked = on; });
    }

    function choose(name) {
        current = name;
        var wasMoving = !!moving;
        live = copy(SCENARIOS[name].status);
        show(live ? copy(live) : null, 'down');
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
            // Play and the other rooms say what they'd do instead of asking
            // for a game folder the mock-up can't start.
            var noteTimer = null;
            document.addEventListener('click', function(ev) {
                if (!ev.target.closest('[data-play]')) return;
                ev.preventDefault();
                ev.stopPropagation();
                note.innerHTML = '<b>Mock-up.</b> On the live page, this opens the game folder prompt and starts the game.';
                clearTimeout(noteTimer);
                noteTimer = setTimeout(function() { note.innerHTML = NOTE; }, 6000);
            }, true);
            var extra = document.getElementById('room-status-extra');
            if (extra) extra.append(makeControls('room'));
            show(null, 'connecting');
            setTimeout(function() { choose(current); }, 700);
        },
        describe: function(kind, label, quick) {
            if (kind === 'host')
                return 'Mock-up: on the live page, Host opens the game folder prompt. Once the game starts, ' +
                       'choose Multiplayer, then Host.';
            if (!label)
                return 'Mock-up: on the live page, Quick Join opens the game folder prompt, then the Join screen. ' +
                       "There's no open game without a password to pick here.";
            return 'Mock-up: on the live page, this opens the game folder prompt, and the game starts with ' + label +
                   ' already selected on its Join screen' +
                   (quick ? ' (Quick Join picks the open game with the most players).' : '.');
        }
    };
})();
