// ?nettest: a scripted player, so a test of the netcode with two windows on
// one computer can be run the same way again. In a match, it plays this
// window's player: sithControl reads its input instead of the keyboard and
// mouse (stdComm_WSRelay_bTestInput).
//
//   ?nettest=strafe        faces the nearest other player with the saber out,
//                          which blocks most saber hits from the front, and
//                          strafes left and right, 700 ms each way. The
//                          attacker fires and rests in 600 ms, so its swings
//                          and shots fall at a different point of each
//                          strafe, not the same one every time.
//                          "strafe:fists" holds fists instead, which block
//                          nothing, and "strafe:400" strafes 400 ms each way
//                          ("strafe:fists,400" for both). With "run" (here
//                          or after attack's weapon), the player runs, as
//                          most do with Always run on: "strafe:fists,run".
//   ?nettest=attack        closes in on the nearest other player and swings
//                          the saber at it. A weapon after it is used
//                          instead, and the player gets it and its ammo:
//                          "attack:fists", "bryar", "rifle", "thermal",
//                          "bowcaster", "repeater", "rail", "sequencer",
//                          "concussion", MotS's "flashbomb", "seeker" (its
//                          seeker rail detonator) and "carbonite", or
//                          "attack:N" for weapon key N.
//                          ",alt" uses its second fire ("attack:rail,alt").
//                          Explosives are used from far enough away that
//                          their blast misses the attacker, and it backs
//                          away from a sequencer charge it drops.
//                          A targeted Force power instead ("attack:grip",
//                          "pull", "blinding", "deadlysight", and MotS's
//                          "push", "chainlightning" and "saberthrow") is used
//                          from 0.6 away, at rank 4, and the player gets it
//                          and the mana for it.
//   ?nettest=attack:all    attacks with every weapon in turn, each fire
//                          that's different, and each targeted Force power
//                          (wsrelay_aTestSteps, those this game has), for 60 s
//                          with no added lag, then 60 s as if this window's
//                          connection were slower (the plan's test condition:
//                          100 ms more round trip, 20 ms jitter, 2% loss).
//                          The hit ledger keeps each step's counts apart.
//                          "attack:all,30" takes 30 s a step.
//
// Both respawn when their player dies, though the target heals every frame,
// so only a hit bigger than its health kills it. So the two players stay in
// reach of each other on any map, an attacker that's far from its target, or
// can't see it, jumps next to it. The hit ledger (packaging/wasm/netstats.js)
// counts what happens; the page's ?netstats panel shows it. Both say in the
// console what this game has for the test, and every 5 s where they are and
// where they have the other player.

#include "stdComm_WSRelay.h"

#include "World/sithThing.h"
#include "World/sithActor.h"
#include "World/sithWorld.h"
#include "Engine/sithCollision.h"
#include "Engine/sithPhysics.h"
#include "Gameplay/sithInventory.h"
#include "Main/Main.h"
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
#define WSRELAY_TEST_FORCE (0.6)        // with a Force power: Grip reaches
                                        // 1 (force_grip.cog)
#define WSRELAY_TEST_FORCE_MS (600)     // a power held this long, so its cog
                                        // picks a target (every 0.33 s)...
#define WSRELAY_TEST_FORCE_REST_MS (400) // ...then let go, which uses it
#define WSRELAY_TEST_FORCE_RANK (4.0)
#define WSRELAY_TEST_THROW_MS (800)     // a thermal detonator goes further
                                        // the longer fire is held: this lobs
                                        // it about 0.8 (weap_thermdet.cog)
#define WSRELAY_TEST_BACKOFF_MS (2500)  // after dropping a sequencer charge,
                                        // run back from it this long
#define WSRELAY_TEST_REPORT_MS (5000)   // between lines in the console
#define WSRELAY_TEST_STEP_SECS (60)     // attack:all, each step
#define WSRELAY_TEST_SETTLE_MS (5000)   // ...and no firing for this long
                                        // after it, so its last hits (a
                                        // thermal detonator's 3 s fuse,
                                        // and their receipts) are settled
                                        // before the next step's

EM_JS_DEPS(stdComm_WSRelayTest, "$stringToUTF8");

// What ?nettest asks for ("" for nothing), from the page (Module.jkNetTest).
EM_JS(void, wsrelay_js_nettest, (char* pOut, int maxLen), {
    stringToUTF8(Module['jkNetTest'] ? String(Module['jkNetTest']) : "", pOut, maxLen);
});

// attack:all's next step: the hit ledger keeps the counts so far under the
// last step's name, and starts over with this window's link as slow as sim
// says (as ?netsim does, "" for no added lag).
EM_JS(void, wsrelay_js_nettest_step, (const char* pName, const char* pSim), {
    if (Module['jkNetStats'] && Module['jkNetStats'].nextStep)
        Module['jkNetStats'].nextStep(UTF8ToString(pName), UTF8ToString(pSim));
});

// The weapons the test can use, by name, and each one's bin in Jedi Knight
// and in MotS (0 where that game has none). Jedi Knight's bin 5 is its
// bowcaster.
static const struct
{
    const char* pName;
    int jk;
    int mots;
} wsrelay_aTestWeapons[] = {
    {"saber", SITHBIN_LIGHTSABER, SITHBIN_MOTS_LIGHTSABER},
    {"fists", SITHBIN_FISTS, SITHBIN_MOTS_FISTS},
    {"bryar", SITHBIN_BRYARPISTOL, SITHBIN_MOTS_BRYARPISTOL},
    {"rifle", SITHBIN_STORMTROOPER_RIFLE, SITHBIN_MOTS_STORMTROOPER_RIFLE},
    {"thermal", SITHBIN_THERMAL_DETONATOR, SITHBIN_MOTS_THERMAL_DETONATOR},
    {"bowcaster", SITHBIN_TUSKEN_PROD, 0},
    {"repeater", SITHBIN_REPEATER, SITHBIN_MOTS_REPEATER},
    {"rail", SITHBIN_RAIL_DETONATOR, SITHBIN_MOTS_RAIL_DETONATOR},
    {"sequencer", SITHBIN_SEQUENCER_CHARGE, SITHBIN_MOTS_SEQUENCER_CHARGE},
    {"concussion", SITHBIN_CONCUSSION_RIFLE, SITHBIN_MOTS_CONCUSSION_RIFLE},
    {"flashbomb", 0, SITHBIN_MOTS_FLASH_BOMB},
    {"seeker", 0, SITHBIN_MOTS_RAIL_SEEKER},
    {"carbonite", 0, SITHBIN_MOTS_CARBO_GUN},
};
#define WSRELAY_TEST_NUM_WEAPONS ((int)(sizeof(wsrelay_aTestWeapons) / sizeof(wsrelay_aTestWeapons[0])))

// The targeted Force powers, by name, and their bins in each game.
static const struct
{
    const char* pName;
    int jk;
    int mots;
} wsrelay_aTestPowers[] = {
    {"grip", SITHBIN_F_GRIP, SITHBIN_F_GRIP},
    {"pull", SITHBIN_F_PULL, SITHBIN_F_PULL},
    {"blinding", SITHBIN_F_BLINDING, SITHBIN_F_BLINDING},
    {"deadlysight", SITHBIN_F_DEADLYSIGHT, SITHBIN_F_DEADLYSIGHT},
    {"push", 0, SITHBIN_F_PUSH},
    {"chainlightning", 0, SITHBIN_F_CHAINLIGHT},
    {"saberthrow", 0, SITHBIN_F_SABERTHROW},
};
#define WSRELAY_TEST_NUM_POWERS ((int)(sizeof(wsrelay_aTestPowers) / sizeof(wsrelay_aTestPowers[0])))

// attack:all's steps: each weapon, and its second fire where that's another
// shot (the Bryar, rifle and fists fire the same way with either), then each
// targeted Force power (with the saber in hand). The saber is there to
// compare with. Explosives come last, and last of all the mines a sequencer
// charge's second fire leaves, which wait 60 s for someone to touch them
// (+seqchrg2), so no other step counts their blasts. A step whose weapon or
// power this game doesn't have is left out: the bowcaster in MotS, and
// MotS's own weapons and powers in Jedi Knight.
static const struct
{
    const char* pWeapon;
    int alt;
    const char* pPower;
    const char* pName;
} wsrelay_aTestSteps[] = {
    {"saber", 0, NULL, "saber"}, {"fists", 0, NULL, "fists"}, {"bryar", 0, NULL, "bryar"}, {"rifle", 0, NULL, "rifle"},
    {"bowcaster", 0, NULL, "bowcaster"}, {"bowcaster", 1, NULL, "bowcaster alt"},
    {"repeater", 0, NULL, "repeater"}, {"repeater", 1, NULL, "repeater alt"},
    {"carbonite", 0, NULL, "carbonite"}, {"carbonite", 1, NULL, "carbonite alt"},
    {"saber", 0, "grip", "grip"}, {"saber", 0, "pull", "pull"}, {"saber", 0, "blinding", "blinding"},
    {"saber", 0, "deadlysight", "deadly sight"}, {"saber", 0, "push", "push"},
    {"saber", 0, "chainlightning", "chain lightning"}, {"saber", 0, "saberthrow", "saber throw"},
    {"concussion", 0, NULL, "concussion"}, {"concussion", 1, NULL, "concussion alt"},
    {"thermal", 0, NULL, "thermal"}, {"thermal", 1, NULL, "thermal alt"},
    {"flashbomb", 0, NULL, "flash bomb"}, {"flashbomb", 1, NULL, "flash bomb alt"},
    {"rail", 0, NULL, "rail"}, {"rail", 1, NULL, "rail alt"}, {"seeker", 0, NULL, "seeker"}, {"seeker", 1, NULL, "seeker alt"},
    {"sequencer", 0, NULL, "sequencer"}, {"sequencer", 1, NULL, "sequencer alt"},
};
#define WSRELAY_TEST_NUM_STEPS ((int)(sizeof(wsrelay_aTestSteps) / sizeof(wsrelay_aTestSteps[0])))

// ...each with no added lag, then the plan's test condition.
static const char* wsrelay_aTestLags[] = {"", "rtt:100,jitter:20,loss:2"};

int stdComm_WSRelay_bTestInput = 0;

static int wsrelay_testMode = -1;       // -1 until ?nettest is read
static int wsrelay_testBin = 0;         // the weapon's bin
static int wsrelay_testTries = 0;       // presses of its key since it was in hand
static int wsrelay_testFire = INPUT_FUNC_FIRE1;
static int wsrelay_testPower = 0;       // a Force power's bin, used instead
static uint32_t wsrelay_testStrafeMs = 700;
static int wsrelay_testRun = 0;         // the run key held throughout
static flex_t wsrelay_testCount = 0.0; // of the weapon, last frame
static int wsrelay_testCycle = 0;       // attack:all
static int wsrelay_testStepSecs = WSRELAY_TEST_STEP_SECS;
static int wsrelay_testStep = -1;       // its step and lag, -1 before the first
static uint32_t wsrelay_testMsecStep = 0;

static uint32_t wsrelay_testMsecFire = 0;
static uint32_t wsrelay_testMsecBackOff = 0;
static uint32_t wsrelay_testMsecSelect = 0;
static uint32_t wsrelay_testMsecRespawn = 0;
static uint32_t wsrelay_testMsecJump = 0;
static uint32_t wsrelay_testMsecSeen = 0;
static uint32_t wsrelay_testMsecReport = 0;
static int wsrelay_testWasDead = 0;
static int wsrelay_testJumps = 0;
static int wsrelay_testDescribed = 0;

static int wsrelay_aTestHeld[INPUT_FUNC_MAX];
static int wsrelay_aTestPresses[INPUT_FUNC_MAX];
static flex_t wsrelay_aTestAxis[INPUT_FUNC_MAX];

// Whether this game has the weapon or Force power in bin: its items.dat lists
// it (sithInventory_g_aTypes), with a cog that uses it.
static int wsrelay_TestHas(int bin)
{
    return bin > 0 && bin < SITHBIN_NUMBINS && (sithInventory_g_aTypes[bin].flags & SITHINVENTORY_TYPE_REGISTERED)
           && sithInventory_g_aTypes[bin].cog;
}

// The bin of a weapon in this game, by its name or its number key ("0" to
// "9"), 0 for one this game doesn't have, or -1 for neither. Key N is for
// the game's Nth weapon, and 0 for its tenth (sithWeapon_HandleWeaponKeys).
static int wsrelay_TestWeaponBin(const char* pName)
{
    for (int i = 0; i < WSRELAY_TEST_NUM_WEAPONS; i++)
    {
        if (!strcmp(pName, wsrelay_aTestWeapons[i].pName))
            return Main_bMotsCompat ? wsrelay_aTestWeapons[i].mots : wsrelay_aTestWeapons[i].jk;
    }
    if (pName[0] >= '0' && pName[0] <= '9' && !pName[1])
    {
        int bin = sithInventory_SelectWeaponFollowing(pName[0] == '0' ? 10 : pName[0] - '0');
        return bin > 0 ? bin : 0;
    }
    return -1;
}

// The bin of a targeted Force power in this game, by its name, 0 for one this
// game doesn't have, or -1 for no power's name.
static int wsrelay_TestPowerBin(const char* pName)
{
    for (int i = 0; i < WSRELAY_TEST_NUM_POWERS; i++)
    {
        if (!strcmp(pName, wsrelay_aTestPowers[i].pName))
            return Main_bMotsCompat ? wsrelay_aTestPowers[i].mots : wsrelay_aTestPowers[i].jk;
    }
    return -1;
}

// The number key that picks the weapon in bin, or 0 for none. A game's keys
// count its weapons in bin order (sithInventory_SelectWeaponPrior): Jedi
// Knight's key N picks bin N, and 0 the saber's, 10. Each of MotS's keys
// picks two weapons in turn, its Nth and the one ten after it (1: the saber,
// then fists), so the test presses until the one it wants is in hand.
static int wsrelay_TestKeyFor(int bin)
{
    int n = sithInventory_SelectWeaponPrior(bin);
    if (n < 1 || n > 20)
        return 0;
    n %= 10;
    return n ? INPUT_FUNC_SELECT1 + n - 1 : INPUT_FUNC_SELECT0;
}

// Whether the weapon in bin is the saber or fists, which need no ammo.
static int wsrelay_TestMelee(int bin)
{
    return bin == SITHBIN_FISTS || bin == SITHBIN_LIGHTSABER || bin == SITHBIN_MOTS_FISTS
           || bin == SITHBIN_MOTS_LIGHTSABER;
}

// Whether the weapon in bin is thrown, further the longer fire is held.
static int wsrelay_TestThrown(int bin)
{
    return bin == SITHBIN_THERMAL_DETONATOR || bin == SITHBIN_MOTS_THERMAL_DETONATOR
           || bin == SITHBIN_MOTS_FLASH_BOMB;
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
        case SITHBIN_MOTS_FISTS:
        case SITHBIN_MOTS_LIGHTSABER:
        case SITHBIN_MOTS_SEQUENCER_CHARGE:
            return 0.0;
        case SITHBIN_RAIL_DETONATOR:
        case SITHBIN_MOTS_RAIL_DETONATOR:
        case SITHBIN_MOTS_RAIL_SEEKER:
            return WSRELAY_TEST_RAIL;
        case SITHBIN_CONCUSSION_RIFLE:
        case SITHBIN_MOTS_CONCUSSION_RIFLE:
            return WSRELAY_TEST_CONCUSSION;
        default:
            return WSRELAY_TEST_GUN;
    }
}

// Attack with the weapon in bin, and its second fire if alt, or with the Force
// power in bin power, that weapon in hand.
static void wsrelay_TestUse(int bin, int alt, int power)
{
    wsrelay_testBin = bin;
    wsrelay_testFire = alt ? INPUT_FUNC_FIRE2 : INPUT_FUNC_FIRE1;
    wsrelay_testPower = power;
    wsrelay_testTries = 0;
    wsrelay_testCount = 0.0;
    wsrelay_testMsecBackOff = 0;
}

// Whether this game has step i's weapon, and its Force power if it has one.
static int wsrelay_TestStepHere(int i)
{
    if (!wsrelay_TestHas(wsrelay_TestWeaponBin(wsrelay_aTestSteps[i].pWeapon)))
        return 0;
    return !wsrelay_aTestSteps[i].pPower || wsrelay_TestHas(wsrelay_TestPowerBin(wsrelay_aTestSteps[i].pPower));
}

// attack:all: moves on to the next step when this one's time is up and its
// hits are settled. Whether to hold fire now: while they settle, and after the
// last step.
static int wsrelay_TestCycle(uint32_t now)
{
    int numSteps = WSRELAY_TEST_NUM_STEPS * 2;
    uint32_t stepMs = (uint32_t)wsrelay_testStepSecs * 1000;
    if (wsrelay_testStep >= numSteps)
        return 1;
    if (wsrelay_testStep >= 0 && now - wsrelay_testMsecStep < stepMs)
        return 0;
    if (wsrelay_testStep >= 0 && now - wsrelay_testMsecStep < stepMs + WSRELAY_TEST_SETTLE_MS)
        return 1;

    // The next step this game has.
    do
        wsrelay_testStep++;
    while (wsrelay_testStep < numSteps && !wsrelay_TestStepHere(wsrelay_testStep / 2));
    wsrelay_testMsecStep = now;
    if (wsrelay_testStep >= numSteps)
    {
        wsrelay_js_nettest_step("", "");
        stdPlatform_Printf("OpenJKDF2: ?nettest: every step is done.\n");
        return 1;
    }

    // Numbered among this game's steps.
    int i = wsrelay_testStep / 2;
    int numHere = 0;
    int before = 0;
    for (int j = 0; j < WSRELAY_TEST_NUM_STEPS; j++)
    {
        if (!wsrelay_TestStepHere(j))
            continue;
        numHere += 2;
        if (j < i)
            before += 2;
    }
    const char* pLag = wsrelay_aTestLags[wsrelay_testStep % 2];
    const char* pPower = wsrelay_aTestSteps[i].pPower;
    wsrelay_TestUse(wsrelay_TestWeaponBin(wsrelay_aTestSteps[i].pWeapon), wsrelay_aTestSteps[i].alt,
                    pPower ? wsrelay_TestPowerBin(pPower) : 0);
    wsrelay_js_nettest_step(wsrelay_aTestSteps[i].pName, pLag);
    stdPlatform_Printf("OpenJKDF2: ?nettest: step %d of %d, %s, %s.\n", before + wsrelay_testStep % 2 + 1, numHere,
                       wsrelay_aTestSteps[i].pName, pLag[0] ? pLag : "no added lag");
    return 0;
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

    // A weapon, which fire, a Force power, or all of them, and how long: for
    // strafe, each way (ms), and for attack:all, each step (s).
    // One this game doesn't have is left out: the saber instead of a weapon,
    // and no Force power.
    int saber = wsrelay_TestWeaponBin("saber");
    int bin = saber;
    int alt = 0;
    int power = 0;
    for (char* pArg = pArgs ? strtok(pArgs, ",") : NULL; pArg; pArg = strtok(NULL, ","))
    {
        if (wsrelay_TestWeaponBin(pArg) >= 0)
        {
            bin = wsrelay_TestWeaponBin(pArg);
            if (!wsrelay_TestHas(bin))
            {
                stdPlatform_Printf("OpenJKDF2: ?nettest: this game has no %s, so the saber instead.\n", pArg);
                bin = saber;
            }
        }
        else if (wsrelay_TestPowerBin(pArg) >= 0 && wsrelay_testMode == WSRELAY_TEST_ATTACK)
        {
            power = wsrelay_TestPowerBin(pArg);
            if (!wsrelay_TestHas(power))
            {
                stdPlatform_Printf("OpenJKDF2: ?nettest: this game has no %s.\n", pArg);
                power = 0;
            }
        }
        else if (!strcmp(pArg, "alt"))
            alt = 1;
        else if (!strcmp(pArg, "run"))
            wsrelay_testRun = 1;
        else if (!strcmp(pArg, "all") && wsrelay_testMode == WSRELAY_TEST_ATTACK)
            wsrelay_testCycle = 1;
        else if (wsrelay_testMode == WSRELAY_TEST_STRAFE && atoi(pArg) >= 100)
            wsrelay_testStrafeMs = atoi(pArg);
        else if (wsrelay_testMode == WSRELAY_TEST_ATTACK && atoi(pArg) >= 5)
            wsrelay_testStepSecs = atoi(pArg);
    }
    wsrelay_TestUse(bin, alt, power);

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
// charges are their own ammo (items.dat). MotS adds carbonite pellets and
// seeker rails, which Jedi Knight's items.dat doesn't list.
static void wsrelay_TestArm(SithThing* pPlayer, int bin)
{
    // Before the last one is used, or its cog picks another weapon.
    flex_t most = sithInventory_g_aTypes[bin].max;
    if (sithInventory_GetInventory(pPlayer, bin) < most * 0.5)
        sithInventory_SetInventory(pPlayer, bin, most);
    if (!sithInventory_IsInventoryAvailable(pPlayer, bin))
        sithInventory_SetInventoryAvailable(pPlayer, bin, 1);

    static const int aAmmo[] = {SITHBIN_ENERGY, SITHBIN_POWER, SITHBIN_RAILCHARGES, SITHBIN_CARBPELLETS,
                                SITHBIN_SEEKRAILS};
    int numAmmo = Main_bMotsCompat ? 5 : 3;
    for (int i = 0; i < numAmmo; i++)
    {
        if (sithInventory_GetInventory(pPlayer, aAmmo[i]) < 100.0)
            sithInventory_SetInventory(pPlayer, aAmmo[i], 200.0);
    }
}

// Gives the player the targeted Force power in bin at rank 4, picked, and
// keeps their mana up: Deadly Sight takes 300 of it (force_deadlysight.cog),
// and the most they can have is 50 a Jedi rank (force_well.cog).
static void wsrelay_TestForce(SithThing* pPlayer, int bin)
{
    if (sithInventory_GetInventory(pPlayer, bin) < WSRELAY_TEST_FORCE_RANK)
        sithInventory_SetInventory(pPlayer, bin, WSRELAY_TEST_FORCE_RANK);
    if (!sithInventory_IsInventoryAvailable(pPlayer, bin))
        sithInventory_SetInventoryAvailable(pPlayer, bin, 1);
    if (sithInventory_GetInventory(pPlayer, SITHBIN_JEDI_RANK) < 8.0)
        sithInventory_SetInventory(pPlayer, SITHBIN_JEDI_RANK, 8.0);
    if (sithInventory_GetInventory(pPlayer, SITHBIN_FORCEMANA) < 350.0)
        sithInventory_SetInventory(pPlayer, SITHBIN_FORCEMANA, 400.0);
    if (pPlayer->actorParams.pPlayer && pPlayer->actorParams.pPlayer->curPower != bin)
        sithInventory_SetCurPower(pPlayer, bin);
}

// The number on a weapon key (wsrelay_TestKeyFor), or -1 for none.
static int wsrelay_TestKeyNumber(int inputFunc)
{
    if (inputFunc == INPUT_FUNC_SELECT0)
        return 0;
    return inputFunc ? inputFunc - INPUT_FUNC_SELECT1 + 1 : -1;
}

// What this game has for the test, once, for whoever reads the console: each
// player slot's thing, and the test's weapons, Force powers and ammo with
// their bins, or that this game doesn't have them, and any weapon it has that
// the test leaves out.
static void wsrelay_TestDescribe(SithThing* pPlayer)
{
    stdPlatform_Printf("OpenJKDF2: ?nettest: %s, this player is thing %d.\n", Main_bMotsCompat ? "MotS" : "Jedi Knight",
                       (int)pPlayer->idx);
    for (int i = 0; i < jkPlayer_maxPlayers; i++)
    {
        SithThing* pThing = jkPlayer_playerInfos[i].pLocalPlayer;
        if (pThing)
            stdPlatform_Printf("OpenJKDF2: ?nettest: slot %d (flags %x): thing %d, type %d, flags %x, at %.3f %.3f %.3f\n",
                               i, jkPlayer_playerInfos[i].flags, (int)pThing->idx, (int)pThing->type, pThing->flags,
                               (double)pThing->position.x, (double)pThing->position.y, (double)pThing->position.z);
    }
    for (int i = 0; i < WSRELAY_TEST_NUM_WEAPONS; i++)
    {
        int bin = wsrelay_TestWeaponBin(wsrelay_aTestWeapons[i].pName);
        if (wsrelay_TestHas(bin))
            stdPlatform_Printf("OpenJKDF2: ?nettest: %s: bin %d, key %d, most %.0f\n", wsrelay_aTestWeapons[i].pName, bin,
                               wsrelay_TestKeyNumber(wsrelay_TestKeyFor(bin)), (double)sithInventory_g_aTypes[bin].max);
        else
            stdPlatform_Printf("OpenJKDF2: ?nettest: %s: not in this game\n", wsrelay_aTestWeapons[i].pName);
    }
    // Any other weapon this game lists, which the test leaves out.
    for (int bin = 1; bin < SITHBIN_NUMBINS; bin++)
    {
        if (!(sithInventory_g_aTypes[bin].flags & ITEMINFO_WEAPON) || !wsrelay_TestHas(bin))
            continue;
        int i = 0;
        while (i < WSRELAY_TEST_NUM_WEAPONS && wsrelay_TestWeaponBin(wsrelay_aTestWeapons[i].pName) != bin)
            i++;
        if (i == WSRELAY_TEST_NUM_WEAPONS)
            stdPlatform_Printf("OpenJKDF2: ?nettest: bin %d: a weapon this game lists that the test leaves out, key %d\n",
                               bin, wsrelay_TestKeyNumber(wsrelay_TestKeyFor(bin)));
    }
    for (int i = 0; i < WSRELAY_TEST_NUM_POWERS; i++)
    {
        int bin = wsrelay_TestPowerBin(wsrelay_aTestPowers[i].pName);
        if (wsrelay_TestHas(bin))
            stdPlatform_Printf("OpenJKDF2: ?nettest: %s: bin %d\n", wsrelay_aTestPowers[i].pName, bin);
        else
            stdPlatform_Printf("OpenJKDF2: ?nettest: %s: not in this game\n", wsrelay_aTestPowers[i].pName);
    }
    static const int aBins[] = {SITHBIN_ENERGY, SITHBIN_POWER, SITHBIN_RAILCHARGES, SITHBIN_CARBPELLETS,
                                SITHBIN_SEEKRAILS, SITHBIN_FORCEMANA, SITHBIN_JEDI_RANK};
    for (int i = 0; i < (int)(sizeof(aBins) / sizeof(aBins[0])); i++)
    {
        SithInventoryType* pType = &sithInventory_g_aTypes[aBins[i]];
        stdPlatform_Printf("OpenJKDF2: ?nettest: bin %d: %s, flags %x, most %.0f, %s\n", aBins[i],
                           (pType->flags & SITHINVENTORY_TYPE_REGISTERED) ? "listed" : "not listed", pType->flags,
                           (double)pType->max, pType->cog ? "with a cog" : "no cog");
    }
}

// Where this player is, and where it has the target, for whoever reads the
// console: the two games' lines, side by side, show whether each has the
// other where it is. And any other player thing in the world: in MotS the
// test player once measured 1.47 to its target while the two windows showed
// the players touching, as if the target it measured weren't the one drawn.
static void wsrelay_TestReportWhere(SithThing* pPlayer, SithThing* pTarget)
{
    intptr_t slot = pTarget->actorParams.pPlayer ? pTarget->actorParams.pPlayer - jkPlayer_playerInfos : -1;
    stdPlatform_Printf("OpenJKDF2: ?nettest: here %.3f %.3f %.3f (thing %d), the target %.3f %.3f %.3f (thing %d, slot %d)\n",
                       (double)pPlayer->position.x, (double)pPlayer->position.y, (double)pPlayer->position.z,
                       (int)pPlayer->idx, (double)pTarget->position.x, (double)pTarget->position.y,
                       (double)pTarget->position.z, (int)pTarget->idx, (int)slot);

    SithWorld* pWorld = sithWorld_g_pCurrentWorld;
    for (int i = 0; pWorld && pWorld->aThings && i <= pWorld->numThings; i++)
    {
        SithThing* pThing = &pWorld->aThings[i];
        if (pThing->type != SITH_THING_PLAYER || pThing == pPlayer || pThing == pTarget || !pThing->sector
            || (pThing->flags & (SITH_TF_DISABLED|SITH_TF_DESTROYED)))
            continue;
        slot = pThing->actorParams.pPlayer ? pThing->actorParams.pPlayer - jkPlayer_playerInfos : -1;
        stdPlatform_Printf("OpenJKDF2: ?nettest: another player thing %d (slot %d, flags %x) at %.3f %.3f %.3f, %.2f away\n",
                           i, (int)slot, pThing->flags, (double)pThing->position.x, (double)pThing->position.y,
                           (double)pThing->position.z, (double)rdVector_Dist3(&pThing->position, &pPlayer->position));
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
    if (!wsrelay_testDescribed)
    {
        wsrelay_testDescribed = 1;
        wsrelay_TestDescribe(pPlayer);
    }

    // Someone's playing: no idle camera after 30 s (sithControl_Update).
    stdControl_bControlsIdle = 0;
    wsrelay_aTestHeld[INPUT_FUNC_FAST] = wsrelay_testRun;

    // attack:all's steps go by the clock, alive or not.
    int quiet = wsrelay_testCycle && wsrelay_TestCycle(now);

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

    // The target heals, so a test isn't held up while it respawns.
    if (wsrelay_testMode == WSRELAY_TEST_STRAFE)
        pPlayer->actorParams.health = pPlayer->actorParams.maxHealth;

    // The weapon, pressing its key as many times as it takes, but not
    // forever: this player may not have it. Other things switch weapons too
    // (a picked-up backpack has), so the tries start over whenever it's in
    // hand.
    int bin = wsrelay_testBin;
    if (!wsrelay_TestMelee(bin))
    {
        // A sequencer charge lands at the player's feet: once one is
        // dropped, run back from it.
        if ((bin == SITHBIN_SEQUENCER_CHARGE || bin == SITHBIN_MOTS_SEQUENCER_CHARGE)
            && sithInventory_GetInventory(pPlayer, bin) < wsrelay_testCount)
            wsrelay_testMsecBackOff = now;
        wsrelay_TestArm(pPlayer, bin);
        wsrelay_testCount = sithInventory_GetInventory(pPlayer, bin);
    }
    if (wsrelay_testPower)
        wsrelay_TestForce(pPlayer, wsrelay_testPower);
    int backOff = wsrelay_testMsecBackOff && now - wsrelay_testMsecBackOff < WSRELAY_TEST_BACKOFF_MS;
    if (sithInventory_GetCurrentWeapon(pPlayer) == bin)
        wsrelay_testTries = 0;
    else if (wsrelay_testTries < 5 && now - wsrelay_testMsecSelect >= WSRELAY_TEST_SELECT_MS && wsrelay_TestKeyFor(bin))
    {
        wsrelay_testMsecSelect = now;
        wsrelay_testTries++;
        wsrelay_TestPress(wsrelay_TestKeyFor(bin));
    }

    SithThing* pTarget = wsrelay_TestTarget(pPlayer);
    if (!pTarget)
        return;
    wsrelay_TestFace(pPlayer, pTarget);
    flex_t dist = rdVector_Dist3(&pTarget->position, &pPlayer->position);

    if (wsrelay_testMode == WSRELAY_TEST_STRAFE)
    {
        wsrelay_aTestAxis[INPUT_FUNC_SLIDE] = (now / wsrelay_testStrafeMs) % 2 ? 1.0 : -1.0;
        if (now - wsrelay_testMsecReport >= WSRELAY_TEST_REPORT_MS)
        {
            wsrelay_testMsecReport = now;
            stdPlatform_Printf("OpenJKDF2: ?nettest: strafing, the nearest other player %.2f away\n", (double)dist);
            wsrelay_TestReportWhere(pPlayer, pTarget);
        }
        return;
    }

    // Attack: keep the target in sight and in reach. Distances are between
    // the players' middles, which can't get closer than their sizes allow.
    flex_t touch = pPlayer->collideSize + pTarget->collideSize;
    flex_t away = wsrelay_testPower ? WSRELAY_TEST_FORCE : wsrelay_TestDistance(bin);
    flex_t close = away == 0.0 ? touch + WSRELAY_TEST_CLOSE : away;
    flex_t reach = away == 0.0 ? touch + WSRELAY_TEST_REACH : away * 1.5;
    int inSight = wsrelay_TestCanSee(pPlayer, pTarget);
    if (inSight)
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
    // Only with the target in sight: a rail detonator fired into a corner
    // next to the attacker blows it up. And only with this weapon in hand,
    // so a new life's Bryar doesn't count as another weapon's. A Force power
    // the same, with any weapon in hand: it's used when let go.
    uint32_t hold = wsrelay_TestThrown(bin) ? WSRELAY_TEST_THROW_MS : WSRELAY_TEST_FIRE_MS;
    uint32_t rest = WSRELAY_TEST_REST_MS;
    if (wsrelay_testPower)
    {
        hold = WSRELAY_TEST_FORCE_MS;
        rest = WSRELAY_TEST_FORCE_REST_MS;
    }
    if (now - wsrelay_testMsecFire >= hold + rest)
        wsrelay_testMsecFire = now;
    int inHand = wsrelay_testPower || sithInventory_GetCurrentWeapon(pPlayer) == bin;
    if (now - wsrelay_testMsecFire < hold && dist <= reach && inSight && inHand && !backOff && !quiet)
        wsrelay_aTestHeld[wsrelay_testPower ? INPUT_FUNC_USESKILL : wsrelay_testFire] = 1;

    // What it's doing, now and then, for whoever is watching the console.
    if (now - wsrelay_testMsecReport >= WSRELAY_TEST_REPORT_MS)
    {
        wsrelay_testMsecReport = now;
        stdPlatform_Printf("OpenJKDF2: ?nettest: target %.2f away (reach %.2f), %s, weapon %d, power %d, %d jumps so far\n",
                           (double)dist, (double)reach, unseen ? "out of sight" : "in sight",
                           sithInventory_GetCurrentWeapon(pPlayer), wsrelay_testPower, wsrelay_testJumps);
        wsrelay_TestReportWhere(pPlayer, pTarget);
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
