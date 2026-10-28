// ?nettest: a scripted player, so a test of the netcode with two windows on
// one computer can be run the same way again. In a match, it plays this
// window's player: sithControl reads its input instead of the keyboard and
// mouse (stdComm_WSRelay_bTestInput).
//
//   ?nettest=strafe        faces the nearest other player with the saber out,
//                          which blocks most saber hits from the front, and
//                          strafes left and right, 600 ms each way.
//                          "strafe:fists" holds fists instead, which block
//                          nothing, and "strafe:400" strafes 400 ms each way
//                          ("strafe:fists,400" for both).
//   ?nettest=attack        closes in on the nearest other player and swings
//                          the saber at it. A weapon after it is used
//                          instead, and the player gets it and its ammo:
//                          "attack:fists", "bryar", "rifle", "thermal",
//                          "bowcaster", "repeater", "rail", "sequencer",
//                          "concussion", or "attack:N" for weapon key N.
//                          ",alt" uses its second fire ("attack:rail,alt").
//                          Explosives are used from far enough away that
//                          their blast misses the attacker, and it backs
//                          away from a sequencer charge it drops.
//
// Both respawn when their player dies. So the two players stay in reach of
// each other on any map, an attacker that's far from its target, or can't see
// it, jumps next to it. The hit ledger (packaging/wasm/netstats.js) counts
// what happens; the page's ?netstats panel shows it.

#include "stdComm_WSRelay.h"

#include "World/sithThing.h"
#include "World/sithActor.h"
#include "Engine/sithCollision.h"
#include "Engine/sithPhysics.h"
#include "Gameplay/sithInventory.h"
#include "Primitives/rdMatrix.h"
#include "Primitives/rdVector.h"
#include "stdPlatform.h"
#include "jk.h"

#include <emscripten.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WSRELAY_TEST_NONE (0)
#define WSRELAY_TEST_STRAFE (1)
#define WSRELAY_TEST_ATTACK (2)

#define WSRELAY_TEST_FIRE_MS (400)      // fire held this long...
#define WSRELAY_TEST_REST_MS (200)      // ...then let go this long
#define WSRELAY_TEST_SELECT_MS (1000)   // between tries at the weapon
#define WSRELAY_TEST_RESPAWN_MS (500)   // between presses of activate, dead
#define WSRELAY_TEST_JUMP_MS (3000)     // between jumps to the target
#define WSRELAY_TEST_UNSEEN_MS (1000)   // out of sight this long: jump
#define WSRELAY_TEST_FAR (2.0)          // further than this: jump
#define WSRELAY_TEST_CLOSE (0.03)       // with the saber or fists, this far
                                        // apart (between the players' edges)
#define WSRELAY_TEST_REACH (0.08)       // ...and swing from this far: the
                                        // blade (0.15 long, weap_saber.cog)
                                        // and a punch (+punch's range of
                                        // 0.15) reach further
#define WSRELAY_TEST_GUN (0.8)          // with a gun, this far apart
#define WSRELAY_TEST_RAIL (1.0)         // a rail detonator's blast reaches
                                        // 0.5 (+raildet_exp)
#define WSRELAY_TEST_CONCUSSION (1.3)   // a concussion bolt's 0.8 (+conc_exp)
#define WSRELAY_TEST_THROW_MS (800)     // a thermal detonator goes further
                                        // the longer fire is held: this lobs
                                        // it about 0.8 (weap_thermdet.cog)
#define WSRELAY_TEST_BACKOFF_MS (2500)  // after dropping a sequencer charge,
                                        // run back from it this long
#define WSRELAY_TEST_REPORT_MS (5000)   // between lines in the console

EM_JS_DEPS(stdComm_WSRelayTest, "$stringToUTF8");

// What ?nettest asks for ("" for nothing), from the page (Module.jkNetTest).
EM_JS(void, wsrelay_js_nettest, (char* pOut, int maxLen), {
    stringToUTF8(Module['jkNetTest'] ? String(Module['jkNetTest']) : "", pOut, maxLen);
});

int stdComm_WSRelay_bTestInput = 0;

static int wsrelay_testMode = -1;       // -1 until ?nettest is read
static int wsrelay_testSelect = INPUT_FUNC_SELECT0;
static int wsrelay_testTries = 0;       // presses of the weapon key, this life
static int wsrelay_testMelee = 1;       // the saber or fists, not a gun
static int wsrelay_testFire = INPUT_FUNC_FIRE1;
static uint32_t wsrelay_testStrafeMs = 600;
static flex_t wsrelay_testCount = 0.0; // of the weapon, last frame

static uint32_t wsrelay_testMsecFire = 0;
static uint32_t wsrelay_testMsecBackOff = 0;
static uint32_t wsrelay_testMsecSelect = 0;
static uint32_t wsrelay_testMsecRespawn = 0;
static uint32_t wsrelay_testMsecJump = 0;
static uint32_t wsrelay_testMsecSeen = 0;
static uint32_t wsrelay_testMsecReport = 0;
static int wsrelay_testWasDead = 0;
static int wsrelay_testJumps = 0;

static int wsrelay_aTestHeld[INPUT_FUNC_MAX];
static int wsrelay_aTestPresses[INPUT_FUNC_MAX];
static flex_t wsrelay_aTestAxis[INPUT_FUNC_MAX];

// The number key of a weapon, by its name or the key itself, or -1. The keys
// pick weapons as in Jedi Knight: 1 fists, 2 Bryar, 3 rifle, 4 thermal
// detonators, 5 bowcaster, 6 repeater, 7 rail detonator, 8 sequencer
// charges, 9 concussion rifle, 0 the saber.
static int wsrelay_TestWeaponKey(const char* pName)
{
    if (!strcmp(pName, "saber")) return 0;
    if (!strcmp(pName, "fists")) return 1;
    if (!strcmp(pName, "bryar")) return 2;
    if (!strcmp(pName, "rifle")) return 3;
    if (!strcmp(pName, "thermal")) return 4;
    if (!strcmp(pName, "bowcaster")) return 5;
    if (!strcmp(pName, "repeater")) return 6;
    if (!strcmp(pName, "rail")) return 7;
    if (!strcmp(pName, "sequencer")) return 8;
    if (!strcmp(pName, "concussion")) return 9;
    if (pName[0] >= '0' && pName[0] <= '9' && !pName[1]) return pName[0] - '0';
    return -1;
}

// How far from the target the attacker stays with the weapon in bin, between
// the players' middles. 0: as close as with the saber, as for a sequencer
// charge, which is dropped at the player's feet.
static flex_t wsrelay_TestDistance(int bin)
{
    switch (bin)
    {
        case SITHBIN_FISTS:
        case SITHBIN_LIGHTSABER:
        case SITHBIN_SEQUENCER_CHARGE:
            return 0.0;
        case SITHBIN_RAIL_DETONATOR:
            return WSRELAY_TEST_RAIL;
        case SITHBIN_CONCUSSION_RIFLE:
            return WSRELAY_TEST_CONCUSSION;
        default:
            return WSRELAY_TEST_GUN;
    }
}

static void wsrelay_TestReadMode()
{
    char text[64];
    char shown[64];
    wsrelay_js_nettest(text, sizeof(text));
    strcpy(shown, text);
    wsrelay_testMode = WSRELAY_TEST_NONE;

    char* pArgs = strchr(text, ':');
    if (pArgs)
        *pArgs++ = 0;
    if (!strcmp(text, "strafe"))
        wsrelay_testMode = WSRELAY_TEST_STRAFE;
    else if (!strcmp(text, "attack"))
        wsrelay_testMode = WSRELAY_TEST_ATTACK;
    else
        return;

    // A weapon, which fire, and for strafe, how long each way.
    int key = 0;
    for (char* pArg = pArgs ? strtok(pArgs, ",") : NULL; pArg; pArg = strtok(NULL, ","))
    {
        if (wsrelay_TestWeaponKey(pArg) >= 0)
            key = wsrelay_TestWeaponKey(pArg);
        else if (!strcmp(pArg, "alt"))
            wsrelay_testFire = INPUT_FUNC_FIRE2;
        else if (wsrelay_testMode == WSRELAY_TEST_STRAFE && atoi(pArg) >= 100)
            wsrelay_testStrafeMs = atoi(pArg);
    }
    wsrelay_testSelect = key ? INPUT_FUNC_SELECT1 + key - 1 : INPUT_FUNC_SELECT0;
    wsrelay_testMelee = key <= 1;

    stdPlatform_Printf("OpenJKDF2: ?nettest: %s plays this window's player in a match.\n", shown);
}

// The nearest other player who's alive, or NULL.
static SithThing* wsrelay_TestTarget(SithThing* pPlayer)
{
    SithThing* pBest = NULL;
    flex_t bestDist = 0.0;
    for (int i = 0; i < jkPlayer_maxPlayers; i++)
    {
        SithThing* pThing = jkPlayer_playerInfos[i].pLocalPlayer;
        if (!(jkPlayer_playerInfos[i].flags & 1) || !pThing || pThing == pPlayer || !pThing->sector
            || (pThing->flags & (SITH_TF_DISABLED|SITH_TF_DEAD|SITH_TF_DESTROYED)))
            continue;
        flex_t dist = rdVector_Dist3(&pThing->position, &pPlayer->position);
        if (!pBest || dist < bestDist)
        {
            pBest = pThing;
            bestDist = dist;
        }
    }
    return pBest;
}

// Whether nothing of the world is between the two players.
static int wsrelay_TestCanSee(SithThing* pPlayer, SithThing* pTarget)
{
    rdVector3 end = pTarget->position;
    if (!sithCollision_FindSectorInRadius(pPlayer->sector, &pPlayer->position, &end, 0.0))
        return 0;
    return rdVector_Dist3(&end, &pTarget->position) < 0.001;
}

// Turns the player to face the target, and tilts its head to look at it.
static void wsrelay_TestFace(SithThing* pPlayer, SithThing* pTarget)
{
    rdVector3 dir;
    rdVector_Sub3(&dir, &pTarget->position, &pPlayer->position);
    flex_t across = (flex_t)sqrt(dir.x * dir.x + dir.y * dir.y);
    if (across < 0.0001)
        return;

    rdVector3 look = {dir.x / across, dir.y / across, 0.0};
    rdMatrix_BuildFromLook34(&pPlayer->orient, &look);
    rdVector_Zero3(&pPlayer->orient.scale);
    rdVector_Zero3(&pPlayer->physicsParams.angularVelocity);

    // From the eyes to the middle of the target.
    flex_t up = dir.z - pPlayer->actorParams.eyeOffset.z;
    rdVector3 head = pPlayer->actorParams.headPYR;
    head.x = (flex_t)(atan2(up, across) * 180.0 / M_PI);
    if (head.x < pPlayer->actorParams.minHeadPitch) head.x = pPlayer->actorParams.minHeadPitch;
    if (head.x > pPlayer->actorParams.maxHeadPitch) head.x = pPlayer->actorParams.maxHeadPitch;
    sithActor_SetHeadPYR(pPlayer, &head);
    // Moving forward can start centring the view (sithControl_PlayerMovement).
    pPlayer->actorParams.flags &= ~SITH_AF_VIEWCENTRING;
}

// Puts the player dist from the target, on the side facing it if there's
// room, else wherever around the target there's the most.
static void wsrelay_TestJumpTo(SithThing* pPlayer, SithThing* pTarget, flex_t dist)
{
    rdVector3 away;
    rdVector_Sub3(&away, &pPlayer->position, &pTarget->position);
    away.z = 0.0;
    if (rdVector_Normalize3Acc(&away) == 0.0)
    {
        away = pTarget->orient.lvec;
        away.z = 0.0;
        if (rdVector_Normalize3Acc(&away) == 0.0)
            away.x = 1.0;
    }

    SithSector* pBestSector = NULL;
    rdVector3 bestPos;
    flex_t bestDist = 0.0;
    for (int i = 0; i < 8; i++)
    {
        flex_t a = (flex_t)(i * M_PI / 4.0);
        rdVector3 dir = {away.x * (flex_t)cos(a) - away.y * (flex_t)sin(a), away.x * (flex_t)sin(a) + away.y * (flex_t)cos(a), 0.0};
        rdVector3 pos;
        rdVector_Copy3(&pos, &pTarget->position);
        rdVector_ScaleAdd3Acc(&pos, &dir, dist);
        // Stops at the first wall, with room for the player.
        SithSector* pSector = sithCollision_FindSectorInRadius(pTarget->sector, &pTarget->position, &pos, pPlayer->collideSize);
        if (!pSector)
            continue;
        flex_t got = rdVector_Dist3(&pos, &pTarget->position);
        if (!pBestSector || got > bestDist)
        {
            pBestSector = pSector;
            bestPos = pos;
            bestDist = got;
        }
        if (got >= dist * 0.95)
            break;
    }
    if (!pBestSector || bestDist < pPlayer->collideSize + pTarget->collideSize)
        return;

    // As sithPlayerActions_MoveToPlayerPosition moves a player.
    pPlayer->position = bestPos;
    sithThing_SetSector(pPlayer, pBestSector, 0);
    sithPhysics_ResetThingMovement(pPlayer);
    sithPhysics_FindFloor(pPlayer, 1);
}

static void wsrelay_TestPress(int inputFunc)
{
    wsrelay_aTestHeld[inputFunc] = 1;
    wsrelay_aTestPresses[inputFunc] = 1;
}

// Gives the player the gun in bin and keeps its ammo up, so a test with a gun
// runs as long as one with the saber: in a match a player starts with the
// Bryar, no other gun, and 50 energy (kyle.cog), about 50 shots. Energy feeds
// the Bryar and the rifle, power the bowcaster, repeater and concussion rifle,
// rail charges the rail detonator, and thermal detonators and sequencer
// charges are their own ammo (items.dat).
static void wsrelay_TestArm(SithThing* pPlayer, int bin)
{
    // Before the last one is used, or its cog picks another weapon.
    flex_t most = sithInventory_g_aTypes[bin].max;
    if (sithInventory_GetInventory(pPlayer, bin) < most * 0.5)
        sithInventory_SetInventory(pPlayer, bin, most);
    if (!sithInventory_IsInventoryAvailable(pPlayer, bin))
        sithInventory_SetInventoryAvailable(pPlayer, bin, 1);

    static const int aAmmo[] = {SITHBIN_ENERGY, SITHBIN_POWER, SITHBIN_RAILCHARGES};
    for (int i = 0; i < (int)(sizeof(aAmmo) / sizeof(aAmmo[0])); i++)
    {
        if (sithInventory_GetInventory(pPlayer, aAmmo[i]) < 100.0)
            sithInventory_SetInventory(pPlayer, aAmmo[i], 200.0);
    }
}

void stdComm_WSRelay_TestTick(SithThing* pPlayer, flex_t deltaSecs)
{
    // Last frame's presses have been read.
    memset(wsrelay_aTestPresses, 0, sizeof(wsrelay_aTestPresses));

    if (wsrelay_testMode < 0)
        wsrelay_TestReadMode();
    stdComm_WSRelay_bTestInput = wsrelay_testMode != WSRELAY_TEST_NONE && pPlayer
                                 && pPlayer == sithPlayer_g_pLocalPlayerThing && stdComm_WSRelay_InMatch();
    if (!stdComm_WSRelay_bTestInput)
        return;

    memset(wsrelay_aTestHeld, 0, sizeof(wsrelay_aTestHeld));
    memset(wsrelay_aTestAxis, 0, sizeof(wsrelay_aTestAxis));
    uint32_t now = stdPlatform_GetTimeMsec();

    // Someone's playing: no idle camera after 30 s (sithControl_Update).
    stdControl_bControlsIdle = 0;

    // Dead: activate respawns, once the game takes it (sithControl_HandlePlayer).
    if (pPlayer->flags & (SITH_TF_DEAD|SITH_TF_DESTROYED))
    {
        wsrelay_testWasDead = 1;
        if (now - wsrelay_testMsecRespawn >= WSRELAY_TEST_RESPAWN_MS)
        {
            wsrelay_testMsecRespawn = now;
            wsrelay_TestPress(INPUT_FUNC_ACTIVATE);
        }
        return;
    }
    if (wsrelay_testWasDead)
    {
        // A new life starts with the default weapon, and its own inventory.
        wsrelay_testWasDead = 0;
        wsrelay_testTries = 0;
        wsrelay_testCount = 0.0;
        wsrelay_testMsecBackOff = 0;
    }

    // The weapon, as many times as it takes, but not forever: this player
    // may not have it. Weapon key N selects bin N, and 0 the saber's, 10.
    int bin = wsrelay_testSelect - INPUT_FUNC_ACTIVATE;
    if (!wsrelay_testMelee)
    {
        // A sequencer charge lands at the player's feet: once one is
        // dropped, run back from it.
        if (bin == SITHBIN_SEQUENCER_CHARGE && sithInventory_GetInventory(pPlayer, bin) < wsrelay_testCount)
            wsrelay_testMsecBackOff = now;
        wsrelay_TestArm(pPlayer, bin);
        wsrelay_testCount = sithInventory_GetInventory(pPlayer, bin);
    }
    int backOff = wsrelay_testMsecBackOff && now - wsrelay_testMsecBackOff < WSRELAY_TEST_BACKOFF_MS;
    if (sithInventory_GetCurrentWeapon(pPlayer) != bin && wsrelay_testTries < 5
        && now - wsrelay_testMsecSelect >= WSRELAY_TEST_SELECT_MS)
    {
        wsrelay_testMsecSelect = now;
        wsrelay_testTries++;
        wsrelay_TestPress(wsrelay_testSelect);
    }

    SithThing* pTarget = wsrelay_TestTarget(pPlayer);
    if (!pTarget)
        return;
    wsrelay_TestFace(pPlayer, pTarget);
    flex_t dist = rdVector_Dist3(&pTarget->position, &pPlayer->position);

    if (wsrelay_testMode == WSRELAY_TEST_STRAFE)
    {
        wsrelay_aTestAxis[INPUT_FUNC_SLIDE] = (now / wsrelay_testStrafeMs) % 2 ? 1.0 : -1.0;
        return;
    }

    // Attack: keep the target in sight and in reach. Distances are between
    // the players' middles, which can't get closer than their sizes allow.
    flex_t touch = pPlayer->collideSize + pTarget->collideSize;
    flex_t away = wsrelay_TestDistance(bin);
    flex_t close = away == 0.0 ? touch + WSRELAY_TEST_CLOSE : away;
    flex_t reach = away == 0.0 ? touch + WSRELAY_TEST_REACH : away * 1.5;
    if (wsrelay_TestCanSee(pPlayer, pTarget))
        wsrelay_testMsecSeen = now;
    int unseen = now - wsrelay_testMsecSeen >= WSRELAY_TEST_UNSEEN_MS;
    if ((dist > WSRELAY_TEST_FAR || unseen) && now - wsrelay_testMsecJump >= WSRELAY_TEST_JUMP_MS)
    {
        wsrelay_testMsecJump = now;
        wsrelay_testMsecSeen = now;
        wsrelay_TestJumpTo(pPlayer, pTarget, close);
        wsrelay_TestFace(pPlayer, pTarget);
        dist = rdVector_Dist3(&pTarget->position, &pPlayer->position);
        wsrelay_testJumps++;
    }

    if (backOff)
    {
        wsrelay_aTestAxis[INPUT_FUNC_FORWARD] = -1.0;
        wsrelay_aTestHeld[INPUT_FUNC_FAST] = 1;
    }
    else if (dist > close)
        wsrelay_aTestAxis[INPUT_FUNC_FORWARD] = 1.0;
    else if (away != 0.0 && dist < close * 0.5)
        wsrelay_aTestAxis[INPUT_FUNC_FORWARD] = -1.0;

    // Fire held a while, then let go, so the weapon's cog hears it again.
    uint32_t hold = bin == SITHBIN_THERMAL_DETONATOR ? WSRELAY_TEST_THROW_MS : WSRELAY_TEST_FIRE_MS;
    uint32_t cycle = hold + WSRELAY_TEST_REST_MS;
    if (now - wsrelay_testMsecFire >= cycle)
        wsrelay_testMsecFire = now;
    if (now - wsrelay_testMsecFire < hold && dist <= reach && !backOff)
        wsrelay_aTestHeld[wsrelay_testFire] = 1;

    // What it's doing, now and then, for whoever is watching the console.
    if (now - wsrelay_testMsecReport >= WSRELAY_TEST_REPORT_MS)
    {
        wsrelay_testMsecReport = now;
        stdPlatform_Printf("OpenJKDF2: ?nettest: target %.2f away (reach %.2f), %s, weapon %d, %d jumps so far\n",
                           (double)dist, (double)reach, unseen ? "out of sight" : "in sight",
                           sithInventory_GetCurrentWeapon(pPlayer), wsrelay_testJumps);
    }
}

int stdComm_WSRelay_TestKey(int inputFunc, int* pPresses)
{
    if (inputFunc < 0 || inputFunc >= INPUT_FUNC_MAX)
        return 0;
    if (pPresses)
        *pPresses = wsrelay_aTestPresses[inputFunc];
    return wsrelay_aTestHeld[inputFunc];
}

flex_t stdComm_WSRelay_TestAxis(int inputFunc)
{
    if (inputFunc < 0 || inputFunc >= INPUT_FUNC_MAX)
        return 0.0;
    return wsrelay_aTestAxis[inputFunc];
}
