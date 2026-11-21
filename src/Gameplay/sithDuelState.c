#include "sithDuelState.h"
#include <string.h>

enum { WIRE_VERSION = 1, WIRE_SNAPSHOT = 0x80 };

static int validId(uint32_t id) { return id > 0 && id <= DUEL_MAX_ID; }
static uint32_t nextNumber(uint32_t n) { return n == UINT32_MAX ? 1 : n + 1; }
static int due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

void sithDuelState_Start(DuelState *s, uint32_t epoch)
{
    *s = (DuelState){ .epoch = epoch, .revision = 1 };
}

int sithDuelState_Offer(DuelState *s, uint32_t actor, uint32_t target, uint32_t now)
{
    if (!s->epoch || s->phase != DUEL_IDLE || !validId(actor) || !validId(target) || actor == target)
        return 0;
    s->revision = nextNumber(s->revision);
    s->generation = s->revision;
    s->a = actor;
    s->b = target;
    s->winner = 0;
    s->reason = DUEL_NONE;
    s->phase = DUEL_OFFER;
    s->deadline = now + 5000;
    return 1;
}

int sithDuelState_Accept(DuelState *s, uint32_t actor, uint32_t now)
{
    if (s->phase != DUEL_OFFER || s->b != actor || due(now, s->deadline))
        return 0;
    s->phase = DUEL_PREPARE;
    s->revision = nextNumber(s->revision);
    s->deadline = now + 2000;
    return 1;
}

int sithDuelState_End(DuelState *s, enum DuelReason reason, uint32_t winner)
{
    if (s->phase == DUEL_IDLE || reason <= DUEL_NONE || reason > DUEL_DRAW)
        return 0;
    if (reason == DUEL_DRAW && s->phase != DUEL_FIGHT)
        return 0;
    if (reason == DUEL_DEFEAT) {
        if (s->phase != DUEL_FIGHT || (winner != s->a && winner != s->b))
            return 0;
    } else if (winner) {
        return 0;
    }
    s->revision = nextNumber(s->revision);
    s->phase = DUEL_IDLE;
    s->reason = reason;
    s->winner = winner;
    s->deadline = 0;
    return 1;
}

int sithDuelState_Cancel(DuelState *s, uint32_t actor)
{
    return (actor == s->a || actor == s->b) && sithDuelState_End(s, DUEL_CANCELLED, 0);
}

int sithDuelState_Tick(DuelState *s, uint32_t now)
{
    if (!due(now, s->deadline))
        return 0;
    if (s->phase == DUEL_OFFER)
        return sithDuelState_End(s, DUEL_EXPIRED, 0);
    if (s->phase != DUEL_PREPARE)
        return 0;
    s->phase = DUEL_FIGHT;
    s->revision = nextNumber(s->revision);
    s->deadline = 0;
    return 1;
}

int sithDuelState_Matches(const DuelState *s, const DuelRequest *r)
{
    return s->epoch && s->epoch == r->epoch && s->revision == r->revision && s->generation == r->generation;
}

// Fixed 28-byte little-endian frames. Byte 0 is the protocol version, byte 1
// distinguishes snapshots from actions, and request bytes 2, 3, 20..27 are zero.
static void put32(uint8_t *p, uint32_t n)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (i * 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void sithDuelState_WriteRequest(uint8_t *p, const DuelRequest *r)
{
    memset(p, 0, DUEL_WIRE_SIZE);
    p[0] = WIRE_VERSION;
    p[1] = (uint8_t)r->action;
    put32(p + 4, r->epoch);
    put32(p + 8, r->revision);
    put32(p + 12, r->generation);
    put32(p + 16, r->target);
}

int sithDuelState_ReadRequest(const uint8_t *p, size_t n, DuelRequest *out)
{
    if (n != DUEL_WIRE_SIZE || p[0] != WIRE_VERSION || p[1] < DUEL_CHALLENGE || p[1] > DUEL_READY
        || p[2] || p[3] || get32(p+20) || get32(p+24))
        return 0;
    DuelRequest r = {p[1], get32(p+4), get32(p+8), get32(p+12), get32(p+16)};
    if (!r.epoch || !r.revision || (r.action == DUEL_CHALLENGE ? !validId(r.target) : r.target != 0))
        return 0;
    *out = r;
    return 1;
}

void sithDuelState_WriteSnapshot(uint8_t *p, const DuelState *s)
{
    p[0] = WIRE_VERSION;
    p[1] = WIRE_SNAPSHOT;
    p[2] = (uint8_t)s->phase;
    p[3] = (uint8_t)s->reason;
    put32(p+4, s->epoch);
    put32(p+8, s->revision);
    put32(p+12, s->generation);
    put32(p+16, s->a);
    put32(p+20, s->b);
    put32(p+24, s->winner);
}

int sithDuelState_ReadSnapshot(DuelState *s, const uint8_t *p, size_t n, uint32_t sender)
{
    if (sender != DUEL_HOST_ID || n != DUEL_WIRE_SIZE || p[0] != WIRE_VERSION || p[1] != WIRE_SNAPSHOT
        || p[2] > DUEL_FIGHT || p[3] > DUEL_DRAW)
        return 0;
    DuelState v = { .phase = p[2], .reason = p[3], .epoch = get32(p+4), .revision = get32(p+8),
                    .generation = get32(p+12), .a = get32(p+16), .b = get32(p+20), .winner = get32(p+24) };
    if (!v.epoch || !v.revision || (s->epoch && (v.epoch != s->epoch || (int32_t)(v.revision - s->revision) <= 0)))
        return 0;
    if (v.phase == DUEL_IDLE && v.reason == DUEL_NONE) {
        if (v.a || v.b || v.winner || v.generation)
            return 0;
    } else {
        if (!v.generation || !validId(v.a) || !validId(v.b) || v.a == v.b
            || ((v.phase == DUEL_IDLE) != (v.reason != DUEL_NONE)))
            return 0;
        if (v.reason == DUEL_DEFEAT ? (v.winner != v.a && v.winner != v.b) : v.winner != 0)
            return 0;
    }
    *s = v;
    return 1;
}
