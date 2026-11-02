#ifndef _SITHSOUNDCLASSSWIM_H
#define _SITHSOUNDCLASSSWIM_H

// Added: six of LucasArts' multiplayer skins (Bossk, Tusken, C3PO, Greedo,
// Sariss and Jan Ors: kybossk.snd, kytusken.snd, ky3po.snd, kygreedo.snd,
// kyfemale.snd) play stroke2.wav for both swim strokes, where Kyle's ky.snd
// plays stroke1.wav on the left one. kysbswim.key strokes every 8 frames at
// 15 fps, so the same short splash every 0.53 s sounds like the swim loop
// running twice as fast. sithSoundClass_LoadEntry gives such a class's left
// stroke Kyle's sound. This file has no engine state, so it can be tested on
// its own (packaging/wasm/test/swimstroke-test.c).

#ifdef __cplusplus
extern "C" {
#endif

// The sound a left surface stroke gets when the class has that pair.
#define SITHSOUNDCLASSSWIM_LEFT "stroke1.wav"

// Whether a class's lswimsurface and rswimsurface lines are that pair: one
// sound each, both stroke2.wav (any case). Names may be NULL ("none" or no
// line).
int sithSoundClassSwim_NeedsLeftStroke(const char* pLeft, int numLeft, const char* pRight, int numRight);

#ifdef __cplusplus
}
#endif

#endif // _SITHSOUNDCLASSSWIM_H
