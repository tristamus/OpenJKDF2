#include "sithDuelState.h"
#include <string.h>

// Version 2: several pairs at once (a pair index in every frame) and the
// host's header, which says whether its game allows duels at all.
enum { WIRE_VERSION = 2, WIRE_SNAPSHOT = 0x80, WIRE_HEADER = 0x81 };

static int validId(uint32_t id) { return id > 0 && id <= DUEL_MAX_ID; }
static uint32_t nextNumber(uint32_t n) { return n == UINT32_MAX ? 1 : n + 1; }
static int due(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }

void sithDuelState_Start(DuelState *s, uint32_t epoch)
{
    uint32_t index = s->index;
    *s = (DuelState){ .epoch = epoch, .revision = 1, .index = index };
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
    if (s->phase == DUEL_IDLE || s->phase == DUEL_FIGHT || !due(now, s->deadline))
        return 0;
    if (s->phase == DUEL_OFFER)
        return sithDuelState_End(s, DUEL_EXPIRED, 0);
    s->phase = DUEL_FIGHT;
    s->revision = nextNumber(s->revision);
    s->deadline = 0;
    return 1;
}

int sithDuelState_Matches(const DuelState *s, const DuelRequest *r)
{
    return s->epoch && s->epoch == r->epoch && s->index == r->index && s->revision == r->revision
        && s->generation == r->generation;
}

// Fixed 28-byte little-endian frames. Byte 0 is the protocol version and
// byte 1 tells a request's action from a pair's snapshot and the header.
//   request:  index u8 at 2 (DUEL_NO_PAIR for a challenge), 0 at 3, epoch,
//             revision, generation, target u32 at 4..19, zero 20..27
//   snapshot: phase u8, reason u8, epoch, revision, generation u32 at 4..15,
//             a, b, winner, index u8 at 16..19, zero 20..27
//   header:   flags u8 at 2 (1: duels allowed), 0 at 3, epoch, revision u32
//             at 4..11, zero 12..27
static void put32(uint8_t *p, uint32_t n)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (i * 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int zero(const uint8_t *p, size_t from)
{
    for (size_t i = from; i < DUEL_WIRE_SIZE; i++)
        if (p[i])
            return 0;
    return 1;
}

void sithDuelState_WriteRequest(uint8_t *p, const DuelRequest *r)
{
    memset(p, 0, DUEL_WIRE_SIZE);
    p[0] = WIRE_VERSION;
    p[1] = (uint8_t)r->action;
    p[2] = (uint8_t)(r->index < DUEL_MAX_PAIRS ? r->index : DUEL_NO_PAIR);
    put32(p + 4, r->epoch);
    put32(p + 8, r->revision);
    put32(p + 12, r->generation);
    put32(p + 16, r->target);
}

int sithDuelState_ReadRequest(const uint8_t *p, size_t n, DuelRequest *out)
{
    if (n != DUEL_WIRE_SIZE || p[0] != WIRE_VERSION || p[1] < DUEL_CHALLENGE || p[1] > DUEL_READY
        || p[3] || !zero(p, 20))
        return 0;
    DuelRequest r = {p[1], get32(p+4), get32(p+8), get32(p+12), get32(p+16), p[2]};
    if (!r.epoch)
        return 0;
    if (r.action == DUEL_CHALLENGE) {
        if (!validId(r.target) || r.index != DUEL_NO_PAIR)
            return 0;
    } else if (r.target || r.index >= DUEL_MAX_PAIRS || !r.revision || !r.generation) {
        return 0;
    }
    *out = r;
    return 1;
}

void sithDuelState_WriteSnapshot(uint8_t *p, const DuelState *s)
{
    memset(p, 0, DUEL_WIRE_SIZE);
    p[0] = WIRE_VERSION;
    p[1] = WIRE_SNAPSHOT;
    p[2] = (uint8_t)s->phase;
    p[3] = (uint8_t)s->reason;
    put32(p+4, s->epoch);
    put32(p+8, s->revision);
    put32(p+12, s->generation);
    p[16] = (uint8_t)s->a;
    p[17] = (uint8_t)s->b;
    p[18] = (uint8_t)s->winner;
    p[19] = (uint8_t)s->index;
}

int sithDuelState_FrameIndex(const uint8_t *p, size_t n)
{
    if (n != DUEL_WIRE_SIZE || p[0] != WIRE_VERSION || p[1] != WIRE_SNAPSHOT || p[19] >= DUEL_MAX_PAIRS)
        return -1;
    return p[19];
}

int sithDuelState_ReadSnapshot(DuelState *s, const uint8_t *p, size_t n, uint32_t sender)
{
    if (sender != DUEL_HOST_ID || sithDuelState_FrameIndex(p, n) < 0
        || p[2] > DUEL_FIGHT || p[3] > DUEL_DRAW || !zero(p, 20))
        return 0;
    DuelState v = { .phase = p[2], .reason = p[3], .epoch = get32(p+4), .revision = get32(p+8),
                    .generation = get32(p+12), .a = p[16], .b = p[17], .winner = p[18], .index = p[19] };
    if (!v.epoch || !v.revision)
        return 0;
    if (s->epoch && (v.epoch != s->epoch || v.index != s->index || (int32_t)(v.revision - s->revision) <= 0))
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

void sithDuelState_WriteHeader(uint8_t *p, const DuelHeader *h)
{
    memset(p, 0, DUEL_WIRE_SIZE);
    p[0] = WIRE_VERSION;
    p[1] = WIRE_HEADER;
    p[2] = h->allowed ? 1 : 0;
    put32(p+4, h->epoch);
    put32(p+8, h->revision);
}

int sithDuelState_ReadHeader(DuelHeader *h, const uint8_t *p, size_t n, uint32_t sender)
{
    if (sender != DUEL_HOST_ID || n != DUEL_WIRE_SIZE || p[0] != WIRE_VERSION || p[1] != WIRE_HEADER
        || p[2] > 1 || p[3] || !zero(p, 12))
        return 0;
    DuelHeader v = { .epoch = get32(p+4), .revision = get32(p+8), .allowed = p[2] };
    if (!v.epoch || !v.revision)
        return 0;
    // The same header comes again every few seconds; an older one never wins.
    if (h->epoch && (v.epoch != h->epoch || (int32_t)(v.revision - h->revision) < 0))
        return 0;
    *h = v;
    return 1;
}
