#include "Dss/sithMulti.h"
#include "Gameplay/sithDuelState.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Host 1, protected player's owner 2, unrelated player's owner 3. Thing
   GUIDs deliberately differ from sender IDs. The host is observing play. */
static SithWorld world;
static SithWorld *sithWorld_g_pCurrentWorld = &world;
static SithSector sector;
static SithSurface floorSurface;
static SithPlayer owner, otherOwner;
static SithThing body, outsider, platform, projectile;
static SithThing *sithPlayer_g_pLocalPlayerThing;
static int phase, motionReads, evidenceCount, killCount, expectEvidence;
int sithNet_isMulti = 1, sithNet_isServer = 1, Main_bMotsCompat;
static int g_submodeFlags;
int stdComm_WSRelay_bViewer;
static SithMessage sithComm_netMsgTmp, packet;
static int sithDuelCombat_Phase(const SithThing *p) { return p == &body ? phase : 0; }
static int sithDuelCombat_AllowsCollision(const SithThing *a, const SithThing *b)
{
    return !phase || !((a == &body && b == &outsider) || (b == &body && a == &outsider));
}
static void sithDuel_OwnerDeath(SithThing *p, uint32_t sender)
{
    assert(p == &body && sender == owner.playerNetId);
    evidenceCount++;
}
SithThing *sithThing_GetGuidThing(int guid)
{
    if (guid == body.guid) return &body;
    if (guid == outsider.guid) return &outsider;
    if (guid == platform.guid) return &platform;
    if (guid == projectile.guid) return &projectile;
    return NULL;
}
static SithSector *sithSector_GetPtrFromIdx(int id) { return id == sector.id ? &sector : NULL; }
static SithSurface *sithSurface_sub_4E63B0(int id) { return id == floorSurface.index ? &floorSurface : NULL; }
static void sithDSSThing_MoveToPos(SithThing *p, rdVector3 *pos, SithSector *s)
{
    p->position = *pos;
    p->sector = s;
}
static void sithActor_KillActor(SithThing *p, SithThing *killer, int cause)
{
    (void)killer; (void)cause;
    if (expectEvidence)
        assert(evidenceCount == killCount + 1); /* Record before the death callback's DEAD early-out. */
    killCount++;
    p->flags |= SITH_TF_DEAD;
}
static void sithComm_SendMsgToPlayer(SithMessage *p, int to, int streams, int reliable)
{
    (void)to; (void)streams; (void)reliable;
    packet = *p;
}
static int stdComm_WSRelay_TakeMotion(SithThing *p, SithMessage *msg)
{
    (void)p; (void)msg;
    motionReads++;
    return 0;
}
#define rdVector_Zero3(v) (*(v) = (rdVector3){0})
#define rdMatrix_ExtractAngles34(m, v) rdVector_Zero3(v)
#define rdMatrix_BuildRotate34(m, v) ((void)(m), (void)(v))
#define sithThing_SetSector(p, s, flags) ((p)->sector = (s))
#define sithThing_ValidateThingPointer(p) ((p) != NULL)
#define sithActor_SetHeadPYR(p, v) ((p)->actorParams.headPYR = *(v))
#define sithThing_DetachThing(p) ((p)->attach_flags = 0, (p)->attachedThing = NULL)
#define sithThing_AttachThingToThing(p, parent) ((p)->attachedThing = (parent))
#define sithThing_AttachThingToThingFace(p, parent, ...) sithThing_AttachThingToThing(p, parent)
#define sithThing_AttachThingToSurface(p, surface, flags) ((p)->attachedSurface = (surface))
#define sithThing_DestroyThing(p) ((p)->flags |= SITH_TF_DESTROYED)
#define sithPlayer_KillPlayer(p) sithActor_KillActor(p, p, 1)
#define sithInterp_Snap(...) ((void)0)
#define sithPuppet_SetNetVelocity(...) ((void)0)
#define sithPuppet_SetMoveMode(...) ((void)0)
#define sithPuppet_SetArmedMode(...) ((void)0)
#define sithPuppet_ResetTrack(...) ((void)0)
#define sithPlayer_debug_loadauto(...) ((void)0)
#define sithCog_ThingSendMessage(...) ((void)0)
#define stdComm_WSRelay_NotePos(...) ((void)0)
#define stdComm_WSRelay_SendOwnPos(...) 0

#include "handlers.inc"

static void deliver(int (*receive)(SithMessage *), unsigned sender)
{
    packet.netMsg.idx = sender;
    receive(&packet);
}

static void reset(void)
{
    phase = 2;
    sector.id = 7;
    floorSurface.index = 11;
    owner.playerNetId = 2;
    otherOwner.playerNetId = 3;
    body = (SithThing){ .type=SITH_THING_PLAYER, .guid=42, .sector=&sector, .moveType=SITH_MT_PHYSICS };
    outsider = (SithThing){ .type=SITH_THING_PLAYER, .guid=43, .sector=&sector };
    platform = (SithThing){ .type=SITH_THING_COG, .guid=44, .sector=&sector, .moveType=SITH_MT_PATH };
    projectile = (SithThing){ .type=SITH_THING_WEAPON, .guid=45, .pParent=&outsider };
    body.actorParams.pPlayer = &owner;
    outsider.actorParams.pPlayer = &otherOwner;
}

static void positionAndState(void)
{
    body.position.x = 47;
    body.physicsParams.vel.y = 12;
    sithDSSThing_Pos(&body, -1, 1);
    body.position.x = body.physicsParams.vel.y = 0;
    deliver(sithDSSThing_ProcessPos, 3);
    assert(body.position.x == 0 && body.physicsParams.vel.y == 0 && motionReads == 0);
    deliver(sithDSSThing_ProcessPos, 2);
    assert(body.position.x == 47 && body.physicsParams.vel.y == 12 && motionReads == 1);

    body.position.x = 0;
    deliver(sithDSSThing_ProcessPos, DUEL_HOST_ID);
    assert(body.position.x == 0); /* Live play. */
    g_submodeFlags = 8;
    deliver(sithDSSThing_ProcessPos, 3);
    assert(body.position.x == 0);
    deliver(sithDSSThing_ProcessPos, DUEL_HOST_ID);
    assert(body.position.x == 47); /* Host's joining snapshot. */
    g_submodeFlags = 0;
    body.position.x = 0;
    stdComm_WSRelay_bViewer = 1;
    deliver(sithDSSThing_ProcessPos, 3);
    assert(body.position.x == 0);
    deliver(sithDSSThing_ProcessPos, DUEL_HOST_ID);
    assert(body.position.x == 47); /* Host's spectator feed. */
    stdComm_WSRelay_bViewer = 0;

    body.flags = SITH_TF_DEAD;
    body.jkFlags = 19;
    body.position.x = 55;
    sithDSSThing_UpdateState(&body, -1, 1);
    body.flags = body.jkFlags = 0;
    body.position.x = 0;
    deliver(sithDSSThing_ProcessStateUpdate, 3);
    assert(body.flags == 0 && body.jkFlags == 0 && body.position.x == 0);
    deliver(sithDSSThing_ProcessStateUpdate, 2);
    assert((body.flags & SITH_TF_DEAD) && body.jkFlags == 19 && body.position.x == 55);
    assert(!evidenceCount && !killCount);
}

static void attachMessage(SithThing *child, SithThing *parent, unsigned sender)
{
    child->attachedThing = parent;
    child->attach_flags = SITH_ATTACH_THING;
    sithDSSThing_Attachment(child, -1, 1, 1);
    child->attachedThing = NULL;
    child->attach_flags = 0;
    deliver(sithDSSThing_ProcessAttachment, sender);
}
static void attachments(void)
{
    attachMessage(&projectile, &body, 3);
    assert(!projectile.attachedThing && !projectile.attach_flags);
    attachMessage(&outsider, &body, 3);
    assert(!outsider.attachedThing && !outsider.attach_flags);
    attachMessage(&body, &outsider, 2);
    assert(!body.attachedThing && !body.attach_flags);
    attachMessage(&body, &platform, 3);
    assert(!body.attachedThing && !body.attach_flags);
    attachMessage(&body, &platform, 2);
    assert(body.attachedThing == &platform && body.attach_flags == SITH_ATTACH_THING);
    body.attachedSurface = &floorSurface;
    body.attach_flags = SITH_ATTACH_SURFACE;
    sithDSSThing_Attachment(&body, -1, 1, 1);
    body.attachedSurface = NULL;
    body.attach_flags = 0;
    deliver(sithDSSThing_ProcessAttachment, 2);
    assert(body.attachedSurface == &floorSurface && body.attach_flags == SITH_ATTACH_SURFACE);
}
static void destroyAndDeath(void)
{
    sithDSSThing_DestroyThing(body.guid, -1);
    deliver(sithDSSThing_ProcessDestroyThing, 3);
    assert(!(body.flags & SITH_TF_DESTROYED));
    deliver(sithDSSThing_ProcessDestroyThing, 2);
    assert(body.flags & SITH_TF_DESTROYED);
    body.flags = SITH_TF_DEAD; /* Owner's state arrived before its reliable death. */
    expectEvidence = 1;
    sithDSSThing_Death(&body, &outsider, 0, -1, 1);
    deliver(sithDSSThing_ProcessDeath, 3);
    assert(!evidenceCount && !killCount);
    deliver(sithDSSThing_ProcessDeath, 2);
    assert(evidenceCount == 1 && killCount == 1);
    sithDSSThing_Death(&body, &body, 1, -1, 1);
    deliver(sithDSSThing_ProcessDeath, 2);
    assert(evidenceCount == 2 && killCount == 2);
}

int main(void)
{
    reset();
    positionAndState();
    reset();
    attachments();
    reset();
    destroyAndDeath();
    phase = 0;
    body.flags = 0;
    sithDSSThing_DestroyThing(body.guid, -1);
    deliver(sithDSSThing_ProcessDestroyThing, 3);
    assert(body.flags & SITH_TF_DESTROYED); /* The original rule outside a duel. */
    puts("Private duel thing-update authority passed.");
}
