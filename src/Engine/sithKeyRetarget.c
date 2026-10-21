#include "sithKeyRetarget.h"

#ifdef SITH_KEY_RETARGET

#include <math.h>
#include <string.h>

#include "Engine/rdKeyframe.h"
#include "Engine/rdroid.h"
#include "General/stdConffile.h"
#include "General/stdString.h"
#include "General/crc32.h"
#include "Main/jkRes.h"
#include "stdPlatform.h"
#include "jk.h"

// The recipe, inside a mod's GOB:
//
//   source 3do\bo.3do retail     model the keys were made for ("retail":
//   target 3do\ky.3do retail     read the game's own copy, not a mod's)
//   legs lleg lcalf lfoot k_lthigh k_lcalf k_lfoot   (optional) scales root
//                                movement by the target's leg length
//   feet k_lfoot k_rfoot         (optional) keeps the target's feet on the
//                                ground in the source's rest pose
//   damage always                (optional) the saber hurts on any touch:
//                                each key turns damage on at its start, and
//                                the source's damage markers are left out,
//                                so an attack hits each thing once
//   bones 2
//   torso k_torso dir neck k_neck
//   rsaber k_rhand blade
//   keys 1
//   rt_bochop.key bochop.key 1.5   (optional) plays 1.5 times as fast
//   end
//
// Bones are named, or @n for node n. Each source bone's world rotation
// drives one target bone, through a fixed correction worked out from the
// two models' rest poses (taken to be the same pose):
//   ref    the bones' rest orientations correspond
//   dir    the bone (towards the named child) points the same way as the
//          source's, with the least twist
//   blade  the target bone takes the source's orientation as it is, so a
//          saber held there points where the source's did
// Target bones not listed keep their rest rotation, and every target node
// keeps its rest position except the root, which follows the source's.

#define RT_RECIPE_PATH "misc\\keyretarget.dat"
#define RT_MAX_NODES (64)
#define RT_MAX_BONES (32)
#define RT_MAX_KEYS (64)
#define RT_MAX_FRAMES (2048)
#define RT_NAME_LEN (32)
#define RT_PATH_LEN (128)
#define RT_DEG (57.29577951308232)

enum
{
    RT_BONE_REF = 0,
    RT_BONE_DIR = 1,
    RT_BONE_BLADE = 2,
};

// Key marker types sithPuppet acts on
enum
{
    RT_MARKER_DAMAGE_ON = 4,
    RT_MARKER_DAMAGE_OFF = 5,
    RT_MARKER_DAMAGE_RESET = 14,
};

enum
{
    RT_STATE_UNREAD = 0,
    RT_STATE_NONE = 1,
    RT_STATE_RECIPE = 2,
    RT_STATE_READY = 3,
};

// Columns are rvec, lvec, uvec, as rdMatrix_Build34 makes them.
typedef struct rtMat
{
    double m[3][3];
} rtMat;

typedef struct rtNode
{
    int parent;
    double pos[3];
    double rot[3];
    char name[RT_NAME_LEN];
} rtNode;

typedef struct rtSkel
{
    int numNodes;
    rtNode nodes[RT_MAX_NODES];
    int order[RT_MAX_NODES]; // parents before children
} rtSkel;

typedef struct rtPose
{
    double pos[RT_MAX_NODES][3];
    double rot[RT_MAX_NODES][3];
} rtPose;

typedef struct rtBone
{
    char src[RT_NAME_LEN];
    char dst[RT_NAME_LEN];
    char srcChild[RT_NAME_LEN];
    char dstChild[RT_NAME_LEN];
    int mode;
} rtBone;

typedef struct rtRecipe
{
    char srcModel[RT_PATH_LEN];
    char dstModel[RT_PATH_LEN];
    int bSrcRetail;
    int bDstRetail;
    int bHasLegs;
    char legs[6][RT_NAME_LEN];
    int bHasFeet;
    char feet[2][RT_NAME_LEN];
    int bDamageAlways;
    int numBones;
    rtBone bones[RT_MAX_BONES];
    int numKeys;
    char outKeys[RT_MAX_KEYS][RT_NAME_LEN];
    char srcKeys[RT_MAX_KEYS][RT_NAME_LEN];
    double speeds[RT_MAX_KEYS];
} rtRecipe;

typedef struct rtState
{
    int state;
    rtRecipe recipe;
    rtSkel src;
    rtSkel dst;
    rtPose srcRest;
    rtPose dstRest;
    int numPairs;
    int pairSrc[RT_MAX_BONES];
    int pairDst[RT_MAX_BONES];
    rtMat corr[RT_MAX_BONES];
    double legScale;
    double ground;

    // Scratch, kept here rather than on the stack
    rtPose srcPose;
    rtPose dstPose;
    rtMat srcWorld[RT_MAX_NODES];
    rtMat dstWorld[RT_MAX_NODES];
    double srcJoints[RT_MAX_NODES][3];
    double dstJoints[RT_MAX_NODES][3];
} rtState;

static rtState sithKeyRetarget_state;

//
// Maths
//

static void sithKeyRetarget_Identity(rtMat* out)
{
    memset(out, 0, sizeof(*out));
    out->m[0][0] = out->m[1][1] = out->m[2][2] = 1.0;
}

// Same as rdMatrix_Build34, in doubles
static void sithKeyRetarget_BuildMat(rtMat* out, const double pyr[3])
{
    double sx = sin(pyr[0] / RT_DEG), cx = cos(pyr[0] / RT_DEG);
    double sy = sin(pyr[1] / RT_DEG), cy = cos(pyr[1] / RT_DEG);
    double sz = sin(pyr[2] / RT_DEG), cz = cos(pyr[2] / RT_DEG);

    out->m[0][0] = -(sz * sy) * sx + cz * cy;
    out->m[1][0] = (sz * cy) * sx + cz * sy;
    out->m[2][0] = -sz * cx;
    out->m[0][1] = -sy * cx;
    out->m[1][1] = cy * cx;
    out->m[2][1] = sx;
    out->m[0][2] = (cz * sy) * sx + sz * cy;
    out->m[1][2] = -sx * (cy * cz) + sy * sz;
    out->m[2][2] = cz * cx;
}

// The inverse of BuildMat, exactly (rdMatrix_ExtractAngles34 approximates)
static void sithKeyRetarget_Extract(double pyr[3], const rtMat* a)
{
    pyr[0] = atan2(a->m[2][1], sqrt(a->m[0][1] * a->m[0][1] + a->m[1][1] * a->m[1][1])) * RT_DEG;
    pyr[1] = atan2(-a->m[0][1], a->m[1][1]) * RT_DEG;
    pyr[2] = atan2(-a->m[2][0], a->m[2][2]) * RT_DEG;
}

// out = a * b
static void sithKeyRetarget_Mul(rtMat* out, const rtMat* a, const rtMat* b)
{
    rtMat tmp;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            tmp.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
    *out = tmp;
}

// out = transpose(a) * b
static void sithKeyRetarget_MulT(rtMat* out, const rtMat* a, const rtMat* b)
{
    rtMat tmp;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            tmp.m[i][j] = a->m[0][i] * b->m[0][j] + a->m[1][i] * b->m[1][j] + a->m[2][i] * b->m[2][j];
    *out = tmp;
}

static void sithKeyRetarget_Apply(double out[3], const rtMat* a, const double v[3])
{
    double tmp[3];
    for (int i = 0; i < 3; i++)
        tmp[i] = a->m[i][0] * v[0] + a->m[i][1] * v[1] + a->m[i][2] * v[2];
    out[0] = tmp[0];
    out[1] = tmp[1];
    out[2] = tmp[2];
}

static double sithKeyRetarget_Length(const double v[3])
{
    return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// The least rotation turning direction a towards direction b
static void sithKeyRetarget_Arc(rtMat* out, const double a[3], const double b[3])
{
    double la = sithKeyRetarget_Length(a), lb = sithKeyRetarget_Length(b);
    double u[3], w[3], v[3], c;

    sithKeyRetarget_Identity(out);
    if (la < 1e-9 || lb < 1e-9)
        return;

    for (int i = 0; i < 3; i++)
    {
        u[i] = a[i] / la;
        w[i] = b[i] / lb;
    }
    v[0] = u[1] * w[2] - u[2] * w[1];
    v[1] = u[2] * w[0] - u[0] * w[2];
    v[2] = u[0] * w[1] - u[1] * w[0];
    c = u[0] * w[0] + u[1] * w[1] + u[2] * w[2];

    if (c < -0.999999)
    {
        // Opposite: half a turn about any axis square to a
        double axis[3] = {0.0, u[2], -u[1]};
        double len = sithKeyRetarget_Length(axis);
        if (len < 1e-6)
        {
            axis[0] = -u[2];
            axis[1] = 0.0;
            axis[2] = u[0];
            len = sithKeyRetarget_Length(axis);
        }
        for (int i = 0; i < 3; i++)
            axis[i] /= len;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                out->m[i][j] = 2.0 * axis[i] * axis[j] - (i == j ? 1.0 : 0.0);
        return;
    }

    // I + [v]x + [v]x^2 / (1 + c)
    {
        double k = 1.0 / (1.0 + c);
        double vx[3][3] = {
            {0.0, -v[2], v[1]},
            {v[2], 0.0, -v[0]},
            {-v[1], v[0], 0.0},
        };
        for (int i = 0; i < 3; i++)
        {
            for (int j = 0; j < 3; j++)
            {
                double sq = vx[i][0] * vx[0][j] + vx[i][1] * vx[1][j] + vx[i][2] * vx[2][j];
                out->m[i][j] = (i == j ? 1.0 : 0.0) + vx[i][j] + sq * k;
            }
        }
    }
}

static double sithKeyRetarget_WrapAngle(double a)
{
    while (a > 180.0)
        a -= 360.0;
    while (a <= -180.0)
        a += 360.0;
    return a;
}

//
// Skeletons
//

static int sithKeyRetarget_FindNode(const rtSkel* pSkel, const char* pName)
{
    if (pName[0] == '@')
    {
        int idx = _atoi(pName + 1);
        return (idx >= 0 && idx < pSkel->numNodes) ? idx : -1;
    }
    for (int i = 0; i < pSkel->numNodes; i++)
    {
        if (!__strcmpi(pSkel->nodes[i].name, pName))
            return i;
    }
    return -1;
}

static int sithKeyRetarget_SortNodes(rtSkel* pSkel)
{
    int depth[RT_MAX_NODES];
    int n = 0;

    for (int i = 0; i < pSkel->numNodes; i++)
    {
        int d = 0;
        int p = pSkel->nodes[i].parent;
        while (p >= 0)
        {
            if (p >= pSkel->numNodes || ++d > pSkel->numNodes)
                return 0;
            p = pSkel->nodes[p].parent;
        }
        depth[i] = d;
    }
    for (int d = 0; d <= pSkel->numNodes && n < pSkel->numNodes; d++)
    {
        for (int i = 0; i < pSkel->numNodes; i++)
        {
            if (depth[i] == d)
                pSkel->order[n++] = i;
        }
    }
    return n == pSkel->numNodes;
}

// Reads a model's hierarchy without loading it, so nothing (materials,
// meshes) is registered with the world.
static int sithKeyRetarget_LoadSkel(rtSkel* pSkel, rtPose* pRest, const char* pPath, int bRetail)
{
    char path[RT_PATH_LEN];
    int bOpened = 0;
    int numNodes = 0;

    memset(pSkel, 0, sizeof(*pSkel));
    stdString_SafeStrCopy(path, pPath, sizeof(path));

    if (bRetail)
    {
        jkRes_bGameFilesOnly = 1;
        bOpened = stdConffile_Open(path);
        jkRes_bGameFilesOnly = 0;
    }
    if (!bOpened && !stdConffile_Open(path))
    {
        stdPlatform_Printf("OpenJKDF2: Key retarget: can't open model `%s`\n", path);
        return 0;
    }

    while (stdConffile_ReadLine())
    {
        if (_sscanf(stdConffile_g_aLine, " hierarchy nodes %d", &numNodes) == 1)
            break;
    }
    if (numNodes <= 0 || numNodes > RT_MAX_NODES)
        goto fail;

    for (int i = 0; i < numNodes; i++)
    {
        rtNode* pNode = &pSkel->nodes[i];
        int idx, mesh, parent, child, sibling, numChildren;
        unsigned int flags, type;
        double pivot[3];

        if (!stdConffile_ReadLine())
            goto fail;
        if (_sscanf(stdConffile_g_aLine, " %d: %x %x %d %d %d %d %d %lf %lf %lf %lf %lf %lf %lf %lf %lf %31s",
                    &idx, &flags, &type, &mesh, &parent, &child, &sibling, &numChildren,
                    &pNode->pos[0], &pNode->pos[1], &pNode->pos[2],
                    &pNode->rot[0], &pNode->rot[1], &pNode->rot[2],
                    &pivot[0], &pivot[1], &pivot[2], pNode->name) != 18)
            goto fail;
        pNode->parent = parent;
        memcpy(pRest->pos[i], pNode->pos, sizeof(pNode->pos));
        memcpy(pRest->rot[i], pNode->rot, sizeof(pNode->rot));
    }
    pSkel->numNodes = numNodes;
    stdConffile_Close();

    if (!sithKeyRetarget_SortNodes(pSkel))
    {
        stdPlatform_Printf("OpenJKDF2: Key retarget: bad hierarchy in `%s`\n", path);
        return 0;
    }
    return 1;

fail:
    stdPlatform_Printf("OpenJKDF2: Key retarget: can't read the hierarchy in `%s`\n", path);
    stdConffile_Close();
    return 0;
}

// World rotation and joint centre of every node. A node's transform is its
// parent's times its own (pos, rot); pivots only offset meshes, so they
// cancel out of both and aren't needed.
static void sithKeyRetarget_World(const rtSkel* pSkel, const rtPose* pPose, rtMat* pWorld, double (*pJoints)[3])
{
    for (int k = 0; k < pSkel->numNodes; k++)
    {
        int i = pSkel->order[k];
        int parent = pSkel->nodes[i].parent;
        rtMat local;

        sithKeyRetarget_BuildMat(&local, pPose->rot[i]);
        if (parent < 0)
        {
            pWorld[i] = local;
            memcpy(pJoints[i], pPose->pos[i], sizeof(pJoints[i]));
            continue;
        }
        sithKeyRetarget_Mul(&pWorld[i], &pWorld[parent], &local);
        sithKeyRetarget_Apply(pJoints[i], &pWorld[parent], pPose->pos[i]);
        for (int c = 0; c < 3; c++)
            pJoints[i][c] += pJoints[parent][c];
    }
}

// The target pose for a source pose
static void sithKeyRetarget_Convert(rtState* pState, const rtPose* pSrc, rtPose* pOut)
{
    rtSkel* pDst = &pState->dst;
    rtMat* srcWorld = pState->srcWorld;
    rtMat* dstWorld = pState->dstWorld;
    rtMat goal[RT_MAX_NODES];
    int bDriven[RT_MAX_NODES];

    sithKeyRetarget_World(&pState->src, pSrc, srcWorld, pState->srcJoints);

    memset(bDriven, 0, sizeof(bDriven));
    for (int b = 0; b < pState->numPairs; b++)
    {
        int t = pState->pairDst[b];
        sithKeyRetarget_Mul(&goal[t], &srcWorld[pState->pairSrc[b]], &pState->corr[b]);
        bDriven[t] = 1;
    }

    for (int k = 0; k < pDst->numNodes; k++)
    {
        int i = pDst->order[k];
        int parent = pDst->nodes[i].parent;
        rtMat parentWorld, local;

        if (parent < 0)
            sithKeyRetarget_Identity(&parentWorld);
        else
            parentWorld = dstWorld[parent];

        if (bDriven[i])
        {
            dstWorld[i] = goal[i];
            sithKeyRetarget_MulT(&local, &parentWorld, &dstWorld[i]);
            sithKeyRetarget_Extract(pOut->rot[i], &local);
        }
        else
        {
            memcpy(pOut->rot[i], pState->dstRest.rot[i], sizeof(pOut->rot[i]));
            sithKeyRetarget_BuildMat(&local, pOut->rot[i]);
            sithKeyRetarget_Mul(&dstWorld[i], &parentWorld, &local);
        }
        memcpy(pOut->pos[i], pState->dstRest.pos[i], sizeof(pOut->pos[i]));
    }

    // The root moves as the source's does, scaled to the target's legs
    for (int c = 0; c < 3; c++)
        pOut->pos[0][c] = pState->dstRest.pos[0][c] + (pSrc->pos[0][c] - pState->srcRest.pos[0][c]) * pState->legScale;
    pOut->pos[0][2] += pState->ground;
}

static double sithKeyRetarget_LowestFoot(rtState* pState, const rtPose* pPose, const int feet[2])
{
    sithKeyRetarget_World(&pState->dst, pPose, pState->dstWorld, pState->dstJoints);
    double a = pState->dstJoints[feet[0]][2];
    double b = pState->dstJoints[feet[1]][2];
    return a < b ? a : b;
}

static double sithKeyRetarget_LegLength(const rtSkel* pSkel, double (*pJoints)[3], char (*names)[RT_NAME_LEN])
{
    int a = sithKeyRetarget_FindNode(pSkel, names[0]);
    int b = sithKeyRetarget_FindNode(pSkel, names[1]);
    int c = sithKeyRetarget_FindNode(pSkel, names[2]);
    double ab[3], bc[3];

    if (a < 0 || b < 0 || c < 0)
        return 0.0;
    for (int i = 0; i < 3; i++)
    {
        ab[i] = pJoints[b][i] - pJoints[a][i];
        bc[i] = pJoints[c][i] - pJoints[b][i];
    }
    return sithKeyRetarget_Length(ab) + sithKeyRetarget_Length(bc);
}

//
// Recipe
//

static int sithKeyRetarget_ParseMode(const char* pMode)
{
    if (!__strcmpi(pMode, "ref"))
        return RT_BONE_REF;
    if (!__strcmpi(pMode, "dir"))
        return RT_BONE_DIR;
    if (!__strcmpi(pMode, "blade"))
        return RT_BONE_BLADE;
    return -1;
}

static int sithKeyRetarget_ReadRecipe(rtRecipe* pRecipe)
{
    char path[RT_PATH_LEN];
    char w[7][RT_PATH_LEN];
    int count, n;

    memset(pRecipe, 0, sizeof(*pRecipe));
    stdString_SafeStrCopy(path, RT_RECIPE_PATH, sizeof(path));
    if (!stdConffile_Open(path))
        return 0;

    while (stdConffile_ReadLine())
    {
        n = _sscanf(stdConffile_g_aLine, "%127s %127s %127s %127s %127s %127s %127s", w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
        if (n <= 0 || !strcmp(w[0], "section:"))
            continue;
        if (!strcmp(w[0], "end"))
            break;

        if (!strcmp(w[0], "source") && n >= 2)
        {
            stdString_SafeStrCopy(pRecipe->srcModel, w[1], RT_PATH_LEN);
            pRecipe->bSrcRetail = (n >= 3 && !strcmp(w[2], "retail"));
        }
        else if (!strcmp(w[0], "target") && n >= 2)
        {
            stdString_SafeStrCopy(pRecipe->dstModel, w[1], RT_PATH_LEN);
            pRecipe->bDstRetail = (n >= 3 && !strcmp(w[2], "retail"));
        }
        else if (!strcmp(w[0], "legs") && n == 7)
        {
            for (int i = 0; i < 6; i++)
                stdString_SafeStrCopy(pRecipe->legs[i], w[i + 1], RT_NAME_LEN);
            pRecipe->bHasLegs = 1;
        }
        else if (!strcmp(w[0], "feet") && n == 3)
        {
            stdString_SafeStrCopy(pRecipe->feet[0], w[1], RT_NAME_LEN);
            stdString_SafeStrCopy(pRecipe->feet[1], w[2], RT_NAME_LEN);
            pRecipe->bHasFeet = 1;
        }
        else if (!strcmp(w[0], "damage") && n == 2 && !strcmp(w[1], "always"))
        {
            pRecipe->bDamageAlways = 1;
        }
        else if (!strcmp(w[0], "bones") && n == 2)
        {
            count = _atoi(w[1]);
            if (count <= 0 || count > RT_MAX_BONES)
                goto fail;
            for (int i = 0; i < count; i++)
            {
                rtBone* pBone = &pRecipe->bones[i];
                if (!stdConffile_ReadLine())
                    goto fail;
                n = _sscanf(stdConffile_g_aLine, "%127s %127s %127s %127s %127s", w[0], w[1], w[2], w[3], w[4]);
                if (n < 3)
                    goto fail;
                pBone->mode = sithKeyRetarget_ParseMode(w[2]);
                if (pBone->mode < 0 || (pBone->mode == RT_BONE_DIR && n < 5))
                    goto fail;
                stdString_SafeStrCopy(pBone->src, w[0], RT_NAME_LEN);
                stdString_SafeStrCopy(pBone->dst, w[1], RT_NAME_LEN);
                if (pBone->mode == RT_BONE_DIR)
                {
                    stdString_SafeStrCopy(pBone->srcChild, w[3], RT_NAME_LEN);
                    stdString_SafeStrCopy(pBone->dstChild, w[4], RT_NAME_LEN);
                }
            }
            pRecipe->numBones = count;
        }
        else if (!strcmp(w[0], "keys") && n == 2)
        {
            count = _atoi(w[1]);
            if (count <= 0 || count > RT_MAX_KEYS)
                goto fail;
            for (int i = 0; i < count; i++)
            {
                if (!stdConffile_ReadLine())
                    goto fail;
                n = _sscanf(stdConffile_g_aLine, "%127s %127s %127s", w[0], w[1], w[2]);
                if (n < 2)
                    goto fail;
                stdString_SafeStrCopy(pRecipe->outKeys[i], w[0], RT_NAME_LEN);
                stdString_SafeStrCopy(pRecipe->srcKeys[i], w[1], RT_NAME_LEN);
                pRecipe->speeds[i] = (n == 3) ? _atof(w[2]) : 1.0;
                if (pRecipe->speeds[i] <= 0.0 || pRecipe->speeds[i] > 10.0)
                    goto fail;
            }
            pRecipe->numKeys = count;
        }
        else
        {
            goto fail;
        }
    }
    stdConffile_Close();

    if (!pRecipe->srcModel[0] || !pRecipe->dstModel[0] || !pRecipe->numBones || !pRecipe->numKeys)
    {
        stdPlatform_Printf("OpenJKDF2: Key retarget: `%s` needs a source, target, bones and keys\n", path);
        return 0;
    }
    stdPlatform_Printf("OpenJKDF2: Key retarget: %d keys from `%s` to `%s`\n", pRecipe->numKeys, pRecipe->srcModel, pRecipe->dstModel);
    return 1;

fail:
    stdPlatform_Printf("OpenJKDF2: Key retarget: can't read line %d of `%s`\n", stdConffile_linenum, path);
    stdConffile_Close();
    return 0;
}

static int sithKeyRetarget_FindKey(const char* pName)
{
    rtState* pState = &sithKeyRetarget_state;

    if (pState->state == RT_STATE_UNREAD)
        pState->state = sithKeyRetarget_ReadRecipe(&pState->recipe) ? RT_STATE_RECIPE : RT_STATE_NONE;
    if (pState->state == RT_STATE_NONE)
        return -1;

    for (int i = 0; i < pState->recipe.numKeys; i++)
    {
        if (!__strcmpi(pState->recipe.outKeys[i], pName))
            return i;
    }
    return -1;
}

// Reads both models and works out each bone's correction
static int sithKeyRetarget_Setup(rtState* pState)
{
    rtRecipe* pRecipe = &pState->recipe;
    rtMat* srcWorld = pState->srcWorld;
    rtMat* dstWorld = pState->dstWorld;
    // Copies, since Convert reuses the scratch
    static rtMat srcRestWorld[RT_MAX_NODES], dstRestWorld[RT_MAX_NODES];
    static double srcRestJoints[RT_MAX_NODES][3], dstRestJoints[RT_MAX_NODES][3];

    if (pState->state == RT_STATE_READY)
        return 1;
    if (pState->state != RT_STATE_RECIPE)
        return 0;

    if (!sithKeyRetarget_LoadSkel(&pState->src, &pState->srcRest, pRecipe->srcModel, pRecipe->bSrcRetail)
        || !sithKeyRetarget_LoadSkel(&pState->dst, &pState->dstRest, pRecipe->dstModel, pRecipe->bDstRetail))
        goto fail;

    sithKeyRetarget_World(&pState->src, &pState->srcRest, srcWorld, pState->srcJoints);
    sithKeyRetarget_World(&pState->dst, &pState->dstRest, dstWorld, pState->dstJoints);
    memcpy(srcRestWorld, srcWorld, sizeof(srcRestWorld));
    memcpy(dstRestWorld, dstWorld, sizeof(dstRestWorld));
    memcpy(srcRestJoints, pState->srcJoints, sizeof(srcRestJoints));
    memcpy(dstRestJoints, pState->dstJoints, sizeof(dstRestJoints));

    pState->numPairs = 0;
    for (int b = 0; b < pRecipe->numBones; b++)
    {
        rtBone* pBone = &pRecipe->bones[b];
        int s = sithKeyRetarget_FindNode(&pState->src, pBone->src);
        int t = sithKeyRetarget_FindNode(&pState->dst, pBone->dst);
        rtMat* pCorr = &pState->corr[pState->numPairs];

        if (s < 0 || t < 0)
        {
            stdPlatform_Printf("OpenJKDF2: Key retarget: no bone `%s` or `%s`\n", pBone->src, pBone->dst);
            goto fail;
        }

        if (pBone->mode == RT_BONE_REF)
        {
            // C = Ws^T * Wt, so Ws * C = Wt at rest
            sithKeyRetarget_MulT(pCorr, &srcRestWorld[s], &dstRestWorld[t]);
        }
        else if (pBone->mode == RT_BONE_DIR)
        {
            int sc = sithKeyRetarget_FindNode(&pState->src, pBone->srcChild);
            int tc = sithKeyRetarget_FindNode(&pState->dst, pBone->dstChild);
            double sd[3], td[3];
            rtMat arc, tmp;

            if (sc < 0 || tc < 0)
            {
                stdPlatform_Printf("OpenJKDF2: Key retarget: no bone `%s` or `%s`\n", pBone->srcChild, pBone->dstChild);
                goto fail;
            }
            for (int i = 0; i < 3; i++)
            {
                sd[i] = srcRestJoints[sc][i] - srcRestJoints[s][i];
                td[i] = dstRestJoints[tc][i] - dstRestJoints[t][i];
            }
            // C = Ws^T * arc(td -> sd) * Wt
            sithKeyRetarget_Arc(&arc, td, sd);
            sithKeyRetarget_Mul(&tmp, &arc, &dstRestWorld[t]);
            sithKeyRetarget_MulT(pCorr, &srcRestWorld[s], &tmp);
        }
        else
        {
            sithKeyRetarget_Identity(pCorr);
        }
        pState->pairSrc[pState->numPairs] = s;
        pState->pairDst[pState->numPairs] = t;
        pState->numPairs++;
    }

    pState->legScale = 1.0;
    if (pRecipe->bHasLegs)
    {
        double srcLen = sithKeyRetarget_LegLength(&pState->src, srcRestJoints, &pRecipe->legs[0]);
        double dstLen = sithKeyRetarget_LegLength(&pState->dst, dstRestJoints, &pRecipe->legs[3]);
        if (srcLen > 1e-9 && dstLen > 1e-9)
            pState->legScale = dstLen / srcLen;
    }

    pState->ground = 0.0;
    if (pRecipe->bHasFeet)
    {
        int feet[2];
        feet[0] = sithKeyRetarget_FindNode(&pState->dst, pRecipe->feet[0]);
        feet[1] = sithKeyRetarget_FindNode(&pState->dst, pRecipe->feet[1]);
        if (feet[0] >= 0 && feet[1] >= 0)
        {
            double restZ = sithKeyRetarget_LowestFoot(pState, &pState->dstRest, feet);
            sithKeyRetarget_Convert(pState, &pState->srcRest, &pState->dstPose);
            pState->ground = restZ - sithKeyRetarget_LowestFoot(pState, &pState->dstPose, feet);
        }
    }

    pState->state = RT_STATE_READY;
    return 1;

fail:
    pState->state = RT_STATE_NONE;
    return 0;
}

// The source key's pose at a frame, as rdPuppet would play it
static void sithKeyRetarget_Sample(const rtSkel* pSkel, const rtPose* pRest, const rdKeyframe* pKey, double frame, rtPose* pOut)
{
    *pOut = *pRest;
    for (uint32_t j = 0; j < pKey->numJoints2 && (int)j < pSkel->numNodes; j++)
    {
        const rdJoint* pJoint = &pKey->aNodes[j];
        const rdAnimEntry* pEntry;
        uint32_t k = 0;
        double dt;

        if (!pJoint->numEntries || !pJoint->aEntries)
            continue;
        while (k + 1 < pJoint->numEntries && (double)pJoint->aEntries[k + 1].frameNum <= frame)
            k++;
        pEntry = &pJoint->aEntries[k];
        dt = frame - (double)pEntry->frameNum;

        pOut->pos[j][0] = pEntry->pos.x;
        pOut->pos[j][1] = pEntry->pos.y;
        pOut->pos[j][2] = pEntry->pos.z;
        pOut->rot[j][0] = pEntry->orientation.x;
        pOut->rot[j][1] = pEntry->orientation.y;
        pOut->rot[j][2] = pEntry->orientation.z;
        if (pEntry->flags & 1)
        {
            pOut->pos[j][0] += pEntry->vel.x * dt;
            pOut->pos[j][1] += pEntry->vel.y * dt;
            pOut->pos[j][2] += pEntry->vel.z * dt;
        }
        if (pEntry->flags & 2)
        {
            pOut->rot[j][0] += pEntry->angularVelocity.x * dt;
            pOut->rot[j][1] += pEntry->angularVelocity.y * dt;
            pOut->rot[j][2] += pEntry->angularVelocity.z * dt;
        }
    }
}

//
// Public
//

void sithKeyRetarget_Reset()
{
    sithKeyRetarget_state.state = RT_STATE_UNREAD;
}

// Damage on at frame 0, then the source's markers in order, without the
// ones that turn damage on or off or start a new list of things hit
static void sithKeyRetarget_DamageAlways(const rdKeyframe* pSrc, rdKeyframe* pKeyframe)
{
    int numMarkers = 0;

    pKeyframe->markers.marker_float[numMarkers] = 0.0;
    pKeyframe->markers.marker_int[numMarkers++] = RT_MARKER_DAMAGE_ON;
    for (uint32_t i = 0; i < pSrc->numMarkers && numMarkers < 8; i++)
    {
        int32_t type = pSrc->markers.marker_int[i];
        if (type == RT_MARKER_DAMAGE_ON || type == RT_MARKER_DAMAGE_OFF || type == RT_MARKER_DAMAGE_RESET)
            continue;
        pKeyframe->markers.marker_float[numMarkers] = pSrc->markers.marker_float[i];
        pKeyframe->markers.marker_int[numMarkers++] = type;
    }
    pKeyframe->numMarkers = numMarkers;
}

static int sithKeyRetarget_Build(const char* pName, rdKeyframe* pKeyframe)
{
    rtState* pState = &sithKeyRetarget_state;
    char name[RT_NAME_LEN];
    char srcPath[RT_PATH_LEN];
    rdKeyframe src;
    double* pFrames = NULL;
    int numNodes, numFrames, keyIdx;

    // The name can be in a line buffer that loading reuses
    stdString_SafeStrCopy(name, pName, sizeof(name));

    keyIdx = sithKeyRetarget_FindKey(name);
    if (keyIdx < 0 || !sithKeyRetarget_Setup(pState))
        return 0;

    memset(&src, 0, sizeof(src));
    stdString_snprintf(srcPath, sizeof(srcPath), "3do\\key\\%s", pState->recipe.srcKeys[keyIdx]);
    if (!rdKeyframe_LoadEntry(srcPath, &src))
        goto fail;

    numNodes = pState->dst.numNodes;
    numFrames = (int)src.numFrames;
    if (numFrames <= 0 || numFrames > RT_MAX_FRAMES)
        goto fail;

    // Every frame of the target pose: pos then rot, per node
    pFrames = (double*)RDROID_ALLOC(sizeof(double) * 6 * numNodes * numFrames);
    if (!pFrames)
        goto fail;
    for (int f = 0; f < numFrames; f++)
    {
        sithKeyRetarget_Sample(&pState->src, &pState->srcRest, &src, (double)f, &pState->srcPose);
        sithKeyRetarget_Convert(pState, &pState->srcPose, &pState->dstPose);
        for (int i = 0; i < numNodes; i++)
        {
            double* pOut = &pFrames[(f * numNodes + i) * 6];
            memcpy(&pOut[0], pState->dstPose.pos[i], sizeof(double) * 3);
            memcpy(&pOut[3], pState->dstPose.rot[i], sizeof(double) * 3);
        }
    }

    rdKeyframe_NewEntry(pKeyframe);
#ifdef SITH_DEBUG_STRUCT_NAMES
    stdString_SafeStrCopy(pKeyframe->name, name, 32);
#endif
#ifdef STDHASHTABLE_CRC32_KEYS
    pKeyframe->namecrc = stdCrc32(name, strlen(name));
#endif
    pKeyframe->flags = src.flags;
    pKeyframe->type = src.type;
    pKeyframe->fps = (flex_t)((double)src.fps * pState->recipe.speeds[keyIdx]);
    pKeyframe->numFrames = src.numFrames;
    pKeyframe->numMarkers = src.numMarkers;
    pKeyframe->markers = src.markers;
    if (pState->recipe.bDamageAlways)
        sithKeyRetarget_DamageAlways(&src, pKeyframe);

    pKeyframe->aNodes = (rdJoint*)RDROID_ALLOC(sizeof(rdJoint) * (numNodes + 1));
    if (!pKeyframe->aNodes)
        goto fail;
    stdPlatform_Memzero32(pKeyframe->aNodes, sizeof(rdJoint) * (numNodes + 1));
    pKeyframe->numJoints = numNodes;
    pKeyframe->numJoints2 = numNodes;

    for (int i = 0; i < numNodes; i++)
    {
        rdJoint* pJoint = &pKeyframe->aNodes[i];

        pJoint->nodeNum = i;
#ifdef SITH_DEBUG_STRUCT_NAMES
        stdString_SafeStrCopy(pJoint->aMeshName, pState->dst.nodes[i].name, 32);
#endif
        pJoint->aEntries = (rdAnimEntry*)RDROID_ALLOC(sizeof(rdAnimEntry) * numFrames + 2);
        if (!pJoint->aEntries)
            goto fail_built;
        pJoint->numEntries = numFrames;

        // An entry a frame, moving linearly to the next
        for (int f = 0; f < numFrames; f++)
        {
            rdAnimEntry* pEntry = &pJoint->aEntries[f];
            const double* pCur = &pFrames[(f * numNodes + i) * 6];
            const double* pNext = (f + 1 < numFrames) ? &pFrames[((f + 1) * numNodes + i) * 6] : pCur;

            pEntry->frameNum = (flex_t)f;
            pEntry->flags = 3;
            pEntry->pos.x = (flex_t)pCur[0];
            pEntry->pos.y = (flex_t)pCur[1];
            pEntry->pos.z = (flex_t)pCur[2];
            pEntry->orientation.x = (flex_t)pCur[3];
            pEntry->orientation.y = (flex_t)pCur[4];
            pEntry->orientation.z = (flex_t)pCur[5];
            pEntry->vel.x = (flex_t)(pNext[0] - pCur[0]);
            pEntry->vel.y = (flex_t)(pNext[1] - pCur[1]);
            pEntry->vel.z = (flex_t)(pNext[2] - pCur[2]);
            pEntry->angularVelocity.x = (flex_t)sithKeyRetarget_WrapAngle(pNext[3] - pCur[3]);
            pEntry->angularVelocity.y = (flex_t)sithKeyRetarget_WrapAngle(pNext[4] - pCur[4]);
            pEntry->angularVelocity.z = (flex_t)sithKeyRetarget_WrapAngle(pNext[5] - pCur[5]);
        }
    }

    RDROID_FREE(pFrames);
    rdKeyframe_FreeEntry(&src);
    stdPlatform_Printf("OpenJKDF2: Key retarget: made `%s` from `%s`, %d frames\n", name, pState->recipe.srcKeys[keyIdx], numFrames);
    return 1;

fail_built:
    rdKeyframe_FreeEntry(pKeyframe);
fail:
    stdPlatform_Printf("OpenJKDF2: Key retarget: couldn't make `%s`\n", name);
    if (pFrames)
        RDROID_FREE(pFrames);
    rdKeyframe_FreeEntry(&src);
    return 0;
}

int sithKeyRetarget_LoadEntry(const char* pName, char* pPath, rdKeyframe* pKeyframe)
{
    if (sithKeyRetarget_FindKey(pName) >= 0)
        return sithKeyRetarget_Build(pName, pKeyframe);
    return rdKeyframe_LoadEntry(pPath, pKeyframe);
}

#endif // SITH_KEY_RETARGET
