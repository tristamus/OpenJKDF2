#ifndef SITH_DUEL_COMBAT_H
#define SITH_DUEL_COMBAT_H

#include "types.h"

int sithDuelCombat_CanEnter(const SithThing *player, char *reason, size_t reasonSize);
void sithDuelCombat_Begin(SithThing *player);
void sithDuelCombat_Reward(SithThing *player);
int sithDuelCombat_Phase(const SithThing *player);
int sithDuelCombat_AllowsDamage(SithThing *victim, SithThing *source, int saberSwing);
int sithDuelCombat_AllowsClaim(SithThing *victim, SithThing *credit, SithThing *sender, int saberSwing);
int sithDuelCombat_AllowsCollision(SithThing *a, SithThing *b);
int sithDuelCombat_AllowsSkill(SithThing *victim, SithThing *source);
int sithDuelCombat_AllowsBin(const SithThing *player, int bin);
int sithDuelCombat_AllowsWeapon(const SithThing *player, int bin);
int sithDuelCombat_AllowsProjectile(SithThing *shooter);

#endif
