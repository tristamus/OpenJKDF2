#include "sithCamera.h"

#include <string.h>

#include "World/sithSector.h"
#include "World/sithThing.h"
#include "World/sithWorld.h"
#include "World/jkPlayer.h"
#include "Engine/sithCollision.h"
#include "Devices/sithControl.h"
#include "Primitives/rdMath.h"
#include "Primitives/rdVector.h"
#include "Primitives/rdMatrix.h"
#include "Gameplay/sithTime.h"
#include "Engine/rdCamera.h"
#include "Engine/sithRender.h"
#include "General/stdMath.h"
#include "Dw/dwCamera.h"
#include "jk.h"
#include "Engine/sithInterp.h"

static rdVector3 sithCamera_trans = {0.0, 0.3, 0.0};
static rdVector3 sithCamera_trans2 = {0.0, 0.2, 0.0};
static rdVector3 sithCamera_trans3 = {0.0, 1.0, 1.0};
static int sithCamera_camIdxToGlobalIdx[2] = {0,1};

int sithCamera_Startup()
{
    SITH_ASSERTREL(sithCamera_bStartup == 0); // Added: J3D assert

    sithCamera_NewEntry(&sithCamera_g_aCameras[0], 0, 0x1, SITHCAMERA_FOV, SITHCAMERA_ASPECT, NULL, NULL, NULL);
    sithCamera_NewEntry(&sithCamera_g_aCameras[1], 0, 0x4, SITHCAMERA_FOV, SITHCAMERA_ASPECT, NULL, NULL, NULL);
    sithCamera_g_aCameras[1].offset.x = 0.0;
    sithCamera_g_aCameras[1].offset.y = -0.2;
    sithCamera_g_aCameras[1].offset.z = 0.06;
    sithCamera_NewEntry(&sithCamera_g_aCameras[2], 0, 0x8, SITHCAMERA_FOV, SITHCAMERA_ASPECT, NULL, NULL, NULL);
    sithCamera_NewEntry(&sithCamera_g_aCameras[4], 0, 0x20, SITHCAMERA_FOV, SITHCAMERA_ASPECT, NULL, NULL, NULL);
    sithCamera_NewEntry(&sithCamera_g_aCameras[5], 0, 0x40, SITHCAMERA_FOV, SITHCAMERA_ASPECT, NULL, NULL, NULL);
    sithCamera_NewEntry(&sithCamera_g_aCameras[6], 0, 0x80, SITHCAMERA_FOV, SITHCAMERA_ASPECT, NULL, NULL, NULL);
#ifdef DW_CAMERA
    if (Main_bDwCompat) {
        sithCamera_NewEntry(&sithCamera_g_aCameras[7], 0, 0x100, SITHCAMERA_FOV, SITHCAMERA_ASPECT, NULL, NULL, NULL);
    }
#endif
    sithCamera_g_curCycleCamNum = 0;
    sithCamera_bStartup = 1;

    return 1;
}

void sithCamera_Shutdown()
{
    sithCamera_Close(); // Added--moved the rdCamera_FreeEntries where they belong

    // Added: Clean reset
#ifdef DW_CAMERA
    memset(sithCamera_g_aCameras, 0, sizeof(SithCamera) * 8);
#else
    memset(sithCamera_g_aCameras, 0, sizeof(SithCamera) * 7);
#endif

    sithCamera_bStartup = 0;
}

int sithCamera_Open(rdCanvas *pCanvas, flex_t aspect)
{
    SITH_ASSERTREL(sithCamera_bStartup == 1); // Added: J3D assert

    if ( sithCamera_bOpen )
        return 0;

    sithCamera_g_aCameras[0].aspectRatio = aspect;
    rdCamera_NewEntry(&sithCamera_g_aCameras[0].rdCamera, sithCamera_g_aCameras[0].rdCamera.fov, 0, SITHCAMERA_ZNEAR_FIRSTPERSON, SITHCAMERA_ZFAR, aspect);
    rdCamera_SetAttenuation(&sithCamera_g_aCameras[0].rdCamera, SITHCAMERA_ATTENUATION_MIN, SITHCAMERA_ATTENUATION_MAX);
    rdCamera_SetCanvas(&sithCamera_g_aCameras[0].rdCamera, pCanvas);
    sithCamera_g_aCameras[1].aspectRatio = aspect;
    rdCamera_NewEntry(&sithCamera_g_aCameras[1].rdCamera, sithCamera_g_aCameras[1].rdCamera.fov, 0, SITHCAMERA_ZNEAR, SITHCAMERA_ZFAR, aspect);
    rdCamera_SetAttenuation(&sithCamera_g_aCameras[1].rdCamera, SITHCAMERA_ATTENUATION_MIN, SITHCAMERA_ATTENUATION_MAX);
    rdCamera_SetCanvas(&sithCamera_g_aCameras[1].rdCamera, pCanvas);
    sithCamera_g_aCameras[2].aspectRatio = aspect;
    rdCamera_NewEntry(&sithCamera_g_aCameras[2].rdCamera, sithCamera_g_aCameras[2].rdCamera.fov, 0, SITHCAMERA_ZNEAR, SITHCAMERA_ZFAR, aspect);
    rdCamera_SetAttenuation(&sithCamera_g_aCameras[2].rdCamera, SITHCAMERA_ATTENUATION_MIN, SITHCAMERA_ATTENUATION_MAX);
    rdCamera_SetCanvas(&sithCamera_g_aCameras[2].rdCamera, pCanvas);
    sithCamera_g_aCameras[4].aspectRatio = aspect;
    rdCamera_NewEntry(&sithCamera_g_aCameras[4].rdCamera, sithCamera_g_aCameras[4].rdCamera.fov, 0, SITHCAMERA_ZNEAR, SITHCAMERA_ZFAR, aspect);
    rdCamera_SetAttenuation(&sithCamera_g_aCameras[4].rdCamera, SITHCAMERA_ATTENUATION_MIN, SITHCAMERA_ATTENUATION_MAX);
    rdCamera_SetCanvas(&sithCamera_g_aCameras[4].rdCamera, pCanvas);
    sithCamera_g_aCameras[5].aspectRatio = aspect;
    rdCamera_NewEntry(&sithCamera_g_aCameras[5].rdCamera, sithCamera_g_aCameras[5].rdCamera.fov, 0, SITHCAMERA_ZNEAR, SITHCAMERA_ZFAR, aspect);
    rdCamera_SetAttenuation(&sithCamera_g_aCameras[5].rdCamera, SITHCAMERA_ATTENUATION_MIN, SITHCAMERA_ATTENUATION_MAX);
    rdCamera_SetCanvas(&sithCamera_g_aCameras[5].rdCamera, pCanvas);
    sithCamera_g_aCameras[6].aspectRatio = aspect;
    rdCamera_NewEntry(&sithCamera_g_aCameras[6].rdCamera, sithCamera_g_aCameras[6].rdCamera.fov, 0, SITHCAMERA_ZNEAR, SITHCAMERA_ZFAR, aspect);
    rdCamera_SetAttenuation(&sithCamera_g_aCameras[6].rdCamera, SITHCAMERA_ATTENUATION_MIN, SITHCAMERA_ATTENUATION_MAX);
    rdCamera_SetCanvas(&sithCamera_g_aCameras[6].rdCamera, pCanvas);
#ifdef DW_CAMERA
    if (Main_bDwCompat) {
        sithCamera_g_aCameras[7].aspectRatio = aspect;
        rdCamera_NewEntry(&sithCamera_g_aCameras[7].rdCamera, sithCamera_g_aCameras[7].rdCamera.fov, 0.0, SITHCAMERA_ZNEAR, SITHCAMERA_ZFAR, aspect);
        rdCamera_SetAttenuation(&sithCamera_g_aCameras[7].rdCamera, SITHCAMERA_ATTENUATION_MIN, SITHCAMERA_ATTENUATION_MAX);
        rdCamera_SetCanvas(&sithCamera_g_aCameras[7].rdCamera, pCanvas);
    }
#endif // DW_CAMERA
    sithCamera_Update(sithCamera_g_pCurCamera);
    sithCamera_ResetRenderView(sithCamera_g_pCurCamera); // Added: nothing left from a closed level
    sithCamera_bOpen = 1;
    return 1;
}

void sithCamera_Close()
{
    if ( sithCamera_bOpen ) {
        sithCamera_bOpen = 0;
        sithCamera_ResetRenderView(NULL); // Added: it points into the world

        // Added: Prevent UAF
        rdCamera_SetCanvas(&sithCamera_g_aCameras[0].rdCamera, NULL);
        rdCamera_SetCanvas(&sithCamera_g_aCameras[1].rdCamera, NULL);
        rdCamera_SetCanvas(&sithCamera_g_aCameras[2].rdCamera, NULL);
        rdCamera_SetCanvas(&sithCamera_g_aCameras[4].rdCamera, NULL);
        rdCamera_SetCanvas(&sithCamera_g_aCameras[5].rdCamera, NULL);
        rdCamera_SetCanvas(&sithCamera_g_aCameras[6].rdCamera, NULL);
#ifdef DW_CAMERA
        rdCamera_SetCanvas(&sithCamera_g_aCameras[7].rdCamera, NULL);
#endif

        // Added: Prevent memleak
        for (int i = 0; i < 7; i++)
        {
            rdCamera_FreeEntry(&sithCamera_g_aCameras[i].rdCamera);
        }
#ifdef DW_CAMERA
        if (Main_bDwCompat) {
            rdCamera_FreeEntry(&sithCamera_g_aCameras[7].rdCamera);
        }
#endif
    }
}

void sithCamera_ResetAllCameras()
{
    SithThing *v0; // eax
    rdVector3 rot; // [esp+Ch] [ebp-Ch] BYREF

    v0 = sithWorld_g_pCurrentWorld->pCameraFocusThing;
    sithCamera_g_stateFlags &= ~1u;
    sithCamera_SetCameraFocus(&sithCamera_g_aCameras[0], v0, v0);
    sithCamera_SetCameraFocus(&sithCamera_g_aCameras[1], v0, v0);
    sithCamera_SetCameraFocus(&sithCamera_g_aCameras[2], v0, v0);
    sithCamera_SetCameraFocus(&sithCamera_g_aCameras[3], v0, v0);
    sithCamera_SetCameraFocus(&sithCamera_g_aCameras[4], v0, v0);
    sithCamera_SetCameraFocus(&sithCamera_g_aCameras[5], v0, v0);
    sithCamera_SetCameraFocus(&sithCamera_g_aCameras[6], v0, v0);
#ifdef DW_CAMERA
    sithCamera_SetCameraFocus(&sithCamera_g_aCameras[7], v0, v0);
#endif

    // TODO: DroidWorks does this? Verify if needed.
    if (Main_bDroidWorks) {
        sithCamera_g_bCurCameraSet = 0;
        sithCamera_SetCurrentCamera(sithCamera_g_aCameras + 7);
        sithCamera_g_curCycleCamNum = 0;
        return;
    }

    sithCamera_g_bCurCameraSet = 0;
    sithCamera_g_aCameras[0].pSecondaryFocusThing = 0;
    sithCamera_g_aCameras[1].pSecondaryFocusThing = 0;
    if ( !sithCamera_g_pCurCamera || sithCamera_g_aCameras[0].dword4 >= sithCamera_g_pCurCamera->dword4 )
    {
        sithCamera_g_pCurCamera = sithCamera_g_aCameras;
        sithCamera_g_bCurCameraSet = 1;
        rdCamera_SetCurrent(&sithCamera_g_aCameras[0].rdCamera);
        if ( sithCamera_g_aCameras[0].type == 32 )
        {
            rdMatrix_Copy34(&sithCamera_idleCamOrient, &sithCamera_g_pCurCamera->pPrimaryFocusThing->orient);
            rot.x = 0.0;
            rot.z = 0.0;
            rot.y = -45.0;
            rdMatrix_PostRotate34(&sithCamera_idleCamOrient, &rot);
        }
        sithCamera_Update(sithCamera_g_pCurCamera);
    }
    sithCamera_g_curCycleCamNum = 0;
}

// MOTS altered
int sithCamera_NewEntry(SithCamera *pCamera, uint32_t a2, uint32_t type, flex_t fov, flex_t aspect, rdCanvas *pCanvas, SithThing *pPrimaryFocus, SithThing *pSecondaryFocus)
{
    pCamera->type = type;
    pCamera->dword4 = a2;
    pCamera->pPrimaryFocusThing = pPrimaryFocus;
    pCamera->fov = fov;
    pCamera->aspectRatio = aspect;
    pCamera->pSecondaryFocusThing = pSecondaryFocus;
    rdCamera_NewEntry(&pCamera->rdCamera, fov, 0, SITHCAMERA_ZNEAR, SITHCAMERA_ZFAR, aspect);
    rdCamera_SetAttenuation(&pCamera->rdCamera, SITHCAMERA_ATTENUATION_MIN, SITHCAMERA_ATTENUATION_MAX);

    if (pCanvas) {
        rdCamera_SetCanvas(&pCamera->rdCamera, pCanvas);
    }

    rdVector_Zero3(&pCamera->lookPos);
    rdVector_Zero3(&pCamera->lookPYR);
    rdVector_Zero3(&pCamera->offset);
#ifndef OPTIMIZE_AWAY_UNUSED_FIELDS
    rdVector_Zero3(&pCamera->vecUnknown1);
#endif
    rdMatrix_Identity34(&pCamera->orient);

#ifdef JKM_CAMERA
    pCamera->bZoomed = 0;
    pCamera->zoomScale = 1.0;
    pCamera->invZoomScale = 1.0;
    pCamera->zoomFov = pCamera->fov;
    pCamera->zoomSpeed = 0.0;
#ifdef QOL_IMPROVEMENTS
    pCamera->zoomScaleOrig = 1.0;
    pCamera->zoomFov = 1.0;
#endif
#endif

    return 1;
}

// MOTS altered
void sithCamera_Update(SithCamera *pCamera)
{
    rdVector3 mode64Tmp;
    rdVector3 v76;
    rdVector3 v2; // [esp+2Ch] [ebp-60h] BYREF
    rdVector3 a1; // [esp+38h] [ebp-54h] BYREF
    rdVector3 v84; // [esp+44h] [ebp-48h] BYREF
    rdVector3 rot; // [esp+50h] [ebp-3Ch] BYREF
    rdMatrix34 out; // [esp+5Ch] [ebp-30h] BYREF

    SITH_ASSERTREL(pCamera != NULL); // Added: J3D assert

    SithThing* focusThing = pCamera->pPrimaryFocusThing;
    flex_t v77 = sithCamera_g_cameraAngleDelta * sithTime_g_frameTimeFlex;
    flex_t v78 = sithCamera_g_cameraPosDelta * sithTime_g_frameTimeFlex;
    switch ( pCamera->type )
    {
        case 1:
            SITH_ASSERTREL(focusThing != NULL); // Added: J3D assert
            SITH_ASSERTREL(sithThing_ValidateThingPointer(focusThing)); // Added: J3D assert
            // MOTS added: scope zoom
#ifdef JKM_CAMERA
#ifndef QOL_IMPROVEMENTS
            // Redundant
            if (pCamera->bZoomed) 
#endif
            {
                sithCamera_UpdateZoom(pCamera);
            }
            rdCamera_SetMipmapScalar(pCamera->invZoomScale);
#endif

            rdMatrix_Copy34(&pCamera->orient, &focusThing->orient);
            if ( focusThing->moveType == SITH_MT_PATH && focusThing->renderData.paJointMatrices)
            {
                rdMatrix_Copy34(&pCamera->orient, focusThing->renderData.paJointMatrices);
            }
            else
            {
                if ( focusThing->type == SITH_THING_ACTOR || focusThing->type == SITH_THING_PLAYER )
                {
                    rdVector_Copy3(&v76, &focusThing->actorParams.headPYR);
                }
                else
                {
                    rdVector_Zero3(&v76);
                }

                if ( focusThing->moveType == SITH_MT_PHYSICS )
                {
                    v76.z = rdMath_clampf(5.0 * rdVector_Dot3(&focusThing->orient.rvec, &focusThing->physicsParams.vel), -8.0, 8.0); 
                }

                // MOTS added: hmm??
                if (Main_bMotsCompat || focusThing == sithPlayer_g_pLocalPlayerThing )
                {
                    rdVector_Add3Acc(&v76, &sithCamera_g_vecCameraAngleOffset);
                }

                rdMatrix_PreRotate34(&pCamera->orient, &v76);
                rdMatrix_PostTranslate34(&pCamera->orient, &focusThing->position);
                if ( focusThing->type == SITH_THING_ACTOR || focusThing->type == SITH_THING_PLAYER )
                {
                    rdMatrix_PreTranslate34(&pCamera->orient, &focusThing->actorParams.eyeOffset);
                    
                    // MOTS added: hmm??
                    if (Main_bMotsCompat || focusThing == sithPlayer_g_pLocalPlayerThing )
                        rdMatrix_PreTranslate34(&pCamera->orient, &sithCamera_g_vecCameraPosOffset);
                }
                rdMatrix_Normalize34(&pCamera->orient);
            }
            // Added: nullptr check
            if (focusThing->sector)
                pCamera->sector = sithCollision_FindSectorInRadius(focusThing->sector, &focusThing->position, &pCamera->orient.scale, 0.02);
            break;
#ifdef DW_CAMERA
        case 0x8:
            // Added: DroidWorks scripted look-at camera
            // deployment.jkl's dx_deployment.cog drives slot 2 with
            // setcamerafocii(2, player, ghost) + setcurrentcamera(2): sit at the
            // secondary focus (the movetoframe ghost) and watch the primary
            if (Main_bDwCompat && pCamera->pPrimaryFocusThing != NULL)
            {
                if (pCamera->pSecondaryFocusThing == NULL)
                {
                    rdMatrix_LookAt(&pCamera->orient, &pCamera->lookPos,
                                    &pCamera->pPrimaryFocusThing->position, 0.0f);
                }
                else
                {
                    rdMatrix_LookAt(&pCamera->orient, &pCamera->pSecondaryFocusThing->position,
                                    &pCamera->pPrimaryFocusThing->position, 0.0f);
                    pCamera->sector = pCamera->pSecondaryFocusThing->sector;
                }
            }
            break;
#endif
        case 4:
            SITH_ASSERTREL(focusThing != NULL); // Added: J3D assert
            SITH_ASSERTREL(sithThing_ValidateThingPointer(focusThing)); // Added: J3D assert
            if ( focusThing->type == SITH_THING_ACTOR || focusThing->type == SITH_THING_PLAYER )
            {
                rdVector_Copy3(&v76, &focusThing->actorParams.headPYR);
            }
            else
            {
                rdVector_Zero3(&v76);
            }

            // MOTS added: hmm??
            if (Main_bMotsCompat)
            {
                rdVector_Add3Acc(&v76, &sithCamera_g_vecCameraAngleOffset);
            }

            rdMatrix_Copy34(&out, &focusThing->orient);
            rdMatrix_PreRotate34(&out, &v76);
            rdMatrix_PostTranslate34(&out, &focusThing->position);
            if ( focusThing->type == SITH_THING_ACTOR || focusThing->type == SITH_THING_PLAYER )
                rdMatrix_PostTranslate34(&out, &focusThing->actorParams.eyeOffset);
            pCamera->sector = sithCamera_SearchSectorInRadius(0, focusThing->sector, &focusThing->position, &out.scale, 0.02, RAYCAST_2000 | RAYCAST_200);
            rdVector_Copy3(&v84, &out.scale);
            rdMatrix_Copy34(&pCamera->orient, &out);

            // MOTS added: hmm?
            if (Main_bMotsCompat) {
                rdMatrix_PreTranslate34(&pCamera->orient,&sithCamera_g_vecCameraPosOffset);
            }

            rdMatrix_PreTranslate34(&out, &sithCamera_trans);
            rdMatrix_PreTranslate34(&pCamera->orient, &pCamera->offset);
            rdMatrix_LookAt(&pCamera->orient, &pCamera->orient.scale, &out.scale, 0.0);
            pCamera->sector = sithCamera_SearchSectorInRadius(0, pCamera->sector, &v84, &pCamera->orient.scale, 0.02, RAYCAST_2000 | RAYCAST_200);
            break;
        case 32:
            SITH_ASSERTREL(focusThing != NULL); // Added: J3D assert
            SITH_ASSERTREL(sithThing_ValidateThingPointer(focusThing)); // Added: J3D assert
            rdMatrix_TransformVector34(&a1, &sithCamera_trans2, &sithCamera_idleCamOrient);
            v2 = (rdVector3){0.0, 0.0, 0.05};
            rdVector_Sub3(&v2, &focusThing->position, &v2);
            rdVector_Add3Acc(&a1, &v2);
            rdMatrix_LookAt(&pCamera->orient, &a1, &v2, 0.0);
            pCamera->sector = sithCamera_SearchSectorInRadius(0, focusThing->sector, &focusThing->position, &pCamera->orient.scale, 0.02, RAYCAST_2000 | RAYCAST_200);
            rot.x = 0.0;
            rot.y = sithTime_g_frameTimeFlex * 8.0;
            rot.z = 0.0;
            rdMatrix_PostRotate34(&sithCamera_idleCamOrient, &rot);
            rdMatrix_Normalize34(&sithCamera_idleCamOrient);
            break;
        case 64:
            rdVector_Normalize3Acc(&sithCamera_trans3);
            rdVector_Neg3(&mode64Tmp, &sithCamera_trans3);
            
            // rdMatrix_BuildFromLook34 ish?
            pCamera->orient.lvec.x = mode64Tmp.x;
            pCamera->orient.lvec.y = mode64Tmp.y;
            pCamera->orient.lvec.z = mode64Tmp.z;
            pCamera->orient.rvec.x = (1.0 * mode64Tmp.y) - (0.0 * mode64Tmp.z);
            pCamera->orient.rvec.y = (0.0 * mode64Tmp.z) - (1.0 * mode64Tmp.x);
            pCamera->orient.rvec.z = (0.0 * mode64Tmp.x) - (0.0 * mode64Tmp.y);
            pCamera->orient.uvec.x = (pCamera->orient.rvec.y * mode64Tmp.z) - (pCamera->orient.rvec.z * mode64Tmp.y);
            pCamera->orient.uvec.y = (pCamera->orient.rvec.z * mode64Tmp.x) - (pCamera->orient.rvec.x * mode64Tmp.z);
            pCamera->orient.uvec.z = (pCamera->orient.rvec.x * mode64Tmp.y) - (pCamera->orient.rvec.y * mode64Tmp.x);

            rdMatrix_Normalize34(&pCamera->orient);
            rdVector_Scale3(&pCamera->orient.scale, &sithCamera_trans3, 0.2);

            rdMatrix_PostTranslate34(&pCamera->orient, &focusThing->position);
            pCamera->sector = sithCamera_SearchSectorInRadius(0, focusThing->sector, &focusThing->position, &pCamera->orient.scale, 0.02, RAYCAST_2000 | RAYCAST_200);
            break;
        case 128:
            SITH_ASSERTREL(focusThing != NULL); // Added: J3D assert
            SITH_ASSERTREL(sithThing_ValidateThingPointer(focusThing)); // Added: J3D assert
            rdMatrix_Copy34(&pCamera->orient, &sithCamera_g_orbCamOrient);
            rdMatrix_PostTranslate34(&pCamera->orient, &focusThing->position);
            pCamera->sector = sithCollision_FindSectorInRadius(focusThing->sector, &focusThing->position, &pCamera->orient.scale, 0.02);
            break;
#ifdef DW_CAMERA
        case 0x100:
            // Added: DroidWorks collision follow camera
            dwCamera_Update(pCamera);
            break;
#endif
        default:
            break;
    }
    pCamera->lookPos = pCamera->orient.scale;
    rdMatrix_ExtractAngles34(&pCamera->orient, &pCamera->lookPYR);

    // TODO what inlined func is this
    if ( sithCamera_g_vecCameraPosOffset.x <= 0.0 )
    {
        if ( sithCamera_g_vecCameraPosOffset.x < 0.0 )
        {
            flex_t v42 = v78 + sithCamera_g_vecCameraPosOffset.x;
            if ( v42 < 0.0 )
            {
                sithCamera_g_vecCameraPosOffset.x = v42;
            }
            else {
                sithCamera_g_vecCameraPosOffset.x = 0.0;
            }
        }
        
    }
    else
    {
        flex_t v41 = sithCamera_g_vecCameraPosOffset.x - v78;
        if ( v41 > 0.0 )
        {
            sithCamera_g_vecCameraPosOffset.x = v41;
        }
        else {
            sithCamera_g_vecCameraPosOffset.x = 0.0;
        }
    }

    if ( sithCamera_g_vecCameraPosOffset.y <= 0.0 )
    {
        if ( sithCamera_g_vecCameraPosOffset.y < 0.0 )
        {
            flex_t v48 = v78 + sithCamera_g_vecCameraPosOffset.y;
            if ( v48 < 0.0 )
            {
                sithCamera_g_vecCameraPosOffset.y = v48;
            }
            else {
                sithCamera_g_vecCameraPosOffset.y = 0.0;
            }
        }
    }
    else
    {
        flex_t v47 = sithCamera_g_vecCameraPosOffset.y - v78;
        if ( v47 > 0.0 )
        {
            sithCamera_g_vecCameraPosOffset.y = v47;
        }
        else {
            sithCamera_g_vecCameraPosOffset.y = 0.0;
        }
    }

    if ( sithCamera_g_vecCameraPosOffset.z <= 0.0 )
    {
        if ( sithCamera_g_vecCameraPosOffset.z < 0.0 )
        {
            flex_t v54 = v78 + sithCamera_g_vecCameraPosOffset.z;
            if ( v54 < 0.0 )
            {
                sithCamera_g_vecCameraPosOffset.z = v54;
            }
            else {
                sithCamera_g_vecCameraPosOffset.z = 0.0;
            }
        }
    }
    else
    {
        flex_t v53 = sithCamera_g_vecCameraPosOffset.z - v78;
        if ( v53 > 0.0 )
        {
            sithCamera_g_vecCameraPosOffset.z = v53;
        }
        else {
            sithCamera_g_vecCameraPosOffset.z = 0.0;
        }
    }
    
    if ( sithCamera_g_vecCameraAngleOffset.x <= 0.0 )
    {
        if ( sithCamera_g_vecCameraAngleOffset.x < 0.0 )
        {
            flex_t v60 = v77 + sithCamera_g_vecCameraAngleOffset.x;
            if ( v60 < 0.0 )
            {
                sithCamera_g_vecCameraAngleOffset.x = v60;
            }
            else {
                sithCamera_g_vecCameraAngleOffset.x = 0.0;
            }
        }
    }
    else
    {
        flex_t v59 = sithCamera_g_vecCameraAngleOffset.x - v77;
        if ( v59 > 0.0 )
        {
            sithCamera_g_vecCameraAngleOffset.x = v59;
        }
        else {
            sithCamera_g_vecCameraAngleOffset.x = 0.0;
        }
    }

    if ( sithCamera_g_vecCameraAngleOffset.y <= 0.0 )
    {
        if ( sithCamera_g_vecCameraAngleOffset.y < 0.0 )
        {
            flex_t v66 = v77 + sithCamera_g_vecCameraAngleOffset.y;
            if ( v66 < 0.0 )
            {
                sithCamera_g_vecCameraAngleOffset.y = v66;
            }
            else {
                sithCamera_g_vecCameraAngleOffset.y = 0.0;
            }
        }
        
    }
    else
    {
        flex_t v65 = sithCamera_g_vecCameraAngleOffset.y - v77;
        if ( v65 > 0.0 )
        {
            sithCamera_g_vecCameraAngleOffset.y = v65;
        }
        else {
            sithCamera_g_vecCameraAngleOffset.y = 0.0;
        }
    }
    
    if ( sithCamera_g_vecCameraAngleOffset.z <= 0.0 )
    {
        if ( sithCamera_g_vecCameraAngleOffset.z < 0.0 )
        {
            flex_t v72 = v77 + sithCamera_g_vecCameraAngleOffset.z;
            if ( v72 < 0.0 )
            {
                sithCamera_g_vecCameraAngleOffset.z = v72;
            }
            else {
                sithCamera_g_vecCameraAngleOffset.z = 0.0;
            }
        }
        
    }
    else
    {
        flex_t v71 = sithCamera_g_vecCameraAngleOffset.z - v77;
        if ( v71 > 0.0 )
        {
            sithCamera_g_vecCameraAngleOffset.z = v71;
        }
        else {
            sithCamera_g_vecCameraAngleOffset.z = 0.0;
        }
    }
    
}

sithCameraRenderView sithCamera_g_renderView;
uint32_t sithCamera_renderViewClamps = 0;

static int sithCamera_VecsEqual(const rdVector3* a, const rdVector3* b)
{
    return memcmp(a, b, sizeof(rdVector3)) == 0;
}

void sithCamera_ResetRenderView(SithCamera *pCamera)
{
    sithCameraRenderView* pView = &sithCamera_g_renderView;
    if (!pCamera)
    {
        memset(pView, 0, sizeof(*pView));
        return;
    }
    rdMatrix_Copy34(&pView->orient, &pCamera->orient);
    pView->lookPos = pCamera->lookPos;
    pView->lookPYR = pCamera->lookPYR;
    pView->sector = pCamera->sector;
    pView->yawLead = 0.0;
    pView->bLeadLocalModel = 0;
    pView->bFocusTrue = 0;
    // As rdCamera_Update would make it from the game camera.
    rdMatrix_InvertOrtho34(&pView->trueView, &pCamera->orient);
}

void sithCamera_BuildRenderView(SithCamera *pCamera)
{
    sithCameraRenderView* pView = &sithCamera_g_renderView;
    sithCamera_ResetRenderView(pCamera);

#ifdef QOL_IMPROVEMENTS
    SithThing* pFocus = pCamera->pPrimaryFocusThing;
    uint32_t type = pCamera->type;
    if (!pFocus || !pCamera->sector)
        return;
    if (type != 1 && type != 4 && type != 32 && type != 64 && type != 128)
        return;
    // From here the view follows its focus. Unless it ends up following
    // where the focus is drawn, the focus is drawn true with it (and so is
    // what the focus stands on), or the model would bob against the view.
    pView->bFocusTrue = 1;
    // A path thing's camera rides its first joint, not its placement.
    if (type == 1 && pFocus->moveType == SITH_MT_PATH && pFocus->renderData.paJointMatrices)
        return;

    int bLocal = (pFocus == sithPlayer_g_pLocalPlayerThing);
    int bInterp = 0;
#ifdef SITHINTERP_ENABLED
    bInterp = sithInterp_IsArmed()
              && (sithInterp_mode == SITHINTERP_MODE_ALL || sithInterp_mode == SITHINTERP_MODE_IDENTITY);
#endif

    // Above 150 fps some frames get no physics step, and the player's yaw
    // only changes in a step, so the view stopped for that frame and turned
    // double in the next. Show the turn the mouse asked for now; the next
    // step turns the player by the same amount. Yaw is the outermost rotation
    // of the head angles, so turning the view about the player's own up axis
    // matches the step's turn. In third person only with interpolation on,
    // which turns the player's model with it.
    flex_t lead = 0.0;
    if (bLocal && (type == 1 || (type == 4 && bInterp)))
        lead = sithControl_GetPendingTurn();
#ifdef SITHINTERP_ENABLED
    // The turn keys turn the player at a steady rate, so between steps the
    // view is drawn turned back to where it was at the drawn time.
    if (bLocal && bInterp && (type == 1 || type == 4))
        lead -= (1.0 - sithInterp_GetAlpha()) * sithInterp_GetLocalKeyYaw();
#endif

    // Where the focus is drawn. The local player keeps its last step's
    // orientation (mouse look never lags); others turn as they're drawn.
    rdVector3 posR = pFocus->position;
    rdMatrix34 orientR;
    rdMatrix_Copy34(&orientR, &pFocus->orient);
    rdVector_Zero3(&orientR.scale);
    int bTurned = 0;
#ifdef SITHINTERP_ENABLED
    if (bInterp)
    {
        rdMatrix34 placement;
        int bTranslateOnly = 0;
        if (bLocal || type >= 32)
        {
            sithInterp_GetRenderPos(pFocus, &posR);
        }
        else if (sithInterp_GetRenderPlacement(pFocus, &placement, &bTranslateOnly))
        {
            posR = placement.scale;
            if (!bTranslateOnly)
            {
                rdMatrix_Copy34(&orientR, &placement);
                rdVector_Zero3(&orientR.scale);
                bTurned = 1;
            }
        }
    }
#endif
    if (lead == 0.0 && !bTurned && sithCamera_VecsEqual(&posR, &pFocus->position))
    {
        // These follow the focus's position only, so it can still turn as drawn.
        if (type >= 32)
            pView->bFocusTrue = 0;
        return;
    }

    // D takes the focus's true placement to its drawn one, turned by the
    // lead: T(posR) * orientR * Yaw(lead) * orient^-1 * T(-pos). The view
    // is D * camera, so whatever rig the camera type builds around the focus
    // moves with it.
    rdMatrix34 D;
    if (type >= 32)
    {
        rdMatrix_Identity34(&D);
        rdVector_Sub3(&D.scale, &posR, &pFocus->position);
    }
    else
    {
        rdMatrix34 turned, trueRot, invTrue;
        rdMatrix_Copy34(&turned, &orientR);
        if (lead != 0.0)
        {
            rdVector3 yaw = { 0.0, lead, 0.0 };
            rdMatrix_PreRotate34(&turned, &yaw);
        }
        rdMatrix_Copy34(&trueRot, &pFocus->orient);
        rdVector_Zero3(&trueRot.scale);
        rdMatrix_InvertOrtho34(&invTrue, &trueRot);
        rdMatrix_Multiply34(&D, &turned, &invTrue);
        rdVector3 moved;
        rdMatrix_TransformVector34(&moved, &pFocus->position, &D);
        rdVector_Sub3(&D.scale, &posR, &moved);
    }

    rdMatrix34 orient;
    rdMatrix_Multiply34(&orient, &D, &pCamera->orient);
    rdMatrix_Normalize34(&orient);

    // The drawn eye must be in a sector, reached from the true one: when a
    // wall is in the way, this frame is drawn from the true view.
    SithSector* pSector = pCamera->sector;
    if (!sithCamera_VecsEqual(&orient.scale, &pCamera->lookPos))
    {
        rdVector3 eye = orient.scale;
        pSector = sithCollision_FindSectorInRadius(pCamera->sector, &pCamera->lookPos, &eye, 0.0);
        if (!pSector || !sithCamera_VecsEqual(&eye, &orient.scale))
        {
            sithCamera_renderViewClamps++;
            return;
        }
        // Known: a third-person eye swung by the turn lead, or drawn behind a
        // boom the wall is shortening, can come nearer a wall than the 0.02
        // the camera keeps, so a sliver at the screen's edge can miss that
        // wall for a frame. A plane-distance test here fell back at doorways,
        // stair risers and with the boom resting on a surface, which was
        // worse; a sphere sweep like sithCamera_Update's would be the fix.
    }

    rdMatrix_Copy34(&pView->orient, &orient);
    pView->lookPos = orient.scale;
    rdMatrix_ExtractAngles34(&orient, &pView->lookPYR);
    pView->sector = pSector;
    pView->yawLead = lead;
    pView->bLeadLocalModel = (bLocal && type == 4 && bInterp);
    pView->bFocusTrue = 0;
#endif // QOL_IMPROVEMENTS
}

void sithCamera_RenderScene()
{
    if ( sithCamera_g_pCurCamera )
    {
        rdCamera_SetCurrent(&sithCamera_g_pCurCamera->rdCamera);
        // Added: drawn from the render view (see sithCamera_BuildRenderView).
        sithCamera_BuildRenderView(sithCamera_g_pCurCamera);
        rdCamera_Update(&sithCamera_g_renderView.orient);
        sithRender_Draw();
    }
}

void sithCamera_SetCurrentToCycleCamera()
{
    SithCamera *v0; // esi

    v0 = &sithCamera_g_aCameras[sithCamera_camIdxToGlobalIdx[sithCamera_g_curCycleCamNum]];
    sithCamera_SetCurrentCamera(v0);
}

int sithCamera_SetCurrentCamera(SithCamera *pCamera)
{
    rdVector3 rot; // [esp+8h] [ebp-Ch] BYREF

    SITH_ASSERTREL(pCamera != NULL); // Added: J3D assert

    if ( sithCamera_g_pCurCamera && pCamera->dword4 < sithCamera_g_pCurCamera->dword4 )
        return 0;
#ifdef DW_CAMERA
    SithCamera* pPrevCamera = sithCamera_g_pCurCamera; // Added: DW follow-cam reset wants the pre-switch camera
#endif
    sithCamera_g_pCurCamera = pCamera;
    sithCamera_g_bCurCameraSet = 1;
    rdCamera_SetCurrent(&pCamera->rdCamera);
    if ( pCamera->type == 32 )
    {
        rdMatrix_Copy34(&sithCamera_idleCamOrient, &sithCamera_g_pCurCamera->pPrimaryFocusThing->orient);
        rot.x = 0.0;
        rot.y = -45.0;
        rot.z = 0.0;
        rdMatrix_PostRotate34(&sithCamera_idleCamOrient, &rot);
    }
#ifdef DW_CAMERA
    // Added: DroidWorks follow camera snap-on-switch
    if ( pCamera->type == 0x100 )
        dwCamera_Reset(pPrevCamera, pCamera);
#endif
    sithCamera_Update(sithCamera_g_pCurCamera);
    return 1;
}

void sithCamera_SetCameraFocus(SithCamera *pCamera, SithThing *pPrimaryFocusThing, SithThing *pSecondaryFocusThing)
{
    SITH_ASSERTREL(pCamera); // Added: J3D assert
    pCamera->pPrimaryFocusThing = pPrimaryFocusThing;
    pCamera->pSecondaryFocusThing = pSecondaryFocusThing;
}

SithSector* sithCamera_SearchSectorInRadius(SithThing *a3, SithSector *pSector, rdVector3 *startPos, rdVector3 *endPos, flex_t unused, int flags)
{
    flex_d_t v7; // st7
    SithSector *v9; // ebx
    SithCollision *i; // ecx
    rdVector3 a5; // [esp+Ch] [ebp-Ch] BYREF
    flex_t a6a; // [esp+28h] [ebp+10h]

    rdVector_Sub3(&a5, endPos, startPos);
    v7 = rdVector_Normalize3Acc(&a5);
    a6a = v7;
    v9 = pSector;
    sithCollision_SearchForCollisions(pSector, a3, startPos, &a5, a6a, unused, flags | RAYCAST_800);
    for ( i = sithCollision_PopStack(); i; i = sithCollision_PopStack() )
    {
        if ( (i->type & SITHCOLLISION_ADJOINCROSS) != 0 )
        {
            SITH_ASSERTREL(i->surface->pAdjoin != NULL); // Added: J3D assert
            v9 = i->surface->pAdjoin->sector;
        }
        else if ( (i->type & SITHCOLLISION_THING) == 0 || (i->pThingCollided->type != SITH_THING_ITEM) && i->distance != 0.0 && i->pThingCollided->type != SITH_THING_WEAPON )
        {
            rdVector_Copy3(endPos, startPos);
            rdVector_ScaleAdd3Acc(endPos, &a5, i->distance);
            break;
        }
    }
    sithCollision_DecreaseStackLevel();
    return v9;
}

void sithCamera_SetPOVShake(rdVector3 *posOffset, rdVector3 *angleOffset, flex_t posDelta, flex_t angleDelta)
{
    rdVector_Copy3(&sithCamera_g_vecCameraPosOffset, posOffset);
    rdVector_Copy3(&sithCamera_g_vecCameraAngleOffset, angleOffset);
    sithCamera_g_cameraPosDelta = posDelta;
    sithCamera_g_cameraAngleDelta = angleDelta;
}

SithThing* sithCamera_GetPrimaryFocus(SithCamera *pCamera)
{
    SITH_ASSERTREL(pCamera != NULL); // Added: J3D assert
    return pCamera->pPrimaryFocusThing;
}

SithThing* sithCamera_GetSecondaryFocus(SithCamera *pCamera)
{
    SITH_ASSERTREL(pCamera != NULL); // Added: J3D assert
    return pCamera->pSecondaryFocusThing;
}

int sithCamera_SetCameraStateFlags(int flags)
{
    int result; // eax

    result = flags;
    sithCamera_g_stateFlags = flags;
    return result;
}

int sithCamera_GetCameraStateFlags()
{
    return sithCamera_g_stateFlags;
}

void sithCamera_CycleCamera()
{
    int cam_id; // eax
    SithCamera *v1; // esi
    rdVector3 rot; // [esp+8h] [ebp-Ch] BYREF

    cam_id = ++sithCamera_g_curCycleCamNum;
    if ( (unsigned int)sithCamera_g_curCycleCamNum >= 2 )
    {
        cam_id = 0;
        sithCamera_g_curCycleCamNum = 0;
    }

    v1 = &sithCamera_g_aCameras[sithCamera_camIdxToGlobalIdx[cam_id]];
    sithCamera_SetCurrentCamera(v1);
}

// MOTS added
void sithCamera_SetZoom(SithCamera *pCamera, flex_t zoomScale, flex_t zoomSpeed)
{
    if (!pCamera) return;
    if (!pCamera->rdCamera.pCanvas) return;

#ifdef JKM_CAMERA
#ifdef QOL_IMPROVEMENTS
    flex_t zoomScaleNew = zoomScale;
    if (jkPlayer_fovIsVertical && pCamera->rdCamera.aspectRatio != 0.0) {
        flex_t horFov = (stdMath_ArcTan3(1.0, stdMath_Tan(jkPlayer_fov * 0.5) / pCamera->rdCamera.aspectRatio) *
 -2.0);
        flex_t zoomFov = zoomScale * horFov;
        zoomScaleNew = zoomFov / horFov;
    }

    if (zoomSpeed != 0.0) 
    {
        pCamera->bZoomed = 1;
        pCamera->zoomScaleOrig = zoomScale;
        pCamera->zoomScale = zoomScaleNew;
        pCamera->zoomSpeed = zoomSpeed / 5.0; // TODO find a more exact scalar than just guessing
        pCamera->invZoomScale = 1.0 / zoomScale;
        return;
    }

    rdCamera_SetFOV(&pCamera->rdCamera, jkPlayer_fov / zoomScale);
    pCamera->zoomSpeed = 0.0;
    pCamera->bZoomed = 0;
    pCamera->zoomFov = zoomScale;
#else
    // I have no idea what's going on here
    flex_t zoomFov = stdMath_ArcTan4(zoomScale * 0.5, 0.0);
    zoomFov = -zoomFov * 2.0;

    if (zoomFov != pCamera->rdCamera.fov) 
    {
        if (zoomSpeed != 0.0) 
        {
            pCamera->bZoomed = 1;
            pCamera->zoomScale = zoomScale;
            pCamera->zoomFov = zoomFov;
            pCamera->zoomSpeed = zoomSpeed;
            pCamera->invZoomScale = 1.0 / zoomScale;
            return;
        }

        rdCamera_SetFOV(&pCamera->rdCamera, zoomFov);
        pCamera->zoomSpeed = 0.0;
        pCamera->bZoomed = 0;
    }
#endif
#endif
}

#ifdef QOL_IMPROVEMENTS
// MOTS added (overhauled, zoomFov is now used as a stored scale value)
void sithCamera_UpdateZoom(SithCamera *pCamera)
{
    flex_t currentScale;
    flex_t fVar2;
    int iVar3;
    int iVar4;
    int zoomDirection;

    if (!pCamera->rdCamera.pCanvas) return;

    // Fix zoomscale if screen size changed mid-zoom
    if (jkPlayer_fovIsVertical && pCamera->rdCamera.aspectRatio != 0.0) {
        flex_t horFov = (stdMath_ArcTan3(1.0, stdMath_Tan(jkPlayer_fov * 0.5) / pCamera->rdCamera.aspectRatio) *
 -2.0);
        flex_t zoomFov = pCamera->zoomScaleOrig * horFov;
        pCamera->zoomScale = zoomFov / horFov;
    }

    if (!pCamera->bZoomed) {
        if (Main_bMotsCompat) {
            rdCamera_SetFOV(&sithCamera_g_pCurCamera->rdCamera, jkPlayer_fov / pCamera->zoomScale); 
        }
        //printf("Zoom: en=%x scale=%f, fov=%f, speed=%f, invScale=%f; %x %x %f\n", pCamera->bZoomed, pCamera->zoomScale, pCamera->zoomFov, pCamera->zoomSpeed, pCamera->invZoomScale, 999, 999, jkPlayer_fov / pCamera->zoomScale);
        return;
    }

    currentScale = pCamera->zoomFov;
    fVar2 = pCamera->zoomScale - currentScale;
    if (0.0 <= fVar2) {
        zoomDirection = 1;
        if (fVar2 < 0.0) {
            zoomDirection = 0;
        }
    }
    else {
        zoomDirection = -1;
    }
    fVar2 = currentScale - pCamera->zoomScale;

    flex_t newScale = ((flex_t)zoomDirection * pCamera->zoomSpeed * sithTime_g_frameTimeFlex + currentScale);
    if (fVar2 >= 0.0) {
        if (fVar2 < 0.0) {
            iVar4 = 0;
        }
        else {
            iVar4 = 1;
        }
    }
    else {
        iVar4 = -1;
    }

    fVar2 = newScale - pCamera->zoomScale;
    if (fVar2 >= 0.0) {
        if (fVar2 > 0.0) {
            iVar3 = 1;
        }
        else {
            iVar3 = 0;
        }
    }
    else {
        iVar3 = -1;
    }

    //printf("Zoom: en=%x scale=%f, fov=%f, speed=%f, invScale=%f; %x %x %f %f %f\n", pCamera->bZoomed, pCamera->zoomScale, pCamera->zoomFov, pCamera->zoomSpeed, pCamera->invZoomScale, iVar4, iVar3, newScale, currentScale - pCamera->zoomFov, newScale - pCamera->zoomFov);

    if (iVar4 != iVar3) {
        newScale = pCamera->zoomScale;
    }
    
    // Added: prevent overshoot
    if (zoomDirection < 0) {
        if (newScale < pCamera->zoomScale) {
            newScale = pCamera->zoomScale;
        }
    }
    else if (zoomDirection > 0) {
        if (newScale > pCamera->zoomScale) {
            newScale = pCamera->zoomScale;
        }
    }

    pCamera->zoomFov = newScale;
    rdCamera_SetFOV(&pCamera->rdCamera, jkPlayer_fov / newScale);
    if ((pCamera->zoomFov == currentScale) || (iVar4 != iVar3)) {
        pCamera->zoomSpeed = 0.0;
        pCamera->bZoomed = 0;
    }
}
#else

// MOTS added (original)
void sithCamera_UpdateZoom(SithCamera *pCamera)
{
    flex_t currentFov;
    flex_t fVar2;
    int iVar3;
    int iVar4;
    int zoomDirection;

    if (!pCamera->rdCamera.pCanvas) return;
    if (!pCamera->bZoomed) {

        // Added
        if (Main_bMotsCompat) {
            rdCamera_SetFOV(&sithCamera_g_pCurCamera->rdCamera, jkPlayer_fov / pCamera->zoomScale); 
        }
        return;
    }

    currentFov = pCamera->rdCamera.fov;
    fVar2 = pCamera->zoomFov - currentFov;
    if (0.0 <= fVar2) {
        zoomDirection = 1;
        if (fVar2 < 0.0) {
            zoomDirection = 0;
        }
    }
    else {
        zoomDirection = -1;
    }
    fVar2 = currentFov - pCamera->zoomFov;

    flex_t newFov = ((flex_t)zoomDirection * pCamera->zoomSpeed * sithTime_g_frameTimeFlex + currentFov);
    if (fVar2 >= 0.0) {
        if (fVar2 < 0.0) {
            iVar4 = 0;
        }
        else {
            iVar4 = 1;
        }
    }
    else {
        iVar4 = -1;
    }

    fVar2 = newFov - pCamera->zoomFov;
    if (fVar2 >= 0.0) {
        if (fVar2 > 0.0) {
            iVar3 = 1;
        }
        else {
            iVar3 = 0;
        }
    }
    else {
        iVar3 = -1;
    }

    if (iVar4 != iVar3) {
        newFov = pCamera->zoomFov;
    }
    
    rdCamera_SetFOV(&pCamera->rdCamera, newFov);
    if ((pCamera->rdCamera.fov == currentFov) || (iVar4 != iVar3)) {
        pCamera->zoomSpeed = 0.0;
        pCamera->bZoomed = 0;
    }
}
#endif

