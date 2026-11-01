// Network measurements for the browser build, and a network simulator to test
// with. Bundled into openjkdf2.js with --pre-js (cmake_modules/plat_wasm.cmake),
// so it always matches the engine it ships with.
//
// stdComm_WSRelay.c hands it each game packet as it arrives and before it
// goes out, and the engine tells it about hits between players
// (stdComm_WSRelay_NoteHit). For each other player it keeps:
//   - ping and jitter on the path game packets take: the reliable data
//     channel (through the host, between joiners) or the lobby relay, from a
//     small ping each second;
//   - ping and loss on a data channel of its own that never resends ('jk-ns'),
//     on a direct link between two games that both have this file;
//   - how steadily their position updates arrive: how many a second, how
//     uneven the gaps between them are, how many gaps are 100 ms or more, and
//     the longest;
//   - a hit ledger. Under the Classic rules another player's game decides
//     whether a hit on them counts; this game only shows its effects. So
//     each game sends the attacker a receipt for each hit it decides on its
//     own player (counted, blocked, or the player was already dead there),
//     and the attacker matches the receipts with the hits it showed: by the
//     attack's number where both games know it (each saber swing and each
//     shot has one), else the next one of that kind in time. A hit with no
//     receipt missed there. A receipt for a hit this game never showed
//     landed there without showing here. Under Hit what you see, the
//     attacker's game claims the hits it showed (stdComm_WSRelayHits.c), and
//     the receipt says whether the victim's game counted the claim or turned
//     it down, why, and how far its player was from where the claim showed
//     them. Blocks the attacker's game decides are final, with no receipt.
//   - how this game draws them, when it draws them from a buffer of their
//     timed updates (stdComm_WSRelayMotion.c, Hit what you see between two
//     games that have it): how far behind where they really are (the buffer
//     the engine reports, and half the ping), how often it ran out of
//     updates and carried them on or held them still, and how often they
//     jumped. Claims that name the moment the target was drawn at are
//     counted apart.
// The page's ?netstats panel (index.html) shows them, and the network log gets
// a line every 10 s in a game.
//
// When this game leaves a match, or its tab closes in one, stdComm_WSRelay.c
// posts the lobby a summary of it (summary()), ?netstats or not, so the
// defaults can be set from real matches: the game, its build, the hit rules
// and reach, how long this game was in the match and at what frame rate, and
// for each other player, by kind, how their link went, how far behind they
// were drawn, and the hits each way, counted or turned down and why, with how
// far back each claim on this game's player reached (the engine says, from
// the moment it named). Counts only: no names, no player numbers. None for a
// match this game was in for under 30 s, or with nobody else.
//
// With ?netsim in the address (Module.jkNetSim), this window's game packets
// wait as if its connection were slower: "rtt:150,jitter:20,loss:2" adds
// 150 ms to each round trip (half on the way in, half on the way out), up to
// 20 ms more each way at random, and loses 2% of packets each way. A packet
// lost on the reliable channel or the relay arrives about a round trip late,
// as a resent one would, and holds up the ones behind it; on 'jk-ns' and the
// fast lane for positions ('jk-pos') it's gone, and the others there each
// wait their own time, so they can come in any order. Set it in one window:
// two with it add up. Hidden tabs run timers late, so keep the window
// showing.
//
// Frames start like a game packet, with idFrom and idTo (u32), then 'J' 'N'
// and two zero bytes, which an engine reads as message 0x4E4A, past DSS_MAX,
// with no resend id: engines without this file ignore them. Then a type
// byte, a version byte and two zero bytes:
//   PING     seq u32, sent f64
//   PONG     seq u32, sent f64 (the ping's), got f64 (the other game's clock)
//   RECEIPT  kind u8, outcome u8, flags u8, why u8 (a turned down claim's
//            reason), damage f32, at f64, attack u32, distance f32 (from where
//            a claim showed the player, -1 if not checked)
// Only a window with ?netstats in its address (Module.jkNetMeasure) pings,
// so in other games this file only answers pings and sends receipts. It pings
// the players its engine says are in the match (name), once its own player is
// in it too (live): a host drops a player who sends it anything before
// they've joined (sithMessage_ProcessMessages). A ping is answered at once,
// on the path it came by; only a game with this file pings, and it takes the
// answer out before its engine sees it. Old games never answer, so after five
// unanswered pings they're only pinged every 15 s.
//
// The engine also asks for another game's clock (sync), under Hit what you
// see, where its shots start as far along as they really are
// (stdComm_WSRelayMotion.c): this file then pings that game, ?netstats or
// not, every second for 10 s and every 5 s after, and gives the engine its
// clock less this one's (clockOffset), from the ping with the shortest round trip
// of the last 16, the way NTP picks.

(function() {
    var MAGIC = [0x4A, 0x4E, 0x00, 0x00]; // 'J' 'N', then a resend id of 0
    var VERSION = 1;
    var HEADER = 16;
    var T_PING = 1, T_PONG = 2, T_RECEIPT = 3;

    var TICK_MS = 250;
    var PING_MS = 1000;         // on the game's path
    var PROBE_MS = 500;         // on 'jk-ns'
    var KEEP_RTTS = 20;         // ping times kept for each path
    var KEEP_PINGS = 40;        // pings counted for loss
    var ANSWER_MS = 2000;       // a ping not answered by then is lost
    var WINDOW_MS = 10000;      // position updates looked at
    var LATE_MS = 100;          // a gap at least this long counts as late
    var QUIET_PINGS = 5;
    var QUIET_PING_MS = 15000;
    var GONE_MS = 10000;        // not heard from for this long: not pinged
    var SYNC_FIRST_MS = 10000;  // a clock the engine asks for: pinged every
    var SYNC_FIRST_PING_MS = 1000;  // second for this long,
    var SYNC_PING_MS = 5000;        // then this often
    var LOG_MS = 10000;
    var ROUTE_MS = 10000;       // how often to look up each link's route

    // Kinds and outcomes, as stdComm_WSRelay_NoteHit sends them.
    var KINDS = ['', 'saber', 'shot', 'splash', 'Force', 'other'];
    var O_HIT = 1, O_BLOCK = 2, O_DEAD = 3, O_TURNED_DOWN = 4;
    var F_BY_MESSAGE = 1;       // decided by a message from the attacker (targeted Force)
    var F_BY_CLAIM = 2;         // a claim from the attacker's game, counted or turned down
    var F_DECIDED = 4;          // this game's block, which is final
    var F_MOMENT = 8;           // a claim that named the moment the target was drawn at
    var F_PAST_REACH = 16;      // ...further back than the reach, so checked without it
    // Why a claim was turned down (stdComm_WSRelayHitCheck.h's WSRELAYHIT_*).
    var WHY = ['', 'dead', 'not there', 'too late', 'unknown attack', 'out of reach', 'already counted', 'bad numbers',
               'not playing by claims'];
    var WHY_REPEAT = 6;
    var KEEP_DISTANCES = 200;
    var KEEP_BUFFERS = 60;      // seconds of each player's buffer kept
    var SUMMARY_MIN_MS = 30000; // in a match for less: no summary
    var SUMMARY_EVERY_MS = 5000; // the frame rate and each link, for it
    // Browsers send at most 64 KB with sendBeacon (and fetch's keepalive).
    var SUMMARY_MAX_BYTES = 60000;
    // How far back claims reached, for the summary: how many reached at least
    // each of these, the reaches a host can pick and a few between.
    var REACHES = [150, 200, 250, 300, 400, 500];

    var clock = {
        now: function() { return performance.now(); },
        later: function(fn, ms) { return setTimeout(fn, ms); },
        every: function(fn, ms) { return setInterval(fn, ms); },
        random: function() { return Math.random(); }
    };

    // Values counted in steps of `step`, up to `top` and one count for
    // everything past it: a whole match's middle and tail, without keeping
    // every value.
    function Hist(step, top) {
        this.step = step;
        this.counts = [];
        for (var i = 0; i <= Math.ceil(top / step); i++) this.counts.push(0);
        this.n = 0;
        this.most = null;
    }
    Hist.prototype.add = function(v) {
        if (typeof v !== 'number' || !isFinite(v) || v < 0) return;
        this.counts[Math.min(this.counts.length - 1, Math.floor(v / this.step))]++;
        this.n++;
        if (this.most === null || v > this.most) this.most = v;
    };
    // The value q of the way up (0.5, the middle), as the middle of its step
    // (past the top, the most), or null with none.
    Hist.prototype.at = function(q) {
        if (!this.n) return null;
        var want = Math.max(1, Math.ceil(q * this.n)), sum = 0, last = this.counts.length - 1;
        for (var i = 0; i < last; i++) {
            sum += this.counts[i];
            if (sum >= want) return Math.min((i + 0.5) * this.step, this.most);
        }
        return this.most;
    };
    // How many were v or more, for v a whole number of steps.
    Hist.prototype.from = function(v) {
        var sum = 0;
        for (var i = Math.floor(v / this.step); i < this.counts.length; i++) sum += this.counts[i];
        return sum;
    };

    // ---- The simulator.

    // "rtt:150,jitter:20,loss:2", or just a number for rtt. Null if it adds
    // nothing.
    function parseSim(text) {
        if (text === undefined || text === null || text === false) return null;
        var sim = { rtt: 0, jitter: 0, loss: 0 };
        String(text).split(/[,;&]/).forEach(function(part) {
            var kv = part.split(/[:=]/);
            if (kv.length === 1) {
                if (/^\s*\d+(\.\d+)?\s*$/.test(kv[0])) sim.rtt = Math.min(Number(kv[0]), 2000);
                return;
            }
            var key = kv[0].trim().toLowerCase(), v = Number(kv[1]);
            if (!isFinite(v) || v < 0) return;
            if (key === 'rtt' || key === 'ping' || key === 'lag') sim.rtt = Math.min(v, 2000);
            else if (key === 'jitter') sim.jitter = Math.min(v, 1000);
            else if (key === 'loss') sim.loss = Math.min(v, 50);
        });
        return sim.rtt || sim.jitter || sim.loss ? sim : null;
    }

    var sim = parseSim(Module['jkNetSim']);

    // From now on, as ?netsim=text would have from the start.
    function setSim(text) {
        sim = parseSim(text);
    }

    // How long one packet waits on its way in or out, or -1 if it's lost on
    // a channel that doesn't resend.
    function simDelay(reliable) {
        var d = sim.rtt / 2 + clock.random() * sim.jitter;
        if (sim.loss && clock.random() * 100 < sim.loss) {
            if (!reliable) return -1;
            d += Math.max(sim.rtt, 50) + 60;
        }
        return d;
    }

    // Packets on one reliable path, in order: each waits its own time, but
    // never comes out before the one ahead of it.
    function Line() {
        this.items = [];
        this.timer = null;
        this.last = 0;
    }
    Line.prototype.push = function(at, fn) {
        if (at < this.last) at = this.last;
        this.last = at;
        this.items.push({ at: at, fn: fn });
        this.arm();
    };
    Line.prototype.arm = function() {
        if (this.timer !== null || !this.items.length) return;
        var self = this;
        this.timer = clock.later(function() {
            self.timer = null;
            var t = clock.now();
            while (self.items.length && self.items[0].at <= t + 0.5)
                self.items.shift().fn();
            self.arm();
        }, Math.max(0, this.items[0].at - clock.now()));
    };

    var lines = {};
    // Bumped when a game starts or ends, so packets still waiting from the
    // last one are dropped.
    var gen = 0;

    function line(key) {
        return lines[key] || (lines[key] = new Line());
    }

    // One packet on a channel that never resends: it waits its own time, or
    // is lost.
    function unordered(fn) {
        var d = simDelay(false), g = gen;
        if (d >= 0) clock.later(function() { if (g === gen) fn(); }, d);
    }

    // A game packet from player `from` over 'direct' (the data channel) or
    // 'relay'. deliver(data) runs when it's due. fast: it came on the fast
    // lane.
    function inbound(from, data, via, deliver, fast) {
        if (!sim) {
            deliver(data);
            return;
        }
        if (fast) return unordered(function() { deliver(data); });
        var g = gen;
        line('in ' + via + ' ' + from).push(clock.now() + simDelay(true), function() {
            if (g === gen) deliver(data);
        });
    }

    // A game packet for route `to` (the player whose link it goes over).
    // send(bytes) sends it when it's due. fast: it goes on the fast lane.
    function outbound(to, bytes, send, fast) {
        if (!sim) return send(bytes);
        if (fast) {
            unordered(function() { send(bytes); });
            return 1;
        }
        var g = gen;
        line('out ' + to).push(clock.now() + simDelay(true), function() {
            if (g === gen) send(bytes);
        });
        return 1;
    }

    // ---- Players.

    var me = { live: false, id: 0, isHost: false };
    var players = {};
    // The players the engine says are in the match, by id: their names. And
    // the roll call each was last named in: the engine names the players in
    // the match every 2 s, after its facts(), which start a roll call, so a
    // player who left stops counting toward the summary's time together.
    var names = {};
    var named = {};
    // ?nettest=attack:all's steps (stdComm_WSRelayTest.c): the one under way,
    // {name, sim}, and the ones done, oldest first, with their counts.
    var step = null, steps = [];
    var lastLog = 0, lastRoutes = 0;
    // This match, for its summary: what the engine says about it (facts()),
    // how long this game has been in it, its frame rates, whether ?netsim
    // slowed it, and whether the summary's gone.
    var match = newMatch();

    function newMatch() {
        return { rules: 0, reach: 0, mots: false, build: '', liveMs: 0, at: 0, sampledAt: 0,
                 fps: new Hist(1, 500), sim: false, sent: false, roll: 0 };
    }

    function newPath() {
        // sent: seq -> time, for pings not yet answered; pings: the last
        // KEEP_PINGS, {seq, t, back}.
        return { seq: 0, sent: {}, pings: [], rtts: [], quiet: 0, nextAt: 0 };
    }

    function player(id) {
        return players[id] || (players[id] = {
            id: id, heard: 0, via: '', route: null, positions: [], fast: [],
            game: newPath(), ns: newPath(), offsets: [], syncSince: 0,
            shown: [], receipts: [], tally: {}, taken: {}, motion: newMotion(),
            // For the match summary: the hello version their game and this
            // one agreed on (-1 unknown), how long they were in the match with
            // this one, how often each kind of link was up, every ping on the
            // game's path, and the round trip the browser measured on the link.
            hello: -1, liveMs: 0, links: {}, pings: new Hist(5, 2000), rtts: new Hist(5, 2000)
        });
    }

    function newMotion() {
        return { steps: 0, carried: 0, held: 0, jumps: 0, warps: 0, restarts: 0, buffers: [], allBuffers: new Hist(5, 1000) };
    }

    function tallyOf(p, kind) {
        return p.tally[kind] || (p.tally[kind] = {
            // Hits shown here, by what their game said.
            counted: 0, blockedThere: 0, deadThere: 0, missedHit: 0, turnedDown: 0,
            // Blocks shown here, by what their game said, and the ones this
            // game decided.
            blockAgreed: 0, hitThere: 0, missedBlock: 0, blockDecided: 0,
            // Receipts for hits this game never showed.
            notShown: 0, blockNotShown: 0, deadNotShown: 0,
            // Targeted Force powers: played there, and turned down there,
            // which their game checks once the power says the moment this
            // game drew their player at.
            byMessage: 0, byMessageTurnedDown: 0,
            // Claims their game had counted already.
            repeats: 0,
            // Claims, how many named a moment, and how many of those were
            // further back than the reach, why each turned down one was,
            // and how far their player was from where each showed them.
            claims: 0, moments: 0, pastReach: 0, why: {}, distances: [], turnedDownDistances: [],
            // How much later their game decided than this one showed, in ms.
            lags: []
        });
    }

    function takenOf(p, kind) {
        return p.taken[kind] || (p.taken[kind] = {
            hit: 0, block: 0, dead: 0, byMessage: 0, turnedDown: 0,
            claims: 0, moments: 0, pastReach: 0, why: {}, distances: [], turnedDownDistances: [],
            // How far back each claim's moment was when this game checked it.
            ages: new Hist(5, 2000)
        });
    }

    // A claim's numbers, in tally or taken c, and in taken, how far back its
    // moment was (age, ms).
    function noteClaim(c, outcome, why, distance, flags, age) {
        c.claims++;
        if (flags & F_MOMENT) c.moments++;
        if (flags & F_MOMENT && flags & F_PAST_REACH) c.pastReach++;
        if (c.ages && flags & F_MOMENT) c.ages.add(age);
        if (outcome === O_TURNED_DOWN) {
            var words = WHY[why] || 'other';
            c.why[words] = (c.why[words] || 0) + 1;
        }
        if (distance >= 0) {
            var list = outcome === O_TURNED_DOWN ? c.turnedDownDistances : c.distances;
            list.push(distance);
            if (list.length > KEEP_DISTANCES) list.shift();
        }
    }

    // ---- Frames.

    function frame(type, to, body) {
        var buf = new ArrayBuffer(HEADER + body);
        var view = new DataView(buf);
        view.setUint32(0, me.id, true);
        view.setUint32(4, to, true);
        new Uint8Array(buf).set(MAGIC, 8);
        view.setUint8(12, type);
        view.setUint8(13, VERSION);
        return { buf: buf, view: view };
    }

    function isFrame(data) {
        if (!(data instanceof ArrayBuffer) || data.byteLength < HEADER) return false;
        var b = new Uint8Array(data, 8, 4);
        return b[0] === MAGIC[0] && b[1] === MAGIC[1] && b[2] === MAGIC[2] && b[3] === MAGIC[3];
    }

    // A player's position update: DSS_THINGPOS (1) without a resend id, 52
    // or 64 bytes, or 60 or 72 with its time (stdComm_WSRelay.c's
    // skipRelayedPos).
    function isPosition(data) {
        var n = data.byteLength, body = n - 8 - 4;
        if (body !== 52 && body !== 64 && body !== 60 && body !== 72) return false;
        var view = new DataView(data);
        return view.getUint16(8, true) === 1 && view.getUint16(10, true) === 0;
    }

    // Over the game's path: a joiner sends everything to the host, which
    // passes on what's for someone else (net.route).
    function sendGame(id, buf) {
        var net = Module.jkNet;
        if (!net || !net.sendPacket) return 0;
        return net.sendPacket(new Uint8Array(buf), net.route ? net.route(id) : me.isHost ? id : 1);
    }

    // Over 'jk-ns', on a direct link.
    function sendProbe(id, buf) {
        var net = Module.jkNet;
        if (!net || !net.nsSend) return 0;
        if (!sim) return net.nsSend(id, buf);
        unordered(function() { net.nsSend(id, buf); });
        return 1;
    }

    function ping(p, path, send) {
        var t = clock.now();
        var seq = ++path.seq;
        var f = frame(T_PING, p.id, 12);
        f.view.setUint32(16, seq, true);
        f.view.setFloat64(20, t, true);
        path.sent[seq] = t;
        path.pings.push({ seq: seq, t: t, back: false });
        while (path.pings.length > KEEP_PINGS)
            delete path.sent[path.pings.shift().seq];
        path.quiet++;
        send(f.buf);
    }

    function gotPong(p, path, view) {
        var seq = view.getUint32(16, true), sent = view.getFloat64(20, true), got = view.getFloat64(28, true);
        if (path.sent[seq] !== sent) return;
        delete path.sent[seq];
        var t = clock.now(), rtt = t - sent;
        path.rtts.push(rtt);
        if (path.rtts.length > KEEP_RTTS) path.rtts.shift();
        if (path === p.game) p.pings.add(rtt);
        path.quiet = 0;
        for (var i = path.pings.length - 1; i >= 0; i--) {
            if (path.pings[i].seq === seq) {
                path.pings[i].back = true;
                break;
            }
        }
        // Their clock less ours, the way NTP reckons it.
        p.offsets.push({ rtt: rtt, offset: got - (sent + t) / 2 });
        if (p.offsets.length > 16) p.offsets.shift();
    }

    // From the engine: it needs player otherId's clock (on), or no longer.
    // Each time it starts, only pings from then count.
    function sync(otherId, on) {
        if (!otherId || otherId === me.id) return;
        var p = player(otherId);
        if (on && !p.syncSince) {
            p.syncSince = clock.now() || 1e-3;
            p.offsets = [];
            p.game.nextAt = 0;
        } else if (!on) {
            p.syncSince = 0;
        }
    }

    // Player otherId's clock less this one's, in ms, or NaN if unknown: for
    // the engine (stdComm_WSRelay_ClockOffset).
    function clockOffset(otherId) {
        var p = players[otherId], offset = p && p.syncSince ? offsetOf(p) : null;
        return offset === null ? NaN : offset;
    }

    // From the recent ping with the shortest round trip, which waited least.
    function offsetOf(p) {
        var best = null;
        p.offsets.forEach(function(o) { if (!best || o.rtt < best.rtt) best = o; });
        return best ? best.offset : null;
    }

    // path is 'game' or 'ns'; reply sends a frame back the same way.
    function onFrame(p, data, path, reply) {
        if (data.byteLength < HEADER) return;
        var view = new DataView(data);
        if (view.getUint8(13) !== VERSION) return;
        var type = view.getUint8(12);
        if (type === T_PING && data.byteLength >= HEADER + 12) {
            var f = frame(T_PONG, p.id, 20);
            f.view.setUint32(16, view.getUint32(16, true), true);
            f.view.setFloat64(20, view.getFloat64(20, true), true);
            f.view.setFloat64(28, clock.now(), true);
            reply(f.buf);
        } else if (type === T_PONG && data.byteLength >= HEADER + 20) {
            gotPong(p, path === 'ns' ? p.ns : p.game, view);
        } else if (type === T_RECEIPT && data.byteLength >= HEADER + 16) {
            gotReceipt(p, view.getUint8(16), view.getUint8(17), view.getUint8(18), view.getFloat64(24, true),
                       data.byteLength >= HEADER + 20 ? view.getUint32(32, true) : 0, view.getUint8(19),
                       data.byteLength >= HEADER + 24 ? view.getFloat32(36, true) : -1);
        }
    }

    // A game packet from player `from` that's due, before the engine gets
    // it, with fast set if it came on the fast lane. Returns true for the
    // frames this file handles itself.
    function take(from, data, via, fast) {
        if (!from || !(data instanceof ArrayBuffer)) return false;
        var p = player(from);
        p.heard = clock.now();
        p.via = via;
        if (isFrame(data)) {
            onFrame(p, data, 'game', function(buf) { sendGame(from, buf); });
            return true;
        }
        if (isPosition(data)) {
            p.positions.push(p.heard);
            p.fast.push(!!fast);
            while (p.positions.length && p.positions[0] < p.heard - WINDOW_MS) {
                p.positions.shift();
                p.fast.shift();
            }
        }
        return false;
    }

    // A message on 'jk-ns' from player `from`.
    function onProbe(from, data) {
        if (!isFrame(data)) return;
        var handle = function() {
            onFrame(player(from), data, 'ns', function(buf) { sendProbe(from, buf); });
        };
        if (!sim) return handle();
        unordered(handle);
    }

    // ---- The hit ledger.

    // How long a hit shown here and its receipt can be apart: their game sees
    // the attack about half a round trip late, and the receipt takes the
    // other half, with room for a slow shot's flight.
    function windowMs(p) {
        var rtt = median(p.game.rtts);
        return Math.min(4000, 1000 + 2 * (rtt === null ? 300 : rtt));
    }

    // The hit or receipt for the same attack: the one with its number, or
    // without one, the oldest of that kind since then.
    function takeMatch(list, kind, attack, since) {
        for (var i = 0; i < list.length; i++) {
            var x = list[i];
            if (x.kind === kind && x.attack === attack && (attack || x.t >= since))
                return list.splice(i, 1)[0];
        }
        return null;
    }

    function settle(p, kind, shown, receipt) {
        var c = tallyOf(p, kind);
        if (receipt.outcome === O_DEAD) c.deadThere++;
        else if (receipt.outcome === O_TURNED_DOWN) c.turnedDown++;
        else if (shown.outcome === O_BLOCK) c[receipt.outcome === O_BLOCK ? 'blockAgreed' : 'hitThere']++;
        else c[receipt.outcome === O_BLOCK ? 'blockedThere' : 'counted']++;
        if (receipt.flags & F_BY_CLAIM) noteClaim(c, receipt.outcome, receipt.why, receipt.distance, receipt.flags);
        var offset = offsetOf(p);
        if (offset !== null) {
            c.lags.push(receipt.at - offset - shown.t);
            if (c.lags.length > 200) c.lags.shift();
        }
    }

    function shownHere(p, kind, outcome, attack) {
        var t = clock.now();
        var shown = { kind: kind, outcome: outcome, attack: attack, t: t };
        var receipt = takeMatch(p.receipts, kind, attack, t - windowMs(p));
        if (receipt) settle(p, kind, shown, receipt);
        else p.shown.push(shown);
    }

    function gotReceipt(p, kind, outcome, flags, at, attack, why, distance) {
        if (!KINDS[kind] || outcome < O_HIT || outcome > O_TURNED_DOWN) return;
        if (flags & F_BY_MESSAGE) {
            var c = tallyOf(p, kind);
            if (outcome === O_TURNED_DOWN) c.byMessageTurnedDown++;
            else c.byMessage++;
            if (flags & F_BY_CLAIM) noteClaim(c, outcome, why, distance === undefined ? -1 : distance, flags);
            return;
        }
        // Their game had counted that attack already, by its own rules, and
        // that receipt took the hit shown here.
        if (outcome === O_TURNED_DOWN && why === WHY_REPEAT) {
            tallyOf(p, kind).repeats++;
            return;
        }
        var t = clock.now();
        var receipt = { kind: kind, outcome: outcome, attack: attack, at: at, t: t, flags: flags,
                        why: why || 0, distance: distance === undefined ? -1 : distance };
        var shown = takeMatch(p.shown, kind, attack, t - windowMs(p));
        if (shown) settle(p, kind, shown, receipt);
        else p.receipts.push(receipt);
    }

    // Hits nothing came back for, and receipts for hits never shown here.
    function expire(t) {
        for (var id in players) {
            var p = players[id], w = windowMs(p), c;
            while (p.shown.length && p.shown[0].t < t - w) {
                var s = p.shown.shift();
                tallyOf(p, s.kind)[s.outcome === O_BLOCK ? 'missedBlock' : 'missedHit']++;
            }
            while (p.receipts.length && p.receipts[0].t < t - w) {
                var r = p.receipts.shift();
                c = tallyOf(p, r.kind);
                if (r.outcome === O_HIT) c.notShown++;
                else if (r.outcome === O_BLOCK) c.blockNotShown++;
                else if (r.outcome === O_TURNED_DOWN) c.turnedDown++;
                else c.deadNotShown++;
                if (r.flags & F_BY_CLAIM) noteClaim(c, r.outcome, r.why, r.distance, r.flags);
            }
        }
    }

    function sendReceipt(p, kind, outcome, damage, flags, attack, why, distance) {
        var f = frame(T_RECEIPT, p.id, 24);
        f.view.setUint8(16, kind);
        f.view.setUint8(17, outcome);
        f.view.setUint8(18, flags);
        f.view.setUint8(19, why);
        f.view.setFloat32(20, damage, true);
        f.view.setFloat64(24, clock.now(), true);
        f.view.setUint32(32, attack, true);
        f.view.setFloat32(36, distance, true);
        sendGame(p.id, f.buf);
    }

    // From the engine: a hit between this game's player and player otherId.
    // mine: this player's hit, shown here; else a hit on this player, which
    // this game decided. flags: F_BY_MESSAGE, decided by a message from the
    // attacker's game rather than here (a targeted Force power), or passed on
    // from another game, and with F_BY_CLAIM, one this game checked, which
    // played (O_HIT) or was turned down; F_BY_CLAIM, the attacker's game
    // claimed it, and this game counted it or turned it down (O_TURNED_DOWN,
    // or O_DEAD) for reason why, with its player distance JKU from where the
    // claim showed them (-1 if not checked); F_MOMENT, with F_BY_CLAIM: the
    // claim named the moment the attacker's game drew this player at, and
    // F_PAST_REACH, one further back than the reach, which was checked as a
    // claim without one; F_DECIDED, this game's block on the other player,
    // which is final.
    // attack: the saber swing's or the shot's number, the same in both
    // games, or 0. age: for a claim on this game's player that named a
    // moment, how far back that was when this game checked it (ms).
    function hit(otherId, mine, kind, outcome, damage, flags, attack, why, distance, age) {
        if (!me.live || !otherId || otherId === me.id || !KINDS[kind]) return;
        var p = player(otherId);
        flags = flags | 0;
        attack = attack >>> 0;
        why = why | 0;
        if (distance === undefined || distance === null || !(distance >= 0)) distance = -1;
        if (mine) {
            if (flags & F_DECIDED) tallyOf(p, kind).blockDecided++;
            else if (!(flags & F_BY_MESSAGE)) shownHere(p, kind, outcome, attack);
            return;
        }
        var c = takenOf(p, kind);
        if (outcome === O_TURNED_DOWN) c.turnedDown++;
        else if (flags & F_BY_MESSAGE) c.byMessage++;
        else c[outcome === O_BLOCK ? 'block' : outcome === O_DEAD ? 'dead' : 'hit']++;
        if (flags & F_BY_CLAIM) noteClaim(c, outcome, why, distance, flags, age);
        sendReceipt(p, kind, outcome, damage, flags & (F_BY_MESSAGE|F_BY_CLAIM|F_MOMENT|F_PAST_REACH), attack, why, distance);
    }

    // From the engine, each second: how it drew player otherId from their
    // buffer (stdComm_WSRelay_NoteMotion).
    function motion(otherId, m) {
        if (!me.live || !otherId || otherId === me.id || !m) return;
        var c = player(otherId).motion;
        c.steps += m.steps | 0;
        c.carried += m.carried | 0;
        c.held += m.held | 0;
        c.jumps += m.jumps | 0;
        c.warps += m.warps | 0;
        c.restarts = m.restarts | 0;
        c.buffers.push(+m.buffer);
        if (c.buffers.length > KEEP_BUFFERS) c.buffers.shift();
        c.allBuffers.add(+m.buffer);
    }

    // How this game draws player p from their buffer, or null if it doesn't:
    // behind is the buffer and half the ping on the game's path.
    function motionOf(p) {
        var c = p.motion;
        if (!c.steps) return null;
        var buffer = median(c.buffers), ping = median(p.game.rtts);
        return { buffer: buffer, behind: buffer !== null && ping !== null ? buffer + ping / 2 : null,
                 carried: c.carried / c.steps, held: c.held / c.steps, jumps: c.jumps, warps: c.warps,
                 restarts: c.restarts, steps: c.steps };
    }

    // ---- Each tick: pings, the route each link takes, and the log line.

    function tick() {
        var t = clock.now();
        expire(t);
        if (!me.live) {
            match.at = 0;
            return;
        }
        keepTime(t);
        var net = Module.jkNet;
        var measure = !!Module['jkNetMeasure'];
        Object.keys(players).forEach(function(key) {
            var p = players[key];
            if ((!measure && !p.syncSince) || p.id === me.id || t - p.heard > GONE_MS || !(p.id in names)) return;
            var g = p.game;
            if (t >= g.nextAt) {
                g.nextAt = t + (measure ? (g.quiet >= QUIET_PINGS ? QUIET_PING_MS : PING_MS)
                                        : t - p.syncSince < SYNC_FIRST_MS ? SYNC_FIRST_PING_MS : SYNC_PING_MS);
                ping(p, g, function(buf) { sendGame(p.id, buf); });
            }
            if (measure && net && net.nsOpen && net.nsOpen(p.id) && t >= p.ns.nextAt) {
                p.ns.nextAt = t + PROBE_MS;
                ping(p, p.ns, function(buf) { sendProbe(p.id, buf); });
            }
        });
        if (t - lastRoutes >= ROUTE_MS) {
            lastRoutes = t;
            lookUpRoutes();
        }
        if (t - lastLog >= LOG_MS) {
            lastLog = t;
            if (net && net.note && Object.keys(players).length) net.note('netstats: ' + logLine(t));
        }
    }

    // Whether each direct link goes straight between the two computers or
    // through a TURN server, from the browser's own stats for it.
    function lookUpRoutes() {
        var net = Module.jkNet;
        if (!net || !net.peers) return;
        Object.keys(net.peers).forEach(function(id) {
            var peer = net.peers[id];
            if (!peer.open || !peer.pc || !peer.pc.getStats) return;
            peer.pc.getStats().then(function(report) {
                var pair = null, cands = {};
                report.forEach(function(s) {
                    if (s.type === 'transport' && s.selectedCandidatePairId) pair = report.get(s.selectedCandidatePairId) || pair;
                    if (s.type === 'local-candidate' || s.type === 'remote-candidate') cands[s.id] = s;
                });
                if (!pair) report.forEach(function(s) {
                    if (!pair && s.type === 'candidate-pair' && s.nominated && s.state === 'succeeded') pair = s;
                });
                if (!pair) return;
                var local = cands[pair.localCandidateId], remote = cands[pair.remoteCandidateId];
                var p = player(Number(id));
                p.route = {
                    local: local ? local.candidateType : '?', remote: remote ? remote.candidateType : '?',
                    rtt: typeof pair.currentRoundTripTime === 'number' ? pair.currentRoundTripTime * 1000 : null
                };
                if (me.live && p.route.rtt !== null) p.rtts.add(p.route.rtt);
            }).catch(function() {});
        });
    }

    // ---- Numbers.

    function median(list) {
        if (!list.length) return null;
        var s = list.slice().sort(function(a, b) { return a - b; });
        var m = s.length >> 1;
        return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
    }

    // The middle, nine in ten and the most of a list, or null.
    function spread(list) {
        if (!list || !list.length) return null;
        var s = list.slice().sort(function(a, b) { return a - b; });
        return { median: median(s), p90: s[Math.min(s.length - 1, Math.ceil(s.length * 0.9) - 1)], most: s[s.length - 1],
                 of: s.length };
    }

    // How much each ping time differs from the one before, on average.
    function jitterOf(list) {
        if (list.length < 2) return null;
        var d = 0;
        for (var i = 1; i < list.length; i++) d += Math.abs(list[i] - list[i - 1]);
        return d / (list.length - 1);
    }

    // The share of pings, among those that have had time to come back, that
    // never did, and how many that is out of.
    function lossOf(path, t) {
        var due = path.pings.filter(function(x) { return x.t < t - ANSWER_MS; });
        if (!due.length) return null;
        var lost = due.filter(function(x) { return !x.back; }).length;
        return { share: lost / due.length, of: due.length };
    }

    // fast, if given: for each update, whether it came on the fast lane.
    function updatesOf(times, t, fast) {
        var list = times.filter(function(x) { return x >= t - WINDOW_MS; });
        if (list.length < 2) return null;
        var onFast = fast ? fast.filter(function(f, i) { return f && times[i] >= t - WINDOW_MS; }).length : 0;
        var sum = 0, longest = 0, late = 0, gaps = [];
        for (var i = 1; i < list.length; i++) {
            var g = list[i] - list[i - 1];
            gaps.push(g);
            sum += g;
            if (g > longest) longest = g;
            if (g >= LATE_MS) late++;
        }
        longest = Math.max(longest, t - list[list.length - 1]);
        var mean = sum / gaps.length, dev = 0;
        gaps.forEach(function(g) { dev += Math.abs(g - mean); });
        var span = Math.max(1, Math.min(WINDOW_MS, t - list[0]));
        return { rate: list.length * 1000 / span, gap: mean, uneven: dev / gaps.length, late: late, longest: longest,
                 fast: onFast / list.length };
    }

    // A host's link to a joiner, a joiner's to the host, and a link between
    // two joiners go straight between the two computers or through a TURN
    // server, or through the lobby relay when they're down. Without a link,
    // two joiners' packets go through the host.
    function pathOf(p) {
        var net = Module.jkNet;
        var peer = net && net.peers ? net.peers[p.id] : null;
        if (peer && peer.open)
            return p.route && (p.route.local === 'relay' || p.route.remote === 'relay') ? 'TURN' : 'direct';
        if (me.isHost || p.id === 1) return 'lobby relay';
        return p.via === 'relay' ? 'lobby relay' : 'via host';
    }

    // ---- The match summary.

    // Each tick in a match: this game's time in it, and each other player's
    // time in it with this one, while the engine's last roll call named them
    // and they're heard from; every SUMMARY_EVERY_MS, the kind of link each
    // player is on, and the frame rate over the last 5 s, once those were all
    // in the match, while the page shows (a hidden tab's says nothing about
    // the computer).
    function keepTime(t) {
        var dt = match.at ? Math.max(0, Math.min(t - match.at, 1000)) : 0;
        match.at = t;
        match.liveMs += dt;
        if (sim) match.sim = true;
        var sample = t - match.sampledAt >= SUMMARY_EVERY_MS;
        if (sample) {
            match.sampledAt = t;
            if (match.liveMs >= 5000 && !(typeof document !== 'undefined' && document.hidden)) match.fps.add(frameRate(t));
        }
        Object.keys(names).forEach(function(key) {
            var p = players[key];
            if (!p || p.id === me.id || t - p.heard > GONE_MS || named[key] !== match.roll) return;
            p.liveMs += dt;
            if (sample) {
                var path = pathOf(p);
                p.links[path] = (p.links[path] || 0) + 1;
            }
        });
    }

    function round(v) { return v === null || v === undefined ? null : Math.round(v); }

    // A count object without its zeros, or null if all are.
    function counts(pairs) {
        var out = {}, any = false;
        for (var k in pairs) {
            if (pairs[k]) {
                out[k] = pairs[k];
                any = true;
            }
        }
        return any ? out : null;
    }

    // This game's hits on another player, of one kind, by what their game
    // said.
    function mineOf(c) {
        var x = counts({
            shown: c.counted + c.blockedThere + c.missedHit + c.deadThere + c.turnedDown,
            counted: c.counted, blocked: c.blockedThere, missed: c.missedHit, dead: c.deadThere,
            turnedDown: c.turnedDown, landed: c.notShown + c.blockNotShown,
            blocks: c.blockAgreed + c.hitThere + c.missedBlock + c.blockDecided,
            blocksCounted: c.hitThere, blocksMissed: c.missedBlock,
            claims: c.claims, moments: c.moments, pastReach: c.pastReach,
            targeted: c.byMessage, targetedTurnedDown: c.byMessageTurnedDown
        });
        if (x && Object.keys(c.why).length) x.why = counts(c.why);
        return x;
    }

    // Another player's hits on this game's player, of one kind, as this game
    // decided them, and how far back the claims among them reached.
    function theirsOf(c) {
        var x = counts({
            counted: c.hit, blocked: c.block, dead: c.dead, turnedDown: c.turnedDown, targeted: c.byMessage,
            claims: c.claims, moments: c.moments, pastReach: c.pastReach
        });
        if (!x) return null;
        if (Object.keys(c.why).length) x.why = counts(c.why);
        var a = c.ages;
        if (a.n) {
            x.ages = { n: a.n, median: round(a.at(0.5)), p90: round(a.at(0.9)), p99: round(a.at(0.99)), most: round(a.most),
                       from: {} };
            REACHES.forEach(function(ms) { x.ages.from[ms] = a.from(ms); });
        }
        return x;
    }

    function otherOf(p) {
        var o = { secs: Math.round(p.liveMs / 1000), hello: p.hello };
        var link = null;
        Object.keys(p.links).forEach(function(k) { if (link === null || p.links[k] > p.links[link]) link = k; });
        o.link = link || pathOf(p);
        var ping = p.pings.at(0.5), rtt = p.rtts.at(0.5);
        if (ping !== null) o.ping = round(ping);
        if (rtt !== null) o.rtt = round(rtt);
        var m = p.motion;
        if (m.steps) {
            var buffer = m.allBuffers.at(0.5);
            o.drawn = { behind: buffer !== null && ping !== null ? round(buffer + ping / 2) : null, buffer: round(buffer),
                        buffer90: round(m.allBuffers.at(0.9)), carried: Math.round(m.carried / m.steps * 1000) / 1000,
                        held: Math.round(m.held / m.steps * 1000) / 1000, jumps: m.jumps, warps: m.warps };
        }
        var mine = {}, theirs = {};
        Object.keys(p.tally).forEach(function(kind) {
            var x = mineOf(p.tally[kind]);
            if (x) mine[KINDS[kind]] = x;
        });
        Object.keys(p.taken).forEach(function(kind) {
            var x = theirsOf(p.taken[kind]);
            if (x) theirs[KINDS[kind]] = x;
        });
        if (Object.keys(mine).length) o.mine = mine;
        if (Object.keys(theirs).length) o.theirs = theirs;
        return o;
    }

    // A summary over SUMMARY_MAX_BYTES (a big match, with every kind of hit
    // between many players) leaves out why claims were turned down, then the
    // players this game spent the least time with, and says how many.
    function fit(s) {
        if (JSON.stringify(s).length <= SUMMARY_MAX_BYTES) return s;
        s.others.forEach(function(o) {
            ['mine', 'theirs'].forEach(function(side) {
                for (var kind in o[side] || {}) delete o[side][kind].why;
            });
        });
        s.others.sort(function(a, b) { return b.secs - a.secs; });
        var dropped = 0;
        while (s.others.length > 1 && JSON.stringify(s).length > SUMMARY_MAX_BYTES - 20) {
            s.others.pop();
            dropped++;
        }
        if (dropped) s.dropped = dropped;
        return s;
    }

    // The summary of the match this game is in, for the page to send once
    // (stdComm_WSRelay.c's net.sendSummary), or null: sent already, under
    // SUMMARY_MIN_MS in the match, or nobody else in it.
    function summary() {
        if (match.sent || match.liveMs < SUMMARY_MIN_MS) return null;
        expire(clock.now());
        var others = Object.keys(players).map(Number).sort(function(a, b) { return a - b; })
            .filter(function(id) { return id !== me.id && players[id].liveMs > 0; })
            .map(function(id) { return otherOf(players[id]); });
        if (!others.length) return null;
        match.sent = true;
        var s = {
            v: 1, game: match.mots ? 'mots' : 'jk', build: match.build, rules: match.rules === 1 ? 'hwys' : 'classic',
            reach: match.reach, host: me.isHost, secs: Math.round(match.liveMs / 1000),
            players: Math.max(Object.keys(names).length, others.length + 1),
            fps: { median: round(match.fps.at(0.5)), low: round(match.fps.at(0.1)) },
            others: others
        };
        if (match.sim) s.sim = true;
        if (Module['jkNetTest']) s.test = true;
        if (Module['jkNetMeasure']) s.netstats = true;
        return fit(s);
    }

    // ---- The report: for the panel, Copy report and the network log.

    // This game's frames a second in a level over the last 5 s, from when
    // Window.c says each one ended (Module.jkFrames), or null. Both games'
    // frame rates matter: each draws, and sends where its player is, once a
    // frame.
    function frameRate(t) {
        var f = Module.jkFrames;
        if (!f || !f.count) return null;
        var n = 0;
        for (var k = 1; k <= Math.min(f.count, f.size) && f.ends[(f.count - k) % f.size] >= t - 5000; k++) n++;
        return n ? n / 5 : null;
    }

    function report() {
        var t = clock.now();
        expire(t);
        var rows = Object.keys(players).map(Number).sort(function(a, b) { return a - b; })
            .filter(function(id) { return id !== me.id; })
            .map(function(id) {
                var p = players[id];
                var hits = Object.keys(p.tally).map(Number).sort().map(function(kind) {
                    var c = p.tally[kind];
                    return { kind: KINDS[kind], c: c, lag: median(c.lags), distance: spread(c.distances),
                             turnedDownDistance: spread(c.turnedDownDistances) };
                });
                var taken = Object.keys(p.taken).map(Number).sort().map(function(kind) {
                    var c = p.taken[kind];
                    return { kind: KINDS[kind], c: c, distance: spread(c.distances),
                             turnedDownDistance: spread(c.turnedDownDistances) };
                });
                return {
                    id: id, name: names[id] || '', path: pathOf(p), route: p.route,
                    heard: p.heard ? t - p.heard : null,
                    ping: median(p.game.rtts), jitter: jitterOf(p.game.rtts),
                    netPing: median(p.ns.rtts), loss: lossOf(p.ns, t),
                    updates: updatesOf(p.positions, t, p.fast),
                    motion: motionOf(p),
                    hits: hits, taken: taken
                };
            });
        return { me: { live: me.live, id: me.id, isHost: me.isHost, fps: frameRate(t) }, sim: sim, rows: rows,
                 step: step ? step.name : null, steps: steps };
    }

    // ?nettest=attack:all moves on to its next step, name ('' after the last):
    // the counts so far are kept under the step that's ending, and start over
    // with this window's link as slow as simText says, as ?netsim would.
    function nextStep(name, simText) {
        if (step) {
            var r = report();
            steps.push({ name: step.name, sim: step.sim, fps: r.me.fps, rows: r.rows.map(function(row) {
                return {
                    id: row.id, name: row.name, ping: row.ping, motion: row.motion,
                    hits: row.hits.map(function(h) {
                        var c = {};
                        for (var k in h.c) if (k !== 'lags' && k !== 'distances' && k !== 'turnedDownDistances') c[k] = h.c[k];
                        return { kind: h.kind, c: c, lag: h.lag, distance: h.distance, turnedDownDistance: h.turnedDownDistance };
                    }),
                    taken: row.taken
                };
            }) });
        }
        setSim(simText);
        reset();
        step = name ? { name: String(name), sim: sim } : null;
    }

    function ms(v) { return v === null || v === undefined ? '-' : Math.round(v) + ' ms'; }
    function pct(v) { return Math.round(v * 1000) / 10 + '%'; }

    function agreement(c) {
        var agree = c.counted + c.blockAgreed + c.blockDecided;
        var all = agree + c.blockedThere + c.deadThere + c.missedHit + c.turnedDown + c.hitThere + c.missedBlock +
                  c.notShown + c.blockNotShown;
        return { agree: agree, of: all };
    }

    function whyWords(why) {
        return Object.keys(why).sort().map(function(k) { return k + ' ' + why[k]; }).join(', ');
    }

    function jku(v) { return (Math.round(v * 100) / 100).toFixed(2); }

    function spreadWords(s) {
        return s ? 'median ' + jku(s.median) + ', 90% ' + jku(s.p90) + ', most ' + jku(s.most) : '';
    }

    function who(row) {
        return 'player ' + row.id + (row.name ? ' ' + row.name : '') + (row.id === 1 ? ' (host)' : '');
    }

    function hitWords(c) {
        var shown = c.counted + c.blockedThere + c.missedHit + c.deadThere + c.turnedDown;
        var parts = [];
        if (shown) {
            var s = shown + ' hit' + (shown === 1 ? '' : 's') + ' shown: ' + c.counted + ' counted';
            if (c.blockedThere) s += ', ' + c.blockedThere + ' blocked there';
            if (c.turnedDown) s += ', ' + c.turnedDown + ' turned down there (' + whyWords(c.why) + ')';
            if (c.missedHit) s += ', ' + c.missedHit + ' missed there';
            if (c.deadThere) s += ', ' + c.deadThere + ' already dead there';
            parts.push(s);
        }
        var blocks = c.blockAgreed + c.hitThere + c.missedBlock + c.blockDecided;
        if (blocks) {
            var b = [];
            if (c.blockDecided) b.push(c.blockDecided + ' decided here');
            if (c.blockAgreed || c.hitThere || c.missedBlock) b.push(c.blockAgreed + ' blocked there');
            if (c.hitThere) b.push(c.hitThere + ' counted there');
            if (c.missedBlock) b.push(c.missedBlock + ' missed there');
            parts.push(blocks + ' block' + (blocks === 1 ? '' : 's') + ' shown: ' + b.join(', '));
        }
        if (c.notShown || c.blockNotShown)
            parts.push((c.notShown + c.blockNotShown) + ' landed there without showing here' +
                       (c.blockNotShown ? ' (' + c.blockNotShown + ' blocked)' : ''));
        if (c.repeats) parts.push(c.repeats + ' claimed after counting there');
        if (c.byMessage || c.byMessageTurnedDown)
            parts.push(c.byMessage + ' targeted' + (c.byMessageTurnedDown ? ', ' + c.byMessageTurnedDown +
                                                     ' turned down there (' + whyWords(c.why) + ')' : ''));
        return parts.join('; ');
    }

    // How many claims named a moment, and how many of those were further back
    // than the reach.
    function momentWords(c) {
        return c.moments + ' at a moment' + (c.pastReach ? ', ' + c.pastReach + ' of them past the reach' : '');
    }

    // How far the player was from where the claims showed them.
    function distanceWords(h) {
        var parts = [];
        if (h.distance) parts.push('counted ' + spreadWords(h.distance));
        if (h.turnedDownDistance) parts.push('turned down ' + spreadWords(h.turnedDownDistance));
        return parts.length ? 'off by ' + parts.join('; ') : '';
    }

    function lagWords(s) { return s ? '+' + s.rtt + ' ms' : 'no lag'; }

    // A step's hits on the other player (or each, with more than one), by kind:
    // how often both games agreed. A targeted Force power agrees when it
    // played there.
    function stepWords(s) {
        var parts = [];
        s.rows.forEach(function(row) {
            row.hits.forEach(function(h) {
                var a = agreement(h.c);
                a.agree += h.c.byMessage;
                a.of += h.c.byMessage + h.c.byMessageTurnedDown;
                if (a.of) parts.push((s.rows.length > 1 ? 'player ' + row.id + ' ' : '') + h.kind + ' ' +
                                     pct(a.agree / a.of) + ' of ' + a.of);
            });
        });
        return parts.join(', ') || 'no hits';
    }

    // ?nettest=attack:all's steps: a line for each weapon, with each lag it
    // was tried at.
    function stepLines() {
        if (!step && !steps.length) return [];
        var out = ['', 'Steps, how often both games agreed:'];
        var order = [], byName = {}, width = 0;
        steps.forEach(function(s) {
            if (!byName[s.name]) {
                byName[s.name] = [];
                order.push(s.name);
                width = Math.max(width, s.name.length);
            }
            byName[s.name].push(s);
        });
        order.forEach(function(name) {
            out.push('  ' + (name + new Array(width + 1).join(' ')).slice(0, width) + '  ' +
                     byName[name].map(function(s) { return lagWords(s.sim) + ': ' + stepWords(s); }).join(' · '));
        });
        out.push(step ? '  now: ' + step.name + ', ' + lagWords(step.sim) : '  done');
        return out;
    }

    function text() {
        var r = report();
        var out = [];
        var simWords = r.sim ? 'Simulating ' + r.sim.rtt + ' ms more round trip, ' + r.sim.jitter + ' ms jitter, ' +
                               r.sim.loss + '% loss each way' : 'No simulated lag';
        var fps = r.me.fps !== null ? Math.round(r.me.fps) : null;
        var fpsWords = fps !== null ? ' ' + fps + (fps === 1 ? ' frame' : ' frames') + ' a second.' : '';
        out.push(r.me.id ? 'You are player ' + r.me.id + (r.me.isHost ? ', the host' : '') +
                           (r.me.live ? '' : ' (not in a match)') + '.' + fpsWords + ' ' + simWords + '.'
                         : 'Not in a game yet. ' + simWords + '.');
        if (!r.rows.length) return out.concat(stepLines());
        out.push('');
        r.rows.forEach(function(row) {
            var u = row.updates;
            out.push(who(row) + ': ' + row.path + ', ping ' + ms(row.ping) + ' (jitter ' + ms(row.jitter) + ')' +
                     (row.loss ? ', loss ' + pct(row.loss.share) + ' of ' + row.loss.of : '') +
                     (row.netPing !== null ? ', link ping ' + ms(row.netPing) : ''));
            out.push('  positions: ' + (u ? u.rate.toFixed(1) + '/s' + (u.fast ? ' (' + pct(u.fast) + ' on the fast lane)' : '') +
                     ', gaps ' + ms(u.gap) + ' (uneven ' + ms(u.uneven) + '), ' + u.late + ' late, longest ' + ms(u.longest)
                     : 'none lately'));
            var m = row.motion;
            if (m) out.push('  drawn from their timed updates: ' + ms(m.behind) + ' behind (buffer ' + ms(m.buffer) + '), carried on ' +
                            pct(m.carried) + ', held still ' + pct(m.held) + ', ' + m.jumps + ' jump' + (m.jumps === 1 ? '' : 's') +
                            ', ' + m.warps + ' warp' + (m.warps === 1 ? '' : 's'));
            row.hits.forEach(function(h) {
                var a = agreement(h.c);
                var words = hitWords(h.c);
                if (!words) return;
                var off = distanceWords(h);
                out.push('  your ' + h.kind + ': ' + words +
                         (a.of ? ' | agree ' + pct(a.agree / a.of) + ' of ' + a.of : '') +
                         (h.c.claims ? ' | ' + h.c.claims + ' claimed' + (h.c.moments ? ' (' + momentWords(h.c) + ')' : '') +
                                       (off ? ', ' + off : '') : '') +
                         (h.lag !== null ? ' | decided ' + ms(h.lag) + ' after you' : ''));
            });
            var taken = row.taken.filter(function(x) { return x.c.hit || x.c.block || x.c.dead || x.c.byMessage || x.c.turnedDown; })
                .map(function(x) {
                    var c = x.c, bits = [];
                    if (c.hit) bits.push(c.hit + ' counted');
                    if (c.turnedDown) bits.push(c.turnedDown + ' turned down (' + whyWords(c.why) + ')');
                    if (c.block) bits.push(c.block + ' blocked');
                    if (c.dead) bits.push(c.dead + ' after you died');
                    if (c.byMessage) bits.push(c.byMessage + ' targeted');
                    var off = distanceWords(x);
                    return x.kind + ' ' + bits.join(', ') + (c.claims ? ' (' + c.claims + ' claimed' +
                                                              (c.moments ? ', ' + momentWords(c) : '') + (off ? ', ' + off : '') + ')' : '');
                });
            if (taken.length) out.push('  their hits on you: ' + taken.join('; '));
        });
        return out.concat(stepLines());
    }

    function logLine(t) {
        var r = report();
        return r.rows.filter(function(row) { return row.heard !== null && row.heard < 60000; }).map(function(row) {
            var u = row.updates;
            var hits = row.hits.map(function(h) {
                var a = agreement(h.c);
                return h.kind + ' ' + h.c.counted + '/' + (h.c.counted + h.c.blockedThere + h.c.missedHit + h.c.deadThere + h.c.turnedDown) +
                       ' counted, agree ' + a.agree + '/' + a.of +
                       (h.c.turnedDown ? ', turned down ' + whyWords(h.c.why) : '') +
                       (h.distance ? ', off by ' + spreadWords(h.distance) : '');
            });
            return 'player ' + row.id + ' ' + row.path + ' ping ' + ms(row.ping) + ' jitter ' + ms(row.jitter) +
                   (row.loss ? ' loss ' + pct(row.loss.share) + '/' + row.loss.of : '') +
                   (u ? ' updates ' + u.rate.toFixed(1) + '/s fast ' + pct(u.fast) + ' late ' + u.late + ' longest ' + ms(u.longest) : ' no updates') +
                   (row.motion ? ' drawn ' + ms(row.motion.behind) + ' behind, buffer ' + ms(row.motion.buffer) + ', carried ' +
                                 pct(row.motion.carried) + ', held ' + pct(row.motion.held) + ', jumps ' + row.motion.jumps : '') +
                   (hits.length ? ' hits ' + hits.join(', ') : '');
        }).join('; ') || 'nobody heard from lately';
    }

    // Clears the counts, but not who's who, and starts the match summary's
    // over with them.
    function reset() {
        for (var id in players) {
            var p = players[id];
            p.positions = [];
            p.fast = [];
            p.shown = [];
            p.receipts = [];
            p.tally = {};
            p.taken = {};
            p.motion = newMotion();
            p.game.rtts = [];
            p.game.pings = [];
            p.game.sent = {};
            p.ns.rtts = [];
            p.ns.pings = [];
            p.ns.sent = {};
            p.liveMs = 0;
            p.links = {};
            p.pings = new Hist(5, 2000);
            p.rtts = new Hist(5, 2000);
        }
        match.liveMs = 0;
        match.fps = new Hist(1, 500);
        match.sim = !!sim;
    }

    // A game starts (hosting or joining): what's still waiting from the last
    // one is dropped, and the counts start over.
    function start(isHost) {
        end();
        players = {};
        names = {};
        named = {};
        me.isHost = !!isHost;
        match = newMatch();
    }

    // A game ends. Its counts stay up until the next one starts.
    function end() {
        gen++;
        lines = {};
        me.live = false;
    }

    Module.jkNetStats = {
        // For tests (packaging/wasm/test/netstats-test.mjs).
        lib: {
            clock: clock, parseSim: parseSim, setSim: setSim,
            isFrame: isFrame, isPosition: isPosition, median: median, jitterOf: jitterOf,
            updatesOf: updatesOf, spread: spread, tick: tick, players: function() { return players; },
            Hist: Hist, REACHES: REACHES, fit: fit,
            O_HIT: O_HIT, O_BLOCK: O_BLOCK, O_DEAD: O_DEAD, O_TURNED_DOWN: O_TURNED_DOWN,
            F_BY_MESSAGE: F_BY_MESSAGE, F_BY_CLAIM: F_BY_CLAIM, F_DECIDED: F_DECIDED, F_MOMENT: F_MOMENT,
            F_PAST_REACH: F_PAST_REACH,
            T_PING: T_PING, T_PONG: T_PONG, T_RECEIPT: T_RECEIPT
        },
        // stdComm_WSRelay.c's transport.
        inbound: inbound,
        outbound: outbound,
        take: take,
        onProbe: onProbe,
        start: start,
        end: end,
        // The engine.
        live: function(on, id, isHost) {
            me.live = !!on;
            me.id = id;
            me.isHost = !!isHost;
        },
        name: function(id, name) {
            if (!id) return;
            names[id] = name;
            named[id] = match.roll;
        },
        // The match's rules (1 Hit what you see) and reach, whether this game
        // runs MotS, and its build; and the hello version this game and
        // player id agreed on, 0 for none.
        facts: function(rules, reach, mots, build) {
            match.roll++;
            match.rules = rules | 0;
            match.reach = reach | 0;
            match.mots = !!mots;
            match.build = String(build || '').slice(0, 16);
        },
        hello: function(id, version) { if (id && id !== me.id) player(id).hello = version | 0; },
        hit: hit,
        motion: motion,
        sync: sync,
        clockOffset: clockOffset,
        // The page.
        report: report,
        text: text,
        reset: reset,
        summary: summary,
        sim: function() { return sim; },
        setSim: setSim,
        // stdComm_WSRelayTest.c's ?nettest=attack:all.
        nextStep: nextStep
    };

    if (typeof setInterval !== 'undefined') clock.every(function() { tick(); }, TICK_MS);
})();
