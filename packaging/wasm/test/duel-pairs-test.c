/* Several private duels in one game, refereed by the production host adapter.
 * Five seats: the host (id 1) and four browser players whose requests arrive
 * as the same 28-byte frames the network carries. Only the transport, clock,
 * console and world callbacks are supplied here.
 */
#include "Gameplay/sithDuel.h"
#include "Gameplay/sithDuelState.h"
#include "Gameplay/sithDuelCombat.h"
#include "Gameplay/sithInventory.h"
#include "Gameplay/jkSaber.h"
#include "Devices/sithComm.h"
#include "Devices/sithConsole.h"
#include "Dss/sithMulti.h"
#include "Engine/sithCollision.h"
#include "Engine/sithPhysics.h"
#include "Primitives/rdMatrix.h"
#include "Primitives/rdVector.h"
#include "World/sithThing.h"
#include "World/sithWeapon.h"
#include "General/stdString.h"
#include "stdPlatform.h"
#include "globals.h"
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { PEOPLE = 5 };

int32_t sithNet_isMulti = 1, sithNet_isServer = 1, sithNet_MultiModeFlags;
int32_t sithNet_bNeedsFullThingSyncForLeaveJoin, sithMulti_newPlayerId;
uint32_t sithMulti_curWelcomePlayerNum, jkPlayer_maxPlayers = PEOPLE;
int stdComm_dplayIdSelf = 1, stdComm_bIsServer = 1, stdComm_Imperial_bActive;
int stdComm_WSRelay_bViewer, jkGuiNetHost_bIsDedicated, playerThingIdx;
int Main_bMotsCompat, sithWeapon_8BD024 = -1;
int sithInventory_bUnkPower, sithInventory_g_bSendDeactivateMessage;
SithControlBinding sithInventory_powerKeybinds[SITHINVENTORY_NUM_POWERKEYBINDS];
SithPlayer jkPlayer_playerInfos[JKPLAYER_NUM_INFOS];
SithInventoryType sithInventory_g_aTypes[SITHBIN_NUMBINS];
SithThing *sithPlayer_g_pLocalPlayerThing;
jkEpisodeLoad jkEpisode_mLoad;

static SithThing people[PEOPLE];
static SithSector sector;
static sithCog saberCog;
static uint32_t clockMs = 1000;

// What every player has been told, decoded the way a browser decodes it.
static DuelHeader told;
static DuelState seen[DUEL_MAX_PAIRS];
static int framesTo[PEOPLE + 1], firstWasHeader[PEOPLE + 1];
static char console[4096], chat[PEOPLE + 1][1024];

int sithComm_SendMsgToPlayer(SithMessage *msg, int to, int outstream, int flags)
{
    (void)outstream; (void)flags;
    const uint8_t *p = (const uint8_t *)msg->pktData;
    assert(msg->netMsg.msg_size == DUEL_WIRE_SIZE && to > 1 && to <= PEOPLE);
    if (!framesTo[to]++) firstWasHeader[to] = p[1] == 0x81;
    DuelHeader h = told;
    if (sithDuelState_ReadHeader(&h, p, DUEL_WIRE_SIZE, DUEL_HOST_ID)) {
        told = h;
        return 1;
    }
    int i = sithDuelState_FrameIndex(p, DUEL_WIRE_SIZE);
    assert(i >= 0);
    DuelState v = {0};
    assert(sithDuelState_ReadSnapshot(&v, p, DUEL_WIRE_SIZE, DUEL_HOST_ID));
    if (seen[i].epoch != v.epoch || (int32_t)(v.revision - seen[i].revision) > 0)
        seen[i] = v;
    return 1;
}

void sithConsole_PrintString(const char *text)
{
    strncat(console, text, sizeof(console) - strlen(console) - 2);
    strcat(console, "\n");
}

void sithMulti_SendChatTo(const char *text, int a, int b, int to)
{
    (void)a; (void)b;
    assert(to > 1 && to <= PEOPLE);
    strncat(chat[to], text, sizeof(chat[to]) - strlen(chat[to]) - 2);
    strcat(chat[to], "\n");
}

uint32_t stdPlatform_GetTimeMsec(void) { return clockMs; }
static char netLog[4096];
int stdPlatform_Printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t used = strlen(netLog);
    int n = vsnprintf(netLog + used, sizeof(netLog) - used, fmt, ap);
    va_end(ap);
    return n;
}
int stdComm_WSRelay_InMatch(void) { return 1; }
int stdString_WcharToChar(char *out, const char16_t *in, int n)
{
    int i = 0;
    for (; i < n && in[i]; ++i) out[i] = (char)in[i];
    out[i] = 0;
    return i;
}
flex_t rdVector_Dist3(const rdVector3 *a, const rdVector3 *b)
{
    return sqrtf((a->x-b->x)*(a->x-b->x)+(a->y-b->y)*(a->y-b->y)+(a->z-b->z)*(a->z-b->z));
}
rdVector3 *rdVector_Add3Acc(rdVector3 *a, const rdVector3 *b)
{
    a->x += b->x; a->y += b->y; a->z += b->z; return a;
}
void rdMatrix_PreRotate34(rdMatrix34 *mat, const rdVector3 *angles) { (void)mat; (void)angles; }
int sithCollision_HasLOS(SithThing *a, SithThing *b, int flags) { (void)a; (void)b; (void)flags; return 1; }
SithSector *sithCollision_FindSectorInRadius(SithSector *s, const rdVector3 *a, rdVector3 *b, flex_t r)
{ (void)a; (void)b; (void)r; return s; }
flex_t sithCollision_SearchForCollisions(SithSector *s, SithThing *t, const rdVector3 *p, const rdVector3 *v, flex_t d, flex_t r, int flags)
{ (void)s; (void)t; (void)p; (void)v; (void)d; (void)r; (void)flags; return 0; }
SithCollision *sithCollision_PopStack(void) { return NULL; }
void sithCollision_DecreaseStackLevel(void) {}
void jkSaber_Disable(SithThing *p) { (void)p; }
void sithWeapon_StartupEntry(void) {}
void sithPhysics_ResetThingMovement(SithThing *p) { (void)p; }
int sithWeapon_ProcessWeaponControls(SithThing *p, flex_t dt) { (void)p; (void)dt; return 0; }
SithThing *sithThing_GetThingParent(SithThing *p) { return p; }
flex_t sithInventory_SetInventory(SithThing *p, int bin, flex_t amount)
{
    return p->actorParams.pPlayer->aItems[bin].amount = amount;
}

static void forget(void)
{
    memset(framesTo, 0, sizeof(framesTo));
    memset(firstWasHeader, 0, sizeof(firstWasHeader));
    console[0] = 0;
    for (int i = 0; i <= PEOPLE; i++) chat[i][0] = 0;
}

static void setup(int dedicated)
{
    memset(people, 0, sizeof(people));
    memset(jkPlayer_playerInfos, 0, sizeof(jkPlayer_playerInfos));
    memset(sithInventory_g_aTypes, 0, sizeof(sithInventory_g_aTypes));
    memset(seen, 0, sizeof(seen));
    told = (DuelHeader){0};
    forget();
    clockMs = 1000;
    sithDuel_bAllowDuels = 1;
    jkGuiNetHost_bIsDedicated = dedicated;
    strcpy(sithInventory_g_aTypes[SITHBIN_LIGHTSABER].fpath, "Lightsaber");
    sithInventory_g_aTypes[SITHBIN_LIGHTSABER].flags = SITHINVENTORY_TYPE_REGISTERED | ITEMINFO_WEAPON;
    sithInventory_g_aTypes[SITHBIN_LIGHTSABER].cog = &saberCog;
    for (int i = 0; i < PEOPLE; i++) {
        SithThing *p = &people[i];
        SithPlayer *info = &jkPlayer_playerInfos[i];
        p->type = SITH_THING_PLAYER; p->signature = 100 + i; p->sector = &sector; p->position.x = (float)i * 0.4f;
        p->actorParams.pPlayer = info; p->actorParams.health = 50; p->actorParams.maxHealth = 100;
        info->pLocalPlayer = p; info->flags = 1; info->playerNetId = i + 1; info->curWeaponID = SITHBIN_LIGHTSABER;
        info->aItems[SITHBIN_LIGHTSABER].amount = 1;
        info->aItems[SITHBIN_LIGHTSABER].state = SITHINVENTORY_ITEM_AVAILABLE;
        char name[8];
        snprintf(name, sizeof(name), "P%d", i + 1);
        for (size_t j = 0; j < strlen(name); j++) info->player_name[j] = (char16_t)name[j];
    }
    // A Dedicated Server host keeps the first seat but isn't in the game.
    if (dedicated) jkPlayer_playerInfos[0].flags = 6;
    playerThingIdx = 0;
    sithPlayer_g_pLocalPlayerThing = &people[0];
    sithDuel_Reset();
    sithDuel_Frame();
    for (int i = 1; i < PEOPLE; i++) sithDuel_Hello(i, (uint32_t)i + 1, 1);
}

static void deliver(uint32_t from, const uint8_t *bytes)
{
    SithMessage msg = {0};
    msg.netMsg.cogMsgId = SITHDUEL_MSG;
    msg.netMsg.idx = from;
    msg.netMsg.msg_size = DUEL_WIRE_SIZE;
    memcpy(msg.pktData, bytes, DUEL_WIRE_SIZE);
    sithDuel_Process(&msg);
}

// A browser player's request, built from what it has been told.
static void ask(uint32_t from, enum DuelAction action, uint32_t target, int index)
{
    DuelRequest r = { action, told.epoch, 0, 0, target, DUEL_NO_PAIR };
    if (index >= 0) {
        r.index = (uint32_t)index;
        r.revision = seen[index].revision;
        r.generation = seen[index].generation;
    }
    uint8_t bytes[DUEL_WIRE_SIZE];
    sithDuelState_WriteRequest(bytes, &r);
    deliver(from, bytes);
}

static int pairOf(uint32_t a, uint32_t b)
{
    for (int i = 0; i < DUEL_MAX_PAIRS; i++)
        if (seen[i].epoch && seen[i].phase != DUEL_IDLE && seen[i].a == a && seen[i].b == b)
            return i;
    return -1;
}

static int lastOf(uint32_t a, uint32_t b, enum DuelReason reason)
{
    for (int i = 0; i < DUEL_MAX_PAIRS; i++)
        if (seen[i].epoch && seen[i].phase == DUEL_IDLE && seen[i].a == a && seen[i].b == b && seen[i].reason == reason)
            return 1;
    return 0;
}

static void startFight(uint32_t a, uint32_t b)
{
    ask(a, DUEL_CHALLENGE, b, -1);
    int i = pairOf(a, b);
    assert(i >= 0 && seen[i].phase == DUEL_OFFER);
    ask(b, DUEL_ACCEPT, 0, i);
    assert(seen[i].phase == DUEL_PREPARE);
    ask(a, DUEL_READY, 0, i);
    ask(b, DUEL_READY, 0, i);
}

static void twoDuelsAtOnce(void)
{
    setup(0);
    assert(told.epoch && told.allowed);
    startFight(2, 3);
    startFight(4, 5);
    clockMs += 2000;
    sithDuel_Frame();
    int p23 = pairOf(2, 3), p45 = pairOf(4, 5);
    assert(p23 >= 0 && p45 >= 0 && p23 != p45);
    assert(seen[p23].phase == DUEL_FIGHT && seen[p45].phase == DUEL_FIGHT);
    for (int slot = 0; slot < PEOPLE; slot++)
        assert(sithDuel_PhaseForPlayer(slot) == (slot ? 2 : 0));
    assert(sithDuel_AreOpponents(1, 2) && sithDuel_AreOpponents(3, 4));
    assert(!sithDuel_AreOpponents(1, 3) && !sithDuel_AreOpponents(2, 4) && !sithDuel_AreOpponents(0, 1));
    // Partners' sabers land; nothing crosses from one duel to the other.
    assert(sithDuelCombat_AllowsDamage(&people[1], &people[2], 1));
    assert(sithDuelCombat_AllowsDamage(&people[3], &people[4], 1));
    assert(!sithDuelCombat_AllowsDamage(&people[1], &people[3], 1));
    assert(!sithDuelCombat_AllowsDamage(&people[4], &people[2], 1));
    assert(!sithDuelCombat_AllowsDamage(&people[0], &people[1], 1));
    assert(!sithDuelCombat_AllowsCollision(&people[2], &people[3]));
    assert(sithDuelCombat_AllowsCollision(&people[1], &people[2]));

    // Nobody can pull a duelist into a second duel.
    forget();
    sithDuel_Command(NULL, "1");
    assert(strstr(console, "P2 is already in a duel."));
    ask(2, DUEL_CHALLENGE, 1, -1);
    assert(strstr(chat[2], "You're already in a duel."));
    assert(pairOf(1, 2) < 0 && pairOf(2, 1) < 0);

    // One duel ends; the other goes on.
    sithDuel_OwnerDeath(&people[2], 3);
    sithDuel_Frame();
    assert(seen[p23].phase == DUEL_IDLE && seen[p23].reason == DUEL_DEFEAT && seen[p23].winner == 2);
    assert(seen[p45].phase == DUEL_FIGHT);
    assert(!sithDuel_PhaseForPlayer(1) && !sithDuel_PhaseForPlayer(2));
    assert(sithDuel_PhaseForPlayer(3) == 2 && sithDuel_PhaseForPlayer(4) == 2);
    assert(sithDuelCombat_AllowsDamage(&people[1], &people[0], 0));

    // A late arrival hears the header first, then every duel under way or just over.
    forget();
    sithDuel_Welcome(3, 4);
    assert(firstWasHeader[4] && framesTo[4] == 3);

    // The two winners and the host can now take on each other.
    forget();
    ask(2, DUEL_CHALLENGE, 1, -1);
    clockMs += 10;
    ask(3, DUEL_CHALLENGE, 1, -1);
    assert(pairOf(2, 1) >= 0 && pairOf(3, 1) >= 0);
    sithDuel_Command(NULL, "accept"); // the newest challenge
    assert(pairOf(3, 1) >= 0 && seen[pairOf(3, 1)].phase == DUEL_PREPARE);
    assert(pairOf(2, 1) < 0 && lastOf(2, 1, DUEL_CANCELLED));
    sithDuel_Command(NULL, "cancel");
    assert(pairOf(3, 1) < 0 && lastOf(3, 1, DUEL_CANCELLED));

    // One challenge of your own at a time.
    forget();
    ask(2, DUEL_CHALLENGE, 3, -1);
    assert(pairOf(2, 3) >= 0);
    ask(2, DUEL_CHALLENGE, 3, -1);
    assert(strstr(chat[2], "Your challenge is still waiting for an answer."));
    ask(2, DUEL_CHALLENGE, 1, -1);
    assert(pairOf(2, 3) < 0 && pairOf(2, 1) >= 0);

    // A request about another pair's numbers changes nothing.
    int p21 = pairOf(2, 1);
    forget();
    DuelRequest wrong = { DUEL_ACCEPT, told.epoch, seen[p21].revision, seen[p21].generation, 0, (uint32_t)p45 };
    uint8_t bytes[DUEL_WIRE_SIZE];
    sithDuelState_WriteRequest(bytes, &wrong);
    deliver(5, bytes);
    assert(strstr(chat[5], "expired or the duel has changed") && seen[p45].phase == DUEL_FIGHT);

    // The host unticks Private duels: every duel and offer ends, everyone is told.
    uint32_t revision = told.revision;
    sithDuel_bAllowDuels = 0;
    sithDuel_Frame();
    assert(!told.allowed && told.revision == revision + 1);
    assert(seen[p45].phase == DUEL_IDLE && seen[p45].reason == DUEL_INELIGIBLE && !seen[p45].winner);
    assert(pairOf(2, 1) < 0);
    for (int slot = 0; slot < PEOPLE; slot++)
        assert(!sithDuel_PhaseForPlayer(slot));
    forget();
    ask(4, DUEL_CHALLENGE, 5, -1);
    assert(strstr(chat[4], "This game's host hasn't turned on private duels.") && pairOf(4, 5) < 0);
    sithDuel_Command(NULL, "1");
    assert(strstr(console, "This game's host hasn't turned on private duels."));
    sithDuel_bAllowDuels = 1;
    sithDuel_Frame();
    assert(told.allowed && told.revision == revision + 2);
}

// The duel host can host as a Dedicated Server and referee without playing.
static void dedicatedHost(void)
{
    setup(1);
    assert(told.epoch && told.allowed);
    startFight(2, 3);
    startFight(4, 5);
    clockMs += 2000;
    sithDuel_Frame();
    int p23 = pairOf(2, 3), p45 = pairOf(4, 5);
    assert(p23 >= 0 && seen[p23].phase == DUEL_FIGHT && p45 >= 0 && seen[p45].phase == DUEL_FIGHT);
    assert(!sithDuel_PhaseForPlayer(0) && sithDuel_PhaseForPlayer(1) == 2 && sithDuel_PhaseForPlayer(4) == 2);
    sithDuel_OwnerDeath(&people[4], 5);
    sithDuel_Frame();
    assert(seen[p45].reason == DUEL_DEFEAT && seen[p45].winner == 4 && seen[p23].phase == DUEL_FIGHT);
}

// A Dedicated Server host hears no hellos, since its seat isn't in the game.
// A game it doesn't know can duel asks for the host's state and is told; any
// duel request counts the same, and the asks are answered once a second.
static void dedicatedHostAsked(void)
{
    setup(1);
    for (int i = 1; i < PEOPLE; i++) sithDuel_Forget(i, (uint32_t)i + 1);
    told = (DuelHeader){0};
    forget();
    clockMs += 2600;
    sithDuel_Frame();
    assert(!framesTo[2] && !framesTo[5]);
    netLog[0] = 0;
    ask(2, DUEL_SYNC, 0, -1);
    assert(framesTo[2] == 1 && firstWasHeader[2] && told.epoch && told.allowed);
    assert(strstr(netLog, "player 2's game asked, and this host hadn't known it can duel."));
    ask(2, DUEL_SYNC, 0, -1);
    assert(framesTo[2] == 1);
    clockMs += 1000;
    ask(2, DUEL_SYNC, 0, -1);
    assert(framesTo[2] == 2);
    // Seats 3-5 haven't asked, so nobody can start a duel yet.
    ask(2, DUEL_CHALLENGE, 3, -1);
    assert(pairOf(2, 3) < 0 && strstr(chat[2], "Every active player must support private duels."));
    ask(4, DUEL_SYNC, 0, -1);
    ask(5, DUEL_SYNC, 0, -1);
    assert(framesTo[4] == 1 && framesTo[5] == 1);
    // Seat 3's challenge is its first word, and it stands.
    ask(3, DUEL_CHALLENGE, 2, -1);
    int i = pairOf(3, 2);
    assert(i >= 0 && seen[i].phase == DUEL_OFFER && framesTo[3] >= 2);
    // A frame from a player who isn't seated, or the host's own id, is no ask.
    clockMs += 1000;
    int before2 = framesTo[2], before5 = framesTo[5];
    ask(1, DUEL_SYNC, 0, -1);
    jkPlayer_playerInfos[4].flags = 0;
    ask(5, DUEL_SYNC, 0, -1);
    assert(framesTo[2] == before2 && framesTo[5] == before5);
}

// The join request's duel tag sits right after its 132 standard bytes. It
// was read from pktData[132], 528 bytes in, so every join counted as a game
// without duels and ended the duels under way.
static void joinTag(void)
{
    const size_t joinBytes = 32 + (16 + 32) * 2 + 4;
    SithMessage m;
    memset(&m, 0, sizeof(m));
    uint8_t *b = (uint8_t *)m.pktData;
    memcpy(b, "01narshadda.jkl", 15);
    uint32_t tag = SITHDUEL_JOIN_TAG;
    memcpy(b + joinBytes, &tag, 4);
    m.netMsg.msg_size = joinBytes + 4;
    assert(sithDuel_JoinTagged(&m, joinBytes));
    m.netMsg.msg_size = joinBytes; // an older game's request
    assert(!sithDuel_JoinTagged(&m, joinBytes));
    memset(b + joinBytes, 0, 4);
    m.pktData[joinBytes] = SITHDUEL_JOIN_TAG; // where it used to be looked for
    m.netMsg.msg_size = joinBytes + 4;
    assert(!sithDuel_JoinTagged(&m, joinBytes));

    // A game that can duel joining mid-duel leaves the duels alone.
    setup(0);
    startFight(2, 3);
    clockMs += 2000;
    sithDuel_Frame();
    int i = pairOf(2, 3);
    assert(i >= 0 && seen[i].phase == DUEL_FIGHT);
    jkPlayer_playerInfos[4].flags = 2;
    jkPlayer_playerInfos[4].playerNetId = 0;
    sithDuel_Forget(4, 5);
    sithDuel_Joining(4, 5, 1);
    jkPlayer_playerInfos[4].flags = 5;
    jkPlayer_playerInfos[4].playerNetId = 5;
    sithDuel_Welcome(4, 5);
    clockMs += 100;
    sithDuel_Frame();
    assert(seen[i].phase == DUEL_FIGHT);
    // One that can't ends them, since its game wouldn't keep the two apart.
    sithDuel_Joining(4, 5, 0);
    assert(seen[i].phase == DUEL_IDLE && seen[i].reason == DUEL_UNSUPPORTED);
}

// Teams, a lobby viewer, and a room whose first seat isn't the host have no duels.
static void notADuelRoom(void)
{
    setup(0);
    sithNet_MultiModeFlags = MULTIMODEFLAG_TEAMS;
    forget();
    ask(2, DUEL_CHALLENGE, 3, -1);
    assert(pairOf(2, 3) < 0);
    sithNet_MultiModeFlags = 0;
    jkPlayer_playerInfos[0].playerNetId = 7;
    ask(2, DUEL_CHALLENGE, 3, -1);
    assert(pairOf(2, 3) < 0);
    jkPlayer_playerInfos[0].playerNetId = 1;
    stdComm_WSRelay_bViewer = 1;
    ask(2, DUEL_CHALLENGE, 3, -1);
    assert(pairOf(2, 3) < 0);
    stdComm_WSRelay_bViewer = 0;
    assert(sithDuel_HostAllows());
    sithNet_MultiModeFlags = MULTIMODEFLAG_TEAMS;
    assert(!sithDuel_HostAllows());
    sithNet_MultiModeFlags = 0;
}

int main(void)
{
    twoDuelsAtOnce();
    dedicatedHost();
    dedicatedHostAsked();
    joinTag();
    notADuelRoom();
    puts("Several private duels at once, the host's box and a Dedicated Server host (told, and asked) and the join tag passed.");
}
