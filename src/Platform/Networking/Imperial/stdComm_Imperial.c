// The Imperial Network Protocol connection: games on the desktop OpenJKDF2
// relay, with desktop players and dedicated servers.
//
// The transport is the desktop's own client (jkrelay_client.c, built with
// JKRELAY_STANDALONE_CLIENT), which in a browser sends its packets over a
// WebSocket to the relay server (jkrelay_server -w, behind wss:// on a TLS
// proxy) instead of UDP. This file is the desktop's glue for it
// (stdComm_dplay.c): game descriptions to and from jkMultiEntry, players, and
// the relay's player events as the engine's DirectPlay ones. Player ids are
// the relay's, as on the desktop, not the lobby's 1..64.
//
// Messages use OpenJKDF2's layouts, as on the web lobby. A desktop host says
// it does too (IMPERIAL_SESSION_OPENJKDF2WIRE); an older one sends JK.EXE's,
// which this build can't read, so its games can't be joined from here.
//
// The server: Module.jkImperialUrl, else ?imperial=<host or URL> on the page,
// else this page's own host at /imperial (wss:// on an https page). A bare
// host means wss://<host>/imperial, host:port ws://host:port/
// (jkrelay_client.c).
//
// A dedicated server's next level (its rotation, an admin's or a vote's pick)
// comes in its game's description (stdComm_Imperial_GetSessionNextMap,
// jkMain_FollowHostMap). The console has the desktop's callvote, vote,
// servermaps and rcon.
//
// Hit rules: the host's (a dedicated server's sv_hitRules, a browser host's
// host screen), which its hellos tell the other games (stdComm_WSRelayHits.c);
// its game lists Hit what you see with IMPERIAL_SESSION_HWYS. Position
// updates are numbered in the relay's data header for its claims.
//
// Maps: a joiner gets the host's custom episode before the level loads, as on
// the lobby (maps.js), from a host that says it serves it
// (IMPERIAL_SESSION_XFER); see imperial_XferPump. Mods: the game list carries
// each host's mods.js tail (jkrelay_proto.h, "Mods"); a joiner gets onto the
// host's mod (mods.js) after joining, restarting into it if it has to.

#include "stdComm_Imperial.h"

#ifdef PLATFORM_WSRELAY

#include "Win95/stdComm.h"
#include "Win95/WinIdk.h"
#include "Dss/sithMulti.h"
#include "Platform/Networking/WSRelay/stdComm_WSRelay.h"
#include "Gameplay/sithPlayer.h"
#include "Devices/sithConsole.h"
#include "General/stdString.h"
#include "Main/jkRes.h"
#include "stdPlatform.h"
#include "jk.h"

#include <emscripten.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DPS_BACKEND_PREFIX jkrelay
#include "dplay_shim.h"

void jkrelay_SetServer(const char* server); // jkrelay_client.c
void jkrelay_Idle(void);
int jkrelay_LostReason(int* pMinutes);
int jkrelay_JoinRefusal(int* pMinutes);
void jkrelay_SetSendSeq(uint32_t seq);
int jkrelay_LastRecvSeq(uint32_t* pSeq);
void jkrelay_SetBigHandler(void (*fn)(uint32_t from, const uint8_t* p, size_t n));
int jkrelay_SendBig(uint32_t idFrom, uint32_t idTo, const uint8_t* p, size_t n);
uint32_t jkrelay_LocalSystemId(void);
uint32_t jkrelay_HostSystemId(void);
int jkrelay_IsHost(void);
void jkrelay_SetHostTail(const char* hex);
const char* jkrelay_GetSessionTail(const uint8_t guid[16]);
int jkrelay_Rcon(const char* server, const char* password, const char* cmd, char* out, size_t cap, int timeoutMs);

// As the desktop's stdComm_dplay.c.
#define IMPERIAL_MAX_SESSIONS (32)
#define IMPERIAL_RET_PLAYERLOST (2)
#define IMPERIAL_RET_NEWPLAYER (DPSYS_DELETEPLAYER)
#define IMPERIAL_HOST_BASE_FLAGS (DPS_SESSION_KEEPALIVE | DPS_SESSION_NODATAMESSAGES)
#define IMPERIAL_SESSIONFLAG_MASK (~(uint32_t)SESSIONFLAG_ISDEDICATED)
#define IMPERIAL_SESSION_NEXTMAP 0x40000000u        // a dedicated host named the next level
#define IMPERIAL_SESSION_OPENJKDF2WIRE 0x20000000u  // the game's messages use OpenJKDF2's layouts
#define IMPERIAL_SESSION_HWYS 0x10000000u           // its host plays Hit what you see
#define IMPERIAL_SESSION_XFER 0x08000000u           // its host serves its map (and mod) to joiners

static const uint8_t imperial_kJkAppGuid[16] = {
    0xC0, 0x13, 0x06, 0xBF, 0x79, 0xDE, 0xD0, 0x11, 0x99, 0xC9, 0x00, 0xA0, 0x24, 0x76, 0xAD, 0x4B
};

int stdComm_Imperial_bActive = 0;
static int imperial_bStarted = 0;
static int imperial_bInGame = 0;
static int imperial_numListed = 0;
static int imperial_aListedWire[IMPERIAL_MAX_SESSIONS];
static int imperial_aListedHwys[IMPERIAL_MAX_SESSIONS];
static int imperial_aListedXfer[IMPERIAL_MAX_SESSIONS];
static char imperial_aListedTail[IMPERIAL_MAX_SESSIONS][256];   // its host's mod (mods.js tail), "" if not said
static char imperial_server[256];
static char imperial_aJoinError[160]; // why the last join failed, for the join screen's dialog

EM_JS(void, imperial_js_server, (char* pOut, int outLen), {
    var url = Module['jkImperialUrl'];
    if (!url && typeof location !== 'undefined') {
        try { url = new URLSearchParams(location.search).get('imperial'); } catch (e) {}
        if (!url)
            url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/imperial';
    }
    stringToUTF8(url ? url : "", pOut, outLen);
});

static const uint8_t* imperial_AppGuid()
{
    const uint32_t* g = WinIdk_GetDplayGuid();
    if (g && (g[0] | g[1] | g[2] | g[3]))
        return (const uint8_t*)g;
    return imperial_kJkAppGuid;
}

static size_t imperial_StrLen16(const uint16_t* s)
{
    size_t n = 0;
    if (s) while (s[n]) n++;
    return n;
}

static void imperial_Append16(uint16_t* dst, size_t dstLen, size_t* pos, const uint16_t* src, size_t srcLen)
{
    for (size_t i = 0; i < srcLen && *pos + 1 < dstLen; i++)
        dst[(*pos)++] = src[i];
    dst[*pos] = 0;
}

static void imperial_AppendAscii(uint16_t* dst, size_t dstLen, size_t* pos, const char* src, size_t srcMax)
{
    for (size_t i = 0; i < srcMax && src[i] && *pos + 1 < dstLen; i++)
        dst[(*pos)++] = (uint8_t)src[i];
    dst[*pos] = 0;
}

static void imperial_CopyToAscii(char* dst, size_t dstLen, const uint16_t* src, size_t srcLen)
{
    size_t n = 0;
    for (; n < srcLen && n + 1 < dstLen; n++)
        dst[n] = (src[n] < 0x80) ? (char)src[n] : '?';
    dst[n] = 0;
}

// "<server>:<episode gob>:<map jkl>", as JK.EXE names its DirectPlay games.
static void imperial_EncodeSessionName(uint16_t* out, size_t outLen, const jkMultiEntry* e)
{
    static const uint16_t colon[1] = { ':' };
    size_t pos = 0;
    size_t nSrv = imperial_StrLen16((const uint16_t*)e->serverName);
    out[0] = 0;
    imperial_Append16(out, outLen, &pos, (const uint16_t*)e->serverName, nSrv < 32 ? nSrv : 31);
    imperial_Append16(out, outLen, &pos, colon, 1);
    imperial_AppendAscii(out, outLen, &pos, e->episodeGobName, sizeof(e->episodeGobName));
    imperial_Append16(out, outLen, &pos, colon, 1);
    imperial_AppendAscii(out, outLen, &pos, e->mapJklFname, sizeof(e->mapJklFname));
}

// From the right, so a ':' in the server name doesn't break it.
static void imperial_DecodeSessionName(const uint16_t* name, jkMultiEntry* e)
{
    size_t len = imperial_StrLen16(name);
    size_t cMap = (size_t)-1, cEp = (size_t)-1;

    for (size_t i = len; i-- > 0; )
    {
        if (name[i] != ':') continue;
        if (cMap == (size_t)-1) cMap = i;
        else { cEp = i; break; }
    }

    memset(e->serverName, 0, sizeof(e->serverName));
    memset(e->episodeGobName, 0, sizeof(e->episodeGobName));
    memset(e->mapJklFname, 0, sizeof(e->mapJklFname));

    if (cMap == (size_t)-1 || cEp == (size_t)-1)
    {
        size_t n = len < 31 ? len : 31;
        memcpy(e->serverName, name, n * sizeof(uint16_t));
        return;
    }

    size_t nSrv = cEp < 31 ? cEp : 31;
    memcpy(e->serverName, name, nSrv * sizeof(uint16_t));
    imperial_CopyToAscii(e->episodeGobName, sizeof(e->episodeGobName), name + cEp + 1, cMap - cEp - 1);
    imperial_CopyToAscii(e->mapJklFname, sizeof(e->mapJklFname), name + cMap + 1, len - cMap - 1);
}

// dwUser1 checksumSeed, dwUser2 multiModeFlags, dwUser3 version | maxRank << 16,
// dwUser4 tickRateMs, as JK.EXE's DirectPlay sessions.
static void imperial_EntryToSession(const jkMultiEntry* e, dps_session* s)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->guidInstance, &e->guidInstance, 16);

    s->flags = IMPERIAL_HOST_BASE_FLAGS | ((uint32_t)e->sessionFlags & IMPERIAL_SESSIONFLAG_MASK);
    if (e->wPassword[0])
    {
        s->flags |= DPS_SESSION_PASSWORDREQUIRED;
        size_t n = imperial_StrLen16((const uint16_t*)e->wPassword);
        if (n > 31) n = 31;
        memcpy(s->password, e->wPassword, n * sizeof(uint16_t));
    }
    else
    {
        s->flags &= ~(uint32_t)DPS_SESSION_PASSWORDREQUIRED;
    }

    s->maxPlayers = (uint32_t)e->maxPlayers;
    imperial_EncodeSessionName(s->name, DPS_NAME_MAX, e);

    s->user[0] = (uint32_t)e->checksumSeed;
    s->user[1] = (uint32_t)e->multiModeFlags;
    s->user[2] = ((uint32_t)e->field_E0 & 0xFFFF) | (((uint32_t)e->maxRank & 0xFFFF) << 16);
    s->user[3] = (uint32_t)e->tickRateMs;
}

static void imperial_SessionToEntry(const dps_session* s, jkMultiEntry* e)
{
    memset(e, 0, sizeof(*e));
    memcpy(&e->guidInstance, s->guidInstance, 16);
    e->maxPlayers     = (int32_t)s->maxPlayers;
    e->numPlayers     = (int32_t)s->curPlayers;
    imperial_DecodeSessionName(s->name, e);
    e->sessionFlags   = (int32_t)s->flags;
    e->checksumSeed   = (int32_t)s->user[0];
    e->multiModeFlags = (int32_t)s->user[1];
    e->field_E0       = (int32_t)(s->user[2] & 0xFFFF);
    e->maxRank        = (int32_t)(s->user[2] >> 16);
    e->tickRateMs     = (int32_t)s->user[3];
}

// ---------------------------------------------------------------------------
// Map downloads (maps.js) on this connection, as on the desktop
// (stdComm_dplay.c): its frames go between machines as the relay client's big
// frames. maps.js knows players as the lobby numbers them, the host 1 and
// joiners 2..64, so a host gives each joiner's machine a number. maps.js puts
// what it sends in a queue (Module.jkImperial.mapSend), which the engine
// sends from (imperial_XferPump), at most IMPERIAL_XFER_RATE bytes a second.
// ---------------------------------------------------------------------------
#define IMPERIAL_XFER_RATE  (1024 * 1024)
#define IMPERIAL_XFER_BURST (64 * 1024)
#define IMPERIAL_XFER_MAX   (64 * 1024 + 64)

static uint32_t imperial_aXferSys[65];   // [2..64]: the machine a number stands for
static int imperial_nextXferAlias = 2;
static double imperial_xferTokens = IMPERIAL_XFER_BURST;
static uint32_t imperial_xferTokensMs = 0;
static uint8_t imperial_aXferBuf[IMPERIAL_XFER_MAX];

EM_JS(void, imperial_js_xfer_init, (int bActive), {
    if (!Module.jkImperial) {
        Module.jkImperial = { out: [] };
        Module.jkImperial.mapSend = function(id, bytes, via) {
            if (!Module.jkImperial.active) return null;
            Module.jkImperial.out.push({ id: id, b: new Uint8Array(bytes.slice(0)) });
            return 'direct';
        };
        Module.jkImperial.mapDirect = function(id) { return !!Module.jkImperial.active; };
    }
    Module.jkImperial.active = !!bActive;
    if (!bActive) Module.jkImperial.out = [];
});

// The next frame maps.js queued (its size, with *pTo), 0 if it doesn't fit, -1 if none.
EM_JS(int, imperial_js_xfer_take, (uint8_t* buf, int cap, int* pTo), {
    var q = Module.jkImperial && Module.jkImperial.out;
    if (!q || !q.length) return -1;
    var f = q.shift();
    if (f.b.length > cap) return 0;
    HEAPU8.set(f.b, buf);
    HEAP32[pTo >> 2] = f.id;
    return f.b.length;
});

EM_JS(void, imperial_js_xfer_frame, (int from, const uint8_t* p, int n), {
    if (Module.jkMaps) Module.jkMaps.onFrame(from, HEAPU8.slice(p, p + n).buffer, 'direct');
});

EM_JS(void, imperial_js_map_fetch, (const char* pName, const char* pExt), {
    if (Module.jkMaps) Module.jkMaps.fetch(UTF8ToString(pName), UTF8ToString(pExt));
});

EM_JS(int, imperial_js_map_state, (), {
    return Module.jkMaps ? Module.jkMaps.state() : 1;
});

EM_JS(void, imperial_js_map_abort, (const char* pReason), {
    if (Module.jkMaps) Module.jkMaps.abort(UTF8ToString(pReason));
});

EM_JS(void, imperial_js_map_host, (const char* pName, const char* pExt), {
    if (Module.jkMaps) Module.jkMaps.host(UTF8ToString(pName), UTF8ToString(pExt));
});

EM_JS(void, imperial_js_map_unhost, (), {
    if (Module.jkMaps) Module.jkMaps.unhost();
});

// mods.js: the same states as maps.js, and IMPERIAL_MOD_RESTART: the engine
// has to restart into the host's mod (or out of ours).
#define IMPERIAL_MOD_RESTART (3)

EM_JS(void, imperial_js_mod_tail, (char* pOut, int maxLen), {
    stringToUTF8(Module.jkMods ? Module.jkMods.tail() : "", pOut, maxLen);
});

EM_JS(void, imperial_js_mod_fetch, (const char* pTail), {
    if (Module.jkMods) Module.jkMods.fetch(UTF8ToString(pTail));
});

EM_JS(int, imperial_js_mod_state, (), {
    return Module.jkMods ? Module.jkMods.state() : 1;
});

EM_JS(void, imperial_js_mod_abort, (const char* pReason), {
    if (Module.jkMods) Module.jkMods.abort(UTF8ToString(pReason));
});

EM_JS(void, imperial_js_mod_restart, (char* pOut, int maxLen), {
    stringToUTF8(Module.jkMods ? Module.jkMods.restart() : "", pOut, maxLen);
});

EM_JS(void, imperial_js_mod_host, (), {
    if (Module.jkMods) Module.jkMods.host();
});

EM_JS(void, imperial_js_mod_unhost, (), {
    if (Module.jkMods) Module.jkMods.unhost();
});

EM_JS(void, imperial_js_mod_describe, (const char* pTail, char* pOut, int maxLen), {
    stringToUTF8(Module.jkMods ? Module.jkMods.describe(UTF8ToString(pTail)) : "", pOut, maxLen);
});

// The game to select again after restarting into its host's mod (kept by the page
// across the engine's restart), as 32 hex digits, "" for none.
EM_JS(void, imperial_js_pending, (const char* pSet, char* pOut, int maxLen), {
    if (!Module.jkImperial) Module.jkImperial = { out: [] };
    if (pSet) Module.jkImperial.pendingGuid = UTF8ToString(pSet);
    if (pOut) stringToUTF8(Module.jkImperial.pendingGuid || "", pOut, maxLen);
});

static uint32_t imperial_XferAliasOf(uint32_t sysId)
{
    for (int i = 2; i <= 64; i++)
        if (imperial_aXferSys[i] == sysId) return (uint32_t)i;
    int a = imperial_nextXferAlias;
    imperial_nextXferAlias = a >= 64 ? 2 : a + 1;
    imperial_aXferSys[a] = sysId;
    return (uint32_t)a;
}

static void imperial_BigFrame(uint32_t from, const uint8_t* p, size_t n)
{
    if (n < 16 || memcmp(p + 8, "JKMT", 4))
        return;
    if (jkrelay_IsHost())
        imperial_js_xfer_frame((int)imperial_XferAliasOf(from), p, (int)n);
    else if (from == jkrelay_HostSystemId())
        imperial_js_xfer_frame(1, p, (int)n);
}

// Sends what maps.js queued, as fast as IMPERIAL_XFER_RATE allows.
static void imperial_XferPump()
{
    uint32_t now = stdPlatform_GetTimeMsec();
    imperial_xferTokens += (double)(now - imperial_xferTokensMs) * (IMPERIAL_XFER_RATE / 1000.0);
    if (imperial_xferTokens > IMPERIAL_XFER_BURST)
        imperial_xferTokens = IMPERIAL_XFER_BURST;
    imperial_xferTokensMs = now;

    while (imperial_xferTokens > 0)
    {
        int to = 0;
        int n = imperial_js_xfer_take(imperial_aXferBuf, sizeof(imperial_aXferBuf), &to);
        if (n < 0)
            break;
        if (n == 0)
            continue;
        uint32_t dst = jkrelay_IsHost() ? (to >= 2 && to <= 64 ? imperial_aXferSys[to] : 0) : (to == 1 ? jkrelay_HostSystemId() : 0);
        if (!dst)
            continue;
        imperial_xferTokens -= n;
        jkrelay_SendBig(jkrelay_LocalSystemId(), dst, imperial_aXferBuf, (size_t)n);
    }
}

// Gets this game onto the host's mod (mods.js), from its tail: 1 ready (or it says
// nothing), IMPERIAL_MOD_RESTART, or 2 failed (mods.js has said why).
static int imperial_GetHostMod(const char* pTail)
{
    if (!pTail || !pTail[0])
        return 1;
    imperial_js_mod_fetch(pTail);

    int state;
    while ((state = imperial_js_mod_state()) == 0) // BUSY
    {
        imperial_XferPump();
        jkrelay_Idle();
        if (!jkrelay_HostSystemId())
            imperial_js_mod_abort("host-left");
        else
            emscripten_sleep(10);
    }
    return state;
}

// The host's map, before the level loads (maps.js shows how it goes). 1 when it's in place.
static int imperial_GetHostMap(const char* pEpisode)
{
    char name[32];
    stdString_SafeStrCopy(name, pEpisode, sizeof(name));
    imperial_js_map_fetch(name, JKRES_GOB_EXT);

    int state;
    while ((state = imperial_js_map_state()) == 0) // maps.js STATE_BUSY
    {
        imperial_XferPump();
        jkrelay_Idle();
        if (!jkrelay_HostSystemId())
            imperial_js_map_abort("host-left");
        else
            emscripten_sleep(10);
    }
    return state == 1; // STATE_READY
}

void stdComm_Imperial_Startup()
{
    jk_snwprintf(jkGuiMultiplayer_aConnections[STDCOMM_IMPERIAL_CONNECTION].name, 0x80, u"Imperial Network Protocol");
    if (jkGuiMultiplayer_numConnections <= STDCOMM_IMPERIAL_CONNECTION)
        jkGuiMultiplayer_numConnections = STDCOMM_IMPERIAL_CONNECTION + 1;
}

// The game doesn't receive while a menu is up (the 30 s stats screen), and
// the server drops players it doesn't hear from for 20 s.
void stdComm_Imperial_Idle()
{
    if (imperial_bStarted)
    {
        jkrelay_Idle();
        if (imperial_bInGame)
            imperial_XferPump();
    }
}

int stdComm_Imperial_OpenConnection()
{
    if (!imperial_bStarted)
    {
        if (jkrelay_Startup(imperial_AppGuid()) != 0)
        {
            stdPlatform_Printf("OpenJKDF2: Imperial Network Protocol: couldn't start.\n");
            return (int)0x887700FA; // DPERR_UNAVAILABLE
        }
        imperial_bStarted = 1;
    }
    imperial_js_server(imperial_server, sizeof(imperial_server));
    jkrelay_SetServer(imperial_server);
    jkrelay_SetBigHandler(imperial_BigFrame);
    imperial_js_xfer_init(1);
    stdPlatform_Printf("OpenJKDF2: Imperial Network Protocol server %s\n", imperial_server);
    stdComm_dword_8321DC = 1;
    return 0;
}

void stdComm_Imperial_CloseConnection()
{
    imperial_js_xfer_init(0);
    if (stdComm_dword_8321DC)
    {
        stdComm_Imperial_Close();
        stdComm_dword_8321DC = 0;
    }
}

static int imperial_SessionCallback(void* ctx, const dps_session* s)
{
    (void)ctx;
    if (imperial_numListed >= IMPERIAL_MAX_SESSIONS)
        return 0;
    imperial_SessionToEntry(s, &jkGuiMultiplayer_aEntries[imperial_numListed]);
    imperial_aListedWire[imperial_numListed] = (s->flags & IMPERIAL_SESSION_OPENJKDF2WIRE) != 0;
    imperial_aListedHwys[imperial_numListed] = (s->flags & IMPERIAL_SESSION_HWYS) != 0;
    imperial_aListedXfer[imperial_numListed] = (s->flags & IMPERIAL_SESSION_XFER) != 0;
    {
        const char* tail = jkrelay_GetSessionTail(s->guidInstance);
        stdString_SafeStrCopy(imperial_aListedTail[imperial_numListed], tail ? tail : "", sizeof(imperial_aListedTail[0]));
    }
    imperial_numListed++;
    return 1;
}

int stdComm_Imperial_EnumSessions()
{
    imperial_numListed = 0;
    memset(jkGuiMultiplayer_aEntries, 0, sizeof(jkMultiEntry) * IMPERIAL_MAX_SESSIONS);
    long hr = jkrelay_EnumSessions(imperial_SessionCallback, NULL);
    dplay_dword_55D618 = imperial_numListed;

    // Back from restarting into a game's mod: select it again.
    {
        char pending[40];
        imperial_js_pending(NULL, pending, sizeof(pending));
        for (int i = 0; pending[0] && i < imperial_numListed; i++)
        {
            char guid[40];
            const uint8_t* g = (const uint8_t*)&jkGuiMultiplayer_aEntries[i].guidInstance;
            for (int k = 0; k < 16; k++)
                snprintf(guid + 2 * k, 3, "%02x", g[k]);
            if (strcmp(guid, pending))
                continue;
            jkGui_guid_556040 = jkGuiMultiplayer_aEntries[i].guidInstance;
            imperial_js_pending("", NULL, 0);
        }
    }
    return (int)hr;
}

static void imperial_ReasonText(int reason, int minutes, char* pOut, int outLen)
{
    if (reason == 2 && minutes > 0)
        snprintf(pOut, outLen, "banned from this server for %d more minute%s", minutes, minutes == 1 ? "" : "s");
    else if (reason == 2)
        snprintf(pOut, outLen, "banned from this server");
    else if (reason == 1)
        snprintf(pOut, outLen, "kicked from this server");
    else
        pOut[0] = 0;
}

// Why a join failed, as the join screen's dialog says it (and the console).
static void imperial_JoinError(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(imperial_aJoinError, sizeof(imperial_aJoinError), fmt, ap);
    va_end(ap);
    stdPlatform_Printf("OpenJKDF2: Couldn't join: %s\n", imperial_aJoinError);
}

int stdComm_Imperial_Open(int idx, char16_t* pwPassword)
{
    jkrelay_StopEnumSessions();
    imperial_aJoinError[0] = 0;

    if (idx < 0 || idx >= imperial_numListed)
        return (int)0x80004005; // E_FAIL

    // Its messages would be JK.EXE's: a desktop build from before
    // IMPERIAL_SESSION_OPENJKDF2WIRE.
    if (!imperial_aListedWire[idx])
    {
        imperial_JoinError("The game's host runs an older OpenJKDF2, which browsers can't play with.");
        return (int)0x80004005;
    }

    // What's left of a game we were kicked from or lost: the relay client
    // turns a new join down while it thinks it's still in one.
    jkrelay_Close();
    stdComm_WSRelay_ResetHits(); // Classic until the host's hello says otherwise
    stdComm_WSRelay_ResetMotion();

    long hr = jkrelay_OpenJoin((const uint8_t*)&jkGuiMultiplayer_aEntries[idx].guidInstance, (const uint16_t*)pwPassword);
    if (hr < 0)
    {
        int minutes = 0;
        int reason = jkrelay_JoinRefusal(&minutes);
        char why[96];
        imperial_ReasonText(reason, minutes, why, sizeof(why));
        if (why[0])
            imperial_JoinError("You are %s.", why);
        else if (hr == (long)0x887700FA)
            imperial_JoinError("The server didn't answer.");
        else if (hr == (long)0x88770168)
            imperial_JoinError("That game isn't on the server any more.");
        else if (hr != (long)0x8877014A && hr != (long)0x88770154) // full, wrong password: the dialog says so
            imperial_JoinError("The server turned the join down (error %08x).", (unsigned)hr);
        return (int)hr;
    }

    // The host's mod first: on another one, the engine restarts before any map matters.
    {
        int modState = imperial_GetHostMod(imperial_aListedTail[idx]);
        if (modState == IMPERIAL_MOD_RESTART)
        {
            char path[sizeof(openjkdf2_aRestartPath)], guid[40];
            const uint8_t* g = (const uint8_t*)&jkGuiMultiplayer_aEntries[idx].guidInstance;
            imperial_js_mod_restart(path, sizeof(path));
            for (int i = 0; i < 16; i++)
                snprintf(guid + 2 * i, 3, "%02x", g[i]);
            imperial_js_pending(guid, NULL, 0);
            stdPlatform_Printf("OpenJKDF2: Restarting with mod path \"%s\" to join the game\n", path);
            jkrelay_Close();
            stdString_SafeStrCopy(openjkdf2_aRestartPath, path, sizeof(openjkdf2_aRestartPath));
            openjkdf2_restartMode = OPENJKDF2_RESTART_PATH;
            g_should_exit = 1;
            return STDCOMM_WSRELAY_RESTART;
        }
        if (modState != 1)
        {
            imperial_JoinError("Couldn't get onto the host's mod.");
            jkrelay_Close();
            return (int)0x80004005;
        }
    }

    // The host's map, before the level loads, from a host that serves it.
    if (imperial_aListedXfer[idx] && !imperial_GetHostMap(jkGuiMultiplayer_aEntries[idx].episodeGobName))
    {
        imperial_JoinError("Couldn't get the host's map %s.", jkGuiMultiplayer_aEntries[idx].episodeGobName);
        jkrelay_Close();
        return (int)0x80004005;
    }

    stdComm_dword_8321E8 = 0;
    stdComm_bIsServer = 0;

    // As JK.EXE: the player first, before DSS_JOINREQUEST.
    uint32_t id = 0;
    if (jkrelay_CreatePlayer((const uint16_t*)jkPlayer_playerShortName, &id) < 0 || !id)
    {
        imperial_JoinError("The server didn't take this player.");
        jkrelay_Close();
        return (int)0x80004005;
    }

    stdComm_dplayIdSelf = id;
    stdComm_dword_8321E0 = 1;
    imperial_bInGame = 1;
    jkGuiMultiplayer_checksumSeed = jkGuiMultiplayer_aEntries[idx].checksumSeed;
    return 0;
}

void stdComm_Imperial_Close()
{
    if (imperial_bInGame && jkrelay_IsHost())
    {
        imperial_js_map_unhost();
        imperial_js_mod_unhost();
    }
    if (stdComm_dword_8321E0)
    {
        if (stdComm_dplayIdSelf)
            jkrelay_DestroyPlayer(stdComm_dplayIdSelf);
        jkrelay_Close();
        stdComm_dword_8321E0 = 0;
        stdComm_bIsServer = 0;
        stdComm_dplayIdSelf = 0;
    }
    imperial_bInGame = 0;
}

int stdComm_Imperial_OpenHost(jkMultiEntry* pEntry)
{
    dps_session s;

    memset(&pEntry->guidInstance, 0, sizeof(pEntry->guidInstance)); // the server picks it
    imperial_EntryToSession(pEntry, &s);
    s.flags |= IMPERIAL_SESSION_OPENJKDF2WIRE;

    // The host screen's hit rules, which its hellos tell the other games.
    stdComm_WSRelay_ResetHits();
    stdComm_WSRelay_ResetMotion();
    stdComm_WSRelay_SetHitRules(stdComm_WSRelay_bClassicHits ? STDCOMM_WSRELAY_HITS_CLASSIC : STDCOMM_WSRELAY_HITS_HWYS,
                                stdComm_WSRelay_hitReachMs);
    if (!stdComm_WSRelay_bClassicHits)
        s.flags |= IMPERIAL_SESSION_HWYS;
    s.flags |= IMPERIAL_SESSION_XFER; // maps.js serves our map (below)

    {
        char tail[256];
        imperial_js_mod_tail(tail, sizeof(tail));
        jkrelay_SetHostTail(tail); // the game list says our mod
    }
    long hr = jkrelay_OpenCreate(&s);
    if (hr < 0)
    {
        stdPlatform_Printf("OpenJKDF2: Imperial Network Protocol: hosting failed (error %08x).\n", (unsigned)hr);
        return (int)hr;
    }
    imperial_bInGame = 1;
    memset(imperial_aXferSys, 0, sizeof(imperial_aXferSys));
    imperial_js_map_host(pEntry->episodeGobName, JKRES_GOB_EXT);
    imperial_js_mod_host();

    if (jkrelay_GetSessionDesc(&s) >= 0)
        memcpy(&pEntry->guidInstance, s.guidInstance, 16);
    return 0;
}

int stdComm_Imperial_GetSession_passwordidk(jkMultiEntry* pEntry)
{
    dps_session cur, s;

    if (jkrelay_GetSessionDesc(&cur) < 0)
        return 0;

    imperial_EntryToSession(pEntry, &s);
    memcpy(s.guidInstance, cur.guidInstance, 16);
    s.flags |= cur.flags & (IMPERIAL_SESSION_OPENJKDF2WIRE | IMPERIAL_SESSION_HWYS);
    return jkrelay_SetSessionDesc(&s) >= 0;
}

int stdComm_Imperial_IdkSessionDesc(jkMultiEntry* pEntry)
{
    dps_session s;

    if (jkrelay_GetSessionDesc(&s) < 0)
        return 0;
    imperial_SessionToEntry(&s, pEntry);
    return 1;
}

// The host's new level and player limit; the rest of the description stays.
void stdComm_Imperial_SetSessionDesc(const char* pJklFname, DWORD maxPlayers)
{
    dps_session s;
    jkMultiEntry e;

    if (jkrelay_GetSessionDesc(&s) < 0)
        return;

    imperial_SessionToEntry(&s, &e);
    memset(e.mapJklFname, 0, sizeof(e.mapJklFname));
    if (pJklFname)
        stdString_SafeStrCopy(e.mapJklFname, pJklFname, sizeof(e.mapJklFname));
    if (jkRes_episodeGobName[0] && __strcmpi(e.episodeGobName, jkRes_episodeGobName))
    {
        stdString_SafeStrCopy(e.episodeGobName, jkRes_episodeGobName, sizeof(e.episodeGobName));
        imperial_js_map_host(e.episodeGobName, JKRES_GOB_EXT); // joiners get this one now
    }

    imperial_EncodeSessionName(s.name, DPS_NAME_MAX, &e);
    s.flags &= ~IMPERIAL_SESSION_NEXTMAP;
    if (maxPlayers > 0 && maxPlayers <= JKPLAYER_NUM_INFOS)
        s.maxPlayers = maxPlayers;
    jkrelay_SetSessionDesc(&s);
}

// 1 when a level has loaded, 0 when it ends (the scores screen).
BOOL stdComm_Imperial_SetSessionFlagidk(int bJoinable)
{
    dps_session s;

    if (jkrelay_GetSessionDesc(&s) < 0)
        return 0;

    if (bJoinable)
        s.flags &= ~(uint32_t)DPS_SESSION_JOINDISABLED;
    else
        s.flags |= DPS_SESSION_JOINDISABLED;

    return jkrelay_SetSessionDesc(&s) >= 0;
}

static int imperial_PlayerCallback(void* ctx, uint32_t id, const uint16_t* shortName, const uint16_t* longName)
{
    DPNAME n;
    (void)ctx;

    memset(&n, 0, sizeof(n));
    n.dwSize = sizeof(n);
    n.lpszShortName = (LPWSTR)shortName;
    n.lpszLongName = (LPWSTR)longName;

    // JK.EXE's own callback fills DirectPlay_aPlayers.
    stdComm_EnumPlayersCallback((DPID)id, 1 /* DPPLAYERTYPE_PLAYER */, &n, 0, NULL);
    return DirectPlay_numPlayers < 0x20;
}

void stdComm_Imperial_EnumPlayers()
{
    DirectPlay_numPlayers = 0;
    jkrelay_EnumPlayers(imperial_PlayerCallback, NULL);
}

// Hit what you see: this player's own position updates to each player get a
// number, in the relay's data header (jkrelay_SetSendSeq), as the fast lane
// numbers them (stdComm_WSRelay.c); a hit claim names the update its game had
// on screen. As the desktop build's stdComm_dplay.c.
typedef struct ImperialSeqTo
{
    uint32_t id;
    uint32_t next;
    uint32_t aSeqs[256];
    double aAt[256];
} ImperialSeqTo;
static ImperialSeqTo imperial_aSeqTo[IMPERIAL_MAX_SESSIONS];
static uint32_t imperial_recvSeq = 0;

static int imperial_IsOwnPosition(const void* pData, uint32_t size)
{
    const uint8_t* p = (const uint8_t*)pData;
    if (size != 4 + 52 && size != 4 + 64)
        return 0;
    if (*(const uint16_t*)&p[0] != DSS_THINGPOS || *(const uint16_t*)&p[2] != 0)
        return 0;
    return sithPlayer_g_pLocalPlayerThing && *(const int32_t*)&p[4] == sithPlayer_g_pLocalPlayerThing->guid;
}

static ImperialSeqTo* imperial_SeqTo(uint32_t id, int bAdd)
{
    ImperialSeqTo* pFree = NULL;
    for (int i = 0; i < IMPERIAL_MAX_SESSIONS; i++)
    {
        if (imperial_aSeqTo[i].id == id)
            return &imperial_aSeqTo[i];
        if (!pFree && !imperial_aSeqTo[i].id)
            pFree = &imperial_aSeqTo[i];
    }
    if (!bAdd)
        return NULL;
    if (!pFree)
        pFree = &imperial_aSeqTo[id % IMPERIAL_MAX_SESSIONS];
    memset(pFree, 0, sizeof(*pFree));
    pFree->id = id;
    pFree->next = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    return pFree;
}

int stdComm_Imperial_InGame()
{
    return stdComm_Imperial_bActive && imperial_bInGame;
}

int stdComm_Imperial_RecvSeq(uint32_t* pSeq)
{
    *pSeq = imperial_recvSeq;
    return imperial_recvSeq != 0;
}

double stdComm_Imperial_SeqSentAt(int toId, uint32_t seq)
{
    ImperialSeqTo* t = imperial_SeqTo((uint32_t)toId, 0);
    if (!t || t->aSeqs[seq & 255] != seq || !t->aAt[seq & 255])
        return -1.0;
    return t->aAt[seq & 255];
}

BOOL stdComm_Imperial_Send(DPID idFrom, DPID idTo, void* lpData, DWORD dwDataSize)
{
    if (!imperial_bInGame)
        return 0;
    // A host's sends to everyone include itself (slot 0, stdComm_SendToPlayer).
    if (idTo && idTo == (DPID)stdComm_dplayIdSelf)
        return 1;
    if (idTo && imperial_IsOwnPosition(lpData, (uint32_t)dwDataSize))
    {
        ImperialSeqTo* t = imperial_SeqTo((uint32_t)idTo, 1);
        uint32_t seq = ++t->next;
        if (!seq)
            seq = ++t->next;
        t->aSeqs[seq & 255] = seq;
        t->aAt[seq & 255] = stdComm_WSRelay_Now();
        jkrelay_SetSendSeq(seq);
    }
    return jkrelay_Send((uint32_t)idFrom, (uint32_t)idTo, lpData, (uint32_t)dwDataSize) >= 0;
}

int stdComm_Imperial_Receive(int* pIdOut, int* pMsgIdOut, int* pLenOut)
{
    imperial_XferPump();

    for (;;)
    {
        uint32_t from = 0, size = 0;
        const void* data = NULL;
        dps_sysmsg sys;

        int r = jkrelay_Receive(&from, &data, &size, &sys);
        if (r == DPS_RECV_NONE)
            return -1;

        if (r == DPS_RECV_DATA)
        {
            uint32_t cap = (uint32_t)*pLenOut;
            if (size > cap)
            {
                stdPlatform_Printf("OpenJKDF2: dropping a %u-byte message (buffer %u)\n", size, cap);
                continue;
            }
            memcpy(pMsgIdOut, data, size);
            *pLenOut = (int)size;
            *pIdOut = (int)from;
            jkrelay_LastRecvSeq(&imperial_recvSeq);
            return 0;
        }

        switch (sys.kind)
        {
            case DPS_SYS_PLAYERCREATED:
                if (sys.playerId == (uint32_t)stdComm_dplayIdSelf)
                    continue;
                *pIdOut = (int)sys.playerId;
                return IMPERIAL_RET_NEWPLAYER;

            case DPS_SYS_PLAYERDESTROYED:
                if (sys.playerId == (uint32_t)stdComm_dplayIdSelf)
                    continue;
                *pIdOut = (int)sys.playerId;
                return IMPERIAL_RET_PLAYERLOST;

            case DPS_SYS_SESSIONLOST:
            {
                int minutes = 0;
                char why[96];
                imperial_ReasonText(jkrelay_LostReason(&minutes), minutes, why, sizeof(why));
                stdPlatform_Printf("OpenJKDF2: Leaving the game: %s.\n", why[0] ? why : "the server closed it");
                // As the host leaving (sithMulti_HandleNewPlayerLost: SERVER_LEFT_GAME).
                *pIdOut = (int)sithNet_serverNetId;
                return IMPERIAL_RET_PLAYERLOST;
            }

            default:
                continue;
        }
    }
}

// Following a dedicated server into another episode (jkMain_FollowHostMap): its copy,
// if we lack it or have another one and it serves it. 1 when it's in place (or
// there's nothing to get).
int stdComm_Imperial_FetchHostMap(const char* pEpisode)
{
    dps_session s;
    if (!stdComm_Imperial_InGame() || jkrelay_IsHost() || !pEpisode || !pEpisode[0])
        return 1;
    if (jkrelay_GetSessionDesc(&s) < 0 || !(s.flags & IMPERIAL_SESSION_XFER))
        return 1;
    return imperial_GetHostMap(pEpisode);
}

int stdComm_Imperial_JoinErrorText(char16_t* pOut, int maxChars)
{
    if (!stdComm_Imperial_bActive || !imperial_aJoinError[0] || maxChars <= 0)
        return 0;
    int i = 0;
    for (; imperial_aJoinError[i] && i < maxChars - 1; i++)
        pOut[i] = (char16_t)(uint8_t)imperial_aJoinError[i];
    pOut[i] = 0;
    return 1;
}

int stdComm_Imperial_GetSessionNextMap(char* pGob, int gobLen, char* pOut, int outLen)
{
    dps_session s;
    jkMultiEntry e;
    if (outLen <= 0 || gobLen <= 0) return 0;
    pOut[0] = 0;
    pGob[0] = 0;
    if (!stdComm_Imperial_bActive || !imperial_bInGame || jkrelay_GetSessionDesc(&s) < 0 || !(s.flags & IMPERIAL_SESSION_NEXTMAP))
        return 0;
    imperial_SessionToEntry(&s, &e);
    stdString_SafeStrCopy(pOut, e.mapJklFname, outLen);
    stdString_SafeStrCopy(pGob, e.episodeGobName, gobLen);
    return pOut[0] != 0;
}

void stdComm_Imperial_DescribeMod(int idx, char16_t* pOut, int maxChars)
{
    char text[128];
    text[0] = 0;
    if (idx >= 0 && idx < imperial_numListed && imperial_aListedTail[idx][0])
        imperial_js_mod_describe(imperial_aListedTail[idx], text, sizeof(text));
    int i = 0;
    for (; text[i] && i < maxChars - 1; i++)
        pOut[i] = (char16_t)(uint8_t)text[i];
    pOut[i] = 0;
}

void stdComm_Imperial_DescribeHits(int idx, int bShort, char16_t* pOut, int maxChars)
{
    const char* text = "";
    if (idx >= 0 && idx < imperial_numListed && imperial_aListedHwys[idx])
        text = bShort ? "Hit what you see" : "Hit rules: Hit what you see";
    else if (idx >= 0 && idx < imperial_numListed)
        text = bShort ? "Classic hits" : "Hit rules: Classic";

    int i = 0;
    for (; text[i] && i < maxChars - 1; i++)
        pOut[i] = (char16_t)(uint8_t)text[i];
    pOut[i] = 0;
}

// ---------------------------------------------------------------------------
// Console: a dedicated server's votes and rcon, as the desktop build's
// (its Main/jkServer.c, where the server's side is)
// ---------------------------------------------------------------------------

#define IMPERIAL_RCON_OUTPUT_MAX 16384
#define IMPERIAL_RCON_TIMEOUT_MS 15000 // a command can wait out the scores screen

static char imperial_aRconPassword[128];
static char imperial_aRconAddress[256];

static void imperial_Print(const char* fmt, ...)
{
    va_list ap;
    char aLine[512];
    va_start(ap, fmt);
    vsnprintf(aLine, sizeof(aLine), fmt, ap);
    va_end(ap);
    sithConsole_PrintString(aLine);
}

static void imperial_Trim(const char* pIn, char* pOut, size_t outLen)
{
    size_t n;
    while ( pIn && (*pIn == ' ' || *pIn == '\t') ) pIn++;
    stdString_SafeStrCopy(pOut, pIn ? pIn : "", outLen);
    n = strlen(pOut);
    while ( n && (pOut[n - 1] == ' ' || pOut[n - 1] == '\t') ) pOut[--n] = 0;
    if ( n >= 2 && pOut[0] == '"' && pOut[n - 1] == '"' )
    {
        memmove(pOut, pOut + 1, n - 2);
        pOut[n - 2] = 0;
    }
}

static void imperial_PrintLines(char* pText)
{
    for ( char* pLine = strtok(pText, "\n"); pLine; pLine = strtok(NULL, "\n") )
        sithConsole_PrintString(pLine);
}

// "callvote ..." / "vote ...": chat for the dedicated server alone, "!callvote ..."
// (its jkServer_HandleChat counts it).
static void imperial_SendVoteCommand(const char* pWhich, const char* pArgStr)
{
    char aArgs[128], aMsg[160];
    if ( !sithNet_isMulti || !stdComm_Imperial_bActive )
    {
        sithConsole_PrintString("Votes are for Imperial Network Protocol dedicated servers.");
        return;
    }
    if ( sithNet_isServer )
    {
        sithConsole_PrintString("Only a dedicated server counts votes.");
        return;
    }
    imperial_Trim(pArgStr, aArgs, sizeof(aArgs));
    snprintf(aMsg, sizeof(aMsg), "!%s %s", pWhich, aArgs);
    sithMulti_SendChatTo(aMsg, -1, -1, sithNet_serverNetId);
}

static int imperial_CmdCallVote(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aArgs[128];
    imperial_Trim(pArgStr, aArgs, sizeof(aArgs));
    if ( !aArgs[0] )
    {
        sithConsole_PrintString("callvote map <level> | kick <name or slot> | rank <0-8> | scorelimit <points> | timelimit <minutes>");
        sithConsole_PrintString("servermaps [text] lists the server's levels; vote yes / vote no votes.");
        return 1;
    }
    imperial_SendVoteCommand("callvote", aArgs);
    return 1;
}

static int imperial_CmdVote(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    imperial_SendVoteCommand("vote", pArgStr);
    return 1;
}

// Where rcon goes: rcon_address, else the server of the game we're in or were last in.
static void imperial_RconServer(char* pOut, size_t outLen)
{
    stdString_SafeStrCopy(pOut, imperial_aRconAddress[0] ? imperial_aRconAddress : imperial_server, outLen);
}

static int imperial_CmdRconPassword(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aArg[128];
    imperial_Trim(pArgStr, aArg, sizeof(aArg));
    if ( !aArg[0] )
    {
        sithConsole_PrintString(imperial_aRconPassword[0] ? "rcon password set (rcon_password <password> to change it)."
                                                          : "rcon_password <password>");
        return 1;
    }
    stdString_SafeStrCopy(imperial_aRconPassword, aArg, sizeof(imperial_aRconPassword));
    sithConsole_PrintString("rcon password set (until the page closes).");
    return 1;
}

static int imperial_CmdRconAddress(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    char aArg[256];
    imperial_Trim(pArgStr, aArg, sizeof(aArg));
    if ( aArg[0] )
        stdString_SafeStrCopy(imperial_aRconAddress, aArg, sizeof(imperial_aRconAddress));
    if ( imperial_aRconAddress[0] )
        imperial_Print("rcon goes to %s.", imperial_aRconAddress);
    else
        imperial_Print(imperial_server[0] ? "rcon goes to %s (the game you're in or were last in)." : "rcon_address <server>", imperial_server);
    return 1;
}

static int imperial_CmdRcon(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    static char aOutput[IMPERIAL_RCON_OUTPUT_MAX];
    char aCmd[512], aServer[256];

    imperial_Trim(pArgStr, aCmd, sizeof(aCmd));
    if ( !aCmd[0] )
    {
        sithConsole_PrintString("rcon <command>   (rcon_password first; rcon_address picks the server)");
        return 1;
    }
    if ( !imperial_aRconPassword[0] )
    {
        sithConsole_PrintString("Set the server's rcon password first: rcon_password <password>");
        return 1;
    }
    imperial_RconServer(aServer, sizeof(aServer));
    if ( !aServer[0] )
    {
        sithConsole_PrintString("Which server? rcon_address <server>");
        return 1;
    }
    if ( !jkrelay_Rcon(aServer, imperial_aRconPassword, aCmd, aOutput, sizeof(aOutput), IMPERIAL_RCON_TIMEOUT_MS) )
    {
        imperial_Print("No answer from %s (rcon reaches dedicated servers running their own server, with an rconPassword and a wsPort).", aServer);
        return 1;
    }
    imperial_PrintLines(aOutput);
    return 1;
}

// "servermaps [text]": the server's levels ("maplist", which its rcon answers
// without the password).
static int imperial_CmdServerMaps(stdDebugConsoleCmd* pCmd, const char* pArgStr)
{
    static char aOutput[IMPERIAL_RCON_OUTPUT_MAX];
    char aArgs[64], aCmd[96], aServer[256];
    imperial_Trim(pArgStr, aArgs, sizeof(aArgs));
    snprintf(aCmd, sizeof(aCmd), aArgs[0] ? "maplist %s" : "maplist", aArgs);
    imperial_RconServer(aServer, sizeof(aServer));
    if ( !aServer[0] || !jkrelay_Rcon(aServer, "", aCmd, aOutput, sizeof(aOutput), IMPERIAL_RCON_TIMEOUT_MS) )
    {
        sithConsole_PrintString("The server didn't answer (dedicated servers running their own server list their levels).");
        return 1;
    }
    imperial_PrintLines(aOutput);
    return 1;
}

void stdComm_Imperial_RegisterCommands()
{
    sithConsole_RegisterCommand(imperial_CmdCallVote, "callvote", 0);
    sithConsole_RegisterCommand(imperial_CmdVote, "vote", 0);
    sithConsole_RegisterCommand(imperial_CmdServerMaps, "servermaps", 0);
    sithConsole_RegisterCommand(imperial_CmdRcon, "rcon", 0);
    sithConsole_RegisterCommand(imperial_CmdRconPassword, "rcon_password", 0);
    sithConsole_RegisterCommand(imperial_CmdRconAddress, "rcon_address", 0);
}

#endif // PLATFORM_WSRELAY
