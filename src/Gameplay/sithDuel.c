#ifdef PLATFORM_WSRELAY
#include "Gameplay/sithDuel.h"
#include "Gameplay/sithDuelState.h"
#include "Gameplay/sithDuelCombat.h"
#include "Devices/sithComm.h"
#include "Devices/sithConsole.h"
#include "Dss/sithMulti.h"
#include "Engine/sithCollision.h"
#include "Gameplay/sithPlayer.h"
#include "General/stdString.h"
#include "Platform/Networking/Imperial/stdComm_Imperial.h"
#include "Platform/Networking/WSRelay/stdComm_WSRelay.h"
#include "Primitives/rdMatrix.h"
#include "Primitives/rdVector.h"
#include "Win95/stdComm.h"
#include "stdPlatform.h"
#include "jk.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The host referees every pair; the others mirror what it sends. Several
// pairs can duel at once, each player in at most one pair and with at most
// one challenge of their own out.
typedef struct DuelPair {
    DuelState s;
    int readyMask;       // host: one bit per participant, after owner-only checks
    int deadMask;        // death events survive a same-update respawn until the host resolves both players
    int replacementMask; // new lives never inherit the previous life's restrictions or reward
    int awaitingSecondDeath;
    uint32_t firstDeathAt;
    uint32_t changedAt;  // last change (a peer: when it heard), so a refresh still carries a fresh end
    uint32_t rewardedGeneration;
} DuelPair;

int sithDuel_bAllowDuels = 0;

static DuelPair pairs[DUEL_MAX_PAIRS];
static DuelHeader header;
static uint32_t nextEpoch;
static uint32_t capableId[JKPLAYER_NUM_INFOS];
static uint32_t lastSnapshotAt;
static int activePairs; // pairs preparing or fighting, so the combat checks can step aside cheaply
static int localCancelPending;
static int localCancelIndex = -1;
static int cancelFromPreparation;
static uint32_t lastCancelAt;
// A peer without the host's header asks for it (askHost), a few times.
static uint32_t askedAt;
static int asks;
// The host answers each seat's asks at most once a second.
static uint32_t answeredAt[JKPLAYER_NUM_INFOS];
static int loggedNoRoom; // the network log says once why a host can't referee

static void sendRequest(enum DuelAction action, uint32_t target, int index);

static uint32_t ownId(void)
{
    return (uint32_t)stdComm_dplayIdSelf;
}

static int slotValid(int slot)
{
    return slot >= 0 && slot < JKPLAYER_NUM_INFOS && (uint32_t)slot < jkPlayer_maxPlayers;
}

static uint32_t seat(int slot)
{
    if (!slotValid(slot) || !(jkPlayer_playerInfos[slot].flags & 1))
        return 0;
    return jkPlayer_playerInfos[slot].playerNetId;
}

static int findSeat(uint32_t id)
{
    if (id && id <= DUEL_MAX_ID)
        for (int i = 0; slotValid(i); i++)
            if (seat(i) == id)
                return i;
    return -1;
}

static SithThing *body(uint32_t id)
{
    int slot = findSeat(id);
    return slot < 0 ? NULL : jkPlayer_playerInfos[slot].pLocalPlayer;
}

static int freeForAll(void)
{
    return !(sithNet_MultiModeFlags & (MULTIMODEFLAG_TEAMS | MULTIMODEFLAG_COOP))
        && !(jkEpisode_mLoad.type & JK_EPISODE_SPECIAL_CTF);
}

// A browser room whose host is the relay's first seat. Its slot 0 is the
// host's even when it hosts as a Dedicated Server and isn't in the game (its
// flags then lack 1), so the host referees without playing.
static int room(void)
{
    return !stdComm_Imperial_bActive && !stdComm_WSRelay_bViewer && stdComm_WSRelay_InMatch()
        && jkPlayer_maxPlayers > 0 && jkPlayer_playerInfos[0].playerNetId == DUEL_HOST_ID
        && (!stdComm_bIsServer || ownId() == DUEL_HOST_ID) && freeForAll();
}

int sithDuel_HostAllows(void)
{
    return sithDuel_bAllowDuels && freeForAll();
}

static int allowed(void)
{
    return stdComm_bIsServer ? sithDuel_bAllowDuels : header.allowed;
}

static int available(const SithThing *p)
{
    return p && p->type == SITH_THING_PLAYER && p->sector
        && !(p->flags & (SITH_TF_DISABLED | SITH_TF_DESTROYED));
}

static int alive(const SithThing *p)
{
    return available(p) && !(p->flags & SITH_TF_DEAD)
        && (p != sithPlayer_g_pLocalPlayerThing || p->actorParams.health > 0.0);
}

static int everyoneCapable(void)
{
    for (int i = 0; slotValid(i); i++) {
        uint32_t id = seat(i);
        if (id && id != ownId() && capableId[i] != id)
            return 0;
    }
    return 1;
}

static int member(const DuelPair *p, uint32_t id)
{
    return id && (p->s.a == id || p->s.b == id);
}

// The pair id is in at minPhase or later, or -1.
static int pairOf(uint32_t id, enum DuelPhase minPhase)
{
    if (!id)
        return -1;
    for (int i = 0; i < DUEL_MAX_PAIRS; i++)
        if (pairs[i].s.phase >= minPhase && pairs[i].s.phase != DUEL_IDLE && member(&pairs[i], id))
            return i;
    return -1;
}

static void recount(void)
{
    activePairs = 0;
    for (int i = 0; i < DUEL_MAX_PAIRS; i++)
        if (pairs[i].s.phase >= DUEL_PREPARE)
            activePairs++;
}

int sithDuel_PhaseForPlayer(int idx)
{
    if (!activePairs || !room())
        return 0;
    uint32_t id = seat(idx);
    int i = pairOf(id, DUEL_PREPARE);
    if (i < 0)
        return 0;
    const DuelPair *p = &pairs[i];
    int bit = id == p->s.a ? 1 : 2;
    if (p->replacementMask & bit)
        return 0;
    if (localCancelPending && localCancelIndex == i && cancelFromPreparation && id == ownId())
        return 1;
    return p->s.phase - DUEL_OFFER;
}

int sithDuel_AreOpponents(int a, int b)
{
    if (a == b || !sithDuel_PhaseForPlayer(a) || !sithDuel_PhaseForPlayer(b))
        return 0;
    return pairOf(seat(a), DUEL_PREPARE) == pairOf(seat(b), DUEL_PREPARE);
}

void sithDuel_Reset(void)
{
    memset(pairs, 0, sizeof(pairs));
    for (int i = 0; i < DUEL_MAX_PAIRS; i++)
        pairs[i].s.index = (uint32_t)i;
    header = (DuelHeader){0};
    memset(capableId, 0, sizeof(capableId));
    lastSnapshotAt = 0;
    activePairs = 0;
    localCancelPending = 0;
    localCancelIndex = -1;
    cancelFromPreparation = 0;
    lastCancelAt = 0;
    askedAt = 0;
    asks = 0;
    memset(answeredAt, 0, sizeof(answeredAt));
    loggedNoRoom = 0;
}

// A separate message avoids overwriting an incoming DSS packet while replying.
static void sendFrame(int id, const uint8_t *bytes)
{
    SithMessage message = {0};
    message.netMsg.cogMsgId = SITHDUEL_MSG;
    message.netMsg.msg_size = DUEL_WIRE_SIZE;
    memcpy(message.pktData, bytes, DUEL_WIRE_SIZE);
    sithComm_SendMsgToPlayer(&message, id, 1, 0); // ordered browser data channel / websocket
}

static void sendHeader(uint32_t id)
{
    uint8_t bytes[DUEL_WIRE_SIZE];
    sithDuelState_WriteHeader(bytes, &header);
    sendFrame(id, bytes);
}

static void snapshot(uint32_t id, int i)
{
    uint8_t bytes[DUEL_WIRE_SIZE];
    sithDuelState_WriteSnapshot(bytes, &pairs[i].s);
    sendFrame(id, bytes);
}

static int recent(const DuelPair *p, uint32_t now)
{
    return p->s.phase != DUEL_IDLE || (p->changedAt && (uint32_t)(now - p->changedAt) < 10000);
}

// Everything a peer needs: the header, then every pair under way or just over.
static void tellAll(uint32_t id)
{
    uint32_t now = stdPlatform_GetTimeMsec();
    sendHeader(id);
    for (int i = 0; i < DUEL_MAX_PAIRS; i++)
        if (recent(&pairs[i], now))
            snapshot(id, i);
}

static void publishPair(int i)
{
    for (int slot = 0; slotValid(slot); slot++) {
        uint32_t id = seat(slot);
        if (id && id != ownId() && capableId[slot] == id)
            snapshot(id, i);
    }
}

static void publish(void)
{
    for (int slot = 0; slotValid(slot); slot++) {
        uint32_t id = seat(slot);
        if (id && id != ownId() && capableId[slot] == id)
            tellAll(id);
    }
    lastSnapshotAt = stdPlatform_GetTimeMsec();
}

// A peer without the host's header asks the host for it. The host learns
// which games can duel from their join requests and hellos, but a Dedicated
// Server host hears no hellos (its seat isn't in the game), and a game that
// never heard back would wait for good. Six asks, from entering the match
// and then 2, 4, 8, 16 and 32 s apart, so a host without duels costs six
// frames; /duel asks again.
static void askHost(uint32_t now)
{
    DuelRequest r = { DUEL_SYNC, 0, 0, 0, 0, DUEL_NO_PAIR };
    uint8_t bytes[DUEL_WIRE_SIZE];
    sithDuelState_WriteRequest(bytes, &r);
    sendFrame(DUEL_HOST_ID, bytes);
    askedAt = now;
    asks++;
}

static void label(uint32_t id, char *text, size_t size)
{
    int slot = findSeat(id);
    if (slot < 0) {
        snprintf(text, size, "player %u", (unsigned)id);
        return;
    }
    stdString_WcharToChar(text, jkPlayer_playerInfos[slot].player_name, size - 1);
    text[size - 1] = 0;
}

static void status(int i)
{
    const DuelState *s = &pairs[i].s;
    char a[40], b[40], line[180];
    label(s->a, a, sizeof(a));
    label(s->b, b, sizeof(b));
    if (s->phase == DUEL_OFFER && s->b == ownId())
        snprintf(line, sizeof(line), "%s challenges you. Aim back and press your duel button, or type /duel accept within five seconds. /duel decline refuses.", a);
    else if (s->phase == DUEL_OFFER && s->a == ownId())
        snprintf(line, sizeof(line), "Duel challenge sent to %s. Type /duel cancel to withdraw.", b);
    else if (s->phase >= DUEL_PREPARE)
        snprintf(line, sizeof(line), s->phase == DUEL_PREPARE ? "%s and %s: prepare to duel." : "%s and %s: fight!", a, b);
    else
        return;
    sithConsole_PrintString(line);
}

static void statusAll(void)
{
    int mine = 0, others = 0;
    for (int i = 0; i < DUEL_MAX_PAIRS; i++) {
        if (pairs[i].s.phase == DUEL_IDLE)
            continue;
        if (member(&pairs[i], ownId())) {
            status(i);
            mine++;
        } else if (pairs[i].s.phase >= DUEL_PREPARE) {
            others++;
        }
    }
    if (!mine)
        sithConsole_PrintString("You aren't in a private duel.");
    if (others) {
        char line[80];
        snprintf(line, sizeof(line), others == 1 ? "1 other private duel is under way." : "%d other private duels are under way.", others);
        sithConsole_PrintString(line);
    }
}

static void changed(int i, const DuelState *old)
{
    DuelPair *p = &pairs[i];
    const DuelState *s = &p->s;
    // A late joiner may receive an old result as its first snapshot. Only a
    // survivor who actually saw this fight can receive its reward.
    int wasMember = old->a == ownId() || old->b == ownId();
    int isMember = member(p, ownId());
    if (s->generation != old->generation) {
        p->deadMask = 0;
        p->replacementMask = 0;
        p->awaitingSecondDeath = 0;
        if (localCancelIndex == i) {
            localCancelPending = 0;
            cancelFromPreparation = 0;
            localCancelIndex = -1;
        }
    }
    recount();
    if (s->phase == DUEL_PREPARE && old->phase != DUEL_PREPARE) {
        int eligible = 1;
        if (isMember) {
            char reason[120];
            eligible = sithDuelCombat_CanEnter(sithPlayer_g_pLocalPlayerThing, reason, sizeof(reason));
            if (!eligible) sithConsole_PrintString(reason);
        }
        SithThing *a = body(s->a), *b = body(s->b);
        if (a) sithDuelCombat_Begin(a);
        if (b) sithDuelCombat_Begin(b);
        if (isMember) {
            if (stdComm_bIsServer) {
                if (eligible) p->readyMask |= s->a == ownId() ? 1 : 2;
            } else {
                sendRequest(eligible ? DUEL_READY : DUEL_CANCEL, 0, i);
            }
        }
    }
    if (s->phase != DUEL_IDLE) {
        if ((s->phase != DUEL_OFFER || isMember) && !(localCancelPending && localCancelIndex == i))
            status(i);
        return;
    }
    if (old->phase == DUEL_IDLE || old->generation != s->generation || old->epoch != s->epoch)
        return;
    if (s->reason == DUEL_DEFEAT) {
        char winner[40], line[100];
        label(s->winner, winner, sizeof(winner));
        snprintf(line, sizeof(line), "%s won the private duel.", winner);
        sithConsole_PrintString(line);
        SithThing *local = sithPlayer_g_pLocalPlayerThing;
        if (old->phase == DUEL_FIGHT && wasMember && s->winner == ownId()
            && old->a == s->a && old->b == s->b && p->rewardedGeneration != s->generation
            && !(localCancelPending && localCancelIndex == i)
            && !((p->deadMask | p->replacementMask) & (s->a == s->winner ? 1 : 2))
            && body(s->winner) == local && alive(local)) {
            p->rewardedGeneration = s->generation;
            sithDuelCombat_Reward(local);
        }
    } else if (wasMember || old->phase >= DUEL_PREPARE) {
        static const char *const reasons[] = { "", "The duel offer expired.", "Duel cancelled.", "Duel declined.",
            "Duel ended: a player left.", "Duel ended: the players separated.", "Duel ended: a player lacks duel support.",
            "", "Duel ended: a player is no longer eligible.", "Both duelists fell. The duel is a draw." };
        // Only the pair hears of an offer that came to nothing.
        if (old->phase >= DUEL_PREPARE || wasMember)
            sithConsole_PrintString(reasons[s->reason]);
    }
}

static void finishHost(int i, const DuelState *old)
{
    DuelPair *p = &pairs[i];
    if (old->revision != p->s.revision) {
        if (p->s.phase == DUEL_PREPARE && old->phase != DUEL_PREPARE) p->readyMask = 0;
        p->changedAt = stdPlatform_GetTimeMsec();
        if (!p->changedAt) p->changedAt = 1;
        changed(i, old);
        publishPair(i);
    }
}

static void endHost(int i, enum DuelReason reason)
{
    DuelState old = pairs[i].s;
    if (sithDuelState_End(&pairs[i].s, reason, 0))
        finishHost(i, &old);
}

static void endAll(enum DuelReason reason)
{
    for (int i = 0; i < DUEL_MAX_PAIRS; i++)
        if (pairs[i].s.phase != DUEL_IDLE)
            endHost(i, reason);
}

// A peer's own copy: it stops applying the pair's rules at once. Its revision
// stays, so a replay of the old state is still ignored.
static void endLocal(int i, enum DuelReason reason)
{
    pairs[i].s.phase = DUEL_IDLE;
    pairs[i].s.reason = reason;
    recount();
}

void sithDuel_Forget(int slot, uint32_t id)
{
    if (slot < 0 || slot >= JKPLAYER_NUM_INFOS || !id)
        return;
    capableId[slot] = 0;
    for (int i = 0; i < DUEL_MAX_PAIRS; i++) {
        if (pairs[i].s.phase == DUEL_IDLE || !member(&pairs[i], id))
            continue;
        // Refuse to apply restrictions to a replacement even if it gets the
        // same network ID.
        if (stdComm_bIsServer)
            endHost(i, DUEL_LEFT);
        else
            endLocal(i, DUEL_LEFT);
    }
}

void sithDuel_Joining(int slot, uint32_t id, int capable)
{
    if (!stdComm_bIsServer || stdComm_Imperial_bActive || !slotValid(slot))
        return;
    capableId[slot] = capable ? id : 0;
    if (!capable)
        endAll(DUEL_UNSUPPORTED);
}

void sithDuel_Welcome(int slot, uint32_t id)
{
    if (stdComm_bIsServer && header.epoch && slotValid(slot) && capableId[slot] == id && id)
        tellAll(id);
}

void sithDuel_Hello(int slot, uint32_t id, int capable)
{
    if (!room() || !id || seat(slot) != id)
        return;
    int first = capableId[slot] != id;
    capableId[slot] = capable ? id : 0;
    if (stdComm_bIsServer && header.epoch) {
        if (!capable) endAll(DUEL_UNSUPPORTED);
        else if (first) tellAll(id);
    }
}

static int nearAndVisible(SithThing *a, SithThing *b)
{
    return alive(a) && alive(b) && rdVector_Dist3(&a->position, &b->position) <= 2.0
        && sithCollision_HasLOS(a, b, 1);
}

// The host's rules for one pair, each frame.
static void judge(int i, uint32_t now)
{
    DuelPair *p = &pairs[i];
    DuelState *s = &p->s;
    DuelState old = *s;
    SithThing *a = body(s->a), *b = body(s->b);
    // A remote proxy's DEAD bit may arrive out of order or be set by
    // unrelated game state. Only its owner's explicit death is evidence.
    int aDead = (p->deadMask & 1) != 0;
    int bDead = (p->deadMask & 2) != 0;
    if (!everyoneCapable()) sithDuelState_End(s, DUEL_UNSUPPORTED, 0);
    else if (!available(a) || !available(b)) sithDuelState_End(s, DUEL_LEFT, 0);
    else if (aDead || bDead) {
        if (s->phase == DUEL_FIGHT && aDead != bDead) {
            SithThing *survivor = aDead ? b : a;
            // Allow a second owner's death to arrive if both bodies
            // appear dead. An unconfirmed result cannot freeze the room.
            if (p->replacementMask & (aDead ? 2 : 1))
                sithDuelState_End(s, DUEL_INELIGIBLE, 0);
            else if (alive(survivor))
                sithDuelState_End(s, DUEL_DEFEAT, aDead ? s->b : s->a);
            else if (!p->awaitingSecondDeath) {
                p->awaitingSecondDeath = 1;
                p->firstDeathAt = now;
            } else if ((uint32_t)(now - p->firstDeathAt) >= 750)
                sithDuelState_End(s, DUEL_INELIGIBLE, 0);
        } else
            sithDuelState_End(s, s->phase == DUEL_FIGHT ? DUEL_DRAW : DUEL_INELIGIBLE, 0);
    } else if (s->phase < DUEL_FIGHT && (!alive(a) || !alive(b)))
        sithDuelState_End(s, DUEL_INELIGIBLE, 0);
    else if (s->phase >= DUEL_PREPARE && rdVector_Dist3(&a->position, &b->position) >= 8.0)
        sithDuelState_End(s, DUEL_SEPARATED, 0);
    else if (s->phase == DUEL_PREPARE && member(p, ownId()) && sithDuel_PhaseForPlayer(playerThingIdx)) {
        char reason[120];
        if (!sithDuelCombat_CanEnter(sithPlayer_g_pLocalPlayerThing, reason, sizeof(reason))) {
            sithConsole_PrintString(reason);
            sithDuelState_End(s, DUEL_INELIGIBLE, 0);
        }
    }
    if (s->phase == DUEL_PREPARE && (int32_t)(now - s->deadline) >= 0 && p->readyMask != 3)
        sithDuelState_End(s, DUEL_INELIGIBLE, 0);
    else
        sithDuelState_Tick(s, now);
    finishHost(i, &old);
}

void sithDuel_Frame(void)
{
    if (!room()) {
        asks = 0;
        for (int i = 0; i < DUEL_MAX_PAIRS; i++) {
            if (pairs[i].s.phase == DUEL_IDLE)
                continue;
            if (stdComm_bIsServer) endHost(i, DUEL_INELIGIBLE);
            else endLocal(i, DUEL_INELIGIBLE);
        }
        return;
    }
    uint32_t now = stdPlatform_GetTimeMsec();
    if (!stdComm_bIsServer) {
        if (!header.epoch && asks < 6 && (!asks || (uint32_t)(now - askedAt) >= (1000u << asks)))
            askHost(now);
        // Owner-only inventory and powers can change between consent and
        // acceptance. Recheck during preparation before the host starts play.
        int mine = pairOf(ownId(), DUEL_PREPARE);
        if (mine >= 0 && pairs[mine].s.phase == DUEL_PREPARE && sithDuel_PhaseForPlayer(playerThingIdx)) {
            char reason[120];
            if (!localCancelPending && !sithDuelCombat_CanEnter(sithPlayer_g_pLocalPlayerThing, reason, sizeof(reason))) {
                sithConsole_PrintString(reason);
                sendRequest(DUEL_CANCEL, 0, mine);
            }
        }
        if (localCancelPending && localCancelIndex >= 0 && pairs[localCancelIndex].s.phase != DUEL_IDLE
            && (uint32_t)(now - lastCancelAt) >= 250)
            sendRequest(DUEL_CANCEL, 0, localCancelIndex);
        return;
    }
    if (!header.epoch) {
        if (!nextEpoch) nextEpoch = now;
        if (!++nextEpoch) ++nextEpoch;
        header.epoch = nextEpoch;
        header.revision = 1;
        header.allowed = sithDuel_bAllowDuels != 0;
        for (int i = 0; i < DUEL_MAX_PAIRS; i++) {
            pairs[i].s.index = (uint32_t)i;
            sithDuelState_Start(&pairs[i].s, header.epoch);
        }
        publish();
    }
    if (!sithDuel_bAllowDuels)
        endAll(DUEL_INELIGIBLE);
    if (header.allowed != (sithDuel_bAllowDuels != 0)) {
        header.allowed = sithDuel_bAllowDuels != 0;
        if (!++header.revision) header.revision = 1;
        publish();
    }
    for (int i = 0; i < DUEL_MAX_PAIRS; i++)
        if (pairs[i].s.phase != DUEL_IDLE)
            judge(i, now);
    if ((uint32_t)(now - lastSnapshotAt) >= 2500)
        publish();
}

void sithDuel_OwnerDeath(SithThing *player, uint32_t senderId)
{
    if (!player || player != body(senderId))
        return;
    for (int i = 0; i < DUEL_MAX_PAIRS; i++) {
        DuelPair *p = &pairs[i];
        if (p->s.phase == DUEL_IDLE) continue;
        if (senderId == p->s.a) p->deadMask |= 1;
        if (senderId == p->s.b) p->deadMask |= 2;
    }
}

void sithDuel_PlayerDied(SithThing *player)
{
    if (player == sithPlayer_g_pLocalPlayerThing)
        sithDuel_OwnerDeath(player, stdComm_dplayIdSelf);
}

void sithDuel_Respawn(SithThing *player)
{
    if (!player) return;
    for (int i = 0; i < DUEL_MAX_PAIRS; i++) {
        DuelPair *p = &pairs[i];
        if (p->s.phase == DUEL_IDLE) continue;
        int bit = player == body(p->s.a) ? 1 : player == body(p->s.b) ? 2 : 0;
        // Scored deaths, including a killed event and respawn in the same tick,
        // are resolved together at frame end. An unscored replacement just exits.
        if (!bit) continue;
        p->replacementMask |= bit;
        recount();
        if (!(p->deadMask & bit)) {
            if (stdComm_bIsServer)
                endHost(i, DUEL_INELIGIBLE);
            else if (player == sithPlayer_g_pLocalPlayerThing)
                sendRequest(DUEL_CANCEL, 0, i);
        }
    }
}

static void tell(uint32_t id, const char *text)
{
    if (id == ownId())
        sithConsole_PrintString(text);
    else
        sithMulti_SendChatTo(text, -1, -1, id);
}

static void tellBusy(uint32_t to, uint32_t id)
{
    char name[40], line[100];
    label(id, name, sizeof(name));
    snprintf(line, sizeof(line), "%s is already in a duel.", name);
    tell(to, line);
}

static int freePair(void)
{
    for (int i = 0; i < DUEL_MAX_PAIRS; i++)
        if (pairs[i].s.phase == DUEL_IDLE)
            return i;
    return -1;
}

// Accepting pair i: its two players' other offers, to and from them, end.
static void acceptPair(int i, uint32_t actor, uint32_t now)
{
    DuelState old = pairs[i].s;
    if (!sithDuelState_Accept(&pairs[i].s, actor, now))
        return;
    finishHost(i, &old);
    for (int j = 0; j < DUEL_MAX_PAIRS; j++)
        if (j != i && pairs[j].s.phase == DUEL_OFFER
            && (member(&pairs[j], pairs[i].s.a) || member(&pairs[j], pairs[i].s.b)))
            endHost(j, DUEL_CANCELLED);
}

static int joinPending(void)
{
    return sithNet_bNeedsFullThingSyncForLeaveJoin
        && (!slotValid(sithMulti_curWelcomePlayerNum) || capableId[sithMulti_curWelcomePlayerNum] != (uint32_t)sithMulti_newPlayerId);
}

static void hostRequest(uint32_t actor, const DuelRequest *r)
{
    int slot = findSeat(actor);
    if (!room() || !stdComm_bIsServer || slot < 0 || (actor != ownId() && capableId[slot] != actor))
        return;
    if (!sithDuel_bAllowDuels) {
        tell(actor, "This game's host hasn't turned on private duels.");
        return;
    }
    int idx = r->index < DUEL_MAX_PAIRS ? (int)r->index : -1;
    // A participant may revoke consent for the active generation even while
    // the host advances from preparation to fighting. End it before ticking.
    if (r->action == DUEL_CANCEL && idx >= 0 && pairs[idx].s.phase != DUEL_IDLE && header.epoch == r->epoch
        && pairs[idx].s.generation == r->generation && member(&pairs[idx], actor)) {
        endHost(idx, DUEL_CANCELLED);
        return;
    }
    sithDuel_Frame();
    if (r->epoch != header.epoch || (idx >= 0 && !sithDuelState_Matches(&pairs[idx].s, r))) {
        if (actor != ownId()) tellAll(actor);
        tell(actor, "That duel request has expired or the duel has changed.");
        return;
    }
    uint32_t now = stdPlatform_GetTimeMsec();
    DuelPair *p = idx >= 0 ? &pairs[idx] : NULL;
    switch (r->action) {
    case DUEL_READY:
        if (p->s.phase == DUEL_PREPARE) {
            if (actor == p->s.a) p->readyMask |= 1;
            if (actor == p->s.b) p->readyMask |= 2;
        }
        return;
    case DUEL_DECLINE:
        if (p->s.phase == DUEL_OFFER && actor == p->s.b)
            endHost(idx, DUEL_DECLINED);
        else
            tell(actor, "That duel action is unavailable.");
        return;
    case DUEL_CANCEL:
        if (member(p, actor) && p->s.phase != DUEL_IDLE)
            endHost(idx, DUEL_CANCELLED);
        else
            tell(actor, "That duel action is unavailable.");
        return;
    case DUEL_SYNC: // answered in sithDuel_Process
        return;
    case DUEL_CHALLENGE:
    case DUEL_ACCEPT:
        break;
    }
    if (!everyoneCapable() || joinPending()) {
        tell(actor, "Every active player must support private duels.");
        return;
    }
    if (pairOf(actor, DUEL_PREPARE) >= 0) {
        tell(actor, "You're already in a duel.");
        return;
    }
    if (r->action == DUEL_ACCEPT) {
        if (p->s.phase != DUEL_OFFER || p->s.b != actor) {
            tell(actor, "That duel action is unavailable.");
            return;
        }
        if (pairOf(p->s.a, DUEL_PREPARE) >= 0) {
            tellBusy(actor, p->s.a);
            return;
        }
        if (!nearAndVisible(body(actor), body(p->s.a))) {
            tell(actor, "Aim at a living player close by and in sight.");
            return;
        }
        acceptPair(idx, actor, now);
        return;
    }
    uint32_t target = r->target;
    if (actor == target || !nearAndVisible(body(actor), body(target))) {
        tell(actor, "Aim at a living player close by and in sight.");
        return;
    }
    if (pairOf(target, DUEL_PREPARE) >= 0) {
        tellBusy(actor, target);
        return;
    }
    // Aiming back at someone who challenged you accepts their challenge.
    for (int i = 0; i < DUEL_MAX_PAIRS; i++) {
        if (pairs[i].s.phase == DUEL_OFFER && pairs[i].s.a == target && pairs[i].s.b == actor
            && (int32_t)(now - pairs[i].s.deadline) < 0) {
            acceptPair(i, actor, now);
            return;
        }
    }
    // One challenge of your own at a time: a new one replaces the last.
    for (int i = 0; i < DUEL_MAX_PAIRS; i++) {
        if (pairs[i].s.phase != DUEL_OFFER || pairs[i].s.a != actor)
            continue;
        if (pairs[i].s.b == target) {
            tell(actor, "Your challenge is still waiting for an answer.");
            return;
        }
        endHost(i, DUEL_CANCELLED);
    }
    int f = freePair();
    if (f < 0) {
        tell(actor, "Too many duels at once. Try again shortly.");
        return;
    }
    DuelState old = pairs[f].s;
    if (sithDuelState_Offer(&pairs[f].s, actor, target, now))
        finishHost(f, &old);
    else
        tell(actor, "That duel action is unavailable.");
}

void sithDuel_Process(SithMessage *msg)
{
    if (msg->netMsg.msg_size != DUEL_WIRE_SIZE)
        return;
    if (!room()) {
        if (stdComm_bIsServer && !loggedNoRoom) {
            loggedNoRoom = 1;
            stdPlatform_Printf("OpenJKDF2: private duels: a player's game asked, but this game can't referee duels"
                               " (in the match %d, first seat %u, own id %u, free-for-all %d).\n",
                               stdComm_WSRelay_InMatch(), (unsigned)(jkPlayer_maxPlayers ? jkPlayer_playerInfos[0].playerNetId : 0),
                               (unsigned)ownId(), freeForAll());
        }
        return;
    }
    uint32_t sender = msg->netMsg.idx; // stamped by transport, never read from the request body
    const uint8_t *bytes = (const uint8_t *)msg->pktData;
    if (stdComm_bIsServer) {
        DuelRequest request;
        if (sender == DUEL_HOST_ID || !sithDuelState_ReadRequest(bytes, DUEL_WIRE_SIZE, &request))
            return;
        // Only a game that can duel sends these, so one the host didn't know
        // could (its join request said nothing, or a hello never came) is
        // one now, and is told everything.
        int slot = findSeat(sender);
        if (slot < 0)
            return;
        int learned = capableId[slot] != sender;
        if (learned) {
            stdPlatform_Printf("OpenJKDF2: private duels: player %u's game asked, and this host hadn't known it can duel.\n", (unsigned)sender);
            capableId[slot] = sender;
        }
        if (request.action == DUEL_SYNC) {
            uint32_t now = stdPlatform_GetTimeMsec();
            if (header.epoch && (learned || !answeredAt[slot] || (uint32_t)(now - answeredAt[slot]) >= 1000)) {
                answeredAt[slot] = now ? now : 1;
                tellAll(sender);
            }
            return;
        }
        if (learned && header.epoch)
            tellAll(sender);
        hostRequest(sender, &request);
        return;
    }
    uint32_t hadEpoch = header.epoch;
    if (sithDuelState_ReadHeader(&header, bytes, DUEL_WIRE_SIZE, sender)) {
        if (header.epoch != hadEpoch)
            stdPlatform_Printf("OpenJKDF2: private duels: the host's are %s.\n", header.allowed ? "on" : "off");
        return;
    }
    int i = sithDuelState_FrameIndex(bytes, DUEL_WIRE_SIZE);
    if (i < 0 || !header.epoch)
        return;
    DuelState old = pairs[i].s;
    if (!old.epoch) {
        // The header's session is this game's; a pair from another is refused.
        pairs[i].s.epoch = header.epoch;
        pairs[i].s.revision = 0;
        pairs[i].s.index = (uint32_t)i;
    }
    if (sithDuelState_ReadSnapshot(&pairs[i].s, bytes, DUEL_WIRE_SIZE, sender)) {
        pairs[i].changedAt = stdPlatform_GetTimeMsec();
        changed(i, &old);
    }
    else if (!old.epoch)
        pairs[i].s = old;
}

static void sendRequest(enum DuelAction action, uint32_t target, int index)
{
    DuelRequest r = { action, header.epoch, 0, 0, target, DUEL_NO_PAIR };
    if (index >= 0 && index < DUEL_MAX_PAIRS) {
        r.index = (uint32_t)index;
        r.revision = pairs[index].s.revision;
        r.generation = pairs[index].s.generation;
        if (action == DUEL_CANCEL && pairs[index].s.phase != DUEL_IDLE && member(&pairs[index], ownId())) {
            if (!localCancelPending || localCancelIndex != index)
                cancelFromPreparation = pairs[index].s.phase < DUEL_FIGHT;
            localCancelPending = 1;
            localCancelIndex = index;
            lastCancelAt = stdPlatform_GetTimeMsec();
        }
    }
    if (stdComm_bIsServer) {
        hostRequest(stdComm_dplayIdSelf, &r);
    } else {
        uint8_t bytes[DUEL_WIRE_SIZE];
        sithDuelState_WriteRequest(bytes, &r);
        sendFrame(DUEL_HOST_ID, bytes);
    }
}

static uint32_t aimedPlayer(SithThing *local)
{
    rdMatrix34 view = local->orient;
    rdMatrix_PreRotate34(&view, &local->actorParams.headPYR);
    rdVector3 eye = local->position;
    rdVector_Add3Acc(&eye, &local->actorParams.eyeOffset);
    SithSector *sector = sithCollision_FindSectorInRadius(local->sector, &local->position, &eye, 0.0);
    if (!sector) return 0;
    sithCollision_SearchForCollisions(sector, local, &eye, &view.lvec, 2.0, 0.025, RAYCAST_2);
    uint32_t id = 0;
    for (SithCollision *hit = sithCollision_PopStack(); hit; hit = sithCollision_PopStack()) {
        if (hit->type & SITHCOLLISION_THING) {
            for (int i = 0; slotValid(i); i++)
                if (seat(i) && jkPlayer_playerInfos[i].pLocalPlayer == hit->pThingCollided)
                    id = seat(i);
            break;
        }
        if (hit->type & SITHCOLLISION_WORLD) break;
    }
    sithCollision_DecreaseStackLevel();
    return id;
}

// The newest challenge to this player, or -1. Only the host has deadlines,
// so it's the one this game heard of last.
static int incoming(void)
{
    int best = -1;
    for (int i = 0; i < DUEL_MAX_PAIRS; i++)
        if (pairs[i].s.phase == DUEL_OFFER && pairs[i].s.b == ownId()
            && (best < 0 || (int32_t)(pairs[i].changedAt - pairs[best].changedAt) > 0))
            best = i;
    return best;
}

// This player's duel, or else their own challenge, or -1.
static int ownPair(void)
{
    int i = pairOf(ownId(), DUEL_PREPARE);
    if (i >= 0) return i;
    for (i = 0; i < DUEL_MAX_PAIRS; i++)
        if (pairs[i].s.phase == DUEL_OFFER && pairs[i].s.a == ownId())
            return i;
    return -1;
}

int sithDuel_Command(stdDebugConsoleCmd *cmd, const char *args)
{
    (void)cmd;
    if (!room()) {
        sithConsole_PrintString("Private duels require a peer-hosted browser free-for-all match.");
        return 1;
    }
    if (!args) args = "";
    while (isspace((unsigned char)*args)) args++;
    char word[24];
    size_t len = strlen(args);
    while (len && isspace((unsigned char)args[len-1])) len--;
    if (len >= sizeof(word)) goto usage;
    for (size_t i = 0; i < len; i++) word[i] = (char)tolower((unsigned char)args[i]);
    word[len] = 0;
    sithDuel_Frame();
    if (!header.epoch) {
        uint32_t now = stdPlatform_GetTimeMsec();
        if (!stdComm_bIsServer && (!asks || (uint32_t)(now - askedAt) >= 1000))
            askHost(now);
        sithConsole_PrintString("Waiting for a host with private-duel support.");
        return 1;
    }
    if (!allowed()) {
        sithConsole_PrintString("This game's host hasn't turned on private duels.");
        return 1;
    }
    if (!strcmp(word, "status")) { statusAll(); return 1; }
    if (!strcmp(word, "cancel")) {
        int i = ownPair();
        if (i < 0) sithConsole_PrintString("You have no challenge or duel to cancel.");
        else sendRequest(DUEL_CANCEL, 0, i);
        return 1;
    }
    if (!strcmp(word, "decline")) {
        int any = 0;
        for (int i = 0; i < DUEL_MAX_PAIRS; i++)
            if (pairs[i].s.phase == DUEL_OFFER && pairs[i].s.b == ownId()) {
                sendRequest(DUEL_DECLINE, 0, i);
                any = 1;
            }
        if (!any) sithConsole_PrintString("Nobody has challenged you.");
        return 1;
    }
    uint32_t target = 0;
    int index = -1;
    enum DuelAction action = DUEL_CHALLENGE;
    SithThing *local = sithPlayer_g_pLocalPlayerThing;
    if (!strcmp(word, "accept")) {
        action = DUEL_ACCEPT;
        index = incoming();
        if (index < 0) {
            sithConsole_PrintString("Nobody has challenged you.");
            return 1;
        }
    }
    else if (!len && alive(local))
        target = aimedPlayer(local);
    else if (isdigit((unsigned char)word[0])) {
        char *end;
        long n = strtol(word, &end, 10);
        if (*end || n < 0 || n >= JKPLAYER_NUM_INFOS) goto usage;
        target = seat((int)n);
    } else if (len) goto usage;
    if (action == DUEL_CHALLENGE && (!target || target == ownId())) {
        sithConsole_PrintString("Aim at another player or use /duel <slot> from the players list.");
        return 1;
    }
    char reason[120] = {0};
    if (!alive(local) || !sithDuelCombat_CanEnter(local, reason, sizeof(reason))) {
        sithConsole_PrintString(*reason ? reason : "You must be alive to duel.");
        return 1;
    }
    sendRequest(action, target, index);
    return 1;
usage:
    sithConsole_PrintString("Usage: duel [slot | accept | decline | cancel | status]");
    return 1;
}

int sithDuel_Chat(const char *line)
{
    if (!line || line[0] != '/') return 0;
    const char *word = "duel";
    for (int i = 0; i < 4; i++)
        if (tolower((unsigned char)line[i+1]) != word[i]) return 0;
    if (line[5] && !isspace((unsigned char)line[5])) return 0;
    return sithDuel_Command(NULL, line + 5);
}
#endif
