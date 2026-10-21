// Network backend for the browser build: a WebSocket relay for the lobby and
// signaling, and WebRTC data channels for game packets.
//
// The browser can't open UDP sockets, so every player keeps one WebSocket to a
// relay (jkdf2mp-bb-server/relay). The relay lists games, lets players join
// them and passes WebRTC signaling between a host and its clients. Once a
// client's data channel to the host is open, game packets go over it. The
// channel is reliable and ordered, like GNS: over a lossy channel sithComm's
// own resends could land after newer state (a death after the respawn) and
// replay it. Until then, or if WebRTC can't connect, packets go through the
// relay instead.
//
// The game side mirrors the GNS backend: a star with the host as id 1, clients
// get ids 2..64, and every packet carries an 8-byte idFrom/idTo header. A
// client sends everything to the host, and the host forwards packets meant for
// another client, which is what GNSServer::Receive does. Through the relay,
// packets go straight to the player named in idTo (the host for 0 or 1).
//
// Text frames are relay control lines, binary frames are game packets:
//   us -> relay   list <game> | host <entry> | update <entry> | join <room>
//                 welcome <id> <entry> | leave | signal <id> <data> | ping
//                 hello <game> <name> | rejoin <room> <id> <key>
//                 rehost <room> <key> <entry>
//   relay -> us   list (<room> <players> <entry>)* | hosted <room>
//                 peer-joined <id> | peer-left <id> | welcome <id> <entry>
//                 host-left | error <code> | signal <id> <data> | ice <data>
//                 seat <room> <id> <key> | rejoined <room> <id> | rehosted <room>
//                 peer-lost <id> | peer-back <id> | host-lost | host-back
// list asks for the games this game can join: those hosted by the same game
// (jk1 or mots), and any whose host hasn't said which it runs. <entry> is a
// hex-encoded jkMultiEntry. <data> is base64 JSON: an SDP offer or answer
// ({"d": ...}) or an ICE candidate ({"c": ...}) for signal, and the
// RTCPeerConnection iceServers list for ice. The JS below handles signal and
// ice itself; the engine never sees them.
//
// Game packets don't need the relay once the data channel is open, so losing
// the socket mid-game doesn't end the match. The relay holds a seat whose
// socket closed without leave for a while and tells the room (peer-lost,
// host-lost); the JS reconnects and takes the seat back with the key the relay
// gave it (seat), and the room hears peer-back or host-back. A seat nobody
// takes back ends as before, with peer-left or host-left. A player only counts
// as gone once the relay says so and its data channel is down too, or once
// sithMulti's own timeout finds it silent.
//
// For the landing page's live counts, the JS also tells the relay this
// player's name (base64 UTF-8) and game (jk1 or mots) with hello, from the
// start rather than once the player opens the Join screen, and keeps the
// socket open outside a game so they stay counted.
//
// A joiner who doesn't have the host's map gets it from the host before the
// level loads (packaging/wasm/maps.js, bundled into this module). Its frames
// travel like game packets, over a second data channel or the relay, and the
// JS hands them to maps.js before the engine's queue.
//
// A host running a mod (-path) adds a tail to its <entry> naming it
// (packaging/wasm/mods.js). A joiner on another mod, or none, gets the
// host's the same way, then the engine restarts into it and the player joins
// again with that game already selected.

#include "stdComm_WSRelay.h"

#include "Win95/stdComm.h"
#include "Dss/sithMulti.h"
#include "Main/Main.h"
#include "General/stdString.h"
#include "stdPlatform.h"
#include "jk.h"

#include <emscripten.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define WSRELAY_MAX_RECV (32 * 1024)
#define WSRELAY_MAX_PACKET (4096)
#define WSRELAY_CONNECT_MS (5000)
#define WSRELAY_REPLY_MS (5000)
#define WSRELAY_LIST_MS (500)
#define WSRELAY_MAX_LISTED (32)
// The mod tail after an entry, in hex: "JKMD", version, name length, a
// 32-byte fingerprint and a name of up to 63 characters (mods.js).
#define WSRELAY_MAX_TAIL_HEX (256)

// How long a joiner with no data channel to the host waits for a lost relay
// to come back before it gives up on the host.
#define WSRELAY_LOBBY_GRACE_MS (20000)

#define WSRELAY_STATE_CONNECTING (0)
#define WSRELAY_STATE_OPEN (1)
#define WSRELAY_STATE_CLOSED (2)

#define wsrelay_verbosePrintf(fmt, ...) if (Main_bVerboseNetworking) \
    { \
        stdPlatform_Printf(fmt, ##__VA_ARGS__);  \
    } \
    ;

extern int Main_bVerboseNetworking;

// The socket, the peer connections and the inbound queue live in JS. The
// engine polls the queue, so nothing calls back into the wasm module while
// Asyncify has it unwound.
EM_JS_DEPS(stdComm_WSRelay, "$UTF8ToString,$stringToUTF8,$lengthBytesUTF8");

EM_JS(void, wsrelay_js_connect, (), {
    var net = Module.jkNet;
    if (!net) {
        net = Module.jkNet = { state: 2, queue: [], ws: null, peers: {}, isHost: false, ice: null,
                               stats: { dcSent: 0, dcRecv: 0, wsSent: 0, wsRecv: 0, wsSkipped: 0 },
                               log: [], lastFrom: {}, seat: null, entry: "", rejoinMs: 1000, relayPosAt: {} };

        /* The network log the page saves ("Save network log" in index.html):
           what happened to the relay and each data channel, the engine's own
           "OpenJKDF2:" lines (the page's print adds those), and every 10 s in
           a game how each link is doing. Only the last 1000 lines are kept. */
        net.note = function(text) {
            net.log.push(new Date().toISOString().substring(11, 23) + ' ' + text);
            if (net.log.length > 1000) net.log.splice(0, net.log.length - 1000);
        };

        net.event = function(text) {
            net.note(text);
            console.log('OpenJKDF2: ' + text);
        };

        /* One line per player we've heard from: the data channel's state, how
           much is waiting in it, and how long since anything came from them,
           then the packets sent and received each way since the last line. */
        net.health = function() {
            var now = performance.now(), s = net.stats, was = net.healthStats || s;
            var ids = Object.keys(net.peers).concat(Object.keys(net.lastFrom))
                .filter(function(id, i, all) { return all.indexOf(id) === i; });
            var line = 'link check: relay ' + ['connecting', 'open', 'closed'][net.state];
            ids.forEach(function(id) {
                var p = net.peers[id], heard = net.lastFrom[id];
                line += '; player ' + id + ' ' + (p ? (p.open ? 'direct' : 'relay') + ' (' + p.pc.connectionState +
                    ', channel ' + p.dc.readyState + ', ' + p.dc.bufferedAmount + ' bytes waiting)' : 'relay') +
                    ', heard ' + (heard ? ((now - heard) / 1000).toFixed(1) + ' s ago' : 'never');
            });
            line += '; sent ' + (s.dcSent - was.dcSent) + ' direct, ' + (s.wsSent - was.wsSent) + ' relay (' +
                    (s.wsSkipped - was.wsSkipped) + ' positions skipped); got ' +
                    (s.dcRecv - was.dcRecv) + ' direct, ' + (s.wsRecv - was.wsRecv) + ' relay';
            net.healthStats = { dcSent: s.dcSent, dcRecv: s.dcRecv, wsSent: s.wsSent, wsRecv: s.wsRecv, wsSkipped: s.wsSkipped };
            return line;
        };
        setInterval(function() { if (net.session) net.note(net.health()); }, 10000);

        /* A tab closed in a game gives up its seat at once (a socket that
           just closes keeps it for a while) and closes its data channels, so
           the others don't wait on it. */
        if (typeof window !== 'undefined') window.addEventListener('pagehide', function() {
            if (!net.session) return;
            if (net.state === 1) try { net.ws.send('leave'); } catch (e) {}
            net.closeAll();
        });

        net.rtcEnabled = function() {
            return Module['jkUseWebRTC'] !== false && typeof RTCPeerConnection !== 'undefined';
        };

        net.signal = function(id, obj) {
            if (net.state === 1) net.ws.send('signal ' + id + ' ' + btoa(JSON.stringify(obj)));
        };

        net.peer = function(id) {
            var p = net.peers[id];
            if (p) return p;
            var pc = new RTCPeerConnection({ iceServers: net.ice || Module['jkIceServers'] || [] });
            var dc = pc.createDataChannel('jk', { negotiated: true, id: 0 });
            dc.binaryType = 'arraybuffer';
            // Map downloads get their own channel, so one never holds up game
            // packets.
            var mc = pc.createDataChannel('jk-map', { negotiated: true, id: 1 });
            mc.binaryType = 'arraybuffer';
            p = net.peers[id] = { pc: pc, dc: dc, mc: mc, open: false };
            var update = function() {
                if (net.peers[id] !== p) return;
                var was = p.open;
                var cs = pc.connectionState;
                p.open = dc.readyState === 'open' && cs !== 'disconnected' && cs !== 'failed' && cs !== 'closed';
                if (p.open && !was) net.event('direct WebRTC link to player ' + id + ' is up');
                if (!p.open && was) net.event('direct link to player ' + id + ' is down (' + cs + ', channel ' + dc.readyState + '), using the relay');
            };
            dc.onopen = update;
            dc.onclose = update;
            pc.onconnectionstatechange = update;
            dc.onmessage = function(ev) { if (net.peers[id] === p) net.onPeerPacket(id, ev.data); };
            mc.onmessage = function(ev) { if (net.peers[id] === p && Module.jkMaps) Module.jkMaps.onFrame(id, ev.data, 'direct'); };
            pc.onicecandidate = function(ev) { if (ev.candidate && net.peers[id] === p) net.signal(id, { c: ev.candidate.toJSON() }); };
            return p;
        };

        net.closePeer = function(id) {
            var p = net.peers[id];
            if (!p) return;
            delete net.peers[id];
            p.open = false;
            try { p.dc.close(); } catch (e) {}
            try { p.mc.close(); } catch (e) {}
            try { p.pc.close(); } catch (e) {}
        };

        net.closeAll = function() {
            for (var id in net.peers) net.closePeer(id);
        };

        net.offer = function(id) {
            var p = net.peer(id);
            p.pc.createOffer()
                .then(function(o) { return p.pc.setLocalDescription(o); })
                .then(function() { if (net.peers[id] === p) net.signal(id, { d: p.pc.localDescription.toJSON() }); })
                .catch(function(e) { console.warn('OpenJKDF2: WebRTC offer to player ' + id + ' failed, using the relay: ' + e); });
        };

        net.onSignal = function(from, obj) {
            if (!net.rtcEnabled()) return;
            var p;
            if (obj.d && obj.d.type === 'offer') {
                if (!net.isHost) return;
                net.closePeer(from);
                p = net.peer(from);
                p.pc.setRemoteDescription(obj.d)
                    .then(function() { return p.pc.createAnswer(); })
                    .then(function(a) { return p.pc.setLocalDescription(a); })
                    .then(function() { if (net.peers[from] === p) net.signal(from, { d: p.pc.localDescription.toJSON() }); })
                    .catch(function(e) { console.warn('OpenJKDF2: WebRTC answer to player ' + from + ' failed, using the relay: ' + e); });
            } else if (obj.d) {
                p = net.peers[from];
                if (p) p.pc.setRemoteDescription(obj.d).catch(function(e) { console.warn('OpenJKDF2: WebRTC answer from player ' + from + ' rejected: ' + e); });
            } else if (obj.c) {
                p = net.peers[from];
                if (p) p.pc.addIceCandidate(obj.c).catch(function() {});
            }
        };

        /* Packets from a data channel. The host knows who is on the other end,
           so it stamps idFrom itself and forwards anything meant for another
           client. */
        net.onPeerPacket = function(id, data) {
            if (!(data instanceof ArrayBuffer) || data.byteLength < 8) return;
            net.stats.dcRecv++;
            net.lastFrom[id] = performance.now();
            if (net.isHost) {
                var dv = new DataView(data);
                dv.setUint32(0, id, true);
                var to = dv.getUint32(4, true);
                if (to > 1) {
                    net.sendPacket(new Uint8Array(data), to);
                    return;
                }
            }
            net.queue.push(data);
        };

        net.sendPacket = function(bytes, id) {
            var p = net.peers[id];
            if (p && p.open) {
                try {
                    p.dc.send(bytes);
                    net.stats.dcSent++;
                    return 1;
                } catch (e) {}
            }
            if (net.state !== 1) return 0;
            if (net.skipRelayedPos(bytes)) {
                net.stats.wsSkipped++;
                return 1;
            }
            net.ws.send(bytes);
            net.stats.wsSent++;
            return 1;
        };

        /* Below a 100 ms tick rate every player sends its position each
           physics step it's due, about 50 a second at the default 16 ms.
           Through the relay each one counts against the lobby's daily limit,
           so a player's position goes through it to each player at most every
           200 ms, as at the original 180 ms, and the others are skipped. The
           next one is never more than a tick away and replaces it. Only
           those: DSS_THINGPOS (1) sent without resends (msgId 0), 52 or 64
           bytes long, which only a player's is (sithDSSThing_Pos adds the
           head pitch for players). Direct links get them all. */
        net.skipRelayedPos = function(bytes) {
            var n = bytes.byteLength;
            if (n !== 8 + 4 + 52 && n !== 8 + 4 + 64) return false;
            var dv = new DataView(bytes.buffer, bytes.byteOffset, n);
            if (dv.getUint16(8, true) !== 1 || dv.getUint16(10, true) !== 0) return false;
            var key = dv.getUint32(4, true) + ' ' + dv.getInt32(12, true);
            var now = performance.now(), last = net.relayPosAt[key];
            if (last !== undefined && now - last < 200) return true;
            net.relayPosAt[key] = now;
            return false;
        };

        /* Map download frames: over the map channel unless via is 'relay' or
           the channel isn't open, else through the relay. Returns the way it
           went, or null. */
        net.mapSend = function(id, bytes, via) {
            var p = net.peers[id];
            if (via !== 'relay' && net.mapDirect(id)) {
                try {
                    p.mc.send(bytes);
                    return 'direct';
                } catch (e) {}
            }
            if (net.state !== 1) return null;
            net.ws.send(bytes);
            return 'relay';
        };

        net.mapDirect = function(id) {
            var p = net.peers[id];
            return !!(p && p.open && p.mc.readyState === 'open');
        };

        /* Returns true for the lines JS handles itself. */
        net.onText = function(text) {
            var sp = text.indexOf(' ');
            var op = sp < 0 ? text : text.substring(0, sp);
            if (op === 'ice') {
                try { net.ice = JSON.parse(atob(text.substring(sp + 1))); } catch (e) {}
                return true;
            }
            if (op === 'signal') {
                var parts = text.split(' ');
                var obj = null;
                try { obj = JSON.parse(atob(parts[2])); } catch (e) {}
                if (obj) net.onSignal(parseInt(parts[1], 10), obj);
                return true;
            }
            /* The seat to take back if the socket drops in a game. */
            if (op === 'seat') {
                var seat = text.split(' ');
                net.seat = { room: parseInt(seat[1], 10), id: parseInt(seat[2], 10), key: seat[3] || "" };
                return true;
            }
            /* A player whose data channel is still up is still playing: the
               engine decides when they're gone (DirectPlay_Receive). */
            var gone = op === 'peer-left' ? net.peers[parseInt(text.substring(sp + 1), 10)] : net.peers[1];
            if ((op === 'peer-left' || op === 'host-left') && !(gone && gone.open)) {
                if (op === 'peer-left') net.closePeer(parseInt(text.substring(sp + 1), 10));
                else net.closeAll();
            }
            return false;
        };
        /* The live counts: this player's name and game, sent when the socket
           opens and whenever they change. Module.jkPresence says where the
           engine keeps them. */
        net.sayHello = function() {
            var who = Module.jkPresence;
            if (!who || net.state !== 1) return;
            var chars = [];
            for (var i = 0; i < 31; i++) {
                var ch = HEAPU16[(who.name >> 1) + i];
                if (!ch) break;
                chars.push(ch);
            }
            var name = String.fromCharCode.apply(null, chars);
            if (!name.trim()) return;
            var bytes = new TextEncoder().encode(name);
            var line = 'hello ' + (HEAP32[who.mots >> 2] ? 'mots' : 'jk1') + ' ' + btoa(String.fromCharCode.apply(null, bytes));
            if (line === net.helloSent) return;
            net.helloSent = line;
            net.ws.send(line);
        };

        /* Outside a game, reconnects a lost socket after a while (the lobby
           restarts when it's updated), so the player stays counted. In a game,
           see rejoinLater. */
        net.retryMs = 2000 + Math.random() * 4000;
        net.keepPresence = function() {
            if (!Module.jkPresence || net.session || net.retry || net.state !== 2) return;
            net.retry = setTimeout(function() {
                net.retry = null;
                if (!net.session) net.connect();
            }, net.retryMs);
            net.retryMs = Math.min(net.retryMs * 2, 120000);
        };

        /* In a game, reconnects a lost socket soon, then takes this player's
           seat back (ws.onopen). The match goes on meanwhile over the data
           channels; the engine gives up on the host only if there's none. */
        net.rejoinLater = function() {
            if (net.retry) return;
            net.retry = setTimeout(function() {
                net.retry = null;
                if (net.session && net.state === 2) net.connect(true);
            }, net.rejoinMs);
            net.rejoinMs = Math.min(net.rejoinMs * 2, 5000);
        };

        /* keepQueue: in a game, packets already in from the data channels
           still go to the engine. */
        net.connect = function(keepQueue) {
            if (net.ws && net.state !== 2) return;
            clearTimeout(net.retry);
            net.retry = null;
            if (net.ws) {
                try { net.ws.close(); } catch (e) {}
            }
            var url = Module['jkRelayUrl'];
            if (!url && typeof location !== 'undefined')
                url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/relay';
            if (!keepQueue) net.queue = [];
            net.state = 0;
            var ws;
            try {
                ws = new WebSocket(url);
            } catch (e) {
                console.error('OpenJKDF2: relay ' + url + ': ' + e);
                net.state = 2;
                return;
            }
            ws.binaryType = 'arraybuffer';
            var keepalive = null;
            ws.onopen = function() {
                net.state = 1;
                net.retryMs = 2000 + Math.random() * 4000;
                console.log('OpenJKDF2: connected to relay ' + url);
                net.helloSent = null;
                net.sayHello();
                /* With game packets on WebRTC, the socket can sit idle for a whole
                   match, and proxies drop idle WebSockets. */
                keepalive = setInterval(function() { if (ws.readyState === 1) ws.send('ping'); }, 25000);
                net.rejoinMs = 1000;
                var seat = net.session && net.seat;
                if (seat) {
                    ws.send(seat.id === 1 ? 'rehost ' + seat.room + ' ' + seat.key + (net.entry ? ' ' + net.entry : "")
                                          : 'rejoin ' + seat.room + ' ' + seat.id + ' ' + seat.key);
                    net.event('relay is back, taking back seat ' + seat.id + ' in room ' + seat.room);
                } else if (net.session) {
                    net.event('relay is back, but it never gave this game a seat to take back');
                }
            };
            ws.onclose = function(ev) {
                if (keepalive) clearInterval(keepalive);
                if (net.ws !== ws) return;
                net.state = 2;
                if (net.session) {
                    net.event('lost the relay (code ' + ev.code + (ev.reason ? ', ' + ev.reason : "") +
                              '), trying again in ' + Math.round(net.rejoinMs / 1000) + ' s');
                    net.rejoinLater();
                }
                net.keepPresence();
            };
            ws.onerror = function() {
                console.error('OpenJKDF2: relay connection error (' + url + ')');
                // Browsers follow up with onclose, but don't wait on it to give up.
                if (net.ws === ws && net.state === 0) net.state = 2;
            };
            ws.onmessage = function(ev) {
                if (net.ws !== ws) return;
                if (typeof ev.data === 'string') {
                    if (net.onText(ev.data)) return;
                } else if (Module.jkMaps && Module.jkMaps.isFrame(ev.data)) {
                    Module.jkMaps.onFrame(new DataView(ev.data).getUint32(0, true), ev.data, 'relay');
                    return;
                } else {
                    net.stats.wsRecv++;
                    if (ev.data.byteLength >= 8) net.lastFrom[new DataView(ev.data).getUint32(0, true)] = performance.now();
                }
                net.queue.push(ev.data);
            };
            net.ws = ws;
        };
    }

    net.connect();
});

EM_JS(int, wsrelay_js_state, (), {
    var net = Module.jkNet;
    return net ? net.state : 2;
});

EM_JS(int, wsrelay_js_send_text, (const char* pText), {
    var net = Module.jkNet;
    if (!net) return 0;
    var text = UTF8ToString(pText);
    var sp = text.indexOf(' ');
    var op = sp < 0 ? text : text.substring(0, sp);
    // A host's entry goes with rehost; hosting, joining or leaving ends the
    // seat this game had.
    if ((op === 'host' || op === 'update') && sp > 0) net.entry = text.substring(sp + 1);
    if (op === 'host' || op === 'join' || op === 'leave') net.seat = null;
    if (net.state !== 1) return 0;
    net.ws.send(text);
    return 1;
});

// Sends a game packet over the data channel to player routeId if it's open,
// else through the relay.
EM_JS(int, wsrelay_js_send_packet, (const void* pData, int len, int routeId), {
    var net = Module.jkNet;
    if (!net) return 0;
    return net.sendPacket(HEAPU8.slice(pData, pData + len), routeId);
});

// Starts a session's WebRTC side: a client offers a data channel to the host,
// and a host answers the offers its clients send.
EM_JS(void, wsrelay_js_rtc_begin, (int isHost), {
    var net = Module.jkNet;
    if (!net) return;
    net.closeAll();
    net.session = true;
    net.isHost = !!isHost;
    net.lastFrom = {};
    net.relayPosAt = {};
    net.healthStats = null;
    net.note((isHost ? 'hosting' : 'joined') + ' room ' + (net.seat ? net.seat.room + ' as player ' + net.seat.id : '(no seat from the relay)'));
    if (!isHost && net.rtcEnabled()) net.offer(1);
});

EM_JS(void, wsrelay_js_rtc_end, (), {
    var net = Module.jkNet;
    if (!net) return;
    net.closeAll();
    if (net.session) net.note('left the game');
    net.session = false;
    net.isHost = false;
    net.seat = null;
    net.rejoinMs = 1000;
    clearTimeout(net.retry);
    net.retry = null;
    net.keepPresence();
});

// Whether the data channel to player id is up, so game packets reach them
// without the relay.
EM_JS(int, wsrelay_js_direct_open, (int id), {
    var net = Module.jkNet;
    var p = net && net.peers[id];
    return p && p.open ? 1 : 0;
});

// Where the engine keeps the player's name and whether it runs MotS, for the
// live counts.
EM_JS(void, wsrelay_js_presence, (const char16_t* pName, const int32_t* pMots), {
    // Catches a new name: the player picks theirs after the engine starts.
    if (!Module.jkPresence)
        setInterval(function() { if (Module.jkNet) Module.jkNet.sayHello(); }, 2000);
    Module.jkPresence = { name: pName, mots: pMots };
});

// The room the landing page's Join button picked, to select on the join
// screen once it's listed. Only once: a restart into its mod keeps it itself.
EM_JS(int, wsrelay_js_take_join_room, (), {
    var room = Number(Module['jkJoinRoom']);
    delete Module['jkJoinRoom'];
    return room > 0 && room < 1e9 ? Math.floor(room) : -1;
});

#define WSRELAY_MAP_BUSY (0)
#define WSRELAY_MAP_READY (1)
#define WSRELAY_MAP_FAILED (2)

// Starts getting the host's episode GOB, if this player doesn't have it.
EM_JS(void, wsrelay_js_map_fetch, (const char* pName, const char* pExt), {
    if (Module.jkMaps) Module.jkMaps.fetch(UTF8ToString(pName), UTF8ToString(pExt));
});

EM_JS(int, wsrelay_js_map_state, (), {
    return Module.jkMaps ? Module.jkMaps.state() : 1;
});

EM_JS(void, wsrelay_js_map_abort, (const char* pReason), {
    if (Module.jkMaps) Module.jkMaps.abort(UTF8ToString(pReason));
});

// Gets our episode ready to send to joiners who don't have it.
EM_JS(void, wsrelay_js_map_host, (const char* pName, const char* pExt), {
    if (Module.jkMaps) Module.jkMaps.host(UTF8ToString(pName), UTF8ToString(pExt));
});

EM_JS(void, wsrelay_js_map_unhost, (), {
    if (Module.jkMaps) Module.jkMaps.unhost();
});

// Same states as the map, and one more: the engine has to restart into the
// host's mod (or out of ours).
#define WSRELAY_MOD_RESTART (3)

// The lobby tail for the mod this tab runs, "" for none.
EM_JS(void, wsrelay_js_mod_tail, (char* pOut, int maxLen), {
    stringToUTF8(Module.jkMods ? Module.jkMods.tail() : "", pOut, maxLen);
});

// Starts getting this tab onto the host's mod, from its lobby tail.
EM_JS(void, wsrelay_js_mod_fetch, (const char* pTail), {
    if (Module.jkMods) Module.jkMods.fetch(UTF8ToString(pTail));
});

EM_JS(int, wsrelay_js_mod_state, (), {
    return Module.jkMods ? Module.jkMods.state() : 1;
});

EM_JS(void, wsrelay_js_mod_abort, (const char* pReason), {
    if (Module.jkMods) Module.jkMods.abort(UTF8ToString(pReason));
});

// The path to restart into ("" for no mod), which becomes the running mod.
EM_JS(void, wsrelay_js_mod_restart, (char* pOut, int maxLen), {
    stringToUTF8(Module.jkMods ? Module.jkMods.restart() : "", pOut, maxLen);
});

EM_JS(void, wsrelay_js_mod_host, (), {
    if (Module.jkMods) Module.jkMods.host();
});

EM_JS(void, wsrelay_js_mod_unhost, (), {
    if (Module.jkMods) Module.jkMods.unhost();
});

// A line about a listed game's mod for the join screen.
EM_JS(void, wsrelay_js_mod_describe, (const char* pTail, char* pOut, int maxLen), {
    stringToUTF8(Module.jkMods ? Module.jkMods.describe(UTF8ToString(pTail)) : "", pOut, maxLen);
});

// Pops one queued message into pOut. Returns its length, or -1 if the queue is
// empty. Text is NUL-terminated (maxLen includes the terminator).
EM_JS(int, wsrelay_js_recv, (void* pOut, int maxLen, int* pIsText), {
    var net = Module.jkNet;
    if (!net || !net.queue.length) return -1;
    var data = net.queue.shift();
    if (typeof data === 'string') {
        stringToUTF8(data, pOut, maxLen);
        HEAP32[pIsText >> 2] = 1;
        return Math.min(lengthBytesUTF8(data), maxLen - 1);
    }
    var bytes = new Uint8Array(data);
    var len = Math.min(bytes.length, maxLen);
    HEAPU8.set(bytes.subarray(0, len), pOut);
    HEAP32[pIsText >> 2] = 0;
    return len;
});

static uint8_t wsrelay_recvBuf[WSRELAY_MAX_RECV];
static uint8_t wsrelay_sendBuf[WSRELAY_MAX_PACKET];
static char wsrelay_textBuf[1024];

static int wsrelay_bHosting = 0;      // our room is open on the relay
static int wsrelay_bHostPending = 0;  // sent "host", waiting for "hosted"
static int wsrelay_bJoining = 0;      // sent "join", waiting for "welcome"
static int wsrelay_bJoined = 0;       // the host welcomed us into its room
static int wsrelay_bHostLeft = 0;      // the relay says the host left
static int wsrelay_bSeatLost = 0;      // the relay wouldn't give our seat back
static uint32_t wsrelay_msecRelayLost = 0; // when the relay went in a game, 0 while it's up
static int wsrelay_joinError = 0;
static int wsrelay_room = -1;
static uint32_t wsrelay_id = 0xFFFFFFFF;

static int wsrelay_aDisconnected[64];
static int wsrelay_numDisconnected = 0;

// A host's players the relay says left while their data channel is still up.
// They count as gone once it goes down (or sithMulti times them out).
static int wsrelay_aLeftPending[64];
static int wsrelay_numLeftPending = 0;

static jkMultiEntry wsrelay_storedEntry;      // the game we host
static jkMultiEntry wsrelay_storedEntryEnum;  // the game we joined
static char wsrelay_hostTail[WSRELAY_MAX_TAIL_HEX + 1];      // our mod, as we host
static char wsrelay_storedTailEnum[WSRELAY_MAX_TAIL_HEX + 1]; // the joined game's mod

// The room to select on the join screen after restarting into its mod. Not an
// engine global, so it lasts through the restart.
static int wsrelay_pendingRoom = -1;

// The relay's latest game list, and the rooms behind the entries the join
// screen is showing right now (jkGuiMultiplayer_aEntries).
static int wsrelay_bListReceived = 0;
static int wsrelay_numListed = 0;
static int wsrelay_aListRooms[WSRELAY_MAX_LISTED];
static jkMultiEntry wsrelay_aListEntries[WSRELAY_MAX_LISTED];
static char wsrelay_aListTails[WSRELAY_MAX_LISTED][WSRELAY_MAX_TAIL_HEX + 1];
static int wsrelay_numShown = 0;
static int wsrelay_aShownRooms[WSRELAY_MAX_LISTED];
static char wsrelay_aShownTails[WSRELAY_MAX_LISTED][WSRELAY_MAX_TAIL_HEX + 1];

static void wsrelay_ToHex(const void* pData, int len, char* pOut)
{
    static const char digits[] = "0123456789abcdef";
    const uint8_t* pBytes = (const uint8_t*)pData;
    for (int i = 0; i < len; i++)
    {
        pOut[i*2] = digits[pBytes[i] >> 4];
        pOut[i*2+1] = digits[pBytes[i] & 0xF];
    }
    pOut[len*2] = 0;
}

static int wsrelay_HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes the first len bytes of pHex.
static int wsrelay_DecodeHex(const char* pHex, void* pOut, int len)
{
    uint8_t* pBytes = (uint8_t*)pOut;
    for (int i = 0; i < len; i++)
    {
        int hi = wsrelay_HexNibble(pHex[i*2]);
        int lo = wsrelay_HexNibble(pHex[i*2+1]);
        if (hi < 0 || lo < 0)
            return 0;
        pBytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

// An entry, then the hex of a mod tail if the host runs a mod, which is
// copied to pTail as it is (mods.js reads it). Anything else is malformed.
static int wsrelay_EntryFromHex(const char* pHex, jkMultiEntry* pOut, char* pTail)
{
    const size_t entryHex = sizeof(jkMultiEntry)*2;
    if (!pHex)
        return 0;
    size_t len = strlen(pHex);
    if (len < entryHex || len > entryHex + WSRELAY_MAX_TAIL_HEX || (len - entryHex) % 2)
        return 0;
    for (size_t i = entryHex; i < len; i++)
    {
        if (wsrelay_HexNibble(pHex[i]) < 0)
            return 0;
    }
    if (!wsrelay_DecodeHex(pHex, pOut, sizeof(jkMultiEntry)))
        return 0;
    memcpy(pTail, pHex + entryHex, len - entryHex);
    pTail[len - entryHex] = 0;
    return 1;
}

static int wsrelay_SendText(const char* pText)
{
    wsrelay_verbosePrintf("Relay << %.80s\n", pText);
    return wsrelay_js_send_text(pText);
}

// Only a host sends entries, each followed by its mod tail.
static void wsrelay_SendEntry(const char* pOp, int id, const jkMultiEntry* pEntry)
{
    char hex[sizeof(jkMultiEntry)*2 + 1];
    wsrelay_ToHex(pEntry, sizeof(jkMultiEntry), hex);
    if (id >= 0)
        snprintf(wsrelay_textBuf, sizeof(wsrelay_textBuf), "%s %d %s%s", pOp, id, hex, wsrelay_hostTail);
    else
        snprintf(wsrelay_textBuf, sizeof(wsrelay_textBuf), "%s %s%s", pOp, hex, wsrelay_hostTail);
    wsrelay_SendText(wsrelay_textBuf);
}

static void wsrelay_ResetSession()
{
    wsrelay_bHosting = 0;
    wsrelay_bHostPending = 0;
    wsrelay_bJoining = 0;
    wsrelay_bJoined = 0;
    wsrelay_bHostLeft = 0;
    wsrelay_bSeatLost = 0;
    wsrelay_msecRelayLost = 0;
    wsrelay_joinError = 0;
    wsrelay_room = -1;
    wsrelay_id = 0xFFFFFFFF;
    wsrelay_numDisconnected = 0;
    wsrelay_numLeftPending = 0;
    wsrelay_js_rtc_end();
    wsrelay_js_map_unhost();
    wsrelay_js_mod_unhost();
}

// Same count GNSServer::RealConnectedPlayers reports to joining clients.
static int wsrelay_RealConnectedPlayers()
{
    int amt = 0;
    for (int i = 0; i < jkPlayer_maxPlayers; i++)
    {
        if (!i && jkGuiNetHost_bIsDedicated) continue;

        if ( (jkPlayer_playerInfos[i].flags & 2) != 0 && !jkPlayer_playerInfos[i].playerNetId ) {
        }
        else {
            amt++;
        }
    }
    return amt;
}

static void wsrelay_UpdateLobby()
{
    if (!wsrelay_bHosting) return;

    wsrelay_storedEntry.multiModeFlags = sithMulti_multiModeFlags;
    wsrelay_SendEntry("update", -1, &wsrelay_storedEntry);
}

// What GNSServer does when a connection comes in: give the client its id and
// the session description.
static void wsrelay_WelcomePeer(int id)
{
    wsrelay_verbosePrintf("Assigning ID: %x\n", id);

    wsrelay_storedEntry.multiModeFlags = sithMulti_multiModeFlags;

    jkMultiEntry entry = wsrelay_storedEntry;
    entry.numPlayers = wsrelay_RealConnectedPlayers();
    entry.maxPlayers = wsrelay_storedEntry.maxPlayers;

    jkPlayer_maxPlayers = wsrelay_storedEntry.maxPlayers; // Hack?

    wsrelay_SendEntry("welcome", id, &entry);
}

static void wsrelay_PeerLeft(int id)
{
    stdPlatform_Printf("OpenJKDF2: Client id %x has left.\n", id);

    for (int i = 0; i < wsrelay_numLeftPending; )
    {
        if (wsrelay_aLeftPending[i] == id)
            wsrelay_aLeftPending[i] = wsrelay_aLeftPending[--wsrelay_numLeftPending];
        else
            i++;
    }

    // Only report players who finished joining, like GNSServer.
    for (int i = 0; i < jkPlayer_maxPlayers; i++)
    {
        if (!i && jkGuiNetHost_bIsDedicated) continue;

        if ( (jkPlayer_playerInfos[i].flags & 2) != 0 && jkPlayer_playerInfos[i].playerNetId == id) {
            if (wsrelay_numDisconnected < 64)
                wsrelay_aDisconnected[wsrelay_numDisconnected++] = id;
            break;
        }
    }
}

// The relay says player id left. With their data channel still up they're
// still playing (their socket to the relay dropped, or an older relay ended
// their seat with it), so that waits until the channel goes down.
static void wsrelay_PeerLeftOnRelay(int id)
{
    if (!wsrelay_js_direct_open(id))
    {
        wsrelay_PeerLeft(id);
        return;
    }

    for (int i = 0; i < wsrelay_numLeftPending; i++)
    {
        if (wsrelay_aLeftPending[i] == id)
            return;
    }
    if (wsrelay_numLeftPending < 64)
        wsrelay_aLeftPending[wsrelay_numLeftPending++] = id;
    stdPlatform_Printf("OpenJKDF2: The relay says client id %x left, but its direct link is up. Keeping it.\n", id);
}

// Pending players whose data channel went down have left.
static void wsrelay_CheckLeftPending()
{
    for (int i = 0; i < wsrelay_numLeftPending; )
    {
        int id = wsrelay_aLeftPending[i];
        if (wsrelay_js_direct_open(id))
            i++;
        else
            wsrelay_PeerLeft(id); // takes it off the list
    }
}

static void wsrelay_HandleList(char* pArgs)
{
    char* pSave = NULL;
    int n = 0;

    for (char* pTok = strtok_r(pArgs, " ", &pSave); pTok; pTok = strtok_r(NULL, " ", &pSave))
    {
        char* pPlayers = strtok_r(NULL, " ", &pSave);
        char* pHex = strtok_r(NULL, " ", &pSave);
        if (!pPlayers || !pHex)
            break;
        if (n >= WSRELAY_MAX_LISTED)
            break;

        jkMultiEntry* pEntry = &wsrelay_aListEntries[n];
        if (!wsrelay_EntryFromHex(pHex, pEntry, wsrelay_aListTails[n]))
            continue;

        int room = atoi(pTok);
        wsrelay_aListRooms[n] = room;
        pEntry->numPlayers = atoi(pPlayers);

        // The join screen keeps its selection by guidInstance, so each room
        // needs its own.
        memset(&pEntry->guidInstance, 0, sizeof(pEntry->guidInstance));
        pEntry->guidInstance.a = 0x4A4B5753; // "JKWS"
        pEntry->guidInstance.b = (uint32_t)room;
        n++;
    }

    wsrelay_numListed = n;
    wsrelay_bListReceived = 1;
}

static void wsrelay_HandleText(char* pText)
{
    wsrelay_verbosePrintf("Relay >> %.80s\n", pText);

    char* pSave = NULL;
    char* pOp = strtok_r(pText, " ", &pSave);
    if (!pOp)
        return;

    if (!strcmp(pOp, "list"))
    {
        wsrelay_HandleList(pSave ? pSave : (char*)"");
    }
    else if (!strcmp(pOp, "hosted"))
    {
        char* pRoom = strtok_r(NULL, " ", &pSave);
        if (wsrelay_bHostPending && pRoom)
        {
            wsrelay_room = atoi(pRoom);
            wsrelay_bHostPending = 0;
            wsrelay_bHosting = 1;
            wsrelay_js_rtc_begin(1);
            stdPlatform_Printf("OpenJKDF2: Hosting room %d on the relay\n", wsrelay_room);
        }
    }
    else if (!strcmp(pOp, "peer-joined"))
    {
        char* pId = strtok_r(NULL, " ", &pSave);
        if (wsrelay_bHosting && pId)
        {
            // The relay gave this id to someone new, so whoever had it is gone.
            for (int i = 0; i < wsrelay_numLeftPending; i++)
            {
                if (wsrelay_aLeftPending[i] == atoi(pId))
                {
                    wsrelay_PeerLeft(atoi(pId));
                    break;
                }
            }
            wsrelay_WelcomePeer(atoi(pId));
            wsrelay_UpdateLobby();
        }
    }
    else if (!strcmp(pOp, "peer-left"))
    {
        char* pId = strtok_r(NULL, " ", &pSave);
        if (wsrelay_bHosting && pId)
        {
            wsrelay_PeerLeftOnRelay(atoi(pId));
            wsrelay_UpdateLobby();
        }
    }
    else if (!strcmp(pOp, "peer-lost") || !strcmp(pOp, "peer-back"))
    {
        char* pId = strtok_r(NULL, " ", &pSave);
        if (wsrelay_bHosting && pId)
        {
            if (!strcmp(pOp, "peer-lost"))
                stdPlatform_Printf("OpenJKDF2: Client id %x lost the relay; its seat is held (direct link %s).\n", atoi(pId), wsrelay_js_direct_open(atoi(pId)) ? "up" : "down");
            else
                stdPlatform_Printf("OpenJKDF2: Client id %x is back on the relay.\n", atoi(pId));
        }
    }
    else if (!strcmp(pOp, "host-lost") || !strcmp(pOp, "host-back"))
    {
        if (wsrelay_bJoined)
        {
            if (!strcmp(pOp, "host-lost"))
                stdPlatform_Printf("OpenJKDF2: The host lost the relay; its seat is held (direct link %s).\n", wsrelay_js_direct_open(1) ? "up" : "down");
            else
            {
                stdPlatform_Printf("OpenJKDF2: The host is back on the relay.\n");
                wsrelay_bHostLeft = 0;
            }
        }
    }
    else if (!strcmp(pOp, "rejoined") || !strcmp(pOp, "rehosted"))
    {
        if (wsrelay_bJoined || wsrelay_bHosting)
        {
            wsrelay_bSeatLost = 0;
            stdPlatform_Printf("OpenJKDF2: Back in room %d on the relay.\n", wsrelay_room);
        }
    }
    else if (!strcmp(pOp, "welcome"))
    {
        char* pId = strtok_r(NULL, " ", &pSave);
        char* pHex = strtok_r(NULL, " ", &pSave);
        jkMultiEntry entry;
        char tail[WSRELAY_MAX_TAIL_HEX + 1];
        if (wsrelay_bJoining && pId && wsrelay_EntryFromHex(pHex, &entry, tail))
        {
            // Same as GNSClient receiving its GNSInfoPacket.
            wsrelay_id = (uint32_t)atoi(pId);
            wsrelay_verbosePrintf("We are ID %x\n", wsrelay_id);

            wsrelay_storedEntryEnum = entry;
            wsrelay_storedEntryEnum.field_E0 = 10;
            strcpy(wsrelay_storedTailEnum, tail);

            // Hack?
            sithMulti_multiModeFlags = wsrelay_storedEntryEnum.multiModeFlags;
            sithNet_MultiModeFlags = wsrelay_storedEntryEnum.multiModeFlags;

            wsrelay_bJoining = 0;
            wsrelay_bJoined = 1;
        }
    }
    else if (!strcmp(pOp, "host-left"))
    {
        if (wsrelay_bJoined)
        {
            stdPlatform_Printf("OpenJKDF2: Host has disconnected%s.\n", wsrelay_js_direct_open(1) ? ", says the relay, but its direct link is up" : "");
            wsrelay_bHostLeft = 1;
        }
    }
    else if (!strcmp(pOp, "error"))
    {
        char* pCode = strtok_r(NULL, " ", &pSave);
        stdPlatform_Printf("OpenJKDF2: Relay error: %s\n", pCode ? pCode : "?");
        if (wsrelay_bJoining)
            wsrelay_joinError = (pCode && !strcmp(pCode, "full")) ? 2 : 1;
        if (wsrelay_bHostPending)
            wsrelay_bHostPending = 0;
        // Our seat wasn't held (or not by this relay), so it can't reach
        // the room for us any more.
        if ((wsrelay_bJoined || wsrelay_bHosting) && pCode && !strcmp(pCode, "no-seat"))
            wsrelay_bSeatLost = 1;
    }
}

// Handles queued control lines. Game packets only mean something inside a
// session, which DirectPlay_Receive handles, so outside one they're dropped.
static void wsrelay_PumpControl()
{
    int isText = 0;
    int len;
    while ((len = wsrelay_js_recv(wsrelay_recvBuf, sizeof(wsrelay_recvBuf), &isText)) >= 0)
    {
        if (isText)
            wsrelay_HandleText((char*)wsrelay_recvBuf);
    }
}

// Lets the browser run (Asyncify) until *pFlag changes from its current value,
// the socket closes, or timeoutMs passes. Returns 1 if *pFlag changed.
static int wsrelay_WaitForChange(int* pFlag, int timeoutMs)
{
    int start = *pFlag;
    uint32_t until = stdPlatform_GetTimeMsec() + timeoutMs;
    while (stdPlatform_GetTimeMsec() < until)
    {
        wsrelay_PumpControl();
        if (*pFlag != start)
            return 1;
        if (wsrelay_js_state() != WSRELAY_STATE_OPEN)
            return 0;
        emscripten_sleep(10);
    }
    return 0;
}

// Makes sure the host's episode is installed, getting it from the host if it's
// missing or another version. Returns 0 if it couldn't be; maps.js has told
// the player why.
static int wsrelay_GetHostMap(const char* pEpisode)
{
    char name[32];
    stdString_SafeStrCopy(name, pEpisode, sizeof(name));
    wsrelay_js_map_fetch(name, JKRES_GOB_EXT);

    int state;
    while ((state = wsrelay_js_map_state()) == WSRELAY_MAP_BUSY)
    {
        wsrelay_PumpControl();
        if (wsrelay_bHostLeft)
            wsrelay_js_map_abort("host-left");
        else if (wsrelay_js_state() != WSRELAY_STATE_OPEN)
            wsrelay_js_map_abort("lobby-lost");
        else
            emscripten_sleep(10);
    }
    return state == WSRELAY_MAP_READY;
}

// Gets this tab onto the host's mod, or off ours if the host runs none.
// Returns WSRELAY_MAP_READY if it already is, WSRELAY_MOD_RESTART if the
// engine has to restart for it, or WSRELAY_MAP_FAILED; mods.js has told the
// player why.
static int wsrelay_GetHostMod()
{
    wsrelay_js_mod_fetch(wsrelay_storedTailEnum);

    int state;
    while ((state = wsrelay_js_mod_state()) == WSRELAY_MAP_BUSY)
    {
        wsrelay_PumpControl();
        if (wsrelay_bHostLeft)
            wsrelay_js_mod_abort("host-left");
        else if (wsrelay_js_state() != WSRELAY_STATE_OPEN)
            wsrelay_js_mod_abort("lobby-lost");
        else
            emscripten_sleep(10);
    }
    return state;
}

static int wsrelay_EnsureOpen(int timeoutMs)
{
    if (wsrelay_js_state() == WSRELAY_STATE_OPEN)
        return 1;

    wsrelay_js_connect();
    uint32_t until = stdPlatform_GetTimeMsec() + timeoutMs;
    while (stdPlatform_GetTimeMsec() < until)
    {
        int state = wsrelay_js_state();
        if (state == WSRELAY_STATE_OPEN)
            return 1;
        if (state == WSRELAY_STATE_CLOSED)
            return 0;
        emscripten_sleep(10);
    }
    return 0;
}

void Hack_ResetClients()
{
    DirectPlay_numPlayers = 32;
    for (int i = 0; i < 32; i++)
    {
        DirectPlay_aPlayers[i].dpId = i+1;
        jk_snwprintf(DirectPlay_aPlayers[i].waName, 32, u"asdf");
    }

    if (stdComm_bIsServer)
        stdComm_dplayIdSelf = 1;
    else
        stdComm_dplayIdSelf = wsrelay_id;
}

void stdComm_WSRelay_Startup()
{
    jkGuiMultiplayer_numConnections = 1;
    jk_snwprintf(jkGuiMultiplayer_aConnections[0].name, 0x80, u"OpenJKDF2 Relay");
    stdComm_dword_8321E0 = 0;

    memset(jkGuiMultiplayer_aEntries, 0, sizeof(jkMultiEntry) * 32);
    dplay_dword_55D618 = 0;

    wsrelay_ResetSession();
    Hack_ResetClients();

    int room = wsrelay_js_take_join_room();
    if (room > 0)
        wsrelay_pendingRoom = room;

    // Counted in the landing page's live counts from the start, not only once
    // the player opens the Join screen.
    wsrelay_js_presence(jkPlayer_playerShortName, &Main_bMotsCompat);
    wsrelay_js_connect();
}

void stdComm_WSRelay_Shutdown()
{
}

// In a game, notes when the relay goes and when it's back. The JS reconnects
// it and takes our seat back; game packets keep going over the data channels.
static void wsrelay_WatchRelay()
{
    if (!wsrelay_bJoined && !wsrelay_bHosting)
        return;

    if (wsrelay_js_state() == WSRELAY_STATE_OPEN)
    {
        if (wsrelay_msecRelayLost)
            stdPlatform_Printf("OpenJKDF2: The relay is back after %u ms.\n", stdPlatform_GetTimeMsec() - wsrelay_msecRelayLost);
        wsrelay_msecRelayLost = 0;
        return;
    }
    if (!wsrelay_msecRelayLost)
    {
        wsrelay_msecRelayLost = stdPlatform_GetTimeMsec();
        if (!wsrelay_msecRelayLost)
            wsrelay_msecRelayLost = 1;
        if (stdComm_bIsServer)
            stdPlatform_Printf("OpenJKDF2: Lost contact with the relay. Still hosting, reconnecting.\n");
        else if (wsrelay_js_direct_open(1))
            stdPlatform_Printf("OpenJKDF2: Lost contact with the relay. Still playing over the direct link to the host, reconnecting.\n");
        else
            stdPlatform_Printf("OpenJKDF2: Lost contact with the relay, and there's no direct link to the host. Reconnecting.\n");
    }
}

// Why a joiner should give up on the host now, or NULL to keep playing. While
// the data channel to the host is up, the host is still there whatever the
// relay says; sithMulti_CheckPlayers still ends a match whose host goes quiet.
static const char* wsrelay_HostGoneReason()
{
    if (stdComm_bIsServer || !wsrelay_bJoined || wsrelay_js_direct_open(1))
        return NULL;
    if (wsrelay_bHostLeft)
        return "the relay says the host left";
    if (wsrelay_bSeatLost)
        return "the relay wouldn't take this player back into the room, and there's no direct link to the host";
    if (wsrelay_msecRelayLost && stdPlatform_GetTimeMsec() - wsrelay_msecRelayLost >= WSRELAY_LOBBY_GRACE_MS)
        return "lost contact with the relay, and there's no direct link to the host";
    return NULL;
}

int DirectPlay_Receive(int *pIdOut, int *pMsgIdOut, int *pLenOut)
{
    int maxLen = *pLenOut;
    *pIdOut = 0;
    *pLenOut = 0;

    Hack_ResetClients();

    wsrelay_WatchRelay();
    if (stdComm_bIsServer)
        wsrelay_CheckLeftPending();

    while (1)
    {
        if (stdComm_bIsServer && wsrelay_numDisconnected)
        {
            *pIdOut = wsrelay_aDisconnected[0];
            wsrelay_numDisconnected--;
            memmove(&wsrelay_aDisconnected[0], &wsrelay_aDisconnected[1], wsrelay_numDisconnected * sizeof(int));
            return 2;
        }

        const char* pHostGone = wsrelay_HostGoneReason();
        if (pHostGone)
        {
            // index.html offers the network log on this line.
            stdPlatform_Printf("OpenJKDF2: Leaving the game: %s.\n", pHostGone);
            wsrelay_bHostLeft = 0;
            wsrelay_bJoined = 0;
            *pIdOut = 1;
            return 2;
        }

        int isText = 0;
        int len = wsrelay_js_recv(wsrelay_recvBuf, sizeof(wsrelay_recvBuf), &isText);
        if (len < 0)
            break;

        if (isText)
        {
            wsrelay_HandleText((char*)wsrelay_recvBuf);
            continue;
        }

        if (!wsrelay_bHosting && !wsrelay_bJoined)
            continue;

        if (len < 8) {
            stdPlatform_Printf("Bad packet size %x\n", len);
            continue;
        }

        uint32_t idFrom = *(uint32_t*)&wsrelay_recvBuf[0];
        uint32_t idTo = *(uint32_t*)&wsrelay_recvBuf[4];

        // Not intended for us
        if (idTo && idTo != (stdComm_bIsServer ? 1 : wsrelay_id))
            continue;

        int outsize = maxLen;
        if (outsize > len - 8)
            outsize = len - 8;

        memcpy(pMsgIdOut, &wsrelay_recvBuf[8], outsize);
        *pIdOut = idFrom;
        *pLenOut = outsize;

        wsrelay_verbosePrintf("Recv %x bytes from %x %x (%s, %x)\n", outsize, idFrom, idTo, sithDSS_IdToStr(*(uint32_t*)pMsgIdOut & 0xFF), *(uint32_t*)pMsgIdOut);
        return 0;
    }

    return -1;
}

BOOL DirectPlay_Send(DPID idFrom, DPID idTo, void *lpData, DWORD dwDataSize)
{
    Hack_ResetClients();

    if (!wsrelay_bHosting && !wsrelay_bJoined)
        return stdComm_bIsServer ? 1 : 0;

    // A host's sends to everyone include itself (slot 0, see
    // stdComm_SendToPlayer). It has no link to itself, so these went to the
    // relay, which dropped them but counted each one against the lobby's
    // daily request limit.
    if (stdComm_bIsServer && (idTo == 0 || idTo == 1))
        return 1;

    if (dwDataSize > WSRELAY_MAX_PACKET-8) dwDataSize = WSRELAY_MAX_PACKET-8;
    *(uint32_t*)&wsrelay_sendBuf[0] = idFrom;
    *(uint32_t*)&wsrelay_sendBuf[4] = idTo;
    memcpy(&wsrelay_sendBuf[8], lpData, dwDataSize);

    wsrelay_verbosePrintf("Sent %x bytes to %x (%s, %x)\n", dwDataSize+8, idTo, sithDSS_IdToStr(*(uint32_t*)lpData & 0xFF), *(uint32_t*)lpData);

    // Clients only have a data channel to the host, which forwards for them.
    int ret = wsrelay_js_send_packet(wsrelay_sendBuf, dwDataSize+8, stdComm_bIsServer ? idTo : 1);

    // A host reports success even with nobody to send to, like GNSServer::Send.
    if (stdComm_bIsServer)
        return 1;
    return ret;
}

int stdComm_OpenConnection(void* a)
{
    stdComm_dword_8321DC = 1;
    return 0;
}

void stdComm_CloseConnection()
{
    if ( stdComm_dword_8321DC )
    {
        if ( stdComm_dword_8321E0 )
        {
            DirectPlay_Close();
            stdComm_dword_8321E0 = 0;
            stdComm_bIsServer = 0;
            stdComm_dplayIdSelf = 0;
        }
        stdComm_dword_8321DC = 0;
    }
}

int stdComm_Open(int idx, char16_t* pwPassword)
{
    DirectPlay_EnumSessions2();

    stdComm_dword_8321E8 = 0;
    stdComm_dword_8321E0 = 1;
    stdComm_dplayIdSelf = DirectPlay_CreatePlayer(jkPlayer_playerShortName, 0);
    stdComm_bIsServer = 0;

    if (idx < 0 || idx >= wsrelay_numShown)
        return 0x80004005;

    jkGuiMultiplayer_checksumSeed = jkGuiMultiplayer_aEntries[idx].checksumSeed;

    if (!wsrelay_EnsureOpen(WSRELAY_CONNECT_MS))
        return 0x80004005;

    wsrelay_ResetSession();
    wsrelay_bJoining = 1;
    snprintf(wsrelay_textBuf, sizeof(wsrelay_textBuf), "join %d", wsrelay_aShownRooms[idx]);
    wsrelay_SendText(wsrelay_textBuf);

    uint32_t until = stdPlatform_GetTimeMsec() + WSRELAY_REPLY_MS;
    while (!wsrelay_bJoined && !wsrelay_joinError && stdPlatform_GetTimeMsec() < until)
    {
        wsrelay_PumpControl();
        if (wsrelay_js_state() != WSRELAY_STATE_OPEN)
            break;
        emscripten_sleep(10);
    }

    if (!wsrelay_bJoined)
    {
        int err = wsrelay_joinError;
        wsrelay_SendText("leave");
        wsrelay_ResetSession();
        return (err == 2) ? 0x8877014A : 0x80004005; // GUINET_GAMEFULL / GUINET_NOGAMECONNECT
    }

    wsrelay_room = wsrelay_aShownRooms[idx];
    wsrelay_js_rtc_begin(0);

    // The mod first: on another one, the engine restarts before any map
    // matters. The direct link both can come over is starting up now.
    int modState = wsrelay_GetHostMod();
    if (modState == WSRELAY_MOD_RESTART)
    {
        char path[sizeof(openjkdf2_aRestartPath)];
        wsrelay_js_mod_restart(path, sizeof(path));
        stdPlatform_Printf("OpenJKDF2: Restarting with mod path \"%s\" to join room %d\n", path, wsrelay_room);
        wsrelay_pendingRoom = wsrelay_room;
        wsrelay_SendText("leave");
        wsrelay_ResetSession();

        // As the Expansions & Mods menu does (jkGUIMods.c). The menus return
        // to the main loop without drawing anything else, which then exits
        // and main() starts the engine again on this path.
        stdString_SafeStrCopy(openjkdf2_aRestartPath, path, sizeof(openjkdf2_aRestartPath));
        openjkdf2_restartMode = OPENJKDF2_RESTART_PATH;
        g_should_exit = 1;
        return STDCOMM_WSRELAY_RESTART;
    }
    if (modState != WSRELAY_MAP_READY)
    {
        wsrelay_SendText("leave");
        wsrelay_ResetSession();
        return 0x80004005; // GUINET_NOGAMECONNECT
    }

    // Before the level loads, which needs the map.
    if (!wsrelay_GetHostMap(wsrelay_storedEntryEnum.episodeGobName))
    {
        wsrelay_SendText("leave");
        wsrelay_ResetSession();
        return 0x80004005; // GUINET_NOGAMECONNECT
    }

    Hack_ResetClients();
    return 0;
}

void stdComm_Close()
{
    if (wsrelay_bHosting || wsrelay_bHostPending || wsrelay_bJoined || wsrelay_bJoining)
        wsrelay_SendText("leave");
    wsrelay_ResetSession();
}

int DirectPlay_SendLobbyMessage(void* pPkt, uint32_t pktLen)
{
    return 1;
}

void DirectPlay_SetSessionDesc(const char* a1, DWORD maxPlayers)
{
    _strncpy(wsrelay_storedEntry.mapJklFname, jkMain_aLevelJklFname, 0x20);
    wsrelay_UpdateLobby();
}

BOOL DirectPlay_SetSessionFlagidk(int a1)
{
    return 1;
}

BOOL DirectPlay_Startup()
{
    jkGuiMultiplayer_numConnections = 0;
    memset(&jkGuiMultiplayer_aConnections, 0, 0x1180u);
    return 1;
}

int DirectPlay_EarlyInit(char16_t* pwIdk, char16_t* pwPlayerName)
{
    return 0;
}

DPID DirectPlay_CreatePlayer(char16_t* pwIdk, int idk2)
{
    return 1;
}

void DirectPlay_Close()
{
}

int DirectPlay_OpenHost(jkMultiEntry* pEntry)
{
    wsrelay_storedEntry = *pEntry;

    jkPlayer_maxPlayers = pEntry->maxPlayers; // Hack?

    stdComm_bIsServer = 1;

    wsrelay_ResetSession();

    // Without a relay the game still runs, just with nobody able to join,
    // which is what hosting did before this backend existed.
    if (!wsrelay_EnsureOpen(WSRELAY_CONNECT_MS))
    {
        stdPlatform_Printf("OpenJKDF2: No relay reachable, hosting offline.\n");
        return 0;
    }

    wsrelay_bHostPending = 1;
    wsrelay_pendingRoom = -1;
    wsrelay_storedEntry.multiModeFlags = sithMulti_multiModeFlags;
    wsrelay_js_mod_tail(wsrelay_hostTail, sizeof(wsrelay_hostTail));
    wsrelay_SendEntry("host", -1, &wsrelay_storedEntry);

    uint32_t until = stdPlatform_GetTimeMsec() + WSRELAY_REPLY_MS;
    while (wsrelay_bHostPending && stdPlatform_GetTimeMsec() < until)
    {
        wsrelay_PumpControl();
        if (wsrelay_js_state() != WSRELAY_STATE_OPEN)
            break;
        emscripten_sleep(10);
    }

    if (!wsrelay_bHosting)
    {
        stdPlatform_Printf("OpenJKDF2: The relay didn't open a room, hosting offline.\n");
        wsrelay_bHostPending = 0;
    }
    else
    {
        wsrelay_js_map_host(wsrelay_storedEntry.episodeGobName, JKRES_GOB_EXT);
        wsrelay_js_mod_host();
    }

    return 0;
}

int DirectPlay_GetSession_passwordidk(jkMultiEntry* pEntry)
{
    wsrelay_storedEntry = *pEntry;

    jkPlayer_maxPlayers = pEntry->maxPlayers; // Hack?

    return 1;
}

int DirectPlay_EnumSessions2()
{
    return 0;
}

int stdComm_EnumSessions(int a, void* b)
{
    Hack_ResetClients();

    // Ask again soon instead of every 5 seconds, like the GNS enum thread.
    jkGuiMultiplayer_dword_5564E8 -= 3000;

    if (wsrelay_js_state() != WSRELAY_STATE_OPEN)
    {
        // Don't block the menu on a connect; the next poll picks it up.
        wsrelay_js_connect();
    }
    else
    {
        // Only the games this one can join: Jedi Knight can't join a game
        // Mysteries of the Sith hosts, or the other way round.
        wsrelay_bListReceived = 0;
        wsrelay_SendText(Main_bMotsCompat ? "list mots" : "list jk1");
        wsrelay_WaitForChange(&wsrelay_bListReceived, WSRELAY_LIST_MS);
    }

    int n = wsrelay_numListed;
    if (wsrelay_js_state() != WSRELAY_STATE_OPEN)
        n = 0;

    memset(jkGuiMultiplayer_aEntries, 0, sizeof(jkMultiEntry) * 32);
    for (int i = 0; i < n; i++)
    {
        jkGuiMultiplayer_aEntries[i] = wsrelay_aListEntries[i];
        wsrelay_aShownRooms[i] = wsrelay_aListRooms[i];
        strcpy(wsrelay_aShownTails[i], wsrelay_aListTails[i]);

        // Back from restarting into this game's mod: select it again. The join
        // screen keeps whichever entry has this guid selected.
        if (wsrelay_aShownRooms[i] == wsrelay_pendingRoom)
        {
            jkGui_guid_556040 = jkGuiMultiplayer_aEntries[i].guidInstance;
            wsrelay_pendingRoom = -1;
        }
    }
    wsrelay_numShown = n;
    dplay_dword_55D618 = n;

    return 0;
}

void DirectPlay_EnumPlayers(int a)
{
    Hack_ResetClients();
}

int DirectPlay_StartSession(void* a, void* b)
{
    return 1;
}

void DirectPlay_Destroy()
{
}

int DirectPlay_IdkSessionDesc(jkMultiEntry* pEntry)
{
    return 1;
}

void stdComm_WSRelay_DescribeMod(int idx, char16_t* pOut, int maxChars)
{
    char text[128];
    text[0] = 0;
    if (idx >= 0 && idx < wsrelay_numShown)
        wsrelay_js_mod_describe(wsrelay_aShownTails[idx], text, sizeof(text));

    // mods.js only writes printable ASCII here.
    int i = 0;
    for (; text[i] && i < maxChars - 1; i++)
        pOut[i] = (char16_t)(uint8_t)text[i];
    pOut[i] = 0;
}
