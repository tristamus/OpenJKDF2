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
// A level that ends while the socket is down (the lobby restarting, a
// network that dropped for a moment) can't say so then, and the game goes on
// to its next level. Its last report, with the kills still waiting and from
// the host the scoreboard as the level ended, is kept (L.last) and goes as
// soon as a report can go again, before anything of the next level:
// - from the host for LAST_MS, also once its next level has begun. The lobby
//   has the match open until the host ends it or opens the next, so the last
//   report followed by the next level's first closes it as it ended. After
//   the next level's first it would open the old match again, so nothing of
//   a level goes while an older level's last is kept.
// - from another game only until its own next level begins: by then the
//   host has opened the next match, and when that's the same level again the
//   lobby would take the old report for the new match.
// It's only for the game still in the seat it had: a game that left, or sits
// in another room by now, drops it. A player who leaves (end "left") gives
// up the seat right after, so that last report can't be kept.
//
// The page says who the player is: Module.jkLadderKey is their browser key
// (from the lobby's /ladder/key), and Module.jkLadderAnon is true when they
// asked not to be recorded by name, in which case the key isn't sent either.
// When the lobby has saved a match this game reported, it says "matched
// <id>", and the page's Module.jkLadderSaved(id), if there, is called with it.
//
// The page also reads two things here for the screenshots it sends to match
// pages (index.html): L.on with L.sentAt, a level the lobby has had a report
// of is being played, and L.ended, how the last level ended.

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
    // A level's last report that couldn't go is kept this long. The lobby
    // closes a match nobody reports to after five minutes.
    var LAST_MS = 5 * 60 * 1000;

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
        timer: null, soon: null,
        last: null,     // an ended level's last report, still to go (L.keep)
        lastTimer: null,
        // How the last level ended, until the next begins: { at, left (its
        // player left, the level went on), told (the lobby had a report of
        // it), host }.
        ended: null
    };

    // Whether a report can go now: the socket is open, this game has a seat in
    // a room, and the lobby keeps a record.
    L.canSend = function() {
        var net = Module.jkNet;
        return !!(net && net.ws && net.state === 1 && net.session && net.seat && net.lobbyCan &&
                  net.lobbyCan.indexOf('match') >= 0);
    };

    // A report of a level: the one being played, or a kept one (of), whose
    // time in the level stopped when it ended.
    L.report = function(end, of) {
        var s = of || L;
        var t = now();
        var kills = s.waiting.slice(0, MAX_EVENTS);
        var r = {
            v: 1, lv: s.lv, map: s.map, title: s.title, t: Math.max(0, Math.round((of ? of.endAt : t) - s.since)),
            build: s.build,
            ev: kills.map(function(k) {
                return [k.n, Math.min(MAX_AGO_MS, Math.max(0, Math.round(t - k.at))), k.victim, k.killer, k.kind];
            })
        };
        if (Module.jkLadderAnon)
            r.anon = 1;
        else if (typeof Module.jkLadderKey === 'string' && Module.jkLadderKey)
            r.pk = Module.jkLadderKey;
        if (s.idle)
            r.idle = 1;
        if (s.host) {
            r.rules = s.rules;
            if (s.sb) {
                r.sb = s.sb;
                r.teams = s.teams;
            }
        }
        if (end)
            r.end = end;
        return { line: 'match ' + b64(JSON.stringify(r)), kills: kills.length };
    };

    // Sends a level's waiting kills, and its end with the last of them if
    // this is it. False if a send failed or there's more than goes at once.
    function sendOf(s, end) {
        // What's too old to place is dropped.
        var t = now();
        while (s.waiting.length && t - s.waiting[0].at > MAX_AGO_MS)
            s.waiting.shift();
        for (var i = 0; i < MAX_AT_ONCE; i++) {
            var last = s.waiting.length <= MAX_EVENTS;
            var r = L.report(last ? end : '', s === L ? null : s);
            try {
                Module.jkNet.ws.send(r.line);
            } catch (e) {
                return false;
            }
            s.waiting.splice(0, r.kills);
            s.sentAt = t;
            if (last)
                return true;
        }
        return !end;
    }

    // Keeps the level's last report, which couldn't go, for when it can
    // (L.flush). Only of a level the lobby has had a report of, so a lobby
    // without a record or a game without a seat is never kept anything for.
    L.keep = function() {
        var net = Module.jkNet;
        L.drop();
        if (!L.sentAt || !net || !net.session || !net.seat)
            return;
        var t = now();
        L.last = { room: net.seat.room, seat: net.seat.id, until: t + LAST_MS, endAt: t,
                   lv: L.lv, map: L.map, title: L.title, build: L.build, host: L.host, idle: L.idle, rules: L.rules,
                   since: L.since, waiting: L.waiting, sb: L.sb, teams: L.teams, sentAt: L.sentAt };
        L.lastTimer = setInterval(L.flush, RETRY_MS);
    };

    L.drop = function() {
        L.last = null;
        clearInterval(L.lastTimer);
        L.lastTimer = null;
    };

    // Sends the kept last report if it can go. True when nothing is kept any
    // more: it went, or it's too old, or its game left or has another seat.
    L.flush = function() {
        var k = L.last;
        if (!k)
            return true;
        var net = Module.jkNet;
        if (now() > k.until || !net || !net.session ||
            (net.seat && (net.seat.room !== k.room || net.seat.id !== k.seat))) {
            L.drop();
            return true;
        }
        if (!L.canSend() || !sendOf(k, 'level'))
            return false;
        L.drop();
        return true;
    };

    // Sends what's waiting, and the level's end if this is it. False if the
    // lobby can't be reached, or keeps no record.
    L.send = function(end) {
        if (!L.canSend())
            return false;
        // An older level's last report goes first, or this one waits for it.
        if (!L.flush())
            return false;
        return sendOf(L, end);
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
        L.ended = null;
        // Its next level: the host has opened the next match by now, so a
        // last report kept from another game than the host's has no match
        // to go to. A host's still closes its own (L.send).
        if (L.last && !L.last.host)
            L.drop();
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
        L.ended = { at: now(), left: !!left, told: L.sentAt > 0, host: L.host };
        // A level's end that can't go now is kept for when it can. Not a
        // player's leaving: the seat is given up right after.
        if (!L.send(left ? 'left' : 'level') && !left)
            L.keep();
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
