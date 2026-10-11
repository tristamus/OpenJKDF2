// WebSocket relay backend for the browser build.
//
// The browser can't open UDP sockets, so every player keeps one WebSocket to a
// relay (jkdf2mp-bb-server/relay) and the relay passes packets between them.
// The game side mirrors the GNS backend: a star with the host as id 1, clients
// get ids 2..64, and every packet carries an 8-byte idFrom/idTo header. The
// relay delivers each packet to the player named in idTo (the host for 0 or 1),
// which is where GNSServer::Receive would have forwarded it.
//
// Text frames are relay control lines, binary frames are game packets:
//   us -> relay   list | host <entry> | update <entry> | join <room>
//                 welcome <id> <entry> | leave
//   relay -> us   list (<room> <players> <entry>)* | hosted <room>
//                 peer-joined <id> | peer-left <id> | welcome <id> <entry>
//                 host-left | error <code>
// <entry> is a hex-encoded jkMultiEntry.

#include "stdComm_WSRelay.h"

#include "Win95/stdComm.h"
#include "Dss/sithMulti.h"
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

#define WSRELAY_STATE_CONNECTING (0)
#define WSRELAY_STATE_OPEN (1)
#define WSRELAY_STATE_CLOSED (2)

#define wsrelay_verbosePrintf(fmt, ...) if (Main_bVerboseNetworking) \
    { \
        stdPlatform_Printf(fmt, ##__VA_ARGS__);  \
    } \
    ;

extern int Main_bVerboseNetworking;

// The socket and its inbound queue live in JS. The engine polls the queue,
// so nothing calls back into the wasm module while Asyncify has it unwound.
EM_JS_DEPS(stdComm_WSRelay, "$UTF8ToString,$stringToUTF8,$lengthBytesUTF8");

EM_JS(void, wsrelay_js_connect, (), {
    var net = Module.jkNet = Module.jkNet || { state: 2, queue: [] };
    if (net.ws && net.state !== 2) return;
    if (net.ws) {
        try { net.ws.close(); } catch (e) {}
    }
    var url = Module['jkRelayUrl'];
    if (!url && typeof location !== 'undefined')
        url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/relay';
    net.queue = [];
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
    ws.onopen = function() { net.state = 1; console.log('OpenJKDF2: connected to relay ' + url); };
    ws.onclose = function() { if (net.ws === ws) net.state = 2; };
    ws.onerror = function() {
        console.error('OpenJKDF2: relay connection error (' + url + ')');
        // Browsers follow up with onclose, but don't wait on it to give up.
        if (net.ws === ws && net.state === 0) net.state = 2;
    };
    ws.onmessage = function(ev) { if (net.ws === ws) net.queue.push(ev.data); };
    net.ws = ws;
});

EM_JS(int, wsrelay_js_state, (), {
    var net = Module.jkNet;
    return net ? net.state : 2;
});

EM_JS(int, wsrelay_js_send_text, (const char* pText), {
    var net = Module.jkNet;
    if (!net || net.state !== 1) return 0;
    net.ws.send(UTF8ToString(pText));
    return 1;
});

EM_JS(int, wsrelay_js_send_binary, (const void* pData, int len), {
    var net = Module.jkNet;
    if (!net || net.state !== 1) return 0;
    net.ws.send(HEAPU8.slice(pData, pData + len));
    return 1;
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
static int wsrelay_bHostLeft = 0;
static int wsrelay_bLostReported = 0;
static int wsrelay_joinError = 0;
static int wsrelay_room = -1;
static uint32_t wsrelay_id = 0xFFFFFFFF;

static int wsrelay_aDisconnected[64];
static int wsrelay_numDisconnected = 0;

static jkMultiEntry wsrelay_storedEntry;      // the game we host
static jkMultiEntry wsrelay_storedEntryEnum;  // the game we joined

// The relay's latest game list, and the rooms behind the entries the join
// screen is showing right now (jkGuiMultiplayer_aEntries).
static int wsrelay_bListReceived = 0;
static int wsrelay_numListed = 0;
static int wsrelay_aListRooms[WSRELAY_MAX_LISTED];
static jkMultiEntry wsrelay_aListEntries[WSRELAY_MAX_LISTED];
static int wsrelay_numShown = 0;
static int wsrelay_aShownRooms[WSRELAY_MAX_LISTED];

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

// Decodes exactly len bytes; anything else is a malformed entry.
static int wsrelay_FromHex(const char* pHex, void* pOut, int len)
{
    if (!pHex || strlen(pHex) != (size_t)len*2)
        return 0;

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

static int wsrelay_SendText(const char* pText)
{
    wsrelay_verbosePrintf("Relay << %.80s\n", pText);
    return wsrelay_js_send_text(pText);
}

static void wsrelay_SendEntry(const char* pOp, int id, const jkMultiEntry* pEntry)
{
    char hex[sizeof(jkMultiEntry)*2 + 1];
    wsrelay_ToHex(pEntry, sizeof(jkMultiEntry), hex);
    if (id >= 0)
        snprintf(wsrelay_textBuf, sizeof(wsrelay_textBuf), "%s %d %s", pOp, id, hex);
    else
        snprintf(wsrelay_textBuf, sizeof(wsrelay_textBuf), "%s %s", pOp, hex);
    wsrelay_SendText(wsrelay_textBuf);
}

static void wsrelay_ResetSession()
{
    wsrelay_bHosting = 0;
    wsrelay_bHostPending = 0;
    wsrelay_bJoining = 0;
    wsrelay_bJoined = 0;
    wsrelay_bHostLeft = 0;
    wsrelay_bLostReported = 0;
    wsrelay_joinError = 0;
    wsrelay_room = -1;
    wsrelay_id = 0xFFFFFFFF;
    wsrelay_numDisconnected = 0;
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
        if (!wsrelay_FromHex(pHex, pEntry, sizeof(jkMultiEntry)))
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
            stdPlatform_Printf("OpenJKDF2: Hosting room %d on the relay\n", wsrelay_room);
        }
    }
    else if (!strcmp(pOp, "peer-joined"))
    {
        char* pId = strtok_r(NULL, " ", &pSave);
        if (wsrelay_bHosting && pId)
        {
            wsrelay_WelcomePeer(atoi(pId));
            wsrelay_UpdateLobby();
        }
    }
    else if (!strcmp(pOp, "peer-left"))
    {
        char* pId = strtok_r(NULL, " ", &pSave);
        if (wsrelay_bHosting && pId)
        {
            wsrelay_PeerLeft(atoi(pId));
            wsrelay_UpdateLobby();
        }
    }
    else if (!strcmp(pOp, "welcome"))
    {
        char* pId = strtok_r(NULL, " ", &pSave);
        char* pHex = strtok_r(NULL, " ", &pSave);
        jkMultiEntry entry;
        if (wsrelay_bJoining && pId && wsrelay_FromHex(pHex, &entry, sizeof(entry)))
        {
            // Same as GNSClient receiving its GNSInfoPacket.
            wsrelay_id = (uint32_t)atoi(pId);
            wsrelay_verbosePrintf("We are ID %x\n", wsrelay_id);

            wsrelay_storedEntryEnum = entry;
            wsrelay_storedEntryEnum.field_E0 = 10;

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
            stdPlatform_Printf("OpenJKDF2: Host has disconnected.\n");
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
}

void stdComm_WSRelay_Shutdown()
{
}

int DirectPlay_Receive(int *pIdOut, int *pMsgIdOut, int *pLenOut)
{
    int maxLen = *pLenOut;
    *pIdOut = 0;
    *pLenOut = 0;

    Hack_ResetClients();

    while (1)
    {
        if (stdComm_bIsServer && wsrelay_numDisconnected)
        {
            *pIdOut = wsrelay_aDisconnected[0];
            wsrelay_numDisconnected--;
            memmove(&wsrelay_aDisconnected[0], &wsrelay_aDisconnected[1], wsrelay_numDisconnected * sizeof(int));
            return 2;
        }

        if (!stdComm_bIsServer && wsrelay_bJoined && wsrelay_bHostLeft)
        {
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

    // Lost the relay mid-game. For a client that's the same as losing the host.
    if ((wsrelay_bJoined || wsrelay_bHosting) && !wsrelay_bLostReported && wsrelay_js_state() == WSRELAY_STATE_CLOSED)
    {
        wsrelay_bLostReported = 1;
        stdPlatform_Printf("OpenJKDF2: Lost contact with the relay.\n");
        if (!stdComm_bIsServer)
        {
            wsrelay_bJoined = 0;
            *pIdOut = 1;
            return 2;
        }
    }

    return -1;
}

BOOL DirectPlay_Send(DPID idFrom, DPID idTo, void *lpData, DWORD dwDataSize)
{
    Hack_ResetClients();

    if (!wsrelay_bHosting && !wsrelay_bJoined)
        return stdComm_bIsServer ? 1 : 0;

    if (dwDataSize > WSRELAY_MAX_PACKET-8) dwDataSize = WSRELAY_MAX_PACKET-8;
    *(uint32_t*)&wsrelay_sendBuf[0] = idFrom;
    *(uint32_t*)&wsrelay_sendBuf[4] = idTo;
    memcpy(&wsrelay_sendBuf[8], lpData, dwDataSize);

    wsrelay_verbosePrintf("Sent %x bytes to %x (%s, %x)\n", dwDataSize+8, idTo, sithDSS_IdToStr(*(uint32_t*)lpData & 0xFF), *(uint32_t*)lpData);

    int ret = wsrelay_js_send_binary(wsrelay_sendBuf, dwDataSize+8);

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
    wsrelay_storedEntry.multiModeFlags = sithMulti_multiModeFlags;
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
        wsrelay_bListReceived = 0;
        wsrelay_SendText("list");
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
