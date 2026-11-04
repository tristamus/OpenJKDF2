// A host's map upload to a player joining mid-match, and the host's game
// packets to the others on the same connection, over a modelled upload:
// node packaging/wasm/test/pacing-test.mjs
//
// The host runs maps.js and the network code's own JavaScript (the EM_JS in
// stdComm_WSRelay.c and stdComm_Imperial.c), the joiner maps.js as the live
// build has it (test/fixtures/maps-503d826.js), which can't be updated. Each
// run is made twice: with this maps.js on the host, and with the live one,
// which sends whatever is asked for at once.
//
// The upload is one queue that leaves at C bytes a second, as one TCP
// connection's would. Of what waits in it, up to K bytes wait where a page
// can't see them (the computer's socket buffer, the router), and only the
// rest shows as the WebSocket's bufferedAmount. The lobby answers ping as
// lobby.js does, behind everything sent before it. A game packet's wait is
// the time from its send to its last byte leaving.
//
// Over direct links the joiner's map channel is that queue, and what waits
// in it beyond K is its bufferedAmount. The other player's link and the
// lobby's socket are connections of their own on the same upload: what they
// send waits behind the K bytes the computer and the router hold, not behind
// the map channel's backlog in the browser.
//
// On the Imperial Network Protocol the engine's pump (imperial_XferPump in
// stdComm_Imperial.c, written out again below) sends from maps.js's queue
// each frame, in pieces of 1360 bytes, at most 1 MiB/s, and with this
// maps.js no more while the socket has 16 KiB waiting. The relay server
// answers the request the engine times (jkrelay_AskTimed) behind everything
// sent before it, as the lobby answers ping; once it has answered one, the
// engine sends the next no sooner than 2 s after the last.
//
// Made-up maps, no game files.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { makeGob, makeFS, makeClock } from './helpers.mjs';

const read = path => readFileSync(new URL('../../../' + path, import.meta.url), 'utf8');
const relayC = read('src/Platform/Networking/WSRelay/stdComm_WSRelay.c');
const imperialC = read('src/Platform/Networking/Imperial/stdComm_Imperial.c');
const newMaps = read('packaging/wasm/maps.js');
const oldMaps = read('packaging/wasm/test/fixtures/maps-503d826.js');

// The EM_JS functions: name -> { params, body } (as transport-test.mjs).
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
const RELAY_JS = emJsFunctions(relayC);
const IMPERIAL_JS = emJsFunctions(imperialC);

function bindJs(fns, env) {
    const out = {};
    for (const [name, fn] of Object.entries(fns)) {
        const f = new Function(...Object.keys(env), ...fn.params, fn.body);
        out[name] = (...args) => f(...Object.values(env), ...args);
    }
    return out;
}

function loadMaps(clock, src, Module) {
    // Joiners here can't unsqueeze, so blocks go as they are.
    new Function('Module', 'setTimeout', 'clearTimeout', 'Date', 'DecompressionStream', src)(
        Module, clock.setTimeout, clock.clearTimeout, clock.Date, undefined);
    return Module.jkMaps;
}

function uplink(clock, C, K) {
    let doneAt = 0;
    const up = {
        // When the last byte of n bytes sent now leaves.
        send(n) {
            const start = Math.max(clock.now(), doneAt);
            doneAt = start + n * 1000 / C;
            return doneAt;
        },
        // The same for another connection's n bytes, which wait only behind
        // what the computer and the router hold.
        sendAhead(n) {
            const ahead = Math.min(up.waiting(), K);
            doneAt = Math.max(clock.now(), doneAt) + n * 1000 / C;
            return clock.now() + (ahead + n) * 1000 / C;
        },
        waiting: () => Math.max(0, doneAt - clock.now()) * C / 1000,
        visible: () => Math.max(0, up.waiting() - K),
    };
    return up;
}

const ONE_WAY = 25;      // ms each way between the host, the lobby and the joiner
const FRAMING = 8;       // a WebSocket message's own bytes, roughly
const GAME_HZ = 20, GAME_BYTES = 100;
const BIG_PIECE = 1360;  // jkrelay_client.c's JKR_BIG_PIECE
const BIG_OVERHEAD = 16 + 8 + 16;

const retailInstall = {
    '/jk1/resource/Res2.gob': makeGob([{ name: 'mat\\a.mat', bytes: randomBytes(3000) }]),
    '/jk1/resource/Res1hi.gob': makeGob([{ name: 'x\\y', bytes: randomBytes(10) }]),
    '/jk1/episode/JK1MP.gob': makeGob([{ name: 'jkl\\01narshadda.jkl', bytes: randomBytes(4000) }]),
};

// One run: { mbit, K, mapBytes, path: 'relay' | 'direct' | 'imperial', paced, pong }.
// pong: the lobby answers ping, or the relay server the timed request.
// matchAt: the other player starts playing this long after the download
// starts, not before it.
async function run({ mbit, K, mapBytes, path, paced, pong = true, limitMs = 900000, matchAt = 0 }) {
    const clock = makeClock();
    const C = mbit * 1e6 / 8;
    const up = uplink(clock, C, K);
    const gob = makeGob([{ name: 'episode.jk', bytes: randomBytes(1000) }, { name: 'big.jkl', bytes: randomBytes(mapBytes) }]);
    const stream = gob.length + 4;
    const host = { FS: makeFS({ ...retailInstall, '/jk1/episode/ctf_pace.gob': gob }) };
    const join = { FS: makeFS({ ...retailInstall }) };
    const waitsAt = [], probesAt = [];
    // On Imperial, the relay server's answers the engine took, and the times
    // the link told maps.js there was no queue, in the transfer.
    let answers = 0, clear = 0;
    const requests = [];
    // lastIn: when the joiner had the last part that came.
    let blockBytes = 0, transferring = false, t0 = Infinity, lastIn = 0;
    const playing = () => clock.now() - t0 >= matchAt;
    const timers = [];

    const toJoiner = (bytes, leaves) => {
        const copy = bytes.slice(0);
        const b = new Uint8Array(copy);
        if (b[12] === 6) {
            blockBytes += copy.byteLength - 40;
            if (transferring)
                lastIn = Math.max(lastIn, leaves + ONE_WAY);
        }
        clock.setTimeout(() => join.jkMaps.onFrame(1, copy, path === 'relay' ? 'relay' : 'direct'), leaves - clock.now() + ONE_WAY);
    };
    const gameSent = leaves => {
        if (transferring)
            waitsAt.push([clock.now() - t0, leaves - clock.now()]);
    };
    const env = (extra) => ({
        Module: host,
        HEAPU8: new Uint8Array(1 << 16), HEAPU16: new Uint16Array(1 << 15), HEAP32: new Int32Array(1 << 14),
        UTF8ToString: () => '', stringToUTF8: () => 0, lengthBytesUTF8: () => 0,
        performance: clock.performance,
        setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout,
        setInterval: clock.setInterval, clearInterval: clock.clearInterval,
        RTCPeerConnection: undefined,
        console: { log() {}, warn() {}, error() {} },
        window: undefined, location: undefined, navigator: undefined, fetch: undefined,
        ...extra,
    });

    let engineFrame;
    if (path === 'relay' || path === 'direct') {
        const direct = path === 'direct';
        let ws = null;
        class WebSocket {
            constructor() {
                ws = this;
                this.readyState = 0;
                clock.setTimeout(() => { this.readyState = 1; this.onopen(); }, 1);
            }
            get bufferedAmount() { return direct ? 0 : up.visible(); }
            send(data) {
                const text = typeof data === 'string';
                const n = (text ? data.length : data.byteLength) + FRAMING;
                const leaves = direct ? up.sendAhead(n) : up.send(n);
                if (text) {
                    if (data === 'ping' && pong)
                        clock.setTimeout(() => this.onmessage({ data: 'pong' }), leaves - clock.now() + 2 * ONE_WAY);
                    return;
                }
                const b = new Uint8Array(data.buffer || data, data.byteOffset || 0, data.byteLength);
                if (b[8] === 0x4A && b[9] === 0x4B)
                    toJoiner(b.slice().buffer, leaves);
                else
                    gameSent(leaves);
            }
            close() {}
        }
        // Direct links to the joiner (2) and the other player (3), as
        // net.peer makes them: the joiner's map channel is the queue above.
        const channel = (id, label) => ({
            label, readyState: 'connecting', binaryType: 'blob', close() {},
            get bufferedAmount() { return id === 2 && label === 'jk-map' ? up.visible() : 0; },
            send(data) {
                const b = new Uint8Array(data.buffer || data, data.byteOffset || 0, data.byteLength);
                if (label === 'jk-map')
                    toJoiner(b.slice().buffer, up.send(b.byteLength + FRAMING));
                else if (id === 3)
                    gameSent(up.sendAhead(b.byteLength + FRAMING));
            },
        });
        class RTCPeerConnection {
            constructor() {
                this.connectionState = 'new';
                this.id = RTCPeerConnection.next++;
            }
            createDataChannel(label) { return channel(this.id, label); }
            getStats() { return Promise.resolve(new Map()); }
            close() {}
        }
        RTCPeerConnection.next = 2;
        const js = bindJs(RELAY_JS, env(direct ? { WebSocket, RTCPeerConnection } : { WebSocket }));
        js.wsrelay_js_connect(0);
        await clock.run(5);
        js.wsrelay_js_rtc_begin(1);
        const net = host.jkNet;
        if (direct) {
            for (const id of [2, 3]) {
                const p = net.peer(id);
                p.pc.connectionState = 'connected';
                p.dc.readyState = p.mc.readyState = 'open';
                p.dc.onopen();
            }
        }
        // Another player, through the lobby or directly: the host sends to
        // it, and hears from it.
        timers.push(clock.setInterval(() => {
            if (!playing())
                return;
            const out = new Uint8Array(GAME_BYTES);
            new DataView(out.buffer).setUint32(4, 3, true);
            out[8] = 10;
            net.sendNow(out, 3);
            const inbound = new Uint8Array(GAME_BYTES);
            new DataView(inbound.buffer).setUint32(0, 3, true);
            inbound[8] = 10;
            if (direct)
                net.peers[3].dc.onmessage({ data: inbound.buffer });
            else
                ws.onmessage({ data: inbound.buffer });
            net.queue = [];
        }, 1000 / GAME_HZ));
        engineFrame = () => js.wsrelay_js_map_pump();
        join.jkNet = {
            mapDirect: () => direct, rtcEnabled: () => direct,
            mapSend(id, bytes) {
                const copy = bytes.slice(0);
                new DataView(copy).setUint32(0, 2, true);   // the lobby stamps the sender
                if (new Uint8Array(copy)[12] === 5)
                    requests.push(new DataView(copy).getUint32(24, true));
                clock.setTimeout(() => direct ? net.peers[2].mc.onmessage({ data: copy }) : ws.onmessage({ data: copy }), 2 * ONE_WAY);
                return direct ? 'direct' : 'relay';
            },
        };
    } else {
        // The relay server's WebSocket, as jkrelay_client.c keeps it.
        const ws = {
            get bufferedAmount() { return up.visible(); },
            send(n) { return up.send(n + FRAMING); },
        };
        host.jkrWs = { next: 2, socks: { 1: { ws, out: [], q: [] } } };
        const js = bindJs(IMPERIAL_JS, env({}));
        js.imperial_js_xfer_init(1);
        const said = host.jkImperial.mapCongested;
        host.jkImperial.mapCongested = (...a) => {
            const c = said(...a);
            if (transferring && c === false)
                clear++;
            return c;
        };
        const RATE = 1024 * 1024, BURST = 64 * 1024;
        let tokens = BURST, tokensMs = clock.now();
        // jkrelay_AskTimed and jkrelay_TimedState: the answer is taken in the
        // engine's next frame after it comes.
        const probe = { seq: 0, out: false, back: false, sentAt: 0, rtt: 0, base: 0, got: 0 };
        const sendProbe = () => {
            if (probe.got ? probe.out || clock.now() - probe.sentAt < 2000
                          : probe.out && clock.now() - probe.sentAt < 10000)
                return;
            if (transferring)
                probesAt.push(clock.now());
            const seq = ++probe.seq;
            probe.out = true;
            probe.back = false;
            probe.sentAt = clock.now();
            const leaves = ws.send(16 + 20);
            if (pong)
                clock.setTimeout(() => { if (probe.seq === seq) probe.back = true; }, leaves - clock.now() + 2 * ONE_WAY);
        };
        if (paced)
            sendProbe();    // as the host's game opens
        // imperial_XferPump.
        engineFrame = () => {
            const now = clock.now();
            if (probe.back) {
                const rtt = now - probe.sentAt;
                probe.out = probe.back = false;
                probe.rtt = rtt;
                probe.base = probe.got ? Math.min(probe.base, rtt) : rtt;
                probe.got++;
                if (transferring)
                    answers++;
            }
            if (paced && js.imperial_js_xfer_serve(1, probe.got, probe.rtt, probe.base, probe.out ? now - probe.sentAt : -1))
                sendProbe();
            tokens = Math.min(BURST, tokens + (now - tokensMs) * RATE / 1000);
            tokensMs = now;
            while (tokens > 0) {
                if (paced && host.jkrWs.socks[1].ws.bufferedAmount >= 16 * 1024)
                    break;
                const f = host.jkImperial.out.shift();
                if (!f)
                    break;
                tokens -= f.b.length;
                let leaves = 0;
                for (let at = 0; at < f.b.length; at += BIG_PIECE)
                    leaves = ws.send(Math.min(BIG_PIECE, f.b.length - at) + BIG_OVERHEAD);
                toJoiner(f.b.slice().buffer, leaves);
            }
        };
        timers.push(clock.setInterval(() => gameSent(ws.send(GAME_BYTES + 32)), 1000 / GAME_HZ));
        join.jkImperial = {
            active: true, mapDirect: () => true,
            mapSend(id, bytes) {
                const copy = bytes.slice(0);
                if (new Uint8Array(copy)[12] === 5)
                    requests.push(new DataView(copy).getUint32(24, true));
                clock.setTimeout(() => host.jkMaps.onFrame(2, copy, 'direct'), 2 * ONE_WAY);
                return 'direct';
            },
        };
    }
    loadMaps(clock, paced ? newMaps : oldMaps, host);
    loadMaps(clock, oldMaps, join);
    timers.push(clock.setInterval(engineFrame, 16));

    host.jkMaps.host('ctf_pace', 'gob');
    await new Promise(r => setTimeout(r, 200));
    await clock.run(3000);
    t0 = clock.now();
    transferring = true;
    join.jkMaps.fetch('ctf_pace', 'gob');
    await clock.until(() => join.jkMaps.state() !== 0, limitMs, 200);
    transferring = false;
    const secs = (clock.now() - t0) / 1000;
    const state = join.jkMaps.state();
    if (state === 0)
        join.jkMaps.abort('cancelled');
    timers.forEach(t => clock.clearInterval(t));
    // The game packets sent while parts were on the way. The joiner's own
    // checks after the last part run on the computer's clock, not the model's,
    // so how long they take here means nothing: xfer is when the last part came.
    const xfer = (lastIn - t0) / 1000;
    const waits = waitsAt.filter(([t]) => t <= lastIn - t0).map(([, w]) => w).sort((a, b) => a - b);
    const at = q => Math.round(waits[Math.min(waits.length - 1, Math.floor(q * waits.length))]);
    const blocks = Math.ceil(stream / 1048576);
    const r = {
        state, secs, xfer, stream, ratio: blockBytes / stream, asks: requests.length, reasks: requests.length - new Set(requests).size,
        blocks, median: at(0.5), p95: at(0.95), max: Math.round(waits[waits.length - 1]),
        rate: paced ? host.jkMaps.uploads().rate : null, waitsAt, probesAt, answers, clear,
        ok: state === 1 && Buffer.compare(Buffer.from(join.FS.files.get('/jk1/episode/ctf_pace.gob') || []), Buffer.from(gob)) === 0,
    };
    return r;
}

function describe(name, r) {
    return `${name}: ${['stopped', 'done', 'failed'][r.state] || r.state} in ${r.secs.toFixed(0)} s ` +
           `(the last part in at ${r.xfer.toFixed(1)} s), ` +
           `${(r.ratio).toFixed(2)}x the stream sent, ${r.reasks} block(s) asked for again; ` +
           `game packets wait median ${r.median} ms, p95 ${r.p95} ms, max ${r.max} ms` +
           (r.rate !== null ? `; rate at the end ${(r.rate / 1000).toFixed(0)} KB/s` : '') +
           (r.probesAt.length ? `; ${r.probesAt.length} timed request(s) to the relay server` : '');
}

const tests = [];
const test = (name, fn) => tests.push({ name, fn });

const K = 1024 * 1024;
for (const s of [
    { path: 'relay', mbit: 1, mapBytes: 3000000 },
    { path: 'relay', mbit: 1, mapBytes: 8000000 },
    { path: 'relay', mbit: 2, mapBytes: 3000000 },
    { path: 'relay', mbit: 2, mapBytes: 8000000 },
    { path: 'imperial', mbit: 2, mapBytes: 3000000 },
    { path: 'imperial', mbit: 2, mapBytes: 8000000 },
]) {
    const what = `${s.path === 'relay' ? 'through the lobby' : 'on the Imperial Network Protocol'} at ${s.mbit} Mbit/s up, ` +
                 `a ${s.mapBytes / 1e6} MB map`;
    test(what + ': the map comes, sent once, and game packets wait far less than with the live host', async () => {
        const paced = await run({ ...s, K, paced: true });
        const old = await run({ ...s, K, paced: false, limitMs: 300000 });
        console.log('    paced     ' + describe(what, paced));
        console.log('    live host ' + describe(what, old));
        assert.ok(paced.ok, 'not installed');
        assert.ok(paced.ratio < 1.02, 'sent ' + paced.ratio.toFixed(2) + 'x');
        assert.equal(paced.reasks, 0);
        assert.ok(paced.max * 4 < old.max, `max wait ${paced.max} ms, live host ${old.max} ms`);
        assert.ok(paced.p95 * 4 < old.p95, `p95 wait ${paced.p95} ms, live host ${old.p95} ms`);
    });
}

test('a download that starts before a match and goes on into it holds the game up only while what went before it leaves', async () => {
    // Before the match the host sends as fast as the socket takes it, so up
    // to K waits where the page can't see it when the match starts (about
    // 10 s at 1 Mbit/s). Game packets then wait for that to leave, and the
    // rate stays low until it has, however the lobby's answer looked then.
    const s = { path: 'relay', mbit: 1, mapBytes: 8000000, matchAt: 6000 };
    const paced = await run({ ...s, K, paced: true });
    console.log('    ' + describe('relay, 1 Mbit/s up, 8 MB, the match 6 s in', paced));
    assert.ok(paced.ok);
    assert.ok(paced.ratio < 1.25, 'sent ' + paced.ratio.toFixed(2) + 'x');
    const late = paced.waitsAt.filter(([t]) => t >= s.matchAt + 15000).map(([, w]) => w);
    assert.ok(late.length > 100);
    assert.ok(Math.max(...late) < 1000, `game packets wait ${Math.round(Math.max(...late))} ms 15 s into the match`);
});

test('on the Imperial Network Protocol the rate grows on a fast upload, as the relay server answers at once', async () => {
    const s = { path: 'imperial', mbit: 20, mapBytes: 8000000 };
    const paced = await run({ ...s, K, paced: true });
    const mute = await run({ ...s, K, paced: true, pong: false });
    console.log('    ' + describe('Imperial, 20 Mbit/s up, 8 MB', paced));
    console.log('    ' + describe('a server that never answers', mute));
    assert.ok(paced.ok && mute.ok);
    assert.ok(paced.ratio < 1.02 && mute.ratio < 1.02);
    assert.equal(paced.reasks, 0);
    assert.equal(mute.reasks, 0);
    assert.ok(paced.xfer * 2 < mute.xfer, `${paced.xfer.toFixed(1)} s, ${mute.xfer.toFixed(1)} s without answers`);
    assert.ok(paced.max < 300, 'game packets wait ' + paced.max + ' ms');
    // Without the server's answers, only what waits in the browser shows:
    // the rate never grows.
    assert.ok(mute.rate < 100000, 'rate ' + mute.rate);
    // The server isn't ours: once it has answered, a request each 2 s at
    // most, and one each 10 s to a server that never does.
    for (const [r, every] of [[paced, 2000], [mute, 10000]]) {
        const gaps = r.probesAt.slice(1).map((t, i) => t - r.probesAt[i]);
        assert.ok(r.probesAt.length >= 2 && Math.min(...gaps) >= every, `timed requests ${Math.min(...gaps)} ms apart`);
        assert.ok(r.probesAt.length <= r.secs * 1000 / every + 1, `${r.probesAt.length} timed requests in ${r.secs.toFixed(0)} s`);
    }
});

test('through a lobby that never answers ping (relay.py) the rate stays where it starts', async () => {
    const s = { path: 'relay', mbit: 1, mapBytes: 3000000 };
    const paced = await run({ ...s, K, paced: true, pong: false });
    console.log('    ' + describe('relay.py, 1 Mbit/s up, 3 MB', paced));
    assert.ok(paced.ok);
    assert.ok(paced.ratio < 1.02);
    assert.equal(paced.reasks, 0);
    assert.equal(paced.rate, 48000);
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
