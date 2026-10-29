// Tests for the browser build's network code: the JavaScript in
// src/Platform/Networking/WSRelay/stdComm_WSRelay.c (its EM_JS functions),
// run here with made-up WebSockets, WebRTC and lobby, for a host and its
// joiners in one room. In Node: node packaging/wasm/test/transport-test.mjs
//
// The made-up WebRTC links two games once their offer and answer have
// crossed, unless the test says those two can't link, and opens each
// negotiated channel once both ends have made it. Messages take the time the
// test gives that pair of games, and an unordered channel that never resends
// can lose them. The lobby passes signals and relays packets as
// jkdf2mp-bb-server's does.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';

const read = path => readFileSync(new URL('../../../' + path, import.meta.url), 'utf8');
const relayC = read('src/Platform/Networking/WSRelay/stdComm_WSRelay.c');
const netstatsJs = read('packaging/wasm/netstats.js');
// The netcode this build speaks, which the engine hands wsrelay_js_connect.
const NETCODE = Number(/#define STDCOMM_WSRELAY_NETCODE \((\d+)\)/.exec(read('src/Platform/Networking/WSRelay/stdComm_WSRelay.h'))[1]);

// The EM_JS functions: name -> { params, body }. Each starts on a line of its
// own and ends with a line that is just "});".
function emJsFunctions(source) {
    const fns = {};
    const re = /^EM_JS\(\s*[^,]+,\s*(\w+)\s*,\s*\(([^)]*)\)\s*,\s*\{\s*$/gm;
    let m;
    while ((m = re.exec(source))) {
        const end = source.indexOf('\n});', re.lastIndex);
        const params = m[2].split(',').map(p => p.trim()).filter(Boolean).map(p => /(\w+)\s*$/.exec(p)[1]);
        fns[m[1]] = { params, body: source.slice(re.lastIndex, end) };
    }
    return fns;
}
const EM_JS = emJsFunctions(relayC);

const flush = () => new Promise(resolve => setImmediate(resolve));

// One clock for every game. Timers run in order as it moves on, and promises
// settle after each one.
function makeWorld() {
    let now = 1000, order = 0, nextId = 0;
    const timers = new Map();
    const world = {
        now: () => now,
        later(fn, ms) {
            const id = ++nextId;
            timers.set(id, { id, at: now + Math.max(0, ms || 0), fn, order: order++ });
            return id;
        },
        every(fn, ms) {
            const id = ++nextId;
            timers.set(id, { id, at: now + ms, fn, order: order++, every: ms });
            return id;
        },
        cancel(id) {
            timers.delete(id);
        },
        async run(ms) {
            const end = now + ms;
            await flush();
            for (;;) {
                let next = null;
                for (const t of timers.values())
                    if (!next || t.at < next.at || (t.at === next.at && t.order < next.order)) next = t;
                if (!next || next.at > end) break;
                now = Math.max(now, next.at);
                if (next.every) {
                    next.at += next.every;
                    next.order = order++;
                } else {
                    timers.delete(next.id);
                }
                next.fn();
                await flush();
            }
            now = end;
            await flush();
        },
        // How long a message takes one way between two games, and whether
        // two games can link directly.
        oneWay: () => 10,
        canLink: () => true,
        lose: () => false,
        games: [],
    };
    world.lobby = makeLobby(world);
    return world;
}

// ---- The lobby: signals and relayed packets, as lobby.js passes them.

function makeLobby(world) {
    const conns = new Map();   // ws -> { room, id }
    const rooms = new Map();   // room -> { host, peers: Map(id -> ws) }
    const lobby = {
        links: true,           // says "lobby links" and passes signals between joiners
        turn: false,           // says "turn" and answers turn with TURN credentials
        turnMs: 100,           // how long getting them takes
        turnTtl: 86400,        // how many seconds they last, 0 for none (only STUN)
        delay: 20,             // how long the lobby takes to pass something on
        signals: [],
        asked: [],             // who asked for TURN credentials: [id, when]
        relayed: 0,
        accept(ws) {
            conns.set(ws, { room: 0, id: 0 });
        },
        opened(ws) {
            lobby.send(ws, 'ice ' + btoa(JSON.stringify(STUN)));
            const can = (lobby.links ? ['links'] : []).concat(lobby.turn ? ['turn'] : []);
            if (can.length) lobby.send(ws, 'lobby ' + can.join(' '));
        },
        send(ws, data) {
            world.later(() => ws.deliver(data), lobby.delay);
        },
        seat(ws, room, id) {
            conns.set(ws, { room, id });
            if (!rooms.has(room)) rooms.set(room, { host: null, peers: new Map() });
            const r = rooms.get(room);
            if (id === 1) r.host = ws;
            else r.peers.set(id, ws);
            lobby.send(ws, `seat ${room} ${id} ${'0'.repeat(32)}`);
            if (id > 1 && r.host) lobby.send(r.host, `peer-joined ${id}`);
        },
        receive(ws, data) {
            const c = conns.get(ws), r = rooms.get(c.room);
            if (typeof data === 'string') {
                const [op, to, blob] = data.split(' ');
                // A socket that dropped takes its seat back.
                if (op === 'rejoin' || op === 'rehost') {
                    const parts = data.split(' ');
                    const room = Number(parts[1]), id = op === 'rehost' ? 1 : Number(parts[2]);
                    const rr = rooms.get(room);
                    conns.set(ws, { room, id });
                    if (id === 1) rr.host = ws;
                    else rr.peers.set(id, ws);
                    lobby.send(ws, op === 'rehost' ? `rehosted ${room}` : `rejoined ${room} ${id}`);
                    return;
                }
                if (op === 'turn' && lobby.turn && r) {
                    lobby.asked.push([c.id, world.now()]);
                    const n = lobby.asked.length;
                    world.later(() => ws.deliver('ice ' + btoa(JSON.stringify(lobby.turnTtl ? STUN.concat([turnServer(n)]) : STUN)) +
                                                 (lobby.turnTtl ? ' ' + lobby.turnTtl : '')), lobby.turnMs);
                }
                if (op !== 'signal' || !r) return;
                const id = Number(to);
                const target = lobby.links ? (id === 1 ? r.host : r.peers.get(id))
                                           : r.host === ws ? r.peers.get(id) : id === 1 ? r.host : null;
                let obj = null;
                try { obj = JSON.parse(atob(blob)); } catch (e) {}
                lobby.signals.push({ from: c.id, to: id, passed: !!target && target !== ws, at: world.now(), obj });
                if (target && target !== ws) lobby.send(target, `signal ${c.id} ${blob}`);
                return;
            }
            if (!r || data.byteLength < 8) return;
            const view = new DataView(data);
            const idTo = view.getUint32(4, true);
            const target = (r.host === ws || (idTo !== 0 && idTo !== 1)) ? r.peers.get(idTo) : r.host;
            if (r.host !== ws) view.setUint32(0, c.id, true);
            if (target && target !== ws) {
                lobby.relayed++;
                lobby.send(target, data);
            }
        },
    };
    return lobby;
}

const STUN = [{ urls: ['stun:stun.example.org:3478'] }];
// The nth credentials the lobby hands out.
const turnServer = n => ({ urls: ['turn:turn.example.org:3478?transport=udp'], username: 'user' + n, credential: 'pass' + n });

function makeWebSocket(world) {
    return class WebSocket {
        constructor(url) {
            this.url = url;
            this.readyState = 0;
            this.binaryType = 'blob';
            world.lobby.accept(this);
            world.later(() => {
                this.readyState = 1;
                if (this.onopen) this.onopen();
                world.lobby.opened(this);
            }, 1);
        }
        send(data) {
            if (this.readyState !== 1) throw new Error('InvalidStateError');
            world.lobby.receive(this, typeof data === 'string' ? data : copy(data));
        }
        close() {
            this.readyState = 3;
        }
        // The lobby goes away.
        drop() {
            if (this.readyState === 3) return;
            this.readyState = 3;
            if (this.onclose) this.onclose({ code: 1006, reason: '' });
        }
        deliver(data) {
            if (this.readyState === 1 && this.onmessage) this.onmessage({ data });
        }
    };
}

function copy(data) {
    if (data instanceof ArrayBuffer) return data.slice(0);
    return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
}

// ---- WebRTC.

function makeRtc(world, game) {
    class DataChannel {
        constructor(pc, label, opts) {
            Object.assign(this, {
                pc, label, id: opts.id, ordered: opts.ordered !== false, resends: opts.maxRetransmits !== 0,
                readyState: 'connecting', bufferedAmount: 0, binaryType: 'blob', sent: 0,
                onopen: null, onclose: null, onmessage: null,
            });
        }
        send(data) {
            if (this.readyState !== 'open') throw new Error('InvalidStateError');
            this.sent++;
            const bytes = copy(data), from = this.pc.game, remote = this.pc.remote;
            const to = remote.game;
            if (!this.resends && world.lose(from.id, to.id, this.label)) return;
            world.later(() => {
                const other = remote.channels[this.id];
                if (other && other.readyState === 'open' && other.onmessage) other.onmessage({ data: bytes });
            }, world.oneWay(from.id, to.id, this.label));
        }
        close() {
            this.end();
        }
        start() {
            if (this.readyState !== 'connecting') return;
            this.readyState = 'open';
            if (this.onopen) this.onopen();
        }
        end() {
            if (this.readyState === 'closed') return;
            this.readyState = 'closed';
            if (this.onclose) this.onclose();
        }
    }

    let made = 0;
    return class RTCPeerConnection {
        constructor(config) {
            // As Chrome does: a TURN server needs its username and credential.
            for (const s of config.iceServers || [])
                for (const u of [].concat(s.urls))
                    if (/^turns?:/.test(u) && !(s.username && s.credential))
                        throw new Error('InvalidAccessError');
            this.config = config;
            this.game = game;
            this.name = 'g' + game.id + '-' + (++made);
            this.channels = {};
            this.connectionState = 'new';
            this.remote = null;
            this.localDescription = null;
            this.remoteDescription = null;
            this.onconnectionstatechange = null;
            this.onicecandidate = null;
            world.pcs = world.pcs || new Map();
            world.pcs.set(this.name, this);
            game.pcs.push(this);
        }
        createDataChannel(label, opts) {
            const ch = new DataChannel(this, label, opts);
            this.channels[opts.id] = ch;
            if (this.connectionState === 'connected' && this.remote.channels[opts.id]) {
                ch.start();
                this.remote.channels[opts.id].start();
            }
            return ch;
        }
        createOffer() {
            return Promise.resolve({ type: 'offer', sdp: this.name });
        }
        createAnswer() {
            return Promise.resolve({ type: 'answer', sdp: this.name });
        }
        setLocalDescription(d) {
            this.localDescription = { type: d.type, sdp: d.sdp, toJSON: () => ({ type: d.type, sdp: d.sdp }) };
            this.tryConnect();
            return Promise.resolve();
        }
        setRemoteDescription(d) {
            this.remoteDescription = d;
            this.tryConnect();
            return Promise.resolve();
        }
        addIceCandidate() {
            return Promise.resolve();
        }
        getStats() {
            return Promise.resolve(new Map());
        }
        tryConnect() {
            const other = this.remoteDescription && world.pcs.get(this.remoteDescription.sdp);
            if (!other || !this.localDescription || !other.remoteDescription || other.remoteDescription.sdp !== this.name ||
                !other.localDescription || this.remote)
                return;
            this.setState('connecting');
            other.setState('connecting');
            world.later(() => {
                if (this.connectionState !== 'connecting' || other.connectionState !== 'connecting') return;
                if (!world.canLink(this.game.id, other.game.id)) {
                    this.setState('failed');
                    other.setState('failed');
                    return;
                }
                this.remote = other;
                other.remote = this;
                this.setState('connected');
                other.setState('connected');
                for (const id in this.channels) {
                    if (other.channels[id]) {
                        this.channels[id].start();
                        other.channels[id].start();
                    }
                }
            }, 2 * world.oneWay(this.game.id, other.game.id));
        }
        setState(state) {
            if (this.connectionState === state || this.connectionState === 'closed') return;
            this.connectionState = state;
            if (this.onconnectionstatechange) this.onconnectionstatechange();
        }
        close() {
            if (this.connectionState === 'closed') return;
            this.connectionState = 'closed';
            for (const ch of Object.values(this.channels)) ch.readyState = 'closed';
            const other = this.remote;
            if (other) world.later(() => other.drop(), world.oneWay(this.game.id, other.game.id));
        }
        // The other end went away, or the path between them did.
        drop() {
            if (this.connectionState === 'closed') return;
            for (const ch of Object.values(this.channels)) ch.end();
            this.setState('disconnected');
        }
    };
}

// ---- Games.

// A game with this file's JavaScript and netstats.js, as openjkdf2.js has
// them, whose player is id in room 1. old: a window with ?netold. relayUrl:
// the lobby's address, as the page gives it.
function makeGame(world, { id, old = false, netsim, relayUrl = 'ws://lobby/relay' } = {}) {
    const game = { id, pcs: [], log: [], queue: null };
    const Module = { jkRelayUrl: relayUrl };
    if (old) Module.jkNetOld = true;
    if (netsim) Module.jkNetSim = netsim;
    new Function('Module', 'setInterval', netstatsJs)(Module, () => 0);
    const clock = Module.jkNetStats.lib.clock;
    clock.now = () => world.now();
    clock.later = (fn, ms) => world.later(fn, ms);
    clock.random = seeded(id * 7919);
    const mem = new ArrayBuffer(1 << 16);
    const env = {
        Module,
        HEAPU8: new Uint8Array(mem), HEAPU16: new Uint16Array(mem), HEAP32: new Int32Array(mem),
        UTF8ToString: () => '', stringToUTF8: () => 0, lengthBytesUTF8: () => 0,
        performance: { now: () => world.now() },
        setTimeout: (fn, ms) => world.later(fn, ms), clearTimeout: t => world.cancel(t),
        setInterval: (fn, ms) => world.every(fn, ms), clearInterval: t => world.cancel(t),
        WebSocket: makeWebSocket(world), RTCPeerConnection: makeRtc(world, game),
        console: { log: t => game.log.push(t), warn: t => game.log.push(t), error: t => game.log.push(t) },
        window: undefined, location: undefined,
    };
    game.js = {};
    for (const [name, fn] of Object.entries(EM_JS)) {
        const f = new Function(...Object.keys(env), ...fn.params, fn.body);
        game.js[name] = (...args) => f(...Object.values(env), ...args);
    }
    game.Module = Module;
    game.mem = env.HEAPU8;
    game.js.wsrelay_js_connect(NETCODE);
    game.net = Module.jkNet;
    world.games.push(game);
    return game;
}

function seeded(seed) {
    let x = seed >>> 0;
    return () => {
        x = (x * 1664525 + 1013904223) >>> 0;
        return x / 4294967296;
    };
}

// The engine sends a packet (DirectPlay_Send): idTo, the bytes after the
// 8-byte header, and whether it's this player's own position update.
function send(game, idTo, body, ownPos = false) {
    const bytes = new Uint8Array(8 + body.length);
    const view = new DataView(bytes.buffer);
    view.setUint32(0, game.isHost ? 1 : game.id, true);
    view.setUint32(4, idTo, true);
    bytes.set(body, 8);
    game.mem.set(bytes, 1024);
    return game.js.wsrelay_js_send_packet(1024, bytes.length, idTo, ownPos ? 1 : 0);
}

// What the engine gets (DirectPlay_Receive): game packets, oldest first.
function received(game) {
    const out = game.net.queue.filter(d => typeof d !== 'string');
    game.net.queue = game.net.queue.filter(d => typeof d === 'string');
    return out.map(d => {
        const view = new DataView(d);
        return { from: view.getUint32(0, true), to: view.getUint32(4, true), msg: view.getUint16(8, true),
                 resend: view.getUint16(10, true), n: d.byteLength >= 16 ? view.getUint32(12, true) : null, bytes: d };
    });
}

// A player's position update, DSS_THINGPOS with 64 bytes: its thing's guid,
// then n where the position goes, to tell them apart.
function position(n) {
    const body = new Uint8Array(4 + 64);
    const view = new DataView(body.buffer);
    view.setUint16(0, 1, true);
    view.setUint32(4, n, true);
    return body;
}

// Any other message, with a resend id, numbered n.
function message(n) {
    const body = new Uint8Array(4 + 12);
    const view = new DataView(body.buffer);
    view.setUint16(0, 30, true);
    view.setUint16(2, (n % 60000) + 1, true);
    view.setUint32(4, n, true);
    return body;
}

// A host and joiners in room 1, in a match. ids: the joiners'. Each joiner
// offers the host a link as it starts (wsrelay_js_rtc_begin).
async function room(world, joiners, { hostOld = false } = {}) {
    const host = makeGame(world, { id: 1, old: hostOld });
    host.isHost = true;
    await world.run(5);
    world.lobby.seat(host.net.ws, 1, 1);
    host.js.wsrelay_js_rtc_begin(1);
    const out = [host];
    for (const j of joiners) {
        const game = makeGame(world, typeof j === 'number' ? { id: j } : j);
        await world.run(5);
        world.lobby.seat(game.net.ws, 1, game.id);
        await world.run(50);
        game.js.wsrelay_js_rtc_begin(0);
        out.push(game);
    }
    await world.run(500);
    return out;
}

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

test('the player\'s own position updates take the fast lane, and everything else the reliable channel', async () => {
    const world = makeWorld();
    const [host, join] = await room(world, [2]);
    assert.ok(host.net.peers[2].open && join.net.peers[1].open, 'linked');
    assert.equal(join.net.peers[1].pos.readyState, 'open');
    assert.equal(host.net.peers[2].pos.readyState, 'open');
    send(join, 1, position(7), true);
    send(join, 1, message(8));
    send(join, 1, position(9));      // a position of something else, from a cog
    await world.run(100);
    const got = received(host);
    assert.deepEqual(got.map(p => p.n), [7, 8, 9]);
    assert.ok(got.every(p => p.from === 2 && p.to === 1));
    assert.equal(got[0].bytes.byteLength, 8 + 4 + 64, 'the engine gets the packet without the lane\'s number');
    assert.equal(join.net.stats.fastSent, 1);
    assert.equal(host.net.stats.fastRecv, 1);
    assert.equal(join.net.peers[1].dc.sent, 2);
    // And the other way.
    send(host, 2, position(10), true);
    await world.run(100);
    assert.deepEqual(received(join).map(p => p.n), [10]);
    assert.equal(host.net.stats.fastSent, 1);
});

test('a window with ?netold opens no fast lane, so its links carry positions as before', async () => {
    const world = makeWorld();
    const [host, join] = await room(world, [{ id: 2, old: true }]);
    assert.ok(host.net.peers[2].open);
    assert.equal(join.net.peers[1].pos, undefined);
    assert.equal(host.net.peers[2].pos, undefined, 'the host only opens it when the joiner says it has it');
    send(join, 1, position(1), true);
    send(host, 2, position(2), true);
    await world.run(100);
    assert.deepEqual(received(host).map(p => p.n), [1]);
    assert.deepEqual(received(join).map(p => p.n), [2]);
    assert.equal(join.net.stats.fastSent + host.net.stats.fastSent, 0);
});

test('an update older than one already taken from that player is dropped', async () => {
    const world = makeWorld();
    const [host] = await room(world, [2]);
    const fast = (seq, n) => {
        const body = position(n);
        const buf = new ArrayBuffer(4 + 8 + body.length);
        const view = new DataView(buf);
        view.setUint32(0, seq, true);
        view.setUint32(4, 2, true);
        view.setUint32(8, 1, true);
        new Uint8Array(buf).set(body, 12);
        host.net.onFastPacket(2, buf);
    };
    fast(4294967290, 1);
    fast(4294967292, 2);
    fast(4294967291, 3);     // late
    fast(4294967292, 4);     // again
    fast(3, 5);              // past the wrap
    await world.run(10);
    assert.deepEqual(received(host).map(p => p.n), [1, 2, 5]);
    assert.equal(host.net.stats.fastOld, 2);
    // A number far behind: the joiner started over.
    fast(100000, 6);
    // One just behind, after a second of nothing: started over too.
    await world.run(1500);
    fast(99990, 7);
    fast(99991, 8);
    fast(99990, 9);
    await world.run(10);
    assert.deepEqual(received(host).map(p => p.n), [6, 7, 8]);
});

test('the host passes a joiner\'s updates on over its fast lane, or without the number over the reliable channel', async () => {
    const world = makeWorld();
    world.lobby.links = false;
    const [host, two, three, four] = await room(world, [2, { id: 3, old: true }, 4]);
    for (const n of [1, 2, 3]) {
        send(two, 3, position(n), true);
        send(two, 4, position(n), true);
    }
    send(two, 3, message(4));
    await world.run(200);
    assert.deepEqual(received(three).map(p => [p.from, p.n]), [[2, 1], [2, 2], [2, 3], [2, 4]]);
    assert.deepEqual(received(four).map(p => [p.from, p.n]), [[2, 1], [2, 2], [2, 3]]);
    assert.equal(four.net.stats.fastRecv, 3);
    assert.equal(three.net.stats.fastRecv, 0);
    assert.equal(host.net.stats.fastSent, 3);
    // With the joiner's own numbers: the host passed them on as they came.
    const seq = Object.keys(four.net.seqFrom);
    assert.deepEqual(seq, ['2 4']);
    assert.equal(four.net.seqFrom['2 4'].seq, two.net.seqTo[4]);
});

// What the engine reads for the next game packet (wsrelay_js_recv), past
// the lobby's text: its length, whether it came on the fast lane, and its
// number.
function recv(game) {
    const view = new DataView(game.mem.buffer);
    for (;;) {
        const len = game.js.wsrelay_js_recv(2048, 4096, 8000, 8008);
        if (len >= 0 && view.getInt32(8000, true)) continue;
        return { len, fast: view.getInt32(8008, true), seq: view.getUint32(8012, true),
                 n: len >= 16 ? view.getUint32(2048 + 12, true) : null };
    }
}

test('a fast lane update reaches the engine with its number, and its sender knows when it sent that one', async () => {
    const world = makeWorld();
    world.lobby.links = false;
    const [host, two, three] = await room(world, [2, 3]);
    send(two, 1, position(1), true);
    const sentAt = world.now();
    send(two, 1, message(2));
    await world.run(100);
    const a = recv(host), b = recv(host);
    assert.deepEqual([a.n, a.fast, a.seq], [1, 1, two.net.seqTo[1]]);
    assert.deepEqual([b.n, b.fast], [2, 0], 'a reliable message has no number');
    assert.equal(recv(host).len, -1);
    // A claim names the update; the sender looks up when it went.
    assert.equal(two.js.wsrelay_js_seq_sent_at(1, a.seq), sentAt);
    assert.equal(two.js.wsrelay_js_seq_sent_at(1, a.seq + 1), -1);
    assert.equal(two.js.wsrelay_js_seq_sent_at(3, a.seq), -1, 'numbers are each receiver\'s own');
    assert.equal(two.js.wsrelay_js_now(), world.now());
    // Through the host to another joiner, with the number the sender gave it
    // for that joiner.
    await world.run(1000);
    send(two, 3, position(3), true);
    const sentAt3 = world.now();
    await world.run(200);
    const c = recv(three);
    assert.deepEqual([c.n, c.fast, c.seq], [3, 1, two.net.seqTo[3]]);
    assert.equal(two.js.wsrelay_js_seq_sent_at(3, c.seq), sentAt3);
    // It remembers the last 256 it sent each player.
    for (let i = 0; i < 256; i++) send(two, 3, position(10 + i), true);
    assert.equal(two.js.wsrelay_js_seq_sent_at(3, c.seq), -1);
    assert.ok(two.js.wsrelay_js_seq_sent_at(3, c.seq + 1) > 0);
});

test('through the relay, a player\'s updates go to each player at most every 200 ms, with their time or without', async () => {
    const world = makeWorld();
    world.canLink = () => false;
    const [host, join] = await room(world, [2]);
    // One thing per size: 52 and 64 bytes as before, 60 and 72 with the
    // time Hit what you see adds (stdComm_WSRelayMotion.c).
    const sizes = [52, 64, 60, 72];
    for (let i = 0; i < 50; i++) {
        sizes.forEach((size, guid) => {
            const body = new Uint8Array(4 + size);
            const view = new DataView(body.buffer);
            view.setUint16(0, 1, true);
            view.setUint32(4, guid, true);
            send(join, 1, body, true);
        });
        await world.run(20);
    }
    await world.run(500);
    const got = received(host).filter(m => m.msg === 1);
    sizes.forEach((size, guid) => {
        const n = got.filter(m => m.n === guid && m.bytes.byteLength === 8 + 4 + size).length;
        assert.ok(n >= 5 && n <= 6, size + ' bytes: ' + n + ' of 50 in 1 s');
    });
    assert.equal(join.net.stats.wsSent + join.net.stats.wsSkipped, 200);
});

test('behind a backlog, an update is dropped rather than sent late', async () => {
    const world = makeWorld();
    const [host, join] = await room(world, [2]);
    join.net.peers[1].pos.bufferedAmount = 5000;
    send(join, 1, position(1), true);
    join.net.peers[1].pos.bufferedAmount = 0;
    send(join, 1, position(2), true);
    await world.run(100);
    assert.deepEqual(received(host).map(p => p.n), [2]);
    assert.equal(join.net.stats.fastBacklog, 1);
});

test('?netsim on the fast lane loses updates and lets them pass each other, and only newer ones get through', async () => {
    const world = makeWorld();
    const [host] = await room(world, [{ id: 2, netsim: 'rtt:100,jitter:60,loss:10' }]);
    const join = world.games[1];
    for (let i = 1; i <= 300; i++) world.later(() => send(join, 1, position(i), true), i * 10);
    await world.run(4000);
    const got = received(host).map(p => p.n);
    assert.ok(got.length > 150 && got.length < 290, 'got ' + got.length);
    for (let i = 1; i < got.length; i++) assert.ok(got[i] > got[i - 1], 'in order: ' + got.slice(i - 1, i + 1));
    assert.ok(host.net.stats.fastOld > 5, 'some came after newer ones: ' + host.net.stats.fastOld);
    // Each counted where it came from, on the fast lane.
    const p = host.Module.jkNetStats.lib.players()[2];
    assert.equal(p.positions.length, got.length);
    assert.ok(p.fast.every(Boolean));
    assert.match(host.Module.jkNetStats.text().join('\n'), /positions: [\d.]+\/s \(100% on the fast lane\)/);
});

test('two joiners link up, and what one sends the other stays in order across the switch', async () => {
    const world = makeWorld();
    // Through the host is slow, the joiners' own link is quick.
    world.oneWay = (a, b) => (a === 1 || b === 1 ? 60 : 5);
    const [host, two, three] = await room(world, [2, 3]);
    let n = 0;
    const timer = world.every(() => {
        n++;
        send(two, 3, message(n));
        send(three, 2, message(n));
        send(two, 3, position(100000 + n), true);
    }, 10);
    await world.run(3000);
    world.cancel(timer);
    await world.run(500);
    assert.ok(two.net.peers[3] && two.net.peers[3].open && three.net.peers[2].open, 'linked');
    assert.equal(two.net.route(3), 3);
    assert.equal(three.net.route(2), 2);
    for (const [game, from] of [[three, 2], [two, 3]]) {
        const got = received(game).filter(p => p.msg === 30);
        assert.deepEqual(got.map(p => p.n), Array.from({ length: n }, (_, i) => i + 1), 'all, in order, from ' + from);
        assert.ok(got.every(p => p.from === from));
    }
    // The link's own frames never reach an engine.
    for (const game of [host, two, three])
        assert.ok(received(game).every(p => p.msg !== 0x4C4A));
    // Later packets went straight there: the host passed on only those
    // from before the switch.
    const viaHost = host.net.stats.dcRecv;
    send(two, 3, message(n + 1));
    await world.run(100);
    assert.equal(host.net.stats.dcRecv, viaHost);
    assert.deepEqual(received(three).map(p => p.n), [n + 1]);
    // Positions between them took their link's fast lane.
    assert.ok(two.net.peers[3].pos.sent > 50);
    assert.ok(world.lobby.signals.some(s => s.from === 2 && s.to === 3 && s.passed));
});

// Each joiner's position updates to the others, every 20 ms: what makes
// them say hello.
function chatter(world, games) {
    let n = 0;
    return world.every(() => {
        n++;
        for (const a of games)
            for (const b of games)
                if (a !== b) send(a, b.id, position(n), true);
    }, 20);
}

test('they link once they hear from each other; the lower id offers, and offers from a higher id or without a key are ignored', async () => {
    const world = makeWorld();
    const [, two, three] = await room(world, [2, 3]);
    await world.run(2000);
    assert.equal(two.net.peers[3], undefined, 'not before they hear from each other');
    chatter(world, [two, three]);
    await world.run(2000);
    const offers = world.lobby.signals.filter(s => {
        return s.passed && ((s.from === 2 && s.to === 3) || (s.from === 3 && s.to === 2));
    });
    assert.ok(offers.length >= 2, 'offer and answer');
    assert.equal(three.pcs.filter(pc => pc.connectionState !== 'closed').length, 2, 'one to the host, one to 2');
    const offer = (game, from, x) => game.net.onSignal(from, { d: { type: 'offer', sdp: 'x' }, x });
    const pcs = three.pcs.length;
    offer(two, 3, { k: 5 });
    offer(three, 2, {});
    await world.run(10);
    assert.equal(three.pcs.length, pcs);
    assert.ok(two.net.peers[3] && two.net.peers[3].open, 'the link was left alone');
});

test('an old lobby, or a joiner with ?netold, keeps two joiners going through the host', async () => {
    for (const setup of ['old lobby', 'netold']) {
        const world = makeWorld();
        if (setup === 'old lobby') world.lobby.links = false;
        const [, two, three] = await room(world, [2, setup === 'netold' ? { id: 3, old: true } : 3]);
        for (let i = 1; i <= 20; i++) world.later(() => send(two, 3, message(i)), i * 50);
        await world.run(3000);
        assert.equal(two.net.peers[3], undefined, setup);
        assert.equal(three.net.peers[2], undefined, setup);
        assert.equal(two.net.route(3), 1);
        assert.deepEqual(received(three).map(p => p.n), Array.from({ length: 20 }, (_, i) => i + 1), setup);
    }
});

test('joiners that can\'t link keep going through the host, and stop trying after four tries', async () => {
    const world = makeWorld();
    world.canLink = (a, b) => a === 1 || b === 1;
    const [, two, three] = await room(world, [2, 3]);
    const timer = world.every(() => {
        send(two, 3, message(1));
        send(three, 2, message(1));
    }, 1000);
    await world.run(5 * 60 * 1000);
    world.cancel(timer);
    const offers = world.lobby.signals.filter(s => s.from === 2 && s.to === 3).length;
    assert.equal(offers, 4);
    assert.equal(two.net.peers[3], undefined);
    assert.equal(two.net.route(3), 1);
    assert.ok(received(three).length > 250);
});

test('a joiners\' link that drops is closed, they go back through the host in order, and link again', async () => {
    const world = makeWorld();
    world.oneWay = (a, b) => (a === 1 || b === 1 ? 40 : 5);
    const [, two, three] = await room(world, [2, 3]);
    let n = 0;
    const timer = world.every(() => {
        send(two, 3, message(++n));
        send(three, 2, position(n), true);
    }, 10);
    await world.run(2000);
    assert.equal(two.net.route(3), 3);
    // The path between them goes.
    world.canLink = (a, b) => a === 1 || b === 1;
    two.net.peers[3].pc.drop();
    three.net.peers[2].pc.drop();
    await world.run(1000);
    assert.equal(two.net.peers[3], undefined);
    assert.equal(two.net.route(3), 1);
    // And comes back.
    world.canLink = () => true;
    await world.run(10000);
    world.cancel(timer);
    await world.run(500);
    assert.ok(two.net.peers[3] && two.net.peers[3].open, 'linked again');
    assert.equal(two.net.route(3), 3);
    const got = received(three).map(p => p.n);
    // Sent on the link as it went, so lost (their sithComm sends those
    // again); the rest in order.
    for (let i = 1; i < got.length; i++) assert.ok(got[i] > got[i - 1], 'in order at ' + i + ': ' + got.slice(i - 1, i + 1));
    assert.ok(got.length > n - 10, got.length + ' of ' + n);
});

test('a joiners\' link that drops and won\'t come back is tried again after 5, 15 and 45 s, then they stay on the host', async () => {
    const world = makeWorld();
    const [, two, three] = await room(world, [2, 3]);
    const timer = world.every(() => {
        send(two, 3, message(1));
        send(three, 2, message(1));
    }, 1000);
    await world.run(3000);
    assert.equal(two.net.route(3), 3);
    world.canLink = (a, b) => a === 1 || b === 1;
    const dropped = world.now();
    two.net.peers[3].pc.drop();
    three.net.peers[2].pc.drop();
    await world.run(5 * 60 * 1000);
    world.cancel(timer);
    const offers = world.lobby.signals.filter(s => s.from === 2 && s.to === 3 && s.at > dropped).map(s => s.at);
    const waits = offers.map((t, i) => (t - (i ? offers[i - 1] : dropped)) / 1000);
    const want = [5, 15, 45];
    assert.equal(waits.length, want.length, 'waits ' + waits);
    // Each after the last one failed, which takes the made-up WebRTC a moment.
    waits.forEach((w, i) => assert.ok(w >= want[i] && w < want[i] + 0.5, 'wait ' + i + ': ' + w));
    assert.equal(two.net.route(3), 1);
    assert.ok(received(three).length > 250);
});

test('packets over a joiners\' link wait for the switch, which comes after what went through the host', async () => {
    const world = makeWorld();
    world.oneWay = (a, b) => (a === 1 || b === 1 ? 300 : 5);
    const [, two, three] = await room(world, [2, 3]);
    // Before the link is up: through the host, 600 ms.
    const times = [];
    let n = 0;
    const timer = world.every(() => {
        send(two, 3, message(++n));
        times.push({ n, route: two.net.route(3) });
    }, 20);
    await world.run(6000);
    world.cancel(timer);
    await world.run(1000);
    assert.ok(times.some(t => t.route === 1) && times.some(t => t.route === 3), 'switched while sending');
    assert.deepEqual(received(three).map(p => p.n), Array.from({ length: n }, (_, i) => i + 1));
    assert.ok(three.net.peers[2].gotSwitch);
});

test('netstats.js counts a joiner\'s packets that the host passed on under that joiner', async () => {
    const world = makeWorld();
    world.lobby.links = false;
    const [, two, three] = await room(world, [2, 3]);
    for (let i = 1; i <= 10; i++) world.later(() => send(two, 3, position(i), true), i * 20);
    await world.run(1000);
    const players = three.Module.jkNetStats.lib.players();
    assert.equal(players[2].positions.length, 10);
    assert.ok(!players[1] || players[1].positions.length === 0);
});

// A host, and a joiner that starts its game (wsrelay_js_rtc_begin) once it
// has its seat; then every 10 ms each sends the other a message, numbered.
// Returns the games and a function that stops them and says how many each
// sent.
async function talking(world, joiner = { id: 2 }, { onSignal, ...hostOpts } = {}) {
    const host = makeGame(world, { id: 1, ...hostOpts });
    host.isHost = true;
    if (onSignal) host.net.onSignal = onSignal;
    await world.run(5);
    world.lobby.seat(host.net.ws, 1, 1);
    host.js.wsrelay_js_rtc_begin(1);
    const join = makeGame(world, joiner);
    await world.run(5);
    world.lobby.seat(join.net.ws, 1, join.id);
    await world.run(world.lobby.delay + 5);
    join.js.wsrelay_js_rtc_begin(0);
    let n = 0;
    const timer = world.every(() => {
        n++;
        send(join, 1, message(n));
        send(host, join.id, message(n));
    }, 10);
    return [host, join, () => {
        world.cancel(timer);
        return n;
    }];
}

// The numbered messages the engine got, in order.
const numbers = game => received(game).filter(p => p.msg === 30).map(p => p.n);
const inOrder = (got, what) => {
    for (let i = 1; i < got.length; i++) assert.ok(got[i] > got[i - 1], what + ', in order at ' + i + ': ' + got.slice(i - 1, i + 1));
};

test('what a joiner and its host send each other stays in order across their switch from the relay to their link', async () => {
    for (const netsim of [undefined, 'rtt:200,jitter:50']) {
        const world = makeWorld();
        world.lobby.delay = 150;      // the relay is slow, their link quick
        const [host, join, stop] = await talking(world, { id: 2, netsim });
        await world.run(3000);
        const n = stop();
        await world.run(1500);
        assert.ok(join.net.peers[1].open && host.net.peers[2].open, 'linked');
        assert.ok(join.net.peers[1].sw && host.net.peers[2].sw, 'both switch');
        assert.ok(join.net.stats.wsSent > 5 && join.net.stats.dcSent > 5 && host.net.stats.wsSent > 5 && host.net.stats.dcSent > 5,
                  'some went through the relay, then the rest over the link');
        const all = Array.from({ length: n }, (_, i) => i + 1);
        assert.deepEqual(numbers(host), all, 'from the joiner, netsim ' + netsim);
        assert.deepEqual(numbers(join), all, 'from the host, netsim ' + netsim);
        assert.ok(join.net.peers[1].gotSwitch && host.net.peers[2].gotSwitch);
        // The switches never reach an engine.
        assert.ok(received(host).concat(received(join)).every(p => p.msg !== 0x4C4A));
    }
});

test('with an older host, a joiner\'s link works as before: nothing waits for a switch, and a drop leaves it open', async () => {
    const world = makeWorld();
    const [host, join, stop] = await talking(world, { id: 2 }, { old: true });
    await world.run(2000);
    assert.ok(join.net.peers[1].open);
    assert.ok(!join.net.peers[1].sw && !host.net.peers[2].sw);
    join.net.peers[1].pc.drop();
    await world.run(3000);
    assert.ok(join.net.peers[1], 'kept, as it may come back');
    assert.equal(world.lobby.signals.filter(s => s.from === 2 && s.to === 1).length, 1, 'not offered again');
    const n = stop();
    await world.run(500);
    assert.ok(numbers(host).length > n - 60);
});

test('a link with the host that drops is closed at once and offered again, and what they send stays in order', async () => {
    const world = makeWorld();
    world.lobby.delay = 60;
    const [host, join, stop] = await talking(world);
    await world.run(1500);
    const first = join.net.peers[1];
    assert.ok(first.open);
    // The path between them goes for a while.
    world.canLink = () => false;
    join.net.peers[1].pc.drop();
    host.net.peers[2].pc.drop();
    await world.run(200);
    assert.equal(join.net.peers[1], undefined, 'the joiner closed it');
    assert.equal(host.net.peers[2], undefined, 'the host closed it');
    await world.run(1500);
    world.canLink = () => true;
    await world.run(4000);
    assert.ok(join.net.peers[1] && join.net.peers[1] !== first && join.net.peers[1].open, 'linked again');
    const n = stop();
    await world.run(1000);
    // Some sent on the link as it went are lost (their sithComm sends those
    // again); the rest come in order.
    for (const [game, what] of [[host, 'from the joiner'], [join, 'from the host']]) {
        const got = numbers(game);
        inOrder(got, what);
        assert.ok(got.length > n - 10, what + ': ' + got.length + ' of ' + n);
    }
    assert.equal(join.net.hostTries, 0, 'starts over once it\'s up');
});

test('a joiner that can\'t link with its host offers again after 1, 3, 9 and 27 s, then every minute, and plays through the relay', async () => {
    const world = makeWorld();
    world.canLink = () => false;
    const [host, join, stop] = await talking(world);
    await world.run(5 * 60 * 1000);
    const n = stop();
    await world.run(500);
    const offers = world.lobby.signals.filter(s => s.from === 2 && s.to === 1).map(s => s.at);
    const gaps = offers.slice(1).map((t, i) => (t - offers[i]) / 1000);
    const want = [1, 3, 9, 27, 60, 60, 60, 60];
    assert.equal(gaps.length, want.length, 'gaps ' + gaps);
    gaps.forEach((g, i) => assert.ok(Math.abs(g - want[i]) < 0.5, 'gap ' + i + ': ' + g));
    assert.ok(host.pcs.every(pc => pc.connectionState === 'closed' || pc === host.net.peers[2]?.pc), 'the host closed the failed ones');
    assert.deepEqual(numbers(host), Array.from({ length: n }, (_, i) => i + 1));
});

test('a link with the host that isn\'t up 15 s after its offer is offered again', async () => {
    const world = makeWorld();
    // The host never answers.
    const [host, join, stop] = await talking(world, { id: 2 }, { onSignal: () => {} });
    await world.run(15000 + 2000);
    stop();
    const offers = world.lobby.signals.filter(s => s.from === 2 && s.to === 1).map(s => s.at);
    assert.equal(offers.length, 2);
    assert.ok(Math.abs(offers[1] - offers[0] - 16000) < 50, 'after 15 s, then 1 s: ' + (offers[1] - offers[0]));
    assert.equal(host.pcs.length, 0);
});

test('TURN: a joiner asks for credentials once it has its seat, and its first offer waits for them, 2 s at most', async () => {
    for (const turnMs of [300, 5000]) {
        const world = makeWorld();
        world.lobby.turn = true;
        world.lobby.turnMs = turnMs;
        const [host, join, stop] = await talking(world);
        await world.run(8000);
        stop();
        const offer = world.lobby.signals.find(s => s.from === 2 && s.to === 1);
        const ask = world.lobby.asked.find(a => a[0] === 2);
        assert.ok(ask, 'asked');
        const pc = join.pcs[0];
        if (turnMs === 300) {
            assert.ok(Math.abs(offer.at - (ask[1] + 300)) <= 25, 'offered once it had them: ' + (offer.at - ask[1]));
            assert.equal(pc.config.iceServers.length, 2);
            assert.match(pc.config.iceServers[1].urls[0], /^turn:/);
            // The host asked when the joiner joined, and had them for its answer.
            assert.ok(world.lobby.asked.some(a => a[0] === 1));
            assert.match(JSON.stringify(host.pcs[0].config.iceServers), /turn:/);
        } else {
            assert.ok(offer.at - ask[1] >= 1900 && offer.at - ask[1] < 2100, 'offered after 2 s without them: ' + (offer.at - ask[1]));
            assert.deepEqual(pc.config.iceServers, STUN);
            // They came later, for the next link.
            assert.match(JSON.stringify(join.net.iceServers()), /turn:/);
        }
        assert.ok(join.net.peers[1].open, 'linked');
        assert.equal(world.lobby.asked.filter(a => a[0] === 2).length, 1, 'asked once');
        // A lobby without TURN: nobody asks or waits.
    }
    const world = makeWorld();
    const [, join] = await talking(world);
    await world.run(100);
    assert.equal(world.lobby.asked.length, 0);
    assert.ok(join.net.peers[1], 'offered at once');
    // Nor does a window with ?netold, which links as before.
    const old = makeWorld();
    old.lobby.turn = true;
    const [host2, join2] = await talking(old, { id: 2, old: true }, { old: true });
    await old.run(100);
    assert.equal(old.lobby.asked.length, 0);
    assert.ok(join2.net.peers[1], 'offered at once');
    assert.deepEqual(host2.net.iceServers(), STUN);
});

test('TURN: credentials are asked for again once half their time is up; a lobby that has none isn\'t asked again for a minute', async () => {
    const world = makeWorld();
    world.lobby.turn = true;
    world.lobby.turnTtl = 100;
    const [, join, stop] = await talking(world);
    await world.run(1000);
    stop();
    const asks = () => world.lobby.asked.filter(a => a[0] === 2).length;
    assert.equal(asks(), 1);
    const first = JSON.stringify(join.net.iceServers());
    assert.equal(join.net.askIce(), false, 'fresh');
    await world.run(50 * 1000);
    assert.equal(join.net.askIce(), true, 'half their time is up');
    await world.run(1000);
    assert.equal(asks(), 2);
    const second = JSON.stringify(join.net.iceServers());
    assert.ok(/turn:/.test(second) && second !== first, 'the new ones');
    // None this time: only STUN, without a time.
    world.lobby.turnTtl = 0;
    await world.run(50 * 1000);
    join.net.askIce();
    await world.run(1000);
    assert.equal(asks(), 3);
    assert.equal(JSON.stringify(join.net.iceServers()), second, 'still the ones that work, until they run out');
    await world.run(30 * 1000);
    assert.equal(join.net.askIce(), false, 'not again within a minute');
    await world.run(20 * 1000);
    assert.deepEqual(join.net.iceServers(), STUN, 'they ran out');
    await world.run(10 * 1000);
    assert.equal(join.net.askIce(), true);
});

test('TURN: the list every game gets when its socket opens never replaces credentials that still work', async () => {
    const world = makeWorld();
    world.lobby.turn = true;
    const [, join, stop] = await talking(world);
    await world.run(1000);
    const ice = JSON.stringify(join.net.iceServers());
    assert.match(ice, /turn:/);
    join.net.ws.drop();
    await world.run(3000);
    assert.equal(join.net.state, 1, 'back');
    assert.equal(JSON.stringify(join.net.iceServers()), ice);
    stop();
});

test('TURN servers the browser won\'t take are left out, and the link is made without them', async () => {
    const world = makeWorld();
    world.lobby.turn = true;
    const [host, join, stop] = await talking(world);
    await world.run(1000);
    // A list with no credential, and a new link.
    join.net.turn = [{ urls: ['turn:turn.example.org:3478'] }];
    join.net.turnEnds = world.now() + 1000000;
    join.net.closePeer(1);
    join.net.hostOffer();
    await world.run(1000);
    stop();
    assert.ok(join.net.peers[1] && join.net.peers[1].open, 'linked');
    assert.deepEqual(join.net.peers[1].pc.config.iceServers, STUN);
    assert.ok(join.net.log.some(t => /refused the TURN servers/.test(t)));
    assert.ok(host.net.peers[2].open);
});

test('a link\'s switch goes over the link when the relay is down, and nothing waits for a switch that can\'t come', async () => {
    const world = makeWorld();
    const [host, join, stop] = await talking(world);
    // The joiner loses the relay as their link comes up.
    let dropped = false;
    const watch = world.every(() => {
        const p = join.net.peers[1];
        if (!dropped && p && p.pc.remoteDescription) {
            dropped = true;
            join.net.ws.drop();
        }
    }, 1);
    await world.run(200);
    world.cancel(watch);
    assert.ok(dropped && join.net.peers[1].open && host.net.peers[2].open);
    // Nothing waited out its 3 s.
    assert.ok(host.net.peers[2].gotSwitch, 'the host got the joiner\'s switch over the link');
    assert.ok(join.net.peers[1].gotSwitch, 'the joiner took the host\'s packets');
    numbers(host);
    numbers(join);
    await world.run(100);
    assert.ok(numbers(host).length > 5 && numbers(join).length > 5, 'still coming, both ways');
    stop();
    assert.ok(!host.log.concat(join.log).some(t => /after 3 s/.test(t)));
    assert.ok(!host.net.log.concat(join.net.log).some(t => /after 3 s/.test(t)));
});

test('a joiner whose relay goes while it waits for its host\'s switch takes what came over the link at once', async () => {
    const world = makeWorld();
    world.lobby.delay = 1000;
    const [host, join, stop] = await talking(world);
    // Until the link is up, then a little.
    for (let i = 0; i < 500 && !(join.net.peers[1] && join.net.peers[1].open); i++) await world.run(10);
    await world.run(100);
    const p = join.net.peers[1];
    assert.ok(p.open && !p.gotSwitch && p.held.length > 3, 'waiting, with ' + (p.held && p.held.length));
    join.net.ws.drop();
    assert.ok(p.gotSwitch && !p.held.length);
    stop();
});

test('a game tells the lobby its build\'s netcode in the address, after what the address has, also when it reconnects', async () => {
    const world = makeWorld();
    const game = makeGame(world, { id: 2 });
    assert.equal(game.net.ws.url, 'ws://lobby/relay?net=' + NETCODE);
    await world.run(5);
    game.net.ws.drop();
    game.js.wsrelay_js_connect(NETCODE);
    assert.equal(game.net.ws.url, 'ws://lobby/relay?net=' + NETCODE);
    const other = makeGame(world, { id: 3, relayUrl: 'ws://lobby/relay?x=1' });
    assert.equal(other.net.ws.url, 'ws://lobby/relay?x=1&net=' + NETCODE);
});

test('the landing page offers Join for the games of the netcode its game speaks', async () => {
    const page = /\n      var NETCODE = (\d+);\n/.exec(read('packaging/wasm/index.html'));
    assert.ok(page, 'index.html has NETCODE');
    assert.equal(Number(page[1]), NETCODE);
});

test('the lobby\'s list of what it can do stays out of the engine\'s queue', async () => {
    const world = makeWorld();
    const [host, join] = await room(world, [2]);
    assert.deepEqual(join.net.lobbyCan, ['links']);
    assert.ok(!host.net.queue.includes('lobby links') && !join.net.queue.includes('lobby links'));
});

// ---- Transfer links (net.mapBulk).

// maps.js as far as the network code sees it: the host sends each frame back
// the way it came, and both ends note the way each came in.
function echoMaps(host, ...joiners) {
    host.maps = [];
    host.Module.jkMaps = {
        isFrame: () => false,
        onFrame(from, data, via) {
            host.maps.push({ from, via });
            host.net.mapSend(from, data, via);
        },
    };
    for (const j of joiners) {
        j.maps = [];
        j.Module.jkMaps = { isFrame: () => false, onFrame: (from, data, via) => j.maps.push({ from, via }) };
    }
}

const bulkSignals = world => world.lobby.signals.filter(s => s.obj && s.obj.b !== undefined);
const openPcs = game => game.pcs.filter(pc => pc.connectionState !== 'closed');

test('a joiner downloading opens three transfer links to its host, and each frame comes back the way it went', async () => {
    const world = makeWorld();
    const [host, join] = await room(world, [2]);
    echoMaps(host, join);
    assert.ok(join.net.peers[1].bulkOk, 'the host said it takes them');
    assert.deepEqual(join.net.mapLanes(1), ['direct'], 'none before a download');
    join.net.mapBulk(1, true);
    await world.run(500);
    assert.deepEqual(join.net.mapLanes(1), ['direct', 'direct1', 'direct2', 'direct3']);
    assert.equal(host.net.bulkCount(2), 3);
    assert.equal(openPcs(join).length, 4, 'the main link and three more');
    for (const via of ['direct', 'direct1', 'direct2', 'direct3'])
        assert.equal(join.net.mapSend(1, new ArrayBuffer(32), via), via);
    await world.run(200);
    assert.deepEqual(host.maps.map(m => m.from + ' ' + m.via), ['2 direct', '2 direct1', '2 direct2', '2 direct3']);
    assert.deepEqual(join.maps.map(m => m.from + ' ' + m.via), ['1 direct', '1 direct1', '1 direct2', '1 direct3']);
    // Game packets never go over them.
    send(join, 1, message(5));
    await world.run(100);
    assert.deepEqual(received(host).map(p => p.n), [5]);
    assert.ok(Object.values(join.net.bulk[1]).every(l => !l.pc.channels[0]), 'a transfer link has only a map channel');
});

test('transfer links close 20 s after the last download, not while another runs', async () => {
    const world = makeWorld();
    const [host, join] = await room(world, [2]);
    echoMaps(host, join);
    join.net.mapBulk(1, true);
    join.net.mapBulk(1, true);
    await world.run(500);
    join.net.mapBulk(1, false);
    await world.run(30000);
    assert.equal(join.net.mapLanes(1).length, 4, 'one download still runs');
    join.net.mapBulk(1, false);
    await world.run(19000);
    assert.equal(join.net.mapLanes(1).length, 4, 'kept for a while, for the next one');
    await world.run(1500);
    assert.deepEqual(join.net.mapLanes(1), ['direct']);
    assert.equal(join.net.bulkCount(), 0);
    assert.equal(host.net.bulkCount(), 0, 'the host let them go when they closed');
    assert.equal(openPcs(join).length, 1);
    assert.equal(openPcs(host).length, 1);
    assert.ok(join.net.peers[1].open && host.net.peers[2].open, 'the main link stays up');
    // A download after that opens them again.
    join.net.mapBulk(1, true);
    await world.run(500);
    assert.equal(join.net.mapLanes(1).length, 4);
});

test('a host from before transfer links, or a window with ?netold, gets no offer for one', async () => {
    for (const opts of [{ hostOld: true }, {}]) {
        const world = makeWorld();
        const [host, join] = await room(world, [opts.hostOld ? 2 : { id: 2, old: true }], opts);
        // A host from before them says nothing of them in its answer, as
        // one with ?netold doesn't.
        assert.ok(!join.net.peers[1].bulkOk);
        join.net.mapBulk(1, true);
        await world.run(2000);
        assert.deepEqual(join.net.mapLanes(1), ['direct']);
        assert.equal(bulkSignals(world).length, 0);
        assert.equal(openPcs(host).length, 1);
    }
});

test('transfer links wait for the main link, and go when it does', async () => {
    const world = makeWorld();
    world.canLink = () => false;
    const [host, join] = await room(world, [2]);
    join.net.mapBulk(1, true);
    await world.run(500);
    assert.deepEqual(join.net.mapLanes(1), [], 'the relay, then');
    assert.equal(bulkSignals(world).length, 0);
    world.canLink = () => true;
    await world.run(3000);
    assert.ok(join.net.mapDirect(1), 'the main link came up');
    join.net.mapLanes(1);
    await world.run(500);
    assert.equal(join.net.mapLanes(1).length, 4);
    world.canLink = () => false;
    join.net.peers[1].pc.drop();
    host.net.peers[2].pc.drop();
    await world.run(200);
    assert.equal(join.net.bulkCount(), 0, 'closed with the main link');
    assert.equal(host.net.bulkCount(), 0);
});

test('the host takes 12 transfer links at most at once, and a joiner it turned down tries again later', async () => {
    const world = makeWorld();
    const games = await room(world, [2, 3, 4, 5, 6]);
    const host = games[0], joiners = games.slice(1);
    echoMaps(host, ...joiners);
    for (const j of joiners) {
        j.net.mapBulk(1, true);
        await world.run(200);
    }
    await world.run(500);
    assert.equal(host.net.bulkCount(), 12);
    assert.deepEqual(joiners.map(j => j.net.mapLanes(1).length), [4, 4, 4, 4, 1]);
    // One finishes, and its links close 20 s on; the last one's offers that
    // went unanswered are made again as its download goes on (maps.js asks
    // for the lanes every 250 ms).
    joiners[0].net.mapBulk(1, false);
    const asking = world.every(() => joiners[4].net.mapLanes(1), 250);
    await world.run(50000);
    world.cancel(asking);
    assert.equal(joiners[0].net.mapLanes(1).length, 1);
    assert.equal(joiners[4].net.mapLanes(1).length, 4);
    assert.equal(host.net.bulkCount(), 12);
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
