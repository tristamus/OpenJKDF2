#ifndef _SITHKEYRETARGET_H
#define _SITHKEYRETARGET_H

#include "types.h"

// Added: keyframes converted at load from another skeleton's animations.
//
// A mod lists them in misc\keyretarget.dat: a source and a target model,
// how the source's bones map onto the target's, and which keys to make
// from which. Each listed key (ex. rt_bochop.key) is built from the
// player's own game files when something first asks for it, so nothing
// derived from them has to be shipped. Keys apply by node index, which is
// why another skeleton's keys can't just be played on a player.
//
// The maths works in doubles, so it is left out of fixed-point builds.
#if !defined(EXPERIMENTAL_FIXED_POINT) && !defined(TARGET_TWL)
#define SITH_KEY_RETARGET
#endif

#ifdef SITH_KEY_RETARGET
typedef struct rdKeyframe rdKeyframe;

// Forgets the cached recipe, so the next world reads the current mod's.
void sithKeyRetarget_Reset();

// Loads a key from pPath (3do\key\<pName>), or builds it instead if the
// current mod's recipe lists it, then adds any markers the mod's
// misc\keymarkers.dat gives it. Returns 0 on failure.
int sithKeyRetarget_LoadEntry(const char* pName, char* pPath, rdKeyframe* pKeyframe);
#else
#define sithKeyRetarget_LoadEntry(pName, pPath, pKeyframe) rdKeyframe_LoadEntry(pPath, pKeyframe)
#endif

#endif // _SITHKEYRETARGET_H
