#ifndef SITH_DUEL_STATE_H
#define SITH_DUEL_STATE_H

#include <stddef.h>
#include <stdint.h>

enum DuelPhase { DUEL_IDLE, DUEL_OFFER, DUEL_PREPARE, DUEL_FIGHT };
enum DuelReason { DUEL_NONE, DUEL_EXPIRED, DUEL_CANCELLED, DUEL_DECLINED, DUEL_LEFT,
                  DUEL_SEPARATED, DUEL_UNSUPPORTED, DUEL_DEFEAT, DUEL_INELIGIBLE, DUEL_DRAW };
enum DuelAction { DUEL_CHALLENGE = 1, DUEL_ACCEPT, DUEL_DECLINE, DUEL_CANCEL, DUEL_READY };
enum { DUEL_WIRE_SIZE = 28, DUEL_HOST_ID = 1, DUEL_MAX_ID = 64 };

typedef struct DuelState {
    uint32_t epoch, revision, generation;
    uint32_t a, b, winner;
    enum DuelPhase phase;
    enum DuelReason reason;
    uint32_t deadline; // Host's clock only. Peers wait for the host's next phase.
} DuelState;

typedef struct DuelRequest {
    enum DuelAction action;
    uint32_t epoch, revision, generation, target;
} DuelRequest;

void sithDuelState_Start(DuelState *state, uint32_t epoch);
int sithDuelState_Offer(DuelState *state, uint32_t actor, uint32_t target, uint32_t now);
int sithDuelState_Accept(DuelState *state, uint32_t actor, uint32_t now);
int sithDuelState_Cancel(DuelState *state, uint32_t actor);
int sithDuelState_End(DuelState *state, enum DuelReason reason, uint32_t winner);
int sithDuelState_Tick(DuelState *state, uint32_t now);
int sithDuelState_Matches(const DuelState *state, const DuelRequest *request);
void sithDuelState_WriteRequest(uint8_t *packet, const DuelRequest *request);
int sithDuelState_ReadRequest(const uint8_t *packet, size_t size, DuelRequest *out);
void sithDuelState_WriteSnapshot(uint8_t *packet, const DuelState *state);
int sithDuelState_ReadSnapshot(DuelState *state, const uint8_t *packet, size_t size, uint32_t sender);
#endif
