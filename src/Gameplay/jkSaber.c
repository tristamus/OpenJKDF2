#include "jkSaber.h"

#include "World/jkPlayer.h"
#include "Engine/rdroid.h"
#include "Engine/rdPuppet.h"
#include "Engine/sithAnimClass.h"
#include "World/sithSoundClass.h"
#include "Gameplay/sithTime.h"
#include "World/sithSurface.h"
#include "Engine/sithPuppet.h"
#include "Dss/sithMulti.h"
#include "World/sithTemplate.h"
#include "World/sithModel.h"
#include "Engine/sithKeyFrame.h"
#include "World/sithSector.h"
#include "Engine/sithCollision.h"
#include "Main/jkSmack.h"
#include "General/stdString.h"
#include "Engine/rdMaterial.h"
#include "Win95/stdDisplay.h"

#include "jk.h"

#define JKSABER_EXTENDTIME (0.3000000)
#define JKSABER_LENGTHCHANGE (0.6000000)

void jkSaber_Startup()
{
}

void jkSaber_Shutdown()
{
}

// Added: colored saber glow. Averages the visible pixels of a blade material's
// first texture, weighted by how colorful each pixel is, so a white core doesn't
// wash the color out. Returns 0 when the texture has no visible pixels (SBX's
// blades are fully transparent), 1 with the color otherwise. A gray or white
// texture gives white.
static int jkSaber_GlowColorFromTexture(rdMaterial* pMaterial, rdVector3* pOut)
{
    if (!pMaterial || !pMaterial->textures || !pMaterial->num_textures)
        return 0;
#ifdef RDMATERIAL_LRU_LOAD_UNLOAD
    if (!pMaterial->bDataLoaded)
        return 0;
#endif

    rdTexture* pTexture = &pMaterial->textures[0];
    if (!pTexture->num_mipmaps)
        return 0;
    tVBuffer* pVBuf = pTexture->texture_struct[0];
    if (!pVBuf)
        return 0;

    int bIs16bit = pVBuf->format.format.is16bit;
    const rdColor24* pPalette = NULL;
    if (!bIs16bit) {
        // The renderer shows 8-bit textures through the level's palette
        if (sithWorld_g_pCurrentWorld && sithWorld_g_pCurrentWorld->colormaps)
            pPalette = sithWorld_g_pCurrentWorld->colormaps->colors;
        else
            pPalette = pMaterial->palette_alloc;
        if (!pPalette)
            return 0;
    }

    int width = pVBuf->format.width;
    int height = pVBuf->format.height;
    if (width <= 0 || height <= 0)
        return 0;
    int rowStride = pVBuf->format.rowSize / (bIs16bit ? 2 : 1);
    if (rowStride < width)
        rowStride = width;
    int bAlpha = pTexture->alpha_en & 1;

    if (!stdDisplay_VBufferLock(pVBuf))
        return 0;
    const uint8_t* pPixels = (const uint8_t*)pVBuf->surface_lock_alloc;
    if (!pPixels) {
        stdDisplay_VBufferUnlock(pVBuf);
        return 0;
    }

    double sumR = 0.0, sumG = 0.0, sumB = 0.0, sumWeight = 0.0;
    int numVisible = 0;
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            flex_t r, g, b;
            if (bIs16bit)
            {
                uint16_t val = ((const uint16_t*)pPixels)[(y * rowStride) + x];
                if (bAlpha) { // RGB1555
                    if (!(val >> 15))
                        continue;
                    r = ((val >> 10) & 0x1F) / 31.0;
                    g = ((val >> 5) & 0x1F) / 31.0;
                    b = (val & 0x1F) / 31.0;
                }
                else { // RGB565
                    r = ((val >> 11) & 0x1F) / 31.0;
                    g = ((val >> 5) & 0x3F) / 63.0;
                    b = (val & 0x1F) / 31.0;
                }
            }
            else
            {
                uint8_t idx = pPixels[(y * rowStride) + x];
                if (idx == 0 || (bAlpha && idx == pTexture->color_transparent))
                    continue;
                r = pPalette[idx].r / 255.0;
                g = pPalette[idx].g / 255.0;
                b = pPalette[idx].b / 255.0;
            }

            flex_t maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
            flex_t minC = r < g ? (r < b ? r : b) : (g < b ? g : b);
            if (maxC < 0.08)
                continue;
            numVisible++;

            flex_t weight = maxC - minC;
            sumR += r * weight;
            sumG += g * weight;
            sumB += b * weight;
            sumWeight += weight;
        }
    }
    stdDisplay_VBufferUnlock(pVBuf);

    if (!numVisible)
        return 0;

    // Mostly gray or white blade: white light
    if (sumWeight < numVisible * 0.06 || sumWeight <= 0.0) {
        rdVector_Set3(pOut, 1.0, 1.0, 1.0);
        return 1;
    }

    flex_t r = sumR / sumWeight;
    flex_t g = sumG / sumWeight;
    flex_t b = sumB / sumWeight;
    flex_t maxC = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (maxC <= 0.0) {
        rdVector_Set3(pOut, 1.0, 1.0, 1.0);
        return 1;
    }
    rdVector_Set3(pOut, r / maxC, g / maxC, b / maxC);
    return 1;
}

// Added: colored saber glow. Color from a color word in the blade material's
// file name, for blades whose texture is blank. Longer words are checked first,
// so "lblue" wins over "blue".
static int jkSaber_GlowColorFromName(const char* pFname, rdVector3* pOut)
{
    static const struct {
        const char* pWord;
        flex32_t r, g, b;
    } aColorWords[] = {
        {"lightblue", 0.45, 0.80, 1.00},
        {"ltblue",    0.45, 0.80, 1.00},
        {"lblue",     0.45, 0.80, 1.00},
        {"magenta",   1.00, 0.30, 0.85},
        {"purple",    0.70, 0.25, 1.00},
        {"violet",    0.70, 0.25, 1.00},
        {"orange",    1.00, 0.50, 0.10},
        {"yellow",    1.00, 0.90, 0.20},
        {"silver",    1.00, 1.00, 1.00},
        {"white",     1.00, 1.00, 1.00},
        {"green",     0.20, 1.00, 0.25},
        {"cyan",      0.20, 0.95, 1.00},
        {"teal",      0.20, 0.95, 1.00},
        {"blue",      0.15, 0.35, 1.00},
        {"gold",      1.00, 0.80, 0.20},
        {"pink",      1.00, 0.35, 0.80},
        {"red",       1.00, 0.12, 0.08},
    };

    if (!pFname)
        return 0;

    char lowered[128];
    stdString_SafeStrCopy(lowered, pFname, sizeof(lowered));
    for (char* c = lowered; *c; c++) {
        if (*c >= 'A' && *c <= 'Z')
            *c = *c - 'A' + 'a';
    }
    // Only the file's own name, not its folder
    const char* pName = lowered;
    for (const char* c = lowered; *c; c++) {
        if (*c == '/' || *c == '\\')
            pName = c + 1;
    }

    for (size_t i = 0; i < sizeof(aColorWords) / sizeof(aColorWords[0]); i++)
    {
        if (strstr(pName, aColorWords[i].pWord)) {
            rdVector_Set3(pOut, aColorWords[i].r, aColorWords[i].g, aColorWords[i].b);
            return 1;
        }
    }
    return 0;
}

// Added: colored saber glow. The color of the light a lit blade casts: from the
// side texture, then the tip texture, then their file names, else white.
static void jkSaber_CalcGlowColor(jkPlayerInfo* saberinfo, const char* material_side_fname, const char* material_tip_fname)
{
    rdVector3* pOut = &saberinfo->saberGlowColor;
    if (jkSaber_GlowColorFromTexture(saberinfo->polyline.face.material, pOut))
        return;
    if (jkSaber_GlowColorFromTexture(saberinfo->polyline.tipFace.material, pOut))
        return;
    if (jkSaber_GlowColorFromName(material_side_fname, pOut))
        return;
    if (jkSaber_GlowColorFromName(material_tip_fname, pOut))
        return;
    rdVector_Set3(pOut, 1.0, 1.0, 1.0);
}

void jkSaber_InitializeSaberInfo(SithThing *thing, char *material_side_fname, char *material_tip_fname, flex_t base_rad, flex_t tip_rad, flex_t len, SithThing *wall_sparks, SithThing *blood_sparks, SithThing *saber_sparks)
{
    if (!thing) return; // Added: Fix nullptr deref in Mots cutscenes

    flex_t length = 0.0;
    jkPlayerInfo* saberinfo = thing->playerInfo;
    if ( saberinfo->polylineThing.polyline )
    {
        length = saberinfo->polyline.length;
        rdThing_FreeEntry(&saberinfo->polylineThing);
        rdPolyline_FreeEntry(&saberinfo->polyline);
        saberinfo->polylineThing.polyline = 0;
    }

#ifdef DEBUG_QOL_CHEATS
    if (thing == sithPlayer_g_pLocalPlayerThing && !sithNet_isMulti) {
        //material_tip_fname = "saberpurple0.mat";
        //material_side_fname = "saberpurple1.mat";
    }
    if (thing == sithPlayer_g_pLocalPlayerThing) {
        //thing->jkFlags |= JKFLAG_DUALSABERS;
    }
#endif

    rdPolyline_FreeEntry(&saberinfo->polyline); // Added: fix memleak
    rdPolyline_NewEntry(&saberinfo->polyline, "Saber", material_side_fname, material_tip_fname, length, base_rad, tip_rad, RD_LIGHTMODE_4_UNK, 0, 0, 0.0);
    rdThing_NewEntry(&saberinfo->polylineThing, thing);
    rdThing_SetPolyline(&saberinfo->polylineThing, &saberinfo->polyline);
    saberinfo->wall_sparks = wall_sparks;
    saberinfo->blood_sparks = blood_sparks;
    saberinfo->saber_sparks = saber_sparks;
    saberinfo->length = len;

    jkSaber_CalcGlowColor(saberinfo, material_side_fname, material_tip_fname); // Added: colored saber glow
}

void jkSaber_PolylineRand(rdThing *thing)
{
    rdPolyline* line = thing->polyline;
    if ( line )
    {
        if ( !(jkPlayer_currentTickIdx & 0xF) )
            line->face.texVertOffset.y = 0.0;
        line->face.texVertOffset.y += (_frand() - 0.8) * 80.0;
    }
}

void jkSaber_Draw(rdMatrix34 *posRotMat)
{
    if ( playerThings[playerThingIdx].actorThing->jkFlags & JKFLAG_SABERON
      && playerThings[playerThingIdx].povModel.model3
      && playerThings[playerThingIdx].polylineThing.model3 )
    {
        if ( playerThings[playerThingIdx].povModel.rdFrameNum != rdroid_frameTrue )
        {
            rdPuppet_BuildJointMatrices(&playerThings[playerThingIdx].povModel, posRotMat);
        }

        jkSaber_PolylineRand(&playerThings[playerThingIdx].polylineThing);
        rdThing_Draw(&playerThings[playerThingIdx].polylineThing, &playerThings[playerThingIdx].povModel.paJointMatrices[5]); // aaaaa hardcoded K_Rhand
        
        // Added: Dual sabers
        if (playerThings[playerThingIdx].actorThing->jkFlags & JKFLAG_DUALSABERS)
            rdThing_Draw(&playerThings[playerThingIdx].polylineThing, &playerThings[playerThingIdx].povModel.paJointMatrices[2]); // K_Lhand
    }
}

void jkSaber_UpdateLength(SithThing *thing)
{
    jkPlayerInfo* playerInfo = thing->playerInfo;
    if (!playerInfo )
    {
        thing->jkFlags &= ~JKFLAG_SABERON;
        return;
    }

    if (!(thing->jkFlags & JKFLAG_SABERON)) {
        playerInfo->polyline.length = 0;
        return; // Added: Wanted more logic in jkSaber_UpdateLength
    }

#if 0
    printf("Saber state: ");
    if (thing->jkFlags & JKFLAG_SABERON) {
        printf("ON ");
    }
    if (thing->jkFlags & JKFLAG_SABERDAMAGE) {
        printf("DAMAGE ");
    }
    if (thing->jkFlags & JKFLAG_SABEREXTEND) {
        printf("EXTEND ");
    }
    if (thing->jkFlags & JKFLAG_SABERRETRACT) {
        printf("RETRACT ");
    }
    if (thing->jkFlags & JKFLAG_DUALSABERS) {
        printf("DUALSABERS ");
    }
    if (thing->jkFlags & JKFLAG_SABERFORCEON) {
        printf("FORCEON ");
    }
    printf(" len=%f %f\n", playerInfo->polyline.length, thing->actorParams.timeLeftLengthChange);
#endif

    if (thing->flags & SITH_TF_DEAD || thing->type == SITH_THING_CORPSE)
    {
        thing->jkFlags |= JKFLAG_SABERRETRACT;
    }

    // Added: HACK fix a bug where the saber gets stuck extended.
    if ((thing->jkFlags & (JKFLAG_SABEREXTEND | JKFLAG_SABERRETRACT)) == (JKFLAG_SABEREXTEND | JKFLAG_SABERRETRACT))
    {
        thing->jkFlags &= ~JKFLAG_SABERRETRACT;
        playerInfo->polyline.length = 0;
    }

    // Added: MotS lights the saber at spawn with "force on" (0x81), which the owner's
    // PC uses up in the same frame, before its flags go out. Other players then only
    // ever see "on", so the blade stayed at length 0 (just the glow at the hilt).
    // Another player's saber that is on and not retracting is out: grow it.
    if (sithNet_isMulti && thing->type == SITH_THING_PLAYER
        && thing != sithPlayer_g_pLocalPlayerThing
        && !(thing->jkFlags & (JKFLAG_SABEREXTEND | JKFLAG_SABERRETRACT | JKFLAG_SABERFORCEON))
        && playerInfo->polyline.length < playerInfo->length)
    {
        thing->jkFlags |= JKFLAG_SABEREXTEND;
    }

    if ( thing->jkFlags & JKFLAG_SABEREXTEND)
    {
        flex_t newLength = playerInfo->polyline.length + (sithTime_g_frameTimeFlex * JKSABER_EXTENDTIME);
        flex_t deltaLen = newLength / playerInfo->length;

        thing->jkFlags &= ~JKFLAG_SABERRETRACT;

        playerInfo->polyline.length = newLength;
        thing->actorParams.timeLeftLengthChange = deltaLen * JKSABER_LENGTHCHANGE;
        if (newLength >= playerInfo->length) // ? verify, IDA crapped out on this comparison
        {
            playerInfo->polyline.length = playerInfo->length;
            thing->actorParams.timeLeftLengthChange = JKSABER_LENGTHCHANGE;
            thing->jkFlags &= ~(JKFLAG_SABERRETRACT | JKFLAG_SABEREXTEND);
        }
    }
    else if ( thing->jkFlags & JKFLAG_SABERRETRACT )
    {
        flex_t newLength = playerInfo->polyline.length - (sithTime_g_frameTimeFlex * JKSABER_EXTENDTIME);
        flex_t deltaLen = newLength / playerInfo->length;

        thing->jkFlags &= ~JKFLAG_SABEREXTEND;

        playerInfo->polyline.length = newLength;
        thing->actorParams.timeLeftLengthChange = deltaLen * JKSABER_LENGTHCHANGE;
        if ( newLength <= 0.0 ) // ? verify, IDA crapped out on this comparison
        {
            playerInfo->polyline.length = 0.0;
            thing->jkFlags &= ~(JKFLAG_SABEREXTEND | JKFLAG_SABERRETRACT | JKFLAG_SABERON);
            thing->actorParams.timeLeftLengthChange = 0.0;
        }
    }
    else if (thing->jkFlags & JKFLAG_SABERFORCEON) // Used for starting a level with the saber on, ie DF2 lv4
    {
        playerInfo->polyline.length = playerInfo->length;
        thing->actorParams.timeLeftLengthChange = JKSABER_LENGTHCHANGE;
        thing->jkFlags &= ~(JKFLAG_SABERRETRACT | JKFLAG_SABEREXTEND);
        thing->jkFlags |= JKFLAG_SABERON;

        // Added? I think my RETRACT | EXTEND fix inavertently exposed a bug
        thing->jkFlags &= ~JKFLAG_SABERFORCEON;
    }

    if ( thing->pPuppetClass->aJoints[JOINTTYPE_PRIMARYWEAP] >= 0 )
    {
        jkSaber_UpdateCollision(thing, thing->pPuppetClass->aJoints[JOINTTYPE_PRIMARYWEAP], 0); // MOTS added: last arg
        if ( thing->jkFlags & JKFLAG_DUALSABERS )
        {
            if ( thing->pPuppetClass->aJoints[JOINTTYPE_SECONDARYWEAP] >= 0 )
                jkSaber_UpdateCollision(thing, thing->pPuppetClass->aJoints[JOINTTYPE_SECONDARYWEAP], 1); // MOTS added: last arg
        }
    }
}

// MOTS added: split into its own func
void  jkSaber_UpdateCollision2(SithThing *pPlayerThing,rdVector3 *pSaberPos,rdVector3 *pSaberDir,jkSaberCollide *pCollideInfo)
{
    SithSector *pSector;
    SithCollision *searchResult;
    SithThing *resultThing;
    rdVector3 local_54;
    rdVector3 local_3c;
    jkPlayerInfo *playerInfo;
    rdMatrix34 tmpMat;
    
    playerInfo = pPlayerThing->playerInfo;
    pSector = sithCollision_FindSectorInRadius(pPlayerThing->sector,&pPlayerThing->position,pSaberPos,0.0);
    if (!pSector) {
        return;
    }
    sithCollision_SearchForCollisions(pSector,pPlayerThing,pSaberPos,pSaberDir,pCollideInfo->bladeLength,0.0,0);
    

    SithSector* pSectorIter = pSector;
    while (1) 
    {
        searchResult = sithCollision_PopStack();
        if (!searchResult)
            break;

        if (searchResult->type & SITHCOLLISION_ADJOINCROSS)
        {
            pSectorIter = searchResult->surface->pAdjoin->sector;
        }
        else if (searchResult->type & SITHCOLLISION_THING) 
        {
            rdVector_Copy3(&local_54, pSaberPos);
            rdVector_ScaleAdd3Acc(&local_54, pSaberDir, searchResult->distance);

            resultThing = searchResult->pThingCollided;

            if ( resultThing->type == SITH_THING_ITEM || resultThing->type == SITH_THING_EXPLOSION || resultThing->type == SITH_THING_PARTICLE )
            {
                continue;
            }
            if (resultThing->actorParams.flags & SITH_AF_DROID 
                || resultThing->type != SITH_THING_ACTOR && resultThing->type != SITH_THING_PLAYER )
            {
                jkSaber_SpawnSparks(playerInfo, &local_54, pSectorIter, SPARKTYPE_WALL);
            }
            if ( pCollideInfo->numDamagedThings == 6 )
            {
                break;
            }

            int foundIdx = 0;
            for (foundIdx = 0; foundIdx < pCollideInfo->numDamagedThings; foundIdx++ )
            {
                if ( searchResult->pThingCollided == pCollideInfo->damagedThings[foundIdx] )
                    break;
            }

            if ( foundIdx < pCollideInfo->numDamagedThings )
            {
                break;
            }

            if ( resultThing->type != SITH_THING_ACTOR 
                 && resultThing->type != SITH_THING_PLAYER 
                 || !(resultThing->actorParams.flags & SITH_AF_BLEEDS) )
            {
                jkSaber_SpawnSparks(playerInfo, &local_54, pSectorIter, SPARKTYPE_BLOOD);

                sithThing_DamageThing(searchResult->pThingCollided, pPlayerThing, pCollideInfo->damage, SITH_DAMAGE_SABER);
                pCollideInfo->damagedThings[pCollideInfo->numDamagedThings++] = searchResult->pThingCollided;
                break;
            }

            // TODO is this a vector func?
            rdVector_Sub3(&local_3c, &local_54, &resultThing->position);
            rdVector_Normalize3Acc(&local_3c);
            rdMatrix_Copy34(&tmpMat, &resultThing->orient);
            if ( resultThing->type == SITH_THING_ACTOR || resultThing->type == SITH_THING_PLAYER )
                rdMatrix_PreRotate34(&tmpMat, &resultThing->actorParams.headPYR);
                
            // TODO: is this a vector func?
            rdVector3 v52 = tmpMat.lvec;
            rdVector_Normalize3Acc(&v52);
            if ( rdVector_Dot3(&v52, &local_3c) >= resultThing->actorParams.fov
              && (_frand() < resultThing->actorParams.chance) )
            {
                if (!(pPlayerThing->actorParams.flags & SITH_AF_INVISIBLE)) // verify
                {
                    sithSoundClass_PlayModeRandom(pPlayerThing, SITH_SC_DEFLECTED);

                    if ( _frand() >= 0.5 )
                        sithPuppet_PlayMode(resultThing, SITH_ANIM_BLOCK2, 0);
                    else
                        sithPuppet_PlayMode(resultThing, SITH_ANIM_BLOCK, 0);

                    jkSaber_SpawnSparks(playerInfo, &local_54, pSectorIter, SPARKTYPE_SABER);

                    sithCog_ThingSendMessage(resultThing, 0, SITH_MESSAGE_BLOCKED);
                    pCollideInfo->damagedThings[pCollideInfo->numDamagedThings++] = searchResult->pThingCollided;
                    break;
                }
            }

            jkSaber_SpawnSparks(playerInfo, &local_54, pSectorIter, SPARKTYPE_BLOOD);

            sithThing_DamageThing(resultThing, pPlayerThing, pCollideInfo->damage, SITH_DAMAGE_SABER);
            pCollideInfo->damagedThings[pCollideInfo->numDamagedThings++] = searchResult->pThingCollided;
            break;
        }
        else if (searchResult->type & SITHCOLLISION_WORLD)
        {
            rdVector_Copy3(&local_54, pSaberPos);
            rdVector_ScaleAdd3Acc(&local_54, pSaberDir, searchResult->distance - 0.001);
            
            jkSaber_SpawnSparks(playerInfo, &local_54, pSectorIter, SPARKTYPE_WALL);

            if ( pCollideInfo->numDamagedSurfaces < 6 )
            {
                int surfaceNum = 0;
                for ( surfaceNum = 0; surfaceNum < pCollideInfo->numDamagedSurfaces; surfaceNum++ )
                {
                    if ( searchResult->surface == pCollideInfo->damagedSurfaces[surfaceNum] )
                        break;
                }
                if ( surfaceNum >= pCollideInfo->numDamagedSurfaces )
                {
                    sithSurface_HandleThingImpact(searchResult->surface, pPlayerThing, pCollideInfo->damage, SITH_DAMAGE_SABER);
                    pCollideInfo->damagedSurfaces[pCollideInfo->numDamagedSurfaces++] = searchResult->surface;
                }
            }
            break;
        }
    }
    sithCollision_DecreaseStackLevel();
}

// MOTS altered: interpolation and multiple blades
void jkSaber_UpdateCollision(SithThing *player, int joint, int bSecondary)
{
    jkPlayerInfo *playerInfo; // ebx
    rdVector3 a2a;
    rdMatrix34 jointMat;
    rdVector3 jointPos;
    rdMatrix34 matrix;
    rdMatrix34 tmpMat;
    rdMatrix34 lastJointMat;
    rdVector3 lerpSaberDir;
    rdVector3 lerpSaberPos;
    rdVector3 lerpPosDelta;
    rdVector3 lerpDirDelta;
    rdMatrix34 *pWhichLastJointMat;
    rdMatrix34 *pGripMat; // Added

    playerInfo = player->playerInfo;

    rdMatrix_Copy34(&matrix, &player->orient);
    rdVector_Copy3(&matrix.scale, &player->position);
    if ( jkSmack_GetCurrentGuiState() == 6 ) {
        rdPuppet_BuildJointMatrices(&player->renderData, &matrix);
    }

    if ( !rdModel3_GetMeshMatrix(&player->renderData, &matrix, joint, &jointMat) )
        return;

    // Added: the blade starts where its hilt is drawn, and points the way it
    // does (jkPlayer_GripMatrix)
    pGripMat = jkPlayer_GripMatrix(player, joint, &jointMat, &tmpMat);
    if (pGripMat != &jointMat)
        rdMatrix_Copy34(&jointMat, pGripMat);

    rdVector_Copy3(&player->actorParams.saberBladePos, &jointMat.scale);
    rdVector_ScaleAdd3Acc(&player->actorParams.saberBladePos, &jointMat.lvec, playerInfo->polyline.length);

    if ( player->jkFlags & JKFLAG_40 )
    {
        player->jkFlags &= ~JKFLAG_40;
        playerInfo->saberCollideInfo.numDamagedThings = 0;
        playerInfo->saberCollideInfo.numDamagedSurfaces = 0;
    }
    if ( !(player->jkFlags & JKFLAG_SABERDAMAGE) )
        return;
    if ( !playerInfo->saberCollideInfo.field_1A4 )
        return;
    
    // Always do ticked saber collision
#ifndef QOL_IMPROVEMENTS
    if (!Main_bMotsCompat) {
        jkSaber_UpdateCollision2(player,&jointMat.scale, &jointMat.lvec, &playerInfo->saberCollideInfo);
        return;
    }
#endif
    
    // MOTS added: interpolation at low FPS
    // This ensures that saber collision is *at least* 20fps-quality,
    // Added: QoL modified to be at least 30fps-quality collisions for DSi
#ifdef QOL_IMPROVEMENTS
    const flex_t saberMinDelta = 0.032; // 30FPS-ish
#else
    const flex_t saberMinDelta = 0.05; // 20FPS
#endif
    rdVector_Copy3(&jointPos, &jointMat.scale);
    rdVector_Copy3(&a2a, &jointMat.lvec);
    if (sithTime_g_frameTimeFlex > saberMinDelta && playerInfo->bHasLastJointMat) 
    {
        pWhichLastJointMat = &playerInfo->lastSaberJointMat;
        if (bSecondary != 0) {
            pWhichLastJointMat = &playerInfo->lastSecondarySaberJointMat;
        }
        flex_t fVar1 = sithTime_g_fps * saberMinDelta;
        rdMatrix_Copy34(&lastJointMat, pWhichLastJointMat);

        rdVector_Sub3(&lerpPosDelta, &jointMat.scale, &lastJointMat.scale);
        rdVector_Sub3(&lerpDirDelta, &jointMat.lvec, &lastJointMat.lvec);
        flex_t stepAmount = fVar1;

        // This will step 0 times at 20fps, once at 10fps, twice at 5fps, etc
        for (; fVar1 < 1.0; fVar1 += stepAmount) {
            rdVector_Copy3(&lerpSaberPos, &lastJointMat.scale);
            rdVector_ScaleAdd3Acc(&lerpSaberPos, &lerpPosDelta, fVar1);

            rdVector_Copy3(&lerpSaberDir, &lastJointMat.lvec);
            rdVector_ScaleAdd3Acc(&lerpSaberDir, &lerpDirDelta, fVar1);

            jkSaber_UpdateCollision2(player,&lerpSaberPos,&lerpSaberDir,&playerInfo->saberCollideInfo);
        }
    }
    jkSaber_UpdateCollision2(player,&jointPos,&a2a,&playerInfo->saberCollideInfo);
    pWhichLastJointMat = &playerInfo->lastSaberJointMat;
    if (bSecondary != 0) {
        pWhichLastJointMat = &playerInfo->lastSecondarySaberJointMat;
    }

    // Store the joint matrix so we can get a delta for the next frame
    rdMatrix_Copy34(pWhichLastJointMat, &jointMat);
    playerInfo->bHasLastJointMat = 1;
}

void jkSaber_SpawnSparks(jkPlayerInfo *pPlayerInfo, rdVector3 *pPos, SithSector *psector, int sparkType)
{
    SithThing *pCreateThingTemplate; // eax
    SithThing *pSpawned; // eax

    if ( sithTime_g_msecGameTime < pPlayerInfo->lastSparkSpawnMs + 200 )
        return;

    if ( sparkType == SPARKTYPE_BLOOD )
    {
        pCreateThingTemplate = pPlayerInfo->blood_sparks;
    }
    else if ( sparkType == SPARKTYPE_SABER )
    {
        pCreateThingTemplate = pPlayerInfo->saber_sparks;
    }
    else // SPARKTYPE_WALL
    {
        pCreateThingTemplate = pPlayerInfo->wall_sparks;
    }
    if ( pCreateThingTemplate )
    {
        pSpawned = sithThing_CreateThingAtPos(pCreateThingTemplate, pPos, &rdroid_identMatrix34, psector, 0);
        if ( pSpawned )
        {
            pSpawned->pParent = pPlayerInfo->actorThing;
            pPlayerInfo->lastSparkSpawnMs = sithTime_g_msecGameTime;
            pSpawned->parentSignature = pPlayerInfo->actorThing->signature;
        }
    }
}

// MOTS altered
void jkSaber_Enable(SithThing *pThing, flex_t damage, flex_t bladeLength, flex_t stunDelay)
{
    if (!pThing || !pThing->playerInfo) return; // MOTS added

    pThing->playerInfo->saberCollideInfo.damage = damage;
    pThing->playerInfo->saberCollideInfo.bladeLength = bladeLength;
    pThing->playerInfo->saberCollideInfo.stunDelay = stunDelay;
    pThing->playerInfo->saberCollideInfo.field_1A4 = 1;
    pThing->playerInfo->saberCollideInfo.numDamagedThings = 0;
    pThing->playerInfo->saberCollideInfo.numDamagedSurfaces = 0;

    _memset(pThing->playerInfo->saberCollideInfo.damagedThings, 0, sizeof(pThing->playerInfo->saberCollideInfo.damagedThings));
    _memset(pThing->playerInfo->saberCollideInfo.damagedSurfaces, 0, sizeof(pThing->playerInfo->saberCollideInfo.damagedSurfaces));
    
    pThing->playerInfo->lastSparkSpawnMs = 0;

#ifdef JKM_SABER
    pThing->playerInfo->bHasLastJointMat = 0; // MOTS added
#endif
}

// MOTS altered
void jkSaber_Disable(SithThing *player)
{
    //MOTS added:
    if (!player || !player->playerInfo) return;

    player->playerInfo->saberCollideInfo.field_1A4 = 0;
#ifdef JKM_SABER
    player->playerInfo->bHasLastJointMat = 0; // MOTS added
#endif
}