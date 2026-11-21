#include "Gameplay/sithDuelCombat.h"
#include "globals.h"
#include "Gameplay/jkSaber.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int sithNet_isMulti = 1, Main_bMotsCompat, sithWeapon_8BD024 = -1;
SithPlayer jkPlayer_playerInfos[JKPLAYER_NUM_INFOS];
SithInventoryType sithInventory_g_aTypes[SITHBIN_NUMBINS];
static SithThing players[3];
SithThing *sithPlayer_g_pLocalPlayerThing = &players[0];
static int phase, resets, disables;
static sithCog cog;
int sithInventory_bUnkPower, sithInventory_g_bSendDeactivateMessage;
SithControlBinding sithInventory_powerKeybinds[SITHINVENTORY_NUM_POWERKEYBINDS];
int sithWeapon_ProcessWeaponControls(SithThing *p, flex_t dt) { (void)p; (void)dt; return 0; }
int sithDuel_PhaseForPlayer(int slot) { return slot >= 0 && slot <= 1 ? phase : 0; }
int sithDuel_AreOpponents(int a, int b) { return phase && ((a == 0 && b == 1) || (a == 1 && b == 0)); }
void jkSaber_Disable(SithThing *p) { (void)p; disables++; }
void sithWeapon_StartupEntry(void) { resets++; }
void sithPhysics_ResetThingMovement(SithThing *p) { (void)p; }
SithThing *sithThing_GetThingParent(SithThing *p)
{
    while (p->pParent && p->pParent->signature == p->parentSignature) p = p->pParent;
    return p;
}
flex_t sithInventory_SetInventory(SithThing *p, int bin, flex_t amount)
{
    return p->actorParams.pPlayer->aItems[bin].amount = amount;
}

/* Drive the actual jkSaber_UpdateCollision2 loop with two intersections.
   These actors do not autoblock. An unexpected animation or COG callback
   means a contact crossed the duel boundary. */
static SithCollision contacts[2];
static int nextContact, wallSparks, bloodSparks, releases;
static SithThing *struck;
static SithSector sector;
static SithCollision *sithCollision_PopStack(void)
{
    return nextContact < 2 ? &contacts[nextContact++] : NULL;
}

static void jkSaberTraceSparks(jkPlayerInfo *info, rdVector3 *pos, SithSector *sec, int kind)
{
    (void)info; (void)pos; (void)sec;
    if (kind == SPARKTYPE_WALL) wallSparks++;
    if (kind == SPARKTYPE_BLOOD) bloodSparks++;
}

static void jkSaberTraceDamage(SithThing *target, SithThing *source, flex_t damage)
{
    assert(sithDuelCombat_AllowsDamage(target, source, 1));
    (void)damage;
    struck = target;
}

/* Presentation/geometry is outside the fixture. The supplied contacts
   already have distances, and every actual hit is captured above. */
#define sithCollision_FindSectorInRadius(...) (&sector)
#define sithCollision_SearchForCollisions(...) ((void)0)
#define sithCollision_DecreaseStackLevel() (++releases)
#define rdVector_Copy3(out, in) (*(out) = *(in))
#define rdVector_ScaleAdd3Acc(out, in, dist) ((void)(out), (void)(in), (void)(dist))
#define rdMatrix_Copy34(out, in) (*(out) = *(in))
#define rdMatrix_PreRotate34(mat, vec) ((void)(mat), (void)(vec))
#define rdVector_Sub3(out, a, b) ((void)(out), (void)(a), (void)(b))
#define rdVector_Normalize3Acc(vec) ((void)(vec))
#define rdVector_Dot3(a, b) ((void)(a), (void)(b), 0.0)
#define _frand() 1.0
#define sithSoundClass_PlayModeRandom(...) assert(0)
#define sithPuppet_PlayMode(...) assert(0)
#define stdComm_WSRelay_NoteHit(...) assert(0)
#define sithCog_ThingSendMessage(...) assert(0)
#define sithSurface_HandleThingImpact(...) assert(0)
#define jkSaber_SpawnSparks jkSaberTraceSparks
#define sithThing_DamageSaber jkSaberTraceDamage
#include "blade-trace.inc"

static void tracePair(SithThing *attacker, SithThing *near, SithThing *far)
{
    contacts[0] = (SithCollision){.type = SITHCOLLISION_THING, .pThingCollided = near, .distance = 0.03};
    contacts[1] = (SithCollision){.type = SITHCOLLISION_THING, .pThingCollided = far, .distance = 0.08};
    struck = NULL;
    nextContact = wallSparks = bloodSparks = releases = 0;
    rdVector3 start = {0}, dir = {0,1,0};
    jkSaberCollide swing = {.bladeLength = 0.1, .damage = 30};
    jkSaber_UpdateCollision2(attacker, &start, &dir, &swing);
    assert(releases == 1);
}

int main(void)
{
    SithThing *a = &players[0], *b = &players[1], *outsider = &players[2];
    for (int i = 0; i < 3; i++) {
        players[i].type = SITH_THING_PLAYER;
        players[i].actorParams.pPlayer = &jkPlayer_playerInfos[i];
        players[i].actorParams.health = 50;
        players[i].actorParams.maxHealth = 117;
        jkPlayer_playerInfos[i].pLocalPlayer = &players[i];
    }
    strcpy(sithInventory_g_aTypes[SITHBIN_LIGHTSABER].fpath, "Lightsaber");
    strcpy(sithInventory_g_aTypes[SITHBIN_F_JUMP].fpath, "Force_Jump");
    sithInventory_g_aTypes[SITHBIN_F_JUMP].cog = &cog;
    sithInventory_g_aTypes[SITHBIN_F_JUMP].flags = SITHINVENTORY_TYPE_AUTOAIM | SITHINVENTORY_TYPE_REGISTERED;
    sithInventory_g_aTypes[SITHBIN_LIGHTSABER].cog = &cog;
    sithInventory_g_aTypes[SITHBIN_LIGHTSABER].flags = SITHINVENTORY_TYPE_REGISTERED | ITEMINFO_WEAPON;
    SithPlayer *info = a->actorParams.pPlayer;
    info->curWeaponID = SITHBIN_LIGHTSABER;
    info->aItems[SITHBIN_LIGHTSABER].amount = 1;
    info->aItems[SITHBIN_LIGHTSABER].state = SITHINVENTORY_ITEM_AVAILABLE;
    char reason[100];
    assert(sithDuelCombat_CanEnter(a, reason, sizeof(reason)));
    strcpy(sithInventory_g_aTypes[SITHBIN_LIGHTSABER].fpath, "ModGun");
    assert(!sithDuelCombat_CanEnter(a, reason, sizeof(reason)));
    strcpy(sithInventory_g_aTypes[SITHBIN_LIGHTSABER].fpath, "Lightsaber");
    sithInventory_powerKeybinds[3] = (SithControlBinding){1, SITHBIN_F_GRIP, 1};
    assert(!sithDuelCombat_CanEnter(a, reason, sizeof(reason)));
    sithInventory_powerKeybinds[3].idk = 0;
    Main_bMotsCompat = 1;
    assert(!sithDuelCombat_CanEnter(a, reason, sizeof(reason)));
    info->curWeaponID = SITHBIN_MOTS_LIGHTSABER;
    sithInventory_g_aTypes[SITHBIN_MOTS_LIGHTSABER] = sithInventory_g_aTypes[SITHBIN_LIGHTSABER];
    info->aItems[SITHBIN_MOTS_LIGHTSABER] = info->aItems[SITHBIN_LIGHTSABER];
    assert(sithDuelCombat_CanEnter(a, reason, sizeof(reason)));
    sithInventory_g_aTypes[SITHBIN_F_GRIP].flags = SITHINVENTORY_TYPE_REGISTERED | SITHINVENTORY_TYPE_AUTOAIM;
    info->aItems[SITHBIN_F_GRIP].state |= SITHINVENTORY_ITEM_ACTIVATED;
    assert(!sithDuelCombat_CanEnter(a, reason, sizeof(reason)));
    info->aItems[SITHBIN_F_GRIP].state = 0;

    SithThing shot = {.type = SITH_THING_WEAPON, .pParent = outsider};
    SithThing blast = {.type = SITH_THING_EXPLOSION, .pParent = &shot};
    SithThing crate = {.type = SITH_THING_COG, .moveType = SITH_MT_PHYSICS};
    SithThing door = {.type = SITH_THING_COG, .moveType = SITH_MT_PATH};
    assert(sithDuelCombat_AllowsDamage(a, &shot, 0));
    assert(sithDuelCombat_AllowsCollision(a, outsider));
    assert(sithDuelCombat_AllowsSkill(a, outsider));
    phase = 1;
    assert(!sithDuelCombat_AllowsDamage(a, b, 1));
    assert(!sithDuelCombat_AllowsBin(a, SITHBIN_F_JUMP));
    sithDuelCombat_Begin(a);
    sithDuelCombat_Begin(b);
    assert(resets == 1 && disables == 2);

    phase = 2;
    tracePair(a, &door, b);
    assert(!struck && nextContact == 1 && wallSparks == 1 && !bloodSparks);
    tracePair(a, outsider, b);
    assert(struck == b && nextContact == 2 && !wallSparks && bloodSparks == 1);
    assert(sithDuelCombat_AllowsDamage(a, b, 1));
    assert(!sithDuelCombat_AllowsDamage(a, b, 0));
    assert(!sithDuelCombat_AllowsDamage(a, outsider, 1));
    assert(!sithDuelCombat_AllowsDamage(outsider, a, 1));
    assert(!sithDuelCombat_AllowsDamage(a, &blast, 0));
    assert(!sithDuelCombat_AllowsDamage(a, &crate, 0));
    assert(sithDuelCombat_AllowsDamage(a, a, 0)); /* falling/drowning */
    assert(sithDuelCombat_AllowsDamage(a, &door, 0)); /* world crusher */
    assert(!sithDuelCombat_AllowsCollision(a, &shot));
    assert(!sithDuelCombat_AllowsCollision(&shot, a));
    assert(!sithDuelCombat_AllowsCollision(a, outsider));
    assert(!sithDuelCombat_AllowsCollision(outsider, a));
    assert(!sithDuelCombat_AllowsCollision(&crate, a));
    assert(sithDuelCombat_AllowsCollision(a, &door));
    shot.pParent = &door;
    assert(sithDuelCombat_AllowsCollision(a, &shot));
    assert(sithDuelCombat_AllowsDamage(a, &blast, 0));
    shot.pParent = outsider;
    assert(sithDuelCombat_AllowsCollision(a, b));
    assert(!sithDuelCombat_AllowsSkill(a, outsider));
    assert(!sithDuelCombat_AllowsSkill(outsider, b));
    assert(!sithDuelCombat_AllowsSkill(a, b));
    assert(sithDuelCombat_AllowsClaim(a, b, b, 1));
    assert(!sithDuelCombat_AllowsClaim(a, b, outsider, 1));
    assert(!sithDuelCombat_AllowsClaim(a, b, b, 0));
    assert(!sithDuelCombat_AllowsClaim(outsider, a, outsider, 0));
    assert(sithDuelCombat_AllowsBin(b, SITHBIN_F_JUMP));
    assert(!sithDuelCombat_AllowsBin(b, SITHBIN_F_SABERTHROW));
    assert(!sithDuelCombat_AllowsBin(a, SITHBIN_BACTATANK));
    shot.pParent = b;
    assert(!sithDuelCombat_AllowsDamage(outsider, &blast, 0));
    assert(!sithDuelCombat_AllowsCollision(&shot, outsider));
    sithInventory_g_aTypes[SITHBIN_SHIELDS].flags = SITHINVENTORY_TYPE_REGISTERED;
    sithInventory_g_aTypes[SITHBIN_SHIELDS].max = 237;
    phase = 0;
    sithDuelCombat_Reward(a);
    assert(a->actorParams.health == 117 && info->aItems[SITHBIN_SHIELDS].amount == 237);
    sithDuelCombat_Reward(b);
    assert(b->actorParams.health == 50);
    a->flags |= SITH_TF_DEAD;
    a->actorParams.health = 0;
    sithDuelCombat_Reward(a);
    assert(a->actorParams.health == 0);
    assert(sithDuelCombat_AllowsCollision(outsider, a));
    puts("Private duel combat rules passed.");
    return 0;
}
