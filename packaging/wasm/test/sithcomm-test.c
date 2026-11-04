// Tests for the reliable-message resend buffer (src/Devices/sithComm.c): acks
// for every message id, the count of slots in use, resends and when they
// stop, a player index that changes hands, the one slot all reliable messages
// share, and how a receiver acks and drops repeats. Made-up games run the
// tree's own sithComm, stdComm, sithPlayer and sithMulti code over a made-up
// network.
// sithcomm-test.sh cuts that code out of the sources and builds this file
// around it. From the top of the source tree:
//
//   sh packaging/wasm/test/sithcomm-test.sh

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>

// The message structs, message ids and packet readers, from src/.
#include "sithcomm-types.inc"

typedef int16_t __int16;
typedef int (*cogMsg_Handler)(SithMessage*);

#define _memset memset
#define _memcpy memcpy
#define SITH_ALLOC malloc
#define SITH_FREE free
#define CHECK_ALIGN(t) (1)
#define NETMSG_IN_START(x) intptr_t _readingPacket = (intptr_t)&x->pktData[0]; \
    uint16_t _readingOutU16; int16_t _readingOutS16; int32_t _readingOutS32;
#define _strncpy strncpy

// What the code reads of a player's thing. The test's players have none.
typedef struct SithThing
{
    uint32_t flags;
    int idx;
    struct SithThing* pTemplate;
    struct { void* model3; } renderData;
} SithThing;
#define SITH_TF_DISABLED 0x80000

// What the code reads of a player's record.
typedef struct SithPlayer
{
    char16_t player_name[32];
    uint32_t flags;
    uint32_t playerNetId;
    SithThing* pLocalPlayer;
    uint32_t msecLastCommTime;
    int numKills;
    int numKilled;
    int numSuicides;
    int teamNum;
    int score;
} SithPlayer;

// One game's copy of the engine globals that the code uses.
typedef struct Game
{
    SithMessage msgTmpBuf[32];
    SithMessage* heapBuf;
    sithCogMsg_Pair msgPairs[128];
    uint32_t msgPairsNext;
    int msgId;
    int idk2;
    SithMessage netMsgTmp;
    SithMessage ackTmp;
    int netId;
    int thingIdx;
    SithPlayer players[32];
    uint32_t maxPlayers;
    int outputstream;
    int inputstream;
    int stopProcessing;
    int submodeFlags;
    int isServer;
    cogMsg_Handler typeFuncs[DSS_MAX];
    uint32_t packetsSent;
    uint32_t bytesSent;
    int running;
} Game;

#define NGAMES 4

static Game games[NGAMES];
static Game* G = &games[0];
static int cur = 0;
static uint32_t sithTime_g_msecGameTime;
static int sithComm_msgInProgress;

#ifndef SITHCOMM_HEAP_MSGBUF
#define sithComm_MsgTmpBuf (G->msgTmpBuf)
#endif
#define sithComm_aMsgPairs (G->msgPairs)
#define sithComm_dword_847E84 (G->msgPairsNext)
#define sithComm_msgId (G->msgId)
#define sithComm_idk2 (G->idk2)
#define sithComm_netMsgTmp (G->netMsgTmp)
#define sithComm_MsgTmpBuf2 (G->ackTmp)
#define stdComm_dplayIdSelf (G->netId)
#define playerThingIdx (G->thingIdx)
#define jkPlayer_playerInfos (G->players)
#define jkPlayer_maxPlayers (G->maxPlayers)
#define sithMessage_g_outputstream (G->outputstream)
#define sithMessage_g_inputstream (G->inputstream)
#define sithMessage_bStopProcessMessages (G->stopProcessing)
#define g_submodeFlags (G->submodeFlags)
#define sithNet_isServer (G->isServer)
#define sithMessage_aTypeFuncs (G->typeFuncs)
#define stdComm_dword_8321F4 (G->packetsSent)
#define stdComm_dword_8321F0 (G->bytesSent)

int sithComm_SendMsgToPlayer(SithMessage *pMessage, int idTo, int outstream, int dwDPFlags);
int sithMessage_ProcessMessages();
void sithComm_ForgetSender(int idx);
void sithComm_ForgetRecipient(int playerIdx);
void sithComm_SyncWithPlayers();
void sithComm_ClearMsgTmpBuf();
int sithComm_cogMsg_Reset(SithMessage *msg);
int stdComm_Recv(SithMessage *msg);
int stdComm_SendToPlayer(SithMessage *msg, int sendto_id);
uint32_t sithPlayer_GetPlayerNum(int playerId);
void sithPlayer_Startup(int idx);
int sithMulti_ProcessSyncPlayers(SithMessage *pMsg);

static void sithMessage_FileWrite(SithMessage* pMessage) { (void)pMessage; }
static void sithMulti_QuitPlayer(int id) { (void)id; }
static void sithMulti_ProcessPlayerLost(int idPlayer) { (void)idPlayer; }
static void sithMulti_SyncPlayers(int idTo, int dpFlags) { (void)idTo; (void)dpFlags; }

// What sithPlayer_Startup and sithMulti_ProcessSyncPlayers use besides
// sithComm: the world, the console, sounds, cogs and the score table.
static void* sithWorld_g_pCurrentWorld;
static SithThing* sithPlayer_g_pLocalPlayerThing;
static int sithNet_MultiModeFlags;
static int sithNet_serverNetId;
static uint32_t sithNet_dword_8C4BA8;
static int16_t sithNet_teamScore[5];
static int sithMulti_quitGameState;
static uint32_t sithMulti_msecQuitGameTime;
static void (*sithMulti_pfNewPlayerJoinedCallback)(void);
#define MULTIMODEFLAG_TEAMS 0x1
#define MULTI_LEAVEJOIN_DELAY_MS 1000
#define SITH_MESSAGE_JOIN 37
#define SITH_MESSAGE_LEAVE 38
static uint32_t stdPlatform_GetTimeMsec(void) { return sithTime_g_msecGameTime; }
static void sithThing_SetThingModel(SithThing* pThing, void* pModel) { (void)pThing; (void)pModel; }
static char16_t* sithStrTable_GetUniStringWithFallback(const char* pKey) { static char16_t none[1]; (void)pKey; return none; }
static int jk_snwprintf(char16_t* pOut, size_t n, const char16_t* pFmt, ...) { (void)pFmt; if (n) pOut[0] = 0; return 0; }
static void sithConsole_PrintWString(const char16_t* pStr) { (void)pStr; }
static void sithConsole_AlertSound(void) {}
static void sithSoundClass_StopSound(SithThing* pThing, const void* pSound) { (void)pThing; (void)pSound; }
static void sithCog_BroadcastMessage(int msg, int senderType, int senderIdx, int sourceType, int sourceIdx)
{
    (void)msg; (void)senderType; (void)senderIdx; (void)sourceType; (void)sourceIdx;
}
static void sithDSSThing_UpdateState(SithThing* pThing, int sendto, int mpFlags) { (void)pThing; (void)sendto; (void)mpFlags; }
static void stdString_CharToWchar(char16_t* pOut, const char* pIn, int n)
{
    int i = 0;
    for (; i < n && pIn[i]; i++)
        pOut[i] = (uint8_t)pIn[i];
    pOut[i] = 0;
}

#ifdef PLATFORM_WSRELAY
// Hit what you see holds only swing, animation and sound messages.
#define STDCOMM_WSRELAY_HITS_MSG (0x434A)
static int stdComm_WSRelay_HoldForTimeline(SithMessage* pMsg) { (void)pMsg; return 0; }
static void stdComm_WSRelay_ProcessHits(SithMessage* pMsg) { (void)pMsg; }
#endif

// The network: every packet any game sends, in order.
typedef struct Packet
{
    int from;        // net ids
    int to;
    uint32_t sentAt;
    int lost;
    int received;
    uint32_t len;
    uint8_t data[2060];
} Packet;

#define MAXPACKETS 2048
#define DELAY 30
#define FRAME 16

static Packet packets[MAXPACKETS];
static int npackets;
static int linkDown[NGAMES][NGAMES];

static int gameOf(int netId)
{
    for (int g = 0; g < NGAMES; g++)
        if (games[g].running && games[g].netId == netId)
            return g;
    return -1;
}

static Packet* addPacket(int from, int to, const void* data, uint32_t len)
{
    if (npackets == MAXPACKETS || len > sizeof(packets[0].data))
    {
        printf("FAIL: too many packets\n");
        exit(1);
    }
    Packet* p = &packets[npackets++];
    memset(p, 0, sizeof(*p));
    p->from = from;
    p->to = to;
    p->sentAt = sithTime_g_msecGameTime;
    p->len = len;
    memcpy(p->data, data, len);
    int a = gameOf(from), b = gameOf(to);
    p->lost = a < 0 || b < 0 || linkDown[a][b];
    return p;
}

int DirectPlay_Send(int idFrom, int idTo, void* data, uint32_t len)
{
    if (idFrom != idTo) // the transports drop a game's packets to itself
        addPacket(idFrom, idTo, data, len);
    return 1;
}

int DirectPlay_Receive(int* pIdOut, int* pMsgIdOut, int* pLenOut)
{
    for (int i = 0; i < npackets; i++)
    {
        Packet* p = &packets[i];
        if (p->lost || p->received || p->to != G->netId || p->sentAt + DELAY > sithTime_g_msecGameTime)
            continue;
        if ((int)p->len > *pLenOut)
            break;
        p->received = 1;
        memcpy(pMsgIdOut, p->data, p->len);
        *pLenOut = p->len;
        *pIdOut = p->from;
        return 0;
    }
    return -1;
}

// The game's own code, which isn't free of these warnings.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "sithcomm-code.inc"
#pragma GCC diagnostic pop

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static void use(int g)
{
#ifdef SITHCOMM_HEAP_MSGBUF
    games[cur].heapBuf = sithComm_pMsgTmpBufHeap;
    sithComm_pMsgTmpBufHeap = games[g].heapBuf;
#endif
    cur = g;
    G = &games[g];
}

static SithMessage* slots(int g)
{
    use(g);
    return sithComm_MsgTmpBuf;
}

static int slotsInUse(int g)
{
    SithMessage* s = slots(g);
    int n = 0;
    for (int i = 0; s && i < 32; i++)
        n += s[i].netMsg.msgId != 0;
    return n;
}

// The test message: a state update numbered n. runs[g][from][n] counts the
// times game g ran number n from game `from`.
#define TESTMSG DSS_SYNCTHING
static int runs[NGAMES][NGAMES][64];

static int onTestMessage(SithMessage* m)
{
    int from = gameOf(m->netMsg.idx);
    uint32_t n = m->pktData[0];
    if (from >= 0 && n < 64)
        runs[cur][from][n]++;
    return 1;
}

// Frames on which some game's sithComm_idk2 was not its number of slots in
// use.
static int countWrong;

// Starts game g as net id netId at player index idx, among n players where
// index j has net id j + 1.
static void startGame(int g, int netId, int idx, int n)
{
    use(g);
    G->running = 1;
    G->netId = netId;
    G->thingIdx = idx;
    G->maxPlayers = n;
    for (int j = 0; j < n; j++)
    {
        G->players[j].flags = 5; // as sithPlayer_ShowPlayer leaves them
        G->players[j].playerNetId = j == idx ? netId : j + 1;
    }
    G->outputstream = 1;
    G->inputstream = 1;
    G->isServer = idx == 0;
    G->msgId = 1; // as sithComm_Startup leaves it
    G->typeFuncs[DSS_RESET] = sithComm_cogMsg_Reset;
    G->typeFuncs[TESTMSG] = onTestMessage;
    G->typeFuncs[DSS_LEAVEJOIN] = sithMulti_ProcessSyncPlayers;
}

// Games 0..n-1, game g as net id g + 1 at index g. Game 0 hosts.
static void setup(int n)
{
    use(cur);
    for (int g = 0; g < NGAMES; g++)
        free(games[g].heapBuf);
#ifdef SITHCOMM_HEAP_MSGBUF
    sithComm_pMsgTmpBufHeap = NULL;
#endif
    memset(games, 0, sizeof(games));
    memset(runs, 0, sizeof(runs));
    memset(linkDown, 0, sizeof(linkDown));
    npackets = 0;
    countWrong = 0;
    sithTime_g_msecGameTime = 5000;
    for (int g = 0; g < n; g++)
        startGame(g, g + 1, g, n);
    use(0);
}

// Fills game g's player table to 32 players. Index j has net id j + 1, and
// those without a game lose every packet.
static void fillTable(int g)
{
    use(g);
    for (int j = G->maxPlayers; j < 32; j++)
    {
        G->players[j].flags = 5;
        G->players[j].playerNetId = j + 1;
    }
    G->maxPlayers = 32;
}

static void runFor(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += FRAME)
    {
        sithTime_g_msecGameTime += FRAME;
        for (int g = 0; g < NGAMES; g++)
        {
            if (!games[g].running)
                continue;
            use(g);
            sithMessage_ProcessMessages();
            if (G->idk2 != slotsInUse(g))
                countWrong++;
        }
    }
}

// Sends test message n from game g, reliably, and returns its msgId.
static int sendTest(int g, int idTo, uint32_t n)
{
    static SithMessage m;
    memset(&m, 0, sizeof(m));
    m.netMsg.cogMsgId = TESTMSG;
    m.netMsg.msg_size = 4;
    m.pktData[0] = n;
    use(g);
    sithComm_SendMsgToPlayer(&m, idTo, 1, 1);
    return m.netMsg.msgId;
}

static int u16At(const Packet* p, int at)
{
    return p->data[at] | (p->data[at + 1] << 8);
}

// Packets of game g's test message msgId, to net id `to` (0: anyone).
static int countSent(int g, int msgId, int to)
{
    int n = 0;
    for (int i = 0; i < npackets; i++)
    {
        Packet* p = &packets[i];
        if (p->from == games[g].netId && (!to || p->to == to) && u16At(p, 0) == TESTMSG && u16At(p, 2) == msgId)
            n++;
    }
    return n;
}

// Acks sent by game g.
static int countAcks(int g)
{
    int n = 0;
    for (int i = 0; i < npackets; i++)
        n += packets[i].from == games[g].netId && u16At(&packets[i], 0) == DSS_RESET;
    return n;
}

static void checkCount(int g, const char* when)
{
    CHECK(games[g].idk2 == slotsInUse(g), "%s: sithComm_idk2 %d, slots in use %d", when, games[g].idk2, slotsInUse(g));
}

static void testAckHighIds(void)
{
    static const int ids[] = { 1, 0x7FFF, 0x8000, 0x8001, 0xFFFF };
    for (int k = 0; k < (int)(sizeof(ids) / sizeof(ids[0])); k++)
    {
        setup(2);
        games[0].msgId = ids[k];
        int id = sendTest(0, 2, 1);
        CHECK(id == ids[k], "msgId %#x went out as %#x", ids[k], id);
        runFor(15000);
        CHECK(countSent(0, id, 0) == 1, "msgId %#x: sent %d times, not once", id, countSent(0, id, 0));
        CHECK(runs[1][0][1] == 1, "msgId %#x: ran %d times", id, runs[1][0][1]);
        CHECK(slotsInUse(0) == 0, "msgId %#x: %d slots still in use", id, slotsInUse(0));
        checkCount(0, "after the ack");
        CHECK(countWrong == 0, "msgId %#x: sithComm_idk2 was wrong on %d frames", id, countWrong);

        // Numbering is unchanged: up by one, and 0 skipped.
        int next = sendTest(0, 2, 2);
        CHECK(next == (id == 0xFFFF ? 1 : id + 1), "after %#x came %#x", id, next);
    }

    // A run of messages across 0x8000, each acked well before the next.
    setup(2);
    games[0].msgId = 0x7FF8;
    int sent = 0;
    for (int n = 0; n < 16; n++)
    {
        sendTest(0, 2, n);
        runFor(2000);
    }
    runFor(12000);
    for (int i = 0; i < npackets; i++)
        sent += packets[i].from == 1 && u16At(&packets[i], 0) == TESTMSG;
    CHECK(sent == 16, "16 messages from 0x7FF8 on took %d packets", sent);
    CHECK(countWrong == 0, "sithComm_idk2 was wrong on %d frames", countWrong);
}

static void testSlotCount(void)
{
    setup(2);
    checkCount(0, "at the start");

    // Two sends while the link is down share the one slot.
    linkDown[0][1] = 1;
    sendTest(0, 2, 1);
    checkCount(0, "after a send");
    runFor(100);
    sendTest(0, 2, 2);
    checkCount(0, "after a send that replaced a waiting one");
    CHECK(slotsInUse(0) == 1, "%d slots in use", slotsInUse(0));

    // The ack frees it.
    linkDown[0][1] = 0;
    runFor(2000);
    CHECK(runs[1][0][2] == 1, "the waiting message ran %d times", runs[1][0][2]);
    CHECK(slotsInUse(0) == 0, "%d slots in use after the ack", slotsInUse(0));
    checkCount(0, "after the ack");

    // A message nobody acks goes out 7 times, 1700 ms apart, and is dropped.
    linkDown[1][0] = 1;
    uint32_t t0 = sithTime_g_msecGameTime;
    int id = sendTest(0, 2, 3);
    runFor(15000);
    uint32_t times[16];
    int n = 0;
    for (int i = 0; i < npackets && n < 16; i++)
        if (packets[i].from == 1 && u16At(&packets[i], 0) == TESTMSG && u16At(&packets[i], 2) == id)
            times[n++] = packets[i].sentAt;
    CHECK(n == 7, "an unacked message went out %d times, not 7", n);
    CHECK(n > 0 && times[0] == t0, "first send at %u, not %u", n > 0 ? times[0] : 0, t0);
    for (int i = 1; i < n; i++)
        CHECK(times[i] - times[i - 1] >= 1700 && times[i] - times[i - 1] < 1700 + FRAME,
              "resend %d came %u ms after the one before", i, times[i] - times[i - 1]);
    CHECK(runs[1][0][3] == 1, "the unacked message ran %d times", runs[1][0][3]);
    CHECK(slotsInUse(0) == 0, "%d slots in use after the last resend", slotsInUse(0));
    checkCount(0, "after the last resend");
    linkDown[1][0] = 0;

    // Forgetting a sender is about received messages and leaves the count.
    linkDown[0][1] = 1;
    sendTest(0, 2, 4);
    use(0);
    sithComm_ForgetSender(2);
    CHECK(slotsInUse(0) == 1, "%d slots in use after ForgetSender", slotsInUse(0));
    checkCount(0, "after ForgetSender");

    // Forgetting the only recipient frees the slot.
    use(0);
    sithComm_ForgetRecipient(1);
    CHECK(slotsInUse(0) == 0, "%d slots in use after forgetting the only recipient", slotsInUse(0));
    checkCount(0, "after ForgetRecipient");
    CHECK(countWrong == 0, "sithComm_idk2 was wrong on %d frames", countWrong);

    // A message to three players stays until the last one is forgotten.
    setup(3);
    linkDown[0][1] = linkDown[0][2] = 1;
    sendTest(0, -1, 1);
    use(0);
    sithComm_ForgetRecipient(1);
    CHECK(slotsInUse(0) == 1, "%d slots in use with one recipient left", slotsInUse(0));
    checkCount(0, "after forgetting one of two recipients");
    use(0);
    sithComm_ForgetRecipient(2);
    CHECK(slotsInUse(0) == 0, "%d slots in use with no recipient left", slotsInUse(0));
    checkCount(0, "after forgetting both recipients");

    // An ack frees a message to everyone only for the player who sent it:
    // the one who didn't get it still gets the resend.
    setup(3);
    linkDown[0][1] = 1;
    id = sendTest(0, -1, 1);
    runFor(500);
    CHECK(slotsInUse(0) == 1 && slots(0)[0].netMsg.field_14 == 2,
          "after one of two acks the message waits for players %08x", slotsInUse(0) ? slots(0)[0].netMsg.field_14 : 0);
    checkCount(0, "after one of two acks");
    linkDown[0][1] = 0;
    runFor(3000);
    CHECK(runs[1][0][1] == 1, "the player still waiting ran it %d times", runs[1][0][1]);
    CHECK(countSent(0, id, 2) == 2, "it went out %d times to the player still waiting, not 2", countSent(0, id, 2));
    CHECK(countSent(0, id, 3) == 1, "it went out %d times to the player who acked, not once", countSent(0, id, 3));
    CHECK(slotsInUse(0) == 0, "%d slots in use after both acks", slotsInUse(0));
    CHECK(countWrong == 0, "sithComm_idk2 was wrong on %d frames", countWrong);

    // Forgetting a recipient before anything was sent (a new session) is
    // fine, also before the retro ports' buffer exists.
    setup(2);
    use(1);
    sithComm_ForgetRecipient(0);
    sithComm_ForgetRecipient(31);
    sithComm_ForgetRecipient(-1);
    sithComm_ForgetRecipient(32);
    checkCount(1, "after forgetting with nothing sent");

    // An index outside the table forgets nobody, also while a message waits
    // for indices 0 and 31 (a player list's player count comes off the wire
    // unchecked). Read from memory, so the compiler can't fold the shifts.
    static volatile int outside[] = { -1, 32, 33, 64 };
    setup(2);
    fillTable(1);
    linkDown[1][0] = 1;
    sendTest(1, -1, 1);
    use(1);
    for (int k = 0; k < (int)(sizeof(outside) / sizeof(outside[0])); k++)
        sithComm_ForgetRecipient(outside[k]);
    uint32_t waiting = slotsInUse(1) ? slots(1)[0].netMsg.field_14 : 0;
    CHECK(waiting == 0xFFFFFFFDu, "after forgetting indices outside the table the message waits for players %08x", waiting);
    checkCount(1, "after forgetting indices outside the table");
    linkDown[1][0] = 0;
    runFor(3000);
    CHECK(runs[0][1][1] == 1, "the host ran the message %d times", runs[0][1][1]);

    // An ack for id 0, which no game sends, frees no slot and stops no resend.
    setup(2);
    linkDown[0][1] = 1;
    sendTest(0, 2, 1);
    runFor(FRAME);
    uint8_t stray[6] = { DSS_RESET, 0, 0, 0, 0, 0 };
    addPacket(2, 1, stray, sizeof(stray));
    runFor(100);
    CHECK(slotsInUse(0) == 1, "%d slots in use after an ack for id 0", slotsInUse(0));
    checkCount(0, "after an ack for id 0");
    linkDown[0][1] = 0;
    runFor(3000);
    CHECK(runs[1][0][1] == 1, "after an ack for id 0 the message ran %d times", runs[1][0][1]);
    CHECK(countWrong == 0, "sithComm_idk2 was wrong on %d frames", countWrong);
}

// The player at index idx leaves game g.
static void leave(int g, int idx)
{
    use(g);
    sithPlayer_Startup(idx);
}

static void put(uint8_t** pp, const void* v, size_t n)
{
    memcpy(*pp, v, n);
    *pp += n;
}

// The host's player list, DSS_LEAVEJOIN as sithMulti_SyncPlayers writes it,
// from game `from` to game `to`. It goes out once and not reliably, as the
// host sends it when a player leaves.
static void sendPlayerList(int from, int to)
{
    static uint8_t d[4 + 10 + 32 * 34];
    uint8_t* p = d;
    Game* h = &games[from];
    uint16_t head[2] = { DSS_LEAVEJOIN, 0 };
    int32_t modeFlags = 0, serverNetId = h->netId;
    int16_t maxPlayers = h->maxPlayers, scores[5] = { 0 };
    char name[16] = { 0 };
    put(&p, head, sizeof(head));
    put(&p, &modeFlags, 4);
    put(&p, &serverNetId, 4);
    put(&p, &maxPlayers, 2);
    for (uint32_t j = 0; j < h->maxPlayers; j++)
    {
        int32_t flags = h->players[j].flags, netId = h->players[j].playerNetId;
        put(&p, &flags, 4);
        if (flags & 4)
        {
            put(&p, &netId, 4);
            put(&p, name, sizeof(name));
            put(&p, scores, sizeof(scores));
        }
    }
    addPacket(h->netId, games[to].netId, d, p - d);
}

// Game 1 (net id 2) is gone, and game 3 joins as net id 4 at its index.
static void joinAtIndex1(void)
{
    games[1].running = 0;
    startGame(3, 4, 1, 3);
}

static void testIndexReassigned(void)
{
    // The host's message to everyone doesn't reach game 1, who leaves; the
    // next player takes index 1 before the resend.
    setup(3);
    linkDown[0][1] = 1;
    int id = sendTest(0, -1, 1);
    runFor(500);
    CHECK(runs[2][0][1] == 1, "game 2 ran the message %d times", runs[2][0][1]);
    leave(0, 1);
    runFor(300);
    joinAtIndex1();
    games[0].players[1].flags = 5;
    games[0].players[1].playerNetId = 4;
    runFor(15000);
    CHECK(countSent(0, id, 4) == 0, "the host resent the last player's message to the new one %d times", countSent(0, id, 4));
    CHECK(runs[3][0][1] == 0, "the new player ran the last player's message %d times", runs[3][0][1]);
    CHECK(slotsInUse(0) == 0, "%d slots in use", slotsInUse(0));
    CHECK(countWrong == 0, "sithComm_idk2 was wrong on %d frames", countWrong);

    // The same on a joiner, who learns from the host's player list that index
    // 1 has a new net id.
    setup(3);
    linkDown[2][1] = 1;
    id = sendTest(2, -1, 1);
    runFor(500);
    CHECK(runs[0][2][1] == 1, "the host ran the message %d times", runs[0][2][1]);
    joinAtIndex1();
    games[0].players[1].playerNetId = 4;
    sendPlayerList(0, 2);
    runFor(100);
    CHECK(games[2].players[1].playerNetId == 4, "the joiner has net id %u at index 1", games[2].players[1].playerNetId);
    runFor(15000);
    CHECK(countSent(2, id, 4) == 0, "the joiner resent the last player's message to the new one %d times", countSent(2, id, 4));
    CHECK(runs[3][2][1] == 0, "the new player ran the last player's message %d times", runs[3][2][1]);
    CHECK(slotsInUse(2) == 0, "%d slots in use", slotsInUse(2));
    CHECK(countWrong == 0, "sithComm_idk2 was wrong on %d frames", countWrong);

    // A player list forgets only the indices whose net id changed: the
    // joiner's message to the host, not through yet, is still resent.
    setup(3);
    linkDown[2][0] = 1;
    id = sendTest(2, 1, 1);
    runFor(500);
    sendPlayerList(0, 2);
    runFor(100);
    CHECK(slotsInUse(2) == 1, "%d slots in use after the player list", slotsInUse(2));
    linkDown[2][0] = 0;
    runFor(3000);
    CHECK(runs[0][2][1] == 1, "the host ran the joiner's message %d times", runs[0][2][1]);
    CHECK(countSent(2, id, 1) == 2, "the joiner's message went out %d times, not 2", countSent(2, id, 1));
    CHECK(slotsInUse(2) == 0, "%d slots in use", slotsInUse(2));
    CHECK(countWrong == 0, "sithComm_idk2 was wrong on %d frames", countWrong);
}

static void testLateAck(void)
{
    // Game 1 acks the host's message to everyone and leaves before the ack
    // arrives. Its ack must not count for anyone else: index 31, who never
    // got the message, still gets every resend.
    setup(2);
    fillTable(0);
    int id = sendTest(0, -1, 1);
    runFor(40);
    CHECK(runs[1][0][1] == 1 && countAcks(1) == 1, "game 1 ran the message %d times", runs[1][0][1]);
    leave(0, 1);
    runFor(100);
    uint32_t waiting = slotsInUse(0) ? slots(0)[0].netMsg.field_14 : 0;
    CHECK(waiting == 0xFFFFFFFCu, "after the late ack the message waits for players %08x", waiting);
    runFor(15000);
    CHECK(countSent(0, id, 32) == 7, "index 31 got the message %d times, not 7", countSent(0, id, 32));
    CHECK(countWrong == 0, "sithComm_idk2 was wrong on %d frames", countWrong);
}

static void testOneSlot(void)
{
    // Five messages while the link is down for a second: only the newest
    // waits, and only it arrives. This is on purpose, so an older message
    // never lands after a newer one.
    setup(2);
    linkDown[0][1] = 1;
    int ids[5];
    for (int n = 0; n < 5; n++)
    {
        ids[n] = sendTest(0, 2, n);
        runFor(20);
    }
    CHECK(slotsInUse(0) == 1, "%d slots in use after 5 sends", slotsInUse(0));
    CHECK(slots(0)[0].netMsg.msgId == ids[4], "slot 0 holds msgId %d, not the newest %d", slots(0)[0].netMsg.msgId, ids[4]);
    checkCount(0, "after 5 sends");
    runFor(1000);
    linkDown[0][1] = 0;
    runFor(15000);
    for (int n = 0; n < 4; n++)
        CHECK(runs[1][0][n] == 0, "older message %d ran %d times", n, runs[1][0][n]);
    CHECK(runs[1][0][4] == 1, "the newest message ran %d times", runs[1][0][4]);
    CHECK(countSent(0, ids[4], 2) == 2, "the newest message went out %d times, not 2", countSent(0, ids[4], 2));
    CHECK(countWrong == 0, "sithComm_idk2 was wrong on %d frames", countWrong);
}

static void testWire(void)
{
    // The ack is DSS_RESET without a msgId of its own, and the acked id in 2
    // bytes, to the sender alone.
    static const int ids[] = { 1, 0x1234, 0x7FFF, 0x8000, 0xFFFF };
    for (int k = 0; k < (int)(sizeof(ids) / sizeof(ids[0])); k++)
    {
        setup(2);
        games[0].msgId = ids[k];
        int id = sendTest(0, 2, 7);
        runFor(200);
        int acks = 0;
        for (int i = 0; i < npackets; i++)
        {
            Packet* p = &packets[i];
            if (p->from != 2)
                continue;
            acks++;
            uint8_t want[6] = { DSS_RESET, 0, 0, 0, id & 0xFF, id >> 8 };
            CHECK(p->to == 1, "the ack for %#x went to %d", id, p->to);
            CHECK(p->len == 6 && !memcmp(p->data, want, 6),
                  "the ack for %#x is %u bytes %02x %02x %02x %02x %02x %02x", id, p->len,
                  p->data[0], p->data[1], p->data[2], p->data[3], p->data[4], p->data[5]);
        }
        CHECK(acks == 1, "%d acks for %#x", acks, id);

        // The message itself: its id, msgId and payload.
        Packet* first = &packets[0];
        uint8_t want[8] = { TESTMSG, 0, id & 0xFF, id >> 8, 7, 0, 0, 0 };
        CHECK(first->from == 1 && first->to == 2 && first->len == 8 && !memcmp(first->data, want, 8),
              "message %#x went out as %u bytes from %d to %d", id, first->len, first->from, first->to);
    }

    // A resend is the same bytes as the first send.
    setup(2);
    linkDown[0][1] = 1;
    sendTest(0, 2, 9);
    runFor(2000);
    CHECK(npackets == 2 && packets[0].len == packets[1].len && !memcmp(packets[0].data, packets[1].data, packets[0].len),
          "the resend differs from the first send (%d packets)", npackets);
}

static void testReceiver(void)
{
    // The receiver's acks are all lost: it acks every copy, and runs the
    // message once.
    setup(2);
    linkDown[1][0] = 1;
    int id = sendTest(0, 2, 1);
    runFor(15000);
    CHECK(countSent(0, id, 2) == 7, "sent %d times", countSent(0, id, 2));
    CHECK(countAcks(1) == 7, "%d acks for 7 copies", countAcks(1));
    CHECK(runs[1][0][1] == 1, "the receiver ran it %d times", runs[1][0][1]);

    // Messages from two senders with the same msgId are both new.
    setup(3);
    sendTest(0, 3, 1);
    sendTest(1, 3, 2);
    runFor(200);
    CHECK(runs[2][0][1] == 1 && runs[2][1][2] == 1, "game 2 ran %d and %d", runs[2][0][1], runs[2][1][2]);
}

int main(void)
{
    testAckHighIds();
    testSlotCount();
    testIndexReassigned();
    testLateAck();
    testOneSlot();
    testWire();
    testReceiver();

    use(cur);
    for (int g = 0; g < NGAMES; g++)
        free(games[g].heapBuf);

    if (failures)
    {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
