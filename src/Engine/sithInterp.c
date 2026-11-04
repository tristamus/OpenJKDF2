#include "sithInterp.h"

#ifdef SITHINTERP_ENABLED

#include <string.h>
#include <math.h>
#include "stdPlatform.h"
#include "General/stdMath.h"
#include "Primitives/rdVector.h"
#include "Primitives/rdMatrix.h"
#include "World/sithWorld.h"
#include "Devices/sithControl.h"

int sithInterp_mode = SITHINTERP_MODE_DEFAULT;

uint32_t sithInterp_unexplainedSnaps = 0;
uint32_t sithInterp_drawnShifted = 0;
uint32_t sithInterp_framesArmed = 0;

// A thing's pose: rvec, lvec and uvec are the orient's rotation (orient.scale
// is the renderer's scratch and is never copied), sig its signature (0 for a
// free slot or no capture).
typedef struct sithInterpPose
{
    rdVector3 pos;
    rdVector3 rvec;
    rdVector3 lvec;
    rdVector3 uvec;
    uint32_t sig;
} sithInterpPose;

typedef struct sithInterpEntry
{
    sithInterpPose prev; // at the top of the frame's last step
    sithInterpPose cur;  // after it
    uint32_t flags;
} sithInterpEntry;

#define SITHINTERP_F_SNAPPED (1) // sithInterp_Snap ran during the last step
#define SITHINTERP_F_LOGGED  (2) // its unexplained jump was logged

// The most unexplained jumps logged a level; the counter keeps counting.
#define SITHINTERP_MAX_LOGGED (16)

// The engine's own threshold for a hard-set position (sithDSSThing_ProcessMoveToPos).
#define SITHINTERP_JUMP_DIST (0.5)

static sithInterpEntry* sithInterp_aEntries = NULL;
static int32_t sithInterp_numEntries = 0;
static SithWorld* sithInterp_pWorld = NULL;

// Entries at or above these have prev.sig / cur.sig 0.
static int32_t sithInterp_prevTop = 0;
static int32_t sithInterp_curTop = 0;

// Bumped by sithInterp_Reset; a prev/cur pair blends only when both were
// captured in the current epoch.
static uint32_t sithInterp_epoch = 1;
static uint32_t sithInterp_prevEpoch = 0;
static uint32_t sithInterp_curEpoch = 0;

static int sithInterp_bInLastStep = 0; // between CapturePrev and CaptureCur
static int sithInterp_bTickValid = 0;  // the last sithUpdate was stepped
static int sithInterp_bArmed = 0;
static flex_d_t sithInterp_alpha = 1.0;     // from the last stepped sithUpdate
static flex_d_t sithInterp_alphaDraw = 1.0; // for the armed draw
static flex_d_t sithInterp_capStepSecs = 0.0;
static int sithInterp_capStepRate = 0; // steps a second, compared instead of capStepSecs: no rounding question
static flex_t sithInterp_localKeyYaw = 0.0;
static uint32_t sithInterp_numLogged = 0;

void sithInterp_Reset()
{
    if (++sithInterp_epoch == 0)
        sithInterp_epoch = 1;
    sithInterp_prevEpoch = 0;
    sithInterp_curEpoch = 0;
    sithInterp_bInLastStep = 0;
    sithInterp_bTickValid = 0;
    sithInterp_bArmed = 0;
    sithInterp_numLogged = 0;
}

// The table follows the current world: one entry per thing slot.
static int sithInterp_EnsureTable()
{
    SithWorld* pWorld = sithWorld_g_pCurrentWorld;
    if (!pWorld || !pWorld->aThings || pWorld->numThingsLoaded <= 0)
        return 0;

    if (pWorld != sithInterp_pWorld || pWorld->numThingsLoaded > sithInterp_numEntries)
    {
        if (pWorld->numThingsLoaded > sithInterp_numEntries)
        {
            sithInterpEntry* pNew = (sithInterpEntry*)SITH_REALLOC(sithInterp_aEntries, sizeof(sithInterpEntry) * pWorld->numThingsLoaded);
            if (!pNew)
                return 0;
            sithInterp_aEntries = pNew;
            sithInterp_numEntries = pWorld->numThingsLoaded;
        }
        memset(sithInterp_aEntries, 0, sizeof(sithInterpEntry) * sithInterp_numEntries);
        sithInterp_prevTop = 0;
        sithInterp_curTop = 0;
        if (pWorld != sithInterp_pWorld)
            sithInterp_Reset();
        sithInterp_pWorld = pWorld;
    }
    return 1;
}

static void sithInterp_CapturePose(sithInterpPose* pPose, const SithThing* pThing)
{
    if (!pThing->type)
    {
        pPose->sig = 0;
        return;
    }
    pPose->pos = pThing->position;
    pPose->rvec = pThing->orient.rvec;
    pPose->lvec = pThing->orient.lvec;
    pPose->uvec = pThing->orient.uvec;
    pPose->sig = pThing->signature;
}

// How many slots a capture covers: sithThing_Update's 0..numThings.
static int32_t sithInterp_NumSlots()
{
    int32_t n = sithInterp_pWorld->numThings + 1;
    if (n < 0)
        n = 0;
    if (n > sithInterp_numEntries)
        n = sithInterp_numEntries;
    return n;
}

void sithInterp_CapturePrev()
{
    sithInterp_bInLastStep = 0;
    if (sithInterp_mode == SITHINTERP_MODE_OFF || !sithInterp_EnsureTable())
    {
        sithInterp_prevEpoch = 0;
        return;
    }

    int32_t n = sithInterp_NumSlots();
    SithThing* aThings = sithInterp_pWorld->aThings;
    for (int32_t i = 0; i < n; i++)
    {
        sithInterp_CapturePose(&sithInterp_aEntries[i].prev, &aThings[i]);
        sithInterp_aEntries[i].flags &= ~SITHINTERP_F_SNAPPED;
    }
    for (int32_t i = n; i < sithInterp_prevTop; i++)
    {
        sithInterp_aEntries[i].prev.sig = 0;
        sithInterp_aEntries[i].flags &= ~SITHINTERP_F_SNAPPED;
    }
    sithInterp_prevTop = n;
    sithInterp_prevEpoch = sithInterp_epoch;
    sithInterp_bInLastStep = 1;
}

void sithInterp_CaptureCur()
{
    sithInterp_bInLastStep = 0;
    if (sithInterp_mode == SITHINTERP_MODE_OFF || sithInterp_prevEpoch != sithInterp_epoch
        || sithWorld_g_pCurrentWorld != sithInterp_pWorld || !sithInterp_EnsureTable())
    {
        sithInterp_curEpoch = 0;
        return;
    }

    int32_t n = sithInterp_NumSlots();
    SithThing* aThings = sithInterp_pWorld->aThings;
    for (int32_t i = 0; i < n; i++)
        sithInterp_CapturePose(&sithInterp_aEntries[i].cur, &aThings[i]);
    for (int32_t i = n; i < sithInterp_curTop; i++)
        sithInterp_aEntries[i].cur.sig = 0;
    sithInterp_curTop = n;
    sithInterp_curEpoch = sithInterp_epoch;
    sithInterp_capStepSecs = (flex_d_t)DELTA_PHYSTICK_FPS;
    sithInterp_capStepRate = (int)TARGET_PHYSTICK_FPS;
    sithInterp_localKeyYaw = sithControl_GetKeyYawPerStep(sithPlayer_g_pLocalPlayerThing);
}

void sithInterp_FrameStepped(flex_d_t rollSteps, flex_d_t stepSecs)
{
    // With round() stepping the rollover is within half a step either way, so
    // the frame's time less half a step lies between the last step's start
    // (alpha 0) and its end (alpha 1).
    flex_d_t alpha = 0.5 + rollSteps;
    if (!(alpha >= 0.0)) // NaN too
        alpha = 0.0;
    if (alpha > 1.0)
        alpha = 1.0;
    sithInterp_alpha = alpha;
    sithInterp_bInLastStep = 0;
    sithInterp_bTickValid = (stepSecs == sithInterp_capStepSecs);
}

void sithInterp_FrameNotStepped()
{
    sithInterp_bInLastStep = 0;
    sithInterp_bTickValid = 0;
    // Things (and the turn keys) move on these frames without a capture, so
    // the pair from before them must not be blended after them.
    sithInterp_prevEpoch = 0;
    sithInterp_curEpoch = 0;
}

void sithInterp_Arm()
{
    sithInterp_bArmed = 0;
    if (sithInterp_bTickValid
        && (g_submodeFlags & 8) == 0
        && sithInterp_mode != SITHINTERP_MODE_OFF
        && sithInterp_prevEpoch == sithInterp_epoch
        && sithInterp_curEpoch == sithInterp_epoch
        && sithInterp_pWorld == sithWorld_g_pCurrentWorld
        && sithInterp_capStepRate == (int)TARGET_PHYSTICK_FPS)
    {
        sithInterp_bArmed = 1;
        sithInterp_alphaDraw = (sithInterp_mode == SITHINTERP_MODE_IDENTITY) ? 1.0 : sithInterp_alpha;
        sithInterp_framesArmed++;
    }
    sithInterp_bTickValid = 0;
}

void sithInterp_Disarm()
{
    sithInterp_bArmed = 0;
}

int sithInterp_IsArmed()
{
    return sithInterp_bArmed;
}

flex_t sithInterp_GetAlpha()
{
    return sithInterp_bArmed ? (flex_t)sithInterp_alphaDraw : (flex_t)1.0;
}

flex_t sithInterp_GetLocalKeyYaw()
{
    return sithInterp_bArmed ? sithInterp_localKeyYaw : (flex_t)0.0;
}

void sithInterp_Snap(SithThing* pThing)
{
    if (!pThing || !sithInterp_aEntries || !sithInterp_pWorld || sithWorld_g_pCurrentWorld != sithInterp_pWorld)
        return;
    int32_t idx = (int32_t)pThing->idx;
    if (idx < 0 || idx >= sithInterp_numEntries || &sithInterp_pWorld->aThings[idx] != pThing)
        return;

    sithInterpEntry* pEntry = &sithInterp_aEntries[idx];
    sithInterp_CapturePose(&pEntry->prev, pThing);
    if (sithInterp_bInLastStep)
        pEntry->flags |= SITHINTERP_F_SNAPPED;
    if (idx >= sithInterp_prevTop)
        sithInterp_prevTop = idx + 1;
}

static int sithInterp_VecsEqual(const rdVector3* a, const rdVector3* b, size_t num)
{
    return memcmp(a, b, sizeof(rdVector3) * num) == 0;
}

// The entry to blend pThing with, or NULL to draw it where it is (snap rules
// S1-S5 of the plan, in that order).
static sithInterpEntry* sithInterp_GetBlendEntry(SithThing* pThing)
{
    // S1: not armed (off, unstepped or loading tick, no tick before the
    // draw, step size or epoch changed).
    if (!sithInterp_bArmed || !pThing)
        return NULL;
    int32_t idx = (int32_t)pThing->idx;
    if (idx < 0 || idx >= sithInterp_numEntries || &sithInterp_pWorld->aThings[idx] != pThing)
        return NULL;
    sithInterpEntry* pEntry = &sithInterp_aEntries[idx];

    // S2: the same thing in both captures and now (not created, freed or its
    // slot reused in between).
    uint32_t sig = pThing->signature;
    if (!sig || pEntry->prev.sig != sig || pEntry->cur.sig != sig)
        return NULL;

    // S3: not moved outside the steps since (controls on a frame with no step,
    // network or cog writes after them, a cog run by the draw itself).
    if (!sithInterp_VecsEqual(&pThing->position, &pEntry->cur.pos, 1)
        || !sithInterp_VecsEqual(&pThing->orient.rvec, &pEntry->cur.rvec, 3))
        return NULL;

    // Not moving: nothing to blend.
    if (sithInterp_VecsEqual(&pEntry->prev.pos, &pEntry->cur.pos, 4))
        return NULL;

    // S5: a jump no step makes (warps without a snap hook, keyframe loops,
    // corrections from older builds and Classic).
    rdVector3 d;
    rdVector_Sub3(&d, &pEntry->cur.pos, &pEntry->prev.pos);
    flex_t limit = SITHINTERP_JUMP_DIST;
    if (pThing->moveType == SITH_MT_PHYSICS)
        limit += 2.0 * rdVector_Len3(&pThing->physicsParams.vel) * sithInterp_capStepSecs;
    if (rdVector_Len3(&d) > limit)
    {
        if (!(pEntry->flags & SITHINTERP_F_SNAPPED))
        {
            sithInterp_unexplainedSnaps++;
            if (!(pEntry->flags & SITHINTERP_F_LOGGED) && sithInterp_numLogged < SITHINTERP_MAX_LOGGED)
            {
                sithInterp_numLogged++;
                pEntry->flags |= SITHINTERP_F_LOGGED;
                stdPlatform_Printf("sithInterp: thing %d (type %d) jumped %f in a step with no snap\n",
                                   idx, (int)pThing->type, (double)rdVector_Len3(&d));
            }
        }
        return NULL;
    }
    return pEntry;
}

int sithInterp_GetRenderPos(SithThing* pThing, rdVector3* pOut)
{
    sithInterpEntry* pEntry = sithInterp_GetBlendEntry(pThing);
    if (!pEntry)
    {
        if (pThing)
            *pOut = pThing->position;
        return 0;
    }
    flex_t a = (flex_t)sithInterp_alphaDraw;
    pOut->x = pEntry->prev.pos.x + (pEntry->cur.pos.x - pEntry->prev.pos.x) * a;
    pOut->y = pEntry->prev.pos.y + (pEntry->cur.pos.y - pEntry->prev.pos.y) * a;
    pOut->z = pEntry->prev.pos.z + (pEntry->cur.pos.z - pEntry->prev.pos.z) * a;
    sithInterp_drawnShifted++;
    return 1;
}

int sithInterp_GetRenderPlacement(SithThing* pThing, rdMatrix34* pOut, int* pbTranslateOnly)
{
    *pbTranslateOnly = 0;
    sithInterpEntry* pEntry = sithInterp_GetBlendEntry(pThing);
    if (!pEntry)
        return 0;

    flex_t a = (flex_t)sithInterp_alphaDraw;
    rdVector3 pos;
    pos.x = pEntry->prev.pos.x + (pEntry->cur.pos.x - pEntry->prev.pos.x) * a;
    pos.y = pEntry->prev.pos.y + (pEntry->cur.pos.y - pEntry->prev.pos.y) * a;
    pos.z = pEntry->prev.pos.z + (pEntry->cur.pos.z - pEntry->prev.pos.z) * a;

    // Particles carry their size in the orient and sprites face the camera:
    // only their position moves.
    int bTranslateOnly = (pThing->type == SITH_THING_PARTICLE
                          || pThing->renderData.type == RD_THING_SPRITE3
                          || pThing->renderData.type == RD_THING_PARTICLE);

    if (bTranslateOnly || sithInterp_VecsEqual(&pEntry->prev.rvec, &pEntry->cur.rvec, 3)
        || rdVector_Dot3(&pEntry->prev.lvec, &pEntry->cur.lvec) < 0.0)
    {
        // S6: a turn of more than 90 degrees in a step (deflected or
        // ricocheting bolts, a blocked saber) isn't swept through.
        pOut->rvec = pEntry->cur.rvec;
        pOut->lvec = pEntry->cur.lvec;
        pOut->uvec = pEntry->cur.uvec;
    }
    else
    {
        pOut->rvec.x = pEntry->prev.rvec.x + (pEntry->cur.rvec.x - pEntry->prev.rvec.x) * a;
        pOut->rvec.y = pEntry->prev.rvec.y + (pEntry->cur.rvec.y - pEntry->prev.rvec.y) * a;
        pOut->rvec.z = pEntry->prev.rvec.z + (pEntry->cur.rvec.z - pEntry->prev.rvec.z) * a;
        pOut->lvec.x = pEntry->prev.lvec.x + (pEntry->cur.lvec.x - pEntry->prev.lvec.x) * a;
        pOut->lvec.y = pEntry->prev.lvec.y + (pEntry->cur.lvec.y - pEntry->prev.lvec.y) * a;
        pOut->lvec.z = pEntry->prev.lvec.z + (pEntry->cur.lvec.z - pEntry->prev.lvec.z) * a;
        pOut->uvec.x = pEntry->prev.uvec.x + (pEntry->cur.uvec.x - pEntry->prev.uvec.x) * a;
        pOut->uvec.y = pEntry->prev.uvec.y + (pEntry->cur.uvec.y - pEntry->prev.uvec.y) * a;
        pOut->uvec.z = pEntry->prev.uvec.z + (pEntry->cur.uvec.z - pEntry->prev.uvec.z) * a;
        rdMatrix_Normalize34(pOut);
    }
    pOut->scale = pos;
    *pbTranslateOnly = bTranslateOnly;
    sithInterp_drawnShifted++;
    return 1;
}

#endif // SITHINTERP_ENABLED
