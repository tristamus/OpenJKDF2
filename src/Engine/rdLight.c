#include "rdLight.h"

#include "stdPlatform.h" // Added: *_ALLOC/*_FREE macros

#include "jk.h"

#include "Primitives/rdMath.h"
#include "Engine/rdroid.h"
#include "General/stdMath.h"

rdLight *rdLight_New()
{
    rdLight *light;

    light = (rdLight*)RDROID_ALLOC(sizeof(rdLight));
    if (!light)
    return 0;

    rdLight_NewEntry(light);

    return light;
}

int rdLight_NewEntry(rdLight *pLight)
{
    pLight->type = 2;
    pLight->bEnabled = 1;
    pLight->direction.x = 0.0;
    pLight->direction.y = 0.0;
    pLight->direction.z = 0.0;
    pLight->intensity = 1.0;
    pLight->color = 0xFFFFFF;
    rdVector_Set3(&pLight->rgb, 1.0, 1.0, 1.0); // Added: colored saber glow
#ifdef JKM_LIGHTING
    pLight->angleX = 0.0;
    pLight->cosAngleX = 0.0;
    pLight->angleY = 0.0;
    pLight->cosAngleY = 0.0;
#else
    pLight->dword20 = 0;
    pLight->dword24 = 0;
#endif
    return 1;
}

void rdLight_Free(rdLight *pLight)
{
    if (pLight)
        RDROID_FREE(pLight);
}

void rdLight_FreeEntry(rdLight *pLight)
{
}

#ifdef JKM_LIGHTING
void rdLight_SetAngles(rdLight *pLight, flex_t angleX, flex_t angleY)
{
    flex_t local_4;
    
    local_4 = 0.0;
    pLight->angleX = angleX;
    pLight->angleY = angleY;

    stdMath_SinCos(angleX, &local_4, &pLight->cosAngleX);
    stdMath_SinCos(angleY, &local_4, &pLight->cosAngleY);

    // Grim Fandango added: divide safety
    flex_d_t denom = (pLight->cosAngleX - pLight->cosAngleY);
    if (denom != 0.0) {
        pLight->lux = 1.0 / denom;
    }
    return;
}
#endif

flex_t rdLight_CalcVertexIntensities(rdLight **apLights, rdVector3 *aLightPos, 
#ifdef JKM_LIGHTING
    rdVector3 *localLightDirs, 
#endif
    int numLights, rdVector3 *aVertexNormal, rdVector3 *aVertices, flex_t *aVertexColors, flex_t *aColors, int numVertices, flex_t scalar)
{
#ifndef JKM_LIGHTING
    int vertexLightsSize;
    rdVector3* vertexIter;
    rdLight **meshLightIter;
    flex_t len;
    flex_t lightMagnitude;
    rdLight *light;
    rdVector3 diff;
    rdVector3 *vertexNormals;
    flex_t *outLights;
    flex_t *idkIter;
    int i, j;

    if (!numVertices)
        return 0.0;

    // TODO: this was inlined from another (uncalled) function
    vertexNormals = aVertexNormal;
    idkIter = aVertexColors;
    outLights = aColors;
    vertexIter = aVertices;
    for (j = 0; j < numVertices; j++)
    {
        *outLights = *idkIter;
        meshLightIter = apLights;
        for (i = 0; i < numLights; i++)
        {
            rdVector_Sub3(&diff, &aLightPos[i], vertexIter);
            light = *meshLightIter;
            len = rdVector_Len3(&diff);
            if ( len < (*meshLightIter)->minRadius )
            {
                rdVector_Normalize3Acc(&diff);
                lightMagnitude = rdVector_Dot3(vertexNormals, &diff);
                if ( lightMagnitude > 0.0 )
                    *outLights += (light->intensity - len * scalar) * lightMagnitude;
            }
            if ( *outLights >= 1.0 )
                break;
            ++meshLightIter;
        }

        ++vertexIter;
        ++outLights;
        ++idkIter;
        ++vertexNormals;
    }
    return 0.0;

#else
    if (!localLightDirs || !Main_bMotsCompat)
    {
        int vertexLightsSize;
        rdVector3* vertexIter;
        rdLight **meshLightIter;
        flex_t len;
        flex_t lightMagnitude;
        rdLight *light;
        rdVector3 diff;
        rdVector3 *vertexNormals;
        flex_t *outLights;
        flex_t *idkIter;
        int i, j;

        if (!numVertices)
            return 0.0;

        // TODO: this was inlined from another (uncalled) function
        vertexNormals = aVertexNormal;
        idkIter = aVertexColors;
        outLights = aColors;
        vertexIter = aVertices;
        for (j = 0; j < numVertices; j++)
        {
            *outLights = *idkIter;
            meshLightIter = apLights;
            for (i = 0; i < numLights; i++)
            {
                rdVector_Sub3(&diff, &aLightPos[i], vertexIter);
                light = *meshLightIter;
                len = rdVector_Len3(&diff);
                if ( len < (*meshLightIter)->minRadius )
                {
                    rdVector_Normalize3Acc(&diff);
                    lightMagnitude = rdVector_Dot3(vertexNormals, &diff);
                    if ( lightMagnitude > 0.0 )
                        *outLights += (light->intensity - len * scalar) * lightMagnitude;
                }
                if ( *outLights >= 1.0 )
                    break;
                ++meshLightIter;
            }

            ++vertexIter;
            ++outLights;
            ++idkIter;
            ++vertexNormals;
        }
        return 0.0;
    }

    flex_t fVar1;
    rdLight *light;
    flex_t lightMagnitude;
    rdVector3 *vertexNormals;
    rdVector3 *lightDirIter;
    flex_t fVar8;
    rdVector3 *vertexIter;
    flex_t local_28;
    rdVector3 diff;
    rdLight** meshLightIter;
    flex_t* outLights;
    
    local_28 = 0.0;
    if (numVertices == 0) return 0.0;
    
    outLights = aColors;
    vertexIter = aVertices;
    vertexNormals = aVertexNormal;
    for (int vertIdx = 0; vertIdx < numVertices; vertIdx++)
    {
        *outLights = *aVertexColors;

        meshLightIter = apLights;
        aVertexNormal = aLightPos;
        lightDirIter = localLightDirs;
        
        for (int i = 0; i < numLights; i++)
        {
            rdVector_Sub3(&diff, aVertexNormal, vertexIter);
            light = *meshLightIter;
            if ((light->minRadius * light->minRadius) > rdVector_Dot3(&diff, &diff))
            {
                fVar8 = rdVector_Normalize3Acc(&diff);
                if (light->type < 3) 
                {
                    lightMagnitude = rdVector_Dot3(vertexNormals, &diff);
                    if (lightMagnitude > 0.0) 
                    {
                        *outLights += (light->intensity - fVar8 * scalar) * lightMagnitude;
                    }
                }
                else 
                {
                    lightMagnitude = rdVector_Dot3(lightDirIter, &diff);
                    if (lightMagnitude > light->cosAngleY)
                    {
                        fVar1 = light->intensity;
                        if (lightMagnitude < light->cosAngleX)
                        {
                            fVar1 = (1.0 - (light->cosAngleX - lightMagnitude) * light->lux) * fVar1;
                        }
                        *outLights += (fVar1 - fVar8 * scalar) * rdVector_Dot3(vertexNormals, lightDirIter);
                    }
                }
            }

            if (*outLights > 1.0) {
                *outLights = 1.0;
            }
            if (*outLights == 1.0) break;
            meshLightIter++;
            aVertexNormal++;
            lightDirIter++;
        }
    
        local_28 += *outLights;
        vertexNormals++;
        vertexIter++;
        outLights++;
    } 
    return (flex_t)(local_28 / (flex_t)numVertices);
#endif
}

// Added: rdLight_CalcVertexIntensities with each light's color applied per channel,
// for meshes lit by a colored saber glow. Same falloff and the same early stop as
// the original, per channel; with white lights each channel matches the original.
void rdLight_CalcVertexIntensitiesRGB(rdLight **apLights, rdVector3 *aLightPos,
#ifdef JKM_LIGHTING
    rdVector3 *localLightDirs,
#endif
    int numLights, rdVector3 *aVertexNormal, rdVector3 *aVertices, flex_t *aVertexColors, flex_t *aRed, flex_t *aGreen, flex_t *aBlue, int numVertices, flex_t scalar)
{
    rdVector3 diff;
#ifdef JKM_LIGHTING
    int bMotsLighting = localLightDirs && Main_bMotsCompat;
#else
    int bMotsLighting = 0;
#endif

    for (int j = 0; j < numVertices; j++)
    {
        // The original's MotS path starts every vertex from the first vertex's color;
        // do the same so a mesh doesn't change brightness when a colored light reaches it
        flex_t red = bMotsLighting ? aVertexColors[0] : aVertexColors[j];
        flex_t green = red;
        flex_t blue = red;

        for (int i = 0; i < numLights; i++)
        {
            rdLight* light = apLights[i];
            rdVector_Sub3(&diff, &aLightPos[i], &aVertices[j]);
            flex_t len = rdVector_Len3(&diff);
            flex_t amount = 0.0;
            if (len >= light->minRadius)
                goto next_light;

            rdVector_Normalize3Acc(&diff);
#ifdef JKM_LIGHTING
            if (bMotsLighting && light->type >= 3)
            {
                // Spot light, as in rdLight_CalcVertexIntensities
                flex_t lightMagnitude = rdVector_Dot3(&localLightDirs[i], &diff);
                if (lightMagnitude > light->cosAngleY)
                {
                    flex_t spotIntensity = light->intensity;
                    if (lightMagnitude < light->cosAngleX)
                        spotIntensity = (1.0 - (light->cosAngleX - lightMagnitude) * light->lux) * spotIntensity;
                    amount = (spotIntensity - len * scalar) * rdVector_Dot3(&aVertexNormal[j], &localLightDirs[i]);
                }
            }
            else
#endif
            {
                flex_t lightMagnitude = rdVector_Dot3(&aVertexNormal[j], &diff);
                if (lightMagnitude > 0.0)
                    amount = (light->intensity - len * scalar) * lightMagnitude;
            }

            red += amount * light->rgb.x;
            green += amount * light->rgb.y;
            blue += amount * light->rgb.z;

next_light:
            // MotS caps each light's sum at full as it goes, Jedi Knight only at the end
            if (bMotsLighting) {
                if (red > 1.0) red = 1.0;
                if (green > 1.0) green = 1.0;
                if (blue > 1.0) blue = 1.0;
            }
            if (red >= 1.0 && green >= 1.0 && blue >= 1.0)
                break;
        }

        aRed[j] = stdMath_Clamp(red, 0.0, 1.0);
        aGreen[j] = stdMath_Clamp(green, 0.0, 1.0);
        aBlue[j] = stdMath_Clamp(blue, 0.0, 1.0);
    }
}

flex_t rdLight_CalcFaceIntensity(rdLight **apLights, rdVector3 *apLightPos, int numLights, rdFace *pFace, rdVector3 *pNormal, rdVector3 *apVertices, flex_t attenuation)
{
  rdVector3 *lightPosIter; // esi
  rdLight *meshLight; // ebx
  int *v9; // eax
  flex_t v10; // st7
  flex_t v11; // st7
  flex_t intensity; // [esp+10h] [ebp-14h]
  int v15; // [esp+14h] [ebp-10h]
  rdVector3 diff; // [esp+18h] [ebp-Ch]
  flex_t meshLightsa; // [esp+28h] [ebp+4h]
  rdLight **meshLightIter; // [esp+2Ch] [ebp+8h]

  intensity = 0.0;
  lightPosIter = apLightPos;
  meshLightIter = apLights;
  for (v15 = 0; v15 < numLights; v15++)
  {
      meshLight = *meshLightIter;
      if ( (*meshLightIter)->bEnabled )
      {
        v9 = pFace->vertexPosIdx;
        rdVector_Sub3(&diff, lightPosIter, &apVertices[*v9]);
        v10 = rdMath_DistancePointToPlane(lightPosIter, pNormal, &apVertices[*v9]);
        meshLightsa = v10;
        if ( v10 < meshLight->minRadius )
        {
          rdVector_Normalize3Acc(&diff);
          v11 = rdVector_Dot3(pNormal, &diff);
          if ( v11 > 0.0 )
            intensity += (meshLight->intensity - meshLightsa * attenuation) * v11;
        }
      }
      if ( intensity >= 1.0 )
        break;
      ++lightPosIter;
      ++meshLightIter;
  }
  return intensity;
}

// TODO? unused
void rdLight_CalcDistVertexIntensities(){}
void rdLight_CalcDistFaceIntensity(){}
