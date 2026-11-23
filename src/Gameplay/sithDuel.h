#ifndef SITH_DUEL_H
#define SITH_DUEL_H

#ifdef PLATFORM_WSRELAY
#include "types.h"

#define SITHDUEL_MSG 0x444A
// Bit 0 was PR 132's single-pair protocol, never released; bit 1 is this
// one (sithDuelState.c, version 2), which several pairs can use at once.
#define SITHDUEL_HELLO_CAP 0x0002
#define SITHDUEL_JOIN_TAG 0x324C444A /* JDL2, after the standard join request */

// The host's choice on Host > Advanced (Private duels), off unless ticked.
// The game a host opens allows duels only when it's ticked.
extern int sithDuel_bAllowDuels;

void sithDuel_Reset(void);
// Whether this game's match allows private duels: a free-for-all in the
// browser whose host ticked Private duels (the host's own view of it).
int sithDuel_HostAllows(void);
void sithDuel_Frame(void);
void sithDuel_Forget(int slot, uint32_t id);
void sithDuel_PlayerDied(SithThing *player); // locally scored death
void sithDuel_OwnerDeath(SithThing *player, uint32_t senderId); // transport-authenticated death
void sithDuel_Respawn(SithThing *player);
void sithDuel_Hello(int slot, uint32_t id, int capable);
void sithDuel_Joining(int slot, uint32_t id, int capable);
void sithDuel_Welcome(int slot, uint32_t id);
void sithDuel_Process(SithMessage *msg);
int sithDuel_Chat(const char *line);
int sithDuel_Command(stdDebugConsoleCmd *cmd, const char *args);
int sithDuel_PhaseForPlayer(int idx);
int sithDuel_AreOpponents(int a, int b);
#endif
#endif
