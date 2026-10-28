// Tests for packaging/wasm/netstats.js: the network measurements, the hit
// ledger and ?netsim. In Node: node packaging/wasm/test/netstats-test.mjs
//
// Each game loads netstats.js as openjkdf2.js does, with its own Module. The
// test runs one clock for every game by hand, and passes packets between the
// games the way stdComm_WSRelay.c's JS does (checked at the end).

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const read = path => readFileSync(new URL('../../../' + path, import.meta.url), 'utf8');
const source = read('packaging/wasm/netstats.js');
const relayC = read('src/Platform/Networking/WSRelay/stdComm_WSRelay.c');
const enums = read('src/types_enums.h');

const SABER = 1, SHOT = 2, FORCE = 4;
const HIT = 1, BLOCK = 2, DEAD = 3;

// One clock for the whole test. Timers run in order as it moves on.
function makeWorld() {
    const timers = [];
    let now = 1000, seq = 0;
    return {
        now: () => now,
        later(fn, ms) {
            timers.push({ at: now + Math.max(0, ms || 0), fn, seq: seq++ });
        },
        run(ms) {
            const end = now + ms;
            for (;;) {
                let next = null;
                for (const t of timers)
                    if (!next || t.at < next.at || (t.at === next.at && t.seq < next.seq)) next = t;
                if (!next || next.at > end) break;
                timers.splice(timers.indexOf(next), 1);
                now = Math.max(now, next.at);
                next.fn();
            }
            now = end;
        },
    };
}

// Random numbers that come out the same every run.
function seeded(seed) {
    let x = seed >>> 0;
    return () => {
        x = (x * 1664525 + 1013904223) >>> 0;
        return x / 4294967296;
    };
}

// A game, as openjkdf2.js loads netstats.js, in a window with ?netstats
// unless measure is false. Its clock reads skew ms off the world's, as
// another computer's would.
function makeGame(world, { id, isHost = false, skew = 0, netsim, measure = true, random = seeded(id) }) {
    const Module = netsim === undefined ? {} : { jkNetSim: netsim };
    if (measure) Module.jkNetMeasure = true;
    new Function('Module', 'setInterval', source)(Module, () => 0);
    const ns = Module.jkNetStats;
    ns.lib.clock.now = () => world.now() + skew;
    ns.lib.clock.later = (fn, ms) => world.later(fn, ms);
    ns.lib.clock.random = random;
    return { id, isHost, Module, ns, queue: [], notes: [] };
}

// The link between two games: oneWay ms each way, on the game's channel and
// on 'jk-ns', whose messages dropProbe can lose. A game without netstats.js
// (old) only queues what it gets.
function link(world, a, b, { oneWay = 20, dropProbe = () => false } = {}) {
    for (const [self, other] of [[a, b], [b, a]]) {
        const deliver = bytes => {
            const data = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
            world.later(() => {
                if (!other.ns) return other.queue.push(data);
                // net.onPeerPacket, then net.takePeerPacket.
                other.ns.inbound(self.id, data, 'direct', d => {
                    if (!other.ns.take(self.id, d, 'direct')) other.queue.push(d);
                });
            }, oneWay);
            return 1;
        };
        if (!self.ns) {
            self.send = bytes => deliver(bytes);
            continue;
        }
        self.Module.jkNet = {
            peers: { [other.id]: { open: true } },
            sendPacket: (bytes, id) => self.ns.outbound(id, bytes, deliver),
            nsOpen: () => !!other.ns,
            nsSend: (id, buf) => {
                const copy = buf.slice(0);
                if (!dropProbe(copy)) world.later(() => other.ns.onProbe(self.id, copy), oneWay);
                return 1;
            },
            note: text => self.notes.push(text),
        };
        self.send = bytes => self.Module.jkNet.sendPacket(bytes, other.id);
    }
}

// Ticks every game as netstats.js's own interval would.
function ticking(world, games) {
    const step = () => {
        games.forEach(g => g.ns && g.ns.lib.tick());
        world.later(step, 250);
    };
    world.later(step, 250);
}

// A player's position update from one game to another (DSS_THINGPOS, 64
// bytes after the 12-byte header).
function position(from, to) {
    const buf = new ArrayBuffer(76);
    const view = new DataView(buf);
    view.setUint32(0, from.id, true);
    view.setUint32(4, to.id, true);
    view.setUint16(8, 1, true);
    view.setUint16(10, 0, true);
    return new Uint8Array(buf);
}

// Two games in a match, each having heard from the other.
function match({ oneWay = 20, hostSim, joinSim, skew = 0, dropProbe, hostMeasures = true } = {}) {
    const world = makeWorld();
    const host = makeGame(world, { id: 1, isHost: true, netsim: hostSim, measure: hostMeasures });
    const join = makeGame(world, { id: 2, skew, netsim: joinSim });
    link(world, host, join, { oneWay, dropProbe });
    for (const g of [host, join]) {
        g.ns.start(g.isHost);
        g.ns.live(1, g.id, g.isHost);
        g.ns.name(g === host ? 2 : 1, '');
    }
    host.send(position(host, join));
    join.send(position(join, host));
    ticking(world, [host, join]);
    world.run(100);
    return { world, host, join };
}

const rowOf = (game, id) => game.ns.report().rows.find(r => r.id === id);
const near = (actual, want, within, what) =>
    assert.ok(actual !== null && Math.abs(actual - want) <= within, `${what}: ${actual}, wanted ${want} ± ${within}`);
const hitsOf = (game, id, kind) => (rowOf(game, id).hits.find(h => h.kind === kind) || {}).c;
const takenOf = (game, id, kind) => (rowOf(game, id).taken.find(h => h.kind === kind) || {}).c;

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test('?netsim as the page passes it', () => {
    const { parseSim } = makeGame(makeWorld(), { id: 1 }).ns.lib;
    assert.deepEqual(parseSim('rtt:150,jitter:20,loss:2'), { rtt: 150, jitter: 20, loss: 2 });
    assert.deepEqual(parseSim('150'), { rtt: 150, jitter: 0, loss: 0 });
    assert.deepEqual(parseSim('ping=100;jitter=5'), { rtt: 100, jitter: 5, loss: 0 });
    assert.deepEqual(parseSim('loss:80,rtt:9000'), { rtt: 2000, jitter: 0, loss: 50 });
    assert.equal(parseSim(''), null);
    assert.equal(parseSim(null), null);
    assert.equal(parseSim(undefined), null);
    assert.equal(parseSim('rtt:-5,loss:x'), null);
    const world = makeWorld();
    assert.deepEqual(makeGame(world, { id: 1, netsim: 'rtt:80' }).ns.sim(), { rtt: 80, jitter: 0, loss: 0 });
    const game = makeGame(world, { id: 1 });
    assert.equal(game.ns.sim(), null);
    // The panel's buttons change it during a game.
    game.ns.setSim('rtt:150,jitter:20,loss:2');
    assert.deepEqual(game.ns.sim(), { rtt: 150, jitter: 20, loss: 2 });
    assert.match(game.ns.text()[0], /Simulating 150 ms more round trip, 20 ms jitter, 2% loss each way\.$/);
    game.ns.setSim('');
    assert.equal(game.ns.sim(), null);
});

test('frames look like messages engines without netstats.js ignore', () => {
    const { host, join, world } = match();
    let frame = null;
    host.Module.jkNet.sendPacket = bytes => {
        frame = bytes;
        return 1;
    };
    world.run(1500);
    assert.ok(frame, 'the host pinged');
    const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    const dssMax = Number(/DSS_MAX\s*=\s*(\d+)/.exec(enums)[1]);
    assert.ok(view.getUint16(8, true) >= dssMax, 'message id past DSS_MAX (sithMessage_ProcessMessages skips it)');
    assert.equal(view.getUint16(10, true), 0, 'no resend id, so no engine acknowledges or resends it');
    assert.equal(view.getUint32(0, true), 1, 'from the host');
    assert.equal(view.getUint32(4, true), 2, 'to the joiner');
    assert.ok(join.ns.lib.isFrame(frame.buffer.slice(frame.byteOffset)));
    assert.ok(!join.ns.lib.isFrame(position(host, join).buffer));
    assert.ok(join.ns.lib.isPosition(position(host, join).buffer));
});

test('pings measure the round trip on the game\'s channel and on its own', () => {
    const { world, host, join } = match({ oneWay: 20 });
    world.run(10000);
    for (const [g, other] of [[host, join], [join, host]]) {
        const row = rowOf(g, other.id);
        near(row.ping, 40, 0.5, 'ping');
        near(row.netPing, 40, 0.5, 'link ping');
        near(row.jitter, 0, 0.5, 'jitter');
        assert.equal(row.loss.share, 0);
        assert.ok(row.loss.of >= 10, 'pings counted for loss: ' + row.loss.of);
        assert.equal(row.path, 'direct');
    }
    // Pings never reach the engine.
    assert.equal(host.queue.length, 1);
    assert.equal(join.queue.length, 1);
});

test('?netsim in one window adds its round trip to both sides\' pings', () => {
    const { world, host, join } = match({ oneWay: 20, hostSim: 'rtt:100' });
    world.run(10000);
    near(rowOf(host, 2).ping, 140, 1, 'host\'s ping');
    near(rowOf(join, 1).ping, 140, 1, 'joiner\'s ping');
    near(rowOf(join, 1).netPing, 140, 1, 'joiner\'s link ping');
});

test('jitter, and loss on the channel that never resends', () => {
    const lose = seeded(11);
    const { world, join } = match({ oneWay: 20, joinSim: 'jitter:30', dropProbe: () => lose() < 0.25 });
    world.run(30000);
    const row = rowOf(join, 1);
    assert.ok(row.jitter > 3 && row.jitter < 40, 'jitter ' + row.jitter);
    // A lost ping or a lost answer both lose that ping: 1 - 0.75 * 0.75.
    assert.ok(row.loss.share > 0.3 && row.loss.share < 0.6, 'loss ' + row.loss.share);
    assert.ok(rowOf(join, 1).ping >= 40, 'the game\'s channel resends, so it never loses one');
});

test('a game without netstats.js is pinged less once it never answers', () => {
    const world = makeWorld();
    const host = makeGame(world, { id: 1, isHost: true });
    const old = { id: 2, queue: [] };
    link(world, host, old);
    host.ns.start(true);
    host.ns.live(1, 1, true);
    host.ns.name(2, '');
    old.send(position(old, host));
    ticking(world, [host]);
    world.run(30000);
    const pings = old.queue.filter(d => host.ns.lib.isFrame(d)).length;
    assert.ok(pings >= 5 && pings <= 8, 'pings sent to the old game: ' + pings);
    assert.equal(rowOf(host, 2).ping, null);
});

test('pings and receipts only once this game\'s player is in the match, to players in it', () => {
    const world = makeWorld();
    const host = makeGame(world, { id: 1, isHost: true });
    const join = makeGame(world, { id: 2 });
    link(world, host, join);
    host.ns.start(true);
    join.ns.start(false);
    host.ns.live(1, 1, true);
    host.send(position(host, join));
    join.send(position(join, host));
    ticking(world, [host, join]);
    const sent = game => {
        const types = {};
        const was = game.Module.jkNet.sendPacket;
        game.Module.jkNet.sendPacket = (bytes, id) => {
            if (game.ns.lib.isFrame(bytes.buffer)) types[bytes[12]] = (types[bytes[12]] || 0) + 1;
            return was(bytes, id);
        };
        return types;
    };
    const fromHost = sent(host), fromJoin = sent(join);
    // The host's engine hasn't said the joiner is in the match yet.
    world.run(5000);
    assert.deepEqual(fromHost, {}, 'the host pinged a player not in the match');
    host.ns.name(2, 'Kyle');
    world.run(5000);
    assert.ok(fromHost[host.ns.lib.T_PING] > 0);
    // A joiner answers (its pongs never reach an engine), but neither pings
    // nor sends receipts before the host has let it in: the host drops a
    // player who sends it anything before then (sithMessage_ProcessMessages).
    join.ns.name(1, 'Jerec');
    join.ns.hit(1, false, SABER, HIT, 10, false);
    world.run(5000);
    assert.equal(fromJoin[join.ns.lib.T_PING], undefined, 'the joiner pinged before it was in');
    assert.equal(fromJoin[join.ns.lib.T_RECEIPT], undefined, 'or sent a receipt');
    assert.ok(fromJoin[join.ns.lib.T_PONG] > 0);
    join.ns.live(1, 2, false);
    join.ns.hit(1, false, SABER, HIT, 10, false);
    world.run(2000);
    assert.ok(fromJoin[join.ns.lib.T_PING] > 0);
    assert.equal(fromJoin[join.ns.lib.T_RECEIPT], 1);
});

test('a window without ?netstats only answers pings and sends receipts', () => {
    const { world, host, join } = match({ hostMeasures: false });
    let pings = 0;
    const send = host.Module.jkNet.sendPacket, probe = host.Module.jkNet.nsSend;
    host.Module.jkNet.sendPacket = (bytes, id) => {
        if (host.ns.lib.isFrame(bytes.buffer.slice(bytes.byteOffset)) && new DataView(bytes.buffer, bytes.byteOffset).getUint8(12) === host.ns.lib.T_PING) pings++;
        return send(bytes, id);
    };
    host.Module.jkNet.nsSend = (id, buf) => {
        if (new DataView(buf).getUint8(12) === host.ns.lib.T_PING) pings++;
        return probe(id, buf);
    };
    world.run(10000);
    assert.equal(pings, 0);
    assert.equal(rowOf(host, 2).ping, null);
    // The joiner, with ?netstats, still gets its answers and receipts.
    near(rowOf(join, 1).ping, 40, 0.5, 'joiner\'s ping');
    join.ns.hit(1, true, SABER, HIT, 10, false, 4);
    host.ns.hit(2, false, SABER, HIT, 10, false, 4);
    world.run(1000);
    assert.equal(hitsOf(join, 1, 'saber').counted, 1);
});

test('position updates: how many, how even, the late ones and the longest gap', () => {
    const { world, host, join } = match();
    world.run(10000);
    const start = world.now();
    for (let t = 0; t < 5000; t += 50) {
        if (t > 2000 && t < 2300) continue;
        world.later(() => host.send(position(host, join)), t);
    }
    world.run(5000);
    const u = rowOf(join, 1).updates;
    assert.ok(u.rate > 18 && u.rate < 21, 'rate ' + u.rate);
    assert.equal(u.late, 1);
    near(u.longest, 300, 1, 'longest gap');
    assert.ok(world.now() - start === 5000);
});

test('a hit shown here and counted in the victim\'s game agrees', () => {
    const { world, host, join } = match();
    world.run(3000);
    join.ns.hit(1, true, SABER, HIT, 10, false);
    world.run(15);
    host.ns.hit(2, false, SABER, HIT, 10, false);
    world.run(1000);
    const c = hitsOf(join, 1, 'saber');
    assert.equal(c.counted, 1);
    assert.equal(c.missedHit + c.blockedThere + c.notShown, 0);
    assert.deepEqual(takenOf(host, 2, 'saber'), { hit: 1, block: 0, dead: 0, byMessage: 0 });
    assert.match(join.ns.text().join('\n'), /your saber: 1 hit shown: 1 counted \| agree 100% of 1/);
    assert.match(host.ns.text().join('\n'), /their hits on you: saber 1 counted/);
});

test('blocked there, missed there, landed without showing here, already dead there', () => {
    const { world, host, join } = match();
    world.run(3000);
    // Blocked in the victim's game: each game rolls its own block chance.
    join.ns.hit(1, true, SABER, HIT, 10, false);
    host.ns.hit(2, false, SABER, BLOCK, 10, false);
    world.run(500);
    // The victim's game never saw it.
    join.ns.hit(1, true, SABER, HIT, 10, false);
    world.run(3000);
    // It saw one this game didn't.
    host.ns.hit(2, false, SABER, HIT, 10, false);
    world.run(3000);
    // The victim had already died in its own game.
    join.ns.hit(1, true, SABER, HIT, 10, false);
    host.ns.hit(2, false, SABER, DEAD, 10, false);
    world.run(500);
    // A block shown here that the victim's game counted as a hit.
    join.ns.hit(1, true, SABER, BLOCK, 10, false);
    host.ns.hit(2, false, SABER, HIT, 10, false);
    world.run(500);
    const c = hitsOf(join, 1, 'saber');
    assert.equal(c.blockedThere, 1);
    assert.equal(c.missedHit, 1);
    assert.equal(c.notShown, 1);
    assert.equal(c.deadThere, 1);
    assert.equal(c.hitThere, 1);
    assert.equal(c.counted, 0);
    const text = join.ns.text().join('\n');
    assert.match(text, /3 hits shown: 0 counted, 1 blocked there, 1 missed there, 1 already dead there/);
    assert.match(text, /1 block shown: 0 blocked there, 1 counted there/);
    assert.match(text, /1 landed there without showing here/);
    assert.match(text, /agree 0% of 5/);
});

test('swings and shots are matched by their number, however close together', () => {
    const { world, host, join } = match({ oneWay: 60 });
    world.run(3000);
    // Quick slashes 250 ms apart. Swing 7 shows as a block here and missed
    // there; 8 and 9 come out the other way round there; the victim's game
    // saw 10 hit, which never showed here.
    join.ns.hit(1, true, SABER, BLOCK, 40, false, 7);
    world.run(250);
    join.ns.hit(1, true, SABER, HIT, 40, false, 8);
    world.run(60);
    host.ns.hit(2, false, SABER, BLOCK, 40, false, 8);
    world.run(190);
    join.ns.hit(1, true, SABER, BLOCK, 40, false, 9);
    world.run(60);
    host.ns.hit(2, false, SABER, HIT, 40, false, 9);
    world.run(190);
    host.ns.hit(2, false, SABER, HIT, 40, false, 10);
    // Two bolts in flight: the second lands there first.
    join.ns.hit(1, true, SHOT, HIT, 30, false, 0x20005);
    join.ns.hit(1, true, SHOT, HIT, 30, false, 0x20006);
    world.run(40);
    host.ns.hit(2, false, SHOT, HIT, 30, false, 0x20006);
    host.ns.hit(2, false, SHOT, BLOCK, 30, false, 0x20005);
    world.run(4000);
    const c = hitsOf(join, 1, 'saber');
    assert.equal(c.missedBlock, 1, 'swing 7');
    assert.equal(c.blockedThere, 1, 'swing 8');
    assert.equal(c.hitThere, 1, 'swing 9');
    assert.equal(c.notShown, 1, 'swing 10');
    assert.equal(c.counted + c.missedHit + c.blockAgreed + c.blockNotShown, 0);
    // Each one's lag is from its own swing: how much later the victim's game
    // decided it.
    assert.deepEqual(c.lags.map(Math.round), [60, 60]);
    const shots = hitsOf(join, 1, 'shot');
    assert.equal(shots.counted, 1);
    assert.equal(shots.blockedThere, 1);
    assert.deepEqual(shots.lags.map(Math.round), [40, 40]);
});

test('hits without a number are matched by time, apart from numbered ones', () => {
    const { world, host, join } = match();
    world.run(3000);
    // Splash damage has no number. A numbered saber swing in between doesn't
    // take its receipt.
    join.ns.hit(1, true, SABER, HIT, 10, false, 3);
    join.ns.hit(1, true, SABER, HIT, 10, false, 0);
    world.run(20);
    host.ns.hit(2, false, SABER, HIT, 10, false, 0);
    world.run(4000);
    const c = hitsOf(join, 1, 'saber');
    assert.equal(c.counted, 1);
    assert.equal(c.missedHit, 1);
    assert.equal(c.notShown, 0);
    // An older game's receipt has no number either (16 bytes after the
    // header, not 20): it matches a hit shown without one.
    const p = join.ns.lib.players()[1];
    join.ns.hit(1, true, SHOT, HIT, 10, false, 0);
    const buf = new ArrayBuffer(32);
    const view = new DataView(buf);
    view.setUint32(0, 1, true);
    view.setUint32(4, 2, true);
    new Uint8Array(buf).set([0x4A, 0x4E, 0, 0], 8);
    view.setUint8(12, join.ns.lib.T_RECEIPT);
    view.setUint8(13, 1);
    view.setUint8(16, SHOT);
    view.setUint8(17, HIT);
    view.setFloat64(24, 0, true);
    assert.ok(join.ns.take(1, buf, 'direct'));
    assert.equal(hitsOf(join, 1, 'shot').counted, 1);
    assert.ok(p.shown.length === 0 && p.receipts.length === 0);
});

test('a receipt that arrives before the hit shows here still matches', () => {
    const { world, host, join } = match({ oneWay: 5 });
    world.run(3000);
    host.ns.hit(2, false, SHOT, HIT, 30, false);
    world.run(200);
    join.ns.hit(1, true, SHOT, HIT, 30, false);
    world.run(3000);
    const c = hitsOf(join, 1, 'shot');
    assert.equal(c.counted, 1);
    assert.equal(c.notShown + c.missedHit, 0);
});

test('targeted Force powers are counted apart', () => {
    const { world, host, join } = match();
    world.run(3000);
    host.ns.hit(2, false, FORCE, HIT, 5, true);
    host.ns.hit(2, false, FORCE, HIT, 5, true);
    world.run(3000);
    assert.equal(hitsOf(join, 1, 'Force').byMessage, 2);
    assert.equal(hitsOf(join, 1, 'Force').notShown, 0);
    assert.equal(takenOf(host, 2, 'Force').byMessage, 2);
    assert.match(join.ns.text().join('\n'), /your Force: 2 targeted/);
});

test('how much later the victim\'s game decided, though the clocks differ', () => {
    const { world, host, join } = match({ oneWay: 30, skew: 123456 });
    world.run(10000);
    for (let i = 0; i < 5; i++) {
        join.ns.hit(1, true, SABER, HIT, 10, false);
        world.run(30);
        host.ns.hit(2, false, SABER, HIT, 10, false);
        world.run(1000);
    }
    near(rowOf(join, 1).hits[0].lag, 30, 1, 'decided after');
    assert.equal(hitsOf(join, 1, 'saber').counted, 5);
});

test('the simulator keeps each reliable path in order and makes lost packets late', () => {
    const world = makeWorld();
    const game = makeGame(world, { id: 2, netsim: 'rtt:100,jitter:40,loss:10', random: seeded(7) });
    game.ns.start(false);
    const sent = [], got = [];
    for (let i = 0; i < 400; i++) {
        world.later(() => {
            sent.push({ i, t: world.now() });
            game.ns.outbound(1, i, b => got.push({ i: b, t: world.now() }));
        }, i * 5);
    }
    world.run(10000);
    assert.equal(got.length, 400);
    assert.deepEqual(got.map(x => x.i), sent.map(x => x.i), 'in order');
    const waits = got.map((x, k) => x.t - sent[k].t);
    assert.ok(Math.min(...waits) >= 50, 'never sooner than half the round trip: ' + Math.min(...waits));
    // A lost one comes about a round trip later, as resent, and holds up the
    // ones behind it.
    assert.ok(Math.max(...waits) >= 50 + 160, 'a lost one is late: ' + Math.max(...waits));
});

test('the simulator loses messages on the channel that never resends', () => {
    const world = makeWorld();
    const game = makeGame(world, { id: 2, netsim: 'loss:50', random: seeded(3) });
    let seen = 0;
    game.Module.jkNet = { nsSend: () => (seen++, 1) };
    game.ns.start(false);
    game.ns.live(1, 2, false);
    // Pings from the other game: half are lost on their way in.
    for (let i = 0; i < 200; i++) {
        const buf = new ArrayBuffer(28);
        const view = new DataView(buf);
        view.setUint32(0, 1, true);
        view.setUint32(4, 2, true);
        new Uint8Array(buf).set([0x4A, 0x4E, 0, 0], 8);
        view.setUint8(12, game.ns.lib.T_PING);
        view.setUint8(13, 1);
        view.setUint32(16, i, true);
        game.ns.onProbe(1, buf);
    }
    world.run(1000);
    // Answers that got in, less those lost on their way out.
    assert.ok(seen > 25 && seen < 80, 'answers that went out: ' + seen);
});

test('packets still waiting when a game ends never arrive', () => {
    const world = makeWorld();
    const game = makeGame(world, { id: 2, netsim: 'rtt:400' });
    game.ns.start(false);
    let out = 0, inn = 0;
    game.ns.outbound(1, new Uint8Array(8), () => out++);
    game.ns.inbound(1, new ArrayBuffer(8), 'direct', () => inn++);
    world.run(100);
    game.ns.end();
    world.run(1000);
    assert.equal(out + inn, 0);
    game.ns.start(false);
    game.ns.outbound(1, new Uint8Array(8), () => out++);
    world.run(1000);
    assert.equal(out, 1);
});

test('the counts stay up when a game ends, and Reset clears them', () => {
    const { world, host, join } = match();
    world.run(3000);
    join.ns.hit(1, true, SABER, HIT, 10, false);
    host.ns.hit(2, false, SABER, HIT, 10, false);
    world.run(1000);
    join.ns.end();
    assert.equal(hitsOf(join, 1, 'saber').counted, 1);
    assert.match(join.ns.text()[0], /You are player 2 \(not in a match\)\. No simulated lag\./);
    join.ns.reset();
    assert.equal(rowOf(join, 1).hits.length, 0);
    assert.equal(rowOf(join, 1).ping, null);
});

test('?nettest=attack:all\'s steps keep their counts apart, each with its lag', () => {
    const { world, host, join } = match();
    world.run(3000);
    join.ns.nextStep('bryar', '');
    // Two shots shown, one counted there.
    join.ns.hit(1, true, SHOT, HIT, 30, false, 101);
    host.ns.hit(2, false, SHOT, HIT, 30, false, 101);
    join.ns.hit(1, true, SHOT, HIT, 30, false, 102);
    world.run(3000);
    join.ns.nextStep('bryar', 'rtt:100,jitter:20,loss:2');
    assert.deepEqual(join.ns.sim(), { rtt: 100, jitter: 20, loss: 2 });
    assert.equal(rowOf(join, 1).hits.length, 0);
    assert.match(join.ns.text().join('\n'), /\n  bryar  no lag: shot 50% of 2\n  now: bryar, \+100 ms$/);
    join.ns.hit(1, true, SHOT, HIT, 30, false, 103);
    host.ns.hit(2, false, SHOT, HIT, 30, false, 103);
    world.run(3000);
    join.ns.nextStep('rail alt', '');
    world.run(3000);
    join.ns.nextStep('', '');
    assert.equal(join.ns.sim(), null);
    const r = join.ns.report();
    assert.equal(r.step, null);
    assert.deepEqual(r.steps.map(x => [x.name, x.sim && x.sim.rtt]), [['bryar', null], ['bryar', 100], ['rail alt', null]]);
    const c = r.steps[0].rows[0].hits[0].c;
    assert.equal(c.counted, 1);
    assert.equal(c.missedHit, 1);
    assert.equal(c.lags, undefined);
    near(r.steps[1].rows[0].ping, 140, 40, 'the second step\'s ping');
    assert.match(join.ns.text().join('\n'),
                 /\nSteps, how often both games agreed:\n  bryar     no lag: shot 50% of 2 · \+100 ms: shot 100% of 1\n  rail alt  no lag: no hits\n  done$/);
});

test('this game\'s frames a second, from when its frames ended', () => {
    const { world, join } = match();
    world.run(10000);
    assert.equal(join.ns.report().me.fps, null);
    // Window.c's ring of frame end times, on this game's clock: 30 a second
    // for the last 6 s.
    const frames = { size: 2048, count: 0, ends: new Float64Array(2048) };
    for (let at = world.now() - 6000; at <= world.now(); at += 1000 / 30)
        frames.ends[frames.count++ % frames.size] = at;
    join.Module.jkFrames = frames;
    near(join.ns.report().me.fps, 30, 0.5, 'frames a second');
    assert.match(join.ns.text()[0], /^You are player 2\. 30 frames a second\. No simulated lag\.$/);
    frames.count = 0;
    frames.ends[frames.count++] = world.now() - 1000;
    near(join.ns.report().me.fps, 0.2, 0.01, 'one frame in 5 s');
    assert.match(join.ns.text()[0], /^You are player 2\. 0 frames a second\./);
    for (let i = 0; i < 4; i++) frames.ends[frames.count++] = world.now() - 900 + i * 100;
    assert.match(join.ns.text()[0], /^You are player 2\. 1 frame a second\./);
});

test('names and the log line', () => {
    const { world, host, join } = match({ hostSim: 'rtt:50,jitter:10,loss:1' });
    host.ns.name(2, 'Kyle');
    join.ns.name(1, 'Jerec');
    world.run(12000);
    const text = host.ns.text();
    assert.match(text[0], /^You are player 1, the host\. Simulating 50 ms more round trip, 10 ms jitter, 1% loss each way\.$/);
    assert.match(text[2], /^player 2 Kyle: direct, ping \d+ ms \(jitter \d+ ms\), loss [\d.]+% of \d+, link ping \d+ ms$/);
    assert.match(join.ns.text()[2], /^player 1 Jerec \(host\): direct/);
    assert.ok(host.notes.some(n => /^netstats: player 2 direct ping \d+ ms jitter \d+ ms loss/.test(n)), host.notes.join('\n'));
});

test('stdComm_WSRelay.c passes packets to netstats.js as this test does', () => {
    for (const line of [
        "Module.jkNetStats.inbound(id, data, 'direct', function(d) { net.takePeerPacket(id, d); });",
        "if (Module.jkNetStats && Module.jkNetStats.take(id, data, 'direct')) return;",
        "return Module.jkNetStats.outbound(id, bytes, function(b) { return net.sendNow(b, id); });",
        "Module.jkNetStats.inbound(from, ev.data, 'relay', function(d) {",
        "if (net.ws === ws && !Module.jkNetStats.take(from, d, 'relay')) net.queue.push(d);",
        "ns.onmessage = function(ev) { if (net.peers[id] === p) Module.jkNetStats.onProbe(id, ev.data); };",
        "{ negotiated: true, id: 2, ordered: false, maxRetransmits: 0 }",
        "if (Module.jkNetStats) Module.jkNetStats.start(isHost);",
        "if (Module.jkNetStats) Module.jkNetStats.live(live, id, isHost);",
        "if (Module.jkNetStats) Module.jkNetStats.hit(otherId, !!mine, kind, outcome, damage, !!byMessage, attack >>> 0);",
    ])
        assert.ok(relayC.includes(line), 'stdComm_WSRelay.c has: ' + line);
    // The kinds and outcomes the engine sends are the ones netstats.js reads.
    for (const [name, n] of [['SABER', 1], ['SHOT', 2], ['SPLASH', 3], ['FORCE', 4], ['OTHER', 5]])
        assert.match(relayC, new RegExp('#define WSRELAY_HIT_' + name + ' \\(' + n + '\\)'));
    const header = read('src/Platform/Networking/WSRelay/stdComm_WSRelay.h');
    for (const [name, n] of [['HIT', 1], ['BLOCK', 2], ['DEAD', 3]])
        assert.match(header, new RegExp('#define STDCOMM_WSRELAY_' + name + ' \\(' + n + '\\)'));
    assert.match(source, /var KINDS = \['', 'saber', 'shot', 'splash', 'Force', 'other'\];/);
    assert.match(source, /var O_HIT = 1, O_BLOCK = 2, O_DEAD = 3;/);
    // Each swing's number goes out with the message that turns the blade on,
    // after the original's fields, and comes back in from it.
    const jkDSS = read('src/Dss/jkDSS.c');
    assert.ok(jkDSS.includes('NETMSG_PUSHU32(stdComm_WSRelay_GetSwing(pPlayerThing));'));
    assert.ok(jkDSS.includes('stdComm_WSRelay_SetSwing(pThing, NETMSG_IN_LEFT(msg) >= 4 ? NETMSG_POPU32() : 0);'));
    assert.ok(read('src/Cog/jkCog.c').includes('stdComm_WSRelay_NewSwing(v2);'));
    // A bolt a ready saber turned away is a block, by whoever fired it: the
    // saber makes the bolt the blocker's, so the shooter is found first.
    const weaponC = read('src/World/sithWeapon.c');
    const shooter = weaponC.indexOf('SithThing* pShooter = sithThing_GetThingParent(pWeapon);');
    const blocked = weaponC.indexOf('&& sithActor_thing_anim_blocked(pWeapon, pThing, pCollision) )');
    const noted = weaponC.indexOf('stdComm_WSRelay_NoteHitBy(pThing, pShooter, pWeapon, pWeapon->weaponParams.damage, pWeapon->weaponParams.damageType, STDCOMM_WSRELAY_BLOCK);');
    assert.ok(shooter >= 0 && blocked > shooter && noted > blocked);
    assert.ok(read('src/World/sithActor.c').includes('a1->pParent = thing2;'));
    // ?nettest=attack:all's steps.
    assert.ok(read('src/Platform/Networking/WSRelay/stdComm_WSRelayTest.c').includes(
        "Module['jkNetStats'].nextStep(UTF8ToString(pName), UTF8ToString(pSim));"));
});

let failed = 0;
for (const t of tests) {
    try {
        await t.fn();
        console.log('ok   ' + t.name);
    } catch (e) {
        failed++;
        console.log('FAIL ' + t.name + '\n' + (e.stack || e));
    }
}
console.log(failed ? `${failed} of ${tests.length} failed` : `all ${tests.length} passed`);
process.exit(failed ? 1 : 0);
