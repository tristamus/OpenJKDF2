#ifndef SITH_DUEL_H
#define SITH_DUEL_H

#ifdef PLATFORM_WSRELAY
#include "types.h"

#define SITHDUEL_MSG 0x444A
#define SITHDUEL_HELLO_CAP 0x0001
#define SITHDUEL_JOIN_TAG 0x314C444A /* JDL1, after the standard join request */

void sithDuel_Reset(void);
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
