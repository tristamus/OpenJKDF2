#ifndef _SITHINTERP_H
#define _SITHINTERP_H

// Added: render interpolation. Physics runs in fixed steps (150 a second, 50
// at a host tickrate of 100 ms or more), so at most refresh rates a frame
// gets one step, none or two, and anything that moves is drawn jumping by
// uneven amounts. This module keeps each thing's pose from the start and the
// end of a frame's last step, and lets the renderer draw it in between, at
// the frame's own time less half a step. Nothing here writes a SithThing or
// the game camera: the simulation, hits, sound and the network see exactly
// what they see without it. See r_interp (sithInterp_mode) for the settings.

#include "types.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(QOL_IMPROVEMENTS) && defined(FIXED_TIMESTEP_PHYS) && !defined(TARGET_RETRO_HOMEBREW)
#define SITHINTERP_ENABLED
#endif

// r_interp: 0 off; 1 things only, camera true; 2 things and camera;
// 9 identity (debug: alpha 1, so everything is drawn at its last step's pose
// through the same code).
#define SITHINTERP_MODE_OFF (0)
#define SITHINTERP_MODE_THINGS (1)
#define SITHINTERP_MODE_ALL (2)
#define SITHINTERP_MODE_IDENTITY (9)

#ifdef SITHINTERP_ENABLED

extern int sithInterp_mode;

// Debug counters (console: interp).
extern uint32_t sithInterp_unexplainedSnaps; // distance or turn fallback fired with no snap hook before it
extern uint32_t sithInterp_drawnShifted;     // things drawn away from their last step's pose
extern uint32_t sithInterp_framesArmed;

// Level open, game load and multiplayer join: nothing captured before it is
// blended with anything after it.
void sithInterp_Reset();

// sithUpdate's stepped branch: CapturePrev at the top of the frame's last
// step, CaptureCur after the step loop (only when it ran a step), then
// FrameStepped with the rollover in steps (-0.5 to 0.5 with round()) and
// the step size, on every frame of that branch, 0-step frames included.
void sithInterp_CapturePrev();
void sithInterp_CaptureCur();
void sithInterp_FrameStepped(flex_d_t rollSteps, flex_d_t stepSecs);
// The other branches (unstepped physics, loading): nothing is blended.
void sithInterp_FrameNotStepped();

// Around the world draw (jkGame_Update): Arm only when the frame's tick was
// stepped and r_interp is on; Disarm once the world, the weapon and the HUD
// are drawn, before anything that can run game code.
void sithInterp_Arm();
void sithInterp_Disarm();
int sithInterp_IsArmed();
flex_t sithInterp_GetAlpha();
// The local player's yaw from the turn keys in the last step (degrees), for
// the view's turn between steps: the view is drawn turned back by
// (1 - alpha) of it.
flex_t sithInterp_GetLocalKeyYaw();

// After code that puts a thing somewhere new outright (spawn, teleport, a
// cog's SetThingPos, a network warp): it isn't drawn sliding there.
void sithInterp_Snap(SithThing* pThing);

// Where to draw pThing this frame. Returns 0 to draw it where it is
// (not armed, new, moved outside the steps, snapped, or not moving), else 1
// with the placement in pOut (orientation in rvec/lvec/uvec, position in
// scale). *pbTranslateOnly is set for things whose orient isn't a rotation
// (particles, sprites): only the position moves.
int sithInterp_GetRenderPlacement(SithThing* pThing, rdMatrix34* pOut, int* pbTranslateOnly);
// The same, position only; pOut gets the live position when it returns 0.
int sithInterp_GetRenderPos(SithThing* pThing, rdVector3* pOut);

#else

#define sithInterp_Reset() do {} while (0)
#define sithInterp_CapturePrev() do {} while (0)
#define sithInterp_CaptureCur() do {} while (0)
#define sithInterp_FrameStepped(a, b) do {} while (0)
#define sithInterp_FrameNotStepped() do {} while (0)
#define sithInterp_Arm() do {} while (0)
#define sithInterp_Disarm() do {} while (0)
#define sithInterp_IsArmed() (0)
#define sithInterp_Snap(t) do {} while (0)

#endif // SITHINTERP_ENABLED

#ifdef __cplusplus
}
#endif

#endif // _SITHINTERP_H
