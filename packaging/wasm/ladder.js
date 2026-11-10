// The match record's game side for the browser build: tells the lobby what
// this game sees of its match, so the lobby can keep a record of every match
// with a page of its own. Bundled into openjkdf2.js with --pre-js
// (cmake_modules/plat_wasm.cmake), so it always matches the engine it ships
// with. The lobby's src/ladder.js (in the jkdf2mp-bb-server repository) says
// what it does with the reports.
//
// The game has no server, and every game keeps score by itself: when a player
// dies, their own game tells the others who did it, and each game counts it.
// So each game reports the kills it counts, and the lobby takes a kill two
// games reported as one that was announced in the match. The host's game also
// sends its scoreboard, which is the one the other games take over.
//
// The engine calls in here (stdComm_WSRelay.c, the match record): start when
// a multiplayer level is loaded, kill for each kill it counts, scores when
// the scoreboard may have changed, end when the level is left. Reports go to
// the lobby as text lines on the socket the game already has,
//
//   match <base64 of UTF-8 JSON>
//
// only in a room of a lobby that said it keeps a record ("lobby ... match"),
// so a lobby without one, an older one, and the Imperial relay never get any.
// One goes when the level starts, within KILL_MS of a kill (kills that come
// close together go in one), every BEAT_MS otherwise, and when the level ends
// or the page closes. The lobby never answers one. Kills seen while the
// socket is down wait for it, each with how long ago it was.
//
// The page says who the player is: Module.jkLadderKey is their browser key
// (from the lobby's /ladder/key), and Module.jkLadderAnon is true when they
// asked not to be recorded by name, in which case the key isn't sent either.
// When the lobby has saved a match this game reported, it says "matched
// <id>", and the page's Module.jkLadderSaved(id), if there, is called with it.

(function() {
    var KILL_MS = 2000;
    var BEAT_MS = 30000;
    var RETRY_MS = 1000;
    // Kills in one report, as many as the lobby takes, and reports at once.
    var MAX_EVENTS = 64;
    var MAX_AT_ONCE = 4;
    // Kills kept while the lobby can't be reached, and for how long.
    var MAX_WAITING = 1024;
    var MAX_AGO_MS = 10 * 60 * 1000;

    function now() {
        return typeof performance !== 'undefined' ? performance.now() : Date.now();
    }

    function b64(text) {
        var bytes = new TextEncoder().encode(text);
        var s = '';
        for (var i = 0; i < bytes.length; i += 4096)
            s += String.fromCharCode.apply(null, bytes.subarray(i, i + 4096));
        return btoa(s);
    }

    // The level's file as the lobby takes it: every game in the room has the
    // same one, so they all name it the same.
    function mapName(file) {
        return String(file || '').replace(/[^A-Za-z0-9_.-]/g, '_').slice(0, 32) || 'unknown';
    }

    var L = {
        on: false,      // in a level that's being reported
        lv: 0,          // which level of this page's it is, from 1
        map: '', title: '', build: '',
        host: false,    // this game hosts, so its rules and scoreboard go too
        idle: false,    // its player isn't playing (a host that only serves)
        rules: null,
        since: 0,       // when this game got into the level
        n: 0,           // kills counted in it
        waiting: [],    // kills not reported yet: {n, at, victim, killer, kind}
        sb: null, teams: null,
        sentAt: 0,
        timer: null, soon: null
    };

    // Whether a report can go now: the socket is open, this game has a seat in
    // a room, and the lobby keeps a record.
    L.canSend = function() {
        var net = Module.jkNet;
        return !!(net && net.ws && net.state === 1 && net.session && net.seat && net.lobbyCan &&
                  net.lobbyCan.indexOf('match') >= 0);
    };

    L.report = function(end) {
        var t = now();
        var kills = L.waiting.slice(0, MAX_EVENTS);
        var r = {
            v: 1, lv: L.lv, map: L.map, title: L.title, t: Math.max(0, Math.round(t - L.since)), build: L.build,
            ev: kills.map(function(k) {
                return [k.n, Math.min(MAX_AGO_MS, Math.max(0, Math.round(t - k.at))), k.victim, k.killer, k.kind];
            })
        };
        if (Module.jkLadderAnon)
            r.anon = 1;
        else if (typeof Module.jkLadderKey === 'string' && Module.jkLadderKey)
            r.pk = Module.jkLadderKey;
        if (L.idle)
            r.idle = 1;
        if (L.host) {
            r.rules = L.rules;
            if (L.sb) {
                r.sb = L.sb;
                r.teams = L.teams;
            }
        }
        if (end)
            r.end = end;
        return { line: 'match ' + b64(JSON.stringify(r)), kills: kills.length };
    };

    // Sends what's waiting, and the level's end if this is it. False if the
    // lobby can't be reached, or keeps no record.
    L.send = function(end) {
        if (!L.canSend())
            return false;
        // What's too old to place is dropped.
        var t = now();
        while (L.waiting.length && t - L.waiting[0].at > MAX_AGO_MS)
            L.waiting.shift();
        for (var i = 0; i < MAX_AT_ONCE; i++) {
            var last = L.waiting.length <= MAX_EVENTS;
            var r = L.report(last ? end : '');
            try {
                Module.jkNet.ws.send(r.line);
            } catch (e) {
                return false;
            }
            L.waiting.splice(0, r.kills);
            L.sentAt = t;
            if (last)
                return true;
        }
        return !end;
    };

    L.tick = function() {
        if (!L.on)
            return;
        var t = now();
        if ((L.waiting.length && t - L.waiting[0].at >= KILL_MS) || t - L.sentAt >= BEAT_MS)
            L.send('');
    };

    L.stop = function() {
        L.on = false;
        clearInterval(L.timer);
        clearTimeout(L.soon);
        L.timer = L.soon = null;
        L.waiting = [];
    };

    // ---- What the engine calls.

    L.start = function(map, title, mode, scoreLimit, timeLimitMin, host, idle, build) {
        L.stop();
        L.on = true;
        L.lv++;
        L.map = mapName(map);
        L.title = String(title || '').slice(0, 48);
        L.build = String(build || '');
        L.host = !!host;
        L.idle = !!idle;
        L.rules = { mode: mode >>> 0, sl: Math.max(0, scoreLimit | 0), tl: Math.max(0, timeLimitMin | 0) };
        L.since = now();
        L.n = 0;
        L.sb = L.teams = null;
        L.sentAt = 0;
        L.timer = setInterval(L.tick, RETRY_MS);
        // Once the engine has given the scoreboard, which comes right after.
        L.soon = setTimeout(function() { L.soon = null; if (L.on) L.send(''); }, 0);
    };

    L.kill = function(victim, killer, kind) {
        if (!L.on)
            return;
        L.waiting.push({ n: ++L.n, at: now(), victim: victim | 0, killer: killer | 0, kind: kind | 0 });
        if (L.waiting.length > MAX_WAITING)
            L.waiting.shift();
    };

    L.scores = function(rows, teams) {
        if (!L.on)
            return;
        L.sb = rows;
        L.teams = teams;
    };

    L.end = function(left) {
        if (!L.on)
            return;
        L.send(left ? 'left' : 'level');
        L.stop();
    };

    // The lobby saved a match this game reported.
    L.saved = function(id) {
        if (!(id > 0))
            return;
        L.lastSaved = id;
        try {
            if (typeof Module.jkLadderSaved === 'function')
                Module.jkLadderSaved(id);
        } catch (e) {
            console.error('OpenJKDF2: the page failed to take match ' + id, e);
        }
    };

    // A page that closes in a match: its last word, before the network code
    // says leave (its listener was added after this one).
    if (typeof window !== 'undefined') {
        window.addEventListener('pagehide', function() {
            if (L.on)
                L.end(true);
        });
    }

    Module.jkLadder = L;
})();
