#ifndef _JKRELAY_PROTO_H
#define _JKRELAY_PROTO_H

// "Imperial Network Protocol" relay protocol, shared by the game client
// (jkrelay_client.c) and the server (tools/jkrelay/jkrelay_server.c).
//
// Every player keeps one UDP "connection" to the relay server and the server
// forwards game traffic between the players of a session. Only outbound
// traffic is needed on the players' side, so no port forwarding: the server
// is the only machine that must be reachable. Hosts and joiners can also get
// a direct link (see "Direct links" below).
//
// The relay replaces DirectPlay's transport only. The game's own messages
// (DSS) are carried unchanged, so gameplay is identical to DirectPlay games.
//
// All integers are little-endian. Every packet starts with jkr_hdr.

#include <stdint.h>

#define JKR_MAGIC           0x314B524Au   // "JRK1"
#define JKR_VERSION         1
#define JKR_DEFAULT_PORT    27900
#define JKR_DEFAULT_SERVER_PORT 27960   // a dedicated server's own (jkrelay_listen.c), console "connect"
#define JKR_MAX_PACKET      1400          // keep under typical internet MTU
#define JKR_MAX_NAME        64            // UTF-16 code units, session/player names
#define JKR_MAX_PASSWORD    32

// Packet types
enum
{
    // client -> server requests (answered with the same seq; retried until answered)
    JKR_HELLO         = 1,   // -> WELCOME. Only packet accepted without a token.
    JKR_HOST          = 2,   // desc, password            -> HOSTED
    JKR_LIST          = 3,   // appGuid, start index[, u32 list flags] -> LISTREPLY
    JKR_JOIN          = 4,   // session guid, password    -> JOINED | FAIL
    JKR_CREATEPLAYER  = 5,   // flags, name               -> PLAYERID
    JKR_DESTROYPLAYER = 6,   // id                        -> OK
    JKR_SETDESC       = 7,   // desc (host only)          -> OK
    JKR_LEAVE         = 8,   //                           -> OK
    JKR_REHOST        = 9,   // see "Resuming" below      -> OK | FAIL
    JKR_REJOIN        = 10,  // see "Resuming" below      -> OK | FAIL
    JKR_ANNOUNCE      = 11,  // see "Master server" below -> OK | FAIL
    JKR_RCON_CHALLENGE = 12, // see "Rcon" below (no token)  -> RCON_NONCE
    JKR_RCON          = 13,  // see "Rcon" below (no token)  -> RCON_OUTPUT...

    // server -> client replies
    JKR_WELCOME       = 32,  // token
    JKR_HOSTED        = 33,  // desc (guid filled), system player id
    JKR_LISTREPLY     = 34,  // total, count, descs
    JKR_JOINED        = 35,  // desc, system player id, number of existing players
    JKR_PLAYERID      = 36,  // id
    JKR_OK            = 37,
    JKR_FAIL          = 38,  // u32 HRESULT
    JKR_UNKNOWN       = 39,  // "who are you": the token (echoed in the header) isn't known
                             // here (the server restarted or dropped us). Not a reply.
    JKR_RCON_NONCE    = 40,  // nonce[16]
    JKR_RCON_OUTPUT   = 41,  // u16 len, text; header reserved = part number | JKR_RCON_LAST

    // server -> client reliable events (seq = event number), client sends ACK
    JKR_EVENT         = 48,
    JKR_ACK           = 49,  // client -> server, seq = highest event number processed

    // either direction, unreliable
    JKR_DATA          = 64,  // u32 from, u32 to (0 = everyone else), payload
    JKR_KEEPALIVE     = 65,

    // player <-> player over a direct link (token = the link's key), unreliable
    JKR_PUNCH         = 66,  // u32 sender's system player id, u8 heard (1 = your punches reach me)
};

// Event kinds (first byte of an EVENT body)
enum
{
    JKR_EV_PLAYER_CREATED   = 1,  // u8 initial, u32 id, u32 sysId, u32 flags, name
    JKR_EV_PLAYER_DESTROYED = 2,  // u32 id
    JKR_EV_SESSION_LOST     = 3,  // optional: u8 reason (JKR_LOST_*), u16 minutes (a ban's; 0 = for good)
    JKR_EV_DESC_CHANGED     = 4,  // desc
    JKR_EV_PEER             = 5,  // see "Direct links" below
};

// Why the game was lost (JKR_EV_SESSION_LOST) or a JOIN/REJOIN refused (JKR_FAIL: after
// the u32 HRESULT, the same u8 reason and u16 minutes). Old clients read neither. 2026-10-28.
#define JKR_LOST_HOST_LEFT  0
#define JKR_LOST_KICKED     1
#define JKR_LOST_BANNED     2

// Direct links (2026-10-27). The server introduces a host and each joiner that
// both asked for it; they punch through their NATs from the socket they use for
// the server, and then send game data (JKR_DATA, token = link key) straight to
// each other. Only host <-> joiner: a joiner never learns another joiner's
// address. A joiner with a link sends everything to the host, which forwards it
// to the other players (over their links, or through the server, which lets the
// host send on other players' behalf). Without a link everything goes through
// the server as before, so old clients and old servers keep working.
//
// JKR_HOST / JKR_JOIN take an optional extension after the password (old
// servers ignore it):
//   u32 flags (JKR_JF_DIRECT), u32 LAN IPv4 (host order), u16 LAN port
// JKR_EV_PEER body after the kind byte:
//   u32 peer's system player id, u32 link key,
//   u32 public IPv4, u16 public port, u32 LAN IPv4, u16 LAN port (host order),
//   u8 flags (JKR_PEER_AT_SERVER)
#define JKR_JF_DIRECT        0x1   // wants a direct link
#define JKR_JOIN_EXT_SIZE    10
#define JKR_EV_PEER_SIZE     22
#define JKR_PEER_AT_SERVER   0x1   // the public address is on the server's own network:
                                   // use the server's address with that port

// Resuming (2026-10-27). When the server restarts, every session on it is
// gone, but the games themselves go on (they live on the players' machines,
// and direct links keep carrying data). The server answers packets with an
// unknown token with JKR_UNKNOWN; a client in a game then handshakes again
// (HELLO) and puts its game back:
//   host:   JKR_REHOST  appGuid[16], desc (its guid kept), password, join ext,
//                       u32 next player id to hand out, players
//   joiner: JKR_REJOIN  session guid[16], password, join ext, players
//   players = u8 count, count x (u32 id, u32 flags, name): the sender's own,
//             its system player first
// Player ids stay the same, so the game never notices. The server takes
// REJOINs for a game only within JKR_REJOIN_WINDOW_MS after its REHOST (until
// the host is back: FAIL NOSESSIONS, and the joiner retries; later: FAIL
// ACCESSDENIED, and the joiner gives up), and tells everyone about the
// returning players as "initial" PLAYER_CREATED events. Players who don't come
// back are removed by each client JKR_REJOIN_WINDOW_MS after it resumed. A host
// keeps its game while the server is away and resumes whenever it's back; a
// joiner gives up after JKR_RESUME_MS unless it has a direct link to the host.
// Master server (2026-10-27). A dedicated server running its own server
// (jkrelay_listen.c) lists itself on a relay server, which then shows it in
// its game list next to the games hosted on it; players join it at its own
// address. The listing comes from the dedicated server's own socket:
//   JKR_ANNOUNCE  appGuid[16], desc (its game, curPlayers filled in), u16 port
//                 -> OK u8 listed (1 once the relay server reached it)
// every JKR_ANNOUNCE_MS and when the game changes; a listing not renewed for
// JKR_ANNOUNCE_TTL_MS is dropped. The relay server lists it at the address
// the announcement came from, with the announced port, once a HELLO it sends
// there is answered (so unreachable servers aren't listed).
// LIST takes optional u32 flags after the start index; with JKR_LF_SERVERS
// the reply (JKR_LR_ADDRS in the header's reserved field) has after each desc
//   u32 IPv4 (host order), u16 port, u8 flags (JKR_LA_*)
// (0, 0, 0 for a game hosted on the relay server itself). Old clients don't
// ask, so they never see games they couldn't join.
#define JKR_LF_SERVERS        0x1
#define JKR_LR_ADDRS          0x1
#define JKR_LIST_ADDR_SIZE    7
#define JKR_LA_SERVER         0x1   // a server of its own, at the address given
#define JKR_LA_AT_SERVER      0x2   // ...on the relay server's own network: use the relay server's address
#define JKR_ANNOUNCE_MS       10000
#define JKR_ANNOUNCE_TTL_MS   35000

// Rcon (2026-10-27): remote admin of a dedicated server's own server
// (jkrelay_listen.c), like Quake 3's, without sending the password. Packets go
// to the server's port without a HELLO first; seq ties the answers to them.
//   JKR_RCON_CHALLENGE  JKR_RCON_PAD zero bytes (no bigger an answer than the ask)
//                       -> JKR_RCON_NONCE nonce[16], valid JKR_RCON_NONCE_MS, once
//   JKR_RCON            nonce[16], mac[32], u16 len, command (ASCII, <= JKR_RCON_MAX_CMD)
//                       mac = HMAC-SHA256(password, nonce || command)
//                       -> JKR_RCON_OUTPUT parts (the command's console output,
//                          or "Bad rcon password."), the last one with JKR_RCON_LAST
// After JKR_RCON_MAX_BAD wrong ones, an address is ignored for JKR_RCON_BLOCK_MS.
#define JKR_RCON_PAD          32
#define JKR_RCON_MAX_CMD      480
#define JKR_RCON_NONCE_MS     10000
#define JKR_RCON_MAX_BAD      5
#define JKR_RCON_BLOCK_MS     300000
#define JKR_RCON_LAST         0x8000

#define JKR_REJOIN_WINDOW_MS  30000
#define JKR_RESUME_MS         60000
#define JKR_RESUME_RETRY_MS   500

#define JKR_PUNCH_MS          250    // punch interval for the first JKR_PUNCH_FAST_MS
#define JKR_PUNCH_FAST_MS     5000
#define JKR_PUNCH_SLOW_MS     2000   // then keep trying at this rate
#define JKR_LINK_KEEPALIVE_MS 1000
#define JKR_LINK_QUIET_MS     2000   // silence before packets go through the server meanwhile
#define JKR_LINK_TIMEOUT_MS   5000   // silence before a link is dropped and punched again

// Player flags (same values as DirectPlay's)
#define JKR_PF_SYSTEM     0x1
#define JKR_PF_HOST       0x2

typedef struct
{
    uint32_t magic;
    uint8_t  type;
    uint8_t  version;
    uint16_t reserved;
    uint32_t token;   // from WELCOME; 0 in HELLO
    uint32_t seq;     // request id, or event number
} jkr_hdr;            // 16 bytes on the wire

#define JKR_HDR_SIZE 16

// Session description on the wire:
//   guid[16], flags u32, maxPlayers u32, curPlayers u32, user[4] u32,
//   u8 nameLen, nameLen x u16
// (= DPSESSIONDESC2's fields as JK uses them, see stdComm_dplay.c)
#define JKR_DESC_FIXED (16 + 4 * 7 + 1)

// Timing
#define JKR_RETRY_MS          250     // request / event resend interval
#define JKR_REQUEST_TIMEOUT_MS 8000
#define JKR_KEEPALIVE_MS      3000    // also keeps players' NAT mappings open
#define JKR_TIMEOUT_MS        20000   // silence before the other side is considered gone

#endif // _JKRELAY_PROTO_H
