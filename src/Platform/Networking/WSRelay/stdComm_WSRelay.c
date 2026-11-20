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
//   us -> relay   list <game> | host <entry> | update <entry> | join <room> [<password>]
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
// that: links (signal between clients), turn (TURN credentials) and pw (it
// checks room passwords). The JS below handles signal, ice and lobby itself;
// the engine never sees them.
//
// Watching a match: a lobby that says feed (jkdf2mp-bb-server's src/feed.js)
// takes a feed for each watched room's match. A game in a room says feed;
// while the room's match is watched the lobby answers feed <room> <token>,
// and the game sends a copy of every message it sends everyone
// (stdComm_SendToPlayer) to /feed/<room>?up=<token>, batched every
// FEED_BATCH_MS, until the lobby says feed off or the game leaves. The JS
// handles feed itself. No keyframes yet: this is the first step, to measure
// what a match sends.
//
// Room passwords: this game tells the lobby it can send them (/relay?pw=1).
// A lobby that says pw then lists no passwords and checks the one the player
// typed, which goes with join as the hex of its UTF-16LE units, for a game
// listed with the password box (sessionFlags 0x400) when something was
// typed: error password if it's wrong, error password wait after too many
// wrong ones. Any other join is bare, so what's left in the box from another
// game stays here. With a lobby that doesn't say pw, the join screen checks
// the password against the list, as it always did, and join goes without
// it. Which a game was listed by is noted as its list comes in, so a socket
// that comes back meanwhile doesn't change what joining it sends.
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
// file. When this game leaves a match, or its tab closes in one, the JS posts
// netstats.js's summary of the match to the lobby's address (/summary): counts
// only, with no names (net.sendSummary).
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
// It doesn't send passwords to the lobby either, so it joins as older games
// do.

#include "stdComm_WSRelay.h"
#include "Platform/Networking/Imperial/stdComm_Imperial.h"

#include "Win95/stdComm.h"
#include "Dss/sithMulti.h"
#include "Devices/sithComm.h"
#include "Devices/sithControl.h"
#include "Devices/sithConsole.h"
#include "World/sithThing.h"
#include "Main/Main.h"
#include "General/stdString.h"
#include "stdPlatform.h"
#include "version.h"
#include "jk.h"
#include "Gui/jkGUITitle.h"
#include "World/sithWorld.h"
#include "General/stdFnames.h"
#include "General/stdStrTable.h"
#include "Main/jkStrings.h"
#include "Main/jkRes.h"
#include "Main/jkPlaylist.h"
#include "World/jkPlayer.h" // Added: what a player holds and sees (spectator mode)
#include "World/sithModel.h"
#include "Engine/sithKeyFrame.h"
#include "Engine/sithPuppet.h"
#include "Engine/sithCamera.h"
#include "Gameplay/sithPlayer.h"
#include "Gameplay/sithTime.h"
#include "General/stdPalEffects.h"
#include "General/stdMath.h"
#include "Main/jkHud.h"

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
                               log: [], lastFrom: {}, playedAt: {}, sentPlayAt: {}, seat: null, entry: "", rejoinMs: 1000, relayPosAt: {},
                               seqTo: {}, seqSent: {}, seqFrom: {}, links: {}, lobbyCan: [], listPw: false, pending: {},
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

        /* A tab closed in a game sends its match summary, gives up its seat
           at once (a socket that just closes keeps it for a while) and closes
           its data channels, so the others don't wait on it. */
        if (typeof window !== 'undefined') window.addEventListener('pagehide', function() {
            if (!net.session) return;
            net.sendSummary();
            if (net.state === 1) try { net.ws.send('leave'); } catch (e) {}
            net.closeAll();
        });

        /* Where the match summary goes: /summary on the lobby's address. */
        net.summaryUrl = function() {
            var url = Module['jkSummaryUrl'] || Module['jkRelayUrl'];
            if (!url && typeof location !== 'undefined')
                url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/relay';
            try {
                var u = new URL(url);
                if (!Module['jkSummaryUrl']) {
                    u.protocol = u.protocol === 'wss:' ? 'https:' : u.protocol === 'ws:' ? 'http:' : u.protocol;
                    u.pathname = u.pathname.substring(0, u.pathname.lastIndexOf('/') + 1) + 'summary';
                }
                u.search = "";
                u.hash = "";
                return u.href;
            } catch (e) {
                return null;
            }
        };

        /* netstats.js's summary of the match this game is leaving (once a
           match, and none for one too short to say anything), with
           sendBeacon, which still goes out as the tab closes. The lobby keeps
           it for reading back later; nothing here waits for an answer. */
        net.sendSummary = function() {
            var stats = Module.jkNetStats;
            var s = stats && stats.summary ? stats.summary() : null;
            if (!s) return false;
            var url = net.summaryUrl(), body = JSON.stringify(s), sent = false;
            if (url && typeof navigator !== 'undefined' && navigator.sendBeacon) {
                try { sent = navigator.sendBeacon(url, body); } catch (e) {}
            }
            if (!sent && url && typeof fetch !== 'undefined') {
                try { fetch(url, { method: 'POST', body: body, keepalive: true }).catch(function() {}); sent = true; } catch (e) {}
            }
            net.note('match summary ' + (sent ? 'sent' : 'not sent') + ' (' + body.length + ' bytes)');
            return sent;
        };

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
            mc.bufferedAmountLowThreshold = 262144;
            mc.onbufferedamountlow = function() { net.pumpMaps(); };
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
            // Map parts for its links; the joiner asks again through the lobby.
            if (Module.jkMaps && Module.jkMaps.forget) Module.jkMaps.forget(+id, true);
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
            net.heardPlaying(id, data);
            net.pumpMaps();
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
            net.playedAt[id] = net.lastFrom[id];
            net.pumpMaps();
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

        /* Another joiner's packet came: says hello to it the first time, and
           makes a try at a link that fell due while nothing came from them
           (net.linkOffer). */
        net.heardFrom = function(from) {
            var l = net.links[from], now = performance.now();
            if (l) {
                l.heardAt = now;
                if (l.due) {
                    l.due = false;
                    net.linkOffer(from);
                }
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
            if (!l || net.peers[id] || now < l.retryAt || l.tries >= 4 || !net.linksOn()) return;
            // Nothing from them for 5 s (loading a level, or stalled): this
            // try goes when they're heard again (net.heardFrom). It used to be
            // dropped, and the pair stayed on the way through the host.
            if (now - l.heardAt > 5000) {
                l.due = true;
                return;
            }
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
                    p.posDropped = (p.posDropped || 0) + 1;
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
            // The lobby closes a socket that sends it more than 64 KiB at
            // once, and a host's ends its room: a part cut for the map channel
            // that closed is left for the joiner to ask for again.
            if (net.state !== 1 || bytes.byteLength > 65536) return null;
            net.ws.send(bytes);
            return 'relay';
        };

        net.mapDirect = function(id) {
            var p = net.peers[id];
            return !!(p && p.open && p.mc.readyState === 'open');
        };

        /* What waits to go to player id the way a map frame sent now would
           go (maps.js sends while it's little). */
        net.mapBacklog = function(id, via) {
            var n = net.bulkNumber(via);
            if (n) {
                var l = (net.bulk[id] || {})[n];
                return l ? l.mc.bufferedAmount : 0;
            }
            if (via !== 'relay' && net.mapDirect(id)) return net.peers[id].mc.bufferedAmount;
            return net.state === 1 ? net.ws.bufferedAmount || 0 : 0;
        };

        /* A host is in a match while a player other than id (who
           downloads, and sends no game packets meanwhile) plays: a game
           packet came from them in the last 2 s, or one came in the last
           minute and the host's game still sends them its own (it does each
           frame while they're in it, net.sentPlayAt). A tab that stalls goes
           quiet for seconds, and the uploads went at once meanwhile
           (pacing-test.mjs); the host's game sends to it until it drops the
           player, and 10 s covers a frame of the host's own that long, after
           which the first look comes before the frame's sends. A joiner that
           downloads before anyone plays gets nothing from the host's game,
           which doesn't know it yet: its uploads still go at once. Uploads to
           id go paced in a match, so they don't hold up the others' game. */
        net.mapInMatch = function(id) {
            if (!net.isHost) return false;
            var now = performance.now();
            for (var k in net.playedAt) {
                if (+k === id) continue;
                var heard = now - net.playedAt[k];
                if (heard < 2000 || (heard < 60000 && now - net.sentPlayAt[k] < 10000)) return true;
            }
            return false;
        };

        /* A packet from player id that the engine sends: not a link's own
           frame or netstats.js's, which come while links are set up and
           players download, before anyone plays. */
        net.heardPlaying = function(id, data) {
            if (data.byteLength >= 12) {
                var b = new Uint8Array(data, 8, 4);
                if (b[0] === 0x4A && (b[1] === 0x4C || b[1] === 0x4E) && b[2] === 0 && b[3] === 0) return;
            }
            net.playedAt[id] = performance.now();
        };

        /* Whether a queue is building on the way out (true), isn't (false),
           or there's no telling (null), for maps.js's rate in a match. What
           the browser has waiting doesn't show what waits in the computer or
           the router, but the lobby's answer to ping does: it comes back
           behind everything sent before the ping on the same connection
           (lobby.js answers it without waking up; relay.py never does).
           Direct links have their own signs: game packets waiting in a data
           channel, position updates dropped behind a backlog, and
           netstats.js's round trips where it pings. A player's own slow or
           jittery link shows the same signs, so those of the players in the
           match count only where most of them show them. Without the lobby's
           answer, they say nothing about what goes through the lobby (lobby).
           Asked at most every 250 ms, never right after a send, which what's
           waiting would count. Of the answer's round trip and of the wait
           for the one out, only what the page can't have held up counts
           (net.heldFrom), against the shortest round trip as it was read: a
           page at 2 fps took answers up to a second late, which looked like
           a queue at every look. net.mapWhy: which sign said so, or how the
           lobby's answers come, for the network log (maps.js). */
        net.mapCongested = function(lobby) {
            var now = performance.now(), lp = net.lobbyPing, queued = false, seen = false, why = [], lobbyWhy = '';
            if (net.state === 1 && lp.got) {
                seen = true;
                var waited = lp.sent.length ? net.heldFrom(now) - lp.sent[0] : 0;
                lobbyWhy = 'lobby round trip ' + Math.round(lp.rtt) + ' ms, at least ' + Math.round(lp.low) + ' of it on the way (shortest ' +
                           Math.round(lp.base) + ')' + (waited > lp.base + 80 ? ', the next out at least ' + Math.round(waited) + ' ms' : '');
                if (lp.low > lp.base + 80 || waited > lp.base + 80) {
                    queued = true;
                    why.push(lobbyWhy);
                }
            }
            if (net.state === 1 && !lp.sent.length) net.pingLobby();
            var playing = 0, backed = 0;
            for (var id in net.peers) {
                var p = net.peers[id];
                if (!p.open) continue;
                seen = true;
                /* Not what waits in a map channel or a transfer link: that's
                   mostly the downloading player's tab slow to take the data,
                   which nobody else's packets wait behind, and maps.js sends
                   it no more while it's full (net.mapBacklog). */
                var dropped = p.posDropped || 0, was = p.posDroppedSeen || 0;
                p.posDroppedSeen = dropped;
                if (!(now - net.playedAt[id] < 2000)) continue;
                playing++;
                if (p.dc.bufferedAmount > 4096 || (p.pos && p.pos.bufferedAmount > 4096) || dropped !== was) backed++;
            }
            if (backed * 2 > playing) {
                queued = true;
                why.push(backed + ' of ' + playing + ' players\' links backed up');
            }
            var ns = Module.jkNetStats && Module.jkNetStats.queueMs ? Module.jkNetStats.queueMs() : null;
            if (ns !== null) {
                seen = true;
                if (ns > 100) {
                    queued = true;
                    why.push('netstats round trips ' + Math.round(ns) + ' ms over the shortest');
                }
            }
            net.mapWhy = why.length ? why.join('; ') : lobbyWhy;
            if (queued) return true;
            if (lobby && !lp.got) return null;
            return seen ? false : null;
        };

        /* The lobby's ping and its answer: the times the pings not yet
           answered went, the last round trip, the shortest, and the least
           the connection took for the last (low: the rest the page may have
           held it up, net.heldFrom), and how many more to send as each
           answer comes (net.pingBase). */
        net.newLobbyPing = function() {
            net.lobbyPing = { sent: [], rtt: 0, base: 0, low: 0, got: 0, more: 0 };
        };
        net.newLobbyPing();
        net.pingLobby = function() {
            var lp = net.lobbyPing;
            if (net.state !== 1) return;
            // A lobby that doesn't answer (relay.py) isn't asked again but
            // for the keepalive.
            if (!lp.got && lp.sent.length >= 8) lp.sent.shift();
            net.ws.send('ping');
            lp.sent.push(performance.now());
        };
        net.onPong = function() {
            var lp = net.lobbyPing;
            if (!lp.sent.length) return;
            var now = performance.now(), sent = lp.sent.shift(), rtt = now - sent;
            lp.base = lp.got ? Math.min(lp.base, rtt) : rtt;
            lp.rtt = rtt;
            lp.low = Math.max(0, Math.min(rtt, net.heldFrom(now) - sent));
            lp.got++;
            if (lp.more > 0 && !lp.sent.length) {
                lp.more--;
                net.pingLobby();
            }
        };
        /* The shortest round trip is the shortest of a few, each sent as the
           last comes back: an answer the page was too busy to take for a
           while (starting up, a level loading, a slow frame) looks as long
           as a queue would, and as the shortest it would hide one that long. */
        net.pingBase = function() {
            net.lobbyPing.more = 3;
            if (!net.lobbyPing.sent.length) net.pingLobby();
        };
        /* The earliest an answer read at t came to the page, or one not read
           by t hasn't come yet, as far as the host's game frames tell
           (wsrelay_js_frame); t itself without them. */
        net.heldFrom = function(t) {
            var h = Module.jkHeld;
            return h ? h.from(t) : t;
        };

        /* maps.js sends what waits for joiners now. */
        net.pumpMaps = function() {
            if (net.isHost && Module.jkMaps && Module.jkMaps.pump) Module.jkMaps.pump();
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
            mc.bufferedAmountLowThreshold = 262144;
            mc.onbufferedamountlow = function() { net.pumpMaps(); };
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
                // In a match a joiner downloads over the map channel alone,
                // which keeps the blocks out at once few enough for the paced
                // upload to keep each of them coming (maps.js).
                if (net.mapInMatch(from)) return;
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
            if (op === 'pong') {
                net.onPong();
                return true;
            }
            /* The lobby saved a match this game reported (ladder.js). */
            if (op === 'matched') {
                if (Module.jkLadder) Module.jkLadder.saved(parseInt(text.substring(sp + 1), 10));
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
               setup between joiners), pw (checking room passwords), match
               (keeping a record of each match, ladder.js). */
            if (op === 'lobby') {
                net.lobbyCan = text.split(' ').slice(1);
                return true;
            }
            /* Whether the lobby a list came from checks passwords, as that
               socket's lobby line said (it comes before any list). Noted as
               the list comes in: a socket that comes back has no lobby line
               until its own arrives. The engine keeps it for each game in the
               list (wsrelay_js_list_pw). ?netold didn't ask for it (pw=1). */
            if (op === 'list') net.listPw = !Module['jkNetOld'] && net.lobbyCan.indexOf('pw') >= 0;
            /* The seat to take back if the socket drops in a game. */
            if (op === 'seat') {
                var seat = text.split(' ');
                net.seat = { room: parseInt(seat[1], 10), id: parseInt(seat[2], 10), key: seat[3] || "" };
                if (net.seat.id > 1) net.askIce();
                net.askFeed();
                return true;
            }
            /* The room's feed for pages watching its match: where to send,
               or to stop. */
            if (op === 'feed') {
                net.onFeed(text.split(' '));
                return true;
            }
            if (op === 'rejoined' || op === 'rehosted') net.askFeed();
            /* A player whose data channel is still up is still playing: the
               engine decides when they're gone (DirectPlay_Receive). */
            var gone = op === 'peer-left' ? net.peers[parseInt(text.substring(sp + 1), 10)] : net.peers[1];
            if ((op === 'peer-left' || op === 'host-left') && !(gone && gone.open)) {
                if (op === 'peer-left') net.closePeer(parseInt(text.substring(sp + 1), 10));
                else net.closeAll();
            }
            /* Map parts waiting for whoever had the id are dropped. */
            if (((op === 'peer-left' && !(gone && gone.open)) || op === 'peer-joined') && Module.jkMaps && Module.jkMaps.forget)
                Module.jkMaps.forget(parseInt(text.substring(sp + 1), 10));
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

        /* Watching a match: this game's copy of what it sends everyone, to
           its room's feed while the lobby says (feed <room> <token>), one
           binary chunk every FEED_BATCH_MS: a 0 (packets), then each packet
           as u16 length (LE) and its bytes from cogMsgId on. The host's game
           also sends keyframes: the world as a player joining now would be
           sent it (sithMulti_FeedKeyframe), as a chunk of kind 1 and, past
           FEED_MAX_CHUNK, more of kind 2. One goes once the feed is open, at
           each new level, every FEED_KEY_MS, and when the feed asks ("key":
           a page is waiting for one). Its first packet is JKF1 and a line of
           JSON on the level (wsrelay_js_feed_key_begin), its last JKF1 and
           {"v":1,"end":true}. Chunks queue in
           order and at most FEED_SENDS go a tick: a lobby's feed takes at
           most 64 KiB a chunk and 20 chunks a second. Every minute the
           network log says how much went, to measure a match's feed. */
        var FEED_BATCH_MS = 250, FEED_MAX_CHUNK = 60000, FEED_LOG_MS = 60000, FEED_RETRIES = 6, FEED_RETRY_MAX_MS = 8000;
        var FEED_KEY_MS = 60000, FEED_KEY_ASK_MS = 5000, FEED_KEY_MAX = 4 * 1024 * 1024, FEED_SENDS = 4, FEED_QUEUE_MAX = 400;
        var FEED_SAID_MAX = 8;
        net.feed = null;
        net.askFeed = function() {
            if (Module['jkNetOld'] || net.state !== 1 || net.lobbyCan.indexOf('feed') < 0) return;
            net.ws.send('feed');
        };
        net.onFeed = function(parts) {
            if (parts[1] === 'off') {
                net.feedStop('the lobby stopped it');
                return;
            }
            var room = parseInt(parts[1], 10), token = parts[2] || '';
            if (!(room > 0) || !/^[0-9a-f]{32}$/.test(token)) return;
            /* The feed is beside the lobby: wss://<host>/feed/<room>. */
            var base = (net.ws && net.ws.url) || Module['jkRelayUrl'] || '';
            var cut = base.lastIndexOf('/relay');
            var url = (cut >= 0 ? base.substring(0, cut) : base) + '/feed/' + room + '?up=' + token;
            if (net.feed && net.feed.url === url) return;
            net.feedStop();
            net.feed = { url: url, ws: null, parts: [], size: 0, out: [], chunks: 0, bytes: 0, packets: 0, keys: 0,
                         key: null, keyWant: true, keyAt: 0, keyAskAt: 0, said: [],
                         since: performance.now(), timer: setInterval(function() { net.feedFlush(); }, FEED_BATCH_MS), tries: 0 };
            net.feedOpen(net.feed);
        };
        net.feedOpen = function(feed) {
            var ws;
            try {
                ws = new WebSocket(feed.url);
            } catch (e) {
                net.note('feed: ' + e);
                return;
            }
            ws.binaryType = 'arraybuffer';
            feed.ws = ws;
            ws.onopen = function() {
                feed.tries = 0;
                feed.keyWant = true;
                net.note('feed: sending to ' + feed.url.split('?')[0]);
            };
            ws.onmessage = function(ev) {
                var now = performance.now();
                if (ev && ev.data === 'key' && now - feed.keyAskAt >= FEED_KEY_ASK_MS) {
                    feed.keyAskAt = now;
                    feed.keyWant = true;
                }
                /* A game watching hasn't got this host's mod: mods.js puts
                   it on the feed (feedUp), at its https address. */
                var mod = ev && typeof ev.data === 'string' ? /^mod ([0-9a-f]{64})$/.exec(ev.data) : null;
                if (mod && net.isHost && Module.jkMods && Module.jkMods.feedUp)
                    Module.jkMods.feedUp(feed.url.replace(/^ws/, 'http'), mod[1], function() { return net.feed === feed; });
                /* A spectator's message, "<name> (watching): <text>" (the
                   feed made it): the engine puts it in the match's chat on
                   its next frame (wsrelay_js_feed_take_say). */
                var said = ev && typeof ev.data === 'string' ? /^say ([\x20-\x7e]{1,126})$/.exec(ev.data) : null;
                if (said && net.isHost && feed.said.length < FEED_SAID_MAX) feed.said.push(said[1]);
            };
            ws.onclose = function(ev) {
                if (net.feed !== feed || feed.ws !== ws) return;
                feed.ws = null;
                var code = ev && ev.code;
                /* The feed ended it, or another socket of this game took its
                   place: no more. Otherwise (a network blip, the feed
                   restarting) try again soon, then less often: 1, 2, 4, 8,
                   8 and 8 s, for about half a minute. What's sent meanwhile
                   is lost, and watchers set it right from the keyframe that
                   follows (keyWant). */
                if (code === 1000 || code === 1003 || code === 1008 || code === 4001 || code === 4002 || feed.tries >= FEED_RETRIES) {
                    net.feedStop('closed (' + code + ')');
                    return;
                }
                feed.tries++;
                setTimeout(function() { if (net.feed === feed && !feed.ws) net.feedOpen(feed); },
                           Math.min(1000 * Math.pow(2, feed.tries - 1), FEED_RETRY_MAX_MS));
            };
        };
        /* A chunk of this kind of these records, as the feed takes it. */
        net.feedChunk = function(kind, parts, size) {
            var out = new Uint8Array(1 + size), at = 1;
            out[0] = kind;
            for (var i = 0; i < parts.length; i++) {
                out.set(parts[i], at);
                at += parts[i].length;
            }
            return out;
        };
        net.feedRecord = function(bytes) {
            var rec = new Uint8Array(2 + bytes.length);
            rec[0] = bytes.length & 255;
            rec[1] = bytes.length >> 8;
            rec.set(bytes, 2);
            return rec;
        };
        /* The packets so far, as a chunk in the queue. */
        net.feedClose = function(feed) {
            if (!feed.size) return;
            feed.out.push(net.feedChunk(0, feed.parts, feed.size));
            feed.parts = [];
            feed.size = 0;
        };
        net.feedCopy = function(bytes) {
            var feed = net.feed;
            if (!feed || !feed.ws || feed.ws.readyState !== 1 || bytes.length > 0xffff) return;
            if (feed.size + 2 + bytes.length > FEED_MAX_CHUNK) net.feedClose(feed);
            var rec = net.feedRecord(bytes);
            feed.parts.push(rec);
            feed.size += rec.length;
            feed.packets++;
        };
        /* Whether the host's game is to make a keyframe now (it does at
           once: feedKeyBegin, feedKeyAdd for each message, feedKeyEnd). */
        net.feedKeyWanted = function(newLevel) {
            var feed = net.feed;
            if (!feed || !net.isHost || !feed.ws || feed.ws.readyState !== 1 || feed.key || feed.out.length > 8) return 0;
            var now = performance.now();
            if (!newLevel && !feed.keyWant && now - feed.keyAt < FEED_KEY_MS) return 0;
            feed.keyWant = false;
            feed.keyAt = now;
            return 1;
        };
        net.feedKeyBegin = function(head) {
            var feed = net.feed;
            if (!feed) return;
            net.feedClose(feed);
            feed.key = { parts: [], size: 0, kind: 1, chunks: [], bytes: 0, packets: 0, failed: '' };
            net.feedKeyAdd(head);
        };
        net.feedKeyAdd = function(bytes) {
            var key = net.feed && net.feed.key;
            if (!key || key.failed) return;
            if (bytes.length > 0xffff) {
                key.failed = 'a message of ' + bytes.length + ' bytes';
                return;
            }
            if (key.size + 2 + bytes.length > FEED_MAX_CHUNK) {
                key.chunks.push(net.feedChunk(key.kind, key.parts, key.size));
                key.parts = [];
                key.size = 0;
                key.kind = 2;
            }
            var rec = net.feedRecord(bytes);
            key.parts.push(rec);
            key.size += rec.length;
            key.bytes += rec.length;
            key.packets++;
            if (key.bytes > FEED_KEY_MAX) key.failed = 'over ' + (FEED_KEY_MAX / 1048576) + ' MiB';
        };
        net.feedKeyEnd = function() {
            var feed = net.feed, key = feed && feed.key;
            if (!key) return;
            /* Its last packet says it's whole: a viewer starts playing what
               came after it once it has all of it. */
            net.feedKeyAdd(new TextEncoder().encode('JKF1' + JSON.stringify({ v: 1, end: true })));
            feed.key = null;
            if (key.failed) {
                net.note('feed: keyframe not sent, ' + key.failed);
                return;
            }
            if (key.size) key.chunks.push(net.feedChunk(key.kind, key.parts, key.size));
            Array.prototype.push.apply(feed.out, key.chunks);
            feed.keys++;
            net.note('feed: keyframe, ' + key.packets + ' packets in ' + key.chunks.length + ' chunks, ' + (key.bytes / 1024).toFixed(1) + ' KiB');
        };
        net.feedFlush = function() {
            var feed = net.feed;
            if (!feed) return;
            var now = performance.now();
            if (feed.ws && feed.ws.readyState === 1) {
                net.feedClose(feed);
                for (var n = 0; n < FEED_SENDS && feed.out.length; n++) {
                    var chunk = feed.out.shift();
                    try {
                        feed.ws.send(chunk.buffer);
                        feed.chunks++;
                        feed.bytes += chunk.length;
                    } catch (e) {}
                }
                /* Too far behind: watchers start again from the next keyframe. */
                if (feed.out.length > FEED_QUEUE_MAX) {
                    feed.out = [];
                    feed.keyWant = true;
                    net.note('feed: too far behind, starting again from a keyframe');
                }
            } else {
                /* Nothing goes while it's closed, and what watchers missed
                   takes a keyframe once it's back. */
                feed.parts = [];
                feed.size = 0;
                feed.out = [];
                feed.keyWant = true;
            }
            if (now - feed.since >= FEED_LOG_MS) {
                var secs = (now - feed.since) / 1000;
                net.note('feed: ' + feed.packets + ' packets in ' + feed.chunks + ' chunks, ' + (feed.bytes / 1024).toFixed(1) + ' KiB in ' +
                         Math.round(secs) + ' s (' + (feed.bytes / 1024 / secs).toFixed(2) + ' KiB/s)' +
                         (feed.keys ? ', ' + feed.keys + (feed.keys > 1 ? ' keyframes' : ' keyframe') : ''));
                feed.packets = 0;
                feed.chunks = 0;
                feed.bytes = 0;
                feed.keys = 0;
                feed.since = now;
            }
        };
        net.feedStop = function(why) {
            var feed = net.feed;
            if (!feed) return;
            net.feed = null;
            clearInterval(feed.timer);
            if (feed.ws) {
                try { feed.ws.close(1000, 'bye'); } catch (e) {}
            }
            if (why) net.note('feed: stopped, ' + why);
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
               lobby only lists and joins games whose host speaks the same.
               pw: this game sends the password typed with join, so a lobby
               that checks passwords can leave them out of its list. ?netold
               doesn't, as older games don't. */
            if (url) url += (url.indexOf('?') < 0 ? '?' : '&') + 'net=' + net.netcode + (Module['jkNetOld'] ? "" : '&pw=1');
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
                net.newLobbyPing();
                /* The shortest round trip is taken while nothing waits on the
                   new socket: one taken later may have come behind a map
                   upload sent before a match, and make a queue look like
                   none (net.mapCongested). */
                net.pingBase();
                keepalive = setInterval(function() { if (ws.readyState === 1 && net.ws === ws) net.pingLobby(); }, 25000);
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
                    net.heardPlaying(from, ev.data);
                    net.pumpMaps();
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
    if (op === 'host' || op === 'join' || op === 'leave') {
        net.seat = null;
        if (net.feedStop) net.feedStop('left the room');
    }
    if (net.state !== 1) return 0;
    net.ws.send(text);
    return 1;
});

// Asks the lobby for a seat in room. Like any join, it ends the seat this
// game had (wsrelay_js_send_text). pPassword, for a game listed with the
// password box by a lobby that checks passwords, is the one the player
// typed: it goes along as the hex of its UTF-16LE units, 31 at most, unless
// it's empty.
EM_JS(int, wsrelay_js_join, (int room, const char16_t* pPassword), {
    var net = Module.jkNet;
    if (!net) return 0;
    var hex = "";
    for (var i = 0; pPassword && i < 31; i++) {
        var ch = HEAPU16[(pPassword >> 1) + i];
        if (!ch) break;
        hex += (0x100 | (ch & 0xFF)).toString(16).substring(1) + (0x100 | (ch >> 8)).toString(16).substring(1);
    }
    net.seat = null;
    if (net.state !== 1) return 0;
    net.ws.send('join ' + room + (hex ? ' ' + hex : ""));
    return 1;
});

// Whether the lobby the list being handled came from checks passwords.
EM_JS(int, wsrelay_js_list_pw, (), {
    var net = Module.jkNet;
    return net && net.listPw ? 1 : 0;
});

// Whether the page started the free multiplayer demo (Module.jkDemo) instead
// of a player's own game files.
EM_JS(int, wsrelay_js_demo, (), {
    return Module.jkDemo ? 1 : 0;
});

// Sends a game packet for player idTo over the link it takes (net.route): a
// data channel if it's open, else the relay. ownPos marks this player's own
// position updates, which take the fast lane where there is one.
EM_JS(int, wsrelay_js_send_packet, (const void* pData, int len, int idTo, int ownPos), {
    var net = Module.jkNet;
    if (!net) return 0;
    // The host's game sends to the players in it (net.mapInMatch).
    if (net.isHost && idTo > 1) net.sentPlayAt[idTo] = performance.now();
    return net.sendPacket(HEAPU8.slice(pData, pData + len), net.route(idTo), ownPos ? net.nextSeq(idTo) : undefined);
});

// A message this game sends everyone, for its room's feed (net.feedCopy),
// while it has one.
EM_JS(void, wsrelay_js_feed_copy, (const void* pData, int len), {
    var net = Module.jkNet;
    if (net && net.feed) net.feedCopy(HEAPU8.subarray(pData, pData + len));
});

// The next spectator's message the feed brought the host's game (net.feed's
// said), into pOut; 0 when there's none.
EM_JS(int, wsrelay_js_feed_take_say, (char* pOut, int maxLen), {
    var net = Module.jkNet;
    var line = net && net.feed && net.feed.said.length ? net.feed.said.shift() : '';
    if (!line) return 0;
    stringToUTF8(line, pOut, maxLen);
    return 1;
});

// Whether the host's game is to send its room's feed a keyframe now
// (net.feedKeyWanted); newLevel: the level isn't the last keyframe's.
EM_JS(int, wsrelay_js_feed_key_wanted, (int newLevel), {
    var net = Module.jkNet;
    return net && net.feedKeyWanted ? net.feedKeyWanted(newLevel) : 0;
});

// A keyframe's start: its first packet is JKF1 and a line of JSON on the
// level, for a viewer to load it and for pages to read without the game.
EM_JS(void, wsrelay_js_feed_key_begin, (const char* pEpisode, const char* pMap, uint32_t gameMs, int modeFlags, uint32_t checksum, int mots), {
    var net = Module.jkNet;
    if (!net || !net.feedKeyBegin) return;
    // tail: the host's mod as the lobby lists it ("" for none), which a
    // viewer needs to be running.
    // sha: the SHA-256 of the episode's GOB, as joiners get it (maps.js), so
    // a viewer without it can get it from the Level Library; '' for a retail
    // one, or until it's been hashed.
    // lib: the Mod Library's id for the host's mod, when the library saved it
    // (mods.js libId), so a viewer without it can get it from there.
    var episode = UTF8ToString(pEpisode);
    var sha = Module.jkMaps && Module.jkMaps.hostedSha ? Module.jkMaps.hostedSha(episode, mots ? 'goo' : 'gob') : '';
    var modLib = Module.jkMods && Module.jkMods.libId ? Module.jkMods.libId() : '';
    var head = 'JKF1' + JSON.stringify({ v: 1, game: mots ? 'mots' : 'jk1', episode: episode, map: UTF8ToString(pMap),
                                         t: gameMs >>> 0, mode: modeFlags, sum: checksum >>> 0,
                                         tail: Module.jkMods ? Module.jkMods.tail() : '', sha: sha || undefined,
                                         lib: modLib || undefined });
    net.feedKeyBegin(new TextEncoder().encode(head));
});

// One of a keyframe's messages, from cogMsgId on (net.feedKeyAdd).
EM_JS(void, wsrelay_js_feed_key_packet, (const void* pData, int len), {
    var net = Module.jkNet;
    if (net && net.feedKeyAdd) net.feedKeyAdd(HEAPU8.subarray(pData, pData + len));
});

EM_JS(void, wsrelay_js_feed_key_end, (), {
    var net = Module.jkNet;
    if (net && net.feedKeyEnd) net.feedKeyEnd();
});

// ---- Watching a match in the game (spectator mode), the feed's side. The
// page starts the engine with Module.jkWatchRoom, and this game plays that
// room's feed (the one pages watch, jkdf2mp-bb-server's src/feed.js) as a
// player with no body who sends nothing (see "Watching a match in the game"
// further down for the engine's side). Module.jkView reads the feed: it
// keeps the latest keyframe and what came after it until the engine is in
// that level, hands it all over at once while the engine synchronizes (as a
// joiner takes its host's big sync), then the rest at the pace it came,
// JITTER_MS behind the newest. A record's packets were sent over the batch
// before it (net.feedFlush), so they play spread over SPREAD_MS. What it
// tells the player goes to the page's Module.jkViewNotice, and the console.

// The room the page's "Watch in the game" picked, once.
EM_JS(int, wsrelay_js_take_watch_room, (), {
    var room = Number(Module['jkWatchRoom']);
    delete Module['jkWatchRoom'];
    return room > 0 && room < 1e9 ? Math.floor(room) : 0;
});

// Opens the room's feed, beside the lobby: wss://<host>/feed/<room>. 0
// without an address for it.
EM_JS(int, wsrelay_js_view_open, (int room), {
    /* LATE_MS: further behind than this, and it waits for the feed again;
       WAIT_MS: how long it waits for a keyframe to start from; RETRIES: how
       many times it opens the feed again after it drops. CATCH_MS: the
       newest record due more than this past JITTER_MS from now (it waited
       for the feed, LATE_MS, and the rest came at once), and it plays
       CATCH_RATE faster until it isn't, so each hold-up doesn't add to how
       far behind the match it is for the rest of it. */
    var JITTER_MS = 500, SPREAD_MS = 250, LATE_MS = 2000, WAIT_MS = 90000, RETRIES = 3, CATCH_MS = 1000, CATCH_RATE = 0.1;
    var old = Module.jkView;
    if (old) old.close();
    var base = Module['jkRelayUrl'];
    if (!base && typeof location !== 'undefined' && location)
        base = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/relay';
    if (!base) return 0;
    var cut = base.indexOf('?');
    if (cut >= 0) base = base.substring(0, cut);
    cut = base.lastIndexOf('/relay');
    /* phase: wait (no keyframe yet), level (one held, from recs[0]), sync
       (the engine is synchronizing in the level want: found once it has
       that level's keyframe), live (in the level), with recs[ri] and its
       packet pi next. lastMs: the time of the last record played. */
    var view = { url: (cut >= 0 ? base.substring(0, cut) : base) + '/feed/' + room, room: room, ws: null, tries: 0,
                 delay: -1, recs: [], ri: 0, pi: 0, phase: 'wait', since: performance.now(), level: null, want: null,
                 found: false, keyDone: false, syncAt: 0, offset: 0, lastMs: -1, late: 0, caught: 0, recvAt: 0, ended: '',
                 closed: false, waitMs: WAIT_MS,
                 resync: false, rekey: false, box: !!Module['jkWatchBox'] };
    Module.jkView = view;
    view.say = function(text) {
        console.log('OpenJKDF2: watching room ' + room + ': ' + text);
        if (typeof Module['jkViewNotice'] === 'function') {
            try { Module['jkViewNotice'](text); } catch (e) {}
        }
    };
    /* Nothing more comes; the engine leaves once it has played what it has. */
    view.end = function(why, text) {
        if (view.ended) return;
        view.ended = why;
        if (text) view.say(text);
        var ws = view.ws;
        view.ws = null;
        if (ws) {
            try { ws.close(1000); } catch (e) {}
        }
    };
    /* The engine left the match. */
    view.close = function() {
        view.closed = true;
        view.end('closed', '');
        if (Module.jkView === view) Module.jkView = null;
    };
    view.open = function() {
        var ws;
        try {
            /* The game in a viewing page's box (index.html's #watch-<room>-embed):
               its page is counted in the room's watching, so the feed leaves
               this one out (?box=1). */
            ws = new WebSocket(view.url + (view.box ? '?box=1' : ''));
        } catch (e) {
            view.end('error', 'The match could not be reached.');
            return;
        }
        ws.binaryType = 'arraybuffer';
        view.ws = ws;
        ws.onopen = function() { view.tries = 0; };
        ws.onmessage = function(ev) {
            if (view.ws !== ws) return;
            var d = ev && ev.data;
            if (typeof d !== 'string') {
                if (d) view.take(d);
                return;
            }
            var parts = d.split(' ');
            if (parts[0] !== 'feed') return;
            if (parts[1] === 'ended') {
                var why = parts.slice(2).join(' ') || 'over';
                view.end(why, why === 'room ended' ? 'The match is over.' :
                              why === 'watching off' ? 'The host turned watching off.' : 'The match stopped sending.');
            } else if (parts[1] === 'waiting') {
                if (view.phase === 'wait') view.say('Waiting for the match to send a picture to start from.');
            } else if (parseInt(parts[1], 10) >= 0) {
                view.delay = parseInt(parts[1], 10);
            }
        };
        ws.onclose = function() {
            if (view.ws !== ws) return;
            view.ws = null;
            if (view.ended) return;
            /* What came meanwhile is lost: the next keyframe sets it right. */
            if (view.phase === 'live') view.resync = true;
            if (view.tries >= RETRIES) {
                view.end('lost', 'Lost the match.');
                return;
            }
            view.tries++;
            setTimeout(function() {
                if (Module.jkView === view && !view.ws && !view.ended) view.open();
            }, 1000 * view.tries);
        };
    };
    /* A keyframe's JKF1 packets: its level (head) and its end (keyEnd). */
    view.marker = function(rec, pkt) {
        if (pkt.length < 5 || pkt[0] !== 74 || pkt[1] !== 75 || pkt[2] !== 70 || pkt[3] !== 49) return false;
        var h = null;
        try { h = JSON.parse(new TextDecoder().decode(pkt.subarray(4))); } catch (e) {}
        if (h && h.v === 1 && h.end) rec.keyEnd = true;
        else if (h && h.v === 1 && typeof h.episode === 'string' && typeof h.map === 'string' && rec.kind === 1) rec.head = h;
        return true;
    };
    /* A batch of records: u32 length (LE), u32 ms, u8 kind, u8 seat, then
       packets, each u16 length and its bytes. */
    view.take = function(buf) {
        var bytes = new Uint8Array(buf), dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), at = 0;
        while (at + 10 <= bytes.length) {
            var len = dv.getUint32(at, true), end = at + 4 + len;
            if (len < 6 || end > bytes.length) break;
            var rec = { ms: dv.getUint32(at + 4, true), kind: bytes[at + 8], seat: bytes[at + 9], packets: [], head: null, keyEnd: false };
            for (var p = at + 10; p + 2 <= end; ) {
                var n = bytes[p] | (bytes[p + 1] << 8);
                if (p + 2 + n > end) break;
                var pkt = bytes.slice(p + 2, p + 2 + n);
                if (rec.kind === 0 || !view.marker(rec, pkt)) rec.packets.push(pkt);
                p += 2 + n;
            }
            at = end;
            view.add(rec);
        }
    };
    view.same = function(h, want) {
        function name(s) {
            s = String(s || '').toLowerCase();
            var dot = s.lastIndexOf('.');
            return dot > 0 ? s.substring(0, dot) : s;
        }
        return !!h && !!want && name(h.episode) === name(want.episode) && name(h.map) === name(want.map);
    };
    view.isKey = function(rec) { return rec.kind === 1 && !!rec.head; };
    view.add = function(rec) {
        /* Until the engine is in the level, only the latest keyframe and what
           came after it are any use. */
        if (view.phase === 'wait' || view.phase === 'level') {
            if (view.isKey(rec)) {
                view.recs = [rec];
                view.ri = 0;
                view.pi = 0;
                view.level = rec.head;
                view.phase = 'level';
            } else if (view.phase === 'level') {
                view.recs.push(rec);
            }
            return;
        }
        if (view.phase === 'sync' && !view.found) {
            if (view.isKey(rec) && view.same(rec.head, view.want)) {
                view.recs = [rec];
                view.ri = 0;
                view.pi = 0;
                view.found = true;
            }
            return;
        }
        view.recs.push(rec);
    };
    /* The engine is synchronizing in this level (the first, or the host's
       next): from its latest keyframe held, or the next to come. */
    view.sync = function(want) {
        view.want = want;
        view.phase = 'sync';
        view.found = false;
        view.keyDone = false;
        view.syncAt = performance.now();
        for (var i = view.recs.length - 1; i >= 0; i--) {
            if (view.isKey(view.recs[i]) && view.same(view.recs[i].head, want)) {
                view.recs = view.recs.slice(i);
                view.ri = 0;
                view.pi = 0;
                view.found = true;
                return;
            }
        }
        view.recs = [];
        view.ri = 0;
        view.pi = 0;
    };
    view.trim = function() {
        if (view.ri > 200) {
            view.recs = view.recs.slice(view.ri);
            view.ri = 0;
        }
    };
    /* The next packet due: [bytes, seat]; or 0 for none yet, -1 for the
       welcome (it's caught up to the feed), -2 once it ended. */
    view.recv = function() {
        var now = performance.now(), r;
        if (view.phase === 'sync') {
            if (view.found) {
                while (view.ri < view.recs.length) {
                    r = view.recs[view.ri];
                    view.lastMs = r.ms;
                    if (view.pi < r.packets.length) return [r.packets[view.pi++], r.seat];
                    /* Whole: its end, or the host's game going on after it
                       (it sends its keyframe's chunks before anything else). */
                    if (r.keyEnd || (view.ri > 0 && r.kind === 0 && r.seat === 1)) view.keyDone = true;
                    view.ri++;
                    view.pi = 0;
                }
                if (view.keyDone && !view.ended) {
                    view.phase = 'live';
                    view.offset = now - view.lastMs + JITTER_MS;
                    view.trim();
                    return -1;
                }
            }
            if (view.ended) return -2;
            if (now - view.syncAt > view.waitMs) {
                view.end('wait', 'Nothing came from the match to start from.');
                return -2;
            }
            return 0;
        }
        if (view.phase !== 'live') return view.ended ? -2 : 0;
        /* Catching up (CATCH_MS): the time since it was last asked, at most
           100 ms of it, CATCH_RATE of that off the offset. caught: how much
           in all. */
        if (view.recs.length) {
            var ahead = view.recs[view.recs.length - 1].ms + view.offset - now - JITTER_MS - CATCH_MS;
            var step = view.recvAt ? Math.min(now - view.recvAt, 100) * CATCH_RATE : 0;
            if (ahead > 0 && step > 0) {
                step = Math.min(step, ahead);
                view.offset -= step;
                view.caught += step;
            }
        }
        view.recvAt = now;
        while (view.ri < view.recs.length) {
            r = view.recs[view.ri];
            /* Keyframes are for starting, and for after the feed dropped
               (resync): the first one newer than what played sets right what
               was missed (rekey, to its end). What's older than what played
               came again after the feed was opened again. */
            if (r.kind === 0) view.rekey = false;
            else if (view.pi === 0 && view.resync && view.isKey(r) && view.same(r.head, view.want) && r.ms >= view.lastMs) {
                view.resync = false;
                view.rekey = true;
            }
            if ((r.kind !== 0 && !view.rekey) || r.ms < view.lastMs || !r.packets.length) {
                if (r.keyEnd) view.rekey = false;
                view.ri++;
                view.pi = 0;
                continue;
            }
            var due = r.ms + view.offset;
            if (view.pi === 0 && now - due > LATE_MS) {
                view.offset = now - r.ms + JITTER_MS;
                due = r.ms + view.offset;
                view.late++;
            }
            if (now < due - SPREAD_MS + SPREAD_MS * (view.pi + 1) / r.packets.length) return 0;
            view.lastMs = r.ms;
            var out = [r.packets[view.pi++], r.seat];
            if (view.pi >= r.packets.length) {
                if (r.keyEnd) view.rekey = false;
                view.ri++;
                view.pi = 0;
            }
            view.trim();
            return out;
        }
        return view.ended ? -2 : 0;
    };
    view.open();
    return 1;
});

// The level of the feed's latest keyframe: 1 with its episode and map, and
// pInfo its multiplayer mode flags and 1 for MotS; 0 while there's none yet;
// -1 if it can't be watched here (the player has been told why).
EM_JS(int, wsrelay_js_view_level, (char* pEpisode, int episodeLen, char* pMap, int mapLen, int* pInfo), {
    var view = Module.jkView;
    if (!view || view.ended) return -1;
    if (view.phase === 'wait') {
        if (performance.now() - view.since > view.waitMs) {
            view.end('wait', 'Nothing came from the match to start from. Try again in a minute.');
            return -1;
        }
        return 0;
    }
    /* The match's mod and MotS Remastered pack (its tail, mods.js) and this
       game's, by their fingerprints (a mod folder's name may differ). When
       they aren't the same, 2: mods.js gets the match's (view_fetch_mod) and
       the engine restarts into it, once (jkWatchModFor). */
    var h = view.level, tail = Module.jkMods ? Module.jkMods.tail() : '';
    var lib = Module.jkMods && Module.jkMods.lib;
    var want = (lib && lib.parseTail(h.tail || '')) || {}, have = (lib && lib.parseTail(tail)) || {};
    var same = (h.tail || '') === tail ||
               (!!lib && !want.bad && (want.fp || null) === (have.fp || null) && (want.pack || null) === (have.pack || null));
    if (!same) {
        var why = '';
        if (!lib || !Module.jkMods.viewFetch)
            why = h.tail ? "This match is played with a mod, and this game can't get onto it."
                         : 'This match is played without a mod. To watch it here, start the game without one first.';
        else if (want.bad)
            why = "This match is played with a mod this game can't read.";
        else if (Module.jkWatchModFor === (h.tail || ''))
            why = "This game couldn't start with the mod this match is played with.";
        if (why) {
            view.end('mod', why);
            return -1;
        }
        return 2;
    }
    delete Module.jkWatchModFor;
    stringToUTF8(h.episode, pEpisode, episodeLen);
    stringToUTF8(h.map, pMap, mapLen);
    HEAP32[pInfo >> 2] = h.mode | 0;
    HEAP32[(pInfo >> 2) + 1] = h.game === 'mots' ? 1 : 0;
    return 1;
});

// The engine is synchronizing in this level (view.sync).
EM_JS(void, wsrelay_js_view_sync, (const char* pEpisode, const char* pMap), {
    var view = Module.jkView;
    if (view) view.sync({ episode: UTF8ToString(pEpisode), map: UTF8ToString(pMap) });
});

// The next packet due (view.recv), from cogMsgId on, into pBuf: its length,
// with the seat it came from (the player's net id) in pFrom; or 0 for none
// yet, -1 for the welcome, -2 once the feed ended.
EM_JS(int, wsrelay_js_view_recv, (void* pBuf, int maxLen, int* pFrom), {
    var view = Module.jkView;
    if (!view) return -2;
    for (;;) {
        var got = view.recv();
        if (typeof got === 'number') return got;
        var pkt = got[0];
        if (pkt.length < 4 || pkt.length > maxLen) continue;
        HEAPU8.set(pkt, pBuf);
        HEAP32[pFrom >> 2] = got[1];
        return pkt.length;
    }
});

// The engine can't play the feed on, for this reason (said to the player).
EM_JS(void, wsrelay_js_view_end, (const char* pWhy, const char* pText), {
    var view = Module.jkView;
    if (view) view.end(UTF8ToString(pWhy), UTF8ToString(pText));
});

// A viewer's message, on the feed's socket: "say <name>\t<text>", the name
// as the feed shows it (printable ASCII, at most 16). 0 when the feed isn't
// open.
EM_JS(int, wsrelay_js_view_say, (const char16_t* pName, const char* pText), {
    var view = Module.jkView;
    if (!view || view.ended || !view.ws || view.ws.readyState !== 1) return 0;
    var chars = [];
    for (var i = 0; pName && i < 31; i++) {
        var ch = HEAPU16[(pName >> 1) + i];
        if (!ch) break;
        chars.push(ch);
    }
    // No \s here: the C compiler reads an EM_JS body as a string and makes
    // \s an s. Past the first replace, a space is the only white space left.
    var name = String.fromCharCode.apply(null, chars).replace(/[^\x20-\x7e]/g, '?').replace(/ +/g, ' ').trim().substring(0, 16).trim();
    try {
        view.ws.send('say ' + (name || 'Someone') + '\t' + UTF8ToString(pText));
    } catch (e) {
        return 0;
    }
    return 1;
});

// Starts getting this game onto the match's mod and Remastered pack, as the
// keyframe says the host runs them (mods.js viewFetch, with the Mod Library's
// id for the mod if it has one, and the feed to ask the host's game by: "mod
// <fp>" on the feed's socket); wsrelay_js_mod_state says when it's done. How
// far along a download is shows over the game, at most once a second and
// when it's all in.
EM_JS(void, wsrelay_js_view_fetch_mod, (), {
    var view = Module.jkView, mods = Module.jkMods;
    if (!view || !view.level || !mods || !mods.viewFetch) return;
    var h = view.level, shown = '', last = 0;
    var feed = { url: view.url.replace(/^ws/, 'http'), ask: function(fp) {
        if (view.ws && view.ws.readyState === 1) {
            try { view.ws.send('mod ' + fp); } catch (e) {}
        }
    } };
    mods.viewFetch(h.tail || '', h.lib || '', function(done, total, from) {
        var pct = total ? Math.floor(100 * Math.min(done, total) / total) : 0, now = performance.now();
        var text = from === 'host' && !total ? "Asking the host's game for this match's mod." :
                   "Getting this match's mod from " + (from === 'host' ? 'its host' : 'the Mod Library') + ': ' + pct + ' percent';
        if (text === shown || (now - last < 1000 && pct < 100)) return;
        shown = text;
        last = now;
        view.say(text);
    }, feed);
});

// Why mods.js couldn't get the match's mod (viewError), "" for nothing to say.
EM_JS(void, wsrelay_js_view_mod_error, (char* pOut, int maxLen), {
    var mods = Module.jkMods;
    stringToUTF8(mods && mods.viewError ? mods.viewError() : '', pOut, maxLen);
});

// The engine restarts into the match's mod: it watches the same room again
// once it has (stdComm_WSRelay_Startup takes it again), and the page doesn't
// take the player back. jkWatchModFor: the tail it restarted for, so it
// doesn't restart for it again if that didn't work.
EM_JS(void, wsrelay_js_view_restart, (int room), {
    var view = Module.jkView;
    Module['jkWatchModFor'] = view && view.level ? view.level.tail || '' : '';
    if (view) view.close();
    Module['jkWatchRoom'] = room;
});

// Starts getting the feed's level as the host runs it, when its keyframe
// says which (sha) and this player may not have it (maps.js viewFetch): 1
// started (wsrelay_js_map_state says when it's done), 0 nothing to get.
// How far along it is shows over the game.
EM_JS(int, wsrelay_js_view_fetch_level, (const char* pExt), {
    var view = Module.jkView, h = view && view.level;
    if (!h || !h.sha || !Module.jkMaps || !Module.jkMaps.viewFetch) return 0;
    var name = h.episode, ext = UTF8ToString(pExt), shown = 0, said = -1;
    function say(text) {
        if (typeof Module['jkViewNotice'] === 'function') {
            try { Module['jkViewNotice'](text); } catch (e) {}
        }
    }
    Module.jkMaps.viewFetch(name, ext, h.sha, function(done, total) {
        var now = performance.now(), all = total > 0 && done >= total;
        if (!(total > 0) || (now - shown < 1000 && !all) || said === 100) return;
        shown = now;
        said = all ? 100 : Math.floor(100 * done / total);
        say("Getting this match's level from the Level Library: " + said + ' percent');
    });
    return 1;
});

// The view is still on (the feed hasn't ended, nor the player left it).
EM_JS(int, wsrelay_js_view_alive, (), {
    var view = Module.jkView;
    return view && !view.ended ? 1 : 0;
});

// The engine left the match, or never got into it. 1 if the page takes the
// player back to watching (Module.jkViewLeft, which loads the lobby page
// again), so this game needn't be anything more.
EM_JS(int, wsrelay_js_view_close, (int room), {
    var view = Module.jkView;
    if (view) view.close();
    if (typeof Module['jkViewLeft'] !== 'function') return 0;
    try {
        return Module['jkViewLeft'](room) ? 1 : 0;
    } catch (e) {
        return 0;
    }
});

// Watching a match in the game: the room, this game's net id (no player's:
// the relay numbers them from 1), and where it's at (see "Watching a match in
// the game" further down).
int stdComm_WSRelay_bViewer = 0;
static int wsrelay_viewRoom = 0;
#define WSRELAY_VIEWER_ID (0x7FFF5657)
static int wsrelay_bViewSyncing = 0;
static int wsrelay_bViewEnded = 0;
#define WSRELAY_VIEW_NONE (0)
#define WSRELAY_VIEW_WELCOME (-1)
#define WSRELAY_VIEW_ENDED (-2)

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
    net.playedAt = {};
    net.sentPlayAt = {};
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
    /* A host's map uploads go by the lobby's round trips (net.mapCongested):
       the shortest again as its game starts, before anyone downloads. */
    if (isHost) net.pingBase();
});

EM_JS(void, wsrelay_js_rtc_end, (), {
    var net = Module.jkNet;
    if (!net) return;
    // While its links are still up, which the summary names.
    if (net.session) net.sendSummary();
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

// Map and mod parts waiting for joiners go out paced (maps.js). The engine's
// frames keep them going in a hidden tab, whose timers barely run.
// Added: when the host's game frames start and end, as near as the page can
// tell, so what an answer from the lobby or the relay server was held up by
// them isn't taken for a queue on the connection (net.mapCongested,
// Module.jkImperial.mapCongested). The engine calls this in each frame
// (stdComm_WSRelay_Frame), near its start: the first call notes the time and
// posts the page a message, which the page takes right after the frame, so
// the time it does is the frame's end. A WebSocket's message that comes
// during a frame driven by requestAnimationFrame is read only once it's over:
// at 16 ms right after it, but Chrome runs the next frame first when the
// refresh is already due, and a message that came during a frame of 33 or
// 400 ms was read after the frame after it, before the page took its own
// (measured). So a message read by t, after the last frame noted as started
// by then, came no sooner than that frame's start if the frame before was
// short (no more than 20 ms after the one before ended), and otherwise no
// sooner than the end of the frame two before (Module.jkHeld.from). One not
// read by t hadn't come by then either. Taken off a round trip, that can be
// more than the page held it up by: frames that run briefly but come far
// apart (a 30 Hz display, the FPS limit) on a page that reads what comes
// between them at once take off up to the time since the frame two before
// ended. But between frames the page is often busy, waiting for the next
// refresh: in headless Chromium on a loaded machine the frames ran 4 ms
// (median) while the page's timer ran late for 129 s of 143, and counting
// only what came in during the frames themselves said a queue at nearly
// every look, and the upload went at the least rate. Up to 20 ms
// isn't taken off at all, as before frames were noted (the 80 ms the pacing
// takes for a queue allows for it): at 60 frames a second that's a whole
// frame at each answer, mostly more than the page held it up, and in
// pacing-test.mjs's model game packets waited up to 400 ms, not 270. A frame that
// doesn't call this, or starts before the page took the last one's message,
// counts as part of the one before, which only takes off less. Nothing is
// taken off without MessageChannel, nor once no frame was noted for a second
// (out of a match): what holds the page up then isn't known.
EM_JS(void, wsrelay_js_frame, (), {
    var h = Module.jkHeld;
    if (!h) {
        if (typeof MessageChannel === 'undefined') return;
        var ch = new MessageChannel();
        h = Module.jkHeld = { frames: [], port: ch.port2 };
        ch.port1.onmessage = function() {
            h.frames[h.frames.length - 1].end = performance.now();
        };
        h.from = function(t) {
            for (var i = h.frames.length - 1; i >= 0; i--) {
                var f = h.frames[i];
                if (f.start > t) continue;
                if (f.end && t - f.end > 1000) return t;
                if (i < 2) break;
                var two = h.frames[i - 2].end, one = h.frames[i - 1].end;
                var from = one - two <= 20 ? Math.min(f.start, one + 20) : two;
                return t - from <= 20 ? t : from;
            }
            return -Infinity;
        };
    }
    var last = h.frames[h.frames.length - 1];
    if (last && !last.end) return;
    h.frames.push({ start: performance.now(), end: 0 });
    if (h.frames.length > 16) h.frames.shift();
    h.port.postMessage(0);
});

EM_JS(void, wsrelay_js_map_pump, (), {
    if (Module.jkNet) Module.jkNet.pumpMaps();
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

// The match's rules and reach, whether this game runs MotS, and its build,
// for the match summary.
EM_JS(void, wsrelay_js_net_facts, (int rules, int reachMs, int mots, const char* pBuild), {
    if (Module.jkNetStats && Module.jkNetStats.facts) Module.jkNetStats.facts(rules, reachMs, !!mots, UTF8ToString(pBuild));
});

// The hello version this game and player id agreed on, 0 for none
// (stdComm_WSRelay_HelloWith), for the match summary.
EM_JS(void, wsrelay_js_net_hello, (int id, int version), {
    if (Module.jkNetStats && Module.jkNetStats.hello) Module.jkNetStats.hello(id, version);
});

// A hit between this game's player and player otherId (stdComm_WSRelay_NoteHit),
// with WSRELAY_HITFLAG_*, and for a claim this game turned down, why
// (reason) and how close its player came (distance, -1 if not checked). For
// a claim that named a moment, how far back that was when this game checked
// it (ageMs, -1 otherwise).
EM_JS(void, wsrelay_js_net_hit, (int otherId, int mine, int kind, int outcome, float damage, int flags, uint32_t attack, int reason, float distance, float ageMs), {
    if (Module.jkNetStats) Module.jkNetStats.hit(otherId, !!mine, kind, outcome, damage, flags, attack >>> 0, reason, distance, ageMs);
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
    if (!net) return -1;
    /* Taken from the front by position, not with shift(): once the array holds
       more than about 16,000 messages, Chromium copies all of it on each
       shift(), so draining a backlog that size took seconds. Taken slots are
       cleared; the array is emptied once all are taken, or trimmed once at
       least half are. */
    var queue = net.queue, taken = queue.taken | 0;
    if (taken >= queue.length) return -1;
    var data = queue[taken];
    queue[taken++] = undefined;
    if (taken === queue.length) {
        queue.length = 0;
        taken = 0;
    } else if (taken >= 1024 && taken * 2 >= queue.length) {
        queue.splice(0, taken);
        taken = 0;
    }
    queue.taken = taken;
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
// full, 3 and 4 its host speaks a newer or an older netcode, 5 a wrong
// password, 6 too many wrong ones lately.
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
// The join screen asked for the list at wsrelay_msecListAsked and hasn't put
// up what came back yet (stdComm_WSRelay_ListReady).
static int wsrelay_bListAsked = 0;
static uint32_t wsrelay_msecListAsked = 0;
// It was done with one before its answer was read (wsrelay_DropListAsk).
static int wsrelay_bListDropped = 0;
// It was given up on (WSRELAY_LIST_MS), and its answer still goes up.
static int wsrelay_bListGaveUp = 0;
// Which game (Main_bMotsCompat) the join screen's last ask was for.
static int wsrelay_bListMots = 0;
// The join screen opened since it last asked the lobby
// (stdComm_WSRelay_ForgetList).
static int wsrelay_bListForget = 0;
// A list came in, or was emptied, since wsrelay_ShowList last put one up.
static int wsrelay_bListUnshown = 0;
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

// Whether the lobby that listed each game checks its password when we join
// it, as that lobby said when the list came in, not when the player joins.
static int wsrelay_aListPw[WSRELAY_MAX_LISTED];
static int wsrelay_aShownPw[WSRELAY_MAX_LISTED];

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

// join, with the password typed for a game the lobby checks it for (NULL
// for any other). The log never shows the password.
static int wsrelay_SendJoin(int room, const char16_t* pwPassword)
{
    if (pwPassword && !pwPassword[0])
        pwPassword = NULL;
    wsrelay_verbosePrintf("Relay << join %d%s\n", room, pwPassword ? " <hidden>" : "");
    return wsrelay_js_join(room, pwPassword);
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
    // The level's player starts, with their shared copies
    // (sithThing_AddSharedStarts), are the most it can take
    // (sithPlayer_PlacePlayers), and slots past them can still be marked as
    // having a start from a bigger level earlier, with no player thing behind
    // them.
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

int stdComm_WSRelay_IsDemo(void)
{
    return wsrelay_js_demo();
}

// The free multiplayer demo's episode (jk1mpdem.gob, its Bespin level).
int stdComm_WSRelay_IsDemoEpisode(const char* pGobName)
{
    return pGobName && !__strnicmp(pGobName, "jk1mpdem", 8) &&
           (!pGobName[8] || !__strcmpi(pGobName + 8, ".gob"));
}

static void wsrelay_HandleList(char* pArgs)
{
    char* pSave = NULL;
    int n = 0;
    int lobbyPw = wsrelay_js_list_pw();
    int demo = stdComm_WSRelay_IsDemo();

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
        // Demo games and full games run different files, so nobody can join
        // across: each kind lists only its own.
        if (stdComm_WSRelay_IsDemoEpisode(pEntry->episodeGobName) != demo)
            continue;

        int room = atoi(pTok);
        wsrelay_aListRooms[n] = room;
        pEntry->numPlayers = atoi(pPlayers);

        int reachMs = 0;
        int rules = wsrelay_EntryHits(pEntry, &reachMs);
        wsrelay_aListHits[n] = rules < 0 ? -1 : (rules | (reachMs << 8));
        wsrelay_aListPw[n] = lobbyPw;

        // The join screen keeps its selection by guidInstance, so each room
        // needs its own.
        memset(&pEntry->guidInstance, 0, sizeof(pEntry->guidInstance));
        pEntry->guidInstance.a = 0x4A4B5753; // "JKWS"
        pEntry->guidInstance.b = (uint32_t)room;
        n++;
    }

    wsrelay_numListed = n;
    wsrelay_bListReceived = 1;
    wsrelay_bListUnshown = 1;
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
            else if (pCode && !strcmp(pCode, "password"))
                wsrelay_joinError = (pDetail && !strcmp(pDetail, "wait")) ? 6 : 5;
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

// The join screen is done with the list it asked for: it closed, the player
// is joining a game from it (stdComm_Open), or the engine restarts. An answer
// still to come isn't put up, nor taken for the next ask's: that would put
// up an old list at once, and every list after it an ask late.
static void wsrelay_DropListAsk()
{
    if (wsrelay_bListAsked)
        wsrelay_bListDropped = 1;
    wsrelay_bListAsked = 0;
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
    else if (stdComm_WSRelay_bViewer) // Added: watching a match in the game
        stdComm_dplayIdSelf = WSRELAY_VIEWER_ID;
    else
        stdComm_dplayIdSelf = wsrelay_id;
}

void stdComm_WSRelay_Startup()
{
    jkGuiMultiplayer_numConnections = 1;
    jk_snwprintf(jkGuiMultiplayer_aConnections[0].name, 0x80, u"OpenJKDF2 Relay");
    stdComm_Imperial_Startup(); // Added: the Imperial Network Protocol, the second connection
    stdComm_dword_8321E0 = 0;

    memset(jkGuiMultiplayer_aEntries, 0, sizeof(jkMultiEntry) * 32);
    dplay_dword_55D618 = 0;
    wsrelay_DropListAsk(); // Added: nor one asked for before a restart

    wsrelay_ResetSession();
    Hack_ResetClients();

    int room = wsrelay_js_take_join_room();
    if (room > 0)
        wsrelay_pendingRoom = room;

    // Added: watching a match in the game (Main_StartupViewer): no part in
    // the lobby, not even its live counts. Once: back from the match, this
    // is a game like any other.
    room = wsrelay_js_take_watch_room();
    if (room > 0)
    {
        wsrelay_viewRoom = room;
        stdComm_WSRelay_bViewer = 1;
        return;
    }

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
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection's
        return stdComm_Imperial_RecvSeq(pSeq);
    *pSeq = (uint32_t)wsrelay_aRecvSeq[1];
    return wsrelay_aRecvSeq[0];
}

double stdComm_WSRelay_SeqSentAt(int toId, uint32_t seq)
{
    if (stdComm_Imperial_bActive) // Added: as above
        return stdComm_Imperial_SeqSentAt(toId, seq);
    return wsrelay_js_seq_sent_at(toId, seq);
}

// Added: the lobby's links resend and keep order; the Imperial Network
// Protocol's desktops are on UDP.
int stdComm_WSRelay_HitsResend()
{
    return stdComm_Imperial_bActive;
}

double stdComm_WSRelay_Now()
{
    return wsrelay_js_now();
}

void stdComm_WSRelay_Frame()
{
    wsrelay_js_frame();
}

void stdComm_WSRelay_SyncClock(int netId, int bOn)
{
    // Added: netstats.js pings over the lobby's links; the Imperial Network
    // Protocol connection measures its players' clocks itself.
    if (netId && stdComm_Imperial_bActive)
        stdComm_Imperial_SyncClock(netId, bOn);
    else if (netId)
        wsrelay_js_net_sync(netId, bOn);
}

double stdComm_WSRelay_ClockOffset(int netId)
{
    if (stdComm_Imperial_bActive) // Added: as above
        return netId ? stdComm_Imperial_ClockOffset(netId) : NAN;
    return netId ? wsrelay_js_net_clock(netId) : NAN;
}

int stdComm_WSRelay_InMatch()
{
    // Added: or in an Imperial Network Protocol game (stdComm_Imperial.c)
    if ((!wsrelay_bHosting && !wsrelay_bJoined && !stdComm_Imperial_InGame()) || !sithNet_isMulti || !sithWorld_g_pCurrentWorld || !sithPlayer_g_pLocalPlayerThing)
        return 0;

    // A joiner is in once the host has welcomed it into the level
    // (sithMulti_ProcessWelcome clears this). Before that, the host would
    // drop it for sending anything (sithMessage_ProcessMessages).
    return stdComm_bIsServer || !(g_submodeFlags & 8);
}

// Tells netstats.js when this game is in a match, which is when it pings the
// other players and keeps the hit ledger, and who they are. And for its match
// summary, the rules, and which hello version this game agreed with each.
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
    int reachMs = 0;
    int rules = stdComm_WSRelay_GetHitRules(&reachMs);
    // First: it starts a roll call, and the players named after it are the
    // ones still in the match.
    wsrelay_js_net_facts(rules, reachMs, Main_bMotsCompat, openjkdf2_aReleaseCommitShort);
    for (int i = 0; i < jkPlayer_maxPlayers; i++)
    {
        if ((jkPlayer_playerInfos[i].flags & 1) && jkPlayer_playerInfos[i].playerNetId)
        {
            wsrelay_js_net_name(jkPlayer_playerInfos[i].playerNetId, jkPlayer_playerInfos[i].player_name);
            wsrelay_js_net_hello(jkPlayer_playerInfos[i].playerNetId, stdComm_WSRelay_HelloWith(i));
        }
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
    float claimAgeMs = -1.0f;
    int bApplying = stdComm_WSRelay_ApplyingClaim(&claimKind, &claimAttack, &distance, &claimMoment, &claimAgeMs);
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
    // id, a thrown saber's too (MotS's Saber Throw), and that shot's for the
    // blast where it hits, which does saber damage too: as its claim names it
    // (hits_Classify). 0 for the rest, which the ledger matches by time
    // instead.
    uint32_t attack = 0;
    if (pSource->type == SITH_THING_WEAPON && (kind == STDCOMM_WSRELAY_KIND_SHOT || kind == STDCOMM_WSRELAY_KIND_SABER))
        attack = (uint32_t)(stdComm_WSRelay_fireGuid ? stdComm_WSRelay_fireGuid : pSource->guid);
    else if (pSource->type == SITH_THING_EXPLOSION && kind == STDCOMM_WSRELAY_KIND_SABER)
        attack = (uint32_t)stdComm_WSRelay_BlastShot(pSource);
    else if (kind == STDCOMM_WSRELAY_KIND_SABER)
        attack = stdComm_WSRelay_GetSwing(pAttacker);

    // A claim's damage, which hit this game's player by the attacker's
    // account: the claim says what it was.
    if (bApplying)
    {
        kind = claimKind;
        attack = claimAttack;
        flags = WSRELAY_HITFLAG_BY_CLAIM | wsrelay_MomentFlags(claimMoment);
    }

    wsrelay_js_net_hit(otherId, mine, kind, outcome, (float)damage, flags, attack, 0, distance, bApplying ? claimAgeMs : -1.0f);
}

void stdComm_WSRelay_NoteTurnedDown(int otherId, int kind, int outcome, flex_t damage, uint32_t attack, int reason, float distance, int moment, float ageMs)
{
    if (!wsrelay_bNetLive || !otherId)
        return;
    wsrelay_js_net_hit(otherId, 0, kind, outcome, (float)damage, WSRELAY_HITFLAG_BY_CLAIM | wsrelay_MomentFlags(moment), attack, reason, distance, ageMs);
}

void stdComm_WSRelay_NoteTargeted(int otherId, int outcome, int reason, float distance, int moment, float ageMs)
{
    if (!wsrelay_bNetLive || !otherId)
        return;
    wsrelay_js_net_hit(otherId, 0, STDCOMM_WSRELAY_KIND_FORCE, outcome, 0.0f,
                       WSRELAY_HITFLAG_BY_MESSAGE | WSRELAY_HITFLAG_BY_CLAIM | wsrelay_MomentFlags(moment), 0, reason, distance, ageMs);
}

void stdComm_WSRelay_NoteMotion(int otherId, double bufferMs, uint32_t steps, uint32_t carried, uint32_t held,
                                uint32_t jumps, uint32_t warps, uint32_t restarts)
{
    if (!wsrelay_bNetLive || !otherId)
        return;
    wsrelay_js_net_motion(otherId, bufferMs, (int)steps, (int)carried, (int)held, (int)jumps, (int)warps, (int)restarts);
}

static void wsrelay_PovReset(); // what each player holds and sees, below

// ---- Watching a match in the game (spectator mode), the engine's side. A
// viewer is a joiner whose host is the room's feed (Module.jkView, above):
// Main_StartupViewer waits for the feed's level (stdComm_WSRelay_ViewerOpen)
// and loads it as a joiner does; the Synchronizing screen takes the feed's
// keyframe and what came after it, leaving out what's over by now (shots,
// sounds, hits); and the welcome a joiner's host sends once it has the world
// comes from here (wsrelay_ViewerWelcome), into a slot no player has. Like
// the dedicated host's, its player has no body, and a free camera that
// follows the players (sithMulti_IsSpectating, sithControl.c). It sends
// nothing at all (DirectPlay_Send), so no game in the match knows it's
// there, and it takes no part in the lobby: not its live counts, its rooms or
// the match record.

// The viewer is done: from here on this game is one like any other, in the
// lobby's live counts too (stdComm_WSRelay_Startup).
static void wsrelay_ViewerOff()
{
    int bPageTakesOver = wsrelay_js_view_close(wsrelay_viewRoom);
    stdComm_WSRelay_bViewer = 0;
    // As stdComm_CloseConnection leaves the join screen's connection, which
    // the viewer opened without one (stdComm_WSRelay_ViewerOpen).
    stdComm_dword_8321E0 = 0;
    stdComm_dword_8321DC = 0;
    stdComm_dplayIdSelf = 0;
    stdComm_bIsServer = 0;
    // A page that doesn't take the player back leaves them a game to play,
    // in the lobby as any player is.
    if (bPageTakesOver)
        return;
    wsrelay_js_presence(jkPlayer_playerShortName, &Main_bMotsCompat);
    wsrelay_js_connect(STDCOMM_WSRELAY_NETCODE);
}

// The level of the match watched isn't in this game's files: a joiner gets
// it from the host (packaging/wasm/maps.js), but the viewer asks it nothing.
void stdComm_WSRelay_ViewerNoLevel(const char* pEpisode)
{
    char aText[160];
    stdPlatform_Printf("OpenJKDF2: Watching room %d: no episode %s here.\n", wsrelay_viewRoom, pEpisode);
    snprintf(aText, sizeof(aText), "Your game files don't have this match's level (%s). Join a match on it once to get it, then watch it in the game.", pEpisode);
    wsrelay_js_view_end("level", aText);
}

// The players' looks (model, saber, sparks: DSS_SABERINFO1-3) as the feed's
// keyframe and what came after it gave them. Once Synchronizing is over, the
// level's start (jkMain, jkPlayer_InitSaber) puts every player back on the
// default green saber, which a joiner sets right from what each player sends
// it when it joins. A viewer joins no one, so it plays these again then
// (stdComm_WSRelay_ViewerInLevel). Each: u16 length, u16 0, i32 seat, then
// the message from cogMsgId on.
#define WSRELAY_VIEW_LOOKS_MAX (32 * 1024)
static uint8_t wsrelay_aViewLooks[WSRELAY_VIEW_LOOKS_MAX];
static int wsrelay_viewLooksLen = 0;
static int wsrelay_viewLooksAt = -1; // the next to play again, -1 until due

static void wsrelay_ViewerKeepLooks(const uint8_t* pMsg, int len, int from)
{
    if (wsrelay_viewLooksAt >= 0 || len <= 0 || len > 0xffff || wsrelay_viewLooksLen + 8 + len > WSRELAY_VIEW_LOOKS_MAX)
        return;
    uint8_t* p = wsrelay_aViewLooks + wsrelay_viewLooksLen;
    uint16_t len16 = (uint16_t)len, zero = 0;
    int32_t from32 = from;
    memcpy(p, &len16, 2);
    memcpy(p + 2, &zero, 2);
    memcpy(p + 4, &from32, 4);
    memcpy(p + 8, pMsg, len);
    wsrelay_viewLooksLen += 8 + len;
}

// In the level, after jkPlayer_InitSaber: the looks kept are due.
void stdComm_WSRelay_ViewerInLevel()
{
    if (!stdComm_WSRelay_bViewer)
        return;
    wsrelay_viewLooksAt = 0;
    stdPlatform_Printf("OpenJKDF2: Watching room %d: the players' looks again, %d bytes.\n", wsrelay_viewRoom, wsrelay_viewLooksLen);
}

// The next look kept, as DirectPlay_Receive gives a message: 0, or -1 when
// there's none left.
static int wsrelay_ViewerNextLooks(int* pIdOut, int* pMsgIdOut, int* pLenOut, int maxLen)
{
    if (wsrelay_viewLooksAt < 0)
        return -1;
    while (wsrelay_viewLooksAt + 8 <= wsrelay_viewLooksLen)
    {
        uint8_t* p = wsrelay_aViewLooks + wsrelay_viewLooksAt;
        uint16_t len16;
        int32_t from32;
        memcpy(&len16, p, 2);
        memcpy(&from32, p + 4, 4);
        wsrelay_viewLooksAt += 8 + len16;
        if ((int)len16 > maxLen)
            continue;
        memcpy(pMsgIdOut, p + 8, len16);
        *pIdOut = from32;
        *pLenOut = len16;
        return 0;
    }
    wsrelay_viewLooksLen = 0;
    wsrelay_viewLooksAt = -1;
    return -1;
}

// The match is played with another mod (or none) or Remastered pack than
// this game runs: mods.js gets the host's, as a joiner's game does but
// without asking the host, and the engine restarts into it to watch the same
// room again (as wsrelay_JoinRoom restarts for a joiner).
// STDCOMM_WSRELAY_VIEW_RESTART once it's restarting, 0 if it can't (the
// player has been told why).
static int wsrelay_ViewerGetMod()
{
    wsrelay_js_view_fetch_mod();
    int got;
    while ((got = wsrelay_js_mod_state()) == WSRELAY_MAP_BUSY)
    {
        if (!wsrelay_js_view_alive())
            wsrelay_js_mod_abort("cancelled");
        emscripten_sleep(50);
    }
    if (got == WSRELAY_MOD_RESTART)
    {
        char path[sizeof(openjkdf2_aRestartPath)];
        wsrelay_js_mod_restart(path, sizeof(path));
        stdPlatform_Printf("OpenJKDF2: Watching room %d: restarting with mod path \"%s\".\n", wsrelay_viewRoom, path);
        wsrelay_js_view_restart(wsrelay_viewRoom);
        stdComm_WSRelay_bViewer = 0;
        stdString_SafeStrCopy(openjkdf2_aRestartPath, path, sizeof(openjkdf2_aRestartPath));
        openjkdf2_restartMode = OPENJKDF2_RESTART_PATH;
        g_should_exit = 1;
        return STDCOMM_WSRELAY_VIEW_RESTART;
    }
    char aText[400];
    wsrelay_js_view_mod_error(aText, sizeof(aText));
    stdPlatform_Printf("OpenJKDF2: Watching room %d: can't get onto the match's mod.\n", wsrelay_viewRoom);
    if (wsrelay_js_view_alive())
        wsrelay_js_view_end("mod", aText[0] ? aText : "This game couldn't get onto the mod this match is played with.");
    wsrelay_ViewerOff();
    return 0;
}

int stdComm_WSRelay_ViewerOpen(char* pEpisode, int episodeLen, char* pMap, int mapLen, int* pModeFlags)
{
    if (!stdComm_WSRelay_bViewer)
        return 0;
    wsrelay_bViewSyncing = 0;
    wsrelay_bViewEnded = 0;
    wsrelay_viewLooksLen = 0;
    wsrelay_viewLooksAt = -1;
    if (!wsrelay_js_view_open(wsrelay_viewRoom))
    {
        wsrelay_ViewerOff();
        return 0;
    }
    stdPlatform_Printf("OpenJKDF2: Watching room %d: waiting for its feed.\n", wsrelay_viewRoom);

    int aInfo[2] = {0, 0};
    int state;
    while ((state = wsrelay_js_view_level(pEpisode, episodeLen, pMap, mapLen, aInfo)) == 0)
        emscripten_sleep(50);
    if (state == 2)
        return wsrelay_ViewerGetMod();
    if (state > 0 && aInfo[1] != Main_bMotsCompat)
    {
        wsrelay_js_view_end("game", aInfo[1] ? "This match is in Mysteries of the Sith. Watch it from a MotS room."
                                              : "This match is in Jedi Knight. Watch it from a Jedi Knight room.");
        state = -1;
    }
    if (state < 0)
    {
        stdPlatform_Printf("OpenJKDF2: Watching room %d: can't.\n", wsrelay_viewRoom);
        wsrelay_ViewerOff();
        return 0;
    }
    stdPlatform_Printf("OpenJKDF2: Watching room %d: episode %s, level %s.\n", wsrelay_viewRoom, pEpisode, pMap);
    *pModeFlags = aInfo[0];

    // The host's version of its level: the player's own, one saved from an
    // earlier game, or the Level Library's (maps.js viewFetch).
    if (wsrelay_js_view_fetch_level(JKRES_GOB_EXT))
    {
        int got;
        while ((got = wsrelay_js_map_state()) == WSRELAY_MAP_BUSY)
        {
            if (!wsrelay_js_view_alive())
                wsrelay_js_map_abort("cancelled");
            emscripten_sleep(50);
        }
        if (got != WSRELAY_MAP_READY)
        {
            char aText[200];
            stdPlatform_Printf("OpenJKDF2: Watching room %d: couldn't get episode %s.\n", wsrelay_viewRoom, pEpisode);
            if (wsrelay_js_view_alive())
            {
                snprintf(aText, sizeof(aText), "Your game files don't have this match's level (%s), and the Level Library doesn't have the host's version of it.", pEpisode);
                wsrelay_js_view_end("level", aText);
            }
            wsrelay_ViewerOff();
            return 0;
        }
    }

    // As stdComm_Open leaves a joiner, under an id no player has.
    stdComm_dword_8321DC = 1;
    stdComm_dword_8321E0 = 1;
    stdComm_dword_8321E8 = 0;
    stdComm_bIsServer = 0;
    stdComm_dplayIdSelf = WSRELAY_VIEWER_ID;
    return 1;
}

// The viewer's slot: one with a player thing and nobody in it, from the top,
// so past the room's players where the level has room (it has a thing for
// each of the 32, sithThing_AddSharedStarts), else the last a joiner would
// be given.
static int wsrelay_ViewerSlot()
{
    for (int i = JKPLAYER_NUM_INFOS - 1; i >= 0; i--)
    {
        SithPlayer* pInfo = &jkPlayer_playerInfos[i];
        if (pInfo->pLocalPlayer && !(pInfo->flags & 1) && (!pInfo->playerNetId || (int)pInfo->playerNetId == stdComm_dplayIdSelf))
            return i;
    }
    return -1;
}

void stdComm_WSRelay_ViewerJoined(int slot)
{
    if (slot < 0 || slot >= JKPLAYER_NUM_INFOS)
        return;
    // As the dedicated host's player (sithMulti_StartupServer): in nobody's
    // list of players, this one's included, and no body.
    SithPlayer* pInfo = &jkPlayer_playerInfos[slot];
    pInfo->flags = 6;
    if (pInfo->pLocalPlayer)
    {
        pInfo->pLocalPlayer->flags |= SITH_TF_DISABLED;
        pInfo->pLocalPlayer->attach_flags = 0;
    }
    stdPlatform_Printf("OpenJKDF2: Watching room %d from slot %d.\n", wsrelay_viewRoom, slot);
    wsrelay_PovReset();
    sithControl_SpectatorNext(1);
}

// Messages a viewer leaves out while it catches up, before it's in the level:
// what's over by now (shots, hits, sounds, animations, chat) and what was
// between other games, as against what still stands (where things are, what
// they're doing, who's playing).
static int wsrelay_ViewerSkipsWhileSyncing(int id)
{
    switch (id)
    {
        case DSS_CHAT:
        case DSS_FIREPROJECTILE:
        case DSS_DAMAGE:
        case DSS_PLAYKEY:
        case DSS_PLAYSOUND:
        case DSS_PLAYKEYMODE:
        case DSS_STOPKEY:
        case DSS_STOPSOUND:
        case DSS_SYNCPALEFFECTS:
        case DSS_WELCOME:
        case DSS_JOINREQUEST:
        case DSS_JOINING:
        case DSS_PLAYSOUNDMODE:
        case DSS_PING:
        case DSS_PINGREPLY:
        case DSS_RESET:
        case DSS_HUDTARGET:
        case DSS_JKPRINTUNISTRING:
            return 1;
    }
    return 0;
}

// The welcome a joiner's host sends once it's had the world (sithMulti_Update's
// big sync), as if from the host: into the viewer's slot, under its id.
static int wsrelay_ViewerWelcome(int* pIdOut, int* pMsgIdOut, int* pLenOut, int maxLen)
{
    int slot = wsrelay_ViewerSlot();
    char16_t aName[16];
    if (slot < 0 || maxLen < 12 + (int)sizeof(aName))
    {
        stdPlatform_Printf("OpenJKDF2: Watching room %d: no slot to watch from.\n", wsrelay_viewRoom);
        wsrelay_js_view_end("full", "This match has no room left to watch it from.");
        wsrelay_bViewEnded = 1;
        return -1;
    }
    memset(aName, 0, sizeof(aName));
    stdString_SafeWStrCopy(aName, jkPlayer_playerShortName, 16);

    uint8_t* p = (uint8_t*)pMsgIdOut;
    uint16_t msgId = DSS_WELCOME;
    uint16_t resendId = 0;
    int32_t playerNum = slot;
    int32_t idPlayer = stdComm_dplayIdSelf;
    memcpy(p, &msgId, 2);
    memcpy(p + 2, &resendId, 2);
    memcpy(p + 4, &playerNum, 4);
    memcpy(p + 8, &idPlayer, 4);
    memcpy(p + 12, aName, sizeof(aName));
    *pIdOut = sithNet_serverNetId ? (int)sithNet_serverNetId : 1;
    *pLenOut = 12 + (int)sizeof(aName);
    return 0;
}

// DirectPlay_Receive for a viewer: the feed's packets as if from the players
// who sent them (the feed's seat is their net id), then the welcome, then, once
// it's ended, the host gone, as for a joiner.
static int wsrelay_ViewerReceive(int* pIdOut, int* pMsgIdOut, int* pLenOut, int maxLen)
{
    int bSync = (g_submodeFlags & 8) != 0;
    if (bSync && !wsrelay_bViewSyncing)
        wsrelay_js_view_sync(jkRes_episodeGobName, jkMain_aLevelJklFname);
    wsrelay_bViewSyncing = bSync;

    // The match is on for as long as its feed is: a gap in it (the feed
    // restarting, and waiting for a keyframe of age) is no host gone quiet.
    // Once it has ended, the host is gone (below).
    if (!wsrelay_bViewEnded)
    {
        for (uint32_t i = 0; i < jkPlayer_maxPlayers && i < JKPLAYER_NUM_INFOS; i++)
            jkPlayer_playerInfos[i].msecLastCommTime = sithTime_g_msecGameTime;
    }

    // In the level: the players' looks from before it started, first.
    if (!bSync && wsrelay_ViewerNextLooks(pIdOut, pMsgIdOut, pLenOut, maxLen) == 0)
        return 0;

    while (1)
    {
        int from = 0;
        int len = wsrelay_js_view_recv(wsrelay_recvBuf, sizeof(wsrelay_recvBuf), &from);
        if (len == WSRELAY_VIEW_NONE)
            return -1;
        if (len == WSRELAY_VIEW_ENDED)
        {
            if (wsrelay_bViewEnded)
                return -1;
            wsrelay_bViewEnded = 1;
            stdPlatform_Printf("OpenJKDF2: Leaving the match: its feed ended.\n");
            // Synchronizing gives up at its next join request
            // (DirectPlay_Send); in the level, the host is gone.
            if (bSync)
                return -1;
            *pIdOut = sithNet_serverNetId ? (int)sithNet_serverNetId : 1;
            return 2;
        }
        if (len == WSRELAY_VIEW_WELCOME)
        {
            if (!bSync)
                continue;
            return wsrelay_ViewerWelcome(pIdOut, pMsgIdOut, pLenOut, maxLen);
        }
        if (len < 4 || len > maxLen)
            continue;
        uint16_t id = 0;
        memcpy(&id, wsrelay_recvBuf, 2);
        // Hit what you see's claims and answers are between the players'
        // games, and nothing a viewer takes part in.
        if (id == STDCOMM_WSRELAY_HITS_MSG || (bSync && wsrelay_ViewerSkipsWhileSyncing(id)))
            continue;
        if (bSync && (id == DSS_SABERINFO1 || id == DSS_SABERINFO2 || id == DSS_SABERINFO3))
            wsrelay_ViewerKeepLooks(wsrelay_recvBuf, len, from);
        memcpy(pMsgIdOut, wsrelay_recvBuf, len);
        *pIdOut = from;
        *pLenOut = len;
        return 0;
    }
}

int DirectPlay_Receive(int *pIdOut, int *pMsgIdOut, int *pLenOut)
{
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
        return stdComm_Imperial_Receive(pIdOut, pMsgIdOut, pLenOut);

    int maxLen = *pLenOut;
    *pIdOut = 0;
    *pLenOut = 0;

    // Added: watching a match in the game: what comes is the room's feed.
    if (stdComm_WSRelay_bViewer)
        return wsrelay_ViewerReceive(pIdOut, pMsgIdOut, pLenOut, maxLen);

    Hack_ResetClients();

    wsrelay_WatchRelay();
    wsrelay_WatchMatch();
    if (stdComm_bIsServer)
    {
        wsrelay_CheckLeftPending();
        wsrelay_js_frame();
        wsrelay_js_map_pump();
    }

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

// Spectators' messages (stdComm_WSRelay_ViewerSay and FeedFrame). At most
// one every WSRELAY_SAY_MS from a viewer, as the feed takes them; what it
// said in the last WSRELAY_SAID_KEEP_MS is kept to know it coming back.
#define WSRELAY_SAY_MS (3000)
#define WSRELAY_SAY_MAX (96)
#define WSRELAY_SAID_MAX (4)
#define WSRELAY_SAID_KEEP_MS (120000)
static char16_t wsrelay_saidWStr[256];
static char wsrelay_aSaid[WSRELAY_SAID_MAX][WSRELAY_SAY_MAX + 1];
static uint32_t wsrelay_aSaidMs[WSRELAY_SAID_MAX];
static int wsrelay_saidNext = 0;
static uint32_t wsrelay_saidLastMs = 0;

// As the feed cleans a message: printable ASCII ('?' for the rest), runs of
// spaces as one, no space at either end, at most max characters.
static void wsrelay_SayClean(char* pOut, const char* pIn, int max)
{
    int n = 0, space = 0;
    for (const unsigned char* p = (const unsigned char*)pIn; *p && n < max; p++)
    {
        unsigned char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v')
        {
            space = n > 0;
            continue;
        }
        if (space && n < max - 1)
            pOut[n++] = ' ';
        space = 0;
        pOut[n++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
    }
    while (n > 0 && pOut[n - 1] == ' ')
        n--;
    pOut[n] = 0;
}

void stdComm_WSRelay_ViewerSay(const char* pStr)
{
    char text[WSRELAY_SAY_MAX + 1];
    if (!stdComm_WSRelay_bViewer || !pStr)
        return;
    wsrelay_SayClean(text, pStr, WSRELAY_SAY_MAX);
    if (!text[0])
        return;
    uint32_t now = stdPlatform_GetTimeMsec();
    if (wsrelay_saidLastMs && now - wsrelay_saidLastMs < WSRELAY_SAY_MS)
    {
        sithConsole_PrintString("Not sent: one message every 3 seconds while watching.");
        return;
    }
    if (!wsrelay_js_view_say(jkPlayer_playerShortName, text))
    {
        sithConsole_PrintString("Not sent: the match can't be reached right now.");
        return;
    }
    wsrelay_saidLastMs = now;
    stdString_SafeStrCopy(wsrelay_aSaid[wsrelay_saidNext], text, sizeof(wsrelay_aSaid[0]));
    wsrelay_aSaidMs[wsrelay_saidNext] = now ? now : 1;
    wsrelay_saidNext = (wsrelay_saidNext + 1) % WSRELAY_SAID_MAX;
}

int stdComm_WSRelay_ViewerOwnEcho(const char* pLine)
{
    if (!stdComm_WSRelay_bViewer || !pLine)
        return 0;
    const char* p = strstr(pLine, " (watching): ");
    if (!p)
        return 0;
    p += strlen(" (watching): ");
    uint32_t now = stdPlatform_GetTimeMsec();
    for (int i = 0; i < WSRELAY_SAID_MAX; i++)
    {
        if (wsrelay_aSaidMs[i] && now - wsrelay_aSaidMs[i] < WSRELAY_SAID_KEEP_MS && !strcmp(p, wsrelay_aSaid[i]))
        {
            wsrelay_aSaidMs[i] = 0;
            return 1;
        }
    }
    return 0;
}

// Called by stdComm_SendToPlayer with each message sent to everyone, once,
// before it goes to each player: a copy for the room's feed, when the lobby
// asked for one (see the top of this file). The bytes are from cogMsgId on.
void stdComm_WSRelay_FeedCopy(const void* pData, uint32_t size)
{
    if (stdComm_Imperial_bActive || (!wsrelay_bHosting && !wsrelay_bJoined))
        return;
    wsrelay_js_feed_copy(pData, (int)size);
}

// The id a keyframe's messages are sent to (sithMulti_FeedKeyframe):
// DirectPlay_Send hands them to the room's feed, never to the network. The
// relay numbers players from 1, so no player has it.
#define WSRELAY_FEED_KEY_ID (0x7FFF4B46)
static int wsrelay_bKeyframing = 0;
// The level ("<episode>/<map>") of the last keyframe: a new level gets one at
// once, since what came before is of no use to a viewer.
static char wsrelay_feedKeyLevel[80];

void stdComm_WSRelay_FeedFrame()
{
    if (stdComm_Imperial_bActive || !wsrelay_bHosting || !stdComm_bIsServer || !sithWorld_g_pCurrentWorld)
        return;
    // Spectators' messages (stdComm_WSRelay_ViewerSay), as the feed made
    // them: in the chat, here and for everyone, and so through the feed for
    // those watching.
    char said[128];
    while (wsrelay_js_feed_take_say(said, sizeof(said)))
    {
        if (!said[0])
            continue;
        jk_snwprintf(wsrelay_saidWStr, 256, u"%S", said);
        sithConsole_AlertSound();
        sithConsole_PrintWString(wsrelay_saidWStr);
        sithMulti_SendChat(said, -1, -1);
    }
    char level[80];
    snprintf(level, sizeof(level), "%s/%s", jkRes_episodeGobName, jkMain_aLevelJklFname);
    if (!wsrelay_js_feed_key_wanted(strcmp(level, wsrelay_feedKeyLevel) != 0))
        return;
    stdString_SafeStrCopy(wsrelay_feedKeyLevel, level, sizeof(wsrelay_feedKeyLevel));
    wsrelay_js_feed_key_begin(jkRes_episodeGobName, jkMain_aLevelJklFname, sithTime_g_msecGameTime,
                              (int)sithNet_MultiModeFlags, (uint32_t)sithNet_checksum, Main_bMotsCompat);
    wsrelay_bKeyframing = 1;
    sithMulti_FeedKeyframe(WSRELAY_FEED_KEY_ID);
    wsrelay_bKeyframing = 0;
    wsrelay_js_feed_key_end();
}

// ---- What a player holds and sees (spectator mode). A viewer that follows a
// player in first person shows what that player's own screen does: their
// first-person model (the weapon held, which no other game knows: weapon
// COGs set it only on the player's own game, jkCog_SetPovModel), its
// animations, how it sways, the status bars and crosshair, and the screen's
// flashes and tints. Each player's own game tells the room's feed, and no
// one else (wsrelay_js_feed_copy only), in its own engine message
// (STDCOMM_WSRELAY_POV_MSG, past DSS_MAX, so engines without this ignore it):
//   STATE  1, player u8, status flags u16 (JKHUD_VALUES_*), model s32 (its
//          id, -1 for none), sway vec3 f32 and magnitude f32, health,
//          max health, shields, force, battery, ammo s16: when any of it
//          changes (at most every 100 ms; the model at once) and every 2 s
//   PLAY   2, player u8, its track s8, 0 u8, model s32, keyframe s32, low
//          priority s32, flags s32: as jkPlayPOVKey plays it
//   STOP   3, player u8, its track s8, 0 u8, fade f32: as jkStopPOVKey
//   PAL    4, player u8, 0 u16, then the flashes (sithPlayer's
//          palEffectsIdx1) and the tint (palEffectsIdx2): tint vec3 f32, add
//          u8 x3, 0 u8, fade f32, filter u8 x3, 0 u8 each: at once for a new
//          flash, else at most every 100 ms when they change, and every 2 s
//   TRACKS 5, player u8, count u8, 0 u8, model s32, then for each animation
//          playing on the model (not one fading out): its track u8, 0 u8,
//          low and high priority s16, 0 s16, keyframe s32, status s32, frame,
//          upper frame and speed f32: every second
// A viewer plays an animation only on the model it was played on there, and
// fades the flashes as sithPlayer_Tick does. TRACKS puts back an animation a
// viewer missed the start of: one held at its end, as a weapon's raise is,
// gives the weapon where it's held. In kit 171 a viewer that came after a
// player picked their Bryar saw it only while they fired (its fire animation),
// gone again after (the model as it is without one), and other weapons
// picked while it watched.

#define WSRELAY_POV_EVERY_MS (2000)
#define WSRELAY_POV_MIN_MS (100)
#define WSRELAY_POV_STATE_LEN (36)
#define WSRELAY_POV_PAL_LEN (52)
#define WSRELAY_POV_TRACKS_MS (1000)
#define WSRELAY_POV_TRACK_LEN (28)
#define WSRELAY_POV_TRACKS_LEN (8 + 4 * WSRELAY_POV_TRACK_LEN)

static uint8_t wsrelay_aPovSent[WSRELAY_POV_STATE_LEN];
static int wsrelay_bPovSent = 0;
static uint32_t wsrelay_povSentMs = 0;
static uint8_t wsrelay_aPalSent[WSRELAY_POV_PAL_LEN];
static int wsrelay_bPalSent = 0;
static uint32_t wsrelay_palSentMs = 0;
static stdPalEffect wsrelay_palLast; // the flashes as they were last frame
static int wsrelay_bTracksSent = 0;
static uint32_t wsrelay_tracksSentMs = 0;

// Whether this game's feed is open (net.feed), so what's sent reaches it.
EM_JS(int, wsrelay_js_feed_on, (), {
    var net = Module.jkNet;
    return net && net.feed && net.feed.ws && net.feed.ws.readyState === 1 ? 1 : 0;
});

static int16_t wsrelay_PovS16(int v)
{
    return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

// Whether this game says what its player holds and sees: a player's game in
// a room whose feed is open.
static int wsrelay_PovSending()
{
    if (stdComm_Imperial_bActive || stdComm_WSRelay_bViewer || (!wsrelay_bHosting && !wsrelay_bJoined) || sithMulti_IsSpectating())
        return 0;
    if (!sithPlayer_g_pLocalPlayerThing || !sithPlayer_g_pLocalPlayer || playerThingIdx >= JKPLAYER_NUM_INFOS || playerThingIdx > 255)
        return 0;
    if (!wsrelay_js_feed_on())
    {
        wsrelay_bPovSent = 0;
        wsrelay_bPalSent = 0;
        wsrelay_bTracksSent = 0;
        return 0;
    }
    return 1;
}

static void wsrelay_PovSend(const uint8_t* pBody, int len)
{
    uint8_t buf[4 + 128];
    uint16_t msgId = STDCOMM_WSRELAY_POV_MSG;
    uint16_t resendId = 0;
    if (len <= 0 || len > 128)
        return;
    memcpy(buf, &msgId, 2);
    memcpy(buf + 2, &resendId, 2);
    memcpy(buf + 4, pBody, len);
    wsrelay_js_feed_copy(buf, len + 4);
}

static void wsrelay_PovPutEffect(uint8_t* p, const stdPalEffect* pEffect)
{
    float f[3] = { (float)pEffect->tint.x, (float)pEffect->tint.y, (float)pEffect->tint.z };
    float fade = (float)pEffect->fade;
    memcpy(p, f, 12);
    p[12] = (uint8_t)stdMath_ClampInt(pEffect->add.x, 0, 255);
    p[13] = (uint8_t)stdMath_ClampInt(pEffect->add.y, 0, 255);
    p[14] = (uint8_t)stdMath_ClampInt(pEffect->add.z, 0, 255);
    p[15] = 0;
    memcpy(p + 16, &fade, 4);
    p[20] = pEffect->filter.x ? 1 : 0;
    p[21] = pEffect->filter.y ? 1 : 0;
    p[22] = pEffect->filter.z ? 1 : 0;
    p[23] = 0;
}

static void wsrelay_PovGetEffect(const uint8_t* p, stdPalEffect* pEffect)
{
    float f[3], fade;
    memcpy(f, p, 12);
    memcpy(&fade, p + 16, 4);
    stdPalEffects_ResetEffect(pEffect);
    pEffect->tint.x = stdMath_Clamp(f[0], 0.0, 1.0);
    pEffect->tint.y = stdMath_Clamp(f[1], 0.0, 1.0);
    pEffect->tint.z = stdMath_Clamp(f[2], 0.0, 1.0);
    pEffect->add.x = p[12];
    pEffect->add.y = p[13];
    pEffect->add.z = p[14];
    pEffect->fade = stdMath_Clamp(fade, 0.0, 1.0);
    pEffect->filter.x = p[20] ? 1 : 0;
    pEffect->filter.y = p[21] ? 1 : 0;
    pEffect->filter.z = p[22] ? 1 : 0;
}

// STATE when it's due: a new model at once.
static void wsrelay_PovState()
{
    SithThing* pThing = sithPlayer_g_pLocalPlayerThing;
    jkPlayerInfo* pInfo = &playerThings[playerThingIdx];
    uint8_t body[WSRELAY_POV_STATE_LEN];
    jkHudValues hud;
    memset(body, 0, sizeof(body));
    jkHud_LocalValues(pThing, &hud);
    int32_t modelId = pInfo->povModel.model3 ? (int32_t)pInfo->povModel.model3->id : -1;
    uint16_t flags16 = (uint16_t)hud.flags;
    float waggle[4] = { (float)jkPlayer_waggleVec.x, (float)jkPlayer_waggleVec.y, (float)jkPlayer_waggleVec.z, (float)jkPlayer_waggleMag };
    int16_t values[6] = { wsrelay_PovS16(hud.health), wsrelay_PovS16(hud.maxHealth), wsrelay_PovS16(hud.shields),
                          wsrelay_PovS16(hud.force), wsrelay_PovS16(hud.battery), wsrelay_PovS16(hud.ammo) };
    body[0] = 1;
    body[1] = (uint8_t)playerThingIdx;
    memcpy(body + 2, &flags16, 2);
    memcpy(body + 4, &modelId, 4);
    memcpy(body + 8, waggle, 16);
    memcpy(body + 24, values, 12);

    uint32_t now = stdPlatform_GetTimeMsec();
    int bChanged = !wsrelay_bPovSent || memcmp(body, wsrelay_aPovSent, sizeof(body)) != 0;
    int bModelNow = !wsrelay_bPovSent || memcmp(body + 4, wsrelay_aPovSent + 4, 4) != 0;
    if (!bModelNow && !(bChanged && now - wsrelay_povSentMs >= WSRELAY_POV_MIN_MS) && now - wsrelay_povSentMs < WSRELAY_POV_EVERY_MS)
        return;
    wsrelay_PovSend(body, sizeof(body));
    memcpy(wsrelay_aPovSent, body, sizeof(body));
    wsrelay_bPovSent = 1;
    wsrelay_povSentMs = now;
}

static void wsrelay_PovPal()
{
    stdPalEffect* p1 = stdPalEffects_GetEffectPointer(sithPlayer_g_pLocalPlayer->palEffectsIdx1);
    stdPalEffect* p2 = stdPalEffects_GetEffectPointer(sithPlayer_g_pLocalPlayer->palEffectsIdx2);
    // A new flash: any of it brighter than a frame ago (it only ever fades).
    int bFlash = p1->tint.x > wsrelay_palLast.tint.x + 0.001 || p1->tint.y > wsrelay_palLast.tint.y + 0.001
              || p1->tint.z > wsrelay_palLast.tint.z + 0.001 || p1->add.x > wsrelay_palLast.add.x
              || p1->add.y > wsrelay_palLast.add.y || p1->add.z > wsrelay_palLast.add.z;
    wsrelay_palLast = *p1;

    uint8_t body[WSRELAY_POV_PAL_LEN];
    memset(body, 0, sizeof(body));
    body[0] = 4;
    body[1] = (uint8_t)playerThingIdx;
    wsrelay_PovPutEffect(body + 4, p1);
    wsrelay_PovPutEffect(body + 28, p2);

    uint32_t now = stdPlatform_GetTimeMsec();
    // What doesn't fade on its own: the tint, the fade, the filters.
    int bChanged = !wsrelay_bPalSent || memcmp(body + 28, wsrelay_aPalSent + 28, 24) != 0
                || memcmp(body + 4 + 16, wsrelay_aPalSent + 4 + 16, 8) != 0;
    if (!bFlash && !(bChanged && now - wsrelay_palSentMs >= WSRELAY_POV_MIN_MS) && now - wsrelay_palSentMs < WSRELAY_POV_EVERY_MS)
        return;
    wsrelay_PovSend(body, sizeof(body));
    memcpy(wsrelay_aPalSent, body, sizeof(body));
    wsrelay_bPalSent = 1;
    wsrelay_palSentMs = now;
}

// TRACKS when it's due.
static void wsrelay_PovTracks()
{
    jkPlayerInfo* pInfo = &playerThings[playerThingIdx];
    rdModel3* pModel = pInfo->povModel.model3;
    rdPuppet* pPuppet = pInfo->povModel.puppet;
    uint32_t now = stdPlatform_GetTimeMsec();
    if (!pModel || !pPuppet || (wsrelay_bTracksSent && now - wsrelay_tracksSentMs < WSRELAY_POV_TRACKS_MS))
        return;
    uint8_t body[WSRELAY_POV_TRACKS_LEN];
    memset(body, 0, sizeof(body));
    int n = 0;
    for (int i = 0; i < RDPUPPET_MAX_TRACKS && i < 4; i++)
    {
        rdPuppetTrack* pTrack = &pPuppet->aTracks[i];
        if (!pTrack->status || !pTrack->keyframe || (pTrack->status & 8))
            continue;
        uint8_t* p = body + 8 + n * WSRELAY_POV_TRACK_LEN;
        int16_t pri[2] = { wsrelay_PovS16(pTrack->lowPri), wsrelay_PovS16(pTrack->highPri) };
        int32_t vals[2] = { (int32_t)pTrack->keyframe->id, (int32_t)pTrack->status };
        float f[3] = { (float)pTrack->field_120, (float)pTrack->upperFrame, (float)pTrack->speed };
        p[0] = (uint8_t)i;
        memcpy(p + 2, pri, 4);
        memcpy(p + 8, vals, 8);
        memcpy(p + 16, f, 12);
        n++;
    }
    int32_t modelId = (int32_t)pModel->id;
    body[0] = 5;
    body[1] = (uint8_t)playerThingIdx;
    body[2] = (uint8_t)n;
    memcpy(body + 4, &modelId, 4);
    wsrelay_PovSend(body, 8 + n * WSRELAY_POV_TRACK_LEN);
    wsrelay_bTracksSent = 1;
    wsrelay_tracksSentMs = now;
}

void stdComm_WSRelay_FeedPovFrame()
{
    if (!wsrelay_PovSending())
        return;
    wsrelay_PovState();
    wsrelay_PovPal();
    wsrelay_PovTracks();
}

void stdComm_WSRelay_FeedPovKey(int track, rdKeyframe* pKeyframe, int lowPri, int flags)
{
    if (!pKeyframe || track < 0 || track > 3 || !wsrelay_PovSending())
        return;
    rdModel3* pModel = playerThings[playerThingIdx].povModel.model3;
    if (!pModel)
        return;
    // The model it plays on first, when that's new (a weapon's raise plays
    // as it's picked).
    wsrelay_PovState();
    uint8_t body[20];
    int32_t vals[4] = { (int32_t)pModel->id, (int32_t)pKeyframe->id, (int32_t)lowPri, (int32_t)flags };
    body[0] = 2;
    body[1] = (uint8_t)playerThingIdx;
    body[2] = (uint8_t)(int8_t)track;
    body[3] = 0;
    memcpy(body + 4, vals, 16);
    wsrelay_PovSend(body, sizeof(body));
}

void stdComm_WSRelay_FeedPovStop(int track, flex_t fadeSecs)
{
    if (track < 0 || track > 3 || !wsrelay_PovSending())
        return;
    uint8_t body[8];
    float fade = (float)fadeSecs;
    body[0] = 3;
    body[1] = (uint8_t)playerThingIdx;
    body[2] = (uint8_t)(int8_t)track;
    body[3] = 0;
    memcpy(body + 4, &fade, 4);
    wsrelay_PovSend(body, sizeof(body));
}

// A viewer's copy of what each player's game said.
typedef struct wsrelay_PovOf
{
    int bState;
    rdVector3 waggleVec;
    flex_t waggleMag;
    jkHudValues hud;
    int8_t aTrack[4]; // their track: ours
    int bPal;
    stdPalEffect flashes;
    stdPalEffect tint;
    flex_t palAt; // sithTime_g_secGameTime when the flashes were said
} wsrelay_PovOf;
static wsrelay_PovOf wsrelay_aPovOf[JKPLAYER_NUM_INFOS];

static void wsrelay_PovReset()
{
    memset(wsrelay_aPovOf, 0, sizeof(wsrelay_aPovOf));
    for (int i = 0; i < JKPLAYER_NUM_INFOS; i++)
        memset(wsrelay_aPovOf[i].aTrack, 0xFF, sizeof(wsrelay_aPovOf[i].aTrack));
}

void stdComm_WSRelay_ProcessPov(SithMessage* pMsg)
{
    if (!stdComm_WSRelay_bViewer || !sithWorld_g_pCurrentWorld)
        return;
    const uint8_t* p = (const uint8_t*)pMsg->pktData;
    int len = (int)pMsg->netMsg.msg_size;
    if (len < 4)
        return;
    int type = p[0];
    int idx = p[1];
    if (idx >= JKPLAYER_NUM_INFOS || idx >= (int)jkPlayer_maxPlayers || idx == (int)playerThingIdx)
        return;
    jkPlayerInfo* pInfo = &playerThings[idx];
    wsrelay_PovOf* pOf = &wsrelay_aPovOf[idx];
    if (!pInfo->actorThing)
        return;
    // Animations are over by the time a viewer's in the level, as for
    // sithPuppet's (wsrelay_ViewerSkipsWhileSyncing).
    int bSync = (g_submodeFlags & 8) != 0;

    if (type == 1 && len >= WSRELAY_POV_STATE_LEN)
    {
        uint16_t flags16;
        int32_t modelId;
        float waggle[4];
        int16_t values[6];
        memcpy(&flags16, p + 2, 2);
        memcpy(&modelId, p + 4, 4);
        memcpy(waggle, p + 8, 16);
        memcpy(values, p + 24, 12);
        rdModel3* pModel = modelId >= 0 ? sithModel_GetModelByIndex(modelId) : NULL;
        if (pModel && (int32_t)pModel->id != modelId)
            pModel = NULL;
        if (pModel != pInfo->povModel.model3 || !pModel)
        {
            if (pModel != pInfo->povModel.model3)
                jkPlayer_SetPovModel(pInfo, pModel);
            memset(pOf->aTrack, 0xFF, sizeof(pOf->aTrack));
        }
        pOf->waggleVec.x = waggle[0];
        pOf->waggleVec.y = waggle[1];
        pOf->waggleVec.z = waggle[2];
        pOf->waggleMag = waggle[3];
        pOf->hud.flags = flags16;
        pOf->hud.health = values[0];
        pOf->hud.maxHealth = values[1];
        pOf->hud.shields = values[2];
        pOf->hud.force = values[3];
        pOf->hud.battery = values[4];
        pOf->hud.ammo = values[5];
        pOf->bState = 1;
    }
    else if (type == 2 && len >= 20 && !bSync)
    {
        int track = (int8_t)p[2];
        int32_t vals[4];
        memcpy(vals, p + 4, 16);
        rdModel3* pModel = pInfo->povModel.model3;
        rdPuppet* pPuppet = pInfo->povModel.puppet;
        if (!pModel || !pPuppet || (int32_t)pModel->id != vals[0])
            return;
        rdKeyframe* pKeyframe = sithKeyFrame_GetByIdx(vals[1]);
        if (!pKeyframe)
            return;
        int ours = sithPuppet_PlayKey(pPuppet, pKeyframe, vals[2], vals[2] + 2, vals[3], 0);
        if (track >= 0 && track < 4)
            pOf->aTrack[track] = (int8_t)ours;
    }
    else if (type == 3 && len >= 8 && !bSync)
    {
        int track = (int8_t)p[2];
        float fade;
        memcpy(&fade, p + 4, 4);
        if (track < 0 || track > 3)
            return;
        int ours = pOf->aTrack[track];
        pOf->aTrack[track] = -1;
        if (ours >= 0 && ours < 4 && pInfo->povModel.puppet)
            sithPuppet_StopKey(pInfo->povModel.puppet, ours, fade);
    }
    else if (type == 4 && len >= WSRELAY_POV_PAL_LEN)
    {
        wsrelay_PovGetEffect(p + 4, &pOf->flashes);
        wsrelay_PovGetEffect(p + 28, &pOf->tint);
        pOf->palAt = sithTime_g_secGameTime;
        pOf->bPal = 1;
    }
    else if (type == 5 && len >= 8 && !bSync)
    {
        int n = p[2];
        int32_t modelId;
        memcpy(&modelId, p + 4, 4);
        rdModel3* pModel = pInfo->povModel.model3;
        rdPuppet* pPuppet = pInfo->povModel.puppet;
        if (n > 4 || len < 8 + n * WSRELAY_POV_TRACK_LEN || !pModel || !pPuppet || (int32_t)pModel->id != modelId)
            return;
        rdKeyframe* aThere[4] = { NULL, NULL, NULL, NULL };
        for (int k = 0; k < n; k++)
        {
            const uint8_t* q = p + 8 + k * WSRELAY_POV_TRACK_LEN;
            int track = q[0];
            int16_t pri[2];
            int32_t vals[2];
            float f[3];
            memcpy(pri, q + 2, 4);
            memcpy(vals, q + 8, 8);
            memcpy(f, q + 16, 12);
            rdKeyframe* pKeyframe = sithKeyFrame_GetByIdx(vals[0]);
            if (!pKeyframe || track > 3)
                continue;
            aThere[k] = pKeyframe;
            int ours = -1;
            for (int j = 0; j < 4; j++)
            {
                if (pPuppet->aTracks[j].status && pPuppet->aTracks[j].keyframe == pKeyframe)
                {
                    ours = j;
                    break;
                }
            }
            if (ours < 0)
            {
                // Missed: as it plays there now, faded in.
                ours = sithPuppet_PlayKey(pPuppet, pKeyframe, pri[0], pri[1], 0x10, 0);
                if (ours < 0 || ours > 3)
                    continue;
                rdPuppetTrack* pTrack = &pPuppet->aTracks[ours];
                flex_t frames = (flex_t)pKeyframe->numFrames;
                flex_t at = (f[0] == f[0]) ? stdMath_Clamp(f[0], 0.0, frames) : 0.0;
                flex_t upperAt = (f[1] == f[1] && f[1] >= 0.0) ? f[1] : 0.0;
                pTrack->status = (vals[1] & ~(0x4 | 0x8)) | 0x1;
                pTrack->playSpeed = 1.0;
                pTrack->field_120 = at;
                pTrack->field_124 = at;
                pTrack->upperFrame = upperAt;
                if (f[2] == f[2] && f[2] >= 0.0 && f[2] < 1000.0)
                    pTrack->speed = f[2];
            }
            pOf->aTrack[track] = (int8_t)ours;
        }
        // Held here and gone there (it stopped there, and this game missed
        // that): it goes here too.
        for (int j = 0; j < 4; j++)
        {
            rdPuppetTrack* pTrack = &pPuppet->aTracks[j];
            if (!pTrack->status || !pTrack->keyframe || (pTrack->status & 8) || !(pTrack->status & (0x10 | 0x20 | 0x100)))
                continue;
            int bThere = 0;
            for (int k = 0; k < n; k++)
                bThere |= aThere[k] == pTrack->keyframe;
            if (!bThere)
                sithPuppet_StopKey(pPuppet, j, 0.1);
        }
    }
}

int stdComm_WSRelay_PovWaggle(int idx, rdVector3* pVec, flex_t* pMag)
{
    if (!stdComm_WSRelay_bViewer || idx < 0 || idx >= JKPLAYER_NUM_INFOS || !wsrelay_aPovOf[idx].bState)
        return 0;
    *pVec = wsrelay_aPovOf[idx].waggleVec;
    *pMag = wsrelay_aPovOf[idx].waggleMag;
    return 1;
}

int stdComm_WSRelay_PovHud(int idx, struct jkHudValues* pOut)
{
    if (!stdComm_WSRelay_bViewer || idx < 0 || idx >= JKPLAYER_NUM_INFOS || !wsrelay_aPovOf[idx].bState)
        return 0;
    *pOut = wsrelay_aPovOf[idx].hud;
    return 1;
}

void stdComm_WSRelay_ViewerPovFrame(int followIdx)
{
    // A spectator's own flashes (its body takes no hits, picks nothing up)
    // would never fade: none.
    stdPalEffects_FlushAllAdds();
    if (!stdComm_WSRelay_bViewer || !sithPlayer_g_pLocalPlayer)
        return;
    stdPalEffect* p1 = stdPalEffects_GetEffectPointer(sithPlayer_g_pLocalPlayer->palEffectsIdx1);
    stdPalEffect* p2 = stdPalEffects_GetEffectPointer(sithPlayer_g_pLocalPlayer->palEffectsIdx2);
    wsrelay_PovOf* pOf = NULL;
    if (followIdx >= 0 && followIdx < JKPLAYER_NUM_INFOS && sithCamera_g_pCurCamera == &sithCamera_g_aCameras[0] && wsrelay_aPovOf[followIdx].bPal)
        pOf = &wsrelay_aPovOf[followIdx];
    if (!pOf)
    {
        stdPalEffects_ResetEffect(p1);
        stdPalEffects_ResetEffect(p2);
        return;
    }
    // Faded since it was said, as sithPlayer_Tick fades a player's own.
    flex_t secs = sithTime_g_secGameTime - pOf->palAt;
    if (secs < 0.0)
        secs = 0.0;
    flex_t tintDown = secs * 0.4;
    int addDown = (int)(secs * 256.0 + 0.5);
    *p1 = pOf->flashes;
    p1->tint.x = stdMath_Clamp(p1->tint.x - tintDown, 0.0, 1.0);
    p1->tint.y = stdMath_Clamp(p1->tint.y - tintDown, 0.0, 1.0);
    p1->tint.z = stdMath_Clamp(p1->tint.z - tintDown, 0.0, 1.0);
    p1->add.x = stdMath_ClampInt(p1->add.x - addDown, 0, 255);
    p1->add.y = stdMath_ClampInt(p1->add.y - addDown, 0, 255);
    p1->add.z = stdMath_ClampInt(p1->add.z - addDown, 0, 255);
    *p2 = pOf->tint;
}

BOOL DirectPlay_Send(DPID idFrom, DPID idTo, void *lpData, DWORD dwDataSize)
{
    // Added: a keyframe's messages go to the room's feed, never to the
    // network (stdComm_WSRelay_FeedFrame).
    if (idTo == WSRELAY_FEED_KEY_ID)
    {
        if (wsrelay_bKeyframing)
            wsrelay_js_feed_key_packet(lpData, (int)dwDataSize);
        return 1;
    }

    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
        return stdComm_Imperial_Send(idFrom, idTo, lpData, dwDataSize);

    // Added: a viewer watching a match in the game sends nothing, and says
    // it did, but for its join request once the feed has ended: that's how
    // Synchronizing gives up (jkGuiMultiplayer_idk).
    if (stdComm_WSRelay_bViewer)
        return !(wsrelay_bViewEnded && dwDataSize >= 2 && *(const uint16_t*)lpData == DSS_JOINREQUEST);

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
    // Added: the second connection is the Imperial Network Protocol's
    stdComm_Imperial_bActive = ((intptr_t)a == STDCOMM_IMPERIAL_CONNECTION);
    if (stdComm_Imperial_bActive)
        return stdComm_Imperial_OpenConnection();

    stdComm_dword_8321DC = 1;
    return 0;
}

void stdComm_CloseConnection()
{
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
    {
        stdComm_Imperial_CloseConnection();
        stdComm_Imperial_bActive = 0;
        return;
    }

    wsrelay_DropListAsk(); // Added: the join screen is gone

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
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
        return stdComm_Imperial_Open(idx, pwPassword);

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
    // A lobby that checks passwords gets the one typed, for a game it listed
    // with the password box; the join screen didn't check it
    // (stdComm_WSRelay_LobbyChecksPassword). Not for one without: its box is
    // hidden, and may still hold what was typed for another game.
    int bSendPw = wsrelay_aShownPw[idx] && (jkGuiMultiplayer_aEntries[idx].sessionFlags & SESSIONFLAG_PASSWORD);
    wsrelay_SendJoin(wsrelay_aShownRooms[idx], bSendPw ? pwPassword : NULL);

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
        if (err == 5)
            return 0x88770154; // GUINET_WRONGPASSWORD
        if (err == 6)
            return STDCOMM_WSRELAY_PASSWORD_WAIT;
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
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
    {
        stdComm_Imperial_Close();
        return;
    }

    // Added: the viewer left the match it watched.
    if (stdComm_WSRelay_bViewer)
    {
        wsrelay_ViewerOff();
        return;
    }

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
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
    {
        stdComm_Imperial_SetSessionDesc(a1, maxPlayers);
        return;
    }

    _strncpy(wsrelay_storedEntry.mapJklFname, jkMain_aLevelJklFname, 0x20);
    // Added: the host's playlist can go on to a level in another episode. The
    // room says which (the lobby's list, players joining now, the match
    // record), and that one goes to joiners who don't have it.
    if (wsrelay_bHosting && jkRes_episodeGobName[0] &&
        __strnicmp(wsrelay_storedEntry.episodeGobName, jkRes_episodeGobName, sizeof(wsrelay_storedEntry.episodeGobName)))
    {
        stdPlatform_Printf("OpenJKDF2: the room's episode is %s now\n", jkRes_episodeGobName);
        stdString_SafeStrCopy(wsrelay_storedEntry.episodeGobName, jkRes_episodeGobName, sizeof(wsrelay_storedEntry.episodeGobName));
        wsrelay_js_map_host(wsrelay_storedEntry.episodeGobName, JKRES_GOB_EXT);
    }
    // Added: jkMain passes the smaller of the host's limit and the level's
    // player starts, which is what JK.EXE's DirectPlay session showed and held
    // joins to. The lobby used to get the host's own number, so a 16-start map
    // hosted for 32 read "3/32" and turned the 17th player away. Now the
    // level's starts have shared copies up to 32 (sithThing_AddSharedStarts),
    // so this is the host's limit unless the level has none.
    if (maxPlayers > 0 && maxPlayers <= JKPLAYER_NUM_INFOS)
        wsrelay_storedEntry.maxPlayers = maxPlayers;
    wsrelay_UpdateLobby();
}

BOOL DirectPlay_SetSessionFlagidk(int a1)
{
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
        return stdComm_Imperial_SetSessionFlagidk(a1);
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
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
        return stdComm_Imperial_CreatePlayer(pwIdk);
    return 1;
}

void DirectPlay_Close()
{
}

int DirectPlay_OpenHost(jkMultiEntry* pEntry)
{
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
        return stdComm_Imperial_OpenHost(pEntry);

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
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
        return stdComm_Imperial_GetSession_passwordidk(pEntry);

    wsrelay_storedEntry = *pEntry;

    jkPlayer_maxPlayers = pEntry->maxPlayers; // Hack?

    return 1;
}

// The relay's latest list onto the join screen (jkGuiMultiplayer_aEntries),
// and none while the socket is down. That list is forgotten then: a give-up
// on the next socket's first ask puts up no games, not the ones from before
// (stdComm_WSRelay_ListReady).
static void wsrelay_ShowList()
{
    int n = wsrelay_numListed;
    if (wsrelay_js_state() != WSRELAY_STATE_OPEN)
        n = wsrelay_numListed = 0;

    memset(jkGuiMultiplayer_aEntries, 0, sizeof(jkMultiEntry) * 32);
    for (int i = 0; i < n; i++)
    {
        jkGuiMultiplayer_aEntries[i] = wsrelay_aListEntries[i];
        wsrelay_aShownRooms[i] = wsrelay_aListRooms[i];
        strcpy(wsrelay_aShownTails[i], wsrelay_aListTails[i]);
        wsrelay_aShownHits[i] = wsrelay_aListHits[i];
        wsrelay_aShownPw[i] = wsrelay_aListPw[i];

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
    wsrelay_bListUnshown = 0;
}

// The join screen is done with the list it asked for: it closed, or the
// player is joining a game from it (stdComm_Open). (Back on the join screen
// from the character screen or an error dialog without either, the answer is
// put up, as the wait had put it up before Ok was taken.)
int DirectPlay_EnumSessions2()
{
    wsrelay_DropListAsk();
    return 0;
}

// The join screen opens (jkGuiMultiplayer_Show). If the answer to its first
// ask is late, it gives up with no games, as on the first visit, not with
// the last visit's: neither the list put up last nor the answer to an ask
// it was done with, which that ask reads first (stdComm_EnumSessions).
void stdComm_WSRelay_ForgetList()
{
    wsrelay_bListForget = 1;
}

int stdComm_EnumSessions(int a, void* b)
{
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
        return stdComm_Imperial_EnumSessions();

    Hack_ResetClients();

    // Ask again soon instead of every 5 seconds, like the GNS enum thread.
    jkGuiMultiplayer_dword_5564E8 -= 3000;

    wsrelay_bListAsked = 0;
    if (wsrelay_js_state() != WSRELAY_STATE_OPEN)
    {
        // Don't block the menu on a connect; the next poll picks it up.
        wsrelay_js_connect(STDCOMM_WSRELAY_NETCODE);
    }
    else
    {
        // Added: what came in since the join screen was done with its last
        // ask (wsrelay_DropListAsk) is read first, as that ask's answer isn't
        // this one's.
        if (wsrelay_bListDropped)
            wsrelay_PumpControl();
        wsrelay_bListDropped = 0;
        // Added: nor is a list the other game asked for, before the engine
        // restarted into this one, what this ask gives up with if its answer
        // is late (stdComm_WSRelay_ListReady), nor one from before the join
        // screen opened (stdComm_WSRelay_ForgetList).
        if (wsrelay_bListMots != Main_bMotsCompat || wsrelay_bListForget)
        {
            wsrelay_numListed = 0;
            wsrelay_bListUnshown = 1;
        }
        wsrelay_bListMots = Main_bMotsCompat;
        wsrelay_bListForget = 0;

        // Only the games this one can join: Jedi Knight can't join a game
        // Mysteries of the Sith hosts, or the other way round.
        wsrelay_bListReceived = 0;
        wsrelay_SendText(Main_bMotsCompat ? "list mots" : "list jk1");

        // Changed: the answer isn't waited for here. The wait (up to
        // WSRELAY_LIST_MS) held the join screen still for a round trip to
        // the lobby every 2 s. The screen puts the list up when it comes in,
        // as it did after the wait (stdComm_WSRelay_ListReady). An answer
        // already here goes up now, as the wait put it up at once.
        wsrelay_PumpControl();
        if (!wsrelay_bListReceived && wsrelay_js_state() == WSRELAY_STATE_OPEN)
        {
            wsrelay_bListAsked = 1;
            wsrelay_bListGaveUp = 0;
            wsrelay_msecListAsked = stdPlatform_GetTimeMsec();
            return STDCOMM_WSRELAY_LIST_LATER;
        }
    }

    wsrelay_ShowList();
    return 0;
}

// Called each frame of the join screen. 1 once the list it asked for
// (stdComm_EnumSessions) is in jkGuiMultiplayer_aEntries, for the screen to
// put up: when the lobby's answer comes, when the socket closes (no games
// then), or after WSRELAY_LIST_MS with the list it had, as the wait did,
// unless that's the one up already, and then again when the answer comes.
int stdComm_WSRelay_ListReady()
{
    if (stdComm_Imperial_bActive || !wsrelay_bListAsked)
        return 0;

    wsrelay_PumpControl();
    if (!wsrelay_bListReceived && wsrelay_js_state() == WSRELAY_STATE_OPEN)
    {
        // After WSRELAY_LIST_MS the list it had, as the wait gave up. The
        // answer still goes up when it comes: the wait left it for the next
        // ask to take for its own, and every list after it went up an ask
        // late.
        if (wsrelay_bListGaveUp || stdPlatform_GetTimeMsec() - wsrelay_msecListAsked < WSRELAY_LIST_MS)
            return 0;
        wsrelay_bListGaveUp = 1;
        // Nothing new since the list up now went up: it isn't put up
        // again, as a rebuild sets the selection back and would undo a
        // click on a row half done (jkGuiMultiplayer_sub_4140B0). A list
        // with no games has no row to click, and goes up again.
        if (!wsrelay_bListUnshown && wsrelay_numShown > 0 && dplay_dword_55D618 == wsrelay_numShown)
            return 0;
        wsrelay_ShowList();
        return 1;
    }

    wsrelay_bListAsked = 0;
    wsrelay_ShowList();
    return 1;
}

void DirectPlay_EnumPlayers(int a)
{
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
    {
        stdComm_Imperial_EnumPlayers();
        return;
    }
    Hack_ResetClients();
}

int DirectPlay_StartSession(void* a, void* b)
{
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
        stdComm_Imperial_EnumPlayers();
    return 1;
}

void DirectPlay_Destroy()
{
}

int DirectPlay_IdkSessionDesc(jkMultiEntry* pEntry)
{
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
        return stdComm_Imperial_IdkSessionDesc(pEntry);
    return 1;
}

int stdComm_WSRelay_LobbyChecksPassword(int idx)
{
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
        return 0;
    return idx >= 0 && idx < wsrelay_numShown && wsrelay_aShownPw[idx];
}

void stdComm_WSRelay_DescribeHits(int idx, int bShort, char16_t* pOut, int maxChars)
{
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
    {
        stdComm_Imperial_DescribeHits(idx, bShort, pOut, maxChars);
        return;
    }

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
    if (stdComm_Imperial_bActive) // Added: the Imperial Network Protocol connection
    {
        stdComm_Imperial_DescribeMod(idx, pOut, maxChars);
        return;
    }

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

// ---- The match record: what this game sees of its match, for the lobby to
// keep (packaging/wasm/ladder.js, which sends it; the lobby's src/ladder.js
// has what becomes of it). Every game counts kills by itself as they're
// announced, so each reports the ones it counts; the host's scoreboard is the
// one the others take over, so only its game's is used.

EM_JS(void, wsrelay_js_match_start, (const char* pMap, const char16_t* pTitle, int mode, int scoreLimit, int timeLimitMin, int bHost, int bIdle, const char* pBuild), {
    if (!Module.jkLadder) return;
    var chars = [];
    for (var i = 0; pTitle && i < 64; i++) {
        var ch = HEAPU16[(pTitle >> 1) + i];
        if (!ch) break;
        chars.push(ch);
    }
    Module.jkLadder.start(UTF8ToString(pMap), String.fromCharCode.apply(null, chars), mode, scoreLimit, timeLimitMin,
                          !!bHost, !!bIdle, UTF8ToString(pBuild));
});

// The host hosting its playlist (jkPlaylist_Position): this level's place in
// it from 0, how many levels it has, whether it loops and the next one's name.
EM_JS(void, wsrelay_js_match_playlist, (int idx, int count, int bLoop, const char16_t* pNext), {
    if (!Module.jkLadder || !Module.jkLadder.playlist) return;
    var chars = [];
    for (var i = 0; pNext && i < 64; i++) {
        var ch = HEAPU16[(pNext >> 1) + i];
        if (!ch) break;
        chars.push(ch);
    }
    Module.jkLadder.playlist(idx, count, !!bLoop, String.fromCharCode.apply(null, chars));
});

EM_JS(void, wsrelay_js_match_kill, (int victim, int killer, int kind), {
    if (Module.jkLadder) Module.jkLadder.kill(victim, killer, kind);
});

// pRows: n rows of six (player id, score, kills, deaths, suicides, team).
// mode: the game's mode flags as they are now, which a level's script can add
// to. scripted: what the level's script has set of a player's by itself
// (STDCOMM_WSRELAY_SCRIPTED_*).
EM_JS(void, wsrelay_js_match_scores, (const int32_t* pRows, int n, const int32_t* pTeams, int mode, int scripted), {
    if (!Module.jkLadder) return;
    var rows = [];
    for (var i = 0; i < n; i++) {
        var row = [];
        for (var j = 0; j < 6; j++) row.push(HEAP32[(pRows >> 2) + i * 6 + j]);
        rows.push(row);
    }
    var teams = [];
    for (var t = 0; t < 5; t++) teams.push(HEAP32[(pTeams >> 2) + t]);
    Module.jkLadder.scores(rows, teams, mode, scripted);
});

EM_JS(void, wsrelay_js_match_end, (int bLeft), {
    if (Module.jkLadder) Module.jkLadder.end(!!bLeft);
});

// What killed this game's player, from when its health ran out until the kill
// is counted.
static int wsrelay_matchKind = 0;
static int wsrelay_bMatchOn = 0;
// What the level's script has set of a player's score by itself, on the host
// (STDCOMM_WSRELAY_SCRIPTED_*). Kept from before the level's start is
// reported, as a script can do it as the level opens, until its end.
static int wsrelay_matchScripted = 0;

void stdComm_WSRelay_MatchScores(void)
{
    static int32_t aRows[32 * 6];
    static int32_t aTeams[5];
    int n = 0;

    if (!wsrelay_bMatchOn)
        return;
    for (int i = 0; i < jkPlayer_maxPlayers && i < 32; i++)
    {
        SithPlayer* pPlayer = &jkPlayer_playerInfos[i];
        if (!(pPlayer->flags & 1) || !pPlayer->playerNetId)
            continue;
        int32_t* pRow = &aRows[n++ * 6];
        pRow[0] = (int32_t)pPlayer->playerNetId;
        pRow[1] = pPlayer->score;
        pRow[2] = pPlayer->numKills;
        pRow[3] = pPlayer->numKilled;
        pRow[4] = pPlayer->numSuicides;
        pRow[5] = pPlayer->teamNum;
    }
    for (int i = 0; i < 5; i++)
        aTeams[i] = sithNet_teamScore[i];
    wsrelay_js_match_scores(aRows, n, aTeams, sithNet_MultiModeFlags, wsrelay_matchScripted);
}

// The host's game clock in its level, in ms: what its time limit goes by
// (sithMulti.c), which falls behind the wall clock when frames take longer
// than the game lets one run (SITHTIME_MAXDELTA). ladder.js reads it for each
// report, for the lobby to show the time left by. -1 when this game isn't the
// host of a level that's reported.
EMSCRIPTEN_KEEPALIVE int stdComm_WSRelay_MatchClock(void)
{
    if (!wsrelay_bMatchOn || !sithNet_isServer)
        return -1;
    return (int)sithTime_g_msecGameTime;
}

void stdComm_WSRelay_MatchScripted(int what)
{
    // The host's only: its scoreboard is the one the others take over.
    if (sithNet_isMulti && sithNet_isServer)
        wsrelay_matchScripted |= what;
}

// The level's title as the loading screen shows it, or the level's name where
// it has none. (jkGuiTitle_quicksave_related_func1 finds it the same way, and
// reads the loading screen's texts again besides, which this leaves alone.)
static char16_t* wsrelay_matchTitle(char* pJklFname)
{
    char aKey[64];
    char16_t* pTitle;

    stdFnames_CopyShortName(aKey, 64, pJklFname);
    jkGuiTitle_sub_4189A0(aKey);
    pTitle = NULL;
#ifdef QOL_IMPROVEMENTS
    pTitle = stdStrTable_GetValue(&jkStrings_tableExtOver, aKey);
#endif
    if (!pTitle)
        pTitle = stdStrTable_GetValue(&jkCog_strings, aKey);
    if (!pTitle)
        pTitle = jkStrings_GetUniStringWithFallback(aKey);
    return pTitle;
}

void stdComm_WSRelay_MatchStart(void)
{
    // The lobby's rooms only: a match on the Imperial Network Protocol relay
    // isn't one of its.
    if (!sithNet_isMulti || !sithWorld_g_pCurrentWorld || stdComm_Imperial_bActive || stdComm_WSRelay_bViewer)
        return;
    wsrelay_matchKind = 0;
    wsrelay_bMatchOn = 1;
    wsrelay_js_match_start(sithWorld_g_pCurrentWorld->map_jkl_fname,
                           wsrelay_matchTitle(sithWorld_g_pCurrentWorld->map_jkl_fname),
                           sithNet_MultiModeFlags, sithNet_scorelimit, sithNet_multiplayer_timelimit / 60000,
                           sithNet_isServer, sithNet_isServer && jkGuiNetHost_bIsDedicated, openjkdf2_aReleaseCommitShort);
    // The host's playlist, for the lobby's room windows to say what's next.
    int count = 0, bLoop = 0;
    const char16_t* pNext = NULL;
    int idx = jkPlaylist_Position(&count, &bLoop, &pNext);
    if (idx >= 0)
        wsrelay_js_match_playlist(idx, count, bLoop, pNext);
    stdComm_WSRelay_MatchScores();
}

void stdComm_WSRelay_MatchDying(SithThing* pVictim, SithThing* pSource, int damageType)
{
    if (wsrelay_bMatchOn && pVictim && pVictim == sithPlayer_g_pLocalPlayerThing)
        wsrelay_matchKind = pSource ? stdComm_WSRelay_HitKind(pSource, damageType) : STDCOMM_WSRELAY_KIND_OTHER;
}

void stdComm_WSRelay_MatchKill(SithPlayer* pVictim, SithThing* pVictimThing, SithThing* pKiller)
{
    if (!wsrelay_bMatchOn || !pVictim || !pVictim->playerNetId)
        return;
    // Nobody's kill, as the game scores it: a fall, the level, the player's
    // own doing.
    int killer = 0;
    if (pKiller && pKiller != pVictimThing && pKiller->type == SITH_THING_PLAYER && pKiller->actorParams.pPlayer)
        killer = (int)pKiller->actorParams.pPlayer->playerNetId;
    int kind = 0;
    if (pVictimThing == sithPlayer_g_pLocalPlayerThing)
    {
        kind = wsrelay_matchKind ? wsrelay_matchKind : STDCOMM_WSRELAY_KIND_OTHER;
        wsrelay_matchKind = 0;
    }
    wsrelay_js_match_kill((int)pVictim->playerNetId, killer, kind);
}

void stdComm_WSRelay_MatchEnd(int bLeft)
{
    if (wsrelay_bMatchOn)
    {
        stdComm_WSRelay_MatchScores();
        wsrelay_bMatchOn = 0;
        wsrelay_js_match_end(bLeft);
    }
    // The next level's script starts with nothing set, also after a level
    // that wasn't reported (the Imperial relay's).
    wsrelay_matchScripted = 0;
}
