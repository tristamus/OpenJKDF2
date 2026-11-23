#include "Gameplay/sithDuelCombat.h"

#ifdef PLATFORM_WSRELAY
#include "Gameplay/sithDuel.h"
#include "Gameplay/sithInventory.h"
#include "Gameplay/jkSaber.h"
#include "World/sithThing.h"
#include "World/sithWeapon.h"
#include "Engine/sithPhysics.h"
#include "Main/Main.h"
#include <stdio.h>
#include <strings.h>

static int duelSlot(const SithThing *thing)
{
    if (!sithNet_isMulti || !thing || thing->type != SITH_THING_PLAYER || !thing->actorParams.pPlayer)
        return -1;
    const SithPlayer *info = thing->actorParams.pPlayer;
    if (info->pLocalPlayer != thing)
        return -1;
    ptrdiff_t slot = info - jkPlayer_playerInfos;
    return slot >= 0 && slot < JKPLAYER_NUM_INFOS ? (int)slot : -1;
}

int sithDuelCombat_Phase(const SithThing *player)
{
    int slot = duelSlot(player);
    return slot < 0 ? 0 : sithDuel_PhaseForPlayer(slot);
}

static SithThing *owner(SithThing *thing)
{
    return thing && thing->pParent ? sithThing_GetThingParent(thing) : thing;
}

static int looseObject(const SithThing *thing)
{
    return thing->moveType == SITH_MT_PHYSICS &&
           (thing->type == SITH_THING_COG || thing->type == SITH_THING_DEBRIS);
}

/* Map-authored machinery may shoot, explode or crush. A player-powered
   ghost turret and a loose crate have no such static map owner. */
static int mapHazard(const SithThing *source)
{
    return source && source->type == SITH_THING_COG && !looseObject(source) &&
           ((uint32_t)source->guid >> 16) == 0;
}

static int saberBin(void)
{
    return Main_bMotsCompat ? SITHBIN_MOTS_LIGHTSABER : SITHBIN_LIGHTSABER;
}

/* items.dat can replace stock bins. The registered display key and script
   filename distinguish a saber/jump from another power assigned that bin. */
static int nameContains(const char *name, const char *word, size_t length)
{
    for (; *name; name++)
        if (!strncasecmp(name, word, length))
            return 1;
    return 0;
}

static int isNamedBin(const SithInventoryType *type, const char *word, size_t length)
{
    if (nameContains(type->fpath, word, length)) return 1;
#ifdef SITH_DEBUG_STRUCT_NAMES
    if (type->cog && nameContains(type->cog->aName, word, length)) return 1;
#endif
    return 0;
}

static int busyInventory(const SithPlayer *info)
{
    if (sithInventory_bUnkPower || sithInventory_g_bSendDeactivateMessage)
        return 1;
    for (int i = 0; i < SITHINVENTORY_NUM_POWERKEYBINDS; i++)
        if (sithInventory_powerKeybinds[i].enabled && sithInventory_powerKeybinds[i].idk)
            return 1;
    for (int i = 0; i < SITHBIN_NUMBINS; ++i) {
        const SithInventoryType *type = &sithInventory_g_aTypes[i];
        if ((type->flags & (ITEMINFO_ITEM | SITHINVENTORY_TYPE_AUTOAIM)) &&
            ((info->aItems[i].state & SITHINVENTORY_ITEM_ACTIVATED) || info->aItems[i].activationDelaySecs > 0))
            return 1;
    }
    return 0;
}

static int alive(const SithThing *player)
{
    return player && player->type == SITH_THING_PLAYER && player->actorParams.pPlayer &&
           player->actorParams.health > 0 &&
           !(player->flags & (SITH_TF_DEAD | SITH_TF_DISABLED | SITH_TF_DESTROYED));
}

int sithDuelCombat_CanEnter(const SithThing *player, char *reason, size_t reasonSize)
{
    const char *why = NULL;
    int saber = saberBin();
    const SithInventoryType *weapon = &sithInventory_g_aTypes[saber];
    if (!alive(player) || player != sithPlayer_g_pLocalPlayerThing)
        why = "You must be alive to duel.";
    else {
        const SithPlayer *info = player->actorParams.pPlayer;
        if (info->curWeaponID != saber || sithWeapon_8BD024 >= 0 ||
            !(weapon->flags & SITHINVENTORY_TYPE_REGISTERED) || !(weapon->flags & ITEMINFO_WEAPON) ||
            !weapon->cog || !isNamedBin(weapon, "saber", 5) || !(info->aItems[saber].state & SITHINVENTORY_ITEM_AVAILABLE) ||
            info->aItems[saber].amount <= 0)
            why = "Equip your lightsaber first.";
        else if ((player->actorParams.flags & (SITH_AF_INVULNERABLE | SITH_AF_INVISIBLE |
                                               SITH_AF_CONTROLSDISABLED | SITH_AF_FREEZE_MOVEMENT)) ||
                 (player->playerInfo && (player->playerInfo->bHasSuperWeapon ||
                   player->playerInfo->bHasSuperShields || player->playerInfo->bHasForceSurge)))
            why = "Wait for your current effect to end.";
        else if (busyInventory(info))
            why = "Finish using your item or Force power first.";
    }
    if (reason && reasonSize)
        snprintf(reason, reasonSize, "%s", why ? why : "");
    return why == NULL;
}

void sithDuelCombat_Begin(SithThing *player)
{
    if (!player) return;
    if (player == sithPlayer_g_pLocalPlayerThing) {
        /* During preparation this takes the normal release path for both
           fire buttons, so the equipped saber COG receives DEACTIVATED. */
        sithWeapon_ProcessWeaponControls(player, 0);
        sithWeapon_StartupEntry();
        sithPhysics_ResetThingMovement(player);
    }
    jkSaber_Disable(player);
}

void sithDuelCombat_Reward(SithThing *player)
{
    if (player != sithPlayer_g_pLocalPlayerThing || !alive(player))
        return;
    player->actorParams.health = player->actorParams.maxHealth;
    const SithInventoryType *shields = &sithInventory_g_aTypes[SITHBIN_SHIELDS];
    if (shields->flags & SITHINVENTORY_TYPE_REGISTERED)
        sithInventory_SetInventory(player, SITHBIN_SHIELDS, shields->max);
}

int sithDuelCombat_AllowsDamage(SithThing *victim, SithThing *source, int saberSwing)
{
    SithThing *attacker = owner(source);
    int victimPhase = sithDuelCombat_Phase(victim);
    int attackerPhase = sithDuelCombat_Phase(attacker);
    if (!victimPhase && !attackerPhase)
        return 1;
    if (victimPhase && source == victim)
        return 1; /* Stock fall, drown and scripted sector hazards. */
    if (saberSwing && victimPhase == 2 && attackerPhase == 2 && source == attacker)
        return sithDuel_AreOpponents(duelSlot(victim), duelSlot(attacker));
    /* A moving door or a map's damage script still works. A thrown crate,
       ghost-fired shot or parentless explosion can have no surviving owner. */
    return victimPhase && !attackerPhase && (!source || mapHazard(attacker));
}

int sithDuelCombat_AllowsClaim(SithThing *victim, SithThing *credit, SithThing *sender, int saberSwing)
{
    if (!sithDuelCombat_Phase(victim) && !sithDuelCombat_Phase(credit) && !sithDuelCombat_Phase(sender))
        return 1;
    return saberSwing && sender == credit && victim != credit &&
           sithDuelCombat_AllowsDamage(victim, credit, 1);
}

static int allowsContact(const SithThing *player, SithThing *other)
{
    if (!sithDuelCombat_Phase(player))
        return 1;
    if (other->type == SITH_THING_PLAYER)
        return sithDuel_AreOpponents(duelSlot(player), duelSlot(other));
    if (other->type == SITH_THING_WEAPON || other->type == SITH_THING_EXPLOSION)
        return mapHazard(owner(other));
    return other->type != SITH_THING_ITEM && !looseObject(other);
}

int sithDuelCombat_AllowsCollision(SithThing *a, SithThing *b)
{
    if (!a || !b || !sithNet_isMulti)
        return 1;
    if (!allowsContact(a, b) || !allowsContact(b, a))
        return 0;
    /* A mine placed just before accepting can still be lying in the map. */
    if (a->type == SITH_THING_WEAPON && b->type == SITH_THING_PLAYER)
        return !sithDuelCombat_Phase(owner(a));
    if (b->type == SITH_THING_WEAPON && a->type == SITH_THING_PLAYER)
        return !sithDuelCombat_Phase(owner(b));
    return 1;
}

int sithDuelCombat_AllowsSkill(SithThing *victim, SithThing *source)
{
    return !sithDuelCombat_Phase(victim) && !sithDuelCombat_Phase(owner(source));
}

int sithDuelCombat_AllowsBin(const SithThing *player, int bin)
{
    int phase = sithDuelCombat_Phase(player);
    if (!phase) return 1;
    if (phase != 2 || bin != SITHBIN_F_JUMP) return 0;
    const SithInventoryType *jump = &sithInventory_g_aTypes[bin];
    return jump->cog && (jump->flags & SITHINVENTORY_TYPE_AUTOAIM) && isNamedBin(jump, "jump", 4);
}

int sithDuelCombat_AllowsWeapon(const SithThing *player, int bin)
{
    return !sithDuelCombat_Phase(player) || bin == saberBin();
}

int sithDuelCombat_AllowsProjectile(SithThing *shooter)
{
    return !sithDuelCombat_Phase(owner(shooter));
}
#endif
