#ifndef _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAY_H
#define _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAY_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef PLATFORM_WSRELAY

// What stdComm_Open returns when the engine has to restart into the host's
// mod before joining (g_should_exit is already set).
#define STDCOMM_WSRELAY_RESTART (0x4A4B5752)

void stdComm_WSRelay_Startup();
void stdComm_WSRelay_Shutdown();

// Writes a line about the mod of listed game idx for the join screen, empty
// if there's nothing to say.
void stdComm_WSRelay_DescribeMod(int idx, char16_t* pOut, int maxChars);

// What happened to a hit between players, for the hit ledger
// (packaging/wasm/netstats.js). Another player's game decides whether a hit
// on them counts; this one only shows it.
#define STDCOMM_WSRELAY_HIT (1)
#define STDCOMM_WSRELAY_BLOCK (2)
#define STDCOMM_WSRELAY_DEAD (3)

// A hit on player pVictim from pSource (a player, or their shot, explosion or
// Force power), before the victim's game applies its damage or a block.
void stdComm_WSRelay_NoteHit(SithThing* pVictim, SithThing* pSource, flex_t damage, int damageType, int outcome);

// As NoteHit, for a hit whose source no longer leads back to its attacker: a
// bolt a ready saber turns away is the blocker's from then on.
void stdComm_WSRelay_NoteHitBy(SithThing* pVictim, SithThing* pAttacker, SithThing* pSource, flex_t damage, int damageType, int outcome);

// So the ledger can tell which attack a hit in either game came from, each
// saber swing a player starts (jkEnableSaber) gets a number, which the
// message that turns their blade on in the other games carries
// (DSS_JKENABLESABER). Set: the number from that message, 0 if it had none.
void stdComm_WSRelay_NewSwing(SithThing* pPlayer);
void stdComm_WSRelay_SetSwing(SithThing* pPlayer, uint32_t swing);
uint32_t stdComm_WSRelay_GetSwing(SithThing* pPlayer);

// A shot's id in every game (its guid) while another game's shot is made here
// (sithDSSThing_ProcessFire), since it can hit before it gets it. 0 otherwise.
extern int stdComm_WSRelay_fireGuid;

// ?nettest's scripted player (stdComm_WSRelayTest.c). While it plays, it
// gives sithControl the input it reads.
extern int stdComm_WSRelay_bTestInput;
void stdComm_WSRelay_TestTick(SithThing* pPlayer, flex_t deltaSecs);
int stdComm_WSRelay_TestKey(int inputFunc, int* pPresses);
flex_t stdComm_WSRelay_TestAxis(int inputFunc);

// Whether this game is in a match with other players: its player has joined,
// which the host checks before taking anything from it.
int stdComm_WSRelay_InMatch();

#endif // PLATFORM_WSRELAY

#ifdef __cplusplus
}
#endif

#endif // _PLATFORM_NETWORKING_WSRELAY_STDCOMM_WSRELAY_H
