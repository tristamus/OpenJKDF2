#include "rdPuppet.h"

#include <math.h>

#include "General/stdMath.h"
#include "Engine/rdroid.h"
#include "Engine/rdThing.h"
#include "stdPlatform.h"
#include "jk.h"

// Un-inlined: Clear track node flags with bounds check (added in Grim Fandango).
static void rdPuppet_ClearTrackNodes(rdPuppet *puppet, int trackNum)
{
    if (puppet->renderData->model3->numHNodes < 0x40)
        _memset(puppet->aTracks[trackNum].aCurKfNodeEntryNums, 0, sizeof(uint32_t) * puppet->renderData->model3->numHNodes);
    else
        _memset(puppet->aTracks[trackNum].aCurKfNodeEntryNums, 0, sizeof(puppet->aTracks[trackNum].aCurKfNodeEntryNums));
}

// Added: the time a track plays a node at, its keyframe's second clock for
// the node types on it (rdKeyframe.upperType)
static flex_t rdPuppet_NodeFrame(const rdPuppetTrack *pTrack, const rdModel3 *pModel, int nodeNum)
{
    const rdKeyframe *pKeyframe = pTrack->keyframe;

    if (pKeyframe->upperNumFrames && nodeNum >= 0 && nodeNum < pModel->numHNodes
        && (pModel->aHierarchyNodes[nodeNum].type & pKeyframe->upperType))
        return pTrack->upperFrame;
    return pTrack->field_120;
}

// Added: a joint's turn as a unit quaternion (w, x, y, z)
static void rdPuppet_TurnFromAngles(flex_t q[4], const rdVector3 *pPyr)
{
    rdMatrix34 m;
    flex_t tr, s;

    rdMatrix_Build34(&m, pPyr, &rdroid_zeroVector3);
    // m.rvec, lvec and uvec are the columns
    tr = m.rvec.x + m.lvec.y + m.uvec.z;
    if (tr > 0.0)
    {
        s = stdMath_Sqrt(tr + 1.0) * 2.0;
        q[0] = 0.25 * s;
        q[1] = (m.lvec.z - m.uvec.y) / s;
        q[2] = (m.uvec.x - m.rvec.z) / s;
        q[3] = (m.rvec.y - m.lvec.x) / s;
    }
    else if (m.rvec.x > m.lvec.y && m.rvec.x > m.uvec.z)
    {
        s = stdMath_Sqrt(1.0 + m.rvec.x - m.lvec.y - m.uvec.z) * 2.0;
        q[0] = (m.lvec.z - m.uvec.y) / s;
        q[1] = 0.25 * s;
        q[2] = (m.lvec.x + m.rvec.y) / s;
        q[3] = (m.uvec.x + m.rvec.z) / s;
    }
    else if (m.lvec.y > m.uvec.z)
    {
        s = stdMath_Sqrt(1.0 + m.lvec.y - m.rvec.x - m.uvec.z) * 2.0;
        q[0] = (m.uvec.x - m.rvec.z) / s;
        q[1] = (m.lvec.x + m.rvec.y) / s;
        q[2] = 0.25 * s;
        q[3] = (m.uvec.y + m.lvec.z) / s;
    }
    else
    {
        s = stdMath_Sqrt(1.0 + m.uvec.z - m.rvec.x - m.lvec.y) * 2.0;
        q[0] = (m.rvec.y - m.lvec.x) / s;
        q[1] = (m.uvec.x + m.rvec.z) / s;
        q[2] = (m.uvec.y + m.lvec.z) / s;
        q[3] = 0.25 * s;
    }
}

// Added: acc += q * weight, q taken on the same side as acc (q and -q are the
// same turn)
static void rdPuppet_AddTurn(flex_t acc[4], const flex_t q[4], flex_t weight)
{
    flex_t d = acc[0] * q[0] + acc[1] * q[1] + acc[2] * q[2] + acc[3] * q[3];

    if (d < 0.0)
        weight = -weight;
    for (int i = 0; i < 4; i++)
        acc[i] += q[i] * weight;
}

// Added: out = the turn t of the way from a to b, along the least turn
static void rdPuppet_MixTurns(flex_t out[4], const flex_t a[4], const flex_t b[4], flex_t t)
{
    flex_t mix[4] = {0.0, 0.0, 0.0, 0.0}, len;

    rdPuppet_AddTurn(mix, a, 1.0 - t);
    rdPuppet_AddTurn(mix, b, t);
    len = stdMath_Sqrt(mix[0] * mix[0] + mix[1] * mix[1] + mix[2] * mix[2] + mix[3] * mix[3]);
    if (len <= 0.0)
    {
        for (int i = 0; i < 4; i++)
            out[i] = b[i];
        return;
    }
    for (int i = 0; i < 4; i++)
        out[i] = mix[i] / len;
}

// Added: a joint matrix from a turn and a position
static void rdPuppet_BuildFromTurn(rdMatrix34 *pOut, const flex_t q[4], const rdVector3 *pPos)
{
    flex_t w = q[0], x = q[1], y = q[2], z = q[3];

    pOut->rvec.x = 1.0 - 2.0 * (y * y + z * z);
    pOut->rvec.y = 2.0 * (x * y + z * w);
    pOut->rvec.z = 2.0 * (x * z - y * w);
    pOut->lvec.x = 2.0 * (x * y - z * w);
    pOut->lvec.y = 1.0 - 2.0 * (x * x + z * z);
    pOut->lvec.z = 2.0 * (y * z + x * w);
    pOut->uvec.x = 2.0 * (x * z + y * w);
    pOut->uvec.y = 2.0 * (y * z - x * w);
    pOut->uvec.z = 1.0 - 2.0 * (x * x + y * y);
    pOut->scale = *pPos;
}

// Added: moves a keyframe's second clock on by real time, round and round
static void rdPuppet_AdvanceUpperClock(rdPuppet *pPuppet, int track, flex_t secDeltaTime)
{
    rdPuppetTrack *pTrack = &pPuppet->aTracks[track];
    flex_t numFrames = (flex_t)pTrack->keyframe->upperNumFrames;

    pTrack->upperFrame += pTrack->keyframe->upperFps * secDeltaTime;
    if (pTrack->upperFrame >= numFrames)
    {
        while (pTrack->upperFrame >= numFrames)
            pTrack->upperFrame -= numFrames;
        rdPuppet_ClearTrackNodes(pPuppet, track);
    }
}

rdPuppet* rdPuppet_New(rdThing *pParent)
{
    rdPuppet* puppet = (rdPuppet *)RDROID_ALLOC(sizeof(rdPuppet));

    if (!puppet )
        return NULL;

    // Added: Moved this memset after the nullptr check
    _memset(puppet, 0, sizeof(rdPuppet));

    puppet->bPaused = 0;
    puppet->renderData = pParent;

    for (int i = 0; i < RDPUPPET_MAX_TRACKS; i++)
    {
        puppet->aTracks[i].field_120 = 0.0;
        puppet->aTracks[i].field_124 = 0.0;
        if ( puppet->aTracks[i].callback )
        {
            puppet->aTracks[i].callback(puppet->renderData->pThing, i, 0);
        }
        puppet->aTracks[i].field_4 = 0;
        puppet->aTracks[i].keyframe = NULL;
        puppet->aTracks[i].callback = NULL;
    }
    pParent->puppet = puppet;
    return puppet;
}

void rdPuppet_Free(rdPuppet *pPuppet)
{
    // Moved: no nullptr deref
    if (!pPuppet) return;

    // Added: prevent UAFs
    for (int i = 0; i < RDPUPPET_MAX_TRACKS; i++)
    {
        pPuppet->aTracks[i].field_4 = 0;
        pPuppet->aTracks[i].keyframe = NULL;
        pPuppet->aTracks[i].callback = NULL;
    }
    
    RDROID_FREE(pPuppet);
}

void rdPuppet_FreeEntry()
{
}

void rdPuppet_SetPause(rdPuppet *pPuppet, int bPaused)
{
    RD_ASSERTREL(pPuppet != NULL); // Added
    pPuppet->bPaused = bPaused;
}

void rdPuppet_SetTrackNoise(rdPuppet *pPuppet, int track, flex_t noise)
{
    RD_ASSERTREL((track >= 0) && (track < RDPUPPET_MAX_TRACKS)); // Added
    RD_ASSERTREL(pPuppet != NULL); // Added
    if ( noise != 0.0f )
    {
        pPuppet->aTracks[track].status |= 0x1000;
    }
    else
    {
        pPuppet->aTracks[track].status &= ~0x1000;
    }
    pPuppet->aTracks[track].field_120 = noise;
}

void rdPuppet_SetTrackPriority(rdPuppet *pPuppet, int track, int lowPri, int heighPri)
{
    RD_ASSERTREL(pPuppet != NULL); // Added
    RD_ASSERTREL((track >= 0) && (track < RDPUPPET_MAX_TRACKS)); // Added
    pPuppet->aTracks[track].lowPri = lowPri;
    pPuppet->aTracks[track].highPri = heighPri;
}

// MOTS altered
void rdPuppet_BuildJointMatrices(rdThing *prdThing, rdMatrix34 *pPlacement)
{
    rdPuppet *puppet; // eax
    rdPuppetTrack *v4; // ebx
    rdJoint *v8; // esi
    int v9; // edi
    uint32_t v10; // eax
    intptr_t v12; // ecx
    rdAnimEntry *v13;
    rdKeyframe *v17; // ebp
    int v18; // ebx
    unsigned int v19; // ecx
    rdJoint *v20; // edx
    uint32_t v21; // eax
    flex_d_t v23; // st7
    rdAnimEntry *v24; // eax
    int v25; // ecx
    flex_d_t v29; // st6
    flex_d_t v30; // st7
    flex_d_t v33; // st7
    flex_d_t v35; // rtt
    flex_d_t v36; // st4
    flex_t v42; // edx
    flex_d_t v45; // st7
    flex_d_t v46; // st7
    flex_d_t v48; // st5
    flex_d_t v50; // st7
    rdVector3 *v61; // ecx
    flex_t v70; // [esp+14h] [ebp-70h]
    flex_t v71; // [esp+18h] [ebp-6Ch]
    int v73; // [esp+1Ch] [ebp-68h]
    int v75; // [esp+20h] [ebp-64h]
    int v77; // [esp+24h] [ebp-60h]
    rdModel3 *model; // [esp+28h] [ebp-5Ch]
    int v80; // [esp+30h] [ebp-54h]
    int v82; // [esp+34h] [ebp-50h]
    rdVector3 a3; // [esp+3Ch] [ebp-48h] BYREF
    rdVector3 a4; // [esp+48h] [ebp-3Ch] BYREF
    flex_t v86; // [esp+54h] [ebp-30h]
    flex_t v87; // [esp+58h] [ebp-2Ch]
    flex_t v88; // [esp+5Ch] [ebp-28h]
    rdVector3 v89; // [esp+60h] [ebp-24h]
    rdVector3 v90; // [esp+6Ch] [ebp-18h]
    rdVector3 v91; // [esp+78h] [ebp-Ch]
    rdVector3 tmp1;

    model = prdThing->model3;
    puppet = prdThing->puppet;
    if ( prdThing->field_18 )
    {
        return;
    }

    // Added: Fix a crash?
    if (!prdThing->paJointMatrices) return;

    if ( !puppet || puppet->bPaused )
    {
        for (int i = 0; i < model->numHNodes; i++)
        {
            rdMatrix_Copy34(&prdThing->paJointMatrices[i], &model->aHierarchyNodes[i].posRotMatrix);
        }
        goto accumulate_finalize;
    }
    
    for (int i = 0; i < RDPUPPET_MAX_TRACKS; i++)
    {
        v4 = &puppet->aTracks[i];

        //if (thing->pThing == sithPlayer_g_pLocalPlayerThing && v4->keyframe)
        //    stdPlatform_Printf("%d %s (%x/%u) %p %x %f\n", i, v4->keyframe->name, v4->keyframe->id, v4->keyframe->id, v4->keyframe, v4->status, v4->playSpeed);

        // Added: aNodes check
        if (!(v4->status && v4->keyframe && v4->keyframe->aNodes)) {
            continue;
        }
        
        for (int j = 0; j < v4->keyframe->numJoints2; j++)
        {
            v8 = &v4->keyframe->aNodes[j];
            v9 = 0;
            if (!v8->numEntries) continue;

            // Added: this spot keeps crashing, add bounds checks
            if (v8->nodeNum < 0 || v8->nodeNum >= RDPUPPET_MAX_NODES)
            {
                v8->nodeNum = 0;
            }

            v10 = v4->aCurKfNodeEntryNums[v8->nodeNum];// nodeNum

            // Added: keep ASAN happy and prevent OOB accesses
            if (v10 >= v8->numEntries) {
                v10 = v8->numEntries - 1;
            }

            if ( v10 == v8->numEntries - 1 ) {
                continue;
            }

            v12 = v10 + 1;

            flex_t nodeFrame = rdPuppet_NodeFrame(v4, model, v8->nodeNum); // Added: was field_120

            // TODO: TODOA had an OOB access here
            if ( nodeFrame < (flex_d_t)v8->aEntries[v10 + 1].frameNum ) {
                continue;
            }

            v13 = &v8->aEntries[v10 + 2];
            do
            {
                if ( v12 == v8->numEntries - 1 )
                {
                    v9 = 1;
                }
                else if ( nodeFrame >= (flex_d_t)v13->frameNum )
                {
                    ++v12;
                    ++v13;
                }
                else
                {
                    v9 = 1;
                }
            }
            while ( !v9 );
            v4->aCurKfNodeEntryNums[j] = v12;
        }
    }

    // Added: with a key that blends by turns playing, the turns are blended
    // alongside the angles, each priority's the same way as its angles
    int bTurnBlend = 0;
    for (int j = 0; j < RDPUPPET_MAX_TRACKS; j++)
    {
        if ((puppet->aTracks[j].status & 2) && puppet->aTracks[j].keyframe && puppet->aTracks[j].keyframe->bTurnBlend)
            bTurnBlend = 1;
    }

    for (v80 = 0; v80 < model->numHNodes; v80++)
    {
        rdHierarchyNode* nodeIter = &model->aHierarchyNodes[v80];
        flex_t topTurn[4] = {0.0, 0.0, 0.0, 0.0}; // Added
        flex_t nextTurn[4] = {0.0, 0.0, 0.0, 0.0}; // Added
        flex_t trackTurn[4]; // Added
        v75 = 0;
        v73 = 0;
        v70 = 0.0;
        v71 = 0.0;
        rdVector_Zero3(&a4);
        rdVector_Zero3(&a3);
        rdVector_Zero3(&v90);
        rdVector_Zero3(&v91);
        for (int j = 0; j < RDPUPPET_MAX_TRACKS; j++)
        {
            rdPuppetTrack* trackIter = &puppet->aTracks[j];
            v17 = trackIter->keyframe;
            if (!v17) {
                continue;
            }
            // Added: lowmem nullptr checks
            if (!v17->aNodes) {
                continue;
            }
            
            v18 = (v17->type & nodeIter->type) ? trackIter->highPri : trackIter->lowPri;
            // Only blend aTracks that are actively playing (status bit 0x2)
            if (!(trackIter->status & 2)) {
                continue;
            }
            
            v19 = nodeIter->idx;
            v20 = &v17->aNodes[v19]; // overflow in orig? added (moved): v19 < v17->numJoints2, added v20->aEntries nullptr check
            if (!(v19 < v17->numJoints2 && v20->numEntries && v20->aEntries)) {
                continue;
            }
            
            if ( v18 >= v73 && (v18 >= v75 || v70 < 1.0) && v19 < v17->numJoints2 )
            {
                v21 = trackIter->aCurKfNodeEntryNums[v19];

                // Added: prevent overflow
                if (v21 >= v20->numEntries) {
                    v21 = v20->numEntries-1;
                }
                if (v21 < 0) {
                    v21 = 0;
                }

                v23 = rdPuppet_NodeFrame(trackIter, model, v19) - v20->aEntries[v21].frameNum; // Added: was field_120
                RD_ASSERTREL(v23 >= 0.0f); // Added
                v24 = &v20->aEntries[v21];
                v25 = v24->flags;
                if (v25 & 1)
                {
                    rdVector_Copy3(&v89, &v24->pos);
                    rdVector_ScaleAdd3Acc(&v89, &v24->vel, v23);
                }
                else
                {
                    rdVector_Copy3(&v89, &v24->pos);
                }
                if (v25 & 2)
                {
                    rdVector_Copy3(&tmp1, &v24->orientation);
                    rdVector_ScaleAdd3Acc(&tmp1, &v24->angularVelocity, v23);
                }
                else
                {
                    rdVector_Copy3(&tmp1, &v24->orientation);
                }
                if (bTurnBlend) // Added
                    rdPuppet_TurnFromAngles(trackTurn, &tmp1);
                rdVector_Sub3Acc(&v89, &nodeIter->pos);
                rdVector_Sub3Acc(&tmp1, &nodeIter->rot);
                rdVector_NormalizeAngleAcute3(&tmp1);
                if (trackIter->playSpeed <= 1.0)
                {
                    // Added: Make sure anims don't leak in blending
                    if (trackIter->playSpeed < 0.0)
                        trackIter->playSpeed = 0.0;

                    rdVector_Scale3Acc(&v89, trackIter->playSpeed);
                    rdVector_Scale3Acc(&tmp1, trackIter->playSpeed);
                }
                // Added: playSpeed was clamped to [0, 1] above when below 1
                flex_t turnWeight = trackIter->playSpeed;
                if (v18 == v75)
                {
                    rdVector_Add3Acc(&a4, &v89);
                    rdVector_Add3Acc(&a3, &tmp1);
                    v70 += trackIter->playSpeed;
                    if (bTurnBlend) // Added
                        rdPuppet_AddTurn(topTurn, trackTurn, turnWeight);
                }
                else if (v18 <= v75)
                {
                    if (v18 <= v73)
                    {
                        RD_ASSERTREL(v18 == v73); // Added
                        RD_ASSERTREL(v75 != v73); // Added
                        rdVector_Add3Acc(&v90, &v89);
                        rdVector_Add3Acc(&v91, &tmp1);
                        v71 += trackIter->playSpeed;
                        if (bTurnBlend) // Added
                            rdPuppet_AddTurn(nextTurn, trackTurn, turnWeight);
                    }
                    else
                    {
                        v90 = v89;
                        rdVector_Copy3(&v91, &tmp1);
                        v71 = trackIter->playSpeed;
                        v73 = v18;
                        if (bTurnBlend) // Added
                        {
                            for (int k = 0; k < 4; k++)
                                nextTurn[k] = trackTurn[k] * turnWeight;
                        }
                    }
                }
                else
                {
                    v90 = a4;
                    v91 = a3;
                    v71 = v70;
                    v73 = v75;
                    a4 = v89;
                    v75 = v18;
                    rdVector_Copy3(&a3, &tmp1);
                    v70 = trackIter->playSpeed;
                    if (bTurnBlend) // Added
                    {
                        for (int k = 0; k < 4; k++)
                        {
                            nextTurn[k] = topTurn[k];
                            topTurn[k] = trackTurn[k] * turnWeight;
                        }
                    }
                }
            }
        }


        if (v70 >= 1.0 || v71 <= 0.0)
        {
            if (v70 > 1.0) {
                v50 = 1.0 / v70;
                rdVector_Scale3Acc(&a4, v50);
                rdVector_Scale3Acc(&a3, v50);
            }
        }
        else
        {
            if (v71 > 1.0) {
                v45 = 1.0 / v71;
                rdVector_Scale3Acc(&v90, v45);
                rdVector_Scale3Acc(&v91, v45);
            }
            v46 = 1.0 - v70;
            rdVector_ScaleAdd3Acc(&a4, &v90, v46);
            rdVector_ScaleAdd3Acc(&a3, &v91, v46);
        }

        rdVector_NormalizeAngleAcute3(&a3);
        rdVector_Add3Acc(&a4, &nodeIter->pos);
        rdVector_Add3Acc(&a3, &nodeIter->rot);

        if (bTurnBlend && v70 > 0.0)
        {
            // Added: as above, by turns: the top priority's turns, less than
            // all the way from the next priority's (or the rest pose) when
            // its weights come to less than 1
            flex_t restTurn[4], fromTurn[4], len;
            rdPuppet_TurnFromAngles(restTurn, &nodeIter->rot);
            len = stdMath_Sqrt(topTurn[0] * topTurn[0] + topTurn[1] * topTurn[1] + topTurn[2] * topTurn[2] + topTurn[3] * topTurn[3]);
            for (int k = 0; k < 4; k++)
                topTurn[k] = len > 0.0 ? topTurn[k] / len : restTurn[k];
            if (v70 >= 1.0 || v71 <= 0.0)
            {
                rdPuppet_MixTurns(trackTurn, restTurn, topTurn, v70 > 1.0 ? 1.0 : v70);
            }
            else
            {
                len = stdMath_Sqrt(nextTurn[0] * nextTurn[0] + nextTurn[1] * nextTurn[1] + nextTurn[2] * nextTurn[2] + nextTurn[3] * nextTurn[3]);
                for (int k = 0; k < 4; k++)
                    nextTurn[k] = len > 0.0 ? nextTurn[k] / len : restTurn[k];
                rdPuppet_MixTurns(fromTurn, restTurn, nextTurn, v71 > 1.0 ? 1.0 : v71);
                rdPuppet_MixTurns(trackTurn, fromTurn, topTurn, v70);
            }
            rdPuppet_BuildFromTurn(&prdThing->paJointMatrices[v80], trackTurn, &a4);
        }
        else
        {
            rdMatrix_Build34(&prdThing->paJointMatrices[v80], &a3, &a4);
        }
        v61 = &prdThing->hierarchyNodes2[v80];
        if ( !rdVector_IsZero3(v61) )
            rdMatrix_PreRotate34(&prdThing->paJointMatrices[v80], &prdThing->hierarchyNodes2[v80]);
    }

accumulate_finalize:
    rdThing_AccumulateMatrices(prdThing, model->aHierarchyNodes, pPlacement);
    prdThing->rdFrameNum = rdroid_frameTrue;
}

int rdPuppet_RemoveTrack(rdPuppet *pPuppet, int track)
{
    RD_ASSERTREL((track >= 0) && (track < RDPUPPET_MAX_TRACKS)); // Added
    RD_ASSERTREL(pPuppet != NULL); // Added
    if ( pPuppet->aTracks[track].callback )
        pPuppet->aTracks[track].callback(pPuppet->renderData->pThing, track, 0);
    pPuppet->aTracks[track].status = 0;
    pPuppet->aTracks[track].keyframe = 0;
    pPuppet->aTracks[track].callback = 0;
    return 1;
}

// MOTS altered
int rdPuppet_UpdateTracks(rdPuppet *pPuppet, flex_t secDeltaTime)
{
    //return _rdPuppet_UpdateTracks(puppet, deltaSeconds);
    
    rdPuppetTrack *v3; // esi
    int v13; // [esp+14h] [ebp-4h]

    RD_ASSERTREL(pPuppet != NULL); // Added

    v13 = 0;
    if (pPuppet->bPaused)
        return 0;

    for (uint32_t v2 = 0; v2 < RDPUPPET_MAX_TRACKS; v2++)
    {
        rdPuppetTrack* track = &pPuppet->aTracks[v2];
        if (!track->status)
            continue;

        ++v13;
        if (track->status & 0x200) continue; // MOTS added

        if ( (track->status & 0x10) == 0 )
        {
            rdPuppet_AdvanceTrack(pPuppet, v2, track->speed * secDeltaTime);
        }

        // Added: a keyframe's second clock runs on time
        if (track->keyframe && track->keyframe->upperNumFrames)
            rdPuppet_AdvanceUpperClock(pPuppet, v2, secDeltaTime);

        if (track->status & 4)
        {
            track->playSpeed += track->fadeSpeed * secDeltaTime;
            if ( track->playSpeed >= 1.0 ) // verified
            {
                track->playSpeed = 1.0;
                track->status &= ~0x4;
            }
        }
        else if (track->status & 8)
        {
            track->playSpeed -= track->fadeSpeed * secDeltaTime;
            
            //if (puppet->renderData->pThing == sithPlayer_g_pLocalPlayerThing)
            //    stdPlatform_Printf("%u %f %f %f %f %u\n", v2, track->playSpeed, track->fadeSpeed, deltaSeconds, track->field_124, track->keyframe->numFrames);
            
            if ( track->playSpeed <= 0.0 ) // verified
            {
                if ( (track->status & 0x100) != 0 )
                {
                    track->status &= ~0x8u;
                    track->status |= 0x10;
                }
                else
                {
                    rdPuppet_RemoveTrack(pPuppet, v2);
                }
            }
        }
    }

    return v13;
}

int rdPuppet_AddTrack(rdPuppet *pPuppet, rdKeyframe *pKFTrack, int lowPriority, int highPriority)
{
    rdPuppetTrack *v4; // ecx
    int newTrackIdx; // esi
    rdPuppetTrack *v6; // eax
    rdPuppetTrack *newTrack; // edx

    RD_ASSERTREL(pPuppet != NULL); // Added
    RD_ASSERTREL(pKFTrack != NULL); // Added

    v4 = pPuppet->aTracks;
    for (newTrackIdx = 0; newTrackIdx < RDPUPPET_MAX_TRACKS; newTrackIdx++)
    {
        if ( !pPuppet->aTracks[newTrackIdx].status )
            break;
    }

    if ( newTrackIdx >= 4 )
    {
        newTrackIdx = 0;
        while ( (v4->status & 8) == 0 || (v4->status & 0x140) != 0 )
        {
            ++newTrackIdx;
            ++v4;
            if ( newTrackIdx >= 4 )
                return -1;
        }

        if ( newTrackIdx >= 4 )
            return -1;

        rdPuppet_RemoveTrack(pPuppet, newTrackIdx);
    }
    
    newTrack = &pPuppet->aTracks[newTrackIdx];
    newTrack->speed = pKFTrack->fps;
    newTrack->keyframe = pKFTrack;
    newTrack->highPri = highPriority;
    newTrack->lowPri = lowPriority;
    newTrack->status |= 1;
    newTrack->playSpeed = 0.0;

    rdPuppet_ResetTrack(pPuppet, newTrackIdx);
    
    return newTrackIdx;
}

void rdPuppet_SetCallback(rdPuppet *pPuppet, int track, rdPuppetTrackCallback_t pfCallback)
{
    RD_ASSERTREL(pPuppet != NULL); // Added
    RD_ASSERTREL((track >= 0) && (track < RDPUPPET_MAX_TRACKS)); // Added
    pPuppet->aTracks[track].callback = pfCallback;
}

int rdPuppet_FadeInTrack(rdPuppet *pPuppet, int track, flex_t speed)
{
    RD_ASSERTREL((track >= 0) && (track < RDPUPPET_MAX_TRACKS)); // Added
    RD_ASSERTREL(pPuppet != NULL); // Added
    pPuppet->aTracks[track].status = pPuppet->aTracks[track].status & ~8u | 6;
    if ( speed <= 0.0 )
    {
        pPuppet->aTracks[track].fadeSpeed = 1.0;
        return 1;
    }
    else
    {
        pPuppet->aTracks[track].fadeSpeed = 1.0 / speed;
        return 1;
    }
}

void rdPuppet_AdvanceTrack(rdPuppet *pPuppet, int track, flex_t frames)
{
    //_rdPuppet_AdvanceTrack(puppet, trackNum, a3);
    //return;
    
    rdKeyframe *v4; // ecx
    rdPuppetTrack *v5; // esi
    flex_d_t v6; // st7
    //unsigned int v11; // ebx
    rdKeyframe *v12; // ecx
    int v20; // [esp+14h] [ebp-8h]
    flex_t v21; // [esp+18h] [ebp-4h]
    flex_t v22; // [esp+2Ch] [ebp+10h]

    RD_ASSERTREL((track >= 0) && (track < RDPUPPET_MAX_TRACKS)); // Added
    RD_ASSERTREL(pPuppet != NULL); // Added
    RD_ASSERTREL(frames >= 0.0f); // Added

    v21 = 0.0;
    v20 = 0;
    v4 = pPuppet->aTracks[track].keyframe;
    v5 = &pPuppet->aTracks[track];
    if ( !v4 || frames == 0.0 )
        return;
    v22 = frames + pPuppet->aTracks[track].field_124;
    v6 = (flex_d_t)v4->numFrames;
    pPuppet->aTracks[track].field_120 = v22;

    if ( v22 >= v6 )
    {
        if (v5->status & 0x20)
        {
            pPuppet->aTracks[track].field_120 = v6;
            v20 = 1;
        }
        else if (v5->status & 0x40)
        {
            pPuppet->aTracks[track].field_120 = v6;
            v5->status |= 0x10;
        }
        else if ( v5->status & 0x80 )
        {
            pPuppet->aTracks[track].fadeSpeed = 4.0;
            pPuppet->aTracks[track].field_120 = v6;
            v5->status &= ~0x4;
            v5->status |= 0x8;
            v5->status |= 0x10;
        }
        else
        {
            v21 = stdMath_Floor(v22 / v6 + 0.5);
            size_t v11 = sizeof(uint32_t) * pPuppet->renderData->model3->numHNodes;
            pPuppet->aTracks[track].field_120 -= (flex_d_t)pPuppet->aTracks[track].keyframe->numFrames * v21;
            
            rdPuppet_ClearTrackNodes(pPuppet, track);
        }
        
    }
    if ( pPuppet->aTracks[track].callback )
    {
        if ( v4->numMarkers )
        {
            RD_ASSERTREL(pPuppet->renderData); // Added
            if ( v21 == 0.0 )
            {
                for (uint32_t v13 = 0; v13 < v4->numMarkers; v13++)
                {
                    if ( v4->markers.marker_float[v13] > (flex_d_t)pPuppet->aTracks[track].field_120 )
                        break;
                    if ( v4->markers.marker_float[v13] > (flex_d_t)pPuppet->aTracks[track].field_124 || pPuppet->aTracks[track].field_124 == 0.0 )
                    {
                        pPuppet->aTracks[track].callback(pPuppet->renderData->pThing, track, v4->markers.marker_int[v13]);
                    }
                }
            }
            else if ( v21 <= 1.0 )
            {
                for (uint32_t v17 = 0; v17 < v4->numMarkers; v17++)
                {
                    if ( v4->markers.marker_float[v17] > (flex_d_t)pPuppet->aTracks[track].field_124
                      || v4->markers.marker_float[v17] <= (flex_d_t)pPuppet->aTracks[track].field_120 )
                    {
                        pPuppet->aTracks[track].callback(pPuppet->renderData->pThing, track, v4->markers.marker_int[v17]);
                    }
                }
            }
            else
            {
                for (uint32_t v15 = 0; v15 < v4->numMarkers; v15++)
                {
                    pPuppet->aTracks[track].callback(pPuppet->renderData->pThing, track, v4->markers.marker_int[v15]);
                }
            }
        }
    }

    if ( v20 )
    {
        rdPuppet_RemoveTrack(pPuppet, track);
    }
    else
    {
        pPuppet->aTracks[track].field_124 = pPuppet->aTracks[track].field_120;
    }
}

int rdPuppet_FadeOutTrack(rdPuppet *pPuppet, int track, flex_t speed)
{
    RD_ASSERTREL((track >= 0) && (track < RDPUPPET_MAX_TRACKS)); // Added
    RD_ASSERTREL(pPuppet != NULL); // Added
    pPuppet->aTracks[track].status = pPuppet->aTracks[track].status & ~4u | 8;
    if ( speed <= 0.0 )
    {
        pPuppet->aTracks[track].fadeSpeed = 1.0;
        return 1;
    }
    else
    {
        pPuppet->aTracks[track].fadeSpeed = 1.0 / speed;
        return 1;
    }
}

void rdPuppet_SetTrackSpeed(rdPuppet *pPuppet, int track, flex_t fps)
{
    RD_ASSERTREL((track >= 0) && (track < RDPUPPET_MAX_TRACKS)); // Added
    RD_ASSERTREL(pPuppet != NULL); // Added
    pPuppet->aTracks[track].speed = fps;
}

int rdPuppet_SetStatus(rdPuppet *pPuppet, int track, int status)
{
    RD_ASSERTREL(pPuppet != NULL); // Added
    RD_ASSERTREL((track >= 0) && (track < RDPUPPET_MAX_TRACKS)); // Added
    pPuppet->aTracks[track].status |= status;
    return 1;
}

int rdPuppet_PlayTrack(rdPuppet *pPuppet, int track)
{
    rdPuppetTrack *v2; // eax

    RD_ASSERTREL((track >= 0) && (track < RDPUPPET_MAX_TRACKS)); // Added
    RD_ASSERTREL(pPuppet != NULL); // Added

    v2 = &pPuppet->aTracks[track];
    RD_ASSERTREL(v2 != NULL); // Added
    v2->status = v2->status & ~0x10u | 2;
    v2->playSpeed = 1.0;
    return 1;
}

void rdPuppet_ResetTrack(rdPuppet *pPuppet, int track)
{
    rdPuppetTrack *v2; // edx

    RD_ASSERT(pPuppet); // Added
    RD_ASSERTREL((track >= 0) && (track < RDPUPPET_MAX_TRACKS)); // Added

    v2 = &pPuppet->aTracks[track];

    rdPuppet_ClearTrackNodes(pPuppet, track);

    v2->field_120 = 0.0;
    v2->field_124 = 0.0;
    v2->upperFrame = 0.0; // Added
    v2->status = 3;
}

int rdPuppet_NewEntry(rdPuppet *pPuppet, rdThing *parent)
{
    RD_ASSERTREL(pPuppet != NULL); // Added
    pPuppet->bPaused = 0;
    pPuppet->renderData = parent;
    for (int i = 0; i < RDPUPPET_MAX_TRACKS; i++)
    {
        pPuppet->aTracks[i].field_120 = 0.0;
        pPuppet->aTracks[i].field_124 = 0.0;

        rdPuppet_RemoveTrack(pPuppet, i);
    }

    return 1;
}
