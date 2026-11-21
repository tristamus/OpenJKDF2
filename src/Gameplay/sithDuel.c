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

static DuelState state;
static uint32_t nextEpoch;
static uint32_t capableId[JKPLAYER_NUM_INFOS];
static uint32_t lastSnapshotAt;
static uint32_t rewardedGeneration;
static int deadMask; // death events survive a same-update respawn until the host resolves both players
static int replacementMask; // new lives never inherit the previous life’s restrictions or reward
static int localCancelPending;
static int cancelFromPreparation;
static int awaitingSecondDeath;
static uint32_t firstDeathAt;
static uint32_t lastCancelAt;
static int readyMask; // one bit per participant, acknowledged after checking owner-only inventory

static void sendRequest(enum DuelAction action, uint32_t target);

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

static int room(void)
{
    return !stdComm_Imperial_bActive && !sithMulti_IsSpectating() && stdComm_WSRelay_InMatch()
        && seat(0) == DUEL_HOST_ID && !(sithNet_MultiModeFlags & (MULTIMODEFLAG_TEAMS | MULTIMODEFLAG_COOP))
        && !(jkEpisode_mLoad.type & JK_EPISODE_SPECIAL_CTF);
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

int sithDuel_PhaseForPlayer(int idx)
{
    if (state.phase < DUEL_PREPARE || !room())
        return 0;
    uint32_t id = seat(idx);
    int bit = id == state.a ? 1 : id == state.b ? 2 : 0;
    if (!bit || (replacementMask & bit))
        return 0;
    if (localCancelPending && cancelFromPreparation && id == ownId())
        return 1;
    return state.phase - DUEL_OFFER;
}

int sithDuel_AreOpponents(int a, int b)
{
    return a != b && sithDuel_PhaseForPlayer(a) && sithDuel_PhaseForPlayer(b);
}

void sithDuel_Reset(void)
{
    state = (DuelState){0};
    memset(capableId, 0, sizeof(capableId));
    lastSnapshotAt = 0;
    rewardedGeneration = 0;
    readyMask = 0;
    deadMask = 0;
    replacementMask = 0;
    localCancelPending = 0;
    cancelFromPreparation = 0;
    awaitingSecondDeath = 0;
    lastCancelAt = 0;
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

static void snapshot(uint32_t id)
{
    uint8_t bytes[DUEL_WIRE_SIZE];
    sithDuelState_WriteSnapshot(bytes, &state);
    sendFrame(id, bytes);
}

static void publish(void)
{
    for (int i = 0; slotValid(i); i++) {
        uint32_t id = seat(i);
        if (id && id != ownId() && capableId[i] == id)
            snapshot(id);
    }
    lastSnapshotAt = stdPlatform_GetTimeMsec();
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

static void status(void)
{
    char a[40], b[40], line[180];
    label(state.a, a, sizeof(a));
    label(state.b, b, sizeof(b));
    if (state.phase == DUEL_OFFER && state.b == ownId())
        snprintf(line, sizeof(line), "%s challenges you. Type /duel accept or /duel decline within five seconds.", a);
    else if (state.phase == DUEL_OFFER && state.a == ownId())
        snprintf(line, sizeof(line), "Duel challenge sent to %s. Type /duel cancel to withdraw.", b);
    else if (state.phase >= DUEL_PREPARE)
        snprintf(line, sizeof(line), state.phase == DUEL_PREPARE ? "%s and %s: prepare to duel." : "%s and %s: fight!", a, b);
    else
        snprintf(line, sizeof(line), "No private duel is active.");
    sithConsole_PrintString(line);
}

static void changed(const DuelState *old)
{
    // A late joiner may receive an old result as its first snapshot. Only a
    // survivor who actually saw this fight can receive its reward.
    int wasMember = old->a == ownId() || old->b == ownId();
    int member = state.a == ownId() || state.b == ownId();
    if (state.generation != old->generation) {
        deadMask = 0;
        replacementMask = 0;
        localCancelPending = 0;
        cancelFromPreparation = 0;
        awaitingSecondDeath = 0;
    }
    if (state.phase == DUEL_PREPARE) {
        int eligible = 1;
        if (member) {
            char reason[120];
            eligible = sithDuelCombat_CanEnter(sithPlayer_g_pLocalPlayerThing, reason, sizeof(reason));
            if (!eligible) sithConsole_PrintString(reason);
        }
        SithThing *a = body(state.a), *b = body(state.b);
        if (a) sithDuelCombat_Begin(a);
        if (b) sithDuelCombat_Begin(b);
        if (member) {
            if (stdComm_bIsServer) {
                if (eligible) readyMask |= state.a == ownId() ? 1 : 2;
            } else {
                sendRequest(eligible ? DUEL_READY : DUEL_CANCEL, 0);
            }
        }
    }
    if (state.phase != DUEL_IDLE) {
        if ((state.phase != DUEL_OFFER || member) && !localCancelPending)
            status();
        return;
    }
    if (old->phase == DUEL_IDLE || old->generation != state.generation || old->epoch != state.epoch)
        return;
    if (state.reason == DUEL_DEFEAT) {
        char winner[40], line[100];
        label(state.winner, winner, sizeof(winner));
        snprintf(line, sizeof(line), "%s won the private duel.", winner);
        sithConsole_PrintString(line);
        SithThing *local = sithPlayer_g_pLocalPlayerThing;
        if (old->phase == DUEL_FIGHT && wasMember && state.winner == ownId()
            && old->a == state.a && old->b == state.b && rewardedGeneration != state.generation
            && !localCancelPending && !((deadMask | replacementMask) & (state.a == state.winner ? 1 : 2)) && body(state.winner) == local && alive(local)) {
            rewardedGeneration = state.generation;
            sithDuelCombat_Reward(local);
        }
    } else if (wasMember || old->phase >= DUEL_PREPARE) {
        static const char *const reasons[] = { "", "The duel offer expired.", "Duel cancelled.", "Duel declined.",
            "Duel ended: a player left.", "Duel ended: the players separated.", "Duel ended: a peer lacks duel support.",
            "", "Duel ended: a player is no longer eligible.", "Both duelists fell. The duel is a draw." };
        sithConsole_PrintString(reasons[state.reason]);
    }
}

static void finishHost(const DuelState *old)
{
    if (old->revision != state.revision) {
        if (state.phase == DUEL_PREPARE) readyMask = 0;
        changed(old);
        publish();
    }
}

static void endHost(enum DuelReason reason)
{
    DuelState old = state;
    if (sithDuelState_End(&state, reason, 0))
        finishHost(&old);
}

void sithDuel_Forget(int slot, uint32_t id)
{
    if (slot < 0 || slot >= JKPLAYER_NUM_INFOS || !id)
        return;
    capableId[slot] = 0;
    if (state.phase != DUEL_IDLE && (state.a == id || state.b == id)) {
        if (stdComm_bIsServer)
            endHost(DUEL_LEFT);
        else {
            // Refuse to apply restrictions to a replacement even if it gets
            // the same network ID. Preserve revision so a replay is ignored.
            state.phase = DUEL_IDLE;
            state.reason = DUEL_LEFT;
        }
    }
}

void sithDuel_Joining(int slot, uint32_t id, int capable)
{
    if (!stdComm_bIsServer || stdComm_Imperial_bActive || !slotValid(slot))
        return;
    capableId[slot] = capable ? id : 0;
    if (!capable)
        endHost(DUEL_UNSUPPORTED);
}

void sithDuel_Welcome(int slot, uint32_t id)
{
    if (stdComm_bIsServer && state.epoch && slotValid(slot) && capableId[slot] == id && id)
        snapshot(id);
}

void sithDuel_Hello(int slot, uint32_t id, int capable)
{
    if (!room() || !id || seat(slot) != id)
        return;
    int first = capableId[slot] != id;
    capableId[slot] = capable ? id : 0;
    if (stdComm_bIsServer && state.epoch) {
        if (!capable) endHost(DUEL_UNSUPPORTED);
        else if (first) snapshot(id);
    }
}

static int nearAndVisible(SithThing *a, SithThing *b)
{
    return alive(a) && alive(b) && rdVector_Dist3(&a->position, &b->position) <= 2.0
        && sithCollision_HasLOS(a, b, 1);
}

void sithDuel_Frame(void)
{
    if (!room()) {
        if (state.phase != DUEL_IDLE) {
            if (stdComm_bIsServer) endHost(DUEL_INELIGIBLE);
            else {
                state.phase = DUEL_IDLE;
                state.reason = DUEL_INELIGIBLE;
            }
        }
        return;
    }
    uint32_t now = stdPlatform_GetTimeMsec();
    if (!stdComm_bIsServer) {
        // Owner-only inventory and powers can change between consent and
        // acceptance. Recheck during preparation before the host starts play.
        if (state.phase == DUEL_PREPARE && sithDuel_PhaseForPlayer(playerThingIdx)) {
            char reason[120];
            if (!localCancelPending && !sithDuelCombat_CanEnter(sithPlayer_g_pLocalPlayerThing, reason, sizeof(reason))) {
                sithConsole_PrintString(reason);
                sendRequest(DUEL_CANCEL, 0);
            }
        }
        if (localCancelPending && state.phase != DUEL_IDLE && (uint32_t)(now - lastCancelAt) >= 250)
            sendRequest(DUEL_CANCEL, 0);
        return;
    }
    if (!state.epoch) {
        if (!nextEpoch) nextEpoch = now;
        if (!++nextEpoch) ++nextEpoch;
        sithDuelState_Start(&state, nextEpoch);
    }
    DuelState old = state;
    if (state.phase != DUEL_IDLE) {
        SithThing *a = body(state.a), *b = body(state.b);
        // A remote proxy's DEAD bit may arrive out of order or be set by
        // unrelated game state. Only its owner's explicit death is evidence.
        int aDead = (deadMask & 1) != 0;
        int bDead = (deadMask & 2) != 0;
        if (!everyoneCapable()) sithDuelState_End(&state, DUEL_UNSUPPORTED, 0);
        else if (!available(a) || !available(b)) sithDuelState_End(&state, DUEL_LEFT, 0);
        else if (aDead || bDead) {
            if (state.phase == DUEL_FIGHT && aDead != bDead) {
                SithThing *survivor = aDead ? b : a;
                // Allow a second owner's death to arrive if both bodies
                // appear dead. An unconfirmed result cannot freeze the room.
                if (replacementMask & (aDead ? 2 : 1))
                    sithDuelState_End(&state, DUEL_INELIGIBLE, 0);
                else if (alive(survivor))
                    sithDuelState_End(&state, DUEL_DEFEAT, aDead ? state.b : state.a);
                else if (!awaitingSecondDeath) {
                    awaitingSecondDeath = 1;
                    firstDeathAt = now;
                } else if ((uint32_t)(now - firstDeathAt) >= 750)
                    sithDuelState_End(&state, DUEL_INELIGIBLE, 0);
            } else
                sithDuelState_End(&state, state.phase == DUEL_FIGHT ? DUEL_DRAW : DUEL_INELIGIBLE, 0);
        } else if (state.phase < DUEL_FIGHT && (!alive(a) || !alive(b)))
            sithDuelState_End(&state, DUEL_INELIGIBLE, 0);
        else if (state.phase >= DUEL_PREPARE && rdVector_Dist3(&a->position, &b->position) >= 8.0)
            sithDuelState_End(&state, DUEL_SEPARATED, 0);
        else if (state.phase == DUEL_PREPARE && sithDuel_PhaseForPlayer(playerThingIdx)) {
            char reason[120];
            if (!sithDuelCombat_CanEnter(sithPlayer_g_pLocalPlayerThing, reason, sizeof(reason))) {
                sithConsole_PrintString(reason);
                sithDuelState_End(&state, DUEL_INELIGIBLE, 0);
            }
        }
        if (state.phase == DUEL_PREPARE && (int32_t)(now - state.deadline) >= 0 && readyMask != 3)
            sithDuelState_End(&state, DUEL_INELIGIBLE, 0);
        else
            sithDuelState_Tick(&state, now);
    }
    finishHost(&old);
    if ((uint32_t)(now - lastSnapshotAt) >= 2500)
        publish();
}

void sithDuel_OwnerDeath(SithThing *player, uint32_t senderId)
{
    if (state.phase == DUEL_IDLE || !player || player != body(senderId)) return;
    if (senderId == state.a) deadMask |= 1;
    if (senderId == state.b) deadMask |= 2;
}

void sithDuel_PlayerDied(SithThing *player)
{
    if (player == sithPlayer_g_pLocalPlayerThing)
        sithDuel_OwnerDeath(player, stdComm_dplayIdSelf);
}

void sithDuel_Respawn(SithThing *player)
{
    if (state.phase == DUEL_IDLE || !player) return;
    int bit = player == body(state.a) ? 1 : player == body(state.b) ? 2 : 0;
    // Scored deaths, including a killed event and respawn in the same tick,
    // are resolved together at frame end. An unscored replacement just exits.
    if (!bit) return;
    replacementMask |= bit;
    if (!(deadMask & bit)) {
        if (stdComm_bIsServer)
            endHost(DUEL_INELIGIBLE);
        else if (player == sithPlayer_g_pLocalPlayerThing) {
            sendRequest(DUEL_CANCEL, 0);
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

static void hostRequest(uint32_t actor, const DuelRequest *r)
{
    int slot = findSeat(actor);
    if (!room() || !stdComm_bIsServer || slot < 0 || (actor != ownId() && capableId[slot] != actor))
        return;
    // A participant may revoke consent for the active generation even while
    // the host advances from preparation to fighting. End it before ticking.
    if (r->action == DUEL_CANCEL && state.phase != DUEL_IDLE && state.epoch == r->epoch
        && state.generation == r->generation && (actor == state.a || actor == state.b)) {
        endHost(DUEL_CANCELLED);
        return;
    }
    sithDuel_Frame();
    if (!sithDuelState_Matches(&state, r)) {
        if (actor != ownId()) snapshot(actor);
        tell(actor, "That duel request has expired or the duel has changed.");
        return;
    }
    if (r->action == DUEL_READY) {
        if (state.phase == DUEL_PREPARE) {
            if (actor == state.a) readyMask |= 1;
            if (actor == state.b) readyMask |= 2;
        }
        return;
    }
    DuelState old = state;
    uint32_t now = stdPlatform_GetTimeMsec();
    int reciprocal = r->action == DUEL_CHALLENGE && state.phase == DUEL_OFFER && state.b == actor && state.a == r->target;
    if (r->action == DUEL_ACCEPT || r->action == DUEL_CHALLENGE) {
        if (!everyoneCapable() || (sithNet_bNeedsFullThingSyncForLeaveJoin
            && (!slotValid(sithMulti_curWelcomePlayerNum) || capableId[sithMulti_curWelcomePlayerNum] != (uint32_t)sithMulti_newPlayerId))) {
            tell(actor, "Every active player must support private duels.");
            return;
        }
        uint32_t target = r->action == DUEL_ACCEPT ? state.a : r->target;
        if (actor == target || !nearAndVisible(body(actor), body(target))) {
            tell(actor, "Choose another living player in sight within 2 JKU.");
            return;
        }
        if (r->action == DUEL_ACCEPT || reciprocal)
            sithDuelState_Accept(&state, actor, now);
        else
            sithDuelState_Offer(&state, actor, target, now);
    } else if (r->action == DUEL_DECLINE && state.phase == DUEL_OFFER && actor == state.b)
        sithDuelState_End(&state, DUEL_DECLINED, 0);
    else if (r->action == DUEL_CANCEL)
        sithDuelState_Cancel(&state, actor);
    if (old.revision == state.revision)
        tell(actor, "That duel action is unavailable.");
    else
        finishHost(&old);
}

void sithDuel_Process(SithMessage *msg)
{
    if (!room() || msg->netMsg.msg_size != DUEL_WIRE_SIZE)
        return;
    uint32_t sender = msg->netMsg.idx; // stamped by transport, never read from the request body
    const uint8_t *bytes = (const uint8_t *)msg->pktData;
    if (stdComm_bIsServer) {
        DuelRequest request;
        if (sender != DUEL_HOST_ID && sithDuelState_ReadRequest(bytes, DUEL_WIRE_SIZE, &request))
            hostRequest(sender, &request);
    } else {
        DuelState old = state;
        if (sithDuelState_ReadSnapshot(&state, bytes, DUEL_WIRE_SIZE, sender))
            changed(&old);
    }
}

static void sendRequest(enum DuelAction action, uint32_t target)
{
    DuelRequest r = { action, state.epoch, state.revision, state.generation, target };
    if (action == DUEL_CANCEL && state.phase != DUEL_IDLE
        && (state.a == ownId() || state.b == ownId())) {
        if (!localCancelPending)
            cancelFromPreparation = state.phase < DUEL_FIGHT;
        localCancelPending = 1;
        lastCancelAt = stdPlatform_GetTimeMsec();
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
    if (!state.epoch) {
        sithConsole_PrintString("Waiting for a host with private-duel support.");
        return 1;
    }
    if (!strcmp(word, "status")) { status(); return 1; }
    if (!strcmp(word, "cancel")) { sendRequest(DUEL_CANCEL, 0); return 1; }
    if (!strcmp(word, "decline")) { sendRequest(DUEL_DECLINE, 0); return 1; }
    uint32_t target = 0;
    enum DuelAction action = DUEL_CHALLENGE;
    SithThing *local = sithPlayer_g_pLocalPlayerThing;
    if (!strcmp(word, "accept"))
        action = DUEL_ACCEPT;
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
    sendRequest(action, target);
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
