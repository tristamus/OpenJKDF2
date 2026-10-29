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
// (packaging/wasm/netstats.js). Under the Classic rules another player's
// game decides whether a hit on them counts, and this one only shows it.
// Under Hit what you see, this game decides its own player's hits on others
// and claims them (stdComm_WSRelayHits.c), and the other game can turn a
// claim down.
#define STDCOMM_WSRELAY_HIT (1)
#define STDCOMM_WSRELAY_BLOCK (2)
#define STDCOMM_WSRELAY_DEAD (3)
#define STDCOMM_WSRELAY_TURNED_DOWN (4)

// Kinds of hit, as the hit ledger names them.
#define STDCOMM_WSRELAY_KIND_SABER (1)
#define STDCOMM_WSRELAY_KIND_SHOT (2)
#define STDCOMM_WSRELAY_KIND_SPLASH (3)
#define STDCOMM_WSRELAY_KIND_FORCE (4)
#define STDCOMM_WSRELAY_KIND_OTHER (5)

// The kind of a hit by pSource with damageType.
int stdComm_WSRelay_HitKind(SithThing* pSource, int damageType);

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
// (sithDSSThing_ProcessFire), which sithWeapon_WeaponFireProjectile gives it
// as it's made, since it can hit at once. 0 otherwise.
extern int stdComm_WSRelay_fireGuid;

// The number the game packet being handled came with on the fast lane, if
// it did (returns 1).
int stdComm_WSRelay_RecvSeq(uint32_t* pSeq);

// When this game sent player toId its position update numbered seq, by
// stdComm_WSRelay_Now's clock (ms), or -1 if it doesn't know.
double stdComm_WSRelay_SeqSentAt(int toId, uint32_t seq);
double stdComm_WSRelay_Now();

// The hit ledger: a claim from player otherId this game turned down, or
// found its player dead for (outcome STDCOMM_WSRELAY_DEAD), and why
// (stdComm_WSRelayHitCheck.h's WSRELAYHIT_*, or
// STDCOMM_WSRELAY_HITS_NOT_PLAYING). distance: how close its player came to
// where the claim showed it, -1 if that wasn't checked.
void stdComm_WSRelay_NoteTurnedDown(int otherId, int kind, int outcome, flex_t damage, uint32_t attack, int reason, float distance);

// ---- Hit what you see (stdComm_WSRelayHits.c).

// The rules a match plays by: the host's choice, on its Advanced screen.
#define STDCOMM_WSRELAY_HITS_CLASSIC (0)
#define STDCOMM_WSRELAY_HITS_HWYS (1)

// How far back a claim can reach, in ms.
#define STDCOMM_WSRELAY_REACH_MIN (150)
#define STDCOMM_WSRELAY_REACH_MAX (500)
#define STDCOMM_WSRELAY_REACH_DEFAULT (300)

// A host lists its rules in its lobby entry: guidInstance.c is this, and
// guidInstance.d the rules, with the reach in ms from bit 8. The lobby and
// older games ignore both.
#define STDCOMM_WSRELAY_HITS_ENTRY_MAGIC (0x4A4B4852) // "JKHR"

// The engine message the rules' messages go in, past DSS_MAX: 'J' 'C'.
#define STDCOMM_WSRELAY_HITS_MSG (0x434A)

// Why a claim was turned down, past stdComm_WSRelayHitCheck.h's reasons:
// this game doesn't play by claims with the sender.
#define STDCOMM_WSRELAY_HITS_NOT_PLAYING (8)

// Another game's shot starts where its shooter was when this game's copy of
// the shooter is farther than this from there, in JKU.
#define STDCOMM_WSRELAY_SHOT_START_SLACK (0.3)

// The host's settings (jkGuiNetHost), which it plays by when it hosts.
extern int stdComm_WSRelay_bClassicHits;
extern int stdComm_WSRelay_hitReachMs;

int stdComm_WSRelay_ClampReach(int reachMs);

// This match's rules, and the reach. A new session starts Classic, until the
// host's entry or hello says otherwise.
void stdComm_WSRelay_SetHitRules(int rules, int reachMs);
int stdComm_WSRelay_GetHitRules(int* pReachMs);
void stdComm_WSRelay_ResetHits();

// A hit on player pVictim from pSource, before sithThing_DamageThing applies
// it. Returns 1 if this game leaves it to the attacker's game, which sends a
// claim for it: the hit only shows here. When this game's own attack hit
// another player, it claims the hit, and the hit shows here as before (0).
int stdComm_WSRelay_ClaimHit(SithThing* pVictim, SithThing* pSource, flex_t damage, int damageType);

// While a claim's damage is applied: the ledger's kind and attack number for
// it, and how close the player came to where the claim showed them.
int stdComm_WSRelay_ApplyingClaim(int* pKind, uint32_t* pAttack, float* pDistance);

// Whether this game claims its hits on player pPlayer, and so decides its
// blocks against them too.
int stdComm_WSRelay_ClaimsTo(SithThing* pPlayer);

// Whether a hit or block on this game's player by pSource is the attacker's
// game's to decide.
int stdComm_WSRelay_DecidedElsewhere(SithThing* pSource, int damageType);

// What other players' games tell this one about their attacks: a swing
// (DSS_JKENABLESABER), a shot (sithDSSThing_ProcessFire: where it was
// fired from, its guid and how its game scaled it), and their position
// updates (sithDSSThing_ProcessPos), from player fromNetId.
void stdComm_WSRelay_NoteSwing(SithThing* pPlayer, uint32_t swing);
void stdComm_WSRelay_NoteShot(SithThing* pTemplate, const rdVector3* pFirePos, int guid, flex_t scale, int scaleFlags, flex_t secsAhead);
void stdComm_WSRelay_NotePos(SithThing* pThing, int fromNetId);

// An explosion made by pWeapon (sithWeapon_CreateWeaponExplosion), so a hit
// by its blast is known by the shot's guid.
void stdComm_WSRelay_NoteBlast(SithThing* pExplosion, SithThing* pWeapon);

// Whether player pBlocker's saber turning pWeapon away here is another
// game's to decide (the one that fired it), which then says so.
int stdComm_WSRelay_BlockDecidedElsewhere(SithThing* pWeapon, SithThing* pBlocker);

// Whether another player is in the fire wait after their last swing, as their
// game said: until it's over, their saber turns no bolt away, as a game's own
// player's doesn't (sithWeapon_fireWait).
int stdComm_WSRelay_InFireWait(SithThing* pPlayer);

// A saber turned pWeapon away (sithActor_thing_anim_blocked). If this game
// fired it, the others' copies follow its new path.
void stdComm_WSRelay_BoltTurned(SithThing* pWeapon, SithThing* pBlocker);

// pBy sets charge pWeapon off: it goes off in fuseMs. Returns 0 if another
// game says when, which this one then hears.
int stdComm_WSRelay_ArmCharge(SithThing* pWeapon, SithThing* pBy, int fuseMs);

// Where another game's shot starts here (sithDSSThing_ProcessFire), from
// where its shooter was: returns 1 with the place and its sector when this
// game's copy of the shooter is too far from there.
int stdComm_WSRelay_ShotStart(SithThing* pShooter, int guid, const rdVector3* pShooterPos, rdVector3* pOutPos, SithSector** ppOutSector);

// A message of the rules' own (sithMessage_ProcessMessages), each frame's
// end (sithMain), and after a fire message, so claims for what a shot hit as
// it was made go after it (sithDSSThing_Fire).
void stdComm_WSRelay_ProcessHits(SithMessage* pMsg);
void stdComm_WSRelay_HitsFrame();
void stdComm_WSRelay_FlushHits();

// The Join list's words for a listed game's hit rules, or fewer of them.
void stdComm_WSRelay_DescribeHits(int idx, int bShort, char16_t* pOut, int maxChars);

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
