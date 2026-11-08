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
// A joiner's tab can also be slow to take what comes on its map channel: it
// takes so many bytes a second, all at once at each of its frames. What came
// and wasn't taken fills the channel's window at the joiner, and then the
// rest waits in the host's browser as the channel's bufferedAmount, not on
// the upload.
//
// On the Imperial Network Protocol the engine's pump (imperial_XferPump in
// stdComm_Imperial.c, written out again below) sends from maps.js's queue
// each frame, in pieces of 1360 bytes, at most 1 MiB/s, and with this
// maps.js no more while the socket has 16 KiB waiting. The relay server
// answers the request the engine times (jkrelay_AskTimed) behind everything
// sent before it, as the lobby answers ping; once it has answered one, the
// engine sends the next no sooner than 2 s after the last.
//
// The host's page can be slow too (hostFrameMs): its game's frames then run
// back to back, 1 ms apart, and nothing else runs on the page during one.
// What comes in while one runs (the lobby's or the relay server's answer, a
// packet, a joiner's request) and a timer due then are taken only after the
// frame after it, as Chrome takes a WebSocket's message that comes during a
// long frame driven by requestAnimationFrame (measured with frames of 33 and
// 400 ms: read 2 frames on; at 16 ms after its own frame, as here); a message
// the page posts to itself (MessageChannel) is taken right after the frame it
// was posted in. The engine's own calls (the map pump, the Imperial pump)
// come at the start of each frame. The frames are that slow from hostSlowAt
// on, and 16 ms before: the menus, where the lobby's first answers come.
// With hostEveryMs, the frames start that many ms apart instead, and the page
// waits for nothing between them (a 30 Hz display, the FPS limit, a hidden
// tab's timer): what comes in during one is taken right after it, and
// between them at once.
// Without hostFrameMs, nothing waits for the page, and the engine's frame
// comes every 16 ms.
//
// Made-up maps, no game files.

import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { makeGob, makeFS, makeClock } from './helpers.mjs';

const read = path => readFileSync(new URL('../../../' + path, import.meta.url), 'utf8');
const relayC = read('src/Platform/Networking/WSRelay/stdComm_WSRelay.c');
const imperialC = read('src/Platform/Networking/Imperial/stdComm_Imperial.c');
// Whether the relay client times the server's answer from when the page took
// it in (jkr_wsRecvAtMs), not from when the engine read it, a frame on.
const answerStamped = /jkr_wsRecvAtMs/.test(read('src/Platform/Networking/Imperial/jkrelay_client.c'));
// How many answers a host's pump asks for whether maps.js does or not.
const timedFirst = +((/#define IMPERIAL_XFER_TIMED_FIRST \((\d+)\)/.exec(imperialC) || [])[1] || 0);
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

// The host's page (see the top): deliver(fn) runs what came in now, when the
// page takes it; post(fn), a message it posted to itself. frameStarts(fn,
// timers) runs fn at the start of each of the game's frames. The page is busy
// for busyFor ms first (a level loading), and its frames are F ms from slowAt
// on, 16 ms before (the menus), every ms apart if given, else back to back.
function hostPage(clock, F, slowAt = 0, busyFor = 0, every = 0) {
    const start = clock.now(), busyUntil = start + busyFor;
    const period = every || F + 1, T0 = start + Math.max(slowAt, busyFor);
    // The frame t falls in, [start, end], or null between frames.
    const frameAt = t => {
        if (t < busyUntil)
            return [start, busyUntil];
        if (!F || t < T0)
            return null;
        const s = T0 + Math.floor((t - T0) / period) * period;
        return t < s + F ? [s, s + F] : null;
    };
    // Read after the frame after the one it came in, or at 16 ms, or with
    // time between them, its own.
    const hops = F > 16 && !every ? 1 : 0;
    const page = {
        deliver(fn) {
            const now = clock.now(), f = frameAt(now);
            if (!f)
                return fn();
            clock.setTimeout(fn, f[1] - now + (f[0] >= T0 ? hops * period : 0));
        },
        post(fn) {
            const now = clock.now(), f = frameAt(now);
            clock.setTimeout(fn, f ? f[1] - now : 0);
        },
        frameStarts(fn, timers) {
            timers.push(clock.setInterval(() => {
                const t = clock.now();
                if (t >= busyUntil && (!F || t < T0))
                    fn();
            }, 16));
            if (F)
                timers.push(clock.setTimeout(() => { fn(); timers.push(clock.setInterval(fn, period)); }, Math.max(0, T0 - clock.now())));
        },
        clock,
    };
    if (!F && !busyFor)
        return page;
    // Timers on the page wait for it as what comes in does.
    const live = new Map();
    let seq = 0;
    page.clock = {
        ...clock,
        setTimeout(fn, ms) {
            const id = ++seq;
            live.set(id, true);
            clock.setTimeout(() => page.deliver(() => { if (live.delete(id)) fn(); }), ms);
            return id;
        },
        clearTimeout(id) { live.delete(id); },
    };
    return page;
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
// takes (direct): the joiner's tab takes this many bytes a second off its map
// channel, at frames frameMs apart, and windowBytes of it may be on the way
// or not taken yet. slowFor: after this long into the download it takes all
// that came at each frame.
// hostFrameMs, hostSlowAt, hostEveryMs: the host's page is slow (see the top);
// hostBusyFor: it's busy that long first, as the lobby's or the relay server's
// first answers come.
// quietAt, quietFor (through the lobby or directly): the other player sends
// nothing from quietAt into the download for quietFor (its tab stalls), while
// the host's game goes on sending to it.
async function run({ mbit, K, mapBytes, path, paced, pong = true, limitMs = 900000, matchAt = 0,
                     takes = 0, frameMs = 16, windowBytes = 256 * 1024, slowFor = Infinity,
                     hostFrameMs = 0, hostSlowAt = 0, hostBusyFor = 0, hostEveryMs = 0, quietAt = Infinity, quietFor = 0 }) {
    const clock = makeClock();
    const page = hostPage(clock, hostFrameMs, hostSlowAt, hostBusyFor, hostEveryMs);
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
    // What the link told maps.js at each look in the transfer: a queue, none,
    // no telling.
    const looks = { yes: 0, no: 0, none: 0 };
    const requests = [];
    // lastIn: when the joiner had the last part that came.
    let blockBytes = 0, transferring = false, t0 = Infinity, lastIn = 0;
    const playing = () => clock.now() - t0 >= matchAt;
    const quiet = () => clock.now() - t0 >= quietAt && clock.now() - t0 < quietAt + quietFor;
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
    // The joiner's map channel when its tab is slow to take what comes: what
    // waits in the host's browser, what came and wasn't taken, and how much
    // of the window that and what's on the way take.
    const slowTab = () => {
        const waiting = [], came = [];
        let waitingBytes = 0, unread = 0, budget = 0;
        const flush = () => {
            while (waiting.length && unread + waiting[0].byteLength <= windowBytes) {
                const f = waiting.shift();
                waitingBytes -= f.byteLength;
                unread += f.byteLength;
                came.push([up.send(f.byteLength + FRAMING) + ONE_WAY, f]);
            }
        };
        timers.push(clock.setInterval(() => {
            budget = clock.now() - t0 >= slowFor ? Infinity : Math.min(0, budget) + takes * frameMs / 1000;
            while (came.length && came[0][0] <= clock.now() && budget > 0) {
                const f = came.shift()[1];
                budget -= f.byteLength;
                if (new Uint8Array(f)[12] === 6 && transferring)
                    lastIn = clock.now();
                join.jkMaps.onFrame(1, f, 'direct');
                // The window opens again as the joiner's ack comes back.
                clock.setTimeout(() => { unread -= f.byteLength; flush(); }, ONE_WAY);
            }
        }, frameMs));
        return {
            send(bytes) {
                if (new Uint8Array(bytes)[12] === 6)
                    blockBytes += bytes.byteLength - 40;
                waiting.push(bytes);
                waitingBytes += bytes.byteLength;
                flush();
            },
            bufferedAmount: () => waitingBytes,
        };
    };
    const gameSent = leaves => {
        if (transferring)
            waitsAt.push([clock.now() - t0, leaves - clock.now()]);
    };
    // A message the host's page posts to itself.
    class MessageChannel {
        constructor() {
            const port1 = { onmessage: null };
            this.port1 = port1;
            this.port2 = { postMessage: data => page.post(() => port1.onmessage && port1.onmessage({ data })) };
        }
    }
    const HEAPU8 = new Uint8Array(1 << 16);
    const env = (extra) => ({
        Module: host,
        HEAPU8, HEAPU16: new Uint16Array(1 << 15), HEAP32: new Int32Array(1 << 14),
        MessageChannel,
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
                        clock.setTimeout(() => page.deliver(() => this.onmessage({ data: 'pong' })), leaves - clock.now() + 2 * ONE_WAY);
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
        // net.peer makes them: the joiner's map channel is the queue above,
        // or waits for a slow tab.
        const slow = direct && takes ? slowTab() : null;
        const channel = (id, label) => ({
            label, readyState: 'connecting', binaryType: 'blob', close() {},
            get bufferedAmount() {
                return id === 2 && label === 'jk-map' ? up.visible() + (slow ? slow.bufferedAmount() : 0) : 0;
            },
            send(data) {
                const b = new Uint8Array(data.buffer || data, data.byteOffset || 0, data.byteLength);
                if (label === 'jk-map' && slow)
                    slow.send(b.slice().buffer);
                else if (label === 'jk-map')
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
        // Another player, through the lobby or directly: the host's game
        // sends to it (DirectPlay_Send), and hears from it.
        timers.push(clock.setInterval(() => {
            if (!playing())
                return;
            HEAPU8.fill(0, 0, GAME_BYTES);
            new DataView(HEAPU8.buffer).setUint32(4, 3, true);
            HEAPU8[8] = 10;
            js.wsrelay_js_send_packet(0, GAME_BYTES, 3, 0);
            if (quiet())
                return;
            const inbound = new Uint8Array(GAME_BYTES);
            new DataView(inbound.buffer).setUint32(0, 3, true);
            inbound[8] = 10;
            page.deliver(() => {
                if (direct)
                    net.peers[3].dc.onmessage({ data: inbound.buffer });
                else
                    ws.onmessage({ data: inbound.buffer });
                net.queue = [];
            });
        }, 1000 / GAME_HZ));
        // DirectPlay_Receive, at the start of each of the host's frames.
        engineFrame = () => {
            if (js.wsrelay_js_frame)
                js.wsrelay_js_frame();
            js.wsrelay_js_map_pump();
        };
        join.jkNet = {
            mapDirect: () => direct, rtcEnabled: () => direct,
            mapSend(id, bytes) {
                const copy = bytes.slice(0);
                new DataView(copy).setUint32(0, 2, true);   // the lobby stamps the sender
                if (new Uint8Array(copy)[12] === 5)
                    requests.push(new DataView(copy).getUint32(24, true));
                clock.setTimeout(() => page.deliver(() => direct ? net.peers[2].mc.onmessage({ data: copy }) : ws.onmessage({ data: copy })), 2 * ONE_WAY);
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
        // The page's note of the host's frames, which the Imperial pump makes
        // (stdComm_WSRelay_Frame).
        const frameJs = RELAY_JS.wsrelay_js_frame ? bindJs({ wsrelay_js_frame: RELAY_JS.wsrelay_js_frame }, env({})) : null;
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
        // jkrelay_AskTimed and jkrelay_TimedState: the page takes the answer
        // in (at, and it waits in the socket's queue), and the engine reads it
        // in its next frame, timed to then or (answerStamped) to at.
        const probe = { seq: 0, out: false, back: false, sentAt: 0, at: 0, rtt: 0, base: 0, got: 0 };
        const unread = host.jkrWs.socks[1].q;
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
                clock.setTimeout(() => page.deliver(() => {
                    if (probe.seq === seq) {
                        probe.back = true;
                        probe.at = clock.now();
                        const d = new Uint8Array(20);
                        d.at = probe.at;
                        unread.push(d);
                    }
                }), leaves - clock.now() + 2 * ONE_WAY);
        };
        if (paced)
            sendProbe();    // as the host's game opens
        // imperial_XferPump.
        const xferPump = () => {
            const now = clock.now();
            if (frameJs)
                frameJs.wsrelay_js_frame();
            if (paced && (js.imperial_js_xfer_serve(1, probe.got, probe.rtt, probe.base, probe.out ? now - probe.sentAt : -1, probe.at) ||
                          probe.got < timedFirst))
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
        // jkrelay_Receive's jkr_Pump: the engine reads what the page took in.
        const readSocket = () => {
            unread.length = 0;
            if (!probe.back)
                return;
            const rtt = (answerStamped ? probe.at : clock.now()) - probe.sentAt;
            probe.out = probe.back = false;
            probe.rtt = rtt;
            probe.base = probe.got ? Math.min(probe.base, rtt) : rtt;
            probe.got++;
            if (transferring)
                answers++;
        };
        // stdComm_Imperial_Receive pumps first each time it's called: in a
        // frame, the first comes before the engine reads the socket, so it
        // knows nothing yet of an answer the page took in since the last
        // frame; the next, once a game packet was read (one comes most frames
        // in a match), knows of it.
        engineFrame = () => {
            xferPump();
            readSocket();
            xferPump();
        };
        timers.push(clock.setInterval(() => gameSent(ws.send(GAME_BYTES + 32)), 1000 / GAME_HZ));
        join.jkImperial = {
            active: true, mapDirect: () => true,
            mapSend(id, bytes) {
                const copy = bytes.slice(0);
                if (new Uint8Array(copy)[12] === 5)
                    requests.push(new DataView(copy).getUint32(24, true));
                clock.setTimeout(() => page.deliver(() => host.jkMaps.onFrame(2, copy, 'direct')), 2 * ONE_WAY);
                return 'direct';
            },
        };
    }
    loadMaps(page.clock, paced ? newMaps : oldMaps, host);
    loadMaps(clock, oldMaps, join);
    page.frameStarts(engineFrame, timers);
    const link = host.jkNet || host.jkImperial;
    const ask = link.mapCongested;
    link.mapCongested = (...a) => {
        const c = ask(...a);
        if (transferring)
            looks[c === true ? 'yes' : c === false ? 'no' : 'none']++;
        return c;
    };

    // The host gets the map ready (hashing it takes real time, not the
    // model's) before anyone asks for it: a joiner that waited for that on
    // the model's clock could give up on a busy computer.
    const log = console.log;
    let ready = false;
    console.log = (...a) => String(a[0]).includes('ready to send map') ? (ready = true) : log(...a);
    try {
        host.jkMaps.host('ctf_pace', 'gob');
        for (let i = 0; !ready && i < 1500; i++)
            await new Promise(r => setTimeout(r, 20));
    } finally {
        console.log = log;
    }
    assert.ok(ready, 'the host never got the map ready');
    await clock.run(6000);
    t0 = clock.now();
    transferring = true;
    join.jkMaps.fetch('ctf_pace', 'gob');
    await clock.until(() => join.jkMaps.state() !== 0, limitMs, 200);
    transferring = false;
    // The joiner's checks after the last part take real time too, which a
    // busy computer can make longer than until() gives them.
    for (let i = 0; join.jkMaps.state() === 0 && lastIn > t0 && i < 1000; i++) {
        await clock.run(10);
        await new Promise(r => setTimeout(r, 10));
    }
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
        rate: paced ? host.jkMaps.uploads().rate : null, waitsAt, probesAt, answers, clear, looks,
        notes: ((host.jkNet && host.jkNet.log) || []).filter(l => / map upload/.test(l)),
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
        // The live host makes them wait seconds, so that alone misses a rate
        // that grows too fast past a slow upload.
        assert.ok(paced.max < 1000, `max wait ${paced.max} ms`);
        if (s.path === 'imperial')
            assert.ok(paced.clear <= paced.answers + 1, `no queue said ${paced.clear} times for ${paced.answers} answers`);
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
    // Each answer that shows no queue grows the rate once, not at each look
    // until the next answer 2 s on.
    assert.ok(paced.clear <= paced.answers + 1, `no queue said ${paced.clear} times for ${paced.answers} answers`);
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

for (const s of [
    { path: 'relay', mbit: 20, mapBytes: 8000000 },
    { path: 'direct', mbit: 20, mapBytes: 8000000 },
    { path: 'direct', mbit: 20, mapBytes: 25000000 },
    { path: 'direct', mbit: 5, mapBytes: 8000000 },
    { path: 'imperial', mbit: 20, mapBytes: 8000000 },
]) {
    const what = `${{ relay: 'through the lobby', direct: 'over the map channel', imperial: 'on the Imperial Network Protocol' }[s.path]} ` +
                 `at ${s.mbit} Mbit/s up, a ${s.mapBytes / 1e6} MB map`;
    // On the Imperial Network Protocol, three times: the rate grows a quarter
    // at each of the relay server's answers, which come 2 s apart.
    const most = s.path === 'imperial' ? 3 : 2;
    test(what + `: the rate soon uses a fast upload, and the map takes at most ${most === 2 ? 'twice' : 'three times'} as long as from the live host and 2 s`, async () => {
        const paced = await run({ ...s, K, paced: true });
        const old = await run({ ...s, K, paced: false });
        console.log('    paced     ' + describe(what, paced));
        console.log('    live host ' + describe(what, old));
        assert.ok(paced.ok && old.ok);
        assert.ok(paced.ratio < 1.02, 'sent ' + paced.ratio.toFixed(2) + 'x');
        assert.equal(paced.reasks, 0);
        assert.ok(paced.xfer < old.xfer * most + 2, `${paced.xfer.toFixed(1)} s, live host ${old.xfer.toFixed(1)} s`);
        assert.ok(paced.max < 500, 'game packets wait ' + paced.max + ' ms');
    });
}

test('on the Imperial Network Protocol at 3.5-4.5 Mbit/s up, and at 2 Mbit/s with the host\'s frames 30 a second: game packets wait under 600 ms', async () => {
    // The relay server answers every 2 s at most: a rate that doubled at
    // each answer went from just under such an upload to twice it, and what
    // it sent over waited in the computer and the router until the next
    // answer, 1-2 s.
    for (const s of [
        { mbit: 3.5, mapBytes: 3000000 },
        { mbit: 3.5, mapBytes: 8000000 },
        { mbit: 4, mapBytes: 8000000 },
        { mbit: 4.5, mapBytes: 8000000 },
        { mbit: 2, mapBytes: 8000000, hostFrameMs: 3, hostEveryMs: 33, hostSlowAt: 1000 },
    ]) {
        const what = `Imperial, ${s.mbit} Mbit/s up, ${s.mapBytes / 1e6} MB` + (s.hostFrameMs ? ', frames 30 a second' : '');
        const r = await run({ ...s, path: 'imperial', K, paced: true });
        console.log('    ' + describe(what, r));
        assert.ok(r.ok);
        assert.ok(r.ratio < 1.02, 'sent ' + r.ratio.toFixed(2) + 'x');
        assert.equal(r.reasks, 0);
        assert.ok(r.max < 600, `${what}: game packets wait ${r.max} ms`);
    }
});

test('over the map channel, a joiner whose tab is slow to take the map gets it at its own pace, the rate isn\'t held down for that, and the other player\'s packets don\'t wait', async () => {
    // The joiner's tab takes 300 KB/s, on a 20 Mbit/s upload: what waits for
    // it fills the map channel in the host's browser, not the upload.
    const s = { path: 'direct', mbit: 20, mapBytes: 8000000, takes: 300000 };
    const slow = await run({ ...s, K, paced: true });
    // The same tab, which takes all that comes 10 s in: by then it has 3 MB,
    // and the other 5 MB at the most rate, 2 MB/s, take 2.5 s.
    const then = await run({ ...s, K, paced: true, slowFor: 10000 });
    console.log('    ' + describe('a tab that takes 300 KB/s', slow));
    console.log('    ' + describe('the same, then fast 10 s in', then));
    for (const r of [slow, then]) {
        assert.ok(r.ok);
        assert.ok(r.ratio < 1.02, 'sent ' + r.ratio.toFixed(2) + 'x');
        assert.equal(r.reasks, 0);
        assert.ok(r.max < 100, 'game packets wait ' + r.max + ' ms');
    }
    assert.ok(slow.xfer < s.mapBytes / s.takes + 2, `${slow.xfer.toFixed(1)} s`);
    assert.ok(slow.rate >= 1000000, 'rate ' + slow.rate);
    assert.ok(then.xfer < 14, `${then.xfer.toFixed(1)} s`);
});

test('on the Imperial Network Protocol each answer with no queue counts once, and a queue at every look until the next', async () => {
    const clock = makeClock();
    const host = {};
    const ws = { bufferedAmount: 0 };
    host.jkrWs = { socks: { 1: { ws, out: [] } } };
    const js = bindJs(IMPERIAL_JS, { Module: host, performance: clock.performance, HEAPU8: new Uint8Array(16), HEAP32: new Int32Array(4) });
    js.imperial_js_xfer_init(1);
    const m = host.jkImperial;
    const serve = (got, rtt, waited) => js.imperial_js_xfer_serve(1, got, rtt, 50, waited, clock.now());
    const looks = n => Array.from({ length: n }, () => m.mapCongested());
    serve(0, 0, -1);
    assert.deepEqual(looks(2), [null, null]);
    serve(1, 50, -1);
    assert.deepEqual(looks(3), [false, null, null]);
    // The next request, out 100 ms, then 140 ms (base + 80 is 130).
    serve(1, 50, 100);
    assert.deepEqual(looks(2), [null, null]);
    serve(1, 50, 140);
    assert.deepEqual(looks(3), [true, true, true]);
    serve(2, 60, -1);
    assert.deepEqual(looks(3), [false, null, null]);
    // A late answer stands until the next.
    serve(3, 200, -1);
    assert.deepEqual(looks(3), [true, true, true]);
    serve(4, 50, -1);
    // What waits in the socket counts at once, an answer once it doesn't.
    ws.bufferedAmount = 16384;
    assert.deepEqual(looks(2), [true, true]);
    ws.bufferedAmount = 0;
    assert.deepEqual(looks(2), [false, null]);
    // A new connection counts its answers from the first again.
    serve(0, 0, -1);
    serve(1, 50, -1);
    assert.deepEqual(looks(2), [false, null]);
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

test('the network log says what the paced rate is and why, at most every few seconds', async () => {
    const r = await run({ path: 'relay', mbit: 2, mapBytes: 8000000, K, paced: true });
    console.log('    ' + r.notes.length + ' lines in ' + r.xfer.toFixed(0) + ' s, the first and the last:');
    console.log('      ' + r.notes[0]);
    console.log('      ' + r.notes[r.notes.length - 1]);
    assert.ok(r.ok);
    assert.ok(r.notes.length >= Math.floor(r.xfer / 10), r.notes.length + ' lines');
    assert.ok(r.notes.length <= r.xfer / 2 + 1, r.notes.length + ' lines');
    assert.ok(!/ \(was /.test(r.notes[0]), r.notes[0]);
    for (const l of r.notes)
        assert.match(l, /^[\d:.]+ map upload paced at \d+ KB\/s( \(was \d+\))?; the link since the last line: \d+ a queue( \(.*\))?, \d+ no queue( \(.*\))?, \d+ no telling( \(.*\))?; \d+ KB waiting for \d+ joiner\(s\), \d+ KB on the way$/);
    assert.ok(r.notes.some(l => /: [1-9]\d* a queue \(lobby round trip \d+ ms, at least \d+ of it on the way \(shortest \d+\)/.test(l)), 'never said why');
});

// The host's page slow (a busy computer, a weak one): the lobby's and the
// relay server's answers wait for its frames before the page takes them, as
// a queue's would. Frames of 400 ms from 1 s on, after the first answers.
const SLOW = { hostFrameMs: 400, hostSlowAt: 1000 };
const nameSlow = s => `${{ relay: 'through the lobby', direct: 'over the map channel', imperial: 'on the Imperial Network Protocol' }[s.path]} ` +
                      `at ${s.mbit} Mbit/s up, a ${s.mapBytes / 1e6} MB map`;

for (const s of [
    { path: 'relay', mbit: 20, mapBytes: 8000000 },
    { path: 'direct', mbit: 20, mapBytes: 25000000 },
    { path: 'imperial', mbit: 20, mapBytes: 8000000 },
]) {
    const what = nameSlow(s) + ', the host\'s frames 400 ms';
    test(what + ': answers late for the page aren\'t taken for a queue, and the map comes about as soon as from the live host', async () => {
        const paced = await run({ ...s, ...SLOW, K, paced: true });
        const old = await run({ ...s, ...SLOW, K, paced: false });
        console.log('    paced     ' + describe(what, paced) + `; a queue said at ${paced.looks.yes} of ${paced.looks.yes + paced.looks.no + paced.looks.none} looks`);
        console.log('    live host ' + describe(what, old));
        assert.ok(paced.ok);
        assert.ok(paced.ratio < 1.02, 'sent ' + paced.ratio.toFixed(2) + 'x');
        assert.equal(paced.reasks, 0);
        assert.equal(paced.looks.yes, 0, 'a queue said at ' + paced.looks.yes + ' looks');
        // At 2.5 frames a second the Imperial pump sends 64 KiB a frame at
        // most, about 160 KB/s, however fast the upload.
        assert.ok(paced.xfer < old.xfer * 2.5 + 2, `${paced.xfer.toFixed(1)} s, live host ${old.xfer.toFixed(1)} s`);
        assert.ok(paced.max < 1000, 'game packets wait ' + paced.max + ' ms');
    });
}

for (const s of [
    { path: 'relay', mbit: 2, mapBytes: 8000000 },
    { path: 'direct', mbit: 5, mapBytes: 8000000 },
]) {
    const what = nameSlow(s) + ', the host\'s frames 400 ms';
    test(what + ': a queue on the upload is still seen, and game packets wait less than with the live host', async () => {
        const paced = await run({ ...s, ...SLOW, K, paced: true });
        const old = await run({ ...s, ...SLOW, K, paced: false });
        console.log('    paced     ' + describe(what, paced) + `; a queue said at ${paced.looks.yes} of ${paced.looks.yes + paced.looks.no + paced.looks.none} looks`);
        console.log('    live host ' + describe(what, old));
        assert.ok(paced.ok);
        assert.ok(paced.ratio < 1.02, 'sent ' + paced.ratio.toFixed(2) + 'x');
        assert.equal(paced.reasks, 0);
        assert.ok(paced.looks.yes > 0, 'a queue never said');
        assert.ok(paced.xfer < old.xfer * 2 + 2, `${paced.xfer.toFixed(1)} s, live host ${old.xfer.toFixed(1)} s`);
        assert.ok(paced.p95 < old.p95, `p95 wait ${paced.p95} ms, live host ${old.p95} ms`);
        if (s.path === 'relay')
            assert.ok(paced.p95 * 2 < old.p95 && paced.max < old.max, `p95 wait ${paced.p95} ms, live host ${old.p95} ms`);
    });
}

test('over the map channel at 20 Mbit/s up, a 25 MB map, the host\'s frames 400 ms, with only 64 KiB of the channel\'s held where the page can\'t see it: a frame sends what the rate allows', async () => {
    // What the computer and the router hold of a data channel's may be
    // little (its window), and the rest waits in the browser: no more than
    // 64 KiB of that a pump held the download to about 2.5 times that a
    // second (72 s), however high the rate.
    const s = { path: 'direct', mbit: 20, mapBytes: 25000000, ...SLOW };
    const paced = await run({ ...s, K: 64 * 1024, paced: true });
    const old = await run({ ...s, K: 64 * 1024, paced: false });
    console.log('    paced     ' + describe(nameSlow(s), paced));
    console.log('    live host ' + describe(nameSlow(s), old));
    assert.ok(paced.ok);
    assert.ok(paced.ratio < 1.02, 'sent ' + paced.ratio.toFixed(2) + 'x');
    assert.equal(paced.reasks, 0);
    assert.ok(paced.xfer < old.xfer * 2.5 + 2, `${paced.xfer.toFixed(1)} s, live host ${old.xfer.toFixed(1)} s`);
    assert.ok(paced.max <= old.max, `game packets wait ${paced.max} ms, live host ${old.max} ms`);
});

test('on the Imperial Network Protocol at 2 Mbit/s up, the host\'s frames 400 ms: the map comes sooner than from the live host, sent once', async () => {
    const s = { path: 'imperial', mbit: 2, mapBytes: 8000000, ...SLOW };
    const paced = await run({ ...s, K, paced: true });
    const old = await run({ ...s, K, paced: false });
    console.log('    paced     ' + describe(nameSlow(s), paced));
    console.log('    live host ' + describe(nameSlow(s), old));
    assert.ok(paced.ok);
    assert.ok(paced.ratio < 1.02, 'sent ' + paced.ratio.toFixed(2) + 'x');
    assert.equal(paced.reasks, 0);
    assert.ok(paced.xfer < old.xfer, `${paced.xfer.toFixed(1)} s, live host ${old.xfer.toFixed(1)} s`);
    assert.ok(paced.max < 1000, 'game packets wait ' + paced.max + ' ms');
});

test('through the lobby at 2 Mbit/s up, the host\'s frames 16 and 33 ms: the queue seen as at full speed', async () => {
    const s = { path: 'relay', mbit: 2, mapBytes: 8000000 };
    const fast = await run({ ...s, K, paced: true });
    console.log('    ' + describe('full speed', fast));
    for (const [F, slowAt] of [[16, 0], [33, 1000]]) {
        const r = await run({ ...s, K, paced: true, hostFrameMs: F, hostSlowAt: slowAt });
        console.log('    ' + describe(`frames of ${F} ms`, r));
        assert.ok(r.ok);
        assert.ok(r.ratio < 1.02, 'sent ' + r.ratio.toFixed(2) + 'x');
        assert.equal(r.reasks, 0);
        assert.ok(r.looks.yes > 0);
        assert.ok(r.xfer < fast.xfer * 1.25, `${r.xfer.toFixed(1)} s, ${fast.xfer.toFixed(1)} s at full speed`);
        assert.ok(r.p95 < fast.p95 * 2, `p95 wait ${r.p95} ms, ${fast.p95} ms at full speed`);
        assert.ok(r.max < 1000, 'game packets wait ' + r.max + ' ms');
    }
});

test('through the lobby, the host\'s frames short but far apart (a 30 Hz display, the FPS limit): the queue seen as at full speed', async () => {
    // Answers that came between frames were read at once: none of it is
    // taken off as the page's (judged by the time between frames' ends,
    // 60-100 ms was at 30 frames a second, and game packets waited up to
    // 669 ms at 5 Mbit/s, 1264 ms at 2 Mbit/s and 10 frames a second).
    for (const [mbit, F, every] of [[5, 3, 33], [2, 5, 100]]) {
        const s = { path: 'relay', mbit, mapBytes: 8000000 };
        const fast = await run({ ...s, K, paced: true });
        const r = await run({ ...s, K, paced: true, hostFrameMs: F, hostEveryMs: every, hostSlowAt: 1000 });
        console.log('    ' + describe(`${mbit} Mbit/s, full speed`, fast));
        console.log('    ' + describe(`${mbit} Mbit/s, frames of ${F} ms every ${every} ms`, r));
        assert.ok(r.ok);
        assert.ok(r.ratio < 1.02, 'sent ' + r.ratio.toFixed(2) + 'x');
        assert.equal(r.reasks, 0);
        assert.ok(r.looks.yes > 0);
        assert.ok(r.p95 < fast.p95 * 1.5, `p95 wait ${r.p95} ms, ${fast.p95} ms at full speed`);
        assert.ok(r.max < 500, 'game packets wait ' + r.max + ' ms');
    }
});

test('through the lobby at 2 Mbit/s up, the host\'s page busy 3 s as the first answers come: the shortest round trip isn\'t taken from them', async () => {
    const s = { path: 'relay', mbit: 2, mapBytes: 8000000 };
    const fast = await run({ ...s, K, paced: true });
    const busy = await run({ ...s, K, paced: true, hostBusyFor: 3000 });
    console.log('    ' + describe('busy 3 s first', busy));
    assert.ok(busy.ok);
    assert.ok(busy.looks.yes > 0);
    assert.ok(busy.p95 < fast.p95 * 1.5, `p95 wait ${busy.p95} ms, ${fast.p95} ms`);
    assert.ok(busy.xfer < fast.xfer * 1.25, `${busy.xfer.toFixed(1)} s, ${fast.xfer.toFixed(1)} s`);
});

// The other player's tab stalls 10 s, 10 s into the download: it sends
// nothing, the host's game goes on sending to it. The upload stays paced,
// and the game packets to it then wait no longer than before.
const QUIET = { quietAt: 10000, quietFor: 10000 };
const quietMax = r => Math.round(Math.max(...r.waitsAt.filter(([t]) => t >= QUIET.quietAt && t < QUIET.quietAt + QUIET.quietFor + 5000).map(([, w]) => w)));
for (const s of [
    { path: 'relay', mbit: 1, mapBytes: 8000000 },
    { path: 'relay', mbit: 2, mapBytes: 8000000 },
    { path: 'direct', mbit: 5, mapBytes: 8000000 },
]) {
    const what = nameSlow(s) + ', the other player silent 10 s';
    test(what + ': still a match, so the upload stays paced', async () => {
        const paced = await run({ ...s, ...QUIET, K, paced: true });
        const old = await run({ ...s, ...QUIET, K, paced: false, limitMs: 300000 });
        console.log('    paced     ' + describe(what, paced) + `; max wait while silent and 5 s on ${quietMax(paced)} ms`);
        console.log('    live host ' + describe(what, old) + `; max wait while silent and 5 s on ${quietMax(old)} ms`);
        assert.ok(paced.ok);
        assert.ok(paced.ratio < 1.02, 'sent ' + paced.ratio.toFixed(2) + 'x');
        assert.equal(paced.reasks, 0);
        assert.ok(quietMax(paced) < 1000, 'game packets wait ' + quietMax(paced) + ' ms while silent');
        assert.ok(quietMax(paced) * 4 < quietMax(old), `${quietMax(paced)} ms, live host ${quietMax(old)} ms`);
        assert.ok(paced.max < 1000, 'game packets wait ' + paced.max + ' ms');
    });
}

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
