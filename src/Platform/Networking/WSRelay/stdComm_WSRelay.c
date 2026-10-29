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
// A link goes straight between the two computers where it can, or through a
// TURN server where they can't reach each other directly, if the relay hands
// out credentials for one: a game in a room asks (turn) and gets them in an
// ice line with how long they last. A client asks as soon as it has its seat,
// and its first offer to the host waits for them, 2 s at most.
//
// A client and its host keep what they send each other in order across the
// move to their link, where both say they can ({"x": {"sw": 1}}): until the
// link is up, each sends through the relay; then it sends a switch through
// the relay, after everything it sent that way, and the rest over the link,
// and each holds what comes over the link until the other's switch is in. A
// link that drops is closed at once, so nothing on it arrives after what then
// goes through the relay, and the client offers a new one after 1, 3, 9 and
// 27 s, then every minute, as it does when one isn't up 15 s after its offer.
// With an older game at the other end, the link works as it did before.
//
// Clients link to each other too, where the lobby passes link setup between
// them (it says "lobby links"), so a packet between two of them needn't go
// through the host. A client that hears from another says hello to it through
// the host, and once both have, the one with the lower id offers a link. Each
// pair uses one path at a time, so its messages stay in the order they were
// sent: until their link is up, packets between them go through the host as
// before; then each switches as above, with its switch going through the host.
// A link that drops is closed at once and tried again after 5, 15 and 45 s.
// Two clients on a link learn each other's IP address, as a host and a client
// do.
//
// Text frames are relay control lines, binary frames are game packets:
//   us -> relay   list <game> | host <entry> | update <entry> | join <room>
//                 welcome <id> <entry> | leave | signal <id> <data> | ping
//                 hello <game> <name> | rejoin <room> <id> <key>
//                 rehost <room> <key> <entry> | turn
//   relay -> us   list (<room> <players> <entry>)* | hosted <room>
//                 peer-joined <id> | peer-left <id> | welcome <id> <entry>
//                 host-left | error <code> | signal <id> <data>
//                 ice <data> [<seconds>] | lobby <what>*
//                 seat <room> <id> <key> | rejoined <room> <id> | rehosted <room>
//                 peer-lost <id> | peer-back <id> | host-lost | host-back
// list asks for the games this game can join: those hosted by the same game
// (jk1 or mots), and any whose host hasn't said which it runs. <entry> is a
// hex-encoded jkMultiEntry. <data> is base64 JSON: an SDP offer or answer
// ({"d": ...}) or an ICE candidate ({"c": ...}) for signal, and the
// RTCPeerConnection iceServers list for ice, with how many seconds its TURN
// credentials last when it has them. lobby says what the relay can do beyond
// that: links (signal between clients) and turn (TURN credentials). The JS
// below handles signal, ice and lobby itself; the engine never sees them.
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
//
// packaging/wasm/netstats.js, also bundled in, sees every game packet on its
// way in and out: it measures each link (pings, position update gaps), keeps
// the hit ledger (stdComm_WSRelay_NoteHit), and with ?netsim makes this
// window's packets wait as if its connection were slower. Its pings go over
// a third data channel that never resends, which both ends only open once
// each has said in its offer or answer ({"x": {"ns": 1}}) that it has the
// file.
//
// A player's own position updates take a fast lane where both ends have one:
// a fourth data channel ('jk-pos') that never waits for or resends a lost
// message, opened once each end has said so ({"x": {"pos": 1}}). A player
// sends its position about 50 times a second and each one replaces the last,
// so on the reliable channel one lost packet held up every update behind it
// until it was resent. Each update on the fast lane starts with a sequence
// number, counted for each sender and receiver, and one older than an update
// already taken from that sender is dropped, so a late one never moves a
// player back. The host passes a joiner's on over its fast lane to the other
// joiners with the joiner's number, or over the reliable channel without it
// to a joiner that has no fast lane. Everything else stays on the reliable
// channel, for the reason above.
//
// A joiner downloading a map or mod from the host opens a few more links to
// it while the download runs, each its own RTCPeerConnection, since a data
// channel carries only so much at a time (net.mapBulk).
//
// ?netold leaves out the fast lane, the switch, links between clients, TURN
// and transfer links, so its links work as they did before them, to compare.

#include "stdComm_WSRelay.h"

#include "Win95/stdComm.h"
#include "Dss/sithMulti.h"
#include "Devices/sithComm.h"
#include "World/sithThing.h"
#include "Main/Main.h"
#include "General/stdString.h"
#include "stdPlatform.h"
#include "jk.h"

#include <emscripten.h>
#include <math.h>
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

EM_JS(void, wsrelay_js_connect, (int netcode), {
    var net = Module.jkNet;
    if (!net) {
        net = Module.jkNet = { state: 2, queue: [], ws: null, peers: {}, isHost: false, ice: null, netcode: netcode,
                               stats: { dcSent: 0, dcRecv: 0, wsSent: 0, wsRecv: 0, wsSkipped: 0,
                                        fastSent: 0, fastRecv: 0, fastOld: 0, fastBacklog: 0 },
                               log: [], lastFrom: {}, seat: null, entry: "", rejoinMs: 1000, relayPosAt: {},
                               seqTo: {}, seqSent: {}, seqFrom: {}, links: {}, lobbyCan: [], pending: {},
                               bulk: {}, bulkUse: {},
                               turn: null, turnEnds: 0, iceUntil: 0, iceAsked: false, iceAskedAt: 0, iceWaiters: [],
                               session: false, sessionNo: 0, hostTries: 0, hostRetry: null };

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
                line += '; player ' + id + ' ' + (p ? (p.open ? 'direct' : p.joiner ? 'through the host' : 'relay') + ' (' + p.pc.connectionState +
                    ', channel ' + p.dc.readyState + ', ' + p.dc.bufferedAmount + ' bytes waiting' +
                    (p.pos ? ', fast lane ' + p.pos.readyState : '') +
                    (net.bulkCount(id) ? ', ' + net.bulkCount(id) + ' transfer links' : '') + ')' : 'relay') +
                    ', heard ' + (heard ? ((now - heard) / 1000).toFixed(1) + ' s ago' : 'never');
            });
            line += '; sent ' + (s.dcSent - was.dcSent) + ' direct, ' + (s.fastSent - was.fastSent) + ' fast (' +
                    (s.fastBacklog - was.fastBacklog) + ' dropped behind a backlog), ' + (s.wsSent - was.wsSent) + ' relay (' +
                    (s.wsSkipped - was.wsSkipped) + ' positions skipped); got ' +
                    (s.dcRecv - was.dcRecv) + ' direct, ' + (s.fastRecv - was.fastRecv) + ' fast (' +
                    (s.fastOld - was.fastOld) + ' too old), ' + (s.wsRecv - was.wsRecv) + ' relay';
            net.healthStats = Object.assign({}, s);
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
            var pc;
            try {
                pc = new RTCPeerConnection({ iceServers: net.iceServers() });
            } catch (e) {
                // A server list the browser won't take: without TURN, then.
                net.event('WebRTC refused the TURN servers (' + e + '), linking without them');
                net.turn = null;
                pc = new RTCPeerConnection({ iceServers: net.ice || Module['jkIceServers'] || [] });
            }
            var dc = pc.createDataChannel('jk', { negotiated: true, id: 0 });
            dc.binaryType = 'arraybuffer';
            // Map downloads get their own channel, so one never holds up game
            // packets.
            var mc = pc.createDataChannel('jk-map', { negotiated: true, id: 1 });
            mc.binaryType = 'arraybuffer';
            // joiner: a link between two joiners (net.linkOffer), not one with
            // the host. sw: both ends switch to it (see the top of the file),
            // which a link with the host does once the other end says it can
            // (net.extraChannels).
            var joiner = !net.isHost && id !== 1;
            p = net.peers[id] = { pc: pc, dc: dc, mc: mc, open: false, joiner: joiner, sw: joiner };
            var update = function() {
                if (net.peers[id] !== p) return;
                var was = p.open;
                var cs = pc.connectionState;
                var why = cs + ', channel ' + dc.readyState;
                p.open = dc.readyState === 'open' && cs !== 'disconnected' && cs !== 'failed' && cs !== 'closed';
                if (p.joiner) {
                    if (p.open && !was) net.linkUp(id, p);
                    else if (!p.open && (was || cs === 'failed' || cs === 'closed')) net.linkDown(id, p, why);
                    return;
                }
                var gone = cs === 'failed' || cs === 'closed' || dc.readyState === 'closed';
                if (p.open && !was) {
                    net.event('direct WebRTC link to player ' + id + ' is up');
                    p.wasUp = true;
                    if (id === 1) net.hostTries = 0;
                    if (p.sw) net.relaySwitch(id, p);
                } else if (!p.open && (p.sw ? was || gone : gone && !p.wasUp)) {
                    // Where both ends switch, a link that drops is closed at
                    // once, so nothing on it comes in after what then goes
                    // through the relay. One that never came up carried
                    // nothing. The joiner offers again (net.hostLinkDown).
                    if (net.isHost) {
                        net.closePeer(id);
                        net.event('direct link to player ' + id + ' is down (' + why + '), using the relay until it offers a new one');
                    } else {
                        net.hostLinkDown(p, why);
                    }
                } else if (!p.open && was) {
                    net.event('direct link to player ' + id + ' is down (' + why + '), using the relay');
                }
            };
            dc.onopen = update;
            dc.onclose = update;
            pc.onconnectionstatechange = update;
            dc.onmessage = function(ev) { if (net.peers[id] === p) net.onPeerPacket(id, ev.data); };
            mc.onmessage = function(ev) { if (net.peers[id] === p && Module.jkMaps) Module.jkMaps.onFrame(id, ev.data, 'direct'); };
            pc.onicecandidate = function(ev) { if (ev.candidate && net.peers[id] === p) net.signal(id, { c: ev.candidate.toJSON() }); };
            return p;
        };

        /* What this game adds to its offer or answer: that it has netstats.js,
           so the other end may open the measurements' channel with it, that
           it has the fast lane, that it switches to a link and back (see
           the top of the file), and that it takes transfer links
           (net.mapBulk). */
        net.extras = function() {
            var x = {};
            if (Module.jkNetStats) x.ns = 1;
            if (!Module['jkNetOld']) x.pos = x.sw = x.bulk = 1;
            return x;
        };

        /* The fast lane for position updates, once both ends have said they
           have it: unordered, and nothing lost is resent (RFC 8831 names game
           positions as what such a channel is for). */
        net.posChannel = function(id, p) {
            if (p.pos || Module['jkNetOld']) return;
            var pos = p.pos = p.pc.createDataChannel('jk-pos', { negotiated: true, id: 3, ordered: false, maxRetransmits: 0 });
            pos.binaryType = 'arraybuffer';
            pos.onmessage = function(ev) { if (net.peers[id] === p) net.onFastPacket(id, ev.data); };
        };

        net.posOpen = function(id) {
            var p = net.peers[id];
            return !!(p && p.open && p.pos && p.pos.readyState === 'open');
        };

        /* The measurements' channel, which never waits for or resends a lost
           message, once both ends have said they have netstats.js. */
        net.nsChannel = function(id, p) {
            if (p.ns || !Module.jkNetStats) return;
            var ns = p.ns = p.pc.createDataChannel('jk-ns', { negotiated: true, id: 2, ordered: false, maxRetransmits: 0 });
            ns.binaryType = 'arraybuffer';
            ns.onmessage = function(ev) { if (net.peers[id] === p) Module.jkNetStats.onProbe(id, ev.data); };
        };

        /* The channels the other end said it has, from its offer or answer,
           and whether both switch. */
        net.extraChannels = function(id, p, x) {
            if (x && x.ns) net.nsChannel(id, p);
            if (x && x.pos) net.posChannel(id, p);
            if (x && x.sw && !Module['jkNetOld']) p.sw = true;
            if (x && x.bulk && !Module['jkNetOld']) p.bulkOk = true;
        };

        net.nsOpen = function(id) {
            var p = net.peers[id];
            return !!(p && p.open && p.ns && p.ns.readyState === 'open');
        };

        net.nsSend = function(id, buf) {
            if (!net.nsOpen(id)) return 0;
            try {
                net.peers[id].ns.send(buf);
                return 1;
            } catch (e) {
                return 0;
            }
        };

        net.closePeer = function(id) {
            var p = net.peers[id];
            if (!p) return;
            delete net.peers[id];
            p.open = false;
            // What came over it and waits for its switch still does (net.hold).
            if (p.held && p.held.length && !p.gotSwitch) {
                if (net.pending[id]) net.release(id, net.pending[id]);
                net.pending[id] = p;
            }
            try { p.dc.close(); } catch (e) {}
            try { p.mc.close(); } catch (e) {}
            if (p.ns) try { p.ns.close(); } catch (e) {}
            if (p.pos) try { p.pos.close(); } catch (e) {}
            try { p.pc.close(); } catch (e) {}
            net.closeBulk(id);
        };

        net.closeAll = function() {
            for (var id in net.peers) net.closePeer(id);
            for (var id in net.bulk) net.closeBulk(id);
        };

        net.offer = function(id) {
            var p = net.peer(id);
            var x = net.extras();
            // A link that switches has a key, which its switch carries.
            if (x.sw) x.k = p.key = 1 + Math.floor(Math.random() * 4294967295);
            p.pc.createOffer()
                .then(function(o) { return p.pc.setLocalDescription(o); })
                .then(function() { if (net.peers[id] === p) net.signal(id, { d: p.pc.localDescription.toJSON(), x: x }); })
                .catch(function(e) { console.warn('OpenJKDF2: WebRTC offer to player ' + id + ' failed, using the relay: ' + e); });
        };

        net.onSignal = function(from, obj) {
            if (!net.rtcEnabled()) return;
            if (obj.b !== undefined) {
                net.onBulkSignal(from, obj);
                return;
            }
            var p;
            if (obj.d && obj.d.type === 'offer') {
                // A host answers its joiners, a joiner another joiner with a
                // lower id.
                var key = obj.x && obj.x.k;
                if (!net.isHost && !(net.linksOn() && from > 1 && from < net.myId() && key)) return;
                net.closePeer(from);
                p = net.peer(from);
                p.key = key || 0;
                net.extraChannels(from, p, obj.x);
                net.askIce();
                p.pc.setRemoteDescription(obj.d)
                    .then(function() { return p.pc.createAnswer(); })
                    .then(function(a) { return p.pc.setLocalDescription(a); })
                    .then(function() { if (net.peers[from] === p) net.signal(from, { d: p.pc.localDescription.toJSON(), x: net.extras() }); })
                    .catch(function(e) { console.warn('OpenJKDF2: WebRTC answer to player ' + from + ' failed, using the relay: ' + e); });
            } else if (obj.d) {
                p = net.peers[from];
                if (p) net.extraChannels(from, p, obj.x);
                if (p) p.pc.setRemoteDescription(obj.d).catch(function(e) { console.warn('OpenJKDF2: WebRTC answer from player ' + from + ' rejected: ' + e); });
            } else if (obj.c) {
                p = net.peers[from];
                if (p) p.pc.addIceCandidate(obj.c).catch(function() {});
            }
        };

        /* Packets from a data channel. The host knows who is on the other end,
           so it stamps idFrom itself and forwards anything meant for another
           client. netstats.js holds each one while ?netsim says to, and takes
           its own frames out. */
        net.onPeerPacket = function(id, data) {
            if (!(data instanceof ArrayBuffer) || data.byteLength < 8) return;
            net.stats.dcRecv++;
            net.lastFrom[id] = performance.now();
            if (Module.jkNetStats) Module.jkNetStats.inbound(id, data, 'direct', function(d) { net.takePeerPacket(id, d); });
            else net.takePeerPacket(id, data);
        };

        /* seq: the packet came on the fast lane with this number, and the
           host passes it on over the fast lane where it can. netstats.js
           counts it under the player who sent it, not the host that passed it
           on. released: it waited for the link's switch (net.hold). */
        net.takePeerPacket = function(id, data, seq, released) {
            var p = net.peers[id];
            // Over a link whose switch hasn't come yet. Fast lane updates
            // don't wait: they're numbered, and come in any order anyway.
            if (!released && seq === undefined && p && p.sw && !p.gotSwitch) {
                if (net.isSwitch(data, p)) {
                    net.release(id, p);
                    return;
                }
                if (net.otherWayUp(p)) {
                    net.hold(id, p, data);
                    return;
                }
                // Nothing can come the other way now.
                p.gotSwitch = true;
            }
            var dv = new DataView(data);
            if (net.isHost) {
                dv.setUint32(0, id, true);
                var to = dv.getUint32(4, true);
                if (to > 1) {
                    net.sendPacket(new Uint8Array(data), to, seq);
                    return;
                }
            } else if (id !== 1) {
                // From another joiner, over a link of our own.
                dv.setUint32(0, id, true);
            }
            net.take(dv.getUint32(0, true), data, 'direct', seq);
        };

        /* A game packet from player `from` that's due: for this file, for
           netstats.js, or for the engine. seq: it came on the fast lane with
           this number, which the engine gets with it (wsrelay_js_recv): a
           claim under Hit what you see names the update its attacker had on
           screen (stdComm_WSRelayHits.c). */
        net.take = function(from, data, via, seq) {
            if (net.isLinkFrame(data)) {
                net.onLinkFrame(from, data);
                return;
            }
            if (from > 1 && !net.isHost) net.heardFrom(from);
            if (Module.jkNetStats && Module.jkNetStats.take(from, data, via, seq !== undefined)) return;
            if (seq !== undefined) data.jkSeq = seq;
            net.queue.push(data);
        };

        /* Position updates on the fast lane: a sequence number, then the
           packet as the reliable channel has it. They can come in any order,
           so one older than the last taken from its sender is dropped; the
           next is never more than a tick away. */
        net.onFastPacket = function(id, data) {
            if (!(data instanceof ArrayBuffer) || data.byteLength < 12) return;
            net.stats.fastRecv++;
            net.lastFrom[id] = performance.now();
            if (Module.jkNetStats) Module.jkNetStats.inbound(id, data, 'direct', function(d) { net.takeFastPacket(id, d); }, true);
            else net.takeFastPacket(id, data);
        };

        net.takeFastPacket = function(id, data) {
            var seq = new DataView(data).getUint32(0, true);
            var packet = data.slice(4);
            var dv = new DataView(packet);
            // Only the host passes on other players' packets.
            if (id !== 1) dv.setUint32(0, id, true);
            if (!net.freshPos(dv.getUint32(0, true), dv.getUint32(4, true), seq)) {
                net.stats.fastOld++;
                return;
            }
            net.takePeerPacket(id, packet, seq);
        };

        /* Whether a fast lane update from player `from` to player `to` is
           newer than the last one taken. A number far behind, or one after a
           second of nothing, means the sender started over (it left and came
           back, or its game restarted). */
        net.freshPos = function(from, to, seq) {
            var key = from + ' ' + to, last = net.seqFrom[key], now = performance.now();
            if (last && now - last.at < 1000) {
                var ahead = (seq - last.seq) | 0;
                if (ahead <= 0 && ahead > -1000) return false;
            }
            net.seqFrom[key] = { seq: seq, at: now };
            return true;
        };

        /* The next number for this player's position updates to player `to`,
           from a random start, so a receiver can tell a sender that started
           over. When each of the last 256 went, so a claim that names one can
           be checked from then (net.seqSentAt). */
        net.nextSeq = function(to) {
            var s = net.seqTo[to];
            if (s === undefined) s = Math.floor(Math.random() * 4294967296);
            s = net.seqTo[to] = (s + 1) >>> 0;
            var sent = net.seqSent[to] || (net.seqSent[to] = { seqs: new Uint32Array(256), at: new Float64Array(256) });
            sent.seqs[s & 255] = s;
            sent.at[s & 255] = performance.now();
            return s;
        };

        net.seqSentAt = function(to, seq) {
            var sent = net.seqSent[to];
            return sent && sent.seqs[seq & 255] === seq && sent.at[seq & 255] ? sent.at[seq & 255] : -1;
        };

        /* The link a packet for player `to` goes over: a host's go straight
           to that player, a joiner's to the host, which passes them on, or
           over a link of its own to that player once it has switched to it. */
        net.route = function(to) {
            if (net.isHost) return to;
            var p = net.peers[to];
            return to > 1 && p && p.joiner && p.open && p.switched ? to : 1;
        };

        /* Direct links between joiners (see the top of the file). */
        var LINK_HELLO = 1, LINK_SWITCH = 2;

        net.myId = function() {
            return net.isHost ? 1 : net.seat ? net.seat.id : 0;
        };

        net.linksOn = function() {
            return !!net.session && !net.isHost && net.myId() > 1 && net.rtcEnabled() && !Module['jkNetOld'] &&
                   net.lobbyCan.indexOf('links') >= 0;
        };

        /* A frame of this file's own, which goes between joiners through the
           host: like a game packet with message id 0x4C4A ('J' 'L'), past
           DSS_MAX, without a resend id, so engines without this ignore it.
           Then the type, a version, two zero bytes and the link's key. */
        net.linkFrame = function(type, to, key) {
            var bytes = new Uint8Array(20), dv = new DataView(bytes.buffer);
            dv.setUint32(0, net.myId(), true);
            dv.setUint32(4, to, true);
            bytes.set([0x4A, 0x4C, 0, 0], 8);
            dv.setUint8(12, type);
            dv.setUint8(13, 1);
            dv.setUint32(16, key || 0, true);
            return bytes;
        };

        net.isLinkFrame = function(data) {
            if (data.byteLength < 20) return false;
            var b = new Uint8Array(data, 8, 4);
            return b[0] === 0x4A && b[1] === 0x4C && b[2] === 0 && b[3] === 0;
        };

        /* Another joiner's packet came: says hello to it the first time. */
        net.heardFrom = function(from) {
            var l = net.links[from], now = performance.now();
            if (l) {
                l.heardAt = now;
                return;
            }
            if (from === net.myId() || !net.linksOn()) return;
            net.links[from] = { tries: 0, retryAt: 0, heardAt: now };
            net.sendPacket(net.linkFrame(LINK_HELLO, from), 1);
        };

        net.onLinkFrame = function(from, data) {
            var dv = new DataView(data);
            if (dv.getUint8(13) !== 1) return;
            var type = dv.getUint8(12), key = dv.getUint32(16, true);
            if (type === LINK_HELLO && from > 1) {
                // Both have said hello: the lower id offers.
                net.heardFrom(from);
                if (net.links[from] && net.myId() < from) net.linkOffer(from);
            } else if (type === LINK_SWITCH && key) {
                // For the link that's up, or one that closed while it waited.
                var p = net.peers[from];
                if (!(p && p.sw && p.key === key)) p = net.pending[from];
                if (p && p.sw && p.key === key) net.release(from, p);
            }
        };

        /* A link's own switch, sent over the link because it couldn't go the
           other way. */
        net.isSwitch = function(data, p) {
            if (!net.isLinkFrame(data)) return false;
            var dv = new DataView(data);
            return dv.getUint8(12) === LINK_SWITCH && dv.getUint8(13) === 1 && dv.getUint32(4, true) === net.myId() &&
                   !!p.key && dv.getUint32(16, true) === p.key;
        };

        /* Whether packets from the player on link p can still come the way
           they came before it: through the host for a link between joiners,
           through the relay for a link with the host. */
        net.otherWayUp = function(p) {
            var host = net.peers[1];
            return net.state === 1 || (p.joiner && !!host && host.open);
        };

        net.linkOffer = function(id) {
            var l = net.links[id], now = performance.now();
            if (!l || net.peers[id] || now < l.retryAt || l.tries >= 4 || now - l.heardAt > 5000 || !net.linksOn()) return;
            l.tries++;
            net.askIce();
            net.offer(id);
            var p = net.peers[id];
            setTimeout(function() {
                if (net.peers[id] === p && !p.open) net.linkDown(id, p, 'not up after 15 s');
            }, 15000);
        };

        net.linkUp = function(id, p) {
            net.event('direct WebRTC link to player ' + id + ' (a joiner) is up');
            // It counts as the first try, so one that drops is tried again
            // after 5 s, 15 s and 45 s, as a new one is (net.linkDown).
            if (net.links[id]) net.links[id].tries = 1;
            // Everything sent to them so far went through the host. The switch
            // goes the same way, after it, and the rest over the link. With
            // no way through the host, nothing is on the way there.
            var frame = net.linkFrame(LINK_SWITCH, id, p.key);
            if (!net.sendPacket(frame, 1)) try { p.dc.send(frame); } catch (e) {}
            p.switched = true;
        };

        /* The same for a link with the host: what went before it went
           through the relay, so the switch does too (on the same netsim line
           as the packets before it), or over the link if the relay is down. */
        net.relaySwitch = function(id, p) {
            var frame = net.linkFrame(LINK_SWITCH, id, p.key);
            var send = function(b) {
                if (net.state === 1) {
                    net.ws.send(b);
                    return 1;
                }
                try {
                    p.dc.send(b);
                    return 1;
                } catch (e) {
                    return 0;
                }
            };
            if (Module.jkNetStats) Module.jkNetStats.outbound(id, frame, send);
            else send(frame);
            p.switched = true;
        };

        /* A joiner's link with its host: offered once it has TURN credentials
           (net.withIce), and again after 1 s, 3 s, 9 s, 27 s and then every
           minute if it isn't up 15 s after an offer, or drops where both ends
           switch. Meanwhile its packets go through the relay. */
        var HOST_RETRY_MS = [1000, 3000, 9000, 27000, 60000];
        net.hostOffer = function() {
            var no = net.sessionNo;
            net.withIce(function() {
                if (!net.session || net.sessionNo !== no || net.isHost || net.peers[1] || !net.rtcEnabled()) return;
                net.offer(1);
                var p = net.peers[1];
                setTimeout(function() {
                    if (net.peers[1] === p && !p.open && !p.wasUp) net.hostLinkDown(p, 'not up after 15 s');
                }, 15000);
            });
        };

        net.hostLinkDown = function(p, why) {
            if (net.peers[1] !== p) return;
            net.closePeer(1);
            var wait = HOST_RETRY_MS[Math.min(net.hostTries, HOST_RETRY_MS.length - 1)];
            net.hostTries++;
            net.event('direct link to the host is down (' + why + '), using the relay; offering a new one in ' + wait / 1000 + ' s');
            var no = net.sessionNo;
            clearTimeout(net.hostRetry);
            net.hostRetry = setTimeout(function() {
                net.hostRetry = null;
                if (net.sessionNo === no) net.hostOffer();
            }, wait);
        };

        /* A link that drops is closed at once, so nothing sent over it comes
           in after what goes through the host instead, and the joiner with
           the lower id tries again after 5 s, 15 s and 45 s. */
        net.linkDown = function(id, p, why) {
            if (net.peers[id] !== p) return;
            net.closePeer(id);
            net.event('direct link to player ' + id + ' (a joiner) is down (' + why + '), going through the host');
            var l = net.links[id];
            if (!l || net.myId() > id) return;
            var wait = 5000 * Math.pow(3, Math.max(0, l.tries - 1));
            l.retryAt = performance.now() + wait;
            setTimeout(function() { net.linkOffer(id); }, wait + 10);
        };

        /* What comes over a link before the other end's switch: what it sent
           the other way before the link came up may still be on the way, so
           these wait, in order, until the switch arrives (3 s at most). */
        net.hold = function(id, p, data) {
            p.held = p.held || [];
            p.held.push(data);
            var no = net.sessionNo;
            if (!p.holdTimer) p.holdTimer = setTimeout(function() {
                if (p.gotSwitch || net.sessionNo !== no) return;
                net.event('no switch from player ' + id + ' after 3 s, taking what came over the link');
                net.release(id, p);
            }, 3000);
        };

        net.release = function(id, p) {
            if (p.gotSwitch) return;
            p.gotSwitch = true;
            clearTimeout(p.holdTimer);
            if (net.pending[id] === p) delete net.pending[id];
            var held = p.held || [];
            p.held = [];
            held.forEach(function(d) { net.takePeerPacket(id, d, undefined, true); });
        };

        /* Once the relay is gone, nothing more comes through it. */
        net.releaseRelayHolds = function() {
            var waiting = Object.keys(net.peers).map(function(id) { return [id, net.peers[id]]; })
                .concat(Object.keys(net.pending).map(function(id) { return [id, net.pending[id]]; }));
            waiting.forEach(function(w) {
                if (w[1].sw && !w[1].gotSwitch && !net.otherWayUp(w[1])) net.release(parseInt(w[0], 10), w[1]);
            });
        };

        /* seq: one of this player's own position updates, or one the host
           passes on, with its number: it goes on the fast lane if that's
           open, and the reliable channel or the relay without it if not. */
        net.sendPacket = function(bytes, id, seq) {
            var fast = seq !== undefined && net.posOpen(id);
            if (Module.jkNetStats) return Module.jkNetStats.outbound(id, bytes, function(b) { return net.sendNow(b, id, fast ? seq : undefined); }, fast);
            return net.sendNow(bytes, id, fast ? seq : undefined);
        };

        net.sendNow = function(bytes, id, seq) {
            var p = net.peers[id];
            if (seq !== undefined && net.posOpen(id)) {
                // Behind a backlog (a slow or stalled link) it would be out of
                // date by the time it got there; the next one will do.
                if (p.pos.bufferedAmount > 4096) {
                    net.stats.fastBacklog++;
                    return 1;
                }
                try {
                    var framed = new Uint8Array(4 + bytes.byteLength);
                    new DataView(framed.buffer).setUint32(0, seq, true);
                    framed.set(bytes, 4);
                    p.pos.send(framed);
                    net.stats.fastSent++;
                    return 1;
                } catch (e) {}
            }
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
           head pitch for players), or 60 or 72 with its time
           (stdComm_WSRelayMotion.c). Direct links get them all. */
        net.skipRelayedPos = function(bytes) {
            var n = bytes.byteLength, body = n - 8 - 4;
            if (body !== 52 && body !== 64 && body !== 60 && body !== 72) return false;
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
            var n = net.bulkNumber(via);
            if (n) {
                var l = (net.bulk[id] || {})[n];
                if (!l || !net.bulkOpen(l)) return null;
                try {
                    l.mc.send(bytes);
                    return via;
                } catch (e) {
                    return null;
                }
            }
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

        /* Transfer links: more links to the host while a map or mod download
           runs. A data channel carries only so much at a time however fast
           the connection (in Chrome about 2 MB/s at a 50 ms round trip,
           packaging/wasm/test/transfer-bench.mjs), and each RTCPeerConnection
           has its own, so maps.js asks for these while it downloads
           (net.mapBulk) and spreads the download over them and the map
           channel (net.mapLanes), each lane named 'direct' and its number.
           Each is a connection of its own with only a map channel, whose
           signals carry its number ({"b": n}). A joiner offers them only to
           a host that said it takes them ({"x": {"bulk": 1}}): one from
           before them would take the offer for the main link. They close
           20 s after the last download, or with the main link. */
        var BULK_LINKS = 3, BULK_IDLE_MS = 20000, BULK_UP_MS = 15000, BULK_RETRY_MS = 30000, BULK_HOST_MAX = 12;

        net.bulkNumber = function(via) {
            var m = /^direct([1-9])$/.exec(via || '');
            return m && +m[1] <= BULK_LINKS ? +m[1] : 0;
        };

        net.bulkOpen = function(l) {
            var cs = l.pc.connectionState;
            return l.mc.readyState === 'open' && cs !== 'disconnected' && cs !== 'failed' && cs !== 'closed';
        };

        net.bulkCount = function(id) {
            var n = 0;
            if (id === undefined) {
                for (var k in net.bulk) n += net.bulkCount(k);
                return n;
            }
            for (var k in net.bulk[id] || {}) n++;
            return n;
        };

        /* A new transfer link with player id, number n, in place of any
           before it. */
        net.bulkLink = function(id, n) {
            var pc;
            try {
                pc = new RTCPeerConnection({ iceServers: net.iceServers() });
            } catch (e) {
                pc = new RTCPeerConnection({ iceServers: net.ice || Module['jkIceServers'] || [] });
            }
            var mc = pc.createDataChannel('jk-map', { negotiated: true, id: 1 });
            mc.binaryType = 'arraybuffer';
            net.closeBulkLink(id, n);
            var links = net.bulk[id] = net.bulk[id] || {};
            var l = links[n] = { pc: pc, mc: mc, at: performance.now() };
            var mine = function() { return !!net.bulk[id] && net.bulk[id][n] === l; };
            var via = 'direct' + n;
            mc.onmessage = function(ev) { if (mine() && Module.jkMaps) Module.jkMaps.onFrame(id, ev.data, via); };
            mc.onopen = function() { if (mine()) net.note('transfer link ' + n + ' to player ' + id + ' is up'); };
            pc.onicecandidate = function(ev) { if (ev.candidate && mine()) net.signal(id, { b: n, c: ev.candidate.toJSON() }); };
            var update = function() {
                var cs = pc.connectionState;
                if (cs === 'failed' || cs === 'closed' || mc.readyState === 'closed') net.closeBulkLink(id, n, l);
            };
            pc.onconnectionstatechange = update;
            mc.onclose = update;
            return l;
        };

        /* Closes link n with player id, if it's still l when l is given. */
        net.closeBulkLink = function(id, n, l) {
            var links = net.bulk[id];
            if (!links || !links[n] || (l && links[n] !== l)) return;
            l = links[n];
            delete links[n];
            try { l.mc.close(); } catch (e) {}
            try { l.pc.close(); } catch (e) {}
        };

        net.closeBulk = function(id) {
            for (var n in net.bulk[id] || {}) net.closeBulkLink(id, n);
            delete net.bulk[id];
        };

        /* maps.js downloading from player id (on) or done (off). While any
           download runs the joiner keeps up to BULK_LINKS links to it. */
        net.mapBulk = function(id, on) {
            var w = net.bulkUse[id] = net.bulkUse[id] || { n: 0, idle: null, tried: {} };
            clearTimeout(w.idle);
            w.idle = null;
            if (on) {
                w.n++;
                net.bulkOffer(id);
                return;
            }
            if (w.n > 0) w.n--;
            if (!w.n) w.idle = setTimeout(function() {
                w.idle = null;
                if (!w.n) net.closeBulk(id);
            }, BULK_IDLE_MS);
        };

        /* Offers the links a download could use and doesn't have: once the
           main link is up, again for one that isn't up after BULK_UP_MS, and
           at most every BULK_RETRY_MS for each. */
        net.bulkOffer = function(id) {
            var w = net.bulkUse[id], p = net.peers[id];
            if (!w || !w.n || !p || !p.bulkOk || net.isHost || !net.mapDirect(id) || Module['jkNetOld'] || net.state !== 1) return;
            var now = performance.now();
            var links = net.bulk[id] || {};
            for (var n = 1; n <= BULK_LINKS; n++) {
                var l = links[n];
                if (l && (net.bulkOpen(l) || now - l.at < BULK_UP_MS)) continue;
                if (w.tried[n] !== undefined && now - w.tried[n] < BULK_RETRY_MS) continue;
                w.tried[n] = now;
                (function(n) {
                    var l = net.bulkLink(id, n);
                    l.pc.createOffer()
                        .then(function(o) { return l.pc.setLocalDescription(o); })
                        .then(function() { if ((net.bulk[id] || {})[n] === l) net.signal(id, { b: n, d: l.pc.localDescription.toJSON() }); })
                        .catch(function(e) { net.closeBulkLink(id, n, l); });
                })(n);
            }
        };

        /* The ways to player id a download can use now: the map channel and
           the transfer links that are up, or none for the relay. */
        net.mapLanes = function(id) {
            net.bulkOffer(id);
            var lanes = net.mapDirect(id) ? ['direct'] : [];
            var links = net.bulk[id] || {};
            for (var n = 1; n <= BULK_LINKS; n++) {
                if (links[n] && net.bulkOpen(links[n])) lanes.push('direct' + n);
            }
            return lanes;
        };

        /* A transfer link's offer, answer or ICE candidate. The host answers
           its joiners' offers, BULK_HOST_MAX links at most at once. */
        net.onBulkSignal = function(from, obj) {
            var n = obj.b;
            if (typeof n !== 'number' || n !== (n | 0) || n < 1 || n > BULK_LINKS || Module['jkNetOld']) return;
            var l = (net.bulk[from] || {})[n];
            if (obj.d && obj.d.type === 'offer') {
                if (!net.isHost || from < 2 || !net.peers[from]) return;
                if (!l && net.bulkCount() >= BULK_HOST_MAX) return;
                l = net.bulkLink(from, n);
                l.pc.setRemoteDescription(obj.d)
                    .then(function() { return l.pc.createAnswer(); })
                    .then(function(a) { return l.pc.setLocalDescription(a); })
                    .then(function() { if ((net.bulk[from] || {})[n] === l) net.signal(from, { b: n, d: l.pc.localDescription.toJSON() }); })
                    .catch(function(e) { net.closeBulkLink(from, n, l); });
            } else if (obj.d && l) {
                l.pc.setRemoteDescription(obj.d).catch(function(e) { net.closeBulkLink(from, n, l); });
            } else if (obj.c && l) {
                l.pc.addIceCandidate(obj.c).catch(function() {});
            }
        };

        /* TURN. A lobby that says "turn" hands out short-lived credentials
           for a TURN server to a game in a room that asks ('turn'), in an
           ice line with how many seconds they last. A link set up without
           them can't go through TURN, so a joiner asks as soon as it has its
           seat, and its first link with the host waits for them (2 s at
           most); a host asks when someone joins. A game asks again for links
           it sets up once half their time is up. */
        net.askIce = function() {
            var now = performance.now();
            if (Module['jkNetOld'] || net.lobbyCan.indexOf('turn') < 0 || now < net.iceUntil || net.state !== 1) return false;
            if (!net.iceAskedAt || now - net.iceAskedAt > 10000) {
                net.iceAskedAt = now;
                net.iceAsked = true;
                net.ws.send('turn');
            }
            return true;
        };

        net.withIce = function(fn) {
            if (!net.askIce()) {
                fn();
                return;
            }
            var done = false;
            var go = function() {
                if (done) return;
                done = true;
                fn();
            };
            net.iceWaiters.push(go);
            setTimeout(go, 2000);
        };

        net.onIce = function(data, seconds) {
            var servers = null;
            try { servers = JSON.parse(atob(data)); } catch (e) {}
            var now = performance.now();
            if (servers && seconds > 0) {
                net.turn = servers;
                net.turnEnds = now + seconds * 1000;
                net.iceUntil = now + seconds * 500;
            } else if (servers) {
                // What every game gets on connect, or the lobby's answer when
                // it couldn't get credentials, which it isn't asked for again
                // for a minute.
                net.ice = servers;
                if (net.iceAsked) net.iceUntil = now + 60000;
            }
            net.iceAsked = false;
            var waiters = net.iceWaiters;
            net.iceWaiters = [];
            waiters.forEach(function(go) { go(); });
        };

        /* The servers a new link uses: TURN credentials while they work. */
        net.iceServers = function() {
            return (performance.now() < net.turnEnds && net.turn) || net.ice || Module['jkIceServers'] || [];
        };

        /* Returns true for the lines JS handles itself. */
        net.onText = function(text) {
            var sp = text.indexOf(' ');
            var op = sp < 0 ? text : text.substring(0, sp);
            if (op === 'ice') {
                var ice = text.split(' ');
                net.onIce(ice[1], parseInt(ice[2], 10));
                return true;
            }
            /* The host's credentials for the link the new player offers. */
            if (op === 'peer-joined') net.askIce();
            if (op === 'signal') {
                var parts = text.split(' ');
                var obj = null;
                try { obj = JSON.parse(atob(parts[2])); } catch (e) {}
                if (obj) net.onSignal(parseInt(parts[1], 10), obj);
                return true;
            }
            /* What the lobby can do beyond the basics: links (passing link
               setup between joiners). */
            if (op === 'lobby') {
                net.lobbyCan = text.split(' ').slice(1);
                return true;
            }
            /* The seat to take back if the socket drops in a game. */
            if (op === 'seat') {
                var seat = text.split(' ');
                net.seat = { room: parseInt(seat[1], 10), id: parseInt(seat[2], 10), key: seat[3] || "" };
                if (net.seat.id > 1) net.askIce();
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
            /* The netcode this build speaks (STDCOMM_WSRELAY_NETCODE): the
               lobby only lists and joins games whose host speaks the same. */
            if (url) url += (url.indexOf('?') < 0 ? '?' : '&') + 'net=' + net.netcode;
            if (!keepQueue) net.queue = [];
            net.lobbyCan = [];
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
                net.releaseRelayHolds();
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
                    if (ev.data.byteLength < 8) return;
                    var from = new DataView(ev.data).getUint32(0, true);
                    net.lastFrom[from] = performance.now();
                    if (Module.jkNetStats) Module.jkNetStats.inbound(from, ev.data, 'relay', function(d) { if (net.ws === ws) net.take(from, d, 'relay'); });
                    else net.take(from, ev.data, 'relay');
                    return;
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

// Sends a game packet for player idTo over the link it takes (net.route): a
// data channel if it's open, else the relay. ownPos marks this player's own
// position updates, which take the fast lane where there is one.
EM_JS(int, wsrelay_js_send_packet, (const void* pData, int len, int idTo, int ownPos), {
    var net = Module.jkNet;
    if (!net) return 0;
    return net.sendPacket(HEAPU8.slice(pData, pData + len), net.route(idTo), ownPos ? net.nextSeq(idTo) : undefined);
});

// Starts a session's WebRTC side: a client offers a data channel to the host,
// and a host answers the offers its clients send.
EM_JS(void, wsrelay_js_rtc_begin, (int isHost), {
    var net = Module.jkNet;
    if (!net) return;
    net.closeAll();
    net.session = true;
    net.sessionNo++;
    net.isHost = !!isHost;
    net.lastFrom = {};
    net.relayPosAt = {};
    net.seqTo = {};
    net.seqSent = {};
    net.seqFrom = {};
    net.links = {};
    net.pending = {};
    net.hostTries = 0;
    clearTimeout(net.hostRetry);
    net.hostRetry = null;
    net.healthStats = null;
    if (Module.jkNetStats) Module.jkNetStats.start(isHost);
    net.note((isHost ? 'hosting' : 'joined') + ' room ' + (net.seat ? net.seat.room + ' as player ' + net.seat.id : '(no seat from the relay)'));
    if (!isHost && net.rtcEnabled()) net.hostOffer();
});

EM_JS(void, wsrelay_js_rtc_end, (), {
    var net = Module.jkNet;
    if (!net) return;
    net.closeAll();
    if (net.session && Module.jkNetStats) {
        net.note('netstats at the end: ' + Module.jkNetStats.text().join(' / '));
        Module.jkNetStats.end();
    }
    if (net.session) net.note('left the game');
    net.session = false;
    net.sessionNo++;
    net.links = {};
    net.pending = {};
    clearTimeout(net.hostRetry);
    net.hostRetry = null;
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

// The measurements (packaging/wasm/netstats.js): whether this game is in a
// match now, and as which player.
EM_JS(void, wsrelay_js_net_live, (int live, int id, int isHost), {
    if (Module.jkNetStats) Module.jkNetStats.live(live, id, isHost);
});

// A player's name, for the network panel.
EM_JS(void, wsrelay_js_net_name, (int id, const char16_t* pName), {
    if (!Module.jkNetStats) return;
    var chars = [];
    for (var i = 0; i < 32; i++) {
        var ch = HEAPU16[(pName >> 1) + i];
        if (!ch) break;
        chars.push(ch);
    }
    Module.jkNetStats.name(id, String.fromCharCode.apply(null, chars));
});

// A hit between this game's player and player otherId (stdComm_WSRelay_NoteHit),
// with WSRELAY_HITFLAG_*, and for a claim this game turned down, why
// (reason) and how close its player came (distance, -1 if not checked).
EM_JS(void, wsrelay_js_net_hit, (int otherId, int mine, int kind, int outcome, float damage, int flags, uint32_t attack, int reason, float distance), {
    if (Module.jkNetStats) Module.jkNetStats.hit(otherId, !!mine, kind, outcome, damage, flags, attack >>> 0, reason, distance);
});

// How player otherId was drawn from their buffer over the last second
// (stdComm_WSRelay_NoteMotion).
EM_JS(void, wsrelay_js_net_motion, (int otherId, double bufferMs, int steps, int carried, int held, int jumps, int warps, int restarts), {
    if (Module.jkNetStats && Module.jkNetStats.motion)
        Module.jkNetStats.motion(otherId, { buffer: bufferMs, steps: steps, carried: carried, held: held, jumps: jumps, warps: warps, restarts: restarts });
});

// Whether this game needs player otherId's clock (stdComm_WSRelay_SyncClock),
// and their clock less this one's, NaN if unknown.
EM_JS(void, wsrelay_js_net_sync, (int otherId, int on), {
    if (Module.jkNetStats && Module.jkNetStats.sync) Module.jkNetStats.sync(otherId, !!on);
});

EM_JS(double, wsrelay_js_net_clock, (int otherId), {
    return Module.jkNetStats && Module.jkNetStats.clockOffset ? Module.jkNetStats.clockOffset(otherId) : NaN;
});

// When this game sent player `to` its position update numbered seq
// (net.nextSeq), or -1 if it no longer knows.
EM_JS(double, wsrelay_js_seq_sent_at, (int to, uint32_t seq), {
    var net = Module.jkNet;
    return net ? net.seqSentAt(to, seq >>> 0) : -1;
});

// The clock net.nextSeq notes those times by.
EM_JS(double, wsrelay_js_now, (), {
    return performance.now();
});

// Pops one queued message into pOut. Returns its length, or -1 if the queue is
// empty. Text is NUL-terminated (maxLen includes the terminator). pSeq gets
// whether a game packet came on the fast lane, and its number (net.take).
EM_JS(int, wsrelay_js_recv, (void* pOut, int maxLen, int* pIsText, int* pSeq), {
    var net = Module.jkNet;
    if (!net || !net.queue.length) return -1;
    var data = net.queue.shift();
    HEAP32[pSeq >> 2] = 0;
    HEAP32[(pSeq >> 2) + 1] = 0;
    if (typeof data === 'string') {
        stringToUTF8(data, pOut, maxLen);
        HEAP32[pIsText >> 2] = 1;
        return Math.min(lengthBytesUTF8(data), maxLen - 1);
    }
    if (data.jkSeq !== undefined) {
        HEAP32[pSeq >> 2] = 1;
        HEAP32[(pSeq >> 2) + 1] = data.jkSeq | 0;
    }
    var bytes = new Uint8Array(data);
    var len = Math.min(bytes.length, maxLen);
    HEAPU8.set(bytes.subarray(0, len), pOut);
    HEAP32[pIsText >> 2] = 0;
    return len;
});

static uint8_t wsrelay_recvBuf[WSRELAY_MAX_RECV];
// Whether the packet in wsrelay_recvBuf came on the fast lane, and its number.
static int32_t wsrelay_aRecvSeq[2];
static uint8_t wsrelay_sendBuf[WSRELAY_MAX_PACKET];
static char wsrelay_textBuf[1024];

static int wsrelay_bHosting = 0;      // our room is open on the relay
static int wsrelay_bHostPending = 0;  // sent "host", waiting for "hosted"
static int wsrelay_bJoining = 0;      // sent "join", waiting for "welcome"
static int wsrelay_bJoined = 0;       // the host welcomed us into its room
static int wsrelay_bHostLeft = 0;      // the relay says the host left
static int wsrelay_bSeatLost = 0;      // the relay wouldn't give our seat back
static uint32_t wsrelay_msecRelayLost = 0; // when the relay went in a game, 0 while it's up
// Why the relay turned our join down: 1 for no room or anything else, 2
// full, 3 and 4 its host speaks a newer or an older netcode.
static int wsrelay_joinError = 0;
static int wsrelay_room = -1;
static uint32_t wsrelay_id = 0xFFFFFFFF;

static int wsrelay_aDisconnected[64];
static int wsrelay_numDisconnected = 0;

// What netstats.js was last told about the match (wsrelay_WatchMatch).
static int wsrelay_bNetLive = 0;
static uint32_t wsrelay_msecNamesSent = 0;

// Saber swings (stdComm_WSRelay_NewSwing): the one each player's blade was
// last turned on for, by player slot, and the last number this game gave.
static uint32_t wsrelay_aSwing[JKPLAYER_NUM_INFOS];
static uint32_t wsrelay_lastSwing = 0;
// When this game's own player last swung, on the motion clock.
static double wsrelay_ownSwingAt = 0.0;
int stdComm_WSRelay_fireGuid = 0;

// A host's players the relay says left while their data channel is still up.
// They count as gone once it goes down (or sithMulti times them out).
static int wsrelay_aLeftPending[64];
static int wsrelay_numLeftPending = 0;

// A host's players it welcomed who don't have a player slot yet, while they
// load the level. Their slots are spoken for (wsrelay_HasFreeSlot).
static int wsrelay_aWelcomed[64];
static int wsrelay_numWelcomed = 0;

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

// The hit rules each listed game's host plays by, as its entry says
// (wsrelay_EntryHits): rules | reach ms << 8, or -1 if it doesn't say.
static int wsrelay_aListHits[WSRELAY_MAX_LISTED];
static int wsrelay_aShownHits[WSRELAY_MAX_LISTED];

// The hit rules an entry says its host plays by, or -1 for an older host's,
// which plays by Classic.
static int wsrelay_EntryHits(const jkMultiEntry* pEntry, int* pReachMs)
{
    if (pEntry->guidInstance.c != STDCOMM_WSRELAY_HITS_ENTRY_MAGIC)
        return -1;
    int rules = (pEntry->guidInstance.d & 0xFF) == STDCOMM_WSRELAY_HITS_HWYS ? STDCOMM_WSRELAY_HITS_HWYS : STDCOMM_WSRELAY_HITS_CLASSIC;
    if (pReachMs)
        *pReachMs = stdComm_WSRelay_ClampReach((int)(pEntry->guidInstance.d >> 8));
    return rules;
}

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

// Only a host sends entries, each followed by its mod tail. Each says what
// hit rules the host plays by (STDCOMM_WSRELAY_HITS_ENTRY_MAGIC).
static void wsrelay_SendEntry(const char* pOp, int id, const jkMultiEntry* pEntry)
{
    char hex[sizeof(jkMultiEntry)*2 + 1];
    jkMultiEntry entry = *pEntry;
    int reachMs = 0;
    int rules = stdComm_WSRelay_GetHitRules(&reachMs);
    entry.guidInstance.c = STDCOMM_WSRELAY_HITS_ENTRY_MAGIC;
    entry.guidInstance.d = (uint32_t)rules | ((uint32_t)reachMs << 8);
    wsrelay_ToHex(&entry, sizeof(jkMultiEntry), hex);
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
    wsrelay_numWelcomed = 0;
    wsrelay_bNetLive = 0;
    wsrelay_msecNamesSent = 0;
    memset(wsrelay_aSwing, 0, sizeof(wsrelay_aSwing));
    stdComm_WSRelay_ResetHits();
    stdComm_WSRelay_ResetMotion();
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

static void wsrelay_Unwelcome(int id)
{
    for (int i = 0; i < wsrelay_numWelcomed; )
    {
        if (wsrelay_aWelcomed[i] == id)
            wsrelay_aWelcomed[i] = wsrelay_aWelcomed[--wsrelay_numWelcomed];
        else
            i++;
    }
}

// Whether a player welcomed now would get a slot. sithMulti only answers
// "Too many players" once a joiner has loaded the level, so without this the
// player past the last free slot loaded it all for "That game is full", and
// two players joining for the last slot both did, for one of them.
static int wsrelay_HasFreeSlot()
{
    // Players welcomed earlier who now have their slot aren't waiting for one.
    for (int i = 0; i < wsrelay_numWelcomed; )
    {
        int bSeated = 0;
        for (int j = 0; j < jkPlayer_maxPlayers; j++)
        {
            if ((jkPlayer_playerInfos[j].flags & 2) && jkPlayer_playerInfos[j].playerNetId == (uint32_t)wsrelay_aWelcomed[i])
            {
                bSeated = 1;
                break;
            }
        }
        if (bSeated)
            wsrelay_aWelcomed[i] = wsrelay_aWelcomed[--wsrelay_numWelcomed];
        else
            i++;
    }

    // The slots sithMulti gives joiners.
    int numFree = 0;
    for (int j = 0; j < jkPlayer_maxPlayers; j++)
    {
        if ((jkPlayer_playerInfos[j].flags & 2) && !jkPlayer_playerInfos[j].playerNetId)
            numFree++;
    }
    return numFree > wsrelay_numWelcomed;
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

    // Hack? Holds joins to the session's limit. Added: only ever lower it.
    // The level's player starts are the most it can take (sithPlayer_PlacePlayers),
    // and slots past them can still be marked as having a start from a bigger
    // level earlier, with no player thing behind them.
    if (wsrelay_storedEntry.maxPlayers > 0 && jkPlayer_maxPlayers > (uint32_t)wsrelay_storedEntry.maxPlayers)
        jkPlayer_maxPlayers = wsrelay_storedEntry.maxPlayers;

    // Added: with no slot left, a player count at the limit, which the
    // joiner takes as full before loading anything (stdComm_Open). Whoever
    // had this id before is gone, waiting or not.
    wsrelay_Unwelcome(id);
    if (!wsrelay_HasFreeSlot())
    {
        stdPlatform_Printf("OpenJKDF2: No player slot left for client id %x; it's told the game is full.\n", id);
        entry.numPlayers = entry.maxPlayers;
    }
    else if (wsrelay_numWelcomed < 64)
    {
        wsrelay_aWelcomed[wsrelay_numWelcomed++] = id;
    }

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
    wsrelay_Unwelcome(id);

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

        int reachMs = 0;
        int rules = wsrelay_EntryHits(pEntry, &reachMs);
        wsrelay_aListHits[n] = rules < 0 ? -1 : (rules | (reachMs << 8));

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

            // The host's hit rules (its hello says so again in the match).
            int reachMs = STDCOMM_WSRELAY_REACH_DEFAULT;
            int rules = wsrelay_EntryHits(&entry, &reachMs);
            stdComm_WSRelay_SetHitRules(rules < 0 ? STDCOMM_WSRELAY_HITS_CLASSIC : rules, reachMs);

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
        char* pDetail = strtok_r(NULL, " ", &pSave);
        stdPlatform_Printf("OpenJKDF2: Relay error: %s%s%s\n", pCode ? pCode : "?", pDetail ? " " : "", pDetail ? pDetail : "");
        if (wsrelay_bJoining)
        {
            if (pCode && !strcmp(pCode, "full"))
                wsrelay_joinError = 2;
            else if (pCode && !strcmp(pCode, "version"))
                wsrelay_joinError = (pDetail && !strcmp(pDetail, "older")) ? 4 : 3;
            else
                wsrelay_joinError = 1;
        }
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
    int32_t aSeq[2];
    int len;
    while ((len = wsrelay_js_recv(wsrelay_recvBuf, sizeof(wsrelay_recvBuf), &isText, aSeq)) >= 0)
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

    wsrelay_js_connect(STDCOMM_WSRELAY_NETCODE);
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
    wsrelay_js_connect(STDCOMM_WSRELAY_NETCODE);
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

int stdComm_WSRelay_RecvSeq(uint32_t* pSeq)
{
    *pSeq = (uint32_t)wsrelay_aRecvSeq[1];
    return wsrelay_aRecvSeq[0];
}

double stdComm_WSRelay_SeqSentAt(int toId, uint32_t seq)
{
    return wsrelay_js_seq_sent_at(toId, seq);
}

double stdComm_WSRelay_Now()
{
    return wsrelay_js_now();
}

void stdComm_WSRelay_SyncClock(int netId, int bOn)
{
    if (netId)
        wsrelay_js_net_sync(netId, bOn);
}

double stdComm_WSRelay_ClockOffset(int netId)
{
    return netId ? wsrelay_js_net_clock(netId) : NAN;
}

int stdComm_WSRelay_InMatch()
{
    if ((!wsrelay_bHosting && !wsrelay_bJoined) || !sithNet_isMulti || !sithWorld_g_pCurrentWorld || !sithPlayer_g_pLocalPlayerThing)
        return 0;

    // A joiner is in once the host has welcomed it into the level
    // (sithMulti_ProcessWelcome clears this). Before that, the host would
    // drop it for sending anything (sithMessage_ProcessMessages).
    return stdComm_bIsServer || !(g_submodeFlags & 8);
}

// Tells netstats.js when this game is in a match, which is when it pings the
// other players and keeps the hit ledger, and who they are.
static void wsrelay_WatchMatch()
{
    int live = stdComm_WSRelay_InMatch();
    if (live != wsrelay_bNetLive)
    {
        wsrelay_bNetLive = live;
        wsrelay_js_net_live(live, stdComm_bIsServer ? 1 : (int)wsrelay_id, stdComm_bIsServer);
    }
    if (!live)
        return;

    // Players come and go, and can change their names.
    uint32_t now = stdPlatform_GetTimeMsec();
    if (wsrelay_msecNamesSent && now - wsrelay_msecNamesSent < 2000)
        return;
    wsrelay_msecNamesSent = now ? now : 1;
    for (int i = 0; i < jkPlayer_maxPlayers; i++)
    {
        if ((jkPlayer_playerInfos[i].flags & 1) && jkPlayer_playerInfos[i].playerNetId)
            wsrelay_js_net_name(jkPlayer_playerInfos[i].playerNetId, jkPlayer_playerInfos[i].player_name);
    }
}

static int wsrelay_PlayerSlot(SithThing* pPlayer)
{
    if (!pPlayer || pPlayer->type != SITH_THING_PLAYER || !pPlayer->actorParams.pPlayer)
        return -1;
    intptr_t slot = pPlayer->actorParams.pPlayer - jkPlayer_playerInfos;
    return (slot >= 0 && slot < JKPLAYER_NUM_INFOS) ? (int)slot : -1;
}

void stdComm_WSRelay_NewSwing(SithThing* pPlayer)
{
    int slot = wsrelay_PlayerSlot(pPlayer);
    if (slot < 0)
        return;
    // Never 0, which stands for a swing without a number.
    if (!++wsrelay_lastSwing)
        wsrelay_lastSwing = 1;
    wsrelay_aSwing[slot] = wsrelay_lastSwing;
    if (pPlayer == sithPlayer_g_pLocalPlayerThing)
        wsrelay_ownSwingAt = stdComm_WSRelay_MotionNow();
}

double stdComm_WSRelay_OwnSwingAt()
{
    return wsrelay_ownSwingAt;
}

void stdComm_WSRelay_SetSwing(SithThing* pPlayer, uint32_t swing)
{
    int slot = wsrelay_PlayerSlot(pPlayer);
    if (slot >= 0)
        wsrelay_aSwing[slot] = swing;
}

uint32_t stdComm_WSRelay_GetSwing(SithThing* pPlayer)
{
    int slot = wsrelay_PlayerSlot(pPlayer);
    return slot >= 0 ? wsrelay_aSwing[slot] : 0;
}

// What the hit ledger hears about a hit (wsrelay_js_net_hit): it was decided
// by a message from the attacker's game (targeted Force), it was a claim
// (stdComm_WSRelayHits.c), and this game's block against a player it claims
// hits on, which is final.
#define WSRELAY_HITFLAG_BY_MESSAGE (1)
#define WSRELAY_HITFLAG_BY_CLAIM (2)
#define WSRELAY_HITFLAG_DECIDED (4)
// A claim that named the moment the target was drawn at, and one whose
// moment was further back than the reach (STDCOMM_WSRELAY_MOMENT_*).
#define WSRELAY_HITFLAG_MOMENT (8)
#define WSRELAY_HITFLAG_PAST_REACH (16)

static int wsrelay_MomentFlags(int moment)
{
    if (moment == STDCOMM_WSRELAY_MOMENT_PAST_REACH)
        return WSRELAY_HITFLAG_MOMENT | WSRELAY_HITFLAG_PAST_REACH;
    return moment == STDCOMM_WSRELAY_MOMENT_AT ? WSRELAY_HITFLAG_MOMENT : 0;
}

int stdComm_WSRelay_HitKind(SithThing* pSource, int damageType)
{
    if (damageType & SITH_DAMAGE_SABER)
        return STDCOMM_WSRELAY_KIND_SABER;
    if (pSource->type == SITH_THING_EXPLOSION)
        return STDCOMM_WSRELAY_KIND_SPLASH;
    if (damageType & SITH_DAMAGE_FORCE)
        return STDCOMM_WSRELAY_KIND_FORCE;
    if (pSource->type == SITH_THING_WEAPON)
        return STDCOMM_WSRELAY_KIND_SHOT;
    return STDCOMM_WSRELAY_KIND_OTHER;
}

void stdComm_WSRelay_NoteHit(SithThing* pVictim, SithThing* pSource, flex_t damage, int damageType, int outcome)
{
    // Whoever fired the shot, set off the explosion or swung the saber.
    if (pSource)
        stdComm_WSRelay_NoteHitBy(pVictim, sithThing_GetThingParent(pSource), pSource, damage, damageType, outcome);
}

void stdComm_WSRelay_NoteHitBy(SithThing* pVictim, SithThing* pAttacker, SithThing* pSource, flex_t damage, int damageType, int outcome)
{
    SithThing* pLocal = sithPlayer_g_pLocalPlayerThing;
    if (!wsrelay_bNetLive || !pLocal || !pVictim || !pSource || pVictim->type != SITH_THING_PLAYER)
        return;
    if (!pAttacker || pAttacker == pVictim || pAttacker->type != SITH_THING_PLAYER)
        return;

    // Only this game's own player's hits on others, which it shows, and hits
    // on its player, which it decides.
    int mine = (pAttacker == pLocal);
    if (!mine && pVictim != pLocal)
        return;

    SithThing* pOther = mine ? pVictim : pAttacker;
    int otherId = pOther->actorParams.pPlayer ? (int)pOther->actorParams.pPlayer->playerNetId : 0;
    if (!otherId)
        return;

    // A targeted Force power this game checked is in the ledger already
    // (stdComm_WSRelay_ForceTakes), not the damage it does.
    if (!mine && stdComm_WSRelay_ApplyingForce())
        return;

    int flags = 0;
    int claimKind = 0;
    uint32_t claimAttack = 0;
    float distance = -1.0f;
    int claimMoment = STDCOMM_WSRELAY_MOMENT_NONE;
    int bApplying = stdComm_WSRelay_ApplyingClaim(&claimKind, &claimAttack, &distance, &claimMoment);
    if (!mine && !bApplying && stdComm_WSRelay_DecidedElsewhere(pSource, damageType))
    {
        // Hit what you see: the attacker's game decides this one, and its
        // claim gets the receipt. What happens here only shows.
        return;
    }
    if (mine && outcome == STDCOMM_WSRELAY_BLOCK && stdComm_WSRelay_ClaimsTo(pVictim))
    {
        // And this game's blocks against a player it claims hits on are
        // final: no receipt comes for them.
        flags |= WSRELAY_HITFLAG_DECIDED;
    }

    // A player who's dead in this game shows nothing. The victim's game says
    // so, for hits its player took after dying there.
    if (pVictim->flags & (SITH_TF_DISABLED|SITH_TF_DEAD|SITH_TF_DESTROYED))
    {
        if (mine)
            return;
        outcome = STDCOMM_WSRELAY_DEAD;
    }

    int kind = stdComm_WSRelay_HitKind(pSource, damageType);

    // Targeted Force powers (SkillTarget) reach the victim's game as a cog
    // message, and the host can damage anyone with DSS_DAMAGE. The message
    // decided those, not this game's simulation.
    if (sithComm_msgInProgress == DSS_SENDTRIGGER || sithComm_msgInProgress == DSS_DAMAGE)
        flags |= WSRELAY_HITFLAG_BY_MESSAGE;

    // The attack, by a number both games know: the saber swing, or the shot's
    // id. 0 for the rest, which the ledger matches by time instead.
    uint32_t attack = 0;
    if (kind == STDCOMM_WSRELAY_KIND_SABER)
        attack = stdComm_WSRelay_GetSwing(pAttacker);
    else if (kind == STDCOMM_WSRELAY_KIND_SHOT)
        attack = (uint32_t)(stdComm_WSRelay_fireGuid ? stdComm_WSRelay_fireGuid : pSource->guid);

    // A claim's damage, which hit this game's player by the attacker's
    // account: the claim says what it was.
    if (bApplying)
    {
        kind = claimKind;
        attack = claimAttack;
        flags = WSRELAY_HITFLAG_BY_CLAIM | wsrelay_MomentFlags(claimMoment);
    }

    wsrelay_js_net_hit(otherId, mine, kind, outcome, (float)damage, flags, attack, 0, distance);
}

void stdComm_WSRelay_NoteTurnedDown(int otherId, int kind, int outcome, flex_t damage, uint32_t attack, int reason, float distance, int moment)
{
    if (!wsrelay_bNetLive || !otherId)
        return;
    wsrelay_js_net_hit(otherId, 0, kind, outcome, (float)damage, WSRELAY_HITFLAG_BY_CLAIM | wsrelay_MomentFlags(moment), attack, reason, distance);
}

void stdComm_WSRelay_NoteTargeted(int otherId, int outcome, int reason, float distance, int moment)
{
    if (!wsrelay_bNetLive || !otherId)
        return;
    wsrelay_js_net_hit(otherId, 0, STDCOMM_WSRELAY_KIND_FORCE, outcome, 0.0f,
                       WSRELAY_HITFLAG_BY_MESSAGE | WSRELAY_HITFLAG_BY_CLAIM | wsrelay_MomentFlags(moment), 0, reason, distance);
}

void stdComm_WSRelay_NoteMotion(int otherId, double bufferMs, uint32_t steps, uint32_t carried, uint32_t held,
                                uint32_t jumps, uint32_t warps, uint32_t restarts)
{
    if (!wsrelay_bNetLive || !otherId)
        return;
    wsrelay_js_net_motion(otherId, bufferMs, (int)steps, (int)carried, (int)held, (int)jumps, (int)warps, (int)restarts);
}

int DirectPlay_Receive(int *pIdOut, int *pMsgIdOut, int *pLenOut)
{
    int maxLen = *pLenOut;
    *pIdOut = 0;
    *pLenOut = 0;

    Hack_ResetClients();

    wsrelay_WatchRelay();
    wsrelay_WatchMatch();
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
        int len = wsrelay_js_recv(wsrelay_recvBuf, sizeof(wsrelay_recvBuf), &isText, wsrelay_aRecvSeq);
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

// This player's own position update, which sithMulti_CheckPlayers sends to
// each player about 50 times a second: DSS_THINGPOS for its own thing without
// a resend id, 52 or 64 bytes (sithDSSThing_Pos adds the head pitch for
// players), or 60 or 72 with its time (stdComm_WSRelayMotion.c). Positions of
// other things, and those a cog sends with a resend id (a teleport), stay on
// the reliable channel.
static int wsrelay_IsOwnPosition(const void* pData, uint32_t size)
{
    const uint8_t* p = (const uint8_t*)pData;
    if (size != 4 + 52 && size != 4 + 64 && size != 4 + 60 && size != 4 + 72)
        return 0;
    if (*(const uint16_t*)&p[0] != DSS_THINGPOS || *(const uint16_t*)&p[2] != 0)
        return 0;
    return sithPlayer_g_pLocalPlayerThing && *(const int32_t*)&p[4] == sithPlayer_g_pLocalPlayerThing->guid;
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

    // A joiner's packets go to the host, which forwards them (net.route).
    int ret = wsrelay_js_send_packet(wsrelay_sendBuf, dwDataSize+8, idTo, wsrelay_IsOwnPosition(lpData, dwDataSize));

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
        if (err == 3 || err == 4)
            return (err == 3) ? STDCOMM_WSRELAY_NEWER : STDCOMM_WSRELAY_OLDER;
        return (err == 2) ? 0x8877014A : 0x80004005; // GUINET_GAMEFULL / GUINET_NOGAMECONNECT
    }

    // Added: the host has no player slot for us (wsrelay_WelcomePeer). Told
    // now, before the level loads, and not by sithMulti after it.
    if (wsrelay_storedEntryEnum.maxPlayers > 0 && wsrelay_storedEntryEnum.numPlayers >= wsrelay_storedEntryEnum.maxPlayers)
    {
        stdPlatform_Printf("OpenJKDF2: Room %d is full.\n", wsrelay_aShownRooms[idx]);
        wsrelay_SendText("leave");
        wsrelay_ResetSession();
        return 0x8877014A; // GUINET_GAMEFULL
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
    // Added: jkMain passes the smaller of the host's limit and the level's
    // player starts, which is what JK.EXE's DirectPlay session showed and held
    // joins to. The lobby used to get the host's own number, so a 16-start map
    // hosted for 32 read "3/32" and turned the 17th player away.
    if (maxPlayers > 0 && maxPlayers <= JKPLAYER_NUM_INFOS)
        wsrelay_storedEntry.maxPlayers = maxPlayers;
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

    // The host's choice of hit rules, which its entry lists and every game in
    // its match plays by.
    stdComm_WSRelay_SetHitRules(stdComm_WSRelay_bClassicHits ? STDCOMM_WSRELAY_HITS_CLASSIC : STDCOMM_WSRELAY_HITS_HWYS,
                                stdComm_WSRelay_hitReachMs);

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
        wsrelay_js_connect(STDCOMM_WSRELAY_NETCODE);
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
        wsrelay_aShownHits[i] = wsrelay_aListHits[i];

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

void stdComm_WSRelay_DescribeHits(int idx, int bShort, char16_t* pOut, int maxChars)
{
    char text[64];
    text[0] = 0;
    if (idx >= 0 && idx < wsrelay_numShown)
    {
        // An older host's entry doesn't say: it plays by Classic.
        int hits = wsrelay_aShownHits[idx];
        if (hits >= 0 && (hits & 0xFF) == STDCOMM_WSRELAY_HITS_HWYS)
            snprintf(text, sizeof(text), bShort ? "Hit what you see" : "Hit rules: Hit what you see (%d ms)", hits >> 8);
        else
            snprintf(text, sizeof(text), bShort ? "Classic hits" : "Hit rules: Classic");
    }

    int i = 0;
    for (; text[i] && i < maxChars - 1; i++)
        pOut[i] = (char16_t)(uint8_t)text[i];
    pOut[i] = 0;
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
