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
// Not here yet: map and mod downloads (the joiner needs the host's), the hit
// rules a host picks (Classic: what the desktop plays).

#include "stdComm_Imperial.h"

#ifdef PLATFORM_WSRELAY

#include "Win95/stdComm.h"
#include "Win95/WinIdk.h"
#include "Dss/sithMulti.h"
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
int jkrelay_Rcon(const char* server, const char* password, const char* cmd, char* out, size_t cap, int timeoutMs);

// As the desktop's stdComm_dplay.c.
#define IMPERIAL_MAX_SESSIONS (32)
#define IMPERIAL_RET_PLAYERLOST (2)
#define IMPERIAL_RET_NEWPLAYER (DPSYS_DELETEPLAYER)
#define IMPERIAL_HOST_BASE_FLAGS (DPS_SESSION_KEEPALIVE | DPS_SESSION_NODATAMESSAGES)
#define IMPERIAL_SESSIONFLAG_MASK (~(uint32_t)SESSIONFLAG_ISDEDICATED)
#define IMPERIAL_SESSION_NEXTMAP 0x40000000u        // a dedicated host named the next level
#define IMPERIAL_SESSION_OPENJKDF2WIRE 0x20000000u  // the game's messages use OpenJKDF2's layouts

static const uint8_t imperial_kJkAppGuid[16] = {
    0xC0, 0x13, 0x06, 0xBF, 0x79, 0xDE, 0xD0, 0x11, 0x99, 0xC9, 0x00, 0xA0, 0x24, 0x76, 0xAD, 0x4B
};

int stdComm_Imperial_bActive = 0;
static int imperial_bStarted = 0;
static int imperial_bInGame = 0;
static int imperial_numListed = 0;
static int imperial_aListedWire[IMPERIAL_MAX_SESSIONS];
static char imperial_server[256];

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
        jkrelay_Idle();
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
    stdPlatform_Printf("OpenJKDF2: Imperial Network Protocol server %s\n", imperial_server);
    stdComm_dword_8321DC = 1;
    return 0;
}

void stdComm_Imperial_CloseConnection()
{
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
    imperial_numListed++;
    return 1;
}

int stdComm_Imperial_EnumSessions()
{
    imperial_numListed = 0;
    memset(jkGuiMultiplayer_aEntries, 0, sizeof(jkMultiEntry) * IMPERIAL_MAX_SESSIONS);
    long hr = jkrelay_EnumSessions(imperial_SessionCallback, NULL);
    dplay_dword_55D618 = imperial_numListed;
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

int stdComm_Imperial_Open(int idx, char16_t* pwPassword)
{
    jkrelay_StopEnumSessions();

    if (idx < 0 || idx >= imperial_numListed)
        return (int)0x80004005; // E_FAIL

    // Its messages would be JK.EXE's: a desktop build from before
    // IMPERIAL_SESSION_OPENJKDF2WIRE.
    if (!imperial_aListedWire[idx])
    {
        stdPlatform_Printf("OpenJKDF2: This game's host runs an older OpenJKDF2, whose messages this one can't read.\n");
        return (int)0x887700FA; // GUINET_NOTAVAIL
    }

    long hr = jkrelay_OpenJoin((const uint8_t*)&jkGuiMultiplayer_aEntries[idx].guidInstance, (const uint16_t*)pwPassword);
    if (hr < 0)
    {
        int minutes = 0;
        char why[96];
        imperial_ReasonText(jkrelay_JoinRefusal(&minutes), minutes, why, sizeof(why));
        stdPlatform_Printf("OpenJKDF2: Couldn't join (error %08x)%s%s.\n", (unsigned)hr, why[0] ? ": " : "", why);
        return (int)hr;
    }

    stdComm_dword_8321E8 = 0;
    stdComm_bIsServer = 0;

    // As JK.EXE: the player first, before DSS_JOINREQUEST.
    uint32_t id = 0;
    if (jkrelay_CreatePlayer((const uint16_t*)jkPlayer_playerShortName, &id) < 0 || !id)
    {
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

    long hr = jkrelay_OpenCreate(&s);
    if (hr < 0)
    {
        stdPlatform_Printf("OpenJKDF2: Imperial Network Protocol: hosting failed (error %08x).\n", (unsigned)hr);
        return (int)hr;
    }
    imperial_bInGame = 1;

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
    s.flags |= cur.flags & IMPERIAL_SESSION_OPENJKDF2WIRE;
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
    if (jkRes_episodeGobName[0])
        stdString_SafeStrCopy(e.episodeGobName, jkRes_episodeGobName, sizeof(e.episodeGobName));

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

BOOL stdComm_Imperial_Send(DPID idFrom, DPID idTo, void* lpData, DWORD dwDataSize)
{
    if (!imperial_bInGame)
        return 0;
    // A host's sends to everyone include itself (slot 0, stdComm_SendToPlayer).
    if (idTo && idTo == (DPID)stdComm_dplayIdSelf)
        return 1;
    return jkrelay_Send((uint32_t)idFrom, (uint32_t)idTo, lpData, (uint32_t)dwDataSize) >= 0;
}

int stdComm_Imperial_Receive(int* pIdOut, int* pMsgIdOut, int* pLenOut)
{
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

void stdComm_Imperial_DescribeHits(int idx, int bShort, char16_t* pOut, int maxChars)
{
    const char* text = "";
    if (idx >= 0 && idx < imperial_numListed)
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
